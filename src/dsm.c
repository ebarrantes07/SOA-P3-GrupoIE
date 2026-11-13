/* libdsm: la biblioteca que enlaza cada aplicación trabajadora (#5).
 *
 * El contrato público está en include/dsm.h y las decisiones en
 * docs/decisiones_diseno.md (D4.1 a D4.4). Las reglas del protocolo del lado
 * del trabajador —qué concesión se acepta, cuándo una invalidación es
 * legítima— viven en worker.c, que no sabe de hilos ni de sockets. Este
 * archivo pone alrededor los hilos, los candados y la red, y está escrito
 * para cumplir una regla del enunciado que conviene tener a la vista al
 * modificarlo: "ninguna espera podrá retener un bloqueo local necesario para
 * recibir la respuesta esperada".
 *
 *
 * DOS HILOS, Y QUIÉN HACE QUÉ (D4.1)
 *
 *   - El HILO DE APLICACIÓN es el que llama a la API. Valida localmente,
 *     anota en el estado qué respuesta espera, envía la solicitud y duerme en
 *     `cv` hasta que la respuesta llegue, la sesión falle o venza el plazo.
 *     No lee nunca del socket.
 *   - El HILO RECEPTOR es el único dueño del lado de lectura del socket. Lee
 *     tramas sin plazo (el coordinador puede estar legítimamente callado
 *     mientras la aplicación calcula), las aplica al estado con wk_apply,
 *     las registra y despierta al hilo de aplicación. Las invalidaciones las
 *     atiende y las confirma él solo, así que se confirman aunque la
 *     aplicación esté bloqueada esperando una concesión o una barrera.
 *
 * Todas las transiciones de la caché, y todas las filas de respuesta del
 * registro, las hace el receptor en el orden en que llegan las tramas. El
 * registro del trabajador queda así en el orden del flujo TCP, que es el
 * orden en que el coordinador las emitió.
 *
 *
 * TRES CANDADOS, Y NINGUNO SE SOSTIENE MIENTRAS SE ESPERA UNA RESPUESTA (D4.2)
 *
 *   mu       el estado compartido: la caché, la respuesta esperada y el error
 *            terminal. Intervalos cortos de cómputo, nunca durante E/S de
 *            red. El hilo de aplicación lo suelta al dormir en cv
 *            (pthread_cond_timedwait lo libera atómicamente), así que el
 *            receptor siempre puede tomarlo para entregar la respuesta.
 *   send_mu  serializa los envíos, porque frame_send exige un solo emisor por
 *            descriptor (D1.6). Lo sostiene quien envía durante UNA trama y
 *            con plazo. El receptor no lo necesita para leer, solo para
 *            mandar un INVALIDATE_ACK después de haber leído; y la escritura
 *            de la aplicación siempre progresa, porque el coordinador nunca
 *            se bloquea enviando (D0.1, D3.2) y por lo tanto sigue leyendo.
 *   log_mu   el registro y sus contadores, que escriben los dos hilos. El
 *            módulo log.c es de un solo hilo (lo usa el coordinador, D0.1);
 *            este candado es lo que lo vuelve utilizable acá.
 *
 * Nunca se toman dos a la vez: sin anidamiento no hay orden de candados que
 * respetar ni interbloqueo posible entre ellos.
 *
 *
 * ERRORES TERMINALES (D4.3)
 *
 * `fatal` es el primer error de sesión. Una vez puesto no cambia, despierta a
 * quien espere y se hace shutdown del socket: el coordinador ve la caída y
 * aborta la sesión para los demás, y el receptor sale de su lectura. Desde
 * ahí toda llamada devuelve ese código y solo queda DSM_finalize (dsm.h).
 */
#include "dsm.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "clock.h"
#include "frame.h"
#include "io.h"
#include "log.h"
#include "net.h"
#include "wire.h"
#include "worker.h"

/* Nombre del componente en la columna `component` del registro. El
 * coordinador escribe "coordinador"; el nodo se distingue por `node_id`. */
#define WORKER_COMPONENT "trabajador"

/* Generación por barrier_id (D0.8). El enunciado no acota los barrier_id,
 * así que es una lista que crece; las aplicaciones usan uno o dos. */
struct barrier_gen {
    uint32_t id;
    uint64_t gen;
};

struct dsm_ctx {
    /* Fijos desde que DSM_init retorna: los dos hilos los leen sin candado
     * porque nadie los escribe después de crear el receptor. */
    int       fd;
    uint32_t  nodes;
    uint32_t  nid;
    uint32_t  timeout_ms;

    /* Solo del hilo de aplicación. */
    enum dsm_status     last_err;
    uint64_t            next_request;   /* D2.1: estrictamente creciente */
    int64_t             held_page;      /* -1 si no hay página adquirida */
    enum dsm_mode       held_mode;
    uint64_t            held_request;
    struct barrier_gen *gens;
    size_t              ngens;
    size_t              capgens;
    pthread_t           rx;
    bool                rx_started;

    /* Protegidos por mu. El marco de una página (w.frames) es la excepción:
     * lo escribe el receptor solo al aplicar la concesión de esa página, y
     * después lo usa la aplicación sin candado hasta el release. El paso de
     * uno a otro ocurre a través de mu, que es lo que lo ordena. */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    struct wk_state w;
    enum dsm_status fatal;        /* DSM_OK mientras la sesión esté sana */
    bool            closing;      /* DSM_finalize empezó la despedida */
    bool            rx_exited;

    pthread_mutex_t send_mu;

    pthread_mutex_t log_mu;
    dsm_log        *lg;          /* NULL si no se pidió registro */
};

/* ---- Textos ------------------------------------------------------------ */

const char *DSM_strerror(enum dsm_status s) {
    switch (s) {
    case DSM_OK:       return "sin error";
    case DSM_EINVAL:   return "argumento inválido";
    case DSM_ESTATE:   return "operación no válida en el estado actual";
    case DSM_ETIMEOUT: return "venció el timeout de operación";
    case DSM_ECONN:    return "se perdió la conexión con el coordinador";
    case DSM_EPROTO:   return "trama inválida o incoherente con el protocolo";
    case DSM_EABORT:   return "el coordinador abortó la sesión";
    case DSM_ENOMEM:   return "memoria insuficiente";
    case DSM_EIO:      return "error al escribir el registro local";
    }
    return "error desconocido";
}

static enum dsm_status status_from_frame(struct frame_io r) {
    if (r.io == IO_TIMEOUT)
        return DSM_ETIMEOUT;
    if (r.io != IO_OK)
        return DSM_ECONN;
    return DSM_EPROTO;
}

/* ---- Registro ----------------------------------------------------------
 *
 * Las filas tienen el formato del coordinador (D3.1), escritas por el mismo
 * módulo, así que una sola herramienta lee los dos registros. Las columnas de
 * estado del directorio van vacías salvo `version`, que el trabajador sí
 * conoce (D4.4). Nunca se llama con mu tomado. */

struct row {
    enum log_event  event;
    uint64_t        request;    /* LOG_NO_REQUEST si no aplica */
    uint32_t        page;       /* DSM_NO_PAGE si no aplica */
    enum dsm_mode   mode;       /* LOG_NO_MODE si no aplica */
    uint64_t        version;    /* DSM_NO_VERSION si no aplica */
    enum dsm_status status;
    uint64_t        dur_ns;     /* LOG_NO_DURATION si no aplica */
};

static struct row row_of(enum log_event ev, uint64_t request, uint32_t page) {
    struct row r = { ev, request, page, (enum dsm_mode)LOG_NO_MODE,
                     DSM_NO_VERSION, DSM_OK, LOG_NO_DURATION };
    return r;
}

static void log_put(dsm_ctx *c, const struct row *r) {
    struct log_row lr = log_row_empty(r->event);

    lr.node_id = c->nid;
    lr.request_id = r->request;
    lr.page_id = r->page == DSM_NO_PAGE ? LOG_NO_PAGE : r->page;
    lr.mode = r->mode;
    lr.status = r->status;
    lr.dur_ns = r->dur_ns;
    lr.has_version = r->version != DSM_NO_VERSION;
    lr.version = r->version;

    pthread_mutex_lock(&c->log_mu);
    log_write(c->lg, &lr);
    pthread_mutex_unlock(&c->log_mu);
}

static void count_sent(dsm_ctx *c, const struct wire_header *h) {
    pthread_mutex_lock(&c->log_mu);
    log_count_sent(c->lg, h->type, h->length);
    pthread_mutex_unlock(&c->log_mu);
}

static void count_recv(dsm_ctx *c, const struct wire_header *h) {
    pthread_mutex_lock(&c->log_mu);
    log_count_recv(c->lg, h->type, h->length);
    pthread_mutex_unlock(&c->log_mu);
}

static uint64_t since(uint64_t t0_ns) {
    return now_monotonic_ns() - t0_ns;
}

/* ---- Envío -------------------------------------------------------------
 *
 * El único camino de escritura al socket, de los dos hilos. Completa los
 * campos comunes (versión, y node_id, que en las tramas del trabajador es
 * siempre el propio) y serializa con send_mu. */
static enum dsm_status send_frame(dsm_ctx *c, struct wire_header *h,
                                  const void *payload, uint64_t deadline_ns) {
    struct frame_io r;

    h->version = DSM_WIRE_VERSION;
    h->node_id = c->nid;

    pthread_mutex_lock(&c->send_mu);
    r = frame_send(c->fd, h, payload, deadline_ns);
    pthread_mutex_unlock(&c->send_mu);

    if (!frame_ok(r))
        return status_from_frame(r);
    count_sent(c, h);
    return DSM_OK;
}

static struct wire_header header(uint8_t type, uint32_t page, uint64_t request,
                                 uint32_t aux, uint32_t length) {
    struct wire_header h;

    memset(&h, 0, sizeof h);
    h.type = type;
    h.page_id = page;
    h.request_id = request;
    h.aux = aux;
    h.length = length;
    return h;
}

/* ---- Error terminal ---------------------------------------------------- */

/* Registra el primer error de sesión, despierta a quien espere y corta el
 * socket. El shutdown hace dos cosas: el coordinador ve la caída y aborta la
 * sesión para los demás (enunciado, comunicación 6), y el receptor sale de su
 * lectura sin plazo. Los errores siguientes no pisan al primero, que es el
 * que explica qué pasó. */
static void fail(dsm_ctx *c, enum dsm_status st) {
    bool first;

    pthread_mutex_lock(&c->mu);
    first = c->fatal == DSM_OK;
    if (first)
        c->fatal = st;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);

    if (first)
        (void)shutdown(c->fd, SHUT_RDWR);
}

static enum dsm_status current_fatal(dsm_ctx *c) {
    enum dsm_status st;

    pthread_mutex_lock(&c->mu);
    st = c->fatal;
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* ---- Espera del hilo de aplicación --------------------------------------
 *
 * Predicado: la respuesta anotada llegó (w.wait.done), o la sesión falló
 * (fatal != DSM_OK). Se reevalúa en cada despertar, así que los despertares
 * espurios no tienen efecto. El plazo es absoluto en el reloj monotónico,
 * igual que en io.h (D1.1): cv se crea con CLOCK_MONOTONIC para que un ajuste
 * del reloj de pared no lo acorte ni lo alargue.
 *
 * Durante la espera mu está LIBERADO y esta función no tiene ningún otro
 * candado: el receptor puede entregar la respuesta, y atender
 * invalidaciones, todo el tiempo que dure. */
static void to_timespec(uint64_t ns, struct timespec *ts) {
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

static enum dsm_status wait_reply(dsm_ctx *c, uint64_t deadline_ns) {
    struct timespec ts;
    enum dsm_status st = DSM_OK;
    bool timed_out = false;

    to_timespec(deadline_ns, &ts);

    pthread_mutex_lock(&c->mu);
    while (!c->w.wait.done && c->fatal == DSM_OK) {
        if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) == ETIMEDOUT &&
            !c->w.wait.done && c->fatal == DSM_OK) {
            timed_out = true;
            break;
        }
    }
    if (!timed_out && !c->w.wait.done)
        st = c->fatal;
    c->w.wait.kind = WK_NONE;
    pthread_mutex_unlock(&c->mu);

    if (timed_out) {
        fail(c, DSM_ETIMEOUT);
        st = current_fatal(c);   /* el receptor pudo haber fallado antes */
    }
    return st;
}

/* Anota la respuesta esperada. Va ANTES de enviar: si la respuesta llegara
 * antes de anotarla, el receptor la tomaría por inesperada. */
static void expect(dsm_ctx *c, const struct wk_wait *wait) {
    pthread_mutex_lock(&c->mu);
    wk_expect(&c->w, wait);
    pthread_mutex_unlock(&c->mu);
}

/* Envía la solicitud anotada y espera su respuesta. Un fallo al enviar es
 * terminal: una trama a medio escribir deja el flujo sin arreglo (frame.h). */
static enum dsm_status request(dsm_ctx *c, struct wire_header *h,
                               const void *payload, uint64_t deadline_ns) {
    enum dsm_status st = send_frame(c, h, payload, deadline_ns);

    if (st != DSM_OK) {
        pthread_mutex_lock(&c->mu);
        c->w.wait.kind = WK_NONE;
        pthread_mutex_unlock(&c->mu);
        fail(c, st);
        return current_fatal(c);
    }
    return wait_reply(c, deadline_ns);
}

/* Fila de una operación que falló. Un plazo vencido va como TIMEOUT, que es
 * el evento que el coordinador usa para lo mismo (D3.5). */
static void log_failure(dsm_ctx *c, uint64_t request, uint32_t page,
                        enum dsm_status st) {
    struct row r = row_of(st == DSM_ETIMEOUT ? LOG_TIMEOUT : LOG_ERROR,
                          request, page);
    r.status = st;
    log_put(c, &r);
}

/* ---- Hilo receptor ------------------------------------------------------ */

static void *receiver_main(void *arg) {
    dsm_ctx *c = arg;
    uint8_t payload[DSM_MAX_PAYLOAD];   /* 4104 bytes, la trama más grande */
    enum dsm_status st = DSM_OK;
    bool error_frame = false;

    while (st == DSM_OK) {
        struct wire_header h;
        struct wk_effect e;
        struct frame_io r;
        uint64_t t0 = 0;
        enum dsm_mode mode = (enum dsm_mode)LOG_NO_MODE;

        /* Sin plazo: esperar al coordinador es el trabajo de este hilo. Los
         * plazos los aplica quien espera una respuesta (wait_reply), y la
         * salida de esta lectura la provoca un shutdown del socket. */
        r = frame_recv(c->fd, &h, payload, DSM_MAX_PAYLOAD, 0u);
        if (!frame_ok(r)) {
            st = status_from_frame(r);
            break;
        }
        count_recv(c, &h);

        pthread_mutex_lock(&c->mu);
        t0 = c->w.wait.t0_ns;
        if (c->w.wait.kind == WK_GRANT)
            mode = c->w.wait.mode;
        else if (c->w.wait.kind == WK_REL_WRITE)
            mode = DSM_WRITE;
        else if (c->w.wait.kind == WK_REL_READ)
            mode = DSM_READ;
        st = wk_apply(&c->w, &h, payload, &e);
        if (st == DSM_OK && e.completed)
            pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);

        if (h.type == DSM_WIRE_ERROR) {
            /* Un rechazo repite el request_id y la página de la trama que
             * rechaza (D3.7), así que la fila se empareja con la del
             * coordinador por (node_id, request_id). Un ERROR que no responde
             * a ninguna trama —aborto, plazo— trae request_id 0. */
            struct row er = row_of(LOG_ERROR,
                                   h.request_id != 0u ? h.request_id
                                                      : LOG_NO_REQUEST,
                                   h.page_id);
            er.status = st;
            log_put(c, &er);
            error_frame = true;
            break;
        }
        if (st != DSM_OK) {
            log_failure(c, h.request_id, h.page_id, st);
            break;
        }

        /* La fila de la respuesta la escribe este hilo, en el orden del
         * flujo; la duración es desde que la aplicación anotó la espera. */
        {
            struct row rr = row_of(e.event,
                                   h.type == DSM_WIRE_BARRIER_RELEASE
                                       ? LOG_NO_REQUEST : h.request_id,
                                   h.page_id);
            if (e.completed) {
                rr.mode = mode;
                rr.version = e.version;
                rr.dur_ns = since(t0);
            }
            log_put(c, &rr);
        }

        if (e.ack) {
            struct wire_header a = header(DSM_WIRE_INVALIDATE_ACK, h.page_id,
                                          h.request_id, h.aux, 0u);

            st = send_frame(c, &a, NULL, io_deadline_in(c->timeout_ms));
            if (st == DSM_OK) {
                struct row ar = row_of(LOG_INVALIDATE_ACK, h.request_id, h.page_id);
                log_put(c, &ar);
            }
        }
    }

    /* Lo que hizo salir a este hilo es el error terminal de la sesión, salvo
     * que la despedida esté en curso: ahí el cierre del coordinador es lo
     * esperado. Un ERROR cuenta siempre, también durante la despedida —por
     * ejemplo, un BYE que deja una barrera sin completar (D0.13)—, para que
     * DSM_finalize no informe un cierre limpio que no lo fue. */
    pthread_mutex_lock(&c->mu);
    bool closing = c->closing;
    pthread_mutex_unlock(&c->mu);
    if (error_frame || !closing)
        fail(c, st == DSM_OK ? DSM_ECONN : st);

    pthread_mutex_lock(&c->mu);
    c->rx_exited = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* ---- Creación y destrucción del contexto -------------------------------- */

static void ctx_free(dsm_ctx *c) {
    if (c == NULL)
        return;
    if (c->fd >= 0)
        net_close(c->fd);
    (void)log_close(c->lg);
    wk_free(&c->w);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->send_mu);
    pthread_mutex_destroy(&c->log_mu);
    free(c->gens);
    free(c);
}

static dsm_ctx *ctx_alloc(uint32_t nodes, uint32_t nid, uint32_t timeout_ms) {
    dsm_ctx *c = calloc(1, sizeof *c);
    pthread_condattr_t ca;

    if (c == NULL)
        return NULL;
    c->fd = -1;
    c->nodes = nodes;
    c->nid = nid;
    c->timeout_ms = timeout_ms;
    c->held_page = -1;
    c->next_request = 1u;      /* 0 queda para HELLO */

    if (pthread_condattr_init(&ca) != 0) {
        free(c);
        return NULL;
    }
    if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&c->cv, &ca) != 0) {
        pthread_condattr_destroy(&ca);
        free(c);
        return NULL;
    }
    pthread_condattr_destroy(&ca);
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->send_mu, NULL);
    pthread_mutex_init(&c->log_mu, NULL);
    return c;
}

/* Recibe SESSION_START durante DSM_init, cuando todavía no hay receptor y
 * este hilo es el único que lee. Un ERROR del coordinador (id repetido,
 * sesión ya iniciada, configuración incompatible) se devuelve con su código.
 * En éxito deja la cantidad de páginas y el run_id. */
static enum dsm_status await_session(dsm_ctx *c, uint64_t deadline_ns,
                                     uint32_t *pages, uint64_t *run_id) {
    struct wire_header h;
    uint8_t payload[DSM_MAX_PAYLOAD];
    struct frame_io r = frame_recv(c->fd, &h, payload, sizeof payload, deadline_ns);
    uint32_t nodes, page_size;

    if (!frame_ok(r))
        return status_from_frame(r);

    if (h.type == DSM_WIRE_ERROR)
        return wk_status_from_wire(h.aux);
    if (h.type != DSM_WIRE_SESSION_START || h.node_id != c->nid)
        return DSM_EPROTO;

    nodes     = wire_get_u32(payload + 0);
    *pages    = wire_get_u32(payload + 4);
    page_size = wire_get_u32(payload + 8);
    *run_id   = wire_get_u64(payload + 16);
    if (nodes != c->nodes || page_size != DSM_PAGE_SIZE ||
        *pages < DSM_MIN_PAGES || *pages > DSM_MAX_PAGES)
        return DSM_EPROTO;
    return DSM_OK;
}

/* El registro se abre recién con SESSION_START, porque cada fila lleva el
 * run_id que manda el coordinador. Para no descubrir una ruta inválida
 * después de haberse registrado —lo que abortaría la sesión de todos— se
 * prueba antes de conectar. */
static bool log_path_writable(const char *path) {
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return false;
    return fclose(f) == 0;
}

dsm_ctx *DSM_init(const char *host, uint16_t port, uint32_t nodes, uint32_t nid,
                  const struct dsm_options *opts, enum dsm_status *err) {
    uint32_t timeout_ms = DSM_DEFAULT_TIMEOUT_MS;
    const char *log_path = NULL;
    enum dsm_status st = DSM_OK;
    dsm_ctx *c = NULL;
    uint64_t deadline, run_id = 0;
    uint32_t pages = 0;
    struct wire_header h;
    uint8_t hello[8];

    if (opts != NULL) {
        if (opts->timeout_ms != 0u)
            timeout_ms = opts->timeout_ms;
        log_path = opts->log_path;
    }

    /* Validación local, antes de abrir nada. */
    if (host == NULL || port == 0u || nodes < DSM_MIN_NODES ||
        nodes > DSM_MAX_NODES || nid >= nodes) {
        st = DSM_EINVAL;
        goto out;
    }
    if (log_path != NULL && !log_path_writable(log_path)) {
        st = DSM_EIO;
        goto out;
    }

    c = ctx_alloc(nodes, nid, timeout_ms);
    if (c == NULL) {
        st = DSM_ENOMEM;
        goto out;
    }

    /* Un solo plazo para todo el arranque: conectar, presentarse y esperar a
     * que el coordinador tenga los N nodos. */
    deadline = io_deadline_in(timeout_ms);

    switch (net_connect(host, port, &c->fd, deadline)) {
    case NET_OK:      break;
    case NET_TIMEOUT: st = DSM_ETIMEOUT; goto out;
    case NET_ADDR:    st = DSM_EINVAL;   goto out;
    default:          st = DSM_ECONN;    goto out;
    }

    /* pages_expected = 0: la región la define el coordinador (D0.7). */
    wire_put_u32(hello + 0, nodes);
    wire_put_u32(hello + 4, 0u);
    h = header(DSM_WIRE_HELLO, DSM_NO_PAGE, 0u, 0u, sizeof hello);
    st = send_frame(c, &h, hello, deadline);
    if (st == DSM_OK)
        st = await_session(c, deadline, &pages, &run_id);
    if (st != DSM_OK)
        goto out;

    if (!wk_init(&c->w, nid, pages)) {
        st = DSM_ENOMEM;
        goto out;
    }
    if (log_path != NULL) {
        c->lg = log_open(log_path, WORKER_COMPONENT, run_id);
        if (c->lg == NULL) {
            st = DSM_EIO;
            goto out;
        }
        /* Las dos tramas del arranque cruzaron antes de abrir el registro. */
        log_count_sent(c->lg, DSM_WIRE_HELLO, sizeof hello);
        log_count_recv(c->lg, DSM_WIRE_SESSION_START, 24u);
    }

    if (pthread_create(&c->rx, NULL, receiver_main, c) != 0) {
        st = DSM_ENOMEM;
        goto out;
    }
    c->rx_started = true;

    {
        struct row r = row_of(LOG_SESSION_START, LOG_NO_REQUEST, DSM_NO_PAGE);
        log_put(c, &r);
    }

out:
    if (st != DSM_OK) {
        ctx_free(c);     /* sin receptor todavía: nada que esperar */
        c = NULL;
    }
    if (err != NULL)
        *err = st;
    return c;
}

uint32_t DSM_page_count(const dsm_ctx *ctx) {
    return ctx != NULL ? ctx->w.pages : 0u;
}

enum dsm_status DSM_last_error(const dsm_ctx *ctx) {
    return ctx != NULL ? ctx->last_err : DSM_EINVAL;
}

/* ---- Operaciones -------------------------------------------------------- */

void *DSM_acquire(dsm_ctx *ctx, uint32_t page_id, enum dsm_mode mode) {
    dsm_ctx *c = ctx;
    enum dsm_status st;
    struct wk_wait wait;
    struct wire_header h;
    uint8_t body[8];
    uint64_t cached;

    if (c == NULL)
        return NULL;
    c->last_err = DSM_OK;

    /* Un error terminal gana a todo, porque dsm.h promete que tras él toda
     * llamada falla con el mismo código. Después vienen los rechazos locales,
     * que no envían nada (criterio de #5). */
    st = current_fatal(c);
    if (st == DSM_OK && (page_id >= c->w.pages ||
                         (mode != DSM_READ && mode != DSM_WRITE)))
        st = DSM_EINVAL;
    if (st == DSM_OK && c->held_page >= 0)
        st = DSM_ESTATE;      /* adquisición anidada */
    if (st != DSM_OK) {
        c->last_err = st;
        return NULL;
    }

    memset(&wait, 0, sizeof wait);
    wait.kind = WK_GRANT;
    wait.page = page_id;
    wait.request = c->next_request++;
    wait.mode = mode;
    wait.t0_ns = now_monotonic_ns();

    pthread_mutex_lock(&c->mu);
    cached = wk_offer(&c->w, page_id);
    wk_expect(&c->w, &wait);
    pthread_mutex_unlock(&c->mu);

    /* Todo acquire consulta al coordinador, con copia o sin ella: la copia
     * solo le ahorra a la concesión el envío de la página. */
    {
        struct row r = row_of(LOG_ACQ_REQ, wait.request, page_id);
        r.mode = mode;
        r.version = cached;
        log_put(c, &r);
    }
    wire_put_u64(body, cached);
    h = header(DSM_WIRE_ACQ_REQ, page_id, wait.request, (uint32_t)mode, sizeof body);
    st = request(c, &h, body, io_deadline_in(c->timeout_ms));
    if (st != DSM_OK) {
        c->last_err = st;
        log_failure(c, wait.request, page_id, st);
        return NULL;
    }

    c->held_page = page_id;
    c->held_mode = mode;
    c->held_request = wait.request;
    return wk_frame(&c->w, page_id);
}

int DSM_release(dsm_ctx *ctx, uint32_t page_id) {
    dsm_ctx *c = ctx;
    enum dsm_status st;
    struct wk_wait wait;
    struct wire_header h;

    if (c == NULL)
        return -1;
    c->last_err = DSM_OK;

    if (page_id >= c->w.pages) {
        c->last_err = DSM_EINVAL;
        return -1;
    }
    if (c->held_page < 0 || (uint32_t)c->held_page != page_id) {
        /* Release sin acquire, o de otra página. Con la sesión caída se
         * informa el error terminal, que es el que explica el estado. */
        st = current_fatal(c);
        c->last_err = st != DSM_OK ? st : DSM_ESTATE;
        return -1;
    }

    /* Desde acá la página deja de estar adquirida para la biblioteca, salga
     * bien o mal el release: si falla, la sesión queda en error terminal y lo
     * único que queda es DSM_finalize, que no debe tropezar con una página
     * que ya no se puede devolver (D4.3). */
    c->held_page = -1;

    st = current_fatal(c);
    if (st != DSM_OK) {
        c->last_err = st;
        return -1;
    }

    memset(&wait, 0, sizeof wait);
    wait.kind = (c->held_mode == DSM_WRITE) ? WK_REL_WRITE : WK_REL_READ;
    wait.page = page_id;
    wait.request = c->held_request;     /* el del acquire (D2.1) */
    wait.t0_ns = now_monotonic_ns();
    expect(c, &wait);

    {
        struct row r = row_of(c->held_mode == DSM_WRITE ? LOG_REL_WRITE
                                                        : LOG_REL_READ,
                              wait.request, page_id);
        r.mode = c->held_mode;
        log_put(c, &r);
    }

    /* En WRITE viaja la página entera: el coordinador publica bytes, no
     * diferencias. Leer el marco sin mu es seguro porque el receptor solo
     * escribe un marco al aplicar la concesión de esa página, y ahora lo que
     * se espera es la confirmación del release. */
    if (c->held_mode == DSM_WRITE) {
        h = header(DSM_WIRE_REL_WRITE, page_id, wait.request, 0u, DSM_PAGE_SIZE);
        st = request(c, &h, wk_frame(&c->w, page_id), io_deadline_in(c->timeout_ms));
    } else {
        h = header(DSM_WIRE_REL_READ, page_id, wait.request, 0u, 0u);
        st = request(c, &h, NULL, io_deadline_in(c->timeout_ms));
    }
    if (st != DSM_OK) {
        c->last_err = st;
        log_failure(c, wait.request, page_id, st);
        return -1;
    }
    return 0;
}

/* Generación actual de un barrier_id, creándola en 0 la primera vez. NULL
 * solo si no hay memoria. */
static struct barrier_gen *barrier_slot(dsm_ctx *c, uint32_t id) {
    for (size_t i = 0; i < c->ngens; i++) {
        if (c->gens[i].id == id)
            return &c->gens[i];
    }
    if (c->ngens == c->capgens) {
        size_t cap = c->capgens != 0u ? c->capgens * 2u : 4u;
        struct barrier_gen *g = realloc(c->gens, cap * sizeof *g);
        if (g == NULL)
            return NULL;
        c->gens = g;
        c->capgens = cap;
    }
    c->gens[c->ngens].id = id;
    c->gens[c->ngens].gen = 0u;
    return &c->gens[c->ngens++];
}

int DSM_barrier(dsm_ctx *ctx, uint32_t barrier_id) {
    dsm_ctx *c = ctx;
    struct barrier_gen *b;
    enum dsm_status st;
    struct wk_wait wait;
    struct wire_header h;
    uint8_t body[8];

    if (c == NULL)
        return -1;
    c->last_err = DSM_OK;

    st = current_fatal(c);
    if (st == DSM_OK && c->held_page >= 0)
        st = DSM_ESTATE;      /* barrera con una página adquirida */
    if (st != DSM_OK) {
        c->last_err = st;
        return -1;
    }

    b = barrier_slot(c, barrier_id);
    if (b == NULL) {
        c->last_err = DSM_ENOMEM;
        return -1;
    }

    memset(&wait, 0, sizeof wait);
    wait.kind = WK_BARRIER;
    wait.page = DSM_NO_PAGE;
    wait.request = c->next_request++;
    wait.barrier = barrier_id;
    wait.gen = b->gen;
    wait.t0_ns = now_monotonic_ns();
    expect(c, &wait);

    {
        struct row r = row_of(LOG_BARRIER_ARRIVE, wait.request, DSM_NO_PAGE);
        log_put(c, &r);
    }
    wire_put_u64(body, b->gen);
    h = header(DSM_WIRE_BARRIER_ARRIVE, DSM_NO_PAGE, wait.request, barrier_id,
               sizeof body);
    st = request(c, &h, body, io_deadline_in(c->timeout_ms));
    if (st != DSM_OK) {
        c->last_err = st;
        log_failure(c, wait.request, DSM_NO_PAGE, st);
        return -1;
    }

    /* La generación avanza solo cuando la barrera se completó: si la sesión
     * cae a mitad, ya no habrá otra llegada que numerar. */
    b->gen++;
    return 0;
}

/* Espera a que el receptor salga, hasta el plazo. false si venció. */
static bool wait_rx_exit(dsm_ctx *c, uint64_t deadline_ns) {
    struct timespec ts;
    bool exited;

    to_timespec(deadline_ns, &ts);
    pthread_mutex_lock(&c->mu);
    while (!c->rx_exited) {
        if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) == ETIMEDOUT)
            break;
    }
    exited = c->rx_exited;
    pthread_mutex_unlock(&c->mu);
    return exited;
}

int DSM_finalize(dsm_ctx *ctx) {
    dsm_ctx *c = ctx;
    bool clean;

    if (c == NULL)
        return -1;

    /* Única salida que deja el contexto vivo (D0.10). */
    if (c->held_page >= 0) {
        c->last_err = DSM_ESTATE;
        return -1;
    }

    pthread_mutex_lock(&c->mu);
    clean = c->fatal == DSM_OK;
    c->closing = true;
    pthread_mutex_unlock(&c->mu);

    if (clean) {
        /* Despedida ordenada (D0.13): BYE, y después cerrar solo el lado de
         * escritura. El coordinador cierra su conexión al procesar el BYE y
         * el receptor lo ve como fin de flujo. Se lo espera hasta el plazo en
         * vez de cortar de inmediato, para no descartar lo que todavía esté
         * en camino; si no cierra a tiempo, se corta igual y el cierre no
         * cuenta como limpio. */
        struct wire_header h = header(DSM_WIRE_BYE, DSM_NO_PAGE,
                                      c->next_request++, 0u, 0u);
        uint64_t deadline = io_deadline_in(c->timeout_ms);
        struct row r = row_of(LOG_NODE_LEAVE, h.request_id, DSM_NO_PAGE);

        log_put(c, &r);
        if (send_frame(c, &h, NULL, deadline) == DSM_OK) {
            (void)shutdown(c->fd, SHUT_WR);
            clean = wait_rx_exit(c, deadline);
        } else {
            clean = false;
        }
    }

    /* Desde acá el receptor sale sí o sí: con el socket cortado en los dos
     * sentidos, su lectura sin plazo termina. Se lo espera antes de liberar
     * nada, porque usa el contexto hasta su última instrucción. */
    (void)shutdown(c->fd, SHUT_RDWR);
    if (c->rx_started)
        (void)pthread_join(c->rx, NULL);

    /* Un ERROR durante la despedida, o cualquier error anterior, deja el
     * cierre como no limpio. */
    {
        enum dsm_status st = current_fatal(c);
        struct row r = row_of(LOG_SESSION_END, LOG_NO_REQUEST, DSM_NO_PAGE);

        if (st != DSM_OK)
            clean = false;
        r.status = st;
        log_put(c, &r);
    }
    if (c->lg != NULL) {
        if (!log_close(c->lg))
            clean = false;
        c->lg = NULL;
    }
    ctx_free(c);
    return clean ? 0 : -1;
}
