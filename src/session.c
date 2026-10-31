#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "log.h"
#include "net.h"
#include "output.h"
#include "relay_common.h"
#include "relay_config.h"
#include "runtime.h"
#include "srt_input.h"
#include "srt_output.h"
#include "stats.h"
#include "stream_state.h"
#include "tracker.h"

#define CHUNK 1456
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
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
    input_t input;   /* the connection to the publisher */
    output_t output; /* the connection to the downstream */
} session_t;

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

    /* A publisher that sets no streamid gets the one in the downstream URI. */
    char default_streamid[1024];
    if (!get_param(s.target->out_query, "streamid", default_streamid, sizeof default_streamid)) {
        default_streamid[0] = '\0';
    }
    input_open(&s.input, &srt_input_transport, conn, s.slot, s.peer_ip, s.peer_port,
               default_streamid);

    if (tracker_acquire_stream(s.slot, s.input.streamid) != 0) {
        tracker_release(s.slot, conn);
        return NULL;
    }

    set_stream_input_info(s.slot, s.input.bonded, s.peer_ip, s.peer_port);
    output_init(&s.output, &srt_output_transport, s.target, s.slot, s.input.streamid, s.peer_ip,
                s.peer_port);
    output_service(&s.output);

    char buf[CHUNK];
    while (relay_running() && input_connected(&s.input)) {
        output_service(&s.output);

        int len = 0;
        input_result_t got = input_recv(&s.input, buf, CHUNK, &len);
        if (got == INPUT_IDLE) {
            /* Go round again so the loop conditions are re-checked. */
            update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 0);
            continue;
        }
        if (got != INPUT_DATA) {
            if (got == INPUT_FAILED) close_reason = "input_error";
            break;
        }
        record_stream_input_progress(s.slot);
        update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 0);

        if (output_send(&s.output, buf, len) == OUTPUT_SENT) {
            record_stream_forward_progress(s.slot, len);
            bytes_forwarded += len;
            packets_forwarded++;
        }
    }

    if (!relay_running()) close_reason = "shutdown";
    update_stream_srt_counters(s.slot, conn, output_socket(&s.output), 1);
    output_close(&s.output);
    relay_logf("Connection closed peer=%s:%d streamid=%s reason=%s duration_ms=%lld "
               "bytes_forwarded=%lld packets_forwarded=%lld\n",
               s.peer_ip, s.peer_port, streamid_or_empty(s.input.streamid), close_reason,
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
