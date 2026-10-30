/* Turning the command line into what the relay runs with (src/relay_options.c): the
 * config file form, the two-URI form, the status port override and every way they can
 * be refused. No libsrt and no logging: a problem is a message, so it can be checked. */

#include "../../src/relay_options.h"
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#define SIX_KEYS                                                                                   \
    "\"input_host\":\"0.0.0.0\",\"input_port\":10081,\"output_host\":\"10.0.1.5\","                \
    "\"output_port\":10080,\"status_port\":9000"

static char g_config_path[64];

/* Writes a goryu.json with the given passphrase fields and returns its path. */
static const char *write_config(const char *passphrase_part, const char *extra) {
    snprintf(g_config_path, sizeof g_config_path, "/tmp/goryu-options-XXXXXX");
    int fd = mkstemp(g_config_path);
    FILE *f = fdopen(fd, "w");
    fprintf(f, "{" SIX_KEYS ",%s%s}\n", passphrase_part, extra);
    fclose(f);
    return g_config_path;
}

static int parse(int argc, char **argv, const char *env, relay_options_t *o, char *err) {
    err[0] = '\0';
    return parse_relay_options(argc, argv, env, o, err, 512);
}

static int parse_file(const char *path, const char *env, relay_options_t *o, char *err) {
    char *argv[] = {"goryu", (char *)path};
    return parse(2, argv, env, o, err);
}

static int parse_uris(const char *in, const char *out, const char *env, relay_options_t *o,
                      char *err) {
    char *argv[] = {"goryu", (char *)in, (char *)out};
    return parse(3, argv, env, o, err);
}

static void test_a_config_file_gives_the_listener_and_the_downstream(void) {
    relay_options_t o;
    char err[512];
    const char *path = write_config("\"passphrase\":\"\"", "");
    CHECK(parse_file(path, NULL, &o, err) == 0);
    CHECK_STR(o.in_host, "0.0.0.0");
    CHECK(o.in_port == 10081);
    CHECK_STR(o.out_host, "10.0.1.5");
    CHECK(o.out_port == 10080);
    CHECK(o.status_port == 9000);
    /* the input is a listener that accepts groups, the output a caller */
    CHECK(strstr(o.in_query, "mode=listener") && strstr(o.in_query, "groupconnect=1"));
    CHECK(strstr(o.in_query, "latency=240") && strstr(o.in_query, "transtype=live"));
    CHECK(strstr(o.out_query, "mode=caller") && strstr(o.out_query, "latency=200"));
    CHECK(!strstr(o.out_query, "groupconnect"));
    CHECK(strstr(o.in_query, "passphrase") == NULL && strstr(o.out_query, "passphrase") == NULL);
    CHECK(o.unknown_key_count == 0 && !o.status_port_env_ignored);
    remove(path);
}

static void test_the_passphrases_reach_their_own_side_only(void) {
    relay_options_t o;
    char err[512];
    const char *path = write_config("\"passphrase\":\"input-pass-123\"",
                                    ",\"output_passphrase\":\"output-pass-456\"");
    CHECK(parse_file(path, NULL, &o, err) == 0);
    CHECK(strstr(o.in_query, "passphrase=input-pass-123"));
    CHECK(strstr(o.out_query, "passphrase=output-pass-456"));
    CHECK(!strstr(o.in_query, "output-pass") && !strstr(o.out_query, "input-pass"));
    /* what is logged at startup has neither */
    CHECK(strstr(o.out_uri_redacted, "srt://10.0.1.5:10080?") != NULL);
    CHECK(strstr(o.out_uri_redacted, "passphrase=<redacted>") != NULL);
    CHECK(strstr(o.out_uri_redacted, "output-pass") == NULL);
    remove(path);

    /* the output uses the input's passphrase unless output_passphrase says otherwise */
    path = write_config("\"passphrase\":\"input-pass-123\"", "");
    CHECK(parse_file(path, NULL, &o, err) == 0);
    CHECK(strstr(o.out_query, "passphrase=input-pass-123"));
    remove(path);
    path = write_config("\"passphrase\":\"input-pass-123\"", ",\"output_passphrase\":\"\"");
    CHECK(parse_file(path, NULL, &o, err) == 0);
    CHECK(strstr(o.in_query, "passphrase=input-pass-123"));
    CHECK(strstr(o.out_query, "passphrase") == NULL);
    remove(path);
}

static void test_unknown_keys_are_passed_on_for_a_warning(void) {
    relay_options_t o;
    char err[512];
    const char *path = write_config("\"passphrase\":\"\"", ",\"outpt_passphrase\":\"\",\"x\":1");
    CHECK(parse_file(path, NULL, &o, err) == 0);
    CHECK(o.unknown_key_count == 2);
    CHECK_STR(o.unknown_key, "outpt_passphrase");
    remove(path);
}

static void test_a_bad_config_file_is_refused_with_its_path_and_the_key(void) {
    relay_options_t o;
    char err[512];
    CHECK(parse_file("/nonexistent/goryu.json", NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad config file /nonexistent/goryu.json: "));
    CHECK(strstr(err, "cannot read"));

    const char *path = write_config("\"passphrase\":\"short\"", "");
    CHECK(parse_file(path, NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad config file "));
    CHECK(strstr(err, "\"passphrase\" must be"));
    CHECK(!strstr(err, "short")); /* the value itself is never in the message */
    remove(path);
}

static void test_two_uris(void) {
    relay_options_t o;
    char err[512];
    CHECK(parse_uris("srt://0.0.0.0:10081?mode=listener&groupconnect=1",
                     "srt://127.0.0.1:10080?mode=caller&latency=120", NULL, &o, err) == 0);
    CHECK_STR(o.in_host, "0.0.0.0");
    CHECK(o.in_port == 10081);
    CHECK_STR(o.in_query, "mode=listener&groupconnect=1");
    CHECK_STR(o.out_host, "127.0.0.1");
    CHECK(o.out_port == 10080);
    CHECK_STR(o.out_query, "mode=caller&latency=120");
    CHECK(o.status_port == RELAY_DEFAULT_STATUS_PORT);
    CHECK_STR(o.out_uri_redacted, "srt://127.0.0.1:10080?mode=caller&latency=120");
}

static void test_a_uri_must_be_srt_and_well_formed(void) {
    relay_options_t o;
    char err[512];
    CHECK(parse_uris("udp://0.0.0.0:10081", "srt://127.0.0.1:10080", NULL, &o, err) == -1);
    CHECK_STR(err, "Input URI must use srt://");
    CHECK(parse_uris("srt://0.0.0.0:10081", "http://127.0.0.1:10080", NULL, &o, err) == -1);
    CHECK_STR(err, "Output URI must use srt://");
    CHECK(parse_uris("not a uri", "srt://127.0.0.1:10080", NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad input URI: "));
    CHECK(parse_uris("srt://0.0.0.0:10081", "srt://127.0.0.1:notaport", NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad output URI: "));
}

/* A passphrase libsrt would refuse used to leave the relay running unencrypted. A URI
 * that fails to parse may carry one, so it is never echoed back. */
static void test_a_refused_uri_never_echoes_its_passphrase(void) {
    relay_options_t o;
    char err[512];
    CHECK(parse_uris("srt://0.0.0.0:10081?passphrase=short", "srt://127.0.0.1:10080", NULL, &o,
                     err) == -1);
    CHECK(strstr(err, "Bad input URI: "));
    CHECK(strstr(err, "\"passphrase\" must be"));
    CHECK(!strstr(err, "passphrase=short"));
    CHECK(parse_uris("srt://0.0.0.0:10081", "srt://127.0.0.1:10080?passphrase=tiny", NULL, &o,
                     err) == -1);
    CHECK(strstr(err, "Bad output URI: "));
    /* the URI that fails to parse carries a passphrase of its own */
    CHECK(parse_uris("srt://0.0.0.0:99999999999?passphrase=abcdefghij", "srt://127.0.0.1:10080",
                     NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad input URI: "));
    CHECK(!strstr(err, "abcdefghij"));
    CHECK(parse_uris("srt://0.0.0.0:10081", "srt://127.0.0.1:99999999999?passphrase=0123456789",
                     NULL, &o, err) == -1);
    CHECK(strstr(err, "Bad output URI: "));
    CHECK(!strstr(err, "0123456789"));
    CHECK(parse_uris("srt://0.0.0.0:10081", "srt://127.0.0.1:10080?passphrase=0123456789", NULL, &o,
                     err) == 0);
    CHECK(strstr(o.out_uri_redacted, "passphrase=<redacted>"));
    CHECK(!strstr(o.out_uri_redacted, "0123456789"));
}

/* SRT_BONDING_STATUS_PORT is the only way to move the status port in the two-URI form,
 * and it overrides the file's status_port too. A value that is not a port is ignored,
 * and the caller is told, instead of the port silently staying where it was. */
static void test_the_status_port_override(void) {
    relay_options_t o;
    char err[512];
    const char *in = "srt://0.0.0.0:10081";
    const char *out = "srt://127.0.0.1:10080";

    CHECK(parse_uris(in, out, "9090", &o, err) == 0);
    CHECK(o.status_port == 9090 && !o.status_port_env_ignored);
    CHECK(parse_uris(in, out, "65535", &o, err) == 0 && o.status_port == 65535);
    CHECK(parse_uris(in, out, "1", &o, err) == 0 && o.status_port == 1);

    CHECK(parse_uris(in, out, NULL, &o, err) == 0);
    CHECK(o.status_port == RELAY_DEFAULT_STATUS_PORT && !o.status_port_env_ignored);
    CHECK(parse_uris(in, out, "", &o, err) == 0); /* set but empty is "not set" */
    CHECK(o.status_port == RELAY_DEFAULT_STATUS_PORT && !o.status_port_env_ignored);

    const char *bad[] = {"abc", "0", "65536", "-1", "9090x", " 9090", "99999999999", "1.5"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(parse_uris(in, out, bad[i], &o, err) == 0);
        CHECK(o.status_port == RELAY_DEFAULT_STATUS_PORT);
        CHECK(o.status_port_env_ignored);
    }

    /* it overrides the config file's own port, and a bad value leaves that one alone */
    const char *path = write_config("\"passphrase\":\"\"", "");
    CHECK(parse_file(path, "9100", &o, err) == 0 && o.status_port == 9100);
    CHECK(parse_file(path, "nope", &o, err) == 0);
    CHECK(o.status_port == 9000 && o.status_port_env_ignored);
    remove(path);
}

static void test_the_wrong_number_of_arguments(void) {
    relay_options_t o;
    char err[512];
    char *none[] = {"goryu"};
    CHECK(parse(1, none, NULL, &o, err) == -1);
    CHECK(err[0] != '\0');
    char *many[] = {"goryu", "a", "b", "c"};
    CHECK(parse(4, many, NULL, &o, err) == -1);
}

int main(void) {
    test_a_config_file_gives_the_listener_and_the_downstream();
    test_the_passphrases_reach_their_own_side_only();
    test_unknown_keys_are_passed_on_for_a_warning();
    test_a_bad_config_file_is_refused_with_its_path_and_the_key();
    test_two_uris();
    test_a_uri_must_be_srt_and_well_formed();
    test_a_refused_uri_never_echoes_its_passphrase();
    test_the_status_port_override();
    test_the_wrong_number_of_arguments();
    TEST_SUMMARY("relay_options");
}
