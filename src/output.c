#include "output.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "net.h"
#include "relay_common.h"
#include "runtime.h"
#include "stream_state.h"
#include "tracker.h"

/* Room for the longest connect failure message: a 1023-byte streamid, libsrt's error
 * text and the reject reason. */
#define OUTPUT_ERROR_MAX 2048

void output_init(output_t *o, const output_transport_t *transport, const output_target_t *target,
                 int slot, const char *streamid, const char *peer_ip, int peer_port) {
    memset(o, 0, sizeof *o);
    o->transport = transport;
    o->target = target;
    o->slot = slot;
    snprintf(o->streamid, sizeof o->streamid, "%s", streamid);
    snprintf(o->peer_ip, sizeof o->peer_ip, "%s", peer_ip);
    o->peer_port = peer_port;
    o->sock = SRT_INVALID_SOCK;
    retry_init(&o->retry);
}

SRTSOCKET output_socket(const output_t *o) {
    return o->sock;
}

/* Through the tracker, so exactly one thread closes the socket (ADR 0011): a takeover or
 * shutdown may have taken it already. */
static void close_socket(int slot, SRTSOCKET sock) {
    if (tracker_take_output(slot, sock) != SRT_INVALID_SOCK) srt_close(sock);
}

/* Drops the connection but leaves the output free to reconnect. */
static void drop_connection(output_t *o) {
    if (o->sock != SRT_INVALID_SOCK) close_socket(o->slot, o->sock);
    o->sock = SRT_INVALID_SOCK;
}

void output_close(output_t *o) {
    drop_connection(o);
    o->closed = 1;
}

/* One attempt at a connected socket, or SRT_INVALID_SOCK with the reason in `err`. The
 * socket is the tracker's before it is configured and connected, so a takeover or
 * shutdown during the (blocking) connect can close it. */
static SRTSOCKET connect_once(const output_t *o, char *err, size_t err_sz) {
    err[0] = '\0';

    SRTSOCKET sock = o->transport->open(err, err_sz);
    if (sock == SRT_INVALID_SOCK) return SRT_INVALID_SOCK;
    tracker_set_output_socket(o->slot, sock);

    if (o->transport->connect(sock, o->target, o->streamid, err, err_sz) != 0) {
        close_socket(o->slot, sock);
        return SRT_INVALID_SOCK;
    }
    if (!tracker_is_current(o->slot, SRT_INVALID_SOCK, sock)) return SRT_INVALID_SOCK;
    return sock;
}

/* Counts a failed connect or send, schedules the next attempt and publishes the
 * count. `what` names the step in the retry log line. Returns the delay in ms. */
static int record_failure(output_t *o, const char *what) {
    int delay_ms = retry_failed(&o->retry, now_ms());
    set_stream_retry_failures(o->slot, o->retry.failures);
    relay_logf("Output %s retry streamid=%s failures=%d retry_ms=%d\n", what,
               streamid_or_empty(o->streamid), o->retry.failures, delay_ms);
    return delay_ms;
}

/* Tries once. On success it starts the healthy-period clock; on failure it counts the
 * failure and records why in the stream's lastError. */
static void connect_with_retry(output_t *o) {
    char err[OUTPUT_ERROR_MAX];
    o->sock = connect_once(o, err, sizeof err);
    if (o->sock != SRT_INVALID_SOCK) {
        set_stream_output_connected(o->slot, 1);
        retry_connected(&o->retry, now_ms());
        return;
    }

    int delay_ms = record_failure(o, "publish");
    set_stream_output_connected(o->slot, 0);
    if (err[0]) {
        set_stream_errorf(o->slot, "Failed to publish to downstream output: %s; retrying in %d ms",
                          err, delay_ms);
    } else {
        set_stream_errorf(o->slot, "Failed to publish to downstream output; retrying in %d ms",
                          delay_ms);
    }
}

void output_service(output_t *o) {
    if (o->closed) return;
    if (o->sock == SRT_INVALID_SOCK && retry_due(&o->retry, now_ms())) connect_with_retry(o);
    if (retry_check_healthy(&o->retry, now_ms())) set_stream_retry_failures(o->slot, 0);
}

/* A send failure almost always means the transport-level connect succeeded (SRS
 * accepts the SRT handshake) but the stream was then rejected or torn down by
 * SRS's on_publish hook. Without backoff, a persistently unauthorized stream key
 * reconnects and fails again in a tight loop bounded only by handshake + hook
 * round-trip time (seen in production at 10-40 ms intervals, sustained
 * indefinitely). So it goes through the same backoff table as a failed connect. */
static void send_failed(output_t *o, const char *reason) {
    const char *op = o->transport->send_op;
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&o->target->out_addr, out_ip, sizeof out_ip, &out_port);
    set_last_errorf("%s: %s", op, reason);
    set_stream_errorf(o->slot, "Relay output error to %s:%d (input peer %s:%d): %s", out_ip,
                      out_port, o->peer_ip, o->peer_port, reason);
    set_stream_output_connected(o->slot, 0);
    relay_logf("%s failed peer=%s:%d streamid=%s error=\"%s\"\n", op, o->peer_ip, o->peer_port,
               streamid_or_empty(o->streamid), reason);
    drop_connection(o);

    record_failure(o, "send");
}

output_send_result_t output_send(output_t *o, const char *buf, int len) {
    if (o->sock == SRT_INVALID_SOCK) return OUTPUT_DOWN;

    char reason[256];
    reason[0] = '\0';
    if (o->transport->send(o->sock, buf, len, reason, sizeof reason) != 0) {
        send_failed(o, reason);
        return OUTPUT_FAILED;
    }
    return OUTPUT_SENT;
}
