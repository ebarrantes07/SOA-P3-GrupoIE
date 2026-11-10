/* clock.h — reloj monotónico compartido por todo el proyecto.
 *
 * Reutilizado de SOA-P2-GrupoIE sin cambios de fondo (ver
 * docs/decisiones_diseno.md, D5).
 *
 * Centraliza clock_gettime(CLOCK_MONOTONIC, ...) en un solo lugar. El
 * enunciado exige medir con reloj monotónico la duración local de
 * acquire/release y el tiempo total, y advierte que no debe suponerse que los
 * relojes físicos de máquinas distintas estén sincronizados: por eso la
 * correlación entre nodos se hace con request_id y con la secuencia de cada
 * componente, nunca restando marcas de tiempo tomadas en máquinas distintas.
 */
#ifndef SOA_P3_CLOCK_H
#define SOA_P3_CLOCK_H

#include <stdint.h>

/* Marca actual del reloj monotónico, en nanosegundos desde un punto de
 * referencia no especificado pero compartido entre todos los procesos de esta
 * máquina (no sobrevive un reinicio, no es comparable entre máquinas
 * distintas). Dos marcas tomadas en el mismo host son restables incluso entre
 * procesos distintos; dos marcas de hosts distintos, no. */
uint64_t now_monotonic_ns(void);

#endif /* SOA_P3_CLOCK_H */
