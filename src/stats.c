#include "stats.h"

#include <string.h>

#include "net.h"
#include "relay_common.h"
#include "runtime.h"
#include "stream_state.h"
#include "tracker.h"

/* The libsrt side of the telemetry: reads the counters into a stats_sample_t and
 * nothing more. Whether a value can be trusted, and whether it is reported, is
 * decided in telemetry.c, where it can be tested without libsrt. */

static void copy_counters(srt_counters_t *dst, const SRT_TRACEBSTATS *src) {
    dst->tsbpd_delay_ms = src->msRcvTsbPdDelay;
    dst->rtt_ms = src->msRTT;
    dst->recv_total = src->pktRecvTotal;
    dst->recv_unique_total = src->pktRecvUniqueTotal;
    dst->recv_loss_total = src->pktRcvLossTotal;
    dst->recv_drop_total = src->pktRcvDropTotal;
    dst->recv_retrans_total = src->pktRcvRetrans;
    dst->bandwidth_mbps = src->mbpsBandwidth;
    dst->recv_rate_mbps = src->mbpsRecvRate;
    dst->belated_total = src->pktRcvBelated;
    dst->belated_avg_ms = src->pktRcvAvgBelatedTime;
    dst->undecrypt_total = src->pktRcvUndecryptTotal;
    dst->reorder_distance = src->pktReorderDistance;
    dst->rcv_buf_ms = src->msRcvBuf;
    dst->sent_total = src->pktSentTotal;
    dst->send_loss_total = src->pktSndLossTotal;
    dst->send_drop_total = src->pktSndDropTotal;
    dst->send_retrans_total = src->pktRetransTotal;
    dst->send_rate_mbps = src->mbpsSendRate;
    dst->snd_buf_ms = src->msSndBuf;
}

/* Reads one socket's statistics. Returns whether libsrt answered. */
static int read_counters(srt_counters_t *dst, SRTSOCKET sock) {
    SRT_TRACEBSTATS stats;
    memset(&stats, 0, sizeof stats);
    int ok = srt_bstats(sock, &stats, 0) != SRT_ERROR;
    copy_counters(dst, &stats);
    return ok;
}

static leg_member_state_t leg_state(SRT_MEMBERSTATUS state) {
    switch (state) {
    case SRT_GST_PENDING:
        return LEG_MEMBER_PENDING;
    case SRT_GST_IDLE:
        return LEG_MEMBER_IDLE;
    case SRT_GST_RUNNING:
        return LEG_MEMBER_RUNNING;
    default:
        return LEG_MEMBER_UNKNOWN;
    }
}

static void read_leg(leg_reading_t *leg, const SRT_SOCKGROUPDATA *member) {
    format_leg_addr(&member->peeraddr, leg->ip, sizeof leg->ip, &leg->port);
    leg->state = leg_state(member->memberstate);
    leg->have_counters = read_counters(&leg->counters, member->id);
}

/* Called with the tracker lock held (see read_tracked), so in_sock cannot be closed. */
static void read_input(stats_sample_t *sample, SRTSOCKET in_sock) {
    sample->have_in = read_counters(&sample->in, in_sock);
    if (!sample->is_group) return;

    SRT_SOCKGROUPDATA members[MAX_GROUP_MEMBERS];
    size_t member_count = MAX_GROUP_MEMBERS;
    memset(members, 0, sizeof members);
    if (srt_group_data(in_sock, members, &member_count) == SRT_ERROR) return;

    sample->legs_read = 1;
    for (size_t i = 0; i < member_count; ++i) {
        if (members[i].id == SRT_INVALID_SOCK) continue;
        if (members[i].memberstate == SRT_GST_BROKEN) continue;
        if (sample->leg_count >= MAX_GROUP_MEMBERS) continue;
        read_leg(&sample->legs[sample->leg_count++], &members[i]);
    }
}

typedef struct sampling {
    stats_sample_t sample;
    SRTSOCKET in_sock;
    SRTSOCKET out_sock;
} sampling_t;

/* Runs under the tracker's lock (tracker_read_sockets), so the sockets it was told are
 * tracked cannot be closed meanwhile. */
static void read_tracked(int input_tracked, int output_tracked, void *ctx) {
    sampling_t *sampling = ctx;
    sampling->sample.can_read_in = input_tracked;
    if (input_tracked) read_input(&sampling->sample, sampling->in_sock);
    if (output_tracked) {
        sampling->sample.have_out = read_counters(&sampling->sample.out, sampling->out_sock);
    }
}

void update_stream_srt_counters(int slot, SRTSOCKET in_sock, SRTSOCKET out_sock, int force) {
    long long now = now_ms();
    if (!stream_state_sample_due(slot, now, force)) return;

    sampling_t sampling;
    memset(&sampling, 0, sizeof sampling);
    sampling.in_sock = in_sock;
    sampling.out_sock = out_sock;
    sampling.sample.taken_at_ms = now;
    sampling.sample.is_group = in_sock != SRT_INVALID_SOCK && (in_sock & SRTGROUP_MASK);

    tracker_read_sockets(slot, in_sock, out_sock, read_tracked, &sampling);
    stream_state_apply_stats(slot, &sampling.sample);
}
