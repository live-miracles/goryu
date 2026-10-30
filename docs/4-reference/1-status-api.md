# Status API

Goryu serves a JSON snapshot of every active or recently active stream on
`127.0.0.1:<status_port>`. The port is loopback only and is not reachable from
other hosts. Any request returns the snapshot. The field names, their order and when each
is `null` are pinned by `tests/unit/test_status_json.c`, because Bunryu mirrors them:

```bash
curl http://127.0.0.1:8081/status
```

The status thread never calls libsrt. Session threads sample `srt_bstats()` and
`srt_group_data()` at most once per second and write the results into shared
state; the status thread only copies that state (see
[Architecture](2-architecture.md)).

The response is compact (no pretty-printing). Pipe it through `jq` or
`python3 -m json.tool` to read it by eye.

Top-level fields:

- `pid`, `startedAtMs`, `updatedAtMs`: process identity and snapshot timing
- `lastError`: the most recent process-level error, if any
- `activeStreamIds`: streamids with a currently connected input
- `streamStates`: one entry per known stream (see below); entries can briefly
  outlive `activeStreamIds` while a session is tearing down

Each `streamStates[]` entry:

- `streamId`, `inputActive`, `outputConnected`
- `retryFailures`: failed downstream attempts (connect or send) since the downstream
  last stayed connected for 10 s; it can be above 0 while `outputConnected` is `true`
- `forwardedPackets`, `forwardedBytes`, `lastPacketAt`, `lastInputPacketAt`:
  Goryu-tracked forwarding progress
- `input`: everything SRT reports about the (possibly bonded) input — see below
- `output`: everything SRT reports about the single downstream connection
  (output is never bonded) — see below
- `lastErrorAt`, `lastError`: most recent per-stream error, if any

`input` fields:

- `bonded`: `true` when the encoder connected as a bonded socket group,
  `false` for an ordinary single-link SRT connection. Decided once when the
  session is accepted. A bonded group with only one leg connected is still
  `bonded: true`.
- `peer`: `{ "ip", "port" }`, the encoder's address as seen by Goryu, for a
  plain input; `null` for a bonded input, where each leg carries its own
  address in `legs[]` (the connection that first opened the group may no
  longer be a member later, so no single peer is reported). A plain input has
  an empty `legs` array, so `peer` is where its address shows up.
- `recvPacketsTotal`, `recvUniquePacketsTotal`, and `recvDropTotal`: group-level
  counters from SRT's bonded group socket. `recvPacketsTotal`,
  `recvLossTotal`, and `retransTotal` can be `null`: SRT does not provide a
  deduplicated group-level loss or receive-retransmission counter, and it does
  not always populate `recvPacketsTotal` for a group. `recvUniquePacketsTotal`
  is the reliable deduplicated total for group-level throughput. Use the
  per-leg values below for loss and retransmission telemetry; Goryu does
  not sum them because that would double-count packets recovered on another
  leg.
- `rttMs`: RTT reported for the input group socket as a whole
- `latencyMs`: negotiated SRT buffering latency for the input, i.e. the
  actual value SRT settled on after the handshake, not just what was
  requested. For a non-bonded input this comes straight from `srt_bstats`
  (`msRcvTsbPdDelay`, the same underlying value `SRTO_LATENCY` reports). A
  bonded group socket's own `srt_bstats` never fills that field in, so for a
  real bonded input this is derived as the max `latencyMs` across `legs[]`
  instead (see below) — no extra libsrt call needed either way.
- `bandwidthMbps`, `recvRateMbps`, `belatedTotal`, `belatedAvgMs`,
  `undecryptTotal`, `reorderDistance`, `rcvBufMs`: estimated capacity, actual
  receive rate, packets dropped for arriving past their delivery deadline
  (count + average lateness), decryption failures, reordering, and
  receive-buffer occupancy (ms) — all read straight from the same
  `srt_bstats` call as the fields above. **Only populated for a non-bonded
  input** (`null` otherwise): like `latencyMs`, a bonded group's own
  `srt_bstats` never fills these in, and there was no reasonable per-leg
  aggregation for most of them (unlike latency, where "max across legs" has
  a clear meaning) — check `legs[]` for the real per-leg numbers on a bonded
  stream instead.
- `legs`: one entry per individual bonded connection currently in the input
  group, read directly from `srt_group_data()` +
  `srt_bstats()` on each member socket — this is real per-leg telemetry from
  libsrt, not something Goryu derives or combines:
  - `ip`, `port`: the leg's peer (encoder-side) address, as seen by Goryu
  - `state`: `pending` | `idle` | `running`. Every leg of a broadcast group is
    `running`; the standby leg of a main/backup group is `idle` (it carries nothing,
    `recvRateMbps` is about 0) until the main leg fails and it becomes `running`. A
    failed leg is not reported as
    `broken`: Goryu leaves broken members out of `legs[]`, and when a link was
    unplugged during testing, libsrt removed the dead leg from the group within
    the one-second sampling interval anyway. A leg that goes down simply
    disappears from the list, so detect it by comparing `legs[]` with the legs
    you have seen before. Goryu does this itself and logs each change, so the
    journal tells you which link dropped and when without polling:
    `Bonded leg joined streamid=... leg=<ip>:<port> legs=2` and
    `Bonded leg left streamid=... leg=<ip>:<port> legs=1` (`legs` is the number
    of legs after the change). A leg that reconnects from a new port is a left and a
    joined. Changes are noticed on the one-second sampling, so a leg that is gone and
    back within a second is not seen.
  - `rttMs`, `recvPacketsTotal`, `recvUniquePacketsTotal`, `recvLossTotal`,
    `recvDropTotal`, `retransTotal`: stats for that leg only
  - `latencyMs`: the negotiated latency for that individual leg's own socket
    (`msRcvTsbPdDelay` from the member's own `srt_bstats`, not the group), so
    if an encoder ever negotiates a different value per leg it's visible here
  - `bandwidthMbps`, `recvRateMbps`, `belatedTotal`, `belatedAvgMs`,
    `undecryptTotal`, `reorderDistance`, `rcvBufMs`: the same set of extra
    stats as `input.*` above, but for that leg's own socket specifically —
    reliably populated per leg even though the group-level `input.*`
    versions are not

`output` fields:

- `sentPacketsTotal`, `sendLossTotal`, `sendDropTotal`, `retransTotal`:
  send-side counters for the downstream connection
- `rttMs`: RTT for the downstream connection
- `latencyMs`: negotiated latency on the downstream `srt://` connection
  Goryu makes to the output (from `srt_bstats`' `msRcvTsbPdDelay`, same value
  as `input.latencyMs`); Goryu's default `build_srt_uri()` requests
  `latency=200` here vs. `latency=240` on the bonded input listener
- `bandwidthMbps`, `sendRateMbps`, `undecryptTotal`, `sndBufMs`: capacity,
  actual send rate, decryption failures, and send-buffer occupancy (ms) —
  the output socket is never a group, so these are populated after the first
  successful output stats sample while connected

Example response for a stream bonded over two legs (reformatted here for
readability — the actual response is one compact line):

```json
{
  "pid": 12345,
  "startedAtMs": 1783569708552,
  "updatedAtMs": 1783569763437,
  "lastError": null,
  "activeStreamIds": ["camera1/main"],
  "streamStates": [
    {
      "streamId": "camera1/main",
      "inputActive": true,
      "outputConnected": true,
      "retryFailures": 0,
      "forwardedPackets": 28,
      "forwardedBytes": 36848,
      "lastPacketAt": 1783569742530,
      "lastInputPacketAt": 1783569742529,
      "input": {
        "bonded": true,
        "peer": null,
        "recvPacketsTotal": null,
        "recvUniquePacketsTotal": 22,
        "recvLossTotal": 0,
        "recvDropTotal": 0,
        "retransTotal": 0,
        "rttMs": null,
        "latencyMs": 240,
        "bandwidthMbps": null,
        "recvRateMbps": null,
        "belatedTotal": null,
        "belatedAvgMs": null,
        "undecryptTotal": null,
        "reorderDistance": null,
        "rcvBufMs": null,
        "legs": [
          {
            "ip": "10.0.1.20",
            "port": 51072,
            "state": "running",
            "rttMs": 24.6,
            "latencyMs": 240,
            "recvPacketsTotal": 26,
            "recvUniquePacketsTotal": 26,
            "recvLossTotal": 0,
            "recvDropTotal": 0,
            "retransTotal": 0,
            "bandwidthMbps": 12.3,
            "recvRateMbps": 0.47,
            "belatedTotal": 0,
            "belatedAvgMs": 0.0,
            "undecryptTotal": 0,
            "reorderDistance": 0,
            "rcvBufMs": 221
          },
          {
            "ip": "10.0.2.31",
            "port": 39356,
            "state": "running",
            "rttMs": 31.2,
            "latencyMs": 240,
            "recvPacketsTotal": 25,
            "recvUniquePacketsTotal": 25,
            "recvLossTotal": 1,
            "recvDropTotal": 0,
            "retransTotal": 0,
            "bandwidthMbps": 9.8,
            "recvRateMbps": 0.46,
            "belatedTotal": 2,
            "belatedAvgMs": 6.4,
            "undecryptTotal": 0,
            "reorderDistance": 1,
            "rcvBufMs": 235
          }
        ]
      },
      "output": {
        "sentPacketsTotal": 21,
        "sendLossTotal": 0,
        "sendDropTotal": 0,
        "retransTotal": 0,
        "rttMs": 0.043,
        "latencyMs": 200,
        "bandwidthMbps": 11.8,
        "sendRateMbps": 0.42,
        "undecryptTotal": 0,
        "sndBufMs": 1
      },
      "lastErrorAt": 0,
      "lastError": null
    }
  ]
}
```
