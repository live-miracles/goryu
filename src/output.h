#ifndef OUTPUT_H
#define OUTPUT_H

#include <netinet/in.h>
#include <sys/socket.h>

#include <srt/srt.h>

#include "backoff.h"

/* Where every session forwards to: the downstream SRT server. */
typedef struct output_target {
    char out_query[1024]; /* SRT options (and optional streamid) from the output URI */
    struct sockaddr_storage out_addr;
    socklen_t out_addrlen;
} output_target_t;

/* The calls an output makes on the downstream, so the connect, retry and failure
 * rules in output.c run without libsrt. srt_output.c is the real one; the unit tests
 * script their own. Each failing call has already logged and set the process
 * lastError, and says why in `err` for the stream's own lastError. */
typedef struct output_transport {
    /* Names the send call in messages ("srt_sendmsg2"). */
    const char *send_op;
    /* A new, unconnected socket, or SRT_INVALID_SOCK. */
    SRTSOCKET (*open)(char *err, size_t err_sz);
    /* Configures the socket for the target and connects it (blocking, bounded), or -1. */
    int (*connect)(SRTSOCKET sock, const output_target_t *target, const char *streamid, char *err,
                   size_t err_sz);
    /* 0, or -1 with the transport's reason in `reason`. */
    int (*send)(SRTSOCKET sock, const char *buf, int len, char *reason, size_t reason_sz);
} output_transport_t;

/* One session's connection to the downstream, from its first connect to its end. It
 * connects when asked, keeps the tracker (which closes it on takeover or shutdown) and
 * the stream's telemetry in step, and backs off after every failed connect or send
 * (ADR 0004). Owned by the session thread; not locked. */
typedef struct output {
    const output_transport_t *transport;
    const output_target_t *target;
    int slot;
    char streamid[1024];
    char peer_ip[INET6_ADDRSTRLEN]; /* the input's peer, for messages */
    int peer_port;
    SRTSOCKET sock; /* SRT_INVALID_SOCK while the connection is down */
    int closed;     /* output_close() was called: it never connects again */
    retry_state_t retry;
} output_t;

typedef enum output_send_result {
    OUTPUT_SENT,
    OUTPUT_DOWN,   /* no connection: nothing was sent, and nothing failed */
    OUTPUT_FAILED, /* the send failed: logged, closed and backed off */
} output_send_result_t;

void output_init(output_t *o, const output_transport_t *transport, const output_target_t *target,
                 int slot, const char *streamid, const char *peer_ip, int peer_port);

/* Call it from the session's loop, at least once a second. Connects if the connection is
 * down and its retry is due (a failure is logged, counted and scheduled), and clears
 * the failure count once the connection has stayed up for OUTPUT_HEALTHY_MS. */
void output_service(output_t *o);

output_send_result_t output_send(output_t *o, const char *buf, int len);

/* The downstream socket, or SRT_INVALID_SOCK while it is down, for the stats sampler. */
SRTSOCKET output_socket(const output_t *o);

/* Ends the connection if there is one, for good: output_service() will not connect
 * again. The session's last act on its output. */
void output_close(output_t *o);

#endif
