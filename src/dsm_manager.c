/* dsm_manager — el coordinador. Un proceso, un hilo, un ciclo poll() (D0.1).
 *
 * Este archivo es DELGADO a propósito: analiza argumentos, abre el socket de
 * escucha y mueve bytes entre los sockets y los buffers de `conn`. Toda la
 * lógica del protocolo vive en `manager.c`, que no conoce descriptores, y por
 * eso se puede probar sin red (ver include/manager.h).
 *
 * Lo único que este archivo decide es CUÁNDO atender a quién, y eso es
 * exactamente lo que el enunciado pide que no se bloquee: se pide POLLOUT
 * solo cuando hay algo encolado, y una escritura que acepta la mitad deja el
 * resto para la vuelta siguiente sin detener la lectura de nadie.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "clock.h"
#include "log.h"
#include "manager.h"
#include "net.h"

/* Lo pone el manejador de señales y lo lee el ciclo. volatile sig_atomic_t
 * es el único tipo que el estándar garantiza seguro para eso. */
static volatile sig_atomic_t pedido_de_salida = 0;

static void on_signal(int sig) {
    (void)sig;
    pedido_de_salida = 1;
}

struct opciones {
    const char *host;
    uint16_t    port;
    uint32_t    nodes;
    uint32_t    pages;
    uint32_t    timeout_ms;
    const char *log_path;
    uint64_t    run_id;
};

static void uso(const char *prog) {
    fprintf(stderr,
        "uso: %s --nodes N [opciones]\n"
        "\n"
        "  --nodes N          trabajadores esperados, %u..%u (obligatorio)\n"
        "  --pages P          páginas de la región, %u..%u (por omisión 16)\n"
        "  --host H           dirección donde escuchar (por omisión todas)\n"
        "  --port P           puerto; 0 toma uno libre y lo imprime\n"
        "  --timeout-ms MS    plazo por operación (por omisión %u)\n"
        "  --log RUTA         registro CSV de la corrida\n"
        "  --run-id N         identificador de corrida; 0 lo toma del reloj\n",
        prog, DSM_MIN_NODES, DSM_MAX_NODES, DSM_MIN_PAGES, DSM_MAX_PAGES,
        DSM_DEFAULT_TIMEOUT_MS);
}

/* Devuelve false si los argumentos no sirven, dejando el motivo impreso. */
static bool parse_args(int argc, char **argv, struct opciones *o) {
    o->host       = NULL;
    o->port       = 0;
    o->nodes      = 0;
    o->pages      = 16;
    o->timeout_ms = DSM_DEFAULT_TIMEOUT_MS;
    o->log_path   = NULL;
    o->run_id     = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool tiene_valor = (i + 1 < argc);
        const char *v = tiene_valor ? argv[i + 1] : NULL;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            uso(argv[0]);
            return false;
        }
        if (!tiene_valor) {
            fprintf(stderr, "%s: falta el valor de %s\n", argv[0], a);
            return false;
        }

        if (strcmp(a, "--nodes") == 0) {
            if (!cli_parse_u32(v, DSM_MIN_NODES, DSM_MAX_NODES, &o->nodes))
                goto malo;
        } else if (strcmp(a, "--pages") == 0) {
            if (!cli_parse_u32(v, DSM_MIN_PAGES, DSM_MAX_PAGES, &o->pages))
                goto malo;
        } else if (strcmp(a, "--port") == 0) {
            if (!cli_parse_u16(v, 0, UINT16_MAX, &o->port))
                goto malo;
        } else if (strcmp(a, "--timeout-ms") == 0) {
            if (!cli_parse_u32(v, 1, 3600000u, &o->timeout_ms))
                goto malo;
        } else if (strcmp(a, "--run-id") == 0) {
            if (!cli_parse_u64(v, 0, UINT64_MAX, &o->run_id))
                goto malo;
        } else if (strcmp(a, "--host") == 0) {
            o->host = v;
        } else if (strcmp(a, "--log") == 0) {
            o->log_path = v;
        } else {
            fprintf(stderr, "%s: opción desconocida: %s\n", argv[0], a);
            return false;
        }
        i++;
        continue;
    malo:
        fprintf(stderr, "%s: valor inválido para %s: %s\n", argv[0], a, v);
        return false;
    }

    if (o->nodes == 0) {
        fprintf(stderr, "%s: falta --nodes\n", argv[0]);
        uso(argv[0]);
        return false;
    }
    return true;
}

/* Atiende lo legible de una conexión: lee una vez y despacha las tramas
 * COMPLETAS que hayan quedado. Lo que llegó a medias espera la vuelta
 * siguiente, que es lo que impide bloquearse con un nodo lento. */
static enum mgr_action servir_lectura(mgr *m, int ci, uint64_t now) {
    struct conn *c = mgr_conn(m, ci);
    enum io_status st = conn_recv(c);
    if (st == IO_ERROR)
        return MGR_ABORT;

    uint8_t pl[DSM_MAX_PAYLOAD];
    struct wire_header h;
    for (;;) {
        enum conn_frame f = conn_next(c, &h, pl, sizeof pl);
        if (f == CONN_FRAME_NONE)
            break;
        if (f == CONN_FRAME_BAD) {
            /* El flujo ya no es confiable (D3.2): no hay dónde
             * resincronizar, así que se corta esta conexión. */
            mgr_send_error(m, ci, DSM_EPROTO, now);
            return MGR_ABORT;
        }
        enum mgr_action a = mgr_dispatch(m, ci, &h, pl, now);
        if (a != MGR_KEEP)
            return a;
    }

    /* El cierre se evalúa DESPUÉS de despachar lo que ya había llegado: los
     * bytes que el par alcanzó a enviar antes de cerrar son válidos. */
    if (st == IO_CLOSED)
        return MGR_CLOSE;
    return MGR_KEEP;
}

/* Vacía lo encolado en todas las conexiones, con un tope de tiempo. Es UN
 * esfuerzo razonable, no una garantía: si un par dejó de leer, no hay forma de
 * entregarle nada y no vale la pena colgarse por él. */
static void vaciar_salidas(mgr *m, uint32_t limite_ms) {
    uint64_t plazo = io_deadline_in(limite_ms);
    for (;;) {
        int pendientes = 0;
        for (int ci = 0; ci < mgr_conn_count(m); ci++) {
            struct conn *c = mgr_conn(m, ci);
            if (c == NULL || conn_pending(c) == 0)
                continue;
            enum io_status st = conn_send(c);
            if (st == IO_ERROR || st == IO_CLOSED)
                continue;      /* ese par ya no escucha; se lo deja */
            if (conn_pending(c) > 0)
                pendientes++;
        }
        if (pendientes == 0)
            return;

        /* Esperar a que alguna acepte más bytes. Se espera en la primera
         * pendiente: para el apagado no vale la pena armar un poll() nuevo
         * con todas, porque lo que queda son unas decenas de bytes. */
        int fd = -1;
        for (int ci = 0; ci < mgr_conn_count(m) && fd < 0; ci++) {
            struct conn *c = mgr_conn(m, ci);
            if (c != NULL && conn_pending(c) > 0)
                fd = mgr_conn_fd(m, ci);
        }
        if (fd < 0 || io_wait(fd, true, plazo) != IO_OK)
            return;            /* venció el plazo, o el par se fue */
    }
}

int main(int argc, char **argv) {
    struct opciones o;
    if (!parse_args(argc, argv, &o))
        return 2;

    /* SIGINT y SIGTERM no matan al proceso: marcan la salida para que el
     * registro se cierre y los nodos reciban un ERROR en vez de un socket
     * que se corta sin explicación. SIGPIPE se ignora porque el transporte
     * usa MSG_NOSIGNAL y maneja EPIPE como un error normal (D1.3). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    uint64_t run_id = o.run_id != 0 ? o.run_id : now_monotonic_ns();

    dsm_log *lg = NULL;
    if (o.log_path != NULL) {
        lg = log_open(o.log_path, "coordinador", run_id);
        if (lg == NULL) {
            fprintf(stderr, "%s: no se pudo abrir el registro %s\n",
                    argv[0], o.log_path);
            return 1;
        }
    }

    mgr *m = mgr_create(o.nodes, o.pages, run_id, lg);
    if (m == NULL) {
        fprintf(stderr, "%s: no se pudo crear la sesión\n", argv[0]);
        log_close(lg);
        return 1;
    }

    int lfd = -1;
    uint16_t bound = 0;
    if (net_listen(o.host, o.port, (int)o.nodes + 1, &lfd, &bound) != NET_OK) {
        fprintf(stderr, "%s: no se pudo escuchar: %s\n", argv[0],
                strerror(errno));
        mgr_destroy(m);
        log_close(lg);
        return 1;
    }
    /* El puerto va a stdout y en una sola línea, para que un script pueda
     * leerlo cuando se pide 0. */
    printf("%u\n", (unsigned)bound);
    fflush(stdout);

    int salida = 0;
    const uint64_t timeout_ns = (uint64_t)o.timeout_ms * 1000000u;

    while (!pedido_de_salida) {
        struct pollfd fds[MGR_MAX_CONNS + 1];
        int idx[MGR_MAX_CONNS + 1];
        int nf = 0;

        /* Se deja de aceptar cuando no queda casillero. */
        bool hay_lugar = false;
        for (int ci = 0; ci < mgr_conn_count(m); ci++)
            if (!mgr_conn_live(m, ci))
                hay_lugar = true;
        if (hay_lugar) {
            fds[nf].fd = lfd;
            fds[nf].events = POLLIN;
            fds[nf].revents = 0;
            idx[nf] = -1;
            nf++;
        }

        for (int ci = 0; ci < mgr_conn_count(m); ci++) {
            if (!mgr_conn_live(m, ci))
                continue;
            struct conn *c = mgr_conn(m, ci);
            fds[nf].fd = mgr_conn_fd(m, ci);
            /* POLLOUT solo si hay algo encolado: pedirlo siempre haría que
             * poll() despertara sin parar. */
            fds[nf].events = POLLIN | (conn_pending(c) > 0 ? POLLOUT : 0);
            fds[nf].revents = 0;
            idx[nf] = ci;
            nf++;
        }

        if (nf == 0)
            break;   /* ni escucha ni conexiones: nada que esperar */

        /* El plazo del poll() es lo que falta para la espera más próxima a
         * vencer, no el plazo completo: si no, una espera que arrancó a
         * mitad de la vuelta anterior se detectaría hasta un plazo entero
         * tarde. */
        int espera_ms = mgr_poll_timeout_ms(m, now_monotonic_ns(),
                                            o.timeout_ms);
        int rc = poll(fds, (nfds_t)nf, espera_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "%s: poll: %s\n", argv[0], strerror(errno));
            salida = 1;
            break;
        }

        uint64_t now = now_monotonic_ns();

        /* Se revisan los plazos SIEMPRE, no solo cuando el poll() se va en
         * blanco: con cuatro nodos, el tráfico de los que sí responden hace
         * que el poll() devuelva actividad constantemente, y una espera
         * vencida de otro nodo nunca se vería. */
        if (mgr_check_timeouts(m, now, timeout_ns)) {
            fprintf(stderr, "dsm_manager: plazo de %u ms vencido; "
                            "se aborta la sesión\n", o.timeout_ms);
            salida = 1;
        }

        for (int i = 0; i < nf && rc > 0; i++) {
            if (fds[i].revents == 0)
                continue;

            if (idx[i] < 0) {
                int nfd = -1;
                if (net_accept(lfd, &nfd, 0) != NET_OK)
                    continue;
                if (mgr_accept_conn(m, nfd) < 0) {
                    net_close(nfd);
                    continue;
                }
                /* Desde acá se acota la espera por los N: antes no, porque el
                 * coordinador se arranca a mano y puede estar un rato largo
                 * sin que nadie se conecte. */
                mgr_registration_clock(m, now);
                continue;
            }

            int ci = idx[i];
            if (!mgr_conn_live(m, ci))
                continue;
            struct conn *c = mgr_conn(m, ci);

            /* Primero escribir: vaciar la salida libera al par para que
             * siga enviando, y lo que no acepte queda para después. */
            if ((fds[i].revents & POLLOUT) != 0) {
                enum io_status ws = conn_send(c);
                if (ws == IO_ERROR || ws == IO_CLOSED) {
                    mgr_drop_conn(m, ci, now);
                    continue;
                }
            }

            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                continue;

            enum mgr_action a = servir_lectura(m, ci, now);
            if (a == MGR_KEEP)
                continue;

            /* Antes de cerrar se intenta vaciar lo encolado: el ERROR o el
             * último ACK le sirven al par para distinguir un cierre normal
             * de uno abrupto. Es UN intento, no una espera. */
            (void)conn_send(c);
            int fd = mgr_conn_fd(m, ci);
            if (a == MGR_ABORT) {
                mgr_abort_session(m, DSM_EABORT, DSM_MAX_NODES, now);
                salida = 1;
            }
            mgr_drop_conn(m, ci, now);
            net_close(fd);
        }

        if (sess_state_of(mgr_session(m)) == SESS_FINISHED)
            break;
        if (sess_state_of(mgr_session(m)) == SESS_ABORTED) {
            /* Se da una vuelta más para que los ERROR encolados salgan, y se
             * termina. */
            for (int ci = 0; ci < mgr_conn_count(m); ci++) {
                struct conn *c = mgr_conn(m, ci);
                if (c != NULL && conn_pending(c) > 0)
                    (void)conn_send(c);
            }
            salida = 1;
            break;
        }
    }

    /* Si salimos por una señal, los trabajadores merecen saber por qué. Sin
     * esto solo ven el socket cerrado, que es indistinguible de que el
     * coordinador se haya muerto de golpe: con el ERROR, el registro del
     * trabajador puede decir "la sesión se abortó" en vez de "se cortó la
     * conexión". */
    if (pedido_de_salida) {
        uint64_t now = now_monotonic_ns();
        if (sess_state_of(mgr_session(m)) != SESS_ABORTED)
            mgr_abort_session(m, DSM_EABORT, DSM_MAX_NODES, now);
        fprintf(stderr, "dsm_manager: señal recibida; se avisa y se cierra\n");
        salida = 1;
    }
    vaciar_salidas(m, 500);

    for (int ci = 0; ci < mgr_conn_count(m); ci++) {
        int fd = mgr_conn_fd(m, ci);
        if (fd >= 0)
            net_close(fd);
    }
    net_close(lfd);
    mgr_destroy(m);
    if (!log_close(lg))
        salida = 1;
    return salida;
}
