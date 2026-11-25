/* app_pi — aproximación distribuida de π y barreras reutilizables (#7).
 *
 * Cada nodo suma su tramo de la serie de Leibniz en una variable LOCAL y
 * publica una sola vez su suma parcial, dentro de un acceso WRITE, en su
 * propia posición de la página 0. Después de una barrera, el nodo 0 reúne las
 * sumas en orden de identificador y las compara con la serie secuencial de los
 * mismos T términos. Nadie actualiza un double compartido entre todos.
 *
 * DOS ERRORES QUE NO SE MEZCLAN. La diferencia contra la referencia
 * secuencial viene solo del ORDEN de la suma (los mismos términos, agrupados
 * distinto) y es la que se exige ≤ 1e-9. El error contra π es el truncamiento
 * de la serie a T términos, del orden de 1/T, y no se califica: se informa
 * para que el informe pueda separarlos.
 *
 * BARRERAS. Después de π, G generaciones del MISMO barrier_id. En cada una el
 * nodo escribe el número de generación en su posición de la página 1, pasa la
 * barrera y lee las de todos. Si alguien saliera antes de que llegaran todos,
 * vería una posición todavía en la generación anterior. Puede ver una ya en la
 * siguiente (un nodo que salió y escribió antes de que este leyera), y eso es
 * correcto, así que se acepta g o g+1 y nada más.
 *
 * Disposición de la región (necesita al menos 2 páginas):
 *   página 0: double partial[N]   suma parcial de cada nodo
 *   página 1: uint64_t gen[N]     última generación escrita por cada nodo
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_common.h"
#include "cli.h"
#include "clock.h"
#include "pi_series.h"

#define PAGE_SUMS 0u
#define PAGE_GENS 1u

#define BARRIER_START 0u
#define BARRIER_SUMS  1u
#define BARRIER_GEN   2u   /* el que se reutiliza G veces */

#define TOLERANCE 1e-9

/* π con más dígitos de los que distingue un double. M_PI no es C17. */
#define PI_REF 3.14159265358979323846264338327950288

struct pi_opts {
    uint64_t terms;
    uint32_t barriers;
};

static enum app_opt pi_opt(const char *flag, const char *v, void *arg) {
    struct pi_opts *o = arg;

    if (strcmp(flag, "--terms") == 0)
        return cli_parse_u64(v, 1, PI_MAX_TERMS, &o->terms)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    if (strcmp(flag, "--barriers") == 0)
        return cli_parse_u32(v, 0, 1000000u, &o->barriers)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    return APP_OPT_UNKNOWN;
}

static const char USAGE[] =
    "  --terms T          términos de la serie (por omisión 100003)\n"
    "  --barriers G       generaciones de barrera tras π (por omisión 100)\n";

/* Publica `value` en la posición `nid` de la página `page`. */
static bool publish(dsm_ctx *ctx, uint32_t page, uint32_t nid,
                    const void *value, size_t size) {
    unsigned char *p = DSM_acquire(ctx, page, DSM_WRITE);

    if (p == NULL)
        return false;
    memcpy(p + (size_t)nid * size, value, size);
    return DSM_release(ctx, page) == 0;
}

/* Las G generaciones. Devuelve APP_EXIT_OK, APP_EXIT_DSM o APP_EXIT_RESULT. */
static int run_barriers(dsm_ctx *ctx, const struct app_conn *c, uint32_t gens) {
    for (uint64_t g = 1; g <= gens; g++) {
        const unsigned char *p;
        uint64_t seen[DSM_MAX_NODES];

        if (!publish(ctx, PAGE_GENS, c->nid, &g, sizeof g))
            return app_fail(ctx, c->nid, "no se pudo publicar la generación");
        if (DSM_barrier(ctx, BARRIER_GEN) != 0)
            return app_fail(ctx, c->nid, "barrera de generación");

        p = DSM_acquire(ctx, PAGE_GENS, DSM_READ);
        if (p == NULL)
            return app_fail(ctx, c->nid, "no se pudo leer las generaciones");
        memcpy(seen, p, (size_t)c->nodes * sizeof seen[0]);
        if (DSM_release(ctx, PAGE_GENS) != 0)
            return app_fail(ctx, c->nid, "no se pudo liberar la lectura");

        for (uint32_t j = 0; j < c->nodes; j++) {
            if (seen[j] != g && seen[j] != g + 1u) {
                fprintf(stderr, "nodo %lu: FALLO: salida prematura en la "
                                "generación %llu: el nodo %lu va en la %llu\n",
                        (unsigned long)c->nid, (unsigned long long)g,
                        (unsigned long)j, (unsigned long long)seen[j]);
                return APP_EXIT_RESULT;
            }
        }
    }
    return APP_EXIT_OK;
}

/* El nodo 0 reúne y compara. Devuelve APP_EXIT_OK, _DSM o _RESULT. */
static int reduce_and_check(dsm_ctx *ctx, const struct app_conn *c,
                            uint64_t terms, double *pi_dist, double *pi_seq,
                            double *seq_ms) {
    const unsigned char *p;
    double partial[DSM_MAX_NODES];
    double sum = 0.0;
    uint64_t t0;

    p = DSM_acquire(ctx, PAGE_SUMS, DSM_READ);
    if (p == NULL)
        return app_fail(ctx, c->nid, "no se pudo leer las sumas parciales");
    memcpy(partial, p, (size_t)c->nodes * sizeof partial[0]);
    if (DSM_release(ctx, PAGE_SUMS) != 0)
        return app_fail(ctx, c->nid, "no se pudo liberar la lectura");

    /* En orden de identificador: el orden de la reducción es parte del
     * resultado, y fijarlo lo vuelve reproducible. */
    for (uint32_t j = 0; j < c->nodes; j++)
        sum += partial[j];
    *pi_dist = 4.0 * sum;

    t0 = now_monotonic_ns();
    *pi_seq = 4.0 * pi_leibniz_sum(0, terms);
    *seq_ms = app_ms(t0, now_monotonic_ns());

    if (!(fabs(*pi_dist - *pi_seq) <= TOLERANCE)) {
        fprintf(stderr, "nodo 0: FALLO: |π distribuido − π secuencial| = %.3e "
                        "supera %.0e\n", fabs(*pi_dist - *pi_seq), TOLERANCE);
        return APP_EXIT_RESULT;
    }
    return APP_EXIT_OK;
}

int main(int argc, char **argv) {
    struct app_conn c;
    struct pi_opts o = { 100003u, 100u };
    struct pi_range r;
    dsm_ctx *ctx;
    double local, pi_dist = 0.0, pi_seq = 0.0, seq_ms = 0.0;
    uint64_t t_start, t_calc0 = 0, t_calc1 = 0;
    int rc = APP_EXIT_OK;

    switch (app_parse(argc, argv, &c, pi_opt, &o, USAGE)) {
    case APP_PARSE_HELP:  return APP_EXIT_OK;
    case APP_PARSE_ERROR: return APP_EXIT_ARGS;
    case APP_PARSE_OK:    break;
    }

    t_start = now_monotonic_ns();
    ctx = app_connect(&c);
    if (ctx == NULL)
        return APP_EXIT_DSM;
    if (DSM_page_count(ctx) < 2u) {
        fprintf(stderr, "nodo %lu: app_pi necesita 2 páginas y la región "
                        "tiene %lu\n", (unsigned long)c.nid,
                (unsigned long)DSM_page_count(ctx));
        return app_finish(ctx, c.nid, APP_EXIT_ARGS);
    }

    /* Iguala el arranque, para que la fase de cálculo se mida igual en
     * todos los nodos. */
    if (DSM_barrier(ctx, BARRIER_START) != 0) {
        rc = app_fail(ctx, c.nid, "barrera de arranque");
        goto out;
    }

    t_calc0 = now_monotonic_ns();
    r = pi_partition(o.terms, c.nodes, c.nid);
    local = pi_leibniz_sum(r.first, r.count);
    if (!publish(ctx, PAGE_SUMS, c.nid, &local, sizeof local)) {
        rc = app_fail(ctx, c.nid, "no se pudo publicar la suma parcial");
        goto out;
    }
    if (DSM_barrier(ctx, BARRIER_SUMS) != 0) {
        rc = app_fail(ctx, c.nid, "barrera de sumas");
        goto out;
    }
    t_calc1 = now_monotonic_ns();

    printf("nodo %lu: términos [%llu, %llu) suma_parcial=%.17g\n",
           (unsigned long)c.nid, (unsigned long long)r.first,
           (unsigned long long)(r.first + r.count), local);

    if (c.nid == 0) {
        rc = reduce_and_check(ctx, &c, o.terms, &pi_dist, &pi_seq, &seq_ms);
        if (rc != APP_EXIT_OK)
            goto out;
    }

    rc = run_barriers(ctx, &c, o.barriers);

out:
    rc = app_finish(ctx, c.nid, rc);
    if (c.nid == 0 && rc == APP_EXIT_OK) {
        /* Una línea clave=valor, para que los experimentos de #9 la lean sin
         * adivinar formato. */
        printf("pi: nodos=%lu terminos=%llu pi_distribuido=%.17g "
               "pi_secuencial=%.17g dif_orden=%.3e err_truncamiento=%.3e "
               "barreras=%lu t_total_ms=%.3f t_calculo_ms=%.3f "
               "t_secuencial_ms=%.3f\n",
               (unsigned long)c.nodes, (unsigned long long)o.terms, pi_dist,
               pi_seq, fabs(pi_dist - pi_seq), fabs(pi_seq - PI_REF),
               (unsigned long)o.barriers,
               app_ms(t_start, now_monotonic_ns()), app_ms(t_calc0, t_calc1),
               seq_ms);
    }
    return rc;
}
