# Reliability

Goryu sits in front of a live ingest path, so it is built to keep running
through bad networks, a downstream that is down, and misbehaving encoders. Every
mechanism below is bounded.

## Downstream retry

If the downstream SRT server is unreachable, or rejects the publish, the session
retries in the background with backoff: 1, 2, 4, 8, then 16 seconds, staying at
16 s thereafter. The encoder's input connection is kept open while it retries.
The connect attempt itself times out after 3 s.

The same backoff applies when the connection succeeds but the first send fails.
That is what a downstream looks like when it accepts the SRT handshake and then
rejects the stream afterwards, for example a publish hook refusing an
unauthorized stream key. Without backoff that case reconnected every 10 to 40 ms
indefinitely in production.

The retry count is cleared only after the downstream connection has stayed up for
10 s. Neither a successful connect nor packets sent show the downstream is healthy:
libsrt takes packets into its send buffer before the downstream's rejection
arrives, so clearing the count on either made that case retry after 1 s, forever,
instead of backing off. It also means `retryFailures` can stay above 0 for up to
10 s after a downstream recovers.

When libsrt reports a reject reason, it is included in that stream's
`lastError`, and the retry count is `retryFailures` in the
[status API](../4-reference/1-status-api.md).

## Duplicate publishers

A second connection with a streamid that is already active waits up to 5 s:

- if the existing session's input has been silent for 5 s, it is treated as
  stale, force-closed (only that session), and the new publisher takes over
  (`Duplicate publisher takeover for stale streamid=...`);
- otherwise the new connection is rejected
  (`Duplicate publisher rejected for active streamid=...`).

This lets an encoder that crashed and reconnected recover quickly, without
letting a second encoder hijack a stream that is still flowing.

Each session wakes from its receive once a second (`SRTO_RCVTIMEO`, 1000 ms) even
when the encoder sends nothing, then re-checks whether it was closed. Up to v3.2.1
it blocked until data arrived, and that made takeover impossible for a **bonded**
encoder: libsrt defers `srt_close()` on a group while another thread is receiving
on it, so the takeover was logged and the stale group was "closed", yet its session
kept running and the new publisher was rejected 5 s later, for as long as the stale
legs stayed connected (reproduced on v3.2.1 with a bonded caller that stays
connected and sends nothing). A plain socket was not affected. The wake-up also
refreshes the stream's counters, so a silent stream still shows legs joining and
leaving in the status API.

## Log volume

libsrt's own log level is set to errors only at startup, so its per-packet
diagnostics (for example receive-queue pressure warnings under sustained loss)
are never formatted. As a backstop, a custom handler caps libsrt's log output
at 20 lines per second for the whole process, regardless of level, and prints
one `N more log line(s) suppressed` summary when it trims.

This exists because a receive-queue backlog once produced about 35 log lines
per second for 16 minutes, which loaded a small host enough to starve
`systemd-journald` and take down its SSH and networking. The cap makes that
failure structurally impossible whatever triggers the next burst.

## Refused SRT options

Every SRT option Goryu applies is checked, and a refusal stops the relay at
startup. An earlier version ignored libsrt's refusal of a too-short passphrase, so
a configured but invalid passphrase left the whole relay, input and output,
running without encryption and accepting callers that sent none.

## Status endpoint

The status HTTP server runs on its own thread and binds to `127.0.0.1` only.
It never calls libsrt: it copies state that the session threads already
collected, so polling it adds no work to the forwarding path. Do not expose the
port; it reports encoder addresses, and it has no authentication: loopback is the
only access control ([why](../adr/0013-status-api-is-loopback-only-and-unauthenticated.md)).

One thread serves it, one client at a time, and a client gets 2 s to send its
request and to take the reply. A client that connects and says nothing is dropped
with `Status client sent no request within 2 s, dropped` (at most 3 such lines per
minute, then a count). Before that limit a single idle local connection stopped the
endpoint, and `SIGTERM` did not stop the relay within 13 s, because shutdown joins
the status thread.

## What is not logged

Bad-passphrase callers are rejected inside libsrt's handshake, so Goryu emits no
log line for them and has no peer address to log. Depending on libsrt's runtime
logging, the journal may still show internal SRT errors for failed handshakes.
Treat those as diagnostic noise, not a stable security log format, and do not
build a fail2ban rule on them. For events where password guessing is a real
concern, restrict who can reach the input UDP port at the edge or firewall.
