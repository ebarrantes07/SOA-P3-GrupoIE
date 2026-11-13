/* Registro de nodos y barreras de la sesión. Ver include/session.h. */
#include "session.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

enum node_slot {
    NODE_EMPTY = 0,
    NODE_REGISTERED,
    NODE_LEFT
};

struct barrier {
    bool     used;
    uint32_t id;
    uint64_t generation;   /* la ABIERTA, no la última liberada */
    uint8_t  arrived;      /* máscara de nodos que ya llegaron a ella */
};

struct dsm_session {
    uint32_t nodes;
    uint32_t pages;
    uint64_t run_id;
    enum sess_state state;

    enum node_slot slot[DSM_MAX_NODES];
    bool           op_open[DSM_MAX_NODES];
    uint64_t       op_start[DSM_MAX_NODES];

    /* Barrera en la que cada nodo está esperando. Un nodo tiene a lo sumo
     * una, igual que tiene a lo sumo un acceso en el directorio: después de
     * llegar queda bloqueado hasta la liberación, así que una llegada a otra
     * barrera solo puede venir de un trabajador desincronizado. */
    bool           in_barrier[DSM_MAX_NODES];
    uint32_t       waiting_on[DSM_MAX_NODES];

    struct barrier barrier[SESS_MAX_BARRIERS];
};

static uint8_t bit(uint32_t n) {
    return (uint8_t)(1u << n);
}

sess *sess_create(uint32_t nodes, uint32_t pages, uint64_t run_id) {
    if (nodes < DSM_MIN_NODES || nodes > DSM_MAX_NODES)
        return NULL;
    if (pages < DSM_MIN_PAGES || pages > DSM_MAX_PAGES)
        return NULL;

    sess *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->nodes  = nodes;
    s->pages  = pages;
    s->run_id = run_id;
    s->state  = SESS_REGISTERING;
    return s;
}

void sess_destroy(sess *s) {
    free(s);
}

uint32_t sess_nodes(const sess *s)  { return s == NULL ? 0 : s->nodes; }
uint32_t sess_pages(const sess *s)  { return s == NULL ? 0 : s->pages; }
uint64_t sess_run_id(const sess *s) { return s == NULL ? 0 : s->run_id; }

enum sess_state sess_state_of(const sess *s) {
    return s == NULL ? SESS_ABORTED : s->state;
}

/* ---- Registro ---------------------------------------------------------- */

enum dsm_status sess_hello(sess *s, uint32_t node, uint32_t nodes_expected,
                           uint32_t pages_expected) {
    if (s == NULL)
        return DSM_EINVAL;
    if (s->state == SESS_ABORTED)
        return DSM_EABORT;

    /* Incorporación tardía: la sesión ya arrancó o ya terminó. Se distingue
     * de un id inválido porque el nodo puede ser perfectamente válido y
     * llegar tarde, y el registro tiene que decir cuál de las dos fue. */
    if (s->state != SESS_REGISTERING)
        return DSM_ESTATE;

    if (node >= s->nodes)
        return DSM_EINVAL;
    if (nodes_expected != s->nodes)
        return DSM_EINVAL;
    /* 0 = "sin opinión", que es lo que manda la biblioteca (D0.7). */
    if (pages_expected != 0u && pages_expected != s->pages)
        return DSM_EINVAL;

    if (s->slot[node] == NODE_REGISTERED)
        return DSM_ESTATE;   /* id repetido */
    if (s->slot[node] == NODE_LEFT)
        return DSM_ESTATE;   /* no se vuelve a entrar */

    s->slot[node] = NODE_REGISTERED;

    /* Con el último de los N, la sesión arranca. */
    if (sess_active(s) == s->nodes)
        s->state = SESS_RUNNING;
    return DSM_OK;
}

bool sess_registered(const sess *s, uint32_t node) {
    if (s == NULL || node >= s->nodes)
        return false;
    return s->slot[node] == NODE_REGISTERED;
}

bool sess_left(const sess *s, uint32_t node) {
    if (s == NULL || node >= s->nodes)
        return false;
    return s->slot[node] == NODE_LEFT;
}

uint32_t sess_active(const sess *s) {
    if (s == NULL)
        return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->nodes; i++)
        if (s->slot[i] == NODE_REGISTERED)
            n++;
    return n;
}

enum dsm_status sess_bye(sess *s, uint32_t node) {
    if (s == NULL)
        return DSM_EINVAL;
    if (s->state == SESS_ABORTED)
        return DSM_EABORT;
    if (node >= s->nodes || s->slot[node] != NODE_REGISTERED)
        return DSM_ESTATE;

    /* Despedirse durante el registro deja la sesión sin poder alcanzar los N
     * nunca: no es una salida ordenada, es un participante perdido antes de
     * empezar. Se aborta acá en vez de esperar a que lo descubra el plazo,
     * porque el motivo ya se conoce y el plazo solo diría "nadie llegó". */
    if (s->state == SESS_REGISTERING) {
        s->state = SESS_ABORTED;
        return DSM_EABORT;
    }

    /* Si alguien está esperando una barrera, esa barrera necesita a TODOS los
     * nodos para liberarse, incluido este. Irse ahora dejaría a los demás
     * esperando para siempre, así que se aborta: la política del enunciado
     * ante la pérdida de un participante es abortar, no seguir con menos
     * (D0.13). */
    if (sess_barrier_pending(s)) {
        s->state = SESS_ABORTED;
        return DSM_EABORT;
    }

    s->slot[node]       = NODE_LEFT;
    s->op_open[node]    = false;
    s->in_barrier[node] = false;
    return DSM_OK;
}

void sess_abort(sess *s) {
    if (s != NULL)
        s->state = SESS_ABORTED;
}

void sess_finish(sess *s) {
    if (s == NULL || s->state == SESS_ABORTED)
        return;
    if (sess_active(s) == 0)
        s->state = SESS_FINISHED;
}

/* ---- Tiempo por nodo --------------------------------------------------- */

void sess_op_begin(sess *s, uint32_t node, uint64_t now_ns) {
    if (s == NULL || node >= s->nodes)
        return;
    s->op_open[node]  = true;
    s->op_start[node] = now_ns;
}

uint64_t sess_op_elapsed(const sess *s, uint32_t node, uint64_t now_ns) {
    if (s == NULL || node >= s->nodes || !s->op_open[node])
        return LOG_NO_DURATION;
    /* El reloj es monotónico, así que no debería retroceder; si lo hiciera,
     * se informa 0 en vez de un número enorme por el desborde del unsigned. */
    return now_ns < s->op_start[node] ? 0u : now_ns - s->op_start[node];
}

void sess_op_end(sess *s, uint32_t node) {
    if (s == NULL || node >= s->nodes)
        return;
    s->op_open[node] = false;
}

bool sess_op_oldest(const sess *s, uint32_t *out_node,
                    uint64_t *out_start_ns) {
    if (s == NULL)
        return false;
    bool hay = false;
    uint32_t nodo = 0;
    uint64_t min = 0;
    for (uint32_t i = 0; i < s->nodes; i++) {
        if (!s->op_open[i] || s->slot[i] != NODE_REGISTERED)
            continue;
        if (!hay || s->op_start[i] < min) {
            hay = true;
            min = s->op_start[i];
            nodo = i;
        }
    }
    if (hay) {
        if (out_node != NULL)     *out_node = nodo;
        if (out_start_ns != NULL) *out_start_ns = min;
    }
    return hay;
}

/* ---- Barreras ---------------------------------------------------------- */

/* Casillero de `id`, o NULL si esa barrera no se usó nunca. No crea nada. */
static struct barrier *barrier_find(sess *s, uint32_t id) {
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++)
        if (s->barrier[i].used && s->barrier[i].id == id)
            return &s->barrier[i];
    return NULL;
}

/* Reserva un casillero para `id`. NULL si se agotaron.
 *
 * Se llama SOLO cuando la llegada ya pasó todas las validaciones. Si se
 * reservara antes, una llegada rechazada consumiría un casillero, y un
 * trabajador desincronizado podría agotar la tabla a fuerza de rechazos. */
static struct barrier *barrier_alloc(sess *s, uint32_t id) {
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++) {
        struct barrier *b = &s->barrier[i];
        if (b->used)
            continue;
        /* La primera generación de la vida de una barrera es la 0, porque el
         * trabajador manda cuántas veces YA pasó (D0.8). */
        b->used       = true;
        b->id         = id;
        b->generation = 0;
        b->arrived    = 0;
        return b;
    }
    return NULL;
}

enum sess_barrier sess_barrier_arrive(sess *s, uint32_t node,
                                      uint32_t barrier_id, uint64_t generation,
                                      enum dsm_status *why) {
    enum dsm_status motivo = DSM_OK;
    enum sess_barrier out  = SESS_BARRIER_REJECT;

    if (s == NULL) {
        motivo = DSM_EINVAL;
        goto fin;
    }
    if (s->state == SESS_ABORTED) {
        motivo = DSM_EABORT;
        goto fin;
    }
    if (s->state != SESS_RUNNING || !sess_registered(s, node)) {
        motivo = DSM_ESTATE;
        goto fin;
    }

    /* Se valida TODO antes de reservar casillero, para que una llegada
     * rechazada no consuma uno. Una barrera que no existe todavía tiene su
     * generación abierta en 0 de forma implícita. */
    struct barrier *b = barrier_find(s, barrier_id);
    uint64_t abierta = (b != NULL) ? b->generation : 0u;

    /* Una generación que no es la abierta significa un nodo desincronizado:
     * se salteó un barrier_id o los invocó en otro orden (D0.8). */
    if (generation != abierta) {
        motivo = DSM_ESTATE;
        goto fin;
    }
    if (b != NULL && (b->arrived & bit(node)) != 0u) {
        motivo = DSM_ESTATE;   /* una sola llegada por nodo y generación */
        goto fin;
    }

    /* Ya está esperando en OTRA barrera. Un trabajador correcto no puede
     * hacer esto: después de llegar se queda bloqueado hasta la liberación.
     * Se rechaza para que la desincronización se vea acá en vez de aparecer
     * como una barrera que no libera nunca. */
    if (s->in_barrier[node] && s->waiting_on[node] != barrier_id) {
        motivo = DSM_ESTATE;
        goto fin;
    }

    if (b == NULL) {
        b = barrier_alloc(s, barrier_id);
        if (b == NULL) {
            motivo = DSM_EINVAL;   /* se agotaron los casilleros */
            goto fin;
        }
    }

    b->arrived |= bit(node);
    s->in_barrier[node]  = true;
    s->waiting_on[node]  = barrier_id;

    /* Se libera cuando llegaron TODOS los nodos de la sesión. No "todos los
     * activos": si alguno se fue, la sesión ya está abortada por D0.13. */
    uint32_t llegados = 0;
    for (uint32_t i = 0; i < s->nodes; i++)
        if ((b->arrived & bit(i)) != 0u)
            llegados++;

    if (llegados == s->nodes) {
        /* La generación avanza acá, no al recibir la primera llegada de la
         * siguiente: así la ronda que sigue tiene contra qué validarse. */
        b->generation++;
        b->arrived = 0;
        /* Al liberarse, todos dejan de estar esperando: la liberación va a
         * todos, así que ninguno sigue bloqueado. */
        for (uint32_t i = 0; i < s->nodes; i++)
            s->in_barrier[i] = false;
        out = SESS_BARRIER_RELEASE;
    } else {
        out = SESS_BARRIER_WAIT;
    }

fin:
    if (why != NULL)
        *why = motivo;
    return out;
}

bool sess_barrier_pending(const sess *s) {
    if (s == NULL)
        return false;
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++)
        if (s->barrier[i].used && s->barrier[i].arrived != 0u)
            return true;
    return false;
}

bool sess_barrier_info(const sess *s, uint32_t barrier_id,
                       uint64_t *out_generation, uint32_t *out_arrived) {
    if (s == NULL)
        return false;
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++) {
        const struct barrier *b = &s->barrier[i];
        if (!b->used || b->id != barrier_id)
            continue;
        if (out_generation != NULL)
            *out_generation = b->generation;
        if (out_arrived != NULL) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < s->nodes; k++)
                if ((b->arrived & bit(k)) != 0u)
                    n++;
            *out_arrived = n;
        }
        return true;
    }
    return false;
}
