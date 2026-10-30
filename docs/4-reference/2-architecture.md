# Architecture

Goryu is one process of about 2,800 lines of C on top of libsrt, split into the
small modules listed [below](#modules). It has three kinds of thread.

## Threads

- **Main (accept loop).** Polls the listener with `srt_epoll_uwait`. Each
  accepted connection, whether the first leg of a new group or a later leg
  joining an existing group in the background, spawns a session thread tracked
  in a fixed-size session table (256 entries).
- **Session threads.** One per accepted stream. Each reads the deduplicated
  payload off the group socket with `srt_recvmsg2`, writes it downstream with
  `srt_sendmsg2`, and samples SRT statistics (below).
- **Status HTTP thread.** Serves the [status API](1-status-api.md), one client at a time with a 2 s limit for each, on a
  loopback-only port.

## Modules

| File | Role |
|---|---|
| `goryu.c` | Entry point: the listener, the accept loop, shutdown order |
| `relay_options.c` | The command line (a config file or two URIs) and the status port variable, turned into one options struct. Pure |
| `session.c` | One session thread: streamid, [duplicate publishers](../3-operations/1-reliability.md#duplicate-publishers), the read/send loop |
| `output.c`, `srt_output.c` | A session's connection to the downstream: connect, [retry](../3-operations/1-reliability.md#downstream-retry) after every failed connect or send, the healthy period, and keeping the tracker and the stream's telemetry in step. `output.c` runs without libsrt through a small transport; `srt_output.c` is the libsrt one ([ADR 0016](../adr/0016-the-output-owns-the-downstream-connection.md)) |
| `tracker.c` | A session's place in the relay: its slot, its sockets (so one thread can close another's and the sampler never reads a closed socket), the streamid it holds, the duplicate-publisher wait and takeover, and the one release every session ends with |
| `stream_state.c` | The shared, mutex-protected telemetry per stream, at the session's slot (begun and ended by the tracker); the process-level `lastError` |
| `stats.c` | The libsrt side of telemetry: reads a session's counters into a plain sample and applies no rule |
| `telemetry.c` | The rules that turn a sample into what the status API reports: which values are valid, totals only moving forward, a bonded group's latency, legs joining and leaving. Pure |
| `status_json.c`, `json_out.c` | The status JSON written from a snapshot it is handed (pure), and the JSON writers |
| `status_api.c` | The loopback HTTP thread around them |
| `relay_config.c` | `goryu.json` (a strict scanner for a flat object), URI and query parsing, passphrase rules, URI redaction |
| `srt_opts.c` | Applies SRT options from a query and reports a refused one |
| `timed_send.c` | Sends a buffer on a socket with one total deadline; the status thread uses it so a client that stops reading cannot hold it |
| `ratelimit.c` | A fixed window that lets a few events through and counts the rest (used for the leg join/leave log lines, the status client lines and libsrt's own log cap) |
| `net.c`, `log.c`, `runtime.c`, `backoff.c` | Address handling; timestamped logging and the libsrt log cap; the run flag, signals and clock; the retry delays and one session's retry state |

A session has one slot, and it indexes both tables: the tracker's (its sockets) and
the stream state's (its telemetry). A stream is only ever stored in its own session's
slot, and a session with an empty streamid holds no stream, so the stream-state calls
ignore it. The tracker alone decides who holds a streamid, and begins and ends the
stream in the stream state ([ADR 0015](../adr/0015-the-tracker-decides-who-holds-a-streamid.md));
`tracker_release` ends the stream before it frees the slot, so a session cannot end
without doing both. The two tables are guarded by their own mutex. The tracker takes the
stream state's while holding its own and never the other way round; the stats sampler
holds the tracker's while it calls libsrt but takes no stream-state lock under it.

## Listener

A single SRT socket is bound with `SRTO_GROUPCONNECT=1` (`mode=listener`,
`transtype=live`, `latency=240` ms). That lets an encoder connect several
redundant legs as one socket group. libsrt merges and deduplicates packets
across legs before Goryu sees them, so Goryu reads one MPEG-TS stream per
group, not per leg. A caller with one ordinary SRT socket is accepted by the
same listener and handled by the same session code, without legs.

## Streamid routing and dedup

The caller's `streamid` is read off the accepted socket and used as the key of a
shared per-stream state slot. It is also copied onto the outgoing connection,
so one listener multiplexes many streams to distinct downstream streamids. A
second publisher on an already-active streamid is either rejected or takes over
a stale session; see
[duplicate publishers](../3-operations/1-reliability.md#duplicate-publishers).

## Forwarding and retry

Each session reads from the group socket and writes to its own downstream
connection. If the downstream is unreachable or rejects the publish, the session
retries with backoff in the background without dropping the encoder's input
connection; see [Reliability](../3-operations/1-reliability.md#downstream-retry).

## Stats collection

```text
status HTTP thread
        |
        v
copies in-memory session state
        |
        +--> never calls libsrt directly
        |
        v
JSON response

session threads
        |
        +--> sample srt_bstats() / srt_group_data() at most once per second
        |
        v
shared session state
```

A per-session sampler (`update_stream_srt_counters`, rate-limited to once per
second per session) reads libsrt's counters under a lock into a plain sample.
`telemetry_apply` then turns the sample into the stream's state under the stream
state's lock, and the status thread copies that state and writes the JSON from the
copy. The sampler runs on the session thread itself, so the status thread
never touches libsrt, and the rules in the middle need no libsrt to be tested. Group-level `srt_bstats` leaves several fields empty for a
bonded group, which is why the status API reports some values per leg only and
derives the input latency from the legs; see the
[field notes](1-status-api.md).

## Logging

libsrt's log level is set to errors only, and a process-wide handler caps what
is left at 20 lines per second. See
[Reliability](../3-operations/1-reliability.md#log-volume) for why. A bonded leg
joining or leaving is logged by the stats sampler (see the
[status API](1-status-api.md)), at most 12 lines per stream per 30 s; a leg that
flaps faster than that is summarised once the window ends
(`Bonded leg events suppressed streamid=... count=N in the last 30 s`), so a bad link
cannot flood the journal the way the libsrt messages once did. The
passphrase is never logged: the startup line shows the output URI with
`passphrase=<redacted>`, and a bad config reports the offending key and the
passphrase's length, not its value.
