#ifndef TRACKER_H
#define TRACKER_H

#include <srt/srt.h>

/* Tracks the sockets of every live session so that a session can be closed from
 * another thread (duplicate takeover, shutdown) and so the stats sampler never
 * reads a socket that has been closed. A session owns a slot from
 * tracker_register() until tracker_unregister().
 *
 * Sockets are closed by whoever "takes" them: tracker_take_* returns the socket
 * only if it is still the tracked one and clears it, so exactly one thread calls
 * srt_close on it. */

void tracker_init(void);

/* Returns the new slot, or -1 when all MAX_ACTIVE_SESSIONS are in use. */
int tracker_register(SRTSOCKET conn);
void tracker_unregister(int slot);

void tracker_set_streamid(int slot, const char *streamid);
void tracker_set_output_socket(int slot, SRTSOCKET sock);

/* Ends a session's hold on its slot, last of all: closes the input socket if it is
 * still the tracked one (a takeover or shutdown may have closed it already) and frees
 * the slot. */
void tracker_release(int slot, SRTSOCKET input);

/* Return `expected` (and stop tracking it) if it is still the slot's socket,
 * otherwise SRT_INVALID_SOCK. The caller closes what it gets. */
SRTSOCKET tracker_take_input(int slot, SRTSOCKET expected);
SRTSOCKET tracker_take_output(int slot, SRTSOCKET expected);

/* True while the slot is in use and holds these sockets (SRT_INVALID_SOCK
 * means "don't care" for that side). */
int tracker_is_current(int slot, SRTSOCKET input_sock, SRTSOCKET output_sock);

/* Closes every session with this streamid except except_slot. */
void tracker_close_streamid(const char *streamid, int except_slot);

/* Shutdown: close every tracked socket, then wait for every session to
 * unregister. */
void tracker_close_all(void);
void tracker_wait_all(void);

/* For the stats sampler: libsrt is queried under the tracker lock, so a socket
 * cannot be closed between the check and the call. */
void tracker_lock(void);
void tracker_unlock(void);
int tracker_input_is_locked(int slot, SRTSOCKET sock);
int tracker_output_is_locked(int slot, SRTSOCKET sock);

#endif
