/* Conversores estrictos reutilizados de SOA-P2-GrupoIE (src/cli.c), ver D5. */
#include "cli.h"

#include <errno.h>
#include <stdlib.h>

/* Todos los caracteres son dígitos decimales y hay al menos uno. Se evita
 * isdigit() a propósito: depende de la locale y acepta formas que aquí no
 * queremos. */
static bool all_digits(const char *s) {
    if (s == NULL || *s == '\0')
        return false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

bool cli_parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out) {
    if (!all_digits(s))
        return false;

    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v < min || v > max)
        return false;

    *out = (uint64_t)v;
    return true;
}

bool cli_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out) {
    uint64_t v;
    if (!cli_parse_u64(s, min, max, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool cli_parse_u16(const char *s, uint16_t min, uint16_t max, uint16_t *out) {
    uint64_t v;
    if (!cli_parse_u64(s, min, max, &v))
        return false;
    *out = (uint16_t)v;
    return true;
}
