/* wire.h — formato de trama del protocolo DSM.
 *
 * Decisión documentada en docs/decisiones_diseno.md (D2). Este archivo es el
 * contrato entre el coordinador y la biblioteca del trabajador: lo congela E0
 * (#1) para que el transporte (#2) y el directorio (#3) puedan escribirse en
 * paralelo. Cambiarlo obliga a revisar los dos lados.
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

/* ---- Límites del protocolo ------------------------------------------- */

#define DSM_WIRE_MAGIC      0x44534D33u   /* "DSM3" */
#define DSM_WIRE_VERSION    1u
#define DSM_HEADER_SIZE     32u

/* Granularidad lógica del protocolo. El enunciado fija 4096 bytes y advierte
 * que no debe suponerse igual a la página del sistema operativo: aquí es un
 * número del protocolo, nada más. */
#define DSM_PAGE_SIZE       4096u

#define DSM_MIN_NODES       1u
#define DSM_MAX_NODES       4u
#define DSM_MIN_PAGES       1u
#define DSM_MAX_PAGES       256u

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
 * Carga de cada tipo (todo entero en orden de red):
 *
 *   HELLO            W->C  8 bytes: u32 nodes_expected, u32 pages_expected
 *   SESSION_START    C->W  24 bytes: u32 nodes, u32 pages, u32 page_size,
 *                          u32 reserved, u64 run_id
 *   ACQ_REQ          W->C  8 bytes: u64 cached_version (DSM_NO_VERSION si no
 *                          hay copia válida). aux = modo (DSM_WIRE_MODE_*)
 *   ACQ_GRANT        C->W  8 bytes: u64 version; más DSM_PAGE_SIZE bytes de
 *                          página si flags trae DSM_WIRE_F_HAS_DATA
 *   INVALIDATE       C->W  sin carga. request_id = la solicitud que la causó
 *   INVALIDATE_ACK   W->C  sin carga. request_id = el de la invalidación
 *   REL_WRITE        W->C  DSM_PAGE_SIZE bytes: la página completa
 *   REL_WRITE_ACK    C->W  8 bytes: u64 version ya publicada
 *   REL_READ         W->C  sin carga
 *   REL_READ_ACK     C->W  sin carga
 *   BARRIER_ARRIVE   W->C  8 bytes: u64 generation. aux = barrier_id
 *   BARRIER_RELEASE  C->W  8 bytes: u64 generation. aux = barrier_id
 *   ERROR            C->W  0..DSM_ERR_DETAIL_MAX bytes de detalle UTF-8 sin
 *                          NUL. aux = código (dsm_status de dsm.h)
 *   BYE              W->C  sin carga
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

/* Modo en el campo aux de ACQ_REQ. Coincide a propósito con enum dsm_mode de
 * dsm.h, pero el que viaja es este número, no el valor del enum. */
#define DSM_WIRE_MODE_READ    1u
#define DSM_WIRE_MODE_WRITE   2u

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
    uint32_t aux;         /* modo, barrier_id o código de error según el tipo */
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
