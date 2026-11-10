/* Tramas completas sobre el transporte. Ver include/frame.h. */
#include "frame.h"

#include <string.h>

static struct frame_io result(enum io_status io, enum wire_status wire) {
    struct frame_io r;
    r.io = io;
    r.wire = wire;
    return r;
}

struct frame_io frame_send(int fd, const struct wire_header *h,
                           const void *payload, uint64_t deadline_ns) {
    uint8_t head[DSM_HEADER_SIZE];
    enum wire_status wst;
    enum io_status ist;

    /* Nada sale al socket antes de validar. */
    wst = wire_check(h);
    if (wst != WIRE_OK)
        return result(IO_OK, wst);
    if (h->length > 0u && payload == NULL)
        return result(IO_OK, WIRE_BAD_LENGTH);

    wire_encode_header(head, h);

    ist = io_send_all(fd, head, sizeof head, deadline_ns);
    if (ist != IO_OK)
        return result(ist, WIRE_OK);

    if (h->length > 0u) {
        ist = io_send_all(fd, payload, h->length, deadline_ns);
        if (ist != IO_OK)
            return result(ist, WIRE_OK);
    }
    return result(IO_OK, WIRE_OK);
}

/* Consume y descarta `n` bytes del socket. Solo se usa cuando una carga
 * legítima no cabe en el buffer de quien llama: el flujo tiene que quedar
 * alineado para que la sesión pueda abortarse ordenadamente en vez de
 * quedarse leyendo basura. */
static enum io_status drain(int fd, uint32_t n, uint64_t deadline_ns) {
    uint8_t scratch[512];

    while (n > 0u) {
        uint32_t take = n < (uint32_t)sizeof scratch ? n : (uint32_t)sizeof scratch;
        enum io_status ist = io_recv_all(fd, scratch, take, deadline_ns);
        if (ist != IO_OK)
            return ist;
        n -= take;
    }
    return IO_OK;
}

struct frame_io frame_recv(int fd, struct wire_header *out_h,
                           void *payload, size_t payload_cap,
                           uint64_t deadline_ns) {
    uint8_t head[DSM_HEADER_SIZE];
    struct wire_header h;
    enum wire_status wst;
    enum io_status ist;

    ist = io_recv_all(fd, head, sizeof head, deadline_ns);
    if (ist != IO_OK)
        return result(ist, WIRE_OK);

    /* A partir de acá el flujo está comprometido si algo falla: la cabecera ya
     * se consumió y su carga sigue en camino. */
    wst = wire_decode_header(head, &h);
    if (wst != WIRE_OK)
        return result(IO_OK, wst);

    if (h.length > 0u) {
        if ((size_t)h.length > payload_cap) {
            ist = drain(fd, h.length, deadline_ns);
            return result(ist, WIRE_BAD_LENGTH);
        }
        ist = io_recv_all(fd, payload, h.length, deadline_ns);
        if (ist != IO_OK)
            return result(ist, WIRE_OK);
    }

    *out_h = h;
    return result(IO_OK, WIRE_OK);
}
