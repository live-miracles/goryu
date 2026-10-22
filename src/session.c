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

typedef struct session_args {
    SRTSOCKET conn;
    const output_target_t *target;
    int tracker_slot;
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
} session_args_t;

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
static void report_connect_failure(const output_target_t *target, SRTSOCKET srt_out,
                                   const char *streamid, char *error_out, size_t error_out_sz) {
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&target->out_addr, out_ip, sizeof out_ip, &out_port);
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
                 out_port, streamid_or_empty(streamid), error_text, detail);
    }
    set_last_errorf("srt_connect target=%s:%d streamid=%s: %s%s", out_ip, out_port,
                    streamid_or_empty(streamid), error_text, detail);
    relay_logf("srt_connect failed target=%s:%d streamid=%s error=\"%s\"%s\n", out_ip, out_port,
               streamid_or_empty(streamid), error_text, log_suffix);
}

static void close_output(int tracker_slot, SRTSOCKET srt_out) {
    if (tracker_take_output(tracker_slot, srt_out) != SRT_INVALID_SOCK) {
        srt_close(srt_out);
    }
}

static SRTSOCKET connect_srt_output(const output_target_t *target, const char *streamid,
                                    int tracker_slot, char *error_out, size_t error_out_sz) {
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
    tracker_set_output_socket(tracker_slot, srt_out);

    SRT_TRANSTYPE tt = SRTT_LIVE;
    srt_setsockflag(srt_out, SRTO_TRANSTYPE, &tt, sizeof tt);
    char opt_error[256];
    if (apply_srt_opts(srt_out, target->out_query, opt_error, sizeof opt_error) != 0) {
        if (error_out && error_out_sz > 0) {
            snprintf(error_out, error_out_sz, "output socket: %s", opt_error);
        }
        set_last_errorf("output socket: %s", opt_error);
        relay_logf("output socket: %s\n", opt_error);
        close_output(tracker_slot, srt_out);
        return SRT_INVALID_SOCK;
    }

    if (streamid && streamid[0]) {
        srt_setsockflag(srt_out, SRTO_STREAMID, streamid, (int)strlen(streamid));
    }

    int cto = 3000;
    srt_setsockflag(srt_out, SRTO_CONNTIMEO, &cto, sizeof cto);

    if (srt_connect(srt_out, (struct sockaddr *)&target->out_addr, (int)target->out_addrlen) ==
        SRT_ERROR) {
        report_connect_failure(target, srt_out, streamid, error_out, error_out_sz);
        close_output(tracker_slot, srt_out);
        return SRT_INVALID_SOCK;
    }
    if (!tracker_is_current(tracker_slot, SRT_INVALID_SOCK, srt_out)) {
        return SRT_INVALID_SOCK;
    }

    return srt_out;
}

/* Tries once. On failure it counts the failure, records why in the stream's
 * lastError and sets *next_retry_at_ms from the backoff table. */
static SRTSOCKET connect_srt_output_with_retry(const output_target_t *target, const char *streamid,
                                               int state_slot, int tracker_slot,
                                               long long *next_retry_at_ms) {
    char output_error[512];
    SRTSOCKET srt_out =
        connect_srt_output(target, streamid, tracker_slot, output_error, sizeof output_error);
    if (srt_out != SRT_INVALID_SOCK) {
        set_stream_output_connected(state_slot, 1);
        if (next_retry_at_ms) *next_retry_at_ms = 0;
        return srt_out;
    }

    increment_stream_retry_failures(state_slot);
    int failures = get_stream_retry_failures(state_slot);
    int delay_ms = retry_delay_ms(failures);
    set_stream_output_connected(state_slot, 0);
    if (output_error[0]) {
        set_stream_errorf(state_slot,
                          "Failed to publish to downstream output: %s; retrying in %d ms",
                          output_error, delay_ms);
    } else {
        set_stream_errorf(state_slot, "Failed to publish to downstream output; retrying in %d ms",
                          delay_ms);
    }
    relay_logf("Output publish retry streamid=%s failures=%d retry_ms=%d\n",
               streamid_or_empty(streamid), failures, delay_ms);
    if (next_retry_at_ms) *next_retry_at_ms = now_ms() + delay_ms;
    return SRT_INVALID_SOCK;
}

/* ---- duplicate publishers ---- */

/* Claims the stream's state for this session. A streamid that is already active
 * waits up to DUPLICATE_TAKEOVER_WAIT_MS: if the holder's input has been silent
 * for DUPLICATE_TAKEOVER_STALE_MS it is closed and replaced, otherwise this
 * session is rejected. Returns 0 to carry on, with *state_slot set (-1 for an
 * empty streamid, which has no state), or -1 after logging why the session
 * must end. */
static int acquire_stream_state(const char *streamid, int tracker_slot, int *state_slot) {
    *state_slot = claim_stream_state(streamid, tracker_slot);

    if (*state_slot == -2) {
        long long deadline = now_ms() + DUPLICATE_TAKEOVER_WAIT_MS;
        int takeover_started = 0;
        do {
            long long now = now_ms();
            long long input_age_ms = 0;
            int stream_present = stream_input_age_ms(streamid, now, &input_age_ms) == 0;

            if (!stream_present) {
                *state_slot = claim_stream_state(streamid, tracker_slot);
                if (*state_slot != -2) break;
            } else if (input_age_ms >= DUPLICATE_TAKEOVER_STALE_MS) {
                if (!takeover_started) {
                    set_last_errorf("Duplicate publisher takeover for stale streamid=%s", streamid);
                    relay_logf("Duplicate publisher takeover for stale streamid=%s\n", streamid);
                    tracker_close_streamid(streamid, tracker_slot);
                    takeover_started = 1;
                }
                *state_slot = claim_stream_state(streamid, tracker_slot);
                if (*state_slot != -2) break;
            }

            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000;
            nanosleep(&ts, NULL);
        } while (relay_running() && *state_slot == -2 && now_ms() < deadline);

        if (*state_slot == -2) {
            set_last_errorf("Duplicate publisher rejected for active streamid=%s", streamid);
            relay_logf("Duplicate publisher rejected for active streamid=%s\n", streamid);
            return -1;
        }
    }
    if (*state_slot < 0 && streamid[0]) {
        set_last_errorf("No stream state slot available for streamid=%s", streamid);
        relay_logf("No stream state slot available for streamid=%s\n", streamid);
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
static void handle_output_send_failure(const output_target_t *target, int state_slot,
                                       int tracker_slot, SRTSOCKET *srt_out, const char *streamid,
                                       const char *peer_ip, int peer_port,
                                       long long *next_output_retry_at_ms) {
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&target->out_addr, out_ip, sizeof out_ip, &out_port);
    set_last_errorf("srt_sendmsg2: %s", srt_getlasterror_str());
    set_stream_errorf(state_slot, "Relay output error to %s:%d (input peer %s:%d): %s", out_ip,
                      out_port, peer_ip, peer_port, srt_getlasterror_str());
    set_stream_output_connected(state_slot, 0);
    relay_logf("srt_sendmsg2 failed peer=%s:%d streamid=%s error=\"%s\"\n", peer_ip, peer_port,
               streamid_or_empty(streamid), srt_getlasterror_str());
    close_output(tracker_slot, *srt_out);
    *srt_out = SRT_INVALID_SOCK;

    increment_stream_retry_failures(state_slot);
    int failures = get_stream_retry_failures(state_slot);
    int delay_ms = retry_delay_ms(failures);
    *next_output_retry_at_ms = now_ms() + delay_ms;
    relay_logf("Output send retry streamid=%s failures=%d retry_ms=%d\n",
               streamid_or_empty(streamid), failures, delay_ms);
}

static void *session_main(void *arg) {
    session_args_t *args = (session_args_t *)arg;
    SRTSOCKET conn = args->conn;
    const output_target_t *target = args->target;
    int tracker_slot = args->tracker_slot;
    char peer_ip[INET6_ADDRSTRLEN];
    snprintf(peer_ip, sizeof peer_ip, "%s", args->peer_ip[0] ? args->peer_ip : "?");
    int peer_port = args->peer_port;
    free(args);
    long long started_at_ms = now_ms();
    long long bytes_forwarded = 0;
    long long packets_forwarded = 0;
    const char *close_reason = "input_disconnected";

    char streamid[1024];
    if (!get_streamid(conn, streamid, sizeof streamid)) {
        if (!get_param(target->out_query, "streamid", streamid, sizeof streamid)) {
            streamid[0] = '\0';
        }
    }
    tracker_set_streamid(tracker_slot, streamid);

    int input_bonded = (conn & SRTGROUP_MASK) != 0;
    int recv_timeout_ms = RECV_TIMEOUT_MS;
    if (srt_setsockflag(conn, SRTO_RCVTIMEO, &recv_timeout_ms, sizeof recv_timeout_ms) ==
        SRT_ERROR) {
        relay_logf("SRTO_RCVTIMEO rejected for peer=%s:%d: %s\n", peer_ip, peer_port,
                   srt_getlasterror_str());
    }
    relay_logf("Accepted %s SRT source peer=%s:%d streamid=%s\n", input_bonded ? "bonded" : "plain",
               peer_ip, peer_port, streamid_or_empty(streamid));

    int state_slot = -1;
    if (acquire_stream_state(streamid, tracker_slot, &state_slot) != 0) {
        if (tracker_take_input(tracker_slot, conn) != SRT_INVALID_SOCK) srt_close(conn);
        tracker_unregister(tracker_slot);
        return NULL;
    }

    SRTSOCKET srt_out = SRT_INVALID_SOCK;
    long long next_output_retry_at_ms = 0;
    set_stream_input_info(state_slot, input_bonded, peer_ip, peer_port);
    srt_out = connect_srt_output_with_retry(target, streamid, state_slot, tracker_slot,
                                            &next_output_retry_at_ms);

    char buf[CHUNK];
    while (relay_running() && stream_input_still_connected(conn)) {
        if (srt_out == SRT_INVALID_SOCK && now_ms() >= next_output_retry_at_ms) {
            srt_out = connect_srt_output_with_retry(target, streamid, state_slot, tracker_slot,
                                                    &next_output_retry_at_ms);
        }

        SRT_MSGCTRL mc = srt_msgctrl_default;
        int r = srt_recvmsg2(conn, buf, CHUNK, &mc);
        if (r == SRT_ERROR) {
            int err = srt_getlasterror(NULL);
            /* No data within RECV_TIMEOUT_MS (libsrt reports it as EASYNCRCV): go round
             * again so the loop conditions are re-checked. */
            if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
                update_stream_srt_counters(state_slot, tracker_slot, conn, srt_out, 0);
                continue;
            }
            if (err != SRT_ECONNLOST && err != SRT_ENOCONN) {
                close_reason = "input_error";
                set_last_errorf("srt_recvmsg2: %s", srt_getlasterror_str());
                set_stream_errorf(state_slot, "Input error from peer %s:%d: %s", peer_ip, peer_port,
                                  srt_getlasterror_str());
                relay_logf("srt_recvmsg2 failed peer=%s:%d streamid=%s error=\"%s\"\n", peer_ip,
                           peer_port, streamid_or_empty(streamid), srt_getlasterror_str());
            }
            break;
        }
        if (r <= 0) continue;
        record_stream_input_progress(state_slot);
        update_stream_srt_counters(state_slot, tracker_slot, conn, srt_out, 0);

        if (srt_out != SRT_INVALID_SOCK) {
            if (srt_sendmsg2(srt_out, buf, r, NULL) == SRT_ERROR) {
                handle_output_send_failure(target, state_slot, tracker_slot, &srt_out, streamid,
                                           peer_ip, peer_port, &next_output_retry_at_ms);
                continue;
            }
            record_stream_forward_progress(state_slot, r);
            bytes_forwarded += r;
            packets_forwarded++;
        }
    }

    if (!relay_running()) close_reason = "shutdown";
    update_stream_srt_counters(state_slot, tracker_slot, conn, srt_out, 1);
    if (srt_out != SRT_INVALID_SOCK) close_output(tracker_slot, srt_out);
    remove_stream_state(state_slot);
    relay_logf("Connection closed peer=%s:%d streamid=%s reason=%s duration_ms=%lld "
               "bytes_forwarded=%lld packets_forwarded=%lld\n",
               peer_ip, peer_port, streamid_or_empty(streamid), close_reason,
               now_ms() - started_at_ms, bytes_forwarded, packets_forwarded);

    if (tracker_take_input(tracker_slot, conn) != SRT_INVALID_SOCK) srt_close(conn);
    tracker_unregister(tracker_slot);
    return NULL;
}

void session_spawn(SRTSOCKET conn, const output_target_t *target,
                   const struct sockaddr_storage *peer) {
    session_args_t *args = (session_args_t *)calloc(1, sizeof *args);
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
