/* The stream state table (src/stream_state.c): claiming a streamid for a session's
 * slot, reusing a slot, what the setters do for a session that holds no stream, and the
 * log lines written when a reading changes the legs. libsrt is not involved: the log and
 * the clock are stubbed here. */

#include "../../src/stream_state.h"
#include "../../src/telemetry.h"
#include "test.h"

#include <stdarg.h>
#include <stdlib.h>

/* ---- stubs for what stream_state.c takes from log.c and runtime.c ---- */

static long long g_now = 1000;
long long now_ms(void) {
    return g_now;
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
    for (int i = 0; i < g_log_count; ++i) {
        if (strstr(g_log[i], text)) return 1;
    }
    return 0;
}

static relay_stream_state_t *g_snap;
static char g_snap_error[LAST_ERROR_MAX];

static const relay_stream_state_t *snapshot(int slot) {
    stream_state_snapshot(g_snap, g_snap_error, sizeof g_snap_error);
    return &g_snap[slot];
}

/* Empties the table: every slot that holds a stream is removed. */
static void reset(void) {
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i)
        remove_stream_state(i);
    set_last_errorf("%s", "");
    g_now = 1000;
    g_log_count = 0;
}

static void test_claiming_a_streamid(void) {
    reset();
    CHECK(claim_stream_state("camera1/main", 4) == CLAIM_STREAM_OK);
    const relay_stream_state_t *st = snapshot(4);
    CHECK_STR(st->streamid, "camera1/main");
    CHECK(st->input_active && !st->output_connected);
    CHECK(st->last_input_packet_at_ms == 1000);
    CHECK(snapshot(3)->streamid[0] == '\0');
}

static void test_an_empty_streamid_has_no_stream(void) {
    reset();
    CHECK(claim_stream_state("", 0) == CLAIM_STREAM_NO_ID);
    CHECK(claim_stream_state(NULL, 0) == CLAIM_STREAM_NO_ID);
    CHECK(snapshot(0)->streamid[0] == '\0');
}

static void test_a_streamid_held_by_another_session_is_in_use(void) {
    reset();
    CHECK(claim_stream_state("camera1/main", 0) == CLAIM_STREAM_OK);
    CHECK(claim_stream_state("camera1/main", 1) == CLAIM_STREAM_IN_USE);
    CHECK(snapshot(1)->streamid[0] == '\0'); /* the loser holds nothing */
    CHECK(claim_stream_state("camera1/backup", 1) == CLAIM_STREAM_OK);
    CHECK_STR(snapshot(0)->streamid, "camera1/main");
    CHECK_STR(snapshot(1)->streamid, "camera1/backup");
}

/* A slot still holding an earlier stream means the earlier session did not remove it
 * before the slot was handed on: that is refused, not overwritten. */
static void test_a_slot_that_still_holds_a_stream_is_busy(void) {
    reset();
    CHECK(claim_stream_state("first", 2) == CLAIM_STREAM_OK);
    CHECK(claim_stream_state("second", 2) == CLAIM_STREAM_SLOT_BUSY);
    CHECK_STR(snapshot(2)->streamid, "first");
    CHECK(claim_stream_state("second", -1) == CLAIM_STREAM_SLOT_BUSY);
    CHECK(claim_stream_state("second", MAX_ACTIVE_SESSIONS) == CLAIM_STREAM_SLOT_BUSY);
}

static void test_a_removed_slot_is_reusable_and_starts_clean(void) {
    reset();
    CHECK(claim_stream_state("first", 7) == CLAIM_STREAM_OK);
    set_stream_retry_failures(7, 5);
    set_stream_errorf(7, "boom");
    record_stream_forward_progress(7, 1316);
    set_stream_output_connected(7, 1);
    remove_stream_state(7);
    CHECK(snapshot(7)->streamid[0] == '\0');

    CHECK(claim_stream_state("second", 7) == CLAIM_STREAM_OK);
    const relay_stream_state_t *st = snapshot(7);
    CHECK_STR(st->streamid, "second");
    CHECK(st->retry_failures == 0 && st->forwarded_bytes == 0 && st->forwarded_packets == 0);
    CHECK(!st->output_connected && st->last_error[0] == '\0');
    /* the old streamid is free again for anyone */
    CHECK(claim_stream_state("first", 8) == CLAIM_STREAM_OK);
}

static void test_the_table_holds_every_slot(void) {
    reset();
    char id[32];
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        snprintf(id, sizeof id, "stream/%d", i);
        CHECK(claim_stream_state(id, i) == CLAIM_STREAM_OK);
    }
    CHECK(claim_stream_state("one/too/many", 0) == CLAIM_STREAM_SLOT_BUSY);
    CHECK_STR(snapshot(MAX_ACTIVE_SESSIONS - 1)->streamid, "stream/255");
}

/* A session without a streamid, or one that lost the claim, calls the setters freely. */
static void test_setters_ignore_a_slot_that_holds_no_stream(void) {
    reset();
    CHECK(claim_stream_state("held", 1) == CLAIM_STREAM_OK);
    const int slots[] = {-1, 0, 2, MAX_ACTIVE_SESSIONS, 100000};
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; ++i) {
        int slot = slots[i];
        set_stream_errorf(slot, "x");
        set_stream_input_info(slot, 0, "1.2.3.4", 5);
        set_stream_output_connected(slot, 1);
        set_stream_retry_failures(slot, 9);
        record_stream_input_progress(slot);
        record_stream_forward_progress(slot, 100);
    }
    /* nothing was written into the unclaimed slots */
    for (int slot = 0; slot < 3; ++slot) {
        if (slot == 1) continue;
        const relay_stream_state_t *st = snapshot(slot);
        CHECK(st->last_error[0] == '\0' && st->input_peer_ip[0] == '\0');
        CHECK(!st->output_connected && st->retry_failures == 0);
        CHECK(st->forwarded_bytes == 0 && st->last_input_packet_at_ms == 0);
    }
    /* and the claimed one was left alone */
    const relay_stream_state_t *held = snapshot(1);
    CHECK(held->last_error[0] == '\0' && held->retry_failures == 0 && held->forwarded_bytes == 0);
}

static void test_setters_write_the_claimed_slot(void) {
    reset();
    CHECK(claim_stream_state("cam", 3) == CLAIM_STREAM_OK);
    g_now = 5000;
    set_stream_errorf(3, "Relay output error to %s:%d", "10.0.0.1", 10080);
    set_stream_output_connected(3, 1);
    set_stream_retry_failures(3, 2);
    record_stream_input_progress(3);
    record_stream_forward_progress(3, 1316);
    record_stream_forward_progress(3, 0); /* nothing sent is not progress */
    record_stream_forward_progress(3, -4);
    set_stream_input_info(3, 0, "10.0.1.20", 40000);
    const relay_stream_state_t *st = snapshot(3);
    CHECK_STR(st->last_error, "Relay output error to 10.0.0.1:10080");
    CHECK(st->last_error_at_ms == 5000);
    CHECK(st->output_connected && st->retry_failures == 2);
    CHECK(st->last_input_packet_at_ms == 5000 && st->last_packet_at_ms == 5000);
    CHECK(st->forwarded_packets == 1 && st->forwarded_bytes == 1316);
    CHECK(!st->input_bonded && st->input_peer_port == 40000);
    CHECK_STR(st->input_peer_ip, "10.0.1.20");
}

/* A bonded group has no single peer: its legs carry the addresses. */
static void test_a_bonded_input_stores_no_peer(void) {
    reset();
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK);
    set_stream_input_info(0, 1, "10.0.1.20", 40000);
    const relay_stream_state_t *st = snapshot(0);
    CHECK(st->input_bonded);
    CHECK(st->input_peer_ip[0] == '\0' && st->input_peer_port == 0);
}

static void test_input_age_for_the_takeover_decision(void) {
    reset();
    long long age = -1;
    CHECK(stream_input_age_ms("nobody", 9000, &age) == -1);
    CHECK(stream_input_age_ms("", 9000, &age) == -1);
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK); /* claimed at 1000 */
    CHECK(stream_input_age_ms("cam", 4000, &age) == 0 && age == 3000);
    g_now = 3500;
    record_stream_input_progress(0);
    CHECK(stream_input_age_ms("cam", 4000, &age) == 0 && age == 500);
    remove_stream_state(0);
    CHECK(stream_input_age_ms("cam", 4000, &age) == -1);
}

static void test_the_process_error_is_kept_apart_from_the_streams(void) {
    reset();
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK);
    set_last_errorf("status bind :%d failed", 8081);
    snapshot(0);
    CHECK_STR(g_snap_error, "status bind :8081 failed");
    CHECK(g_snap[0].last_error[0] == '\0');
}

static void test_a_snapshot_is_a_copy(void) {
    reset();
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK);
    relay_stream_state_t *copy = malloc(MAX_ACTIVE_SESSIONS * sizeof *copy);
    char err[LAST_ERROR_MAX];
    stream_state_snapshot(copy, err, sizeof err);
    record_stream_forward_progress(0, 100);
    set_last_errorf("later");
    CHECK(copy[0].forwarded_bytes == 0);
    CHECK_STR(err, "");
    free(copy);
}

/* -1 means "this slot holds no stream", so the sampler can skip a session without one. */
static void test_the_stats_clock(void) {
    reset();
    CHECK(stream_state_last_stats_at_ms(0) == -1);
    CHECK(stream_state_last_stats_at_ms(-1) == -1);
    CHECK(stream_state_last_stats_at_ms(MAX_ACTIVE_SESSIONS) == -1);
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK);
    CHECK(stream_state_last_stats_at_ms(0) == 0);
    stats_sample_t s;
    memset(&s, 0, sizeof s);
    s.taken_at_ms = 4321;
    stream_state_apply_stats(0, &s);
    CHECK(stream_state_last_stats_at_ms(0) == 4321);
    /* a reading for a slot without a stream is dropped */
    stream_state_apply_stats(5, &s);
    CHECK(snapshot(5)->last_stats_at_ms == 0);
}

/* The wrapper writes the leg log lines the docs describe, with the stream's id. */
static void test_leg_changes_are_logged_with_the_streamid(void) {
    reset();
    CHECK(claim_stream_state("camera1/main", 0) == CLAIM_STREAM_OK);
    stats_sample_t s;
    memset(&s, 0, sizeof s);
    s.is_group = 1;
    s.have_in = 1;
    s.can_read_in = 1;
    s.legs_read = 1;
    s.leg_count = 2;
    for (int i = 0; i < 2; ++i) {
        snprintf(s.legs[i].ip, sizeof s.legs[i].ip, "127.0.0.%d", i + 1);
        s.legs[i].port = 40001 + i;
        s.legs[i].state = LEG_MEMBER_RUNNING;
    }
    s.taken_at_ms = 1000;
    stream_state_apply_stats(0, &s);
    CHECK(logged("Bonded leg joined streamid=camera1/main leg=127.0.0.1:40001 legs=2"));
    CHECK(logged("Bonded leg joined streamid=camera1/main leg=127.0.0.2:40002 legs=2"));

    s.leg_count = 1;
    s.taken_at_ms = 2000;
    stream_state_apply_stats(0, &s);
    CHECK(logged("Bonded leg left streamid=camera1/main leg=127.0.0.2:40002 legs=1"));
    CHECK(snapshot(0)->leg_count == 1);

    /* a reading for a slot with no stream logs nothing and changes nothing */
    int before = g_log_count;
    stream_state_apply_stats(6, &s);
    CHECK(g_log_count == before);
}

/* A flapping leg: at most LEG_LOG_MAX_PER_WINDOW lines, then one summary. */
static void test_a_flapping_leg_logs_a_bounded_number_of_lines(void) {
    reset();
    CHECK(claim_stream_state("cam", 0) == CLAIM_STREAM_OK);
    stats_sample_t up, down;
    memset(&up, 0, sizeof up);
    up.is_group = 1;
    up.have_in = 1;
    up.can_read_in = 1;
    up.legs_read = 1;
    up.leg_count = 1;
    snprintf(up.legs[0].ip, sizeof up.legs[0].ip, "127.0.0.1");
    up.legs[0].port = 40001;
    down = up;
    down.leg_count = 0;
    for (int i = 0; i < 18; ++i) {
        stats_sample_t *s = i % 2 == 0 ? &up : &down;
        s->taken_at_ms = 1000 + i * 100;
        stream_state_apply_stats(0, s);
    }
    int lines = 0;
    for (int i = 0; i < g_log_count; ++i)
        if (strstr(g_log[i], "Bonded leg ")) lines++;
    CHECK(lines == LEG_LOG_MAX_PER_WINDOW);
    CHECK(!logged("suppressed"));

    down.taken_at_ms = 1000 + LEG_LOG_WINDOW_MS + 5000;
    stream_state_apply_stats(0, &down);
    CHECK(logged("Bonded leg events suppressed streamid=cam count=6 in the last 30 s"));
}

int main(void) {
    g_snap = calloc(MAX_ACTIVE_SESSIONS, sizeof *g_snap);
    if (!g_snap) return 2;
    test_claiming_a_streamid();
    test_an_empty_streamid_has_no_stream();
    test_a_streamid_held_by_another_session_is_in_use();
    test_a_slot_that_still_holds_a_stream_is_busy();
    test_a_removed_slot_is_reusable_and_starts_clean();
    test_the_table_holds_every_slot();
    test_setters_ignore_a_slot_that_holds_no_stream();
    test_setters_write_the_claimed_slot();
    test_a_bonded_input_stores_no_peer();
    test_input_age_for_the_takeover_decision();
    test_the_process_error_is_kept_apart_from_the_streams();
    test_a_snapshot_is_a_copy();
    test_the_stats_clock();
    test_leg_changes_are_logged_with_the_streamid();
    test_a_flapping_leg_logs_a_bounded_number_of_lines();
    free(g_snap);
    TEST_SUMMARY("stream_state");
}
