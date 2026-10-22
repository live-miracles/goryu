#include "status_api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <srt/srt.h>

#include "json_out.h"
#include "log.h"
#include "relay_common.h"
#include "runtime.h"
#include "stream_state.h"

static int g_status_port = 8081;
static long long g_started_at_ms = 0;

static const char *member_state_str(int member_state) {
    switch (member_state) {
    case SRT_GST_PENDING:
        return "pending";
    case SRT_GST_IDLE:
        return "idle";
    case SRT_GST_RUNNING:
        return "running";
    case SRT_GST_BROKEN:
        return "broken";
    default:
        return "unknown";
    }
}

static void write_status_response(int client_fd) {
    relay_stream_state_t states[MAX_ACTIVE_SESSIONS];
    char last_error[LAST_ERROR_MAX];
    long long updated_at_ms = now_ms();

    stream_state_snapshot(states, last_error, sizeof last_error);

    int out_fd = dup(client_fd);
    if (out_fd < 0) return;
    FILE *f = fdopen(out_fd, "w");
    if (!f) {
        close(out_fd);
        return;
    }

    fputs("HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Cache-Control: no-store\r\n"
          "Connection: close\r\n"
          "\r\n"
          "{",
          f);

    int top_first = 1;
    json_ll(f, &top_first, "pid", (long long)getpid());
    json_ll(f, &top_first, "startedAtMs", g_started_at_ms);
    json_ll(f, &top_first, "updatedAtMs", updated_at_ms);
    json_str(f, &top_first, "lastError", last_error[0] ? last_error : NULL);

    json_key(f, &top_first, "activeStreamIds");
    fputc('[', f);
    int first = 1;
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        if (!states[i].streamid[0] || !states[i].input_active) continue;
        fprintf(f, "%s\"", first ? "" : ",");
        json_write_escaped(f, states[i].streamid);
        fputc('"', f);
        first = 0;
    }
    fputc(']', f);

    json_key(f, &top_first, "streamStates");
    fputc('[', f);
    first = 1;
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; ++i) {
        const relay_stream_state_t *s = &states[i];
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
        json_dbl_opt(f, &inf, "belatedAvgMs",
                     s->input_extra_stats_valid && s->input_belated_avg_valid,
                     s->input_belated_avg_ms);
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
            json_dbl_opt(f, &lf, "belatedAvgMs", leg->stats_valid && leg->belated_avg_valid,
                         leg->belated_avg_ms);
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
    fclose(f);
}

static void *status_http_main(void *arg) {
    (void)arg;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        set_last_errorf("status socket(): %s", strerror(errno));
        perror("status socket");
        return NULL;
    }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((unsigned short)g_status_port);

    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        set_last_errorf("status bind :%d failed: %s", g_status_port, strerror(errno));
        relay_logf("status bind :%d failed: %s\n", g_status_port, strerror(errno));
        close(srv);
        return NULL;
    }
    if (listen(srv, 16) < 0) {
        set_last_errorf("status listen failed: %s", strerror(errno));
        relay_logf("status listen failed: %s\n", strerror(errno));
        close(srv);
        return NULL;
    }

    relay_logf("Status HTTP listening on 127.0.0.1:%d\n", g_status_port);

    while (relay_running()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        int client = accept(srv, NULL, NULL);
        if (client < 0) continue;

        char req[1024];
        ssize_t req_len = recv(client, req, sizeof req, 0);
        if (req_len <= 0) {
            close(client);
            continue;
        }
        write_status_response(client);
        close(client);
    }

    close(srv);
    return NULL;
}

int status_server_start(int port, long long started_at_ms, pthread_t *tid) {
    g_status_port = port;
    g_started_at_ms = started_at_ms;
    return pthread_create(tid, NULL, status_http_main, NULL) == 0 ? 0 : -1;
}
