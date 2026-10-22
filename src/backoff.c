#include "backoff.h"

static const int RETRY_DELAYS_MS[] = {1000, 2000, 4000, 8000, 16000};

int retry_delay_ms(int failures) {
    int max_idx = (int)(sizeof RETRY_DELAYS_MS / sizeof RETRY_DELAYS_MS[0]) - 1;
    if (failures <= 1) return RETRY_DELAYS_MS[0];
    if (failures - 1 > max_idx) return RETRY_DELAYS_MS[max_idx];
    return RETRY_DELAYS_MS[failures - 1];
}
