# Sending bonded SRT

Goryu is the receiving half. Something still has to send a bonded stream, and
today that is the harder half to find: most encoders and tools open an ordinary
single-link SRT socket.

## What the sender must do

An SRT sender that Goryu can merge is a **caller** that:

1. creates a libsrt socket group, of type **broadcast** or **backup** (main/backup;
   see [below](#broadcast-and-mainbackup)), which is the sender's choice, not
   something to configure on Goryu;
2. adds one member per network link, each bound to that link's local address;
3. sets the **same `streamid`** on every member. Goryu copies it onto the
   downstream connection, so it must be the one your
   [server expects](3-downstream-servers.md).

Goryu is tested with both group types, on loopback legs. A plain single-link caller
works on the same port and needs nothing special.

## Broadcast and main/backup

libsrt 1.5 offers two ways to run a group (a third, load balancing, is not in this
version). The receiver, Goryu, treats them alike: it merges whatever arrives and
forwards one stream. They differ in what the sender puts on each link.

| | Broadcast | Main/backup (`backup`) |
|---|---|---|
| What each link carries | Every link carries the whole stream | Only the main link carries it; the others sit idle with keepalives |
| Which link is main | None, all are equal | The link with the highest weight |
| If a link fails | Nothing to do: the others already have every packet | The sender activates a standby link once the main one is judged unstable |
| Uplink cost | Every link pays for the full bitrate (two links, twice the data) | One link's worth, which matters on metered cellular data |
| Weak spot | Bandwidth | The switchover: packets sent while the main link was failing but not yet declared unstable can be late or lost, so give it latency headroom |

In Goryu's status API the difference shows in each leg's `state`: every leg of a
broadcast group is `running`, while the standby of a main/backup group is `idle`
(and `recvRateMbps` is about 0) until it takes over.

What was checked, on loopback legs with the test caller (`--type backup --weights 10,5`)
and a 4 Mbps stream: the main leg was `running` at about 4.4 Mbps and the backup `idle`
at 0.000. When the main leg was closed, the backup became `running`, `forwardedBytes`
kept growing at about 4 Mbps, and the downstream did not reconnect (`retryFailures` 0).
A main/backup group needs real traffic before libsrt settles on a main leg: with a
0.12 Mbps test stream both legs stayed active. Not measured: how many packets a real
switchover loses on a link that degrades before it dies, because loopback legs never do.

## What can send one

| Sender | Status |
|---|---|
| AJA BRIDGE LIVE | Sent a bonded group over two paths across the internet to a deployed Goryu v3.3.0 relay (about 41 to 44 ms RTT, 22.6 Mbps per leg), forwarded to SRS. Both legs saw packet loss and retransmissions and the stream carried on; the longest session ran 21 h 51 min. Unplugging one of its two LAN cables for about 45 s dropped the group to one leg, forwarding stayed at about 22 Mbps, the downstream never disconnected, and the leg rejoined by itself. This is the setup we run our live events on every day. AJA lists SRT bonding over the public internet from firmware v1.16 ([release notes](https://www.aja.com/pdf/2024/AJA_BRIDGE-LIVE_ReleaseNotes_v1.16.pdf)), which needs its Annual Maintenance Option. |
| `tests/e2e/bonded_caller.c` in this repository | Used by every bonded test in `scripts/test-e2e.sh`. A test tool, not a product; see below. |
| Your own libsrt program | `srt_create_group(SRT_GTYPE_BROADCAST)`, then `srt_connect_group()` with one `srt_prepare_endpoint()` per link (`SRT_GTYPE_BACKUP` and a weight per link for main/backup); the whole test caller is 196 lines, mostly option parsing. Haivision's [bonding quick start](https://github.com/Haivision/srt/blob/master/docs/features/bonding-quick-start.md) is the reference. |
| Nimble Streamer | Documents a bonded SRT push output (`srt-bonding-enabled`, `srt-group-nodes`) in [its bonding guide](https://softvelum.com/2024/07/deliver-receive-srt-bonded-streams/). Not tested with Goryu. |
| FFmpeg | Cannot: its `srt://` protocol opens one socket. The end-to-end tests had to write their own caller for this reason. |
| `srt-live-transmit`, `srt-test-live` (libsrt) | Not usable as a sender. `srt-live-transmit` takes one input and one output; `srt-test-live` from libsrt 1.5.6 failed to create a sending group in our test (`group option setter: this option (21) is socket- or link-specific`). |

Hardware and mobile encoders that advertise SRT bonding vary in what they send
(libsrt socket groups, or a vendor protocol that needs the vendor's own
receiver). Check that yours sends a libsrt socket group (broadcast or main/backup) before relying on it.

## Try it with the test caller

This needs no second uplink. The caller binds one leg to `127.0.0.1` and one to
`127.0.0.2`, which Goryu sees as two peers. It reads MPEG-TS on stdin.

Build it against a libsrt 1.5 built with bonding. The repository's Docker image
has one, so after `bash scripts/build-local.sh`:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/w -w /w goryu-dev:v1.5.6 \
  sh -c 'gcc -O2 -o objs/bonded-caller tests/e2e/bonded_caller.c $(pkg-config --cflags --libs srt) -lpthread'
```

Then send a test pattern to a running Goryu:

```bash
ffmpeg -re -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 25 -c:a aac -f mpegts - |
LD_LIBRARY_PATH=objs/lib objs/bonded-caller --port 10081 --streamid camera1/main \
  --locals 127.0.0.1,127.0.0.2
```

`--drop-leg 1 --drop-after 9` closes the second leg after nine seconds, which is
the quickest way to see the point of bonding. `--type backup --weights 10,5` sends a
main/backup group instead (use a real bitrate, a few Mbps, so libsrt settles on the main
leg), and `--flap-leg 1 --flap-interval 1.2 --flap-toggles 18` makes a leg go up and down. Poll the status API during the run:

```bash
curl -s http://127.0.0.1:8081/status | jq '.streamStates[0] |
  {streamId, forwardedBytes, legs: [.input.legs[] | {ip, state, rttMs}]}'
```

With the leg dropped, `legs` goes from two entries to one, `forwardedBytes` keeps
growing, and `outputConnected` stays `true`: the downstream never notices. In a
run on loopback, `forwardedBytes` read 472,444, 708,008 and 933,044 at 8, 12
and 16 seconds, with the leg dropped at 9.

Loopback legs do not lose packets or add jitter. This shows leg handling and
deduplication, not how bonding behaves on a failing real link.

## Real links

Bind each member to the address of its own interface (`--locals <ip1>,<ip2>`).
On Linux a socket bound to a source address still leaves by whatever route the
destination matches, so two uplinks on different subnets usually need
source-based routing (`ip rule` per source address) for each leg to leave through
its own link. Check with `tcpdump -i <iface> udp port 10081` on each interface
that both carry traffic.
