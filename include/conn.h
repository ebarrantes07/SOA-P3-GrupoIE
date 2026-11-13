/* conn.h — una conexión del coordinador: buffer de entrada, buffer de salida
 * y extracción de tramas. Encabezado INTERNO.
 *
 * Es la pieza que hace posible el requisito central de #4: *"una escritura
 * parcial no bloquea la recepción de nada"*. El coordinador tiene un solo hilo
 * y un ciclo `poll()` (D0.1), así que no puede quedarse esperando a nadie: ni
 * a que un socket lento acepte el resto de una trama, ni a que un nodo
 * termine de enviar la que empezó.
 *
 *
 * POR QUÉ NO USA `frame.h` (D1.5)
 *
 * `frame_recv` lee hasta completar la trama, y `frame_send` escribe hasta
 * vaciarla. Las dos sirven al hilo receptor del trabajador, que no tiene nada
 * mejor que hacer mientras espera. En el coordinador bloquearse en un nodo
 * detendría a los otros tres, que es exactamente lo que el enunciado prohíbe.
 *
 * Acá el ciclo es el inverso: se leen los bytes que haya, se extraen las
 * tramas COMPLETAS que ya estén, y lo que quedó a medias espera en el buffer
 * hasta la próxima vuelta del `poll()`. Igual del lado de la salida: se
 * encolan tramas enteras y se escribe lo que el socket acepte.
 *
 *
 * EL NÚCLEO NO TOCA SOCKETS
 *
 * `conn_feed`, `conn_next`, `conn_queue` y `conn_take` son funciones puras
 * sobre los dos buffers. El socket aparece solo en `conn_recv` y `conn_send`,
 * que son envolturas de un intento de `io_read_some`/`io_write_some`. Esa
 * separación es deliberada: el comportamiento difícil —una trama partida en
 * pedazos arbitrarios, dos tramas en una lectura, una escritura que acepta la
 * mitad— se prueba alimentando bytes a mano, sin red, de forma determinista y
 * sin depender de cuánto quiera aceptar el núcleo ese día.
 *
 *
 * TAMAÑO DE LOS BUFFERS, Y QUÉ PASA SI SE LLENA LA SALIDA
 *
 * Los dos son fijos. El de entrada tiene que poder contener una trama
 * completa; se le da el doble para que una lectura pueda traer una trama y el
 * principio de la siguiente sin desperdiciar la llamada.
 *
 * El de salida necesita un razonamiento. Lo que un nodo puede tener encolado a
 * la vez está acotado por el protocolo: a lo sumo una concesión con página
 * (4136 bytes), porque un nodo tiene a lo sumo un acceso en curso; a lo sumo
 * N-1 invalidaciones, porque cada ronda abierta corresponde a una solicitud
 * WRITE en la cabeza de alguna página y cada una es de un nodo distinto; y
 * unas pocas confirmaciones. En total, menos de 5 KB.
 *
 * Pero eso vale para un nodo que se comporta. Un nodo que no lee su socket y
 * sigue enviando —por ejemplo, llegadas a barrera de muchas generaciones sin
 * esperar ninguna liberación— puede acumular salida sin tope. Por eso
 * `conn_queue` puede fallar, y cuando falla NO es un error del coordinador:
 * es un nodo que no respeta el protocolo. Quien llama lo trata como se trata
 * una desconexión, abortando esa sesión. Un buffer creciente solo cambiaría el
 * síntoma de "se llenó el buffer" a "se agotó la memoria del coordinador".
 */
#ifndef SOA_P3_CONN_H
#define SOA_P3_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "wire.h"

/* Entrada: una trama completa más el principio de la siguiente. */
#define CONN_IN_CAP   (2u * DSM_MAX_FRAME)

/* Salida: cómodamente por encima de los menos de 5 KB que el protocolo
 * permite acumular (ver arriba), para que llenarla signifique de verdad que el
 * par dejó de leer, y no que la cota estaba justa. */
#define CONN_OUT_CAP  16384u

/* Resultado de intentar extraer una trama del buffer de entrada. */
enum conn_frame {
    CONN_FRAME_READY = 0,  /* hay una trama completa y válida en *out_h      */
    CONN_FRAME_NONE,       /* todavía no llegó entera: esperar más bytes     */
    CONN_FRAME_BAD         /* llegó entera y es inválida; ver conn_wire(c)   */
};

struct conn {
    int    fd;

    uint8_t in[CONN_IN_CAP];
    size_t  in_head;       /* primer byte sin consumir */
    size_t  in_len;        /* bytes válidos a partir de in_head */

    uint8_t out[CONN_OUT_CAP];
    size_t  out_head;
    size_t  out_len;

    enum wire_status wire;  /* motivo del último CONN_FRAME_BAD */
    bool   peer_closed;     /* el par cerró su lado de escritura */
};

/* Deja la conexión lista sobre `fd`, que ya debe estar conectado y no
 * bloqueante (eso lo hace net.h). No toma posesión del descriptor. */
void conn_init(struct conn *c, int fd);

/* ---- Núcleo, sin sockets ---------------------------------------------- */

/* Agrega `n` bytes recibidos al buffer de entrada. Devuelve false si no
 * caben, lo que solo puede pasar si quien llama no respetó
 * conn_in_space(). */
bool conn_feed(struct conn *c, const void *bytes, size_t n);

/* Espacio libre del buffer de entrada, en bytes. Es el máximo que tiene
 * sentido leer del socket en esta vuelta. */
size_t conn_in_space(const struct conn *c);

/* Extrae la primera trama completa del buffer de entrada.
 *
 * Con CONN_FRAME_READY, *out_h queda con la cabecera y los primeros
 * out_h->length bytes de `payload` con la carga, y los bytes de esa trama se
 * consumen del buffer. `payload_cap` debe ser al menos DSM_MAX_PAYLOAD.
 *
 * Con CONN_FRAME_NONE no se consume nada: faltan bytes.
 *
 * Con CONN_FRAME_BAD la trama llegó entera y no pasó wire_check, y el motivo
 * queda en conn_wire(). NO se consume: el flujo ya no es confiable, porque si
 * la cabecera es basura su `length` también lo es y no se sabe dónde empieza
 * la trama siguiente. Quien llama aborta la sesión de ese nodo. */
enum conn_frame conn_next(struct conn *c, struct wire_header *out_h,
                          void *payload, size_t payload_cap);

/* Motivo del último CONN_FRAME_BAD. */
enum wire_status conn_wire(const struct conn *c);

/* Serializa una trama al buffer de salida. Valida la cabecera ANTES de
 * escribir un solo byte, igual que frame_send.
 *
 * Devuelve false si la cabecera es inválida —error de programación nuestro— o
 * si no cabe, que significa que el par dejó de leer (ver el encabezado). Los
 * dos casos dejan el buffer intacto: una trama a medias en la salida
 * corrompería el flujo tanto como una a medias en la entrada. */
bool conn_queue(struct conn *c, const struct wire_header *h,
                const void *payload);

/* Bytes pendientes de escribir. Es la condición para pedir POLLOUT: pedirlo
 * siempre haría que el poll() despertara sin parar. */
size_t conn_pending(const struct conn *c);

/* Consume hasta `cap` bytes de la salida. Solo lo usan las pruebas, para
 * verificar los bytes exactos que habrían salido al socket. */
size_t conn_take(struct conn *c, void *buf, size_t cap);

/* ---- Envolturas de socket, un intento cada una ------------------------ */

/* UN intento de lectura hacia el buffer de entrada. No espera, no reintenta:
 * el que espera es el poll() del ciclo. IO_AGAIN significa que no había nada,
 * IO_CLOSED que el par cerró (y queda registrado en conn_peer_closed). */
enum io_status conn_recv(struct conn *c);

/* UN intento de escritura desde el buffer de salida. Lo que el socket no
 * acepte queda encolado para la próxima vuelta: esta función no bloquea, que
 * es justamente lo que permite que una escritura parcial no detenga la
 * recepción de los demás. */
enum io_status conn_send(struct conn *c);

/* El par cerró su lado de escritura. */
bool conn_peer_closed(const struct conn *c);

#endif /* SOA_P3_CONN_H */
