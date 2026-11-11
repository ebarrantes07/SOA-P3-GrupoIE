/* test_wire.c — pruebas de la serialización del protocolo (#2).
 *
 * Dos cosas que estas pruebas hacen a propósito:
 *
 *   - Fijan los BYTES, no solo el ida y vuelta. Una prueba que solo codifica
 *     y decodifica pasa igual si el orden de bytes está invertido en las dos
 *     direcciones; la trama en el cable estaría mal y nadie se enteraría
 *     hasta intentar hablar con otra implementación.
 *   - Recorren los catorce tipos desde la tabla del propio módulo, así un
 *     tipo nuevo queda cubierto sin tocar las pruebas.
 */
#include "test_util.h"
#include "wire.h"

#include <string.h>

static const uint8_t ALL_TYPES[] = {
    DSM_WIRE_HELLO, DSM_WIRE_SESSION_START, DSM_WIRE_ACQ_REQ, DSM_WIRE_ACQ_GRANT,
    DSM_WIRE_INVALIDATE, DSM_WIRE_INVALIDATE_ACK, DSM_WIRE_REL_WRITE,
    DSM_WIRE_REL_WRITE_ACK, DSM_WIRE_REL_READ, DSM_WIRE_REL_READ_ACK,
    DSM_WIRE_BARRIER_ARRIVE, DSM_WIRE_BARRIER_RELEASE, DSM_WIRE_ERROR, DSM_WIRE_BYE,
};
static const size_t N_TYPES = sizeof(ALL_TYPES) / sizeof(ALL_TYPES[0]);

/* Cabecera válida mínima para un tipo: longitud en su mínimo, page_id según
 * lo que el tipo exija, y aux con un valor admisible donde importa. */
static struct wire_header valid_for(uint8_t type) {
    struct wire_header h;
    uint32_t min = 0;

    memset(&h, 0, sizeof h);
    h.version    = DSM_WIRE_VERSION;
    h.type       = type;
    h.node_id    = 1;
    h.request_id = 7;

    if (type == DSM_WIRE_ACQ_REQ)
        h.aux = DSM_WIRE_MODE_WRITE;
    if (type == DSM_WIRE_INVALIDATE)
        h.aux = 2;

    wire_payload_range(type, &min, NULL);
    h.length = min;

    /* Se prueba sin página y, si el tipo la exige, con una en rango. */
    h.page_id = DSM_NO_PAGE;
    if (wire_check(&h) != WIRE_OK)
        h.page_id = 3;

    return h;
}

static bool same_header(const struct wire_header *a, const struct wire_header *b) {
    return a->version == b->version && a->type == b->type &&
           a->flags == b->flags && a->reserved == b->reserved &&
           a->length == b->length && a->node_id == b->node_id &&
           a->page_id == b->page_id && a->aux == b->aux &&
           a->request_id == b->request_id;
}

/* ---- Orden de bytes --------------------------------------------------- */

static bool test_network_byte_order(void) {
    uint8_t b[8];

    wire_put_u32(b, 0x44534D33u);
    CHECK(b[0] == 0x44 && b[1] == 0x53 && b[2] == 0x4D && b[3] == 0x33);
    CHECK(wire_get_u32(b) == 0x44534D33u);

    wire_put_u64(b, 0x0102030405060708ull);
    CHECK(b[0] == 0x01 && b[1] == 0x02 && b[2] == 0x03 && b[3] == 0x04);
    CHECK(b[4] == 0x05 && b[5] == 0x06 && b[6] == 0x07 && b[7] == 0x08);
    CHECK(wire_get_u64(b) == 0x0102030405060708ull);

    /* Extremos: el bit más alto no debe perderse ni propagarse por signo. */
    wire_put_u32(b, 0xFFFFFFFFu);
    CHECK(wire_get_u32(b) == 0xFFFFFFFFu);
    wire_put_u32(b, 0x80000000u);
    CHECK(b[0] == 0x80 && wire_get_u32(b) == 0x80000000u);
    wire_put_u64(b, UINT64_MAX);
    CHECK(wire_get_u64(b) == UINT64_MAX);
    wire_put_u64(b, 0x8000000000000000ull);
    CHECK(b[0] == 0x80 && wire_get_u64(b) == 0x8000000000000000ull);

    return true;
}

/* Fija el layout completo de la cabecera. Si alguien mueve un campo, esta
 * prueba falla con el byte exacto que cambió. */
static bool test_header_bytes_pinned(void) {
    struct wire_header h;
    uint8_t buf[DSM_HEADER_SIZE];

    static const uint8_t expected[DSM_HEADER_SIZE] = {
        0x44, 0x53, 0x4D, 0x33,                            /*  0 magia "DSM3" */
        0x01,                                              /*  4 versión      */
        0x11,                                              /*  5 ACQ_GRANT    */
        0x01,                                              /*  6 HAS_DATA     */
        0x00,                                              /*  7 reservado    */
        0x00, 0x00, 0x10, 0x08,                            /*  8 longitud 4104 */
        0x00, 0x00, 0x00, 0x02,                            /* 12 node_id 2    */
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,    /* 16 request_id   */
        0x00, 0x00, 0x00, 0x05,                            /* 24 page_id 5    */
        0x00, 0x00, 0x00, 0x00,                            /* 28 aux          */
    };

    memset(&h, 0, sizeof h);
    h.version    = DSM_WIRE_VERSION;
    h.type       = DSM_WIRE_ACQ_GRANT;
    h.flags      = DSM_WIRE_F_HAS_DATA;
    h.length     = 8u + DSM_PAGE_SIZE;
    h.node_id    = 2;
    h.request_id = 0x0102030405060708ull;
    h.page_id    = 5;

    CHECK(wire_check(&h) == WIRE_OK);
    wire_encode_header(buf, &h);
    CHECK(memcmp(buf, expected, sizeof expected) == 0);
    CHECK(DSM_HEADER_SIZE == 32u);

    return true;
}

/* ---- Ida y vuelta ----------------------------------------------------- */

static bool test_roundtrip_every_type(void) {
    for (size_t i = 0; i < N_TYPES; i++) {
        struct wire_header in = valid_for(ALL_TYPES[i]);
        struct wire_header out;
        uint8_t buf[DSM_HEADER_SIZE];

        CHECK(wire_check(&in) == WIRE_OK);
        wire_encode_header(buf, &in);
        memset(&out, 0xAA, sizeof out);
        CHECK(wire_decode_header(buf, &out) == WIRE_OK);
        CHECK(same_header(&in, &out));
    }
    return true;
}

/* Los dos tipos cuya longitud no es fija, en su máximo: una concesión con la
 * página completa y un error con el detalle más largo. */
static bool test_roundtrip_max_payload(void) {
    struct wire_header grant = valid_for(DSM_WIRE_ACQ_GRANT);
    struct wire_header err = valid_for(DSM_WIRE_ERROR);
    struct wire_header out;
    uint8_t buf[DSM_HEADER_SIZE];

    grant.flags = DSM_WIRE_F_HAS_DATA;
    grant.length = 8u + DSM_PAGE_SIZE;
    CHECK(wire_check(&grant) == WIRE_OK);
    wire_encode_header(buf, &grant);
    CHECK(wire_decode_header(buf, &out) == WIRE_OK);
    CHECK(same_header(&grant, &out));
    CHECK(out.length == DSM_MAX_PAYLOAD);

    err.length = DSM_ERR_DETAIL_MAX;
    CHECK(wire_check(&err) == WIRE_OK);
    wire_encode_header(buf, &err);
    CHECK(wire_decode_header(buf, &out) == WIRE_OK);
    CHECK(same_header(&err, &out));

    return true;
}

/* ---- Rechazos --------------------------------------------------------- */

static bool test_rejects_magic_and_version(void) {
    struct wire_header h = valid_for(DSM_WIRE_BYE);
    struct wire_header out;
    uint8_t buf[DSM_HEADER_SIZE];

    wire_encode_header(buf, &h);
    buf[0] ^= 0xFFu;
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_MAGIC);

    h.version = DSM_WIRE_VERSION + 1u;
    wire_encode_header(buf, &h);
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_VERSION);

    /* La magia se comprueba antes que la versión: una trama que no es de este
     * protocolo no debe reportarse como "versión distinta". */
    buf[0] ^= 0xFFu;
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_MAGIC);

    return true;
}

static bool test_rejects_unknown_type(void) {
    struct wire_header h = valid_for(DSM_WIRE_BYE);
    struct wire_header out;
    uint8_t buf[DSM_HEADER_SIZE];
    uint32_t min = 0, max = 0;

    h.type = 0x55u;              /* ningún tipo usa este código */
    CHECK(wire_check(&h) == WIRE_BAD_TYPE);
    wire_encode_header(buf, &h);
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_TYPE);
    CHECK(!wire_payload_range(0x55u, &min, &max));
    CHECK(strcmp(wire_type_name(0x55u), "?") == 0);

    return true;
}

/* Un byte menos y un byte más que lo admisible, para cada tipo. */
static bool test_rejects_bad_length(void) {
    for (size_t i = 0; i < N_TYPES; i++) {
        struct wire_header h = valid_for(ALL_TYPES[i]);
        uint32_t min = 0, max = 0;

        CHECK(wire_payload_range(ALL_TYPES[i], &min, &max));

        if (min > 0u) {
            h.length = min - 1u;
            CHECK(wire_check(&h) == WIRE_BAD_LENGTH);
        }
        h.length = max + 1u;
        CHECK(wire_check(&h) == WIRE_BAD_LENGTH);
    }
    return true;
}

static bool test_flag_rules(void) {
    struct wire_header grant = valid_for(DSM_WIRE_ACQ_GRANT);
    struct wire_header bye = valid_for(DSM_WIRE_BYE);

    /* Un bit de bandera que no existe se rechaza, no se ignora. */
    grant.flags = 0x02u;
    CHECK(wire_check(&grant) == WIRE_BAD_FIELD);
    grant.flags = 0x80u;
    CHECK(wire_check(&grant) == WIRE_BAD_FIELD);

    /* HAS_DATA solo tiene sentido en una concesión. */
    bye.flags = DSM_WIRE_F_HAS_DATA;
    CHECK(wire_check(&bye) == WIRE_BAD_FIELD);

    /* La bandera y la longitud tienen que contar la misma historia: este es
     * el caso que, sin el chequeo, haría que el receptor leyera 4096 bytes
     * de la trama siguiente creyendo que son la página. */
    grant.flags = DSM_WIRE_F_HAS_DATA;
    grant.length = 8u;
    CHECK(wire_check(&grant) == WIRE_BAD_LENGTH);

    grant.flags = 0u;
    grant.length = 8u + DSM_PAGE_SIZE;
    CHECK(wire_check(&grant) == WIRE_BAD_LENGTH);

    return true;
}

static bool test_page_id_rules(void) {
    struct wire_header req = valid_for(DSM_WIRE_ACQ_REQ);
    struct wire_header hello = valid_for(DSM_WIRE_HELLO);
    struct wire_header err = valid_for(DSM_WIRE_ERROR);

    /* Un tipo que se refiere a una página la exige, y en rango. */
    CHECK(req.page_id == 3u);
    req.page_id = DSM_NO_PAGE;
    CHECK(wire_check(&req) == WIRE_BAD_FIELD);
    req.page_id = DSM_MAX_PAGES;
    CHECK(wire_check(&req) == WIRE_BAD_FIELD);
    req.page_id = DSM_MAX_PAGES - 1u;
    CHECK(wire_check(&req) == WIRE_OK);

    /* Un tipo que no habla de páginas tiene que traer el centinela. */
    CHECK(hello.page_id == DSM_NO_PAGE);
    hello.page_id = 0;
    CHECK(wire_check(&hello) == WIRE_BAD_FIELD);

    /* ERROR admite las dos cosas: puede ser sobre una página o sobre la
     * sesión. */
    CHECK(wire_check(&err) == WIRE_OK);
    err.page_id = 7;
    CHECK(wire_check(&err) == WIRE_OK);
    err.page_id = DSM_MAX_PAGES;
    CHECK(wire_check(&err) == WIRE_BAD_FIELD);

    return true;
}

static bool test_node_and_aux_rules(void) {
    struct wire_header req = valid_for(DSM_WIRE_ACQ_REQ);
    struct wire_header inval = valid_for(DSM_WIRE_INVALIDATE);
    struct wire_header err = valid_for(DSM_WIRE_ERROR);

    /* node_id tiene que ser un nodo de la sesión... */
    req.node_id = DSM_MAX_NODES;
    CHECK(wire_check(&req) == WIRE_BAD_FIELD);
    req.node_id = DSM_MAX_NODES - 1u;
    CHECK(wire_check(&req) == WIRE_OK);

    /* ...salvo en ERROR, que tiene que poder devolverle a alguien el
     * identificador fuera de rango con el que se presentó. */
    err.node_id = 99;
    CHECK(wire_check(&err) == WIRE_OK);

    /* El modo de una solicitud solo puede ser READ o WRITE. */
    req.aux = 0;
    CHECK(wire_check(&req) == WIRE_BAD_FIELD);
    req.aux = 3;
    CHECK(wire_check(&req) == WIRE_BAD_FIELD);
    req.aux = DSM_WIRE_MODE_READ;
    CHECK(wire_check(&req) == WIRE_OK);

    /* En una invalidación, aux es el nodo solicitante (D0.12). */
    inval.aux = DSM_MAX_NODES;
    CHECK(wire_check(&inval) == WIRE_BAD_FIELD);
    inval.aux = 0;
    CHECK(wire_check(&inval) == WIRE_OK);

    /* Y su confirmación repite ese mismo campo, porque la ronda se identifica
     * por (solicitante, request_id) y no solo por el número (D2.1). Si acá no
     * se acotara, el coordinador recibiría un solicitante fuera de rango y
     * tendría que defenderse él: la trama ya no sería válida pero pasaría. */
    struct wire_header ack = valid_for(DSM_WIRE_INVALIDATE_ACK);
    ack.aux = DSM_MAX_NODES;
    CHECK(wire_check(&ack) == WIRE_BAD_FIELD);
    ack.aux = DSM_MAX_NODES - 1u;
    CHECK(wire_check(&ack) == WIRE_OK);

    return true;
}

/* `reserved` vale cero en esta versión (D0.2). Se prueba por el camino real,
 * el de decodificar: el byte 7 llega del cable, y es ahí donde un valor
 * distinto de cero tiene que detener la trama. */
static bool test_reserved_must_be_zero(void) {
    struct wire_header h = valid_for(DSM_WIRE_BYE);
    CHECK(wire_check(&h) == WIRE_OK);

    h.reserved = 1u;
    CHECK(wire_check(&h) == WIRE_BAD_FIELD);

    /* En el cable: se arma una trama válida y se ensucia solo el byte 7. */
    uint8_t buf[DSM_HEADER_SIZE];
    struct wire_header ok = valid_for(DSM_WIRE_BYE);
    wire_encode_header(buf, &ok);
    CHECK(buf[7] == 0u);

    struct wire_header out;
    CHECK(wire_decode_header(buf, &out) == WIRE_OK);

    buf[7] = 0xFFu;
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_FIELD);

    return true;
}

/* Una decodificación que falla no debe dejar la cabecera a medio llenar: el
 * coordinador valida antes de tocar su estado, y este módulo hace lo mismo. */
static bool test_decode_leaves_output_untouched(void) {
    struct wire_header h = valid_for(DSM_WIRE_ACQ_REQ);
    struct wire_header out;
    uint8_t buf[DSM_HEADER_SIZE];
    uint8_t sentinel[sizeof(struct wire_header)];

    h.aux = 42;                  /* modo inválido */
    wire_encode_header(buf, &h);

    memset(&out, 0x5A, sizeof out);
    memset(sentinel, 0x5A, sizeof sentinel);
    CHECK(wire_decode_header(buf, &out) == WIRE_BAD_FIELD);
    CHECK(memcmp(&out, sentinel, sizeof out) == 0);

    return true;
}

static bool test_names_and_statuses(void) {
    static const enum wire_status all[] = {
        WIRE_OK, WIRE_BAD_MAGIC, WIRE_BAD_VERSION,
        WIRE_BAD_TYPE, WIRE_BAD_LENGTH, WIRE_BAD_FIELD,
    };

    for (size_t i = 0; i < N_TYPES; i++) {
        const char *n = wire_type_name(ALL_TYPES[i]);
        CHECK(n != NULL);
        CHECK(n[0] != '\0');
        CHECK(strcmp(n, "?") != 0);
    }
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = wire_strstatus(all[i]);
        CHECK(s != NULL);
        CHECK(s[0] != '\0');
    }
    /* Las longitudes se pueden pedir de a una. */
    uint32_t only_max = 0;
    CHECK(wire_payload_range(DSM_WIRE_REL_WRITE, NULL, &only_max));
    CHECK(only_max == DSM_PAGE_SIZE);

    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"network_byte_order", test_network_byte_order},
        {"header_bytes_pinned", test_header_bytes_pinned},
        {"roundtrip_every_type", test_roundtrip_every_type},
        {"roundtrip_max_payload", test_roundtrip_max_payload},
        {"rejects_magic_and_version", test_rejects_magic_and_version},
        {"rejects_unknown_type", test_rejects_unknown_type},
        {"rejects_bad_length", test_rejects_bad_length},
        {"flag_rules", test_flag_rules},
        {"page_id_rules", test_page_id_rules},
        {"node_and_aux_rules", test_node_and_aux_rules},
        {"reserved_must_be_zero", test_reserved_must_be_zero},
        {"decode_leaves_output_untouched", test_decode_leaves_output_untouched},
        {"names_and_statuses", test_names_and_statuses},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
