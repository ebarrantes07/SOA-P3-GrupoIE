/* frame.h — enviar y recibir tramas completas. Encabezado INTERNO.
 *
 * Compone los dos módulos de abajo y no agrega protocolo propio:
 *
 *   wire.h   bytes  <-> campos      (sin red)
 *   io.h     bytes  <-> socket      (sin protocolo)
 *   frame.h  tramas <-> socket      (los dos juntos)
 *
 * Lo usa el hilo receptor del trabajador. El coordinador NO lo usa: su ciclo
 * poll() no puede bloquearse, así que arma las tramas con su propio buffer de
 * entrada y wire_decode_header (ver D1 y D16).
 *
 * EL FLUJO NO SE RECUPERA DE UNA TRAMA A MEDIAS. Si frame_recv falla después
 * de haber leído la cabecera, los bytes de carga que faltaban siguen en
 * camino y ya no hay forma de saber dónde empieza la trama siguiente: TCP
 * entrega bytes en orden pero no dice dónde termina cada mensaje. Por eso
 * cualquier fallo de transporte a mitad de una trama obliga a abortar la
 * sesión, no a reintentar.
 */
#ifndef SOA_P3_FRAME_H
#define SOA_P3_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "wire.h"

/* Las dos clases de fallo se devuelven separadas en vez de fundidas en un
 * solo código: quien llama reacciona igual a casi todas (abortar la sesión),
 * pero el registro tiene que distinguir "el par cerró" de "la trama era
 * inválida", y el informe tiene que poder contar cada una. */
struct frame_io {
    enum io_status   io;     /* IO_OK si el transporte funcionó */
    enum wire_status wire;   /* WIRE_OK si la trama es válida */
};

static inline bool frame_ok(struct frame_io r) {
    return r.io == IO_OK && r.wire == WIRE_OK;
}

/* Valida la cabecera, la serializa y la envía junto con su carga.
 *
 * La validación va primero y es a propósito: una cabecera incoherente se
 * rechaza sin escribir un solo byte, así un error de programación de un lado
 * no puede desalinear el flujo del otro. `payload` puede ser NULL solo si
 * h->length es 0.
 *
 * UN SOLO EMISOR POR DESCRIPTOR A LA VEZ. Si varios hilos envían por el mismo
 * socket, quien llama serializa: esta función no lleva candado. La trama se
 * arma completa y se entrega en una sola llamada a io_send_all, pero una
 * escritura parcial puede partirla en varias llamadas al sistema, y entre dos
 * de ellas otro hilo podría meter sus bytes en el medio. El resultado es un
 * flujo corrupto del que no se vuelve, porque el receptor usa `length` como
 * única autoridad de dónde termina cada trama (ver el aviso de arriba).
 *
 * En el trabajador esto pasa: envían el hilo de aplicación (ACQ_REQ, los
 * releases, las barreras) y el hilo receptor (INVALIDATE_ACK). La
 * serialización vive ahí, con un mutex de envío por contexto (#5). */
struct frame_io frame_send(int fd, const struct wire_header *h,
                           const void *payload, uint64_t deadline_ns);

/* Recibe una trama: 32 bytes de cabecera, se decodifica y valida, y después
 * los h->length bytes de carga que la cabecera anuncia.
 *
 * `payload_cap` debe ser al menos DSM_MAX_PAYLOAD, que es la carga de la
 * trama más grande del protocolo. Si una carga legítima no cupiera, la
 * función igual la consume del socket antes de reportar el error: dejar los
 * bytes a medio leer mataría el flujo, y es mejor degradar un error de
 * programación a un error de protocolo que corromper la conexión.
 *
 * En éxito, *out_h queda con la cabecera y los primeros h->length bytes de
 * `payload` con la carga. */
struct frame_io frame_recv(int fd, struct wire_header *out_h,
                           void *payload, size_t payload_cap,
                           uint64_t deadline_ns);

#endif /* SOA_P3_FRAME_H */
