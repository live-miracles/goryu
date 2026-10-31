/* A session's connection to the publisher (src/input.c): what it learns when it opens
 * the accepted connection (streamid, bonded or plain), the four things a receive can
 * come to, and that a failure is told once in the log, the process lastError and the
 * stream's lastError. The transport is scripted here; the tracker and the stream state
 * are the real ones. */

#include "../../src/input.h"
#include "../../src/relay_common.h"
#include "../../src/stream_state.h"
#include "../../src/tracker.h"
#include "test.h"

#include <stdarg.h>
#include <stdlib.h>

/* ---- stubs for what the modules take from libsrt, log.c and runtime.c ---- */

static long long g_now;
long long now_ms(void) {
    return g_now;
}
long long wall_ms(void) {
    return g_now + 1700000000000LL;
}
int relay_running(void) {
    return 1;
}
void sleep_ms(int ms) {
    g_now += ms;
}
int srt_close(SRTSOCKET u) {
    (void)u;
    return 0;
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

static char g_conn_streamid[1024]; /* what the publisher set on the connection */
static int g_conn_bonded;
static char g_timeout_error[256];
static int g_timeout_asked_ms;
static int g_is_connected;
static input_result_t g_recv_result;
static int g_recv_len;
static char g_recv_reason[256];
static int g_recvs;

static void fake_open(SRTSOCKET conn, int recv_timeout_ms, input_info_t *info) {
    (void)conn;
    g_timeout_asked_ms = recv_timeout_ms;
    snprintf(info->streamid, sizeof info->streamid, "%s", g_conn_streamid);
    info->bonded = g_conn_bonded;
    snprintf(info->timeout_error, sizeof info->timeout_error, "%s", g_timeout_error);
}

static int fake_connected(SRTSOCKET conn) {
    (void)conn;
    return g_is_connected;
}

static input_result_t fake_recv(SRTSOCKET conn, char *buf, int cap, int *len, char *reason,
                                size_t reason_sz) {
    (void)conn;
    ++g_recvs;
    if (g_recv_result == INPUT_DATA) {
        memset(buf, 'x', (size_t)g_recv_len < (size_t)cap ? (size_t)g_recv_len : (size_t)cap);
        *len = g_recv_len;
    }
    if (g_recv_result == INPUT_FAILED) snprintf(reason, reason_sz, "%s", g_recv_reason);
    return g_recv_result;
}

static const input_transport_t g_fake = {
    .recv_op = "fake_recv",
    .open = fake_open,
    .connected = fake_connected,
    .recv = fake_recv,
};

/* ---- the harness ---- */

static relay_stream_state_t *g_snap;
static char g_snap_error[LAST_ERROR_MAX];
static const relay_stream_state_t *snapshot(int slot) {
    stream_state_snapshot(g_snap, g_snap_error, sizeof g_snap_error);
    return &g_snap[slot];
}

static input_t g_in;
static int g_slot;

/* An accepted connection whose publisher set `conn_streamid` (none when empty), opened
 * with `default_streamid` as the fallback. The session holds the stream it ends up
 * with, as session.c does. */
static void start(const char *conn_streamid, int bonded, const char *default_streamid) {
    tracker_init();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        stream_state_end(i);
    g_now = 1000;
    g_log_count = 0;
    snprintf(g_conn_streamid, sizeof g_conn_streamid, "%s", conn_streamid);
    g_conn_bonded = bonded;
    g_timeout_error[0] = '\0';
    g_timeout_asked_ms = 0;
    g_is_connected = 1;
    g_recv_result = INPUT_IDLE;
    g_recv_len = 0;
    g_recv_reason[0] = '\0';
    g_recvs = 0;

    g_slot = tracker_register(10);
    CHECK(g_slot >= 0);
    input_open(&g_in, &g_fake, 10, g_slot, "10.0.1.20", 40000, default_streamid);
    CHECK(tracker_acquire_stream(g_slot, g_in.streamid) == 0);
}

static void finish(void) {
    tracker_release(g_slot, 10);
}

/* ---- opening ---- */

static void test_open_takes_the_streamid_from_the_connection(void) {
    start("camera1/main", 0, "from/uri");
    CHECK_STR(g_in.streamid, "camera1/main");
    CHECK(!g_in.bonded);
    CHECK(g_timeout_asked_ms == INPUT_RECV_TIMEOUT_MS);
    CHECK(logged("Accepted plain SRT source peer=10.0.1.20:40000 streamid=camera1/main") == 1);
    finish();
}

static void test_open_reports_a_bonded_group(void) {
    start("camera1/main", 1, "");
    CHECK(g_in.bonded);
    CHECK(logged("Accepted bonded SRT source peer=10.0.1.20:40000 streamid=camera1/main") == 1);
    finish();
}

/* A publisher that sets no streamid gets the one in the downstream URI. */
static void test_open_falls_back_to_the_default_streamid(void) {
    start("", 0, "from/uri");
    CHECK_STR(g_in.streamid, "from/uri");
    CHECK(logged("streamid=from/uri") == 1);
    finish();
}

static void test_open_without_any_streamid_has_no_stream(void) {
    start("", 0, "");
    CHECK_STR(g_in.streamid, "");
    CHECK(logged("streamid=(empty)") == 1);
    CHECK(snapshot(g_slot)->streamid[0] == '\0');
    finish();
}

/* A refused receive timeout is logged and the input carries on: the session still
 * forwards, it only notices a closed connection later. */
static void test_a_refused_receive_timeout_is_logged_and_tolerated(void) {
    start("camera1/main", 0, "");
    finish();

    g_log_count = 0;
    snprintf(g_timeout_error, sizeof g_timeout_error, "fake timeout refused");
    g_slot = tracker_register(10);
    input_open(&g_in, &g_fake, 10, g_slot, "10.0.1.20", 40000, "");
    CHECK(logged("SRTO_RCVTIMEO rejected for peer=10.0.1.20:40000: fake timeout refused") == 1);
    CHECK(logged("Accepted plain SRT source") == 1);
    char buf[16];
    int len = 0;
    g_recv_result = INPUT_IDLE;
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_IDLE);
    tracker_release(g_slot, 10);
}

/* ---- receiving ---- */

static void test_data_is_returned_with_its_length(void) {
    start("camera1/main", 0, "");
    char buf[32];
    int len = 0;
    g_recv_result = INPUT_DATA;
    g_recv_len = 7;
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_DATA);
    CHECK(len == 7 && buf[0] == 'x' && buf[6] == 'x');
    CHECK(g_log_count == 1); /* the accept line only */
    finish();
}

/* Nothing arrived within the timeout, and a lost connection: neither is an error, so
 * neither is logged or recorded. */
static void test_idle_and_closed_are_not_errors(void) {
    start("camera1/main", 0, "");
    char buf[32];
    int len = 0;
    g_recv_result = INPUT_IDLE;
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_IDLE);
    g_recv_result = INPUT_CLOSED;
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_CLOSED);
    CHECK(g_log_count == 1);
    const relay_stream_state_t *st = snapshot(g_slot);
    CHECK(st->last_error[0] == '\0' && g_snap_error[0] == '\0');
    finish();
}

/* A failure is told in the log, the process lastError and the stream's lastError, with
 * the transport's name for the call and its reason. */
static void test_a_failed_receive_is_recorded_everywhere(void) {
    start("camera1/main", 0, "");
    char buf[32];
    int len = 0;
    g_recv_result = INPUT_FAILED;
    snprintf(g_recv_reason, sizeof g_recv_reason, "fake receive broke");
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_FAILED);
    CHECK(logged("fake_recv failed peer=10.0.1.20:40000 streamid=camera1/main "
                 "error=\"fake receive broke\"") == 1);
    const relay_stream_state_t *st = snapshot(g_slot);
    CHECK_STR(g_snap_error, "fake_recv: fake receive broke");
    CHECK_STR(st->last_error, "Input error from peer 10.0.1.20:40000: fake receive broke");
    finish();
}

static void test_a_failure_without_a_stream_still_reaches_the_log_and_process_error(void) {
    start("", 0, "");
    char buf[32];
    int len = 0;
    g_recv_result = INPUT_FAILED;
    snprintf(g_recv_reason, sizeof g_recv_reason, "fake receive broke");
    CHECK(input_recv(&g_in, buf, sizeof buf, &len) == INPUT_FAILED);
    CHECK(logged("fake_recv failed peer=10.0.1.20:40000 streamid=(empty)") == 1);
    snapshot(g_slot);
    CHECK_STR(g_snap_error, "fake_recv: fake receive broke");
    finish();
}

static void test_connected_asks_the_transport(void) {
    start("camera1/main", 0, "");
    CHECK(input_connected(&g_in));
    g_is_connected = 0;
    CHECK(!input_connected(&g_in));
    CHECK(g_recvs == 0); /* asking never reads */
    finish();
}

int main(void) {
    g_snap = calloc(MAX_ACTIVE_SESSIONS, sizeof *g_snap);
    test_open_takes_the_streamid_from_the_connection();
    test_open_reports_a_bonded_group();
    test_open_falls_back_to_the_default_streamid();
    test_open_without_any_streamid_has_no_stream();
    test_a_refused_receive_timeout_is_logged_and_tolerated();
    test_data_is_returned_with_its_length();
    test_idle_and_closed_are_not_errors();
    test_a_failed_receive_is_recorded_everywhere();
    test_a_failure_without_a_stream_still_reaches_the_log_and_process_error();
    test_connected_asks_the_transport();
    free(g_snap);
    TEST_SUMMARY("test_input");
}
