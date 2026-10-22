/* Retry backoff (src/backoff.c) and the status API's JSON writers (src/json_out.c). */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* open_memstream */
#endif
#include "../../src/backoff.h"
#include "../../src/json_out.h"
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

int main(void) {
    test_backoff_sequence();
    test_backoff_edges();
    test_backoff_never_decreases_and_is_bounded();
    test_escaping();
    test_escaping_long_string();
    test_object_commas_and_types();
    test_optional_values();
    TEST_SUMMARY("backoff_json");
}
