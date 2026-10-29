/*
 * closing_sink.c - a test downstream that accepts the SRT handshake and then drops
 * the connection.
 *
 * This is what a downstream looks like when it accepts the connection and then
 * rejects the stream, for example a publish hook refusing an unauthorized stream
 * key. ffmpeg as a listener cannot do it, so scripts/test-e2e.sh uses this. It
 * listens on one port, accepts every caller, keeps the connection open for
 * --hold-ms and closes it. It never reads, so everything the relay sends in that
 * time is buffered and the relay only learns of the close afterwards. Built against
 * the same libsrt as Goryu; see scripts/test-e2e.sh.
 *
 *   closing_sink --port 24222 [--hold-ms 50]
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <srt/srt.h>

static void die(const char *what) {
    fprintf(stderr, "closing_sink: %s: %s\n", what, srt_getlasterror_str());
    fflush(stderr);
    _exit(2); /* exit() runs libsrt's static destructors with its threads still live */
}

int main(int argc, char **argv) {
    int port = 0;
    int hold_ms = 50;

    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--port")) {
            port = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "--hold-ms")) {
            hold_ms = atoi(argv[i + 1]);
        } else {
            fprintf(stderr, "closing_sink: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (port <= 0) {
        fprintf(stderr, "usage: closing_sink --port PORT [--hold-ms MS]\n");
        return 2;
    }

    if (srt_startup() == SRT_ERROR) die("srt_startup");
    SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) die("srt_create_socket");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (srt_bind(listener, (struct sockaddr *)&sa, sizeof sa) == SRT_ERROR) die("srt_bind");
    if (srt_listen(listener, 5) == SRT_ERROR) die("srt_listen");

    for (;;) {
        SRTSOCKET conn = srt_accept(listener, NULL, NULL);
        if (conn == SRT_INVALID_SOCK) die("srt_accept");
        fprintf(stderr, "closing_sink: accepted, closing in %d ms\n", hold_ms);
        usleep((useconds_t)hold_ms * 1000);
        srt_close(conn);
    }
}
