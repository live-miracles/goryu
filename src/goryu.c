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
#include "relay_config.h"
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

typedef struct relay_args {
    char input_uri[2048];
    char output_uri[2048];
    int status_port;
} relay_args_t;

/* The two-URI form takes SRT options straight from the query strings, so apply
 * the same passphrase rule the config file gets. */
static int check_uri_passphrase(const char *query, const char *which) {
    char pass[512];
    char err[256];
    if (get_param(query, "passphrase", pass, sizeof pass) &&
        check_passphrase(pass, err, sizeof err) != 0) {
        relay_logf("Bad %s URI: %s\n", which, err);
        return -1;
    }
    return 0;
}

/* Fills args from the command line: either goryu.json or the two URIs. */
static int read_args(int argc, char *argv[], relay_args_t *args) {
    args->status_port = 8081;

    if (argc == 2) {
        file_config_t file_cfg;
        char err[256];
        if (load_config_file(argv[1], &file_cfg, err, sizeof err) < 0) {
            relay_logf("Bad config file %s: %s\n", argv[1], err);
            return -1;
        }
        if (build_srt_uri(args->input_uri, sizeof args->input_uri, file_cfg.input_host,
                          file_cfg.input_port, file_cfg.passphrase, 1) != 0 ||
            build_srt_uri(args->output_uri, sizeof args->output_uri, file_cfg.output_host,
                          file_cfg.output_port, file_cfg.passphrase, 0) != 0) {
            relay_logf("Bad config file %s: failed to build SRT URI\n", argv[1]);
            return -1;
        }
        args->status_port = file_cfg.status_port;
    } else {
        snprintf(args->input_uri, sizeof args->input_uri, "%s", argv[1]);
        snprintf(args->output_uri, sizeof args->output_uri, "%s", argv[2]);
    }

    const char *status_port = getenv("SRT_BONDING_STATUS_PORT");
    if (status_port && status_port[0]) {
        int port = atoi(status_port);
        if (port > 0 && port <= 65535) args->status_port = port;
    }
    return 0;
}

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

    relay_args_t args;
    if (read_args(argc, argv, &args) < 0) return 1;

    /* A URI can carry a passphrase, so only a redacted copy is ever logged. */
    char logged_input_uri[sizeof args.input_uri];
    char logged_output_uri[sizeof args.output_uri];
    redact_srt_uri(args.input_uri, logged_input_uri, sizeof logged_input_uri);
    redact_srt_uri(args.output_uri, logged_output_uri, sizeof logged_output_uri);

    char in_scheme[8], in_host[256], in_query[1024];
    char out_scheme[8], out_host[256], out_query[1024];
    int in_port, out_port;

    if (parse_uri(args.input_uri, in_scheme, sizeof in_scheme, in_host, sizeof in_host, &in_port,
                  in_query, sizeof in_query) < 0) {
        relay_logf("Bad input URI: %s\n", logged_input_uri);
        return 1;
    }
    if (parse_uri(args.output_uri, out_scheme, sizeof out_scheme, out_host, sizeof out_host,
                  &out_port, out_query, sizeof out_query) < 0) {
        relay_logf("Bad output URI: %s\n", logged_output_uri);
        return 1;
    }
    if (strcmp(in_scheme, "srt") != 0) {
        relay_logf("Input URI must use srt://\n");
        return 1;
    }
    if (strcmp(out_scheme, "srt") != 0) {
        relay_logf("Output URI must use srt://\n");
        return 1;
    }
    if (check_uri_passphrase(in_query, "input") < 0 ||
        check_uri_passphrase(out_query, "output") < 0) {
        return 1;
    }

    output_target_t target;
    memset(&target, 0, sizeof target);
    snprintf(target.out_query, sizeof target.out_query, "%s", out_query);
    if (resolve_output_addr(out_host, out_port, &target.out_addr, &target.out_addrlen) < 0) {
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

    SRTSOCKET srv = open_listener(in_host, in_port, in_query);
    if (srv == SRT_INVALID_SOCK) {
        srt_cleanup();
        return 1;
    }

    relay_logf("Listening on SRT (bonded and plain) %s:%d (backlog=%d) -> %s\n", in_host, in_port,
               LISTEN_BACKLOG, logged_output_uri);

    pthread_t status_tid;
    int status_thread_started =
        status_server_start(args.status_port, started_at_ms, &status_tid) == 0;
    if (!status_thread_started) {
        set_last_errorf("pthread_create(status) failed");
        relay_logf("pthread_create(status) failed\n");
    }

    int ep = srt_epoll_create();
    if (ep < 0) {
        set_last_errorf("srt_epoll_create: %s", srt_getlasterror_str());
        relay_logf("srt_epoll_create: %s\n", srt_getlasterror_str());
        srt_close(srv);
        relay_request_stop();
        if (status_thread_started) pthread_join(status_tid, NULL);
        srt_cleanup();
        return 1;
    }
    int ep_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(ep, srv, &ep_events) == SRT_ERROR) {
        set_last_errorf("srt_epoll_add_usock: %s", srt_getlasterror_str());
        relay_logf("srt_epoll_add_usock: %s\n", srt_getlasterror_str());
        srt_epoll_release(ep);
        srt_close(srv);
        relay_request_stop();
        if (status_thread_started) pthread_join(status_tid, NULL);
        srt_cleanup();
        return 1;
    }

    accept_loop(srv, ep, &target);

    relay_request_stop();
    srt_epoll_release(ep);
    srt_close(srv);
    tracker_close_all();
    tracker_wait_all();
    if (status_thread_started) pthread_join(status_tid, NULL);
    srt_cleanup();
    return 0;
}
