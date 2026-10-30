#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "net.h"
#include "output.h"
#include "relay_common.h"
#include "relay_config.h"
#include "runtime.h"
#include "srt_output.h"
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

typedef struct session_args {
    SRTSOCKET conn;
    const output_target_t *target;
    int slot;
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
} session_args_t;

/* What one session thread knows about itself. Its helpers take this instead of the
 * same handful of values each time. */
typedef struct session {
    const output_target_t *target;
    /* This session's slot: its sockets and streamid are in the tracker's table and, once
     * it holds a stream, its telemetry is at the same index in the stream state's. */
    int slot;
    char streamid[1024];
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
    output_t output; /* the connection to the downstream */
} session_t;

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

/* ---- the session ---- */

static void *session_main(void *arg) {
    session_args_t *args = arg;
    SRTSOCKET conn = args->conn;
    session_t s;
    memset(&s, 0, sizeof s);
    s.target = args->target;
    s.slot = args->slot;
    snprintf(s.peer_ip, sizeof s.peer_ip, "%s", args->peer_ip[0] ? args->peer_ip : "?");
    s.peer_port = args->peer_port;
    free(args);
    long long started_at_ms = now_ms();
    long long bytes_forwarded = 0;
    long long packets_forwarded = 0;
    const char *close_reason = "input_disconnected";

    if (!get_streamid(conn, s.streamid, sizeof s.streamid)) {
        if (!get_param(s.target->out_query, "streamid", s.streamid, sizeof s.streamid)) {
            s.streamid[0] = '\0';
        }
    }

    int input_bonded = (conn & SRTGROUP_MASK) != 0;
    int recv_timeout_ms = RECV_TIMEOUT_MS;
    if (srt_setsockflag(conn, SRTO_RCVTIMEO, &recv_timeout_ms, sizeof recv_timeout_ms) ==
        SRT_ERROR) {
        relay_logf("SRTO_RCVTIMEO rejected for peer=%s:%d: %s\n", s.peer_ip, s.peer_port,
                   srt_getlasterror_str());
    }
    relay_logf("Accepted %s SRT source peer=%s:%d streamid=%s\n", input_bonded ? "bonded" : "plain",
               s.peer_ip, s.peer_port, streamid_or_empty(s.streamid));

    if (tracker_acquire_stream(s.slot, s.streamid) != 0) {
        tracker_release(s.slot, conn);
        return NULL;
    }

    set_stream_input_info(s.slot, input_bonded, s.peer_ip, s.peer_port);
    output_init(&s.output, &srt_output_transport, s.target, s.slot, s.streamid, s.peer_ip,
                s.peer_port);
    output_service(&s.output);

    char buf[CHUNK];
    while (relay_running() && stream_input_still_connected(conn)) {
        output_service(&s.output);

        SRT_MSGCTRL mc = srt_msgctrl_default;
        int r = srt_recvmsg2(conn, buf, CHUNK, &mc);
        if (r == SRT_ERROR) {
            int err = srt_getlasterror(NULL);
            /* No data within RECV_TIMEOUT_MS (libsrt reports it as EASYNCRCV): go round
             * again so the loop conditions are re-checked. */
            if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
                update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 0);
                continue;
            }
            if (err != SRT_ECONNLOST && err != SRT_ENOCONN) {
                close_reason = "input_error";
                set_last_errorf("srt_recvmsg2: %s", srt_getlasterror_str());
                set_stream_errorf(s.slot, "Input error from peer %s:%d: %s", s.peer_ip, s.peer_port,
                                  srt_getlasterror_str());
                relay_logf("srt_recvmsg2 failed peer=%s:%d streamid=%s error=\"%s\"\n", s.peer_ip,
                           s.peer_port, streamid_or_empty(s.streamid), srt_getlasterror_str());
            }
            break;
        }
        if (r <= 0) continue;
        record_stream_input_progress(s.slot);
        update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 0);

        if (output_send(&s.output, buf, r) == OUTPUT_SENT) {
            record_stream_forward_progress(s.slot, r);
            bytes_forwarded += r;
            packets_forwarded++;
        }
    }

    if (!relay_running()) close_reason = "shutdown";
    update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 1);
    output_close(&s.output);
    relay_logf("Connection closed peer=%s:%d streamid=%s reason=%s duration_ms=%lld "
               "bytes_forwarded=%lld packets_forwarded=%lld\n",
               s.peer_ip, s.peer_port, streamid_or_empty(s.streamid), close_reason,
               now_ms() - started_at_ms, bytes_forwarded, packets_forwarded);

    tracker_release(s.slot, conn);
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
    args->slot = tracker_register(conn);
    if (args->slot < 0) {
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
        tracker_release(args->slot, conn);
        free(args);
        return;
    }
    pthread_detach(tid);
}
