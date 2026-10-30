#include "status_json.h"

#include "json_out.h"
#include "relay_common.h"

static const char *member_state_str(leg_member_state_t state) {
    switch (state) {
    case LEG_MEMBER_PENDING:
        return "pending";
    case LEG_MEMBER_IDLE:
        return "idle";
    case LEG_MEMBER_RUNNING:
        return "running";
    default:
        return "unknown";
    }
}

void status_json_write(FILE *f, const status_snapshot_t *snap) {
    fputc('{', f);
    int top_first = 1;
    json_ll(f, &top_first, "pid", snap->pid);
    json_ll(f, &top_first, "startedAtMs", snap->started_at_ms);
    json_ll(f, &top_first, "updatedAtMs", snap->updated_at_ms);
    json_str(f, &top_first, "lastError", snap->last_error[0] ? snap->last_error : NULL);

    json_key(f, &top_first, "activeStreamIds");
    fputc('[', f);
    int first = 1;
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!snap->states[i].streamid[0] || !snap->states[i].input_active) continue;
        fprintf(f, "%s\"", first ? "" : ",");
        json_write_escaped(f, snap->states[i].streamid);
        fputc('"', f);
        first = 0;
    }
    fputc(']', f);

    json_key(f, &top_first, "streamStates");
    fputc('[', f);
    first = 1;
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        const relay_stream_state_t *s = &snap->states[i];
        if (!s->streamid[0]) continue;
        fprintf(f, "%s{", first ? "" : ",");
        first = 0;

        int sf = 1;
        json_str(f, &sf, "streamId", s->streamid);
        json_bool(f, &sf, "inputActive", s->input_active);
        json_bool(f, &sf, "outputConnected", s->output_connected);
        json_int(f, &sf, "retryFailures", s->retry_failures);
        json_ll(f, &sf, "forwardedPackets", s->forwarded_packets);
        json_ll(f, &sf, "forwardedBytes", s->forwarded_bytes);
        json_ll(f, &sf, "lastPacketAt", s->last_packet_at_ms);
        json_ll(f, &sf, "lastInputPacketAt", s->last_input_packet_at_ms);

        json_key(f, &sf, "input");
        fputc('{', f);
        int inf = 1;
        json_bool(f, &inf, "bonded", s->input_bonded);
        json_key(f, &inf, "peer");
        if (s->input_bonded || !s->input_peer_ip[0]) {
            fputs("null", f);
        } else {
            fputc('{', f);
            int pf = 1;
            json_str(f, &pf, "ip", s->input_peer_ip);
            json_int(f, &pf, "port", s->input_peer_port);
            fputc('}', f);
        }
        json_ll_opt(f, &inf, "recvPacketsTotal", s->recv_packets_total_valid,
                    s->recv_packets_total);
        json_ll(f, &inf, "recvUniquePacketsTotal", s->recv_unique_packets_total);
        json_int_opt(f, &inf, "recvLossTotal", s->recv_loss_total_valid, s->recv_loss_total);
        json_int(f, &inf, "recvDropTotal", s->recv_drop_total);
        json_int_opt(f, &inf, "retransTotal", s->retrans_total_valid, s->retrans_total);
        json_dbl_opt(f, &inf, "rttMs", s->input_rtt_valid, s->input_rtt_ms);
        json_int_opt(f, &inf, "latencyMs", s->input_latency_valid, s->input_latency_ms);
        json_dbl_opt(f, &inf, "bandwidthMbps", s->input_extra_stats_valid, s->input_bandwidth_mbps);
        json_dbl_opt(f, &inf, "recvRateMbps", s->input_extra_stats_valid, s->input_recv_rate_mbps);
        json_ll_opt(f, &inf, "belatedTotal", s->input_extra_stats_valid, s->input_belated_total);
        json_dbl_opt(f, &inf, "belatedAvgMs", s->input_belated_avg_valid, s->input_belated_avg_ms);
        json_int_opt(f, &inf, "undecryptTotal", s->input_extra_stats_valid,
                     s->input_undecrypt_total);
        json_int_opt(f, &inf, "reorderDistance", s->input_extra_stats_valid,
                     s->input_reorder_distance);
        json_int_opt(f, &inf, "rcvBufMs", s->input_extra_stats_valid, s->input_rcv_buf_ms);

        json_key(f, &inf, "legs");
        fputc('[', f);
        int leg_first = 1;
        for (int j = 0; j < s->leg_count; ++j) {
            const relay_leg_state_t *leg = &s->legs[j];
            fprintf(f, "%s{", leg_first ? "" : ",");
            leg_first = 0;

            int lf = 1;
            json_str(f, &lf, "ip", leg->ip);
            json_int(f, &lf, "port", leg->port);
            json_str(f, &lf, "state", member_state_str(leg->member_state));
            json_dbl_opt(f, &lf, "rttMs", leg->rtt_valid, leg->rtt_ms);
            json_int_opt(f, &lf, "latencyMs", leg->latency_valid, leg->latency_ms);
            json_ll_opt(f, &lf, "recvPacketsTotal", leg->stats_valid, leg->recv_packets_total);
            json_ll_opt(f, &lf, "recvUniquePacketsTotal", leg->stats_valid,
                        leg->recv_unique_packets_total);
            json_int_opt(f, &lf, "recvLossTotal", leg->stats_valid, leg->recv_loss_total);
            json_int_opt(f, &lf, "recvDropTotal", leg->stats_valid, leg->recv_drop_total);
            json_int_opt(f, &lf, "retransTotal", leg->stats_valid, leg->retrans_total);
            json_dbl_opt(f, &lf, "bandwidthMbps", leg->stats_valid, leg->bandwidth_mbps);
            json_dbl_opt(f, &lf, "recvRateMbps", leg->stats_valid, leg->recv_rate_mbps);
            json_ll_opt(f, &lf, "belatedTotal", leg->stats_valid, leg->belated_total);
            json_dbl_opt(f, &lf, "belatedAvgMs", leg->belated_avg_valid, leg->belated_avg_ms);
            json_int_opt(f, &lf, "undecryptTotal", leg->stats_valid, leg->undecrypt_total);
            json_int_opt(f, &lf, "reorderDistance", leg->stats_valid, leg->reorder_distance);
            json_int_opt(f, &lf, "rcvBufMs", leg->stats_valid, leg->rcv_buf_ms);
            fputc('}', f);
        }
        fputs("]}", f);

        json_key(f, &sf, "output");
        fputc('{', f);
        int outf = 1;
        json_ll(f, &outf, "sentPacketsTotal", s->output_sent_packets_total);
        json_int(f, &outf, "sendLossTotal", s->output_send_loss_total);
        json_int(f, &outf, "sendDropTotal", s->output_send_drop_total);
        json_int(f, &outf, "retransTotal", s->output_retrans_total);
        json_dbl_opt(f, &outf, "rttMs", s->output_rtt_valid, s->output_rtt_ms);
        json_int_opt(f, &outf, "latencyMs", s->output_latency_valid, s->output_latency_ms);
        json_dbl_opt(f, &outf, "bandwidthMbps", s->output_extra_stats_valid,
                     s->output_bandwidth_mbps);
        json_dbl_opt(f, &outf, "sendRateMbps", s->output_extra_stats_valid,
                     s->output_send_rate_mbps);
        json_int_opt(f, &outf, "undecryptTotal", s->output_extra_stats_valid,
                     s->output_undecrypt_total);
        json_int_opt(f, &outf, "sndBufMs", s->output_extra_stats_valid, s->output_snd_buf_ms);
        fputc('}', f);

        json_ll(f, &sf, "lastErrorAt", s->last_error_at_ms);
        json_str(f, &sf, "lastError", s->last_error[0] ? s->last_error : NULL);
        fputc('}', f);
    }
    fputc(']', f);

    fputs("}\n", f);
}
