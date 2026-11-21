/* test_e2e_coherence.c — el caso de coherencia de tres nodos (#6).
 *
 * Es el criterio del issue, palabra por palabra: "los lectores concurrentes
 * coexisten, los ACK anteceden al grant de escritura en el log, y la lectura
 * posterior observa los datos nuevos". Las tres cosas se verifican acá, dos de
 * ellas sobre el CSV del coordinador porque son afirmaciones sobre el ORDEN de
 * lo que pasó y no sobre el resultado final.
 *
 * Por qué es una prueba de C y no un script como el contador: hay que controlar
 * CUÁNDO actúa cada nodo —dos lectores a la vez, el escritor después— y eso
 * necesita procesos que compartan el código de la prueba. Las aplicaciones de
 * #7 harán esto mismo desde afuera.
 *
 * EL PAPEL DE LAS ESPERAS. Los `proc_sleep_ms` CREAN el escenario: los lectores
 * retienen la página un rato y el escritor pide a un tercio de ese rato, así su
 * solicitud llega mientras ellos la tienen. Pero no se VERIFICA nada con
 * tiempos: lo que se verifica es el orden de las filas del registro, y una de
 * las afirmaciones es justamente que el solapamiento ocurrió. Si las esperas
 * no alcanzaran en una máquina lenta, la prueba FALLA en vez de pasar sin haber
 * probado nada, que es la trampa de este tipo de prueba.
 */
#include "dsm.h"
#include "test_csv.h"
#include "test_proc.h"
#include "test_util.h"

#include <string.h>

#define NODOS        3u
#define PAGINAS      1u
#define TIMEOUT_MS   5000u    /* holgado: acá se mide orden, no latencia */
#define WAIT_MS      30000u   /* tope para que un proceso termine */
#define RETENCION_MS 300u     /* cuánto retienen la página los lectores */
#define VALOR_NUEVO  0xC0FFEEull

/* En un hijo: si falla, lo dice por stderr (sin buffer) y termina con 1. */
#define WCHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FALLO (nodo %u) %s:%d: %s\n", nid, __FILE__, \
                    __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

static int coherence_node(uint16_t port, uint32_t nid, void *arg) {
    struct dsm_options o = { TIMEOUT_MS, (const char *)arg };
    dsm_ctx *c = DSM_init("127.0.0.1", port, NODOS, nid, &o, NULL);
    uint64_t *p;

    WCHECK(c != NULL);

    /* Iguala el arranque: sin esto un lector podría adquirir y liberar antes
     * de que el otro llegue, y entonces nunca coexistirían. */
    WCHECK(DSM_barrier(c, 0) == 0);

    if (nid < 2) {
        /* LECTORES. Adquieren y se quedan con la página: entre ellos para
         * coexistir, y respecto del escritor para que su solicitud llegue
         * mientras la tienen. */
        p = DSM_acquire(c, 0, DSM_READ);
        WCHECK(p != NULL);
        WCHECK(*p == 0);                 /* la región arranca en cero */
        proc_sleep_ms(RETENCION_MS);
        WCHECK(DSM_release(c, 0) == 0);
    } else {
        /* ESCRITOR. Pide después de que los lectores tengan la página, así su
         * ACQ_REQ se encola: el coordinador no puede concederlo hasta que los
         * dos liberen Y confirmen la invalidación de sus copias retenidas. */
        proc_sleep_ms(RETENCION_MS / 3u);
        p = DSM_acquire(c, 0, DSM_WRITE);
        WCHECK(p != NULL);
        WCHECK(*p == 0);                 /* nadie escribió todavía */
        *p = VALOR_NUEVO;
        WCHECK(DSM_release(c, 0) == 0);
    }

    /* Desde acá la escritura está publicada para todos. */
    WCHECK(DSM_barrier(c, 1) == 0);

    /* LA LECTURA POSTERIOR. Los tres tienen que ver el dato nuevo, incluidos
     * los dos cuya copia fue invalidada: si alguno sirviera su copia vieja sin
     * pedir permiso, acá se vería. */
    p = DSM_acquire(c, 0, DSM_READ);
    WCHECK(p != NULL);
    WCHECK(*p == VALOR_NUEVO);
    WCHECK(DSM_release(c, 0) == 0);

    WCHECK(DSM_finalize(c) == 0);
    return 0;
}

/* ¿Los dos lectores coexistían en el momento en que el escritor pidió?
 *
 * Se lee de UNA fila: la del ACQ_REQ del escritor, que lleva el estado del
 * directorio tal como estaba al procesarla. Si ahí readers_mask vale 0x03, en
 * ese instante los nodos 0 y 1 tenían permiso de lectura a la vez. Eso cubre
 * las dos mitades del criterio de un tiro: que los lectores concurrentes
 * coexistan, y que la solicitud de escritura haya llegado mientras lo hacían.
 *
 * Comparar posiciones de filas —el ACQ_REQ antes de los REL_READ— probaría lo
 * mismo con más pasos y suponiendo que el orden del archivo es el orden real.
 * La máscara no supone nada: es el estado medido.
 *
 * Se exige la máscara y no un conteo de 2 para que no se cumpla por accidente
 * con otra pareja de nodos. Y es ESTA la afirmación que impide que la prueba
 * pase sin haber probado nada: si las esperas no produjeran el solapamiento en
 * una máquina lenta, acá la máscara sería 0x00 y la prueba falla. */
static bool lectores_coexisten_cuando_el_escritor_pide(const struct csv *c) {
    char ev[32], nd[16], mode[8], mask[16], cuenta[16];

    for (size_t i = 0; i < c->n; i++) {
        field(c->rows[i], F_EVENT, ev, sizeof ev);
        field(c->rows[i], F_NODE, nd, sizeof nd);
        field(c->rows[i], F_MODE, mode, sizeof mode);
        if (strcmp(ev, "ACQ_REQ") != 0 || strcmp(nd, "2") != 0 ||
            strcmp(mode, "WRITE") != 0)
            continue;
        field(c->rows[i], F_MASK, mask, sizeof mask);
        field(c->rows[i], F_READERS, cuenta, sizeof cuenta);
        if (strcmp(mask, "0x03") == 0 && strcmp(cuenta, "2") == 0)
            return true;
        fprintf(stderr, "  al pedir la escritura habia %s lectores (mascara "
                        "%s), se esperaban los nodos 0 y 1\n", cuenta, mask);
        return false;
    }
    fprintf(stderr, "  no hay ninguna solicitud de escritura del nodo 2\n");
    return false;
}

/* ¿Los ACK de invalidación anteceden al grant de escritura? Es la garantía
 * dura: el coordinador no entrega la exclusividad hasta que las dos copias
 * retenidas estén confirmadas como inválidas. Sin esto, el escritor escribiría
 * sobre datos que otro todavía cree vigentes. */
static bool acks_antes_del_grant(const struct csv *c) {
    size_t ack0 = csv_first(c, "INVALIDATE_ACK", "0");
    size_t ack1 = csv_first(c, "INVALIDATE_ACK", "1");
    size_t grant = 0;
    char ev[32], nd[16], mode[8];

    /* El grant de ESCRITURA del nodo 2, no su lectura posterior. */
    for (size_t i = 0; i < c->n; i++) {
        field(c->rows[i], F_EVENT, ev, sizeof ev);
        field(c->rows[i], F_NODE, nd, sizeof nd);
        field(c->rows[i], F_MODE, mode, sizeof mode);
        if (strcmp(ev, "ACQ_GRANT") == 0 && strcmp(nd, "2") == 0 &&
            strcmp(mode, "WRITE") == 0) {
            grant = i + 1;
            break;
        }
    }
    if (ack0 == 0 || ack1 == 0 || grant == 0 || ack0 > grant || ack1 > grant) {
        fprintf(stderr, "  ACK en %zu y %zu, grant de escritura en %zu\n",
                ack0, ack1, grant);
        return false;
    }
    return true;
}

static bool test_three_nodes_two_readers_then_a_writer(void) {
    char clog[] = "/tmp/e2e_coherence_XXXXXX";
    struct proc_coord co;
    struct csv c;
    pid_t w[NODOS];
    bool ok = true;
    int fd = mkstemp(clog);

    CHECK(fd >= 0);
    close(fd);
    test_tmp_keep(clog);
    CHECK(proc_coord_start(&co, NODOS, PAGINAS, TIMEOUT_MS, clog));
    /* Los trabajadores no escriben registro propio: todo lo que esta prueba
     * afirma sobre el orden está en el del coordinador, que es el único que ve
     * a los tres nodos en una sola secuencia. */
    for (uint32_t i = 0; i < NODOS; i++)
        w[i] = proc_worker(coherence_node, co.port, i, NULL);

    /* Se espera a TODOS, sin cortar en el primero que falle, y después se
     * juzga. No es solo por el informe: `proc_wait` mata y recoge al proceso
     * que no termine dentro del tope, así que recorrer la lista entera es lo
     * que garantiza que no quede ninguno vivo. Cortando en el primer fallo se
     * incumplía el criterio de #6 —"no queda ningún proceso"— precisamente
     * cuando la prueba falla, que es cuando más importa. Lo encontró Isaac
     * revisando el #16.
     *
     * El valor nuevo lo verifica cada nodo en su propio proceso, así que un
     * código distinto de 0 ya dice que alguien no lo vio. */
    for (uint32_t i = 0; i < NODOS; i++) {
        if (proc_wait(w[i], WAIT_MS) != 0) {
            fprintf(stderr, "  el trabajador %lu no terminó en 0\n",
                    (unsigned long)i);
            ok = false;
        }
    }
    /* El coordinador sale en 0 solo si los tres se despidieron. */
    if (proc_wait(co.pid, WAIT_MS) != 0) {
        fprintf(stderr, "  el coordinador no terminó en 0\n");
        ok = false;
    }

    /* El registro se juzga solo si la corrida llegó a término: sobre un CSV
     * truncado las afirmaciones dirían cosas confusas de algo que ya se sabe
     * mal, y taparían el motivo real. */
    if (ok) {
        if (csv_load(clog, &c)) {
            ok = lectores_coexisten_cuando_el_escritor_pide(&c) &&
                 acks_antes_del_grant(&c);
            csv_free(&c);
        } else {
            fprintf(stderr, "  no se pudo leer el registro del coordinador\n");
            ok = false;
        }
    }

    unlink(clog);   /* en todos los caminos, también al fallar */
    return ok;
}

int main(void) {
    static const struct test_case tests[] = {
        { "three_nodes_two_readers_then_a_writer",
          test_three_nodes_two_readers_then_a_writer },
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
