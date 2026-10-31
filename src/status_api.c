#include "status_api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <srt/srt.h>

#include "log.h"
#include "ratelimit.h"
#include "relay_common.h"
#include "runtime.h"
#include "status_json.h"
#include "stream_state.h"
#include "timed_send.h"

/* The status port has no authentication, and one thread serves it. A client that
 * connects and sends nothing, or never reads the reply, must not hold that thread:
 * the endpoint would stall and so would shutdown, which joins it. Measured before
 * the limit: one idle local connection stopped the endpoint, and SIGTERM did not
 * stop the relay within 13 s. A real client sends its request at once. The limit
 * is for the request, and again for the whole reply. */
#define STATUS_CLIENT_TIMEOUT_S 2
/* The log lines about such clients: a poller stuck in a loop could otherwise write
 * one every STATUS_CLIENT_TIMEOUT_S, for as long as it runs. */
#define STATUS_LOG_MAX_PER_WINDOW 3
#define STATUS_LOG_WINDOW_MS 60000

static int g_status_port = 8081;
static long long g_started_at_ms = 0;

/* Returns 0 when the whole response was sent, -1 when it was not (the client
 * stopped reading, or the connection failed). The response is built in memory first
 * (at most one entry per session table slot) so that sending it has one deadline;
 * through stdio on the socket every buffer flush would wait for the client again. */
static int write_status_response(int client_fd) {
    /* The whole session table, about 1.5 MB: static, not on the stack. A thread's stack
     * is sized from the process's stack limit, and under a limit of 1 MB the first status
     * request overflowed it and crashed the relay. Only the status thread calls this, one
     * client at a time, so the buffer is not shared. */
    static relay_stream_state_t states[MAX_ACTIVE_SESSIONS];
    char last_error[LAST_ERROR_MAX];
    long long updated_at_ms = wall_ms();

    stream_state_snapshot(states, last_error, sizeof last_error);

    char *body = NULL;
    size_t body_len = 0;
    FILE *f = open_memstream(&body, &body_len);
    if (!f) return -1;

    fputs("HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Cache-Control: no-store\r\n"
          "Connection: close\r\n"
          "\r\n",
          f);

    status_snapshot_t snap = {
        .pid = (long long)getpid(),
        .started_at_ms = g_started_at_ms,
        .updated_at_ms = updated_at_ms,
        .last_error = last_error,
        .states = states,
    };
    status_json_write(f, &snap);
    if (fclose(f) != 0) {
        free(body);
        return -1;
    }
    int rc = send_all_within(client_fd, body, body_len, STATUS_CLIENT_TIMEOUT_S * 1000);
    free(body);
    return rc;
}

static void *status_http_main(void *arg) {
    (void)arg;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        set_last_errorf("status socket(): %s", strerror(errno));
        perror("status socket");
        return NULL;
    }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((unsigned short)g_status_port);

    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        set_last_errorf("status bind :%d failed: %s", g_status_port, strerror(errno));
        relay_logf("status bind :%d failed: %s\n", g_status_port, strerror(errno));
        close(srv);
        return NULL;
    }
    if (listen(srv, 16) < 0) {
        set_last_errorf("status listen failed: %s", strerror(errno));
        relay_logf("status listen failed: %s\n", strerror(errno));
        close(srv);
        return NULL;
    }

    relay_logf("Status HTTP listening on 127.0.0.1:%d\n", g_status_port);

    rate_window_t client_log = {0}; /* only this thread uses it */
    while (relay_running()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select(srv + 1, &rfds, NULL, NULL, &tv);

        long long suppressed =
            rate_window_take_suppressed(&client_log, now_ms(), STATUS_LOG_WINDOW_MS);
        if (suppressed > 0) {
            relay_logf("Status client problems suppressed count=%lld in the last %d s\n",
                       suppressed, STATUS_LOG_WINDOW_MS / 1000);
        }
        if (ready <= 0) continue;

        int client = accept(srv, NULL, NULL);
        if (client < 0) continue;

        struct timeval client_tv = {STATUS_CLIENT_TIMEOUT_S, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof client_tv);

        char req[1024];
        ssize_t req_len = recv(client, req, sizeof req, 0);
        const char *problem = NULL;
        if (req_len < 0) {
            problem = "sent no request";
        } else if (req_len > 0 && write_status_response(client) != 0) {
            problem = "did not read the response";
        } /* req_len == 0: the client closed without a request (a port probe); nothing to say */
        if (problem && rate_window_allow(&client_log, now_ms(), STATUS_LOG_MAX_PER_WINDOW,
                                         STATUS_LOG_WINDOW_MS)) {
            relay_logf("Status client %s within %d s, dropped\n", problem, STATUS_CLIENT_TIMEOUT_S);
        }
        close(client);
    }

    close(srv);
    return NULL;
}

int status_server_start(int port, long long started_at_ms, pthread_t *tid) {
    g_status_port = port;
    g_started_at_ms = started_at_ms;
    return pthread_create(tid, NULL, status_http_main, NULL) == 0 ? 0 : -1;
}
