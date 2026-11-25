/* app_common.c — ver app_common.h. */
#include "app_common.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"

/* nid 0 es válido, así que "sin dar" no puede ser 0. */
#define NID_UNSET UINT32_MAX

static void usage(const char *prog, const char *usage_extra) {
    fprintf(stderr,
        "uso: %s --port P --nodes N --id I [opciones]\n"
        "\n"
        "  --port P           puerto del coordinador (obligatorio)\n"
        "  --nodes N          nodos de la sesión, %u..%u (obligatorio)\n"
        "  --id I             identificador de este nodo, 0..N-1 (obligatorio)\n"
        "  --host H           dirección del coordinador (por omisión 127.0.0.1)\n"
        "  --timeout-ms MS    plazo por operación (por omisión %u)\n"
        "  --log RUTA         registro CSV de este trabajador\n"
        "%s",
        prog, DSM_MIN_NODES, DSM_MAX_NODES, DSM_DEFAULT_TIMEOUT_MS,
        usage_extra != NULL ? usage_extra : "");
}

/* APP_OPT_UNKNOWN si `flag` no es de conexión. */
static enum app_opt parse_conn(const char *flag, const char *v,
                               struct app_conn *c) {
    if (strcmp(flag, "--port") == 0)
        return cli_parse_u16(v, 1, UINT16_MAX, &c->port) ? APP_OPT_TAKEN
                                                         : APP_OPT_INVALID;
    if (strcmp(flag, "--nodes") == 0)
        return cli_parse_u32(v, DSM_MIN_NODES, DSM_MAX_NODES, &c->nodes)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    if (strcmp(flag, "--id") == 0)
        return cli_parse_u32(v, 0, DSM_MAX_NODES - 1u, &c->nid)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    if (strcmp(flag, "--timeout-ms") == 0)
        return cli_parse_u32(v, 1, 3600000u, &c->timeout_ms)
                   ? APP_OPT_TAKEN : APP_OPT_INVALID;
    if (strcmp(flag, "--host") == 0) {
        c->host = v;
        return APP_OPT_TAKEN;
    }
    if (strcmp(flag, "--log") == 0) {
        c->log_path = v;
        return APP_OPT_TAKEN;
    }
    return APP_OPT_UNKNOWN;
}

enum app_parse app_parse(int argc, char **argv, struct app_conn *c,
                         app_opt_fn extra, void *arg, const char *usage_extra) {
    c->host       = "127.0.0.1";
    c->port       = 0;
    c->nodes      = 0;
    c->nid        = NID_UNSET;
    c->timeout_ms = DSM_DEFAULT_TIMEOUT_MS;
    c->log_path   = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v;
        enum app_opt r;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0], usage_extra);
            return APP_PARSE_HELP;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: falta el valor de %s\n", argv[0], a);
            return APP_PARSE_ERROR;
        }
        v = argv[++i];
        r = parse_conn(a, v, c);
        if (r == APP_OPT_UNKNOWN && extra != NULL)
            r = extra(a, v, arg);
        if (r == APP_OPT_UNKNOWN) {
            fprintf(stderr, "%s: opción desconocida: %s\n", argv[0], a);
            return APP_PARSE_ERROR;
        }
        if (r == APP_OPT_INVALID) {
            fprintf(stderr, "%s: valor inválido para %s: %s\n", argv[0], a, v);
            return APP_PARSE_ERROR;
        }
    }

    if (c->port == 0 || c->nodes == 0 || c->nid == NID_UNSET) {
        fprintf(stderr, "%s: faltan --port, --nodes o --id\n", argv[0]);
        return APP_PARSE_ERROR;
    }
    /* Contra los nodos de ESTA sesión, no contra el máximo del protocolo. */
    if (c->nid >= c->nodes) {
        fprintf(stderr, "%s: --id %lu fuera de rango para --nodes %lu\n",
                argv[0], (unsigned long)c->nid, (unsigned long)c->nodes);
        return APP_PARSE_ERROR;
    }
    return APP_PARSE_OK;
}

dsm_ctx *app_connect(const struct app_conn *c) {
    struct dsm_options opts;
    enum dsm_status err = DSM_OK;
    dsm_ctx *ctx;

    memset(&opts, 0, sizeof opts);
    opts.timeout_ms = c->timeout_ms;
    opts.log_path   = c->log_path;
    ctx = DSM_init(c->host, c->port, c->nodes, c->nid, &opts, &err);
    if (ctx == NULL)
        fprintf(stderr, "nodo %lu: no se pudo iniciar la sesión: %s\n",
                (unsigned long)c->nid, DSM_strerror(err));
    return ctx;
}

int app_fail(dsm_ctx *ctx, uint32_t nid, const char *what) {
    fprintf(stderr, "nodo %lu: %s: %s\n", (unsigned long)nid, what,
            DSM_strerror(DSM_last_error(ctx)));
    return APP_EXIT_DSM;
}

int app_finish(dsm_ctx *ctx, uint32_t nid, int rc) {
    /* Si la sesión ya estaba en error, esto devuelve -1 y no agrega nada que
     * no se haya informado. */
    if (DSM_finalize(ctx) != 0 && rc == APP_EXIT_OK) {
        fprintf(stderr, "nodo %lu: el cierre de la sesión no fue limpio\n",
                (unsigned long)nid);
        return APP_EXIT_DSM;
    }
    return rc;
}

double app_ms(uint64_t from_ns, uint64_t to_ns) {
    return (double)(to_ns - from_ns) / 1e6;
}
