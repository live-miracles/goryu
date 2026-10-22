#ifndef RELAY_CONFIG_H
#define RELAY_CONFIG_H

#include <stddef.h>

/* libsrt accepts a passphrase of 10 to 79 bytes (SRTO_PASSPHRASE). Anything
 * else makes srt_setsockflag fail, which used to leave the relay running
 * unencrypted, so the config is refused instead. An empty passphrase means
 * "no encryption". */
#define PASSPHRASE_MIN_LEN 10
#define PASSPHRASE_MAX_LEN 79

typedef struct file_config {
    char input_host[256];
    int input_port;
    char output_host[256];
    int output_port;
    int status_port;
    char passphrase[256];
    /* The passphrase for the downstream connection. It is the optional key
     * "output_passphrase" in goryu.json; without that key it is a copy of
     * passphrase, so one passphrase covers both sides. An explicit "" means the
     * downstream is not encrypted. */
    char output_passphrase[256];
    int output_passphrase_explicit;
} file_config_t;

int parse_uri(const char *uri, char *scheme, size_t scheme_sz, char *host, size_t host_sz,
              int *port, char *query, size_t query_sz);
int get_param(const char *query, const char *key, char *val, size_t val_sz);
int build_srt_uri(char *out, size_t out_sz, const char *host, int port, const char *passphrase,
                  int include_groupconnect);

/* Copies uri to out with the value of any "passphrase" parameter replaced by
 * "<redacted>", so a URI can be logged. Truncates to out_sz like snprintf. */
void redact_srt_uri(const char *uri, char *out, size_t out_sz);

/* Returns 0 for an empty passphrase or one of 10 to 79 bytes. Otherwise -1,
 * with a message in err that names the key (for example "passphrase" or
 * "output_passphrase") and states the length, never the passphrase. */
int check_passphrase(const char *key, const char *passphrase, char *err, size_t err_sz);

/* Parses the flat JSON object that goryu.json holds: six required keys and the
 * optional output_passphrase. Returns 0, or
 * -1 with a one-line reason in err (when err is not NULL). The reason names the
 * offending key, never a value, so it is safe to log. */
int parse_config_json(const char *json, file_config_t *cfg, char *err, size_t err_sz);
int load_config_file(const char *path, file_config_t *cfg, char *err, size_t err_sz);

#endif
