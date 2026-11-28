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
make               # compila el coordinador, la biblioteca y las cuatro aplicaciones
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

## Arquitectura de procesos

Una sesión son **N + 1 procesos** que solo se comunican por TCP:

```
                     dsm_manager                  coordinador: un solo hilo, poll()
            (directorio + datos confirmados)      dueño de la versión oficial de cada página
              /        |          |       \
           TCP        TCP        TCP      TCP
            /          |          |         \
     trabajador 0  trabajador 1  ...  trabajador N-1     1 ≤ N ≤ 4
     app + libdsm  app + libdsm       app + libdsm       un hilo de aplicación
     marco local   marco local        marco local        + un hilo receptor interno
```

- **`dsm_manager`** es el coordinador. Guarda, por página, el contenido confirmado, su versión, el
  escritor activo, los lectores activos, las copias en caché de cada nodo y la cola de pedidos.
  Decide cada concesión e invalidación en un solo hilo, así que hay un único orden de eventos, y
  lo escribe en un registro CSV.
- **Cada trabajador** es un proceso aparte con su propio espacio de direcciones. Enlaza
  `libdsm`, que mantiene un marco local por página y un hilo receptor que atiende las
  invalidaciones mientras la aplicación trabaja. La aplicación lee y escribe el marco local
  solamente entre `DSM_acquire` y `DSM_release`.
- **Todos los trabajadores de una sesión corren la misma aplicación**, cada uno con su `--id`.
  El id decide la parte del trabajo y, en algunas aplicaciones, el rol (en `app_rw` el nodo 0
  escribe y los demás leen).

La interfaz está en `include/dsm.h`:

| Operación | Qué hace |
|---|---|
| `DSM_init` | conecta al coordinador y espera a que estén los N nodos |
| `DSM_acquire(ctx, page, mode)` | pide `DSM_READ` o `DSM_WRITE` y devuelve el puntero al marco local |
| `DSM_release(ctx, page)` | devuelve el permiso; si era de escritura, publica la página |
| `DSM_barrier(ctx, id)` | barrera colectiva reutilizable |
| `DSM_finalize(ctx)` | cierra la participación y libera sockets, memoria e hilos |
| `DSM_last_error(ctx)` | el último error, para distinguir un error de uso de uno de sesión |

Al liberar, el nodo **conserva la copia**. Si la vuelve a pedir y nadie la publicó mientras
tanto, la concesión llega sin datos. Toda adquisición consulta al coordinador; lo que la copia
ahorra es la transferencia, no la consulta.

## Uso

### Inicio: primero el coordinador, después los trabajadores

1. Se arranca el coordinador con la cantidad de nodos y páginas. Con `--port 0` toma un puerto
   libre y lo imprime **en la primera línea de stdout**.
2. Se arrancan los N trabajadores, uno por id de `0` a `N-1`, contra ese puerto. Pueden
   arrancar en cualquier orden. La sesión empieza cuando se registraron los N.
3. Cada trabajador termina solo e imprime su resultado. El coordinador cierra cuando se
   despidieron todos.

```bash
./dsm_manager --nodes 2 --pages 4 --port 0 --log corrida.csv > puerto.txt &
until [ -s puerto.txt ]; do sleep 0.1; done     # el puerto sale cuando ya escucha
PORT=$(head -1 puerto.txt)
./app_counter --port "$PORT" --nodes 2 --id 0 --iters 1000 &
./app_counter --port "$PORT" --nodes 2 --id 1 --iters 1000
wait
```

Los trabajadores tienen que arrancar **después** de que el coordinador imprima el puerto: antes
no hay nadie escuchando y salen con 2. Con un puerto fijo (`--port 5000`) vale lo mismo.

```
nodo 1: contador=2000 esperado=2000
nodo 0: contador=2000 esperado=2000
```

Después el registro se puede auditar con `./scripts/verificar_invariantes.py corrida.csv`.

En dos máquinas se usa `--host` en los dos lados: la dirección donde escucha el coordinador y
la dirección a la que se conecta cada trabajador. La suite corre todo por loopback.

### `dsm_manager`

| Opción | Valores | Por omisión |
|---|---|---|
| `--nodes N` | trabajadores esperados, 1..4 | obligatorio |
| `--pages P` | páginas de la región, 1..256, de 4096 bytes cada una | 16 |
| `--host H` | dirección donde escuchar | todas |
| `--port P` | puerto; `0` toma uno libre y lo imprime | — |
| `--timeout-ms MS` | plazo por operación; vencido, se aborta la sesión | 10000 |
| `--log RUTA` | registro CSV de la corrida, con el estado de la página en cada evento | sin registro |
| `--run-id N` | identificador de la corrida; `0` lo toma del reloj | 0 |

### Opciones comunes de las aplicaciones

| Opción | Valores | Por omisión |
|---|---|---|
| `--port P` | puerto del coordinador | obligatorio |
| `--nodes N` | nodos de la sesión, 1..4; igual al del coordinador | obligatorio |
| `--id I` | identificador de este nodo, 0..N-1, único en la sesión | obligatorio |
| `--host H` | dirección del coordinador | 127.0.0.1 |
| `--timeout-ms MS` | plazo por operación | 10000 |
| `--log RUTA` | registro CSV de este trabajador | sin registro |

`--help` imprime el uso y sale con 0.

### Las aplicaciones

| Ejecutable | Qué hace | Opciones propias | Imprime |
|---|---|---|---|
| `app_counter` | cada nodo incrementa K veces un contador compartido dentro de una adquisición de escritura; el final debe ser N×K | `--iters K` (1000), `--page P` (0) | `contador= esperado=` |
| `app_pi` | reparte T términos de la serie de Leibniz, cada nodo publica su suma parcial y el nodo 0 reduce y compara con la serie secuencial; después reutiliza una barrera G veces | `--terms T` (100003), `--barriers G` (100) | `pi_distribuido= pi_secuencial= t_total_ms= t_calculo_ms=` |
| `app_rw` | el nodo 0 publica V versiones de una página y los demás las leen y verifican byte a byte. Con `--reads M` mide localidad: M lecturas de una versión, en un intervalo (`batch`) o con una adquisición por lectura (`repeated`) | `--versions V` (2), `--page P` (0), `--reads M`, `--access batch\|repeated` | `rol= accesos_locales= adquisiciones= t_lectura_ms=` |
| `app_granularity` | cada nodo incrementa su propio contador R veces, con los contadores en una sola página (`shared`) o uno por página (`separate`); mide el falso compartimiento | `--layout shared\|separate`, `--rounds R` (100) | `adquisiciones_trabajo= incrementos= t_trabajo_ms=` |

`app_pi` necesita 2 páginas, `app_granularity --layout separate` necesita N, y las demás 1.

## Errores esperados

**Códigos de salida de las aplicaciones** (D5.1):

| Código | Significa | Ejemplos |
|---|---|---|
| 0 | terminó y verificó su resultado | — |
| 1 | argumentos inválidos; se rechazan **antes** de abrir un socket, salvo la página, que se valida al conocer la región | falta `--port`, `--id 2` con `--nodes 2`, `--terms 0`, `--access zzz`, opción desconocida, `--page 99` en una región de 4 |
| 2 | falló el DSM | no hay coordinador en ese puerto, venció el plazo, la sesión se abortó, id repetido |
| 3 | la sesión funcionó pero el resultado no cuadra | el contador no llegó a N×K, π no coincide con la serie secuencial, una lectura vio bytes de otra versión |

**Códigos de salida del coordinador:** 0 si la sesión terminó en orden; 1 si se abortó (un
nodo se cayó, venció un plazo o llegó SIGINT/SIGTERM) o si no pudo abrir el registro o
escuchar; 2 si los argumentos son inválidos.

**Situaciones que se rechazan a propósito:**

- **Un id repetido.** El coordinador rechaza al segundo nodo sin tocar la sesión del primero.
  Como nunca junta los N, la sesión vence su plazo de registro y nadie termina en 0.
- **Una página que no existe en la sesión.** Se sabe recién al arrancar, porque la región la
  fija el coordinador. El trabajador sale con 1, con un mensaje que dice cuántas páginas hay, y
  se despide en orden: el coordinador termina en 0.
- **Un error de uso** (adquisición anidada, liberar sin haber adquirido, modo inválido). La
  biblioteca lo detecta localmente y devuelve `DSM_EINVAL` o `DSM_ESTATE` sin ir a la red. El
  contexto sigue vivo.
- **Un error de sesión** (`DSM_ECONN`, `DSM_ETIMEOUT`, `DSM_EPROTO`, `DSM_EABORT`). Es
  terminal: la caída de un participante o del coordinador aborta la sesión. No hay reconexión,
  ni recuperación de escrituras no publicadas, ni persistencia (D3.5, D0.13).
- **El coordinador espera indefinidamente hasta que se conecta el primer nodo**, a propósito:
  se arranca a mano y puede pasar un rato. El plazo de registro empieza con la primera
  conexión.

## Experimentos

```bash
make
python3 scripts/experimento.py                    # matriz completa: 16 configuraciones
python3 scripts/experimento.py --solo localidad   # un solo experimento
python3 scripts/experimento.py --rapido --reps 2  # matriz mínima, la que usa make test
```

Cada corrida deja `results/<experimento>/<config>_r<n>.json` (la salida de cada nodo, los
códigos, el entorno y el comando) y su registro `.csv`. Antes de cada configuración corre una
repetición de calentamiento que se descarta y queda declarada. Una corrida que falla no detiene
la matriz: queda escrita con su motivo y al final se listan las fallidas.

| Experimento | Aplicación | Qué se varía |
|---|---|---|
| localidad de lectura | `app_rw --reads M` | M = 1, 8, 64 lecturas, en un intervalo o en uno por lectura, N = 2 |
| falso compartimiento | `app_granularity --rounds 50` | contadores en una página o en una por nodo, N = 2 y 4 |
| costo de distribución | `app_pi --barriers 0` | N = 1, 2, 4 con T = 100 003 y 200 000 003 |

Las cifras del informe salen de las 160 corridas de `results/seleccionados/` (10 repeticiones
por configuración, en WSL2). Su README dice cómo regenerar desde ahí el agregado, las tablas y
las figuras sin volver a medir.

## Material reutilizado

Del Proyecto 2 de los mismos integrantes,
[SOA-P2-GrupoIE](https://github.com/ebarrantes07/SOA-P2-GrupoIE), se reutiliza, con la
procedencia declarada en cada archivo y en D0.5:

- `src/clock.c` e `include/clock.h`, el reloj monotónico, sin cambios de fondo;
- los conversores estrictos de argumentos de `src/cli.c` (`cli_parse_u32`, `cli_parse_u64`);
- la base de `scripts/graficas.py`: la clase `Svg` y la paleta.

**Licencia:** el Proyecto 2 es trabajo propio de los mismos integrantes, así que se reutiliza
sin restricciones de terceros. El informe usa la plantilla LaTeX `IEEEtran` bajo su propia
licencia. Los scripts usan solo la biblioteca estándar de Python. El resto del código es
original de este proyecto.

## Gestión y trazabilidad

El trabajo se organizó en **10 issues**, de `E0` a `E9`, cada uno una etapa con su responsable,
sus fechas, sus dependencias y sus criterios de aceptación como lista de casillas. Entró por
**13 pull requests** con 77 commits, cada uno revisado y aprobado por el otro integrante antes
de fusionarse en `main`, que está protegida: pide una aprobación y se fusiona con merge commit,
sin aplastar la historia.

### Issues y pull requests

**Responsable** es quien figura en el issue; **Abrió** es quien abrió el pull request;
**Revisó** es quien lo revisó y aprobó.

| Issue | Etapa | Responsable | PR | Abrió | Revisó |
|---|---|---|---|---|---|
| [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) | E0 · Andamiaje del repo y protocolo congelado | Ambos | [#11](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/11) | Emmanuel | Isaac (pidió cambios, luego aprobó) |
| [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2) | E1 · Transporte: tramas e I/O parcial | Emmanuel | [#13](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/13) | Emmanuel | Isaac |
| [#3](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/3) | E2 · Directorio: estado por página, colas e invalidaciones | Isaac | [#12](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/12) | Isaac | Emmanuel |
| [#4](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/4) | E3 · Coordinador: sesión, poll(), barreras y registro | Emmanuel | [#14](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/14) | Emmanuel | Isaac |
| [#5](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/5) | E4 · libdsm: API, caché local e hilo receptor | Isaac | [#15](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/15) | Isaac | Emmanuel |
| [#6](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/6) | E5 · Primer end-to-end: contador N×K y `make test` | Ambos | [#16](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/16) | Emmanuel | Isaac (pidió cambios, luego aprobó) |
| [#7](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/7) | E6 · Aplicaciones: π, lectores/escritor y granularidad | Isaac | [#18](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/18) | Isaac | Emmanuel |
| [#8](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/8) | E7 · Robustez y observabilidad: verificador, fallos inyectados | Emmanuel | [#17](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/17) | Emmanuel | Isaac |
| [#9](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/9) | E8 · Experimentos | Ambos | [#19](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/19), [#20](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/20) | Emmanuel, Isaac | Isaac, Emmanuel |
| [#10](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/10) | E9 · Informe, README final y entrega etiquetada | Ambos | [#21](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/21), [#22](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/22), [#23](https://github.com/ebarrantes07/SOA-P3-GrupoIE/pull/23) y el de este README | Emmanuel, Isaac | Isaac, Emmanuel |

E8 y E9 se dividieron en dos mitades con un contrato escrito entre ellas. En E8, Emmanuel hizo
las métricas, el análisis y las figuras (#19), e Isaac el lanzador de la matriz y la localidad
en `app_rw` (#20). Cada uno probó su mitad contra la del otro antes de fusionar.

### Responsabilidades

El reparto siguió los módulos.

**Emmanuel Barrantes** hizo el transporte (tramas, I/O parcial, sockets no bloqueantes); el
coordinador `dsm_manager` con su sesión, sus barreras, sus plazos y su registro CSV; el verificador de invariantes, la inyección de
fallos y `make test-robustez`; las métricas, el análisis y las figuras de los experimentos; y en
el informe, la arquitectura, el protocolo, las pruebas, los resultados, el análisis, la
comparación con la DSM transparente, las decisiones y los límites.

**Isaac Moreno** hizo el directorio, que es la máquina de estados por página con sus colas e
invalidaciones; la biblioteca `libdsm` con su caché local, su hilo receptor y la máquina de
estados del trabajador; las aplicaciones `app_pi`, `app_rw` y `app_granularity`; el lanzador de
la matriz de experimentos y la medición de localidad; y en el informe, el resumen, la
introducción, las conclusiones, la declaración de transparencia y la entrega, además de este
README.

Fueron conjuntos el andamiaje del repositorio, el protocolo congelado en `include/wire.h`, la
planificación y la primera prueba de punta a punta con `app_counter` (E5).

### Revisiones

Las revisiones no fueron de forma. Tres ejemplos:

- **#19:** la revisión encontró que una corrida sin registro entraba a las estadísticas sin
  tráfico y sin aviso, y que en la figura 3 las etiquetas se enciman cuando las dos series
  convergen. Ahora esa corrida se descarta con su motivo, y una prueba exige 12 px entre
  etiquetas.
- **#20:** Emmanuel corrió las dos mitades de E8 juntas en WSL2 y encontró que una corrida cuyos
  nodos mueren antes de conectarse costaba 35 s, porque el coordinador espera a propósito. La
  espera pasó a depender de que los nodos hayan salido bien: 2 s en vez de 35.
- **#22:** en la revisión, una cifra del informe decía «cuatro barreras» y los registros
  muestran dos. Se corrigió en el #23, contra los diez registros de esa configuración.

Las decisiones se anotaron en [`docs/decisiones_diseno.md`](docs/decisiones_diseno.md) en el
mismo pull request que las implementó, con su fecha, su contexto y las alternativas descartadas.

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

## Versión entregada

- **Repositorio:** https://github.com/ebarrantes07/SOA-P3-GrupoIE
- **Etiqueta:** `p3-entrega` (anotada)

El identificador completo del commit etiquetado se registra en el comprobante de entrega y no
acá, porque este README forma parte de ese commit.
