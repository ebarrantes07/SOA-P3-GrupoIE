/* net.h — crear y aceptar conexiones TCP. Encabezado INTERNO.
 *
 * Las pruebas obligatorias corren por loopback, pero el enunciado pide que el
 * diseño acepte direcciones IP configurables para la corrida complementaria
 * en dos equipos. Por eso tanto el coordinador como el trabajador reciben la
 * dirección como texto y la resuelven con getaddrinfo, que sirve para un
 * nombre o para una IP literal, de la familia que sea.
 *
 * TODO SOCKET QUE SALE DE ACÁ queda configurado igual:
 *
 *   - NO BLOQUEANTE. Es la precondición que io.h documenta para que los
 *     plazos se respeten: con un descriptor bloqueante, recv puede dormir
 *     indefinidamente dentro del sistema y el plazo solo acotaría las esperas
 *     que llegan a ver EAGAIN. Sin esto no habría forma de cumplir el timeout
 *     de 10 segundos ni de detectar una desconexión.
 *   - TCP_NODELAY (D17). Sin él, el algoritmo de Nagle retiene mensajes de
 *     control chicos esperando juntarlos con los siguientes, y los
 *     experimentos medirían el buffer de TCP en vez del protocolo.
 */
#ifndef SOA_P3_NET_H
#define SOA_P3_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum net_status {
    NET_OK = 0,
    NET_ADDR,       /* la dirección no se pudo resolver o es inválida */
    NET_TIMEOUT,    /* venció el plazo */
    NET_ERROR       /* error del sistema; errno queda con el motivo */
};

const char *net_strstatus(enum net_status s);

/* Abre un socket de escucha en `host`:`port`.
 *
 * `host` NULL escucha en todas las interfaces. `port` 0 pide uno efímero al
 * sistema, y entonces *bound_port dice cuál quedó — las pruebas lo usan para
 * no depender de que un puerto fijo esté libre.
 *
 * El socket lleva SO_REUSEADDR: sin él, volver a escuchar en el mismo puerto
 * justo después de cerrar falla mientras la conexión anterior está en
 * TIME_WAIT, y la suite no podría correr dos veces seguidas. */
enum net_status net_listen(const char *host, uint16_t port, int backlog,
                           int *out_fd, uint16_t *bound_port);

/* Acepta una conexión pendiente, esperando hasta el plazo. */
enum net_status net_accept(int listen_fd, int *out_fd, uint64_t deadline_ns);

/* Conecta con `host`:`port`. Retorna cuando la conexión está establecida o
 * vence el plazo; un rechazo del otro extremo es NET_ERROR con ECONNREFUSED. */
enum net_status net_connect(const char *host, uint16_t port, int *out_fd,
                            uint64_t deadline_ns);

/* Cierra sin perder el errno de quien llama. */
void net_close(int fd);

/* Dirección del par en texto, para los registros: "127.0.0.1:54321". Devuelve
 * false si no se pudo obtener; en ese caso `out` queda con una cadena vacía. */
bool net_peer_name(int fd, char *out, size_t cap);

#endif /* SOA_P3_NET_H */
