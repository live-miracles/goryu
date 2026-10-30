#ifndef RELAY_COMMON_H
#define RELAY_COMMON_H

/* Limits and tiny helpers shared by the relay's modules. */

/* Session table size. The same slot index identifies a session in the tracker
 * (sockets, streamid) and in the stream state (telemetry). */
#define MAX_ACTIVE_SESSIONS 256
#define MAX_GROUP_MEMBERS 16
#define LAST_ERROR_MAX 512

/* A session reads libsrt's counters at most this often (ADR 0002). */
#define STATS_SAMPLE_INTERVAL_MS 1000

/* A second publisher on an active streamid waits this long for the holder to end or
 * go stale; a holder whose input has been silent for DUPLICATE_TAKEOVER_STALE_MS is
 * closed and replaced, otherwise the newcomer is rejected (ADR 0005). */
#define DUPLICATE_TAKEOVER_WAIT_MS 5000
#define DUPLICATE_TAKEOVER_STALE_MS 5000

static inline int max_int(int a, int b) {
    return a > b ? a : b;
}

static inline long long max_ll(long long a, long long b) {
    return a > b ? a : b;
}

#endif
