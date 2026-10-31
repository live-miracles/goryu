#include "input.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "relay_common.h"
#include "stream_state.h"

void input_open(input_t *in, const input_transport_t *transport, SRTSOCKET conn, int slot,
                const char *peer_ip, int peer_port, const char *default_streamid) {
    memset(in, 0, sizeof *in);
    in->transport = transport;
    in->conn = conn;
    in->slot = slot;
    snprintf(in->peer_ip, sizeof in->peer_ip, "%s", peer_ip && peer_ip[0] ? peer_ip : "?");
    in->peer_port = peer_port;

    input_info_t info;
    memset(&info, 0, sizeof info);
    transport->open(conn, INPUT_RECV_TIMEOUT_MS, &info);
    in->bonded = info.bonded;
    snprintf(in->streamid, sizeof in->streamid, "%s",
             info.streamid[0] ? info.streamid : (default_streamid ? default_streamid : ""));

    if (info.timeout_error[0]) {
        relay_logf("SRTO_RCVTIMEO rejected for peer=%s:%d: %s\n", in->peer_ip, in->peer_port,
                   info.timeout_error);
    }
    relay_logf("Accepted %s SRT source peer=%s:%d streamid=%s\n", in->bonded ? "bonded" : "plain",
               in->peer_ip, in->peer_port, streamid_or_empty(in->streamid));
}

int input_connected(const input_t *in) {
    return in->transport->connected(in->conn);
}

input_result_t input_recv(input_t *in, char *buf, int cap, int *len) {
    char reason[256];
    reason[0] = '\0';
    input_result_t result = in->transport->recv(in->conn, buf, cap, len, reason, sizeof reason);
    if (result == INPUT_FAILED) {
        const char *op = in->transport->recv_op;
        set_last_errorf("%s: %s", op, reason);
        set_stream_errorf(in->slot, "Input error from peer %s:%d: %s", in->peer_ip, in->peer_port,
                          reason);
        relay_logf("%s failed peer=%s:%d streamid=%s error=\"%s\"\n", op, in->peer_ip,
                   in->peer_port, streamid_or_empty(in->streamid), reason);
    }
    return result;
}
