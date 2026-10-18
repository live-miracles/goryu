# Configuration

## Command line

```text
goryu <config.json>
goryu <srt-input-uri> <srt-output-uri>
goryu --version
```

The config-file form is the one to use. The two-URI form takes SRT socket
options directly from each URI's query string (`transtype`, `groupconnect`,
`latency`, `rcvlatency`, `passphrase`, `pbkeylen`) and listens for status on
port `8081`.

## goryu.json

```json
{
  "input_host": "0.0.0.0",
  "input_port": 10081,
  "output_host": "127.0.0.1",
  "output_port": 10080,
  "status_port": 8081,
  "passphrase": "secret-value"
}
```

| Field | Meaning |
|---|---|
| `input_host` | Interface the SRT listener binds to (an IPv4 address; `0.0.0.0` for all) |
| `input_port` | UDP port encoders connect to, bonded or plain |
| `output_host` | Downstream SRT server host |
| `output_port` | Downstream SRT server port |
| `status_port` | TCP port of the [status API](../4-reference/1-status-api.md), bound to `127.0.0.1` only |
| `passphrase` | SRT passphrase for the input and the output; `""` disables encryption |

All six fields are required, and the three ports must be 1 to 65535. A missing
field or a port out of range makes Goryu exit at startup with `Bad config file`.
The parser reads a flat JSON object by key, so keep the file to these six keys
and no comments.

In this mode the input listener runs `transtype=live` with `latency=240` ms and
group connections enabled, and the downstream connection is a caller with
`transtype=live` and `latency=200` ms. The negotiated values are reported in the
status API.

## Streamid routing

Goryu does not interpret the `streamid`. It accepts any streamid an encoder
sends, keys that stream's state on it, and copies it verbatim onto the
downstream connection. One listener can therefore serve many logical streams,
each landing on whatever the downstream does with that streamid (for example, a
publish authorization hook that checks it as a stream key).

Only one publisher per streamid is active at a time; see
[duplicate publishers](../3-operations/1-reliability.md#duplicate-publishers).

## Passphrase

When `passphrase` is set, Goryu sets `SRTO_PASSPHRASE` on the input listener and
on the output connection, and leaves validation to libsrt's normal encrypted
handshake. Encoders must use the same passphrase, and so must the downstream
server. To run unencrypted end to end, use `""`.

A caller with a wrong passphrase is rejected by libsrt during the handshake,
before Goryu is handed a connection, so Goryu never learns the caller's address
for those attempts. See
[what is not logged](../3-operations/1-reliability.md#what-is-not-logged).
