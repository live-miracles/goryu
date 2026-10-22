/*
 * bonded_caller.c - a test encoder that sends one stream as a bonded SRT group.
 *
 * ffmpeg cannot open a socket group, so the end-to-end tests use this instead. It
 * reads MPEG-TS from stdin (pipe ffmpeg into it), sends it over a broadcast
 * group with one leg per --locals address, and can drop or add a leg part way
 * through. Each leg is bound to a different local address (127.0.0.1 and
 * 127.0.0.2 are both loopback), which gives the relay two distinct peers without
 * needing a second network interface. Built against the same libsrt as Goryu; see
 * scripts/test-e2e.sh.
 *
 *   bonded_caller --port 24011 --streamid demo/key --locals 127.0.0.1,127.0.0.2
 *                 [--host 127.0.0.1] [--passphrase PW] [--latency MS]
 *                 [--drop-leg INDEX --drop-after SECONDS]
 *                 [--add-local IP --add-after SECONDS]
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <srt/srt.h>

#define TS_CHUNK 1316 /* 7 TS packets: the default SRT live payload size */
#define MAX_LEGS 8

static void die(const char *what) {
    fprintf(stderr, "bonded_caller: %s: %s\n", what, srt_getlasterror_str());
    fflush(stderr);
    _exit(2); /* exit() runs libsrt's static destructors with its threads still live */
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static struct sockaddr_in make_addr(const char *ip, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        fprintf(stderr, "bonded_caller: bad address %s\n", ip);
        exit(2);
    }
    return sa;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *streamid = "";
    const char *passphrase = "";
    const char *locals = "127.0.0.1,127.0.0.2";
    const char *add_local = NULL;
    int port = 0;
    int latency = -1;
    int drop_leg = -1;
    double drop_after = 0;
    double add_after = 0;

    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i];
        const char *v = argv[i + 1];
        if (!strcmp(k, "--host"))
            host = v;
        else if (!strcmp(k, "--port"))
            port = atoi(v);
        else if (!strcmp(k, "--streamid"))
            streamid = v;
        else if (!strcmp(k, "--passphrase"))
            passphrase = v;
        else if (!strcmp(k, "--locals"))
            locals = v;
        else if (!strcmp(k, "--latency"))
            latency = atoi(v);
        else if (!strcmp(k, "--drop-leg"))
            drop_leg = atoi(v);
        else if (!strcmp(k, "--drop-after"))
            drop_after = atof(v);
        else if (!strcmp(k, "--add-local"))
            add_local = v;
        else if (!strcmp(k, "--add-after"))
            add_after = atof(v);
        else {
            fprintf(stderr, "bonded_caller: unknown option %s\n", k);
            return 2;
        }
    }
    if (port <= 0) {
        fprintf(stderr, "bonded_caller: --port is required\n");
        return 2;
    }

    srt_startup();
    SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    if (group == SRT_INVALID_SOCK) die("srt_create_group");

    /* Groups are live-only, so no SRTO_TRANSTYPE (libsrt rejects it on a group). */
    if (streamid[0]) srt_setsockflag(group, SRTO_STREAMID, streamid, (int)strlen(streamid));
    if (latency >= 0) srt_setsockflag(group, SRTO_LATENCY, &latency, sizeof latency);
    if (passphrase[0]) {
        int keylen = 16;
        if (srt_setsockflag(group, SRTO_PASSPHRASE, passphrase, (int)strlen(passphrase)) ==
            SRT_ERROR)
            die("SRTO_PASSPHRASE");
        srt_setsockflag(group, SRTO_PBKEYLEN, &keylen, sizeof keylen);
    }

    struct sockaddr_in target = make_addr(host, port);
    struct sockaddr_in src[MAX_LEGS];
    SRT_SOCKGROUPCONFIG cfg[MAX_LEGS];
    int legs = 0;
    char list[256];
    snprintf(list, sizeof list, "%s", locals);
    for (char *tok = strtok(list, ","); tok && legs < MAX_LEGS; tok = strtok(NULL, ",")) {
        src[legs] = make_addr(tok, 0);
        cfg[legs] = srt_prepare_endpoint((struct sockaddr *)&src[legs], (struct sockaddr *)&target,
                                         sizeof target);
        ++legs;
    }
    if (legs == 0) {
        fprintf(stderr, "bonded_caller: --locals needs at least one address\n");
        return 2;
    }

    if (srt_connect_group(group, cfg, legs) == SRT_ERROR) die("srt_connect_group");
    fprintf(stderr, "bonded_caller: connected, %d leg(s)\n", legs);

    double start = now_s();
    int dropped = 0, added = 0;
    char buf[TS_CHUNK];
    long long sent = 0;
    for (;;) {
        double t = now_s() - start;
        if (drop_leg >= 0 && drop_leg < legs && !dropped && t >= drop_after) {
            srt_close(cfg[drop_leg].id);
            dropped = 1;
            fprintf(stderr, "bonded_caller: dropped leg %d\n", drop_leg);
        }
        if (add_local && !added && t >= add_after) {
            struct sockaddr_in extra_src = make_addr(add_local, 0);
            SRT_SOCKGROUPCONFIG extra = srt_prepare_endpoint(
                (struct sockaddr *)&extra_src, (struct sockaddr *)&target, sizeof target);
            if (srt_connect_group(group, &extra, 1) == SRT_ERROR) die("add leg");
            added = 1;
            fprintf(stderr, "bonded_caller: added leg %s\n", add_local);
        }

        size_t n = fread(buf, 1, sizeof buf, stdin);
        if (n == 0) break;
        if (srt_sendmsg2(group, buf, (int)n, NULL) == SRT_ERROR) die("srt_sendmsg2");
        ++sent;
    }
    fprintf(stderr, "bonded_caller: sent %lld message(s)\n", sent);

    srt_close(group);
    srt_cleanup();
    return 0;
}
