/* test_util.h — convención mínima compartida por tests/test_*.c.
 *
 * Reutilizado de SOA-P2-GrupoIE (ver docs/decisiones_diseno.md, D0.5).
 *
 * Cada archivo de prueba define funciones test_*() que devuelven bool
 * (true = pasó) y usan CHECK() para cada aserto, en vez de assert(): CHECK no
 * aborta el programa, así una sola corrida reporta TODAS las fallas de esa
 * prueba en vez de detenerse en la primera.
 */
#ifndef SOA_P3_TEST_UTIL_H
#define SOA_P3_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return false; \
        } \
    } while (0)

struct test_case {
    const char *name;
    bool (*fn)(void);
};

/* Corre cada caso, imprime OK/FALLO por nombre y un resumen. Devuelve la
 * cantidad de casos fallidos (0 = éxito), pensado para usarse como el código
 * de salida de main(). */
static inline int test_util_run(const struct test_case *tests, size_t n) {
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        printf("%s... ", tests[i].name);
        fflush(stdout);
        if (tests[i].fn()) {
            printf("OK\n");
        } else {
            printf("FALLO\n");
            failed++;
        }
    }
    printf("%d/%zu pruebas fallidas\n", failed, n);
    return failed;
}

#endif /* SOA_P3_TEST_UTIL_H */
