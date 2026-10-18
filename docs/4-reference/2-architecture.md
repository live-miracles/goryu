# Architecture

Goryu is one process of about 2,000 lines of C (`src/goryu.c` and a small config
parser in `src/relay_config.c`) on top of libsrt. It has three kinds of thread.

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
[Reliability](../3-operations/1-reliability.md#log-volume) for why.
