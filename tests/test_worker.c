/* test_worker.c — reglas del trabajador, sin red ni hilos (#5).
 *
 * worker.c es la parte de libdsm que decide qué hacer con cada trama del
 * coordinador. Acá se le alimentan tramas a mano y se mira el estado: una
 * prueba por regla de wk_apply (worker.h), y cada una comprueba también que
 * una trama rechazada no cambió nada. Lo que necesita hilos y sockets —que la
 * invalidación se confirme con la aplicación bloqueada, los plazos, la
 * despedida— está en test_dsm.c y en test_e2e_dsm.c.
 */
#include "worker.h"
#include "test_util.h"

#include <string.h>

#define NID   1u
#define PAGES 3u

static struct wire_header hdr(uint8_t type, uint32_t page, uint64_t req,
                              uint32_t aux) {
    struct wire_header h;

    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type = type;
    h.node_id = NID;
    h.page_id = page;
    h.request_id = req;
    h.aux = aux;
    return h;
}

static void expect_grant(struct wk_state *w, uint32_t page, uint64_t req,
                         enum dsm_mode mode) {
    struct wk_wait a;

    memset(&a, 0, sizeof a);
    a.kind = WK_GRANT;
    a.page = page;
    a.request = req;
    a.mode = mode;
    wk_expect(w, &a);
}

static void expect_release(struct wk_state *w, enum wk_wait_kind kind,
                           uint32_t page, uint64_t req) {
    struct wk_wait a;

    memset(&a, 0, sizeof a);
    a.kind = kind;
    a.page = page;
    a.request = req;
    wk_expect(w, &a);
}

/* Concesión con o sin la página. `fill` es el byte con que se llena. */
static enum dsm_status grant(struct wk_state *w, uint32_t page, uint64_t req,
                             uint64_t version, int fill, struct wk_effect *e) {
    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, page, req, 0u);
    uint8_t p[8u + DSM_PAGE_SIZE];

    wire_put_u64(p, version);
    h.length = 8u;
    if (fill >= 0) {
        memset(p + 8, fill, DSM_PAGE_SIZE);
        h.flags = DSM_WIRE_F_HAS_DATA;
        h.length = 8u + DSM_PAGE_SIZE;
    }
    return wk_apply(w, &h, p, e);
}

static enum dsm_status ack_write(struct wk_state *w, uint32_t page, uint64_t req,
                                 uint64_t version, struct wk_effect *e) {
    struct wire_header h = hdr(DSM_WIRE_REL_WRITE_ACK, page, req, 0u);
    uint8_t p[8];

    h.length = 8u;
    wire_put_u64(p, version);
    return wk_apply(w, &h, p, e);
}

static enum dsm_status simple(struct wk_state *w, uint8_t type, uint32_t page,
                              uint64_t req, uint32_t aux, struct wk_effect *e) {
    struct wire_header h = hdr(type, page, req, aux);
    return wk_apply(w, &h, NULL, e);
}

/* Lleva `page` a copia retenida con `version`: concesión READ con datos y su
 * liberación confirmada. */
static bool cache_page(struct wk_state *w, uint32_t page, uint64_t req,
                       uint64_t version) {
    struct wk_effect e;

    expect_grant(w, page, req, DSM_READ);
    CHECK(grant(w, page, req, version, 0x5A, &e) == DSM_OK);
    expect_release(w, WK_REL_READ, page, req);
    CHECK(simple(w, DSM_WIRE_REL_READ_ACK, page, req, 0u, &e) == DSM_OK);
    CHECK(w->slot[page].state == WK_CACHED);
    return true;
}

/* Dos estados iguales, byte por byte, incluidos los marcos. */
static bool same_state(const struct wk_state *a, const struct wk_state *b) {
    return memcmp(a->slot, b->slot, PAGES * sizeof *a->slot) == 0 &&
           memcmp(a->frames, b->frames, (size_t)PAGES * DSM_PAGE_SIZE) == 0 &&
           a->wait.kind == b->wait.kind && a->wait.page == b->wait.page &&
           a->wait.request == b->wait.request && a->wait.mode == b->wait.mode &&
           a->wait.barrier == b->wait.barrier && a->wait.gen == b->wait.gen &&
           a->wait.done == b->wait.done;
}

/* Copia profunda, para comparar el antes y el después de un rechazo. */
static bool snapshot(const struct wk_state *w, struct wk_state *copy) {
    if (!wk_init(copy, w->nid, w->pages))
        return false;
    memcpy(copy->slot, w->slot, PAGES * sizeof *w->slot);
    memcpy(copy->frames, w->frames, (size_t)PAGES * DSM_PAGE_SIZE);
    copy->wait = w->wait;
    return true;
}

/* ---- Creación ----------------------------------------------------------- */

static bool test_init_rejects_bad_args(void) {
    struct wk_state w;

    CHECK(!wk_init(NULL, 0, 1));
    CHECK(!wk_init(&w, DSM_MAX_NODES, 1));
    CHECK(!wk_init(&w, 0, 0));
    CHECK(!wk_init(&w, 0, DSM_MAX_PAGES + 1u));
    CHECK(wk_init(&w, 0, DSM_MAX_PAGES));
    wk_free(&w);
    wk_free(&w);          /* idempotente */
    wk_free(NULL);
    return true;
}

static bool test_starts_empty_and_zeroed(void) {
    struct wk_state w;

    CHECK(wk_init(&w, NID, PAGES));
    for (uint32_t p = 0; p < PAGES; p++) {
        CHECK(w.slot[p].state == WK_EMPTY);
        CHECK(wk_offer(&w, p) == DSM_NO_VERSION);
        for (size_t i = 0; i < DSM_PAGE_SIZE; i++)
            CHECK(wk_frame(&w, p)[i] == 0u);
    }
    CHECK(wk_offer(&w, PAGES) == DSM_NO_VERSION);   /* fuera de rango */
    wk_free(&w);
    return true;
}

/* ---- Concesiones -------------------------------------------------------- */

static bool test_grant_with_data_fills_frame(void) {
    struct wk_state w;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    expect_grant(&w, 2, 10, DSM_WRITE);
    CHECK(grant(&w, 2, 10, 7, 0xAB, &e) == DSM_OK);
    CHECK(e.completed && !e.ack && e.event == LOG_ACQ_GRANT && e.version == 7u);
    CHECK(w.wait.done);
    CHECK(w.slot[2].state == WK_WRITE && w.slot[2].version == 7u);
    CHECK(wk_frame(&w, 2)[0] == 0xAB && wk_frame(&w, 2)[DSM_PAGE_SIZE - 1] == 0xAB);
    CHECK(wk_frame(&w, 1)[0] == 0u);                 /* las demás, intactas */
    CHECK(wk_offer(&w, 2) == DSM_NO_VERSION);        /* permiso, no copia */
    wk_free(&w);
    return true;
}

/* La copia retenida se ofrece, y una concesión sin datos de esa versión la
 * reusa sin tocar los bytes. */
static bool test_grant_without_data_reuses_cached_copy(void) {
    struct wk_state w;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    CHECK(cache_page(&w, 0, 1, 4));
    CHECK(wk_offer(&w, 0) == 4u);

    expect_grant(&w, 0, 2, DSM_READ);
    CHECK(grant(&w, 0, 2, 4, -1, &e) == DSM_OK);
    CHECK(w.slot[0].state == WK_READ);
    CHECK(wk_frame(&w, 0)[100] == 0x5A);
    wk_free(&w);
    return true;
}

static bool test_grant_without_data_needs_matching_copy(void) {
    struct wk_state w, before;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));

    /* Sin copia. */
    expect_grant(&w, 1, 1, DSM_READ);
    CHECK(snapshot(&w, &before));
    CHECK(grant(&w, 1, 1, 0, -1, &e) == DSM_EPROTO);
    CHECK(same_state(&w, &before));
    wk_free(&before);

    /* Con copia, pero de otra versión. */
    CHECK(cache_page(&w, 0, 2, 4));
    expect_grant(&w, 0, 3, DSM_READ);
    CHECK(snapshot(&w, &before));
    CHECK(grant(&w, 0, 3, 5, -1, &e) == DSM_EPROTO);
    CHECK(same_state(&w, &before));
    wk_free(&before);
    wk_free(&w);
    return true;
}

/* Una concesión tiene que responder a lo anotado: página, request_id, y que
 * haya algo anotado y no respondido ya. */
static bool test_grant_must_answer_the_pending_request(void) {
    struct wk_state w, before;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    CHECK(grant(&w, 0, 1, 0, 0, &e) == DSM_EPROTO);          /* nada anotado */

    expect_grant(&w, 0, 5, DSM_READ);
    CHECK(snapshot(&w, &before));
    CHECK(grant(&w, 1, 5, 0, 0, &e) == DSM_EPROTO);          /* otra página */
    CHECK(grant(&w, 0, 4, 0, 0, &e) == DSM_EPROTO);          /* otro request */
    CHECK(same_state(&w, &before));
    wk_free(&before);

    CHECK(grant(&w, 0, 5, 0, 0, &e) == DSM_OK);
    CHECK(grant(&w, 0, 5, 0, 0, &e) == DSM_EPROTO);          /* respondida ya */

    expect_release(&w, WK_REL_READ, 0, 5);
    CHECK(grant(&w, 0, 5, 0, 0, &e) == DSM_EPROTO);          /* se espera otra cosa */
    wk_free(&w);
    return true;
}

/* ---- Liberaciones ------------------------------------------------------- */

static bool test_publish_bumps_version_by_one_and_keeps_copy(void) {
    struct wk_state w, before;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    expect_grant(&w, 1, 3, DSM_WRITE);
    CHECK(grant(&w, 1, 3, 8, 0, &e) == DSM_OK);

    expect_release(&w, WK_REL_WRITE, 1, 3);
    CHECK(snapshot(&w, &before));
    CHECK(ack_write(&w, 1, 3, 8, &e) == DSM_EPROTO);         /* no subió */
    CHECK(ack_write(&w, 1, 3, 10, &e) == DSM_EPROTO);        /* subió dos */
    CHECK(ack_write(&w, 1, 4, 9, &e) == DSM_EPROTO);         /* otro request */
    CHECK(same_state(&w, &before));
    wk_free(&before);

    CHECK(ack_write(&w, 1, 3, 9, &e) == DSM_OK);
    CHECK(e.completed && e.event == LOG_WRITE_PUBLISHED && e.version == 9u);
    CHECK(w.slot[1].state == WK_CACHED && wk_offer(&w, 1) == 9u);
    wk_free(&w);
    return true;
}

static bool test_release_ack_needs_the_matching_permission(void) {
    struct wk_state w;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    expect_grant(&w, 0, 1, DSM_READ);
    CHECK(grant(&w, 0, 1, 0, 0, &e) == DSM_OK);

    /* Se espera la confirmación de un READ, pero llega la de un WRITE. */
    expect_release(&w, WK_REL_READ, 0, 1);
    CHECK(ack_write(&w, 0, 1, 1, &e) == DSM_EPROTO);
    CHECK(simple(&w, DSM_WIRE_REL_READ_ACK, 0, 2, 0u, &e) == DSM_EPROTO);
    CHECK(simple(&w, DSM_WIRE_REL_READ_ACK, 0, 1, 0u, &e) == DSM_OK);
    CHECK(e.event == LOG_READ_RELEASED && w.slot[0].state == WK_CACHED);
    CHECK(wk_offer(&w, 0) == 0u);

    /* La espera anotada no alcanza: la página tiene que tener el permiso
     * que se libera. Una confirmación sobre una copia retenida pasaría a
     * "liberar" un acceso que no existe. */
    expect_release(&w, WK_REL_READ, 0, 7);
    CHECK(simple(&w, DSM_WIRE_REL_READ_ACK, 0, 7, 0u, &e) == DSM_EPROTO);
    expect_release(&w, WK_REL_WRITE, 0, 7);
    CHECK(ack_write(&w, 0, 7, 1, &e) == DSM_EPROTO);
    CHECK(w.slot[0].state == WK_CACHED && w.slot[0].version == 0u);
    wk_free(&w);
    return true;
}

/* ---- Invalidaciones ----------------------------------------------------- */

static bool test_invalidate_drops_copy_and_asks_for_ack(void) {
    struct wk_state w;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    CHECK(cache_page(&w, 2, 1, 3));
    CHECK(simple(&w, DSM_WIRE_INVALIDATE, 2, 77, 3u, &e) == DSM_OK);
    CHECK(e.ack && !e.completed && e.event == LOG_INVALIDATE);
    CHECK(w.slot[2].state == WK_INVALID);
    CHECK(wk_offer(&w, 2) == DSM_NO_VERSION);

    /* Una invalidación de una página sin copia también se confirma: el
     * coordinador puede mandarla por una copia que ya se descartó, y sin el
     * ACK su ronda no cerraría nunca. */
    CHECK(simple(&w, DSM_WIRE_INVALIDATE, 0, 78, 3u, &e) == DSM_OK);
    CHECK(e.ack && w.slot[0].state == WK_EMPTY);
    wk_free(&w);
    return true;
}

/* La invalidación no es una respuesta: no completa ni consume la espera
 * anotada, aunque llegue en medio de ella. */
static bool test_invalidate_does_not_touch_the_pending_wait(void) {
    struct wk_state w;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    CHECK(cache_page(&w, 0, 1, 2));
    expect_grant(&w, 1, 2, DSM_WRITE);
    CHECK(simple(&w, DSM_WIRE_INVALIDATE, 0, 50, 2u, &e) == DSM_OK);
    CHECK(!w.wait.done && w.wait.kind == WK_GRANT);
    CHECK(grant(&w, 1, 2, 0, 0, &e) == DSM_OK);
    wk_free(&w);
    return true;
}

static bool test_invalidate_on_active_permission_is_rejected(void) {
    struct wk_state w, before;
    struct wk_effect e;

    CHECK(wk_init(&w, NID, PAGES));
    expect_grant(&w, 0, 1, DSM_READ);
    CHECK(grant(&w, 0, 1, 0, 0, &e) == DSM_OK);
    CHECK(snapshot(&w, &before));
    CHECK(simple(&w, DSM_WIRE_INVALIDATE, 0, 9, 2u, &e) == DSM_EPROTO);
    CHECK(!e.ack);
    CHECK(same_state(&w, &before));
    wk_free(&before);

    expect_grant(&w, 1, 2, DSM_WRITE);
    CHECK(grant(&w, 1, 2, 0, 0, &e) == DSM_OK);
    CHECK(simple(&w, DSM_WIRE_INVALIDATE, 1, 9, 2u, &e) == DSM_EPROTO);
    wk_free(&w);
    return true;
}

/* ---- Barreras ----------------------------------------------------------- */

static bool test_barrier_release_matches_id_and_generation(void) {
    struct wk_state w;
    struct wk_effect e;
    struct wk_wait a;
    struct wire_header h = hdr(DSM_WIRE_BARRIER_RELEASE, DSM_NO_PAGE, 0u, 4u);
    uint8_t p[8];

    CHECK(wk_init(&w, NID, PAGES));
    memset(&a, 0, sizeof a);
    a.kind = WK_BARRIER;
    a.page = DSM_NO_PAGE;
    a.barrier = 4;
    a.gen = 2;
    wk_expect(&w, &a);
    h.length = 8u;

    wire_put_u64(p, 1);
    CHECK(wk_apply(&w, &h, p, &e) == DSM_EPROTO);     /* otra generación */
    h.aux = 5u;
    wire_put_u64(p, 2);
    CHECK(wk_apply(&w, &h, p, &e) == DSM_EPROTO);     /* otra barrera */
    CHECK(!w.wait.done);

    h.aux = 4u;
    CHECK(wk_apply(&w, &h, p, &e) == DSM_OK);
    CHECK(e.completed && e.event == LOG_BARRIER_RELEASE && w.wait.done);
    CHECK(wk_apply(&w, &h, p, &e) == DSM_EPROTO);     /* dos veces */
    wk_free(&w);
    return true;
}

/* ---- Validaciones generales -------------------------------------------- */

static bool test_frames_for_another_node_or_page_are_rejected(void) {
    struct wk_state w;
    struct wk_effect e;
    struct wire_header h = hdr(DSM_WIRE_INVALIDATE, 0, 1, 0u);

    CHECK(wk_init(&w, NID, PAGES));
    CHECK(cache_page(&w, 0, 1, 0));

    h.node_id = NID + 1u;                  /* dirigida a otro nodo */
    CHECK(wk_apply(&w, &h, NULL, &e) == DSM_EPROTO);
    CHECK(w.slot[0].state == WK_CACHED);

    h.node_id = NID;
    h.page_id = PAGES;                     /* fuera de la región de la sesión */
    CHECK(wk_apply(&w, &h, NULL, &e) == DSM_EPROTO);
    wk_free(&w);
    return true;
}

static bool test_error_frames_carry_their_code(void) {
    struct wk_state w;
    struct wk_effect e;
    struct wire_header h = hdr(DSM_WIRE_ERROR, DSM_NO_PAGE, 0u, 0u);

    CHECK(wk_init(&w, NID, PAGES));
    for (uint32_t code = DSM_EINVAL; code <= DSM_EIO; code++) {
        h.aux = code;
        CHECK(wk_apply(&w, &h, NULL, &e) == (enum dsm_status)code);
    }
    h.aux = DSM_OK;                        /* un "error" que no es error */
    CHECK(wk_apply(&w, &h, NULL, &e) == DSM_EPROTO);
    h.aux = 999u;                          /* código desconocido (D0.11) */
    CHECK(wk_apply(&w, &h, NULL, &e) == DSM_EPROTO);

    /* Un ERROR puede llegar con cualquier node_id: el coordinador se lo
     * manda también a quien todavía no tiene uno válido. */
    h.node_id = 3u;
    h.aux = DSM_EABORT;
    CHECK(wk_apply(&w, &h, NULL, &e) == DSM_EABORT);
    wk_free(&w);
    return true;
}

static bool test_types_the_coordinator_never_sends_are_rejected(void) {
    static const uint8_t types[] = {
        DSM_WIRE_HELLO, DSM_WIRE_SESSION_START, DSM_WIRE_ACQ_REQ,
        DSM_WIRE_INVALIDATE_ACK, DSM_WIRE_REL_WRITE, DSM_WIRE_REL_READ,
        DSM_WIRE_BARRIER_ARRIVE, DSM_WIRE_BYE,
    };
    struct wk_state w;
    struct wk_effect e;
    uint8_t p[DSM_MAX_PAYLOAD];

    memset(p, 0, sizeof p);
    CHECK(wk_init(&w, NID, PAGES));
    for (size_t i = 0; i < sizeof types; i++) {
        struct wire_header h = hdr(types[i], 0, 1, 0u);
        CHECK(wk_apply(&w, &h, p, &e) == DSM_EPROTO);
    }
    wk_free(&w);
    return true;
}

int main(void) {
    static const struct test_case tests[] = {
        { "init_rejects_bad_args",                      test_init_rejects_bad_args },
        { "starts_empty_and_zeroed",                    test_starts_empty_and_zeroed },
        { "grant_with_data_fills_frame",                test_grant_with_data_fills_frame },
        { "grant_without_data_reuses_cached_copy",      test_grant_without_data_reuses_cached_copy },
        { "grant_without_data_needs_matching_copy",     test_grant_without_data_needs_matching_copy },
        { "grant_must_answer_the_pending_request",      test_grant_must_answer_the_pending_request },
        { "publish_bumps_version_by_one_and_keeps_copy", test_publish_bumps_version_by_one_and_keeps_copy },
        { "release_ack_needs_the_matching_permission",  test_release_ack_needs_the_matching_permission },
        { "invalidate_drops_copy_and_asks_for_ack",     test_invalidate_drops_copy_and_asks_for_ack },
        { "invalidate_does_not_touch_the_pending_wait", test_invalidate_does_not_touch_the_pending_wait },
        { "invalidate_on_active_permission_is_rejected", test_invalidate_on_active_permission_is_rejected },
        { "barrier_release_matches_id_and_generation",  test_barrier_release_matches_id_and_generation },
        { "frames_for_another_node_or_page_are_rejected", test_frames_for_another_node_or_page_are_rejected },
        { "error_frames_carry_their_code",              test_error_frames_carry_their_code },
        { "types_the_coordinator_never_sends_are_rejected", test_types_the_coordinator_never_sends_are_rejected },
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
