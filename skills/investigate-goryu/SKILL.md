---
name: investigate-goryu
description: Inspect a deployed Goryu relay read-only to explain an incident, a warning or a user report - the loopback status API, journald, service state and deployed version. Use when asked what happened to a stream, why an encoder or downstream misbehaved, or to check a relay's health. NOT for restarting, redeploying or changing its config.
---

# Investigate a deployed Goryu (read-only)

A relay may be carrying a live stream. Look, don't touch.

## Rules

- **Read-only.** No `systemctl restart/stop`, no config edits, no `kill`, no
  upgrades. Restarting drops every active stream (the config is only read at
  startup), so it is a separate request the user must make, and never during a live
  event unless they say so.
- **Don't write down secrets.** The status API and the journal contain streamids
  (usually stream keys), encoder IPs and ports, and downstream addresses. The
  config contains the SRT passphrase: don't `cat` `goryu.json`; query a field only if
  the question needs it, and never repeat the passphrase.
- Keep output small: filter with `jq`, `grep`, `tail`, `cut -c1-300` instead of
  dumping whole responses or journals into the conversation.
- The status port is loopback-only, so run these on the relay's own host (over the
  user's SSH). If you can't get there, ask the user to run the commands and paste
  the output. For a Bunryu deployment, host access and layout are in that repo's
  `investigate-prod` skill; Goryu runs there as `goryu.service`.

## Where things are

| Thing | Where |
|---|---|
| Status API | `http://127.0.0.1:<status_port>/status` (8081 by default) |
| Process logs | stdout/stderr, so journald under the unit that runs it (`goryu.service` in the install docs and in Bunryu) |
| Config | the path given on the command line, six keys, may hold the passphrase |
| Version | `LD_LIBRARY_PATH=<dir>/lib <dir>/bin/goryu --version`; compare with the latest tag |

## Recipes

```bash
systemctl is-active goryu.service
sudo journalctl -u goryu.service --since '-1 hour' --no-pager | tail -100 | cut -c1-300

# one line per stream
curl -s http://127.0.0.1:8081/status | jq -c '.streamStates[] |
  {streamId, inputActive, outputConnected, retryFailures, lastError,
   bonded: .input.bonded, legs: (.input.legs | length)}'

# per-leg view of one bonded stream (a failed leg is absent, not "broken")
curl -s http://127.0.0.1:8081/status | jq -c --arg s 'camera1/main' \
  '.streamStates[] | select(.streamId==$s) | .input.legs[] |
   {ip, state, rttMs, recvLossTotal, retransTotal, recvRateMbps, rcvBufMs}'

# is data moving? two samples a few seconds apart
for i in 1 2; do curl -s http://127.0.0.1:8081/status |
  jq -c '[.streamStates[] | {streamId, forwardedBytes}]'; sleep 5; done
```

The process-level `lastError`, `pid` and `startedAtMs` show whether it restarted
recently (a new `startedAtMs` or pid versus what the user remembers).

## Reading the evidence

Log lines the relay writes (`key=value`, local time of the host):
`Accepted plain SRT source` / bonded accept, `Output publish retry ... failures=N
retry_ms=N`, `srt_connect failed ... error=...`, `Duplicate publisher rejected` /
`takeover`, `Connection closed ... reason=... duration_ms=... bytes_forwarded=...`,
and `N more log line(s) suppressed` when the libsrt log cap trimmed output.

| Symptom | Look at |
|---|---|
| Encoder connected, nothing downstream | `outputConnected` false, `retryFailures`, stream `lastError` (it carries libsrt's reject reason); the downstream's own logs/hook |
| Reconnect loop every few seconds | `Connection closed ... reason=` and `duration_ms`; a downstream that accepts then rejects shows up as backoff, not a tight loop |
| New encoder can't take over a stream | `Duplicate publisher rejected`: the old session was still sending within the last 5 s |
| One leg missing | `input.legs` shorter than before; the leg's link is down or libsrt dropped it within a second |
| High loss on a leg | that leg's `recvLossTotal`, `retransTotal`, `rttMs`, `belatedTotal`; group-level loss fields are null by design |
| Bad passphrase | no Goryu line exists for it ([why](../../docs/3-operations/1-reliability.md#what-is-not-logged)); check the encoder and edge firewall |
| Host sluggish with Goryu logging heavily | `suppressed` lines and the log rate; see [Log volume](../../docs/3-operations/1-reliability.md#log-volume) |

Field meanings and null rules: [Status API](../../docs/4-reference/1-status-api.md).

## Method

1. Pin the time window, the streamid and the host's local time zone.
2. Check the deployed version; the relay often lags `master`, so a behavior in the
   logs may be fixed or changed in the repo.
3. Read the transitions around the window first (accept, retry, close), then the
   status counters.
4. Separate what the logs and status show from what you infer, and say what you
   could not see. A status snapshot is the present only; counters before a restart
   are gone, and the journal is the only history.
5. If the finding should be kept, write it without secrets: replace stream keys
   with `<redacted-streamid>`, passphrases with `<redacted-srt-passphrase>`, and
   encoder addresses with `<redacted-ip>`. Keep non-secret parts (a `keyNN_` prefix,
   a bare downstream hostname). Before staging, grep the new text for `streamid`,
   `passphrase`, `srt://`, and IPv4/IPv6 patterns and justify each hit.
