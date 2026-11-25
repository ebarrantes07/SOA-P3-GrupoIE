/* test_pi_series.c — el reparto de la serie y la tolerancia numérica (#7).
 *
 * El criterio de cobertura es exacto y se prueba en proceso, sin red: para
 * varios T y N, las partes son contiguas, empiezan en 0, no se solapan, suman
 * T y están balanceadas. La tolerancia de 1e-9 también se comprueba acá con
 * la misma reducción que hace app_pi; e2e_apps.sh la vuelve a exigir con
 * procesos de verdad.
 */
#include "pi_series.h"
#include "test_util.h"

#include <math.h>

static bool covers_exactly(uint64_t terms, uint32_t parts) {
    uint64_t next = 0, min = UINT64_MAX, max = 0;

    for (uint32_t i = 0; i < parts; i++) {
        struct pi_range r = pi_partition(terms, parts, i);

        /* Contigua con la anterior: ni hueco ni solape. */
        CHECK(r.first == next);
        next = r.first + r.count;
        if (r.count < min)
            min = r.count;
        if (r.count > max)
            max = r.count;
    }
    CHECK(next == terms);          /* llega exactamente a T */
    CHECK(max - min <= 1u);        /* balanceada */
    return true;
}

static bool test_partition_covers_every_term_once(void) {
    static const uint64_t ts[] = { 1, 2, 3, 4, 5, 7, 8, 9, 1000, 100003,
                                   UINT64_C(1) << 40, PI_MAX_TERMS };

    for (size_t k = 0; k < sizeof ts / sizeof ts[0]; k++)
        for (uint32_t n = 1; n <= 4; n++)
            CHECK(covers_exactly(ts[k], n));
    return true;
}

static bool test_partition_with_fewer_terms_than_parts(void) {
    /* T=2, N=4: dos partes de un término y dos vacías, todo cubierto. */
    CHECK(pi_partition(2, 4, 0).count == 1);
    CHECK(pi_partition(2, 4, 1).count == 1);
    CHECK(pi_partition(2, 4, 2).count == 0);
    CHECK(pi_partition(2, 4, 3).count == 0);
    CHECK(pi_partition(2, 4, 3).first == 2);
    return true;
}

static bool test_partition_rejects_bad_index(void) {
    CHECK(pi_partition(10, 0, 0).count == 0);
    CHECK(pi_partition(10, 2, 2).count == 0);
    return true;
}

static bool test_leibniz_first_terms(void) {
    CHECK(pi_leibniz_sum(0, 0) == 0.0);
    CHECK(pi_leibniz_sum(0, 1) == 1.0);
    CHECK(pi_leibniz_sum(1, 1) == -1.0 / 3.0);     /* k impar resta */
    CHECK(pi_leibniz_sum(0, 2) == 1.0 - 1.0 / 3.0);
    return true;
}

static bool test_leibniz_large_index_no_overflow(void) {
    /* Con k = 2^51, un 2k+1 entero en 32 bits habría desbordado hace rato.
     * En double el término es positivo, finito y del tamaño esperado. */
    uint64_t k = UINT64_C(1) << 51;
    double t = pi_leibniz_sum(k, 1);

    CHECK(t > 0.0 && isfinite(t));
    CHECK(fabs(t - 1.0 / (2.0 * (double)k + 1.0)) == 0.0);
    return true;
}

/* La misma reducción que app_pi: sumas parciales en orden de identificador. */
static double distributed(uint64_t terms, uint32_t parts) {
    double sum = 0.0;

    for (uint32_t i = 0; i < parts; i++) {
        struct pi_range r = pi_partition(terms, parts, i);
        sum += pi_leibniz_sum(r.first, r.count);
    }
    return 4.0 * sum;
}

static bool test_distributed_matches_sequential(void) {
    const uint64_t t = 100003;
    double seq = 4.0 * pi_leibniz_sum(0, t);

    for (uint32_t n = 1; n <= 4; n++)
        CHECK(fabs(distributed(t, n) - seq) <= 1e-9);
    /* Con N=1 no hay reagrupación: tiene que ser idéntico, no solo cercano. */
    CHECK(distributed(t, 1) == seq);
    /* El truncamiento es otra cosa, del orden de 1/T, y mucho mayor que la
     * tolerancia: si se confundieran, el criterio no podría cumplirse. */
    CHECK(fabs(seq - 3.14159265358979323846) > 1e-6);
    CHECK(fabs(seq - 3.14159265358979323846) < 2.0 / (double)t);
    return true;
}

int main(void) {
    static const struct test_case tests[] = {
        { "partition_covers_every_term_once",
          test_partition_covers_every_term_once },
        { "partition_with_fewer_terms_than_parts",
          test_partition_with_fewer_terms_than_parts },
        { "partition_rejects_bad_index", test_partition_rejects_bad_index },
        { "leibniz_first_terms", test_leibniz_first_terms },
        { "leibniz_large_index_no_overflow",
          test_leibniz_large_index_no_overflow },
        { "distributed_matches_sequential",
          test_distributed_matches_sequential },
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
