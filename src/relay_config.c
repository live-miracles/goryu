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

/* Finds "key": and returns the position of its value, or NULL. This is a text
 * search, not a JSON parser: the file is documented to be a flat object of six
 * keys (docs/2-usage/1-configuration.md). */
static const char *json_find_value(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    if (!p) return NULL;
    return skip_ws(p + 1);
}

/* Reads a string value. Only the escapes \" \\ and \/ are decoded; any other
 * escape is refused rather than silently turned into a different string (a
 * passphrase containing \n would otherwise be read as the letter n). */
static int json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    const char *p = json_find_value(json, key);
    if (!p || *p != '"') return 0;
    ++p;

    size_t len = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (*p != '"' && *p != '\\' && *p != '/') return 0;
        }
        if (len + 1 >= out_sz) return 0;
        out[len++] = *p++;
    }
    if (*p != '"') return 0;
    out[len] = '\0';
    return 1;
}

/* Reads an integer value: digits only (optional sign), no fraction or exponent,
 * and it must fit an int, so 4294977377 is refused instead of wrapping to 10081. */
static int json_get_int(const char *json, const char *key, int *out) {
    const char *p = json_find_value(json, key);
    if (!p) return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX) return 0;
    if (*end != '\0' && *end != ',' && *end != '}' && *end != ' ' && *end != '\n' && *end != '\r' &&
        *end != '\t')
        return 0;
    *out = (int)v;
    return 1;
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

int check_passphrase(const char *passphrase, char *err, size_t err_sz) {
    size_t len = strlen(passphrase);
    if (len == 0) return 0;
    if (len >= PASSPHRASE_MIN_LEN && len <= PASSPHRASE_MAX_LEN) return 0;
    if (err && err_sz > 0) {
        snprintf(err, err_sz,
                 "\"passphrase\" must be %d to %d characters (got %zu); use \"\" to disable "
                 "encryption",
                 PASSPHRASE_MIN_LEN, PASSPHRASE_MAX_LEN, len);
    }
    return -1;
}

static int port_in_range(int port) {
    return port > 0 && port <= 65535;
}

int parse_config_json(const char *json, file_config_t *cfg, char *err, size_t err_sz) {
    memset(cfg, 0, sizeof *cfg);

#define FAIL_KEY(key)                                                                              \
    do {                                                                                           \
        set_err(err, err_sz, "missing or invalid \"%s\"", key);                                    \
        return -1;                                                                                 \
    } while (0)

    if (!json_get_string(json, "input_host", cfg->input_host, sizeof cfg->input_host))
        FAIL_KEY("input_host");
    if (!json_get_int(json, "input_port", &cfg->input_port) || !port_in_range(cfg->input_port))
        FAIL_KEY("input_port");
    if (!json_get_string(json, "output_host", cfg->output_host, sizeof cfg->output_host))
        FAIL_KEY("output_host");
    if (!json_get_int(json, "output_port", &cfg->output_port) || !port_in_range(cfg->output_port))
        FAIL_KEY("output_port");
    if (!json_get_int(json, "status_port", &cfg->status_port) || !port_in_range(cfg->status_port))
        FAIL_KEY("status_port");
    if (!json_get_string(json, "passphrase", cfg->passphrase, sizeof cfg->passphrase))
        FAIL_KEY("passphrase");
#undef FAIL_KEY

    if (check_passphrase(cfg->passphrase, err, err_sz) != 0) return -1;
    return 0;
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
