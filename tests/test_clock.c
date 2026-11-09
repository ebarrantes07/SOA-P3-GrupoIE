/* test_clock.c — prueba de now_monotonic_ns(). */
#include "clock.h"
#include "test_util.h"

static bool test_never_decreases(void) {
    uint64_t prev = now_monotonic_ns();
    for (int i = 0; i < 10000; i++) {
        uint64_t now = now_monotonic_ns();
        CHECK(now >= prev);   /* única garantía real de un reloj monotónico */
        prev = now;
    }
    return true;
}

static bool test_advances(void) {
    /* Un reloj que nunca decrece pero tampoco avanza pasaría la prueba
     * anterior. Esta exige progreso real tras un trabajo no trivial, sin
     * depender de cuánto tarde: solo de que la resta sea positiva. */
    uint64_t t0 = now_monotonic_ns();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 2000000; i++)
        sink += (uint64_t)i;
    uint64_t t1 = now_monotonic_ns();
    CHECK(sink > 0);
    CHECK(t1 > t0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"never_decreases", test_never_decreases},
        {"advances", test_advances},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
