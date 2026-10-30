#ifndef STREAM_STATE_H
#define STREAM_STATE_H

#include <arpa/inet.h>

#include "ratelimit.h"
#include "relay_common.h"

/* The relay's shared, mutex-protected telemetry: one relay_stream_state_t per
 * stream, in the slot that the tracker also uses for the session's sockets.
 * Session threads write it; the status thread only reads a snapshot, so it never
 * calls libsrt. These types do not mention libsrt, so the rules in telemetry.c and
 * the writer in status_json.c can be unit-tested without it. */

/* One socket's SRT counters as libsrt reports them, copied by stats.c without
 * interpretation. Whether a figure is reported, or left out of the status API as null, is
 * decided by the *_reportable() rules in telemetry.c, from these values. Only the fields
 * the relay uses are here. */
typedef struct srt_counters {
    int tsbpd_delay_ms; /* msRcvTsbPdDelay: the negotiated latency */
    double rtt_ms;
    long long recv_total;
    long long recv_unique_total;
    int recv_loss_total;
    int recv_drop_total;
    int recv_retrans_total;
    double bandwidth_mbps;
    double recv_rate_mbps;
    long long belated_total;
    double belated_avg_ms;
    int undecrypt_total;
    int reorder_distance;
    int rcv_buf_ms;
    long long sent_total;
    int send_loss_total;
    int send_drop_total;
    int send_retrans_total;
    double send_rate_mbps;
    int snd_buf_ms;
} srt_counters_t;

/* What libsrt says about a leg of a bonded group. Broken legs are not reported at
 * all (a failed leg disappears from the status API's legs[]), so there is no
 * state for them. */
typedef enum leg_member_state {
    LEG_MEMBER_UNKNOWN,
    LEG_MEMBER_PENDING,
    LEG_MEMBER_IDLE,
    LEG_MEMBER_RUNNING,
} leg_member_state_t;

/* One member of a bonded group, as read from libsrt; the stream state keeps the last
 * reading of each. */
typedef struct leg_reading {
    char ip[INET6_ADDRSTRLEN];
    int port;
    leg_member_state_t state;
    int have_counters; /* reading the member's own statistics worked */
    srt_counters_t counters;
} leg_reading_t;

/* A bonded leg as it is logged: where it connects from. */
typedef struct relay_leg_ref {
    char ip[INET6_ADDRSTRLEN];
    int port;
} relay_leg_ref_t;

/* What the last successful reading of the input socket was of. The figures that a
 * group socket never fills in are told apart by this (see telemetry.h). */
typedef enum input_kind {
    INPUT_UNREAD, /* no reading has succeeded yet */
    INPUT_PLAIN,
    INPUT_GROUP,
} input_kind_t;

typedef struct relay_stream_state {
    char streamid[1024];
    int input_active;
    int output_connected;
    int retry_failures;
    long long last_input_packet_at_ms;
    long long forwarded_packets;
    long long forwarded_bytes;
    long long last_packet_at_ms;
    /* The input's counters from its last successful reading, with the totals only
     * moving forward. Which of them are reported is decided from these, when the status
     * is written. */
    srt_counters_t input;
    input_kind_t input_kind;
    int input_read_ok; /* the latest reading of the input succeeded */
    /* The negotiated latency, 0 when it is not known. A plain input has it in its own
     * counters; a group's is taken from its legs. */
    int input_latency_ms;
    /* The downstream's counters from its latest reading on this connection. The totals
     * are the current connection's, so they may restart from 0. */
    srt_counters_t output;
    int have_output_counters; /* output holds a reading; cleared when the connection drops */
    leg_reading_t legs[MAX_GROUP_MEMBERS];
    int leg_count;
    /* The legs last reported as joined or left in the log, so each change is
     * logged once, and the limiter that keeps a flapping leg from flooding it. */
    relay_leg_ref_t logged_legs[MAX_GROUP_MEMBERS];
    int logged_leg_count;
    rate_window_t leg_log;
    /* Set once when the session starts. A plain (non-group) input has no legs[]
     * to carry the encoder's address, so it is reported here instead; for a
     * bonded group the per-leg addresses in legs[] are authoritative. */
    int input_bonded;
    char input_peer_ip[INET6_ADDRSTRLEN];
    int input_peer_port;
    long long last_stats_at_ms;
    char last_error[LAST_ERROR_MAX];
    long long last_error_at_ms;
} relay_stream_state_t;

/* One reading of libsrt for a session, taken by stats.c and applied to the stream's
 * state in one step by telemetry_apply(). */
typedef struct stats_sample {
    long long taken_at_ms;
    int can_read_in; /* the input socket was still the tracked one */
    int is_group;    /* the input is a bonded group */
    int have_in;
    srt_counters_t in;
    int have_out;
    srt_counters_t out;
    leg_reading_t legs[MAX_GROUP_MEMBERS];
    int leg_count;
    /* srt_group_data succeeded, so legs[] is the real set (possibly empty) and
     * not just "nothing could be read". */
    int legs_read;
} stats_sample_t;

/* The most recent process-level error, shown as lastError in the status API. */
void set_last_errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Per-stream state, indexed by the session's slot (the same index the tracker uses for
 * its sockets). Every function ignores a slot that is outside the table or holds no
 * stream, so a session that has no streamid, or that lost the claim, can call them
 * freely. */
void set_stream_errorf(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A stream begins in a slot when its session claims the streamid and ends when the
 * session releases the slot. Only tracker.c calls these two, because the tracker decides
 * who holds a streamid (see tracker_acquire_stream), so the table never has an owner of
 * its own to disagree with. */

/* Starts the stream in `slot`. Returns -1 and changes nothing for an empty streamid, a
 * slot outside the table, or a slot that still holds a stream: that stream is never
 * overwritten. */
int stream_state_begin(int slot, const char *streamid);
void stream_state_end(int slot);
/* 0 and the age of the slot's last input packet, or -1 if the slot holds no stream. */
int stream_state_input_age_ms(int slot, long long now, long long *age_ms);
void set_stream_input_info(int slot, int bonded, const char *peer_ip, int peer_port);
void set_stream_output_connected(int slot, int connected);
void set_stream_retry_failures(int slot, int failures);
void record_stream_input_progress(int slot);
void record_stream_forward_progress(int slot, int bytes);

/* Whether the session should read libsrt now: the slot holds a stream and it was
 * last sampled at least STATS_SAMPLE_INTERVAL_MS ago (or never), or `force` is set. A
 * session without a stream is never due, so it skips sampling. */
int stream_state_sample_due(int slot, long long now, int force);
void stream_state_apply_stats(int slot, const stats_sample_t *sample);

/* Copies every state (MAX_ACTIVE_SESSIONS of them) and the process-level error. */
void stream_state_snapshot(relay_stream_state_t *states, char *last_error, size_t last_error_sz);

#endif
