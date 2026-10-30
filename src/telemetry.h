#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "stream_state.h"

/* The rules that turn a reading of libsrt's counters into what the status API
 * reports. This is the one place that decides whether a value is reportable (the
 * *_reportable() rules below, which the status writer turns into null), that totals only
 * move forward, and when a bonded leg has joined or left. It is pure: no libsrt, no lock,
 * no logging, so it is unit-tested with fixtures. stats.c produces the reading and
 * stream_state.c holds the lock and writes the log lines.
 *
 * The state keeps libsrt's counters as read and never a verdict about them, so a value
 * and the rule for showing it cannot drift apart: the rule is evaluated where the value
 * is written out. */

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

/* Whether a figure is reported (true) or left out as null (false). A counter that is
 * not listed here is always reported. libsrt reports 0 for what it does not measure, so a
 * zero rtt or latency means "unknown", and it fills in some figures for a plain socket
 * only. */
int rtt_reportable(double rtt_ms);
int latency_reportable(int latency_ms);
/* libsrt can expose an uninitialized ~1e16 ms value after packet loss. */
int belated_avg_reportable(double belated_avg_ms);

/* The input's totals: a group socket reports no total of packets received, and no loss
 * or retransmissions, only unique packets and group drops. */
int input_recv_total_reportable(const relay_stream_state_t *st);
int input_loss_retrans_reportable(const relay_stream_state_t *st);
/* Bandwidth, receive rate, belated, undecrypt, reorder distance and buffer: only for the
 * latest reading of a plain socket, because CUDTGroup::bstatsSocket() never fills them
 * in for a group. */
int input_extra_reportable(const relay_stream_state_t *st);
int input_belated_avg_reportable(const relay_stream_state_t *st);
/* The input's latency is reportable when input_latency_ms is. */

/* A bonded leg's figures: all are left out when its own statistics could not be read. */
int leg_counters_reportable(const leg_reading_t *leg);
int leg_rtt_reportable(const leg_reading_t *leg);
int leg_latency_reportable(const leg_reading_t *leg);
int leg_belated_avg_reportable(const leg_reading_t *leg);

/* The downstream's figures, while its counters are held. */
int output_extra_reportable(const relay_stream_state_t *st);
int output_rtt_reportable(const relay_stream_state_t *st);
int output_latency_reportable(const relay_stream_state_t *st);

#endif
