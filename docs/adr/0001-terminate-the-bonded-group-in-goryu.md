# Terminate the bonded group in Goryu and forward plain SRT

Most SRT servers (SRS, MediaMTX, datarhei Core) reject a bonded socket group, so Goryu accepts the group itself, lets libsrt merge and deduplicate the legs, and publishes the one resulting stream downstream as ordinary SRT. The same listener and session code also accept a plain single-link caller, which is why the project was renamed from `srt-bonding-relay`.

## Considered options

- **libsrt's `srt-live-transmit`.** It does not work as a bonded relay: it aborts after about 10 KB on a group, and a libsrt maintainer confirmed on Haivision/srt#3418 that the app does not support socket groups.
- **SRTLA or a vendor bonding protocol.** These are different protocols, so a sender that speaks libsrt groups cannot use them.
- **A server that accepts bonded SRT itself** (Nimble Streamer) needs no relay; Goryu is for everything else.

The measurements and links are in [Alternatives](../1-getting-started/3-alternatives.md).
