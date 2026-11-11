/* directory.c — directorio de páginas del coordinador (E2, #3).
 *
 * Contrato y reglas en include/directory.h. Este archivo no incluye nada de
 * red ni de hilos: es una máquina de estados alimentada por eventos.
 */
#include "directory.h"

#include <stdlib.h>
#include <string.h>

/* ---- Estado ------------------------------------------------------------ */

/* Una solicitud que espera en la cola de su página. */
struct request {
    uint32_t      node;
    enum dsm_mode mode;
    uint64_t      request_id;
    uint64_t      cached_version;
};

/* Dónde está cada nodo. Un nodo tiene a lo sumo una cosa en curso en todo el
 * directorio, porque la biblioteca permite una sola página adquirida y un solo
 * hilo de aplicación: o espera en una cola, o lee, o escribe, o nada. */
enum node_state {
    NODE_IDLE = 0,
    NODE_WAITING,
    NODE_READING,
    NODE_WRITING
};

struct node {
    enum node_state state;
    uint32_t        page;    /* válido si state != NODE_IDLE */
    uint64_t        request; /* request_id del acceso en curso o del último;
                              * los request_id de un nodo solo crecen (D21) */
    bool            has_request;
};

struct page {
    uint64_t version;
    int32_t  writer;          /* -1 = ninguno */
    uint8_t  readers;         /* lectores activos */
    uint8_t  holders;         /* copias retenidas de la versión vigente */

    /* Ronda de invalidación. `round_open` mientras se esperan ACK para la
     * solicitud WRITE de la cabeza. Al cerrarse, request_id y `inval_acked`
     * se conservan hasta la ronda siguiente para reconocer un ACK repetido. */
    bool     round_open;
    bool     round_known;     /* hubo al menos una ronda en esta página */
    uint64_t round_request;
    uint32_t round_requester;
    uint8_t  inval_pending;
    uint8_t  inval_acked;

    /* Cola FIFO circular. Cada nodo tiene a lo sumo una solicitud en curso,
     * así que DSM_MAX_NODES entradas nunca se desbordan. */
    struct request queue[DSM_MAX_NODES];
    uint32_t       qhead;
    uint32_t       qlen;
};

struct dir {
    uint32_t     nodes;
    uint32_t     pages;
    dir_emit_fn  emit;
    void        *user;
    uint8_t      left;        /* nodos que ya se despidieron */
    struct node  node[DSM_MAX_NODES];
    struct page *page;        /* `pages` entradas */
    uint8_t     *data;        /* pages * DSM_PAGE_SIZE bytes confirmados */
};

/* ---- Utilidades -------------------------------------------------------- */

static uint8_t bit(uint32_t node) {
    return (uint8_t)(1u << node);
}

static uint32_t popcount8(uint8_t m) {
    uint32_t n = 0;
    for (; m != 0; m &= (uint8_t)(m - 1u))
        n++;
    return n;
}

static uint8_t *page_bytes(const dir *d, uint32_t page_id) {
    return d->data + (size_t)page_id * DSM_PAGE_SIZE;
}

static const struct request *queue_head(const struct page *p) {
    return &p->queue[p->qhead];
}

static void queue_push(struct page *p, const struct request *r) {
    p->queue[(p->qhead + p->qlen) % DSM_MAX_NODES] = *r;
    p->qlen++;
}

static void queue_pop(struct page *p) {
    p->qhead = (p->qhead + 1u) % DSM_MAX_NODES;
    p->qlen--;
}

/* Validación común de nodo y página. No mira el estado. */
static enum dir_status check_ids(const dir *d, uint32_t node, uint32_t page_id) {
    if (d == NULL || node >= d->nodes || page_id >= d->pages)
        return DIR_EINVAL;
    if (d->left & bit(node))
        return DIR_ESTATE;
    return DIR_OK;
}

/* ---- Concesiones ------------------------------------------------------- */

/* Concede la solicitud `r` sobre la página. Quien llama ya comprobó que las
 * reglas lo permiten. Actualiza el estado ANTES de emitir. */
static void grant(dir *d, uint32_t page_id, const struct request *r) {
    struct page *p = &d->page[page_id];
    uint8_t b = bit(r->node);

    /* La coincidencia de versión solo ahorra los datos si además el
     * directorio sabe que esa copia sigue vigente. Un nodo que no figura
     * entre las copias retenidas recibe los datos aunque declare la versión
     * correcta: su copia pudo haber sido invalidada. */
    bool has_copy = (p->holders & b) && r->cached_version == p->version;

    if (r->mode == DSM_READ) {
        p->readers |= b;
        p->holders |= b;
        d->node[r->node].state = NODE_READING;
    } else {
        /* Todas las demás copias ya confirmaron su invalidación: la única
         * copia válida que queda es la del escritor. */
        p->writer  = (int32_t)r->node;
        p->holders = b;
        d->node[r->node].state = NODE_WRITING;
    }
    d->node[r->node].page = page_id;

    struct dir_action a = {
        .type       = DIR_ACT_GRANT,
        .node       = r->node,
        .page_id    = page_id,
        .request_id = r->request_id,
        .requester  = r->node,
        .mode       = r->mode,
        .version    = p->version,
        .data       = has_copy ? NULL : page_bytes(d, page_id),
    };
    d->emit(d->user, &a);
}

/* Abre la ronda de invalidación para la solicitud WRITE `r`: a todas las
 * copias retenidas menos la del propio solicitante. Emite un INVALIDATE por
 * destino, en orden de nodo. */
static void open_round(dir *d, uint32_t page_id, const struct request *r) {
    struct page *p = &d->page[page_id];

    p->round_open      = true;
    p->round_known     = true;
    p->round_request   = r->request_id;
    p->round_requester = r->node;
    p->inval_pending   = (uint8_t)(p->holders & ~bit(r->node));
    p->inval_acked     = 0;

    for (uint32_t n = 0; n < d->nodes; n++) {
        if (!(p->inval_pending & bit(n)))
            continue;
        struct dir_action a = {
            .type       = DIR_ACT_INVALIDATE,
            .node       = n,
            .page_id    = page_id,
            .request_id = r->request_id,
            .requester  = r->node,
            .mode       = r->mode,
            .version    = p->version,
            .data       = NULL,
        };
        d->emit(d->user, &a);
    }
}

/* Avanza la cola de una página todo lo que las reglas permitan. Se llama
 * después de cada evento que pueda habilitar algo. Al volver, o la cola está
 * vacía, o hay un escritor activo, o la cabeza es un WRITE que espera a que
 * terminen los lectores o a que lleguen los ACK de su ronda. */
static void pump(dir *d, uint32_t page_id) {
    struct page *p = &d->page[page_id];

    while (p->writer < 0 && p->qlen > 0) {
        struct request r = *queue_head(p);

        if (r.mode == DSM_READ) {
            /* Sin escritor y sin WRITE delante: se concede, y el ciclo sigue
             * con el siguiente, así se agrupan los lectores consecutivos. */
            queue_pop(p);
            grant(d, page_id, &r);
            continue;
        }

        /* WRITE en la cabeza. Primero terminan los accesos activos. */
        if (p->readers != 0)
            return;

        /* Después, sus invalidaciones. La ronda se abre una sola vez por
         * solicitud: si ya está abierta para esta cabeza, solo se espera. */
        bool round_for_head = p->round_open &&
                              p->round_request == r.request_id &&
                              p->round_requester == r.node;
        if (!round_for_head)
            open_round(d, page_id, &r);
        if (p->inval_pending != 0)
            return;

        /* Todos confirmaron: se cierra la ronda y se concede. */
        p->round_open = false;
        queue_pop(p);
        grant(d, page_id, &r);
        return;
    }
}

/* Confirma la invalidación de `node` en la ronda abierta: su copia deja de
 * contar como retenida. Lo comparten el ACK y la despedida. */
static void confirm_invalidation(struct page *p, uint32_t node) {
    p->inval_pending &= (uint8_t)~bit(node);
    p->inval_acked   |= bit(node);
    p->holders       &= (uint8_t)~bit(node);
}

/* ---- API --------------------------------------------------------------- */

dir *dir_create(uint32_t nodes, uint32_t pages, dir_emit_fn emit, void *user) {
    if (nodes < DSM_MIN_NODES || nodes > DSM_MAX_NODES ||
        pages < DSM_MIN_PAGES || pages > DSM_MAX_PAGES || emit == NULL)
        return NULL;

    dir *d = calloc(1, sizeof *d);
    if (d == NULL)
        return NULL;

    d->page = calloc(pages, sizeof *d->page);
    d->data = calloc(pages, DSM_PAGE_SIZE);   /* páginas iniciales en cero */
    if (d->page == NULL || d->data == NULL) {
        dir_destroy(d);
        return NULL;
    }

    d->nodes = nodes;
    d->pages = pages;
    d->emit  = emit;
    d->user  = user;
    for (uint32_t i = 0; i < pages; i++)
        d->page[i].writer = -1;
    return d;
}

void dir_destroy(dir *d) {
    if (d == NULL)
        return;
    free(d->data);
    free(d->page);
    free(d);
}

enum dir_status dir_acquire(dir *d, uint32_t node, uint32_t page_id,
                            enum dsm_mode mode, uint64_t request_id,
                            uint64_t cached_version) {
    enum dir_status st = check_ids(d, node, page_id);
    if (st != DIR_OK)
        return st;
    if (mode != DSM_READ && mode != DSM_WRITE)
        return DIR_EINVAL;
    if (d->node[node].state != NODE_IDLE)
        return DIR_ESTATE;   /* adquisición anidada o solicitud duplicada */

    /* Los request_id de un nodo crecen estrictamente. Junto con el
     * solicitante que repite el ACK, eso vuelve única la clave de cada ronda
     * de invalidación en toda la sesión, y un ACK rezagado de una ronda
     * anterior no puede confundirse con el de la actual (D21). */
    if (d->node[node].has_request && request_id <= d->node[node].request)
        return DIR_ESTATE;

    struct request r = {
        .node           = node,
        .mode           = mode,
        .request_id     = request_id,
        .cached_version = cached_version,
    };
    d->node[node].state       = NODE_WAITING;
    d->node[node].page        = page_id;
    d->node[node].request     = request_id;
    d->node[node].has_request = true;
    queue_push(&d->page[page_id], &r);
    pump(d, page_id);
    return DIR_OK;
}

enum dir_status dir_invalidate_ack(dir *d, uint32_t node, uint32_t page_id,
                                   uint32_t requester, uint64_t request_id) {
    enum dir_status st = check_ids(d, node, page_id);
    if (st != DIR_OK)
        return st;
    if (requester >= d->nodes)
        return DIR_EINVAL;

    /* La ronda se identifica por (solicitante, request_id), no solo por el
     * request_id: los request_id son únicos por nodo, no en la sesión. */
    struct page *p = &d->page[page_id];
    if (!p->round_known || p->round_requester != requester ||
        p->round_request != request_id)
        return DIR_ESTATE;   /* ACK de una ronda que no es la última */
    if (p->inval_acked & bit(node))
        return DIR_DUPLICATE;
    if (!p->round_open || !(p->inval_pending & bit(node)))
        return DIR_ESTATE;   /* a ese nodo no se le pidió invalidar */

    confirm_invalidation(p, node);
    if (p->inval_pending == 0)
        pump(d, page_id);
    return DIR_OK;
}

enum dir_status dir_release_write(dir *d, uint32_t node, uint32_t page_id,
                                  const uint8_t *data, uint64_t request_id) {
    enum dir_status st = check_ids(d, node, page_id);
    if (st != DIR_OK)
        return st;
    if (data == NULL)
        return DIR_EINVAL;

    struct page *p = &d->page[page_id];
    if (p->writer != (int32_t)node || d->node[node].request != request_id)
        return DIR_ESTATE;   /* no es el escritor, o no es SU acceso */

    /* Publicación: datos y versión cambian ANTES de confirmar al escritor,
     * y la confirmación sale ANTES que las concesiones que habilita. */
    memcpy(page_bytes(d, page_id), data, DSM_PAGE_SIZE);
    p->version++;
    p->writer  = -1;
    p->holders = bit(node);   /* el escritor conserva la versión que publicó */
    d->node[node].state = NODE_IDLE;

    struct dir_action a = {
        .type       = DIR_ACT_WRITE_PUBLISHED,
        .node       = node,
        .page_id    = page_id,
        .request_id = request_id,
        .requester  = node,
        .mode       = DSM_WRITE,
        .version    = p->version,
        .data       = NULL,
    };
    d->emit(d->user, &a);

    pump(d, page_id);
    return DIR_OK;
}

enum dir_status dir_release_read(dir *d, uint32_t node, uint32_t page_id,
                                 uint64_t request_id) {
    enum dir_status st = check_ids(d, node, page_id);
    if (st != DIR_OK)
        return st;

    struct page *p = &d->page[page_id];
    if (!(p->readers & bit(node)) || d->node[node].request != request_id)
        return DIR_ESTATE;   /* no es lector, o no es SU acceso */

    /* La copia queda en `holders`: retenida y revocable. */
    p->readers &= (uint8_t)~bit(node);
    d->node[node].state = NODE_IDLE;

    struct dir_action a = {
        .type       = DIR_ACT_READ_RELEASED,
        .node       = node,
        .page_id    = page_id,
        .request_id = request_id,
        .requester  = node,
        .mode       = DSM_READ,
        .version    = p->version,
        .data       = NULL,
    };
    d->emit(d->user, &a);

    pump(d, page_id);
    return DIR_OK;
}

enum dir_status dir_node_leave(dir *d, uint32_t node) {
    if (d == NULL || node >= d->nodes)
        return DIR_EINVAL;
    if (d->left & bit(node))
        return DIR_ESTATE;
    if (d->node[node].state != NODE_IDLE)
        return DIR_ESTATE;   /* despedida con un acceso o solicitud vivos */

    d->left |= bit(node);
    for (uint32_t i = 0; i < d->pages; i++) {
        struct page *p = &d->page[i];
        bool was_pending = p->round_open && (p->inval_pending & bit(node));
        if (was_pending) {
            /* Su copia deja de existir: eso es exactamente lo que el ACK
             * confirmaría. Sin esto, el WRITE de esa ronda no avanzaría
             * nunca (D13). */
            confirm_invalidation(p, node);
            if (p->inval_pending == 0)
                pump(d, i);
        } else {
            p->holders &= (uint8_t)~bit(node);
        }
    }
    return DIR_OK;
}

uint32_t dir_pages(const dir *d) {
    return d == NULL ? 0 : d->pages;
}

uint32_t dir_nodes(const dir *d) {
    return d == NULL ? 0 : d->nodes;
}

bool dir_page_info(const dir *d, uint32_t page_id, struct dir_page_info *out) {
    if (d == NULL || out == NULL || page_id >= d->pages)
        return false;
    const struct page *p = &d->page[page_id];
    out->version       = p->version;
    out->writer        = p->writer;
    out->readers       = p->readers;
    out->holders       = p->holders;
    out->inval_pending = p->round_open ? p->inval_pending : 0;
    out->reader_count  = popcount8(p->readers);
    out->pending_acks  = popcount8(out->inval_pending);
    out->queued        = p->qlen;
    return true;
}

const uint8_t *dir_page_data(const dir *d, uint32_t page_id) {
    if (d == NULL || page_id >= d->pages)
        return NULL;
    return page_bytes(d, page_id);
}

bool dir_check_invariants(const dir *d) {
    if (d == NULL)
        return false;

    uint8_t all   = (uint8_t)((1u << d->nodes) - 1u);
    uint8_t alive = (uint8_t)(all & ~d->left);

    /* Dónde aparece cada nodo, para cruzarlo con su estado declarado. */
    uint32_t seen[DSM_MAX_NODES] = {0};

    for (uint32_t i = 0; i < d->pages; i++) {
        const struct page *p = &d->page[i];

        if ((p->readers | p->holders | p->inval_pending) & ~alive)
            return false;                       /* nodos ajenos o idos */
        if ((p->readers & ~p->holders) != 0)
            return false;                       /* lector sin copia */
        if (p->qlen > DSM_MAX_NODES)
            return false;

        if (p->writer >= 0) {
            uint32_t w = (uint32_t)p->writer;
            /* SWMR: un escritor, ningún lector, ninguna otra copia válida. */
            if (w >= d->nodes || p->readers != 0 || p->holders != bit(w) ||
                p->round_open)
                return false;
            if (d->node[w].state != NODE_WRITING || d->node[w].page != i)
                return false;
            seen[w]++;
        }

        if (p->round_open) {
            /* Ronda solo para un WRITE en la cabeza, sin lectores activos,
             * y solo sobre copias que siguen retenidas. */
            if (p->qlen == 0 || p->readers != 0)
                return false;
            const struct request *h = queue_head(p);
            if (h->mode != DSM_WRITE || h->node != p->round_requester ||
                h->request_id != p->round_request)
                return false;
            if ((p->inval_pending & ~p->holders) != 0 ||
                (p->inval_pending & bit(p->round_requester)) != 0)
                return false;
        }

        /* Progreso: tras cada evento la cola quedó tan adelantada como se
         * podía. Si no hay escritor, la cabeza es un WRITE que espera a los
         * lectores o a sus ACK; nunca un READ concedible ni un WRITE listo. */
        if (p->writer < 0 && p->qlen > 0) {
            const struct request *h = queue_head(p);
            if (h->mode != DSM_WRITE)
                return false;
            if (p->readers == 0 && (!p->round_open || p->inval_pending == 0))
                return false;
        }

        for (uint32_t n = 0; n < d->nodes; n++) {
            if (p->readers & bit(n)) {
                if (d->node[n].state != NODE_READING || d->node[n].page != i)
                    return false;
                seen[n]++;
            }
        }
        for (uint32_t k = 0; k < p->qlen; k++) {
            const struct request *r = &p->queue[(p->qhead + k) % DSM_MAX_NODES];
            if (r->node >= d->nodes || (d->left & bit(r->node)))
                return false;
            if (d->node[r->node].state != NODE_WAITING || d->node[r->node].page != i ||
                d->node[r->node].request != r->request_id)
                return false;
            seen[r->node]++;
        }
    }

    /* Cada nodo ocupado aparece exactamente una vez; uno ocioso, ninguna. */
    for (uint32_t n = 0; n < d->nodes; n++) {
        uint32_t expected = d->node[n].state == NODE_IDLE ? 0u : 1u;
        if (seen[n] != expected)
            return false;
    }
    return true;
}

const char *dir_strstatus(enum dir_status s) {
    switch (s) {
    case DIR_OK:        return "correcto";
    case DIR_EINVAL:    return "argumento inválido";
    case DIR_ESTATE:    return "evento incompatible con el estado de la página o del nodo";
    case DIR_DUPLICATE: return "ACK de invalidación repetido, descartado";
    }
    return "estado de directorio desconocido";
}
