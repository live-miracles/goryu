# A socket is untracked before it is closed, and only the thread that untracked it closes it

Duplicate takeover and shutdown close sockets that belong to another session's thread, and the stats sampler asks libsrt about those same sockets. So every close goes through the tracker: the closer removes the socket from the tracker while holding its lock (`tracker_take_*`, which succeeds only if the socket is still the tracked one), releases the lock, and only then calls `srt_close`. Exactly one thread therefore closes each socket. The sampler queries libsrt only while holding the tracker lock and only for a socket it has just confirmed is still tracked, so it never reads a socket that has been closed or is being closed.

## Consequences

Session code must not call `srt_close` on a tracked socket directly: it takes the socket first, as `close_output` does for the output. A session ends its hold on its slot with `tracker_release`, which takes the input, closes it if it got it, and only then unregisters the slot; a rejected session, a finished one and a failed `pthread_create` all use it. The calls before a session is registered (allocation failure, session table full) are not tracked, so they close directly.

It pairs with the sampler holding the tracker lock across its libsrt calls (see [ADR 0002](0002-status-thread-never-calls-libsrt.md)).
