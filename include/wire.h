/* wire.h — formato de trama del protocolo DSM. Encabezado INTERNO.
 *
 * Decisiones en docs/decisiones_diseno.md (D2, D12, D13, D14). Este archivo
 * es el contrato entre el coordinador y la biblioteca del trabajador: lo
 * congeló E0 (#1) y lo ajustó la revisión del PR #11. Cambiarlo obliga a
 * revisar los dos lados.
 *
 * Una aplicación NO incluye este archivo: incluye dsm.h, que define la API y
 * los límites públicos. La dirección del include es a propósito —wire.h toma
 * los límites de dsm.h— para que el tamaño de página y los rangos existan en
 * un solo lugar y no se desincronicen (D14).
 *
 * Reglas que vienen del enunciado y que este archivo hace cumplir:
 *
 *   - Enteros de tamaño fijo y orden de bytes definido (big-endian, el orden
 *     de red). Nada de `sizeof(struct)` ni de valores de `enum` en el cable:
 *     los códigos de tipo son constantes con valor explícito y el byte que
 *     viaja es exactamente ese número.
 *   - Toda referencia de red usa `page_id` y desplazamientos. Ningún puntero
 *     del proceso se transmite ni se almacena como referencia compartida.
 *   - El contenido de una página viaja como bytes opacos.
 */
#ifndef SOA_P3_WIRE_H
#define SOA_P3_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsm.h"

/* ---- Límites del protocolo -------------------------------------------
 *
 * DSM_PAGE_SIZE, DSM_MIN/MAX_NODES y DSM_MIN/MAX_PAGES vienen de dsm.h. */

#define DSM_WIRE_MAGIC      0x44534D33u   /* "DSM3" */
#define DSM_WIRE_VERSION    1u
#define DSM_HEADER_SIZE     32u

/* Carga máxima: una versión de 64 bits más una página completa, que es lo que
 * lleva una concesión con datos. Ninguna otra trama es mayor. */
#define DSM_MAX_PAYLOAD     (8u + DSM_PAGE_SIZE)
#define DSM_MAX_FRAME       (DSM_HEADER_SIZE + DSM_MAX_PAYLOAD)

/* Detalle textual máximo de un mensaje de error, sin NUL. */
#define DSM_ERR_DETAIL_MAX  256u

/* Centinelas. `page_id` usa el máximo de u32 cuando el tipo no se refiere a
 * ninguna página; la versión usa el máximo de u64 cuando el emisor no tiene
 * ninguna copia válida en caché. */
#define DSM_NO_PAGE         UINT32_MAX
#define DSM_NO_VERSION      UINT64_MAX

/* ---- Códigos de tipo -------------------------------------------------
 *
 * Valores fijos de un byte. No se reordenan ni se reutilizan: un número
 * retirado queda retirado.
 *
 * Carga y uso de `aux` por tipo (todo entero en orden de red):
 *
 *   HELLO            W->C  8 bytes: u32 nodes_expected, u32 pages_expected.
 *                          pages_expected = 0 significa "sin opinión", que es
 *                          lo que manda la biblioteca: la región la define el
 *                          coordinador. Si llega distinto de cero y no
 *                          coincide con la configuración de la sesión, el
 *                          coordinador rechaza el registro (D7).
 *   SESSION_START    C->W  24 bytes: u32 nodes, u32 pages, u32 page_size,
 *                          u32 reserved, u64 run_id. De aquí toma el
 *                          trabajador la cantidad de páginas.
 *   ACQ_REQ          W->C  8 bytes: u64 cached_version (DSM_NO_VERSION si no
 *                          hay copia válida). aux = modo (DSM_WIRE_MODE_*)
 *   ACQ_GRANT        C->W  8 bytes: u64 version; más DSM_PAGE_SIZE bytes de
 *                          página si flags trae DSM_WIRE_F_HAS_DATA
 *   INVALIDATE       C->W  sin carga. node_id = destinatario;
 *                          request_id = la solicitud que la causó;
 *                          aux = node_id del nodo SOLICITANTE (D12)
 *   INVALIDATE_ACK   W->C  sin carga. request_id y aux = los del INVALIDATE
 *                          que confirma (solicitud y solicitante, D21)
 *   REL_WRITE        W->C  DSM_PAGE_SIZE bytes: la página completa.
 *                          request_id = el del ACQ_REQ del acceso (D21)
 *   REL_WRITE_ACK    C->W  8 bytes: u64 version ya publicada
 *   REL_READ         W->C  sin carga. request_id = el del ACQ_REQ (D21)
 *   REL_READ_ACK     C->W  sin carga
 *   BARRIER_ARRIVE   W->C  8 bytes: u64 generation, contada por el propio
 *                          trabajador para ese barrier_id. aux = barrier_id
 *   BARRIER_RELEASE  C->W  8 bytes: u64 generation. aux = barrier_id
 *   ERROR            C->W  0..DSM_ERR_DETAIL_MAX bytes de detalle UTF-8 sin
 *                          NUL. aux = código (enum dsm_status de dsm.h, con
 *                          sus valores explícitos; uno desconocido se trata
 *                          como DSM_EPROTO)
 *   BYE              W->C  sin carga. Salida ordenada, ver abajo.
 *
 *
 * CORRELACIÓN (D12)
 *
 * Cada trabajador asigna sus propios request_id, así que un request_id solo
 * identifica una operación junto con el nodo que la originó: la clave es
 * (node_id solicitante, request_id). En las tramas que van del coordinador a
 * un trabajador, `node_id` es el destinatario, no el solicitante — por eso
 * INVALIDATE lleva al solicitante en `aux`. Sin ese campo, el log del
 * coordinador no podría atribuir una invalidación a la solicitud WRITE que la
 * causó sin llevar contabilidad aparte, y ese log es justamente lo que lee el
 * verificador de invariantes.
 *
 * Los request_id de un trabajador crecen estrictamente durante la sesión
 * (pueden saltar, nunca repetirse) y un acceso completo usa uno solo: el
 * ACQ_REQ lo estrena, y la concesión, el release y su confirmación lo
 * repiten. El coordinador rechaza un ACQ_REQ que no crezca y un release con
 * otro request_id (D21).
 *
 * Un INVALIDATE_ACK repite request_id y aux del INVALIDATE que confirma, así
 * que lleva la clave completa de la ronda. Emparejarlo solo por request_id no
 * alcanza: dos nodos pueden usar el mismo número, y un ACK rezagado de una
 * ronda anterior se contaría en la actual (D21, hallazgo de la revisión del
 * PR #12). Su node_id es el del nodo que confirma.
 *
 *
 * SALIDA ORDENADA: BYE (D13)
 *
 * BYE es la despedida que manda DSM_finalize. Al recibirlo, el coordinador:
 *
 *   - saca al nodo del conjunto de copias retenidas de todas las páginas, y
 *     de los lectores activos si por algún motivo quedara alguno, para no
 *     mandarle después un INVALIDATE a alguien que ya se fue y quedarse
 *     esperando un ACK que no va a llegar;
 *   - cierra su conexión.
 *
 * Un BYE es legítimo solo si ese nodo no tiene ninguna página adquirida —
 * DSM_finalize ya lo exige del lado de la biblioteca— y si ninguna barrera
 * abierta queda sin poder completarse. Si el nodo se despide mientras otros
 * esperan en una barrera que necesita su llegada, esa barrera es insatisfacible
 * y la sesión se aborta: es el mismo caso que perder un participante, y la
 * política del enunciado ante eso es abortar, no seguir con menos nodos. Los
 * que esperaban despiertan con DSM_EABORT.
 */
#define DSM_WIRE_HELLO            0x01u
#define DSM_WIRE_SESSION_START    0x02u
#define DSM_WIRE_ACQ_REQ          0x10u
#define DSM_WIRE_ACQ_GRANT        0x11u
#define DSM_WIRE_INVALIDATE       0x12u
#define DSM_WIRE_INVALIDATE_ACK   0x13u
#define DSM_WIRE_REL_WRITE        0x14u
#define DSM_WIRE_REL_WRITE_ACK    0x15u
#define DSM_WIRE_REL_READ         0x16u
#define DSM_WIRE_REL_READ_ACK     0x17u
#define DSM_WIRE_BARRIER_ARRIVE   0x20u
#define DSM_WIRE_BARRIER_RELEASE  0x21u
#define DSM_WIRE_ERROR            0x7Eu
#define DSM_WIRE_BYE              0x7Fu

/* Modo en el campo aux de ACQ_REQ. El número que viaja es este, no el valor
 * del enum de dsm.h; los static_assert de abajo garantizan que no se
 * desincronicen. */
#define DSM_WIRE_MODE_READ    1u
#define DSM_WIRE_MODE_WRITE   2u

_Static_assert((unsigned)DSM_READ == DSM_WIRE_MODE_READ,
               "DSM_READ y DSM_WIRE_MODE_READ deben ser el mismo número");
_Static_assert((unsigned)DSM_WRITE == DSM_WIRE_MODE_WRITE,
               "DSM_WRITE y DSM_WIRE_MODE_WRITE deben ser el mismo número");

/* Banderas del byte flags. */
#define DSM_WIRE_F_HAS_DATA   0x01u

/* ---- Cabecera --------------------------------------------------------- */

/* Representación en memoria de la cabecera. NO se escribe en el socket tal
 * cual: wire_encode_header la serializa campo por campo. El orden de los
 * miembros aquí no tiene por qué coincidir con el del cable. */
struct wire_header {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t length;      /* bytes de carga después de la cabecera */
    uint32_t node_id;
    uint32_t page_id;     /* DSM_NO_PAGE si el tipo no se refiere a una */
    uint32_t aux;         /* modo, barrier_id, solicitante o código de error */
    uint64_t request_id;
};

/* Resultado de decodificar o validar. Un valor distinto de WIRE_OK describe
 * por qué la trama se rechaza; el coordinador responde ERROR o aborta la
 * sesión, según el caso, pero nunca modifica su estado antes de validar. */
enum wire_status {
    WIRE_OK = 0,
    WIRE_BAD_MAGIC,       /* no es una trama de este protocolo */
    WIRE_BAD_VERSION,     /* versión de protocolo distinta */
    WIRE_BAD_TYPE,        /* código de tipo desconocido */
    WIRE_BAD_LENGTH,      /* longitud imposible para ese tipo */
    WIRE_BAD_FIELD        /* modo, page_id o bandera inválidos para ese tipo */
};

/* ---- Serialización (implementada en src/wire.c, issue #2) ------------- */

/* Escribe la cabecera en buf. No valida: se asume que h ya pasó wire_check. */
void wire_encode_header(uint8_t buf[DSM_HEADER_SIZE], const struct wire_header *h);

/* Lee una cabecera y valida magia, versión, tipo y longitud. En cualquier
 * resultado distinto de WIRE_OK, *out queda sin garantías. */
enum wire_status wire_decode_header(const uint8_t buf[DSM_HEADER_SIZE],
                                    struct wire_header *out);

/* Valida la coherencia interna de una cabecera: longitud admisible para su
 * tipo, modo conocido en ACQ_REQ, page_id presente donde el tipo lo exige y
 * ausente donde no, banderas aplicables. Es el chequeo que corre el
 * coordinador antes de tocar su estado. */
enum wire_status wire_check(const struct wire_header *h);

/* Longitudes de carga admisibles para un tipo. Ambas son 0 para los tipos sin
 * carga, e iguales entre sí para los de carga fija. Devuelve false si el tipo
 * es desconocido. */
bool wire_payload_range(uint8_t type, uint32_t *min_len, uint32_t *max_len);

/* Nombre del tipo para los registros y los mensajes de error ("ACQ_GRANT").
 * Devuelve "?" si el código es desconocido; nunca NULL. */
const char *wire_type_name(uint8_t type);

/* Texto de un wire_status, en español y sin salto de línea final. */
const char *wire_strstatus(enum wire_status s);

/* ---- Enteros en orden de red (implementados en src/wire.c) ------------
 *
 * Existen para que el coordinador y la biblioteca no escriban dos veces la
 * misma conversión, y para que las cargas se armen campo por campo sin pasar
 * nunca por un struct volcado en bruto. */
void     wire_put_u32(uint8_t *p, uint32_t v);
void     wire_put_u64(uint8_t *p, uint64_t v);
uint32_t wire_get_u32(const uint8_t *p);
uint64_t wire_get_u64(const uint8_t *p);

#endif /* SOA_P3_WIRE_H */
