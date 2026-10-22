#ifndef BACKOFF_H
#define BACKOFF_H

/* Delay before the next downstream connect attempt after `failures` consecutive
 * failures: 1, 2, 4, 8, then 16 seconds, staying at 16 s. Values of 1 or less
 * give the first delay. */
int retry_delay_ms(int failures);

#endif
