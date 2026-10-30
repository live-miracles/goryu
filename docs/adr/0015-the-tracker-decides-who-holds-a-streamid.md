# The tracker decides who holds a streamid, and one release ends a session

A session's place in the relay is its slot, its sockets and the streamid it holds, and the tracker owns all three. `tracker_acquire_stream` runs the claim and the duplicate wait and takeover (ADR 0005), and `tracker_release` ends the stream, closes the input and frees the slot, in that order. The stream state no longer answers "who holds streamid X": the tracker records a streamid only when the claim succeeds, and tells the stream state to begin and end the stream in that slot.

Before, the answer lived in two tables. The stream state decided the claim, the tracker kept its own copy of the streamid from the moment the session started, and `session.c` had to call `remove_stream_state` before `tracker_release` (a comment, not an interface) or the next session on the slot was refused. A takeover closed every session with that streamid in the tracker, including duplicates that were still waiting and had never claimed it. And the takeover decision, the most delicate timing in the relay, could only be exercised by end-to-end scenarios that sleep 6 to 15 s each.

## Considered options

- **Keep the claim in the stream state and look the holder up there.** The check and the close would then span two locks, so a holder that released in between could leave a newer session on the same slot closed by mistake.
- **A new module above both tables.** The slot, the sockets and the streamid would still be spread over three modules, with the ordering rule in the new one.

## Consequences

The tracker keeps a copy of the held streamid beside the stream state's (which the status snapshot needs), and takes the stream state's lock while holding its own, never the reverse; a change that makes the stream state call the tracker would invert that. A takeover closes only the holder it judged stale, in the same step under the lock; a second duplicate that is still waiting meets the new holder and is rejected the normal way. A session ends only through `tracker_release`, so a new way to end cannot forget one of the two steps.

The wait and the takeover run against `now_ms`, `sleep_ms`, `relay_running` and `srt_close`. The unit tests supply a fake clock whose sleep advances it and a recording `srt_close`, with `tests/unit/fake/srt/srt.h` in place of libsrt's header, so `tests/unit/test_tracker.c` covers the wait, a stale holder that ends, one that never does, shutdown during the wait and each socket closing exactly once.
