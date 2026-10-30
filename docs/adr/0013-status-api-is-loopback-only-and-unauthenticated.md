# The status API is loopback-only and unauthenticated, and each client gets 2 s

The status API binds `127.0.0.1` with no authentication, and its address is not configurable (only the port is). Any connection gets the whole snapshot, which holds streamids (often stream keys), each encoder's address and the downstream's reject reasons, so being reachable only from the host is the access control. Reaching it from another machine is deliberately not supported; use an SSH tunnel, or a reverse proxy that adds its own authentication.

One thread serves it, one client at a time, and that thread is joined at shutdown. A client therefore gets 2 s to send its request and 2 s to take the reply. Measured on 2026-10-30 before the limit: one local connection that sent nothing stopped the endpoint, and `SIGTERM` did not stop the relay within 13 s. Log lines about such clients are limited to 3 per minute, then a count.

## Consequences

In Docker the container needs host networking, because a published port cannot reach `127.0.0.1` inside it (see `docker/compose.yaml`). A slow but legitimate client waits behind others for up to 2 s each, which a poller every second or more does not notice.
