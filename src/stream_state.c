#include "stream_state.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "runtime.h"

static pthread_mutex_t g_streams_mu = PTHREAD_MUTEX_INITIALIZER;
static relay_stream_state_t g_streams[MAX_ACTIVE_SESSIONS];
static char g_last_error[LAST_ERROR_MAX];

/* A bonded leg that joins or leaves is logged, at most this many lines per stream in
 * a window; the rest are counted and summarised when the window ends. A bad link
 * that reconnects every few seconds would otherwise write a line each time, for as
 * long as it lasts. */
#define LEG_LOG_MAX_PER_WINDOW 12
#define LEG_LOG_WINDOW_MS 30000

typedef struct leg_event {
    int joined;
    relay_leg_ref_t leg;
} leg_event_t;

static int leg_listed(const relay_leg_ref_t *list, int n, const char *ip, int port) {
    for (int i = 0; i < n; ++i) {
        if (list[i].port == port && strcmp(list[i].ip, ip) == 0) return 1;
    }
    return 0;
}

/* Compares the legs of this reading with the last logged set, fills events with the
 * ones to log (left first, then joined) and remembers the new set. Must hold the
 * lock. */
static int collect_leg_events(relay_stream_state_t *st, const relay_leg_state_t *legs,
                              int leg_count, long long now, leg_event_t *events) {
    relay_leg_ref_t now_set[MAX_GROUP_MEMBERS];
    for (int i = 0; i < leg_count; ++i) {
        snprintf(now_set[i].ip, sizeof now_set[i].ip, "%s", legs[i].ip);
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

void set_last_errorf(const char *fmt, ...) {
    pthread_mutex_lock(&g_streams_mu);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof g_last_error, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_streams_mu);
}

void set_stream_errorf(int slot, const char *fmt, ...) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_streams[slot].last_error, sizeof g_streams[slot].last_error, fmt, ap);
    va_end(ap);
    g_streams[slot].last_error_at_ms = now_ms();
    pthread_mutex_unlock(&g_streams_mu);
}

int claim_stream_state(const char *streamid, int preferred_slot) {
    if (!streamid || !streamid[0]) return -1;
    pthread_mutex_lock(&g_streams_mu);
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (g_streams[i].streamid[0] && strcmp(g_streams[i].streamid, streamid) == 0) {
            slot = -2;
            break;
        }
    }
    if (slot == -1) {
        if (preferred_slot >= 0 && preferred_slot < MAX_ACTIVE_SESSIONS &&
            !g_streams[preferred_slot].streamid[0]) {
            slot = preferred_slot;
        }
        if (slot >= 0) {
            memset(&g_streams[slot], 0, sizeof g_streams[slot]);
            strncpy(g_streams[slot].streamid, streamid, sizeof g_streams[slot].streamid - 1);
            g_streams[slot].streamid[sizeof g_streams[slot].streamid - 1] = '\0';
            g_streams[slot].input_active = 1;
            g_streams[slot].last_input_packet_at_ms = now_ms();
        }
    }
    pthread_mutex_unlock(&g_streams_mu);
    return slot;
}

int stream_input_age_ms(const char *streamid, long long now, long long *age_ms) {
    if (!streamid || !streamid[0]) return -1;

    int found = 0;
    long long last_input_at = 0;

    pthread_mutex_lock(&g_streams_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!g_streams[i].streamid[0]) continue;
        if (strcmp(g_streams[i].streamid, streamid) != 0) continue;
        found = 1;
        last_input_at = g_streams[i].last_input_packet_at_ms;
        break;
    }
    pthread_mutex_unlock(&g_streams_mu);

    if (!found) return -1;
    if (last_input_at <= 0) last_input_at = now;
    if (age_ms) *age_ms = now - last_input_at;
    return 0;
}

void remove_stream_state(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    memset(&g_streams[slot], 0, sizeof g_streams[slot]);
    pthread_mutex_unlock(&g_streams_mu);
}

void set_stream_input_info(int slot, int bonded, const char *peer_ip, int peer_port) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    g_streams[slot].input_bonded = bonded;
    if (!bonded) {
        snprintf(g_streams[slot].input_peer_ip, sizeof g_streams[slot].input_peer_ip, "%s",
                 peer_ip);
        g_streams[slot].input_peer_port = peer_port;
    }
    pthread_mutex_unlock(&g_streams_mu);
}

void set_stream_output_connected(int slot, int connected) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    g_streams[slot].output_connected = connected;
    if (!connected) {
        g_streams[slot].output_rtt_valid = 0;
        g_streams[slot].output_latency_valid = 0;
        g_streams[slot].output_extra_stats_valid = 0;
    }
    // Deliberately leave last_error/last_error_at_ms in place on reconnect: a
    // status poller can easily land after a fast reconnect already cleared it,
    // hiding a real failure that just happened. last_error is superseded by a
    // newer set_stream_errorf() call, or reset to empty by claim_stream_state()
    // when the slot is later reused for a fresh session.
    // retry_failures is not reset here: a connect that succeeds does not make the
    // downstream healthy (see OUTPUT_HEALTHY_MS in backoff.h).
    pthread_mutex_unlock(&g_streams_mu);
}

void set_stream_retry_failures(int slot, int failures) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    g_streams[slot].retry_failures = failures;
    pthread_mutex_unlock(&g_streams_mu);
}

void record_stream_input_progress(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;
    pthread_mutex_lock(&g_streams_mu);
    g_streams[slot].last_input_packet_at_ms = now_ms();
    pthread_mutex_unlock(&g_streams_mu);
}

void record_stream_forward_progress(int slot, int bytes) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS || bytes <= 0) return;
    pthread_mutex_lock(&g_streams_mu);
    g_streams[slot].forwarded_packets++;
    g_streams[slot].forwarded_bytes += bytes;
    g_streams[slot].last_packet_at_ms = now_ms();
    pthread_mutex_unlock(&g_streams_mu);
}

long long stream_state_last_stats_at_ms(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return 0;
    pthread_mutex_lock(&g_streams_mu);
    long long last = g_streams[slot].last_stats_at_ms;
    pthread_mutex_unlock(&g_streams_mu);
    return last;
}

void stream_state_snapshot(relay_stream_state_t *states, char *last_error, size_t last_error_sz) {
    pthread_mutex_lock(&g_streams_mu);
    memcpy(states, g_streams, sizeof g_streams);
    snprintf(last_error, last_error_sz, "%s", g_last_error);
    pthread_mutex_unlock(&g_streams_mu);
}

/* Applies one libsrt reading to the stream's state. The counters only ever move
 * forward (max of old and new), so a reading taken just after a leg dropped out
 * cannot make a total go backwards. */
void stream_state_apply_stats(int slot, const stats_sample_t *sample) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    const int can_read_in = sample->can_read_in;
    const int is_group_sock = sample->is_group;
    const int have_in = sample->have_in;
    const int have_in_latency = sample->have_in_latency;
    const int in_latency_ms = sample->in_latency_ms;
    const int have_out = sample->have_out;
    const SRT_TRACEBSTATS in_stats = sample->in_stats;
    const SRT_TRACEBSTATS out_stats = sample->out_stats;
    const relay_leg_state_t *legs = sample->legs;
    const int leg_count = sample->leg_count;
    const long long now = sample->taken_at_ms;

    leg_event_t events[2 * MAX_GROUP_MEMBERS];
    int event_count = 0;
    long long suppressed = 0;
    char streamid[sizeof g_streams[slot].streamid];

    pthread_mutex_lock(&g_streams_mu);
    if (can_read_in) {
        g_streams[slot].input_latency_valid = have_in_latency;
        if (have_in_latency) g_streams[slot].input_latency_ms = in_latency_ms;
    }
    g_streams[slot].input_extra_stats_valid = have_in && !is_group_sock;
    if (g_streams[slot].input_extra_stats_valid) {
        g_streams[slot].input_bandwidth_mbps = in_stats.mbpsBandwidth;
        g_streams[slot].input_recv_rate_mbps = in_stats.mbpsRecvRate;
        g_streams[slot].input_belated_total = in_stats.pktRcvBelated;
        g_streams[slot].input_belated_avg_ms = in_stats.pktRcvAvgBelatedTime;
        g_streams[slot].input_belated_avg_valid = sample->in_belated_avg_valid;
        g_streams[slot].input_undecrypt_total = in_stats.pktRcvUndecryptTotal;
        g_streams[slot].input_reorder_distance = in_stats.pktReorderDistance;
        g_streams[slot].input_rcv_buf_ms = in_stats.msRcvBuf;
    }
    if (have_in) {
        g_streams[slot].input_rtt_ms = in_stats.msRTT;
        g_streams[slot].input_rtt_valid = in_stats.msRTT > 0.0;
        g_streams[slot].recv_packets_total_valid =
            !(is_group_sock && in_stats.pktRecvTotal == 0 && in_stats.pktRecvUniqueTotal > 0);
        /* SRT group sockets only expose unique receive and group-drop
         * statistics. Loss/retransmission are meaningful per member socket,
         * but there is no deduplicated group equivalent. */
        g_streams[slot].recv_loss_total_valid = have_in && !is_group_sock;
        g_streams[slot].retrans_total_valid = have_in && !is_group_sock;
        g_streams[slot].recv_packets_total =
            max_ll(g_streams[slot].recv_packets_total, in_stats.pktRecvTotal);
        g_streams[slot].recv_unique_packets_total =
            max_ll(g_streams[slot].recv_unique_packets_total, in_stats.pktRecvUniqueTotal);
        g_streams[slot].recv_loss_total =
            max_int(g_streams[slot].recv_loss_total, in_stats.pktRcvLossTotal);
        g_streams[slot].recv_drop_total =
            max_int(g_streams[slot].recv_drop_total, in_stats.pktRcvDropTotal);
        g_streams[slot].retrans_total =
            max_int(g_streams[slot].retrans_total, in_stats.pktRcvRetrans);
    }
    if (have_out) {
        /* msRcvTsbPdDelay is the same negotiated latency SRTO_LATENCY reports,
         * already present in this bstats call. */
        g_streams[slot].output_latency_valid = out_stats.msRcvTsbPdDelay > 0;
        g_streams[slot].output_latency_ms = out_stats.msRcvTsbPdDelay;
        g_streams[slot].output_rtt_ms = out_stats.msRTT;
        g_streams[slot].output_rtt_valid = out_stats.msRTT > 0.0;
        g_streams[slot].output_extra_stats_valid = 1;
        g_streams[slot].output_sent_packets_total = out_stats.pktSentTotal;
        g_streams[slot].output_send_loss_total = out_stats.pktSndLossTotal;
        g_streams[slot].output_send_drop_total = out_stats.pktSndDropTotal;
        g_streams[slot].output_retrans_total = out_stats.pktRetransTotal;
        g_streams[slot].output_bandwidth_mbps = out_stats.mbpsBandwidth;
        g_streams[slot].output_send_rate_mbps = out_stats.mbpsSendRate;
        g_streams[slot].output_undecrypt_total = out_stats.pktRcvUndecryptTotal;
        g_streams[slot].output_snd_buf_ms = out_stats.msSndBuf;
    }
    g_streams[slot].leg_count = leg_count;
    for (int li = 0; li < leg_count; ++li) {
        g_streams[slot].legs[li] = legs[li];
    }
    g_streams[slot].last_stats_at_ms = now;
    if (sample->legs_read) {
        event_count = collect_leg_events(&g_streams[slot], legs, leg_count, now, events);
    }
    suppressed = rate_window_take_suppressed(&g_streams[slot].leg_log, now, LEG_LOG_WINDOW_MS);
    snprintf(streamid, sizeof streamid, "%s", g_streams[slot].streamid);
    pthread_mutex_unlock(&g_streams_mu);

    for (int i = 0; i < event_count; ++i) {
        relay_logf("Bonded leg %s streamid=%s leg=%s:%d legs=%d\n",
                   events[i].joined ? "joined" : "left", streamid, events[i].leg.ip,
                   events[i].leg.port, leg_count);
    }
    if (suppressed > 0) {
        relay_logf("Bonded leg events suppressed streamid=%s count=%lld in the last %d s\n",
                   streamid, suppressed, LEG_LOG_WINDOW_MS / 1000);
    }
}
