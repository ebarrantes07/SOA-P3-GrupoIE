/* test_e2e_dsm.c — libdsm contra el dsm_manager real (#5).
 *
 * test_dsm.c prueba la biblioteca contra un coordinador falso, que da el
 * momento exacto de cada trama. Esta prueba hace lo complementario: procesos
 * trabajadores de verdad, cada uno con su contexto y su hilo receptor, contra
 * el binario ./dsm_manager de #4. Lo que se verifica acá es lo que solo
 * aparece con los dos lados reales: que una invalidación se confirme con la
 * aplicación bloqueada en una barrera y el coordinador conceda recién después
 * de ese ACK, que la caída del coordinador o de otro trabajador despierte a
 * quien espera, y que los registros de los dos lados se correspondan fila a
 * fila.
 *
 * El contador N×K y el caso de coherencia de tres nodos son de #6, con las
 * aplicaciones; acá no se repiten.
 *
 * Los plazos son cortos para que la suite no tarde: lo que se verifica es que
 * el error llegue dentro del plazo más los 2 s de limpieza del enunciado, no
 * que el plazo valga 10 s.
 */
#include "dsm.h"
#include "test_proc.h"
#include "test_util.h"

#include <string.h>

#define TIMEOUT_MS  3000u     /* plazo de la biblioteca y del coordinador */
#define MARGIN_MS   2000u     /* limpieza que concede el enunciado */
#define WAIT_MS     20000u    /* tope para que un proceso termine */

/* En un hijo: si falla, lo dice por stderr (sin buffer) y termina con 1. */
#define WCHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FALLO (nodo %u) %s:%d: %s\n", nid, __FILE__, \
                    __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

static dsm_ctx *join(uint16_t port, uint32_t nodes, uint32_t nid,
                     uint32_t timeout_ms, const char *log_path) {
    struct dsm_options o = { timeout_ms, log_path };
    return DSM_init("127.0.0.1", port, nodes, nid, &o, NULL);
}

/* ---- Invalidación con la aplicación en una barrera ---------------------- */

/* El nodo 0 se queda con una copia de la página 0 y entra a la barrera 1. El
 * nodo 1 espera un momento —para que el 0 ya esté bloqueado en la barrera—
 * y escribe la página: el coordinador tiene que invalidar la copia del 0 y no
 * puede conceder hasta tener su ACK. Si el receptor no confirmara, el nodo 1
 * no recibiría la concesión nunca y la prueba vencería. */
static int inval_node(uint16_t port, uint32_t nid, void *arg) {
    dsm_ctx *c = join(port, 2, nid, TIMEOUT_MS, (const char *)arg);
    uint64_t *p;

    WCHECK(c != NULL);
    if (nid == 0) {
        p = DSM_acquire(c, 0, DSM_READ);
        WCHECK(p != NULL && *p == 0u);
        WCHECK(DSM_release(c, 0) == 0);
        WCHECK(DSM_barrier(c, 0) == 0);
        WCHECK(DSM_barrier(c, 1) == 0);    /* acá llega la invalidación */
        p = DSM_acquire(c, 0, DSM_READ);
        WCHECK(p != NULL && *p == 42u);    /* el dato nuevo, no la copia */
        WCHECK(DSM_release(c, 0) == 0);
    } else {
        WCHECK(DSM_barrier(c, 0) == 0);
        proc_sleep_ms(300);
        p = DSM_acquire(c, 0, DSM_WRITE);
        WCHECK(p != NULL);
        *p = 42u;
        WCHECK(DSM_release(c, 0) == 0);
        WCHECK(DSM_barrier(c, 1) == 0);
    }
    WCHECK(DSM_finalize(c) == 0);
    return 0;
}

/* Busca en el CSV del coordinador la invalidación al nodo 0 y comprueba que
 * su ACK aparece antes de la concesión WRITE al nodo 1. */
static bool ack_precedes_write_grant(const char *path) {
    char line[512];
    FILE *f = fopen(path, "r");
    long inval = -1, ack = -1, grant = -1, n = 0;

    CHECK(f != NULL);
    while (fgets(line, sizeof line, f) != NULL) {
        char *col[8];
        char *s = line;
        int k = 0;

        n++;
        while (k < 8 && s != NULL) {
            col[k++] = s;
            s = strchr(s, ',');
            if (s != NULL)
                *s++ = '\0';
        }
        if (k < 8)
            continue;
        /* col[3] node_id, col[6] event */
        if (inval < 0 && strcmp(col[6], "INVALIDATE") == 0 && strcmp(col[3], "0") == 0)
            inval = n;
        else if (inval > 0 && ack < 0 && strcmp(col[6], "INVALIDATE_ACK") == 0 &&
                 strcmp(col[3], "0") == 0)
            ack = n;
        else if (inval > 0 && grant < 0 && strcmp(col[6], "ACQ_GRANT") == 0 &&
                 strcmp(col[3], "1") == 0)
            grant = n;
    }
    fclose(f);
    if (!(inval > 0 && ack > inval && grant > ack))
        fprintf(stderr, "  INVALIDATE en %ld, ACK en %ld, GRANT en %ld\n",
                inval, ack, grant);
    CHECK(inval > 0 && ack > inval && grant > ack);
    return true;
}

static bool test_invalidation_acked_while_in_barrier(void) {
    char clog[] = "/tmp/e2e_dsm_coord_XXXXXX";
    struct proc_coord co;
    pid_t w[2];
    int fd = mkstemp(clog);

    CHECK(fd >= 0);
    close(fd);
    CHECK(proc_coord_start(&co, 2, 2, TIMEOUT_MS, clog));
    for (uint32_t i = 0; i < 2; i++)
        w[i] = proc_worker(inval_node, co.port, i, NULL);
    CHECK(proc_wait(w[0], WAIT_MS) == 0);
    CHECK(proc_wait(w[1], WAIT_MS) == 0);
    CHECK(proc_wait(co.pid, WAIT_MS) == 0);       /* sesión terminada en orden */
    CHECK(ack_precedes_write_grant(clog));
    unlink(clog);
    return true;
}

/* ---- Caídas -------------------------------------------------------------- */

/* Los dos nodos esperan en barreras distintas, que nunca se completan. Lo que
 * los saca es la caída del coordinador, no su plazo: el plazo es largo. */
static int barrier_forever(uint16_t port, uint32_t nid, void *arg) {
    dsm_ctx *c = join(port, 2, nid, 10u * TIMEOUT_MS, NULL);
    enum dsm_status st;

    (void)arg;
    WCHECK(c != NULL);
    WCHECK(DSM_barrier(c, 10u + nid) == -1);
    st = DSM_last_error(c);
    WCHECK(st == DSM_ECONN || st == DSM_EABORT);
    WCHECK(DSM_finalize(c) == -1);
    return 0;
}

static bool test_coordinator_death_wakes_barrier(void) {
    struct proc_coord co;
    pid_t w[2];
    uint64_t t0;

    CHECK(proc_coord_start(&co, 2, 1, 10u * TIMEOUT_MS, NULL));
    for (uint32_t i = 0; i < 2; i++)
        w[i] = proc_worker(barrier_forever, co.port, i, NULL);
    proc_sleep_ms(500);
    t0 = now_monotonic_ns();
    proc_kill(co.pid);                    /* SIGKILL: no alcanza a avisar */
    CHECK(proc_wait(w[0], WAIT_MS) == 0);
    CHECK(proc_wait(w[1], WAIT_MS) == 0);
    CHECK((now_monotonic_ns() - t0) / 1000000u < MARGIN_MS);
    return true;
}

/* El nodo 1 toma la página 0 para escribir y se queda colgado con ella; el
 * nodo 0 la pide y queda esperando. Cuando el 1 muere, el coordinador aborta
 * la sesión y el 0 despierta con error. */
static int holder_or_waiter(uint16_t port, uint32_t nid, void *arg) {
    dsm_ctx *c = join(port, 2, nid, 10u * TIMEOUT_MS, NULL);
    enum dsm_status st;

    (void)arg;
    WCHECK(c != NULL);
    if (nid == 1) {
        WCHECK(DSM_acquire(c, 0, DSM_WRITE) != NULL);
        for (;;)
            pause();                      /* hasta el SIGKILL */
    }
    proc_sleep_ms(300);                   /* que el 1 ya la tenga */
    WCHECK(DSM_acquire(c, 0, DSM_READ) == NULL);
    st = DSM_last_error(c);
    WCHECK(st == DSM_ECONN || st == DSM_EABORT);
    WCHECK(DSM_finalize(c) == -1);
    return 0;
}

static bool test_killed_worker_wakes_the_waiter(void) {
    struct proc_coord co;
    pid_t w[2];
    uint64_t t0;

    CHECK(proc_coord_start(&co, 2, 1, 10u * TIMEOUT_MS, NULL));
    for (uint32_t i = 0; i < 2; i++)
        w[i] = proc_worker(holder_or_waiter, co.port, i, NULL);
    proc_sleep_ms(800);
    t0 = now_monotonic_ns();
    proc_kill(w[1]);
    CHECK(proc_wait(w[0], WAIT_MS) == 0);
    CHECK((now_monotonic_ns() - t0) / 1000000u < MARGIN_MS);
    CHECK(proc_wait(co.pid, WAIT_MS) != 0);    /* sesión abortada */
    return true;
}

/* ---- Registros de los dos lados ----------------------------------------- */

#define ROUNDS 20u

static int counter_node(uint16_t port, uint32_t nid, void *arg) {
    char path[128];
    dsm_ctx *c;

    snprintf(path, sizeof path, "%s.w%u", (const char *)arg, nid);
    c = join(port, 2, nid, TIMEOUT_MS, path);
    WCHECK(c != NULL);
    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint64_t *p = DSM_acquire(c, 0, DSM_WRITE);
        WCHECK(p != NULL);
        (*p)++;
        WCHECK(DSM_release(c, 0) == 0);
    }
    WCHECK(DSM_barrier(c, 0) == 0);
    WCHECK(DSM_finalize(c) == 0);
    return 0;
}

struct csv {
    char   header[512];
    char **rows;
    size_t n;
};

static bool csv_load(const char *path, struct csv *out) {
    char line[512];
    FILE *f = fopen(path, "r");
    size_t cap = 0;

    memset(out, 0, sizeof *out);
    if (f == NULL || fgets(out->header, sizeof out->header, f) == NULL) {
        if (f != NULL)
            fclose(f);
        return false;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (out->n == cap) {
            cap = cap ? cap * 2 : 64;
            out->rows = realloc(out->rows, cap * sizeof *out->rows);
        }
        out->rows[out->n++] = strdup(line);
    }
    fclose(f);
    return true;
}

static void csv_free(struct csv *c) {
    for (size_t i = 0; i < c->n; i++)
        free(c->rows[i]);
    free(c->rows);
}

/* Columna k de una fila, copiada a `out`. */
static void field(const char *row, int k, char *out, size_t cap) {
    const char *s = row;

    for (int i = 0; i < k && s != NULL; i++) {
        s = strchr(s, ',');
        if (s != NULL)
            s++;
    }
    if (s == NULL) {
        out[0] = '\0';
        return;
    }
    size_t n = strcspn(s, ",\n");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

enum { F_RUN = 0, F_COMP = 1, F_NODE = 3, F_REQ = 4, F_PAGE = 5, F_EVENT = 6,
       F_VERSION = 9 };

/* ¿Tiene el coordinador una fila `event` con ese nodo y request_id, y la
 * misma versión si se pide? */
static bool coord_has(const struct csv *co, const char *event, const char *node,
                      const char *req, const char *version) {
    char ev[32], nd[16], rq[32], ver[32];

    for (size_t i = 0; i < co->n; i++) {
        field(co->rows[i], F_EVENT, ev, sizeof ev);
        if (strcmp(ev, event) != 0)
            continue;
        field(co->rows[i], F_NODE, nd, sizeof nd);
        field(co->rows[i], F_REQ, rq, sizeof rq);
        if (strcmp(nd, node) != 0 || strcmp(rq, req) != 0)
            continue;
        if (version == NULL)
            return true;
        field(co->rows[i], F_VERSION, ver, sizeof ver);
        return strcmp(ver, version) == 0;
    }
    return false;
}

/* El mismo formato y el mismo run_id en los tres archivos, y cada solicitud,
 * concesión y publicación del trabajador tiene su fila en el coordinador con
 * el mismo (node_id, request_id) (D0.12), y la publicación con la misma
 * versión. */
static bool test_logs_correlate_by_node_and_request(void) {
    char base[] = "/tmp/e2e_dsm_logs_XXXXXX";
    char path[128], run[32] = "", r2[32];
    struct proc_coord co;
    struct csv cl, wl;
    pid_t w[2];
    size_t matched = 0;
    int fd = mkstemp(base);

    CHECK(fd >= 0);
    close(fd);
    CHECK(proc_coord_start(&co, 2, 1, TIMEOUT_MS, base));
    for (uint32_t i = 0; i < 2; i++)
        w[i] = proc_worker(counter_node, co.port, i, base);
    CHECK(proc_wait(w[0], WAIT_MS) == 0);
    CHECK(proc_wait(w[1], WAIT_MS) == 0);
    CHECK(proc_wait(co.pid, WAIT_MS) == 0);

    CHECK(csv_load(base, &cl));
    CHECK(cl.n > 0);
    field(cl.rows[0], F_RUN, run, sizeof run);

    for (uint32_t i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s.w%u", base, i);
        CHECK(csv_load(path, &wl));
        CHECK(strcmp(wl.header, cl.header) == 0);      /* mismo formato */
        for (size_t r = 0; r < wl.n; r++) {
            char ev[32], nd[16], rq[32], ver[32], comp[32];

            field(wl.rows[r], F_RUN, r2, sizeof r2);
            field(wl.rows[r], F_COMP, comp, sizeof comp);
            CHECK(strcmp(r2, run) == 0);
            CHECK(strcmp(comp, "trabajador") == 0);
            field(wl.rows[r], F_EVENT, ev, sizeof ev);
            field(wl.rows[r], F_NODE, nd, sizeof nd);
            field(wl.rows[r], F_REQ, rq, sizeof rq);
            field(wl.rows[r], F_VERSION, ver, sizeof ver);
            if (strcmp(ev, "ACQ_REQ") == 0 || strcmp(ev, "ACQ_GRANT") == 0 ||
                strcmp(ev, "REL_WRITE") == 0) {
                CHECK(coord_has(&cl, ev, nd, rq, NULL));
                matched++;
            }
            if (strcmp(ev, "WRITE_PUBLISHED") == 0) {
                CHECK(coord_has(&cl, ev, nd, rq, ver));
                matched++;
            }
        }
        csv_free(&wl);
        unlink(path);
    }
    csv_free(&cl);
    unlink(base);
    CHECK(matched == 2u * ROUNDS * 4u);    /* cuatro filas por incremento */
    return true;
}

int main(void) {
    static const struct test_case tests[] = {
        { "invalidation_acked_while_in_barrier",   test_invalidation_acked_while_in_barrier },
        { "coordinator_death_wakes_barrier",       test_coordinator_death_wakes_barrier },
        { "killed_worker_wakes_the_waiter",        test_killed_worker_wakes_the_waiter },
        { "logs_correlate_by_node_and_request",    test_logs_correlate_by_node_and_request },
    };
    signal(SIGPIPE, SIG_IGN);
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
