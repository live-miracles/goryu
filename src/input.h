#ifndef INPUT_H
#define INPUT_H

#include <arpa/inet.h>
#include <stddef.h>

#include <srt/srt.h>

/* How long a receive may block without data. A session whose publisher goes silent
 * would otherwise sit in the call indefinitely, and srt_close() from another thread
 * (duplicate takeover) does not wake a blocked receive on a bonded group: libsrt defers
 * closing a group that is in use. Waking once a second lets the session notice that it
 * was closed. */
#define INPUT_RECV_TIMEOUT_MS 1000

/* What a receive came to. */
typedef enum input_result {
    INPUT_DATA,   /* `len` bytes are in the buffer */
    INPUT_IDLE,   /* nothing arrived within INPUT_RECV_TIMEOUT_MS: ask again */
    INPUT_CLOSED, /* the connection ended, as connections do: not an error */
    INPUT_FAILED, /* the receive failed: logged and recorded already */
} input_result_t;

/* What the session learns about an accepted connection. */
typedef struct input_info {
    char streamid[1024]; /* the one the publisher set on the connection, "" if none */
    int bonded;          /* a group, not a plain connection */
    /* Why the receive timeout could not be applied, "" if it was. Not fatal: the
     * session still forwards, it only notices a closed connection later. */
    char timeout_error[256];
} input_info_t;

/* The calls an input makes on the accepted connection, so the reporting rules in input.c
 * run without libsrt. srt_input.c is the real one; the unit tests script their own. */
typedef struct input_transport {
    /* Names the receive call in messages ("srt_recvmsg2"). */
    const char *recv_op;
    /* Applies the receive timeout and describes the connection. */
    void (*open)(SRTSOCKET conn, int recv_timeout_ms, input_info_t *info);
    /* Whether the connection is still up (or still coming up). */
    int (*connected)(SRTSOCKET conn);
    /* One receive of at most `cap` bytes, bounded by the receive timeout. Returns
     * INPUT_DATA with `*len` > 0; INPUT_IDLE for a timeout (or nothing read);
     * INPUT_CLOSED when the connection was lost; INPUT_FAILED with libsrt's reason in
     * `reason` for anything else. It neither logs nor records. */
    input_result_t (*recv)(SRTSOCKET conn, char *buf, int cap, int *len, char *reason,
                           size_t reason_sz);
} input_transport_t;

/* One session's connection to its publisher, from accept to the end. Owned by the
 * session thread; not locked. The socket itself belongs to the tracker (the session ends
 * with tracker_release, ADR 0011), so an input never closes it. */
typedef struct input {
    const input_transport_t *transport;
    SRTSOCKET conn;
    int slot;
    int bonded;
    char streamid[1024]; /* the connection's, else the default; "" when neither */
    char peer_ip[INET6_ADDRSTRLEN];
    int peer_port;
} input_t;

/* Opens an accepted connection: applies the receive timeout, settles the streamid (the
 * connection's own, else `default_streamid`, which may be "") and logs the accept. */
void input_open(input_t *in, const input_transport_t *transport, SRTSOCKET conn, int slot,
                const char *peer_ip, int peer_port, const char *default_streamid);

/* The session's loop runs while this is true. */
int input_connected(const input_t *in);

/* One receive. A failure is told here, once: in the log, the process lastError and the
 * stream's lastError. Closing and idling are not failures and say nothing. */
input_result_t input_recv(input_t *in, char *buf, int cap, int *len);

#endif
