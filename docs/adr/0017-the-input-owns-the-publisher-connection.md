# The input owns the publisher connection, and reaches libsrt through a transport

A session's connection to the publisher is one module, `input.c`, the counterpart of the output (ADR 0016). `input_open` settles the streamid (the one the publisher set on the connection, else the one in the downstream URI) and logs the accept; `input_connected` is the loop's condition; `input_recv` returns one of four outcomes (data, idle, closed, failed) and, for a failure, tells it once in the log, the process `lastError` and the stream's `lastError`. `input.c` does not call libsrt: applying the receive timeout, reading the streamid, the group test, the receive and the sorting of libsrt's error codes are an `input_transport_t`, and `srt_input.c` is the libsrt one. `session.c` is left with the order a session lives in.

Before, all of this was inline in `session_main`, about 90 lines that also acquired the stream, drove the output and counted the forwarded bytes. Nothing but the end-to-end run exercised it, so the streamid fallback and the rule that a lost connection is a normal end while any other receive error is reported had no check that runs in seconds.

## Considered options

- **Leave it in `session.c`.** The cost is the one ADR 0016 recorded for the output: a rule that lives in the order of a long function is only seen by scenarios that take minutes.
- **Have `input.c` sort libsrt's error codes, with the codes in the fake `srt.h`.** The test would then pin the classification, but against its own copy of the constants, so it could not notice a copy that differs from libsrt's. Both sides would agree and both could be wrong. The sorting is four lines in `srt_input.c` and its contract is written in `input.h`.
- **One module for both connections.** The input has no retry state and no reconnect, and the output has no streamid to settle, so the shared interface would be the sum of the two.

## Consequences

The classification of libsrt's codes (which error is idle, which is a normal end) stays in `srt_input.c`, which is untested outside the end-to-end run; what the session does with each outcome is in `input.c` and `session.c`, and what is told about a failure is covered by `tests/unit/test_input.c`. The input never closes its socket: the tracker owns it and the session ends with `tracker_release` (ADR 0011). The receive timeout, and the reason for it (a takeover cannot wake a blocked receive on a group, ADR 0005), now sit in `input.h` beside the code that applies it.

A receive that returns 0 bytes used to skip the stream's counters sample and is now an idle outcome that takes it, which is at most once a second either way. The log lines and `lastError` texts are the ones the session wrote before.
