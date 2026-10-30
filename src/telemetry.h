#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "stream_state.h"

/* The rules that turn a reading of libsrt's counters into what the status API
 * reports. This is the one place that decides whether a value is reportable (the
 * *_valid flags, which the status writer turns into null), that totals only move
 * forward, and when a bonded leg has joined or left. It is pure: no libsrt, no lock,
 * no logging, so it is unit-tested with fixtures. stats.c produces the reading and
 * stream_state.c holds the lock and writes the log lines. */

/* A leg joining or leaving the group, to be logged. */
typedef struct leg_event {
    int joined;
    relay_leg_ref_t leg;
} leg_event_t;

/* A bonded leg that joins or leaves is logged, at most this many lines per stream in
 * a window; the rest are counted and summarised when the window ends. A bad link
 * that reconnects every few seconds would otherwise write a line each time, for as
 * long as it lasts. */
#define LEG_LOG_MAX_PER_WINDOW 12
#define LEG_LOG_WINDOW_MS 30000

typedef struct telemetry_result {
    leg_event_t events[2 * MAX_GROUP_MEMBERS];
    int event_count;
    /* Leg events dropped by the limiter in a window that has just ended, for one
     * "suppressed" summary line; 0 otherwise. */
    long long suppressed;
} telemetry_result_t;

/* Applies one reading to a stream's state and reports the leg events to log. The
 * counters only ever move forward (max of old and new), so a reading taken just
 * after a leg dropped out cannot make a total go backwards. */
void telemetry_apply(relay_stream_state_t *st, const stats_sample_t *sample,
                     telemetry_result_t *result);

#endif
