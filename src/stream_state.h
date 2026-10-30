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

/* What libsrt says about a leg of a bonded group. Broken legs are not reported at
 * all (a failed leg disappears from the status API's legs[]), so there is no
 * state for them. */
typedef enum leg_member_state {
    LEG_MEMBER_UNKNOWN,
    LEG_MEMBER_PENDING,
    LEG_MEMBER_IDLE,
    LEG_MEMBER_RUNNING,
} leg_member_state_t;

typedef struct relay_leg_state {
    char ip[INET6_ADDRSTRLEN];
    int port;
    leg_member_state_t member_state;
    int stats_valid;
    long long recv_packets_total;
    long long recv_unique_packets_total;
    int recv_loss_total;
    int recv_drop_total;
    int retrans_total;
    double rtt_ms;
    int rtt_valid;
    int latency_ms;
    int latency_valid;
    double bandwidth_mbps;
    double recv_rate_mbps;
    long long belated_total;
    double belated_avg_ms;
    int belated_avg_valid; /* implies stats_valid: read_leg sets it only after a successful read */
    int undecrypt_total;
    int reorder_distance;
    int rcv_buf_ms;
} relay_leg_state_t;

/* A bonded leg as it is logged: where it connects from. */
typedef struct relay_leg_ref {
    char ip[INET6_ADDRSTRLEN];
    int port;
} relay_leg_ref_t;

typedef struct relay_stream_state {
    char streamid[1024];
    int input_active;
    int output_connected;
    int retry_failures;
    long long last_input_packet_at_ms;
    long long forwarded_packets;
    long long forwarded_bytes;
    long long last_packet_at_ms;
    long long recv_packets_total;
    int recv_packets_total_valid;
    long long recv_unique_packets_total;
    int recv_loss_total_valid;
    int recv_loss_total;
    int recv_drop_total;
    int retrans_total_valid;
    int retrans_total;
    double input_rtt_ms;
    int input_rtt_valid;
    int input_latency_ms;
    int input_latency_valid;
    /* Only meaningful for a non-bonded input: CUDTGroup::bstatsSocket() never
     * populates these for a group socket, so they'd otherwise silently read 0. */
    int input_extra_stats_valid;
    double input_bandwidth_mbps;
    double input_recv_rate_mbps;
    long long input_belated_total;
    double input_belated_avg_ms;
    int input_belated_avg_valid; /* implies input_extra_stats_valid */
    int input_undecrypt_total;
    int input_reorder_distance;
    int input_rcv_buf_ms;
    double output_rtt_ms;
    int output_rtt_valid;
    int output_latency_ms;
    int output_latency_valid;
    int output_extra_stats_valid;
    long long output_sent_packets_total;
    int output_send_loss_total;
    int output_send_drop_total;
    int output_retrans_total;
    double output_bandwidth_mbps;
    double output_send_rate_mbps;
    int output_undecrypt_total;
    int output_snd_buf_ms;
    relay_leg_state_t legs[MAX_GROUP_MEMBERS];
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

/* One socket's SRT counters as libsrt reports them, copied by stats.c without
 * interpretation. Which of them can be trusted, and which are left out of the status
 * API, is decided by telemetry.c. Only the fields the relay uses are here. */
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

/* One member of a bonded group, as read from libsrt. */
typedef struct leg_reading {
    char ip[INET6_ADDRSTRLEN];
    int port;
    leg_member_state_t state;
    int have_counters; /* reading the member's own statistics worked */
    srt_counters_t counters;
} leg_reading_t;

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

typedef enum claim_result {
    CLAIM_STREAM_OK,        /* the slot now holds the stream */
    CLAIM_STREAM_NO_ID,     /* an empty streamid has no stream: nothing is claimed */
    CLAIM_STREAM_IN_USE,    /* another session holds the streamid */
    CLAIM_STREAM_SLOT_BUSY, /* the slot still holds an earlier stream, or is not a slot */
} claim_result_t;

/* Claims `slot` (the session's own) for streamid. A slot that still holds a stream
 * is never overwritten: the session that held it must remove_stream_state() before
 * the slot is handed on. */
claim_result_t claim_stream_state(const char *streamid, int slot);
/* 0 and the age of the stream's last input packet, or -1 if streamid is unknown. */
int stream_input_age_ms(const char *streamid, long long now, long long *age_ms);
void remove_stream_state(int slot);
void set_stream_input_info(int slot, int bonded, const char *peer_ip, int peer_port);
void set_stream_output_connected(int slot, int connected);
void set_stream_retry_failures(int slot, int failures);
void record_stream_input_progress(int slot);
void record_stream_forward_progress(int slot, int bytes);

/* When the slot's stream was last sampled (0 if never), or -1 when the slot holds no
 * stream, so a session without one skips sampling. */
long long stream_state_last_stats_at_ms(int slot);
void stream_state_apply_stats(int slot, const stats_sample_t *sample);

/* Copies every state (MAX_ACTIVE_SESSIONS of them) and the process-level error. */
void stream_state_snapshot(relay_stream_state_t *states, char *last_error, size_t last_error_sz);

#endif
