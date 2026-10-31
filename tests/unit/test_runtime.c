#include <time.h>

#include "../../src/runtime.h"
#include "test.h"

/* The two clocks are not interchangeable: now_ms decides (it cannot be stepped) and
 * wall_ms is shown (it can). The time since boot is not the time since 1970, which is how
 * a decision that read the wrong one would show here. The real clocks are used. */

static void test_now_ms_is_not_the_wall_clock(void) {
    long long mono = now_ms();
    long long wall = wall_ms();
    CHECK(wall > 1700000000000LL);      /* after November 2023: milliseconds since the epoch */
    CHECK(mono > 0 && mono < wall / 2); /* since boot, not since 1970 */
}

static void test_now_ms_only_moves_forward(void) {
    long long last = now_ms();
    int went_back = 0;
    for (int i = 0; i < 1000; ++i) {
        long long now = now_ms();
        if (now < last) went_back = 1;
        last = now;
    }
    CHECK(!went_back);
    long long before = now_ms();
    sleep_ms(30);
    long long slept = now_ms() - before;
    CHECK(slept >= 20 && slept < 1000);
}

int main(void) {
    test_now_ms_is_not_the_wall_clock();
    test_now_ms_only_moves_forward();
    TEST_SUMMARY("runtime");
}
