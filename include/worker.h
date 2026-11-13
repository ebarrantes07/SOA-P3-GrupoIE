/* worker.h — estado del trabajador: caché de páginas y respuesta esperada.
 * Encabezado INTERNO de libdsm (#5).
 *
 * Es al trabajador lo que manager.c es al coordinador (D3.4): la parte del
 * protocolo que no necesita sockets ni hilos. `wk_apply` recibe una trama ya
 * decodificada y decide qué le hace al estado; `src/dsm.c` pone los hilos, el
 * candado y la red alrededor. Así cada regla —qué concesión se acepta, cuándo
 * una invalidación es legítima, qué versión tiene que traer una
 * publicación— se prueba alimentando tramas a mano, sin procesos ni
 * esperas (tests/test_worker.c).
 *
 *
 * CINCO ESTADOS POR PÁGINA (#5)
 *
 *   WK_EMPTY    nunca hubo copia
 *   WK_INVALID  hubo copia y el coordinador la invalidó
 *   WK_CACHED   copia vigente retenida, sin permiso
 *   WK_READ     permiso READ activo
 *   WK_WRITE    permiso WRITE activo
 *
 * Solo WK_CACHED se ofrece como `cached_version` al pedir acceso, y ningún
 * estado evita la consulta: la copia ahorra la transferencia, no el permiso
 * (dsm.h, "permiso activo y copia en caché").
 *
 *
 * UNA SOLA RESPUESTA ESPERADA A LA VEZ
 *
 * La aplicación es un hilo y cada llamada espera la suya, así que alcanza un
 * `struct wk_wait`. Se anota ANTES de enviar la solicitud: si la respuesta
 * llegara antes de anotarla, se tomaría por inesperada. Toda trama que no sea
 * una invalidación tiene que coincidir con lo anotado; si no, es DSM_EPROTO,
 * porque el coordinador nunca manda respuestas que nadie pidió (D4.1).
 */
#ifndef SOA_P3_WORKER_H
#define SOA_P3_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "dsm.h"
#include "log.h"
#include "wire.h"

enum wk_page_state {
    WK_EMPTY = 0,
    WK_INVALID,
    WK_CACHED,
    WK_READ,
    WK_WRITE
};

enum wk_wait_kind {
    WK_NONE = 0,
    WK_GRANT,
    WK_REL_WRITE,
    WK_REL_READ,
    WK_BARRIER
};

/* La respuesta que espera el hilo de aplicación. */
struct wk_wait {
    enum wk_wait_kind kind;
    uint32_t          page;      /* GRANT y REL_*                   */
    uint64_t          request;   /* GRANT y REL_* (D2.1)            */
    enum dsm_mode     mode;      /* GRANT                           */
    uint32_t          barrier;   /* BARRIER                         */
    uint64_t          gen;       /* BARRIER (D0.8)                  */
    uint64_t          t0_ns;     /* inicio de la operación, para dur_ns */
    bool              done;
};

struct wk_slot {
    enum wk_page_state state;
    uint64_t           version;  /* DSM_NO_VERSION salvo en CACHED/READ/WRITE */
};

struct wk_state {
    uint32_t        nid;
    uint32_t        pages;
    struct wk_slot *slot;     /* pages entradas */
    uint8_t        *frames;   /* pages × DSM_PAGE_SIZE: el marco de cada página */
    struct wk_wait  wait;
};

/* Qué provocó una trama, para que quien llama haga la E/S y el registro
 * después de soltar el candado. */
struct wk_effect {
    enum log_event event;     /* fila del registro que corresponde */
    bool           completed; /* cerró la espera anotada (wait.done) */
    bool           ack;       /* hay que mandar INVALIDATE_ACK: misma página,
                               * request_id y aux que la trama (D2.1) */
    uint64_t       version;   /* versión resultante, o DSM_NO_VERSION */
};

/* Reserva el estado de `pages` páginas, todas en WK_EMPTY y con el marco en
 * ceros. false si falta memoria o los argumentos no sirven. */
bool wk_init(struct wk_state *w, uint32_t nid, uint32_t pages);
void wk_free(struct wk_state *w);

/* Versión que se ofrece al pedir `page`: la de la copia retenida, o
 * DSM_NO_VERSION si no hay una vigente. */
uint64_t wk_offer(const struct wk_state *w, uint32_t page);

/* Marco de `page`: la memoria que DSM_acquire entrega. */
uint8_t *wk_frame(struct wk_state *w, uint32_t page);

/* Anota la respuesta esperada, con done en false. */
void wk_expect(struct wk_state *w, const struct wk_wait *wait);

/* Aplica una trama recibida del coordinador. Devuelve DSM_OK y llena *out, o
 * el error que la vuelve inaceptable sin haber cambiado nada del estado.
 *
 * Un ERROR devuelve su código (D0.11): uno desconocido, o un DSM_OK que no
 * tiene sentido en un error, se traduce a DSM_EPROTO. Todos los demás
 * rechazos son DSM_EPROTO, y son de un coordinador que no sigue el
 * protocolo:
 *
 *   - node_id distinto de este nodo (en C->W es el destinatario, D0.12), o
 *     page_id fuera de la región;
 *   - una respuesta que no coincide en tipo, página, request_id o
 *     generación con la anotada, o que llega sin nada anotado;
 *   - una concesión sin datos cuando la copia no existe o es de otra versión,
 *     que serviría datos viejos con permiso nuevo;
 *   - una publicación cuya versión no es la concedida más uno (D2.1);
 *   - un INVALIDATE sobre un permiso activo: el coordinador solo invalida
 *     copias retenidas, porque un escritor espera a que terminen los
 *     lectores antes de abrir su ronda (D2.1);
 *   - cualquier tipo que el coordinador no emite. */
enum dsm_status wk_apply(struct wk_state *w, const struct wire_header *h,
                         const uint8_t *payload, struct wk_effect *out);

/* Código de una trama ERROR, con la traducción de D0.11. */
enum dsm_status wk_status_from_wire(uint32_t code);

#endif /* SOA_P3_WORKER_H */
