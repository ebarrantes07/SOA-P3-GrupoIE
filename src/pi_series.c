/* pi_series.c — ver pi_series.h. */
#include "pi_series.h"

struct pi_range pi_partition(uint64_t terms, uint32_t parts, uint32_t idx) {
    struct pi_range r = { 0, 0 };
    uint64_t base, extra;

    if (parts == 0 || idx >= parts)
        return r;
    base  = terms / parts;
    extra = terms % parts;
    /* Las `extra` primeras partes llevan base+1; las anteriores a idx que lo
     * llevan son min(idx, extra). */
    r.first = (uint64_t)idx * base + (idx < extra ? idx : extra);
    r.count = base + (idx < extra ? 1u : 0u);
    return r;
}

double pi_leibniz_sum(uint64_t first, uint64_t count) {
    double sum = 0.0;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t k = first + i;
        double term = 1.0 / (2.0 * (double)k + 1.0);

        sum += (k & 1u) ? -term : term;
    }
    return sum;
}
