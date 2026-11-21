/* test_csv.h — leer el CSV de una corrida en las pruebas de punta a punta.
 *
 * El registro del coordinador y el de cada trabajador comparten formato por
 * construcción (D3.1, D4.4), así que una sola herramienta los lee. Esto es esa
 * herramienta del lado de las pruebas: carga el archivo en memoria, saca
 * columnas por índice y busca filas.
 *
 * Vive acá, y no dentro de una prueba, porque lo usan al menos dos: el punta a
 * punta de libdsm (#5) y el caso de coherencia (#6). Es el mismo motivo por el
 * que el arranque de procesos vive en test_proc.h.
 *
 * Las funciones son `static inline` a propósito: así las que una prueba no use
 * no disparan -Wunused-function, que con -Werror sería un error de compilación.
 */
#ifndef SOA_P3_TEST_CSV_H
#define SOA_P3_TEST_CSV_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Índices de columna del esquema de log.h (D3.1). Se nombran para que una
 * prueba no dependa de contar comas a mano. */
enum { F_RUN = 0, F_COMP = 1, F_SEQ = 2, F_NODE = 3, F_REQ = 4, F_PAGE = 5,
       F_EVENT = 6, F_STATUS = 7, F_MODE = 8, F_VERSION = 9, F_WRITER = 10,
       F_READERS = 11, F_MASK = 12, F_PENDING = 13 };

struct csv {
    char   header[512];
    char **rows;
    size_t n;
};

static inline bool csv_load(const char *path, struct csv *out) {
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
            if (out->rows == NULL)
                return false;
        }
        out->rows[out->n++] = strdup(line);
    }
    fclose(f);
    return true;
}

static inline void csv_free(struct csv *c) {
    for (size_t i = 0; i < c->n; i++)
        free(c->rows[i]);
    free(c->rows);
    c->rows = NULL;
    c->n = 0;
}

/* Columna k de una fila, copiada a `out`. Cadena vacía si la fila no llega a
 * esa columna, que es lo mismo que una celda vacía: una fila corta y una celda
 * ausente se tratan igual porque para una prueba significan lo mismo. */
static inline void field(const char *row, int k, char *out, size_t cap) {
    const char *s = row;
    size_t n;

    for (int i = 0; i < k && s != NULL; i++) {
        s = strchr(s, ',');
        if (s != NULL)
            s++;
    }
    if (s == NULL) {
        out[0] = '\0';
        return;
    }
    n = strcspn(s, ",\n");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* ¿Tiene el registro una fila `event` con ese nodo y request_id, y la misma
 * versión si se pide? `version` NULL no la compara. */
static inline bool coord_has(const struct csv *co, const char *event,
                             const char *node, const char *req,
                             const char *version) {
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

/* Número de fila (1 = la primera después del encabezado) del primer evento
 * `event` del nodo `node`, o 0 si no está. `node` NULL no lo compara.
 *
 * Devuelve la POSICIÓN en el archivo y no la columna seq, porque es lo que
 * permite comparar el orden entre filas sin suponer que seq empiece en 1 ni
 * que no tenga huecos. */
static inline size_t csv_first(const struct csv *c, const char *event,
                               const char *node) {
    char ev[32], nd[16];

    for (size_t i = 0; i < c->n; i++) {
        field(c->rows[i], F_EVENT, ev, sizeof ev);
        if (strcmp(ev, event) != 0)
            continue;
        if (node != NULL) {
            field(c->rows[i], F_NODE, nd, sizeof nd);
            if (strcmp(nd, node) != 0)
                continue;
        }
        return i + 1;
    }
    return 0;
}

#endif /* SOA_P3_TEST_CSV_H */
