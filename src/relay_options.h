#ifndef RELAY_OPTIONS_H
#define RELAY_OPTIONS_H

#include <stddef.h>

#define RELAY_DEFAULT_STATUS_PORT 8081

/* What the relay runs with, from either form of the command line: a config file, or an
 * input and an output SRT URI. */
typedef struct relay_options {
    char in_host[256];
    int in_port;
    char in_query[1024]; /* SRT socket options of the listener */
    char out_host[256];
    int out_port;
    char out_query[1024];        /* SRT socket options of the downstream connection */
    char out_uri_redacted[2048]; /* the downstream as it may be logged: no passphrase */
    int status_port;
    /* Things the relay does about, but does not refuse over. */
    int unknown_key_count;       /* keys in the config file that Goryu does not know */
    char unknown_key[64];        /* the first of them */
    int status_port_env_ignored; /* the status port variable was set to something that is not a port
                                  */
} relay_options_t;

/* Reads the command line (argc is 2 for a config file, 3 for two URIs) without
 * logging or touching libsrt. status_port_env is the value of SRT_BONDING_STATUS_PORT
 * (NULL if unset): the only way to move the status port in the two-URI form, and it
 * overrides the config file's status_port too.
 *
 * Returns 0, or -1 with a one-line reason in err. The reason never contains a
 * passphrase: it names the key, and a URI is only ever written redacted. */
int parse_relay_options(int argc, char *argv[], const char *status_port_env, relay_options_t *opts,
                        char *err, size_t err_sz);

#endif
