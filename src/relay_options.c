#include "relay_options.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relay_config.h"

static int fail(char *err, size_t err_sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Writes the reason, if the caller wants one, and returns -1. */
static int fail(char *err, size_t err_sz, const char *fmt, ...) {
    if (err && err_sz > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_sz, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* The two-URI form takes SRT options straight from the query strings, so it gets the
 * same passphrase rule as the config file. */
static int check_uri_passphrase(const char *query, const char *which, char *err, size_t err_sz) {
    char pass[512];
    char reason[256];
    if (get_param(query, "passphrase", pass, sizeof pass) &&
        check_passphrase("passphrase", pass, reason, sizeof reason) != 0) {
        return fail(err, err_sz, "Bad %s URI: %s", which, reason);
    }
    return 0;
}

/* The SRT URIs of a config file: the listener's and the downstream's. */
static int uris_from_config_file(const char *path, char *in_uri, size_t in_sz, char *out_uri,
                                 size_t out_sz, relay_options_t *opts, char *err, size_t err_sz) {
    file_config_t cfg;
    char reason[256];
    if (load_config_file(path, &cfg, reason, sizeof reason) < 0) {
        return fail(err, err_sz, "Bad config file %s: %s", path, reason);
    }
    if (build_srt_uri(in_uri, in_sz, cfg.input_host, cfg.input_port, cfg.passphrase, 1) != 0 ||
        build_srt_uri(out_uri, out_sz, cfg.output_host, cfg.output_port, cfg.output_passphrase,
                      0) != 0) {
        return fail(err, err_sz, "Bad config file %s: failed to build SRT URI", path);
    }
    opts->status_port = cfg.status_port;
    opts->unknown_key_count = cfg.unknown_key_count;
    snprintf(opts->unknown_key, sizeof opts->unknown_key, "%s", cfg.unknown_key);
    return 0;
}

/* A port number and nothing else. */
static int parse_port(const char *text, int *port) {
    char *end = NULL;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || v < 1 || v > 65535) return 0;
    if (text[0] < '0' || text[0] > '9') return 0; /* no sign and no leading space */
    *port = (int)v;
    return 1;
}

int parse_relay_options(int argc, char *argv[], const char *status_port_env, relay_options_t *opts,
                        char *err, size_t err_sz) {
    memset(opts, 0, sizeof *opts);
    opts->status_port = RELAY_DEFAULT_STATUS_PORT;

    char in_uri[2048], out_uri[2048];
    if (argc == 2) {
        if (uris_from_config_file(argv[1], in_uri, sizeof in_uri, out_uri, sizeof out_uri, opts,
                                  err, err_sz) != 0)
            return -1;
    } else if (argc == 3) {
        snprintf(in_uri, sizeof in_uri, "%s", argv[1]);
        snprintf(out_uri, sizeof out_uri, "%s", argv[2]);
    } else {
        return fail(err, err_sz, "expected a config file or an input and an output URI");
    }

    /* A URI can carry a passphrase, so only a redacted copy is ever written down. */
    char in_redacted[sizeof in_uri];
    redact_srt_uri(in_uri, in_redacted, sizeof in_redacted);
    redact_srt_uri(out_uri, opts->out_uri_redacted, sizeof opts->out_uri_redacted);

    char in_scheme[8], out_scheme[8];
    if (parse_uri(in_uri, in_scheme, sizeof in_scheme, opts->in_host, sizeof opts->in_host,
                  &opts->in_port, opts->in_query, sizeof opts->in_query) < 0) {
        return fail(err, err_sz, "Bad input URI: %s", in_redacted);
    }
    if (parse_uri(out_uri, out_scheme, sizeof out_scheme, opts->out_host, sizeof opts->out_host,
                  &opts->out_port, opts->out_query, sizeof opts->out_query) < 0) {
        return fail(err, err_sz, "Bad output URI: %s", opts->out_uri_redacted);
    }
    if (strcmp(in_scheme, "srt") != 0) return fail(err, err_sz, "Input URI must use srt://");
    if (strcmp(out_scheme, "srt") != 0) return fail(err, err_sz, "Output URI must use srt://");
    if (check_uri_passphrase(opts->in_query, "input", err, err_sz) < 0 ||
        check_uri_passphrase(opts->out_query, "output", err, err_sz) < 0) {
        return -1;
    }

    if (status_port_env && status_port_env[0]) {
        if (parse_port(status_port_env, &opts->status_port) == 0) opts->status_port_env_ignored = 1;
    }
    return 0;
}
