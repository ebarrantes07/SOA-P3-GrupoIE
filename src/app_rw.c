/* app_rw — un escritor, varios lectores y la invalidación de sus copias (#7).
 *
 * El nodo 0 publica en una página una versión y un patrón que se deriva de
 * ella. Después del release y de una barrera, los demás adquieren READ y
 * verifican la página entera, byte a byte. Se repite con V versiones.
 *
 * LO QUE DEMUESTRA LA SEGUNDA VERSIÓN. Tras leer la versión v, cada lector
 * conserva una copia en caché. Cuando el escritor pide WRITE para v+1, el
 * coordinador tiene que invalidar esas copias antes de concederlo. Si un
 * lector siguiera usando la suya, leería v donde debía leer v+1, y como el
 * patrón cambia EN CADA BYTE de una versión a la siguiente, ningún byte
 * viejo pasaría por nuevo. Las filas INVALIDATE del registro del coordinador
 * son la otra mitad de la evidencia; e2e_apps.sh las cuenta.
 *
 * La barrera después de las lecturas no es ceremonia: sin ella, un lector
 * lento podría adquirir cuando el escritor ya publicó v+1, y lo que se
 * verifica es que cada lector vea CADA versión.
 *
 * Disposición de la página:
 *   bytes 0..7      uint64_t version
 *   bytes 8..4095   pattern(version, i)
 */
#include <stdio.h>
#include <string.h>

#include "app_common.h"
#include "cli.h"

#define WRITER 0u

#define BARRIER_PUBLISHED 0u
#define BARRIER_READ      1u

#define HEADER sizeof(uint64_t)

struct rw_opts {
    uint32_t versions;
    uint32_t page;
};

static enum app_opt rw_opt(const char *flag, const char *v, void *arg) {
    struct rw_opts *o = arg;

    if (strcmp(flag, "--versions") == 0)
        return cli_parse_u32(v, 1, 1000000u, &o->versions)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    if (strcmp(flag, "--page") == 0)
        return cli_parse_u32(v, 0, DSM_MAX_PAGES - 1u, &o->page)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    return APP_OPT_UNKNOWN;
}

static const char USAGE[] =
    "  --versions V       versiones que publica el escritor (por omisión 2)\n"
    "  --page P           página que se usa (por omisión 0)\n";

/* Byte i del patrón de la versión v. De v a v+1 cada byte cambia en 151, que
 * no es múltiplo de 256: no hay un solo byte igual entre dos versiones
 * consecutivas. La versión 0 es la región inicial, toda en cero. */
static unsigned char pattern(uint64_t v, size_t i) {
    if (v == 0)
        return 0;
    return (unsigned char)(i * 31u + v * 151u + 7u);
}

/* Bytes de la página que no coinciden con la versión v (0 = página exacta). */
static size_t mismatches(const unsigned char *p, uint64_t v) {
    uint64_t got;
    size_t bad = 0;

    memcpy(&got, p, sizeof got);
    if (got != v)
        bad += HEADER;
    for (size_t i = HEADER; i < DSM_PAGE_SIZE; i++)
        if (p[i] != pattern(v, i))
            bad++;
    return bad;
}

/* El escritor: comprueba que la página tenga la versión anterior completa y
 * publica la nueva. */
static int write_version(dsm_ctx *ctx, uint32_t page, uint64_t v) {
    unsigned char *p = DSM_acquire(ctx, page, DSM_WRITE);
    size_t bad;

    if (p == NULL)
        return app_fail(ctx, WRITER, "no se pudo adquirir para escribir");
    bad = mismatches(p, v - 1u);
    memcpy(p, &v, sizeof v);
    for (size_t i = HEADER; i < DSM_PAGE_SIZE; i++)
        p[i] = pattern(v, i);
    if (DSM_release(ctx, page) != 0)
        return app_fail(ctx, WRITER, "no se pudo publicar");
    if (bad != 0) {
        fprintf(stderr, "nodo 0: FALLO: antes de la versión %llu la página "
                        "tenía %zu bytes distintos de la %llu\n",
                (unsigned long long)v, bad, (unsigned long long)(v - 1u));
        return APP_EXIT_RESULT;
    }
    return APP_EXIT_OK;
}

/* Un lector: la página entera tiene que ser la versión v. */
static int read_version(dsm_ctx *ctx, uint32_t nid, uint32_t page,
                        uint64_t v) {
    const unsigned char *p = DSM_acquire(ctx, page, DSM_READ);
    size_t bad;

    if (p == NULL)
        return app_fail(ctx, nid, "no se pudo adquirir para leer");
    bad = mismatches(p, v);
    if (DSM_release(ctx, page) != 0)
        return app_fail(ctx, nid, "no se pudo liberar la lectura");
    if (bad != 0) {
        fprintf(stderr, "nodo %lu: FALLO: versión %llu con %zu de %u bytes "
                        "distintos\n", (unsigned long)nid,
                (unsigned long long)v, bad, DSM_PAGE_SIZE);
        return APP_EXIT_RESULT;
    }
    return APP_EXIT_OK;
}

int main(int argc, char **argv) {
    struct app_conn c;
    struct rw_opts o = { 2u, 0u };
    dsm_ctx *ctx;
    int rc = APP_EXIT_OK;
    uint64_t v;

    switch (app_parse(argc, argv, &c, rw_opt, &o, USAGE)) {
    case APP_PARSE_HELP:  return APP_EXIT_OK;
    case APP_PARSE_ERROR: return APP_EXIT_ARGS;
    case APP_PARSE_OK:    break;
    }

    ctx = app_connect(&c);
    if (ctx == NULL)
        return APP_EXIT_DSM;
    if (o.page >= DSM_page_count(ctx)) {
        fprintf(stderr, "nodo %lu: la página %lu no existe: la región tiene "
                        "%lu\n", (unsigned long)c.nid, (unsigned long)o.page,
                (unsigned long)DSM_page_count(ctx));
        return app_finish(ctx, c.nid, APP_EXIT_ARGS);
    }

    for (v = 1; v <= o.versions && rc == APP_EXIT_OK; v++) {
        if (c.nid == WRITER)
            rc = write_version(ctx, o.page, v);
        if (rc != APP_EXIT_OK)
            break;
        if (DSM_barrier(ctx, BARRIER_PUBLISHED) != 0) {
            rc = app_fail(ctx, c.nid, "barrera de publicación");
            break;
        }
        if (c.nid != WRITER)
            rc = read_version(ctx, c.nid, o.page, v);
        if (rc != APP_EXIT_OK)
            break;
        if (DSM_barrier(ctx, BARRIER_READ) != 0) {
            rc = app_fail(ctx, c.nid, "barrera de lectura");
            break;
        }
    }

    rc = app_finish(ctx, c.nid, rc);
    if (rc == APP_EXIT_OK)
        printf("rw: nodo=%lu rol=%s versiones=%lu bytes_verificados=%llu\n",
               (unsigned long)c.nid, c.nid == WRITER ? "escritor" : "lector",
               (unsigned long)o.versions,
               (unsigned long long)(c.nid == WRITER
                   ? 0u : (uint64_t)o.versions * DSM_PAGE_SIZE));
    return rc;
}
