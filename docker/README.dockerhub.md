# Goryu

A small SRT bonding relay for Linux. An encoder sends one stream over several network
links as a bonded SRT group; Goryu accepts the group on one port, merges the legs into a
single deduplicated stream, and forwards ordinary SRT to your server under the same
`streamid`. SRS, MediaMTX and Restreamer's engine reject a bonded group on their own;
Goryu sits in front of them.

## Run

Create `goryu.json` (`output_port` is your server's SRT port, for example `8890` for
MediaMTX or `10080` for SRS):

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

```bash
docker run -d --name goryu --network host --restart unless-stopped \
  -v "$PWD/goryu.json:/etc/goryu/goryu.json:ro" nandisha0/goryu:3.4
curl http://127.0.0.1:8081/status
```

Host networking is needed because the status API listens on `127.0.0.1` only. The config
must be readable by uid 10001 (`chmod 644`). Open the input port for UDP.

## Tags

`X.Y.Z` is an exact release, `X.Y` follows the newest patch of a minor version, and
`latest` the newest release. Pin an exact version for anything long-lived. linux/amd64
only, like the release. Each image installs the matching GitHub release and checks its
checksum.

## More

Source, documentation, the list of servers tested and how to send a bonded stream:
<https://github.com/live-miracles/goryu>
