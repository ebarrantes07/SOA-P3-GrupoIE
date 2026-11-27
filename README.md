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

Los ejecutables no necesitan nada más que la biblioteca C. La suite sí: `make test` corre
`scripts/e2e_verificador.sh`, que invoca el verificador de invariantes, así que **python3** es
obligatorio para probar (solo módulos de la biblioteca estándar: no hay nada que instalar con
pip). `pgrep`, del paquete `procps`, es opcional: si falta, `run_tests.sh` lo avisa y sigue, pero
deja de comprobar que la suite no haya dejado procesos del programa vivos.

**Probado en:**

| Componente | Versión |
|---|---|
| Sistema | Ubuntu 24.04.4 LTS, x86-64 (WSL2) |
| Compilador | GCC 13.3.0 |
| make | GNU Make 4.3 |
| Biblioteca C | glibc 2.39 |
| python3 (solo para las pruebas) | 3.12.3 |
| pgrep (opcional) | procps-ng 4.0.4 |

## Compilación

```bash
make               # compila los ejecutables cuyo main ya exista
make test          # pruebas automatizadas, con límite de tiempo por binario
make test-san      # la misma suite bajo AddressSanitizer y UndefinedBehaviorSanitizer
make test-tsan     # la misma suite bajo ThreadSanitizer (compilación aparte)
make test-robustez # la suite repetida con fallos inyectados (tarda minutos)
make clean         # elimina objetos y ejecutables
```

`make test` devuelve un código distinto de cero ante cualquier incumplimiento, y comprueba al
terminar que no haya quedado ningún proceso del programa vivo ni temporales sin borrar. Las tres
suites se compilan por separado: ASan/UBSan y TSan no se pueden combinar en un mismo binario.

`make test-robustez` repite la suite completa siete veces, una por cada tamaño de fragmento de 1 a
7 bytes, que fuerza a que cada envío y cada recepción se partan en pedazos de ese tamaño. Agrega una
octava pasada, dirigida solo a las pruebas de la biblioteca, con un retardo inyectado en una ventana
de concurrencia que de otro modo es demasiado angosta para que una prueba la alcance. Va aparte de
`make test` porque tarda unos dos minutos.

El registro de una corrida se puede auditar sin leer el código:

```bash
./scripts/verificar_invariantes.py registro.csv
```

Comprueba, reconstruyendo el estado a partir de los eventos, que haya un solo escritor por página,
que no coexistan lectores ni copias ajenas con un escritor, que cada concesión de escritura llegue
después de todos los ACK de invalidación que requería, que las versiones no retrocedan y que un ACK
repetido no cierre una ronda.

Del mismo registro salen las métricas de una corrida:

```bash
./scripts/metricas.py registro.csv
```

Informa mensajes, bytes (con las cabeceras incluidas), páginas transferidas, invalidaciones y
adquisiciones, separando el tráfico del protocolo de coherencia del de la coordinación
experimental. Comprueba tres identidades antes de informar —las páginas y las invalidaciones
derivadas de los eventos contra las columnas acumuladas, y los bytes por grupo contra el total— y
marca la corrida si alguna no cierra, en vez de dar un número que no se sostiene.

Y de los resultados de una matriz de experimentos sale el resumen por configuración:

```bash
./scripts/analisis.py results/
```

Agrupa las repeticiones de cada configuración e informa media, desviación estándar muestral y n
de cada métrica, tomando el tráfico del registro de cada corrida. Ninguna corrida se descarta en
silencio: una que no terminó en 0, o cuyo tráfico no cuadra, sale de las estadísticas pero se
informa con su nombre y su motivo.

Y de ese resumen salen las figuras del informe:

```bash
./scripts/analisis.py --json results/ | ./scripts/graficas.py --out results/
```

Tres SVG, uno por experimento: localidad de lectura, falso compartimiento y costo de
distribución. Sin dependencias —los scripts siguen usando solo la biblioteca estándar— y con la
desviación dibujada sobre cada marca. Si a los datos les falta una métrica, la figura no se
dibuja y se dice cuál faltó.

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
