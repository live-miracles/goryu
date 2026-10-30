#ifndef SESSION_H
#define SESSION_H

#include <sys/socket.h>

#include <srt/srt.h>

#include "output.h"

/* Takes ownership of an accepted connection (bonded group or plain socket) and
 * starts a detached session thread for it. If the session table is full or the
 * thread cannot start, the connection is closed and the failure is logged and
 * recorded as lastError; the relay keeps accepting. */
void session_spawn(SRTSOCKET conn, const output_target_t *target,
                   const struct sockaddr_storage *peer);

#endif
