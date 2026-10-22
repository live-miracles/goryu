#ifndef LOG_H
#define LOG_H

/* Prefixes the line with a local-time, millisecond timestamp and writes it to
 * stderr. Logs are private and unredacted, but never pass a passphrase: use
 * redact_srt_uri() for any URI. */
void relay_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Sets libsrt's log level to errors only and installs a handler that caps
 * libsrt's own output at 20 lines per second for the whole process. Call after
 * srt_startup(). See docs/3-operations/1-reliability.md#log-volume. */
void log_install_srt_cap(void);

#endif
