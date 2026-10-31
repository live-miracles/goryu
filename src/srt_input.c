#include "srt_input.h"

#include <stdio.h>
#include <string.h>

/* libsrt's side of an input: the receive timeout, the streamid, the group test and the
 * receive, with libsrt's error codes sorted into what input.h says a receive can come to.
 * It logs and records nothing; input.c does. */

/* The streamid the publisher set on the connection, or "". */
static void read_streamid(SRTSOCKET conn, char *sid, size_t sid_sz) {
    sid[0] = '\0';
    int sid_len = (int)sid_sz - 1;
    if (srt_getsockflag(conn, SRTO_STREAMID, sid, &sid_len) == SRT_ERROR) return;
    if (sid_len < 0) sid_len = 0;
    if ((size_t)sid_len >= sid_sz) sid_len = (int)sid_sz - 1;
    sid[sid_len] = '\0';
}

static void transport_open(SRTSOCKET conn, int recv_timeout_ms, input_info_t *info) {
    info->bonded = (conn & SRTGROUP_MASK) != 0;
    read_streamid(conn, info->streamid, sizeof info->streamid);
    if (srt_setsockflag(conn, SRTO_RCVTIMEO, &recv_timeout_ms, sizeof recv_timeout_ms) ==
        SRT_ERROR) {
        snprintf(info->timeout_error, sizeof info->timeout_error, "%s", srt_getlasterror_str());
    }
}

static int transport_connected(SRTSOCKET conn) {
    SRT_SOCKSTATUS st = srt_getsockstate(conn);
    return st == SRTS_CONNECTED || st == SRTS_LISTENING || st == SRTS_CONNECTING;
}

static input_result_t transport_recv(SRTSOCKET conn, char *buf, int cap, int *len, char *reason,
                                     size_t reason_sz) {
    SRT_MSGCTRL mc = srt_msgctrl_default;
    int r = srt_recvmsg2(conn, buf, cap, &mc);
    if (r == SRT_ERROR) {
        int err = srt_getlasterror(NULL);
        /* No data within the receive timeout (libsrt reports it as EASYNCRCV). */
        if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) return INPUT_IDLE;
        if (err == SRT_ECONNLOST || err == SRT_ENOCONN) return INPUT_CLOSED;
        snprintf(reason, reason_sz, "%s", srt_getlasterror_str());
        return INPUT_FAILED;
    }
    if (r <= 0) return INPUT_IDLE;
    *len = r;
    return INPUT_DATA;
}

const input_transport_t srt_input_transport = {
    .recv_op = "srt_recvmsg2",
    .open = transport_open,
    .connected = transport_connected,
    .recv = transport_recv,
};
