# Goryu

Goryu is an SRT relay that accepts a publisher's connection, bonded or plain, on one listener and forwards one clean MPEG-TS stream to a downstream SRT server under the same streamid. It exists because most SRT servers cannot accept a bonded group.

## Language

### Streams and sessions

**Publisher**:
The SRT caller that sends a stream to Goryu, over one plain connection or a bonded group. It is usually an encoder, and the docs use "encoder" for the same thing; "publisher" is the role, and the word in the log lines.
_Avoid_: sender, source

**Streamid**:
The label a publisher sets on its connection. Goryu uses it to identify the stream and copies it onto the output.
_Avoid_: stream name, path

**Stream**:
One publication, identified by its streamid. At most one stream per streamid is active at a time, and each appears as one entry in the status API.
_Avoid_: channel, feed

**Session**:
One accepted publisher connection in Goryu, from accept until it ends. A session with a streamid holds that stream; a session with an empty streamid still forwards but has no stream, and a rejected duplicate never gets one.
_Avoid_: connection, client, worker

**Slot**:
A session's place in the session table, from accept until it releases it. The tracker keeps its sockets and the streamid it holds there, and the stream state keeps its telemetry at the same index. A slot holds at most one stream.

**Input**:
A session's side facing the publisher: one plain connection or one bonded group.

**Output**:
A session's connection to the downstream, made under the same streamid as the input.

**Downstream**:
The SRT server a session's output connects to.
_Avoid_: destination, upstream, sink

### Bonding

**Bonded input**:
An input where the publisher connects two or more redundant legs as one group. A group with a single leg connected is still bonded.

**Plain input**:
An input where the publisher uses one ordinary SRT connection, without legs.
_Avoid_: single-link, non-bonded

**Group**:
The SRT socket group that carries a bonded input. Goryu reads one deduplicated MPEG-TS stream from it, not one per leg.
_Avoid_: bundle

**Leg**:
One link of a group.
_Avoid_: path, member

**Broadcast group**:
A group where every leg carries the whole stream.

**Main/backup group**:
A group where one leg carries the stream and the others stand by.
_Avoid_: failover group

### Duplicate publishers

**Duplicate publisher**:
A publisher that connects with a streamid that already has an active stream.

**Stale**:
Describes a stream whose input has been silent for 5 seconds.

**Takeover**:
A duplicate publisher replacing a stale stream's session. If the stream is not stale, the duplicate is rejected instead.
_Avoid_: hijack, failover

### Surroundings

**Status API**:
The loopback-only JSON endpoint that reports every active stream, its legs and its output.

**Bunryu**:
The media server product that runs Goryu as its bonded-SRT ingress, generates `goryu.json` and polls the status API.
