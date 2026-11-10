/* test_net.c — pruebas de las conexiones TCP (#2).
 *
 * Todo por loopback y con PUERTO EFÍMERO: la suite no puede depender de que
 * un puerto fijo esté libre en la máquina de quien la corra. El puerto que
 * toca se consulta con net_listen y de ahí sale el que usa el que conecta.
 */
#include "clock.h"
#include "frame.h"
#include "net.h"
#include "test_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOOPBACK  "127.0.0.1"

/* Levanta un par conectado por TCP: escucha en un puerto efímero, conecta y
 * acepta. Alcanza con un hilo: el sistema completa el saludo de TCP usando la
 * cola del socket que escucha, así que connect termina antes de que nadie
 * llame a accept. */
static bool connected_pair(int *listen_fd, int *client, int *server,
                           uint16_t *port) {
    CHECK(net_listen(LOOPBACK, 0, 4, listen_fd, port) == NET_OK);
    CHECK(*port != 0);
    CHECK(net_connect(LOOPBACK, *port, client, io_deadline_in(5000)) == NET_OK);
    CHECK(net_accept(*listen_fd, server, io_deadline_in(5000)) == NET_OK);
    return true;
}

static bool test_listen_on_ephemeral_port(void) {
    int fd = -1, fd2 = -1;
    uint16_t a = 0, b = 0;

    CHECK(net_listen(LOOPBACK, 0, 4, &fd, &a) == NET_OK);
    CHECK(fd >= 0);
    CHECK(a != 0);

    /* Dos escuchas simultáneas obtienen puertos distintos. */
    CHECK(net_listen(LOOPBACK, 0, 4, &fd2, &b) == NET_OK);
    CHECK(b != 0);
    CHECK(a != b);

    net_close(fd);
    net_close(fd2);
    return true;
}

static bool test_connect_and_exchange_frame(void) {
    int lfd = -1, c = -1, s = -1;
    uint16_t port = 0;
    struct wire_header h, out;
    uint8_t payload[DSM_MAX_PAYLOAD], got[DSM_MAX_PAYLOAD];

    CHECK(connected_pair(&lfd, &c, &s, &port));

    /* Una trama de verdad por una conexión de verdad: la concesión con la
     * página completa, que es la más grande del protocolo. */
    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type    = DSM_WIRE_ACQ_GRANT;
    h.flags   = DSM_WIRE_F_HAS_DATA;
    h.length  = 8u + DSM_PAGE_SIZE;
    h.node_id = 2;
    h.page_id = 5;
    h.request_id = 77;
    for (uint32_t i = 0; i < h.length; i++)
        payload[i] = (uint8_t)(i * 13u + 5u);

    CHECK(frame_ok(frame_send(s, &h, payload, io_deadline_in(5000))));
    CHECK(frame_ok(frame_recv(c, &out, got, sizeof got, io_deadline_in(5000))));
    CHECK(out.type == DSM_WIRE_ACQ_GRANT);
    CHECK(out.length == h.length);
    CHECK(out.request_id == 77u);
    CHECK(memcmp(payload, got, h.length) == 0);

    /* Y en la otra dirección. */
    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type    = DSM_WIRE_INVALIDATE_ACK;
    h.node_id = 2;
    h.page_id = 5;
    h.request_id = 78;
    CHECK(frame_ok(frame_send(c, &h, NULL, io_deadline_in(5000))));
    CHECK(frame_ok(frame_recv(s, &out, got, sizeof got, io_deadline_in(5000))));
    CHECK(out.type == DSM_WIRE_INVALIDATE_ACK);
    CHECK(out.request_id == 78u);

    net_close(c);
    net_close(s);
    net_close(lfd);
    return true;
}

/* Las dos propiedades que el resto del proyecto da por sentadas. */
static bool test_sockets_are_configured(void) {
    int lfd = -1, c = -1, s = -1;
    uint16_t port = 0;
    int fds[3];

    CHECK(connected_pair(&lfd, &c, &s, &port));
    fds[0] = c;
    fds[1] = s;
    fds[2] = lfd;

    for (int i = 0; i < 3; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        CHECK(fl >= 0);
        CHECK((fl & O_NONBLOCK) != 0);     /* precondición del plazo en io.h */
    }

    /* TCP_NODELAY en los conectados (D17). No aplica al que escucha. */
    for (int i = 0; i < 2; i++) {
        int on = 0;
        socklen_t len = sizeof on;
        CHECK(getsockopt(fds[i], IPPROTO_TCP, TCP_NODELAY, &on, &len) == 0);
        CHECK(on != 0);
    }

    net_close(c);
    net_close(s);
    net_close(lfd);
    return true;
}

static bool test_accept_respects_deadline(void) {
    int lfd = -1, fd = -1;
    uint16_t port = 0;
    uint64_t t0, elapsed_ms;

    CHECK(net_listen(LOOPBACK, 0, 4, &lfd, &port) == NET_OK);

    t0 = now_monotonic_ns();
    CHECK(net_accept(lfd, &fd, io_deadline_in(60)) == NET_TIMEOUT);
    elapsed_ms = (now_monotonic_ns() - t0) / 1000000ull;
    CHECK(elapsed_ms >= 50u);
    CHECK(elapsed_ms < 5000u);
    CHECK(fd == -1);

    net_close(lfd);
    return true;
}

static bool test_connect_refused(void) {
    int lfd = -1, fd = -1;
    uint16_t port = 0;

    /* Se toma un puerto efímero y se cierra: nadie escucha ahí. */
    CHECK(net_listen(LOOPBACK, 0, 4, &lfd, &port) == NET_OK);
    net_close(lfd);

    CHECK(net_connect(LOOPBACK, port, &fd, io_deadline_in(2000)) == NET_ERROR);
    CHECK(fd == -1);
    return true;
}

static bool test_bad_address_is_rejected(void) {
    int fd = -1;
    uint16_t port = 0;

    /* 203.0.113.0/24 es el bloque TEST-NET-3 de la RFC 5737: reservado para
     * documentación y que no está en ninguna interfaz, así que el bind falla
     * sin tocar la red ni depender de resolución de nombres. */
    CHECK(net_listen("203.0.113.1", 0, 4, &fd, &port) == NET_ERROR);
    CHECK(fd == -1);

    /* Un host nulo al conectar no tiene a dónde ir. */
    CHECK(net_connect(NULL, 9, &fd, io_deadline_in(1000)) == NET_ADDR);
    return true;
}

static bool test_peer_name(void) {
    int lfd = -1, c = -1, s = -1;
    uint16_t port = 0;
    char name[64];

    CHECK(connected_pair(&lfd, &c, &s, &port));

    /* El servidor ve al cliente en loopback, con el puerto efímero que le
     * tocó; el cliente ve al servidor en el puerto que pidió. */
    CHECK(net_peer_name(s, name, sizeof name));
    CHECK(strncmp(name, LOOPBACK ":", strlen(LOOPBACK) + 1) == 0);

    CHECK(net_peer_name(c, name, sizeof name));
    char expected[64];
    (void)snprintf(expected, sizeof expected, "%s:%u", LOOPBACK, (unsigned)port);
    CHECK(strcmp(name, expected) == 0);

    /* Un descriptor sin par no tiene nombre, y la salida queda vacía. */
    name[0] = 'x';
    CHECK(!net_peer_name(lfd, name, sizeof name));
    CHECK(name[0] == '\0');

    net_close(c);
    net_close(s);
    net_close(lfd);
    return true;
}

/* ---- Fragmentación sobre TCP de verdad ------------------------------
 *
 * Las pruebas de 1 a 7 bytes de test_frame.c corren sobre socketpair, que es
 * un par conectado pero de dominio UNIX. Este caso repite lo mismo sobre una
 * conexión TCP por loopback, que es el transporte que el proyecto va a usar:
 * cambian el encolado, el tamaño de los buffers y, con TCP_NODELAY, la
 * política de envío, así que vale ejercitarlo también acá.
 */
struct tcp_sender {
    int  fd;
    bool ok;
};

static void *tcp_frame_sender(void *arg) {
    struct tcp_sender *t = arg;
    struct wire_header h;

    memset(&h, 0, sizeof h);
    h.version = DSM_WIRE_VERSION;
    h.type    = DSM_WIRE_BARRIER_ARRIVE;
    h.node_id = 3;
    h.page_id = DSM_NO_PAGE;
    h.aux     = 1;
    h.length  = 8u;
    h.request_id = 4242;

    uint8_t payload[8];
    wire_put_u64(payload, 99u);

    t->ok = frame_ok(frame_send(t->fd, &h, payload, io_deadline_in(20000)));
    return NULL;
}

static bool test_fragmented_over_tcp(void) {
    int lfd = -1, c = -1, s = -1;
    uint16_t port = 0;
    pthread_t th;
    struct tcp_sender t;
    struct wire_header out;
    uint8_t got[DSM_MAX_PAYLOAD];

    CHECK(connected_pair(&lfd, &c, &s, &port));

    /* Un byte por llamada al sistema, en las dos direcciones. */
    io_set_chunk_limit(1);

    t.fd = s;
    t.ok = false;
    CHECK(pthread_create(&th, NULL, tcp_frame_sender, &t) == 0);
    CHECK(frame_ok(frame_recv(c, &out, got, sizeof got, io_deadline_in(20000))));
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(t.ok);

    CHECK(out.type == DSM_WIRE_BARRIER_ARRIVE);
    CHECK(out.request_id == 4242u);
    CHECK(out.aux == 1u);
    CHECK(out.length == 8u);
    CHECK(wire_get_u64(got) == 99u);

    io_set_chunk_limit(0);
    net_close(c);
    net_close(s);
    net_close(lfd);
    return true;
}

static bool test_statuses(void) {
    static const enum net_status all[] = {
        NET_OK, NET_ADDR, NET_TIMEOUT, NET_ERROR,
    };

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = net_strstatus(all[i]);
        CHECK(s != NULL && s[0] != '\0');
    }
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"listen_on_ephemeral_port", test_listen_on_ephemeral_port},
        {"connect_and_exchange_frame", test_connect_and_exchange_frame},
        {"sockets_are_configured", test_sockets_are_configured},
        {"accept_respects_deadline", test_accept_respects_deadline},
        {"connect_refused", test_connect_refused},
        {"bad_address_is_rejected", test_bad_address_is_rejected},
        {"peer_name", test_peer_name},
        {"fragmented_over_tcp", test_fragmented_over_tcp},
        {"statuses", test_statuses},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
