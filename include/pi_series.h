/* pi_series.h — la serie de Leibniz y su reparto entre nodos (#7).
 *
 * Es aritmética pura, sin DSM ni red: así la cobertura del reparto y la
 * tolerancia numérica se prueban en proceso (test_pi_series.c), y app_pi
 * solo se ocupa de publicar y reunir.
 *
 *   π ≈ 4 × Σ (-1)^k / (2k + 1),  k = 0 .. T-1
 */
#ifndef SOA_P3_PI_SERIES_H
#define SOA_P3_PI_SERIES_H

#include <stdint.h>

/* Mayor T admitido: 2^52. Hasta ahí todo k es exacto en un double, así que
 * el denominador 2k+1 se calcula en punto flotante sin perder el índice. */
#define PI_MAX_TERMS (UINT64_C(1) << 52)

/* Términos [first, first + count) asignados a una parte. */
struct pi_range {
    uint64_t first;
    uint64_t count;
};

/* Reparto por división entera balanceada: las primeras T mod N partes reciben
 * un término más. Las partes son contiguas, en orden de `idx`, y entre todas
 * cubren [0, T) exactamente una vez, también cuando T no es múltiplo de N o
 * es menor que N (entonces algunas quedan vacías). `parts` > 0 e
 * `idx` < `parts`. */
struct pi_range pi_partition(uint64_t terms, uint32_t parts, uint32_t idx);

/* Σ (-1)^k / (2k + 1) sobre [first, first + count), SIN el factor 4. El
 * denominador se forma en double: nunca hay un 2k+1 entero que desborde. */
double pi_leibniz_sum(uint64_t first, uint64_t count);

#endif /* SOA_P3_PI_SERIES_H */
