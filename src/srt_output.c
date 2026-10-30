#include "srt_output.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "net.h"
#include "relay_common.h"
#include "srt_opts.h"
#include "stream_state.h"

/* libsrt's side of an output: the socket, its options and the connect. Every failure is
 * logged and set as the process lastError here, where libsrt's error code and reject
 * reason are at hand. */

/* Records a failed connect in the process error, the log and err. The reject reason,
 * when libsrt gives one, is part of all three. */
static void report_connect_failure(SRTSOCKET sock, const output_target_t *target,
                                   const char *streamid, char *err, size_t err_sz) {
    char out_ip[INET6_ADDRSTRLEN] = "?";
    int out_port = 0;
    format_leg_addr(&target->out_addr, out_ip, sizeof out_ip, &out_port);
    int error_code = srt_getlasterror(NULL);
    char error_text[256];
    snprintf(error_text, sizeof error_text, "%s", srt_getlasterror_str());
    int reject_reason = error_code == SRT_ECONNREJ ? srt_getrejectreason(sock) : SRT_REJ_UNKNOWN;
    const char *reason_text =
        reject_reason != SRT_REJ_UNKNOWN ? srt_rejectreason_str(reject_reason) : NULL;

    char detail[128] = "";
    char log_suffix[128] = "";
    if (reject_reason != SRT_REJ_UNKNOWN) {
        const char *reason = reason_text ? reason_text : "unknown";
        snprintf(detail, sizeof detail, " (reject=%s/%d)", reason, reject_reason);
        snprintf(log_suffix, sizeof log_suffix, " reject=%s/%d", reason, reject_reason);
    }

    if (err && err_sz > 0) {
        snprintf(err, err_sz, "srt_connect target=%s:%d streamid=%s: %s%s", out_ip, out_port,
                 streamid_or_empty(streamid), error_text, detail);
    }
    set_last_errorf("srt_connect target=%s:%d streamid=%s: %s%s", out_ip, out_port,
                    streamid_or_empty(streamid), error_text, detail);
    relay_logf("srt_connect failed target=%s:%d streamid=%s error=\"%s\"%s\n", out_ip, out_port,
               streamid_or_empty(streamid), error_text, log_suffix);
}

static SRTSOCKET transport_open(char *err, size_t err_sz) {
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        snprintf(err, err_sz, "srt_create_socket(out): %s", srt_getlasterror_str());
        set_last_errorf("srt_create_socket(out): %s", srt_getlasterror_str());
        relay_logf("srt_create_socket(out): %s\n", srt_getlasterror_str());
    }
    return sock;
}

static int transport_connect(SRTSOCKET sock, const output_target_t *target, const char *streamid,
                             char *err, size_t err_sz) {
    SRT_TRANSTYPE tt = SRTT_LIVE;
    srt_setsockflag(sock, SRTO_TRANSTYPE, &tt, sizeof tt);
    char opt_error[256];
    if (apply_srt_opts(sock, target->out_query, opt_error, sizeof opt_error) != 0) {
        snprintf(err, err_sz, "output socket: %s", opt_error);
        set_last_errorf("output socket: %s", opt_error);
        relay_logf("output socket: %s\n", opt_error);
        return -1;
    }

    if (streamid[0]) srt_setsockflag(sock, SRTO_STREAMID, streamid, (int)strlen(streamid));

    int cto = 3000;
    srt_setsockflag(sock, SRTO_CONNTIMEO, &cto, sizeof cto);

    if (srt_connect(sock, (struct sockaddr *)&target->out_addr, (int)target->out_addrlen) ==
        SRT_ERROR) {
        report_connect_failure(sock, target, streamid, err, err_sz);
        return -1;
    }
    return 0;
}

static int transport_send(SRTSOCKET sock, const char *buf, int len, char *reason,
                          size_t reason_sz) {
    if (srt_sendmsg2(sock, buf, len, NULL) != SRT_ERROR) return 0;
    snprintf(reason, reason_sz, "%s", srt_getlasterror_str());
    return -1;
}

const output_transport_t srt_output_transport = {
    .send_op = "srt_sendmsg2",
    .open = transport_open,
    .connect = transport_connect,
    .send = transport_send,
};
