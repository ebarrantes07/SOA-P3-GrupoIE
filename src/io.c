/* Transporte a prueba de transferencias parciales. Ver include/io.h para el
 * contrato y docs/decisiones_diseno.md (D16) para por qué hay dos niveles.
 */
#include "io.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "cli.h"
#include "clock.h"

/* ---- Límite de fragmentación ------------------------------------------ */

#define CHUNK_LIMIT_MAX  65536u

static pthread_once_t  chunk_once = PTHREAD_ONCE_INIT;
static _Atomic size_t  chunk_limit = 0;   /* 0 = sin límite */

/* El valor del entorno se lee UNA vez, con pthread_once: el hilo receptor y
 * el de aplicación pueden llegar acá a la vez, y una inicialización perezosa
 * con una bandera común sería una carrera que ThreadSanitizer reportaría con
 * razón. El valor en sí es atómico porque las pruebas lo cambian. */
static void load_chunk_limit(void) {
    const char *s = getenv("DSM_IO_CHUNK_MAX");
    uint32_t v = 0;

    /* Un valor inválido se ignora en silencio en vez de escribir a stderr:
     * esto es una biblioteca y no le corresponde contaminar la salida del
     * programa. Quien quiera verificar qué quedó configurado llama a
     * io_chunk_limit, y el registro del trabajador lo anota. */
    if (s != NULL && cli_parse_u32(s, 1u, CHUNK_LIMIT_MAX, &v))
        atomic_store(&chunk_limit, (size_t)v);
}

static size_t chunk_cap(size_t len) {
    size_t lim;

    pthread_once(&chunk_once, load_chunk_limit);
    lim = atomic_load(&chunk_limit);
    return (lim != 0u && len > lim) ? lim : len;
}

void io_set_chunk_limit(size_t max_bytes) {
    /* Se fuerza la carga del entorno antes de pisarla, para que un
     * pthread_once posterior no vuelva a aplicar la variable encima del valor
     * que puso la prueba. */
    pthread_once(&chunk_once, load_chunk_limit);
    atomic_store(&chunk_limit, max_bytes);
}

size_t io_chunk_limit(void) {
    pthread_once(&chunk_once, load_chunk_limit);
    return atomic_load(&chunk_limit);
}

/* ---- Estados ---------------------------------------------------------- */

const char *io_strstatus(enum io_status s) {
    switch (s) {
    case IO_OK:      return "completo";
    case IO_AGAIN:   return "ahora no se puede mover nada";
    case IO_CLOSED:  return "el par cerró la conexión";
    case IO_TIMEOUT: return "venció el plazo";
    case IO_ERROR:   return "error del sistema";
    }
    return "estado desconocido";
}

/* Traduce un errno de send/recv al estado correspondiente. El par que se va
 * se reporta como IO_CLOSED en sus tres formas: cierre ordenado (recv
 * devuelve 0, que se maneja aparte), tubería rota y reinicio por el par. */
static enum io_status status_from_errno(int e) {
    if (e == EAGAIN || e == EWOULDBLOCK)
        return IO_AGAIN;
    if (e == EPIPE || e == ECONNRESET || e == ENOTCONN || e == ESHUTDOWN)
        return IO_CLOSED;
    return IO_ERROR;
}

/* ---- Un intento ------------------------------------------------------- */

enum io_status io_write_some(int fd, const void *buf, size_t len, size_t *moved) {
    if (moved != NULL)
        *moved = 0;
    if (len == 0u)
        return IO_OK;

    for (;;) {
        /* MSG_NOSIGNAL: escribir a un par que cerró devuelve EPIPE en vez de
         * levantar SIGPIPE, cuya acción por omisión es matar el proceso. Sin
         * esto, una desconexión —que es un caso de prueba obligatorio— haría
         * desaparecer al trabajador sin dejar registro. */
        ssize_t n = send(fd, buf, chunk_cap(len), MSG_NOSIGNAL);
        if (n > 0) {
            if (moved != NULL)
                *moved = (size_t)n;
            return IO_OK;
        }
        if (n == 0)
            return IO_CLOSED;
        if (errno == EINTR)
            continue;          /* no pasó nada todavía; volver a intentar */
        return status_from_errno(errno);
    }
}

enum io_status io_read_some(int fd, void *buf, size_t len, size_t *moved) {
    if (moved != NULL)
        *moved = 0;
    if (len == 0u)
        return IO_OK;

    for (;;) {
        ssize_t n = recv(fd, buf, chunk_cap(len), 0);
        if (n > 0) {
            if (moved != NULL)
                *moved = (size_t)n;
            return IO_OK;
        }
        if (n == 0)
            return IO_CLOSED;  /* fin de archivo: el par cerró ordenadamente */
        if (errno == EINTR)
            continue;
        return status_from_errno(errno);
    }
}

/* ---- Espera ----------------------------------------------------------- */

uint64_t io_deadline_in(uint32_t ms) {
    if (ms == 0u)
        return 0u;
    return now_monotonic_ns() + (uint64_t)ms * 1000000ull;
}

/* Milisegundos que faltan para el plazo, para poll. Devuelve -1 si no hay
 * plazo (esperar indefinidamente) y 0 si ya venció. */
static int poll_timeout_ms(uint64_t deadline_ns) {
    uint64_t now;

    if (deadline_ns == 0u)
        return -1;
    now = now_monotonic_ns();
    if (now >= deadline_ns)
        return 0;
    uint64_t left_ms = (deadline_ns - now) / 1000000ull;
    if (left_ms > (uint64_t)INT32_MAX)
        return INT32_MAX;
    /* Redondear hacia arriba para no devolver 0 y convertir la espera en un
     * ciclo de sondeo cuando falta menos de un milisegundo. */
    if (left_ms == 0u)
        return 1;
    return (int)left_ms;
}

enum io_status io_wait(int fd, bool for_write, uint64_t deadline_ns) {
    for (;;) {
        struct pollfd p;
        p.fd = fd;
        p.events = for_write ? POLLOUT : POLLIN;
        p.revents = 0;

        int ms = poll_timeout_ms(deadline_ns);
        if (ms == 0)
            return IO_TIMEOUT;

        int r = poll(&p, 1, ms);
        if (r > 0) {
            /* POLLHUP y POLLERR no se devuelven como IO_CLOSED acá: con
             * POLLIN todavía puede quedar dato por leer en el buffer, y es
             * recv quien debe reportar el fin. Devolver listo y dejar que el
             * intento siguiente decida evita perder los últimos bytes de una
             * conexión que ya cerró del otro lado. */
            if ((p.revents & (POLLERR | POLLNVAL)) != 0 && (p.revents & p.events) == 0)
                return IO_CLOSED;
            return IO_OK;
        }
        if (r == 0)
            return IO_TIMEOUT;
        if (errno == EINTR)
            continue;          /* la espera se reanuda con el plazo recalculado */
        return IO_ERROR;
    }
}

/* ---- El ciclo completo ------------------------------------------------ */

enum io_status io_send_all(int fd, const void *buf, size_t len, uint64_t deadline_ns) {
    const unsigned char *p = buf;
    size_t left = len;

    while (left > 0u) {
        size_t n = 0;
        enum io_status st = io_write_some(fd, p, left, &n);

        if (st == IO_OK) {
            p += n;
            left -= n;
            continue;
        }
        if (st != IO_AGAIN)
            return st;

        st = io_wait(fd, true, deadline_ns);
        if (st != IO_OK)
            return st;
    }
    return IO_OK;
}

enum io_status io_recv_all(int fd, void *buf, size_t len, uint64_t deadline_ns) {
    unsigned char *p = buf;
    size_t left = len;

    while (left > 0u) {
        size_t n = 0;
        enum io_status st = io_read_some(fd, p, left, &n);

        if (st == IO_OK) {
            p += n;
            left -= n;
            continue;
        }
        if (st != IO_AGAIN)
            return st;

        st = io_wait(fd, false, deadline_ns);
        if (st != IO_OK)
            return st;
    }
    return IO_OK;
}
