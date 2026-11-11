/* Pruebas de los buffers por conexión (#4).
 *
 * Casi todo se prueba SIN sockets: se alimentan bytes a mano, en los pedazos
 * que uno elija, y se verifica qué tramas salen. Eso permite forzar de forma
 * determinista los casos que en una red real aparecen por azar: una trama
 * partida en 4136 pedazos de un byte, dos tramas en una sola lectura, una
 * escritura que acepta la mitad. Al final hay una prueba sobre TCP de verdad,
 * para que lo demás no quede verificado solo contra mi propio modelo.
 */
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "conn.h"
#include "net.h"
#include "test_util.h"

static uint8_t PAYLOAD[DSM_MAX_PAYLOAD];

/* Cabecera válida de cada tipo que se usa acá. */
static struct wire_header hdr(uint8_t type, uint32_t len) {
    struct wire_header h;
    memset(&h, 0, sizeof h);
    h.version    = DSM_WIRE_VERSION;
    h.type       = type;
    h.length     = len;
    h.node_id    = 1;
    h.request_id = 42;
    h.page_id    = (type == DSM_WIRE_HELLO || type == DSM_WIRE_BYE)
                       ? DSM_NO_PAGE : 0;
    if (type == DSM_WIRE_ACQ_REQ)
        h.aux = DSM_WIRE_MODE_READ;
    if (type == DSM_WIRE_ACQ_GRANT && len > 8u)
        h.flags = DSM_WIRE_F_HAS_DATA;
    return h;
}

static bool same_header(const struct wire_header *a,
                        const struct wire_header *b) {
    return a->version == b->version && a->type == b->type &&
           a->flags == b->flags && a->reserved == b->reserved &&
           a->length == b->length && a->node_id == b->node_id &&
           a->page_id == b->page_id && a->aux == b->aux &&
           a->request_id == b->request_id;
}

/* Serializa una trama a `buf` como lo haría el emisor. */
static size_t bytes_of(uint8_t *buf, const struct wire_header *h,
                       const void *payload) {
    wire_encode_header(buf, h);
    if (h->length > 0)
        memcpy(buf + DSM_HEADER_SIZE, payload, h->length);
    return (size_t)DSM_HEADER_SIZE + h->length;
}

/* ---- Entrada: extracción --------------------------------------------- */

static bool test_whole_frame_round_trips(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_REQ, 8u);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);

    CHECK(conn_feed(&c, raw, n));
    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&h, &got));
    CHECK(memcmp(pl, PAYLOAD, 8u) == 0);

    /* Consumida: no queda nada. */
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_NONE);
    CHECK(conn_in_space(&c) == CONN_IN_CAP);
    return true;
}

/* El caso que la red produce sola: la trama llega en pedazos de un byte. No
 * hay trama hasta el ÚLTIMO byte, y entonces sale entera. */
static bool test_frame_arriving_one_byte_at_a_time(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);
    CHECK(n == DSM_HEADER_SIZE + 8u + DSM_PAGE_SIZE);

    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    for (size_t i = 0; i + 1 < n; i++) {
        CHECK(conn_feed(&c, raw + i, 1));
        CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_NONE);
    }
    CHECK(conn_feed(&c, raw + n - 1, 1));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&h, &got));
    CHECK(memcmp(pl, PAYLOAD, h.length) == 0);
    return true;
}

/* Dos tramas en una sola lectura: salen las dos, en orden, sin volver al
 * socket en el medio. */
static bool test_two_frames_in_one_read(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header a = hdr(DSM_WIRE_ACQ_REQ, 8u);
    struct wire_header b = hdr(DSM_WIRE_BYE, 0u);
    b.node_id = 2;

    uint8_t raw[2u * DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &a, PAYLOAD);
    n += bytes_of(raw + n, &b, NULL);
    CHECK(conn_feed(&c, raw, n));

    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&a, &got));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&b, &got));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_NONE);
    return true;
}

/* Una trama entera más el principio de la siguiente: lo que sobra espera, y
 * se completa después sin perder un byte. Es el caso que obliga a que el
 * buffer de entrada tenga más de una trama de capacidad. */
static bool test_leftover_waits_for_the_rest(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header a = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    struct wire_header b = hdr(DSM_WIRE_REL_READ, 0u);
    uint8_t ra[DSM_MAX_FRAME], rb[DSM_MAX_FRAME];
    size_t na = bytes_of(ra, &a, PAYLOAD);
    size_t nb = bytes_of(rb, &b, NULL);

    /* Llega `a` completa y 10 bytes de `b`. */
    CHECK(conn_feed(&c, ra, na));
    CHECK(conn_feed(&c, rb, 10));

    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&a, &got));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_NONE);

    /* El resto de `b`. */
    CHECK(conn_feed(&c, rb + 10, nb - 10));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&b, &got));
    return true;
}

/* Una cabecera inválida mata el flujo: se reporta el motivo y NO se consume,
 * porque con un `length` que no es confiable no se sabe dónde empieza la
 * trama siguiente. */
static bool test_bad_header_kills_the_stream(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_REQ, 8u);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);
    raw[0] = 0x00;                 /* magia equivocada */

    CHECK(conn_feed(&c, raw, n));
    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_BAD);
    CHECK(conn_wire(&c) == WIRE_BAD_MAGIC);

    /* Sigue ahí: no se intentó resincronizar, y eso es a propósito. */
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_BAD);

    /* Un campo inválido con cabecera por lo demás sana: mismo tratamiento,
     * distinto motivo. */
    struct conn c2;
    conn_init(&c2, -1);
    struct wire_header mala = hdr(DSM_WIRE_ACQ_REQ, 8u);
    mala.aux = 7;                  /* modo que no existe */
    uint8_t r2[DSM_MAX_FRAME];
    wire_encode_header(r2, &mala);
    memcpy(r2 + DSM_HEADER_SIZE, PAYLOAD, 8u);
    CHECK(conn_feed(&c2, r2, DSM_HEADER_SIZE + 8u));
    CHECK(conn_next(&c2, &got, pl, sizeof pl) == CONN_FRAME_BAD);
    CHECK(conn_wire(&c2) == WIRE_BAD_FIELD);
    return true;
}

/* ---- Salida ------------------------------------------------------------ */

/* Lo encolado sale byte por byte igual a lo que serializa el emisor. */
static bool test_queued_bytes_match_the_wire(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    CHECK(conn_queue(&c, &h, PAYLOAD));
    CHECK(conn_pending(&c) == DSM_HEADER_SIZE + 8u + DSM_PAGE_SIZE);

    uint8_t esperado[DSM_MAX_FRAME], real[DSM_MAX_FRAME];
    size_t n = bytes_of(esperado, &h, PAYLOAD);
    CHECK(conn_take(&c, real, sizeof real) == n);
    CHECK(memcmp(real, esperado, n) == 0);
    CHECK(conn_pending(&c) == 0);
    return true;
}

/* Una cabecera inválida no escribe un solo byte, igual que en frame_send. */
static bool test_queue_validates_before_writing(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header mala = hdr(DSM_WIRE_ACQ_REQ, 8u);
    mala.aux = 9;                              /* modo inexistente */
    CHECK(!conn_queue(&c, &mala, PAYLOAD));
    CHECK(conn_pending(&c) == 0);

    /* Longitud que no corresponde al tipo. */
    struct wire_header larga = hdr(DSM_WIRE_REL_READ, 100u);
    CHECK(!conn_queue(&c, &larga, PAYLOAD));
    CHECK(conn_pending(&c) == 0);

    /* Carga anunciada sin carga que copiar. */
    struct wire_header sinpl = hdr(DSM_WIRE_ACQ_REQ, 8u);
    CHECK(!conn_queue(&c, &sinpl, NULL));
    CHECK(conn_pending(&c) == 0);

    /* Y después de los tres rechazos, una válida sigue entrando. */
    struct wire_header buena = hdr(DSM_WIRE_REL_READ, 0u);
    CHECK(conn_queue(&c, &buena, NULL));
    CHECK(conn_pending(&c) == DSM_HEADER_SIZE);
    return true;
}

/* Que la salida se llene no es un error del coordinador: es un par que dejó
 * de leer. Lo que acá se fija es que al llenarse NO se escriba una trama a
 * medias, que corrompería el flujo igual que una entrada a medias. */
static bool test_full_output_rejects_without_truncating(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header grande = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    size_t tam = DSM_HEADER_SIZE + 8u + DSM_PAGE_SIZE;

    size_t encoladas = 0;
    while (conn_queue(&c, &grande, PAYLOAD))
        encoladas++;

    /* Caben tres de 4136 en 16384, y la cuarta no. */
    CHECK(encoladas == CONN_OUT_CAP / tam);
    size_t antes = conn_pending(&c);
    CHECK(antes == encoladas * tam);

    /* El rechazo no dejó basura: lo pendiente es múltiplo exacto del tamaño
     * de trama, así que no hay ninguna partida. */
    CHECK(antes % tam == 0);

    /* Una trama chica sí entra en lo que sobra. */
    struct wire_header chica = hdr(DSM_WIRE_REL_READ, 0u);
    CHECK(conn_queue(&c, &chica, NULL));
    CHECK(conn_pending(&c) == antes + DSM_HEADER_SIZE);

    /* Y todo lo encolado se recupera entero y en orden. */
    uint8_t esperado[DSM_MAX_FRAME];
    size_t ne = bytes_of(esperado, &grande, PAYLOAD);
    for (size_t i = 0; i < encoladas; i++) {
        uint8_t real[DSM_MAX_FRAME];
        size_t leidos = 0;
        while (leidos < ne)
            leidos += conn_take(&c, real + leidos, ne - leidos);
        CHECK(memcmp(real, esperado, ne) == 0);
    }
    return true;
}

/* El caso del enunciado: una escritura parcial. El socket acepta un pedazo,
 * lo que queda espera, y al reanudar los bytes siguen siendo los correctos y
 * en orden — no se reenvía ni se pierde nada. */
static bool test_partial_write_resumes_exactly(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    CHECK(conn_queue(&c, &h, PAYLOAD));

    uint8_t esperado[DSM_MAX_FRAME];
    size_t n = bytes_of(esperado, &h, PAYLOAD);

    /* Se "escriben" 100 bytes, luego 1, luego el resto en pedazos de 500. */
    uint8_t real[DSM_MAX_FRAME];
    size_t leidos = conn_take(&c, real, 100);
    CHECK(leidos == 100);
    CHECK(conn_pending(&c) == n - 100);

    leidos += conn_take(&c, real + leidos, 1);
    CHECK(leidos == 101);

    while (conn_pending(&c) > 0)
        leidos += conn_take(&c, real + leidos, 500);

    CHECK(leidos == n);
    CHECK(memcmp(real, esperado, n) == 0);
    return true;
}

/* ---- Límites de los buffers ------------------------------------------- */

static bool test_feed_respects_capacity(void) {
    struct conn c;
    conn_init(&c, -1);

    CHECK(conn_in_space(&c) == CONN_IN_CAP);

    /* Más de lo que cabe se rechaza, sin copiar nada. */
    static uint8_t enorme[CONN_IN_CAP + 1];
    CHECK(!conn_feed(&c, enorme, sizeof enorme));
    CHECK(conn_in_space(&c) == CONN_IN_CAP);

    /* Exactamente lo que cabe entra. */
    CHECK(conn_feed(&c, enorme, CONN_IN_CAP));
    CHECK(conn_in_space(&c) == 0);

    /* Y un byte más, no. */
    CHECK(!conn_feed(&c, enorme, 1));

    /* Argumentos imposibles no revientan. */
    CHECK(!conn_feed(NULL, enorme, 1));
    CHECK(!conn_feed(&c, NULL, 1));
    CHECK(conn_feed(&c, NULL, 0));
    CHECK(conn_in_space(NULL) == 0);
    CHECK(conn_pending(NULL) == 0);
    CHECK(!conn_peer_closed(NULL));
    return true;
}

/* El buffer de entrada se compacta cuando hace falta: muchas tramas seguidas
 * se procesan sin que el espacio se agote por avanzar la cabeza. */
static bool test_input_compacts_across_many_frames(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);

    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    for (int i = 0; i < 50; i++) {
        CHECK(conn_feed(&c, raw, n));
        CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
        CHECK(same_header(&h, &got));
        CHECK(memcmp(pl, PAYLOAD, h.length) == 0);
    }
    CHECK(conn_in_space(&c) == CONN_IN_CAP);
    return true;
}

/* El caso que de verdad obliga a compactar: la cabeza quedó avanzada por un
 * sobrante Y lo que llega no cabe al final del buffer, aunque sí cabría si se
 * moviera el sobrante al principio. Sin compactar, la copia se sale del
 * arreglo.
 *
 * Hay que construirlo con cuidado, porque los dos casos fáciles NO lo
 * ejercitan: si se consume todo, conn_next devuelve la cabeza a 0 sola; y si
 * el sobrante es chico, lo que llega entra al final sin mover nada. */
static bool test_compaction_when_tail_is_too_small(void) {
    struct conn c;
    conn_init(&c, -1);

    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);
    CHECK(n == DSM_MAX_FRAME);
    CHECK(CONN_IN_CAP == 2u * DSM_MAX_FRAME);

    /* A entera más los primeros 10 bytes de B. */
    CHECK(conn_feed(&c, raw, n));
    CHECK(conn_feed(&c, raw, 10));

    struct wire_header got;
    uint8_t pl[DSM_MAX_PAYLOAD];
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);

    /* Ahora la cabeza está en n y quedan 10 bytes. Al final del buffer hay
     * sitio para CONN_IN_CAP - n - 10 bytes; se alimenta MÁS que eso, pero
     * menos que el espacio total. Solo entra si se compacta. */
    size_t cola = CONN_IN_CAP - n - 10;
    size_t extra = cola + 74;
    CHECK(extra <= conn_in_space(&c));
    CHECK(extra > cola);

    /* Esos bytes son el resto de B y el principio de C. */
    CHECK(conn_feed(&c, raw + 10, n - 10));           /* completa B */
    size_t de_c = extra - (n - 10);
    CHECK(conn_feed(&c, raw, de_c));                  /* principio de C */

    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&h, &got));
    CHECK(memcmp(pl, PAYLOAD, h.length) == 0);        /* B llegó intacta */

    /* Y C, completada después, también: es la que detecta una copia que se
     * fue de rango, porque sus bytes son los que caerían fuera. */
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_NONE);
    CHECK(conn_feed(&c, raw + de_c, n - de_c));
    CHECK(conn_next(&c, &got, pl, sizeof pl) == CONN_FRAME_READY);
    CHECK(same_header(&h, &got));
    CHECK(memcmp(pl, PAYLOAD, h.length) == 0);
    return true;
}

/* ---- Sobre TCP de verdad ---------------------------------------------- */

struct emisor {
    uint16_t port;
    size_t   tramas;
};

static void *emitir(void *arg) {
    struct emisor *e = arg;
    int fd = -1;
    if (net_connect("127.0.0.1", e->port, &fd, io_deadline_in(3000)) != NET_OK)
        return NULL;

    /* Se envía con un límite de fragmentación de 1 byte, para que el
     * receptor vea las tramas partidas de la peor forma posible. */
    io_set_chunk_limit(1);
    struct wire_header h = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    uint8_t raw[DSM_MAX_FRAME];
    size_t n = bytes_of(raw, &h, PAYLOAD);
    for (size_t i = 0; i < e->tramas; i++) {
        if (io_send_all(fd, raw, n, io_deadline_in(3000)) != IO_OK)
            break;
    }
    io_set_chunk_limit(0);
    net_close(fd);
    return NULL;
}

/* Lo de arriba verifica el módulo contra mi propio modelo de cómo llegan los
 * bytes. Esto lo verifica contra TCP, que es el que decide de verdad. */
static bool test_over_real_tcp(void) {
    int lfd = -1, sfd = -1;
    uint16_t port = 0;
    CHECK(net_listen("127.0.0.1", 0, 4, &lfd, &port) == NET_OK);

    struct emisor e = {port, 3};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, emitir, &e) == 0);
    CHECK(net_accept(lfd, &sfd, io_deadline_in(3000)) == NET_OK);

    struct conn c;
    conn_init(&c, sfd);

    struct wire_header esperada = hdr(DSM_WIRE_ACQ_GRANT, 8u + DSM_PAGE_SIZE);
    size_t vistas = 0;
    uint64_t plazo = io_deadline_in(5000);
    while (vistas < e.tramas) {
        enum io_status st = conn_recv(&c);
        if (st == IO_AGAIN) {
            CHECK(io_wait(sfd, false, plazo) == IO_OK);
            continue;
        }
        if (st == IO_CLOSED)
            break;
        CHECK(st == IO_OK);

        struct wire_header got;
        uint8_t pl[DSM_MAX_PAYLOAD];
        enum conn_frame f;
        while ((f = conn_next(&c, &got, pl, sizeof pl)) == CONN_FRAME_READY) {
            CHECK(same_header(&esperada, &got));
            CHECK(memcmp(pl, PAYLOAD, got.length) == 0);
            vistas++;
        }
        CHECK(f == CONN_FRAME_NONE);
    }
    CHECK(vistas == e.tramas);

    pthread_join(t, NULL);
    net_close(sfd);
    net_close(lfd);
    return true;
}

int main(void) {
    for (size_t i = 0; i < sizeof PAYLOAD; i++)
        PAYLOAD[i] = (uint8_t)(i * 31u + 7u);

    const struct test_case tests[] = {
        {"whole_frame_round_trips", test_whole_frame_round_trips},
        {"frame_arriving_one_byte_at_a_time", test_frame_arriving_one_byte_at_a_time},
        {"two_frames_in_one_read", test_two_frames_in_one_read},
        {"leftover_waits_for_the_rest", test_leftover_waits_for_the_rest},
        {"bad_header_kills_the_stream", test_bad_header_kills_the_stream},
        {"queued_bytes_match_the_wire", test_queued_bytes_match_the_wire},
        {"queue_validates_before_writing", test_queue_validates_before_writing},
        {"full_output_rejects_without_truncating", test_full_output_rejects_without_truncating},
        {"partial_write_resumes_exactly", test_partial_write_resumes_exactly},
        {"feed_respects_capacity", test_feed_respects_capacity},
        {"input_compacts_across_many_frames", test_input_compacts_across_many_frames},
        {"compaction_when_tail_is_too_small", test_compaction_when_tail_is_too_small},
        {"over_real_tcp", test_over_real_tcp},
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]);
}
