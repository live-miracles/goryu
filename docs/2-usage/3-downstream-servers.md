# Downstream servers

Goryu forwards plain SRT, so any server that accepts an SRT publish can sit
behind it. The server never sees a bonded group, which is the point: servers
built on a libsrt without bonding, or on a pure-Go SRT stack, reject the group
outright.

## What was tested

A bonded broadcast group (two legs, `tests/e2e/bonded_caller.c`) was sent to
each server's SRT port, first directly and then through Goryu, and the stream was
read back from the server over SRT. Legs were simulated on loopback (127.0.0.1 and
127.0.0.2), not real links. Separately, an AJA BRIDGE LIVE encoder sending two
legs across the internet was forwarded to SRS by a deployed relay (see [Sending bonded SRT](2-sending-bonded-srt.md#what-can-send-one)).

| Server (version tested) | Bonded group sent directly | Through Goryu |
|---|---|---|
| [SRS](https://github.com/ossrs/srs) 7.0.189 | rejected | accepted, stream read back |
| [MediaMTX](https://github.com/bluenviron/mediamtx) v1.21.2 | rejected | accepted, stream read back |
| [datarhei Core](https://github.com/datarhei/core) 16.16.0 (the engine of [Restreamer](https://github.com/datarhei/restreamer)) | rejected | accepted, stream read back |

Directly, the sender's libsrt logs `HS EXT: agent is a group member, but the
listener did not respond with group ID. Rejecting.`: the server's listener does
not accept groups. Through Goryu the status API showed `bonded: true` with two
legs and `outputConnected: true`.

Restreamer's web interface was not run; the test used its Core with the SRT
server enabled.

## Configure the streamid

Goryu copies the encoder's `streamid` unchanged, so **the encoder must send the
streamid the server expects for a publish**. Goryu's `output_host` and
`output_port` point at the server's SRT port.

| Server | Publish streamid | Goryu `output_port` | Notes |
|---|---|---|---|
| SRS | `#!::r=live/stream,m=publish` | `10080` | Enable `srt_server` and `srt { enabled on; }` for the vhost. |
| MediaMTX | `publish:stream` | `8890` | With authentication: `publish:stream:user:pass`. |
| datarhei Core / Restreamer | `stream,mode:publish` | `6000` | Needs `CORE_SRT_ENABLE=true` (`srt.enable`). A configured token goes on the end: `,token:<token>`. |

Read the stream back to check it arrived (the playback streamid differs):

```bash
ffprobe 'srt://127.0.0.1:10080?streamid=#!::r=live/stream,m=request'   # SRS
ffprobe 'srt://127.0.0.1:8890?streamid=read:stream'                    # MediaMTX
ffprobe 'srt://127.0.0.1:6000?streamid=stream,mode:request'            # Core
```

## Example: MediaMTX

```bash
docker run -d --network host bluenviron/mediamtx
```

```json
{
  "input_host": "0.0.0.0",
  "input_port": 10081,
  "output_host": "127.0.0.1",
  "output_port": 8890,
  "status_port": 8081,
  "passphrase": ""
}
```

Run Goryu with that config, point a bonded sender at `srt://<host>:10081` with
`streamid=publish:stream` (see [Sending bonded SRT](2-sending-bonded-srt.md)),
and play `srt://<host>:8890?streamid=read:stream`. SRS and Core are the same
with the port and streamid from the table.

## Things that differ from a direct publish

- **The passphrase carries over.** `passphrase` is applied to the input and, unless
  `output_passphrase` is set, to the output, so the server must use the same one (or
  none). Set `output_passphrase` to give the downstream its own. See
  [Passphrase](1-configuration.md#passphrase).

- **Latency starts at** 240 ms on the input and 200 ms downstream; see
  [Configuration](1-configuration.md#goryujson). libsrt negotiates the larger of the
  two sides' settings, so a server configured for more raises the downstream
  value. `output.latencyMs` in the status API is what was negotiated.
- **A rejected publish is retried**, with the reason in the stream's `lastError`,
  so a wrong streamid or token shows in the [status API](../4-reference/1-status-api.md)
  and the log rather than as a silent drop. See
  [Downstream retry](../3-operations/1-reliability.md#downstream-retry).
- **Same host:** `input_port` must differ from the server's SRT port.
