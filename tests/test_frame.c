/* test_frame.c — pruebas de tramas completas (#2).
 *
 * Es la prueba central del issue: el enunciado exige limitar cada envío y cada
 * recepción a entre 1 y 7 bytes y obtener los mismos resultados funcionales,
 * sin desalineación de tramas. Acá se ejercita eso sobre un socketpair, con
 * los catorce tipos y con un flujo largo de tramas pegadas.
 */
#include "frame.h"
#include "test_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint8_t ALL_TYPES[] = {
    DSM_WIRE_HELLO, DSM_WIRE_SESSION_START, DSM_WIRE_ACQ_REQ, DSM_WIRE_ACQ_GRANT,
    DSM_WIRE_INVALIDATE, DSM_WIRE_INVALIDATE_ACK, DSM_WIRE_REL_WRITE,
    DSM_WIRE_REL_WRITE_ACK, DSM_WIRE_REL_READ, DSM_WIRE_REL_READ_ACK,
    DSM_WIRE_BARRIER_ARRIVE, DSM_WIRE_BARRIER_RELEASE, DSM_WIRE_ERROR, DSM_WIRE_BYE,
};
static const size_t N_TYPES = sizeof(ALL_TYPES) / sizeof(ALL_TYPES[0]);

static bool make_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) != 0)
            return false;
    }
    return true;
}

/* Trama número i, construida de forma determinista para que el que envía y el
 * que recibe puedan generar la misma secuencia sin comunicarse. */
static void frame_for(size_t i, struct wire_header *h, uint8_t *payload) {
    uint8_t type = ALL_TYPES[i % N_TYPES];
    uint32_t min = 0;

    memset(h, 0, sizeof *h);
    h->version    = DSM_WIRE_VERSION;
    h->type       = type;
    h->node_id    = (uint32_t)(i % DSM_MAX_NODES);
    h->request_id = (uint64_t)i * 1000u + 7u;

    if (type == DSM_WIRE_ACQ_REQ)
        h->aux = (i % 2u) ? DSM_WIRE_MODE_READ : DSM_WIRE_MODE_WRITE;
    if (type == DSM_WIRE_INVALIDATE)
        h->aux = (uint32_t)(i % DSM_MAX_NODES);
    if (type == DSM_WIRE_BARRIER_ARRIVE || type == DSM_WIRE_BARRIER_RELEASE)
        h->aux = (uint32_t)(i % 4u);

    wire_payload_range(type, &min, NULL);
    h->length = min;

    /* Una de cada tres concesiones lleva la página completa, que es el caso
     * de 4096 bytes que el issue pide cubrir. */
    if (type == DSM_WIRE_ACQ_GRANT && (i % 3u) == 0u) {
        h->flags = DSM_WIRE_F_HAS_DATA;
        h->length = 8u + DSM_PAGE_SIZE;
    }

    h->page_id = DSM_NO_PAGE;
    if (wire_check(h) != WIRE_OK)
        h->page_id = (uint32_t)(i % DSM_MAX_PAGES);

    /* Carga con un patrón que depende de i, así una trama entregada en el
     * lugar de otra se detecta. */
    for (uint32_t k = 0; k < h->length; k++)
        payload[k] = (uint8_t)(i * 31u + k * 7u + 1u);
}

static bool same_header(const struct wire_header *a, const struct wire_header *b) {
    return a->version == b->version && a->type == b->type &&
           a->flags == b->flags && a->reserved == b->reserved &&
           a->length == b->length && a->node_id == b->node_id &&
           a->page_id == b->page_id && a->aux == b->aux &&
           a->request_id == b->request_id;
}

/* ---- Intercambio con dos hilos --------------------------------------
 *
 * El que envía corre en su propio hilo a propósito. Enviar y recibir en el
 * mismo hilo no funciona con topes chicos: el socket de dominio UNIX
 * contabiliza su capacidad por los buffers internos que ocupa CADA envío, no
 * por bytes, así que 4104 envíos de un byte desbordan la capacidad mucho
 * antes de terminar la trama y el envío se queda esperando a un lector que
 * nunca va a correr. Con dos hilos se turnan, que además es como funciona el
 * sistema real: el hilo receptor del trabajador y el coordinador viven en
 * procesos distintos.
 */
struct exchange_arg {
    int    fd;
    size_t from;
    size_t count;
    size_t sent;
    bool   ok;
};

static void *exchange_sender(void *arg) {
    struct exchange_arg *a = arg;
    uint8_t payload[DSM_MAX_PAYLOAD];

    a->ok = true;
    for (size_t i = a->from; i < a->from + a->count; i++) {
        struct wire_header h;
        frame_for(i, &h, payload);
        if (!frame_ok(frame_send(a->fd, &h, payload, io_deadline_in(30000)))) {
            a->ok = false;
            return NULL;
        }
        a->sent++;
    }
    return NULL;
}

/* Manda `count` tramas y las verifica una por una del otro lado, con el tope
 * de fragmentación en `limit`. */
static bool exchange(size_t from, size_t count, size_t limit) {
    int fds[2];
    pthread_t th;
    struct exchange_arg a;
    uint8_t expect[DSM_MAX_PAYLOAD], got[DSM_MAX_PAYLOAD];
    bool ok = true;

    CHECK(make_pair(fds));
    io_set_chunk_limit(limit);

    a.fd = fds[0];
    a.from = from;
    a.count = count;
    a.sent = 0;
    a.ok = false;
    CHECK(pthread_create(&th, NULL, exchange_sender, &a) == 0);

    for (size_t i = from; i < from + count && ok; i++) {
        struct wire_header want, out;
        struct frame_io r;

        frame_for(i, &want, expect);
        memset(got, 0, sizeof got);
        memset(&out, 0xAA, sizeof out);
        r = frame_recv(fds[1], &out, got, sizeof got, io_deadline_in(30000));
        if (!frame_ok(r) || !same_header(&want, &out) ||
            memcmp(expect, got, want.length) != 0)
            ok = false;
    }

    CHECK(pthread_join(th, NULL) == 0);
    CHECK(ok);
    CHECK(a.ok);
    CHECK(a.sent == count);

    io_set_chunk_limit(0);
    close(fds[0]);
    close(fds[1]);
    return true;
}

/* ---- Los catorce tipos, con cada tope de 1 a 7 ---------------------- */

/* El rango exacto que el enunciado exige tolerar. Cada pasada manda dos de
 * cada tipo, así la concesión aparece con la página completa y sin ella. */
static bool test_roundtrip_all_types(void) {
    for (size_t limit = 1; limit <= 7; limit++) {
        if (!exchange(0, N_TYPES * 2u, limit))
            return false;
    }
    return true;
}

/* Quinientas tramas pegadas, de tipos y tamaños mezclados. Si una sola se
 * leyera con la longitud equivocada, todas las siguientes llegarían corridas
 * y el patrón de la carga no coincidiría. */
static bool test_stream_stays_aligned(void) {
    return exchange(0, 500, 5);
}

/* ---- Rechazos -------------------------------------------------------- */

/* Una cabecera incoherente no debe poner un solo byte en el socket: si
 * saliera, el otro lado quedaría desalineado por un error que es nuestro. */
static bool test_send_rejects_without_writing(void) {
    int fds[2];
    struct wire_header liar;
    uint8_t payload[DSM_MAX_PAYLOAD];
    size_t moved = 0;
    struct frame_io r;

    CHECK(make_pair(fds));
    memset(payload, 0, sizeof payload);
    memset(&liar, 0, sizeof liar);
    liar.version = DSM_WIRE_VERSION;
    liar.type    = DSM_WIRE_ACQ_GRANT;
    liar.node_id = 1;
    liar.page_id = 5;
    liar.flags   = DSM_WIRE_F_HAS_DATA;   /* dice traer la página... */
    liar.length  = 8u;                    /* ...y declara solo 8 bytes */

    r = frame_send(fds[0], &liar, payload, io_deadline_in(1000));
    CHECK(r.io == IO_OK);
    CHECK(r.wire == WIRE_BAD_LENGTH);

    /* Y el socket sigue vacío. */
    CHECK(io_read_some(fds[1], payload, sizeof payload, &moved) == IO_AGAIN);

    /* Carga nula con length distinto de cero también se rechaza. */
    liar.flags = 0;
    liar.length = 8u;
    r = frame_send(fds[0], &liar, NULL, io_deadline_in(1000));
    CHECK(r.wire == WIRE_BAD_LENGTH);
    CHECK(io_read_some(fds[1], payload, sizeof payload, &moved) == IO_AGAIN);

    close(fds[0]);
    close(fds[1]);
    return true;
}

static bool test_recv_rejects_garbage(void) {
    int fds[2];
    struct wire_header out;
    uint8_t garbage[DSM_HEADER_SIZE], payload[DSM_MAX_PAYLOAD];
    struct frame_io r;

    CHECK(make_pair(fds));
    memset(garbage, 0x5A, sizeof garbage);
    CHECK(io_send_all(fds[0], garbage, sizeof garbage, io_deadline_in(1000)) == IO_OK);

    r = frame_recv(fds[1], &out, payload, sizeof payload, io_deadline_in(1000));
    CHECK(r.io == IO_OK);
    CHECK(r.wire == WIRE_BAD_MAGIC);

    close(fds[0]);
    close(fds[1]);
    return true;
}

static bool test_recv_reports_close(void) {
    int fds[2];
    struct wire_header out, h;
    uint8_t payload[DSM_MAX_PAYLOAD];
    uint8_t head[DSM_HEADER_SIZE];
    struct frame_io r;

    /* Cierre en medio de la cabecera. */
    CHECK(make_pair(fds));
    CHECK(io_send_all(fds[0], "parcial", 7, io_deadline_in(1000)) == IO_OK);
    close(fds[0]);
    r = frame_recv(fds[1], &out, payload, sizeof payload, io_deadline_in(1000));
    CHECK(r.io == IO_CLOSED);
    close(fds[1]);

    /* Cierre en medio de la carga: la cabecera llega entera y anuncia 8
     * bytes que nunca van a venir. */
    CHECK(make_pair(fds));
    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type    = DSM_WIRE_BARRIER_ARRIVE;
    h.node_id = 1;
    h.page_id = DSM_NO_PAGE;
    h.length  = 8u;
    CHECK(wire_check(&h) == WIRE_OK);
    wire_encode_header(head, &h);
    CHECK(io_send_all(fds[0], head, sizeof head, io_deadline_in(1000)) == IO_OK);
    close(fds[0]);
    r = frame_recv(fds[1], &out, payload, sizeof payload, io_deadline_in(1000));
    CHECK(r.io == IO_CLOSED);
    close(fds[1]);
    return true;
}

/* Si la carga no cabe en el buffer de quien llama, la trama se consume igual
 * para que el flujo quede alineado: la trama siguiente tiene que poder
 * leerse. Degrada un error de programación a un error de protocolo en vez de
 * corromper la conexión. */
static bool test_small_cap_drains_and_keeps_alignment(void) {
    int fds[2];
    struct wire_header big, small, out;
    uint8_t payload[DSM_MAX_PAYLOAD], tiny[4];
    struct frame_io r;

    CHECK(make_pair(fds));

    /* Sin tope de fragmentación: acá se prueba el descarte, no el troceado, y
     * con un tope chico el envío secuencial no cabría en el socket por la
     * contabilidad de buffers internos. */
    frame_for(6, &big, payload);                 /* i=6: REL_WRITE, 4096 bytes */
    CHECK(big.type == DSM_WIRE_REL_WRITE);
    CHECK(big.length == DSM_PAGE_SIZE);
    CHECK(frame_ok(frame_send(fds[0], &big, payload, io_deadline_in(5000))));

    frame_for(13, &small, payload);              /* la trama siguiente */
    CHECK(frame_ok(frame_send(fds[0], &small, payload, io_deadline_in(5000))));

    /* Buffer ridículamente chico: la carga no cabe. */
    r = frame_recv(fds[1], &out, tiny, sizeof tiny, io_deadline_in(5000));
    CHECK(r.io == IO_OK);
    CHECK(r.wire == WIRE_BAD_LENGTH);

    /* Y el flujo sigue alineado: la trama siguiente se lee entera. */
    r = frame_recv(fds[1], &out, payload, sizeof payload, io_deadline_in(5000));
    CHECK(frame_ok(r));
    CHECK(same_header(&small, &out));

    close(fds[0]);
    close(fds[1]);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"roundtrip_all_types", test_roundtrip_all_types},
        {"stream_stays_aligned", test_stream_stays_aligned},
        {"send_rejects_without_writing", test_send_rejects_without_writing},
        {"recv_rejects_garbage", test_recv_rejects_garbage},
        {"recv_reports_close", test_recv_reports_close},
        {"small_cap_drains_and_keeps_alignment", test_small_cap_drains_and_keeps_alignment},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
