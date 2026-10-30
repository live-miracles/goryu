# The status thread never calls libsrt

Session threads sample libsrt's counters into shared state and the status thread only copies that state, so polling the status API cannot slow forwarding or deadlock on a libsrt lock. The cost is that a stream's numbers are up to a second old, and the sampler must hold the tracker's lock while it calls libsrt so a socket cannot be closed between the check and the read.

A session thread wakes at least once a second even when the publisher is silent, so a silent stream's numbers still refresh (see [ADR 0005](0005-duplicate-publisher-waits-then-takes-over-a-stale-session.md)).
