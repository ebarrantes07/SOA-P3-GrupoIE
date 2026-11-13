/* Estado del trabajador. Ver include/worker.h. */
#include "worker.h"

#include <stdlib.h>
#include <string.h>

bool wk_init(struct wk_state *w, uint32_t nid, uint32_t pages) {
    if (w == NULL || nid >= DSM_MAX_NODES || pages < DSM_MIN_PAGES ||
        pages > DSM_MAX_PAGES)
        return false;

    memset(w, 0, sizeof *w);
    w->nid = nid;
    w->pages = pages;
    w->slot = calloc(pages, sizeof *w->slot);
    /* calloc deja los marcos en cero: la región empieza llena de ceros, como
     * pide el enunciado, aunque nadie la lea sin antes recibir la página. */
    w->frames = calloc(pages, DSM_PAGE_SIZE);
    if (w->slot == NULL || w->frames == NULL) {
        wk_free(w);
        return false;
    }
    for (uint32_t p = 0; p < pages; p++) {
        w->slot[p].state = WK_EMPTY;
        w->slot[p].version = DSM_NO_VERSION;
    }
    return true;
}

void wk_free(struct wk_state *w) {
    if (w == NULL)
        return;
    free(w->slot);
    free(w->frames);
    w->slot = NULL;
    w->frames = NULL;
}

uint64_t wk_offer(const struct wk_state *w, uint32_t page) {
    if (page >= w->pages || w->slot[page].state != WK_CACHED)
        return DSM_NO_VERSION;
    return w->slot[page].version;
}

uint8_t *wk_frame(struct wk_state *w, uint32_t page) {
    return w->frames + (size_t)page * DSM_PAGE_SIZE;
}

void wk_expect(struct wk_state *w, const struct wk_wait *wait) {
    w->wait = *wait;
    w->wait.done = false;
}

enum dsm_status wk_status_from_wire(uint32_t code) {
    if (code >= (uint32_t)DSM_EINVAL && code <= (uint32_t)DSM_EIO)
        return (enum dsm_status)code;
    return DSM_EPROTO;
}

/* La trama responde a lo anotado: mismo tipo de espera, sin responder dos
 * veces, y la misma operación. */
static bool answers(const struct wk_wait *a, enum wk_wait_kind kind,
                    const struct wire_header *h) {
    return a->kind == kind && !a->done && a->page == h->page_id &&
           a->request == h->request_id;
}

enum dsm_status wk_apply(struct wk_state *w, const struct wire_header *h,
                         const uint8_t *payload, struct wk_effect *out) {
    struct wk_wait *a = &w->wait;
    struct wk_slot *s = NULL;

    out->event = LOG_ERROR;
    out->completed = false;
    out->ack = false;
    out->version = DSM_NO_VERSION;

    if (h->type == DSM_WIRE_ERROR)
        return wk_status_from_wire(h->aux);

    if (h->node_id != w->nid)
        return DSM_EPROTO;
    if (h->page_id != DSM_NO_PAGE) {
        if (h->page_id >= w->pages)
            return DSM_EPROTO;
        s = &w->slot[h->page_id];
    }

    switch (h->type) {
    case DSM_WIRE_ACQ_GRANT: {
        uint64_t version = wire_get_u64(payload);

        if (!answers(a, WK_GRANT, h))
            return DSM_EPROTO;
        if ((h->flags & DSM_WIRE_F_HAS_DATA) != 0u) {
            memcpy(wk_frame(w, h->page_id), payload + 8, DSM_PAGE_SIZE);
        } else if (s->state != WK_CACHED || s->version != version) {
            /* "Usá tu copia", y no la tenemos o es otra versión. */
            return DSM_EPROTO;
        }
        s->version = version;
        s->state = (a->mode == DSM_WRITE) ? WK_WRITE : WK_READ;
        out->event = LOG_ACQ_GRANT;
        break;
    }

    case DSM_WIRE_REL_WRITE_ACK: {
        uint64_t version = wire_get_u64(payload);

        if (!answers(a, WK_REL_WRITE, h) || s->state != WK_WRITE)
            return DSM_EPROTO;
        if (version != s->version + 1u)
            return DSM_EPROTO;
        s->version = version;
        s->state = WK_CACHED;      /* el escritor conserva su copia (D2.1) */
        out->event = LOG_WRITE_PUBLISHED;
        break;
    }

    case DSM_WIRE_REL_READ_ACK:
        if (!answers(a, WK_REL_READ, h) || s->state != WK_READ)
            return DSM_EPROTO;
        s->state = WK_CACHED;
        out->event = LOG_READ_RELEASED;
        break;

    case DSM_WIRE_BARRIER_RELEASE:
        /* La liberación es una sola trama para todos los nodos, así que no
         * repite el request_id de ninguna llegada: se empareja por barrera y
         * generación (D0.8). */
        if (a->kind != WK_BARRIER || a->done || a->barrier != h->aux ||
            a->gen != wire_get_u64(payload))
            return DSM_EPROTO;
        out->event = LOG_BARRIER_RELEASE;
        break;

    case DSM_WIRE_INVALIDATE:
        if (s->state == WK_READ || s->state == WK_WRITE)
            return DSM_EPROTO;
        if (s->state == WK_CACHED)
            s->state = WK_INVALID;
        s->version = DSM_NO_VERSION;
        out->event = LOG_INVALIDATE;
        out->ack = true;
        return DSM_OK;   /* no es la respuesta que espera la aplicación */

    default:
        return DSM_EPROTO;
    }

    a->done = true;
    out->completed = true;
    out->version = (s != NULL) ? s->version : DSM_NO_VERSION;
    return DSM_OK;
}
