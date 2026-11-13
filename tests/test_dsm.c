/* test_dsm.c — pruebas de libdsm (#5) contra un coordinador falso.
 *
 * El coordinador real es #4 y llega por otra rama; la prueba de punta a punta
 * con los dos es #6. Acá el coordinador es un hilo que sigue un guion: recibe
 * exactamente las tramas que la prueba espera, en el orden que espera, y
 * responde lo que la prueba decide. Eso tiene una ventaja que el coordinador
 * real no daría: el momento de cada trama es determinista. "La invalidación
 * llega mientras la aplicación espera la concesión" no depende de una
 * carrera: el guion recibe el ACQ_REQ, y recién después manda el INVALIDATE y
 * se queda esperando el ACK sin conceder nada. Si el ACK llega, lo mandó el
 * receptor, porque el hilo de aplicación sigue bloqueado en DSM_acquire.
 *
 * Cada guion verifica además que no llegue ninguna trama de más: termina
 * esperando BYE y fin de flujo, o fin de flujo a secas. Así "se rechaza
 * localmente, antes de enviar nada" se prueba por la ausencia de tramas.
 */
#include "dsm.h"
#include "test_util.h"

#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "frame.h"
#include "io.h"
#include "net.h"
#include "wire.h"

#define FAKE_WAIT_MS   5000u   /* plazo de cada paso del coordinador falso */
#define APP_TIMEOUT_MS 3000u   /* timeout de operación de la biblioteca */
#define RUN_ID         99u

/* ---- Coordinador falso -------------------------------------------------- */

struct fake {
    int        lfd;
    int        fd;
    uint16_t   port;
    bool     (*script)(struct fake *f);
    bool       ok;
    pthread_t  th;
    uint32_t   nid;      /* el node_id con que se presentó el trabajador */
};

static void *fake_main(void *arg) {
    struct fake *f = arg;

    f->ok = false;
    if (net_accept(f->lfd, &f->fd, io_deadline_in(FAKE_WAIT_MS)) != NET_OK) {
        fprintf(stderr, "  coordinador falso: nadie se conectó\n");
        return NULL;
    }
    f->ok = f->script(f);
    return NULL;
}

static bool fake_start(struct fake *f, bool (*script)(struct fake *)) {
    memset(f, 0, sizeof *f);
    f->fd = -1;
    f->script = script;
    if (net_listen("127.0.0.1", 0, 4, &f->lfd, &f->port) != NET_OK)
        return false;
    return pthread_create(&f->th, NULL, fake_main, f) == 0;
}

/* Espera al guion y cierra lo suyo. Devuelve si el guion se cumplió. */
static bool fake_join(struct fake *f) {
    (void)pthread_join(f->th, NULL);
    net_close(f->fd);
    net_close(f->lfd);
    return f->ok;
}

static bool fk_recv(struct fake *f, struct wire_header *h, uint8_t *payload) {
    struct frame_io r = frame_recv(f->fd, h, payload, DSM_MAX_PAYLOAD,
                                   io_deadline_in(FAKE_WAIT_MS));
    if (!frame_ok(r))
        fprintf(stderr, "  coordinador falso: recepción falló (%s / %s)\n",
                io_strstatus(r.io), wire_strstatus(r.wire));
    return frame_ok(r);
}

static bool fk_expect(struct fake *f, uint8_t type, struct wire_header *h,
                      uint8_t *payload) {
    CHECK(fk_recv(f, h, payload));
    if (h->type != type)
        fprintf(stderr, "  coordinador falso: esperaba %s y llegó %s\n",
                wire_type_name(type), wire_type_name(h->type));
    CHECK(h->type == type);
    CHECK(h->node_id == f->nid);
    return true;
}

static bool fk_send(struct fake *f, uint8_t type, uint32_t page, uint64_t req,
                    uint32_t aux, uint8_t flags, const void *payload, uint32_t len) {
    struct wire_header h;

    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type = type;
    h.flags = flags;
    h.length = len;
    h.node_id = f->nid;
    h.page_id = page;
    h.request_id = req;
    h.aux = aux;
    CHECK(frame_ok(frame_send(f->fd, &h, payload, io_deadline_in(FAKE_WAIT_MS))));
    return true;
}

/* El trabajador cerró su conexión (shutdown o close) y no mandó nada más. */
static bool fk_eof(struct fake *f) {
    struct wire_header h;
    uint8_t payload[DSM_MAX_PAYLOAD];
    struct frame_io r = frame_recv(f->fd, &h, payload, sizeof payload,
                                   io_deadline_in(FAKE_WAIT_MS));
    if (frame_ok(r))
        fprintf(stderr, "  coordinador falso: trama de más: %s\n",
                wire_type_name(h.type));
    CHECK(r.io == IO_CLOSED);
    return true;
}

static bool fk_handshake(struct fake *f, uint32_t nodes, uint32_t pages) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD], s[24];

    CHECK(fk_recv(f, &h, p));
    CHECK(h.type == DSM_WIRE_HELLO);
    CHECK(wire_get_u32(p + 0) == nodes);
    CHECK(wire_get_u32(p + 4) == 0u);    /* sin opinión sobre páginas (D0.7) */
    f->nid = h.node_id;

    wire_put_u32(s + 0, nodes);
    wire_put_u32(s + 4, pages);
    wire_put_u32(s + 8, DSM_PAGE_SIZE);
    wire_put_u32(s + 12, 0u);
    wire_put_u64(s + 16, RUN_ID);
    return fk_send(f, DSM_WIRE_SESSION_START, DSM_NO_PAGE, 0u, 0u, 0u, s, sizeof s);
}

/* Concesión: con datos si `data` no es NULL. */
static bool fk_grant(struct fake *f, uint32_t page, uint64_t req, uint64_t version,
                     const uint8_t *data) {
    uint8_t p[8u + DSM_PAGE_SIZE];

    wire_put_u64(p, version);
    if (data == NULL)
        return fk_send(f, DSM_WIRE_ACQ_GRANT, page, req, 0u, 0u, p, 8u);
    memcpy(p + 8, data, DSM_PAGE_SIZE);
    return fk_send(f, DSM_WIRE_ACQ_GRANT, page, req, 0u, DSM_WIRE_F_HAS_DATA,
                   p, sizeof p);
}

/* Recibe un ACQ_REQ y comprueba página, modo y versión ofrecida. */
static bool fk_acq(struct fake *f, uint32_t page, enum dsm_mode mode,
                   uint64_t cached, uint64_t *req) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_expect(f, DSM_WIRE_ACQ_REQ, &h, p));
    CHECK(h.page_id == page);
    CHECK(h.aux == (uint32_t)mode);
    CHECK(wire_get_u64(p) == cached);
    *req = h.request_id;
    return true;
}

/* Recibe un REL_READ del acceso `req` y lo confirma. */
static bool fk_rel_read(struct fake *f, uint32_t page, uint64_t req) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_expect(f, DSM_WIRE_REL_READ, &h, p));
    CHECK(h.page_id == page);
    CHECK(h.request_id == req);        /* el mismo del acquire (D2.1) */
    return fk_send(f, DSM_WIRE_REL_READ_ACK, page, req, 0u, 0u, NULL, 0u);
}

/* Recibe un REL_WRITE del acceso `req`, copia la página a `out` (si no es
 * NULL) y confirma la publicación como `version`. */
static bool fk_rel_write(struct fake *f, uint32_t page, uint64_t req,
                         uint64_t version, uint8_t *out) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD], v[8];

    CHECK(fk_expect(f, DSM_WIRE_REL_WRITE, &h, p));
    CHECK(h.page_id == page);
    CHECK(h.request_id == req);
    CHECK(h.length == DSM_PAGE_SIZE);
    if (out != NULL)
        memcpy(out, p, DSM_PAGE_SIZE);
    wire_put_u64(v, version);
    return fk_send(f, DSM_WIRE_REL_WRITE_ACK, page, req, 0u, 0u, v, sizeof v);
}

/* Manda una invalidación y exige su ACK con la clave completa de la ronda:
 * página, request_id y solicitante (D2.1). */
static bool fk_invalidate(struct fake *f, uint32_t page, uint64_t req,
                          uint32_t requester) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_send(f, DSM_WIRE_INVALIDATE, page, req, requester, 0u, NULL, 0u));
    CHECK(fk_expect(f, DSM_WIRE_INVALIDATE_ACK, &h, p));
    CHECK(h.page_id == page);
    CHECK(h.request_id == req);
    CHECK(h.aux == requester);
    return true;
}

static bool fk_barrier(struct fake *f, uint32_t id, uint64_t gen, uint64_t *req) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_expect(f, DSM_WIRE_BARRIER_ARRIVE, &h, p));
    CHECK(h.aux == id);
    CHECK(wire_get_u64(p) == gen);
    if (req != NULL)
        *req = h.request_id;
    return true;
}

static bool fk_barrier_release(struct fake *f, uint32_t id, uint64_t gen) {
    uint8_t p[8];

    wire_put_u64(p, gen);
    return fk_send(f, DSM_WIRE_BARRIER_RELEASE, DSM_NO_PAGE, 0u, id, 0u, p, sizeof p);
}

/* Despedida: BYE, y el trabajador cierra su lado de escritura. El falso
 * cierra después, como el coordinador real al procesar un BYE (D0.13). */
static bool fk_bye(struct fake *f) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_expect(f, DSM_WIRE_BYE, &h, p));
    CHECK(fk_eof(f));
    net_close(f->fd);
    f->fd = -1;
    return true;
}

/* ---- Lado de la aplicación --------------------------------------------- */

static dsm_ctx *app_init(struct fake *f, uint32_t nodes, uint32_t nid,
                         uint32_t timeout_ms, const char *log_path,
                         enum dsm_status *err) {
    struct dsm_options o;

    memset(&o, 0, sizeof o);
    o.timeout_ms = timeout_ms;
    o.log_path = log_path;
    return DSM_init("127.0.0.1", f->port, nodes, nid, &o, err);
}

static void fill(uint8_t *page, uint8_t seed) {
    for (size_t i = 0; i < DSM_PAGE_SIZE; i++)
        page[i] = (uint8_t)(seed + i * 13u);
}

static bool is_filled(const uint8_t *page, uint8_t seed) {
    for (size_t i = 0; i < DSM_PAGE_SIZE; i++) {
        if (page[i] != (uint8_t)(seed + i * 13u))
            return false;
    }
    return true;
}

static uint64_t ms_since(uint64_t t0) {
    return (now_monotonic_ns() - t0) / 1000000u;
}

/* ---- DSM_init ----------------------------------------------------------- */

static bool test_init_rejects_bad_args(void) {
    enum dsm_status err = DSM_OK;

    CHECK(DSM_init(NULL, 5000, 1, 0, NULL, &err) == NULL && err == DSM_EINVAL);
    CHECK(DSM_init("127.0.0.1", 0, 1, 0, NULL, &err) == NULL && err == DSM_EINVAL);
    CHECK(DSM_init("127.0.0.1", 5000, 0, 0, NULL, &err) == NULL && err == DSM_EINVAL);
    CHECK(DSM_init("127.0.0.1", 5000, 5, 0, NULL, &err) == NULL && err == DSM_EINVAL);
    CHECK(DSM_init("127.0.0.1", 5000, 2, 2, NULL, &err) == NULL && err == DSM_EINVAL);
    CHECK(DSM_init("127.0.0.1", 5000, 2, 7, NULL, NULL) == NULL);
    CHECK(DSM_page_count(NULL) == 0u);
    CHECK(DSM_last_error(NULL) == DSM_EINVAL);
    CHECK(DSM_acquire(NULL, 0, DSM_READ) == NULL);
    CHECK(DSM_release(NULL, 0) == -1);
    CHECK(DSM_barrier(NULL, 0) == -1);
    CHECK(DSM_finalize(NULL) == -1);
    return true;
}

static bool script_handshake(struct fake *f) {
    CHECK(fk_handshake(f, 2, 7));
    return fk_bye(f);
}

/* Presentación con nodes y nid propios, páginas tomadas de SESSION_START, y
 * despedida limpia. */
static bool test_init_handshake_and_bye(void) {
    struct fake f;
    enum dsm_status err = DSM_EIO;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_handshake));
    c = app_init(&f, 2, 1, APP_TIMEOUT_MS, NULL, &err);
    CHECK(c != NULL && err == DSM_OK);
    CHECK(DSM_page_count(c) == 7u);
    CHECK(DSM_last_error(c) == DSM_OK);
    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    CHECK(f.nid == 1u);
    return true;
}

static bool script_bye_rejected(struct fake *f) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];
    const char why[] = "barrera sin completar";

    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_expect(f, DSM_WIRE_BYE, &h, p));
    /* D0.13: un BYE que deja una barrera sin completar aborta la sesión. */
    CHECK(fk_send(f, DSM_WIRE_ERROR, DSM_NO_PAGE, 0u, DSM_EABORT, 0u, why,
                  (uint32_t)strlen(why)));
    net_close(f->fd);
    f->fd = -1;
    return true;
}

/* Un ERROR que llega durante la despedida no se pierde: DSM_finalize no
 * informa un cierre limpio que no lo fue. */
static bool test_error_during_bye_is_not_a_clean_close(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_bye_rejected));
    c = app_init(&f, 2, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

static bool script_reject(struct fake *f) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];
    const char why[] = "id repetido";

    CHECK(fk_recv(f, &h, p));
    f->nid = h.node_id;
    CHECK(fk_send(f, DSM_WIRE_ERROR, DSM_NO_PAGE, 0u, DSM_EINVAL, 0u, why,
                  (uint32_t)strlen(why)));
    return fk_eof(f);
}

/* El coordinador rechaza el registro: el código viaja tal cual. */
static bool test_init_rejected_by_coordinator(void) {
    struct fake f;
    enum dsm_status err = DSM_OK;

    CHECK(fake_start(&f, script_reject));
    CHECK(app_init(&f, 2, 0, APP_TIMEOUT_MS, NULL, &err) == NULL);
    CHECK(err == DSM_EINVAL);
    CHECK(fake_join(&f));
    return true;
}

static bool script_wrong_nodes(struct fake *f) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD], s[24];

    CHECK(fk_recv(f, &h, p));
    f->nid = h.node_id;
    /* La sesión es de 2 nodos y el trabajador declaró 3. */
    wire_put_u32(s + 0, 2u);
    wire_put_u32(s + 4, 4u);
    wire_put_u32(s + 8, DSM_PAGE_SIZE);
    wire_put_u32(s + 12, 0u);
    wire_put_u64(s + 16, RUN_ID);
    CHECK(fk_send(f, DSM_WIRE_SESSION_START, DSM_NO_PAGE, 0u, 0u, 0u, s, sizeof s));
    return fk_eof(f);
}

/* Un SESSION_START que no coincide con lo que el trabajador declaró es un
 * error de protocolo, no una sesión que arranca a medias. */
static bool test_init_session_mismatch(void) {
    struct fake f;
    enum dsm_status err = DSM_OK;

    CHECK(fake_start(&f, script_wrong_nodes));
    CHECK(app_init(&f, 3, 0, APP_TIMEOUT_MS, NULL, &err) == NULL);
    CHECK(err == DSM_EPROTO);
    CHECK(fake_join(&f));
    return true;
}

static bool script_silent(struct fake *f) {
    struct wire_header h;
    uint8_t p[DSM_MAX_PAYLOAD];

    CHECK(fk_recv(f, &h, p));
    CHECK(h.type == DSM_WIRE_HELLO);
    f->nid = h.node_id;
    return fk_eof(f);     /* nunca arranca la sesión; el trabajador se va */
}

/* Si la sesión no se completa, DSM_init no espera para siempre. */
static bool test_init_times_out(void) {
    struct fake f;
    enum dsm_status err = DSM_OK;
    uint64_t t0 = now_monotonic_ns();

    CHECK(fake_start(&f, script_silent));
    CHECK(app_init(&f, 2, 0, 300, NULL, &err) == NULL);
    CHECK(err == DSM_ETIMEOUT);
    CHECK(ms_since(t0) >= 250u && ms_since(t0) < 2000u);
    CHECK(fake_join(&f));
    return true;
}

static bool test_init_without_coordinator(void) {
    int lfd;
    uint16_t port;
    enum dsm_status err = DSM_OK;

    /* Un puerto que estuvo libre hace un instante y ya no escucha nadie. */
    CHECK(net_listen("127.0.0.1", 0, 1, &lfd, &port) == NET_OK);
    net_close(lfd);
    CHECK(DSM_init("127.0.0.1", port, 1, 0, NULL, &err) == NULL);
    CHECK(err == DSM_ECONN);
    return true;
}

static bool test_init_unwritable_log(void) {
    struct dsm_options o = { 0u, "/no/existe/este/directorio/w.csv" };
    enum dsm_status err = DSM_OK;

    /* Falla antes de conectar: no hace falta coordinador. */
    CHECK(DSM_init("127.0.0.1", 5000, 1, 0, &o, &err) == NULL);
    CHECK(err == DSM_EIO);
    return true;
}

/* ---- Rechazos locales --------------------------------------------------- */

static bool script_one_read(struct fake *f) {
    uint64_t req;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 1, 4));
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 0u, zero));
    CHECK(fk_rel_read(f, 0, req));
    return fk_bye(f);    /* y nada más: todo lo demás se rechazó localmente */
}

static bool test_local_rejections_send_nothing(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_one_read));
    c = app_init(&f, 1, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);

    CHECK(DSM_acquire(c, 4, DSM_READ) == NULL);              /* fuera de rango */
    CHECK(DSM_last_error(c) == DSM_EINVAL);
    CHECK(DSM_acquire(c, 0, (enum dsm_mode)3) == NULL);      /* modo inválido */
    CHECK(DSM_last_error(c) == DSM_EINVAL);
    CHECK(DSM_release(c, 0) == -1);                          /* sin acquire */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_release(c, 9) == -1);
    CHECK(DSM_last_error(c) == DSM_EINVAL);

    CHECK(DSM_acquire(c, 0, DSM_READ) != NULL);
    CHECK(DSM_last_error(c) == DSM_OK);
    CHECK(DSM_acquire(c, 1, DSM_READ) == NULL);              /* anidada */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_acquire(c, 0, DSM_WRITE) == NULL);             /* la misma, otra vez */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_barrier(c, 0) == -1);                          /* con página */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_release(c, 1) == -1);                          /* otra página */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_finalize(c) == -1);                            /* con página: */
    CHECK(DSM_last_error(c) == DSM_ESTATE);                  /* ctx sigue vivo */

    CHECK(DSM_release(c, 0) == 0);
    CHECK(DSM_release(c, 0) == -1);                          /* ya liberada */
    CHECK(DSM_last_error(c) == DSM_ESTATE);
    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    return true;
}

/* ---- Caché y permisos --------------------------------------------------- */

static bool script_read_then_cached(struct fake *f) {
    uint64_t r1, r2;
    uint8_t data[DSM_PAGE_SIZE];

    fill(data, 5);
    CHECK(fk_handshake(f, 1, 2));
    CHECK(fk_acq(f, 1, DSM_READ, DSM_NO_VERSION, &r1));
    CHECK(fk_grant(f, 1, r1, 3u, data));
    CHECK(fk_rel_read(f, 1, r1));

    /* Segunda lectura: el trabajador ofrece su versión, y la consulta llega
     * igual — tener la copia no da permiso (enunciado y dsm.h). */
    CHECK(fk_acq(f, 1, DSM_READ, 3u, &r2));
    CHECK(r2 > r1);                       /* estrictamente creciente (D2.1) */
    CHECK(fk_grant(f, 1, r2, 3u, NULL));  /* sin reenviar la página */
    CHECK(fk_rel_read(f, 1, r2));
    return fk_bye(f);
}

static bool test_cached_copy_still_asks_permission(void) {
    struct fake f;
    dsm_ctx *c;
    uint8_t *p;

    CHECK(fake_start(&f, script_read_then_cached));
    c = app_init(&f, 1, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);

    p = DSM_acquire(c, 1, DSM_READ);
    CHECK(p != NULL && is_filled(p, 5));
    CHECK(DSM_release(c, 1) == 0);

    p = DSM_acquire(c, 1, DSM_READ);
    CHECK(p != NULL && is_filled(p, 5));    /* los bytes de la caché */
    CHECK(DSM_release(c, 1) == 0);

    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    return true;
}

static uint8_t g_published[DSM_PAGE_SIZE];

static bool script_write_publishes(struct fake *f) {
    uint64_t r1, r2;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 1, 1));
    CHECK(fk_acq(f, 0, DSM_WRITE, DSM_NO_VERSION, &r1));
    CHECK(fk_grant(f, 0, r1, 0u, zero));
    CHECK(fk_rel_write(f, 0, r1, 1u, g_published));

    /* El escritor conserva su copia, ya con la versión publicada. */
    CHECK(fk_acq(f, 0, DSM_READ, 1u, &r2));
    CHECK(fk_grant(f, 0, r2, 1u, NULL));
    CHECK(fk_rel_read(f, 0, r2));
    return fk_bye(f);
}

/* El release de WRITE manda la página completa con el request_id del acceso y
 * no retorna hasta la confirmación. */
static bool test_write_release_publishes_whole_page(void) {
    struct fake f;
    dsm_ctx *c;
    uint8_t *p;

    memset(g_published, 0, sizeof g_published);
    CHECK(fake_start(&f, script_write_publishes));
    c = app_init(&f, 1, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);

    p = DSM_acquire(c, 0, DSM_WRITE);
    CHECK(p != NULL);
    fill(p, 42);
    CHECK(DSM_release(c, 0) == 0);

    p = DSM_acquire(c, 0, DSM_READ);
    CHECK(p != NULL && is_filled(p, 42));
    CHECK(DSM_release(c, 0) == 0);

    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    CHECK(is_filled(g_published, 42));
    return true;
}

static bool script_grant_without_copy(struct fake *f) {
    uint64_t req;

    CHECK(fk_handshake(f, 1, 1));
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 0u, NULL));   /* "usa tu copia", y no la hay */
    return fk_eof(f);
}

/* Una concesión sin datos sobre una copia inexistente serviría datos que la
 * biblioteca no tiene: error de protocolo, terminal. */
static bool test_grant_without_copy_is_protocol_error(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_grant_without_copy));
    c = app_init(&f, 1, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_READ) == NULL);
    CHECK(DSM_last_error(c) == DSM_EPROTO);
    CHECK(DSM_barrier(c, 0) == -1);         /* terminal: todo falla igual */
    CHECK(DSM_last_error(c) == DSM_EPROTO);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

static bool script_publish_skips_version(struct fake *f) {
    uint64_t req;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 1, 1));
    CHECK(fk_acq(f, 0, DSM_WRITE, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 5u, zero));
    CHECK(fk_rel_write(f, 0, req, 9u, NULL));   /* debía ser 6 */
    return fk_eof(f);
}

/* Una publicación sube la versión exactamente en uno. Una confirmación con
 * otra versión no es de esta escritura: error de protocolo, y el release
 * informa que el estado de la publicación quedó indeterminado. */
static bool test_publish_with_other_version_is_protocol_error(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_publish_skips_version));
    c = app_init(&f, 1, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_WRITE) != NULL);
    CHECK(DSM_release(c, 0) == -1);
    CHECK(DSM_last_error(c) == DSM_EPROTO);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

static bool script_inval_on_active(struct fake *f) {
    uint64_t req;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 0u, zero));
    /* Invalidar un permiso vivo viola el protocolo: el ACK no debe llegar. */
    CHECK(fk_send(f, DSM_WIRE_INVALIDATE, 0, 50u, 1u, 0u, NULL, 0u));
    return fk_eof(f);
}

static bool test_invalidate_on_active_permission_aborts(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_inval_on_active));
    c = app_init(&f, 2, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_READ) != NULL);
    CHECK(fake_join(&f));                   /* el trabajador ya cortó */
    CHECK(DSM_release(c, 0) == -1);
    CHECK(DSM_last_error(c) == DSM_EPROTO);
    CHECK(DSM_finalize(c) == -1);           /* sin página: libera igual */
    return true;
}

/* ---- Invalidaciones con la aplicación bloqueada ------------------------- */

/* Deja la página 0 en caché con versión 2. */
static bool fk_cache_page0(struct fake *f) {
    uint64_t req;
    uint8_t data[DSM_PAGE_SIZE];

    fill(data, 9);
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 2u, data));
    return fk_rel_read(f, 0, req);
}

static bool script_inval_during_grant(struct fake *f) {
    uint64_t req, req0;
    uint8_t data[DSM_PAGE_SIZE];

    fill(data, 1);
    CHECK(fk_handshake(f, 4, 2));
    CHECK(fk_cache_page0(f));

    /* La aplicación está bloqueada en DSM_acquire(1, WRITE) y la concesión no
     * sale hasta que llegue el ACK: si llega, lo mandó el receptor. */
    CHECK(fk_acq(f, 1, DSM_WRITE, DSM_NO_VERSION, &req));
    CHECK(fk_invalidate(f, 0, 77u, 2u));
    CHECK(fk_grant(f, 1, req, 0u, data));
    CHECK(fk_rel_write(f, 1, req, 1u, NULL));

    /* La copia invalidada ya no se ofrece. */
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req0));
    CHECK(fk_grant(f, 0, req0, 3u, data));
    CHECK(fk_rel_read(f, 0, req0));
    return fk_bye(f);
}

static bool test_invalidation_acked_while_waiting_for_grant(void) {
    struct fake f;
    dsm_ctx *c;
    uint8_t *p;

    CHECK(fake_start(&f, script_inval_during_grant));
    c = app_init(&f, 4, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);

    p = DSM_acquire(c, 0, DSM_READ);
    CHECK(p != NULL && is_filled(p, 9));
    CHECK(DSM_release(c, 0) == 0);

    p = DSM_acquire(c, 1, DSM_WRITE);
    CHECK(p != NULL && is_filled(p, 1));
    CHECK(DSM_release(c, 1) == 0);

    p = DSM_acquire(c, 0, DSM_READ);
    CHECK(p != NULL && is_filled(p, 1));    /* la página nueva, no la retenida */
    CHECK(DSM_release(c, 0) == 0);

    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    return true;
}

static bool script_inval_during_barrier(struct fake *f) {
    uint64_t req;

    CHECK(fk_handshake(f, 4, 1));
    CHECK(fk_cache_page0(f));

    CHECK(fk_barrier(f, 5, 0, NULL));
    CHECK(fk_invalidate(f, 0, 12u, 3u));    /* la aplicación está en la barrera */
    CHECK(fk_barrier_release(f, 5, 0));

    /* Generaciones por barrier_id (D0.8): la 5 sigue en 1, la 6 empieza en 0. */
    CHECK(fk_barrier(f, 5, 1, NULL));
    CHECK(fk_barrier_release(f, 5, 1));
    CHECK(fk_barrier(f, 6, 0, NULL));
    CHECK(fk_barrier_release(f, 6, 0));

    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 3u, (const uint8_t[DSM_PAGE_SIZE]){ 0 }));
    CHECK(fk_rel_read(f, 0, req));
    return fk_bye(f);
}

static bool test_invalidation_acked_while_in_barrier(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_inval_during_barrier));
    c = app_init(&f, 4, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_READ) != NULL);
    CHECK(DSM_release(c, 0) == 0);
    CHECK(DSM_barrier(c, 5) == 0);
    CHECK(DSM_barrier(c, 5) == 0);
    CHECK(DSM_barrier(c, 6) == 0);
    CHECK(DSM_acquire(c, 0, DSM_READ) != NULL);
    CHECK(DSM_release(c, 0) == 0);
    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));
    return true;
}

static bool script_wrong_generation(struct fake *f) {
    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_barrier(f, 0, 0, NULL));
    CHECK(fk_barrier_release(f, 0, 1));     /* una generación que no es la abierta */
    return fk_eof(f);
}

static bool test_barrier_release_of_other_generation(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_wrong_generation));
    c = app_init(&f, 2, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_barrier(c, 0) == -1);
    CHECK(DSM_last_error(c) == DSM_EPROTO);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

/* ---- Caídas y plazos ---------------------------------------------------- */

static bool script_drop_in_barrier(struct fake *f) {
    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_barrier(f, 0, 0, NULL));
    net_close(f->fd);          /* el coordinador muere con la barrera abierta */
    f->fd = -1;
    return true;
}

/* La caída de la sesión despierta con error a quien espera en una barrera,
 * mucho antes del timeout. */
static bool test_session_drop_wakes_barrier(void) {
    struct fake f;
    dsm_ctx *c;
    uint64_t t0;

    CHECK(fake_start(&f, script_drop_in_barrier));
    c = app_init(&f, 2, 0, 8000u, NULL, NULL);
    CHECK(c != NULL);
    t0 = now_monotonic_ns();
    CHECK(DSM_barrier(c, 0) == -1);
    CHECK(DSM_last_error(c) == DSM_ECONN);
    CHECK(ms_since(t0) < 2000u);
    CHECK(DSM_acquire(c, 0, DSM_READ) == NULL);     /* terminal */
    CHECK(DSM_last_error(c) == DSM_ECONN);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

static bool script_abort_in_acquire(struct fake *f) {
    uint64_t req;
    const char why[] = "nodo 1 perdido";

    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_acq(f, 0, DSM_WRITE, DSM_NO_VERSION, &req));
    CHECK(fk_send(f, DSM_WIRE_ERROR, DSM_NO_PAGE, 0u, DSM_EABORT, 0u, why,
                  (uint32_t)strlen(why)));
    return fk_eof(f);
}

static bool test_abort_wakes_acquire(void) {
    struct fake f;
    dsm_ctx *c;

    CHECK(fake_start(&f, script_abort_in_acquire));
    c = app_init(&f, 2, 0, APP_TIMEOUT_MS, NULL, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_WRITE) == NULL);
    CHECK(DSM_last_error(c) == DSM_EABORT);
    CHECK(DSM_finalize(c) == -1);
    CHECK(fake_join(&f));
    return true;
}

static bool script_never_grants(struct fake *f) {
    uint64_t req;

    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_acq(f, 0, DSM_READ, DSM_NO_VERSION, &req));
    return fk_eof(f);     /* el trabajador vence y corta: el coordinador lo ve */
}

static bool test_acquire_times_out_and_drops_session(void) {
    struct fake f;
    dsm_ctx *c;
    uint64_t t0;

    CHECK(fake_start(&f, script_never_grants));
    c = app_init(&f, 2, 0, 300u, NULL, NULL);
    CHECK(c != NULL);
    t0 = now_monotonic_ns();
    CHECK(DSM_acquire(c, 0, DSM_READ) == NULL);
    CHECK(DSM_last_error(c) == DSM_ETIMEOUT);
    CHECK(ms_since(t0) >= 250u && ms_since(t0) < 2000u);
    CHECK(fake_join(&f));
    CHECK(DSM_finalize(c) == -1);
    return true;
}

/* ---- Recursos ----------------------------------------------------------- */

/* Entradas de un directorio de /proc, o -1 si no existe (fuera de Linux). */
static int count_entries(const char *path) {
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    (void)closedir(d);
    return n;
}

/* Hilos del proceso, esperando hasta 2 s a que la cuenta llegue a `want`.
 *
 * pthread_join garantiza que el hilo terminó, pero bajo emulación (qemu-user,
 * que es como corre la suite en una Mac con Docker amd64) la entrada de
 * /proc/self/task puede tardar un instante en desaparecer después. La espera
 * absorbe eso; un hilo realmente abandonado no desaparece nunca y la prueba
 * falla igual. */
static int settle_tasks(int want) {
    int n = count_entries("/proc/self/task");

    for (int i = 0; i < 200 && n != want; i++) {
        struct timespec ts = { 0, 10 * 1000000L };
        (void)nanosleep(&ts, NULL);
        n = count_entries("/proc/self/task");
    }
    return n;
}

static bool script_some_traffic(struct fake *f) {
    uint64_t req;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 2, 3));
    CHECK(fk_acq(f, 2, DSM_WRITE, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 2, req, 0u, zero));
    CHECK(fk_rel_write(f, 2, req, 1u, NULL));
    CHECK(fk_barrier(f, 0, 0, NULL));
    CHECK(fk_barrier_release(f, 0, 0));
    return fk_bye(f);
}

/* DSM_finalize no deja hilos ni descriptores: se cuentan antes de DSM_init y
 * después de DSM_finalize, con el coordinador falso ya cerrado. */
static bool test_finalize_leaves_no_threads_or_fds(void) {
    struct fake f;
    dsm_ctx *c;
    int fds0 = count_entries("/proc/self/fd");
    int th0 = settle_tasks(1);    /* las pruebas anteriores ya unieron sus hilos */

    for (int round = 0; round < 3; round++) {
        CHECK(fake_start(&f, script_some_traffic));
        c = app_init(&f, 2, 1, APP_TIMEOUT_MS, NULL, NULL);
        CHECK(c != NULL);
        if (th0 >= 0)
            CHECK(settle_tasks(th0 + 2) == th0 + 2);   /* falso + receptor */
        CHECK(DSM_acquire(c, 2, DSM_WRITE) != NULL);
        CHECK(DSM_release(c, 2) == 0);
        CHECK(DSM_barrier(c, 0) == 0);
        CHECK(DSM_finalize(c) == 0);
        CHECK(fake_join(&f));
    }
    if (fds0 >= 0) {
        CHECK(count_entries("/proc/self/fd") == fds0);
        CHECK(settle_tasks(th0) == th0);
    }
    return true;
}

/* ---- Registro CSV ------------------------------------------------------- */

static bool script_logged(struct fake *f) {
    uint64_t req;
    uint8_t zero[DSM_PAGE_SIZE];

    memset(zero, 0, sizeof zero);
    CHECK(fk_handshake(f, 2, 1));
    CHECK(fk_acq(f, 0, DSM_WRITE, DSM_NO_VERSION, &req));
    CHECK(fk_grant(f, 0, req, 0u, zero));
    CHECK(fk_rel_write(f, 0, req, 1u, NULL));
    CHECK(fk_barrier(f, 3, 0, NULL));
    CHECK(fk_invalidate(f, 0, 40u, 1u));
    CHECK(fk_barrier_release(f, 3, 0));
    return fk_bye(f);
}

/* Separa una línea CSV sin comillas en sus campos (modifica la línea). */
static int split_csv(char *line, char *fields[], int max) {
    int n = 0;
    char *p = line;

    line[strcspn(line, "\n")] = '\0';
    while (n < max) {
        fields[n++] = p;
        p = strchr(p, ',');
        if (p == NULL)
            break;
        *p++ = '\0';
    }
    return n;
}

/* Columnas del registro (D3.1): el mismo formato que el coordinador. */
enum { COL_RUN, COL_COMP, COL_SEQ, COL_NODE, COL_REQ, COL_PAGE, COL_EVENT,
       COL_STATUS, COL_MODE, COL_VERSION, COL_WRITER, COL_READERS,
       COL_MASK, COL_PENDING, COL_INVALS = 19, COL_DURATION = 20, NCOLS = 21 };

static bool test_log_records_events_with_correlation(void) {
    static const char *const expected[] = {
        "SESSION_START", "ACQ_REQ", "ACQ_GRANT", "REL_WRITE", "WRITE_PUBLISHED",
        "BARRIER_ARRIVE", "INVALIDATE", "INVALIDATE_ACK", "BARRIER_RELEASE",
        "NODE_LEAVE", "SESSION_END",
    };
    const size_t n_expected = sizeof expected / sizeof expected[0];
    char path[] = "/tmp/test_dsm_log_XXXXXX";
    char line[512], *fl[32];
    char acq_req[32] = "";
    struct fake f;
    dsm_ctx *c;
    FILE *in;
    size_t i = 0;
    int fd = mkstemp(path);

    CHECK(fd >= 0);
    close(fd);

    CHECK(fake_start(&f, script_logged));
    c = app_init(&f, 2, 1, APP_TIMEOUT_MS, path, NULL);
    CHECK(c != NULL);
    CHECK(DSM_acquire(c, 0, DSM_WRITE) != NULL);
    CHECK(DSM_release(c, 0) == 0);
    CHECK(DSM_barrier(c, 3) == 0);
    CHECK(DSM_finalize(c) == 0);
    CHECK(fake_join(&f));

    in = fopen(path, "r");
    CHECK(in != NULL);
    CHECK(fgets(line, sizeof line, in) != NULL);
    CHECK(split_csv(line, fl, 32) == NCOLS);
    CHECK(strcmp(fl[COL_RUN], "run_id") == 0 && strcmp(fl[COL_EVENT], "event") == 0);
    CHECK(strcmp(fl[COL_DURATION], "dur_ns") == 0);

    while (fgets(line, sizeof line, in) != NULL) {
        CHECK(split_csv(line, fl, 32) == NCOLS);
        CHECK(i < n_expected);
        if (strcmp(fl[COL_EVENT], expected[i]) != 0)
            fprintf(stderr, "  fila %zu: %s, esperaba %s\n", i, fl[COL_EVENT], expected[i]);
        CHECK(strcmp(fl[COL_EVENT], expected[i]) == 0);
        CHECK(strtoull(fl[COL_SEQ], NULL, 10) == i + 1u);   /* secuencia local */
        CHECK(strcmp(fl[COL_RUN], "99") == 0);               /* el de SESSION_START */
        CHECK(strcmp(fl[COL_COMP], "trabajador") == 0);
        CHECK(strcmp(fl[COL_NODE], "1") == 0);
        CHECK(fl[COL_STATUS][0] == '\0');                   /* nada falló */

        /* El estado del directorio no lo conoce el trabajador (D4.4). */
        CHECK(fl[COL_WRITER][0] == '\0' && fl[COL_READERS][0] == '\0');
        CHECK(fl[COL_MASK][0] == '\0' && fl[COL_PENDING][0] == '\0');

        /* Un acceso completo comparte request_id: solicitud, concesión,
         * release y publicación (D2.1). */
        if (strcmp(fl[COL_EVENT], "ACQ_REQ") == 0) {
            (void)snprintf(acq_req, sizeof acq_req, "%s", fl[COL_REQ]);
            CHECK(fl[COL_VERSION][0] == '\0');     /* sin copia que ofrecer */
        }
        if (strcmp(fl[COL_EVENT], "ACQ_GRANT") == 0 ||
            strcmp(fl[COL_EVENT], "REL_WRITE") == 0 ||
            strcmp(fl[COL_EVENT], "WRITE_PUBLISHED") == 0) {
            CHECK(strcmp(fl[COL_REQ], acq_req) == 0);
            CHECK(strcmp(fl[COL_PAGE], "0") == 0);
            CHECK(strcmp(fl[COL_MODE], "WRITE") == 0);
        }
        if (strcmp(fl[COL_EVENT], "ACQ_GRANT") == 0 ||
            strcmp(fl[COL_EVENT], "WRITE_PUBLISHED") == 0 ||
            strcmp(fl[COL_EVENT], "BARRIER_RELEASE") == 0)
            CHECK(fl[COL_DURATION][0] != '\0');      /* duración medida */
        if (strcmp(fl[COL_EVENT], "ACQ_GRANT") == 0)
            CHECK(strcmp(fl[COL_VERSION], "0") == 0);
        if (strcmp(fl[COL_EVENT], "WRITE_PUBLISHED") == 0)
            CHECK(strcmp(fl[COL_VERSION], "1") == 0);
        if (strncmp(fl[COL_EVENT], "INVALIDATE", 10) == 0) {
            CHECK(strcmp(fl[COL_REQ], "40") == 0);     /* la ronda ajena */
            CHECK(strcmp(fl[COL_INVALS], "1") == 0);   /* contada al recibirla */
        }
        i++;
    }
    (void)fclose(in);
    (void)unlink(path);
    CHECK(i == n_expected);
    return true;
}

int main(void) {
    static const struct test_case tests[] = {
        { "init_rejects_bad_args",                      test_init_rejects_bad_args },
        { "init_handshake_and_bye",                     test_init_handshake_and_bye },
        { "error_during_bye_is_not_a_clean_close",      test_error_during_bye_is_not_a_clean_close },
        { "init_rejected_by_coordinator",               test_init_rejected_by_coordinator },
        { "init_session_mismatch",                      test_init_session_mismatch },
        { "init_times_out",                             test_init_times_out },
        { "init_without_coordinator",                   test_init_without_coordinator },
        { "init_unwritable_log",                        test_init_unwritable_log },
        { "local_rejections_send_nothing",              test_local_rejections_send_nothing },
        { "cached_copy_still_asks_permission",          test_cached_copy_still_asks_permission },
        { "write_release_publishes_whole_page",         test_write_release_publishes_whole_page },
        { "grant_without_copy_is_protocol_error",       test_grant_without_copy_is_protocol_error },
        { "publish_with_other_version_is_protocol_error", test_publish_with_other_version_is_protocol_error },
        { "invalidate_on_active_permission_aborts",     test_invalidate_on_active_permission_aborts },
        { "invalidation_acked_while_waiting_for_grant", test_invalidation_acked_while_waiting_for_grant },
        { "invalidation_acked_while_in_barrier",        test_invalidation_acked_while_in_barrier },
        { "barrier_release_of_other_generation",        test_barrier_release_of_other_generation },
        { "session_drop_wakes_barrier",                 test_session_drop_wakes_barrier },
        { "abort_wakes_acquire",                        test_abort_wakes_acquire },
        { "acquire_times_out_and_drops_session",        test_acquire_times_out_and_drops_session },
        { "finalize_leaves_no_threads_or_fds",          test_finalize_leaves_no_threads_or_fds },
        { "log_records_events_with_correlation",        test_log_records_events_with_correlation },
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]) == 0 ? 0 : 1;
}
