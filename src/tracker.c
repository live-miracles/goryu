#include "tracker.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "relay_common.h"
#include "runtime.h"
#include "stream_state.h"

typedef struct active_session {
    int in_use;
    /* The streamid this session holds: set when it wins the claim, empty before and
     * after. The only record of who holds a streamid. */
    char streamid[1024];
    SRTSOCKET input_sock;
    SRTSOCKET output_sock;
} active_session_t;

static pthread_mutex_t g_tracker_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tracker_cond = PTHREAD_COND_INITIALIZER;
static active_session_t g_active[MAX_ACTIVE_SESSIONS];
static int g_active_threads = 0;

void tracker_init(void) {
    pthread_mutex_lock(&g_tracker_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        memset(&g_active[i], 0, sizeof g_active[i]);
        g_active[i].input_sock = SRT_INVALID_SOCK;
        g_active[i].output_sock = SRT_INVALID_SOCK;
    }
    g_active_threads = 0;
    pthread_mutex_unlock(&g_tracker_mu);
}

int tracker_register(SRTSOCKET conn) {
    int slot = -1;

    pthread_mutex_lock(&g_tracker_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!g_active[i].in_use) {
            slot = i;
            memset(&g_active[i], 0, sizeof g_active[i]);
            g_active[i].in_use = 1;
            g_active[i].input_sock = conn;
            g_active[i].output_sock = SRT_INVALID_SOCK;
            g_active_threads++;
            break;
        }
    }
    pthread_mutex_unlock(&g_tracker_mu);

    return slot;
}

void tracker_set_output_socket(int slot, SRTSOCKET sock) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use) {
        g_active[slot].output_sock = sock;
    }
    pthread_mutex_unlock(&g_tracker_mu);
}

SRTSOCKET tracker_take_input(int slot, SRTSOCKET expected) {
    SRTSOCKET sock = SRT_INVALID_SOCK;

    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return SRT_INVALID_SOCK;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use && g_active[slot].input_sock == expected) {
        sock = g_active[slot].input_sock;
        g_active[slot].input_sock = SRT_INVALID_SOCK;
    }
    pthread_mutex_unlock(&g_tracker_mu);

    return sock;
}

SRTSOCKET tracker_take_output(int slot, SRTSOCKET expected) {
    SRTSOCKET sock = SRT_INVALID_SOCK;

    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return SRT_INVALID_SOCK;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use && g_active[slot].output_sock == expected) {
        sock = g_active[slot].output_sock;
        g_active[slot].output_sock = SRT_INVALID_SOCK;
    }
    pthread_mutex_unlock(&g_tracker_mu);

    return sock;
}

int tracker_is_current(int slot, SRTSOCKET input_sock, SRTSOCKET output_sock) {
    int current = 0;

    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return 0;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use &&
        (input_sock == SRT_INVALID_SOCK || g_active[slot].input_sock == input_sock) &&
        (output_sock == SRT_INVALID_SOCK || g_active[slot].output_sock == output_sock)) {
        current = 1;
    }
    pthread_mutex_unlock(&g_tracker_mu);

    return current;
}

/* Stops tracking the slot's sockets and appends them to sockets[] for the caller
 * to close once it has released the lock. Clearing them here, under the lock, is
 * what makes exactly one thread call srt_close on each socket. Must hold the
 * lock. */
static void take_slot_sockets(int slot, SRTSOCKET *sockets, int *count) {
    if (g_active[slot].input_sock != SRT_INVALID_SOCK) {
        sockets[(*count)++] = g_active[slot].input_sock;
        g_active[slot].input_sock = SRT_INVALID_SOCK;
    }
    if (g_active[slot].output_sock != SRT_INVALID_SOCK) {
        sockets[(*count)++] = g_active[slot].output_sock;
        g_active[slot].output_sock = SRT_INVALID_SOCK;
    }
}

/* The slot of the session holding streamid, or -1. Must hold the lock. */
static int holder_of(const char *streamid) {
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (g_active[i].in_use && g_active[i].streamid[0] &&
            strcmp(g_active[i].streamid, streamid) == 0) {
            return i;
        }
    }
    return -1;
}

typedef enum claim_result {
    CLAIM_OK,        /* the slot now holds the stream */
    CLAIM_IN_USE,    /* another session holds the streamid */
    CLAIM_SLOT_BUSY, /* the slot is not registered or still holds an earlier stream */
} claim_result_t;

/* One attempt to claim streamid for `slot`. When another session holds it and
 * `may_take_over` is set and that holder's input has been silent for
 * DUPLICATE_TAKEOVER_STALE_MS, the holder's sockets are taken into sockets[] (for the
 * caller to close) and *took_over is set. The check and the take are one step under the
 * lock, so what is closed is the session that was judged stale. */
static claim_result_t claim_once(int slot, const char *streamid, long long now, int may_take_over,
                                 SRTSOCKET sockets[2], int *count, int *took_over) {
    claim_result_t result;

    pthread_mutex_lock(&g_tracker_mu);
    int holder = holder_of(streamid);
    if (holder >= 0) {
        result = CLAIM_IN_USE;
        long long input_age_ms = 0;
        if (may_take_over && stream_state_input_age_ms(holder, now, &input_age_ms) == 0 &&
            input_age_ms >= DUPLICATE_TAKEOVER_STALE_MS) {
            take_slot_sockets(holder, sockets, count);
            *took_over = 1;
        }
    } else if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS || !g_active[slot].in_use ||
               g_active[slot].streamid[0] || stream_state_begin(slot, streamid) != 0) {
        result = CLAIM_SLOT_BUSY;
    } else {
        snprintf(g_active[slot].streamid, sizeof g_active[slot].streamid, "%s", streamid);
        result = CLAIM_OK;
    }
    pthread_mutex_unlock(&g_tracker_mu);
    return result;
}

int tracker_acquire_stream(int slot, const char *streamid) {
    if (!streamid || !streamid[0]) return 0;

    long long deadline = now_ms() + DUPLICATE_TAKEOVER_WAIT_MS;
    int takeover_started = 0;
    claim_result_t claim;
    do {
        SRTSOCKET sockets[2];
        int count = 0;
        int took_over = 0;
        claim =
            claim_once(slot, streamid, now_ms(), !takeover_started, sockets, &count, &took_over);
        if (took_over) {
            takeover_started = 1;
            set_last_errorf("Duplicate publisher takeover for stale streamid=%s", streamid);
            relay_logf("Duplicate publisher takeover for stale streamid=%s\n", streamid);
            for (int i = 0; i < count; ++i) {
                srt_close(sockets[i]);
            }
        }
        if (claim != CLAIM_IN_USE) break;
        sleep_ms(50);
    } while (relay_running() && now_ms() < deadline);

    if (claim == CLAIM_IN_USE) {
        set_last_errorf("Duplicate publisher rejected for active streamid=%s", streamid);
        relay_logf("Duplicate publisher rejected for active streamid=%s\n", streamid);
        return -1;
    }
    if (claim == CLAIM_SLOT_BUSY) {
        set_last_errorf("No stream state slot available for streamid=%s", streamid);
        relay_logf("No stream state slot available for streamid=%s\n", streamid);
        return -1;
    }
    return 0;
}

static void unregister_slot(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use) {
        memset(&g_active[slot], 0, sizeof g_active[slot]);
        g_active[slot].input_sock = SRT_INVALID_SOCK;
        g_active[slot].output_sock = SRT_INVALID_SOCK;
        if (g_active_threads > 0) g_active_threads--;
        pthread_cond_broadcast(&g_tracker_cond);
    }
    pthread_mutex_unlock(&g_tracker_mu);
}

/* The stream goes before the slot is handed on: the next session to get this slot
 * claims it, and a slot that still holds a stream is refused. */
static void end_stream(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use && g_active[slot].streamid[0]) {
        stream_state_end(slot);
        g_active[slot].streamid[0] = '\0';
    }
    pthread_mutex_unlock(&g_tracker_mu);
}

void tracker_release(int slot, SRTSOCKET input) {
    end_stream(slot);
    if (tracker_take_input(slot, input) != SRT_INVALID_SOCK) srt_close(input);
    unregister_slot(slot);
}

void tracker_close_all(void) {
    SRTSOCKET sockets[MAX_ACTIVE_SESSIONS * 2];
    int count = 0;

    pthread_mutex_lock(&g_tracker_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!g_active[i].in_use) continue;
        take_slot_sockets(i, sockets, &count);
    }
    pthread_mutex_unlock(&g_tracker_mu);

    for (int i = 0; i < count; ++i) {
        srt_close(sockets[i]);
    }
}

void tracker_wait_all(void) {
    pthread_mutex_lock(&g_tracker_mu);
    while (g_active_threads > 0) {
        pthread_cond_wait(&g_tracker_cond, &g_tracker_mu);
    }
    pthread_mutex_unlock(&g_tracker_mu);
}

static int socket_is_tracked(int slot, SRTSOCKET sock, int output) {
    return slot >= 0 && slot < MAX_ACTIVE_SESSIONS && sock != SRT_INVALID_SOCK &&
           g_active[slot].in_use &&
           (output ? g_active[slot].output_sock : g_active[slot].input_sock) == sock;
}

void tracker_read_sockets(int slot, SRTSOCKET input, SRTSOCKET output, tracker_read_fn read_fn,
                          void *ctx) {
    pthread_mutex_lock(&g_tracker_mu);
    read_fn(socket_is_tracked(slot, input, 0), socket_is_tracked(slot, output, 1), ctx);
    pthread_mutex_unlock(&g_tracker_mu);
}
