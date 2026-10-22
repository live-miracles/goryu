#ifndef SESSION_H
#define SESSION_H

#include <sys/socket.h>

#include <srt/srt.h>

/* Where every session forwards to: the downstream SRT server. */
typedef struct output_target {
    char out_query[1024]; /* SRT options (and optional streamid) from the output URI */
    struct sockaddr_storage out_addr;
    socklen_t out_addrlen;
} output_target_t;

/* Takes ownership of an accepted connection (bonded group or plain socket) and
 * starts a detached session thread for it. If the session table is full or the
 * thread cannot start, the connection is closed and the failure is logged and
 * recorded as lastError; the relay keeps accepting. */
void session_spawn(SRTSOCKET conn, const output_target_t *target,
                   const struct sockaddr_storage *peer);

#endif
