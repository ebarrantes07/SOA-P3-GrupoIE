/* test_io.c — pruebas del transporte (#2).
 *
 * Todo corre sobre un socketpair: un par conectado sin puertos ni direcciones,
 * así la suite no depende de que nada esté libre en la máquina.
 *
 * Lo que estas pruebas persiguen es el camino de transferencia parcial, que es
 * el que el enunciado va a forzar limitando cada operación a entre 1 y 7
 * bytes, y los tres finales que no son "se movieron todos los bytes": el par
 * que se va, el plazo que vence, y la señal que interrumpe.
 */
#include "clock.h"
#include "io.h"
#include "test_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static bool make_pair(int fds[2], bool nonblock) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;
    if (nonblock) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(fds[i], F_GETFL, 0);
            if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) != 0)
                return false;
        }
    }
    return true;
}

static void fill_pattern(unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)(i * 31u + 7u);
}

/* ---- El límite de fragmentación ------------------------------------- */

static bool test_chunk_limit_caps_transfers(void) {
    int fds[2];
    unsigned char out[64];
    size_t moved = 0;

    CHECK(make_pair(fds, true));
    fill_pattern(out, sizeof out);

    io_set_chunk_limit(1);
    CHECK(io_chunk_limit() == 1u);
    CHECK(io_write_some(fds[0], out, sizeof out, &moved) == IO_OK);
    CHECK(moved == 1u);          /* se pidieron 64 y se movió 1 */

    io_set_chunk_limit(7);
    CHECK(io_write_some(fds[0], out, sizeof out, &moved) == IO_OK);
    CHECK(moved == 7u);

    io_set_chunk_limit(0);       /* sin límite */
    CHECK(io_write_some(fds[0], out, sizeof out, &moved) == IO_OK);
    CHECK(moved == sizeof out);

    /* Y del lado de la lectura, lo mismo. */
    io_set_chunk_limit(3);
    unsigned char in[64];
    CHECK(io_read_some(fds[1], in, sizeof in, &moved) == IO_OK);
    CHECK(moved == 3u);

    io_set_chunk_limit(0);
    close(fds[0]);
    close(fds[1]);
    return true;
}

/* ---- El ciclo completo bajo fragmentación --------------------------- */

struct xfer {
    int             fd;
    unsigned char  *buf;
    size_t          len;
    enum io_status  st;
};

static void *sender(void *arg) {
    struct xfer *x = arg;
    x->st = io_send_all(x->fd, x->buf, x->len, io_deadline_in(20000));
    return NULL;
}

/* Dos hilos y un plazo amplio: el que envía y el que recibe se turnan a
 * través del buffer del socket, así que el camino de EAGAIN más poll se
 * ejercita de verdad en las dos direcciones. Con un solo hilo y una carga
 * mayor que el buffer, el envío se quedaría esperando a un lector que nunca
 * va a correr. */
static bool roundtrip_fragmented(size_t len, size_t limit) {
    int fds[2];
    unsigned char *out = malloc(len);
    unsigned char *in = malloc(len);
    pthread_t th;
    struct xfer x;
    bool ok = false;

    CHECK(out != NULL && in != NULL);
    CHECK(make_pair(fds, true));
    fill_pattern(out, len);
    memset(in, 0, len);
    io_set_chunk_limit(limit);

    x.fd = fds[0];
    x.buf = out;
    x.len = len;
    x.st = IO_ERROR;

    CHECK(pthread_create(&th, NULL, sender, &x) == 0);
    enum io_status rst = io_recv_all(fds[1], in, len, io_deadline_in(20000));
    CHECK(pthread_join(th, NULL) == 0);

    CHECK(rst == IO_OK);
    CHECK(x.st == IO_OK);
    CHECK(memcmp(out, in, len) == 0);
    ok = true;

    io_set_chunk_limit(0);
    close(fds[0]);
    close(fds[1]);
    free(out);
    free(in);
    return ok;
}

static bool test_fragmented_seven_bytes(void) {
    /* El caso del enunciado: nada mayor que 7 bytes por operación. */
    return roundtrip_fragmented(64u * 1024u, 7u);
}

static bool test_fragmented_one_byte(void) {
    /* El peor caso imaginable: un byte por llamada al sistema. */
    return roundtrip_fragmented(4096u, 1u);
}

static bool test_fragmented_whole_frame(void) {
    /* Una trama del protocolo completa, del tamaño de una concesión con
     * página: 32 de cabecera más 8 de versión más 4096 de datos. */
    return roundtrip_fragmented(32u + 8u + 4096u, 5u);
}

/* ---- El par que se va ----------------------------------------------- */

static bool test_recv_detects_close(void) {
    int fds[2];
    unsigned char in[16];

    CHECK(make_pair(fds, true));
    close(fds[0]);                /* el par cierra sin mandar nada */
    CHECK(io_recv_all(fds[1], in, sizeof in, io_deadline_in(1000)) == IO_CLOSED);
    close(fds[1]);

    /* Con datos a medias: llegan 4 bytes y se cierra; pedir 16 tiene que
     * reportar el cierre, no quedarse esperando los 12 que faltan. */
    CHECK(make_pair(fds, true));
    CHECK(io_send_all(fds[0], "hola", 4, io_deadline_in(1000)) == IO_OK);
    close(fds[0]);
    CHECK(io_recv_all(fds[1], in, sizeof in, io_deadline_in(1000)) == IO_CLOSED);
    CHECK(memcmp(in, "hola", 4) == 0);   /* lo que sí llegó, llegó */
    close(fds[1]);
    return true;
}

/* Si faltara MSG_NOSIGNAL, esta prueba no fallaría: mataría al proceso con
 * SIGPIPE y la suite entera se caería sin diagnóstico. */
static bool test_write_to_closed_peer_does_not_kill(void) {
    int fds[2];
    unsigned char out[4096];
    enum io_status st;

    CHECK(make_pair(fds, true));
    fill_pattern(out, sizeof out);
    close(fds[1]);

    st = io_send_all(fds[0], out, sizeof out, io_deadline_in(1000));
    CHECK(st == IO_CLOSED);       /* EPIPE traducido, y el proceso sigue vivo */
    close(fds[0]);
    return true;
}

/* ---- El plazo -------------------------------------------------------- */

static bool test_deadline_expires(void) {
    int fds[2];
    unsigned char in[16];
    uint64_t t0, elapsed_ms;

    CHECK(make_pair(fds, true));

    t0 = now_monotonic_ns();
    CHECK(io_recv_all(fds[1], in, sizeof in, io_deadline_in(60)) == IO_TIMEOUT);
    elapsed_ms = (now_monotonic_ns() - t0) / 1000000ull;

    /* Que esperó de verdad, y que no se quedó colgado. */
    CHECK(elapsed_ms >= 50u);
    CHECK(elapsed_ms < 5000u);

    /* Un plazo ya vencido no espera nada. */
    t0 = now_monotonic_ns();
    CHECK(io_recv_all(fds[1], in, sizeof in, t0 - 1u) == IO_TIMEOUT);
    CHECK((now_monotonic_ns() - t0) / 1000000ull < 1000u);

    close(fds[0]);
    close(fds[1]);
    return true;
}

static bool test_again_on_full_buffer(void) {
    int fds[2];
    unsigned char out[65536];
    size_t moved = 0;
    enum io_status st = IO_OK;
    int guard = 0;

    CHECK(make_pair(fds, true));
    fill_pattern(out, sizeof out);

    /* Llenar el buffer del socket hasta que no entre nada más. Es el estado
     * en que el ciclo poll() del coordinador va a encontrarse cuando un
     * trabajador no lee: tiene que poder seguir con los demás. */
    while (guard++ < 10000) {
        st = io_write_some(fds[0], out, sizeof out, &moved);
        if (st != IO_OK)
            break;
    }
    CHECK(st == IO_AGAIN);
    CHECK(guard < 10000);

    /* Y después de drenar, vuelve a aceptar.
     *
     * Hace falta drenar de verdad, no un poco: el socket de dominio UNIX no
     * contabiliza su capacidad byte a byte sino por los buffers internos que
     * cada envío ocupa, y uno se libera cuando se consume completo. Medido en
     * esta máquina: el buffer se llena con 233152 bytes en 4 envíos, una
     * lectura de 4096 bytes no libera nada que el escritor pueda usar, y
     * recién tras drenar unos 69632 bytes vuelve a aceptar. Por eso la prueba
     * cicla en vez de suponer que una lectura alcanza. */
    unsigned char in[65536];
    bool accepted = false;
    for (int i = 0; i < 100 && !accepted; i++) {
        size_t n = 0;
        if (io_read_some(fds[1], in, sizeof in, &n) != IO_OK)
            break;
        accepted = (io_write_some(fds[0], out, 1, &moved) == IO_OK);
    }
    CHECK(accepted);

    close(fds[0]);
    close(fds[1]);
    return true;
}

/* ---- La señal que interrumpe ---------------------------------------- */

static volatile sig_atomic_t alarms = 0;

static void on_alarm(int sig) {
    (void)sig;
    alarms++;
}

static void *delayed_writer(void *arg) {
    struct xfer *x = arg;
    struct timespec ts = { 0, 120 * 1000 * 1000 };   /* 120 ms */
    nanosleep(&ts, NULL);
    x->st = io_send_all(x->fd, x->buf, x->len, io_deadline_in(5000));
    return NULL;
}

/* El recv se interrumpe con SIGALRM antes de que haya datos. Sin el reintento
 * de EINTR, io_recv_all devolvería IO_ERROR y la sesión se abortaría por una
 * señal inofensiva. */
static bool test_eintr_is_retried(void) {
    int fds[2];
    unsigned char payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char in[8];
    struct sigaction sa, old_sa;
    struct itimerval it;
    sigset_t block, old_mask;
    pthread_t th;
    struct xfer x;

    CHECK(make_pair(fds, false));     /* bloqueante: el recv se duerme adentro */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = 0;                  /* SIN SA_RESTART: queremos el EINTR */
    CHECK(sigaction(SIGALRM, &sa, &old_sa) == 0);

    /* La alarma tiene que caerle a ESTE hilo, no al que escribe: se bloquea
     * SIGALRM antes de crearlo para que la herede bloqueada. */
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    CHECK(pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0);

    x.fd = fds[0];
    x.buf = payload;
    x.len = sizeof payload;
    x.st = IO_ERROR;
    CHECK(pthread_create(&th, NULL, delayed_writer, &x) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);

    alarms = 0;
    memset(&it, 0, sizeof it);
    it.it_value.tv_usec = 30 * 1000;  /* 30 ms, mucho antes de los 120 */
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);

    /* Sin plazo: el descriptor es bloqueante y lo que se prueba es el EINTR. */
    enum io_status rst = io_recv_all(fds[0 + 1], in, sizeof in, 0);

    CHECK(pthread_join(th, NULL) == 0);
    CHECK(alarms > 0);                /* la señal llegó de verdad */
    CHECK(rst == IO_OK);              /* y el ciclo la absorbió */
    CHECK(memcmp(in, payload, sizeof payload) == 0);

    sigaction(SIGALRM, &old_sa, NULL);
    close(fds[0]);
    close(fds[1]);
    return true;
}

/* ---- Varios ---------------------------------------------------------- */

static bool test_deadline_helper_and_statuses(void) {
    static const enum io_status all[] = {
        IO_OK, IO_AGAIN, IO_CLOSED, IO_TIMEOUT, IO_ERROR,
    };

    CHECK(io_deadline_in(0) == 0u);               /* 0 = sin plazo */
    uint64_t d = io_deadline_in(100);
    CHECK(d > now_monotonic_ns());
    CHECK(d - now_monotonic_ns() <= 100ull * 1000000ull);

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = io_strstatus(all[i]);
        CHECK(s != NULL && s[0] != '\0');
    }

    /* Longitud cero es un no-op exitoso, no un error. */
    int fds[2];
    CHECK(make_pair(fds, true));
    size_t moved = 99;
    CHECK(io_write_some(fds[0], "x", 0, &moved) == IO_OK);
    CHECK(moved == 0u);
    CHECK(io_send_all(fds[0], "x", 0, io_deadline_in(100)) == IO_OK);
    CHECK(io_recv_all(fds[1], &moved, 0, io_deadline_in(100)) == IO_OK);
    close(fds[0]);
    close(fds[1]);

    /* Un valor fuera de rango en el entorno se ignora. */
    io_set_chunk_limit(0);
    CHECK(io_chunk_limit() == 0u);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"chunk_limit_caps_transfers", test_chunk_limit_caps_transfers},
        {"fragmented_seven_bytes", test_fragmented_seven_bytes},
        {"fragmented_one_byte", test_fragmented_one_byte},
        {"fragmented_whole_frame", test_fragmented_whole_frame},
        {"recv_detects_close", test_recv_detects_close},
        {"write_to_closed_peer_does_not_kill", test_write_to_closed_peer_does_not_kill},
        {"deadline_expires", test_deadline_expires},
        {"again_on_full_buffer", test_again_on_full_buffer},
        {"eintr_is_retried", test_eintr_is_retried},
        {"deadline_helper_and_statuses", test_deadline_helper_and_statuses},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
