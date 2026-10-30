#include "stream_state.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "runtime.h"
#include "telemetry.h"

static pthread_mutex_t g_streams_mu = PTHREAD_MUTEX_INITIALIZER;
static relay_stream_state_t g_streams[MAX_ACTIVE_SESSIONS];
static char g_last_error[LAST_ERROR_MAX];

void set_last_errorf(const char *fmt, ...) {
    pthread_mutex_lock(&g_streams_mu);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof g_last_error, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_streams_mu);
}

/* The slot's stream, or NULL when the slot is outside the table or holds no stream.
 * Every per-stream function goes through this, so a session without a stream can call
 * them all freely. Must hold g_streams_mu. */
static relay_stream_state_t *stream_in(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS || !g_streams[slot].streamid[0]) return NULL;
    return &g_streams[slot];
}

void set_stream_errorf(int slot, const char *fmt, ...) {
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(st->last_error, sizeof st->last_error, fmt, ap);
        va_end(ap);
        st->last_error_at_ms = now_ms();
    }
    pthread_mutex_unlock(&g_streams_mu);
}

claim_result_t claim_stream_state(const char *streamid, int slot) {
    if (!streamid || !streamid[0]) return CLAIM_STREAM_NO_ID;
    claim_result_t result = CLAIM_STREAM_OK;
    pthread_mutex_lock(&g_streams_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (g_streams[i].streamid[0] && strcmp(g_streams[i].streamid, streamid) == 0) {
            result = CLAIM_STREAM_IN_USE;
            break;
        }
    }
    if (result == CLAIM_STREAM_OK) {
        if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS || g_streams[slot].streamid[0]) {
            result = CLAIM_STREAM_SLOT_BUSY;
        } else {
            memset(&g_streams[slot], 0, sizeof g_streams[slot]);
            snprintf(g_streams[slot].streamid, sizeof g_streams[slot].streamid, "%s", streamid);
            g_streams[slot].input_active = 1;
            g_streams[slot].last_input_packet_at_ms = now_ms();
        }
    }
    pthread_mutex_unlock(&g_streams_mu);
    return result;
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
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) {
        st->input_bonded = bonded;
        if (!bonded) {
            snprintf(st->input_peer_ip, sizeof st->input_peer_ip, "%s", peer_ip);
            st->input_peer_port = peer_port;
        }
    }
    pthread_mutex_unlock(&g_streams_mu);
}

void set_stream_output_connected(int slot, int connected) {
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) {
        st->output_connected = connected;
        if (!connected) st->have_output_counters = 0;
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
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) st->retry_failures = failures;
    pthread_mutex_unlock(&g_streams_mu);
}

void record_stream_input_progress(int slot) {
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) st->last_input_packet_at_ms = now_ms();
    pthread_mutex_unlock(&g_streams_mu);
}

void record_stream_forward_progress(int slot, int bytes) {
    if (bytes <= 0) return;
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (st) {
        st->forwarded_packets++;
        st->forwarded_bytes += bytes;
        st->last_packet_at_ms = now_ms();
    }
    pthread_mutex_unlock(&g_streams_mu);
}

long long stream_state_last_stats_at_ms(int slot) {
    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    long long last = st ? st->last_stats_at_ms : -1;
    pthread_mutex_unlock(&g_streams_mu);
    return last;
}

void stream_state_snapshot(relay_stream_state_t *states, char *last_error, size_t last_error_sz) {
    pthread_mutex_lock(&g_streams_mu);
    memcpy(states, g_streams, sizeof g_streams);
    snprintf(last_error, last_error_sz, "%s", g_last_error);
    pthread_mutex_unlock(&g_streams_mu);
}

/* Applies one libsrt reading to the stream's state (the rules are in telemetry.c) and
 * logs the bonded legs that joined or left. */
void stream_state_apply_stats(int slot, const stats_sample_t *sample) {
    telemetry_result_t result;
    char streamid[sizeof g_streams[0].streamid];
    int leg_count;

    pthread_mutex_lock(&g_streams_mu);
    relay_stream_state_t *st = stream_in(slot);
    if (!st) {
        pthread_mutex_unlock(&g_streams_mu);
        return;
    }
    telemetry_apply(st, sample, &result);
    leg_count = st->leg_count;
    snprintf(streamid, sizeof streamid, "%s", st->streamid);
    pthread_mutex_unlock(&g_streams_mu);

    for (int i = 0; i < result.event_count; ++i) {
        relay_logf("Bonded leg %s streamid=%s leg=%s:%d legs=%d\n",
                   result.events[i].joined ? "joined" : "left", streamid, result.events[i].leg.ip,
                   result.events[i].leg.port, leg_count);
    }
    if (result.suppressed > 0) {
        relay_logf("Bonded leg events suppressed streamid=%s count=%lld in the last %d s\n",
                   streamid, result.suppressed, LEG_LOG_WINDOW_MS / 1000);
    }
}
