#include "telemetry.h"

#include <math.h>
#include <string.h>

int rtt_reportable(double rtt_ms) {
    return rtt_ms > 0.0;
}

int latency_reportable(int latency_ms) {
    return latency_ms > 0;
}

/* A live SRT packet cannot be belated by anything remotely close to this; keep a
 * conservative upper bound at the relay API boundary. */
#define MAX_REASONABLE_BELATED_AVG_MS 1000000000.0

int belated_avg_reportable(double belated_avg_ms) {
    return isfinite(belated_avg_ms) && belated_avg_ms >= 0.0 &&
           belated_avg_ms <= MAX_REASONABLE_BELATED_AVG_MS;
}

int input_recv_total_reportable(const relay_stream_state_t *st) {
    if (st->input_kind == INPUT_UNREAD) return 0;
    /* A group's total can read 0 while its unique count does not: that 0 is not a count. */
    return !(st->input_kind == INPUT_GROUP && st->input.recv_total == 0 &&
             st->input.recv_unique_total > 0);
}

int input_loss_retrans_reportable(const relay_stream_state_t *st) {
    return st->input_kind == INPUT_PLAIN;
}

int input_extra_reportable(const relay_stream_state_t *st) {
    return st->input_kind == INPUT_PLAIN && st->input_read_ok;
}

int input_belated_avg_reportable(const relay_stream_state_t *st) {
    return input_extra_reportable(st) && belated_avg_reportable(st->input.belated_avg_ms);
}

int leg_counters_reportable(const leg_reading_t *leg) {
    return leg->have_counters;
}

int leg_rtt_reportable(const leg_reading_t *leg) {
    return leg->have_counters && rtt_reportable(leg->counters.rtt_ms);
}

int leg_latency_reportable(const leg_reading_t *leg) {
    return leg->have_counters && latency_reportable(leg->counters.tsbpd_delay_ms);
}

int leg_belated_avg_reportable(const leg_reading_t *leg) {
    return leg->have_counters && belated_avg_reportable(leg->counters.belated_avg_ms);
}

int output_extra_reportable(const relay_stream_state_t *st) {
    return st->have_output_counters;
}

int output_rtt_reportable(const relay_stream_state_t *st) {
    return st->have_output_counters && rtt_reportable(st->output.rtt_ms);
}

int output_latency_reportable(const relay_stream_state_t *st) {
    return st->have_output_counters && latency_reportable(st->output.tsbpd_delay_ms);
}

static int leg_listed(const relay_leg_ref_t *list, int n, const char *ip, int port) {
    for (int i = 0; i < n; ++i) {
        if (list[i].port == port && strcmp(list[i].ip, ip) == 0) return 1;
    }
    return 0;
}

/* Compares the legs of this reading with the last logged set, records the new set,
 * and returns the changes the limiter lets through. A leg that reconnects from a new
 * port is a left and a joined. */
static int collect_leg_events(relay_stream_state_t *st, const leg_reading_t *legs, int leg_count,
                              long long now, leg_event_t *events) {
    relay_leg_ref_t now_set[MAX_GROUP_MEMBERS];
    for (int i = 0; i < leg_count; ++i) {
        memcpy(now_set[i].ip, legs[i].ip, sizeof now_set[i].ip);
        now_set[i].ip[sizeof now_set[i].ip - 1] = '\0';
        now_set[i].port = legs[i].port;
    }

    leg_event_t all[2 * MAX_GROUP_MEMBERS];
    int n = 0;
    for (int i = 0; i < st->logged_leg_count; ++i) {
        if (leg_listed(now_set, leg_count, st->logged_legs[i].ip, st->logged_legs[i].port))
            continue;
        all[n].joined = 0;
        all[n++].leg = st->logged_legs[i];
    }
    for (int i = 0; i < leg_count; ++i) {
        if (leg_listed(st->logged_legs, st->logged_leg_count, now_set[i].ip, now_set[i].port))
            continue;
        all[n].joined = 1;
        all[n++].leg = now_set[i];
    }

    st->logged_leg_count = leg_count;
    memcpy(st->logged_legs, now_set, sizeof(relay_leg_ref_t) * (size_t)leg_count);

    int kept = 0;
    for (int i = 0; i < n; ++i) {
        if (rate_window_allow(&st->leg_log, now, LEG_LOG_MAX_PER_WINDOW, LEG_LOG_WINDOW_MS)) {
            events[kept++] = all[i];
        }
    }
    return kept;
}

void telemetry_apply(relay_stream_state_t *st, const stats_sample_t *sample,
                     telemetry_result_t *result) {
    const srt_counters_t *in = &sample->in;
    const int is_group = sample->is_group;
    const int have_in = sample->have_in;

    leg_reading_t legs[MAX_GROUP_MEMBERS];
    int leg_count = sample->leg_count;
    for (int i = 0; i < leg_count; ++i) {
        legs[i] = sample->legs[i];
        legs[i].ip[sizeof legs[i].ip - 1] = '\0';
        /* A reading that failed has no figures, whatever the buffer held. */
        if (!legs[i].have_counters) memset(&legs[i].counters, 0, sizeof legs[i].counters);
    }

    /* The input's negotiated latency. A plain input has it in its own counters.
     * CUDTGroup::bstatsSocket() never fills in msRcvTsbPdDelay for a group, so the
     * group's latency is the largest one that any of its legs reported. */
    int in_latency_ms = 0;
    if (!is_group) {
        if (have_in && latency_reportable(in->tsbpd_delay_ms)) in_latency_ms = in->tsbpd_delay_ms;
    } else {
        for (int i = 0; i < leg_count; ++i) {
            if (!legs[i].have_counters || !latency_reportable(legs[i].counters.tsbpd_delay_ms))
                continue;
            in_latency_ms = max_int(in_latency_ms, legs[i].counters.tsbpd_delay_ms);
        }
    }
    if (sample->can_read_in) st->input_latency_ms = in_latency_ms;

    st->input_read_ok = have_in;
    if (have_in) {
        srt_counters_t held = *in;
        /* The totals only ever move forward (max of old and new), so a reading taken
         * just after a leg dropped out cannot make one go backwards. */
        held.recv_total = max_ll(st->input.recv_total, in->recv_total);
        held.recv_unique_total = max_ll(st->input.recv_unique_total, in->recv_unique_total);
        held.recv_loss_total = max_int(st->input.recv_loss_total, in->recv_loss_total);
        held.recv_drop_total = max_int(st->input.recv_drop_total, in->recv_drop_total);
        held.recv_retrans_total = max_int(st->input.recv_retrans_total, in->recv_retrans_total);
        st->input = held;
        st->input_kind = is_group ? INPUT_GROUP : INPUT_PLAIN;
    }
    if (sample->have_out) {
        st->output = sample->out;
        st->have_output_counters = 1;
    }

    st->leg_count = leg_count;
    memcpy(st->legs, legs, sizeof(leg_reading_t) * (size_t)leg_count);
    st->last_stats_at_ms = sample->taken_at_ms;

    result->event_count = 0;
    if (sample->legs_read) {
        result->event_count =
            collect_leg_events(st, legs, leg_count, sample->taken_at_ms, result->events);
    }
    result->suppressed =
        rate_window_take_suppressed(&st->leg_log, sample->taken_at_ms, LEG_LOG_WINDOW_MS);
}
