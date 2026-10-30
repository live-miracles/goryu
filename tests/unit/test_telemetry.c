/* The rules that turn a libsrt reading into the stream's reported state
 * (src/telemetry.c): when a value is reportable, that totals never go backwards, and
 * when a bonded leg has joined or left. No libsrt is involved: a reading is a struct. */

#include "../../src/telemetry.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>

static relay_stream_state_t g_state;
static telemetry_result_t g_result;

static void reset(void) {
    memset(&g_state, 0, sizeof g_state);
    memset(&g_result, 0, sizeof g_result);
}

static void apply(const stats_sample_t *sample) {
    memset(&g_result, 0, sizeof g_result);
    telemetry_apply(&g_state, sample, &g_result);
}

/* A healthy plain (non-bonded) reading. */
static stats_sample_t plain_sample(long long now) {
    stats_sample_t s;
    memset(&s, 0, sizeof s);
    s.taken_at_ms = now;
    s.can_read_in = 1;
    s.have_in = 1;
    s.in.tsbpd_delay_ms = 240;
    s.in.rtt_ms = 1.5;
    s.in.recv_total = 1000;
    s.in.recv_unique_total = 990;
    s.in.recv_loss_total = 10;
    s.in.recv_drop_total = 2;
    s.in.recv_retrans_total = 8;
    s.in.bandwidth_mbps = 20.0;
    s.in.recv_rate_mbps = 4.0;
    s.in.belated_total = 3;
    s.in.belated_avg_ms = 12.5;
    s.in.undecrypt_total = 1;
    s.in.reorder_distance = 4;
    s.in.rcv_buf_ms = 90;
    return s;
}

static void add_leg(stats_sample_t *s, const char *ip, int port, int latency_ms) {
    leg_reading_t *leg = &s->legs[s->leg_count++];
    snprintf(leg->ip, sizeof leg->ip, "%s", ip);
    leg->port = port;
    leg->state = LEG_MEMBER_RUNNING;
    leg->have_counters = 1;
    leg->counters.tsbpd_delay_ms = latency_ms;
    leg->counters.rtt_ms = 2.0;
    leg->counters.recv_total = 500;
    leg->counters.belated_avg_ms = 1.0;
}

/* A healthy bonded reading with two legs. */
static stats_sample_t group_sample(long long now) {
    stats_sample_t s;
    memset(&s, 0, sizeof s);
    s.taken_at_ms = now;
    s.can_read_in = 1;
    s.is_group = 1;
    s.have_in = 1;
    s.in.rtt_ms = 0.0;
    s.in.recv_total = 0;
    s.in.recv_unique_total = 400;
    s.in.recv_drop_total = 1;
    s.legs_read = 1;
    add_leg(&s, "127.0.0.1", 40001, 240);
    add_leg(&s, "127.0.0.2", 40002, 300);
    return s;
}

static void test_a_plain_input_reports_everything(void) {
    reset();
    stats_sample_t s = plain_sample(1000);
    apply(&s);
    CHECK(g_state.input_extra_stats_valid);
    CHECK(g_state.recv_loss_total_valid && g_state.retrans_total_valid);
    CHECK(g_state.recv_packets_total_valid);
    CHECK(g_state.input_rtt_valid && g_state.input_rtt_ms == 1.5);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 240);
    CHECK(g_state.input_bandwidth_mbps == 20.0 && g_state.input_recv_rate_mbps == 4.0);
    CHECK(g_state.input_belated_total == 3 && g_state.input_belated_avg_valid);
    CHECK(g_state.input_belated_avg_ms == 12.5);
    CHECK(g_state.input_undecrypt_total == 1 && g_state.input_reorder_distance == 4);
    CHECK(g_state.input_rcv_buf_ms == 90);
    CHECK(g_state.recv_packets_total == 1000 && g_state.recv_unique_packets_total == 990);
    CHECK(g_state.recv_loss_total == 10 && g_state.recv_drop_total == 2);
    CHECK(g_state.retrans_total == 8);
    CHECK(g_state.last_stats_at_ms == 1000);
    CHECK(g_state.leg_count == 0);
}

/* CUDTGroup::bstatsSocket() never fills these for a group, so they are left out
 * rather than reported as a misleading 0. */
static void test_a_bonded_input_leaves_the_socket_only_counters_out(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(!g_state.input_extra_stats_valid);
    CHECK(!g_state.input_belated_avg_valid);
    CHECK(!g_state.recv_loss_total_valid);
    CHECK(!g_state.retrans_total_valid);
    CHECK(!g_state.input_rtt_valid); /* the group's own rtt is 0 */
    /* unique packets and group drops are what a group does report */
    CHECK(g_state.recv_unique_packets_total == 400 && g_state.recv_drop_total == 1);
    CHECK(g_state.leg_count == 2);
    CHECK_STR(g_state.legs[0].ip, "127.0.0.1");
    CHECK(g_state.legs[1].port == 40002);
}

/* A group's total can read 0 while its unique count does not: that 0 is not a count. */
static void test_a_group_total_of_zero_with_unique_packets_is_not_reported(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(!g_state.recv_packets_total_valid);
    s.in.recv_total = 450;
    apply(&s);
    CHECK(g_state.recv_packets_total_valid && g_state.recv_packets_total == 450);
    s.in.recv_total = 0;
    s.in.recv_unique_total = 0;
    apply(&s);
    CHECK(g_state.recv_packets_total_valid); /* nothing received yet is a real 0 */
    /* a plain input never has this ambiguity */
    stats_sample_t p = plain_sample(2000);
    p.in.recv_total = 0;
    reset();
    apply(&p);
    CHECK(g_state.recv_packets_total_valid);
}

/* The group's latency is whichever its legs negotiated: the largest one reported. */
static void test_a_groups_latency_comes_from_its_legs(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 300);

    /* a leg that reported no latency does not count; none at all means no latency */
    s.legs[0].counters.tsbpd_delay_ms = 0;
    s.legs[1].counters.tsbpd_delay_ms = 0;
    apply(&s);
    CHECK(!g_state.input_latency_valid);

    stats_sample_t none = group_sample(2000);
    none.leg_count = 0;
    none.legs_read = 0;
    reset();
    apply(&none);
    CHECK(!g_state.input_latency_valid);

    /* a leg whose own statistics could not be read has no latency to offer */
    stats_sample_t unread = group_sample(3000);
    unread.legs[1].have_counters = 0;
    reset();
    apply(&unread);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 240);
}

/* When the input socket is no longer the tracked one it is not read, and what was
 * known about its latency is left alone. */
static void test_an_input_that_was_not_read_keeps_its_latency(void) {
    reset();
    stats_sample_t s = plain_sample(1000);
    apply(&s);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 240);

    stats_sample_t unread;
    memset(&unread, 0, sizeof unread);
    unread.taken_at_ms = 2000;
    apply(&unread);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 240);
    CHECK(!g_state.input_extra_stats_valid);
    CHECK(g_state.last_stats_at_ms == 2000);
    /* the totals are not touched either */
    CHECK(g_state.recv_packets_total == 1000);
}

static void test_a_plain_inputs_latency_and_rtt_need_a_positive_value(void) {
    reset();
    stats_sample_t s = plain_sample(1000);
    s.in.tsbpd_delay_ms = 0;
    s.in.rtt_ms = 0.0;
    apply(&s);
    CHECK(!g_state.input_latency_valid && !g_state.input_rtt_valid);
    s.in.tsbpd_delay_ms = 120;
    s.in.rtt_ms = 0.25;
    apply(&s);
    CHECK(g_state.input_latency_valid && g_state.input_latency_ms == 120);
    CHECK(g_state.input_rtt_valid && g_state.input_rtt_ms == 0.25);
    /* a failed read reports nothing, whatever the numbers say */
    s.have_in = 0;
    apply(&s);
    CHECK(!g_state.input_latency_valid && !g_state.input_extra_stats_valid);
}

/* Totals only move forward, so a reading taken just after a leg dropped out cannot
 * make one go backwards. */
static void test_totals_never_go_backwards(void) {
    reset();
    stats_sample_t s = plain_sample(1000);
    apply(&s);
    s.in.recv_total = 400;
    s.in.recv_unique_total = 300;
    s.in.recv_loss_total = 1;
    s.in.recv_drop_total = 0;
    s.in.recv_retrans_total = 0;
    apply(&s);
    CHECK(g_state.recv_packets_total == 1000);
    CHECK(g_state.recv_unique_packets_total == 990);
    CHECK(g_state.recv_loss_total == 10);
    CHECK(g_state.recv_drop_total == 2);
    CHECK(g_state.retrans_total == 8);
    s.in.recv_total = 1500;
    apply(&s);
    CHECK(g_state.recv_packets_total == 1500);
}

/* libsrt can expose a garbage value for this statistic; it must not reach the API. */
static void test_a_garbage_belated_average_is_not_reported(void) {
    const double bad[] = {NAN, INFINITY, -INFINITY, -1.0, 1e16, 1000000001.0};
    reset();
    stats_sample_t s = plain_sample(1000);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        s.in.belated_avg_ms = bad[i];
        add_leg(&s, "127.0.0.1", 40001, 240);
        s.legs[0].counters.belated_avg_ms = bad[i];
        apply(&s);
        CHECK(!g_state.input_belated_avg_valid);
        CHECK(!g_state.legs[0].belated_avg_valid);
        s.leg_count = 0;
    }
    const double good[] = {0.0, 12.5, 1000000000.0};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; ++i) {
        s.in.belated_avg_ms = good[i];
        add_leg(&s, "127.0.0.1", 40001, 240);
        s.legs[0].counters.belated_avg_ms = good[i];
        apply(&s);
        CHECK(g_state.input_belated_avg_valid && g_state.legs[0].belated_avg_valid);
        s.leg_count = 0;
    }
    /* once the extras are not reported, a stale "valid" belated average goes with them */
    reset();
    s = plain_sample(1000);
    apply(&s);
    CHECK(g_state.input_belated_avg_valid);
    stats_sample_t group = group_sample(2000);
    group.in.belated_avg_ms = 12.5;
    apply(&group);
    CHECK(!g_state.input_belated_avg_valid);
}

static void test_a_leg_reports_its_own_counters_and_validity(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    s.legs[0].counters.rtt_ms = 0.0;
    s.legs[0].counters.tsbpd_delay_ms = 0;
    s.legs[0].state = LEG_MEMBER_IDLE;
    apply(&s);
    const relay_leg_state_t *a = &g_state.legs[0];
    const relay_leg_state_t *b = &g_state.legs[1];
    CHECK(a->stats_valid && !a->rtt_valid && !a->latency_valid);
    CHECK(a->member_state == LEG_MEMBER_IDLE);
    CHECK(b->stats_valid && b->rtt_valid && b->rtt_ms == 2.0);
    CHECK(b->latency_valid && b->latency_ms == 300);
    CHECK(b->recv_packets_total == 500 && b->member_state == LEG_MEMBER_RUNNING);
}

/* A leg whose own statistics could not be read is still listed, with nothing else. */
static void test_a_leg_that_could_not_be_read_is_listed_without_numbers(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    s.legs[1].have_counters = 0;
    s.legs[1].counters.recv_total = 777; /* leftovers must not leak through */
    s.legs[1].counters.rtt_ms = 9.0;
    apply(&s);
    const relay_leg_state_t *leg = &g_state.legs[1];
    CHECK_STR(leg->ip, "127.0.0.2");
    CHECK(leg->port == 40002 && leg->member_state == LEG_MEMBER_RUNNING);
    CHECK(!leg->stats_valid && !leg->rtt_valid && !leg->latency_valid);
    CHECK(!leg->belated_avg_valid);
    CHECK(leg->recv_packets_total == 0 && leg->rtt_ms == 0.0);
}

static void test_an_address_that_fills_its_buffer_is_still_terminated(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    memset(s.legs[0].ip, 'x', sizeof s.legs[0].ip); /* no NUL at all */
    apply(&s);
    CHECK(strlen(g_state.legs[0].ip) == sizeof g_state.legs[0].ip - 1);
}

static void test_the_output_is_reported_only_when_it_was_read(void) {
    reset();
    stats_sample_t s;
    memset(&s, 0, sizeof s);
    s.taken_at_ms = 1000;
    s.have_out = 1;
    s.out.tsbpd_delay_ms = 200;
    s.out.rtt_ms = 0.8;
    s.out.sent_total = 5000;
    s.out.send_loss_total = 4;
    s.out.send_drop_total = 3;
    s.out.send_retrans_total = 2;
    s.out.bandwidth_mbps = 30.0;
    s.out.send_rate_mbps = 5.0;
    s.out.undecrypt_total = 1;
    s.out.snd_buf_ms = 70;
    apply(&s);
    CHECK(g_state.output_extra_stats_valid);
    CHECK(g_state.output_latency_valid && g_state.output_latency_ms == 200);
    CHECK(g_state.output_rtt_valid && g_state.output_rtt_ms == 0.8);
    CHECK(g_state.output_sent_packets_total == 5000 && g_state.output_send_loss_total == 4);
    CHECK(g_state.output_send_drop_total == 3 && g_state.output_retrans_total == 2);
    CHECK(g_state.output_bandwidth_mbps == 30.0 && g_state.output_send_rate_mbps == 5.0);
    CHECK(g_state.output_undecrypt_total == 1 && g_state.output_snd_buf_ms == 70);

    /* the output was not read this time (it reconnected): the last values stay */
    stats_sample_t unread;
    memset(&unread, 0, sizeof unread);
    unread.taken_at_ms = 2000;
    apply(&unread);
    CHECK(g_state.output_extra_stats_valid && g_state.output_sent_packets_total == 5000);

    s.out.tsbpd_delay_ms = 0;
    s.out.rtt_ms = 0.0;
    apply(&s);
    CHECK(!g_state.output_latency_valid && !g_state.output_rtt_valid);
}

/* ---- bonded leg events, which are logged ---- */

static int has_event(int joined, const char *ip, int port) {
    for (int i = 0; i < g_result.event_count; ++i) {
        const leg_event_t *e = &g_result.events[i];
        if (e->joined == joined && e->leg.port == port && strcmp(e->leg.ip, ip) == 0) return 1;
    }
    return 0;
}

static void test_legs_joining_and_leaving_are_reported_once(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(g_result.event_count == 2);
    CHECK(has_event(1, "127.0.0.1", 40001) && has_event(1, "127.0.0.2", 40002));

    s.taken_at_ms = 2000;
    apply(&s); /* nothing changed */
    CHECK(g_result.event_count == 0);

    s.leg_count = 1; /* the second leg went away */
    s.taken_at_ms = 3000;
    apply(&s);
    CHECK(g_result.event_count == 1 && has_event(0, "127.0.0.2", 40002));

    /* a leg that reconnects from a new port is a leave and a join */
    s.legs[0].port = 40009;
    s.taken_at_ms = 4000;
    apply(&s);
    CHECK(g_result.event_count == 2);
    CHECK(has_event(0, "127.0.0.1", 40001) && has_event(1, "127.0.0.1", 40009));
}

/* A reading where the group's members could not be listed says nothing about the legs:
 * it must not look like every leg left, nor change what was last logged. */
static void test_a_failed_group_read_is_not_every_leg_leaving(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(g_result.event_count == 2);

    stats_sample_t failed = group_sample(2000);
    failed.leg_count = 0;
    failed.legs_read = 0;
    apply(&failed);
    CHECK(g_result.event_count == 0);
    CHECK(g_state.logged_leg_count == 2);

    s.taken_at_ms = 3000; /* the same two legs are back: nothing to say */
    apply(&s);
    CHECK(g_result.event_count == 0);
}

/* A leg that flaps cannot flood the log: 12 lines per window, the rest are counted. */
static void test_a_flapping_leg_is_limited_and_summarised(void) {
    reset();
    stats_sample_t up = group_sample(0);
    up.leg_count = 1;
    stats_sample_t down = up;
    down.leg_count = 0;
    int logged = 0;
    long long now = 0;
    for (int i = 0; i < 18; ++i) { /* 18 toggles, each one event, within one window */
        stats_sample_t *s = i % 2 == 0 ? &up : &down;
        s->taken_at_ms = now += 1000;
        apply(s);
        logged += g_result.event_count;
        CHECK(g_result.suppressed == 0); /* the summary comes when the window ends */
    }
    CHECK(logged == LEG_LOG_MAX_PER_WINDOW);

    /* the window ends: one summary says how many were dropped, once */
    down.taken_at_ms = LEG_LOG_WINDOW_MS + 2000;
    apply(&down);
    CHECK(g_result.suppressed == 18 - LEG_LOG_MAX_PER_WINDOW);
    down.taken_at_ms += 1000;
    apply(&down);
    CHECK(g_result.suppressed == 0);
}

/* The summary of a burst that then goes quiet must still appear: it is taken on every
 * reading, not only when another event arrives. */
static void test_the_suppressed_summary_arrives_without_another_event(void) {
    reset();
    stats_sample_t up = group_sample(0);
    up.leg_count = 1;
    stats_sample_t down = up;
    down.leg_count = 0;
    long long now = 0;
    for (int i = 0; i < 16; ++i) {
        stats_sample_t *s = i % 2 == 0 ? &up : &down;
        s->taken_at_ms = now += 100;
        apply(s);
    }
    stats_sample_t quiet = down; /* the leg stays down: no more events */
    quiet.taken_at_ms = LEG_LOG_WINDOW_MS + 5000;
    apply(&quiet);
    CHECK(g_result.event_count == 0);
    CHECK(g_result.suppressed == 16 - LEG_LOG_MAX_PER_WINDOW);
}

static void test_no_legs_clears_the_reported_legs(void) {
    reset();
    stats_sample_t s = group_sample(1000);
    apply(&s);
    CHECK(g_state.leg_count == 2);
    s.leg_count = 0;
    s.legs_read = 1;
    s.taken_at_ms = 2000;
    apply(&s);
    CHECK(g_state.leg_count == 0);
    CHECK(g_result.event_count == 2 && has_event(0, "127.0.0.1", 40001));
}

int main(void) {
    test_a_plain_input_reports_everything();
    test_a_bonded_input_leaves_the_socket_only_counters_out();
    test_a_group_total_of_zero_with_unique_packets_is_not_reported();
    test_a_groups_latency_comes_from_its_legs();
    test_an_input_that_was_not_read_keeps_its_latency();
    test_a_plain_inputs_latency_and_rtt_need_a_positive_value();
    test_totals_never_go_backwards();
    test_a_garbage_belated_average_is_not_reported();
    test_a_leg_reports_its_own_counters_and_validity();
    test_a_leg_that_could_not_be_read_is_listed_without_numbers();
    test_an_address_that_fills_its_buffer_is_still_terminated();
    test_the_output_is_reported_only_when_it_was_read();
    test_legs_joining_and_leaving_are_reported_once();
    test_a_failed_group_read_is_not_every_leg_leaving();
    test_a_flapping_leg_is_limited_and_summarised();
    test_the_suppressed_summary_arrives_without_another_event();
    test_no_legs_clears_the_reported_legs();
    TEST_SUMMARY("telemetry");
}
