/* Serialización del protocolo DSM. Ver include/wire.h para el formato y
 * docs/decisiones_diseno.md (D0.2) para por qué es así.
 *
 * Este módulo no incluye un solo encabezado de sockets: trabaja sobre buffers
 * de bytes y se prueba completo sin red. El transporte vive en io.c.
 */
#include "wire.h"

/* ---- Enteros en orden de red ------------------------------------------
 *
 * Byte por byte a propósito, en vez de htonl/htobe64: así el resultado no
 * depende del orden del anfitrión, no hace falta <arpa/inet.h> ni las
 * extensiones de <endian.h>, y no hay ningún acceso desalineado ni conversión
 * de puntero a un tipo más ancho — que sería comportamiento indefinido si el
 * buffer no estuviera alineado, y un buffer de red nunca garantiza estarlo. */

void wire_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void wire_put_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

uint32_t wire_get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

uint64_t wire_get_u64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

/* ---- Tabla de tipos ---------------------------------------------------
 *
 * Nombre y longitudes admisibles de cada tipo, en una sola tabla, para que no
 * puedan desincronizarse entre wire_type_name y wire_payload_range. Con
 * catorce tipos, recorrerla linealmente no es un problema: una trama se
 * valida una vez y el costo lo domina el socket.
 *
 * REQ_PAGE marca los tipos que se refieren a una página concreta y que por lo
 * tanto exigen un page_id en rango; los demás exigen DSM_NO_PAGE. ERROR es la
 * excepción y admite las dos cosas: un error puede ser sobre una página o
 * sobre la sesión. */
#define REQ_PAGE   1u
#define NO_PAGE    0u
#define PAGE_ANY   2u

struct type_spec {
    uint8_t     type;
    const char *name;
    uint32_t    min_len;
    uint32_t    max_len;
    unsigned    page;      /* REQ_PAGE, NO_PAGE o PAGE_ANY */
};

static const struct type_spec TYPES[] = {
    { DSM_WIRE_HELLO,           "HELLO",            8u,  8u,                NO_PAGE  },
    { DSM_WIRE_SESSION_START,   "SESSION_START",   24u, 24u,                NO_PAGE  },
    { DSM_WIRE_ACQ_REQ,         "ACQ_REQ",          8u,  8u,                REQ_PAGE },
    { DSM_WIRE_ACQ_GRANT,       "ACQ_GRANT",        8u,  DSM_MAX_PAYLOAD,   REQ_PAGE },
    { DSM_WIRE_INVALIDATE,      "INVALIDATE",       0u,  0u,                REQ_PAGE },
    { DSM_WIRE_INVALIDATE_ACK,  "INVALIDATE_ACK",   0u,  0u,                REQ_PAGE },
    { DSM_WIRE_REL_WRITE,       "REL_WRITE",        DSM_PAGE_SIZE, DSM_PAGE_SIZE, REQ_PAGE },
    { DSM_WIRE_REL_WRITE_ACK,   "REL_WRITE_ACK",    8u,  8u,                REQ_PAGE },
    { DSM_WIRE_REL_READ,        "REL_READ",         0u,  0u,                REQ_PAGE },
    { DSM_WIRE_REL_READ_ACK,    "REL_READ_ACK",     0u,  0u,                REQ_PAGE },
    { DSM_WIRE_BARRIER_ARRIVE,  "BARRIER_ARRIVE",   8u,  8u,                NO_PAGE  },
    { DSM_WIRE_BARRIER_RELEASE, "BARRIER_RELEASE",  8u,  8u,                NO_PAGE  },
    { DSM_WIRE_ERROR,           "ERROR",            0u,  DSM_ERR_DETAIL_MAX, PAGE_ANY },
    { DSM_WIRE_BYE,             "BYE",              0u,  0u,                NO_PAGE  },
};

static const size_t TYPES_N = sizeof(TYPES) / sizeof(TYPES[0]);

static const struct type_spec *spec_for(uint8_t type) {
    for (size_t i = 0; i < TYPES_N; i++) {
        if (TYPES[i].type == type)
            return &TYPES[i];
    }
    return NULL;
}

bool wire_payload_range(uint8_t type, uint32_t *min_len, uint32_t *max_len) {
    const struct type_spec *s = spec_for(type);
    if (s == NULL)
        return false;
    if (min_len != NULL)
        *min_len = s->min_len;
    if (max_len != NULL)
        *max_len = s->max_len;
    return true;
}

const char *wire_type_name(uint8_t type) {
    const struct type_spec *s = spec_for(type);
    return s != NULL ? s->name : "?";
}

const char *wire_strstatus(enum wire_status s) {
    switch (s) {
    case WIRE_OK:          return "trama válida";
    case WIRE_BAD_MAGIC:   return "no es una trama de este protocolo";
    case WIRE_BAD_VERSION: return "versión de protocolo distinta";
    case WIRE_BAD_TYPE:    return "tipo de mensaje desconocido";
    case WIRE_BAD_LENGTH:  return "longitud de carga imposible para el tipo";
    case WIRE_BAD_FIELD:   return "campo inválido para el tipo";
    }
    return "estado desconocido";
}

/* ---- Validación ------------------------------------------------------- */

enum wire_status wire_check(const struct wire_header *h) {
    const struct type_spec *s = spec_for(h->type);
    if (s == NULL)
        return WIRE_BAD_TYPE;

    if (h->length < s->min_len || h->length > s->max_len)
        return WIRE_BAD_LENGTH;

    /* Banderas: solo HAS_DATA está definida, y solo tiene sentido en una
     * concesión. Un bit que no se conoce es basura o un protocolo distinto,
     * así que se rechaza en vez de ignorarse. */
    if ((h->flags & (uint8_t)~DSM_WIRE_F_HAS_DATA) != 0u)
        return WIRE_BAD_FIELD;
    if ((h->flags & DSM_WIRE_F_HAS_DATA) != 0u && h->type != DSM_WIRE_ACQ_GRANT)
        return WIRE_BAD_FIELD;

    /* `reserved` vale cero en esta versión del protocolo (D0.2). Se rechaza por
     * el mismo argumento que un bit desconocido de `flags`: si no es cero, la
     * trama viene de otro protocolo o está corrupta. Además deja garantizado
     * que hoy el campo es cero, que es lo que hace falta para poder usarlo
     * algún día sin romper nada. */
    if (h->reserved != 0u)
        return WIRE_BAD_FIELD;

    /* En una concesión, la bandera y la longitud tienen que contarse la misma
     * historia: con datos son la versión más la página, y sin datos es solo
     * la versión. Es el chequeo que atrapa una concesión que dice traer la
     * página y no la trae — si no, el receptor leería 4096 bytes de la trama
     * siguiente. */
    if (h->type == DSM_WIRE_ACQ_GRANT) {
        uint32_t expected = (h->flags & DSM_WIRE_F_HAS_DATA) != 0u
                                ? 8u + DSM_PAGE_SIZE
                                : 8u;
        if (h->length != expected)
            return WIRE_BAD_LENGTH;
    }

    /* page_id: presente y en rango donde el tipo lo exige, ausente donde no.
     * El rango se compara contra el máximo del protocolo, no contra la
     * cantidad de páginas de la sesión, que este módulo no conoce: esa
     * validación la hace el coordinador con su propia configuración. */
    switch (s->page) {
    case REQ_PAGE:
        if (h->page_id >= DSM_MAX_PAGES)
            return WIRE_BAD_FIELD;
        break;
    case NO_PAGE:
        if (h->page_id != DSM_NO_PAGE)
            return WIRE_BAD_FIELD;
        break;
    default:
        if (h->page_id != DSM_NO_PAGE && h->page_id >= DSM_MAX_PAGES)
            return WIRE_BAD_FIELD;
        break;
    }

    /* node_id es el emisor o el destinatario según el tipo, y en los dos
     * casos tiene que ser un nodo de la sesión. La excepción es ERROR: un
     * rechazo puede ir dirigido a quien se presentó con un identificador
     * fuera de rango, y la trama tiene que poder devolverle ese número. */
    if (h->type != DSM_WIRE_ERROR && h->node_id >= DSM_MAX_NODES)
        return WIRE_BAD_FIELD;

    /* aux según el tipo. En BARRIER_* es el barrier_id, que el enunciado no
     * acota, así que cualquier valor sirve. En ERROR es un dsm_status, y un
     * código desconocido se traduce a DSM_EPROTO del lado del receptor (D0.11),
     * no se rechaza acá. */
    if (h->type == DSM_WIRE_ACQ_REQ &&
        h->aux != DSM_WIRE_MODE_READ && h->aux != DSM_WIRE_MODE_WRITE)
        return WIRE_BAD_FIELD;

    /* En una invalidación, aux es el nodo que pidió la escritura (D0.12), y su
     * confirmación repite ese mismo campo: la ronda se identifica por
     * (solicitante, request_id), no solo por el número (D2.1). Las dos tramas
     * se validan igual, porque llevan lo mismo. */
    if ((h->type == DSM_WIRE_INVALIDATE || h->type == DSM_WIRE_INVALIDATE_ACK) &&
        h->aux >= DSM_MAX_NODES)
        return WIRE_BAD_FIELD;

    return WIRE_OK;
}

/* ---- Cabecera --------------------------------------------------------- */

/* La magia la escribe esta función; no es un campo de struct wire_header.
 * La versión sí sale del struct, para que una prueba pueda forjar una trama
 * con versión equivocada y comprobar que se rechaza. */
void wire_encode_header(uint8_t buf[DSM_HEADER_SIZE], const struct wire_header *h) {
    wire_put_u32(buf +  0, DSM_WIRE_MAGIC);
    buf[4] = h->version;
    buf[5] = h->type;
    buf[6] = h->flags;
    buf[7] = h->reserved;
    wire_put_u32(buf +  8, h->length);
    wire_put_u32(buf + 12, h->node_id);
    wire_put_u64(buf + 16, h->request_id);
    wire_put_u32(buf + 24, h->page_id);
    wire_put_u32(buf + 28, h->aux);
}

enum wire_status wire_decode_header(const uint8_t buf[DSM_HEADER_SIZE],
                                    struct wire_header *out) {
    if (wire_get_u32(buf + 0) != DSM_WIRE_MAGIC)
        return WIRE_BAD_MAGIC;

    /* Se arma en una variable local y se copia a *out solo si todo validó.
     * Así quien llama nunca recibe una cabecera a medio llenar, que es la
     * versión de "el coordinador no modifica su estado antes de validar"
     * aplicada a este módulo. */
    struct wire_header h;
    h.version    = buf[4];
    h.type       = buf[5];
    h.flags      = buf[6];
    h.reserved   = buf[7];
    h.length     = wire_get_u32(buf +  8);
    h.node_id    = wire_get_u32(buf + 12);
    h.request_id = wire_get_u64(buf + 16);
    h.page_id    = wire_get_u32(buf + 24);
    h.aux        = wire_get_u32(buf + 28);

    if (h.version != DSM_WIRE_VERSION)
        return WIRE_BAD_VERSION;

    enum wire_status st = wire_check(&h);
    if (st != WIRE_OK)
        return st;

    *out = h;
    return WIRE_OK;
}
