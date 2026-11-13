/* Pruebas del registro CSV del coordinador (#4).
 *
 * El módulo no lleva reloj (ver log.h), así que estas pruebas comparan el
 * archivo producido LÍNEA POR LÍNEA contra el texto esperado. No hay nada
 * aproximado que verificar: la misma secuencia de llamadas tiene que producir
 * siempre los mismos bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "test_util.h"
#include "wire.h"

/* Archivo temporal propio de cada caso, para que corran en cualquier orden. */
static char ruta[256];

static const char *temp(const char *nombre) {
    snprintf(ruta, sizeof ruta, "build/test_log_%s.csv", nombre);
    return ruta;
}

/* Lee el archivo completo. El llamador libera. NULL si no se pudo leer. */
static char *leer(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t leidos = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[leidos] = '\0';
    return buf;
}

/* Compara contenido completo e informa la primera línea que difiere, que es
 * mucho más útil que un "no son iguales" cuando el CSV tiene 20 columnas. */
static bool igual(const char *path, const char *esperado) {
    char *real = leer(path);
    if (real == NULL) {
        fprintf(stderr, "  no se pudo leer %s\n", path);
        return false;
    }
    bool ok = strcmp(real, esperado) == 0;
    if (!ok) {
        const char *a = real, *b = esperado;
        int linea = 1;
        while (*a != '\0' && *b != '\0') {
            const char *fa = strchr(a, '\n'), *fb = strchr(b, '\n');
            size_t la = fa ? (size_t)(fa - a) : strlen(a);
            size_t lb = fb ? (size_t)(fb - b) : strlen(b);
            if (la != lb || memcmp(a, b, la) != 0) {
                fprintf(stderr, "  linea %d difiere\n    real:     %.*s\n"
                                "    esperado: %.*s\n",
                        linea, (int)la, a, (int)lb, b);
                break;
            }
            if (!fa || !fb) break;
            a = fa + 1; b = fb + 1; linea++;
        }
    }
    free(real);
    return ok;
}

#define HDR "run_id,component,seq,node_id,request_id,page_id,event,status," \
            "mode,version,writer,readers,readers_mask,inval_pending," \
            "msgs_in,msgs_out,bytes_in,bytes_out,page_xfers,invals,dur_ns\n"

/* ---- Apertura y rechazos ---------------------------------------------- */

static bool test_open_writes_header(void) {
    const char *p = temp("header");
    dsm_log *lg = log_open(p, "coordinador", 7);
    CHECK(lg != NULL);
    CHECK(log_seq(lg) == 0);          /* el encabezado no es una fila */
    CHECK(log_close(lg));
    CHECK(igual(p, HDR));
    return true;
}

static bool test_open_rejects_bad_args(void) {
    CHECK(log_open(NULL, "coordinador", 1) == NULL);
    CHECK(log_open(temp("bad"), NULL, 1) == NULL);
    CHECK(log_open(temp("bad"), "", 1) == NULL);

    /* Una coma o un salto en el nombre rompería el CSV: se rechaza en vez de
     * escaparse, así el archivo nunca necesita reglas de comillas. */
    CHECK(log_open(temp("bad"), "coordi,nador", 1) == NULL);
    CHECK(log_open(temp("bad"), "coordi\nnador", 1) == NULL);
    CHECK(log_open(temp("bad"), "coordi\"nador", 1) == NULL);

    /* Un nombre más largo que el buffer se rechaza, no se trunca. */
    char largo[128];
    memset(largo, 'x', sizeof largo - 1);
    largo[sizeof largo - 1] = '\0';
    CHECK(log_open(temp("bad"), largo, 1) == NULL);

    /* Una ruta imposible no es un aborto, es un NULL. */
    CHECK(log_open("build/no/existe/ese/directorio.csv", "c", 1) == NULL);
    return true;
}

/* Un registro deshabilitado (NULL) tiene que aceptar todas las llamadas, para
 * que el coordinador no pregunte antes de cada una. */
static bool test_null_log_is_disabled(void) {
    struct log_row row = log_row_empty(LOG_SESSION_START);
    log_write(NULL, &row);
    log_count_sent(NULL, DSM_WIRE_INVALIDATE, 0);
    log_count_recv(NULL, DSM_WIRE_REL_WRITE, DSM_PAGE_SIZE);
    CHECK(log_seq(NULL) == 0);
    struct log_traffic t = log_traffic_now(NULL);
    CHECK(t.msgs_in == 0 && t.msgs_out == 0 && t.bytes_in == 0);
    CHECK(t.bytes_out == 0 && t.page_xfers == 0 && t.invals == 0);
    CHECK(log_close(NULL));           /* cerrar NULL es éxito, no fallo */
    return true;
}

/* ---- Celdas vacías ----------------------------------------------------- */

/* Lo que el evento no usa queda vacío, no en 0: el nodo 0 es un nodo real, y
 * una columna con 0xFFFFFFFF contaminaría cualquier promedio en E8. */
static bool test_absent_fields_are_empty_cells(void) {
    const char *p = temp("vacias");
    dsm_log *lg = log_open(p, "coordinador", 42);
    CHECK(lg != NULL);

    struct log_row row = log_row_empty(LOG_SESSION_START);
    log_write(lg, &row);
    CHECK(log_seq(lg) == 1);
    CHECK(log_close(lg));

    CHECK(igual(p, HDR
        "42,coordinador,1,,,,SESSION_START,,,,,,,,0,0,0,0,0,0,\n"));
    return true;
}

/* Y el nodo 0 sí sale como 0, que es la otra mitad del contrato. */
static bool test_node_zero_is_not_absent(void) {
    const char *p = temp("nodo0");
    dsm_log *lg = log_open(p, "coordinador", 1);
    CHECK(lg != NULL);

    struct log_row row = log_row_empty(LOG_NODE_JOIN);
    row.node_id = 0;
    log_write(lg, &row);
    CHECK(log_close(lg));

    CHECK(igual(p, HDR "1,coordinador,1,0,,,NODE_JOIN,,,,,,,,0,0,0,0,0,0,\n"));
    return true;
}

/* ---- Secuencia --------------------------------------------------------- */

static bool test_sequence_is_global_and_increasing(void) {
    const char *p = temp("seq");
    dsm_log *lg = log_open(p, "coordinador", 1);
    CHECK(lg != NULL);

    /* Eventos de nodos y páginas distintas: la secuencia es una sola para
     * todos, no una por nodo ni una por página. */
    for (uint32_t i = 0; i < 5; i++) {
        struct log_row row = log_row_empty(LOG_ACQ_REQ);
        row.node_id = i % 4;
        row.page_id = i;
        row.request_id = 100 + i;
        log_write(lg, &row);
        CHECK(log_seq(lg) == (uint64_t)i + 1);
    }
    CHECK(log_close(lg));

    /* Se verifica la columna 3 de cada fila: 1,2,3,4,5 sin huecos. */
    char *txt = leer(p);
    CHECK(txt != NULL);
    const char *l = strchr(txt, '\n') + 1;      /* saltar encabezado */
    for (int i = 1; i <= 5; i++) {
        char esperado[32];
        snprintf(esperado, sizeof esperado, "1,coordinador,%d,", i);
        CHECK(strncmp(l, esperado, strlen(esperado)) == 0);
        l = strchr(l, '\n') + 1;
    }
    CHECK(*l == '\0');
    free(txt);
    return true;
}

/* ---- Estado de la página ----------------------------------------------- */

/* El estado sale del directorio, no de una copia: se le pasa la instantánea
 * y se comprueba que las cinco columnas salgan de ahí. */
static bool test_page_state_comes_from_directory(void) {
    const char *p = temp("estado");
    dsm_log *lg = log_open(p, "coordinador", 9);
    CHECK(lg != NULL);

    struct dir_page_info info = {
        .version = 12, .writer = -1,
        .readers = 0x06, .holders = 0x0E,
        .inval_pending = 0x08,
        .reader_count = 2, .pending_acks = 1, .queued = 3,
    };
    struct log_row row = log_row_empty(LOG_ACQ_GRANT);
    row.node_id = 2; row.page_id = 5; row.request_id = 77;
    row.mode = DSM_READ; row.page = &info; row.dur_ns = 1500;
    log_write(lg, &row);

    /* Un escritor activo sale con su id, no con -1. */
    info.writer = 3; info.readers = 0; info.reader_count = 0;
    info.pending_acks = 0; info.version = 13;
    struct log_row w = log_row_empty(LOG_WRITE_PUBLISHED);
    w.node_id = 3; w.page_id = 5; w.request_id = 78;
    w.mode = DSM_WRITE; w.page = &info;
    log_write(lg, &w);
    CHECK(log_close(lg));

    CHECK(igual(p, HDR
        "9,coordinador,1,2,77,5,ACQ_GRANT,,READ,12,-1,2,0x06,1,0,0,0,0,0,0,1500\n"
        "9,coordinador,2,3,78,5,WRITE_PUBLISHED,,WRITE,13,3,0,0x00,0,0,0,0,0,0,0,\n"));
    return true;
}

/* ---- Tráfico ----------------------------------------------------------- */

/* Criterio de #4: "el tamaño de página se cuenta una sola vez por
 * transmisión". Quien llama no pasa contadores, así que no puede contar dos
 * veces; lo que se prueba acá es que la derivación por tipo sea la correcta. */
static bool test_page_counted_once_per_transfer(void) {
    const char *p = temp("trafico");
    dsm_log *lg = log_open(p, "coordinador", 1);
    CHECK(lg != NULL);

    /* Una concesión CON página: una transferencia. */
    log_count_sent(lg, DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    struct log_traffic t = log_traffic_now(lg);
    CHECK(t.msgs_out == 1 && t.page_xfers == 1);
    CHECK(t.bytes_out == DSM_HEADER_SIZE + 8u + DSM_PAGE_SIZE);

    /* Una concesión SIN página: NO es transferencia, aunque el tipo la
     * admita. Es el caso de la copia retenida que sigue vigente. */
    log_count_sent(lg, DSM_WIRE_ACQ_GRANT, 8u);
    t = log_traffic_now(lg);
    CHECK(t.msgs_out == 2 && t.page_xfers == 1);
    CHECK(t.bytes_out == 2u * DSM_HEADER_SIZE + 16u + DSM_PAGE_SIZE);

    /* Un REL_WRITE recibido: siempre lleva página, y cuenta en la misma
     * columna aunque vaya en el otro sentido. */
    log_count_recv(lg, DSM_WIRE_REL_WRITE, DSM_PAGE_SIZE);
    t = log_traffic_now(lg);
    CHECK(t.msgs_in == 1 && t.page_xfers == 2);
    CHECK(t.bytes_in == DSM_HEADER_SIZE + DSM_PAGE_SIZE);

    /* Un tipo que no puede llevar página nunca cuenta como transferencia. */
    log_count_sent(lg, DSM_WIRE_REL_WRITE_ACK, 8u);
    log_count_recv(lg, DSM_WIRE_ACQ_REQ, 8u);
    t = log_traffic_now(lg);
    CHECK(t.page_xfers == 2);

    /* Y tampoco si le pasan una longitud imposible para ese tipo. Estas
     * funciones no validan tramas —eso es wire_check— así que el chequeo de
     * tipo es lo único que las separa de contar una transferencia inventada.
     * Con tramas válidas no puede ocurrir: solo ACQ_GRANT y REL_WRITE admiten
     * carga de 4096 o más, y la mayor de las demás es ERROR con 256. Se fija
     * acá porque sin esta línea, quitar el chequeo de tipo no hace fallar
     * ninguna prueba. */
    log_count_sent(lg, DSM_WIRE_REL_READ_ACK, DSM_PAGE_SIZE);
    log_count_recv(lg, DSM_WIRE_ERROR, DSM_PAGE_SIZE);
    t = log_traffic_now(lg);
    CHECK(t.page_xfers == 2);

    /* Las invalidaciones se cuentan aparte, y solo al enviarlas: el ACK que
     * vuelve no es otra invalidación. */
    CHECK(t.invals == 0);
    log_count_sent(lg, DSM_WIRE_INVALIDATE, 0);
    log_count_sent(lg, DSM_WIRE_INVALIDATE, 0);
    log_count_recv(lg, DSM_WIRE_INVALIDATE_ACK, 0);
    t = log_traffic_now(lg);
    CHECK(t.invals == 2);

    CHECK(log_close(lg));
    return true;
}

/* Los contadores que salen en una fila son los del momento de escribirla, no
 * los finales: así una fila dice cuánto tráfico había hasta ese evento. */
static bool test_rows_carry_running_counters(void) {
    const char *p = temp("acumulado");
    dsm_log *lg = log_open(p, "coordinador", 3);
    CHECK(lg != NULL);

    struct log_row r1 = log_row_empty(LOG_SESSION_START);
    log_write(lg, &r1);

    log_count_recv(lg, DSM_WIRE_ACQ_REQ, 8u);         /* 40 bytes */
    log_count_sent(lg, DSM_WIRE_INVALIDATE, 0u);      /* 32 bytes */

    struct log_row r2 = log_row_empty(LOG_INVALIDATE);
    r2.node_id = 1; r2.page_id = 0; r2.request_id = 5;
    log_write(lg, &r2);
    CHECK(log_close(lg));

    CHECK(igual(p, HDR
        "3,coordinador,1,,,,SESSION_START,,,,,,,,0,0,0,0,0,0,\n"
        "3,coordinador,2,1,5,0,INVALIDATE,,,,,,,,1,1,40,32,0,1,\n"));
    return true;
}

/* ---- Errores y nombres ------------------------------------------------- */

/* Un LOG_ERROR escribe el texto del dsm_status en la columna de evento: es lo
 * que hace legible el archivo sin tener que recordar códigos. */
static bool test_error_row_names_the_status(void) {
    const char *p = temp("error");
    dsm_log *lg = log_open(p, "coordinador", 1);
    CHECK(lg != NULL);

    struct log_row row = log_row_empty(LOG_ERROR);
    row.node_id = 2; row.page_id = 300; row.status = DSM_EINVAL;
    log_write(lg, &row);
    CHECK(log_close(lg));

    CHECK(igual(p, HDR
        "1,coordinador,1,2,,300,ERROR,EINVAL,,,,,,,0,0,0,0,0,0,\n"));

    /* Y el estado va en SU columna, no en la del evento: una fila de error
     * conserva qué se intentaba además de por qué falló. */
    const char *p2 = temp("error2");
    dsm_log *lg2 = log_open(p2, "coordinador", 1);
    CHECK(lg2 != NULL);
    struct log_row r2 = log_row_empty(LOG_ACQ_REQ);
    r2.node_id = 1; r2.page_id = 9; r2.request_id = 4;
    r2.status = DSM_ESTATE;
    log_write(lg2, &r2);
    CHECK(log_close(lg2));
    CHECK(igual(p2, HDR
        "1,coordinador,1,1,4,9,ACQ_REQ,ESTATE,,,,,,,0,0,0,0,0,0,\n"));
    return true;
}

/* Todo dsm_status tiene token, ninguno repite, y DSM_OK es celda vacía. */
static bool test_status_names(void) {
    const enum dsm_status todos[] = {
        DSM_EINVAL, DSM_ESTATE, DSM_ETIMEOUT, DSM_ECONN,
        DSM_EPROTO, DSM_EABORT, DSM_ENOMEM, DSM_EIO,
    };
    size_t n = sizeof todos / sizeof todos[0];
    CHECK(strcmp(log_status_name(DSM_OK), "") == 0);
    for (size_t i = 0; i < n; i++) {
        const char *a = log_status_name(todos[i]);
        CHECK(a != NULL && a[0] != '\0' && strcmp(a, "?") != 0);
        CHECK(strchr(a, ',') == NULL);
        for (size_t j = i + 1; j < n; j++)
            CHECK(strcmp(a, log_status_name(todos[j])) != 0);
    }
    CHECK(strcmp(log_status_name((enum dsm_status)99), "?") == 0);
    return true;
}

static bool test_event_names(void) {
    /* Todo evento declarado tiene nombre, y ninguno repite. */
    const enum log_event todos[] = {
        LOG_SESSION_START, LOG_SESSION_END, LOG_NODE_JOIN, LOG_NODE_LEAVE,
        LOG_ACQ_REQ, LOG_ACQ_GRANT, LOG_INVALIDATE, LOG_INVALIDATE_ACK,
        LOG_REL_WRITE, LOG_WRITE_PUBLISHED, LOG_REL_READ, LOG_READ_RELEASED,
        LOG_BARRIER_ARRIVE, LOG_BARRIER_RELEASE, LOG_TIMEOUT, LOG_ERROR,
    };
    size_t n = sizeof todos / sizeof todos[0];
    for (size_t i = 0; i < n; i++) {
        const char *a = log_event_name(todos[i]);
        CHECK(a != NULL && a[0] != '\0' && strcmp(a, "?") != 0);
        /* Ningún nombre lleva coma: iría a una columna del CSV. */
        CHECK(strchr(a, ',') == NULL);
        for (size_t j = i + 1; j < n; j++)
            CHECK(strcmp(a, log_event_name(todos[j])) != 0);
    }
    /* Un valor fuera del enum no desreferencia nada. */
    CHECK(strcmp(log_event_name((enum log_event)999), "?") == 0);
    return true;
}

/* El archivo queda con buffer por línea, así que lo ya registrado está en
 * disco antes de cerrar: es lo que hace útil al registro cuando el proceso
 * muere a mitad de una corrida (#8). */
static bool test_rows_reach_disk_before_close(void) {
    const char *p = temp("flush");
    dsm_log *lg = log_open(p, "coordinador", 1);
    CHECK(lg != NULL);

    struct log_row row = log_row_empty(LOG_NODE_JOIN);
    row.node_id = 1;
    log_write(lg, &row);

    /* Sin cerrar: el archivo ya tiene las dos líneas. */
    char *txt = leer(p);
    CHECK(txt != NULL);
    CHECK(strstr(txt, "NODE_JOIN") != NULL);
    free(txt);

    CHECK(log_close(lg));
    return true;
}

int main(void) {
    const struct test_case tests[] = {
        {"open_writes_header", test_open_writes_header},
        {"open_rejects_bad_args", test_open_rejects_bad_args},
        {"null_log_is_disabled", test_null_log_is_disabled},
        {"absent_fields_are_empty_cells", test_absent_fields_are_empty_cells},
        {"node_zero_is_not_absent", test_node_zero_is_not_absent},
        {"sequence_is_global_and_increasing", test_sequence_is_global_and_increasing},
        {"page_state_comes_from_directory", test_page_state_comes_from_directory},
        {"page_counted_once_per_transfer", test_page_counted_once_per_transfer},
        {"rows_carry_running_counters", test_rows_carry_running_counters},
        {"error_row_names_the_status", test_error_row_names_the_status},
        {"status_names", test_status_names},
        {"event_names", test_event_names},
        {"rows_reach_disk_before_close", test_rows_reach_disk_before_close},
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]);
}
