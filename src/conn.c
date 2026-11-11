/* Buffers y extracción de tramas por conexión. Ver include/conn.h. */
#include "conn.h"

#include <string.h>

void conn_init(struct conn *c, int fd) {
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->fd   = fd;
    c->wire = WIRE_OK;
}

/* ---- Entrada ----------------------------------------------------------- */

/* Mueve lo no consumido al principio. Se llama solo cuando hace falta
 * espacio, no en cada extracción: así una lectura que trae varias tramas se
 * recorre sin copiar nada. */
static void compact_in(struct conn *c) {
    if (c->in_head == 0)
        return;
    if (c->in_len > 0)
        memmove(c->in, c->in + c->in_head, c->in_len);
    c->in_head = 0;
}

size_t conn_in_space(const struct conn *c) {
    if (c == NULL)
        return 0;
    /* El espacio real incluye lo que liberaría una compactación. */
    return CONN_IN_CAP - c->in_len;
}

bool conn_feed(struct conn *c, const void *bytes, size_t n) {
    if (c == NULL || (bytes == NULL && n > 0))
        return false;
    if (n > conn_in_space(c))
        return false;
    if (n == 0)
        return true;

    /* Si no cabe al final, se compacta: el espacio existe, está al frente. */
    if (c->in_head + c->in_len + n > CONN_IN_CAP)
        compact_in(c);

    memcpy(c->in + c->in_head + c->in_len, bytes, n);
    c->in_len += n;
    return true;
}

enum conn_frame conn_next(struct conn *c, struct wire_header *out_h,
                          void *payload, size_t payload_cap) {
    if (c == NULL || out_h == NULL || payload_cap < DSM_MAX_PAYLOAD)
        return CONN_FRAME_BAD;

    if (c->in_len < DSM_HEADER_SIZE)
        return CONN_FRAME_NONE;

    const uint8_t *p = c->in + c->in_head;

    /* La cabecera se decodifica sin consumirla: si la carga todavía no llegó,
     * hay que volver a encontrarla igual en la próxima vuelta. */
    struct wire_header h;
    enum wire_status wst = wire_decode_header(p, &h);
    if (wst != WIRE_OK) {
        /* No se consume nada: con una cabecera inválida, su `length` tampoco
         * es confiable, así que no se sabe dónde empezaría la trama
         * siguiente. El flujo murió; quien llama aborta la sesión. */
        c->wire = wst;
        return CONN_FRAME_BAD;
    }

    size_t total = (size_t)DSM_HEADER_SIZE + h.length;
    if (c->in_len < total)
        return CONN_FRAME_NONE;

    if (h.length > 0)
        memcpy(payload, p + DSM_HEADER_SIZE, h.length);
    *out_h = h;

    c->in_head += total;
    c->in_len  -= total;
    if (c->in_len == 0)
        c->in_head = 0;   /* vacío: se vuelve al principio sin copiar */
    return CONN_FRAME_READY;
}

enum wire_status conn_wire(const struct conn *c) {
    return c == NULL ? WIRE_OK : c->wire;
}

/* ---- Salida ------------------------------------------------------------ */

static void compact_out(struct conn *c) {
    if (c->out_head == 0)
        return;
    if (c->out_len > 0)
        memmove(c->out, c->out + c->out_head, c->out_len);
    c->out_head = 0;
}

bool conn_queue(struct conn *c, const struct wire_header *h,
                const void *payload) {
    if (c == NULL || h == NULL)
        return false;

    /* Se valida antes de tocar el buffer, igual que frame_send: una cabecera
     * incoherente no escribe un solo byte. */
    if (wire_check(h) != WIRE_OK)
        return false;
    if (h->length > 0 && payload == NULL)
        return false;
    if (h->length > DSM_MAX_PAYLOAD)
        return false;

    size_t total = (size_t)DSM_HEADER_SIZE + h->length;
    if (total > CONN_OUT_CAP - c->out_len)
        return false;   /* el par dejó de leer: lo decide quien llama */

    if (c->out_head + c->out_len + total > CONN_OUT_CAP)
        compact_out(c);

    uint8_t *dst = c->out + c->out_head + c->out_len;
    wire_encode_header(dst, h);
    if (h->length > 0)
        memcpy(dst + DSM_HEADER_SIZE, payload, h->length);
    c->out_len += total;
    return true;
}

size_t conn_pending(const struct conn *c) {
    return c == NULL ? 0 : c->out_len;
}

size_t conn_take(struct conn *c, void *buf, size_t cap) {
    if (c == NULL || buf == NULL || cap == 0 || c->out_len == 0)
        return 0;
    size_t n = c->out_len < cap ? c->out_len : cap;
    memcpy(buf, c->out + c->out_head, n);
    c->out_head += n;
    c->out_len  -= n;
    if (c->out_len == 0)
        c->out_head = 0;
    return n;
}

/* ---- Sockets: un intento cada una ------------------------------------- */

enum io_status conn_recv(struct conn *c) {
    if (c == NULL)
        return IO_ERROR;

    size_t space = conn_in_space(c);
    if (space == 0)
        return IO_AGAIN;   /* nada que hacer hasta que se consuman tramas */

    /* Se lee al final del buffer; si no hay hueco contiguo, se compacta
     * primero para poder aprovechar la lectura completa. */
    if (c->in_head + c->in_len + space > CONN_IN_CAP)
        compact_in(c);

    size_t moved = 0;
    enum io_status st = io_read_some(c->fd, c->in + c->in_head + c->in_len,
                                    space, &moved);
    if (moved > 0)
        c->in_len += moved;
    if (st == IO_CLOSED)
        c->peer_closed = true;
    return st;
}

enum io_status conn_send(struct conn *c) {
    if (c == NULL)
        return IO_ERROR;
    if (c->out_len == 0)
        return IO_OK;

    size_t moved = 0;
    enum io_status st = io_write_some(c->fd, c->out + c->out_head,
                                      c->out_len, &moved);
    if (moved > 0) {
        c->out_head += moved;
        c->out_len  -= moved;
        if (c->out_len == 0)
            c->out_head = 0;
    }
    return st;
}

bool conn_peer_closed(const struct conn *c) {
    return c != NULL && c->peer_closed;
}
