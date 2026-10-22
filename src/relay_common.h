#ifndef RELAY_COMMON_H
#define RELAY_COMMON_H

/* Limits and tiny helpers shared by the relay's modules. */

/* Session table size. The same slot index identifies a session in the tracker
 * (sockets) and in the stream state (telemetry). */
#define MAX_ACTIVE_SESSIONS 256
#define MAX_GROUP_MEMBERS 16
#define LAST_ERROR_MAX 512

static inline int max_int(int a, int b) {
    return a > b ? a : b;
}

static inline long long max_ll(long long a, long long b) {
    return a > b ? a : b;
}

#endif
