#ifndef BACKOFF_H
#define BACKOFF_H

/* Delay before the next downstream connect attempt after `failures` consecutive
 * failures: 1, 2, 4, 8, then 16 seconds, staying at 16 s. Values of 1 or less
 * give the first delay. */
int retry_delay_ms(int failures);

/* How long the downstream connection must stay up before the failure count is
 * cleared. A downstream that accepts the handshake and then rejects the stream (a
 * publish hook refusing the key) drops the connection within tens of milliseconds,
 * after libsrt has already accepted a few packets into its send buffer, so neither
 * a successful connect nor a successful send shows the downstream is healthy.
 * Clearing the count on either made every such cycle retry after 1 s, forever.
 * The value is well above the 10 to 40 ms between reconnects seen in production. */
#define OUTPUT_HEALTHY_MS 10000

/* The retry state of one session's downstream connection. Every operation takes the
 * current time in milliseconds, so it needs no clock and no libsrt. Not locked: it
 * belongs to the session thread. */
typedef struct retry_state {
    int failures;                 /* failed attempts since the connection last stayed up */
    long long next_attempt_at_ms; /* earliest time of the next connect attempt */
    long long up_at_ms;           /* when the connection came up; 0 when down or healthy */
} retry_state_t;

void retry_init(retry_state_t *r);

/* A connect or a send failed. Returns the delay before the next attempt and sets
 * next_attempt_at_ms. A connection that had stayed up for OUTPUT_HEALTHY_MS counts
 * as healthy first, so this failure is the first of a new run. */
int retry_failed(retry_state_t *r, long long now);

/* A connect succeeded. */
void retry_connected(retry_state_t *r, long long now);

/* Clears the failure count once the connection has stayed up for OUTPUT_HEALTHY_MS.
 * Returns 1 when it does so (once per connection), 0 otherwise. */
int retry_check_healthy(retry_state_t *r, long long now);

/* 1 when a new connect attempt may start. */
int retry_due(const retry_state_t *r, long long now);

#endif
