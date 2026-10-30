#include "telemetry.h"

#include <math.h>
#include <string.h>

/* libsrt can expose an uninitialized ~1e16 ms value for this statistic after
 * packet loss. A live SRT packet cannot be belated by anything remotely close
 * to this; keep a conservative upper bound at the relay API boundary. */
#define MAX_REASONABLE_BELATED_AVG_MS 1000000000.0

static int valid_belated_avg_ms(double value) {
    return isfinite(value) && value >= 0.0 && value <= MAX_REASONABLE_BELATED_AVG_MS;
}

/* A leg as reported: its own counters, each with the rule for when to show it. */
static void report_leg(relay_leg_state_t *out, const leg_reading_t *in) {
    memset(out, 0, sizeof *out);
    memcpy(out->ip, in->ip, sizeof out->ip);
    out->ip[sizeof out->ip - 1] = '\0';
    out->port = in->port;
    out->member_state = in->state;
    out->stats_valid = in->have_counters;
    if (!in->have_counters) return;

    const srt_counters_t *c = &in->counters;
    /* tsbpd_delay_ms is the same negotiated latency SRTO_LATENCY reports. */
    out->latency_valid = c->tsbpd_delay_ms > 0;
    out->latency_ms = c->tsbpd_delay_ms;
    out->rtt_ms = c->rtt_ms;
    out->rtt_valid = c->rtt_ms > 0.0;
    out->recv_packets_total = c->recv_total;
    out->recv_unique_packets_total = c->recv_unique_total;
    out->recv_loss_total = c->recv_loss_total;
    out->recv_drop_total = c->recv_drop_total;
    out->retrans_total = c->recv_retrans_total;
    out->bandwidth_mbps = c->bandwidth_mbps;
    out->recv_rate_mbps = c->recv_rate_mbps;
    out->belated_total = c->belated_total;
    out->belated_avg_ms = c->belated_avg_ms;
    out->belated_avg_valid = valid_belated_avg_ms(c->belated_avg_ms);
    out->undecrypt_total = c->undecrypt_total;
    out->reorder_distance = c->reorder_distance;
    out->rcv_buf_ms = c->rcv_buf_ms;
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
static int collect_leg_events(relay_stream_state_t *st, const relay_leg_state_t *legs,
                              int leg_count, long long now, leg_event_t *events) {
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
    const srt_counters_t *out = &sample->out;
    const int is_group = sample->is_group;
    const int have_in = sample->have_in;

    relay_leg_state_t legs[MAX_GROUP_MEMBERS];
    int leg_count = sample->leg_count;
    for (int i = 0; i < leg_count; ++i) {
        report_leg(&legs[i], &sample->legs[i]);
    }

    /* The input's negotiated latency. A plain input has it in its own counters.
     * CUDTGroup::bstatsSocket() never fills in msRcvTsbPdDelay for a group, so the
     * group's latency is the largest one that any of its legs reported. */
    int have_in_latency = 0;
    int in_latency_ms = 0;
    if (!is_group) {
        have_in_latency = have_in && in->tsbpd_delay_ms > 0;
        in_latency_ms = in->tsbpd_delay_ms;
    } else {
        for (int i = 0; i < leg_count; ++i) {
            if (!legs[i].latency_valid) continue;
            have_in_latency = 1;
            in_latency_ms = max_int(in_latency_ms, legs[i].latency_ms);
        }
    }

    if (sample->can_read_in) {
        st->input_latency_valid = have_in_latency;
        if (have_in_latency) st->input_latency_ms = in_latency_ms;
    }

    /* bandwidth, receive rate, belated, undecrypt, reorder and buffer are only
     * meaningful for a non-bonded input: CUDTGroup::bstatsSocket() never populates
     * them for a group socket, so they would otherwise silently read 0. */
    st->input_extra_stats_valid = have_in && !is_group;
    st->input_belated_avg_valid =
        st->input_extra_stats_valid && valid_belated_avg_ms(in->belated_avg_ms);
    if (st->input_extra_stats_valid) {
        st->input_bandwidth_mbps = in->bandwidth_mbps;
        st->input_recv_rate_mbps = in->recv_rate_mbps;
        st->input_belated_total = in->belated_total;
        st->input_belated_avg_ms = in->belated_avg_ms;
        st->input_undecrypt_total = in->undecrypt_total;
        st->input_reorder_distance = in->reorder_distance;
        st->input_rcv_buf_ms = in->rcv_buf_ms;
    }
    if (have_in) {
        st->input_rtt_ms = in->rtt_ms;
        st->input_rtt_valid = in->rtt_ms > 0.0;
        st->recv_packets_total_valid =
            !(is_group && in->recv_total == 0 && in->recv_unique_total > 0);
        /* SRT group sockets only expose unique receive and group-drop statistics.
         * Loss and retransmission are meaningful per member socket, but there is no
         * deduplicated group equivalent. */
        st->recv_loss_total_valid = !is_group;
        st->retrans_total_valid = !is_group;
        st->recv_packets_total = max_ll(st->recv_packets_total, in->recv_total);
        st->recv_unique_packets_total =
            max_ll(st->recv_unique_packets_total, in->recv_unique_total);
        st->recv_loss_total = max_int(st->recv_loss_total, in->recv_loss_total);
        st->recv_drop_total = max_int(st->recv_drop_total, in->recv_drop_total);
        st->retrans_total = max_int(st->retrans_total, in->recv_retrans_total);
    }
    if (sample->have_out) {
        st->output_latency_valid = out->tsbpd_delay_ms > 0;
        st->output_latency_ms = out->tsbpd_delay_ms;
        st->output_rtt_ms = out->rtt_ms;
        st->output_rtt_valid = out->rtt_ms > 0.0;
        st->output_extra_stats_valid = 1;
        st->output_sent_packets_total = out->sent_total;
        st->output_send_loss_total = out->send_loss_total;
        st->output_send_drop_total = out->send_drop_total;
        st->output_retrans_total = out->send_retrans_total;
        st->output_bandwidth_mbps = out->bandwidth_mbps;
        st->output_send_rate_mbps = out->send_rate_mbps;
        st->output_undecrypt_total = out->undecrypt_total;
        st->output_snd_buf_ms = out->snd_buf_ms;
    }

    st->leg_count = leg_count;
    for (int i = 0; i < leg_count; ++i) {
        st->legs[i] = legs[i];
    }
    st->last_stats_at_ms = sample->taken_at_ms;

    result->event_count = 0;
    if (sample->legs_read) {
        result->event_count =
            collect_leg_events(st, legs, leg_count, sample->taken_at_ms, result->events);
    }
    result->suppressed =
        rate_window_take_suppressed(&st->leg_log, sample->taken_at_ms, LEG_LOG_WINDOW_MS);
}
