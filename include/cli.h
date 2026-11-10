/* cli.h — conversores estrictos para los argumentos de línea de comandos.
 *
 * Los conversores vienen de SOA-P2-GrupoIE (src/cli.c), ver
 * docs/decisiones_diseno.md (D5). El análisis por rol es distinto en este
 * proyecto — el coordinador recibe puerto, nodos y páginas; cada trabajador
 * recibe host, puerto, nodos e identificador — y se escribe cuando existan
 * los ejecutables, en #4 y #5.
 *
 * El módulo solo convierte y valida texto: no abre archivos, no crea sockets
 * y no toca ninguna sesión, así que un rechazo nunca deja recursos a medias.
 * El enunciado pide que los argumentos inválidos produzcan un rechazo claro,
 * y eso empieza por no aceptar nunca una conversión dudosa.
 */
#ifndef SOA_P3_CLI_H
#define SOA_P3_CLI_H

#include <stdbool.h>
#include <stdint.h>

/* Entero decimal sin signo dentro de [min, max]. Rechaza cadena vacía, signo
 * (incluido '+'), espacios al inicio o al final, basura después de los
 * dígitos y desbordamiento. strtoull por sí solo acepta todo eso, y '-1' lo
 * convertiría en un número enorme. */
bool cli_parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out);
bool cli_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out);

/* Puerto TCP en [min, max], con el rango de un u16. Un puerto 0 pediría al
 * sistema uno efímero, lo que no sirve para que los trabajadores sepan a
 * dónde conectarse, así que el mínimo útil es 1. */
bool cli_parse_u16(const char *s, uint16_t min, uint16_t max, uint16_t *out);

#endif /* SOA_P3_CLI_H */
