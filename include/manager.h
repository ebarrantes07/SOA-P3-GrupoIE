/* manager.h — el coordinador: traduce entre tramas y los tres módulos de
 * estado. Encabezado INTERNO.
 *
 * Acá se junta todo. El directorio (D2.1) decide a quién conceder, la sesión
 * (D3.3) lleva los participantes y las barreras, `conn` (D3.2) mueve bytes y
 * el registro (D3.1) deja la traza. Este módulo es el único que conoce a los
 * cuatro, y su trabajo es exactamente ese: traducir.
 *
 *   trama recibida  ->  evento al directorio o a la sesión
 *   acción emitida  ->  trama encolada en la conexión del destinatario
 *
 * No hay lógica de coherencia acá: si una regla de SWMR estuviera escrita en
 * este archivo, estaría en el lugar equivocado.
 *
 *
 * SIN SOCKETS, OTRA VEZ
 *
 * `mgr_dispatch` recibe una trama ya decodificada y `mgr_accept_conn` registra
 * una conexión por su índice, no por su descriptor. El ciclo `poll()` vive en
 * `src/dsm_manager.c` y lo único que hace es mover bytes entre los sockets y
 * los buffers de `conn`.
 *
 * Esa frontera permite probar el protocolo completo —registro, concesiones,
 * invalidaciones, barreras, rechazos— alimentando tramas a mano, de forma
 * determinista y sin procesos. Lo que queda del lado del socket es tan poco
 * que su prueba natural es la de punta a punta.
 *
 *
 * NODO Y CONEXIÓN SON COSAS DISTINTAS
 *
 * Una conexión existe desde que se acepta, y recién con su `HELLO` se le
 * asocia un `node_id`. Por eso hay dos índices: el de conexión (el casillero
 * donde vive el buffer) y el `node_id` (lo que entiende el protocolo). Antes
 * del HELLO, una conexión no tiene nodo; después, el mapeo es uno a uno.
 */
#ifndef SOA_P3_MANAGER_H
#define SOA_P3_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "conn.h"
#include "directory.h"
#include "log.h"
#include "session.h"

/* Conexiones simultáneas. Se aceptan hasta una más que los nodos esperados
 * para poder RECHAZAR a un recién llegado con un ERROR explicativo en vez de
 * cerrarle el socket en la cara: el criterio de #4 pide que una incorporación
 * tardía se rechace "con error explícito". */
#define MGR_MAX_CONNS (DSM_MAX_NODES + 1u)

/* Qué debe hacer el ciclo con una conexión después de atenderla. */
enum mgr_action {
    MGR_KEEP = 0,    /* seguir atendiéndola                                */
    MGR_CLOSE,       /* vaciar lo pendiente y cerrarla; la sesión sigue    */
    MGR_ABORT        /* abortar la sesión entera                           */
};

typedef struct dsm_manager mgr;

/* Crea el coordinador de una sesión de `nodes` nodos y `pages` páginas.
 * `lg` puede ser NULL (sin registro). Devuelve NULL si los argumentos no
 * sirven o falta memoria; no toma posesión de `lg`. */
mgr *mgr_create(uint32_t nodes, uint32_t pages, uint64_t run_id, dsm_log *lg);
void mgr_destroy(mgr *m);

sess *mgr_session(mgr *m);
dir  *mgr_directory(mgr *m);

/* ---- Conexiones -------------------------------------------------------- */

/* Registra una conexión aceptada sobre `fd` y devuelve su índice, o -1 si no
 * hay lugar. El `fd` solo se guarda para que el ciclo lo encuentre; este
 * módulo no lee ni escribe en él. */
int  mgr_accept_conn(mgr *m, int fd);

/* Da de baja una conexión. Si tenía un nodo asociado, el nodo cuenta como
 * perdido: se lo saca del directorio y la sesión se aborta, porque la política
 * del enunciado ante la pérdida de un participante es abortar (D0.13). Una
 * conexión sin nodo (nunca mandó HELLO) se descarta sin más.
 *
 * `now_ns` por el mismo motivo que en mgr_dispatch: sacar un nodo puede
 * desbloquear concesiones para otros, y esas concesiones llevan su duración
 * al registro. */
void mgr_drop_conn(mgr *m, int ci, uint64_t now_ns);

struct conn *mgr_conn(mgr *m, int ci);
int  mgr_conn_count(const mgr *m);
int  mgr_conn_fd(const mgr *m, int ci);
bool mgr_conn_live(const mgr *m, int ci);

/* Índice de conexión de un nodo, o -1. Y el nodo de una conexión, o -1 si
 * todavía no mandó HELLO. */
int  mgr_conn_of_node(const mgr *m, uint32_t node);
int  mgr_node_of_conn(const mgr *m, int ci);

/* ---- El trabajo ------------------------------------------------------- */

/* Atiende una trama recibida por la conexión `ci`.
 *
 * `now_ns` es el instante en que llegó, con reloj monotónico: de acá sale la
 * columna de duración del registro y el control de plazos. Se pasa en vez de
 * leerse adentro para que las pruebas sean deterministas, igual que en el
 * registro (D3.1).
 *
 * Todo lo que haya que responder queda encolado en los buffers de salida de
 * las conexiones correspondientes; esta función nunca escribe a un socket.
 *
 * EL REGISTRO MUESTRA EL ANTES Y EL DESPUÉS. Una fila de trama recibida
 * (ACQ_REQ, REL_WRITE, INVALIDATE_ACK…) lleva el estado de la página tal como
 * estaba cuando la trama llegó, y la fila de la acción que esa trama provoca
 * (ACQ_GRANT, WRITE_PUBLISHED…) lleva el estado resultante. Por eso la fila de
 * un INVALIDATE_ACK todavía muestra la invalidación como pendiente: deja de
 * estarlo en la fila siguiente. Es a propósito, y es lo que vuelve auditable
 * cada transición sin tener que adivinar en qué momento se tomó la foto.
 *
 * Una trama que el protocolo no admite en ese momento no cambia nada de
 * estado: se le encola un ERROR al emisor con el motivo y se sigue. Lo que
 * devuelve MGR_ABORT es lo que no tiene arreglo: una trama inválida en el
 * flujo, un nodo perdido, o una salida que deja una barrera sin completar. */
enum mgr_action mgr_dispatch(mgr *m, int ci, const struct wire_header *h,
                             const void *payload, uint64_t now_ns);

/* Le encola un ERROR a `ci` con ese código. Lo usa el ciclo para los fallos
 * que detecta él: trama ilegible, plazo vencido, aborto de la sesión. */
void mgr_send_error(mgr *m, int ci, enum dsm_status st, uint64_t now_ns);

/* Le encola un ERROR a todos los nodos vivos menos `except_node`
 * (DSM_MAX_NODES para no excluir a nadie) y aborta la sesión. Es lo que hace
 * que "el resto de los nodos recibe error en lugar de esperar
 * indefinidamente". */
void mgr_abort_session(mgr *m, enum dsm_status st, uint32_t except_node,
                       uint64_t now_ns);

#endif /* SOA_P3_MANAGER_H */
