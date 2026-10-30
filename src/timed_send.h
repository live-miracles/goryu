#ifndef TIMED_SEND_H
#define TIMED_SEND_H

#include <stddef.h>

/* Sends all of buf on a stream socket, giving up once timeout_ms has passed in
 * total (not per call), so a peer that stops reading cannot hold the caller.
 * Returns 0 when everything was sent, -1 on timeout or a socket error. Never
 * raises SIGPIPE. */
int send_all_within(int fd, const char *buf, size_t len, int timeout_ms);

#endif
