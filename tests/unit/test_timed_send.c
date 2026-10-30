/* Sending with a total deadline (src/timed_send.c), over a socketpair. */

#include "../../src/timed_send.h"
#include "test.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void small_sndbuf(int fd) {
    int size = 4096;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
}

static void test_small_send_arrives_intact(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(send_all_within(sv[0], "hello status", 12, 1000) == 0);
    char got[32] = "";
    /* non-blocking, so a send that did nothing fails the test instead of hanging it */
    CHECK(recv(sv[1], got, sizeof got - 1, MSG_DONTWAIT) == 12);
    CHECK_STR(got, "hello status");
    close(sv[0]);
    close(sv[1]);
}

/* More than the socket buffers hold, to a reader that keeps up: the partial sends
 * must add up to the whole buffer. The reader is a child that counts the bytes. */
static void test_large_send_to_a_reader_that_keeps_up(void) {
    enum { LEN = 4 * 1024 * 1024 };
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    small_sndbuf(sv[0]);
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        static char buf[65536];
        long long total = 0;
        ssize_t n;
        while ((n = read(sv[1], buf, sizeof buf)) > 0)
            total += n;
        _exit(total == LEN ? 0 : 1);
    }
    close(sv[1]);
    char *data = malloc(LEN);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 'x', LEN);
    CHECK(send_all_within(sv[0], data, LEN, 10000) == 0);
    close(sv[0]);
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(data);
}

/* A peer that never reads: the call returns after about the timeout, not never. */
static void test_a_peer_that_never_reads_is_given_up_on(void) {
    enum { LEN = 8 * 1024 * 1024 };
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    small_sndbuf(sv[0]);
    char *data = malloc(LEN);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 'x', LEN);
    long long start = now_ms();
    CHECK(send_all_within(sv[0], data, LEN, 300) == -1);
    long long took = now_ms() - start;
    CHECK(took >= 250);
    CHECK(took < 2000);
    close(sv[0]);
    close(sv[1]);
    free(data);
}

/* The deadline covers the whole send: a slow trickle does not restart it. */
static void test_the_deadline_is_total_not_per_call(void) {
    enum { LEN = 8 * 1024 * 1024 };
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    small_sndbuf(sv[0]);
    pid_t child = fork();
    if (child == 0) { /* reads a little every 50 ms, far too slowly to finish */
        close(sv[0]);
        char buf[2048];
        for (int i = 0; i < 100; ++i) {
            if (read(sv[1], buf, sizeof buf) <= 0) break;
            usleep(50000);
        }
        _exit(0);
    }
    close(sv[1]);
    char *data = malloc(LEN);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 'x', LEN);
    long long start = now_ms();
    CHECK(send_all_within(sv[0], data, LEN, 400) == -1);
    long long took = now_ms() - start;
    CHECK(took >= 350);
    CHECK(took < 2000);
    close(sv[0]);
    waitpid(child, NULL, 0);
    free(data);
}

/* A closed peer is an error at once, and does not kill the process with SIGPIPE. */
static void test_a_closed_peer_is_an_error_not_a_signal(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    long long start = now_ms();
    CHECK(send_all_within(sv[0], "x", 1, 1000) == -1);
    CHECK(now_ms() - start < 500);
    close(sv[0]);
}

static void test_nothing_to_send(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(send_all_within(sv[0], "", 0, 100) == 0);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    test_small_send_arrives_intact();
    test_large_send_to_a_reader_that_keeps_up();
    test_a_peer_that_never_reads_is_given_up_on();
    test_the_deadline_is_total_not_per_call();
    test_a_closed_peer_is_an_error_not_a_signal();
    test_nothing_to_send();
    TEST_SUMMARY("timed_send");
}
