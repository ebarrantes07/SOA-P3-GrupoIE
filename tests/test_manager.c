/* Pruebas del coordinador (#4), sin sockets.
 *
 * Se alimentan tramas con mgr_dispatch y se lee lo que quedó encolado en los
 * buffers de salida. Eso cubre el protocolo completo —registro, concesiones,
 * invalidaciones, barreras, rechazos, pérdida de un nodo— de forma
 * determinista y sin procesos. Lo que queda afuera es el ciclo poll(), que
 * solo mueve bytes y se prueba de punta a punta.
 */
#include <string.h>

#include "manager.h"
#include "test_util.h"

/* Trama de salida ya decodificada, para poder afirmar sobre ella. */
struct salida {
    struct wire_header h;
    uint8_t pl[DSM_MAX_PAYLOAD];
};

/* Saca todo lo encolado en `ci` y lo decodifica. Devuelve cuántas tramas. */
static size_t drenar(mgr *m, int ci, struct salida *out, size_t cap) {
    struct conn *c = mgr_conn(m, ci);
    if (c == NULL)
        return 0;

    /* Los bytes de salida se vuelven a meter por la entrada de una conexión
     * auxiliar, que es la forma honesta de verificarlos: si no se pudieran
     * volver a extraer como tramas, el emisor estaría mal. */
    static struct conn espejo;
    conn_init(&espejo, -1);

    uint8_t buf[CONN_OUT_CAP];
    size_t n = conn_take(c, buf, sizeof buf);
    if (n == 0)
        return 0;
    if (!conn_feed(&espejo, buf, n))
        return 0;

    size_t vistas = 0;
    while (vistas < cap) {
        enum conn_frame f = conn_next(&espejo, &out[vistas].h,
                                      out[vistas].pl, DSM_MAX_PAYLOAD);
        if (f != CONN_FRAME_READY)
            break;
        vistas++;
    }
    return vistas;
}

/* Cuenta cuántas de las tramas drenadas son de un tipo. */
static size_t cuantas(const struct salida *s, size_t n, uint8_t type) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i].h.type == type)
            k++;
    return k;
}

static const struct salida *buscar(const struct salida *s, size_t n,
                                   uint8_t type) {
    for (size_t i = 0; i < n; i++)
        if (s[i].h.type == type)
            return &s[i];
    return NULL;
}

/* ---- Constructores de tramas entrantes -------------------------------- */

static struct wire_header hello(uint32_t node, uint32_t nodes, uint32_t pages,
                                uint8_t *pl) {
    wire_put_u32(pl + 0, nodes);
    wire_put_u32(pl + 4, pages);
    struct wire_header h;
    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type    = DSM_WIRE_HELLO;
    h.length  = 8u;
    h.node_id = node;
    h.page_id = DSM_NO_PAGE;
    return h;
}

static struct wire_header op(uint8_t type, uint32_t node, uint32_t page,
                             uint64_t req, uint32_t len) {
    struct wire_header h;
    memset(&h, 0, sizeof h);
    h.version    = DSM_WIRE_VERSION;
    h.type       = type;
    h.length     = len;
    h.node_id    = node;
    h.page_id    = page;
    h.request_id = req;
    return h;
}

/* Registra los `n` nodos; la conexión i queda con el nodo i. */
static bool arrancar(mgr *m, uint32_t n, uint32_t pages) {
    uint8_t pl[8];
    for (uint32_t i = 0; i < n; i++) {
        int ci = mgr_accept_conn(m, 100 + (int)i);
        if (ci != (int)i)
            return false;
        struct wire_header h = hello(i, n, 0, pl);
        if (mgr_dispatch(m, ci, &h, pl, 1000) != MGR_KEEP)
            return false;
        (void)pages;
    }
    return sess_state_of(mgr_session(m)) == SESS_RUNNING;
}

/* ---- Registro ---------------------------------------------------------- */

static bool test_session_start_goes_to_everyone(void) {
    mgr *m = mgr_create(2, 8, 777, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 8));

    /* SESSION_START lleva la configuración que define el coordinador: de ahí
     * el trabajador toma la cantidad de páginas (D0.7). */
    struct salida s[8];
    for (uint32_t n = 0; n < 2; n++) {
        size_t k = drenar(m, (int)n, s, 8);
        const struct salida *ss = buscar(s, k, DSM_WIRE_SESSION_START);
        CHECK(ss != NULL);
        CHECK(ss->h.length == 24u);
        CHECK(ss->h.node_id == n);
        CHECK(wire_get_u32(ss->pl + 0) == 2u);            /* nodes     */
        CHECK(wire_get_u32(ss->pl + 4) == 8u);            /* pages     */
        CHECK(wire_get_u32(ss->pl + 8) == DSM_PAGE_SIZE); /* page_size */
        CHECK(wire_get_u64(ss->pl + 16) == 777u);         /* run_id    */
    }

    /* Nadie recibió SESSION_START antes de que estuvieran los dos: la primera
     * conexión no tenía nada encolado hasta el segundo HELLO. */
    mgr_destroy(m);

    m = mgr_create(2, 8, 1, NULL);
    CHECK(m != NULL);
    uint8_t pl[8];
    int c0 = mgr_accept_conn(m, 10);
    struct wire_header h = hello(0, 2, 0, pl);
    CHECK(mgr_dispatch(m, c0, &h, pl, 1000) == MGR_KEEP);
    CHECK(drenar(m, c0, s, 8) == 0);     /* todavía nada */
    mgr_destroy(m);
    return true;
}

static bool test_duplicate_id_gets_an_error_not_a_slot(void) {
    mgr *m = mgr_create(3, 4, 1, NULL);
    CHECK(m != NULL);
    uint8_t pl[8];

    int c0 = mgr_accept_conn(m, 10);
    struct wire_header h = hello(1, 3, 0, pl);
    CHECK(mgr_dispatch(m, c0, &h, pl, 1000) == MGR_KEEP);
    CHECK(mgr_node_of_conn(m, c0) == 1);

    /* Otra conexión con el mismo id: ERROR explícito, y NO se le asigna el
     * nodo, que sigue siendo del primero. */
    int c1 = mgr_accept_conn(m, 11);
    CHECK(mgr_dispatch(m, c1, &h, pl, 1000) == MGR_KEEP);
    CHECK(mgr_node_of_conn(m, c1) == -1);
    CHECK(mgr_conn_of_node(m, 1) == c0);

    struct salida s[4];
    size_t k = drenar(m, c1, s, 4);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);
    mgr_destroy(m);
    return true;
}

static bool test_late_join_is_rejected_with_an_error(void) {
    mgr *m = mgr_create(1, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 1, 4));

    /* La sesión ya arrancó: el recién llegado recibe ERROR en vez de un
     * socket cerrado en la cara. Hay casillero para él de más justamente
     * para poder contestarle. */
    uint8_t pl[8];
    int cx = mgr_accept_conn(m, 99);
    CHECK(cx >= 0);
    struct wire_header h = hello(0, 1, 0, pl);
    CHECK(mgr_dispatch(m, cx, &h, pl, 2000) == MGR_KEEP);

    struct salida s[4];
    size_t k = drenar(m, cx, s, 4);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);
    mgr_destroy(m);
    return true;
}

/* Operar antes del HELLO no tiene emisor conocido. */
static bool test_operation_before_hello_is_rejected(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    int ci = mgr_accept_conn(m, 10);

    uint8_t pl[8];
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header h = op(DSM_WIRE_ACQ_REQ, 0, 0, 1, 8u);
    h.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, ci, &h, pl, 1000) == MGR_KEEP);

    struct salida s[4];
    size_t k = drenar(m, ci, s, 4);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);
    mgr_destroy(m);
    return true;
}

/* Un nodo no puede operar en nombre de otro: el directorio solo ve el número
 * que se le pasa, así que el coordinador tiene que atarlo a la conexión. */
static bool test_node_cannot_impersonate_another(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    (void)drenar(m, 0, NULL, 0);

    uint8_t pl[8];
    wire_put_u64(pl, DSM_NO_VERSION);
    /* La conexión 0 es del nodo 0, y manda una solicitud firmada por el 1. */
    struct wire_header h = op(DSM_WIRE_ACQ_REQ, 1, 0, 5, 8u);
    h.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &h, pl, 1000) == MGR_KEEP);

    struct salida s[4];
    size_t k = drenar(m, 0, s, 4);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_EINVAL);

    /* Y el nodo 1 no quedó con nada adquirido. */
    struct dir_page_info info;
    CHECK(dir_page_info(mgr_directory(m), 0, &info));
    CHECK(info.writer == -1 && info.readers == 0);
    mgr_destroy(m);
    return true;
}

/* Criterio de #4: una página fuera de rango se rechaza "con error explícito y
 * sin acceso fuera de límites".
 *
 * El rango que importa es el de la SESIÓN, no el del protocolo: wire_check
 * solo acota contra DSM_MAX_PAGES (256), porque no conoce la configuración de
 * esta corrida. Una página 100 en una sesión de 4 es una trama perfectamente
 * válida que el coordinador tiene que rechazar igual. */
static bool test_page_out_of_session_range_is_rejected(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8 + DSM_PAGE_SIZE];
    memset(pl, 0, sizeof pl);

    /* La página 100 existe para el protocolo y no para esta sesión. */
    const uint32_t fuera = 100;
    CHECK(fuera < DSM_MAX_PAGES);

    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header q = op(DSM_WIRE_ACQ_REQ, 0, fuera, 1, 8u);
    q.aux = DSM_WIRE_MODE_READ;
    CHECK(wire_check(&q) == WIRE_OK);    /* la trama es válida */
    CHECK(mgr_dispatch(m, 0, &q, pl, 1000) == MGR_KEEP);

    size_t k = drenar(m, 0, s, 8);
    CHECK(cuantas(s, k, DSM_WIRE_ACQ_GRANT) == 0);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_EINVAL);

    /* Lo mismo con los releases y la última página válida, que sí pasa. */
    struct wire_header rw = op(DSM_WIRE_REL_WRITE, 0, fuera, 1,
                               DSM_PAGE_SIZE);
    CHECK(mgr_dispatch(m, 0, &rw, pl, 1100) == MGR_KEEP);
    k = drenar(m, 0, s, 8);
    e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_EINVAL);

    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header ok = op(DSM_WIRE_ACQ_REQ, 0, 3, 2, 8u);
    ok.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 0, &ok, pl, 1200) == MGR_KEEP);
    k = drenar(m, 0, s, 8);
    CHECK(buscar(s, k, DSM_WIRE_ACQ_GRANT) != NULL);
    mgr_destroy(m);
    return true;
}

/* Un rechazo no deja al nodo con una espera abierta.
 *
 * El cronómetro de la operación tiene que arrancar ANTES de consultar al
 * directorio —una concesión inmediata lo cierra desde dentro de dir_acquire—,
 * así que una trama rechazada alcanza a abrirlo. Si no se deshace, el plazo
 * de esa espera vence y aborta la sesión con ETIMEOUT: el registro culparía a
 * un nodo lento por lo que fue un error de argumentos.
 *
 * Se cubren los cuatro sitios que abren el reloj: las tres tramas de
 * directorio y la llegada a una barrera. */
static bool test_rejection_leaves_no_pending_wait(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    const uint64_t plazo = 1000;         /* ns */
    const uint32_t fuera = 100;          /* página válida para el protocolo */
    CHECK(fuera < DSM_MAX_PAGES);

    uint8_t pl[8 + DSM_PAGE_SIZE];
    memset(pl, 0, sizeof pl);

    /* Punto de partida: con la sesión corriendo y nadie esperando, ningún
     * plazo vence por más que avance el reloj. */
    CHECK(!mgr_check_timeouts(m, 1000 + 1000 * plazo, plazo));

    /* (a) ACQ_REQ rechazado. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header q = op(DSM_WIRE_ACQ_REQ, 0, fuera, 1, 8u);
    q.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 0, &q, pl, 1000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ERROR) != NULL);
    CHECK(!mgr_check_timeouts(m, 1000 + 10 * plazo, plazo));

    /* (b) REL_WRITE rechazado. */
    struct wire_header rw = op(DSM_WIRE_REL_WRITE, 0, fuera, 2, DSM_PAGE_SIZE);
    CHECK(mgr_dispatch(m, 0, &rw, pl, 2000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ERROR) != NULL);
    CHECK(!mgr_check_timeouts(m, 2000 + 10 * plazo, plazo));

    /* (c) REL_READ rechazado. */
    struct wire_header rr = op(DSM_WIRE_REL_READ, 0, fuera, 3, 0u);
    CHECK(mgr_dispatch(m, 0, &rr, NULL, 3000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ERROR) != NULL);
    CHECK(!mgr_check_timeouts(m, 3000 + 10 * plazo, plazo));

    /* (d) Llegada a una barrera rechazada por generación. */
    uint8_t bp[8];
    wire_put_u64(bp, 3u);
    struct wire_header b = op(DSM_WIRE_BARRIER_ARRIVE, 0, DSM_NO_PAGE, 0u, 8u);
    b.aux = 1u;
    CHECK(mgr_dispatch(m, 0, &b, bp, 4000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ERROR) != NULL);
    CHECK(!mgr_check_timeouts(m, 4000 + 10 * plazo, plazo));

    /* Cuatro rechazos y la sesión sigue en pie. */
    CHECK(sess_state_of(mgr_session(m)) == SESS_RUNNING);

    /* El contraste, que es lo que le da valor a lo anterior: una espera REAL
     * sí vence. Sin esta mitad la prueba pasaría igual con un coordinador que
     * no midiera ningún plazo. El nodo 1 queda encolado detrás del escritor. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 1, 10, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 5000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ACQ_GRANT) != NULL);

    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r2 = op(DSM_WIRE_ACQ_REQ, 1, 1, 11, 8u);
    r2.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r2, pl, 6000) == MGR_KEEP);
    CHECK(drenar(m, 1, s, 8) == 0);                  /* esperando de verdad */

    CHECK(!mgr_check_timeouts(m, 6000 + plazo / 2, plazo));
    CHECK(mgr_check_timeouts(m, 6000 + plazo, plazo));
    CHECK(sess_state_of(mgr_session(m)) == SESS_ABORTED);
    mgr_destroy(m);
    return true;
}

/* Rechazar un reenvío no borra la espera que ese nodo ya tenía en pie.
 *
 * Un ACQ_REQ reenviado no es un "duplicado" para el directorio: con una
 * solicitud viva devuelve DIR_ESTATE y la trama se rechaza. Ese rechazo pasa
 * por el mismo camino que cierra el cronómetro, y acá NO tiene que cerrarlo,
 * porque la espera original sigue abierta y es la única señal que delata al
 * nodo que no responde.
 *
 * Es el caso que justifica las dos mitades del arreglo: que sess_op_begin no
 * pise un reloj abierto (y lo diga en el retorno), y que from_dir cierre solo
 * lo que abrió esta trama. Si cualquiera de las dos faltara, el plazo o se
 * correría a 2500 o desaparecería, y un nodo perdido que reintenta más seguido
 * que el plazo no se detectaría nunca. */
static bool test_resend_keeps_the_pending_wait(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    const uint64_t plazo = 1000;
    uint8_t pl[8 + DSM_PAGE_SIZE];
    memset(pl, 0, sizeof pl);

    /* El nodo 0 se queda con la escritura; el nodo 1 pide y queda encolado. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 1, 10, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 1000) == MGR_KEEP);
    (void)drenar(m, 0, s, 8);

    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = op(DSM_WIRE_ACQ_REQ, 1, 1, 11, 8u);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r, pl, 2000) == MGR_KEEP);
    CHECK(drenar(m, 1, s, 8) == 0);

    /* Reenvía la MISMA solicitud a mitad de camino del plazo: se la rechazan
     * por tener una solicitud viva, no se la conceden. */
    wire_put_u64(pl, DSM_NO_VERSION);
    CHECK(mgr_dispatch(m, 1, &r, pl, 2000 + plazo / 2) == MGR_KEEP);
    size_t k = drenar(m, 1, s, 8);
    CHECK(cuantas(s, k, DSM_WIRE_ACQ_GRANT) == 0);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);

    /* Y la espera original sobrevive al rechazo, contada desde la primera
     * solicitud: vence a 2000 + plazo, no a 2500 + plazo ni nunca. */
    CHECK(!mgr_check_timeouts(m, 2000 + plazo - 1, plazo));
    CHECK(mgr_check_timeouts(m, 2000 + plazo, plazo));
    CHECK(sess_state_of(mgr_session(m)) == SESS_ABORTED);
    mgr_destroy(m);
    return true;
}

/* Criterio de #4: con un escritor activo no se concede ningún otro acceso
 * hasta que libere. */
static bool test_active_writer_blocks_everyone(void) {
    mgr *m = mgr_create(3, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 3, 4));
    struct salida s[8];
    for (int i = 0; i < 3; i++) (void)drenar(m, i, s, 8);

    uint8_t pl[8 + DSM_PAGE_SIZE];
    memset(pl, 0, sizeof pl);

    /* n0 toma la escritura. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 1, 1, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 1000) == MGR_KEEP);
    CHECK(buscar(s, drenar(m, 0, s, 8), DSM_WIRE_ACQ_GRANT) != NULL);

    /* n1 pide lectura y n2 escritura: ninguno recibe NADA, ni concesión ni
     * error — quedan encolados, que es lo correcto. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r1 = op(DSM_WIRE_ACQ_REQ, 1, 1, 1, 8u);
    r1.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r1, pl, 1100) == MGR_KEEP);
    struct wire_header w2 = op(DSM_WIRE_ACQ_REQ, 2, 1, 1, 8u);
    w2.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 2, &w2, pl, 1200) == MGR_KEEP);

    CHECK(drenar(m, 1, s, 8) == 0);
    CHECK(drenar(m, 2, s, 8) == 0);
    struct dir_page_info info;
    CHECK(dir_page_info(mgr_directory(m), 1, &info));
    CHECK(info.writer == 0 && info.readers == 0 && info.queued == 2);

    /* Recién al liberar, se concede al siguiente de la cola. */
    memset(pl, 0x5E, DSM_PAGE_SIZE);
    struct wire_header rel = op(DSM_WIRE_REL_WRITE, 0, 1, 1, DSM_PAGE_SIZE);
    CHECK(mgr_dispatch(m, 0, &rel, pl, 1300) == MGR_KEEP);

    size_t k = drenar(m, 1, s, 8);
    const struct salida *g = buscar(s, k, DSM_WIRE_ACQ_GRANT);
    CHECK(g != NULL && g->h.aux == DSM_WIRE_MODE_READ);
    CHECK(g->pl[8] == 0x5E);          /* ve lo que el escritor publicó */
    mgr_destroy(m);
    return true;
}

/* ---- Concesiones ------------------------------------------------------- */

static bool test_read_is_granted_with_data(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8];
    wire_put_u64(pl, DSM_NO_VERSION);   /* no tengo copia */
    struct wire_header h = op(DSM_WIRE_ACQ_REQ, 0, 2, 10, 8u);
    h.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 0, &h, pl, 5000) == MGR_KEEP);

    size_t k = drenar(m, 0, s, 8);
    const struct salida *g = buscar(s, k, DSM_WIRE_ACQ_GRANT);
    CHECK(g != NULL);
    CHECK(g->h.request_id == 10);
    CHECK(g->h.page_id == 2);
    CHECK(g->h.aux == DSM_WIRE_MODE_READ);
    /* Sin copia válida, la concesión trae la página, y la bandera y la
     * longitud cuentan la misma historia. */
    CHECK((g->h.flags & DSM_WIRE_F_HAS_DATA) != 0);
    CHECK(g->h.length == 8u + DSM_PAGE_SIZE);
    CHECK(wire_get_u64(g->pl) == 0u);   /* versión inicial */
    mgr_destroy(m);
    return true;
}

/* La secuencia completa de una escritura con un lector que tiene copia:
 * solicitud, invalidación, ACK, concesión, publicación. */
static bool test_write_round_trip_with_invalidation(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8 + DSM_PAGE_SIZE];

    /* n1 lee la página 0 y la suelta: le queda copia retenida. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = op(DSM_WIRE_ACQ_REQ, 1, 0, 1, 8u);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r, pl, 1000) == MGR_KEEP);
    struct wire_header rr = op(DSM_WIRE_REL_READ, 1, 0, 1, 0u);
    CHECK(mgr_dispatch(m, 1, &rr, NULL, 1100) == MGR_KEEP);
    (void)drenar(m, 1, s, 8);

    /* n0 pide WRITE: tiene que salir un INVALIDATE para n1, con el
     * solicitante en aux (D0.12), y NINGUNA concesión todavía. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 0, 7, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 2000) == MGR_KEEP);

    size_t k1 = drenar(m, 1, s, 8);
    const struct salida *inv = buscar(s, k1, DSM_WIRE_INVALIDATE);
    CHECK(inv != NULL);
    CHECK(inv->h.node_id == 1);        /* destinatario */
    CHECK(inv->h.aux == 0u);           /* solicitante */
    CHECK(inv->h.request_id == 7);

    struct salida s0[8];
    CHECK(cuantas(s0, drenar(m, 0, s0, 8), DSM_WIRE_ACQ_GRANT) == 0);

    /* n1 confirma, repitiendo request_id y solicitante (D2.1). */
    struct wire_header ack = op(DSM_WIRE_INVALIDATE_ACK, 1, 0, 7, 0u);
    ack.aux = 0u;
    CHECK(mgr_dispatch(m, 1, &ack, NULL, 2500) == MGR_KEEP);

    /* Ahora sí la concesión, y con la duración medida desde la solicitud. */
    size_t k0 = drenar(m, 0, s0, 8);
    const struct salida *g = buscar(s0, k0, DSM_WIRE_ACQ_GRANT);
    CHECK(g != NULL && g->h.aux == DSM_WIRE_MODE_WRITE);
    CHECK(g->h.request_id == 7);

    /* n0 escribe y suelta: publica y sube la versión. */
    memset(pl, 0xAB, DSM_PAGE_SIZE);
    struct wire_header rw = op(DSM_WIRE_REL_WRITE, 0, 0, 7, DSM_PAGE_SIZE);
    CHECK(mgr_dispatch(m, 0, &rw, pl, 3000) == MGR_KEEP);

    k0 = drenar(m, 0, s0, 8);
    const struct salida *wa = buscar(s0, k0, DSM_WIRE_REL_WRITE_ACK);
    CHECK(wa != NULL && wire_get_u64(wa->pl) == 1u);

    struct dir_page_info info;
    CHECK(dir_page_info(mgr_directory(m), 0, &info));
    CHECK(info.version == 1 && info.writer == -1);
    CHECK(dir_page_data(mgr_directory(m), 0)[0] == 0xAB);
    mgr_destroy(m);
    return true;
}

/* Un ACK rezagado de otra ronda se rechaza y no concede nada (D2.1). */
static bool test_stale_ack_is_rejected(void) {
    mgr *m = mgr_create(3, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 3, 4));
    struct salida s[8];
    for (int i = 0; i < 3; i++) (void)drenar(m, i, s, 8);

    uint8_t pl[8];

    /* n1 y n2 toman copia. */
    for (uint32_t n = 1; n <= 2; n++) {
        wire_put_u64(pl, DSM_NO_VERSION);
        struct wire_header r = op(DSM_WIRE_ACQ_REQ, n, 0, 1, 8u);
        r.aux = DSM_WIRE_MODE_READ;
        CHECK(mgr_dispatch(m, (int)n, &r, pl, 1000) == MGR_KEEP);
        struct wire_header rr = op(DSM_WIRE_REL_READ, n, 0, 1, 0u);
        CHECK(mgr_dispatch(m, (int)n, &rr, NULL, 1100) == MGR_KEEP);
    }

    /* n0 pide WRITE con request_id 5. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 0, 5, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 2000) == MGR_KEEP);

    /* n1 confirma bien; n2 manda un ACK con OTRO solicitante. */
    struct wire_header a1 = op(DSM_WIRE_INVALIDATE_ACK, 1, 0, 5, 0u);
    a1.aux = 0u;
    CHECK(mgr_dispatch(m, 1, &a1, NULL, 2100) == MGR_KEEP);

    struct wire_header a2 = op(DSM_WIRE_INVALIDATE_ACK, 2, 0, 5, 0u);
    a2.aux = 2u;                    /* solicitante equivocado */
    CHECK(mgr_dispatch(m, 2, &a2, NULL, 2200) == MGR_KEEP);

    struct salida s2[8];
    size_t k = drenar(m, 2, s2, 8);
    const struct salida *e = buscar(s2, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);

    /* Y el WRITE sigue esperando. */
    struct dir_page_info info;
    CHECK(dir_page_info(mgr_directory(m), 0, &info));
    CHECK(info.writer == -1 && info.pending_acks == 1);
    mgr_destroy(m);
    return true;
}

/* Criterio de #4: páginas distintas se atienden de forma independiente. Una
 * espera de ACK en una no impide atender otra. */
static bool test_pages_are_independent(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8];

    /* n1 retiene copia de la página 0. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = op(DSM_WIRE_ACQ_REQ, 1, 0, 1, 8u);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r, pl, 1000) == MGR_KEEP);
    struct wire_header rr = op(DSM_WIRE_REL_READ, 1, 0, 1, 0u);
    CHECK(mgr_dispatch(m, 1, &rr, NULL, 1100) == MGR_KEEP);
    (void)drenar(m, 1, s, 8);

    /* n0 pide WRITE de la 0: queda esperando el ACK de n1. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 0, 7, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 2000) == MGR_KEEP);
    struct dir_page_info i0;
    CHECK(dir_page_info(mgr_directory(m), 0, &i0));
    CHECK(i0.pending_acks == 1 && i0.writer == -1);

    /* Mientras eso espera, n1 pide la página 3 y se le concede de una. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r3 = op(DSM_WIRE_ACQ_REQ, 1, 3, 2, 8u);
    r3.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r3, pl, 2100) == MGR_KEEP);

    size_t k = drenar(m, 1, s, 8);
    const struct salida *g = buscar(s, k, DSM_WIRE_ACQ_GRANT);
    CHECK(g != NULL && g->h.page_id == 3);

    /* Y la página 0 sigue donde estaba: una no arrastró a la otra. */
    CHECK(dir_page_info(mgr_directory(m), 0, &i0));
    CHECK(i0.pending_acks == 1 && i0.writer == -1);
    mgr_destroy(m);
    return true;
}

/* ---- Barreras ---------------------------------------------------------- */

static bool test_barrier_release_goes_to_all(void) {
    mgr *m = mgr_create(3, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 3, 4));
    struct salida s[8];
    for (int i = 0; i < 3; i++) (void)drenar(m, i, s, 8);

    uint8_t pl[8];
    wire_put_u64(pl, 0u);                      /* generación 0 */

    for (uint32_t n = 0; n < 2; n++) {
        struct wire_header b = op(DSM_WIRE_BARRIER_ARRIVE, n, DSM_NO_PAGE,
                                  0u, 8u);
        b.aux = 5u;
        CHECK(mgr_dispatch(m, (int)n, &b, pl, 1000 + n) == MGR_KEEP);
        /* Nadie se libera todavía. */
        CHECK(cuantas(s, drenar(m, (int)n, s, 8),
                      DSM_WIRE_BARRIER_RELEASE) == 0);
    }

    struct wire_header b = op(DSM_WIRE_BARRIER_ARRIVE, 2, DSM_NO_PAGE, 0u, 8u);
    b.aux = 5u;
    CHECK(mgr_dispatch(m, 2, &b, pl, 1500) == MGR_KEEP);

    /* Los TRES reciben la liberación, con el barrier_id en aux y la
     * generación que acaba de cerrarse en la carga. */
    for (int i = 0; i < 3; i++) {
        size_t k = drenar(m, i, s, 8);
        const struct salida *rel = buscar(s, k, DSM_WIRE_BARRIER_RELEASE);
        CHECK(rel != NULL);
        CHECK(rel->h.aux == 5u);
        CHECK(rel->h.node_id == (uint32_t)i);
        CHECK(wire_get_u64(rel->pl) == 0u);
    }
    mgr_destroy(m);
    return true;
}

static bool test_barrier_wrong_generation_errors(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8];
    wire_put_u64(pl, 3u);                      /* generación inventada */
    struct wire_header b = op(DSM_WIRE_BARRIER_ARRIVE, 0, DSM_NO_PAGE, 0u, 8u);
    b.aux = 1u;
    CHECK(mgr_dispatch(m, 0, &b, pl, 1000) == MGR_KEEP);

    size_t k = drenar(m, 0, s, 8);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ESTATE);
    mgr_destroy(m);
    return true;
}

/* ---- Salida y pérdida -------------------------------------------------- */

static bool test_bye_is_ordered_and_closes(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    struct wire_header b = op(DSM_WIRE_BYE, 0, DSM_NO_PAGE, 0u, 0u);
    CHECK(mgr_dispatch(m, 0, &b, NULL, 1000) == MGR_CLOSE);
    CHECK(sess_left(mgr_session(m), 0));
    CHECK(sess_state_of(mgr_session(m)) == SESS_RUNNING);

    /* Cerrar su conexión después de un BYE NO aborta: ya se había ido. */
    mgr_drop_conn(m, 0, 1100);
    CHECK(sess_state_of(mgr_session(m)) == SESS_RUNNING);
    mgr_destroy(m);
    return true;
}

/* Criterio de #4: perder un trabajador hace que el resto reciba error. */
static bool test_losing_a_node_aborts_and_tells_the_rest(void) {
    mgr *m = mgr_create(3, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 3, 4));
    struct salida s[8];
    for (int i = 0; i < 3; i++) (void)drenar(m, i, s, 8);

    /* Se cae la conexión del nodo 1, sin BYE. */
    mgr_drop_conn(m, 1, 2000);
    CHECK(sess_state_of(mgr_session(m)) == SESS_ABORTED);

    /* Los otros dos reciben ERROR con ECONN, en vez de esperar para
     * siempre. */
    for (int i = 0; i < 3; i += 2) {
        size_t k = drenar(m, i, s, 8);
        const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
        CHECK(e != NULL && e->h.aux == (uint32_t)DSM_ECONN);
    }
    mgr_destroy(m);
    return true;
}

/* Si el nodo que se cae tenía copia y otro esperaba su ACK, su salida del
 * directorio desbloquea esa ronda: sin eso el WRITE no avanzaría nunca. */
static bool test_lost_node_releases_the_round_it_blocked(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    uint8_t pl[8];
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = op(DSM_WIRE_ACQ_REQ, 1, 0, 1, 8u);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 1, &r, pl, 1000) == MGR_KEEP);
    struct wire_header rr = op(DSM_WIRE_REL_READ, 1, 0, 1, 0u);
    CHECK(mgr_dispatch(m, 1, &rr, NULL, 1100) == MGR_KEEP);
    (void)drenar(m, 1, s, 8);

    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header w = op(DSM_WIRE_ACQ_REQ, 0, 0, 7, 8u);
    w.aux = DSM_WIRE_MODE_WRITE;
    CHECK(mgr_dispatch(m, 0, &w, pl, 2000) == MGR_KEEP);
    (void)drenar(m, 0, s, 8);

    struct dir_page_info info;
    CHECK(dir_page_info(mgr_directory(m), 0, &info));
    CHECK(info.pending_acks == 1);

    /* Se cae n1, que era el que debía confirmar. */
    mgr_drop_conn(m, 1, 2500);

    /* La ronda se cerró: su copia dejó de existir, que es lo que el ACK
     * confirmaría (D0.13). La sesión igual queda abortada. */
    CHECK(dir_page_info(mgr_directory(m), 0, &info));
    CHECK(info.pending_acks == 0);
    CHECK(sess_state_of(mgr_session(m)) == SESS_ABORTED);
    mgr_destroy(m);
    return true;
}

/* Una sesión abortada no procesa nada más: solo informa. */
static bool test_aborted_session_only_reports(void) {
    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 2, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);
    (void)drenar(m, 1, s, 8);

    mgr_abort_session(m, DSM_ETIMEOUT, DSM_MAX_NODES, 3000);
    CHECK(sess_state_of(mgr_session(m)) == SESS_ABORTED);
    (void)drenar(m, 0, s, 8);

    uint8_t pl[8];
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = op(DSM_WIRE_ACQ_REQ, 0, 0, 1, 8u);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(mgr_dispatch(m, 0, &r, pl, 3100) == MGR_KEEP);

    size_t k = drenar(m, 0, s, 8);
    CHECK(cuantas(s, k, DSM_WIRE_ACQ_GRANT) == 0);
    const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
    CHECK(e != NULL && e->h.aux == (uint32_t)DSM_EABORT);
    mgr_destroy(m);
    return true;
}

/* Un tipo que el coordinador EMITE no lo puede recibir: eso es otro
 * protocolo. */
static bool test_wrong_direction_is_a_protocol_error(void) {
    mgr *m = mgr_create(1, 4, 1, NULL);
    CHECK(m != NULL);
    CHECK(arrancar(m, 1, 4));
    struct salida s[8];
    (void)drenar(m, 0, s, 8);

    const uint8_t salientes[] = {
        DSM_WIRE_ACQ_GRANT, DSM_WIRE_INVALIDATE, DSM_WIRE_REL_WRITE_ACK,
        DSM_WIRE_REL_READ_ACK, DSM_WIRE_BARRIER_RELEASE, DSM_WIRE_ERROR,
    };
    for (size_t i = 0; i < sizeof salientes / sizeof salientes[0]; i++) {
        uint8_t pl[DSM_MAX_PAYLOAD];
        memset(pl, 0, sizeof pl);
        uint32_t min = 0, max = 0;
        CHECK(wire_payload_range(salientes[i], &min, &max));
        struct wire_header h = op(salientes[i], 0, 0, 1, min);
        if (salientes[i] == DSM_WIRE_BARRIER_RELEASE ||
            salientes[i] == DSM_WIRE_ERROR)
            h.page_id = DSM_NO_PAGE;
        CHECK(mgr_dispatch(m, 0, &h, pl, 1000) == MGR_KEEP);

        size_t k = drenar(m, 0, s, 8);
        const struct salida *e = buscar(s, k, DSM_WIRE_ERROR);
        CHECK(e != NULL && e->h.aux == (uint32_t)DSM_EPROTO);
    }
    mgr_destroy(m);
    return true;
}

static bool test_create_and_slots(void) {
    CHECK(mgr_create(0, 4, 1, NULL) == NULL);
    CHECK(mgr_create(DSM_MAX_NODES + 1, 4, 1, NULL) == NULL);
    CHECK(mgr_create(2, 0, 1, NULL) == NULL);

    mgr *m = mgr_create(2, 4, 1, NULL);
    CHECK(m != NULL);

    /* Hay un casillero de más para poder contestarle a un recién llegado. */
    int vistos = 0;
    while (mgr_accept_conn(m, 200 + vistos) >= 0)
        vistos++;
    CHECK(vistos == (int)MGR_MAX_CONNS);
    CHECK((uint32_t)vistos == DSM_MAX_NODES + 1u);

    CHECK(mgr_conn(m, -1) == NULL);
    CHECK(mgr_conn(m, 999) == NULL);
    CHECK(mgr_node_of_conn(m, 0) == -1);
    CHECK(mgr_conn_of_node(m, 0) == -1);
    CHECK(mgr_conn_of_node(m, DSM_MAX_NODES) == -1);
    CHECK(mgr_conn_fd(m, 0) == 200);
    CHECK(mgr_conn_live(m, 0));
    mgr_drop_conn(m, 0, 1);
    CHECK(!mgr_conn_live(m, 0));
    CHECK(mgr_conn_fd(m, 0) == -1);

    mgr_destroy(m);
    mgr_destroy(NULL);
    CHECK(mgr_session(NULL) == NULL && mgr_directory(NULL) == NULL);
    CHECK(mgr_accept_conn(NULL, 1) == -1);
    mgr_drop_conn(NULL, 0, 1);
    mgr_send_error(NULL, 0, DSM_EINVAL, 1);
    mgr_abort_session(NULL, DSM_EINVAL, 0, 1);
    return true;
}

int main(void) {
    const struct test_case tests[] = {
        {"create_and_slots", test_create_and_slots},
        {"session_start_goes_to_everyone", test_session_start_goes_to_everyone},
        {"duplicate_id_gets_an_error_not_a_slot", test_duplicate_id_gets_an_error_not_a_slot},
        {"late_join_is_rejected_with_an_error", test_late_join_is_rejected_with_an_error},
        {"operation_before_hello_is_rejected", test_operation_before_hello_is_rejected},
        {"node_cannot_impersonate_another", test_node_cannot_impersonate_another},
        {"page_out_of_session_range_is_rejected", test_page_out_of_session_range_is_rejected},
        {"rejection_leaves_no_pending_wait", test_rejection_leaves_no_pending_wait},
        {"resend_keeps_the_pending_wait", test_resend_keeps_the_pending_wait},
        {"active_writer_blocks_everyone", test_active_writer_blocks_everyone},
        {"read_is_granted_with_data", test_read_is_granted_with_data},
        {"write_round_trip_with_invalidation", test_write_round_trip_with_invalidation},
        {"stale_ack_is_rejected", test_stale_ack_is_rejected},
        {"pages_are_independent", test_pages_are_independent},
        {"barrier_release_goes_to_all", test_barrier_release_goes_to_all},
        {"barrier_wrong_generation_errors", test_barrier_wrong_generation_errors},
        {"bye_is_ordered_and_closes", test_bye_is_ordered_and_closes},
        {"losing_a_node_aborts_and_tells_the_rest", test_losing_a_node_aborts_and_tells_the_rest},
        {"lost_node_releases_the_round_it_blocked", test_lost_node_releases_the_round_it_blocked},
        {"aborted_session_only_reports", test_aborted_session_only_reports},
        {"wrong_direction_is_a_protocol_error", test_wrong_direction_is_a_protocol_error},
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]);
}
