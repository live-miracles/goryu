/* A session's connection to the downstream (src/output.c): connecting, the retry
 * backoff after every failed connect or send, the healthy period, and that the tracker
 * and the stream's telemetry follow the connection. The transport is scripted here, the
 * tracker, stream state and backoff are the real ones, and the clock is fake, so the
 * 1, 2, 4, 8 and 16 s schedule runs in microseconds. */

#include "../../src/output.h"
#include "../../src/relay_common.h"
#include "../../src/stream_state.h"
#include "../../src/tracker.h"
#include "test.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdlib.h>

/* ---- stubs for what the modules take from libsrt, log.c and runtime.c ---- */

static long long g_now;
long long now_ms(void) {
    return g_now;
}
int relay_running(void) {
    return 1;
}
void sleep_ms(int ms) {
    g_now += ms;
}

#define MAX_CLOSED 32
static SRTSOCKET g_closed[MAX_CLOSED];
static int g_closed_count;
int srt_close(SRTSOCKET u) {
    if (g_closed_count < MAX_CLOSED) g_closed[g_closed_count] = u;
    ++g_closed_count;
    return 0;
}
static int closed_times(SRTSOCKET sock) {
    int n = 0;
    for (int i = 0; i < g_closed_count && i < MAX_CLOSED; ++i)
        if (g_closed[i] == sock) ++n;
    return n;
}

static char g_log[64][256];
static int g_log_count;
void relay_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_count < 64) vsnprintf(g_log[g_log_count++], sizeof g_log[0], fmt, ap);
    va_end(ap);
}
static int logged(const char *text) {
    int n = 0;
    for (int i = 0; i < g_log_count; ++i)
        if (strstr(g_log[i], text)) ++n;
    return n;
}

/* ---- the scripted transport ---- */

static int g_open_fails, g_connect_fails, g_send_fails;
static int g_opens, g_connects, g_sends;
static SRTSOCKET g_next_sock;
static void (*g_on_connect)(SRTSOCKET sock);
static int g_tracked_during_connect;
static int g_slot;

static SRTSOCKET fake_open(char *err, size_t err_sz) {
    ++g_opens;
    if (g_open_fails) {
        snprintf(err, err_sz, "fake open failed");
        return SRT_INVALID_SOCK;
    }
    return g_next_sock++;
}

static int fake_connect(SRTSOCKET sock, const output_target_t *target, const char *streamid,
                        char *err, size_t err_sz) {
    (void)target;
    (void)streamid;
    ++g_connects;
    g_tracked_during_connect = tracker_is_current(g_slot, SRT_INVALID_SOCK, sock);
    if (g_on_connect) g_on_connect(sock);
    if (g_connect_fails) {
        snprintf(err, err_sz, "fake connect refused");
        return -1;
    }
    return 0;
}

static int fake_send(SRTSOCKET sock, const char *buf, int len, char *reason, size_t reason_sz) {
    (void)sock;
    (void)buf;
    (void)len;
    ++g_sends;
    if (g_send_fails) {
        snprintf(reason, reason_sz, "fake send broken");
        return -1;
    }
    return 0;
}

static const output_transport_t g_fake = {
    .send_op = "fake_send",
    .open = fake_open,
    .connect = fake_connect,
    .send = fake_send,
};

/* ---- the harness ---- */

static relay_stream_state_t *g_snap;
static char g_snap_error[LAST_ERROR_MAX];
static const relay_stream_state_t *snapshot(int slot) {
    stream_state_snapshot(g_snap, g_snap_error, sizeof g_snap_error);
    return &g_snap[slot];
}

static output_target_t g_target;
static output_t g_out;

/* A registered session that holds `streamid` (none when empty), and its output. */
static void start(const char *streamid) {
    tracker_init();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        stream_state_end(i);
    g_now = 1000;
    g_closed_count = 0;
    g_log_count = 0;
    g_open_fails = g_connect_fails = g_send_fails = 0;
    g_opens = g_connects = g_sends = 0;
    g_next_sock = 100;
    g_on_connect = NULL;
    g_tracked_during_connect = 0;

    memset(&g_target, 0, sizeof g_target);
    struct sockaddr_in *sin = (struct sockaddr_in *)&g_target.out_addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(10080);
    inet_pton(AF_INET, "10.0.0.1", &sin->sin_addr);
    g_target.out_addrlen = sizeof *sin;

    g_slot = tracker_register(10);
    CHECK(g_slot >= 0);
    CHECK(tracker_acquire_stream(g_slot, streamid) == 0);
    output_init(&g_out, &g_fake, &g_target, g_slot, streamid, "10.0.1.20", 40000);
}

static void finish(void) {
    output_close(&g_out);
    tracker_release(g_slot, 10);
}

static const char *stream_error(void) {
    return snapshot(g_slot)->last_error;
}

/* ---- connecting ---- */

static void test_a_connect_makes_the_output_current(void) {
    start("camera1/main");
    output_service(&g_out);
    CHECK(g_opens == 1 && g_connects == 1);
    CHECK(output_socket(&g_out) == 100);
    CHECK(tracker_is_current(g_slot, SRT_INVALID_SOCK, 100));
    const relay_stream_state_t *st = snapshot(g_slot);
    CHECK(st->output_connected && st->retry_failures == 0);
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_SENT && g_sends == 1);

    output_service(&g_out); /* connected: nothing more to do */
    CHECK(g_opens == 1);
    finish();
}

/* A takeover or shutdown during the (blocking) connect can only close a socket that the
 * tracker already has. */
static void test_the_socket_is_tracked_before_it_connects(void) {
    start("camera1/main");
    output_service(&g_out);
    CHECK(g_tracked_during_connect == 1);
    finish();
}

static void test_a_failed_connect_closes_its_socket_once_and_backs_off(void) {
    start("camera1/main");
    g_connect_fails = 1;
    output_service(&g_out);
    CHECK(g_connects == 1 && output_socket(&g_out) == SRT_INVALID_SOCK);
    CHECK(closed_times(100) == 1);
    CHECK(!tracker_is_current(g_slot, SRT_INVALID_SOCK, 100));
    const relay_stream_state_t *st = snapshot(g_slot);
    CHECK(!st->output_connected && st->retry_failures == 1);
    CHECK_STR(st->last_error, "Failed to publish to downstream output: fake connect refused; "
                              "retrying in 1000 ms");
    CHECK(logged("Output publish retry streamid=camera1/main failures=1 retry_ms=1000") == 1);
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_DOWN && g_sends == 0);
    finish();
}

static void test_a_failed_open_is_a_failure_too(void) {
    start("camera1/main");
    g_open_fails = 1;
    output_service(&g_out);
    CHECK(g_opens == 1 && g_connects == 0 && g_closed_count == 0);
    CHECK(snapshot(g_slot)->retry_failures == 1);
    CHECK_STR(stream_error(),
              "Failed to publish to downstream output: fake open failed; retrying in 1000 ms");
    finish();
}

/* Every wait is bounded: 1, 2, 4, 8, then 16 s, and no attempt before its time. */
static void test_the_retry_schedule_is_bounded_and_waits_its_turn(void) {
    start("camera1/main");
    g_connect_fails = 1;
    const int delays[] = {1000, 2000, 4000, 8000, 16000, 16000, 16000};
    output_service(&g_out); /* the first attempt is at once */
    int connects = 1;
    CHECK(g_connects == connects);
    for (size_t i = 0; i < sizeof delays / sizeof delays[0]; ++i) {
        char want[64];
        snprintf(want, sizeof want, "failures=%d retry_ms=%d", (int)i + 1, delays[i]);
        CHECK(logged(want) == 1);
        g_now += delays[i] - 1;
        output_service(&g_out);
        CHECK(g_connects == connects); /* not due yet */
        g_now += 1;
        output_service(&g_out);
        CHECK(g_connects == ++connects);
        CHECK(snapshot(g_slot)->retry_failures == (int)i + 2);
    }
    finish();
}

/* ---- the downstream that accepts and then drops (ADR 0004) ---- */

static void test_a_failed_send_closes_and_backs_off(void) {
    start("camera1/main");
    output_service(&g_out);
    g_send_fails = 1;
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_FAILED);
    CHECK(closed_times(100) == 1);
    CHECK(output_socket(&g_out) == SRT_INVALID_SOCK);
    const relay_stream_state_t *st = snapshot(g_slot);
    CHECK(!st->output_connected && st->retry_failures == 1);
    CHECK_STR(st->last_error, "Relay output error to 10.0.0.1:10080 (input peer 10.0.1.20:40000): "
                              "fake send broken");
    CHECK_STR(g_snap_error, "fake_send: fake send broken");
    CHECK(logged("fake_send failed peer=10.0.1.20:40000 streamid=camera1/main "
                 "error=\"fake send broken\"") == 1);
    CHECK(logged("Output send retry streamid=camera1/main failures=1 retry_ms=1000") == 1);
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_DOWN && g_sends == 1); /* nothing sent */
    finish();
}

/* A connect that succeeds does not make the downstream healthy: a sink that accepts
 * and then drops must keep doubling the wait, not reconnect every second. */
static void test_a_reconnect_does_not_clear_the_failure_count(void) {
    start("camera1/main");
    g_send_fails = 1;
    const int delays[] = {1000, 2000, 4000, 8000};
    output_service(&g_out);
    for (size_t i = 0; i < sizeof delays / sizeof delays[0]; ++i) {
        CHECK(output_send(&g_out, "x", 1) == OUTPUT_FAILED);
        CHECK(snapshot(g_slot)->retry_failures == (int)i + 1);
        char want[64];
        snprintf(want, sizeof want, "failures=%d retry_ms=%d", (int)i + 1, delays[i]);
        CHECK(logged(want) == 1);
        g_now += delays[i] - 1;
        output_service(&g_out);
        CHECK(g_connects == (int)i + 1); /* not before its time */
        g_now += 1;
        output_service(&g_out);
        CHECK(g_connects == (int)i + 2);
        CHECK(snapshot(g_slot)->output_connected);
        CHECK(snapshot(g_slot)->retry_failures == (int)i + 1); /* the connect did not clear it */
    }
    finish();
}

static void test_the_failure_count_clears_after_the_healthy_period(void) {
    start("camera1/main");
    g_connect_fails = 1;
    output_service(&g_out); /* one failure */
    g_connect_fails = 0;
    g_now += 1000;
    output_service(&g_out); /* connected */
    CHECK(snapshot(g_slot)->output_connected && snapshot(g_slot)->retry_failures == 1);

    g_now += OUTPUT_HEALTHY_MS - 1;
    output_service(&g_out);
    CHECK(snapshot(g_slot)->retry_failures == 1); /* not yet */
    g_now += 1;
    output_service(&g_out);
    CHECK(snapshot(g_slot)->retry_failures == 0);

    /* the next failure starts a new run: the first delay again */
    g_send_fails = 1;
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_FAILED);
    CHECK(snapshot(g_slot)->retry_failures == 1);
    CHECK(logged("Output send retry streamid=camera1/main failures=1 retry_ms=1000") == 1);
    finish();
}

/* ---- closed from another thread ---- */

static void shutdown_during_connect(SRTSOCKET sock) {
    (void)sock;
    tracker_close_all(); /* what shutdown or a takeover does while this thread connects */
}

static void test_an_output_closed_during_connect_is_a_failure_and_closed_once(void) {
    start("camera1/main");
    g_on_connect = shutdown_during_connect;
    output_service(&g_out);
    CHECK(g_connects == 1);
    CHECK(output_socket(&g_out) == SRT_INVALID_SOCK);
    CHECK(closed_times(100) == 1); /* by the closer; this thread did not close it again */
    CHECK(snapshot(g_slot)->retry_failures == 1 && !snapshot(g_slot)->output_connected);
    finish();
}

static void test_closing_the_output_is_idempotent_and_closes_once(void) {
    start("camera1/main");
    output_service(&g_out);
    output_close(&g_out);
    output_close(&g_out);
    CHECK(closed_times(100) == 1 && output_socket(&g_out) == SRT_INVALID_SOCK);

    output_service(&g_out); /* a closed output never connects again */
    CHECK(g_connects == 1 && g_opens == 1);
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_DOWN && g_sends == 0);

    /* a takeover took the socket first: the session's close finds it gone */
    start("camera1/main");
    output_service(&g_out);
    tracker_close_all();
    output_close(&g_out);
    CHECK(closed_times(100) == 1);
    finish();
}

/* A session with no stream (an empty streamid) still forwards; the telemetry calls do
 * nothing for it. */
static void test_a_session_without_a_stream_still_forwards(void) {
    start("");
    output_service(&g_out);
    CHECK(output_socket(&g_out) == 100);
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_SENT);
    g_send_fails = 1;
    CHECK(output_send(&g_out, "x", 1) == OUTPUT_FAILED);
    CHECK(snapshot(g_slot)->streamid[0] == '\0' && snapshot(g_slot)->retry_failures == 0);
    CHECK(logged("streamid=(empty)") >= 1);
    finish();
}

int main(void) {
    g_snap = calloc(MAX_ACTIVE_SESSIONS, sizeof *g_snap);
    test_a_connect_makes_the_output_current();
    test_the_socket_is_tracked_before_it_connects();
    test_a_failed_connect_closes_its_socket_once_and_backs_off();
    test_a_failed_open_is_a_failure_too();
    test_the_retry_schedule_is_bounded_and_waits_its_turn();
    test_a_failed_send_closes_and_backs_off();
    test_a_reconnect_does_not_clear_the_failure_count();
    test_the_failure_count_clears_after_the_healthy_period();
    test_an_output_closed_during_connect_is_a_failure_and_closed_once();
    test_closing_the_output_is_idempotent_and_closes_once();
    test_a_session_without_a_stream_still_forwards();
    free(g_snap);
    TEST_SUMMARY("test_output");
}
