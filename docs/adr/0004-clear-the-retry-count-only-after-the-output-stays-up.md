# Clear the downstream retry count only after the output has stayed up for 10 s

A downstream can accept the SRT handshake and then reject the stream, for example a publish hook refusing a stream key. Goryu retries on a 1, 2, 4, 8, 16 s schedule, and the count that drives it is cleared only after the output connection has stayed up for 10 s (`OUTPUT_HEALTHY_MS`).

## Considered options

- **Clear on a successful connect.** The failed send that follows always counted as the first failure, so the retry was 1 s forever. Before backoff applied to that path at all, it reconnected every 10 to 40 ms and drove steady memory growth in production.
- **Clear on the first successful send.** libsrt takes packets into its send buffer before the drop arrives, and at 4 Mbps a send succeeds in every cycle, so this behaved the same as the first option.

## Consequences

`retryFailures` in the status API can stay above 0 for up to 10 s after a downstream recovers. `tests/unit/test_output.c` pins the rule without a process: a connect that succeeds does not clear the count, the count clears after exactly `OUTPUT_HEALTHY_MS`, and the delays double up to 16 s. The `downstream_drops` scenario uses a downstream that closes 50 ms after accepting (`tests/e2e/closing_sink.c`) and a 4 Mbps publisher, because a quiet publisher would let a wrong fix pass.
