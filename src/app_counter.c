/* app_counter — el contador compartido: N nodos, K incrementos cada uno (#6).
 *
 * Es la aplicación más simple que puede fallar de forma VISIBLE si la
 * exclusividad no se respeta. Cada incremento adquiere WRITE, suma uno y
 * libera, así que el leer-modificar-escribir entero queda dentro de un mismo
 * intervalo exclusivo. Si el DSM sirviera dos escritores a la vez, los dos
 * leerían el mismo valor, los dos escribirían ese valor más uno, y el total
 * quedaría por debajo de N*K. Una actualización perdida no puede pasar
 * inadvertida: el resultado esperado es un número exacto, no un rango.
 *
 * Partir el incremento en un READ y un WRITE separados NO sería atómico, y
 * ninguna barrera posterior lo arreglaría (dsm.h). De ahí que el acceso sea
 * uno solo, en modo WRITE, aunque la primera cosa que se haga sea leer.
 *
 * LA PÁGINA SE USA COMO MEMORIA. El contador es un `uint64_t` leído y escrito
 * directamente, no con los conversores de orden de red de wire.h: el protocolo
 * ordena los bytes de SUS cabeceras, pero la región compartida son bytes que
 * la aplicación interpreta como quiera, y usarla como memoria normal es
 * justamente lo que este proyecto demuestra. Queda limitado a nodos de la
 * misma arquitectura, que es el caso de esta entrega. La alineación está
 * garantizada: el marco sale de `calloc` y la página mide 4096 bytes, múltiplo
 * de 8, así que su inicio sirve para un `uint64_t`.
 *
 * CÓDIGOS DE SALIDA, para que `make test` sepa qué falló sin leer la salida:
 *
 *   0  el contador terminó en N*K
 *   1  argumentos inválidos. Casi todos se rechazan antes de abrir un
 *      socket; la excepción es `--page`, que solo se puede validar contra la
 *      cantidad de páginas que el coordinador define al arrancar la sesión
 *      (D0.7), así que ese caso sale con 1 ya conectado
 *   2  el DSM falló (conexión, plazo, sesión abortada, rechazo)
 *   3  la sesión funcionó pero el contador NO terminó en N*K
 *
 * El 3 es el que importa: separa "no se pudo correr" de "corrió y el
 * resultado está mal", que son dos problemas distintos.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dsm.h"

#define SALIDA_OK        0
#define SALIDA_ARGS      1
#define SALIDA_DSM       2
#define SALIDA_RESULTADO 3

/* Barreras: la primera iguala el arranque, la segunda espera a que todos
 * terminen de sumar antes de que alguien lea el total. */
#define BARRERA_ARRANQUE 0u
#define BARRERA_FIN      1u

struct opciones {
    const char *host;
    uint16_t    port;
    uint32_t    nodes;
    uint32_t    nid;
    uint64_t    iters;
    uint32_t    page;
    uint32_t    timeout_ms;
    const char *log_path;
};

/* nid 0 es válido, así que "sin dar" no puede ser 0: se usa un centinela
 * fuera del rango de nodos posibles. */
#define NID_SIN_DAR UINT32_MAX

static void uso(const char *prog) {
    fprintf(stderr,
        "uso: %s --port P --nodes N --id I [opciones]\n"
        "\n"
        "  --port P           puerto del coordinador (obligatorio)\n"
        "  --nodes N          nodos de la sesión, %u..%u (obligatorio)\n"
        "  --id I             identificador de este nodo, 0..N-1 (obligatorio)\n"
        "  --iters K          incrementos por nodo (por omisión 1000)\n"
        "  --page P           página del contador, 0..%u (por omisión 0)\n"
        "  --host H           dirección del coordinador (por omisión 127.0.0.1)\n"
        "  --timeout-ms MS    plazo por operación (por omisión %u)\n"
        "  --log RUTA         registro CSV de este trabajador\n",
        prog, DSM_MIN_NODES, DSM_MAX_NODES, DSM_MAX_PAGES - 1u,
        DSM_DEFAULT_TIMEOUT_MS);
}

/* Pedir ayuda no es un error, así que no puede devolver lo mismo que un
 * argumento inválido: quien escribe `--help` espera un 0. */
enum parse_result { PARSE_OK, PARSE_HELP, PARSE_ERROR };

/* PARSE_ERROR si los argumentos no sirven, dejando el motivo impreso. */
static enum parse_result parse_args(int argc, char **argv,
                                    struct opciones *o) {
    o->host       = "127.0.0.1";
    o->port       = 0;
    o->nodes      = 0;
    o->nid        = NID_SIN_DAR;
    o->iters      = 1000;
    o->page       = 0;
    o->timeout_ms = DSM_DEFAULT_TIMEOUT_MS;
    o->log_path   = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool tiene_valor = (i + 1 < argc);
        const char *v = tiene_valor ? argv[i + 1] : NULL;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            uso(argv[0]);
            return PARSE_HELP;
        }
        if (!tiene_valor) {
            fprintf(stderr, "%s: falta el valor de %s\n", argv[0], a);
            return PARSE_ERROR;
        }

        if (strcmp(a, "--port") == 0) {
            if (!cli_parse_u16(v, 1, UINT16_MAX, &o->port))
                goto malo;
        } else if (strcmp(a, "--nodes") == 0) {
            if (!cli_parse_u32(v, DSM_MIN_NODES, DSM_MAX_NODES, &o->nodes))
                goto malo;
        } else if (strcmp(a, "--id") == 0) {
            if (!cli_parse_u32(v, 0, DSM_MAX_NODES - 1u, &o->nid))
                goto malo;
        } else if (strcmp(a, "--iters") == 0) {
            /* Se acepta todo el rango de un u64 para que la validación de
             * desborde de más abajo sea una comprobación de verdad y no
             * aritmética que el tipo ya garantiza. */
            if (!cli_parse_u64(v, 1, UINT64_MAX, &o->iters))
                goto malo;
        } else if (strcmp(a, "--page") == 0) {
            /* Rango del PROTOCOLO. Que la página exista en esta sesión no se
             * sabe hasta que el coordinador diga cuántas hay. */
            if (!cli_parse_u32(v, 0, DSM_MAX_PAGES - 1u, &o->page))
                goto malo;
        } else if (strcmp(a, "--timeout-ms") == 0) {
            if (!cli_parse_u32(v, 1, 3600000u, &o->timeout_ms))
                goto malo;
        } else if (strcmp(a, "--host") == 0) {
            o->host = v;
        } else if (strcmp(a, "--log") == 0) {
            o->log_path = v;
        } else {
            fprintf(stderr, "%s: opción desconocida: %s\n", argv[0], a);
            return PARSE_ERROR;
        }
        i++;
        continue;
    malo:
        fprintf(stderr, "%s: valor inválido para %s: %s\n", argv[0], a, v);
        return PARSE_ERROR;
    }

    if (o->port == 0) {
        fprintf(stderr, "%s: falta --port\n", argv[0]);
        return PARSE_ERROR;
    }
    if (o->nodes == 0) {
        fprintf(stderr, "%s: falta --nodes\n", argv[0]);
        return PARSE_ERROR;
    }
    if (o->nid == NID_SIN_DAR) {
        fprintf(stderr, "%s: falta --id\n", argv[0]);
        return PARSE_ERROR;
    }
    /* El identificador se valida contra los nodos de ESTA sesión, no contra
     * el máximo del protocolo: con --nodes 2, el id 3 no existe. */
    if (o->nid >= o->nodes) {
        fprintf(stderr, "%s: --id %lu fuera de rango para --nodes %lu\n",
                argv[0], (unsigned long)o->nid, (unsigned long)o->nodes);
        return PARSE_ERROR;
    }
    /* Desborde del total esperado, ANTES de conectarse: una sesión que no
     * puede dar un resultado verificable no vale la pena arrancarla, y
     * abortarla a mitad le arruinaría la corrida a los otros nodos. */
    if (o->nodes > UINT64_MAX / o->iters) {
        fprintf(stderr,
                "%s: --nodes %lu por --iters %llu desborda un uint64\n",
                argv[0], (unsigned long)o->nodes,
                (unsigned long long)o->iters);
        return PARSE_ERROR;
    }
    return PARSE_OK;
}

/* Imprime el motivo de una falla del DSM con el estado que la explica. */
static int falla(dsm_ctx *ctx, uint32_t nid, const char *que) {
    enum dsm_status st = DSM_last_error(ctx);

    fprintf(stderr, "nodo %lu: %s: %s\n", (unsigned long)nid, que,
            DSM_strerror(st));
    return SALIDA_DSM;
}

int main(int argc, char **argv) {
    struct opciones o;
    struct dsm_options opts;
    enum dsm_status err = DSM_OK;
    dsm_ctx *ctx;
    uint64_t esperado, visto;
    uint64_t *contador;
    const uint64_t *lectura;
    int salida = SALIDA_OK;

    switch (parse_args(argc, argv, &o)) {
    case PARSE_OK:    break;
    case PARSE_HELP:  return SALIDA_OK;
    case PARSE_ERROR: return SALIDA_ARGS;
    }

    esperado = (uint64_t)o.nodes * o.iters;

    memset(&opts, 0, sizeof opts);
    opts.timeout_ms = o.timeout_ms;
    opts.log_path   = o.log_path;

    ctx = DSM_init(o.host, o.port, o.nodes, o.nid, &opts, &err);
    if (ctx == NULL) {
        fprintf(stderr, "nodo %lu: no se pudo iniciar la sesión: %s\n",
                (unsigned long)o.nid, DSM_strerror(err));
        return SALIDA_DSM;
    }

    /* La región la define el coordinador (D0.7), así que la página pedida se
     * valida recién acá. Se informa cuántas hay, que es lo que le sirve a
     * quien se equivocó. */
    if (o.page >= DSM_page_count(ctx)) {
        fprintf(stderr, "nodo %lu: la página %lu no existe: la región tiene "
                        "%lu\n", (unsigned long)o.nid, (unsigned long)o.page,
                (unsigned long)DSM_page_count(ctx));
        (void)DSM_finalize(ctx);
        return SALIDA_ARGS;
    }

    /* Barrera de arranque. No es ceremonia: DSM_init espera a que estén los N
     * registrados, pero no a que estén los N LISTOS para sumar. Sin ella, el
     * nodo 0 puede terminar sus K incrementos antes de que el último arranque,
     * y entonces nunca hay dos nodos peleando por la página: el total saldría
     * bien incluso con una exclusividad rota. La barrera es lo que vuelve real
     * la contención que esta prueba quiere medir. */
    if (DSM_barrier(ctx, BARRERA_ARRANQUE) != 0) {
        salida = falla(ctx, o.nid, "barrera de arranque");
        goto fin;
    }

    for (uint64_t i = 0; i < o.iters; i++) {
        contador = DSM_acquire(ctx, o.page, DSM_WRITE);
        if (contador == NULL) {
            salida = falla(ctx, o.nid, "no se pudo adquirir para escribir");
            goto fin;
        }
        /* Leer, modificar y escribir DENTRO del mismo intervalo exclusivo.
         * Acá es donde el proyecto se juega: entre estas dos líneas ningún
         * otro nodo tiene permiso ni una copia válida utilizable. */
        (*contador)++;
        if (DSM_release(ctx, o.page) != 0) {
            salida = falla(ctx, o.nid, "no se pudo liberar la escritura");
            goto fin;
        }
    }

    /* Hasta que no hayan sumado todos, el total no está completo: sin esta
     * barrera, la lectura de abajo podría ver un valor parcial que es
     * correcto en ese instante pero menor que N*K. */
    if (DSM_barrier(ctx, BARRERA_FIN) != 0) {
        salida = falla(ctx, o.nid, "barrera de fin");
        goto fin;
    }

    /* Lo verifica CADA nodo, no solo el 0: así además se comprueba que los
     * lectores concurrentes vean todos el mismo valor, que es la otra mitad
     * de la coherencia. */
    lectura = DSM_acquire(ctx, o.page, DSM_READ);
    if (lectura == NULL) {
        salida = falla(ctx, o.nid, "no se pudo adquirir para leer");
        goto fin;
    }
    visto = *lectura;
    if (DSM_release(ctx, o.page) != 0) {
        salida = falla(ctx, o.nid, "no se pudo liberar la lectura");
        goto fin;
    }

    printf("nodo %lu: contador=%llu esperado=%llu\n", (unsigned long)o.nid,
           (unsigned long long)visto, (unsigned long long)esperado);
    if (visto != esperado) {
        fprintf(stderr, "nodo %lu: FALLO: se perdieron %llu actualizaciones\n",
                (unsigned long)o.nid, (unsigned long long)(esperado - visto));
        salida = SALIDA_RESULTADO;
    }

fin:
    /* Se despide siempre, también al fallar: así el coordinador distingue una
     * salida ordenada de una caída, y los otros nodos no esperan un plazo
     * entero por alguien que ya se fue. Si la sesión ya estaba en error, esto
     * devuelve -1 y no agrega nada que no se haya informado. */
    if (DSM_finalize(ctx) != 0 && salida == SALIDA_OK) {
        fprintf(stderr, "nodo %lu: el cierre de la sesión no fue limpio\n",
                (unsigned long)o.nid);
        salida = SALIDA_DSM;
    }
    return salida;
}
