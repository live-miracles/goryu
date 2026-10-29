#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backoff.h"
#include "log.h"
#include "net.h"
#include "relay_common.h"
#include "relay_config.h"
#include "runtime.h"
#include "srt_opts.h"
#include "stats.h"
#include "stream_state.h"
#include "tracker.h"

#define CHUNK 1456
/* How long srt_recvmsg2 may block without data. A session whose publisher goes
 * silent would otherwise sit in the call indefinitely, and srt_close() from
 * another thread (duplicate takeover) does not wake a blocked receive on a bonded
 * group: libsrt defers closing a group that is in use. Waking once a second lets
 * the session notice that it was closed. */
#define RECV_TIMEOUT_MS 1000
#define DUPLICATE_TAKEOVER_WAIT_MS 5000
#define DUPLICATE_TAKEOVER_STALE_MS 5000
/* Room for the longest connect failure message: a 1023-byte streamid, libsrt's error
 * text and the reject reason. */
#define OUTPUT_ERROR_MAX 2048

typedef struct session_args {
    SRTSOCKET conn;
    const output_target_t *target;
    int tracker_slot;
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
} session_args_t;

/* What one session thread knows about itself. Its helpers take this instead of the
 * same handful of values each time. */
typedef struct session {
    const output_target_t *target;
    int tracker_slot; /* this session's sockets, in tracker.c */
    int state_slot;   /* its telemetry, in stream_state.c; -1 for an empty streamid */
    char streamid[1024];
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
    SRTSOCKET srt_out; /* the downstream socket; SRT_INVALID_SOCK while it is down */
    retry_state_t retry;
} session_t;

static const char *streamid_or_empty(const char *streamid) {
    return streamid && streamid[0] ? streamid : "(empty)";
}

static int stream_input_still_connected(SRTSOCKET conn) {
    SRT_SOCKSTATUS st = srt_getsockstate(conn);
    return st == SRTS_CONNECTED || st == SRTS_LISTENING || st == SRTS_CONNECTING;
}

static int get_streamid(SRTSOCKET sock, char *sid, size_t sid_sz) {
    if (sid_sz == 0) return 0;
    sid[0] = '\0';
    int sid_len = (int)sid_sz - 1;
    if (srt_getsockflag(sock, SRTO_STREAMID, sid, &sid_len) == SRT_ERROR) return 0;
    if (sid_len < 0) sid_len = 0;
    if ((size_t)sid_len >= sid_sz) sid_len = (int)sid_sz - 1;
    sid[sid_len] = '\0';
    return sid_len > 0;
}

/* ---- downstream connection ---- */

/* Records a failed connect in the process error, the log and error_out. The
 * reject reason, when libsrt gives one, is part of all three. */
static void report_connect_failure(const session_t *s, SRTSOCKET srt_out, char *error_out,
                                   size_t error_out_sz) {
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&s->target->out_addr, out_ip, sizeof out_ip, &out_port);
    int error_code = srt_getlasterror(NULL);
    char error_text[256];
    snprintf(error_text, sizeof error_text, "%s", srt_getlasterror_str());
    int reject_reason = error_code == SRT_ECONNREJ ? srt_getrejectreason(srt_out) : SRT_REJ_UNKNOWN;
    const char *reason_text =
        reject_reason != SRT_REJ_UNKNOWN ? srt_rejectreason_str(reject_reason) : NULL;

    char detail[128] = "";
    char log_suffix[128] = "";
    if (reject_reason != SRT_REJ_UNKNOWN) {
        const char *reason = reason_text ? reason_text : "unknown";
        snprintf(detail, sizeof detail, " (reject=%s/%d)", reason, reject_reason);
        snprintf(log_suffix, sizeof log_suffix, " reject=%s/%d", reason, reject_reason);
    }

    if (error_out && error_out_sz > 0) {
        snprintf(error_out, error_out_sz, "srt_connect target=%s:%d streamid=%s: %s%s", out_ip,
                 out_port, streamid_or_empty(s->streamid), error_text, detail);
    }
    set_last_errorf("srt_connect target=%s:%d streamid=%s: %s%s", out_ip, out_port,
                    streamid_or_empty(s->streamid), error_text, detail);
    relay_logf("srt_connect failed target=%s:%d streamid=%s error=\"%s\"%s\n", out_ip, out_port,
               streamid_or_empty(s->streamid), error_text, log_suffix);
}

static void close_output(int tracker_slot, SRTSOCKET srt_out) {
    if (tracker_take_output(tracker_slot, srt_out) != SRT_INVALID_SOCK) {
        srt_close(srt_out);
    }
}

static SRTSOCKET connect_srt_output(const session_t *s, char *error_out, size_t error_out_sz) {
    if (error_out && error_out_sz > 0) error_out[0] = '\0';

    SRTSOCKET srt_out = srt_create_socket();
    if (srt_out == SRT_INVALID_SOCK) {
        if (error_out && error_out_sz > 0) {
            snprintf(error_out, error_out_sz, "srt_create_socket(out): %s", srt_getlasterror_str());
        }
        set_last_errorf("srt_create_socket(out): %s", srt_getlasterror_str());
        relay_logf("srt_create_socket(out): %s\n", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }
    tracker_set_output_socket(s->tracker_slot, srt_out);

    SRT_TRANSTYPE tt = SRTT_LIVE;
    srt_setsockflag(srt_out, SRTO_TRANSTYPE, &tt, sizeof tt);
    char opt_error[256];
    if (apply_srt_opts(srt_out, s->target->out_query, opt_error, sizeof opt_error) != 0) {
        if (error_out && error_out_sz > 0) {
            snprintf(error_out, error_out_sz, "output socket: %s", opt_error);
        }
        set_last_errorf("output socket: %s", opt_error);
        relay_logf("output socket: %s\n", opt_error);
        close_output(s->tracker_slot, srt_out);
        return SRT_INVALID_SOCK;
    }

    if (s->streamid[0]) {
        srt_setsockflag(srt_out, SRTO_STREAMID, s->streamid, (int)strlen(s->streamid));
    }

    int cto = 3000;
    srt_setsockflag(srt_out, SRTO_CONNTIMEO, &cto, sizeof cto);

    if (srt_connect(srt_out, (struct sockaddr *)&s->target->out_addr,
                    (int)s->target->out_addrlen) == SRT_ERROR) {
        report_connect_failure(s, srt_out, error_out, error_out_sz);
        close_output(s->tracker_slot, srt_out);
        return SRT_INVALID_SOCK;
    }
    if (!tracker_is_current(s->tracker_slot, SRT_INVALID_SOCK, srt_out)) {
        return SRT_INVALID_SOCK;
    }

    return srt_out;
}

/* Counts a failed connect or send, schedules the next attempt and publishes the
 * count. `what` names the step in the retry log line. Returns the delay in ms. */
static int record_output_failure(session_t *s, const char *what) {
    int delay_ms = retry_failed(&s->retry, now_ms());
    set_stream_retry_failures(s->state_slot, s->retry.failures);
    relay_logf("Output %s retry streamid=%s failures=%d retry_ms=%d\n", what,
               streamid_or_empty(s->streamid), s->retry.failures, delay_ms);
    return delay_ms;
}

/* Tries once and sets s->srt_out. On success it starts the healthy-period clock; on
 * failure it counts the failure and records why in the stream's lastError. */
static void connect_srt_output_with_retry(session_t *s) {
    char output_error[OUTPUT_ERROR_MAX];
    s->srt_out = connect_srt_output(s, output_error, sizeof output_error);
    if (s->srt_out != SRT_INVALID_SOCK) {
        set_stream_output_connected(s->state_slot, 1);
        retry_connected(&s->retry, now_ms());
        return;
    }

    int delay_ms = record_output_failure(s, "publish");
    set_stream_output_connected(s->state_slot, 0);
    if (output_error[0]) {
        set_stream_errorf(s->state_slot,
                          "Failed to publish to downstream output: %s; retrying in %d ms",
                          output_error, delay_ms);
    } else {
        set_stream_errorf(s->state_slot,
                          "Failed to publish to downstream output; retrying in %d ms", delay_ms);
    }
}

/* ---- duplicate publishers ---- */

/* Claims the stream's state for this session. A streamid that is already active
 * waits up to DUPLICATE_TAKEOVER_WAIT_MS: if the holder's input has been silent
 * for DUPLICATE_TAKEOVER_STALE_MS it is closed and replaced, otherwise this
 * session is rejected. Returns 0 to carry on, with s->state_slot set (-1 for an
 * empty streamid, which has no state), or -1 after logging why the session
 * must end. */
static int acquire_stream_state(session_t *s) {
    s->state_slot = claim_stream_state(s->streamid, s->tracker_slot);

    if (s->state_slot == CLAIM_STREAM_IN_USE) {
        long long deadline = now_ms() + DUPLICATE_TAKEOVER_WAIT_MS;
        int takeover_started = 0;
        do {
            long long now = now_ms();
            long long input_age_ms = 0;
            int stream_present = stream_input_age_ms(s->streamid, now, &input_age_ms) == 0;

            if (!stream_present) {
                s->state_slot = claim_stream_state(s->streamid, s->tracker_slot);
                if (s->state_slot != CLAIM_STREAM_IN_USE) break;
            } else if (input_age_ms >= DUPLICATE_TAKEOVER_STALE_MS) {
                if (!takeover_started) {
                    set_last_errorf("Duplicate publisher takeover for stale streamid=%s",
                                    s->streamid);
                    relay_logf("Duplicate publisher takeover for stale streamid=%s\n", s->streamid);
                    tracker_close_streamid(s->streamid, s->tracker_slot);
                    takeover_started = 1;
                }
                s->state_slot = claim_stream_state(s->streamid, s->tracker_slot);
                if (s->state_slot != CLAIM_STREAM_IN_USE) break;
            }

            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000;
            nanosleep(&ts, NULL);
        } while (relay_running() && s->state_slot == CLAIM_STREAM_IN_USE && now_ms() < deadline);

        if (s->state_slot == CLAIM_STREAM_IN_USE) {
            set_last_errorf("Duplicate publisher rejected for active streamid=%s", s->streamid);
            relay_logf("Duplicate publisher rejected for active streamid=%s\n", s->streamid);
            return -1;
        }
    }
    if (s->state_slot < 0 && s->streamid[0]) {
        set_last_errorf("No stream state slot available for streamid=%s", s->streamid);
        relay_logf("No stream state slot available for streamid=%s\n", s->streamid);
        return -1;
    }
    return 0;
}

/* ---- the session ---- */

/* A send failure almost always means the transport-level connect succeeded (SRS
 * accepts the SRT handshake) but the stream was then rejected or torn down by
 * SRS's on_publish hook. Without backoff, a persistently unauthorized stream key
 * reconnects and fails again in a tight loop bounded only by handshake + hook
 * round-trip time (seen in production at 10-40 ms intervals, sustained
 * indefinitely). So it goes through the same backoff table as a failed connect. */
static void handle_output_send_failure(session_t *s) {
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&s->target->out_addr, out_ip, sizeof out_ip, &out_port);
    set_last_errorf("srt_sendmsg2: %s", srt_getlasterror_str());
    set_stream_errorf(s->state_slot, "Relay output error to %s:%d (input peer %s:%d): %s", out_ip,
                      out_port, s->peer_ip, s->peer_port, srt_getlasterror_str());
    set_stream_output_connected(s->state_slot, 0);
    relay_logf("srt_sendmsg2 failed peer=%s:%d streamid=%s error=\"%s\"\n", s->peer_ip,
               s->peer_port, streamid_or_empty(s->streamid), srt_getlasterror_str());
    close_output(s->tracker_slot, s->srt_out);
    s->srt_out = SRT_INVALID_SOCK;

    record_output_failure(s, "send");
}

static void *session_main(void *arg) {
    session_args_t *args = arg;
    SRTSOCKET conn = args->conn;
    session_t s;
    memset(&s, 0, sizeof s);
    s.target = args->target;
    s.tracker_slot = args->tracker_slot;
    s.state_slot = -1;
    s.srt_out = SRT_INVALID_SOCK;
    snprintf(s.peer_ip, sizeof s.peer_ip, "%s", args->peer_ip[0] ? args->peer_ip : "?");
    s.peer_port = args->peer_port;
    free(args);
    retry_init(&s.retry);
    long long started_at_ms = now_ms();
    long long bytes_forwarded = 0;
    long long packets_forwarded = 0;
    const char *close_reason = "input_disconnected";

    if (!get_streamid(conn, s.streamid, sizeof s.streamid)) {
        if (!get_param(s.target->out_query, "streamid", s.streamid, sizeof s.streamid)) {
            s.streamid[0] = '\0';
        }
    }
    tracker_set_streamid(s.tracker_slot, s.streamid);

    int input_bonded = (conn & SRTGROUP_MASK) != 0;
    int recv_timeout_ms = RECV_TIMEOUT_MS;
    if (srt_setsockflag(conn, SRTO_RCVTIMEO, &recv_timeout_ms, sizeof recv_timeout_ms) ==
        SRT_ERROR) {
        relay_logf("SRTO_RCVTIMEO rejected for peer=%s:%d: %s\n", s.peer_ip, s.peer_port,
                   srt_getlasterror_str());
    }
    relay_logf("Accepted %s SRT source peer=%s:%d streamid=%s\n", input_bonded ? "bonded" : "plain",
               s.peer_ip, s.peer_port, streamid_or_empty(s.streamid));

    if (acquire_stream_state(&s) != 0) {
        if (tracker_take_input(s.tracker_slot, conn) != SRT_INVALID_SOCK) srt_close(conn);
        tracker_unregister(s.tracker_slot);
        return NULL;
    }

    set_stream_input_info(s.state_slot, input_bonded, s.peer_ip, s.peer_port);
    connect_srt_output_with_retry(&s);

    char buf[CHUNK];
    while (relay_running() && stream_input_still_connected(conn)) {
        if (s.srt_out == SRT_INVALID_SOCK && retry_due(&s.retry, now_ms())) {
            connect_srt_output_with_retry(&s);
        }
        if (retry_check_healthy(&s.retry, now_ms())) set_stream_retry_failures(s.state_slot, 0);

        SRT_MSGCTRL mc = srt_msgctrl_default;
        int r = srt_recvmsg2(conn, buf, CHUNK, &mc);
        if (r == SRT_ERROR) {
            int err = srt_getlasterror(NULL);
            /* No data within RECV_TIMEOUT_MS (libsrt reports it as EASYNCRCV): go round
             * again so the loop conditions are re-checked. */
            if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
                update_stream_srt_counters(s.state_slot, s.tracker_slot, conn, s.srt_out, 0);
                continue;
            }
            if (err != SRT_ECONNLOST && err != SRT_ENOCONN) {
                close_reason = "input_error";
                set_last_errorf("srt_recvmsg2: %s", srt_getlasterror_str());
                set_stream_errorf(s.state_slot, "Input error from peer %s:%d: %s", s.peer_ip,
                                  s.peer_port, srt_getlasterror_str());
                relay_logf("srt_recvmsg2 failed peer=%s:%d streamid=%s error=\"%s\"\n", s.peer_ip,
                           s.peer_port, streamid_or_empty(s.streamid), srt_getlasterror_str());
            }
            break;
        }
        if (r <= 0) continue;
        record_stream_input_progress(s.state_slot);
        update_stream_srt_counters(s.state_slot, s.tracker_slot, conn, s.srt_out, 0);

        if (s.srt_out != SRT_INVALID_SOCK) {
            if (srt_sendmsg2(s.srt_out, buf, r, NULL) == SRT_ERROR) {
                handle_output_send_failure(&s);
                continue;
            }
            record_stream_forward_progress(s.state_slot, r);
            bytes_forwarded += r;
            packets_forwarded++;
        }
    }

    if (!relay_running()) close_reason = "shutdown";
    update_stream_srt_counters(s.state_slot, s.tracker_slot, conn, s.srt_out, 1);
    if (s.srt_out != SRT_INVALID_SOCK) close_output(s.tracker_slot, s.srt_out);
    remove_stream_state(s.state_slot);
    relay_logf("Connection closed peer=%s:%d streamid=%s reason=%s duration_ms=%lld "
               "bytes_forwarded=%lld packets_forwarded=%lld\n",
               s.peer_ip, s.peer_port, streamid_or_empty(s.streamid), close_reason,
               now_ms() - started_at_ms, bytes_forwarded, packets_forwarded);

    if (tracker_take_input(s.tracker_slot, conn) != SRT_INVALID_SOCK) srt_close(conn);
    tracker_unregister(s.tracker_slot);
    return NULL;
}

void session_spawn(SRTSOCKET conn, const output_target_t *target,
                   const struct sockaddr_storage *peer) {
    session_args_t *args = calloc(1, sizeof *args);
    if (!args) {
        set_last_errorf("calloc(session): %s", strerror(errno));
        relay_logf("calloc(session): %s\n", strerror(errno));
        srt_close(conn);
        return;
    }
    args->conn = conn;
    args->target = target;
    format_leg_addr(peer, args->peer_ip, sizeof args->peer_ip, &args->peer_port);
    args->tracker_slot = tracker_register(conn);
    if (args->tracker_slot < 0) {
        set_last_errorf("Too many active sessions; rejecting connection");
        relay_logf("Too many active sessions; rejecting connection\n");
        srt_close(conn);
        free(args);
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, session_main, args) != 0) {
        set_last_errorf("pthread_create(session) failed");
        relay_logf("pthread_create failed\n");
        tracker_unregister(args->tracker_slot);
        srt_close(conn);
        free(args);
        return;
    }
    pthread_detach(tid);
}
