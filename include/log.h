/* log.h — registro CSV del coordinador. Encabezado INTERNO.
 *
 * El enunciado pide un registro con cuatro grupos de campos: identificación,
 * estado de la página, tráfico y tiempo, con una secuencia global creciente.
 * Este módulo es ese registro, y nada más: no decide nada del protocolo.
 *
 * ES UN FORMATEADOR, NO LLEVA RELOJ. La duración de cada operación la calcula
 * quien registra, que es el único que sabe cuándo empezó (la sesión guarda el
 * instante de inicio por nodo, y un nodo tiene a lo sumo una operación en
 * curso). Así este módulo no depende del tiempo y sus pruebas son
 * deterministas: la misma secuencia de llamadas produce siempre el mismo
 * archivo, byte por byte.
 *
 *
 * EL ESTADO SE TOMA DEL DIRECTORIO, NO SE DUPLICA
 *
 * Las columnas de estado (versión, escritor, lectores, invalidaciones
 * pendientes) salen de un `struct dir_page_info`, que es la instantánea que
 * el directorio ya sabe producir. No se lleva contabilidad paralela: una copia
 * del estado que se actualizara aparte podría desincronizarse del directorio,
 * y entonces el registro mentiría justo cuando más importa, que es al auditar
 * un invariante roto.
 *
 *
 * VALORES AUSENTES: CELDA VACÍA, NO CENTINELA
 *
 * Un `BARRIER_RELEASE` no tiene página y un `SESSION_START` no tiene nodo. En
 * esos casos la celda del CSV queda vacía en vez de llevar 0xFFFFFFFF. Es para
 * que los scripts de E8 lean la columna como número con un valor faltante, en
 * vez de tener que saber qué centinela significa "no aplica" en cada columna —
 * y para que un promedio no se contamine con 4294967295.
 *
 *
 * EL TAMAÑO DE PÁGINA SE CUENTA UNA SOLA VEZ (criterio de #4)
 *
 * El tráfico no lo informa quien registra el evento: lo cuentan
 * `log_count_sent` y `log_count_recv`, que reciben el tipo y la longitud de
 * carga de la trama y derivan solos los bytes totales, si llevaba página y si
 * era una invalidación. Quien llama no puede contar dos veces porque no pasa
 * ningún contador. Se invocan desde el único lugar donde una trama cruza de
 * verdad hacia el socket o desde él (E3, el módulo de conexión).
 *
 *
 * UN REGISTRO NULO ESTÁ DESHABILITADO
 *
 * Todas las funciones aceptan `lg == NULL` y no hacen nada. Así el coordinador
 * no necesita preguntar si hay registro antes de cada llamada, y correr sin
 * `--log` no cambia una sola línea de su código.
 */
#ifndef SOA_P3_LOG_H
#define SOA_P3_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "directory.h"
#include "dsm.h"

/* Eventos que el registro distingue. Los códigos son explícitos y el nombre
 * que sale al CSV es el del `switch` de log_event_name: el valor numérico no
 * aparece nunca en el archivo, así que agregar un evento al final no cambia
 * ninguna corrida anterior. */
enum log_event {
    LOG_SESSION_START = 1,  /* los N trabajadores se registraron           */
    LOG_SESSION_END,        /* cierre ordenado                             */
    LOG_NODE_JOIN,          /* HELLO aceptado                              */
    LOG_NODE_LEAVE,         /* BYE o desconexión                           */
    LOG_ACQ_REQ,            /* solicitud recibida                          */
    LOG_ACQ_GRANT,          /* acceso concedido                            */
    LOG_INVALIDATE,         /* invalidación enviada                        */
    LOG_INVALIDATE_ACK,     /* confirmación recibida                       */
    LOG_REL_WRITE,          /* liberación de escritura recibida            */
    LOG_WRITE_PUBLISHED,    /* página publicada y versión subida           */
    LOG_REL_READ,           /* liberación de lectura recibida              */
    LOG_READ_RELEASED,      /* confirmada                                  */
    LOG_BARRIER_ARRIVE,     /* llegada a una barrera                       */
    LOG_BARRIER_RELEASE,    /* barrera liberada                            */
    LOG_TIMEOUT,            /* venció el plazo de una operación            */
    LOG_ERROR               /* se rechazó algo, con su dsm_status          */
};

/* Nombre que va al CSV. Devuelve "?" para un valor desconocido en vez de
 * NULL, para que un error de programación no termine en desreferencia. */
const char *log_event_name(enum log_event e);

/* Token corto del dsm_status para la columna `status`. Cadena vacía para
 * DSM_OK, que es "no aplica".
 *
 * NO es DSM_strerror, a propósito y por dos razones. La primera es práctica:
 * DSM_strerror lo implementa `libdsm` (#5), y el coordinador no se enlaza
 * contra la biblioteca del trabajador. La segunda es que sirven a lectores
 * distintos: DSM_strerror da una frase en español para que una aplicación la
 * muestre, y esta columna necesita un token estable, corto y sin comas, que
 * los scripts de E8 puedan agrupar. Que las dos tablas no coincidan no es una
 * inconsistencia: es que no describen lo mismo. */
const char *log_status_name(enum dsm_status s);

/* Lo que no aplica a un evento. Al escribir la fila se vuelven celda
 * vacía. */
#define LOG_NO_NODE     UINT32_MAX
#define LOG_NO_REQUEST  UINT64_MAX
#define LOG_NO_PAGE     UINT32_MAX
#define LOG_NO_MODE     0
#define LOG_NO_DURATION UINT64_MAX

/* Una fila. Se arma con inicializadores designados, así que los campos que no
 * se mencionen quedan en 0 — y por eso los centinelas de "no aplica" NO son 0:
 * un `struct log_row row = {.event = LOG_SESSION_START}` tiene que salir con
 * node_id vacío, no con node_id 0, que es un nodo real. Usar
 * log_row_empty() para obtener una fila con todo en "no aplica". */
struct log_row {
    enum log_event event;
    uint32_t       node_id;
    uint64_t       request_id;
    uint32_t       page_id;
    enum dsm_mode  mode;

    /* Estado de la página. NULL cuando el evento no se refiere a una. */
    const struct dir_page_info *page;

    /* El código que se devolvió o se registró. Va en su propia columna, no
     * en la de evento: así una fila de error conserva QUÉ se intentaba
     * (el evento) además de POR QUÉ falló (el estado). DSM_OK = celda
     * vacía. */
    enum dsm_status status;

    /* Duración local de la operación que este evento cierra, en nanosegundos,
     * calculada por quien registra. LOG_NO_DURATION cuando no cierra nada. */
    uint64_t dur_ns;
};

/* Fila con todos los campos en "no aplica". El patrón de uso es:
 *
 *     struct log_row row = log_row_empty(LOG_ACQ_GRANT);
 *     row.node_id = n; row.page_id = p; row.page = &info;
 *     log_write(lg, &row);
 */
struct log_row log_row_empty(enum log_event event);

typedef struct dsm_log dsm_log;

/* Abre el registro y escribe la línea de encabezado.
 *
 * `path` es el archivo a crear o truncar. `component` identifica quién
 * escribe ("coordinador", o el nombre del trabajador en E4), y va en cada
 * fila para poder unir los registros de varios procesos en un solo análisis.
 * `run_id` identifica la corrida y es el mismo que el coordinador manda en
 * SESSION_START, así que las filas de los trabajadores se emparejan con las
 * del coordinador por esa columna.
 *
 * Devuelve NULL si no se pudo abrir, o si `component` tiene una coma o un
 * salto de línea: se rechaza en vez de escaparse, porque es un nombre que
 * elegimos nosotros y un CSV sin comillas es mucho más fácil de leer a mano
 * que uno con reglas de escape.
 *
 * El archivo queda con buffer por línea: si el proceso muere a mitad de una
 * corrida —que es exactamente lo que se provoca en #8— las filas anteriores
 * ya están en disco. */
dsm_log *log_open(const char *path, const char *component, uint64_t run_id);

/* Cierra y libera. Acepta NULL. Devuelve false si alguna escritura falló en
 * algún momento de la corrida o si el cierre falló: un registro truncado por
 * disco lleno tiene que poder reportarse, no pasar inadvertido. */
bool log_close(dsm_log *lg);

/* Escribe una fila y le asigna la siguiente secuencia. La secuencia empieza
 * en 1 y crece de uno en uno; la asigna esta función y nadie más, que es lo
 * que la vuelve creciente sin esfuerzo con un solo hilo (D0.1). */
void log_write(dsm_log *lg, const struct log_row *row);

/* Contabilidad de tráfico. `type` es el código de tipo del protocolo y
 * `payload_len` los bytes de carga que anuncia la cabecera; los bytes totales
 * (cabecera incluida), si la trama llevaba página y si era una invalidación
 * los deriva esta función. Se llaman una vez por trama que efectivamente
 * cruzó, y actualizan los contadores que salen en las filas siguientes. */
void log_count_sent(dsm_log *lg, uint8_t type, uint32_t payload_len);
void log_count_recv(dsm_log *lg, uint8_t type, uint32_t payload_len);

/* Contadores acumulados de la sesión, para el resumen final y las pruebas. */
struct log_traffic {
    uint64_t msgs_in, msgs_out;
    uint64_t bytes_in, bytes_out;
    uint64_t page_xfers;   /* tramas que llevaron una página completa */
    uint64_t invals;       /* INVALIDATE enviados */
};

/* Instantánea de los contadores. Con `lg == NULL` devuelve todo en cero. */
struct log_traffic log_traffic_now(const dsm_log *lg);

/* Secuencia de la última fila escrita (0 si no se escribió ninguna). */
uint64_t log_seq(const dsm_log *lg);

#endif /* SOA_P3_LOG_H */
