#ifndef STATS_H
#define STATS_H

#include <srt/srt.h>

/* Reads libsrt's counters for one session (input socket or group, its member
 * legs, and the downstream socket) and stores them in the stream's state. It runs
 * on the session thread, at most once per second unless force is set, so the
 * status thread never touches libsrt. slot is the stream-state slot (-1 for a
 * stream without a streamid: nothing to update) and tracker_slot the session's
 * tracker slot. */
void update_stream_srt_counters(int slot, int tracker_slot, SRTSOCKET in_sock, SRTSOCKET out_sock,
                                int force);

#endif
