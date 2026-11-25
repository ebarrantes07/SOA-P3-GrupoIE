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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return false; \
        } \
    } while (0)

/* ---- Temporales que se borran pase lo que pase -------------------------
 *
 * Una prueba que crea un archivo con mkstemp y lo borra al final lo deja
 * atrás cuando FALLA, porque CHECK retorna antes de llegar al unlink. En C no
 * hay `defer`, así que el patrón "borrar al final" solo funciona en el camino
 * bueno.
 *
 * No es cosmético: desde #8 run_tests.sh comprueba que la suite no deje
 * recursos tomados, así que un temporal olvidado haría que UN fallo se
 * informara como dos problemas —la prueba que falló y una fuga— y el segundo
 * taparía al primero.
 *
 * Quien crea el temporal lo registra acá y se olvida: se borra al terminar el
 * proceso, haya pasado o fallado. Borrarlo igual en el camino bueno sigue
 * estando bien; un unlink repetido falla en silencio. */
#define TEST_TMP_MAX 32

static char  test_tmp_paths[TEST_TMP_MAX][256];
static size_t test_tmp_n = 0;

static void test_tmp_cleanup(void) {
    for (size_t i = 0; i < test_tmp_n; i++)
        (void)unlink(test_tmp_paths[i]);
}

static inline void test_tmp_keep(const char *path) {
    static bool registrado = false;

    if (path == NULL || test_tmp_n >= TEST_TMP_MAX ||
        strlen(path) >= sizeof test_tmp_paths[0])
        return;
    if (!registrado) {
        if (atexit(test_tmp_cleanup) != 0)
            return;
        registrado = true;
    }
    snprintf(test_tmp_paths[test_tmp_n++], sizeof test_tmp_paths[0], "%s",
             path);
}

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
