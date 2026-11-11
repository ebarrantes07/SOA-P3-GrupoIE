/* io.h — envío y recepción sobre sockets, a prueba de transferencias
 * parciales. Encabezado INTERNO.
 *
 * El módulo trabaja sobre SOCKETS, no sobre tuberías ni archivos: usa send y
 * recv con MSG_NOSIGNAL para que escribir a un par que ya cerró devuelva un
 * error en vez de matar el proceso con SIGPIPE. Las pruebas usan socketpair.
 *
 * Dos niveles a propósito (ver docs/decisiones_diseno.md, D1.1):
 *
 *   - UN INTENTO: io_read_some / io_write_some. Mueven lo que se pueda ahora y
 *     devuelven IO_AGAIN si no se puede nada. Es lo que necesita el ciclo
 *     poll() del coordinador (D0.1), que no puede bloquearse nunca: hace un
 *     intento, guarda lo que quedó pendiente en su buffer y sigue atendiendo
 *     a los demás.
 *   - EL CICLO COMPLETO: io_recv_all / io_send_all. Insisten hasta mover todos
 *     los bytes pedidos o hasta que venza el plazo, esperando disponibilidad
 *     con poll cuando no hay nada que hacer. Es lo que necesita el hilo
 *     receptor del trabajador.
 *
 * PRECONDICIÓN DEL PLAZO: para que el plazo se respete, el descriptor debe ser
 * no bloqueante. Con un descriptor bloqueante estas funciones son correctas,
 * pero recv puede dormir indefinidamente dentro del sistema y el plazo solo
 * acota las esperas que llegan a ver EAGAIN. net.c pone todos sus sockets en
 * modo no bloqueante justamente por esto.
 *
 * NINGUNA ESPERA ACTIVA: cuando no hay nada que mover, se espera en poll. No
 * hay reintentos en ciclo ni sleep en ningún camino.
 */
#ifndef SOA_P3_IO_H
#define SOA_P3_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum io_status {
    IO_OK = 0,      /* se movieron todos los bytes pedidos */
    IO_AGAIN,       /* ahora no se puede mover nada (solo el nivel de un intento) */
    IO_CLOSED,      /* el par cerró ordenadamente, o la conexión se rompió */
    IO_TIMEOUT,     /* venció el plazo */
    IO_ERROR        /* error del sistema; errno queda con el motivo */
};

/* Texto de un estado, en español y sin salto de línea final. Nunca NULL. */
const char *io_strstatus(enum io_status s);

/* ---- Un intento ------------------------------------------------------
 *
 * Mueven entre 1 y `len` bytes y escriben la cantidad en *moved (que puede
 * ser NULL). IO_OK significa que se movió al menos un byte, no que se movieron
 * todos: quien llama tiene que mirar *moved y seguir desde ahí.
 *
 * EINTR se reintenta adentro, porque significa que no pasó nada todavía y
 * quien llama no gana nada enterándose. */
enum io_status io_write_some(int fd, const void *buf, size_t len, size_t *moved);
enum io_status io_read_some(int fd, void *buf, size_t len, size_t *moved);

/* ---- Espera ----------------------------------------------------------
 *
 * Espera hasta que el descriptor esté listo para leer (for_write = false) o
 * para escribir (for_write = true). `deadline_ns` es un instante del reloj
 * monotónico; 0 significa sin plazo, y entonces espera indefinidamente.
 * Devuelve IO_OK si quedó listo, IO_TIMEOUT si venció el plazo, IO_CLOSED si
 * el par se fue, o IO_ERROR. */
enum io_status io_wait(int fd, bool for_write, uint64_t deadline_ns);

/* ---- El ciclo completo -----------------------------------------------
 *
 * Insisten hasta mover exactamente `len` bytes. IO_OK solo se devuelve con
 * todo movido; en cualquier otro caso parte de los bytes puede haberse movido
 * ya, y la conexión queda en un estado indeterminado desde el punto de vista
 * del protocolo: una trama a medio enviar no se puede retomar, hay que
 * abortar la sesión. */
enum io_status io_send_all(int fd, const void *buf, size_t len, uint64_t deadline_ns);
enum io_status io_recv_all(int fd, void *buf, size_t len, uint64_t deadline_ns);

/* Instante límite a partir de ahora, con el reloj monotónico. `ms` en 0
 * devuelve 0, que estas funciones interpretan como "sin plazo". */
uint64_t io_deadline_in(uint32_t ms);

/* ---- Límite de fragmentación -----------------------------------------
 *
 * Tope de bytes por operación, para ejercitar el camino de transferencia
 * parcial a voluntad. El enunciado pide una prueba que limite cada envío y
 * cada recepción a entre 1 y 7 bytes; con esto la prueba de punta a punta
 * corre los binarios reales sin recompilar nada.
 *
 * Se configura con la variable de entorno DSM_IO_CHUNK_MAX, leída una sola
 * vez. Un valor fuera de [1, 65536] se ignora. 0 significa sin límite.
 * io_set_chunk_limit es para las pruebas unitarias: se llama antes de empezar
 * a usar los descriptores, no en medio del tráfico. */
void   io_set_chunk_limit(size_t max_bytes);
size_t io_chunk_limit(void);

#endif /* SOA_P3_IO_H */
