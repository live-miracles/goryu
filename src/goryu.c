/*
 * goryu.c
 *
 * Long-running SRT bonding ingress relay.
 *
 * Listens on one encoder-facing SRT port with SRTO_GROUPCONNECT enabled,
 * accepts bonded/redundant SRT groups, and forwards the deduplicated MPEG-TS
 * payload into a downstream SRT output. The incoming caller streamid is
 * copied to the outgoing SRT connection, so a single listener can serve many
 * logical streams.
 *
 * Usage:
 *   goryu <config.json>
 *   goryu <srt-input-uri> <srt-output-uri>
 *
 * Example JSON:
 *   {
 *     "input_host": "0.0.0.0",
 *     "input_port": 10081,
 *     "output_host": "127.0.0.1",
 *     "output_port": 10080,
 *     "status_port": 8081,
 *     "passphrase": "secret-value"
 *   }
 *
 * This file is the entry point: argument and config handling, the listener and
 * the accept loop. The rest lives in the modules listed in AGENTS.md.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <srt/srt.h>

#include "log.h"
#include "net.h"
#include "relay_options.h"
#include "runtime.h"
#include "session.h"
#include "srt_opts.h"
#include "status_api.h"
#include "stream_state.h"
#include "tracker.h"

#define LISTEN_BACKLOG 64
#define MAX_EVENTS 16

#ifndef RELAY_VERSION
#define RELAY_VERSION "dev"
#endif

static const char *RELAY_VERSION_STRING = RELAY_VERSION;

/* Creates, configures, binds and starts listening on the encoder-facing socket.
 * Returns SRT_INVALID_SOCK after logging why. */
static SRTSOCKET open_listener(const char *host, int port, const char *query) {
    SRTSOCKET srv = srt_create_socket();
    if (srv == SRT_INVALID_SOCK) {
        set_last_errorf("srt_create_socket: %s", srt_getlasterror_str());
        relay_logf("srt_create_socket: %s\n", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }

    char opt_error[256];
    if (apply_srt_opts(srv, query, opt_error, sizeof opt_error) != 0) {
        set_last_errorf("input socket: %s", opt_error);
        relay_logf("input socket: %s\n", opt_error);
        srt_close(srv);
        return SRT_INVALID_SOCK;
    }

    struct sockaddr_in sa;
    if (resolve_input_bind_addr(host, port, &sa) < 0) {
        srt_close(srv);
        return SRT_INVALID_SOCK;
    }

    if (srt_bind(srv, (struct sockaddr *)&sa, sizeof sa) == SRT_ERROR) {
        set_last_errorf("srt_bind %s:%d: %s", host, port, srt_getlasterror_str());
        relay_logf("srt_bind %s:%d: %s\n", host, port, srt_getlasterror_str());
        srt_close(srv);
        return SRT_INVALID_SOCK;
    }
    if (srt_listen(srv, LISTEN_BACKLOG) == SRT_ERROR) {
        set_last_errorf("srt_listen: %s", srt_getlasterror_str());
        relay_logf("srt_listen: %s\n", srt_getlasterror_str());
        srt_close(srv);
        return SRT_INVALID_SOCK;
    }
    return srv;
}

/* Accepts connections until the process is asked to stop; each one becomes a
 * session thread. */
static void accept_loop(SRTSOCKET srv, int ep, const output_target_t *target) {
    while (relay_running()) {
        SRT_EPOLL_EVENT ev[MAX_EVENTS];
        int n = srt_epoll_uwait(ep, ev, MAX_EVENTS, 1000);
        if (n <= 0) continue;

        for (int i = 0; i < n && relay_running(); ++i) {
            if (ev[i].fd != srv) continue;

            struct sockaddr_storage peer;
            int plen = (int)sizeof peer;
            SRTSOCKET conn = srt_accept(srv, (struct sockaddr *)&peer, &plen);
            if (conn == SRT_INVALID_SOCK) {
                set_last_errorf("srt_accept: %s", srt_getlasterror_str());
                relay_logf("srt_accept: %s\n", srt_getlasterror_str());
                continue;
            }
            session_spawn(conn, target, &peer);
        }
    }
}

/* Stops the relay, in the order that matters: ask everything to stop, close the
 * listener so no session starts, close every session's sockets and wait for the
 * session threads, then join the status thread and release libsrt. ep is -1 when no
 * epoll was created. Every way out of main after the listener is open goes through
 * here, so the order is written once. */
static void shutdown_relay(SRTSOCKET srv, int ep, int status_thread_started, pthread_t status_tid) {
    relay_request_stop();
    if (ep >= 0) srt_epoll_release(ep);
    srt_close(srv);
    tracker_close_all();
    tracker_wait_all();
    if (status_thread_started) pthread_join(status_tid, NULL);
    srt_cleanup();
}

int main(int argc, char *argv[]) {
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        puts(RELAY_VERSION_STRING);
        return 0;
    }

    if (argc != 2 && argc != 3) {
        fprintf(stderr,
                "Usage: %s [--version]\n"
                "   or: %s <config.json>\n"
                "   or: %s <srt-input-uri> <srt-output-uri>\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    relay_options_t opts;
    char err[512];
    if (parse_relay_options(argc, argv, getenv("SRT_BONDING_STATUS_PORT"), &opts, err, sizeof err) <
        0) {
        relay_logf("%s\n", err);
        return 1;
    }
    /* Ignored, not refused, so a config with an extra key still starts and a status
     * port variable that is not a port does not stop the relay; but a misspelled
     * optional key changes what the relay does, so they are said. */
    if (opts.unknown_key_count > 0) {
        relay_logf("Config file %s: ignoring %d unknown key(s), the first is \"%s\"; "
                   "check the spelling\n",
                   argv[1], opts.unknown_key_count, opts.unknown_key);
    }
    if (opts.status_port_env_ignored) {
        relay_logf("SRT_BONDING_STATUS_PORT is not a port number (1 to 65535), ignored; "
                   "the status port is %d\n",
                   opts.status_port);
    }

    output_target_t target;
    memset(&target, 0, sizeof target);
    snprintf(target.out_query, sizeof target.out_query, "%s", opts.out_query);
    if (resolve_output_addr(opts.out_host, opts.out_port, &target.out_addr, &target.out_addrlen) <
        0) {
        return 1;
    }

    long long started_at_ms = now_ms();

    if (relay_install_signal_handlers() != 0) {
        relay_logf("install signal handlers failed: %s\n", strerror(errno));
        return 1;
    }
    tracker_init();

    srt_startup();
    /* Cap libsrt's own log volume so a receive-queue backlog or other internal
     * condition can't flood the process's log output badly enough to starve the
     * host it runs on. */
    log_install_srt_cap();

    SRTSOCKET srv = open_listener(opts.in_host, opts.in_port, opts.in_query);
    if (srv == SRT_INVALID_SOCK) {
        srt_cleanup();
        return 1;
    }

    relay_logf("Listening on SRT (bonded and plain) %s:%d (backlog=%d) -> %s\n", opts.in_host,
               opts.in_port, LISTEN_BACKLOG, opts.out_uri_redacted);

    pthread_t status_tid;
    int status_thread_started =
        status_server_start(opts.status_port, started_at_ms, &status_tid) == 0;
    if (!status_thread_started) {
        set_last_errorf("pthread_create(status) failed");
        relay_logf("pthread_create(status) failed\n");
    }

    int ep = srt_epoll_create();
    if (ep < 0) {
        set_last_errorf("srt_epoll_create: %s", srt_getlasterror_str());
        relay_logf("srt_epoll_create: %s\n", srt_getlasterror_str());
        shutdown_relay(srv, -1, status_thread_started, status_tid);
        return 1;
    }
    int ep_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(ep, srv, &ep_events) == SRT_ERROR) {
        set_last_errorf("srt_epoll_add_usock: %s", srt_getlasterror_str());
        relay_logf("srt_epoll_add_usock: %s\n", srt_getlasterror_str());
        shutdown_relay(srv, ep, status_thread_started, status_tid);
        return 1;
    }

    accept_loop(srv, ep, &target);

    shutdown_relay(srv, ep, status_thread_started, status_tid);
    return 0;
}
