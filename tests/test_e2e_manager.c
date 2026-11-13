/* Punta a punta del coordinador (#4): levanta el binario dsm_manager de
 * verdad y lo somete a las situaciones que no se pueden provocar sin
 * procesos ni sockets.
 *
 * Las demás pruebas del coordinador alimentan tramas a mano (test_manager.c).
 * Esta cubre lo que queda: el ciclo poll(), el análisis de argumentos, y los
 * criterios de #4 que hablan de perder un trabajador.
 *
 * Los plazos de las pruebas son cortos a propósito (`--timeout-ms` chico) para
 * que la suite no tarde: lo que se verifica es que el aborto ocurra DENTRO del
 * plazo configurado más un margen de limpieza, no que el plazo valga 10
 * segundos.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "frame.h"
#include "net.h"
#include "test_util.h"

#define PLAZO_MS 400u          /* plazo de operación del coordinador */
#define MARGEN_MS 2000u        /* limpieza que el criterio de #4 concede */

/* ---- Levantar el coordinador ------------------------------------------ */

struct coord {
    pid_t    pid;
    uint16_t port;
};

/* Arranca ./dsm_manager y lee el puerto que imprime. El pipe es lo que
 * sincroniza: la lectura se bloquea hasta que el socket está escuchando, así
 * que no hace falta esperar a ciegas. */
static bool coord_start_log(struct coord *c, uint32_t nodes, uint32_t pages,
                            uint32_t timeout_ms, const char *log_path) {
    int pfd[2];
    if (pipe(pfd) != 0)
        return false;

    char s_nodes[16], s_pages[16], s_to[16];
    snprintf(s_nodes, sizeof s_nodes, "%lu", (unsigned long)nodes);
    snprintf(s_pages, sizeof s_pages, "%lu", (unsigned long)pages);
    snprintf(s_to, sizeof s_to, "%lu", (unsigned long)timeout_ms);

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        /* Su stderr al vacío: los avisos de plazo vencido son correctos y
         * esperados en dos de estas pruebas, y mezclados con la salida de la
         * suite parecerían fallas. */
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        /* setsid: el coordinador queda en su propio grupo, así una señal al
         * grupo de la suite no se lo lleva por delante antes de tiempo. */
        setsid();
        if (log_path != NULL)
            execl("./dsm_manager", "dsm_manager", "--nodes", s_nodes,
                  "--pages", s_pages, "--port", "0",
                  "--timeout-ms", s_to, "--log", log_path, (char *)NULL);
        else
            execl("./dsm_manager", "dsm_manager", "--nodes", s_nodes,
                  "--pages", s_pages, "--port", "0",
                  "--timeout-ms", s_to, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    char buf[32];
    size_t n = 0;
    while (n + 1 < sizeof buf) {
        ssize_t r = read(pfd[0], buf + n, 1);
        if (r <= 0)
            break;
        if (buf[n] == '\n')
            break;
        n++;
    }
    buf[n] = '\0';
    close(pfd[0]);

    c->pid  = pid;
    c->port = (uint16_t)atoi(buf);
    if (c->port == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }
    return true;
}

static bool coord_start(struct coord *c, uint32_t nodes, uint32_t pages,
                        uint32_t timeout_ms) {
    return coord_start_log(c, nodes, pages, timeout_ms, NULL);
}

/* Espera a que termine, con un tope. Devuelve su código, o -1 si hubo que
 * matarlo. Nunca deja el proceso vivo: un coordinador huérfano se quedaría
 * con el puerto y rompería la prueba siguiente. */
static int coord_wait(struct coord *c, uint32_t limite_ms) {
    uint64_t fin = now_monotonic_ns() + (uint64_t)limite_ms * 1000000u;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(c->pid, &st, WNOHANG);
        if (r == c->pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        if (r < 0)
            return -1;
        if (now_monotonic_ns() >= fin) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            return -1;
        }
        /* Espera corta cediendo el procesador, sin ocupar el núcleo: el
         * coordinador necesita tiempo real para avanzar. */
        struct timespec ts = {0, 2000000};   /* 2 ms */
        nanosleep(&ts, NULL);
    }
}

static void coord_kill(struct coord *c) {
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
}

/* ---- Un trabajador mínimo --------------------------------------------- */

struct wk {
    int      fd;
    uint32_t nid;
    uint64_t req;
};

static struct wire_header mk(uint8_t type, uint32_t node, uint32_t page,
                             uint64_t req, uint32_t len) {
    struct wire_header h;
    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION; h.type = type; h.length = len;
    h.node_id = node; h.page_id = page; h.request_id = req;
    return h;
}

static bool wk_send(struct wk *w, const struct wire_header *h,
                    const void *pl) {
    return frame_ok(frame_send(w->fd, h, pl, io_deadline_in(3000)));
}

/* Recibe la próxima trama. Confirma sola las invalidaciones que lleguen en el
 * medio: si no, la ronda de otro nodo nunca se cierra. */
static bool wk_recv(struct wk *w, struct wire_header *h, uint8_t *pl,
                    uint32_t plazo_ms) {
    for (int i = 0; i < 30; i++) {
        struct frame_io r = frame_recv(w->fd, h, pl, DSM_MAX_PAYLOAD,
                                       io_deadline_in(plazo_ms));
        if (!frame_ok(r))
            return false;
        if (h->type != DSM_WIRE_INVALIDATE)
            return true;
        struct wire_header a = mk(DSM_WIRE_INVALIDATE_ACK, w->nid,
                                  h->page_id, h->request_id, 0);
        a.aux = h->aux;
        if (!wk_send(w, &a, NULL))
            return false;
    }
    return false;
}

static bool wk_hello(struct wk *w, const struct coord *c, uint32_t nid,
                     uint32_t nodes) {
    w->nid = nid;
    w->req = 0;
    w->fd  = -1;
    if (net_connect("127.0.0.1", c->port, &w->fd, io_deadline_in(3000))
            != NET_OK)
        return false;
    uint8_t pl[8];
    wire_put_u32(pl + 0, nodes);
    wire_put_u32(pl + 4, 0u);       /* "sin opinión" sobre las páginas */
    struct wire_header h = mk(DSM_WIRE_HELLO, nid, DSM_NO_PAGE, 0, 8);
    return wk_send(w, &h, pl);
}

static void wk_close(struct wk *w) {
    if (w->fd >= 0)
        net_close(w->fd);
    w->fd = -1;
}

/* ---- El camino que funciona ------------------------------------------- */

/* Dos trabajadores: se registran, escriben con invalidación entre medio, se
 * sincronizan en una barrera, leen y se despiden. El coordinador tiene que
 * terminar solo y con código 0. */
static bool test_happy_path(void) {
    struct coord c;
    CHECK(coord_start(&c, 2, 4, 5000));

    struct wk w[2];
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    for (uint32_t i = 0; i < 2; i++)
        CHECK(wk_hello(&w[i], &c, i, 2));

    /* SESSION_START, con lo que define el coordinador (D0.7). */
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_SESSION_START);
        CHECK(wire_get_u32(pl + 0) == 2u);
        CHECK(wire_get_u32(pl + 4) == 4u);
        CHECK(wire_get_u32(pl + 8) == DSM_PAGE_SIZE);
    }

    /* Los dos escriben la página 0, uno después del otro. */
    for (uint32_t i = 0; i < 2; i++) {
        w[i].req++;
        wire_put_u64(pl, DSM_NO_VERSION);
        struct wire_header q = mk(DSM_WIRE_ACQ_REQ, i, 0, w[i].req, 8);
        q.aux = DSM_WIRE_MODE_WRITE;
        CHECK(wk_send(&w[i], &q, pl));

        /* El otro puede tener que confirmar una invalidación para que esto
         * avance, así que se atiende a los dos mientras se espera. */
        bool concedido = false;
        for (int vuelta = 0; vuelta < 10 && !concedido; vuelta++) {
            for (uint32_t k = 0; k < 2; k++) {
                struct frame_io r = frame_recv(w[k].fd, &h, pl,
                                               DSM_MAX_PAYLOAD,
                                               io_deadline_in(200));
                if (!frame_ok(r))
                    continue;
                if (h.type == DSM_WIRE_INVALIDATE) {
                    struct wire_header a = mk(DSM_WIRE_INVALIDATE_ACK, k,
                                              h.page_id, h.request_id, 0);
                    a.aux = h.aux;
                    CHECK(wk_send(&w[k], &a, NULL));
                } else if (h.type == DSM_WIRE_ACQ_GRANT && k == i) {
                    CHECK(h.aux == DSM_WIRE_MODE_WRITE);
                    concedido = true;
                }
            }
        }
        CHECK(concedido);

        static uint8_t pagina[DSM_PAGE_SIZE];
        memset(pagina, (int)(0xC0u + i), sizeof pagina);
        struct wire_header rw = mk(DSM_WIRE_REL_WRITE, i, 0, w[i].req,
                                   DSM_PAGE_SIZE);
        CHECK(wk_send(&w[i], &rw, pagina));
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_REL_WRITE_ACK);
        CHECK(wire_get_u64(pl) == (uint64_t)i + 1u);   /* versión sube */
    }

    /* Barrera: nadie pasa hasta que lleguen los dos. */
    for (uint32_t i = 0; i < 2; i++) {
        w[i].req++;
        wire_put_u64(pl, 0u);
        struct wire_header b = mk(DSM_WIRE_BARRIER_ARRIVE, i, DSM_NO_PAGE,
                                  w[i].req, 8);
        b.aux = 1u;
        CHECK(wk_send(&w[i], &b, pl));
    }
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_BARRIER_RELEASE);
        CHECK(h.aux == 1u && wire_get_u64(pl) == 0u);
    }

    /* Los dos leen: tienen que ver la MISMA versión, la última publicada. */
    for (uint32_t i = 0; i < 2; i++) {
        w[i].req++;
        wire_put_u64(pl, DSM_NO_VERSION);
        struct wire_header q = mk(DSM_WIRE_ACQ_REQ, i, 0, w[i].req, 8);
        q.aux = DSM_WIRE_MODE_READ;
        CHECK(wk_send(&w[i], &q, pl));
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_ACQ_GRANT);
        CHECK(wire_get_u64(pl) == 2u);
        CHECK((h.flags & DSM_WIRE_F_HAS_DATA) != 0);
        CHECK(pl[8] == 0xC1);           /* lo que escribió el nodo 1 */

        struct wire_header rr = mk(DSM_WIRE_REL_READ, i, 0, w[i].req, 0);
        CHECK(wk_send(&w[i], &rr, NULL));
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_REL_READ_ACK);
    }

    for (uint32_t i = 0; i < 2; i++) {
        struct wire_header by = mk(DSM_WIRE_BYE, i, DSM_NO_PAGE, 0, 0);
        CHECK(wk_send(&w[i], &by, NULL));
    }
    for (uint32_t i = 0; i < 2; i++)
        wk_close(&w[i]);

    /* Con todos despedidos, el coordinador termina por su cuenta. */
    CHECK(coord_wait(&c, 3000) == 0);
    return true;
}

/* ---- Criterio: perder un trabajador ----------------------------------- */

/* Un trabajador que desaparece sin despedirse. El resto tiene que recibir un
 * error, no esperar indefinidamente — y acá no hace falta esperar el plazo,
 * porque el socket cerrado lo delata en el acto. */
static bool test_killed_worker_tells_the_rest(void) {
    struct coord c;
    CHECK(coord_start(&c, 3, 4, 5000));

    struct wk w[3];
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    for (uint32_t i = 0; i < 3; i++)
        CHECK(wk_hello(&w[i], &c, i, 3));
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_SESSION_START);
    }

    /* El nodo 2 se cae: se le cierra el socket sin BYE. */
    uint64_t t0 = now_monotonic_ns();
    wk_close(&w[2]);

    /* Los otros dos reciben ERROR con ECONN. */
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_ERROR);
        CHECK(h.aux == (uint32_t)DSM_ECONN);
    }
    uint64_t ms = (now_monotonic_ns() - t0) / 1000000u;
    CHECK(ms < MARGEN_MS);

    for (uint32_t i = 0; i < 2; i++)
        wk_close(&w[i]);
    CHECK(coord_wait(&c, 3000) >= 0);
    return true;
}

/* El caso difícil: un trabajador VIVO que deja de responder. El socket sigue
 * abierto, así que nada lo delata; lo único que lo detecta es el plazo.
 *
 * Es el criterio de #4 medido: el resto recibe error dentro del plazo
 * configurado más el margen de limpieza. */
static bool test_stuck_worker_hits_the_deadline(void) {
    struct coord c;
    CHECK(coord_start(&c, 2, 4, PLAZO_MS));

    struct wk w[2];
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    for (uint32_t i = 0; i < 2; i++)
        CHECK(wk_hello(&w[i], &c, i, 2));
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_SESSION_START);
    }

    /* El nodo 0 toma la página 0 para escribir y NUNCA la suelta. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header q0 = mk(DSM_WIRE_ACQ_REQ, 0, 0, 1, 8);
    q0.aux = DSM_WIRE_MODE_WRITE;
    CHECK(wk_send(&w[0], &q0, pl));
    CHECK(wk_recv(&w[0], &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_ACQ_GRANT);

    /* El nodo 1 pide la misma página: queda esperando a alguien que no va a
     * contestar nunca. */
    uint64_t t0 = now_monotonic_ns();
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header q1 = mk(DSM_WIRE_ACQ_REQ, 1, 0, 1, 8);
    q1.aux = DSM_WIRE_MODE_READ;
    CHECK(wk_send(&w[1], &q1, pl));

    /* Tiene que llegarle un ERROR de plazo, no una concesión. */
    CHECK(wk_recv(&w[1], &h, pl, PLAZO_MS + MARGEN_MS));
    CHECK(h.type == DSM_WIRE_ERROR);
    CHECK(h.aux == (uint32_t)DSM_ETIMEOUT);

    uint64_t ms = (now_monotonic_ns() - t0) / 1000000u;
    /* No antes del plazo: si abortara antes, una operación lenta pero
     * legítima moriría sin motivo. */
    CHECK(ms + 50u >= PLAZO_MS);
    /* Y no mucho después: es el criterio de #4. */
    CHECK(ms <= PLAZO_MS + MARGEN_MS);

    /* El que se colgó también se entera. */
    CHECK(wk_recv(&w[0], &h, pl, 2000));
    CHECK(h.type == DSM_WIRE_ERROR);
    CHECK(h.aux == (uint32_t)DSM_ETIMEOUT);

    wk_close(&w[0]);
    wk_close(&w[1]);
    CHECK(coord_wait(&c, 3000) >= 0);
    return true;
}

/* Si los N no llegan nunca, el coordinador no espera para siempre: el plazo
 * de registro arranca con la primera conexión. */
static bool test_registration_has_a_deadline(void) {
    struct coord c;
    CHECK(coord_start(&c, 2, 4, PLAZO_MS));

    struct wk w;
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    /* Solo uno de los dos se presenta. */
    CHECK(wk_hello(&w, &c, 0, 2));

    uint64_t t0 = now_monotonic_ns();
    CHECK(wk_recv(&w, &h, pl, PLAZO_MS + MARGEN_MS));
    CHECK(h.type == DSM_WIRE_ERROR);
    CHECK(h.aux == (uint32_t)DSM_ETIMEOUT);
    uint64_t ms = (now_monotonic_ns() - t0) / 1000000u;
    CHECK(ms + 50u >= PLAZO_MS);
    CHECK(ms <= PLAZO_MS + MARGEN_MS);

    wk_close(&w);
    CHECK(coord_wait(&c, 3000) >= 0);
    return true;
}

/* Criterio de #4: una incorporación tardía se rechaza con error explícito,
 * no cerrándole el socket. */
static bool test_late_connection_gets_an_error(void) {
    struct coord c;
    CHECK(coord_start(&c, 1, 4, 5000));

    struct wk w, tarde;
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    CHECK(wk_hello(&w, &c, 0, 1));
    CHECK(wk_recv(&w, &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_SESSION_START);

    /* La sesión ya arrancó, y aparece otro. */
    CHECK(wk_hello(&tarde, &c, 0, 1));
    CHECK(wk_recv(&tarde, &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_ERROR);
    CHECK(h.aux == (uint32_t)DSM_ESTATE);

    wk_close(&tarde);
    wk_close(&w);
    coord_kill(&c);
    return true;
}

/* ¿Y si el que muere es el coordinador?
 *
 * Hay dos casos y se comportan distinto, a propósito:
 *
 *   SIGKILL: no puede ejecutar nada. El núcleo cierra sus sockets, los
 *            trabajadores ven el cierre y eso es todo lo que se puede
 *            ofrecer. Lo detectan al instante.
 *
 *   SIGTERM/SIGINT: es un apagado deliberado, así que alcanza a avisar. Manda
 *            ERROR con DSM_EABORT y vacía la salida antes de cerrar.
 *
 * La diferencia importa para el registro del trabajador: con el aviso puede
 * decir "la sesión se abortó" en vez de "se cortó la conexión", que es
 * exactamente lo que no se sabría distinguir sin esto.
 *
 * Lo que NINGUNO de los dos cubre es que se caiga la máquina o la red: ahí no
 * llega ni un FIN, y el trabajador se queda esperando hasta que vence SU
 * propio plazo (`dsm_options.timeout_ms`). Eso es responsabilidad de #5 y no
 * hay nada que el coordinador pueda hacer al respecto. */
static bool test_coordinator_death(void) {
    const int senales[] = {SIGTERM, SIGKILL};
    for (size_t k = 0; k < 2; k++) {
        struct coord c;
        CHECK(coord_start(&c, 2, 4, 5000));

        struct wk w[2];
        struct wire_header h;
        static uint8_t pl[DSM_MAX_PAYLOAD];
        for (uint32_t i = 0; i < 2; i++)
            CHECK(wk_hello(&w[i], &c, i, 2));
        for (uint32_t i = 0; i < 2; i++) {
            CHECK(wk_recv(&w[i], &h, pl, 3000));
            CHECK(h.type == DSM_WIRE_SESSION_START);
        }

        uint64_t t0 = now_monotonic_ns();
        kill(c.pid, senales[k]);
        waitpid(c.pid, NULL, 0);

        if (senales[k] == SIGTERM) {
            /* Alcanzó a avisar: primero el ERROR, después el cierre. */
            for (uint32_t i = 0; i < 2; i++) {
                CHECK(wk_recv(&w[i], &h, pl, 3000));
                CHECK(h.type == DSM_WIRE_ERROR);
                CHECK(h.aux == (uint32_t)DSM_EABORT);
            }
        }

        /* En los dos casos, lo siguiente es el cierre del flujo, y el
         * trabajador lo ve sin esperar ningún plazo. */
        for (uint32_t i = 0; i < 2; i++) {
            struct frame_io r = frame_recv(w[i].fd, &h, pl, DSM_MAX_PAYLOAD,
                                           io_deadline_in(3000));
            CHECK(!frame_ok(r));
            CHECK(r.io == IO_CLOSED);
        }
        uint64_t ms = (now_monotonic_ns() - t0) / 1000000u;
        CHECK(ms < MARGEN_MS);

        for (uint32_t i = 0; i < 2; i++)
            wk_close(&w[i]);
    }
    return true;
}

/* Criterio de #4: "el log registra solicitud, concesión, invalidación, ACK,
 * publicación, barrera liberada y error, con secuencia global creciente".
 *
 * test_log.c ya verifica el formato con filas sintéticas. Esto verifica que
 * una corrida REAL las produzca: que los siete eventos aparezcan de verdad y
 * que la secuencia no tenga huecos ni repeticiones a lo largo de toda la
 * sesión. */
static bool test_log_of_a_real_run(void) {
    const char *ruta = "build/test_e2e_run.csv";
    remove(ruta);

    struct coord c;
    CHECK(coord_start_log(&c, 2, 4, 5000, ruta));

    struct wk w[2];
    struct wire_header h;
    static uint8_t pl[DSM_MAX_PAYLOAD];

    for (uint32_t i = 0; i < 2; i++)
        CHECK(wk_hello(&w[i], &c, i, 2));
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_SESSION_START);
    }

    /* n1 lee y suelta: queda con copia retenida, que es lo que fuerza una
     * invalidación cuando n0 escriba. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header r = mk(DSM_WIRE_ACQ_REQ, 1, 0, 1, 8);
    r.aux = DSM_WIRE_MODE_READ;
    CHECK(wk_send(&w[1], &r, pl));
    CHECK(wk_recv(&w[1], &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_ACQ_GRANT);
    struct wire_header rr = mk(DSM_WIRE_REL_READ, 1, 0, 1, 0);
    CHECK(wk_send(&w[1], &rr, NULL));
    CHECK(wk_recv(&w[1], &h, pl, 3000));

    /* n0 escribe: invalidación, ACK (lo manda wk_recv solo), concesión. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header q = mk(DSM_WIRE_ACQ_REQ, 0, 0, 1, 8);
    q.aux = DSM_WIRE_MODE_WRITE;
    CHECK(wk_send(&w[0], &q, pl));
    bool concedido = false;
    for (int v = 0; v < 20 && !concedido; v++) {
        for (uint32_t k = 0; k < 2; k++) {
            struct frame_io fr = frame_recv(w[k].fd, &h, pl, DSM_MAX_PAYLOAD,
                                            io_deadline_in(200));
            if (!frame_ok(fr))
                continue;
            if (h.type == DSM_WIRE_INVALIDATE) {
                struct wire_header a = mk(DSM_WIRE_INVALIDATE_ACK, k,
                                          h.page_id, h.request_id, 0);
                a.aux = h.aux;
                CHECK(wk_send(&w[k], &a, NULL));
            } else if (h.type == DSM_WIRE_ACQ_GRANT && k == 0) {
                concedido = true;
            }
        }
    }
    CHECK(concedido);

    static uint8_t pagina[DSM_PAGE_SIZE];
    memset(pagina, 0x3C, sizeof pagina);
    struct wire_header rw = mk(DSM_WIRE_REL_WRITE, 0, 0, 1, DSM_PAGE_SIZE);
    CHECK(wk_send(&w[0], &rw, pagina));
    CHECK(wk_recv(&w[0], &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_REL_WRITE_ACK);

    /* Una barrera, para que quede su liberación. */
    for (uint32_t i = 0; i < 2; i++) {
        wire_put_u64(pl, 0u);
        struct wire_header b = mk(DSM_WIRE_BARRIER_ARRIVE, i, DSM_NO_PAGE,
                                  2, 8);
        b.aux = 1u;
        CHECK(wk_send(&w[i], &b, pl));
    }
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(wk_recv(&w[i], &h, pl, 3000));
        CHECK(h.type == DSM_WIRE_BARRIER_RELEASE);
    }

    /* Y un error: una página que no existe en esta sesión. */
    wire_put_u64(pl, DSM_NO_VERSION);
    struct wire_header mala = mk(DSM_WIRE_ACQ_REQ, 0, 200, 3, 8);
    mala.aux = DSM_WIRE_MODE_READ;
    CHECK(wk_send(&w[0], &mala, pl));
    CHECK(wk_recv(&w[0], &h, pl, 3000));
    CHECK(h.type == DSM_WIRE_ERROR);

    for (uint32_t i = 0; i < 2; i++) {
        struct wire_header by = mk(DSM_WIRE_BYE, i, DSM_NO_PAGE, 0, 0);
        CHECK(wk_send(&w[i], &by, NULL));
        wk_close(&w[i]);
    }
    CHECK(coord_wait(&c, 3000) == 0);

    /* Ahora se audita el archivo. */
    FILE *f = fopen(ruta, "r");
    CHECK(f != NULL);

    char linea[1024];
    CHECK(fgets(linea, sizeof linea, f) != NULL);
    CHECK(strncmp(linea, "run_id,component,seq,", 21) == 0);

    const char *obligatorios[] = {
        "ACQ_REQ", "ACQ_GRANT", "INVALIDATE", "INVALIDATE_ACK",
        "WRITE_PUBLISHED", "BARRIER_RELEASE", "ERROR",
    };
    bool visto[7] = {false};
    unsigned long esperada = 1, filas = 0;
    bool seq_ok = true;

    while (fgets(linea, sizeof linea, f) != NULL) {
        filas++;
        /* La secuencia es la tercera columna y tiene que crecer de uno en
         * uno, sin huecos ni repeticiones, a lo largo de TODA la sesión. */
        char *c1 = strchr(linea, ',');
        char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
        char *c3 = c2 ? strchr(c2 + 1, ',') : NULL;
        if (c2 == NULL || c3 == NULL) { seq_ok = false; break; }
        if (strtoul(c2 + 1, NULL, 10) != esperada) { seq_ok = false; break; }
        esperada++;

        for (size_t i = 0; i < 7; i++) {
            char patron[32];
            snprintf(patron, sizeof patron, ",%s,", obligatorios[i]);
            if (strstr(linea, patron) != NULL)
                visto[i] = true;
        }
    }
    fclose(f);

    CHECK(seq_ok);
    CHECK(filas > 10);
    for (size_t i = 0; i < 7; i++) {
        if (!visto[i])
            fprintf(stderr, "  falta el evento %s en el registro\n",
                    obligatorios[i]);
        CHECK(visto[i]);
    }
    return true;
}

/* Argumentos: lo que no sirve se rechaza con un código distinto de 0 y sin
 * quedarse escuchando. */
static bool test_bad_arguments_are_rejected(void) {
    const char *malos[][3] = {
        {"--nodes", "0", NULL},
        {"--nodes", "99", NULL},
        {"--nodes", "2", "--pages"},      /* valor faltante */
        {"--nodes", "dos", NULL},
        {"--qué", "2", NULL},
    };
    for (size_t i = 0; i < sizeof malos / sizeof malos[0]; i++) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); }
            execl("./dsm_manager", "dsm_manager", malos[i][0], malos[i][1],
                  malos[i][2], (char *)NULL);
            _exit(127);
        }
        int st = 0;
        CHECK(waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st));
        CHECK(WEXITSTATUS(st) != 0);
        CHECK(WEXITSTATUS(st) != 127);   /* se ejecutó de verdad */
    }
    return true;
}

int main(void) {
    /* Si un trabajador de prueba cierra antes, no queremos morir por
     * SIGPIPE: el transporte ya devuelve el error. */
    signal(SIGPIPE, SIG_IGN);

    const struct test_case tests[] = {
        {"bad_arguments_are_rejected", test_bad_arguments_are_rejected},
        {"happy_path", test_happy_path},
        {"late_connection_gets_an_error", test_late_connection_gets_an_error},
        {"killed_worker_tells_the_rest", test_killed_worker_tells_the_rest},
        {"stuck_worker_hits_the_deadline", test_stuck_worker_hits_the_deadline},
        {"registration_has_a_deadline", test_registration_has_a_deadline},
        {"log_of_a_real_run", test_log_of_a_real_run},
        {"coordinator_death", test_coordinator_death},
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]);
}
