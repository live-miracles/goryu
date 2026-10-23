# Alternatives

Goryu does one thing: terminate an SRT bonding group (libsrt socket groups) and
forward ordinary SRT. Whether it is the right tool depends on what your encoder
sends and what your server accepts.

## If your server can accept bonded SRT itself

Then you do not need a relay. [Nimble Streamer](https://softvelum.com/2024/07/deliver-receive-srt-bonded-streams/)
documents receiving bonded SRT input (broadcast and main/backup groups). SRS has
an open proposal to accept bonded publishers
([ossrs/srs#4771](https://github.com/ossrs/srs/issues/4771)); until it ships,
SRS rejects a group. MediaMTX and datarhei Core were tested with a bonded group
and rejected it too (see [Downstream servers](../2-usage/3-downstream-servers.md)).

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

## Other tools

- **libsrt's own apps.** `srt-live-transmit` takes one input and one output and
  does not document group support. `srt-test-live` is libsrt's bonding test
  application, not something to run as a relay.
- **Your own libsrt program.** Goryu is about 2,800 lines of C; if you need other
  behaviour, the [architecture](../4-reference/2-architecture.md) is short enough
  to fork.
- **Plain SRT relays** such as [voc/srtrelay](https://github.com/voc/srtrelay)
  fan one SRT stream out to many viewers; its README does not mention bonding.
