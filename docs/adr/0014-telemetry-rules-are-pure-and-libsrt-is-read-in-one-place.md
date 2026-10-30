# Telemetry rules are a pure module, and libsrt is read in one place

`stats.c` copies libsrt's counters into a plain sample and applies no rule. `telemetry.c` decides what the status API reports from that sample: which values are valid (the `*_valid` flags that become `null`), that totals only move forward, a bonded group's latency from its legs, and which legs joined or left. `status_json.c` writes the JSON from a snapshot it is handed. None of the three includes libsrt, so the unit tests link them without it; the CI unit job has no libsrt headers.

Before, whether a value was reportable was decided in `stats.c`, again in `stream_state_apply_stats`, and a third time as null rules in the HTTP writer, and only the end-to-end jq checks covered any of it. The rules had already drifted once: the belated-average validity had to be repeated in the writer and went stale when the extra stats became invalid (ba6044e).

## Considered options

- **Keep the rules inside `stream_state_apply_stats`, under the lock.** They cannot be unit-tested there, because `stream_state.c` logs through `log.c`, which installs libsrt's log handler.
- **More end-to-end scenarios.** Each costs minutes, and a bonded input with real loss or a dead leg is only partly reproducible on loopback.

## Consequences

A new reported field is a counter in `srt_counters_t` and its copy in `stats.c`, a rule in `telemetry.c`, a field in `relay_stream_state_t`, and a line in `status_json.c`, with a test at the rule and at the JSON. The shape of the JSON is pinned by an exact string in `tests/unit/test_status_json.c`, so changing it means editing that test, which is the moment to tell Bunryu. `stats.c` stays untested outside the end-to-end run: it is the thin libsrt adapter.
