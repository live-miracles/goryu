#include "runtime.h"

#include <signal.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_running = 1;

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

int relay_running(void) {
    return g_running;
}

void relay_request_stop(void) {
    g_running = 0;
}

static void on_signal(int s) {
    (void)s;
    g_running = 0;
}

int relay_install_signal_handlers(void) {
    struct sigaction stop_sa;
    memset(&stop_sa, 0, sizeof stop_sa);
    stop_sa.sa_handler = on_signal;
    sigemptyset(&stop_sa.sa_mask);

    if (sigaction(SIGINT, &stop_sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &stop_sa, NULL) < 0) return -1;

    struct sigaction pipe_sa;
    memset(&pipe_sa, 0, sizeof pipe_sa);
    pipe_sa.sa_handler = SIG_IGN;
    sigemptyset(&pipe_sa.sa_mask);
    if (sigaction(SIGPIPE, &pipe_sa, NULL) < 0) return -1;

    return 0;
}
