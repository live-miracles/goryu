#ifndef RATELIMIT_H
#define RATELIMIT_H

/* A fixed window that lets at most max_per_window events through, counting the
 * rest, so something that can repeat (a flapping link) cannot flood the log.
 * Zero-initialise it. Not thread-safe: callers hold their own lock. */
typedef struct rate_window {
    long long window_start_ms;
    int count;
    long long suppressed;
    int started;
} rate_window_t;

/* Returns 1 if this event may be emitted, 0 if it is suppressed (and counted).
 * A new window starts window_ms after the previous one began. */
int rate_window_allow(rate_window_t *w, long long now_ms, int max_per_window, long long window_ms);

/* The number of events suppressed in a window that has ended, for a "N events
 * suppressed" summary line; 0 while the window is still open or nothing was
 * suppressed. Reading it resets the count. Call it on every tick, not only when
 * an event arrives, or the summary of a burst that then goes quiet is never seen. */
long long rate_window_take_suppressed(rate_window_t *w, long long now_ms, long long window_ms);

#endif
