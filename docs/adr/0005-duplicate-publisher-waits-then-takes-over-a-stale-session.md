# A duplicate publisher waits 5 s, then takes over a stale session or is rejected

A second connection on a streamid that is already active waits up to 5 s. If the existing session's input has been silent for 5 s it is stale, is force-closed, and the newcomer takes over; otherwise the newcomer is rejected. This lets an encoder that crashed and reconnected recover quickly without letting a second encoder hijack a stream that is still flowing.

## Consequences

Each session wakes from its receive once a second (`SRTO_RCVTIMEO`, 1000 ms) even when nothing arrives, and then checks whether it was closed. libsrt defers `srt_close()` on a group while another thread is receiving on it, so without the wake-up a stale bonded session was logged as closed but kept running, and the new publisher was rejected 5 s later. Up to v3.2.1 a bonded takeover could never succeed; a plain socket was not affected.
