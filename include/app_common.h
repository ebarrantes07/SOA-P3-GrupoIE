/* app_common.h — lo que comparten las aplicaciones de #7.
 *
 * Las tres aplicaciones (app_pi, app_rw, app_granularity) tienen la misma
 * forma: opciones de conexión, DSM_init, un trabajo y una despedida que se
 * hace siempre, también al fallar. Esto es esa forma; cada aplicación agrega
 * solo sus opciones propias y su trabajo.
 *
 * Los códigos de salida son los mismos que los de app_counter (D5.1), para
 * que un script los interprete igual en las cuatro:
 */
#ifndef SOA_P3_APP_COMMON_H
#define SOA_P3_APP_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#include "dsm.h"

#define APP_EXIT_OK     0   /* terminó y el resultado se verificó */
#define APP_EXIT_ARGS   1   /* argumentos inválidos */
#define APP_EXIT_DSM    2   /* el DSM falló: conexión, plazo, aborto, rechazo */
#define APP_EXIT_RESULT 3   /* la sesión funcionó pero el resultado está mal */

/* Opciones de conexión, comunes a todas. */
struct app_conn {
    const char *host;
    uint16_t    port;
    uint32_t    nodes;
    uint32_t    nid;
    uint32_t    timeout_ms;
    const char *log_path;
};

enum app_opt {
    APP_OPT_TAKEN,     /* la opción era de la aplicación y su valor sirve */
    APP_OPT_UNKNOWN,   /* no la reconoce */
    APP_OPT_INVALID,   /* la reconoce, pero el valor no sirve */
};

/* Opciones propias de cada aplicación. Se llama con cada par opción-valor que
 * no sea de conexión. */
typedef enum app_opt (*app_opt_fn)(const char *flag, const char *value,
                                   void *arg);

enum app_parse { APP_PARSE_OK, APP_PARSE_HELP, APP_PARSE_ERROR };

/* Lee las opciones de conexión y le pasa las demás a `extra`. `usage_extra`
 * describe las propias en la ayuda. Deja el motivo impreso si falla. */
enum app_parse app_parse(int argc, char **argv, struct app_conn *c,
                         app_opt_fn extra, void *arg, const char *usage_extra);

/* DSM_init con las opciones de conexión; imprime el motivo si falla. */
dsm_ctx *app_connect(const struct app_conn *c);

/* Informa una falla del DSM con el estado que la explica y devuelve
 * APP_EXIT_DSM, para usarlo como `rc = app_fail(...)`. */
int app_fail(dsm_ctx *ctx, uint32_t nid, const char *what);

/* Se despide siempre: así el coordinador distingue una salida ordenada de una
 * caída. Devuelve `rc`, o APP_EXIT_DSM si el cierre falló y no había otra
 * falla que informar. */
int app_finish(dsm_ctx *ctx, uint32_t nid, int rc);

/* Milisegundos entre dos lecturas de now_monotonic_ns. */
double app_ms(uint64_t from_ns, uint64_t to_ns);

#endif /* SOA_P3_APP_COMMON_H */
