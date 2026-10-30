/* Config parsing, URI handling and passphrase rules (src/relay_config.c). */

#include "../../src/relay_config.h"
#include "test.h"

#include <stdlib.h>

#define CFG_FMT                                                                                    \
    "{\"input_host\":\"0.0.0.0\",\"input_port\":%s,\"output_host\":\"127.0.0.1\","                 \
    "\"output_port\":10080,\"status_port\":8081,\"passphrase\":\"%s\"}"

static int parse_with(const char *input_port, const char *passphrase, file_config_t *cfg, char *err,
                      size_t err_sz) {
    char json[1024];
    snprintf(json, sizeof json, CFG_FMT, input_port, passphrase);
    return parse_config_json(json, cfg, err, err_sz);
}

static void repeat(char *out, char c, int n) {
    memset(out, c, (size_t)n);
    out[n] = '\0';
}

static void test_valid_config(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_with("10081", "", &cfg, err, sizeof err) == 0);
    CHECK_STR(cfg.input_host, "0.0.0.0");
    CHECK(cfg.input_port == 10081);
    CHECK_STR(cfg.output_host, "127.0.0.1");
    CHECK(cfg.output_port == 10080);
    CHECK(cfg.status_port == 8081);
    CHECK_STR(cfg.passphrase, "");
}

/* The shape Bunryu writes: JSON.stringify(config, null, 4) plus a newline. */
static void test_pretty_printed_config(void) {
    const char *json = "{\n"
                       "    \"input_host\": \"0.0.0.0\",\n"
                       "    \"input_port\": 10081,\n"
                       "    \"output_host\": \"127.0.0.1\",\n"
                       "    \"output_port\": 10080,\n"
                       "    \"status_port\": 8081,\n"
                       "    \"passphrase\": \"abcdefghij0123456789\"\n"
                       "}\n";
    file_config_t cfg;
    CHECK(parse_config_json(json, &cfg, NULL, 0) == 0);
    CHECK(cfg.input_port == 10081 && cfg.status_port == 8081);
    CHECK_STR(cfg.passphrase, "abcdefghij0123456789");
}

static void test_key_order_and_whitespace_do_not_matter(void) {
    const char *json = "{ \"passphrase\" : \"\" ,\"status_port\"\t:\n8081,\"output_port\":2,"
                       "\"output_host\":\"h\",\"input_port\":1,\"input_host\":\"i\" }";
    file_config_t cfg;
    CHECK(parse_config_json(json, &cfg, NULL, 0) == 0);
    CHECK(cfg.input_port == 1 && cfg.output_port == 2 && cfg.status_port == 8081);
}

static void test_missing_keys_are_named(void) {
    static const char *keys[] = {"input_host",  "input_port",  "output_host",
                                 "output_port", "status_port", "passphrase"};
    char json[1024];
    for (int skip = 0; skip < 6; ++skip) {
        json[0] = '\0';
        strcat(json, "{");
        for (int k = 0; k < 6; ++k) {
            if (k == skip) continue;
            char part[128];
            int is_str = k == 0 || k == 2 || k == 5;
            snprintf(part, sizeof part, is_str ? "\"%s\":\"x\"," : "\"%s\":1,", keys[k]);
            strcat(json, part);
        }
        strcat(json, "\"extra\":0}");
        file_config_t cfg;
        char err[256] = "";
        CHECK(parse_config_json(json, &cfg, err, sizeof err) == -1);
        CHECK(strstr(err, keys[skip]) != NULL);
    }
}

static void test_wrong_value_types(void) {
    file_config_t cfg;
    CHECK(parse_with("\"10081\"", "", &cfg, NULL, 0) == -1); /* port as a string */
    CHECK(parse_with("null", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("true", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("", "", &cfg, NULL, 0) == -1); /* empty value */
    const char *no_pass = "{\"input_host\":\"a\",\"input_port\":1,\"output_host\":\"a\","
                          "\"output_port\":1,\"status_port\":1,\"passphrase\":null}";
    CHECK(parse_config_json(no_pass, &cfg, NULL, 0) == -1);
    const char *num_host = "{\"input_host\":5,\"input_port\":1,\"output_host\":\"a\","
                           "\"output_port\":1,\"status_port\":1,\"passphrase\":\"\"}";
    CHECK(parse_config_json(num_host, &cfg, NULL, 0) == -1);
}

static void test_port_range_and_format(void) {
    file_config_t cfg;
    CHECK(parse_with("1", "", &cfg, NULL, 0) == 0);
    CHECK(parse_with("65535", "", &cfg, NULL, 0) == 0);
    CHECK(parse_with("0", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("-1", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("65536", "", &cfg, NULL, 0) == -1);
    /* these used to be read as 10081 (the long was cast to int, or the tail ignored) */
    CHECK(parse_with("4294977377", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("99999999999999999999", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("10081.9", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("1e3", "", &cfg, NULL, 0) == -1);
    CHECK(parse_with("10081abc", "", &cfg, NULL, 0) == -1);
}

static void test_all_three_ports_are_checked(void) {
    file_config_t cfg;
    const char *bad_out = "{\"input_host\":\"a\",\"input_port\":1,\"output_host\":\"a\","
                          "\"output_port\":70000,\"status_port\":1,\"passphrase\":\"\"}";
    const char *bad_status = "{\"input_host\":\"a\",\"input_port\":1,\"output_host\":\"a\","
                             "\"output_port\":1,\"status_port\":0,\"passphrase\":\"\"}";
    CHECK(parse_config_json(bad_out, &cfg, NULL, 0) == -1);
    CHECK(parse_config_json(bad_status, &cfg, NULL, 0) == -1);
}

static void test_passphrase_length_boundaries(void) {
    file_config_t cfg;
    char pass[300];
    char err[256];
    for (int len = 0; len <= 100; ++len) {
        repeat(pass, 'Q', len);
        err[0] = '\0';
        int rc = parse_with("1", pass, &cfg, err, sizeof err);
        int allowed = len == 0 || (len >= PASSPHRASE_MIN_LEN && len <= PASSPHRASE_MAX_LEN);
        CHECK((rc == 0) == allowed);
        if (!allowed) {
            CHECK(strstr(err, "passphrase") != NULL);
            CHECK(strstr(err, pass) == NULL || len == 0); /* never echoes the secret */
        }
    }
    repeat(pass, 'Q', 255);
    CHECK(parse_with("1", pass, &cfg, NULL, 0) == -1);
    repeat(pass, 'Q', 256); /* longer than the buffer */
    CHECK(parse_with("1", pass, &cfg, NULL, 0) == -1);
}

static void test_passphrase_error_message(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_with("1", "short", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "10 to 79") != NULL);
    CHECK(strstr(err, "got 5") != NULL);
    CHECK(strstr(err, "short") == NULL);
}

static void test_check_passphrase(void) {
    char err[128];
    CHECK(check_passphrase("passphrase", "", NULL, 0) == 0);
    CHECK(check_passphrase("passphrase", "123456789", err, sizeof err) == -1);
    CHECK(check_passphrase("passphrase", "1234567890", err, sizeof err) == 0);
    /* length is counted in bytes, as libsrt does: 5 x 2-byte characters = 10 */
    CHECK(check_passphrase("passphrase", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", err,
                           sizeof err) == 0);
    CHECK(check_passphrase("passphrase", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", err, sizeof err) ==
          -1);
}

static void test_passphrase_escapes(void) {
    file_config_t cfg;
    CHECK(parse_with("1", "ab\\\"cd\\\\ef\\/gh01", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.passphrase, "ab\"cd\\ef/gh01");
    /* other escapes are refused instead of silently becoming a different string */
    CHECK(parse_with("1", "abcdefgh\\n12", &cfg, NULL, 0) == -1);
    CHECK(parse_with("1", "abcdefgh\\u004112", &cfg, NULL, 0) == -1);
    CHECK(parse_with("1", "abcdefgh\\t012", &cfg, NULL, 0) == -1);
}

static void test_unterminated_string(void) {
    file_config_t cfg;
    const char *json = "{\"input_host\":\"a\",\"input_port\":1,\"output_host\":\"a\","
                       "\"output_port\":1,\"status_port\":1,\"passphrase\":\"abcdefghijkl";
    CHECK(parse_config_json(json, &cfg, NULL, 0) == -1);
}

static void test_host_length(void) {
    file_config_t cfg;
    char host[400];
    char json[1024];
    repeat(host, 'h', 255);
    snprintf(json, sizeof json,
             "{\"input_host\":\"%s\",\"input_port\":1,\"output_host\":\"a\",\"output_port\":1,"
             "\"status_port\":1,\"passphrase\":\"\"}",
             host);
    CHECK(parse_config_json(json, &cfg, NULL, 0) == 0);
    repeat(host, 'h', 256);
    snprintf(json, sizeof json,
             "{\"input_host\":\"%s\",\"input_port\":1,\"output_host\":\"a\",\"output_port\":1,"
             "\"status_port\":1,\"passphrase\":\"\"}",
             host);
    CHECK(parse_config_json(json, &cfg, NULL, 0) == -1);
}

static void test_empty_and_garbage(void) {
    file_config_t cfg;
    CHECK(parse_config_json("", &cfg, NULL, 0) == -1);
    CHECK(parse_config_json("not json", &cfg, NULL, 0) == -1);
    CHECK(parse_config_json("{}", &cfg, NULL, 0) == -1);
}

static void test_cfg_is_cleared_on_failure(void) {
    file_config_t cfg;
    memset(&cfg, 0xAB, sizeof cfg);
    CHECK(parse_config_json("{}", &cfg, NULL, 0) == -1);
    CHECK(cfg.input_port == 0 && cfg.passphrase[0] == '\0');
}

/* The six required keys with valid values, for tests that add or break something else. */
#define SIX_KEYS                                                                                   \
    "\"input_host\":\"a\",\"input_port\":1,\"output_host\":\"b\",\"output_port\":2,"               \
    "\"status_port\":3,\"passphrase\":\"\""

/* A value that happens to be a key's name is a value, not that key. */
static void test_a_value_can_be_a_key_name(void) {
    const char *json = "{\"input_host\":\"passphrase\",\"input_port\":1,"
                       "\"output_host\":\"status_port\",\"output_port\":2,\"status_port\":3,"
                       "\"passphrase\":\"\"}";
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_config_json(json, &cfg, err, sizeof err) == 0);
    CHECK_STR(cfg.input_host, "passphrase");
    CHECK_STR(cfg.output_host, "status_port");
    CHECK(cfg.status_port == 3);
}

static void test_duplicate_keys_are_refused(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_config_json("{" SIX_KEYS ",\"input_host\":\"c\"}", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "input_host") != NULL);
    err[0] = '\0';
    CHECK(parse_config_json("{" SIX_KEYS ",\"output_passphrase\":\"\",\"output_passphrase\":\"\"}",
                            &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "output_passphrase") != NULL);
}

/* The file is a flat object: a nested value is refused, not searched for keys. */
static void test_nested_values_are_refused(void) {
    file_config_t cfg;
    CHECK(parse_config_json("{" SIX_KEYS ",\"extra\":{\"output_passphrase\":\"\"}}", &cfg, NULL,
                            0) == -1);
    CHECK(parse_config_json("{" SIX_KEYS ",\"extra\":[1,2]}", &cfg, NULL, 0) == -1);
    CHECK(parse_config_json("{\"extra\":{" SIX_KEYS "}}", &cfg, NULL, 0) == -1);
}

static void test_text_after_the_object_is_refused(void) {
    file_config_t cfg;
    CHECK(parse_config_json("{" SIX_KEYS "}", &cfg, NULL, 0) == 0);
    CHECK(parse_config_json("{" SIX_KEYS "}\n  \n", &cfg, NULL, 0) == 0);
    CHECK(parse_config_json("{" SIX_KEYS "} garbage", &cfg, NULL, 0) == -1);
    CHECK(parse_config_json("{" SIX_KEYS "}{" SIX_KEYS "}", &cfg, NULL, 0) == -1);
}

static void test_syntax_errors_are_refused(void) {
    file_config_t cfg;
    CHECK(parse_config_json("{" SIX_KEYS ",}", &cfg, NULL, 0) == -1);        /* trailing comma */
    CHECK(parse_config_json("{" SIX_KEYS "\"x\":1}", &cfg, NULL, 0) == -1);  /* missing comma */
    CHECK(parse_config_json("{" SIX_KEYS ",\"x\" 1}", &cfg, NULL, 0) == -1); /* missing colon */
    CHECK(parse_config_json("{" SIX_KEYS ",x:1}", &cfg, NULL, 0) == -1);     /* key not a string */
    CHECK(parse_config_json("{" SIX_KEYS, &cfg, NULL, 0) == -1);             /* no closing brace */
    CHECK(parse_config_json("[" SIX_KEYS "]", &cfg, NULL, 0) == -1);         /* not an object */
    CHECK(parse_config_json("  {" SIX_KEYS "}", &cfg, NULL, 0) == 0); /* leading space is fine */
    /* the old text search read a file that starts with a UTF-8 byte order mark */
    CHECK(parse_config_json("\xEF\xBB\xBF{" SIX_KEYS "}", &cfg, NULL, 0) == 0);
    CHECK(parse_config_json("{\xEF\xBB\xBF" SIX_KEYS "}", &cfg, NULL, 0) == -1);
}

/* An optional key with a typo is ignored, but it must be visible to the caller. */
static void test_unknown_keys_are_counted(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_config_json("{" SIX_KEYS "}", &cfg, err, sizeof err) == 0);
    CHECK(cfg.unknown_key_count == 0 && cfg.unknown_key[0] == '\0');

    CHECK(parse_config_json("{" SIX_KEYS
                            ",\"outpt_passphrase\":\"\",\"b\":true,\"c\":null,\"d\":1.5}",
                            &cfg, err, sizeof err) == 0);
    CHECK(cfg.unknown_key_count == 4);
    CHECK_STR(cfg.unknown_key, "outpt_passphrase");
    CHECK(!cfg.output_passphrase_explicit);

    /* the name is logged, so a newline or a non-ASCII byte in it cannot reach the log as itself */
    CHECK(parse_config_json("{" SIX_KEYS ",\"a\nb\xc3\xa9\\\"c\":1}", &cfg, err, sizeof err) == 0);
    CHECK_STR(cfg.unknown_key, "a?b??\"c");

    /* a very long name is cut to fit, not an error */
    char json[512], name[200];
    repeat(name, 'k', 150);
    snprintf(json, sizeof json, "{%s,\"%s\":1}", SIX_KEYS, name);
    CHECK(parse_config_json(json, &cfg, err, sizeof err) == 0);
    CHECK(cfg.unknown_key_count == 1 && strlen(cfg.unknown_key) == sizeof cfg.unknown_key - 1);
}

/* Errors name the key or a byte offset, never anything the file contained. */
static void test_syntax_errors_do_not_echo_the_file(void) {
    file_config_t cfg;
    char err[256] = "";
    const char *bad[] = {
        "{\"passphrase\":\"supersecret0123\" \"x\":1}",
        "{\"passphrase\":\"supersecret0123\",\"passphrase\":\"supersecret0123\"}",
        "{\"supersecret0123\":{}}",
        "supersecret0123",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        err[0] = '\0';
        CHECK(parse_config_json(bad[i], &cfg, err, sizeof err) == -1);
        CHECK(err[0] != '\0');
        CHECK(strstr(err, "supersecret") == NULL);
    }
}

static void test_load_config_file(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(load_config_file("/nonexistent/goryu.json", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "cannot read") != NULL);

    char path[] = "/tmp/goryu-unit-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    fprintf(f, CFG_FMT, "10081", "abcdefghij12");
    fclose(f);
    CHECK(load_config_file(path, &cfg, err, sizeof err) == 0);
    CHECK(cfg.input_port == 10081);
    CHECK_STR(cfg.passphrase, "abcdefghij12");

    f = fopen(path, "w"); /* now empty */
    fclose(f);
    CHECK(load_config_file(path, &cfg, err, sizeof err) == -1);
    remove(path);
    /* Not regular files: a directory (ftell reports LONG_MAX on some filesystems,
     * which used to become a huge allocation), a device that never ends, a pipe. */
    CHECK(load_config_file("/tmp", &cfg, err, sizeof err) == -1);
    CHECK(load_config_file(".", &cfg, err, sizeof err) == -1);
    CHECK(load_config_file("/dev/zero", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "cannot read") != NULL);

    /* A config is a few hundred bytes; an oversized file is refused, not slurped. */
    char big_path[] = "/tmp/goryu-unit-big-XXXXXX";
    fd = mkstemp(big_path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    f = fdopen(fd, "w");
    fprintf(f, CFG_FMT, "10081", "");
    for (int i = 0; i < 70000; ++i)
        fputc(' ', f);
    fclose(f);
    CHECK(load_config_file(big_path, &cfg, err, sizeof err) == -1);
    remove(big_path);
}

/* ---- URIs ---- */

typedef struct {
    const char *uri;
    int ok;
    const char *host;
    int port;
    const char *query;
} uri_case_t;

static void test_parse_uri_table(void) {
    static const uri_case_t cases[] = {
        {"srt://0.0.0.0:10081?mode=listener", 1, "0.0.0.0", 10081, "mode=listener"},
        {"srt://example.com:1", 1, "example.com", 1, ""},
        {"srt://host:65535", 1, "host", 65535, ""},
        {"srt://[::1]:10081", 1, "[::1]", 10081, ""},
        {"srt://host:10081?streamid=a:b", 1, "host", 10081, "streamid=a:b"},
        {"srt://:10081", 1, "", 10081, ""},
        {"srt://host", 0, "", 0, ""},
        {"srt://host:", 0, "", 0, ""},
        {"srt://host:0", 0, "", 0, ""},
        {"srt://host:65536", 0, "", 0, ""},
        {"srt://host:-5", 0, "", 0, ""},
        {"srt://host:+5", 0, "", 0, ""},
        {"srt://host:10081abc", 0, "", 0, ""},
        {"srt://host:1.5", 0, "", 0, ""},
        {"srt://host:99999999999999999999", 0, "", 0, ""},
        {"notauri", 0, "", 0, ""},
        {"", 0, "", 0, ""},
        {"://host:1", 1, "host", 1, ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char scheme[8] = "", host[64] = "", query[128] = "";
        int port = -1;
        int rc = parse_uri(cases[i].uri, scheme, sizeof scheme, host, sizeof host, &port, query,
                           sizeof query);
        if (cases[i].ok) {
            CHECK(rc == 0);
            CHECK_STR(host, cases[i].host);
            CHECK(port == cases[i].port);
            CHECK_STR(query, cases[i].query);
        } else {
            if (rc != -1) printf("  (uri: %s)\n", cases[i].uri);
            CHECK(rc == -1);
        }
    }
}

static void test_parse_uri_buffers(void) {
    char scheme[8], host[8], query[8];
    int port;
    /* a host that does not fit is an error, not a silently shortened host */
    CHECK(parse_uri("srt://0123456789:1", scheme, sizeof scheme, host, sizeof host, &port, query,
                    sizeof query) == -1);
    CHECK(parse_uri("srt://0123456:1", scheme, sizeof scheme, host, sizeof host, &port, query,
                    sizeof query) == 0);
    CHECK_STR(host, "0123456");
    CHECK(parse_uri("verylongscheme://h:1", scheme, sizeof scheme, host, sizeof host, &port, query,
                    sizeof query) == -1);
    /* a long query is cut to the buffer and stays terminated */
    CHECK(parse_uri("srt://h:1?aaaaaaaaaaaaaaaaaaaa", scheme, sizeof scheme, host, sizeof host,
                    &port, query, sizeof query) == 0);
    CHECK(strlen(query) == sizeof query - 1);
    /* a NULL query buffer is allowed */
    CHECK(parse_uri("srt://h:1?x=1", scheme, sizeof scheme, host, sizeof host, &port, NULL, 0) ==
          0);
}

static void test_get_param(void) {
    char v[16];
    CHECK(get_param("streamid=abc&x=1", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "abc");
    CHECK(get_param("a=1&streamid=abc", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "abc");
    CHECK(get_param("xstreamid=1", "streamid", v, sizeof v) == 0); /* not a suffix match */
    CHECK(get_param("streamidx=1", "streamid", v, sizeof v) == 0);
    CHECK(get_param("", "streamid", v, sizeof v) == 0);
    CHECK(get_param("streamid", "streamid", v, sizeof v) == 0); /* no '=' */
    CHECK(get_param("streamid=&a=1", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "");
    CHECK(get_param("streamid=a%2Fb", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "a/b");
    CHECK(get_param("streamid=a%2fb%20c", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "a/b c");
    /* malformed escapes are kept as typed */
    CHECK(get_param("streamid=a%ZZb", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "a%ZZb");
    CHECK(get_param("streamid=ab%", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "ab%");
    CHECK(get_param("streamid=ab%4", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "ab%4");
    /* a value longer than the buffer is cut and terminated */
    CHECK(get_param("streamid=0123456789abcdefXYZ", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "0123456789abcde");
    /* only the first match counts */
    CHECK(get_param("streamid=first&streamid=second", "streamid", v, sizeof v) == 1);
    CHECK_STR(v, "first");
}

static void test_build_srt_uri(void) {
    char uri[2048];
    CHECK(build_srt_uri(uri, sizeof uri, "0.0.0.0", 10081, "", 1) == 0);
    CHECK_STR(uri, "srt://0.0.0.0:10081?mode=listener&groupconnect=1&transtype=live&latency=240");
    CHECK(build_srt_uri(uri, sizeof uri, "127.0.0.1", 10080, "", 0) == 0);
    CHECK_STR(uri, "srt://127.0.0.1:10080?mode=caller&transtype=live&latency=200");
    CHECK(build_srt_uri(uri, sizeof uri, "h", 1, NULL, 0) == 0);
    CHECK(strstr(uri, "passphrase") == NULL);
    CHECK(build_srt_uri(uri, sizeof uri, "h", 1, "abcdefghij", 0) == 0);
    CHECK(strstr(uri, "&passphrase=abcdefghij&pbkeylen=16") != NULL);
    /* reserved characters are percent-encoded and survive a round trip */
    CHECK(build_srt_uri(uri, sizeof uri, "h", 1, "p@ss w&rd=1234", 0) == 0);
    CHECK(strstr(uri, "passphrase=p%40ss%20w%26rd%3D1234&") != NULL);
    char scheme[8], host[64], query[1024], back[128];
    int port;
    CHECK(parse_uri(uri, scheme, sizeof scheme, host, sizeof host, &port, query, sizeof query) ==
          0);
    CHECK(get_param(query, "passphrase", back, sizeof back) == 1);
    CHECK_STR(back, "p@ss w&rd=1234");
}

static void test_build_srt_uri_truncation_is_an_error(void) {
    char uri[40];
    CHECK(build_srt_uri(uri, sizeof uri, "hostname", 1, "", 0) == -1);
    char tiny[1];
    CHECK(build_srt_uri(tiny, sizeof tiny, "h", 1, "", 0) == -1);
    CHECK(build_srt_uri(NULL, 10, "h", 1, "", 0) == -1);
    CHECK(build_srt_uri(uri, 0, "h", 1, "", 0) == -1);
    /* the longest passphrase the config allows, fully escaped, still fits */
    char big[2048], pass[PASSPHRASE_MAX_LEN + 1];
    memset(pass, '%', PASSPHRASE_MAX_LEN);
    pass[PASSPHRASE_MAX_LEN] = '\0';
    CHECK(build_srt_uri(big, sizeof big, "h", 1, pass, 0) == 0);
}

static void test_redact_srt_uri(void) {
    char out[256];
    redact_srt_uri("srt://h:1?mode=caller&passphrase=secretvalue12&pbkeylen=16", out, sizeof out);
    CHECK_STR(out, "srt://h:1?mode=caller&passphrase=<redacted>&pbkeylen=16");
    redact_srt_uri("srt://h:1?passphrase=secretvalue12", out, sizeof out);
    CHECK_STR(out, "srt://h:1?passphrase=<redacted>");
    redact_srt_uri("srt://h:1?a=1&passphrase=p%40ss%26w", out, sizeof out);
    CHECK_STR(out, "srt://h:1?a=1&passphrase=<redacted>");
    redact_srt_uri("srt://h:1?mode=caller", out, sizeof out);
    CHECK_STR(out, "srt://h:1?mode=caller");
    redact_srt_uri("", out, sizeof out);
    CHECK_STR(out, "");
    /* only a parameter with exactly this name is redacted */
    redact_srt_uri("srt://h:1?xpassphrase=1&passphrasex=2", out, sizeof out);
    CHECK_STR(out, "srt://h:1?xpassphrase=1&passphrasex=2");
    /* an empty value is masked too (it is still a passphrase parameter) */
    redact_srt_uri("srt://h:1?passphrase=&a=1", out, sizeof out);
    CHECK_STR(out, "srt://h:1?passphrase=<redacted>&a=1");
    /* two of them */
    redact_srt_uri("srt://h:1?passphrase=aaaaaaaaaa&passphrase=bbbbbbbbbb", out, sizeof out);
    CHECK_STR(out, "srt://h:1?passphrase=<redacted>&passphrase=<redacted>");
    /* a small buffer truncates and terminates, and the secret still cannot leak */
    char small[24];
    redact_srt_uri("srt://h:1?passphrase=secretvalue12", small, sizeof small);
    CHECK(strlen(small) < sizeof small);
    CHECK(strstr(small, "secret") == NULL);
    char one[1] = {'x'};
    redact_srt_uri("srt://h:1", one, sizeof one);
    CHECK(one[0] == '\0');
    redact_srt_uri("srt://h:1", NULL, 0); /* must not crash */
}

static void test_redacting_a_built_uri(void) {
    char uri[2048], red[2048];
    CHECK(build_srt_uri(uri, sizeof uri, "127.0.0.1", 10080, "my secret/pass+word", 0) == 0);
    redact_srt_uri(uri, red, sizeof red);
    CHECK(strstr(red, "secret") == NULL);
    CHECK(strstr(red, "passphrase=<redacted>&pbkeylen=16") != NULL);
}

/* ---- output_passphrase ---- */

#define OUT_FMT                                                                                    \
    "{\"input_host\":\"0.0.0.0\",\"input_port\":10081,\"output_host\":\"127.0.0.1\","              \
    "\"output_port\":10080,\"status_port\":8081,\"passphrase\":\"%s\"%s}"

static int parse_out(const char *passphrase, const char *extra, file_config_t *cfg, char *err,
                     size_t err_sz) {
    char json[1024];
    snprintf(json, sizeof json, OUT_FMT, passphrase, extra);
    return parse_config_json(json, cfg, err, err_sz);
}

static void test_output_passphrase_defaults_to_the_input_passphrase(void) {
    file_config_t cfg;
    CHECK(parse_out("abcdefghij12", "", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.output_passphrase, "abcdefghij12");
    CHECK(cfg.output_passphrase_explicit == 0);
    /* no encryption on the input means none on the output either */
    CHECK(parse_out("", "", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.output_passphrase, "");
    CHECK(cfg.output_passphrase_explicit == 0);
}

static void test_output_passphrase_can_be_empty_or_different(void) {
    file_config_t cfg;
    /* encrypted input, plain downstream */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"\"", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.passphrase, "abcdefghij12");
    CHECK_STR(cfg.output_passphrase, "");
    CHECK(cfg.output_passphrase_explicit == 1);
    /* a different downstream passphrase */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"zyxwvutsrq99\"", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.passphrase, "abcdefghij12");
    CHECK_STR(cfg.output_passphrase, "zyxwvutsrq99");
    /* plain input, encrypted downstream */
    CHECK(parse_out("", ",\"output_passphrase\":\"zyxwvutsrq99\"", &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.passphrase, "");
    CHECK_STR(cfg.output_passphrase, "zyxwvutsrq99");
    /* the same value written out is explicit, and equal */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"abcdefghij12\"", &cfg, NULL, 0) == 0);
    CHECK(cfg.output_passphrase_explicit == 1);
}

static void test_output_passphrase_is_not_confused_with_passphrase(void) {
    file_config_t cfg;
    /* the key written first, and in the middle of the object */
    const char *first = "{\"output_passphrase\":\"\",\"input_host\":\"a\",\"input_port\":1,"
                        "\"output_host\":\"a\",\"output_port\":1,\"status_port\":1,"
                        "\"passphrase\":\"abcdefghij12\"}";
    CHECK(parse_config_json(first, &cfg, NULL, 0) == 0);
    CHECK_STR(cfg.passphrase, "abcdefghij12");
    CHECK_STR(cfg.output_passphrase, "");
    /* only output_passphrase present: passphrase is still required */
    const char *only = "{\"output_passphrase\":\"abcdefghij12\",\"input_host\":\"a\","
                       "\"input_port\":1,\"output_host\":\"a\",\"output_port\":1,"
                       "\"status_port\":1}";
    char err[256] = "";
    CHECK(parse_config_json(only, &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "\"passphrase\"") != NULL);
}

static void test_output_passphrase_is_validated(void) {
    file_config_t cfg;
    char err[256];
    for (int len = 0; len <= 100; ++len) {
        char pass[128], extra[256];
        repeat(pass, 'Q', len);
        snprintf(extra, sizeof extra, ",\"output_passphrase\":\"%s\"", pass);
        err[0] = '\0';
        int rc = parse_out("abcdefghij12", extra, &cfg, err, sizeof err);
        int allowed = len == 0 || (len >= PASSPHRASE_MIN_LEN && len <= PASSPHRASE_MAX_LEN);
        CHECK((rc == 0) == allowed);
        if (!allowed) {
            CHECK(strstr(err, "\"output_passphrase\"") != NULL); /* names the right key */
            CHECK(strstr(err, pass) == NULL);                    /* and never the value */
        }
    }
    /* the input passphrase is still checked when the output one is fine */
    CHECK(parse_out("short", ",\"output_passphrase\":\"zyxwvutsrq99\"", &cfg, err, sizeof err) ==
          -1);
    CHECK(strstr(err, "\"passphrase\" must be") != NULL);
    /* a short value is not echoed, and not the other key's name */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"tiny\"", &cfg, err, sizeof err) ==
          -1);
    CHECK(strstr(err, "tiny") == NULL);
    CHECK(strstr(err, "got 4") != NULL);
}

static void test_output_passphrase_wrong_types_are_errors(void) {
    file_config_t cfg;
    char err[256] = "";
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":null", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "output_passphrase") != NULL);
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":12345678901", &cfg, NULL, 0) == -1);
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":true", &cfg, NULL, 0) == -1);
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"unterminated", &cfg, NULL, 0) == -1);
    /* unsupported escapes are refused here too, not read as a different string */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"abcdefgh\\n12\"", &cfg, NULL, 0) ==
          -1);
    /* ...while an escaped backslash is a valid character of the passphrase */
    CHECK(parse_out("abcdefghij12", ",\"output_passphrase\":\"abcdefgh\\\\n12\"", &cfg, NULL, 0) ==
          0);
    CHECK_STR(cfg.output_passphrase, "abcdefgh\\n12");
}

static void test_each_side_gets_its_own_uri_passphrase(void) {
    file_config_t cfg;
    char in_uri[512], out_uri[512], red[512];
    CHECK(parse_out("inputpass-123", ",\"output_passphrase\":\"outputpass-456\"", &cfg, NULL, 0) ==
          0);
    CHECK(build_srt_uri(in_uri, sizeof in_uri, "0.0.0.0", 10081, cfg.passphrase, 1) == 0);
    CHECK(build_srt_uri(out_uri, sizeof out_uri, "127.0.0.1", 10080, cfg.output_passphrase, 0) ==
          0);
    CHECK(strstr(in_uri, "passphrase=inputpass-123&pbkeylen=16") != NULL);
    CHECK(strstr(out_uri, "passphrase=outputpass-456&pbkeylen=16") != NULL);
    CHECK(strstr(in_uri, "outputpass") == NULL && strstr(out_uri, "inputpass") == NULL);
    /* a plain downstream has no passphrase parameter at all */
    CHECK(parse_out("inputpass-123", ",\"output_passphrase\":\"\"", &cfg, NULL, 0) == 0);
    CHECK(build_srt_uri(out_uri, sizeof out_uri, "127.0.0.1", 10080, cfg.output_passphrase, 0) ==
          0);
    CHECK(strstr(out_uri, "passphrase") == NULL && strstr(out_uri, "pbkeylen") == NULL);
    /* both stay out of the log */
    redact_srt_uri(in_uri, red, sizeof red);
    CHECK(strstr(red, "inputpass") == NULL);
}

int main(void) {
    test_valid_config();
    test_pretty_printed_config();
    test_key_order_and_whitespace_do_not_matter();
    test_missing_keys_are_named();
    test_wrong_value_types();
    test_port_range_and_format();
    test_all_three_ports_are_checked();
    test_passphrase_length_boundaries();
    test_passphrase_error_message();
    test_check_passphrase();
    test_passphrase_escapes();
    test_unterminated_string();
    test_host_length();
    test_empty_and_garbage();
    test_cfg_is_cleared_on_failure();
    test_a_value_can_be_a_key_name();
    test_duplicate_keys_are_refused();
    test_nested_values_are_refused();
    test_text_after_the_object_is_refused();
    test_syntax_errors_are_refused();
    test_unknown_keys_are_counted();
    test_syntax_errors_do_not_echo_the_file();
    test_load_config_file();
    test_parse_uri_table();
    test_parse_uri_buffers();
    test_get_param();
    test_build_srt_uri();
    test_build_srt_uri_truncation_is_an_error();
    test_redact_srt_uri();
    test_redacting_a_built_uri();
    test_output_passphrase_defaults_to_the_input_passphrase();
    test_output_passphrase_can_be_empty_or_different();
    test_output_passphrase_is_not_confused_with_passphrase();
    test_output_passphrase_is_validated();
    test_output_passphrase_wrong_types_are_errors();
    test_each_side_gets_its_own_uri_passphrase();
    TEST_SUMMARY("relay_config");
}
