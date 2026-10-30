/* A session's place in the relay (src/tracker.c): the slot table, the streamid claim, the
 * duplicate-publisher wait and takeover, the ordered release, and that each socket is
 * closed exactly once. libsrt is not involved: srt_close is recorded, and the clock is
 * fake, so the 5 s waits run in microseconds. Each step of "a session ends" that the
 * tracker orders is checked through the stream state's snapshot. */

#include "../../src/relay_common.h"
#include "../../src/stream_state.h"
#include "../../src/tracker.h"
#include "test.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- stubs for what tracker.c takes from libsrt, log.c and runtime.c ---- */

static long long g_now;
static int g_running;
/* Called by sleep_ms() with the time slept so far in the wait, so a test can play the
 * other threads: a holder that keeps sending, one that notices it was closed, ... */
static void (*g_on_sleep)(void);
static int g_sleeps;

long long now_ms(void) {
    return g_now;
}
int relay_running(void) {
    return g_running;
}
void sleep_ms(int ms) {
    g_now += ms;
    ++g_sleeps;
    if (g_on_sleep) g_on_sleep();
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

static relay_stream_state_t *g_snap;
static char g_snap_error[LAST_ERROR_MAX];
static const relay_stream_state_t *snapshot(int slot) {
    stream_state_snapshot(g_snap, g_snap_error, sizeof g_snap_error);
    return &g_snap[slot];
}

static void reset(void) {
    tracker_init();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        stream_state_end(i);
    g_now = 1000;
    g_running = 1;
    g_on_sleep = NULL;
    g_sleeps = 0;
    g_closed_count = 0;
    g_log_count = 0;
}

/* A session's registration, with a socket number derived from its slot's order. */
static int start_session(SRTSOCKET input) {
    int slot = tracker_register(input);
    CHECK(slot >= 0);
    return slot;
}

/* ---- the slot table ---- */

static void test_the_table_is_bounded(void) {
    reset();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        CHECK(tracker_register(100 + i) == i);
    CHECK(tracker_register(999) == -1);
    tracker_release(5, 105);
    CHECK(tracker_register(999) == 5); /* a released slot is handed on */
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        tracker_release(i, i == 5 ? 999 : 100 + i);
}

/* ---- the claim ---- */

static void test_a_free_streamid_is_claimed(void) {
    reset();
    int a = start_session(10);
    CHECK(tracker_acquire_stream(a, "camera1/main") == 0);
    CHECK_STR(snapshot(a)->streamid, "camera1/main");
    CHECK(g_sleeps == 0 && g_log_count == 0);
    tracker_release(a, 10);
}

static void test_an_empty_streamid_has_no_stream(void) {
    reset();
    int a = start_session(10);
    CHECK(tracker_acquire_stream(a, "") == 0);
    CHECK(tracker_acquire_stream(a, NULL) == 0);
    CHECK(snapshot(a)->streamid[0] == '\0');
    /* it holds nothing, so two such sessions never contend */
    int b = start_session(11);
    CHECK(tracker_acquire_stream(b, "") == 0);
    CHECK(g_sleeps == 0);
    tracker_release(a, 10);
    tracker_release(b, 11);
}

/* ---- a duplicate publisher ---- */

static int g_holder_slot;
static void holder_keeps_sending(void) {
    record_stream_input_progress(g_holder_slot);
}

static void test_a_duplicate_of_a_live_stream_waits_then_is_rejected(void) {
    reset();
    int holder = start_session(10);
    CHECK(tracker_acquire_stream(holder, "camera1/main") == 0);
    int dup = start_session(11);

    g_holder_slot = holder;
    g_on_sleep = holder_keeps_sending;
    long long started = g_now;
    CHECK(tracker_acquire_stream(dup, "camera1/main") == -1);
    CHECK(g_now - started >= DUPLICATE_TAKEOVER_WAIT_MS);
    CHECK(g_now - started < DUPLICATE_TAKEOVER_WAIT_MS + 100);
    CHECK(logged("Duplicate publisher rejected for active streamid=camera1/main") == 1);
    CHECK(logged("takeover") == 0);
    CHECK(closed_times(10) == 0 && closed_times(11) == 0); /* nobody is closed */
    CHECK_STR(snapshot(holder)->streamid, "camera1/main");
    CHECK(snapshot(dup)->streamid[0] == '\0');

    /* the rejected session still ends with a release, and the holder is untouched */
    tracker_release(dup, 11);
    CHECK(closed_times(11) == 1);
    CHECK_STR(snapshot(holder)->streamid, "camera1/main");
    tracker_release(holder, 10);
}

/* The stale holder's thread notices its socket was closed and ends its session. */
static int g_holder_input;
static void holder_notices_it_was_closed(void) {
    if (closed_times(g_holder_input) > 0 && g_holder_slot >= 0) {
        tracker_release(g_holder_slot, g_holder_input);
        g_holder_slot = -1;
    }
}

static void test_a_stale_holder_is_taken_over(void) {
    reset();
    int holder = start_session(10);
    CHECK(tracker_acquire_stream(holder, "camera1/main") == 0);
    tracker_set_output_socket(holder, 20);
    g_now += DUPLICATE_TAKEOVER_STALE_MS; /* its input has been silent for 5 s */
    int dup = start_session(11);

    g_holder_slot = holder;
    g_holder_input = 10;
    g_on_sleep = holder_notices_it_was_closed;
    CHECK(tracker_acquire_stream(dup, "camera1/main") == 0);
    CHECK(logged("Duplicate publisher takeover for stale streamid=camera1/main") == 1);
    CHECK(logged("rejected") == 0);
    /* both of the holder's sockets were closed, once each, though its own release ran
     * after the takeover took them */
    CHECK(closed_times(10) == 1 && closed_times(20) == 1);
    CHECK_STR(snapshot(dup)->streamid, "camera1/main");
    CHECK(snapshot(holder)->streamid[0] == '\0');
    tracker_release(dup, 11);
}

static void test_a_stale_holder_that_never_ends_is_closed_once_and_the_newcomer_rejected(void) {
    reset();
    int holder = start_session(10);
    CHECK(tracker_acquire_stream(holder, "camera1/main") == 0);
    g_now += DUPLICATE_TAKEOVER_STALE_MS;
    int dup = start_session(11);

    CHECK(tracker_acquire_stream(dup, "camera1/main") == -1);
    CHECK(logged("takeover") == 1); /* the holder is closed once, not every 50 ms */
    CHECK(closed_times(10) == 1);
    CHECK(logged("rejected") == 1);
    CHECK_STR(snapshot(holder)->streamid, "camera1/main");
    tracker_release(dup, 11);
    tracker_release(holder, 10);
    CHECK(closed_times(10) == 1); /* its release finds the input already taken */
}

/* A takeover closes the stale holder and nobody else: a second duplicate that is still
 * waiting out its own 5 s keeps its sockets. */
static void test_a_takeover_closes_only_the_holder(void) {
    reset();
    int holder = start_session(10);
    CHECK(tracker_acquire_stream(holder, "camera1/main") == 0);
    g_now += DUPLICATE_TAKEOVER_STALE_MS;
    int waiting = start_session(11); /* a second duplicate, not yet acquired */
    int taker = start_session(12);

    g_holder_slot = holder;
    g_holder_input = 10;
    g_on_sleep = holder_notices_it_was_closed;
    CHECK(tracker_acquire_stream(taker, "camera1/main") == 0);
    CHECK(closed_times(10) == 1);
    CHECK(closed_times(11) == 0 && closed_times(12) == 0);

    /* the second duplicate now meets a fresh holder and is rejected the normal way */
    g_holder_slot = taker;
    g_on_sleep = holder_keeps_sending;
    CHECK(tracker_acquire_stream(waiting, "camera1/main") == -1);
    CHECK(closed_times(12) == 0);
    tracker_release(waiting, 11);
    tracker_release(taker, 12);
}

static void test_shutdown_ends_the_wait(void) {
    reset();
    int holder = start_session(10);
    CHECK(tracker_acquire_stream(holder, "camera1/main") == 0);
    int dup = start_session(11);
    g_running = 0;
    long long started = g_now;
    CHECK(tracker_acquire_stream(dup, "camera1/main") == -1);
    CHECK(g_now - started <= 50); /* one poll, not the whole 5 s */
    tracker_release(dup, 11);
    tracker_release(holder, 10);
}

/* ---- release ---- */

/* One release frees the streamid and the slot: the next session on the same slot and the
 * next publisher of the same streamid are both accepted at once. */
static void test_release_frees_the_stream_and_the_slot_together(void) {
    reset();
    int a = start_session(10);
    CHECK(tracker_acquire_stream(a, "camera1/main") == 0);
    tracker_release(a, 10);
    CHECK(snapshot(a)->streamid[0] == '\0');

    int b = start_session(11);
    CHECK(b == a); /* the slot is reused */
    CHECK(tracker_acquire_stream(b, "camera1/main") == 0);
    CHECK(g_sleeps == 0 && g_log_count == 0);
    tracker_release(b, 11);

    int c = start_session(12);
    CHECK(tracker_acquire_stream(c, "camera1/other") == 0);
    CHECK_STR(snapshot(c)->streamid, "camera1/other");
    tracker_release(c, 12);
}

static void test_a_session_that_never_claimed_releases_cleanly(void) {
    reset();
    int a = start_session(10);
    int b = start_session(11);
    CHECK(tracker_acquire_stream(b, "camera1/main") == 0);
    tracker_release(a, 10); /* a holds nothing, and must not touch b's stream */
    CHECK_STR(snapshot(b)->streamid, "camera1/main");
    CHECK(closed_times(10) == 1);
    tracker_release(b, 11);
}

/* ---- the sampler's read ---- */

typedef struct read_result {
    int calls;
    int input_tracked;
    int output_tracked;
} read_result_t;

static void record_read(int input_tracked, int output_tracked, void *ctx) {
    read_result_t *r = ctx;
    ++r->calls;
    r->input_tracked = input_tracked;
    r->output_tracked = output_tracked;
}

static read_result_t read_sockets(int slot, SRTSOCKET in, SRTSOCKET out) {
    read_result_t r = {0, -1, -1};
    tracker_read_sockets(slot, in, out, record_read, &r);
    return r;
}

static void test_a_read_says_which_sockets_are_still_tracked(void) {
    reset();
    int a = start_session(10);
    tracker_set_output_socket(a, 20);

    read_result_t r = read_sockets(a, 10, 20);
    CHECK(r.calls == 1 && r.input_tracked == 1 && r.output_tracked == 1);
    r = read_sockets(a, 10, SRT_INVALID_SOCK); /* "no output socket" is never tracked */
    CHECK(r.input_tracked == 1 && r.output_tracked == 0);
    r = read_sockets(a, 11, 21); /* sockets that are not the slot's */
    CHECK(r.input_tracked == 0 && r.output_tracked == 0);
    r = read_sockets(a + 1, 10, 20); /* another slot's */
    CHECK(r.input_tracked == 0 && r.output_tracked == 0);
    r = read_sockets(-1, 10, 20);
    CHECK(r.calls == 1 && r.input_tracked == 0 && r.output_tracked == 0);

    /* once a taker has the socket it is no longer reported, so it is never read */
    CHECK(tracker_take_output(a, 20) == 20);
    r = read_sockets(a, 10, 20);
    CHECK(r.input_tracked == 1 && r.output_tracked == 0);
    CHECK(tracker_take_input(a, 10) == 10);
    r = read_sockets(a, 10, 20);
    CHECK(r.input_tracked == 0 && r.output_tracked == 0);
    tracker_release(a, 10);
    r = read_sockets(a, 10, 20); /* a released slot tracks nothing */
    CHECK(r.input_tracked == 0 && r.output_tracked == 0);
}

typedef struct taker {
    int slot;
    volatile int done;
    SRTSOCKET got;
} taker_t;

static void *take_input_thread(void *arg) {
    taker_t *t = arg;
    t->got = tracker_take_input(t->slot, 10);
    t->done = 1;
    return NULL;
}

typedef struct hold {
    taker_t taker;
    int done_while_held;
} hold_t;

static void hold_while_a_taker_waits(int input_tracked, int output_tracked, void *ctx) {
    (void)input_tracked;
    (void)output_tracked;
    hold_t *h = ctx;
    pthread_t tid;
    pthread_create(&tid, NULL, take_input_thread, &h->taker);
    usleep(100 * 1000); /* ample time for the taker to run, if it could */
    h->done_while_held = h->taker.done;
    pthread_detach(tid);
}

/* What the sampler relies on: while it reads, no other thread can take (and so close) a
 * socket. The taker only gets it after the read returns. */
static void test_no_socket_can_be_taken_during_a_read(void) {
    reset();
    int a = start_session(10);
    hold_t h;
    memset(&h, 0, sizeof h);
    h.taker.slot = a;
    tracker_read_sockets(a, 10, SRT_INVALID_SOCK, hold_while_a_taker_waits, &h);
    CHECK(h.done_while_held == 0);
    for (int i = 0; i < 200 && !h.taker.done; ++i)
        usleep(10 * 1000);
    CHECK(h.taker.done == 1);
    CHECK(h.taker.got == 10);
    tracker_release(a, 10);
    CHECK(closed_times(10) == 0); /* the taker has the input; it would close it */
}

/* ---- sockets are closed exactly once ---- */

static void test_each_socket_is_closed_by_exactly_one_taker(void) {
    reset();
    int a = start_session(10);
    tracker_set_output_socket(a, 20);
    CHECK(tracker_is_current(a, 10, 20));
    CHECK(tracker_take_output(a, 21) == SRT_INVALID_SOCK); /* not the tracked socket */
    CHECK(tracker_take_output(a, 20) == 20);
    CHECK(tracker_take_output(a, 20) == SRT_INVALID_SOCK); /* already taken */
    CHECK(!tracker_is_current(a, SRT_INVALID_SOCK, 20));
    CHECK(tracker_take_input(a, 10) == 10);
    tracker_release(a, 10); /* the input was taken, so the release does not close it */
    CHECK(closed_times(10) == 0);

    int b = start_session(11);
    tracker_release(b, 11);
    CHECK(closed_times(11) == 1); /* an untaken input is closed by its release */
}

static void test_shutdown_closes_every_socket_once(void) {
    reset();
    int a = start_session(10);
    int b = start_session(11);
    tracker_set_output_socket(b, 21);
    tracker_close_all();
    CHECK(closed_times(10) == 1 && closed_times(11) == 1 && closed_times(21) == 1);
    tracker_release(a, 10); /* each session's own release finds its sockets taken */
    tracker_release(b, 11);
    CHECK(closed_times(10) == 1 && closed_times(11) == 1 && closed_times(21) == 1);
    tracker_wait_all(); /* every session released, so this returns */
}

int main(void) {
    g_snap = calloc(MAX_ACTIVE_SESSIONS, sizeof *g_snap);
    test_the_table_is_bounded();
    test_a_free_streamid_is_claimed();
    test_an_empty_streamid_has_no_stream();
    test_a_duplicate_of_a_live_stream_waits_then_is_rejected();
    test_a_stale_holder_is_taken_over();
    test_a_stale_holder_that_never_ends_is_closed_once_and_the_newcomer_rejected();
    test_a_takeover_closes_only_the_holder();
    test_shutdown_ends_the_wait();
    test_release_frees_the_stream_and_the_slot_together();
    test_a_session_that_never_claimed_releases_cleanly();
    test_a_read_says_which_sockets_are_still_tracked();
    test_no_socket_can_be_taken_during_a_read();
    test_each_socket_is_closed_by_exactly_one_taker();
    test_shutdown_closes_every_socket_once();
    free(g_snap);
    TEST_SUMMARY("test_tracker");
}
