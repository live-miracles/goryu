#ifndef STATUS_API_H
#define STATUS_API_H

#include <pthread.h>

/* Starts the status HTTP thread. It binds 127.0.0.1:port (loopback only, never
 * another interface) and answers every request with the JSON snapshot described
 * in docs/4-reference/1-status-api.md. The thread reads only the stream-state
 * snapshot; it never calls libsrt. It exits when relay_running() turns false.
 * Returns 0, or -1 if the thread could not be created. A bind failure is logged
 * and recorded as lastError by the thread, and the relay keeps forwarding. */
int status_server_start(int port, long long started_at_ms, pthread_t *tid);

#endif
