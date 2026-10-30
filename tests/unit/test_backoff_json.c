/* Retry backoff and retry state (src/backoff.c) and the status API's JSON writers (src/json_out.c).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* open_memstream */
#endif
#include "../../src/backoff.h"
#include "../../src/json_out.h"
#include "../../src/ratelimit.h"
#include "test.h"

#include <stdlib.h>

static void test_backoff_sequence(void) {
    /* 1, 2, 4, 8, then 16 s and staying there: see docs/3-operations/1-reliability.md */
    static const int expect[] = {1000, 2000, 4000, 8000, 16000, 16000, 16000};
    for (int failures = 1; failures <= 7; ++failures) {
        CHECK(retry_delay_ms(failures) == expect[failures - 1]);
    }
}

static void test_backoff_edges(void) {
    CHECK(retry_delay_ms(0) == 1000);
    CHECK(retry_delay_ms(-1) == 1000);
    CHECK(retry_delay_ms(-2147483647 - 1) == 1000);
    CHECK(retry_delay_ms(100) == 16000);
    CHECK(retry_delay_ms(2147483647) == 16000);
}

static void test_backoff_never_decreases_and_is_bounded(void) {
    int prev = 0;
    for (int failures = 0; failures < 1000; ++failures) {
        int d = retry_delay_ms(failures);
        CHECK(d >= prev);
        CHECK(d >= 1000 && d <= 16000);
        prev = d;
    }
}

static void test_retry_escalates_when_connects_succeed_and_sends_fail(void) {
    /* The downstream accepts the handshake and drops the connection 50 ms later: every
     * cycle is a successful connect followed by a failed send. */
    static const int expect[] = {1000, 2000, 4000, 8000, 16000, 16000};
    retry_state_t r;
    retry_init(&r);
    long long now = 5000;
    for (int i = 0; i < 6; ++i) {
        retry_connected(&r, now);
        CHECK(!retry_check_healthy(&r, now + 10));
        now += 50;
        CHECK(retry_failed(&r, now) == expect[i]);
        CHECK(r.failures == i + 1);
        CHECK(!retry_due(&r, now + expect[i] - 1));
        CHECK(retry_due(&r, now + expect[i]));
        now += expect[i];
    }
}

static void test_retry_clears_after_the_healthy_period(void) {
    retry_state_t r;
    retry_init(&r);
    retry_failed(&r, 0);
    retry_failed(&r, 1000);
    CHECK(r.failures == 2);

    retry_connected(&r, 4000);
    CHECK(!retry_check_healthy(&r, 4000 + OUTPUT_HEALTHY_MS - 1));
    CHECK(r.failures == 2);
    CHECK(retry_check_healthy(&r, 4000 + OUTPUT_HEALTHY_MS));
    CHECK(r.failures == 0);
    /* Once per connection, and a healthy connection that fails starts over. */
    CHECK(!retry_check_healthy(&r, 4000 + 2 * OUTPUT_HEALTHY_MS));
    CHECK(retry_failed(&r, 40000) == 1000);
    CHECK(r.failures == 1);
}

static void test_retry_failure_after_the_healthy_period_starts_over(void) {
    /* The failure arrives before anything called retry_check_healthy, for example
     * during a receive that blocked past the healthy time. */
    retry_state_t r;
    retry_init(&r);
    retry_failed(&r, 0);
    retry_failed(&r, 1000);
    retry_failed(&r, 3000);
    retry_connected(&r, 10000);
    CHECK(retry_failed(&r, 10000 + OUTPUT_HEALTHY_MS - 1) == 8000);
    CHECK(r.failures == 4);

    retry_connected(&r, 100000);
    CHECK(retry_failed(&r, 100000 + OUTPUT_HEALTHY_MS) == 1000);
    CHECK(r.failures == 1);
}

static void test_retry_connect_failures_without_a_connection(void) {
    retry_state_t r;
    retry_init(&r);
    CHECK(retry_due(&r, 0));
    CHECK(!retry_check_healthy(&r, 1000000));
    CHECK(retry_failed(&r, 100) == 1000);
    CHECK(retry_failed(&r, 1100) == 2000);
    CHECK(r.next_attempt_at_ms == 3100);
    CHECK(r.up_at_ms == 0);
}

/* Runs fn against an in-memory FILE and returns the text it wrote. */
typedef void (*writer_fn)(FILE *);
static char *capture(writer_fn fn) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    fn(f);
    fclose(f);
    return buf;
}

static char *escaped(const char *s) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    json_write_escaped(f, s);
    fclose(f);
    return buf;
}

#define ESC(in, expected)                                                                          \
    do {                                                                                           \
        char *r_ = escaped(in);                                                                    \
        CHECK_STR(r_, expected);                                                                   \
        free(r_);                                                                                  \
    } while (0)

static void test_escaping(void) {
    ESC("plain", "plain");
    ESC("", "");
    ESC("a\"b", "a\\\"b");
    ESC("a\\b", "a\\\\b");
    ESC("a/b", "a/b");
    ESC("line1\nline2", "line1\\nline2");
    ESC("a\rb\tc", "a\\rb\\tc");
    ESC("\x01\x1f", "\\u0001\\u001f");
    ESC("\x7f", "\x7f");
    ESC("caf\xc3\xa9 \xe2\x9c\x93", "caf\xc3\xa9 \xe2\x9c\x93"); /* UTF-8 passes through */
    ESC("\"\\\"\\", "\\\"\\\\\\\"\\\\");
    /* a streamid that tries to break out of the JSON string */
    ESC("x\",\"pid\":1,\"y\":\"", "x\\\",\\\"pid\\\":1,\\\"y\\\":\\\"");
}

static void test_escaping_long_string(void) {
    char *long_s = malloc(100001);
    memset(long_s, '"', 100000);
    long_s[100000] = '\0';
    char *r = escaped(long_s);
    CHECK(strlen(r) == 200000);
    free(r);
    free(long_s);
}

static void w_object(FILE *f) {
    int first = 1;
    fputc('{', f);
    json_bool(f, &first, "b", 1);
    json_bool(f, &first, "c", 0);
    json_str(f, &first, "s", "a\"b");
    json_str(f, &first, "n", NULL);
    json_ll(f, &first, "big", 9007199254740993LL);
    json_int(f, &first, "neg", -5);
    fputc('}', f);
}

static void test_object_commas_and_types(void) {
    char *r = capture(w_object);
    CHECK_STR(r, "{\"b\":true,\"c\":false,\"s\":\"a\\\"b\",\"n\":null,"
                 "\"big\":9007199254740993,\"neg\":-5}");
    free(r);
}

static void w_opts(FILE *f) {
    int first = 1;
    fputc('{', f);
    json_ll_opt(f, &first, "a", 1, 12345678901LL);
    json_ll_opt(f, &first, "b", 0, 12345678901LL);
    json_int_opt(f, &first, "c", 1, 7);
    json_int_opt(f, &first, "d", 0, 7);
    json_dbl_opt(f, &first, "e", 1, 1.23456);
    json_dbl_opt(f, &first, "f", 0, 1.23456);
    json_dbl_opt(f, &first, "g", 1, 0.0);
    json_dbl_opt(f, &first, "h", 1, -0.0004);
    fputc('}', f);
}

static void test_optional_values(void) {
    char *r = capture(w_opts);
    CHECK_STR(r, "{\"a\":12345678901,\"b\":null,\"c\":7,\"d\":null,\"e\":1.235,\"f\":null,"
                 "\"g\":0.000,\"h\":-0.000}");
    free(r);
}

static void test_rate_window_limits_and_counts(void) {
    rate_window_t w = {0};
    /* 3 per 1000 ms: the first three pass, the rest are counted */
    CHECK(rate_window_allow(&w, 1000, 3, 1000) == 1);
    CHECK(rate_window_allow(&w, 1100, 3, 1000) == 1);
    CHECK(rate_window_allow(&w, 1200, 3, 1000) == 1);
    CHECK(rate_window_allow(&w, 1300, 3, 1000) == 0);
    CHECK(rate_window_allow(&w, 1900, 3, 1000) == 0);
    CHECK(w.suppressed == 2);
    /* still inside the window: no summary yet */
    CHECK(rate_window_take_suppressed(&w, 1999, 1000) == 0);
    CHECK(w.suppressed == 2);
    /* the window has ended: the summary is reported once, then cleared */
    CHECK(rate_window_take_suppressed(&w, 2000, 1000) == 2);
    CHECK(rate_window_take_suppressed(&w, 2001, 1000) == 0);
    /* and a new window lets events through again */
    CHECK(rate_window_allow(&w, 2500, 3, 1000) == 1);
}

static void test_rate_window_summary_without_a_later_event(void) {
    rate_window_t w = {0};
    for (int i = 0; i < 10; ++i)
        rate_window_allow(&w, 5000 + i, 2, 60000);
    CHECK(w.suppressed == 8);
    /* a burst that then goes quiet is still summarised once its window ends */
    CHECK(rate_window_take_suppressed(&w, 5000 + 59999, 60000) == 0);
    CHECK(rate_window_take_suppressed(&w, 5000 + 60000, 60000) == 8);
}

/* libsrt's log cap (log.c) asks the window, on every message, for the summary of a
 * window that has ended and then whether this line may go out. At the 2026-09-29
 * rate (about 35 lines a second) that must write 20 lines a second, drop the rest,
 * and say how many were dropped once the next second begins. */
static void test_rate_window_as_the_libsrt_log_cap_uses_it(void) {
    rate_window_t w = {0};
    long long summaries = 0;
    int written = 0;
    for (int i = 0; i < 35; ++i) { /* 35 messages in one second, 28 ms apart */
        long long now = 10000 + i * 28;
        long long suppressed = rate_window_take_suppressed(&w, now, 1000);
        CHECK(suppressed == 0);
        written += rate_window_allow(&w, now, 20, 1000);
    }
    CHECK(written == 20);
    /* the next second's first message reports the 15 that were dropped, and is written */
    long long now = 11000;
    summaries = rate_window_take_suppressed(&w, now, 1000);
    CHECK(summaries == 15);
    CHECK(rate_window_allow(&w, now, 20, 1000) == 1);
    CHECK(rate_window_take_suppressed(&w, now + 1, 1000) == 0);
    /* a quiet spell and then one message: nothing to report, and it is written */
    CHECK(rate_window_take_suppressed(&w, 30000, 1000) == 0);
    CHECK(rate_window_allow(&w, 30000, 20, 1000) == 1);
}

static void test_rate_window_edges(void) {
    rate_window_t w = {0};
    CHECK(rate_window_allow(&w, 100, 0, 1000) == 0); /* a limit of 0 blocks everything */
    CHECK(w.suppressed == 1);
    rate_window_t z = {0};
    CHECK(rate_window_allow(&z, 0, 1, 1000) == 1); /* a clock reading of 0 works */
    CHECK(rate_window_allow(&z, 10, 1, 1000) == 0);
    rate_window_t c = {0};
    CHECK(rate_window_allow(&c, 9000, 1, 1000) == 1);
    CHECK(rate_window_allow(&c, 8000, 1, 1000) == 1); /* the clock stepped back: new window */
}

int main(void) {
    test_backoff_sequence();
    test_backoff_edges();
    test_backoff_never_decreases_and_is_bounded();
    test_retry_escalates_when_connects_succeed_and_sends_fail();
    test_retry_clears_after_the_healthy_period();
    test_retry_failure_after_the_healthy_period_starts_over();
    test_retry_connect_failures_without_a_connection();
    test_escaping();
    test_escaping_long_string();
    test_object_commas_and_types();
    test_optional_values();
    test_rate_window_limits_and_counts();
    test_rate_window_summary_without_a_later_event();
    test_rate_window_as_the_libsrt_log_cap_uses_it();
    test_rate_window_edges();
    TEST_SUMMARY("backoff_json");
}
