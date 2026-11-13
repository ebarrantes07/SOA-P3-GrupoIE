/* session.h — registro de nodos y barreras de la sesión. Encabezado INTERNO.
 *
 * Es la contabilidad por nodo del coordinador: quién se registró, quién se
 * fue, cuándo empezó la operación en curso de cada uno, y en qué generación va
 * cada barrera. No decide nada sobre páginas —eso es el directorio— y no toca
 * sockets —eso es `conn`.
 *
 * Esa frontera es la que la vuelve verificable: todas las pruebas de este
 * módulo son secuencias de llamadas sin red y sin estado compartido, y el
 * coordinador (la rebanada siguiente) solo orquesta.
 *
 *
 * NO MAPEA NODOS A CONEXIONES
 *
 * Deliberadamente. La sesión habla de `node_id`, y qué descriptor le
 * corresponde lo sabe el coordinador. Si este módulo guardara descriptores,
 * sus pruebas necesitarían sockets para nada.
 *
 *
 * BARRERAS: TABLA FIJA, Y POR QUÉ NO SE LIBERA EL CASILLERO
 *
 * El `barrier_id` viene en `aux` y es un u32, así que no se puede indexar un
 * arreglo con él. Hay una tabla de SESS_MAX_BARRIERS casilleros, y cada uno
 * recuerda la generación ABIERTA de su barrera.
 *
 * El casillero NO se libera al liberarse la barrera: si se liberara, se
 * perdería la generación y la ronda siguiente llegaría con un número que no se
 * podría validar contra nada, que es exactamente lo que D0.8 quiere detectar.
 * Por eso una barrera, una vez usada, se queda con su casillero para siempre.
 *
 * Si se agotan los casilleros, una barrera nueva se rechaza con DSM_EINVAL en
 * vez de desalojar a otra. Desalojar sería silencioso y volvería a perder la
 * generación; rechazar se ve. Las cuatro aplicaciones del enunciado usan un
 * puñado de barreras, así que el tope no se alcanza en la práctica, pero el
 * protocolo admite cualquier u32 y el coordinador no puede confiar en eso.
 *
 *
 * LA GENERACIÓN EMPIEZA EN 0 (D0.8)
 *
 * El trabajador manda "cuántas veces ya pasó" por ese `barrier_id`, así que la
 * primera llegada de la vida de una barrera trae 0. Un casillero nuevo arranca
 * con la generación 0 abierta, y por eso una primera llegada con generación
 * distinta de 0 se rechaza en vez de aceptarse como punto de partida: un nodo
 * desincronizado se ve en su primera barrera, no 80 generaciones después.
 */
#ifndef SOA_P3_SESSION_H
#define SOA_P3_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "dsm.h"

/* Barreras distintas que una sesión puede usar. Ver arriba por qué es fijo. */
#define SESS_MAX_BARRIERS 16u

enum sess_state {
    SESS_REGISTERING = 1,  /* faltan HELLO para completar los N          */
    SESS_RUNNING,          /* los N se registraron; se mandó SESSION_START */
    SESS_ABORTED,          /* se perdió un participante o hubo un fallo   */
    SESS_FINISHED          /* todos se despidieron en orden               */
};

/* Qué hacer con una llegada a barrera. */
enum sess_barrier {
    SESS_BARRIER_WAIT = 0,  /* contada; faltan nodos                      */
    SESS_BARRIER_RELEASE,   /* completa: liberar a TODOS los nodos        */
    SESS_BARRIER_REJECT     /* duplicada, desincronizada o sin casillero  */
};

typedef struct dsm_session sess;

/* Crea la sesión. `nodes` en [DSM_MIN_NODES, DSM_MAX_NODES] y `pages` en
 * [DSM_MIN_PAGES, DSM_MAX_PAGES]; `run_id` es el de esta corrida, que el
 * coordinador manda en SESSION_START. NULL si los argumentos no sirven o
 * falta memoria. */
sess *sess_create(uint32_t nodes, uint32_t pages, uint64_t run_id);
void  sess_destroy(sess *s);

uint32_t        sess_nodes(const sess *s);
uint32_t        sess_pages(const sess *s);
uint64_t        sess_run_id(const sess *s);
enum sess_state sess_state_of(const sess *s);

/* ---- Registro ---------------------------------------------------------- */

/* HELLO de `node`, con lo que el trabajador declara.
 *
 * `nodes_expected` tiene que coincidir con la sesión. `pages_expected` puede
 * ser 0, que significa "sin opinión" y es lo que manda la biblioteca (D0.7);
 * distinto de 0 tiene que coincidir.
 *
 * Devuelve DSM_OK si quedó registrado, o el motivo del rechazo:
 *   DSM_EINVAL  node fuera de [0, nodes), o nodes/pages declarados que no
 *               coinciden con la sesión
 *   DSM_ESTATE  ese node_id ya estaba registrado, el nodo ya se despidió, o
 *               la sesión ya arrancó (incorporación tardía)
 *   DSM_EABORT  la sesión está abortada
 *
 * Cuando el último de los N se registra, el estado pasa a SESS_RUNNING: eso
 * es lo que quien llama usa para mandar SESSION_START a todos. */
enum dsm_status sess_hello(sess *s, uint32_t node, uint32_t nodes_expected,
                           uint32_t pages_expected);

bool sess_registered(const sess *s, uint32_t node);
bool sess_left(const sess *s, uint32_t node);

/* Cuántos se registraron y todavía no se fueron. */
uint32_t sess_active(const sess *s);

/* BYE de `node`. Este módulo verifica lo que le toca: que el nodo esté
 * registrado y que ninguna barrera tenga llegadas pendientes.
 *
 * Que el nodo no tenga páginas adquiridas lo verifica el directorio
 * (`dir_node_leave`), no esto: es su estado, no el nuestro. Quien llama hace
 * las dos comprobaciones (D0.13).
 *
 * Devuelve DSM_OK (salida ordenada), DSM_ESTATE (no estaba registrado o ya se
 * había ido) o DSM_EABORT, que deja la sesión en SESS_ABORTED. Dos casos
 * abortan, y por la misma razón de fondo: la política del enunciado ante la
 * pérdida de un participante es abortar, no seguir con menos nodos.
 *
 *   - Hay una barrera con llegadas sin liberar: esa barrera necesita a todos
 *     los nodos, así que irse dejaría a los demás esperando para siempre.
 *   - La sesión está todavía registrando: irse ahora la deja sin poder
 *     alcanzar los N nunca. Se aborta acá en vez de esperar a que lo
 *     descubra el plazo, que solo podría decir "nadie llegó". */
enum dsm_status sess_bye(sess *s, uint32_t node);

/* Aborta la sesión. Idempotente. Lo llama el coordinador ante una
 * desconexión, un plazo vencido o una trama inválida. */
void sess_abort(sess *s);

/* Marca que todos se despidieron en orden. Solo tiene efecto si no queda
 * ningún nodo activo y la sesión no estaba abortada. */
void sess_finish(sess *s);

/* ---- Tiempo por nodo, para la columna dur_ns del registro ------------- */

/* Marca el inicio de la operación en curso de `node`. Un nodo tiene a lo sumo
 * una operación a la vez (es un invariante del directorio), así que alcanza un
 * instante por nodo y no hace falta una tabla por (nodo, request_id). */
void sess_op_begin(sess *s, uint32_t node, uint64_t now_ns);

/* Nanosegundos desde sess_op_begin. LOG_NO_DURATION si no hay operación
 * marcada, para que la celda del CSV quede vacía en vez de decir 0 — que
 * significaría "tardó nada", no "no se sabe". */
uint64_t sess_op_elapsed(const sess *s, uint32_t node, uint64_t now_ns);

/* Cierra el reloj de `node`: su operación recibió respuesta. Idempotente. */
void sess_op_end(sess *s, uint32_t node);

/* El nodo con la espera más antigua todavía abierta, y cuándo empezó. false
 * si nadie está esperando.
 *
 * Es lo que el coordinador usa para los plazos. El reloj mide cuánto espera
 * un nodo una RESPUESTA del coordinador, no cuánto tiene una página en la
 * mano: lo segundo es trabajo de la aplicación y no tiene por qué acotarse.
 * La distinción es la que hace que el plazo detecte lo que importa — si un
 * nodo se muere teniendo una página, los demás se quedan esperando y SU
 * espera es la que vence. */
bool sess_op_oldest(const sess *s, uint32_t *out_node, uint64_t *out_start_ns);

/* ---- Barreras ---------------------------------------------------------- */

/* BARRIER_ARRIVE de `node` en `barrier_id`, declarando `generation`.
 *
 * Con SESS_BARRIER_RELEASE, la barrera se completó: quien llama manda
 * BARRIER_RELEASE con esa misma generación a todos los nodos activos, y acá la
 * generación ya avanzó a la siguiente.
 *
 * Con SESS_BARRIER_REJECT, `why` queda con el motivo. `why` puede ser NULL.
 * Es DSM_ESTATE cuando el nodo está desincronizado, que son tres casos:
 * llegada repetida en la misma generación, generación que no es la abierta, y
 * llegada a una barrera cuando ya estaba esperando en otra. El último no lo
 * puede producir un trabajador correcto —después de llegar queda bloqueado
 * hasta la liberación— y se rechaza para que se vea acá, en vez de aparecer
 * como una barrera que no libera nunca. Es DSM_EINVAL cuando el nodo no sirve
 * o se agotaron los casilleros. */
enum sess_barrier sess_barrier_arrive(sess *s, uint32_t node,
                                      uint32_t barrier_id, uint64_t generation,
                                      enum dsm_status *why);

/* Alguna barrera tiene llegadas sin liberar. Es la condición que vuelve
 * ilegítimo un BYE (D0.13). */
bool sess_barrier_pending(const sess *s);

/* Generación ABIERTA de `barrier_id`, y cuántos llegaron a ella. Para el
 * registro y las pruebas. Devuelve false si esa barrera no se usó nunca. */
bool sess_barrier_info(const sess *s, uint32_t barrier_id,
                       uint64_t *out_generation, uint32_t *out_arrived);

#endif /* SOA_P3_SESSION_H */
