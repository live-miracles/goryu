#include "status_json.h"

#include "json_out.h"
#include "relay_common.h"
#include "telemetry.h"

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
        const srt_counters_t *in = &s->input;
        json_ll_opt(f, &inf, "recvPacketsTotal", input_recv_total_reportable(s), in->recv_total);
        json_ll(f, &inf, "recvUniquePacketsTotal", in->recv_unique_total);
        json_int_opt(f, &inf, "recvLossTotal", input_loss_retrans_reportable(s),
                     in->recv_loss_total);
        json_int(f, &inf, "recvDropTotal", in->recv_drop_total);
        json_int_opt(f, &inf, "retransTotal", input_loss_retrans_reportable(s),
                     in->recv_retrans_total);
        json_dbl_opt(f, &inf, "rttMs", rtt_reportable(in->rtt_ms), in->rtt_ms);
        json_int_opt(f, &inf, "latencyMs", latency_reportable(s->input_latency_ms),
                     s->input_latency_ms);
        int extra = input_extra_reportable(s);
        json_dbl_opt(f, &inf, "bandwidthMbps", extra, in->bandwidth_mbps);
        json_dbl_opt(f, &inf, "recvRateMbps", extra, in->recv_rate_mbps);
        json_ll_opt(f, &inf, "belatedTotal", extra, in->belated_total);
        json_dbl_opt(f, &inf, "belatedAvgMs", input_belated_avg_reportable(s), in->belated_avg_ms);
        json_int_opt(f, &inf, "undecryptTotal", extra, in->undecrypt_total);
        json_int_opt(f, &inf, "reorderDistance", extra, in->reorder_distance);
        json_int_opt(f, &inf, "rcvBufMs", extra, in->rcv_buf_ms);

        json_key(f, &inf, "legs");
        fputc('[', f);
        int leg_first = 1;
        for (int j = 0; j < s->leg_count; ++j) {
            const leg_reading_t *leg = &s->legs[j];
            const srt_counters_t *c = &leg->counters;
            int have = leg_counters_reportable(leg);
            fprintf(f, "%s{", leg_first ? "" : ",");
            leg_first = 0;

            int lf = 1;
            json_str(f, &lf, "ip", leg->ip);
            json_int(f, &lf, "port", leg->port);
            json_str(f, &lf, "state", member_state_str(leg->state));
            json_dbl_opt(f, &lf, "rttMs", leg_rtt_reportable(leg), c->rtt_ms);
            json_int_opt(f, &lf, "latencyMs", leg_latency_reportable(leg), c->tsbpd_delay_ms);
            json_ll_opt(f, &lf, "recvPacketsTotal", have, c->recv_total);
            json_ll_opt(f, &lf, "recvUniquePacketsTotal", have, c->recv_unique_total);
            json_int_opt(f, &lf, "recvLossTotal", have, c->recv_loss_total);
            json_int_opt(f, &lf, "recvDropTotal", have, c->recv_drop_total);
            json_int_opt(f, &lf, "retransTotal", have, c->recv_retrans_total);
            json_dbl_opt(f, &lf, "bandwidthMbps", have, c->bandwidth_mbps);
            json_dbl_opt(f, &lf, "recvRateMbps", have, c->recv_rate_mbps);
            json_ll_opt(f, &lf, "belatedTotal", have, c->belated_total);
            json_dbl_opt(f, &lf, "belatedAvgMs", leg_belated_avg_reportable(leg),
                         c->belated_avg_ms);
            json_int_opt(f, &lf, "undecryptTotal", have, c->undecrypt_total);
            json_int_opt(f, &lf, "reorderDistance", have, c->reorder_distance);
            json_int_opt(f, &lf, "rcvBufMs", have, c->rcv_buf_ms);
            fputc('}', f);
        }
        fputs("]}", f);

        json_key(f, &sf, "output");
        fputc('{', f);
        int outf = 1;
        const srt_counters_t *out = &s->output;
        int out_extra = output_extra_reportable(s);
        json_ll(f, &outf, "sentPacketsTotal", out->sent_total);
        json_int(f, &outf, "sendLossTotal", out->send_loss_total);
        json_int(f, &outf, "sendDropTotal", out->send_drop_total);
        json_int(f, &outf, "retransTotal", out->send_retrans_total);
        json_dbl_opt(f, &outf, "rttMs", output_rtt_reportable(s), out->rtt_ms);
        json_int_opt(f, &outf, "latencyMs", output_latency_reportable(s), out->tsbpd_delay_ms);
        json_dbl_opt(f, &outf, "bandwidthMbps", out_extra, out->bandwidth_mbps);
        json_dbl_opt(f, &outf, "sendRateMbps", out_extra, out->send_rate_mbps);
        json_int_opt(f, &outf, "undecryptTotal", out_extra, out->undecrypt_total);
        json_int_opt(f, &outf, "sndBufMs", out_extra, out->snd_buf_ms);
        fputc('}', f);

        json_ll(f, &sf, "lastErrorAt", s->last_error_at_ms);
        json_str(f, &sf, "lastError", s->last_error[0] ? s->last_error : NULL);
        fputc('}', f);
    }
    fputc(']', f);

    fputs("}\n", f);
}
