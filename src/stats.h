#ifndef STATS_H
#define STATS_H

#include <srt/srt.h>

/* Reads libsrt's counters for one session (input socket or group, its member
 * legs, and the downstream socket) and stores them in the stream's state. It runs
 * on the session thread, at most once per second unless force is set, so the
 * status thread never touches libsrt. slot is the session's slot; a session that
 * holds no stream (an empty streamid) has nothing to update and returns at once. */
void update_stream_srt_counters(int slot, SRTSOCKET in_sock, SRTSOCKET out_sock, int force);

#endif
