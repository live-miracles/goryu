#ifndef STREAM_STATE_H
#define STREAM_STATE_H

#include <arpa/inet.h>

#include <srt/srt.h>

#include "ratelimit.h"
#include "relay_common.h"

/* The relay's shared, mutex-protected telemetry: one relay_stream_state_t per
 * stream, in the slot that the tracker also uses for the session's sockets.
 * Session threads write it; the status thread only reads a snapshot, so it never
 * calls libsrt. */

typedef struct relay_leg_state {
    char ip[INET6_ADDRSTRLEN];
    int port;
    int member_state;
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
    int belated_avg_valid;
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
    int input_belated_avg_valid;
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

/* One reading of libsrt's counters for a session, taken by stats.c and applied
 * to the stream's state in one step. */
typedef struct stats_sample {
    long long taken_at_ms;
    int can_read_in; /* the input socket was still the tracked one */
    int is_group;    /* the input is a bonded group */
    int have_in;
    SRT_TRACEBSTATS in_stats;
    int in_belated_avg_valid;
    int have_in_latency;
    int in_latency_ms;
    int have_out;
    SRT_TRACEBSTATS out_stats;
    relay_leg_state_t legs[MAX_GROUP_MEMBERS];
    int leg_count;
    /* srt_group_data succeeded, so legs[] is the real set (possibly empty) and
     * not just "nothing could be read". */
    int legs_read;
} stats_sample_t;

/* The most recent process-level error, shown as lastError in the status API. */
void set_last_errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Per-stream state. Every function ignores a slot outside the table, so a
 * session without a streamid (slot -1) can call them freely. */
void set_stream_errorf(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Claims the slot for streamid. Returns the slot, -1 for an empty streamid or
 * when preferred_slot is taken, or -2 when another session already holds the
 * streamid. */
int claim_stream_state(const char *streamid, int preferred_slot);
/* 0 and the age of the stream's last input packet, or -1 if streamid is unknown. */
int stream_input_age_ms(const char *streamid, long long now, long long *age_ms);
void remove_stream_state(int slot);
void set_stream_input_info(int slot, int bonded, const char *peer_ip, int peer_port);
void set_stream_output_connected(int slot, int connected);
void increment_stream_retry_failures(int slot);
int get_stream_retry_failures(int slot);
void record_stream_input_progress(int slot);
void record_stream_forward_progress(int slot, int bytes);

long long stream_state_last_stats_at_ms(int slot);
void stream_state_apply_stats(int slot, const stats_sample_t *sample);

/* Copies every state (MAX_ACTIVE_SESSIONS of them) and the process-level error. */
void stream_state_snapshot(relay_stream_state_t *states, char *last_error, size_t last_error_sz);

#endif
