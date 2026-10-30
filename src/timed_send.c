#include "timed_send.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int send_all_within(int fd, const char *buf, size_t len, int timeout_ms) {
    long long deadline = monotonic_ms() + timeout_ms;

    while (len > 0) {
        long long left = deadline - monotonic_ms();
        if (left <= 0) return -1;

        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int ready = poll(&pfd, 1, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;

        /* Never blocks: poll() and the deadline decide how long to wait. */
        ssize_t n = send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}
