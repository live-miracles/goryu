/* The status API's JSON (src/status_json.c): the contract Bunryu mirrors, which fields
 * exist and when they are null. See docs/4-reference/1-status-api.md. The writer is
 * handed a snapshot, so a fixture is all it needs. */

#include "../../src/status_json.h"
#include "test.h"

#include <stdlib.h>

static relay_stream_state_t *g_states;

static void reset(void) {
    memset(g_states, 0, MAX_ACTIVE_SESSIONS * sizeof *g_states);
}

/* Renders the snapshot to a malloc'd string. */
static char *render(const char *last_error) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    status_snapshot_t snap = {1234, 1000, 2000, last_error, g_states};
    status_json_write(f, &snap);
    fclose(f);
    return buf;
}

static int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

/* A plain input with every value reportable. */
static relay_stream_state_t *add_plain(int slot, const char *streamid) {
    relay_stream_state_t *p = &g_states[slot];
    snprintf(p->streamid, sizeof p->streamid, "%s", streamid);
    p->input_active = 1;
    p->output_connected = 1;
    p->forwarded_packets = 100;
    p->forwarded_bytes = 131200;
    p->last_packet_at_ms = 5;
    p->last_input_packet_at_ms = 4;
    snprintf(p->input_peer_ip, sizeof p->input_peer_ip, "10.0.1.20");
    p->input_peer_port = 40000;
    p->recv_packets_total_valid = 1;
    p->recv_packets_total = 100;
    p->recv_unique_packets_total = 99;
    p->recv_loss_total_valid = 1;
    p->recv_loss_total = 1;
    p->retrans_total_valid = 1;
    p->retrans_total = 2;
    p->input_rtt_valid = 1;
    p->input_rtt_ms = 1.25;
    p->input_latency_valid = 1;
    p->input_latency_ms = 240;
    p->input_extra_stats_valid = 1;
    p->input_bandwidth_mbps = 20.5;
    p->input_recv_rate_mbps = 4.0;
    p->input_belated_total = 3;
    p->input_belated_avg_valid = 1;
    p->input_belated_avg_ms = 12.5;
    p->input_reorder_distance = 4;
    p->input_rcv_buf_ms = 90;
    p->output_extra_stats_valid = 1;
    p->output_sent_packets_total = 100;
    p->output_rtt_valid = 1;
    p->output_rtt_ms = 0.5;
    p->output_latency_valid = 1;
    p->output_latency_ms = 200;
    p->output_bandwidth_mbps = 30;
    p->output_send_rate_mbps = 4;
    p->output_snd_buf_ms = 70;
    return p;
}

/* A bonded input as telemetry.c leaves it: the socket-only counters are not valid. */
static relay_stream_state_t *add_bonded(int slot, const char *streamid) {
    relay_stream_state_t *p = add_plain(slot, streamid);
    p->input_bonded = 1;
    p->input_peer_ip[0] = '\0';
    p->recv_loss_total_valid = 0;
    p->retrans_total_valid = 0;
    p->input_rtt_valid = 0;
    p->input_extra_stats_valid = 0;
    p->input_belated_avg_valid = 0;
    p->leg_count = 2;
    for (int i = 0; i < 2; ++i) {
        relay_leg_state_t *leg = &p->legs[i];
        snprintf(leg->ip, sizeof leg->ip, "127.0.0.%d", i + 1);
        leg->port = 40001 + i;
        leg->member_state = i == 0 ? LEG_MEMBER_RUNNING : LEG_MEMBER_IDLE;
        leg->stats_valid = 1;
        leg->recv_packets_total = 50 + i;
        leg->rtt_valid = 1;
        leg->rtt_ms = 2.0;
        leg->latency_valid = 1;
        leg->latency_ms = 300;
        leg->belated_avg_valid = 1;
        leg->belated_avg_ms = 1.0;
    }
    return p;
}

/* The whole object for one plain stream, byte for byte. A change here is a change to
 * the contract with Bunryu. */
static void test_a_plain_stream_golden(void) {
    reset();
    add_plain(3, "camera1/main");
    char *json = render("");
    CHECK_STR(
        json,
        "{\"pid\":1234,\"startedAtMs\":1000,\"updatedAtMs\":2000,\"lastError\":null,"
        "\"activeStreamIds\":[\"camera1/main\"],\"streamStates\":[{\"streamId\":\"camera1/main\","
        "\"inputActive\":true,\"outputConnected\":true,\"retryFailures\":0,"
        "\"forwardedPackets\":100,\"forwardedBytes\":131200,\"lastPacketAt\":5,"
        "\"lastInputPacketAt\":4,\"input\":{\"bonded\":false,\"peer\":{\"ip\":\"10.0.1.20\","
        "\"port\":40000},\"recvPacketsTotal\":100,\"recvUniquePacketsTotal\":99,"
        "\"recvLossTotal\":1,\"recvDropTotal\":0,\"retransTotal\":2,\"rttMs\":1.250,"
        "\"latencyMs\":240,\"bandwidthMbps\":20.500,\"recvRateMbps\":4.000,\"belatedTotal\":3,"
        "\"belatedAvgMs\":12.500,\"undecryptTotal\":0,\"reorderDistance\":4,\"rcvBufMs\":90,"
        "\"legs\":[]},\"output\":{\"sentPacketsTotal\":100,\"sendLossTotal\":0,"
        "\"sendDropTotal\":0,\"retransTotal\":0,\"rttMs\":0.500,\"latencyMs\":200,"
        "\"bandwidthMbps\":30.000,\"sendRateMbps\":4.000,\"undecryptTotal\":0,\"sndBufMs\":70},"
        "\"lastErrorAt\":0,\"lastError\":null}]}\n");
    free(json);
}

static void test_an_empty_relay(void) {
    reset();
    char *json = render("");
    CHECK_STR(json, "{\"pid\":1234,\"startedAtMs\":1000,\"updatedAtMs\":2000,\"lastError\":null,"
                    "\"activeStreamIds\":[],\"streamStates\":[]}\n");
    free(json);
    json = render("bind failed: Address already in use");
    CHECK(contains(json, "\"lastError\":\"bind failed: Address already in use\","));
    free(json);
}

/* A bonded input has no socket-level counters: they are null, not a misleading 0. */
static void test_a_bonded_input_has_null_socket_counters(void) {
    reset();
    relay_stream_state_t *p = add_bonded(0, "cam/bonded");
    /* the per-leg addresses are authoritative: even an address that was stored is not shown */
    snprintf(p->input_peer_ip, sizeof p->input_peer_ip, "10.0.1.20");
    p->input_peer_port = 40000;
    char *json = render("");
    const char *nulls[] = {
        "\"peer\":null",          "\"recvLossTotal\":null",  "\"retransTotal\":null",
        "\"bandwidthMbps\":null", "\"recvRateMbps\":null",   "\"belatedTotal\":null",
        "\"belatedAvgMs\":null",  "\"undecryptTotal\":null", "\"reorderDistance\":null",
        "\"rcvBufMs\":null"};
    /* the input object ends where its legs begin: look only at that part */
    const char *input = strstr(json, "\"input\":{");
    const char *legs = strstr(json, "\"legs\":[");
    CHECK(input && legs && input < legs);
    char head[2048];
    snprintf(head, sizeof head, "%.*s", (int)(legs - input), input);
    for (size_t i = 0; i < sizeof nulls / sizeof nulls[0]; ++i) {
        if (!contains(head, nulls[i])) printf("  missing in input: %s\n", nulls[i]);
        CHECK(contains(head, nulls[i]));
    }
    CHECK(contains(head, "\"bonded\":true"));
    /* what a group does report stays a number */
    CHECK(contains(head, "\"recvPacketsTotal\":100"));
    CHECK(contains(head, "\"recvUniquePacketsTotal\":99"));
    CHECK(contains(head, "\"recvDropTotal\":0"));
    CHECK(contains(head, "\"latencyMs\":240"));
    CHECK(contains(head, "\"rttMs\":null")); /* the group's own rtt is not valid */
    free(json);
}

static void test_a_bonded_streams_legs_are_listed_with_their_own_numbers(void) {
    reset();
    add_bonded(0, "cam/bonded");
    char *json = render("");
    CHECK(contains(json, "\"legs\":[{\"ip\":\"127.0.0.1\",\"port\":40001,\"state\":\"running\","
                         "\"rttMs\":2.000,\"latencyMs\":300,\"recvPacketsTotal\":50,"));
    CHECK(contains(json, "{\"ip\":\"127.0.0.2\",\"port\":40002,\"state\":\"idle\","));
    CHECK(contains(json, "\"recvPacketsTotal\":51,"));
    CHECK(contains(json, "\"belatedAvgMs\":1.000,"));
    free(json);
}

/* A failed leg disappears from legs[]: it is not reported with state "broken". */
static void test_a_leg_that_is_gone_is_not_listed(void) {
    reset();
    relay_stream_state_t *p = add_bonded(0, "cam/bonded");
    p->leg_count = 1;
    char *json = render("");
    CHECK(contains(json, "127.0.0.1"));
    CHECK(!contains(json, "127.0.0.2"));
    CHECK(!contains(json, "broken"));
    p->leg_count = 0;
    free(json);
    json = render("");
    CHECK(contains(json, "\"legs\":[]"));
    free(json);
}

/* A leg whose own statistics could not be read is listed, with null where numbers go. */
static void test_a_leg_without_statistics_has_null_numbers(void) {
    reset();
    relay_stream_state_t *p = add_bonded(0, "cam/bonded");
    p->legs[1].stats_valid = 0;
    p->legs[1].rtt_valid = 0;
    p->legs[1].latency_valid = 0;
    p->legs[1].belated_avg_valid = 0;
    char *json = render("");
    const char *second = strstr(json, "{\"ip\":\"127.0.0.2\"");
    CHECK(second != NULL);
    if (second) {
        CHECK(contains(second,
                       "\"port\":40002,\"state\":\"idle\",\"rttMs\":null,\"latencyMs\":null,"
                       "\"recvPacketsTotal\":null,\"recvUniquePacketsTotal\":null,"
                       "\"recvLossTotal\":null,\"recvDropTotal\":null,\"retransTotal\":null,"
                       "\"bandwidthMbps\":null,\"recvRateMbps\":null,\"belatedTotal\":null,"
                       "\"belatedAvgMs\":null,\"undecryptTotal\":null,"
                       "\"reorderDistance\":null,\"rcvBufMs\":null}"));
    }
    /* the first leg is untouched */
    CHECK(contains(json, "\"recvPacketsTotal\":50,"));
    free(json);
}

/* belatedAvgMs has its own flag: a garbage average is null while the rest are numbers. */
static void test_belated_average_is_null_on_its_own(void) {
    reset();
    relay_stream_state_t *p = add_plain(0, "cam/plain");
    p->input_belated_avg_valid = 0;
    char *json = render("");
    CHECK(contains(json, "\"belatedTotal\":3,\"belatedAvgMs\":null,\"undecryptTotal\":0"));
    free(json);
}

static void test_the_output_is_null_until_it_has_been_read(void) {
    reset();
    relay_stream_state_t *p = add_plain(0, "cam/plain");
    p->output_extra_stats_valid = 0;
    p->output_rtt_valid = 0;
    p->output_latency_valid = 0;
    char *json = render("");
    CHECK(contains(json, "\"output\":{\"sentPacketsTotal\":100,\"sendLossTotal\":0,"
                         "\"sendDropTotal\":0,\"retransTotal\":0,\"rttMs\":null,\"latencyMs\":null,"
                         "\"bandwidthMbps\":null,\"sendRateMbps\":null,\"undecryptTotal\":null,"
                         "\"sndBufMs\":null}"));
    free(json);
}

/* An unused slot is skipped; a stream whose input is gone is still listed but is not
 * active. Several streams keep slot order. */
static void test_streams_and_active_streams(void) {
    reset();
    add_plain(2, "second");
    add_plain(0, "first");
    add_plain(5, "gone")->input_active = 0;
    char *json = render("");
    CHECK(contains(json, "\"activeStreamIds\":[\"first\",\"second\"],"));
    const char *a = strstr(json, "\"streamId\":\"first\"");
    const char *b = strstr(json, "\"streamId\":\"second\"");
    const char *c = strstr(json, "\"streamId\":\"gone\"");
    CHECK(a && b && c && a < b && b < c);
    CHECK(contains(c, "\"inputActive\":false"));
    free(json);
}

/* A plain input whose address is not known yet has no peer. */
static void test_a_plain_input_without_an_address_has_a_null_peer(void) {
    reset();
    relay_stream_state_t *p = add_plain(0, "cam/plain");
    p->input_peer_ip[0] = '\0';
    char *json = render("");
    CHECK(contains(json, "\"input\":{\"bonded\":false,\"peer\":null,"));
    free(json);
}

static void test_text_is_escaped(void) {
    reset();
    relay_stream_state_t *p = add_plain(0, "a\"b\\c\nd");
    snprintf(p->last_error, sizeof p->last_error, "rejected: \"no\"\tkey");
    p->last_error_at_ms = 77;
    char *json = render("process \"error\"");
    CHECK(contains(json, "\"streamId\":\"a\\\"b\\\\c\\nd\""));
    CHECK(contains(json, "\"activeStreamIds\":[\"a\\\"b\\\\c\\nd\"]"));
    CHECK(contains(json, "\"lastErrorAt\":77,\"lastError\":\"rejected: \\\"no\\\"\\tkey\""));
    CHECK(contains(json, "\"lastError\":\"process \\\"error\\\"\",\"activeStreamIds\""));
    free(json);
}

static void test_retry_failures_and_member_states(void) {
    reset();
    relay_stream_state_t *p = add_bonded(0, "cam/bonded");
    p->retry_failures = 3;
    p->output_connected = 0;
    p->legs[0].member_state = LEG_MEMBER_PENDING;
    p->legs[1].member_state = LEG_MEMBER_UNKNOWN;
    char *json = render("");
    CHECK(contains(json, "\"outputConnected\":false,\"retryFailures\":3,"));
    CHECK(contains(json, "\"state\":\"pending\""));
    CHECK(contains(json, "\"state\":\"unknown\""));
    free(json);
}

int main(void) {
    g_states = calloc(MAX_ACTIVE_SESSIONS, sizeof *g_states);
    if (!g_states) return 2;
    test_a_plain_stream_golden();
    test_an_empty_relay();
    test_a_bonded_input_has_null_socket_counters();
    test_a_bonded_streams_legs_are_listed_with_their_own_numbers();
    test_a_leg_that_is_gone_is_not_listed();
    test_a_leg_without_statistics_has_null_numbers();
    test_belated_average_is_null_on_its_own();
    test_the_output_is_null_until_it_has_been_read();
    test_streams_and_active_streams();
    test_a_plain_input_without_an_address_has_a_null_peer();
    test_text_is_escaped();
    test_retry_failures_and_member_states();
    free(g_states);
    TEST_SUMMARY("status_json");
}
