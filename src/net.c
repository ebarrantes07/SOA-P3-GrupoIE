/* Conexiones TCP. Ver include/net.h y docs/decisiones_diseno.md (D17). */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dsm.h"
#include "io.h"

const char *net_strstatus(enum net_status s) {
    switch (s) {
    case NET_OK:      return "conexión establecida";
    case NET_ADDR:    return "dirección inválida o no resoluble";
    case NET_TIMEOUT: return "venció el plazo";
    case NET_ERROR:   return "error del sistema";
    }
    return "estado desconocido";
}

void net_close(int fd) {
    int saved = errno;

    if (fd >= 0)
        (void)close(fd);
    errno = saved;      /* cerrar no debe pisar el motivo del fallo anterior */
}

static bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static bool set_nodelay(int fd) {
    int on = 1;

    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) == 0;
}

/* Deja el descriptor como lo espera el resto del proyecto. */
static bool configure(int fd) {
    return set_nonblocking(fd) && set_nodelay(fd);
}

/* El puerto se pasa a getaddrinfo como texto porque es lo que su interfaz
 * acepta; 0 significa que el sistema elige uno. */
static void port_text(uint16_t port, char *buf, size_t cap) {
    (void)snprintf(buf, cap, "%u", (unsigned)port);
}

enum net_status net_listen(const char *host, uint16_t port, int backlog,
                           int *out_fd, uint16_t *bound_port) {
    struct addrinfo hints, *res = NULL, *it = NULL;
    char service[16];
    int fd = -1;

    if (out_fd == NULL)
        return NET_ERROR;
    *out_fd = -1;
    if (bound_port != NULL)
        *bound_port = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;          /* IPv4 o IPv6, lo que resuelva */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;          /* host NULL = todas las interfaces */
    port_text(port, service, sizeof service);

    if (getaddrinfo(host, service, &hints, &res) != 0)
        return NET_ADDR;

    /* Se prueba cada dirección resuelta y se usa la primera que funcione: con
     * host NULL pueden venir una IPv6 y una IPv4, y no todas las máquinas
     * tienen las dos configuradas. */
    for (it = res; it != NULL; it = it->ai_next) {
        int on = 1;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
            continue;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
            bind(fd, it->ai_addr, it->ai_addrlen) != 0 ||
            listen(fd, backlog > 0 ? backlog : (int)DSM_MAX_NODES) != 0 ||
            !configure(fd)) {
            net_close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (fd < 0)
        return NET_ERROR;

    /* Con puerto efímero hay que preguntar cuál tocó: es la única forma de
     * que otro proceso pueda conectarse. */
    if (bound_port != NULL) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;

        if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
            net_close(fd);
            return NET_ERROR;
        }
        if (ss.ss_family == AF_INET)
            *bound_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        else if (ss.ss_family == AF_INET6)
            *bound_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }

    *out_fd = fd;
    return NET_OK;
}

enum net_status net_accept(int listen_fd, int *out_fd, uint64_t deadline_ns) {
    if (out_fd == NULL)
        return NET_ERROR;
    *out_fd = -1;

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);

        if (fd >= 0) {
            if (!configure(fd)) {
                net_close(fd);
                return NET_ERROR;
            }
            *out_fd = fd;
            return NET_OK;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return NET_ERROR;

        /* Nada pendiente: se espera disponibilidad con el mismo mecanismo que
         * usa el transporte, en vez de duplicar la lógica de poll. */
        switch (io_wait(listen_fd, false, deadline_ns)) {
        case IO_OK:      break;
        case IO_TIMEOUT: return NET_TIMEOUT;
        default:         return NET_ERROR;
        }
    }
}

enum net_status net_connect(const char *host, uint16_t port, int *out_fd,
                            uint64_t deadline_ns) {
    struct addrinfo hints, *res = NULL, *it = NULL;
    char service[16];
    enum net_status last = NET_ERROR;

    if (out_fd == NULL)
        return NET_ERROR;
    *out_fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    port_text(port, service, sizeof service);

    if (host == NULL || getaddrinfo(host, service, &hints, &res) != 0)
        return NET_ADDR;

    for (it = res; it != NULL; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        int err = 0;
        socklen_t len = sizeof err;

        if (fd < 0)
            continue;

        /* No bloqueante ANTES de conectar: así connect retorna de inmediato
         * con EINPROGRESS y el plazo se puede respetar. Con un descriptor
         * bloqueante, connect esperaría el tiempo que el sistema decida. */
        if (!configure(fd)) {
            net_close(fd);
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            *out_fd = fd;
            freeaddrinfo(res);
            return NET_OK;
        }
        if (errno != EINPROGRESS) {
            net_close(fd);
            last = NET_ERROR;
            continue;
        }

        /* Un socket en curso de conexión se vuelve escribible cuando la
         * conexión termina, con éxito o con fallo; SO_ERROR dice cuál fue. */
        switch (io_wait(fd, true, deadline_ns)) {
        case IO_OK:
            break;
        case IO_TIMEOUT:
            net_close(fd);
            freeaddrinfo(res);
            return NET_TIMEOUT;
        default:
            net_close(fd);
            last = NET_ERROR;
            continue;
        }

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            net_close(fd);
            errno = err;
            last = NET_ERROR;
            continue;
        }

        *out_fd = fd;
        freeaddrinfo(res);
        return NET_OK;
    }

    freeaddrinfo(res);
    return last;
}

bool net_peer_name(int fd, char *out, size_t cap) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    /* Tamaños explícitos en vez de NI_MAXHOST y NI_MAXSERV, que son
     * extensiones de glibc y quedan ocultas con _POSIX_C_SOURCE. La forma
     * textual más larga de una dirección IPv6 es
     * "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255", de 45 caracteres, más
     * un posible sufijo de zona; 64 bytes sobran. Un puerto son 5 dígitos. */
    char ip[64], port[8];

    if (out == NULL || cap == 0u)
        return false;
    out[0] = '\0';

    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return false;
    if (getnameinfo((struct sockaddr *)&ss, len, ip, sizeof ip, port, sizeof port,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return false;

    (void)snprintf(out, cap, "%s:%s", ip, port);
    return true;
}
