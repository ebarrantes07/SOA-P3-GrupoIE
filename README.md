# SOA-P3-GrupoIE

Proyecto 3 de MC6004 Sistemas Operativos Avanzados (II semestre 2026): sistema de **memoria
compartida distribuida (DSM)** en espacio de usuario. Un coordinador central y varios procesos
trabajadores cooperan por TCP para compartir páginas, replicar lecturas y conceder escritura
exclusiva sin servir datos obsoletos.

No hay memoria compartida real: cada nodo tiene su propia copia de las páginas y la ilusión la
construye el protocolo. El enunciado lo prohíbe explícitamente, así que ni `shm_open`, ni
`MAP_SHARED`, ni memoria System V transportan datos DSM.

## Integrantes

| Nombre | Carné | Usuario de GitHub |
|---|---|---|
| Emmanuel Barrantes Vargas | 200012366 | [ebarrantes07](https://github.com/ebarrantes07) |
| Isaac Moreno Fuentes | 2018119181 | [ifmoreno-sp](https://github.com/ifmoreno-sp) |

## Estado

En construcción. El trabajo está organizado en diez issues (`E0`–`E9`), cada uno una sesión con
sus criterios de aceptación; las dependencias entre ellos están declaradas en el propio issue.
Este README se completa en `E9` con la arquitectura de procesos, los parámetros de cada
ejecutable, los errores esperados y ejemplos de ejecución.

## Entorno

**Objetivo:** GNU/Linux x86-64, GCC y GNU make, C17 con APIs POSIX (sockets TCP y pthreads).
Compilación con `-std=c17 -Wall -Wextra -Wpedantic -Werror -pthread`.

**Probado en:**

| Componente | Versión |
|---|---|
| Sistema | Ubuntu 24.04.4 LTS, x86-64 (WSL2) |
| Compilador | GCC 13.3.0 |
| make | GNU Make 4.3 |
| Biblioteca C | glibc 2.39 |

## Compilación

```bash
make           # compila los ejecutables cuyo main ya exista
make test      # pruebas automatizadas, con límite de tiempo por binario
make test-san  # la misma suite bajo AddressSanitizer y UndefinedBehaviorSanitizer
make test-tsan # la misma suite bajo ThreadSanitizer (compilación aparte)
make clean     # elimina objetos y ejecutables
```

`make test` devuelve un código distinto de cero ante cualquier incumplimiento. Las tres suites se
compilan por separado: ASan/UBSan y TSan no se pueden combinar en un mismo binario.

## Organización del repositorio

```
include/   cabeceras públicas: wire.h (protocolo), dsm.h (API de la biblioteca)
src/       módulos y ejecutables
tests/     pruebas unitarias, una por módulo (tests/test_<modulo>.c)
scripts/   experimentos, análisis y verificación de invariantes
docs/      decisiones de diseño e informe
results/   resultados seleccionados que reproduce el informe
```

## Documentación

- [`docs/decisiones_diseno.md`](docs/decisiones_diseno.md) — bitácora de decisiones de diseño,
  con fecha, contexto y alternativas descartadas.
- `include/dsm.h` — contrato de la biblioteca: propiedad de la memoria, errores, y la diferencia
  entre tener un permiso activo y conservar una copia en caché.
- `include/wire.h` — formato de trama y tipos de mensaje del protocolo.
