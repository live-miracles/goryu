#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <srt/srt.h>

#include "ratelimit.h"
#include "runtime.h"

/* Prefixes every log line with a local-time, millisecond-precision timestamp
 * (matching SRS's "[2014-05-27 19:21:27.276]" convention) so log output can
 * be correlated during troubleshooting without relying on the process
 * supervisor to stamp it. */
void relay_logf(const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char time_buf[32];
    strftime(time_buf, sizeof time_buf, "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(stderr, "[%s.%03ld] ", time_buf, ts.tv_nsec / 1000000);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* libsrt's own internal logging, capped at the source.
 *
 * Incident (2026-09-29): a receive-queue backlog on a bonded leg made libsrt
 * log "No room to store incoming packet"/"RCV-DROPPED" once per affected
 * packet — ~35 lines/sec sustained for 16 minutes (34k+ lines from that one
 * message class alone). On a small host that log volume alone was enough
 * CPU/IO load to starve systemd-journald, which cascaded into every
 * D-Bus-dependent service (logind, resolved, networkd) going unresponsive —
 * taking SSH and the dashboard down with it, well beyond just this relay.
 *
 * Two layers of defense:
 *  1. srt_setloglevel(LOG_ERR) below stops libsrt from even formatting/
 *     emitting anything at WARNING or below — this is what the incident's
 *     message class was tagged at, so this alone would have prevented it.
 *  2. This handler is a hard backstop for whatever's still allowed through:
 *     a flat cap on total lines per second, regardless of level or message
 *     content, so no future high-volume message class (known or not) can
 *     reproduce the same failure. Excess lines are dropped, not queued —
 *     with a one-line "N more suppressed" summary once the window rolls
 *     over, so the fact that something was noisy is still visible without
 *     the volume itself being the danger. */
#define SRT_LOG_MAX_LINES_PER_SEC 20
#define SRT_LOG_WINDOW_MS 1000

static pthread_mutex_t g_srt_log_mu = PTHREAD_MUTEX_INITIALIZER;
static rate_window_t g_srt_log_window; /* the same window the leg log lines use */

static void srt_log_handler(void *opaque, int level, const char *file, int line, const char *area,
                            const char *message) {
    (void)opaque;
    (void)file;
    (void)line;

    long long now = now_ms();

    pthread_mutex_lock(&g_srt_log_mu);
    long long suppressed_to_report =
        rate_window_take_suppressed(&g_srt_log_window, now, SRT_LOG_WINDOW_MS);
    int emit =
        rate_window_allow(&g_srt_log_window, now, SRT_LOG_MAX_LINES_PER_SEC, SRT_LOG_WINDOW_MS);
    pthread_mutex_unlock(&g_srt_log_mu);

    if (suppressed_to_report > 0) {
        relay_logf("srt: %lld more log line(s) suppressed in the last second (rate limit)\n",
                   suppressed_to_report);
    }
    if (emit) {
        relay_logf("srt[%d] %s: %s\n", level, area ? area : "?", message ? message : "");
    }
}

void log_install_srt_cap(void) {
    srt_setloglevel(LOG_ERR);
    srt_setloghandler(NULL, srt_log_handler);
}
