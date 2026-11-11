/* test_directory.c — pruebas del directorio de páginas (E2, #3).
 *
 * El directorio se prueba sin red: cada caso le entrega una secuencia de
 * eventos y revisa las acciones que emite. Hay dos familias:
 *
 *   - casos dirigidos, uno por criterio de aceptación del issue y por cada
 *     validación;
 *   - una prueba de estrés con trabajadores simulados que comprueba, después
 *     de cada evento, los invariantes internos y que ninguna concesión sirva
 *     datos viejos, ni por transferencia ni por caché.
 */
#include "directory.h"
#include "test_util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Registro de acciones --------------------------------------------- */

#define MAX_REC 64

/* Copia de una acción. `data` solo vale durante emit, así que se copian los
 * bytes y se guarda si venían. */
struct rec_action {
    struct dir_action a;
    bool              had_data;
    uint8_t           data[DSM_PAGE_SIZE];
};

struct recorder {
    size_t            n;
    struct rec_action act[MAX_REC];

    /* Para comprobar que la publicación precede a la confirmación: cuando
     * llega WRITE_PUBLISHED se mira el estado del directorio en ese instante. */
    const dir *d;
    bool       published_seen;
    uint64_t   version_at_ack;
    uint8_t    byte_at_ack;
};

static void record(void *user, const struct dir_action *a) {
    struct recorder *r = user;
    if (r->n >= MAX_REC)
        abort();   /* una prueba no debería producir tantas acciones */
    struct rec_action *ra = &r->act[r->n++];
    ra->a = *a;
    ra->had_data = a->data != NULL;
    if (a->data != NULL)
        memcpy(ra->data, a->data, DSM_PAGE_SIZE);
    ra->a.data = NULL;

    if (a->type == DIR_ACT_WRITE_PUBLISHED && r->d != NULL) {
        struct dir_page_info info;
        dir_page_info(r->d, a->page_id, &info);
        r->published_seen = true;
        r->version_at_ack = info.version;
        r->byte_at_ack    = dir_page_data(r->d, a->page_id)[0];
    }
}

static void rec_clear(struct recorder *r) {
    r->n = 0;
}

/* Cuántas acciones de un tipo hay hacia un nodo (UINT32_MAX = cualquiera). */
static size_t rec_count(const struct recorder *r, enum dir_action_type t, uint32_t node) {
    size_t c = 0;
    for (size_t i = 0; i < r->n; i++)
        if (r->act[i].a.type == t && (node == UINT32_MAX || r->act[i].a.node == node))
            c++;
    return c;
}

/* La primera acción de un tipo hacia un nodo, o NULL. */
static const struct rec_action *rec_find(const struct recorder *r,
                                         enum dir_action_type t, uint32_t node) {
    for (size_t i = 0; i < r->n; i++)
        if (r->act[i].a.type == t && r->act[i].a.node == node)
            return &r->act[i];
    return NULL;
}

static const uint64_t NOV = DSM_NO_VERSION;

/* Página llena de un byte, para publicar y reconocer versiones. */
static void fill(uint8_t page[DSM_PAGE_SIZE], uint8_t v) {
    memset(page, v, DSM_PAGE_SIZE);
}

static bool all_bytes(const uint8_t *p, uint8_t v) {
    for (size_t i = 0; i < DSM_PAGE_SIZE; i++)
        if (p[i] != v)
            return false;
    return true;
}

#define INV(d) CHECK(dir_check_invariants(d))

/* ---- Creación y validación -------------------------------------------- */

static bool test_create_rejects_bad_args(void) {
    struct recorder r = {0};
    CHECK(dir_create(0, 1, record, &r) == NULL);
    CHECK(dir_create(5, 1, record, &r) == NULL);
    CHECK(dir_create(1, 0, record, &r) == NULL);
    CHECK(dir_create(1, 257, record, &r) == NULL);
    CHECK(dir_create(1, 1, NULL, &r) == NULL);

    dir *d = dir_create(4, 256, record, &r);
    CHECK(d != NULL);
    CHECK(dir_nodes(d) == 4 && dir_pages(d) == 256);
    struct dir_page_info info;
    CHECK(dir_page_info(d, 255, &info) && info.version == 0 && info.writer == -1);
    CHECK(!dir_page_info(d, 256, &info));
    CHECK(all_bytes(dir_page_data(d, 0), 0));   /* inicialmente en cero */
    CHECK(dir_page_data(d, 256) == NULL);
    INV(d);
    dir_destroy(d);
    dir_destroy(NULL);
    return true;
}

static bool test_rejects_out_of_range_and_bad_mode(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 4, record, &r);
    CHECK(d != NULL);
    CHECK(dir_acquire(d, 2, 0, DSM_READ, 1, NOV) == DIR_EINVAL);   /* nodo */
    CHECK(dir_acquire(d, 0, 4, DSM_READ, 1, NOV) == DIR_EINVAL);   /* página */
    CHECK(dir_acquire(d, 0, UINT32_MAX, DSM_READ, 1, NOV) == DIR_EINVAL);
    CHECK(dir_acquire(d, 0, 0, (enum dsm_mode)0, 1, NOV) == DIR_EINVAL);
    CHECK(dir_acquire(d, 0, 0, (enum dsm_mode)3, 1, NOV) == DIR_EINVAL);
    CHECK(dir_acquire(NULL, 0, 0, DSM_READ, 1, NOV) == DIR_EINVAL);
    CHECK(dir_release_write(d, 0, 0, NULL, 1) == DIR_EINVAL);
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 1) == DIR_EINVAL);        /* solicitante */
    CHECK(dir_node_leave(d, 7) == DIR_EINVAL);
    CHECK(r.n == 0);   /* ningún rechazo emite nada */
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_rejects_out_of_state(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 2, record, &r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 9);

    /* Release sin acquire, en los dos modos. */
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_ESTATE);
    CHECK(dir_release_write(d, 0, 0, page, 1) == DIR_ESTATE);

    /* Adquisición anidada: en la misma página y en otra. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 2, NOV) == DIR_ESTATE);
    CHECK(dir_acquire(d, 0, 1, DSM_WRITE, 3, NOV) == DIR_ESTATE);

    /* Release de otro modo o de otra página. */
    CHECK(dir_release_write(d, 0, 0, page, 1) == DIR_ESTATE);
    CHECK(dir_release_read(d, 0, 1, 1) == DIR_ESTATE);

    /* Despedida con un acceso vivo. */
    CHECK(dir_node_leave(d, 0) == DIR_ESTATE);

    /* ACK que nadie pidió. */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 1) == DIR_ESTATE);

    rec_clear(&r);
    INV(d);
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_READ_RELEASED, 0) == 1);

    /* Un nodo que se despidió ya no puede hacer nada. */
    CHECK(dir_node_leave(d, 0) == DIR_OK);
    CHECK(dir_node_leave(d, 0) == DIR_ESTATE);
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 6, NOV) == DIR_ESTATE);
    INV(d);
    dir_destroy(d);
    return true;
}

/* ---- Correlación por request_id (D21) --------------------------------- */

static bool test_request_id_must_increase(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 1, record, &r);

    CHECK(dir_acquire(d, 0, 0, DSM_READ, 5, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 5) == DIR_OK);
    rec_clear(&r);

    /* Repetido o menor: rechazado sin emitir nada. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 5, NOV) == DIR_ESTATE);
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 3, NOV) == DIR_ESTATE);
    CHECK(r.n == 0);
    INV(d);

    /* Con saltos sí: los números gastados en barreras no cuentan. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 9, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 9) == DIR_OK);

    /* Cada nodo lleva su propia secuencia: el 5 de otro nodo es válido. */
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 5, NOV) == DIR_OK);
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_release_needs_its_request_id(void) {
    struct recorder r = {0};
    dir *d = dir_create(1, 1, record, &r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 4);

    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 4, NOV) == DIR_OK);
    rec_clear(&r);

    /* El escritor correcto con otro request_id: no publica ni confirma. */
    CHECK(dir_release_write(d, 0, 0, page, 3) == DIR_ESTATE);
    CHECK(dir_release_write(d, 0, 0, page, 5) == DIR_ESTATE);
    CHECK(r.n == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.version == 0 && info.writer == 0);
    CHECK(all_bytes(dir_page_data(d, 0), 0));

    /* Con el suyo, sí; y la confirmación repite ese número. */
    CHECK(dir_release_write(d, 0, 0, page, 4) == DIR_OK);
    const struct rec_action *ack = rec_find(&r, DIR_ACT_WRITE_PUBLISHED, 0);
    CHECK(ack && ack->a.request_id == 4 && ack->a.version == 1);

    /* Lo mismo en READ. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 7, 1) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 8) == DIR_ESTATE);
    rec_clear(&r);
    CHECK(dir_release_read(d, 0, 0, 7) == DIR_OK);
    ack = rec_find(&r, DIR_ACT_READ_RELEASED, 0);
    CHECK(ack && ack->a.request_id == 7);
    INV(d);
    dir_destroy(d);
    return true;
}

/* La secuencia del hallazgo 1 de la revisión del PR #12: el mismo nodo pide
 * dos WRITE seguidos sobre la misma página. Antes, si repetía el request_id,
 * el ACK rezagado de la primera ronda completaba la segunda. */
static bool test_stale_ack_same_requester(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 1);

    CHECK(dir_acquire(d, 1, 0, DSM_READ, 100, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 100) == DIR_OK);          /* n1 retiene */
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 5, NOV) == DIR_OK);  /* ronda (0, 5) */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 5) == DIR_OK);
    CHECK(dir_release_write(d, 0, 0, page, 5) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 101, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 101) == DIR_OK);          /* n0 y n1 retienen */
    rec_clear(&r);

    /* Reutilizar el 5 ya no es posible. */
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 5, NOV) == DIR_ESTATE);
    CHECK(r.n == 0);

    /* Con el siguiente número, la ronda nueva es (0, 6). */
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 6, 1) == DIR_OK);
    const struct rec_action *inv = rec_find(&r, DIR_ACT_INVALIDATE, 1);
    CHECK(inv && inv->a.request_id == 6 && inv->a.requester == 0);
    rec_clear(&r);

    /* El ACK rezagado de la ronda (0, 5) no cuenta y no concede nada. */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 5) == DIR_ESTATE);
    CHECK(r.n == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.pending_acks == 1 && info.writer == -1);
    INV(d);

    /* El verdadero, sí. */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 6) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 0) == 1);
    INV(d);
    dir_destroy(d);
    return true;
}

/* El caso que la regla de request_id creciente sola no cubre: dos
 * solicitantes distintos usan el mismo número, porque cada nodo lleva su
 * propia secuencia. Por eso el ACK repite también al solicitante. */
static bool test_stale_ack_other_requester_same_id(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 2);

    CHECK(dir_acquire(d, 1, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 1) == DIR_OK);
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 5, NOV) == DIR_OK);  /* ronda (0, 5) */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 5) == DIR_OK);
    CHECK(dir_release_write(d, 0, 0, page, 5) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 2, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 2) == DIR_OK);
    rec_clear(&r);

    /* El nodo 2 estrena su secuencia y también usa el 5: ronda (2, 5). */
    CHECK(dir_acquire(d, 2, 0, DSM_WRITE, 5, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 2);
    rec_clear(&r);

    /* Un ACK rezagado de n1 para la ronda (0, 5) tiene el mismo request_id,
     * pero otro solicitante: se rechaza. */
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 5) == DIR_ESTATE);
    CHECK(r.n == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.pending_acks == 2);
    INV(d);

    CHECK(dir_invalidate_ack(d, 0, 0, 2, 5) == DIR_OK);
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 5) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 1);
    INV(d);
    dir_destroy(d);
    return true;
}

/* ---- Lectores ---------------------------------------------------------- */

static bool test_concurrent_readers(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);

    CHECK(dir_acquire(d, 0, 0, DSM_READ, 10, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 11, NOV) == DIR_OK);
    INV(d);

    /* Los dos concedidos de inmediato, con datos (no tenían copia), versión 0,
     * y cada concesión correlacionada con su solicitud. */
    CHECK(r.n == 2);
    const struct rec_action *g0 = rec_find(&r, DIR_ACT_GRANT, 0);
    const struct rec_action *g1 = rec_find(&r, DIR_ACT_GRANT, 1);
    CHECK(g0 && g0->had_data && g0->a.version == 0 && g0->a.request_id == 10);
    CHECK(g1 && g1->had_data && g1->a.version == 0 && g1->a.request_id == 11);
    CHECK(g0->a.mode == DSM_READ && all_bytes(g0->data, 0));

    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.reader_count == 2 && info.writer == -1 && info.holders == 0x3);
    dir_destroy(d);
    return true;
}

static bool test_version_match_skips_data_not_permission(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 1, record, &r);

    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    rec_clear(&r);

    /* Mismo nodo, copia retenida, misma versión: permiso concedido SIN datos.
     * El permiso se pidió y se registró: el nodo vuelve a ser lector activo. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 3, 0) == DIR_OK);
    const struct rec_action *g = rec_find(&r, DIR_ACT_GRANT, 0);
    CHECK(g && !g->had_data && g->a.version == 0 && g->a.request_id == 3);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.readers == 0x1);
    CHECK(dir_release_read(d, 0, 0, 3) == DIR_OK);
    rec_clear(&r);

    /* El trabajador descartó su copia: declara DSM_NO_VERSION y recibe datos. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 5, NOV) == DIR_OK);
    g = rec_find(&r, DIR_ACT_GRANT, 0);
    CHECK(g && g->had_data);
    CHECK(dir_release_read(d, 0, 0, 5) == DIR_OK);
    rec_clear(&r);

    /* Un nodo SIN copia retenida que declara la versión correcta recibe
     * datos igual: el directorio no confía en la declaración sola. */
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 7, 0) == DIR_OK);
    g = rec_find(&r, DIR_ACT_GRANT, 1);
    CHECK(g && g->had_data);
    INV(d);
    dir_destroy(d);
    return true;
}

/* ---- Escritura --------------------------------------------------------- */

/* El escenario de coherencia del enunciado: dos lectores, un escritor
 * posterior y una lectura nueva. */
static bool test_write_waits_for_readers_and_acks(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);
    r.d = d;

    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 1, NOV) == DIR_OK);
    rec_clear(&r);

    /* WRITE de 2 con lectores activos: ni concesión ni invalidaciones aún. */
    CHECK(dir_acquire(d, 2, 0, DSM_WRITE, 7, NOV) == DIR_OK);
    CHECK(r.n == 0);
    INV(d);

    /* Termina un lector: todavía queda otro acceso activo. */
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 0);
    CHECK(rec_count(&r, DIR_ACT_GRANT, UINT32_MAX) == 0);
    rec_clear(&r);

    /* Termina el otro: se invalidan las dos copias retenidas, no la del
     * solicitante, y cada INVALIDATE lleva la solicitud y al solicitante. */
    CHECK(dir_release_read(d, 1, 0, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 2);
    const struct rec_action *i0 = rec_find(&r, DIR_ACT_INVALIDATE, 0);
    CHECK(i0 && i0->a.request_id == 7 && i0->a.requester == 2);
    CHECK(rec_find(&r, DIR_ACT_INVALIDATE, 2) == NULL);
    CHECK(rec_count(&r, DIR_ACT_GRANT, UINT32_MAX) == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.pending_acks == 2);
    INV(d);
    rec_clear(&r);

    /* Primer ACK: falta otro, no hay concesión. */
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 7) == DIR_OK);
    CHECK(r.n == 0);
    INV(d);

    /* El escritor recibe la concesión solo después del último ACK. */
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 7) == DIR_OK);
    const struct rec_action *g = rec_find(&r, DIR_ACT_GRANT, 2);
    CHECK(g && g->a.mode == DSM_WRITE && g->had_data && g->a.request_id == 7);
    dir_page_info(d, 0, &info);
    CHECK(info.writer == 2 && info.holders == 0x4 && info.pending_acks == 0);
    INV(d);
    rec_clear(&r);

    /* Publica: la confirmación sale con la versión nueva ya instalada. */
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 0xAB);
    CHECK(dir_release_write(d, 2, 0, page, 7) == DIR_OK);
    CHECK(r.published_seen && r.version_at_ack == 1 && r.byte_at_ack == 0xAB);
    const struct rec_action *ack = rec_find(&r, DIR_ACT_WRITE_PUBLISHED, 2);
    CHECK(ack && ack->a.version == 1 && ack->a.request_id == 7);
    rec_clear(&r);

    /* Nueva lectura de un antiguo lector: datos nuevos, versión 1, aunque
     * declare la versión 0 que tenía. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 9, 0) == DIR_OK);
    g = rec_find(&r, DIR_ACT_GRANT, 0);
    CHECK(g && g->had_data && g->a.version == 1 && all_bytes(g->data, 0xAB));
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_duplicate_ack_not_counted(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);

    /* Copias retenidas en 0 y 1. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 1) == DIR_OK);
    rec_clear(&r);

    CHECK(dir_acquire(d, 2, 0, DSM_WRITE, 5, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 2);
    rec_clear(&r);

    /* El ACK repetido de 0 no cuenta como el de 1: no hay concesión. */
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 5) == DIR_OK);
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 5) == DIR_DUPLICATE);
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 5) == DIR_DUPLICATE);
    CHECK(r.n == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.pending_acks == 1 && info.writer == -1);
    INV(d);

    /* Un ACK con otro request_id u otro solicitante tampoco cuenta. */
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 6) == DIR_ESTATE);
    CHECK(dir_invalidate_ack(d, 1, 0, 0, 5) == DIR_ESTATE);
    CHECK(r.n == 0);

    CHECK(dir_invalidate_ack(d, 1, 0, 2, 5) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 1);

    /* Después de cerrada la ronda, el repetido sigue reconociéndose. */
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 5) == DIR_DUPLICATE);
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_reader_does_not_jump_queued_writer(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);

    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);   /* lector activo */
    CHECK(dir_acquire(d, 1, 0, DSM_WRITE, 1, NOV) == DIR_OK);  /* WRITE en cola */
    rec_clear(&r);

    /* Un lector nuevo es compatible con el lector activo, pero hay un WRITE
     * anterior en la cola: espera detrás. */
    CHECK(dir_acquire(d, 2, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(r.n == 0);
    struct dir_page_info info;
    dir_page_info(d, 0, &info);
    CHECK(info.queued == 2 && info.readers == 0x1);
    INV(d);

    /* Sale el lector: se invalida su copia; tras el ACK, WRITE para 1. */
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, 0) == 1);
    CHECK(rec_count(&r, DIR_ACT_GRANT, UINT32_MAX) == 0);
    CHECK(dir_invalidate_ack(d, 0, 0, 1, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 1) == 1);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 0);
    rec_clear(&r);

    /* Publica 1: primero su confirmación, después la lectura de 2, que ve la
     * versión nueva. */
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 0x11);
    CHECK(dir_release_write(d, 1, 0, page, 1) == DIR_OK);
    CHECK(r.n == 2);
    CHECK(r.act[0].a.type == DIR_ACT_WRITE_PUBLISHED && r.act[0].a.node == 1);
    CHECK(r.act[1].a.type == DIR_ACT_GRANT && r.act[1].a.node == 2);
    CHECK(r.act[1].a.version == 1 && all_bytes(r.act[1].data, 0x11));
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_old_version_never_served_during_write(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 1, record, &r);

    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 1, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 0) == 1);
    rec_clear(&r);

    /* Mientras 0 escribe, ni una lectura ni otra escritura reciben nada,
     * aunque el directorio todavía tenga la versión 0 en sus datos. */
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 2, 0, DSM_WRITE, 1, NOV) == DIR_OK);
    CHECK(r.n == 0);
    CHECK(all_bytes(dir_page_data(d, 0), 0));
    INV(d);

    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 0x5A);
    CHECK(dir_release_write(d, 0, 0, page, 1) == DIR_OK);

    /* El lector recibe la versión 1, no la 0. El WRITE de 2 queda detrás. */
    const struct rec_action *g = rec_find(&r, DIR_ACT_GRANT, 1);
    CHECK(g && g->a.version == 1 && all_bytes(g->data, 0x5A));
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 0);
    INV(d);
    rec_clear(&r);

    /* Al salir el lector, se invalidan las copias de 0 (el escritor anterior)
     * y de 1; con los ACK, 2 escribe sobre la versión 1. */
    CHECK(dir_release_read(d, 1, 0, 1) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 2);
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 1) == DIR_OK);
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 1) == DIR_OK);
    g = rec_find(&r, DIR_ACT_GRANT, 2);
    CHECK(g && g->a.version == 1 && g->had_data && all_bytes(g->data, 0x5A));
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_writer_keeps_its_copy(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 1, record, &r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 3);

    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 1, NOV) == DIR_OK);
    CHECK(dir_release_write(d, 0, 0, page, 1) == DIR_OK);
    rec_clear(&r);

    /* Escribe otra vez con su copia vigente: sin invalidaciones ni datos. Es
     * el patrón del contador cuando un solo nodo trabaja. */
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 3, 1) == DIR_OK);
    CHECK(r.n == 1);
    CHECK(r.act[0].a.type == DIR_ACT_GRANT && !r.act[0].had_data);
    INV(d);
    dir_destroy(d);
    return true;
}

static bool test_pages_are_independent(void) {
    struct recorder r = {0};
    dir *d = dir_create(2, 2, record, &r);

    /* Un escritor en la página 0 no detiene una lectura de la página 1. */
    CHECK(dir_acquire(d, 0, 0, DSM_WRITE, 1, NOV) == DIR_OK);
    CHECK(dir_acquire(d, 1, 1, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 1) == 1);
    INV(d);
    dir_destroy(d);
    return true;
}

/* ---- Despedida (D13) --------------------------------------------------- */

static bool test_leave_counts_as_ack(void) {
    struct recorder r = {0};
    dir *d = dir_create(3, 2, record, &r);

    /* 0 retiene copias de las dos páginas; 1 retiene la de la página 0. */
    CHECK(dir_acquire(d, 0, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 0, 1) == DIR_OK);
    CHECK(dir_acquire(d, 0, 1, DSM_READ, 3, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 0, 1, 3) == DIR_OK);
    CHECK(dir_acquire(d, 1, 0, DSM_READ, 1, NOV) == DIR_OK);
    CHECK(dir_release_read(d, 1, 0, 1) == DIR_OK);
    rec_clear(&r);

    CHECK(dir_acquire(d, 2, 0, DSM_WRITE, 9, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 2);
    CHECK(dir_invalidate_ack(d, 1, 0, 2, 9) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 0);
    rec_clear(&r);

    /* 0 se despide sin haber confirmado: cuenta como su ACK y el WRITE sale. */
    CHECK(dir_node_leave(d, 0) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 2) == 1);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 0);
    INV(d);

    /* Y su ACK tardío ya no se acepta, ni como bueno ni como nuevo. */
    CHECK(dir_invalidate_ack(d, 0, 0, 2, 9) == DIR_ESTATE);

    /* Su copia de la página 1 también desapareció: un WRITE allí no le manda
     * INVALIDATE a nadie. */
    rec_clear(&r);
    uint8_t page[DSM_PAGE_SIZE];
    fill(page, 1);
    CHECK(dir_release_write(d, 2, 0, page, 9) == DIR_OK);
    rec_clear(&r);
    CHECK(dir_acquire(d, 1, 1, DSM_WRITE, 11, NOV) == DIR_OK);
    CHECK(rec_count(&r, DIR_ACT_INVALIDATE, UINT32_MAX) == 0);
    CHECK(rec_count(&r, DIR_ACT_GRANT, 1) == 1);
    INV(d);
    dir_destroy(d);
    return true;
}

/* ---- Estrés con trabajadores simulados -------------------------------- */

/* PRNG pequeño y determinista (xorshift64*), para que una falla se pueda
 * reproducir con la misma semilla. */
static uint64_t rng_state;

static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ull) >> 33) % n;
}

#define S_NODES 4
#define S_PAGES 3

/* Lo que un trabajador sabe de sí mismo, como lo sabría libdsm. */
struct sim_node {
    enum { S_IDLE, S_WAITING, S_HOLDING } state;
    uint32_t      page;
    enum dsm_mode mode;
    uint64_t      next_req;
    uint64_t      req;
    /* Caché local: versión y contenido por página, o DSM_NO_VERSION. */
    uint64_t      cached[S_PAGES];
    uint8_t       cache_byte[S_PAGES];
    /* INVALIDATE recibidos y aún no confirmados, con la clave que el ACK
     * tiene que repetir: solicitud y solicitante. */
    bool          inval_due[S_PAGES];
    uint64_t      inval_req[S_PAGES];
    uint32_t      inval_from[S_PAGES];
};

struct sim {
    dir            *d;
    struct sim_node n[S_NODES];
    /* Modelo de referencia: la última versión publicada de cada página y el
     * byte con que se publicó. Es lo que cualquier concesión debe entregar. */
    uint64_t        version[S_PAGES];
    uint8_t         byte[S_PAGES];
    bool            failed;
    uint64_t        grants, writes, invalidations, data_transfers, cache_hits;
};

static void sim_emit(void *user, const struct dir_action *a) {
    struct sim *s = user;
    struct sim_node *n = &s->n[a->node];

    switch (a->type) {
    case DIR_ACT_GRANT:
        s->grants++;
        if (n->state != S_WAITING || n->page != a->page_id || n->req != a->request_id ||
            n->mode != a->mode)
            s->failed = true;   /* concesión que nadie esperaba */
        if (a->version != s->version[a->page_id])
            s->failed = true;   /* versión vieja */
        if (a->data != NULL) {
            s->data_transfers++;
            if (!all_bytes(a->data, s->byte[a->page_id]))
                s->failed = true;   /* datos viejos por la red */
            n->cached[a->page_id]     = a->version;
            n->cache_byte[a->page_id] = a->data[0];
        } else {
            /* Sin datos: el trabajador usa su caché. Tiene que ser vigente. */
            s->cache_hits++;
            if (n->cached[a->page_id] != a->version ||
                n->cache_byte[a->page_id] != s->byte[a->page_id])
                s->failed = true;   /* datos viejos por caché */
        }
        n->state = S_HOLDING;
        break;
    case DIR_ACT_INVALIDATE:
        s->invalidations++;
        if (a->node == a->requester)
            s->failed = true;
        /* Nunca se invalida a quien tiene un acceso activo sobre la página:
         * el WRITE espera a que terminen los accesos antes de invalidar. */
        if (n->state == S_HOLDING && n->page == a->page_id)
            s->failed = true;
        /* El receptor de libdsm descarta la copia al recibirla; el ACK se
         * manda más tarde, en un paso aleatorio, para mezclar los órdenes. */
        n->cached[a->page_id]    = DSM_NO_VERSION;
        n->inval_due[a->page_id] = true;
        n->inval_req[a->page_id]  = a->request_id;
        n->inval_from[a->page_id] = a->requester;
        break;
    case DIR_ACT_WRITE_PUBLISHED:
        s->writes++;
        if (a->version != s->version[a->page_id] || a->request_id != n->req)
            s->failed = true;
        n->cached[a->page_id] = a->version;
        n->state = S_IDLE;
        break;
    case DIR_ACT_READ_RELEASED:
        if (a->request_id != n->req)
            s->failed = true;   /* confirmación de otra operación */
        n->state = S_IDLE;
        break;
    }
}

/* Un paso aleatorio del sistema. Devuelve false si el directorio rechazó un
 * evento que un trabajador correcto podía mandar. */
static bool sim_step(struct sim *s) {
    uint32_t id = rnd(S_NODES);
    struct sim_node *n = &s->n[id];

    /* A veces, confirmar una invalidación pendiente. */
    if (rnd(3) == 0) {
        for (uint32_t p = 0; p < S_PAGES; p++) {
            if (n->inval_due[p]) {
                n->inval_due[p] = false;
                return dir_invalidate_ack(s->d, id, p, n->inval_from[p],
                                          n->inval_req[p]) == DIR_OK;
            }
        }
    }

    if (n->state == S_IDLE) {
        n->page = rnd(S_PAGES);
        n->mode = rnd(2) ? DSM_READ : DSM_WRITE;
        /* Crece con saltos, como cuando la biblioteca gasta números en
         * barreras entre un acceso y otro. Los nodos arrancan en el mismo
         * número, así que se repiten request_id entre nodos a propósito. */
        n->next_req += 1u + rnd(3);
        n->req   = n->next_req;
        n->state = S_WAITING;
        return dir_acquire(s->d, id, n->page, n->mode, n->req, n->cached[n->page]) == DIR_OK;
    }
    if (n->state == S_HOLDING) {
        uint64_t rel = n->req;   /* el release lleva el id del acquire */
        if (n->mode == DSM_READ)
            return dir_release_read(s->d, id, n->page, rel) == DIR_OK;
        uint8_t page[DSM_PAGE_SIZE];
        uint8_t b = (uint8_t)(s->byte[n->page] + 1u + id);
        fill(page, b);
        n->cache_byte[n->page] = b;
        /* El modelo cambia antes de que el directorio confirme, porque desde
         * el punto de vista del protocolo la publicación ocurre al procesar
         * el REL_WRITE, antes de su ACK. */
        s->version[n->page]++;
        s->byte[n->page] = b;
        return dir_release_write(s->d, id, n->page, page, rel) == DIR_OK;
    }
    return true;   /* esperando: no hace nada */
}

/* Libera todo lo adquirido y confirma toda invalidación, hasta que nadie
 * espere. Si algo queda esperando sin nada pendiente, hay un bloqueo. */
static bool sim_drain(struct sim *s) {
    for (int round = 0; round < 1000; round++) {
        bool busy = false;
        for (uint32_t id = 0; id < S_NODES; id++) {
            struct sim_node *n = &s->n[id];
            for (uint32_t p = 0; p < S_PAGES; p++) {
                if (n->inval_due[p]) {
                    n->inval_due[p] = false;
                    if (dir_invalidate_ack(s->d, id, p, n->inval_from[p],
                                           n->inval_req[p]) != DIR_OK)
                        return false;
                    busy = true;
                }
            }
            if (n->state == S_HOLDING) {
                uint64_t rel = n->req;
                if (n->mode == DSM_READ) {
                    if (dir_release_read(s->d, id, n->page, rel) != DIR_OK)
                        return false;
                } else {
                    uint8_t page[DSM_PAGE_SIZE];
                    fill(page, s->byte[n->page]);
                    s->version[n->page]++;
                    if (dir_release_write(s->d, id, n->page, page, rel) != DIR_OK)
                        return false;
                }
                busy = true;
            }
            if (n->state == S_WAITING)
                busy = true;
        }
        if (!busy)
            return true;
    }
    return false;   /* alguien sigue esperando sin que nada lo destrabe */
}

static bool run_stress(uint64_t seed, int steps) {
    struct sim s;
    memset(&s, 0, sizeof s);
    rng_state = seed;
    s.d = dir_create(S_NODES, S_PAGES, sim_emit, &s);
    CHECK(s.d != NULL);
    for (uint32_t i = 0; i < S_NODES; i++)
        for (uint32_t p = 0; p < S_PAGES; p++)
            s.n[i].cached[p] = DSM_NO_VERSION;

    for (int i = 0; i < steps; i++) {
        CHECK(sim_step(&s));
        CHECK(!s.failed);
        CHECK(dir_check_invariants(s.d));
    }
    CHECK(sim_drain(&s));
    CHECK(!s.failed);
    CHECK(dir_check_invariants(s.d));

    /* Que el recorrido de verdad ejercitó todos los caminos. */
    CHECK(s.writes > 100 && s.invalidations > 100);
    CHECK(s.data_transfers > 100 && s.cache_hits > 10);

    /* Al final todos se despiden y no queda estado de nadie. */
    for (uint32_t id = 0; id < S_NODES; id++)
        CHECK(dir_node_leave(s.d, id) == DIR_OK);
    CHECK(dir_check_invariants(s.d));
    for (uint32_t p = 0; p < S_PAGES; p++) {
        struct dir_page_info info;
        dir_page_info(s.d, p, &info);
        CHECK(info.holders == 0 && info.queued == 0 && info.writer == -1);
        CHECK(info.version == s.version[p]);
    }
    dir_destroy(s.d);
    return true;
}

static bool test_stress_random_schedules(void) {
    /* Varias semillas: cada una es un entrelazado distinto y reproducible. */
    for (uint64_t seed = 1; seed <= 20; seed++) {
        if (!run_stress(seed * 0x9E3779B97F4A7C15ull, 5000)) {
            fprintf(stderr, "  semilla que falla: %llu\n", (unsigned long long)seed);
            return false;
        }
    }
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"create_rejects_bad_args", test_create_rejects_bad_args},
        {"rejects_out_of_range_and_bad_mode", test_rejects_out_of_range_and_bad_mode},
        {"rejects_out_of_state", test_rejects_out_of_state},
        {"request_id_must_increase", test_request_id_must_increase},
        {"release_needs_its_request_id", test_release_needs_its_request_id},
        {"stale_ack_same_requester", test_stale_ack_same_requester},
        {"stale_ack_other_requester_same_id", test_stale_ack_other_requester_same_id},
        {"concurrent_readers", test_concurrent_readers},
        {"version_match_skips_data_not_permission", test_version_match_skips_data_not_permission},
        {"write_waits_for_readers_and_acks", test_write_waits_for_readers_and_acks},
        {"duplicate_ack_not_counted", test_duplicate_ack_not_counted},
        {"reader_does_not_jump_queued_writer", test_reader_does_not_jump_queued_writer},
        {"old_version_never_served_during_write", test_old_version_never_served_during_write},
        {"writer_keeps_its_copy", test_writer_keeps_its_copy},
        {"pages_are_independent", test_pages_are_independent},
        {"leave_counts_as_ack", test_leave_counts_as_ack},
        {"stress_random_schedules", test_stress_random_schedules},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
