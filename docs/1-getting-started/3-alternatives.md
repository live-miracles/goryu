# Alternatives

Goryu does one thing: terminate an SRT bonding group (libsrt socket groups) and
forward ordinary SRT. Whether it is the right tool depends on what your encoder
sends and what your server accepts.

| Your encoder sends | Your server | Use |
|---|---|---|
| A libsrt socket group (broadcast or main/backup) | Accepts bonded SRT itself (Nimble Streamer) | No relay needed |
| A libsrt socket group | SRS, MediaMTX, datarhei Core or any plain SRT server | Goryu |
| SRTLA | Any plain SRT server | `srtla_rec`, not Goryu |
| A vendor bonding protocol | The vendor's receiver | Not Goryu |

## If your server can accept bonded SRT itself

Then you do not need a relay. [Nimble Streamer](https://softvelum.com/2024/07/deliver-receive-srt-bonded-streams/)
documents receiving bonded SRT input (broadcast and main/backup groups). SRS has
an open proposal to accept bonded publishers
([ossrs/srs#4771](https://github.com/ossrs/srs/issues/4771)); until it ships,
SRS rejects a group. MediaMTX and datarhei Core were tested with a bonded group
and rejected it too (see [Downstream servers](../2-usage/3-downstream-servers.md)).

## srt-live-transmit

`srt-live-transmit` is libsrt's own sample app and the first thing most people try
as a relay: an SRT listener in, an SRT caller out. It accepts a group when the
listener sets `groupconnect=1`, but it did not work as a relay for us, which is why
Goryu was written.

```bash
srt-live-transmit "srt://:9000?mode=listener&groupconnect=1" "srt://127.0.0.1:9001?mode=caller"
```

Test setup: libsrt built with `--enable-bonding --enable-apps`, a broadcast group
from `tests/e2e/bonded_caller.c` (legs on 127.0.0.1 and 127.0.0.2), about 9 s of
800 kb/s MPEG-TS, and a second `srt-live-transmit` as the downstream.

| Input | Result |
|---|---|
| Plain SRT, with or without `groupconnect=1` | Works: about 987 KB reaches the downstream |
| Bonded group, one leg or two, libsrt 1.5.6, 1.5.7 and master (`9f23902`) | 10,528 bytes arrive, then the process aborts (exit 134) with `corrupted size vs. prev_size` |
| The same group through Goryu | Both legs accepted, about 1.25 MB reaches the downstream |

The result was the same on every run and on all three libsrt versions. The cause is in
the app, not in libsrt's bonding: `SrtSource::Read` (`apps/transmitmedia.cpp`) passes an
uninitialized `SRT_MSGCTRL` to `srt_recvmsg2`, and for a group libsrt writes the group's
member data through the garbage `grpdata` pointer. Valgrind reports the invalid write;
plain sockets never touch that field, which is why they work. Initializing the struct
(`SRT_MSGCTRL ctrl = srt_msgctrl_default;`) was enough for the same test to deliver about
1.2 MB with no abort. It is reported upstream as
[Haivision/srt#3418](https://github.com/Haivision/srt/issues/3418); until a release has
the fix, `srt-live-transmit` from a stock build is not usable here.

A patched build is not a drop-in replacement for Goryu. With two legs, the log shows the
second leg refused with `Listener's backlog exceeded` (the app listens with a backlog of
1), and we did not check how many legs the patched relay ends up with. It is also one
input and one output per process. This is a loopback test with one sender, not a survey
of every build or option.

## SRTLA

[SRTLA](https://github.com/BELABOX/srtla) (from BELABOX, with a more recently active
fork, [irlserver/srtla](https://github.com/irlserver/srtla)) is a different design:
a proxy layer that spreads one SRT connection over several links, built for
bonding cellular modems in mobile streaming. The sender runs an SRTLA sender and
the receiver runs `srtla_rec` in front of a plain SRT server.

It is not interchangeable with Goryu. SRTLA is its own protocol, so a sender that
speaks SRTLA cannot talk to Goryu, and an encoder that sends a libsrt socket group
cannot talk to `srtla_rec`. Pick by what your encoder implements. With native SRT
bonding in broadcast mode every link carries the whole stream, which gives
seamless protection against a link failing; SRTLA's selling point is also
combining the capacity of several links.

## Vendor bonding

Some bonded-cellular products use their own protocol and their own receiver, not
libsrt socket groups; Haivision's bonded cellular path, for one, uses its SST
protocol into StreamHub. Those senders do not work with Goryu. SRT Mini Server's
[SRT Bonding Agent](https://srtminiserver.com/product_sba/) bonds the output of
any encoder on Windows; its page does not say how, so it has not been tried with
Goryu.

## Build your own

- **A libsrt program.** Goryu is about 2,800 lines of C; if you need other
  behaviour, the [architecture](../4-reference/2-architecture.md) is short enough
  to fork.
- **A Go library.** [zsiec/srtgo](https://pkg.go.dev/github.com/zsiec/srtgo) is a
  pure-Go SRT implementation with bonding modes and a listener option for groups. It
  is a library, not a relay, and it has not been tried with Goryu.
- **Plain SRT relays** such as [voc/srtrelay](https://github.com/voc/srtrelay)
  fan one SRT stream out to many viewers; its README does not mention bonding.
- **`srt-test-live`** is libsrt's bonding test application. As a sender in our test
  it failed to create a group (see [Sending bonded SRT](../2-usage/2-sending-bonded-srt.md)).
