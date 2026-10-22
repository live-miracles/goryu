# Architecture

Goryu is one process of about 2,600 lines of C on top of libsrt, split into the
small modules listed [below](#modules). It has three kinds of thread.

## Threads

- **Main (accept loop).** Polls the listener with `srt_epoll_uwait`. Each
  accepted connection, whether the first leg of a new group or a later leg
  joining an existing group in the background, spawns a session thread tracked
  in a fixed-size session table (256 entries).
- **Session threads.** One per accepted stream. Each reads the deduplicated
  payload off the group socket with `srt_recvmsg2`, writes it downstream with
  `srt_sendmsg2`, and samples SRT statistics (below).
- **Status HTTP thread.** Serves the [status API](1-status-api.md) on a
  loopback-only port.

## Modules

| File | Role |
|---|---|
| `goryu.c` | Entry point: arguments and config, the listener, the accept loop, shutdown order |
| `session.c` | One session thread: streamid, [duplicate publishers](../3-operations/1-reliability.md#duplicate-publishers), downstream connect and [retry](../3-operations/1-reliability.md#downstream-retry), the read/send loop |
| `tracker.c` | The sockets of every live session, so one thread can close another's and the sampler never reads a closed socket |
| `stream_state.c` | The shared, mutex-protected telemetry per stream, plus the process-level `lastError` |
| `stats.c` | Reads libsrt's counters for a session into a sample that `stream_state.c` applies |
| `status_api.c`, `json_out.c` | The loopback HTTP thread and the JSON writers |
| `relay_config.c` | `goryu.json`, URI and query parsing, passphrase rules, URI redaction |
| `srt_opts.c` | Applies SRT options from a query and reports a refused one |
| `net.c`, `log.c`, `runtime.c`, `backoff.c` | Address handling; timestamped logging and the libsrt log cap; the run flag, signals and clock; the retry delays |

The tracker and the stream state use the same slot index for a session. Both are
guarded by their own mutex and are never held at the same time, except that the
stats sampler holds the tracker's while it calls libsrt.

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
second per session) reads libsrt's counters under a lock and writes them into
the shared state. It runs on the session thread itself, so the status thread
never touches libsrt. Group-level `srt_bstats` leaves several fields empty for a
bonded group, which is why the status API reports some values per leg only and
derives the input latency from the legs; see the
[field notes](1-status-api.md).

## Logging

libsrt's log level is set to errors only, and a process-wide handler caps what
is left at 20 lines per second. See
[Reliability](../3-operations/1-reliability.md#log-volume) for why. The
passphrase is never logged: the startup line shows the output URI with
`passphrase=<redacted>`, and a bad config reports the offending key and the
passphrase's length, not its value.
