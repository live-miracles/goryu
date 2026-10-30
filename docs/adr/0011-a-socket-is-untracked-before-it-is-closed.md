# A socket is untracked before it is closed, and only the thread that untracked it closes it

Duplicate takeover and shutdown close sockets that belong to another session's thread, and the stats sampler asks libsrt about those same sockets. So every close goes through the tracker: the closer removes the socket from the tracker while holding its lock (`tracker_take_*`, which succeeds only if the socket is still the tracked one), releases the lock, and only then calls `srt_close`. Exactly one thread therefore closes each socket. The sampler queries libsrt only while holding the tracker lock and only for a socket it has just confirmed is still tracked, so it never reads a socket that has been closed or is being closed.

## Consequences

Session code must not call `srt_close` on a tracked socket directly: it takes the socket first, as `session_main` does for its input and `close_output` does for its output. The calls before a session is registered (allocation failure, session table full) are not tracked, so they close directly. The one tracked exception is the `pthread_create` failure path in `session_spawn`, which closes the input after `tracker_unregister`; a shutdown at that moment could close it twice.

It pairs with the sampler holding the tracker lock across its libsrt calls (see [ADR 0002](0002-status-thread-never-calls-libsrt.md)).
