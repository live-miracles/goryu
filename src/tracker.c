#include "tracker.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "relay_common.h"

typedef struct active_session {
    int in_use;
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

void tracker_set_streamid(int slot, const char *streamid) {
    if (slot < 0 || slot >= MAX_ACTIVE_SESSIONS) return;

    pthread_mutex_lock(&g_tracker_mu);
    if (g_active[slot].in_use) {
        g_active[slot].streamid[0] = '\0';
        if (streamid) {
            snprintf(g_active[slot].streamid, sizeof g_active[slot].streamid, "%s", streamid);
        }
    }
    pthread_mutex_unlock(&g_tracker_mu);
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

void tracker_close_streamid(const char *streamid, int except_slot) {
    SRTSOCKET sockets[MAX_ACTIVE_SESSIONS * 2];
    int count = 0;

    if (!streamid || !streamid[0]) return;

    pthread_mutex_lock(&g_tracker_mu);
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!g_active[i].in_use || i == except_slot) continue;
        if (strcmp(g_active[i].streamid, streamid) != 0) continue;
        take_slot_sockets(i, sockets, &count);
    }
    pthread_mutex_unlock(&g_tracker_mu);

    for (int i = 0; i < count; ++i) {
        srt_close(sockets[i]);
    }
}

void tracker_unregister(int slot) {
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

void tracker_lock(void) {
    pthread_mutex_lock(&g_tracker_mu);
}

void tracker_unlock(void) {
    pthread_mutex_unlock(&g_tracker_mu);
}

int tracker_input_is_locked(int slot, SRTSOCKET sock) {
    return slot >= 0 && slot < MAX_ACTIVE_SESSIONS && sock != SRT_INVALID_SOCK &&
           g_active[slot].in_use && g_active[slot].input_sock == sock;
}

int tracker_output_is_locked(int slot, SRTSOCKET sock) {
    return slot >= 0 && slot < MAX_ACTIVE_SESSIONS && sock != SRT_INVALID_SOCK &&
           g_active[slot].in_use && g_active[slot].output_sock == sock;
}
