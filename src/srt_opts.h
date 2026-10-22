#ifndef SRT_OPTS_H
#define SRT_OPTS_H

#include <stddef.h>

#include <srt/srt.h>

/* Applies the SRT options named in a URI query (transtype, groupconnect,
 * latency, rcvlatency, passphrase, pbkeylen) to sock. Returns 0, or -1 with the
 * rejected option and libsrt's reason in err. The reason never includes an
 * option's value, so a bad passphrase is not echoed. */
int apply_srt_opts(SRTSOCKET sock, const char *query, char *err, size_t err_sz);

#endif
