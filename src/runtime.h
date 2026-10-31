#ifndef RUNTIME_H
#define RUNTIME_H

/* Process-wide run flag, signal handling and the clocks. */

/* Milliseconds on a clock that only moves forward (CLOCK_MONOTONIC), from an arbitrary
 * start. Every timer, backoff, staleness and rate window reads this, so a step of the
 * system clock (NTP, a VM resuming) can neither stall a retry for the length of the step
 * nor make healthy streams look stale. It means nothing outside this process. */
long long now_ms(void);

/* Milliseconds since the Unix epoch (CLOCK_REALTIME), for the timestamps the status API
 * shows. It can jump, so nothing is decided from it. */
long long wall_ms(void);

/* Sleeps for about `ms`; a signal may end it early. Loops that wait on a clock sleep
 * through this so a test can advance a fake clock instead of waiting. */
void sleep_ms(int ms);

/* True until SIGINT/SIGTERM (or relay_request_stop) asks the process to end.
 * Every loop in the relay polls this, so shutdown is cooperative. */
int relay_running(void);
void relay_request_stop(void);

/* SIGINT and SIGTERM clear the run flag; SIGPIPE is ignored so a closed peer
 * surfaces as an error return instead of killing the process. */
int relay_install_signal_handlers(void);

#endif
