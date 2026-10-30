# The output owns the downstream connection, and reaches libsrt through a transport

A session's connection to the downstream is one module, `output.c`: it connects, backs off after every failed connect or send, clears the failure count after the healthy period (ADR 0004), and keeps the tracker and the stream's telemetry in step. The session calls `output_service` from its loop, `output_send` for each packet and `output_close` once at the end. `output.c` does not call libsrt: the socket, its options, the connect (with the reject reason) and the send are an `output_transport_t`, and `srt_output.c` is the libsrt one.

Before, the same connection was kept in four places in `session.c` (the socket, the tracker's output socket, the stream's `output_connected` and `retryFailures`, and the retry state), changed together by hand at nine call sites. Two rules had no more than a comment behind them: the socket must be the tracker's before it is configured and connected, or a takeover or shutdown during the blocking connect cannot close it (ADR 0011); and a successful connect must not clear the failure count (ADR 0004). The second is the rule ADR 0004 records, and the retry state's own unit tests cannot see it, because it lives in the order the session calls the retry state.

## Considered options

- **Leave the connection in `session.c` and test it end to end.** The only scenario that fails when the count is cleared too early is `downstream_drops`, which needs a downstream that accepts and drops and a 4 Mbps publisher, and takes minutes.
- **Link libsrt stubs into the unit tests instead of a transport.** `output.c` would call about ten libsrt functions and constants, and the test's copy of that API would drift from libsrt's. The transport is three calls whose contract is in `output.h`.

## Consequences

The transport is a seam with two adapters, libsrt in production and a scripted one in `tests/unit/test_output.c`, which runs the real tracker, stream state and backoff against a fake clock: the 1, 2, 4, 8, 16 s schedule, a downstream that accepts and then drops, the healthy period, and an output closed by a takeover during its connect. A new downstream behavior goes in `output.c` with a test there; only what needs libsrt's error codes goes in `srt_output.c`, which stays untested outside the end-to-end run.

`output_close` is final: an output never reconnects after it. The log lines and `lastError` texts are the ones the session wrote before, except that the name of the send call comes from the transport (`send_op`).
