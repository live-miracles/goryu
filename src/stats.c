#include "stats.h"

#include <math.h>
#include <string.h>

#include "net.h"
#include "relay_common.h"
#include "runtime.h"
#include "stream_state.h"
#include "tracker.h"

/* libsrt can expose an uninitialized ~1e16 ms value for this statistic after
 * packet loss. A live SRT packet cannot be belated by anything remotely close
 * to this; keep a conservative upper bound at the relay API boundary. */
#define MAX_REASONABLE_BELATED_AVG_MS 1000000000.0

static int valid_belated_avg_ms(double value) {
    return isfinite(value) && value >= 0.0 && value <= MAX_REASONABLE_BELATED_AVG_MS;
}

/* Fills one leg from its member socket's own statistics. */
static void read_leg(relay_leg_state_t *leg, const SRT_SOCKGROUPDATA *member) {
    SRT_TRACEBSTATS member_stats;
    memset(&member_stats, 0, sizeof member_stats);
    int have_member_stats = srt_bstats(member->id, &member_stats, 0) != SRT_ERROR;

    format_leg_addr(&member->peeraddr, leg->ip, sizeof leg->ip, &leg->port);
    leg->member_state = member->memberstate;
    leg->stats_valid = have_member_stats;
    if (!have_member_stats) return;

    /* msRcvTsbPdDelay is the same negotiated latency SRTO_LATENCY
     * reports, already present in this bstats call. */
    leg->latency_valid = member_stats.msRcvTsbPdDelay > 0;
    leg->latency_ms = member_stats.msRcvTsbPdDelay;
    leg->rtt_ms = member_stats.msRTT;
    leg->rtt_valid = member_stats.msRTT > 0.0;
    leg->recv_packets_total = member_stats.pktRecvTotal;
    leg->recv_unique_packets_total = member_stats.pktRecvUniqueTotal;
    leg->recv_loss_total = member_stats.pktRcvLossTotal;
    leg->recv_drop_total = member_stats.pktRcvDropTotal;
    leg->retrans_total = member_stats.pktRcvRetrans;
    leg->bandwidth_mbps = member_stats.mbpsBandwidth;
    leg->recv_rate_mbps = member_stats.mbpsRecvRate;
    leg->belated_total = member_stats.pktRcvBelated;
    leg->belated_avg_ms = member_stats.pktRcvAvgBelatedTime;
    leg->belated_avg_valid = valid_belated_avg_ms(member_stats.pktRcvAvgBelatedTime);
    leg->undecrypt_total = member_stats.pktRcvUndecryptTotal;
    leg->reorder_distance = member_stats.pktReorderDistance;
    leg->rcv_buf_ms = member_stats.msRcvBuf;
}

/* Called with the tracker lock held, so in_sock cannot be closed meanwhile. */
static void read_input(stats_sample_t *sample, SRTSOCKET in_sock) {
    sample->have_in = srt_bstats(in_sock, &sample->in_stats, 0) != SRT_ERROR;
    sample->in_belated_avg_valid = valid_belated_avg_ms(sample->in_stats.pktRcvAvgBelatedTime);

    if (!sample->is_group) {
        sample->have_in_latency = sample->have_in && sample->in_stats.msRcvTsbPdDelay > 0;
        sample->in_latency_ms = sample->in_stats.msRcvTsbPdDelay;
        return;
    }

    SRT_SOCKGROUPDATA members[MAX_GROUP_MEMBERS];
    size_t member_count = MAX_GROUP_MEMBERS;
    memset(members, 0, sizeof members);
    if (srt_group_data(in_sock, members, &member_count) != SRT_ERROR) {
        sample->legs_read = 1;
        for (size_t i = 0; i < member_count; ++i) {
            if (members[i].id == SRT_INVALID_SOCK) continue;
            if (members[i].memberstate == SRT_GST_BROKEN) continue;
            if (sample->leg_count >= MAX_GROUP_MEMBERS) continue;
            read_leg(&sample->legs[sample->leg_count++], &members[i]);
        }
    }

    /* CUDTGroup::bstatsSocket() never fills in msRcvTsbPdDelay, so derive the
     * group's negotiated latency from whichever legs actually reported one. */
    for (int li = 0; li < sample->leg_count; ++li) {
        if (!sample->legs[li].latency_valid) continue;
        sample->have_in_latency = 1;
        sample->in_latency_ms = max_int(sample->in_latency_ms, sample->legs[li].latency_ms);
    }
}

void update_stream_srt_counters(int slot, int tracker_slot, SRTSOCKET in_sock, SRTSOCKET out_sock,
                                int force) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    long long now = now_ms();
    long long last = stream_state_last_stats_at_ms(slot);
    if (!force && last > 0 && now - last < 1000) return;

    stats_sample_t sample;
    memset(&sample, 0, sizeof sample);
    sample.taken_at_ms = now;
    sample.is_group = in_sock != SRT_INVALID_SOCK && (in_sock & SRTGROUP_MASK);

    tracker_lock();
    sample.can_read_in = tracker_input_is_locked(tracker_slot, in_sock);
    int can_read_out = tracker_output_is_locked(tracker_slot, out_sock);
    if (sample.can_read_in) read_input(&sample, in_sock);
    if (can_read_out) {
        sample.have_out = srt_bstats(out_sock, &sample.out_stats, 0) != SRT_ERROR;
    }
    tracker_unlock();

    stream_state_apply_stats(slot, &sample);
}
