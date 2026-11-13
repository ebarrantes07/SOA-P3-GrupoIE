/* test_proc.h — levantar el coordinador y trabajadores como procesos, para
 * las pruebas de punta a punta.
 *
 * El arranque del coordinador es el de test_e2e_manager.c (#4): fork, exec de
 * ./dsm_manager con --port 0, y lectura del puerto que imprime. El pipe es lo
 * que sincroniza: la lectura se bloquea hasta que el socket escucha, así que
 * no hay que esperar a ciegas. Vive acá para que las pruebas de libdsm (#5) y
 * las de las aplicaciones (#6) no lo copien; test_e2e_manager.c puede pasar a
 * usarlo cuando convenga.
 *
 * Nada de esto deja procesos vivos: toda espera tiene tope, y al vencer se
 * mata al proceso con SIGKILL y se lo recoge. Un coordinador huérfano se
 * quedaría con el puerto, y un trabajador huérfano violaría el criterio de
 * que no quede ningún proceso del programa después de la suite.
 */
#ifndef SOA_P3_TEST_PROC_H
#define SOA_P3_TEST_PROC_H

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"

struct proc_coord {
    pid_t    pid;
    uint16_t port;
};

/* Arranca ./dsm_manager. `log_path` puede ser NULL. Su stderr va al vacío:
 * los avisos de plazo vencido o de señal son esperados en varias pruebas. */
static inline bool proc_coord_start(struct proc_coord *c, uint32_t nodes,
                                    uint32_t pages, uint32_t timeout_ms,
                                    const char *log_path) {
    int pfd[2];
    char s_nodes[16], s_pages[16], s_to[16], buf[32];
    size_t n = 0;
    pid_t pid;

    if (pipe(pfd) != 0)
        return false;
    snprintf(s_nodes, sizeof s_nodes, "%lu", (unsigned long)nodes);
    snprintf(s_pages, sizeof s_pages, "%lu", (unsigned long)pages);
    snprintf(s_to, sizeof s_to, "%lu", (unsigned long)timeout_ms);

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);

        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        if (dn >= 0) {
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        setsid();
        if (log_path != NULL)
            execl("./dsm_manager", "dsm_manager", "--nodes", s_nodes,
                  "--pages", s_pages, "--port", "0", "--timeout-ms", s_to,
                  "--log", log_path, (char *)NULL);
        else
            execl("./dsm_manager", "dsm_manager", "--nodes", s_nodes,
                  "--pages", s_pages, "--port", "0", "--timeout-ms", s_to,
                  (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    while (n + 1 < sizeof buf) {
        ssize_t r = read(pfd[0], buf + n, 1);
        if (r <= 0 || buf[n] == '\n')
            break;
        n++;
    }
    buf[n] = '\0';
    close(pfd[0]);

    c->pid = pid;
    c->port = (uint16_t)atoi(buf);
    if (c->port == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }
    return true;
}

/* Espera a que `pid` termine, con un tope en milisegundos. Devuelve su código
 * de salida, 128 + señal si murió por una, o -1 si hubo que matarlo. */
static inline int proc_wait(pid_t pid, uint32_t limit_ms) {
    uint64_t end = now_monotonic_ns() + (uint64_t)limit_ms * 1000000u;

    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);

        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        if (r < 0)
            return -1;
        if (now_monotonic_ns() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return -1;
        }
        /* Espera corta cediendo el procesador: el proceso necesita tiempo
         * real para avanzar. Es la prueba la que espera, no la biblioteca. */
        struct timespec ts = { 0, 2000000 };   /* 2 ms */
        nanosleep(&ts, NULL);
    }
}

static inline void proc_kill(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* Lanza un trabajador: un proceso hijo que corre fn(port, nid, arg) y termina
 * con lo que devuelva. -1 si no se pudo crear. */
static inline pid_t proc_worker(int (*fn)(uint16_t port, uint32_t nid, void *arg),
                                uint16_t port, uint32_t nid, void *arg) {
    pid_t pid;

    /* Se vacía ANTES del fork: si no, el hijo heredaría lo que la suite tenga
     * en el buffer de stdout y lo imprimiría otra vez. El hijo termina con
     * _exit, que no vacía nada; sus mensajes van a stderr, sin buffer. */
    fflush(NULL);
    pid = fork();
    if (pid == 0)
        _exit(fn(port, nid, arg));
    return pid;
}

static inline void proc_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

#endif /* SOA_P3_TEST_PROC_H */
