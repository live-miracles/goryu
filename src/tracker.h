#ifndef TRACKER_H
#define TRACKER_H

#include <srt/srt.h>

/* A session's place in the relay: its slot, the sockets it owns, and the streamid it
 * holds. A session owns a slot from tracker_register() until tracker_release(), and
 * the same slot indexes its telemetry in the stream state.
 *
 * Sockets are closed by whoever "takes" them: tracker_take_* returns the socket
 * only if it is still the tracked one and clears it, so exactly one thread calls
 * srt_close on it.
 *
 * The tracker alone decides who holds a streamid, under its one lock, and tells the
 * stream state when a stream begins and ends. It takes the stream state's lock while
 * holding its own and never the other way round. */

void tracker_init(void);

/* Returns the new slot, or -1 when all MAX_ACTIVE_SESSIONS are in use. */
int tracker_register(SRTSOCKET conn);

/* Makes this session the holder of `streamid`; call it once, after tracker_register()
 * and before the stream is used. A streamid that another session holds waits up to
 * DUPLICATE_TAKEOVER_WAIT_MS: a holder whose input has been silent for
 * DUPLICATE_TAKEOVER_STALE_MS has its sockets closed (it is the only session closed) and
 * is replaced once it releases its slot; otherwise this session is rejected. Returns 0
 * to carry on (an empty streamid has no stream and carries on without one), or -1 after
 * logging why the session must end. Either way the session still ends with
 * tracker_release(). */
int tracker_acquire_stream(int slot, const char *streamid);

/* Ends a session's hold on its slot, last of all: ends its stream if it holds one (so the
 * next session on the slot or the streamid can claim it), closes the input socket if it
 * is still the tracked one (a takeover or shutdown may have closed it already), and frees
 * the slot. The one call every way a session ends makes. */
void tracker_release(int slot, SRTSOCKET input);

void tracker_set_output_socket(int slot, SRTSOCKET sock);

/* Return `expected` (and stop tracking it) if it is still the slot's socket,
 * otherwise SRT_INVALID_SOCK. The caller closes what it gets. */
SRTSOCKET tracker_take_input(int slot, SRTSOCKET expected);
SRTSOCKET tracker_take_output(int slot, SRTSOCKET expected);

/* True while the slot is in use and holds these sockets (SRT_INVALID_SOCK
 * means "don't care" for that side). */
int tracker_is_current(int slot, SRTSOCKET input_sock, SRTSOCKET output_sock);

/* Shutdown: close every tracked socket, then wait for every session to
 * release its slot. */
void tracker_close_all(void);
void tracker_wait_all(void);

/* For the stats sampler: calls `read_fn(input_tracked, output_tracked, ctx)` with the
 * tracker's lock held, where each flag says whether that socket is still the slot's
 * tracked one (SRT_INVALID_SOCK never is). While `read_fn` runs no thread can take or close
 * a socket, so libsrt may be queried for exactly the sockets that were flagged. `read_fn`
 * must be quick and must not call the tracker. */
typedef void (*tracker_read_fn)(int input_tracked, int output_tracked, void *ctx);
void tracker_read_sockets(int slot, SRTSOCKET input, SRTSOCKET output, tracker_read_fn read_fn,
                          void *ctx);

#endif
