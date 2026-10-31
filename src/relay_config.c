#include "relay_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* fmt must contain exactly one %s. */
static void set_err(char *err, size_t err_sz, const char *fmt, const char *arg) {
    if (!err || err_sz == 0) return;
    snprintf(err, err_sz, fmt, arg);
}

int parse_uri(const char *uri, char *scheme, size_t scheme_sz, char *host, size_t host_sz,
              int *port, char *query, size_t query_sz) {
    const char *p = strstr(uri, "://");
    if (!p) return -1;

    size_t slen = (size_t)(p - uri);
    if (slen >= scheme_sz) return -1;
    memcpy(scheme, uri, slen);
    scheme[slen] = '\0';
    p += 3;

    const char *qmark = strchr(p, '?');
    const char *host_end = qmark ? qmark : p + strlen(p);
    const char *colon = NULL;
    for (const char *c = host_end; c > p; --c) {
        if (*(c - 1) == ':') {
            colon = c - 1;
            break;
        }
    }
    if (!colon) return -1;

    size_t hlen = (size_t)(colon - p);
    if (hlen >= host_sz) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    char portbuf[16];
    size_t plen = (size_t)(host_end - colon - 1);
    if (plen == 0 || plen >= sizeof portbuf) return -1;
    for (size_t i = 0; i < plen; ++i) {
        if (colon[1 + i] < '0' || colon[1 + i] > '9') return -1;
    }
    memcpy(portbuf, colon + 1, plen);
    portbuf[plen] = '\0';
    *port = atoi(portbuf);
    if (*port <= 0 || *port > 65535) return -1;

    if (query && query_sz > 0) {
        query[0] = '\0';
        if (qmark) {
            size_t qlen = strlen(qmark + 1);
            if (qlen >= query_sz) qlen = query_sz - 1;
            memcpy(query, qmark + 1, qlen);
            query[qlen] = '\0';
        }
    }
    return 0;
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int get_param(const char *query, const char *key, char *val, size_t val_sz) {
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= val_sz) vlen = val_sz - 1;
            memcpy(val, v, vlen);
            val[vlen] = '\0';
            char *src = val;
            char *dst = val;
            while (*src) {
                if (src[0] == '%' && is_hex(src[1]) && is_hex(src[2])) {
                    char hex[3];
                    hex[0] = src[1];
                    hex[1] = src[2];
                    hex[2] = '\0';
                    *dst++ = (char)strtol(hex, NULL, 16);
                    src += 3;
                    continue;
                }
                *dst++ = *src++;
            }
            *dst = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        ++p;
    return p;
}

/* goryu.json is a flat object of the keys in docs/2-usage/1-configuration.md, with
 * string and integer values. It is read in one pass by the scanner below, not by a
 * JSON library (the build depends on nothing but libsrt and OpenSSL). Anything outside
 * that shape (a duplicate key, a nested value, a missing comma, text after the
 * object) is refused with the byte where it went wrong, so it cannot be mistaken
 * for a setting. */

typedef enum { STR_OK, STR_UNUSABLE, STR_SYNTAX } str_result_t;

/* Scans the string whose opening quote *pp points at and moves *pp past the
 * closing quote. Copies it into out (NULL with out_sz 0 to skip it), always
 * NUL-terminated, decoding only the escapes \" \\ and \/. Returns STR_UNUSABLE for
 * a well-formed string that cannot be a value: any other escape is refused rather
 * than silently turned into a different string (a passphrase containing \n would
 * otherwise be read as the letter n), and so is one that does not fit. Returns
 * STR_SYNTAX, leaving *pp alone, when the string never ends. */
static str_result_t scan_string(const char **pp, char *out, size_t out_sz) {
    const char *p = *pp + 1;
    size_t len = 0;
    int usable = 1;

    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p) break;
            if (*p != '"' && *p != '\\' && *p != '/') usable = 0;
        }
        if (len + 1 < out_sz)
            out[len] = *p;
        else
            usable = 0;
        ++len;
        ++p;
    }
    if (*p != '"') return STR_SYNTAX;
    if (out_sz > 0) out[len < out_sz ? len : out_sz - 1] = '\0';
    *pp = p + 1;
    return usable ? STR_OK : STR_UNUSABLE;
}

/* The end of a number or literal (true, false, null): the next delimiter. */
static const char *scan_token(const char *p) {
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
        ++p;
    return p;
}

/* The shape of a value the relay ignores: a JSON number or true, false or null. */
static int is_number_or_literal(const char *tok, const char *end) {
    size_t n = (size_t)(end - tok);
    if ((n == 4 && memcmp(tok, "true", 4) == 0) || (n == 4 && memcmp(tok, "null", 4) == 0) ||
        (n == 5 && memcmp(tok, "false", 5) == 0))
        return 1;
    if (!(*tok == '-' || (*tok >= '0' && *tok <= '9'))) return 0;
    for (const char *c = tok; c < end; ++c) {
        if (!((*c >= '0' && *c <= '9') || *c == '-' || *c == '+' || *c == '.' || *c == 'e' ||
              *c == 'E'))
            return 0;
    }
    return 1;
}

/* An integer value: digits only (optional sign), no fraction or exponent, and it
 * must fit an int, so 4294977377 is refused instead of wrapping to 10081. */
static int parse_int_token(const char *tok, const char *end, int *out) {
    char *num_end = NULL;
    errno = 0;
    long v = strtol(tok, &num_end, 10);
    if (num_end != end || errno == ERANGE || v < INT_MIN || v > INT_MAX) return 0;
    *out = (int)v;
    return 1;
}

typedef struct known_key {
    const char *name;
    int is_int;
    size_t offset; /* of the field in file_config_t */
    size_t size;   /* of a string field */
} known_key_t;

#define STRING_KEY(field)                                                                          \
    #field, 0, offsetof(file_config_t, field), sizeof(((file_config_t *)0)->field)
#define INT_KEY(field) #field, 1, offsetof(file_config_t, field), 0

/* In the order they are checked, so the first problem reported is the first key's. */
static const known_key_t KNOWN_KEYS[] = {
    {STRING_KEY(input_host)},        {INT_KEY(input_port)},  {STRING_KEY(output_host)},
    {INT_KEY(output_port)},          {INT_KEY(status_port)}, {STRING_KEY(passphrase)},
    {STRING_KEY(output_passphrase)},
};
#define KNOWN_KEY_COUNT (sizeof KNOWN_KEYS / sizeof KNOWN_KEYS[0])
#define OUTPUT_PASSPHRASE_KEY (KNOWN_KEY_COUNT - 1) /* the only optional key */

static int syntax_error(char *err, size_t err_sz, const char *json, const char *at,
                        const char *what) {
    if (err && err_sz > 0) {
        snprintf(err, err_sz, "not a flat JSON object: %s at byte %zu", what, (size_t)(at - json));
    }
    return -1;
}

/* Keeps the first unknown key's name for the warning, cut to fit and with anything
 * that is not printable ASCII replaced, because it is written to the log. */
static void note_unknown_key(file_config_t *cfg, const char *name) {
    if (cfg->unknown_key_count++ > 0) return;
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof cfg->unknown_key; ++n) {
        unsigned char c = (unsigned char)name[n];
        cfg->unknown_key[n] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    cfg->unknown_key[n] = '\0';
}

/* Reads the object into cfg. seen[i] and usable[i] say, for each known key, that it
 * appeared and that its value has the right type and fits. A value of the wrong
 * type is not a syntax error: parse_config_json reports it by key name. */
static int scan_config(const char *json, file_config_t *cfg, int *seen, int *usable, char *err,
                       size_t err_sz) {
    /* A UTF-8 byte order mark, which some Windows editors put at the start of a file,
     * is not part of the object. */
    const char *p = skip_ws(strncmp(json, "\xEF\xBB\xBF", 3) == 0 ? json + 3 : json);
    if (*p != '{') return syntax_error(err, err_sz, json, p, "expected '{'");
    p = skip_ws(p + 1);

    int first = 1;
    for (;;) {
        if (first && *p == '}') {
            ++p;
            break;
        }
        if (*p != '"') return syntax_error(err, err_sz, json, p, "expected a key");
        const char *at = p;
        char name[64];
        str_result_t key_result = scan_string(&p, name, sizeof name);
        if (key_result == STR_SYNTAX)
            return syntax_error(err, err_sz, json, at, "unterminated key");

        int idx = -1;
        for (size_t i = 0; key_result == STR_OK && i < KNOWN_KEY_COUNT; ++i) {
            if (strcmp(name, KNOWN_KEYS[i].name) == 0) idx = (int)i;
        }
        p = skip_ws(p);
        if (*p != ':') return syntax_error(err, err_sz, json, p, "expected ':'");
        p = skip_ws(p + 1);

        if (idx < 0) {
            note_unknown_key(cfg, name);
        } else if (seen[idx]) {
            set_err(err, err_sz, "duplicate \"%s\"", KNOWN_KEYS[idx].name);
            return -1;
        } else {
            seen[idx] = 1;
        }
        char *field = idx >= 0 ? (char *)cfg + KNOWN_KEYS[idx].offset : NULL;
        int want_string = idx >= 0 && !KNOWN_KEYS[idx].is_int;

        if (*p == '{' || *p == '[') return syntax_error(err, err_sz, json, p, "nested value");
        if (*p == '"') {
            at = p;
            str_result_t value_result =
                scan_string(&p, want_string ? field : NULL, want_string ? KNOWN_KEYS[idx].size : 0);
            if (value_result == STR_SYNTAX)
                return syntax_error(err, err_sz, json, at, "unterminated string");
            if (want_string) usable[idx] = value_result == STR_OK;
        } else {
            const char *end = scan_token(p);
            if (end == p) return syntax_error(err, err_sz, json, p, "expected a value");
            if (idx >= 0) {
                if (KNOWN_KEYS[idx].is_int) usable[idx] = parse_int_token(p, end, (int *)field);
            } else if (!is_number_or_literal(p, end)) {
                return syntax_error(err, err_sz, json, p, "unexpected value");
            }
            p = end;
        }

        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            first = 0;
            continue;
        }
        if (*p == '}') {
            ++p;
            break;
        }
        return syntax_error(err, err_sz, json, p, "expected ',' or '}'");
    }

    p = skip_ws(p);
    if (*p) return syntax_error(err, err_sz, json, p, "unexpected text after the object");
    return 0;
}

typedef struct uri_writer {
    char *buf;
    size_t size;
    size_t used;
    int overflow;
} uri_writer_t;

static void uri_put(uri_writer_t *w, const char *s) {
    size_t n = strlen(s);
    if (w->overflow || w->used + n >= w->size) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->used, s, n + 1);
    w->used += n;
}

static void append_param(uri_writer_t *w, int *first, const char *key, const char *value) {
    uri_put(w, *first ? "?" : "&");
    uri_put(w, key);
    uri_put(w, "=");
    uri_put(w, value);
    *first = 0;
}

static void append_param_int(uri_writer_t *w, int *first, const char *key, int value) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%d", value);
    append_param(w, first, key, tmp);
}

static int uri_is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

static int url_encode_component(const char *in, char *out, size_t out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if (uri_is_unreserved(*p)) {
            if (used + 1 >= out_sz) return -1;
            out[used++] = (char)*p;
        } else {
            if (used + 3 >= out_sz) return -1;
            out[used++] = '%';
            out[used++] = hex[*p >> 4];
            out[used++] = hex[*p & 0x0f];
        }
    }

    if (used >= out_sz) return -1;
    out[used] = '\0';
    return 0;
}

int build_srt_uri(char *out, size_t out_sz, const char *host, int port, const char *passphrase,
                  int include_groupconnect) {
    if (!out || out_sz == 0) return -1;
    uri_writer_t w = {out, out_sz, 0, 0};
    out[0] = '\0';

    char head[320];
    snprintf(head, sizeof head, "srt://%s:%d", host, port);
    uri_put(&w, head);

    int first = 1;
    append_param(&w, &first, "mode", include_groupconnect ? "listener" : "caller");
    if (include_groupconnect) append_param_int(&w, &first, "groupconnect", 1);
    append_param(&w, &first, "transtype", "live");
    append_param_int(&w, &first, "latency", include_groupconnect ? 240 : 200);
    if (passphrase && passphrase[0]) {
        char encoded_passphrase[sizeof(((file_config_t *)0)->passphrase) * 3];
        if (url_encode_component(passphrase, encoded_passphrase, sizeof encoded_passphrase) != 0) {
            return -1;
        }
        append_param(&w, &first, "passphrase", encoded_passphrase);
        append_param_int(&w, &first, "pbkeylen", 16);
    }
    return w.overflow ? -1 : 0;
}

void redact_srt_uri(const char *uri, char *out, size_t out_sz) {
    static const char key[] = "passphrase=";
    static const char mask[] = "<redacted>";
    size_t used = 0;
    if (!out || out_sz == 0) return;

    for (size_t i = 0; uri[i];) {
        int at_param_start = i > 0 && (uri[i - 1] == '?' || uri[i - 1] == '&');
        if (at_param_start && strncmp(uri + i, key, sizeof key - 1) == 0) {
            const char *pieces[2] = {key, mask};
            for (int k = 0; k < 2; ++k) {
                for (const char *c = pieces[k]; *c; ++c) {
                    if (used + 1 < out_sz) out[used++] = *c;
                }
            }
            i += sizeof key - 1;
            while (uri[i] && uri[i] != '&')
                ++i;
            continue;
        }
        if (used + 1 < out_sz) out[used++] = uri[i];
        ++i;
    }
    out[used] = '\0';
}

int check_passphrase(const char *key, const char *passphrase, char *err, size_t err_sz) {
    size_t len = strlen(passphrase);
    if (len == 0) return 0;
    if (len >= PASSPHRASE_MIN_LEN && len <= PASSPHRASE_MAX_LEN) return 0;
    if (err && err_sz > 0) {
        snprintf(err, err_sz,
                 "\"%s\" must be %d to %d characters (got %zu); use \"\" to disable encryption",
                 key, PASSPHRASE_MIN_LEN, PASSPHRASE_MAX_LEN, len);
    }
    return -1;
}

static int port_in_range(int port) {
    return port > 0 && port <= 65535;
}

static int read_config(const char *json, file_config_t *cfg, char *err, size_t err_sz) {
    int seen[KNOWN_KEY_COUNT] = {0};
    int usable[KNOWN_KEY_COUNT] = {0};
    if (scan_config(json, cfg, seen, usable, err, err_sz) != 0) return -1;

    for (size_t i = 0; i < KNOWN_KEY_COUNT; ++i) {
        /* output_passphrase is optional. Present means the downstream has its own
         * passphrase, "" included; a key that is present but not a string is an
         * error, not "absent". */
        if (i == OUTPUT_PASSPHRASE_KEY && !seen[i]) continue;
        int in_range =
            KNOWN_KEYS[i].is_int ? port_in_range(*(int *)((char *)cfg + KNOWN_KEYS[i].offset)) : 1;
        if (!seen[i] || !usable[i] || !in_range) {
            set_err(err, err_sz, "missing or invalid \"%s\"", KNOWN_KEYS[i].name);
            return -1;
        }
    }

    if (seen[OUTPUT_PASSPHRASE_KEY]) {
        cfg->output_passphrase_explicit = 1;
    } else {
        snprintf(cfg->output_passphrase, sizeof cfg->output_passphrase, "%s", cfg->passphrase);
    }

    if (check_passphrase("passphrase", cfg->passphrase, err, err_sz) != 0) return -1;
    if (check_passphrase("output_passphrase", cfg->output_passphrase, err, err_sz) != 0) return -1;
    return 0;
}

int parse_config_json(const char *json, file_config_t *cfg, char *err, size_t err_sz) {
    memset(cfg, 0, sizeof *cfg);
    int rc = read_config(json, cfg, err, err_sz);
    if (rc != 0) memset(cfg, 0, sizeof *cfg); /* no half-read passphrase is left behind */
    return rc;
}

/* A config is a few hundred bytes. Refuse anything that is not a regular file
 * (a directory reports a size of LONG_MAX on some filesystems, /dev/zero never
 * ends) and anything over this limit, instead of trusting the size. */
#define MAX_CONFIG_BYTES (64 * 1024)

static int read_file(const char *path, char **buf_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_CONFIG_BYTES) {
        fclose(f);
        return -1;
    }
    size_t len = (size_t)st.st_size;
    char *buf = calloc(len + 1, 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[len] = '\0';
    *buf_out = buf;
    return 0;
}

int load_config_file(const char *path, file_config_t *cfg, char *err, size_t err_sz) {
    char *json = NULL;
    if (read_file(path, &json) != 0) {
        set_err(err, err_sz, "cannot read %s (not a regular file, or over 64 KiB)", path);
        return -1;
    }
    int rc = parse_config_json(json, cfg, err, err_sz);
    free(json);
    return rc;
}
