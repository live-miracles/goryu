#include "srt_opts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relay_config.h"

/* Every srt_setsockflag result is checked. A refused option used to be ignored,
 * which is how a passphrase libsrt rejected (it must be 10 to 79 bytes) left the
 * relay running without encryption. */
static int set_flag(SRTSOCKET sock, SRT_SOCKOPT opt, const char *name, const void *val, int len,
                    char *err, size_t err_sz) {
    if (srt_setsockflag(sock, opt, val, len) != SRT_ERROR) return 0;
    if (err && err_sz > 0) {
        snprintf(err, err_sz, "libsrt rejected option %s: %s", name, srt_getlasterror_str());
    }
    return -1;
}

int apply_srt_opts(SRTSOCKET sock, const char *query, char *err, size_t err_sz) {
    char val[512];
    if (err && err_sz > 0) err[0] = '\0';

    if (get_param(query, "transtype", val, sizeof val)) {
        SRT_TRANSTYPE tt = strcmp(val, "live") == 0 ? SRTT_LIVE : SRTT_FILE;
        if (set_flag(sock, SRTO_TRANSTYPE, "transtype", &tt, sizeof tt, err, err_sz) != 0)
            return -1;
    }
    if (get_param(query, "groupconnect", val, sizeof val)) {
        int v = atoi(val);
        if (set_flag(sock, SRTO_GROUPCONNECT, "groupconnect", &v, sizeof v, err, err_sz) != 0) {
            return -1;
        }
    }
    if (get_param(query, "latency", val, sizeof val)) {
        int v = atoi(val);
        if (set_flag(sock, SRTO_LATENCY, "latency", &v, sizeof v, err, err_sz) != 0) return -1;
    }
    if (get_param(query, "rcvlatency", val, sizeof val)) {
        int v = atoi(val);
        if (set_flag(sock, SRTO_RCVLATENCY, "rcvlatency", &v, sizeof v, err, err_sz) != 0) {
            return -1;
        }
    }
    if (get_param(query, "passphrase", val, sizeof val)) {
        if (set_flag(sock, SRTO_PASSPHRASE, "passphrase", val, (int)strlen(val), err, err_sz) !=
            0) {
            return -1;
        }
    }
    if (get_param(query, "pbkeylen", val, sizeof val)) {
        int v = atoi(val);
        if (set_flag(sock, SRTO_PBKEYLEN, "pbkeylen", &v, sizeof v, err, err_sz) != 0) return -1;
    }
    return 0;
}
