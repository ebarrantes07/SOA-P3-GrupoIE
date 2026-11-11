/* directory.h — directorio de páginas del coordinador (E2, #3).
 *
 * Es el núcleo de corrección del protocolo y es un módulo PURO: no incluye
 * ningún encabezado de red, no crea hilos y no escribe registros. Recibe
 * eventos ya validados por el transporte (una solicitud de acceso, un ACK de
 * invalidación, un release, una despedida) y responde emitiendo acciones: a
 * quién conceder, a quién invalidar y a quién confirmar. El coordinador
 * traduce cada acción a una trama; las pruebas la registran y la comparan.
 * Así los invariantes se prueban alimentando secuencias de eventos, sin
 * sockets de por medio (ver docs/decisiones_diseno.md, D21).
 *
 * Es un encabezado interno del coordinador, así que incluye wire.h: los
 * valores que recibe (DSM_NO_VERSION, request_id, el solicitante en aux) son
 * los del protocolo.
 *
 *
 * ESTADO POR PÁGINA
 *
 *   - datos confirmados (DSM_PAGE_SIZE bytes) y su versión, que empieza en 0
 *     con la página llena de ceros y sube en 1 con cada release WRITE;
 *   - lectores activos y escritor activo (o ninguno);
 *   - copias retenidas: los nodos cuya caché tiene la versión confirmada.
 *     Basta un conjunto, sin versión por nodo, porque la versión solo cambia
 *     al publicar un WRITE y antes de conceder ese WRITE se invalidaron todas
 *     las copias ajenas: toda copia retenida es de la versión vigente;
 *   - la ronda de invalidación en curso: a quiénes se les pidió invalidar,
 *     quiénes ya confirmaron y qué solicitud la causó;
 *   - la cola FIFO de solicitudes que todavía no se pueden conceder.
 *
 * Con N <= 4, cada conjunto de nodos es una máscara de bits. Confirmar un ACK
 * es apagar un bit, y apagar un bit dos veces no cuenta dos veces.
 *
 *
 * REGLAS DE CONCESIÓN (las del enunciado, en el orden de la cola)
 *
 *   - READ: se concede si no hay escritor activo y no hay un WRITE delante en
 *     la cola. Los lectores consecutivos de la cabeza se conceden juntos.
 *   - WRITE: espera a que terminen los accesos activos; después invalida las
 *     copias retenidas ajenas y espera todos sus ACK; recién entonces concede.
 *     Mientras hay escritor no se concede nada de esa página.
 *   - En toda concesión, los datos viajan salvo que el solicitante tenga una
 *     copia retenida Y declare esa misma versión. La coincidencia ahorra la
 *     transferencia, nunca el permiso: la solicitud se procesa igual.
 *
 *
 * ORDEN DE LAS ACCIONES
 *
 * Las acciones se emiten de forma sincrónica, dentro de la llamada que las
 * provocó, y en el orden en que deben enviarse. En particular, un release
 * WRITE publica los datos y sube la versión ANTES de emitir su confirmación,
 * y emite la confirmación ANTES que las concesiones que habilita.
 *
 * El puntero `data` de una concesión apunta a los datos confirmados del
 * directorio y solo es válido durante la llamada a `emit`: el coordinador
 * copia esos bytes a su buffer de salida ahí mismo. Como el coordinador tiene
 * un solo hilo (D1), ningún otro evento puede cambiar la página mientras tanto.
 *
 *
 * CORRELACIÓN (D12, D21)
 *
 * Un acceso completo lleva un solo request_id: el de su ACQ_REQ, que se
 * repite en la concesión, en el release y en su confirmación. Los request_id
 * de un mismo nodo crecen estrictamente; pueden saltar (la biblioteca usa el
 * mismo contador para barreras), pero nunca repetirse ni bajar. Así la clave
 * (solicitante, request_id) es única en toda la sesión, y es la que identifica
 * una ronda de invalidación: un ACK de invalidación tiene que repetir las dos
 * partes. Un ACK rezagado de una ronda anterior se rechaza en vez de contarse
 * en la actual, aunque el número coincida.
 *
 *
 * QUÉ NO HACE
 *
 * No valida tramas (eso es wire_check), no lleva sesión ni barreras (E3, #4)
 * y no decide qué hacer ante un error: devuelve un dir_status y el
 * coordinador responde ERROR o aborta. Un evento rechazado NO modifica el
 * estado ni emite acciones.
 */
#ifndef SOA_P3_DIRECTORY_H
#define SOA_P3_DIRECTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "dsm.h"
#include "wire.h"

/* Resultado de un evento. Ninguno distinto de DIR_OK cambia el estado. */
enum dir_status {
    DIR_OK = 0,
    DIR_EINVAL,       /* nodo o página fuera de rango, modo desconocido,
                       * puntero nulo */
    DIR_ESTATE,       /* el evento no corresponde al estado: solicitud con
                       * otra en curso, request_id que no crece, release
                       * sin acceso, de otro modo o con otro request_id,
                       * ACK que nadie pidió o de otra ronda, despedida con
                       * un acceso vivo, evento de un nodo que ya se fue */
    DIR_DUPLICATE     /* ACK de invalidación ya contado. No es un error del
                       * protocolo que obligue a abortar: se registra y se
                       * descarta, y no cambia nada */
};

/* Tipo de acción. El coordinador la traduce a la trama indicada. */
enum dir_action_type {
    DIR_ACT_GRANT = 1,       /* -> ACQ_GRANT (con datos si data != NULL)  */
    DIR_ACT_INVALIDATE,      /* -> INVALIDATE                             */
    DIR_ACT_WRITE_PUBLISHED, /* -> REL_WRITE_ACK                          */
    DIR_ACT_READ_RELEASED    /* -> REL_READ_ACK                           */
};

struct dir_action {
    enum dir_action_type type;
    uint32_t node;           /* destinatario de la trama */
    uint32_t page_id;
    uint64_t request_id;     /* GRANT y confirmaciones: el del acceso, que es
                              * el de su ACQ_REQ. INVALIDATE: el de la
                              * solicitud WRITE que la causó (D12) */
    uint32_t requester;      /* INVALIDATE: nodo que pidió el WRITE, va en aux
                              * (D12). En las demás es igual a `node` */
    enum dsm_mode mode;      /* GRANT: modo concedido */
    uint64_t version;        /* GRANT: versión concedida.
                              * WRITE_PUBLISHED: versión recién publicada */
    const uint8_t *data;     /* GRANT con datos: DSM_PAGE_SIZE bytes, válidos
                              * solo durante la llamada a emit. NULL si la
                              * concesión va sin datos o la acción no es GRANT */
};

/* Receptor de acciones. Se invoca dentro del evento que las causa. No debe
 * llamar de vuelta al directorio. */
typedef void (*dir_emit_fn)(void *user, const struct dir_action *action);

/* Instantánea del estado de una página, para el registro CSV del coordinador
 * (versión, escritor, lectores e invalidaciones pendientes, como pide el
 * enunciado) y para las pruebas. */
struct dir_page_info {
    uint64_t version;
    int32_t  writer;          /* -1 si no hay escritor activo */
    uint8_t  readers;         /* máscara de lectores activos */
    uint8_t  holders;         /* máscara de copias retenidas vigentes */
    uint8_t  inval_pending;   /* máscara de ACK de invalidación pendientes */
    uint32_t reader_count;
    uint32_t pending_acks;
    uint32_t queued;          /* solicitudes en la cola FIFO */
};

typedef struct dir dir;

/* Crea el directorio de una sesión con `nodes` en [DSM_MIN_NODES,
 * DSM_MAX_NODES] y `pages` en [DSM_MIN_PAGES, DSM_MAX_PAGES], todas las páginas
 * en cero y versión 0. `emit` no puede ser NULL; `user` se le pasa tal cual.
 * Devuelve NULL si los argumentos son inválidos o falta memoria. */
dir *dir_create(uint32_t nodes, uint32_t pages, dir_emit_fn emit, void *user);

/* Libera el directorio. Acepta NULL. */
void dir_destroy(dir *d);

/* ACQ_REQ de `node` sobre `page_id`. `cached_version` es la que declara el
 * trabajador (DSM_NO_VERSION si no tiene copia válida). Concede de inmediato
 * si las reglas lo permiten, o encola la solicitud; en los dos casos devuelve
 * DIR_OK y lo que se concedió llega por `emit`. Un nodo tiene a lo sumo una
 * solicitud o un acceso en curso en todo el directorio, y `request_id` debe
 * ser mayor que el de su solicitud anterior (DIR_ESTATE si no). */
enum dir_status dir_acquire(dir *d, uint32_t node, uint32_t page_id,
                            enum dsm_mode mode, uint64_t request_id,
                            uint64_t cached_version);

/* INVALIDATE_ACK de `node` para `page_id`. `requester` y `request_id` son los
 * que traía el INVALIDATE (aux y request_id) y deben identificar la ronda en
 * curso. Un ACK repetido de la última ronda devuelve DIR_DUPLICATE y no
 * cuenta; uno de una ronda anterior devuelve DIR_ESTATE. */
enum dir_status dir_invalidate_ack(dir *d, uint32_t node, uint32_t page_id,
                                   uint32_t requester, uint64_t request_id);

/* REL_WRITE de `node`, que debe ser el escritor activo de `page_id`, con el
 * mismo `request_id` con que adquirió (DIR_ESTATE si no). `data`
 * son los DSM_PAGE_SIZE bytes de la página, recibidos COMPLETOS: el
 * coordinador solo llama aquí con la trama entera, así una transferencia
 * incompleta nunca se publica. Publica, sube la versión, emite
 * WRITE_PUBLISHED y después las concesiones que eso habilite. */
enum dir_status dir_release_write(dir *d, uint32_t node, uint32_t page_id,
                                  const uint8_t *data, uint64_t request_id);

/* REL_READ de `node`, que debe ser lector activo de `page_id`, con el mismo
 * `request_id` con que adquirió (DIR_ESTATE si no). Su copia queda
 * retenida y revocable. Emite READ_RELEASED y después las concesiones que eso
 * habilite. */
enum dir_status dir_release_read(dir *d, uint32_t node, uint32_t page_id,
                                 uint64_t request_id);

/* BYE de `node` (D13). El nodo no puede tener un acceso ni una solicitud en
 * curso (DIR_ESTATE si los tiene). Se lo saca de las copias retenidas de
 * todas las páginas; si alguna ronda de invalidación esperaba su ACK, la
 * despedida cuenta como ese ACK, porque su copia deja de existir, y el WRITE
 * que esperaba puede concederse. Después de esto, cualquier evento de ese
 * nodo devuelve DIR_ESTATE. Las barreras no son de este módulo. */
enum dir_status dir_node_leave(dir *d, uint32_t node);

/* Cantidad de páginas y de nodos con que se creó. */
uint32_t dir_pages(const dir *d);
uint32_t dir_nodes(const dir *d);

/* Llena *out con el estado de `page_id`. Devuelve false si está fuera de
 * rango o algún puntero es NULL. */
bool dir_page_info(const dir *d, uint32_t page_id, struct dir_page_info *out);

/* Datos confirmados de `page_id` (DSM_PAGE_SIZE bytes), solo lectura. NULL si
 * está fuera de rango. Mientras hay un escritor activo, esto sigue siendo la
 * versión anterior, que el directorio NO sirve a nadie. */
const uint8_t *dir_page_data(const dir *d, uint32_t page_id);

/* Verifica los invariantes internos de todo el directorio: a lo sumo un
 * escritor por página y, si existe, sin lectores, sin otras copias y sin ronda
 * abierta; lectores y pendientes de ACK contenidos en las copias retenidas;
 * cada nodo en un solo lugar (en la cola, leyendo o escribiendo); nada de los
 * nodos que se despidieron. Devuelve true si todo cuadra. Es O(páginas) y
 * está pensado para pruebas y para el modo de depuración del coordinador. */
bool dir_check_invariants(const dir *d);

/* Texto de un dir_status, en español y sin salto de línea final. */
const char *dir_strstatus(enum dir_status s);

#endif /* SOA_P3_DIRECTORY_H */
