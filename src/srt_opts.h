#ifndef SRT_OPTS_H
#define SRT_OPTS_H

#include <stddef.h>

#include <srt/srt.h>

/* Applies the SRT options named in a URI query (transtype, groupconnect,
 * latency, rcvlatency, passphrase, pbkeylen) to sock. Returns 0, or -1 with the
 * rejected option and libsrt's reason in err. The reason never includes an
 * option's value, so a bad passphrase is not echoed. */
int apply_srt_opts(SRTSOCKET sock, const char *query, char *err, size_t err_sz);

/* One srt_setsockflag whose refusal is reported: 0, or -1 with "libsrt rejected option
 * <name>: <libsrt's reason>" in err. `name` is the option's name, never its value. */
int set_srt_flag(SRTSOCKET sock, SRT_SOCKOPT opt, const char *name, const void *val, int len,
                 char *err, size_t err_sz);

#endif
