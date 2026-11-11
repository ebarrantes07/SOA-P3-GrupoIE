/* Reutilizado de SOA-P2-GrupoIE (src/clock.c), ver D0.5. */
#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t now_monotonic_ns(void) {
    struct timespec ts;

    /* Chequeo explícito y no un assert: esto es una llamada al sistema que
     * puede fallar en ejecución, no una precondición de quien programa. Sin
     * el chequeo, en modo release ts quedaría sin inicializar y esa basura
     * viajaría a las duraciones del log y de ahí a las mediciones del
     * informe: números corruptos sin un solo aviso. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        abort();
    }

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
