#include "backoff.h"

static const int RETRY_DELAYS_MS[] = {1000, 2000, 4000, 8000, 16000};

int retry_delay_ms(int failures) {
    int max_idx = (int)(sizeof RETRY_DELAYS_MS / sizeof RETRY_DELAYS_MS[0]) - 1;
    if (failures <= 1) return RETRY_DELAYS_MS[0];
    if (failures - 1 > max_idx) return RETRY_DELAYS_MS[max_idx];
    return RETRY_DELAYS_MS[failures - 1];
}

void retry_init(retry_state_t *r) {
    r->failures = 0;
    r->next_attempt_at_ms = 0;
    r->up_at_ms = 0;
}

int retry_check_healthy(retry_state_t *r, long long now) {
    if (!r->up_at_ms || now - r->up_at_ms < OUTPUT_HEALTHY_MS) return 0;
    r->failures = 0;
    r->up_at_ms = 0;
    return 1;
}

int retry_failed(retry_state_t *r, long long now) {
    retry_check_healthy(r, now);
    r->up_at_ms = 0;
    r->failures++;
    int delay_ms = retry_delay_ms(r->failures);
    r->next_attempt_at_ms = now + delay_ms;
    return delay_ms;
}

void retry_connected(retry_state_t *r, long long now) {
    r->up_at_ms = now;
}

int retry_due(const retry_state_t *r, long long now) {
    return now >= r->next_attempt_at_ms;
}
