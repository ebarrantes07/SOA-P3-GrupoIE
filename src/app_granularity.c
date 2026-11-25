/* app_granularity — el mismo trabajo con una página compartida o una por nodo
 * (#7).
 *
 * Cada nodo incrementa SU contador R veces. Los contadores son
 * independientes desde la lógica de la aplicación; lo único que cambia entre
 * los dos diseños es dónde viven:
 *
 *   shared     todos en la página 0, el del nodo i en el desplazamiento 8·i
 *   separate   el del nodo i en la página i, desplazamiento 0
 *
 * Con `shared`, cada escritura invalida la copia de los demás aunque ninguno
 * lea el contador ajeno: es false sharing a la escala de la página DSM. Con
 * `separate` nadie toca la página de otro y no hay invalidaciones.
 *
 * EL MISMO TRABAJO EN LOS DOS. R adquisiciones WRITE y R incrementos por
 * nodo, y una sola adquisición READ de verificación al final. Las rondas se
 * coordinan con una barrera cada una, para que el patrón de acceso sea
 * comparable entre diseños y no dependa de quién arrancó antes. Esas barreras
 * son coordinación EXPERIMENTAL, no parte del trabajo, y se informan por
 * separado.
 *
 * La verificación con `shared` comprueba los N contadores, no solo el propio:
 * que compartir página no le haga perder un incremento a nadie es justamente
 * lo que el diseño pone a prueba.
 */
#include <stdio.h>
#include <string.h>

#include "app_common.h"
#include "cli.h"
#include "clock.h"

#define BARRIER_START 0u
#define BARRIER_ROUND 1u

enum layout { LAYOUT_SHARED, LAYOUT_SEPARATE };

struct gran_opts {
    enum layout layout;
    uint32_t    rounds;
};

static enum app_opt gran_opt(const char *flag, const char *v, void *arg) {
    struct gran_opts *o = arg;

    if (strcmp(flag, "--layout") == 0) {
        if (strcmp(v, "shared") == 0)
            o->layout = LAYOUT_SHARED;
        else if (strcmp(v, "separate") == 0)
            o->layout = LAYOUT_SEPARATE;
        else
            return APP_OPT_INVALID;
        return APP_OPT_TAKEN;
    }
    if (strcmp(flag, "--rounds") == 0)
        return cli_parse_u32(v, 1, 10000000u, &o->rounds)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    return APP_OPT_UNKNOWN;
}

static const char USAGE[] =
    "  --layout L         shared (una página) o separate (una por nodo)\n"
    "  --rounds R         incrementos por nodo, uno por ronda (por omisión 100)\n";

static uint32_t page_of(enum layout l, uint32_t nid) {
    return l == LAYOUT_SHARED ? 0u : nid;
}

static size_t offset_of(enum layout l, uint32_t nid) {
    return l == LAYOUT_SHARED ? (size_t)nid * sizeof(uint64_t) : 0u;
}

/* Al final: el contador propio, y con `shared` también los ajenos, tienen que
 * valer R. */
static int verify(dsm_ctx *ctx, const struct app_conn *c,
                  const struct gran_opts *o) {
    uint32_t page = page_of(o->layout, c->nid);
    uint32_t first = o->layout == LAYOUT_SHARED ? 0u : c->nid;
    uint32_t last  = o->layout == LAYOUT_SHARED ? c->nodes - 1u : c->nid;
    const unsigned char *p = DSM_acquire(ctx, page, DSM_READ);
    uint64_t seen[DSM_MAX_NODES];
    int rc = APP_EXIT_OK;

    if (p == NULL)
        return app_fail(ctx, c->nid, "no se pudo adquirir para verificar");
    for (uint32_t j = first; j <= last; j++)
        memcpy(&seen[j], p + offset_of(o->layout, j), sizeof seen[j]);
    if (DSM_release(ctx, page) != 0)
        return app_fail(ctx, c->nid, "no se pudo liberar la verificación");

    for (uint32_t j = first; j <= last; j++) {
        if (seen[j] != o->rounds) {
            fprintf(stderr, "nodo %lu: FALLO: el contador del nodo %lu vale "
                            "%llu y debía valer %lu\n", (unsigned long)c->nid,
                    (unsigned long)j, (unsigned long long)seen[j],
                    (unsigned long)o->rounds);
            rc = APP_EXIT_RESULT;
        }
    }
    return rc;
}

int main(int argc, char **argv) {
    struct app_conn c;
    struct gran_opts o = { LAYOUT_SHARED, 100u };
    dsm_ctx *ctx;
    uint32_t page, needed;
    size_t off;
    uint64_t t0 = 0, t1 = 0, acquires = 0, increments = 0, barriers = 0;
    int rc = APP_EXIT_OK;

    switch (app_parse(argc, argv, &c, gran_opt, &o, USAGE)) {
    case APP_PARSE_HELP:  return APP_EXIT_OK;
    case APP_PARSE_ERROR: return APP_EXIT_ARGS;
    case APP_PARSE_OK:    break;
    }

    ctx = app_connect(&c);
    if (ctx == NULL)
        return APP_EXIT_DSM;
    needed = o.layout == LAYOUT_SHARED ? 1u : c.nodes;
    if (DSM_page_count(ctx) < needed) {
        fprintf(stderr, "nodo %lu: el diseño separate necesita %lu páginas y "
                        "la región tiene %lu\n", (unsigned long)c.nid,
                (unsigned long)needed, (unsigned long)DSM_page_count(ctx));
        return app_finish(ctx, c.nid, APP_EXIT_ARGS);
    }
    page = page_of(o.layout, c.nid);
    off  = offset_of(o.layout, c.nid);

    if (DSM_barrier(ctx, BARRIER_START) != 0) {
        rc = app_fail(ctx, c.nid, "barrera de arranque");
        goto out;
    }
    barriers++;

    t0 = now_monotonic_ns();
    for (uint32_t r = 0; r < o.rounds; r++) {
        unsigned char *p = DSM_acquire(ctx, page, DSM_WRITE);
        uint64_t v;

        if (p == NULL) {
            rc = app_fail(ctx, c.nid, "no se pudo adquirir para escribir");
            goto out;
        }
        acquires++;
        /* Leer, sumar y escribir dentro del mismo intervalo WRITE. */
        memcpy(&v, p + off, sizeof v);
        v++;
        memcpy(p + off, &v, sizeof v);
        increments++;
        if (DSM_release(ctx, page) != 0) {
            rc = app_fail(ctx, c.nid, "no se pudo liberar la escritura");
            goto out;
        }
        if (DSM_barrier(ctx, BARRIER_ROUND) != 0) {
            rc = app_fail(ctx, c.nid, "barrera de ronda");
            goto out;
        }
        barriers++;
    }
    t1 = now_monotonic_ns();

    rc = verify(ctx, &c, &o);

out:
    rc = app_finish(ctx, c.nid, rc);
    if (rc == APP_EXIT_OK)
        printf("granularidad: nodo=%lu diseno=%s rondas=%lu "
               "adquisiciones_trabajo=%llu incrementos=%llu "
               "barreras_coordinacion=%llu adquisiciones_verificacion=1 "
               "t_trabajo_ms=%.3f\n",
               (unsigned long)c.nid,
               o.layout == LAYOUT_SHARED ? "shared" : "separate",
               (unsigned long)o.rounds, (unsigned long long)acquires,
               (unsigned long long)increments, (unsigned long long)barriers,
               app_ms(t0, t1));
    return rc;
}
