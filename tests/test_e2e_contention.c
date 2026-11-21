/* test_e2e_contention.c — competencia por una página, 1000 rondas (#8).
 *
 * Cuatro nodos alternando lecturas y escrituras sobre la MISMA página. El
 * criterio del issue pide tres cosas: que no haya escritores simultáneos, que
 * no haya valores incoherentes, y que todos los nodos progresen. Cada una se
 * comprueba donde se puede comprobar de verdad:
 *
 *   - Escritores simultáneos: NO se comprueba acá. Desde dentro de un nodo no
 *     se ve quién más tiene permiso; lo único que se vería es el efecto, y el
 *     efecto se puede perder. Lo comprueba `verificar_invariantes.py` sobre el
 *     registro del coordinador, que es quien ve a los cuatro a la vez (P1).
 *   - Valores incoherentes: sí acá, con el invariante de más abajo.
 *   - Que todos progresen: cada nodo hace un número EXACTO de rondas, así que
 *     terminar con código 0 ya lo demuestra. Un nodo al que se le negara el
 *     turno vencería su plazo y saldría con error.
 *
 * EL INVARIANTE QUE DETECTA UN VALOR INCOHERENTE
 *
 * La página lleva un total y un aporte por nodo. Cada escritura incrementa los
 * dos DENTRO del mismo intervalo exclusivo, así que en todo momento vale:
 *
 *     total == suma de los aportes
 *
 * Eso es más fuerte que mirar solo el total. Un total que no cuadra con la
 * suma solo puede venir de que nos sirvieron una página a medias, una versión
 * vieja mezclada con una nueva, o una escritura perdida en el medio: cosas
 * que un contador suelto no distingue de "todavía no llegó". Y se comprueba al
 * ENTRAR a cada acceso, también en los de escritura: si lo que nos sirvieron
 * ya no cuadra, el problema es anterior a nosotros.
 *
 * Al final, con una barrera de por medio, el total tiene que ser exactamente
 * NODOS * (RONDAS/2) y cada aporte RONDAS/2, porque la alternancia es
 * determinista. Eso convierte "no se perdió nada" en una igualdad y no en una
 * estimación.
 *
 * La página se usa como memoria, igual que en app_counter (D5.1): un struct de
 * uint64_t no lleva relleno, el marco sale de calloc y la página mide 4096,
 * múltiplo de 8, así que su inicio sirve para el struct.
 */
#include "dsm.h"
#include "test_csv.h"
#include "test_proc.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

#define NODOS       4u
#define PAGINAS     1u
#define RONDAS      1000u      /* por nodo; par, para que la mitad escriba */
#define PAGINA      0u
#define TIMEOUT_MS  20000u     /* holgado: acá se mide corrección, no latencia */
#define WAIT_MS     120000u    /* tope para que un proceso termine */

/* En un hijo: si falla, lo dice por stderr (sin buffer) y termina con 1. */
#define WCHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FALLO (nodo %lu) %s:%d: %s\n", \
                    (unsigned long)nid, __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

struct compartido {
    uint64_t total;
    uint64_t aporte[DSM_MAX_NODES];
};

static bool suma_cuadra(const struct compartido *s) {
    uint64_t suma = 0;

    for (uint32_t i = 0; i < DSM_MAX_NODES; i++)
        suma += s->aporte[i];
    return suma == s->total;
}

static int contention_node(uint16_t port, uint32_t nid, void *arg) {
    struct dsm_options o = { TIMEOUT_MS, (const char *)arg };
    dsm_ctx *c = DSM_init("127.0.0.1", port, NODOS, nid, &o, NULL);
    uint64_t escrituras = 0, lecturas = 0;

    WCHECK(c != NULL);
    WCHECK(DSM_page_count(c) > PAGINA);

    /* Iguala el arranque: sin esto los primeros nodos harían sus rondas sin
     * nadie con quien competir, y la prueba mediría otra cosa (D5.1). */
    WCHECK(DSM_barrier(c, 0) == 0);

    for (uint32_t r = 0; r < RONDAS; r++) {
        /* La alternancia depende de la ronda Y del nodo, así que en cada
         * momento hay nodos queriendo escribir y nodos queriendo leer. Con
         * `r % 2` solos todos harían lo mismo a la vez y los lectores nunca
         * coexistirían con nada. */
        if (((r + nid) % 2u) == 0u) {
            struct compartido *s = DSM_acquire(c, PAGINA, DSM_WRITE);

            WCHECK(s != NULL);
            WCHECK(suma_cuadra(s));
            s->total++;
            s->aporte[nid]++;
            escrituras++;
            WCHECK(DSM_release(c, PAGINA) == 0);
        } else {
            const struct compartido *s = DSM_acquire(c, PAGINA, DSM_READ);

            WCHECK(s != NULL);
            WCHECK(suma_cuadra(s));
            lecturas++;
            WCHECK(DSM_release(c, PAGINA) == 0);
        }
    }

    /* Hasta que no terminen todos, el total no está completo. */
    WCHECK(DSM_barrier(c, 1) == 0);

    {
        const struct compartido *s = DSM_acquire(c, PAGINA, DSM_READ);
        uint64_t esperado = (uint64_t)NODOS * (RONDAS / 2u);

        WCHECK(s != NULL);
        WCHECK(suma_cuadra(s));
        WCHECK(s->total == esperado);
        for (uint32_t i = 0; i < NODOS; i++)
            WCHECK(s->aporte[i] == RONDAS / 2u);
        WCHECK(DSM_release(c, PAGINA) == 0);
    }

    WCHECK(escrituras == RONDAS / 2u);
    WCHECK(lecturas == RONDAS - RONDAS / 2u);
    fprintf(stderr, "  nodo %lu: %llu escrituras, %llu lecturas\n",
            (unsigned long)nid, (unsigned long long)escrituras,
            (unsigned long long)lecturas);

    WCHECK(DSM_finalize(c) == 0);
    return 0;
}

/* Corre el verificador sobre el registro. Devuelve:
 *    1  verificó y pasó
 *    0  verificó y encontró una violación
 *   -1  no se pudo correr la herramienta
 *
 * El -1 no es un fallo de la prueba: es el caso que el criterio de #8 pide
 * tratar aparte —"si una herramienta no funciona en el entorno, se registra el
 * diagnóstico y se presenta una verificación alternativa, sin afirmar que
 * pasó"—. Acá la verificación alternativa son las comprobaciones que cada nodo
 * hizo en su propio proceso, que siguen valiendo; lo que no se puede afirmar
 * sin la herramienta es que no hubo escritores simultáneos. */
static int correr_verificador(const char *log)
{
    static const char *const script = "scripts/verificar_invariantes.py";
    char cmd[512];
    FILE *f;

    /* Las dos cosas que pueden faltar se distinguen de una violación, porque
     * se informan distinto. La ruta es relativa a la raíz del repositorio,
     * igual que el `./dsm_manager` de test_proc.h: la suite ya se corre desde
     * ahí. Si no se encuentra, es un problema de entorno y no del protocolo,
     * y confundirlos haría que un `cd` mal puesto pareciera un defecto. */
    f = fopen(script, "r");
    if (f == NULL)
        return -1;
    (void)fclose(f);
    if (system("python3 --version > /dev/null 2>&1") != 0)
        return -1;

    (void)snprintf(cmd, sizeof cmd, "python3 %s '%s'", script, log);
    return system(cmd) == 0 ? 1 : 0;
}

static bool test_four_nodes_alternating_for_a_thousand_rounds(void) {
    char clog[] = "/tmp/e2e_contention_XXXXXX";
    struct proc_coord co;
    pid_t w[NODOS];
    bool ok = true;
    int fd = mkstemp(clog);

    CHECK(fd >= 0);
    close(fd);
    CHECK(proc_coord_start(&co, NODOS, PAGINAS, TIMEOUT_MS, clog));
    for (uint32_t i = 0; i < NODOS; i++)
        w[i] = proc_worker(contention_node, co.port, i, NULL);

    /* Se espera a TODOS antes de juzgar: proc_wait mata y recoge al que no
     * termine, así que recorrer la lista entera es lo que garantiza que no
     * sobreviva ninguno aunque la prueba falle (#16). */
    for (uint32_t i = 0; i < NODOS; i++) {
        if (proc_wait(w[i], WAIT_MS) != 0) {
            fprintf(stderr, "  el nodo %lu no completó sus rondas\n",
                    (unsigned long)i);
            ok = false;
        }
    }
    if (proc_wait(co.pid, WAIT_MS) != 0) {
        fprintf(stderr, "  el coordinador no terminó en 0\n");
        ok = false;
    }

    /* P1 —sin escritores simultáneos— se comprueba desde el registro, que es
     * el único lugar donde se ve a los cuatro nodos en una sola secuencia. */
    if (ok) {
        switch (correr_verificador(clog)) {
        case 1:
            break;
        case 0:
            fprintf(stderr, "  el verificador encontró una violación\n");
            ok = false;
            break;
        default:
            fprintf(stderr,
                    "  AVISO: falta python3 o el script, así que los\n"
                    "         invariantes del registro NO se verificaron.\n"
                    "         Lo que sí vale: cada nodo comprobó la coherencia\n"
                    "         de lo que leyó y completó sus rondas exactas.\n"
                    "         Queda sin verificar que no hubo escritores\n"
                    "         simultáneos; eso se ve solo desde el registro.\n");
            break;
        }
    }

    unlink(clog);
    return ok;
}

int main(void) {
    static const struct test_case tests[] = {
        { "four_nodes_alternating_for_a_thousand_rounds",
          test_four_nodes_alternating_for_a_thousand_rounds },
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
