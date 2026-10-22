#include "ratelimit.h"

static void roll_if_expired(rate_window_t *w, long long now_ms, long long window_ms) {
    if (!w->started || now_ms - w->window_start_ms >= window_ms || now_ms < w->window_start_ms) {
        w->started = 1;
        w->window_start_ms = now_ms;
        w->count = 0;
    }
}

int rate_window_allow(rate_window_t *w, long long now_ms, int max_per_window, long long window_ms) {
    roll_if_expired(w, now_ms, window_ms);
    if (w->count < max_per_window) {
        w->count++;
        return 1;
    }
    w->suppressed++;
    return 0;
}

long long rate_window_take_suppressed(rate_window_t *w, long long now_ms, long long window_ms) {
    if (w->suppressed == 0) return 0;
    if (w->started && now_ms >= w->window_start_ms && now_ms - w->window_start_ms < window_ms) {
        return 0; /* the window is still open */
    }
    long long n = w->suppressed;
    w->suppressed = 0;
    return n;
}
