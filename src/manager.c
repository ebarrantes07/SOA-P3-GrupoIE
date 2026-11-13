/* El coordinador: traduce entre tramas y los módulos de estado.
 * Ver include/manager.h. */
#include "manager.h"

#include <stdlib.h>
#include <string.h>

struct conn_slot {
    bool        live;
    int         fd;
    int         node;     /* -1 hasta que mande HELLO */
    struct conn c;
};

struct dsm_manager {
    dir     *d;
    sess    *s;
    dsm_log *lg;

    struct conn_slot slot[MGR_MAX_CONNS];
    int  conn_of_node[DSM_MAX_NODES];

    /* Contexto del evento en curso, para que el callback del directorio
     * sepa en qué momento está. El directorio emite sus acciones DENTRO de
     * la llamada que las causa, así que un solo juego de campos alcanza: no
     * hay dos eventos en vuelo con un solo hilo (D0.1). */
    uint64_t now_ns;
    bool     failed;      /* una acción no se pudo encolar: abortar */

    uint64_t reg_start_ns;  /* 0 hasta la primera conexión aceptada */
};

/* ---- Utilidades de trama ---------------------------------------------- */

static struct wire_header base(uint8_t type, uint32_t node, uint32_t page,
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

/* Encola una trama en la conexión de `node`. false si ese nodo no tiene
 * conexión viva o si su buffer de salida está lleno — y lo segundo significa
 * que dejó de leer (D3.2). */
static bool send_to(mgr *m, uint32_t node, const struct wire_header *h,
                    const void *payload) {
    if (node >= DSM_MAX_NODES)
        return false;
    int ci = m->conn_of_node[node];
    if (ci < 0 || !m->slot[ci].live)
        return false;
    if (!conn_queue(&m->slot[ci].c, h, payload))
        return false;
    log_count_sent(m->lg, h->type, h->length);
    return true;
}

static void log_row(mgr *m, enum log_event ev, uint32_t node, uint64_t req,
                    uint32_t page, enum dsm_mode mode, enum dsm_status st,
                    uint64_t dur) {
    struct log_row r = log_row_empty(ev);
    r.node_id    = node;
    r.request_id = req;
    r.page_id    = page;
    r.mode       = mode;
    r.status     = st;
    r.dur_ns     = dur;

    /* El estado de la página sale del directorio, no de una copia (D3.1). */
    struct dir_page_info info;
    if (page != LOG_NO_PAGE && dir_page_info(m->d, page, &info))
        r.page = &info;
    log_write(m->lg, &r);
}

/* ---- Acciones del directorio -> tramas -------------------------------- */

static void on_action(void *user, const struct dir_action *a) {
    mgr *m = user;
    bool ok = true;

    switch (a->type) {
    case DIR_ACT_GRANT: {
        /* Con datos o sin datos: lo decide el directorio según si el nodo ya
         * tiene la versión vigente en su caché. La bandera y la longitud
         * tienen que contar la misma historia o wire_check lo rechaza. */
        uint8_t pl[8u + DSM_PAGE_SIZE];
        wire_put_u64(pl, a->version);
        uint32_t len = 8u;
        struct wire_header h = base(DSM_WIRE_ACQ_GRANT, a->node, a->page_id,
                                    a->request_id, len);
        h.aux = (a->mode == DSM_WRITE) ? DSM_WIRE_MODE_WRITE
                                       : DSM_WIRE_MODE_READ;
        if (a->data != NULL) {
            memcpy(pl + 8u, a->data, DSM_PAGE_SIZE);
            h.length = 8u + DSM_PAGE_SIZE;
            h.flags  = DSM_WIRE_F_HAS_DATA;
        }
        ok = send_to(m, a->node, &h, pl);
        log_row(m, LOG_ACQ_GRANT, a->node, a->request_id, a->page_id,
                a->mode, DSM_OK,
                sess_op_elapsed(m->s, a->node, m->now_ns));
        sess_op_end(m->s, a->node);   /* su espera terminó */
        break;
    }
    case DIR_ACT_INVALIDATE: {
        struct wire_header h = base(DSM_WIRE_INVALIDATE, a->node, a->page_id,
                                    a->request_id, 0u);
        h.aux = a->requester;    /* el solicitante, que el ACK repite (D2.1) */
        ok = send_to(m, a->node, &h, NULL);
        log_row(m, LOG_INVALIDATE, a->node, a->request_id, a->page_id,
                (enum dsm_mode)LOG_NO_MODE, DSM_OK, LOG_NO_DURATION);
        break;
    }
    case DIR_ACT_WRITE_PUBLISHED: {
        uint8_t pl[8];
        wire_put_u64(pl, a->version);
        struct wire_header h = base(DSM_WIRE_REL_WRITE_ACK, a->node,
                                    a->page_id, a->request_id, 8u);
        ok = send_to(m, a->node, &h, pl);
        log_row(m, LOG_WRITE_PUBLISHED, a->node, a->request_id, a->page_id,
                DSM_WRITE, DSM_OK,
                sess_op_elapsed(m->s, a->node, m->now_ns));
        sess_op_end(m->s, a->node);
        break;
    }
    case DIR_ACT_READ_RELEASED: {
        struct wire_header h = base(DSM_WIRE_REL_READ_ACK, a->node,
                                    a->page_id, a->request_id, 0u);
        ok = send_to(m, a->node, &h, NULL);
        log_row(m, LOG_READ_RELEASED, a->node, a->request_id, a->page_id,
                DSM_READ, DSM_OK,
                sess_op_elapsed(m->s, a->node, m->now_ns));
        sess_op_end(m->s, a->node);
        break;
    }
    }

    /* Si una acción no se pudo entregar, el estado del directorio ya cambió
     * y el nodo nunca va a saberlo: no hay forma de seguir coherentes, así
     * que se marca para abortar al volver del evento. No se aborta acá
     * porque el directorio prohíbe llamarlo de vuelta desde el callback. */
    if (!ok)
        m->failed = true;
}

/* ---- Creación ---------------------------------------------------------- */

mgr *mgr_create(uint32_t nodes, uint32_t pages, uint64_t run_id, dsm_log *lg) {
    mgr *m = calloc(1, sizeof *m);
    if (m == NULL)
        return NULL;

    m->lg = lg;
    m->s  = sess_create(nodes, pages, run_id);
    m->d  = dir_create(nodes, pages, on_action, m);
    if (m->s == NULL || m->d == NULL) {
        mgr_destroy(m);
        return NULL;
    }
    for (uint32_t i = 0; i < DSM_MAX_NODES; i++)
        m->conn_of_node[i] = -1;
    for (uint32_t i = 0; i < MGR_MAX_CONNS; i++)
        m->slot[i].node = -1;
    return m;
}

void mgr_destroy(mgr *m) {
    if (m == NULL)
        return;
    dir_destroy(m->d);
    sess_destroy(m->s);
    free(m);
}

sess *mgr_session(mgr *m)   { return m == NULL ? NULL : m->s; }
dir  *mgr_directory(mgr *m) { return m == NULL ? NULL : m->d; }

/* ---- Conexiones -------------------------------------------------------- */

int mgr_accept_conn(mgr *m, int fd) {
    if (m == NULL)
        return -1;
    for (uint32_t i = 0; i < MGR_MAX_CONNS; i++) {
        if (m->slot[i].live)
            continue;
        m->slot[i].live = true;
        m->slot[i].fd   = fd;
        m->slot[i].node = -1;
        conn_init(&m->slot[i].c, fd);
        return (int)i;
    }
    return -1;
}

static bool slot_ok(const mgr *m, int ci) {
    return m != NULL && ci >= 0 && (uint32_t)ci < MGR_MAX_CONNS;
}

void mgr_drop_conn(mgr *m, int ci, uint64_t now_ns) {
    if (!slot_ok(m, ci) || !m->slot[ci].live)
        return;

    int node = m->slot[ci].node;
    m->slot[ci].live = false;
    m->slot[ci].node = -1;
    if (node < 0)
        return;   /* nunca mandó HELLO: no era un participante */

    m->conn_of_node[node] = -1;

    /* Si ya se había despedido, esto es el cierre normal de su socket y no
     * hay nada que hacer. Si no, el nodo se perdió: hay que sacarlo del
     * directorio para que ninguna ronda de invalidación quede esperando un
     * ACK que no va a llegar, y abortar, porque la política del enunciado
     * ante la pérdida de un participante es abortar y no seguir con menos
     * (D0.13).
     *
     * El orden importa: primero el directorio, cuyo dir_node_leave puede
     * desbloquear concesiones y emitirlas; después el aborto, que le encola
     * el ERROR a los que quedan. Al revés, esas concesiones se encolarían
     * detrás de un ERROR que ya cerró la sesión. */
    if (sess_left(m->s, (uint32_t)node))
        return;

    m->now_ns = now_ns;
    m->failed = false;
    dir_node_leave(m->d, (uint32_t)node);
    mgr_abort_session(m, DSM_ECONN, (uint32_t)node, now_ns);
}

struct conn *mgr_conn(mgr *m, int ci) {
    return slot_ok(m, ci) && m->slot[ci].live ? &m->slot[ci].c : NULL;
}

int mgr_conn_count(const mgr *m) {
    return m == NULL ? 0 : (int)MGR_MAX_CONNS;
}

int mgr_conn_fd(const mgr *m, int ci) {
    return slot_ok(m, ci) && m->slot[ci].live ? m->slot[ci].fd : -1;
}

bool mgr_conn_live(const mgr *m, int ci) {
    return slot_ok(m, ci) && m->slot[ci].live;
}

int mgr_conn_of_node(const mgr *m, uint32_t node) {
    if (m == NULL || node >= DSM_MAX_NODES)
        return -1;
    return m->conn_of_node[node];
}

int mgr_node_of_conn(const mgr *m, int ci) {
    return slot_ok(m, ci) && m->slot[ci].live ? m->slot[ci].node : -1;
}

/* ---- Errores y aborto -------------------------------------------------- */

/* El detalle del ERROR es texto corto para que el registro del trabajador
 * diga algo legible; el código va en `aux`, que es lo que se interpreta. */
static void queue_error(mgr *m, int ci, enum dsm_status st) {
    if (!slot_ok(m, ci) || !m->slot[ci].live)
        return;
    const char *txt = log_status_name(st);
    uint32_t len = (uint32_t)strlen(txt);
    if (len > DSM_ERR_DETAIL_MAX)
        len = DSM_ERR_DETAIL_MAX;

    int node = m->slot[ci].node;
    struct wire_header h = base(DSM_WIRE_ERROR,
                                node >= 0 ? (uint32_t)node : 0u,
                                DSM_NO_PAGE, 0u, len);
    h.aux = (uint32_t)st;
    if (conn_queue(&m->slot[ci].c, &h, txt))
        log_count_sent(m->lg, h.type, h.length);
}

void mgr_send_error(mgr *m, int ci, enum dsm_status st, uint64_t now_ns) {
    if (!slot_ok(m, ci))
        return;
    m->now_ns = now_ns;
    queue_error(m, ci, st);
    int node = m->slot[ci].node;
    log_row(m, LOG_ERROR, node >= 0 ? (uint32_t)node : LOG_NO_NODE,
            LOG_NO_REQUEST, LOG_NO_PAGE, (enum dsm_mode)LOG_NO_MODE, st,
            LOG_NO_DURATION);
}

void mgr_abort_session(mgr *m, enum dsm_status st, uint32_t except_node,
                       uint64_t now_ns) {
    if (m == NULL)
        return;
    m->now_ns = now_ns;

    /* El ERROR se le encola a cada nodo vivo ANTES de marcar la sesión como
     * abortada, porque encolarlo depende de que su conexión siga en pie. */
    for (uint32_t i = 0; i < MGR_MAX_CONNS; i++) {
        int node = m->slot[i].node;
        if (!m->slot[i].live || node < 0)
            continue;
        if ((uint32_t)node == except_node)
            continue;
        queue_error(m, (int)i, st);
    }
    sess_abort(m->s);
    log_row(m, LOG_SESSION_END, LOG_NO_NODE, LOG_NO_REQUEST, LOG_NO_PAGE,
            (enum dsm_mode)LOG_NO_MODE, st, LOG_NO_DURATION);
}

/* ---- Plazos ------------------------------------------------------------ */

void mgr_registration_clock(mgr *m, uint64_t now_ns) {
    if (m == NULL || m->reg_start_ns != 0)
        return;
    m->reg_start_ns = now_ns;
}

/* La espera más próxima a vencer, o false si nadie espera. */
static bool earliest_wait(const mgr *m, uint64_t *out_start) {
    if (sess_state_of(m->s) == SESS_REGISTERING) {
        if (m->reg_start_ns == 0)
            return false;
        *out_start = m->reg_start_ns;
        return true;
    }
    if (sess_state_of(m->s) != SESS_RUNNING)
        return false;
    return sess_op_oldest(m->s, NULL, out_start);
}

bool mgr_check_timeouts(mgr *m, uint64_t now_ns, uint64_t timeout_ns) {
    if (m == NULL || timeout_ns == 0)
        return false;
    if (sess_state_of(m->s) == SESS_ABORTED ||
        sess_state_of(m->s) == SESS_FINISHED)
        return false;

    uint64_t start = 0;
    if (!earliest_wait(m, &start))
        return false;
    if (now_ns < start || now_ns - start < timeout_ns)
        return false;

    /* Quien espera no es el culpable, pero igual recibe el error: el nodo que
     * no responde puede ser cualquiera, y la política es abortar la sesión
     * entera y que todos se enteren (D0.13). */
    m->now_ns = now_ns;
    log_row(m, LOG_TIMEOUT, LOG_NO_NODE, LOG_NO_REQUEST, LOG_NO_PAGE,
            (enum dsm_mode)LOG_NO_MODE, DSM_ETIMEOUT, now_ns - start);
    mgr_abort_session(m, DSM_ETIMEOUT, DSM_MAX_NODES, now_ns);
    return true;
}

int mgr_poll_timeout_ms(const mgr *m, uint64_t now_ns, uint32_t timeout_ms) {
    if (m == NULL || timeout_ms == 0)
        return 1000;

    uint64_t start = 0;
    if (!earliest_wait(m, &start))
        return (int)timeout_ms;

    uint64_t limite = start + (uint64_t)timeout_ms * 1000000u;
    if (now_ns >= limite)
        return 1;              /* ya venció: despertar en seguida */
    uint64_t falta_ms = (limite - now_ns) / 1000000u;
    /* Nunca 0: con 0 el poll() no esperaría nada y el ciclo giraría en vacío
     * quemando CPU hasta que el plazo venciera de verdad. */
    if (falta_ms == 0)
        return 1;
    if (falta_ms > timeout_ms)
        return (int)timeout_ms;
    return (int)falta_ms;
}

/* ---- Despacho de tramas ------------------------------------------------ */

/* Manda SESSION_START a todos: de ahí el trabajador toma la cantidad de
 * páginas, porque la región la define el coordinador (D0.7). */
static bool start_session(mgr *m) {
    uint8_t pl[24];
    wire_put_u32(pl +  0, sess_nodes(m->s));
    wire_put_u32(pl +  4, sess_pages(m->s));
    wire_put_u32(pl +  8, DSM_PAGE_SIZE);
    wire_put_u32(pl + 12, 0u);                 /* reserved */
    wire_put_u64(pl + 16, sess_run_id(m->s));

    bool ok = true;
    for (uint32_t n = 0; n < sess_nodes(m->s); n++) {
        struct wire_header h = base(DSM_WIRE_SESSION_START, n, DSM_NO_PAGE,
                                    0u, 24u);
        if (!send_to(m, n, &h, pl))
            ok = false;
    }
    log_row(m, LOG_SESSION_START, LOG_NO_NODE, LOG_NO_REQUEST, LOG_NO_PAGE,
            (enum dsm_mode)LOG_NO_MODE, DSM_OK, LOG_NO_DURATION);
    return ok;
}

/* Rechaza una trama sin cambiar nada de estado: ERROR al emisor y fila en el
 * registro. Es el camino de todo lo que el protocolo no admite en ese
 * momento pero que no mata la sesión. */
static enum mgr_action reject(mgr *m, int ci, const struct wire_header *h,
                             enum dsm_status st, enum log_event ev) {
    queue_error(m, ci, st);
    int node = m->slot[ci].node;
    log_row(m, ev, node >= 0 ? (uint32_t)node : LOG_NO_NODE,
            h->request_id, h->page_id, (enum dsm_mode)LOG_NO_MODE, st,
            LOG_NO_DURATION);
    return MGR_KEEP;
}

static enum mgr_action on_hello(mgr *m, int ci, const struct wire_header *h,
                                const uint8_t *pl) {
    /* Una conexión manda HELLO una sola vez. */
    if (m->slot[ci].node >= 0)
        return reject(m, ci, h, DSM_ESTATE, LOG_ERROR);

    uint32_t nodes_exp = wire_get_u32(pl + 0);
    uint32_t pages_exp = wire_get_u32(pl + 4);
    uint32_t node      = h->node_id;

    /* Si ese node_id ya tiene conexión, el id está repetido: se rechaza sin
     * tocar la sesión, porque el registro del primero sigue siendo válido. */
    if (node < DSM_MAX_NODES && m->conn_of_node[node] >= 0)
        return reject(m, ci, h, DSM_ESTATE, LOG_ERROR);

    enum dsm_status st = sess_hello(m->s, node, nodes_exp, pages_exp);
    if (st != DSM_OK)
        return reject(m, ci, h, st, LOG_ERROR);

    m->slot[ci].node      = (int)node;
    m->conn_of_node[node] = ci;
    log_row(m, LOG_NODE_JOIN, node, LOG_NO_REQUEST, LOG_NO_PAGE,
            (enum dsm_mode)LOG_NO_MODE, DSM_OK, LOG_NO_DURATION);

    /* Con el último de los N, la sesión arranca. */
    if (sess_state_of(m->s) == SESS_RUNNING && !start_session(m))
        return MGR_ABORT;
    return MGR_KEEP;
}

/* Traduce un dir_status a la respuesta del coordinador. DIR_DUPLICATE no es
 * un error del protocolo: se registra y se descarta (D2.1).
 *
 * `opened` dice si fue ESTA trama la que abrió el reloj de espera del nodo
 * (lo devuelve sess_op_begin). Hace falta porque el reloj se abre antes de
 * consultar al directorio —y no puede ser después: una concesión inmediata
 * lo cierra desde dentro de dir_acquire, así que abrirlo al volver dejaría
 * abierta una espera ya respondida—. Si el directorio rechaza, hay que
 * deshacer exactamente eso: sin el cierre, mgr_check_timeouts abortaría la
 * sesión por una espera que no existe, y el registro culparía a un plazo
 * vencido en lugar de a este rechazo. */
static enum mgr_action from_dir(mgr *m, int ci, const struct wire_header *h,
                                enum dir_status ds, enum log_event ev,
                                bool opened) {
    if (ds == DIR_OK)
        return m->failed ? MGR_ABORT : MGR_KEEP;
    if (ds == DIR_DUPLICATE) {
        log_row(m, ev, (uint32_t)m->slot[ci].node, h->request_id, h->page_id,
                (enum dsm_mode)LOG_NO_MODE, DSM_OK, LOG_NO_DURATION);
        return MGR_KEEP;
    }
    if (opened)
        sess_op_end(m->s, (uint32_t)m->slot[ci].node);
    enum dsm_status st = (ds == DIR_EINVAL) ? DSM_EINVAL : DSM_ESTATE;
    return reject(m, ci, h, st, LOG_ERROR);
}

enum mgr_action mgr_dispatch(mgr *m, int ci, const struct wire_header *h,
                             const void *payload, uint64_t now_ns) {
    if (m == NULL || !slot_ok(m, ci) || !m->slot[ci].live || h == NULL)
        return MGR_ABORT;

    m->now_ns = now_ns;
    m->failed = false;
    const uint8_t *pl = payload;
    log_count_recv(m->lg, h->type, h->length);

    /* Una sesión abortada no procesa nada más: solo informa. */
    if (sess_state_of(m->s) == SESS_ABORTED)
        return reject(m, ci, h, DSM_EABORT, LOG_ERROR);

    if (h->type == DSM_WIRE_HELLO)
        return on_hello(m, ci, h, pl);

    /* De acá en adelante hace falta un nodo registrado: una trama de
     * operación antes del HELLO no tiene emisor conocido. */
    int node = m->slot[ci].node;
    if (node < 0)
        return reject(m, ci, h, DSM_ESTATE, LOG_ERROR);

    /* El node_id de la trama tiene que ser el del dueño de la conexión. Sin
     * esto, un nodo podría operar en nombre de otro y el directorio le
     * creería, porque él solo ve el número que se le pasa. */
    if (h->node_id != (uint32_t)node)
        return reject(m, ci, h, DSM_EINVAL, LOG_ERROR);

    /* Y la sesión tiene que estar corriendo: entre el HELLO propio y el
     * último ajeno no se puede operar todavía. */
    if (sess_state_of(m->s) != SESS_RUNNING && h->type != DSM_WIRE_BYE)
        return reject(m, ci, h, DSM_ESTATE, LOG_ERROR);

    switch (h->type) {
    case DSM_WIRE_ACQ_REQ: {
        enum dsm_mode mode = (h->aux == DSM_WIRE_MODE_WRITE) ? DSM_WRITE
                                                             : DSM_READ;
        uint64_t cached = wire_get_u64(pl);
        log_row(m, LOG_ACQ_REQ, (uint32_t)node, h->request_id, h->page_id,
                mode, DSM_OK, LOG_NO_DURATION);
        /* El cronómetro arranca con la solicitud, así que la duración que
         * sale en la concesión es lo que el nodo esperó de verdad. */
        bool opened = sess_op_begin(m->s, (uint32_t)node, now_ns);
        enum dir_status ds = dir_acquire(m->d, (uint32_t)node, h->page_id,
                                         mode, h->request_id, cached);
        return from_dir(m, ci, h, ds, LOG_ACQ_REQ, opened);
    }
    case DSM_WIRE_INVALIDATE_ACK: {
        log_row(m, LOG_INVALIDATE_ACK, (uint32_t)node, h->request_id,
                h->page_id, (enum dsm_mode)LOG_NO_MODE, DSM_OK,
                LOG_NO_DURATION);
        enum dir_status ds = dir_invalidate_ack(m->d, (uint32_t)node,
                                                h->page_id, h->aux,
                                                h->request_id);
        return from_dir(m, ci, h, ds, LOG_INVALIDATE_ACK, false);
    }
    case DSM_WIRE_REL_WRITE: {
        log_row(m, LOG_REL_WRITE, (uint32_t)node, h->request_id, h->page_id,
                DSM_WRITE, DSM_OK, LOG_NO_DURATION);
        bool opened = sess_op_begin(m->s, (uint32_t)node, now_ns);
        /* La página llega COMPLETA: `conn` no entrega una trama a medias, así
         * que una transferencia incompleta nunca se publica. */
        enum dir_status ds = dir_release_write(m->d, (uint32_t)node,
                                               h->page_id, pl, h->request_id);
        return from_dir(m, ci, h, ds, LOG_REL_WRITE, opened);
    }
    case DSM_WIRE_REL_READ: {
        log_row(m, LOG_REL_READ, (uint32_t)node, h->request_id, h->page_id,
                DSM_READ, DSM_OK, LOG_NO_DURATION);
        bool opened = sess_op_begin(m->s, (uint32_t)node, now_ns);
        enum dir_status ds = dir_release_read(m->d, (uint32_t)node,
                                              h->page_id, h->request_id);
        return from_dir(m, ci, h, ds, LOG_REL_READ, opened);
    }
    case DSM_WIRE_BARRIER_ARRIVE: {
        uint64_t gen = wire_get_u64(pl);
        log_row(m, LOG_BARRIER_ARRIVE, (uint32_t)node, LOG_NO_REQUEST,
                LOG_NO_PAGE, (enum dsm_mode)LOG_NO_MODE, DSM_OK,
                LOG_NO_DURATION);
        enum dsm_status why = DSM_OK;
        enum sess_barrier r = sess_barrier_arrive(m->s, (uint32_t)node,
                                                  h->aux, gen, &why);
        if (r == SESS_BARRIER_REJECT)
            return reject(m, ci, h, why, LOG_ERROR);

        /* Llegar a una barrera es esperar: si otro nodo no llega nunca, es
         * ESTA espera la que vence y delata la pérdida. Acá el reloj sí
         * puede arrancar DESPUÉS de validar, porque sess_barrier_arrive no
         * emite nada: una llegada rechazada no deja a nadie esperando. */
        if (r == SESS_BARRIER_WAIT) {
            sess_op_begin(m->s, (uint32_t)node, now_ns);
            return MGR_KEEP;
        }

        /* Completa: la liberación va a TODOS, con la generación que acaba de
         * cerrarse (no la nueva, que ya avanzó en la sesión). */
        uint8_t out[8];
        wire_put_u64(out, gen);
        for (uint32_t n = 0; n < sess_nodes(m->s); n++) {
            struct wire_header rh = base(DSM_WIRE_BARRIER_RELEASE, n,
                                         DSM_NO_PAGE, 0u, 8u);
            rh.aux = h->aux;
            if (!send_to(m, n, &rh, out))
                return MGR_ABORT;
        }
        for (uint32_t n = 0; n < sess_nodes(m->s); n++)
            sess_op_end(m->s, n);
        log_row(m, LOG_BARRIER_RELEASE, LOG_NO_NODE, LOG_NO_REQUEST,
                LOG_NO_PAGE, (enum dsm_mode)LOG_NO_MODE, DSM_OK,
                LOG_NO_DURATION);
        return MGR_KEEP;
    }
    case DSM_WIRE_BYE: {
        /* Las dos comprobaciones de D0.13, cada una en su dueño: que no haya
         * barrera esperándolo la hace la sesión, y que no tenga páginas
         * adquiridas la hace el directorio. */
        enum dir_status ds = dir_node_leave(m->d, (uint32_t)node);
        if (ds != DIR_OK) {
            reject(m, ci, h, DSM_ESTATE, LOG_ERROR);
            return MGR_ABORT;   /* se va con una página en la mano */
        }
        if (m->failed)
            return MGR_ABORT;

        enum dsm_status st = sess_bye(m->s, (uint32_t)node);
        log_row(m, LOG_NODE_LEAVE, (uint32_t)node, LOG_NO_REQUEST,
                LOG_NO_PAGE, (enum dsm_mode)LOG_NO_MODE, st,
                LOG_NO_DURATION);
        if (st != DSM_OK)
            return MGR_ABORT;   /* dejaba una barrera sin completar */

        if (sess_active(m->s) == 0) {
            sess_finish(m->s);
            log_row(m, LOG_SESSION_END, LOG_NO_NODE, LOG_NO_REQUEST,
                    LOG_NO_PAGE, (enum dsm_mode)LOG_NO_MODE, DSM_OK,
                    LOG_NO_DURATION);
        }
        return MGR_CLOSE;
    }
    default:
        /* Todo lo demás son tipos que el coordinador EMITE, no recibe. Que
         * lleguen significa que el otro lado habla otro protocolo. */
        return reject(m, ci, h, DSM_EPROTO, LOG_ERROR);
    }
}
