# Decisiones de diseño

Bitácora viva de decisiones de diseño del Proyecto 3 (MC6004 SOA). Cada entrada queda numerada
(D1, D2…) y no se borra aunque una decisión posterior la reemplace — en ese caso la entrada nueva
dice explícitamente a cuál reemplaza y por qué. El objetivo es que cualquiera (incluido el
profesor) pueda reconstruir por qué el código quedó como quedó sin tener que preguntar.

---

## D1 — El coordinador es un proceso de un solo hilo con `poll()`

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

### Contexto

El coordinador mantiene, por página, los datos confirmados, la versión, los lectores activos, el
escritor, las copias en caché y las solicitudes pendientes. El enunciado pide además que las
solicitudes de páginas diferentes se puedan procesar de forma independiente y que "ninguna espera
podrá retener un bloqueo local necesario para recibir la respuesta esperada".

Las dos formas naturales de construirlo son un hilo por trabajador con un mutex sobre el
directorio, o un solo hilo con multiplexación de entrada/salida.

### Decisión

Un solo hilo con un ciclo `poll()` y un buffer de entrada y otro de salida por conexión.

### Por qué

- El directorio deja de tener carreras **por construcción**: no hay dos hilos que lo toquen, así
  que no hay que razonar sobre bloqueos ni sobre su orden.
- La secuencia creciente de eventos que el enunciado exige en el log sale correcta sin esfuerzo:
  hay un único punto que la asigna.
- El orden de los mensajes hacia cada socket queda garantizado. Con un hilo por trabajador, una
  concesión podría adelantarse a una invalidación enviada antes por otro hilo, y ese es
  exactamente el tipo de error que el protocolo no debe admitir.
- ThreadSanitizer no tiene prácticamente nada que analizar en este proceso, así que su valor se
  concentra donde sí hay concurrencia local: la biblioteca del trabajador.

La independencia entre páginas no viene de los hilos sino de las colas: una solicitud que no se
puede conceder se encola en su página y el ciclo sigue atendiendo las demás.

### Costo que se acepta

Hay que manejar escrituras parciales con `POLLOUT` y un buffer de salida por conexión, porque una
página de 4096 bytes puede no entrar de una sola vez en el buffer del socket. Es trabajo real,
pero acotado, y de todas formas hay que escribir el manejo de transferencias parciales para el
lado de recepción.

---

## D2 — Formato de trama: cabecera fija de 32 bytes, campo por campo, en orden de red

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

### Contexto

El enunciado exige un protocolo enmarcado con versión, tipo, longitud, `node_id`, `request_id` y
`page_id`; enteros de tamaño fijo con orden de bytes definido; y prohíbe enviar estructuras C
mediante `sizeof(struct)` o valores de `enum` dependientes del compilador.

### Decisión

Cabecera de 32 bytes, serializada campo por campo en orden de red (big-endian), con este layout:

| Offset | Campo | Tipo | Notas |
|---|---|---|---|
| 0 | `magic` | u32 | `0x44534D33` (`"DSM3"`) |
| 4 | `version` | u8 | 1 |
| 5 | `type` | u8 | código numérico fijo, declarado en `wire.h` |
| 6 | `flags` | u8 | bit 0 = la trama trae los datos de la página |
| 7 | `reserved` | u8 | 0 |
| 8 | `length` | u32 | bytes de carga después de la cabecera |
| 12 | `node_id` | u32 | emisor o destinatario según el tipo |
| 16 | `request_id` | u64 | correlación entre solicitud, concesión, invalidación y ACK |
| 24 | `page_id` | u32 | `0xFFFFFFFF` cuando no aplica |
| 28 | `aux` | u32 | campo por tipo: modo, `barrier_id`, código de error |

Las versiones de página son de 64 bits y viajan en la carga, no en `aux`.

### Por qué

- Los códigos de tipo se declaran como constantes con valor explícito y el byte en el cable es
  exactamente ese número. Un `enum` del compilador nunca se serializa.
- `request_id` de 64 bits permite correlacionar sin reutilizar identificadores durante una sesión,
  que es lo que el log necesita para reconstruir una transición READ a WRITE.
- `aux` evita inventar un tipo de mensaje por cada variante pequeña, sin volver la cabecera
  variable.
- Una cabecera de tamaño fijo hace trivial la regla de recepción: leer 32 bytes, validar, y leer
  exactamente `length` más.

### Alternativa descartada

Cabecera de longitud variable con campos opcionales por tipo. Ahorra algunos bytes y complica
todas las validaciones; con 4 nodos y páginas de 4 KB, 32 bytes de cabecera son irrelevantes
frente a la carga.

---

## D3 — El `release` de READ también se confirma con ACK

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

### Contexto

El enunciado describe `DSM_release` así: "En WRITE envía la página completa y espera la
confirmación de su publicación. En READ registra el fin de la lectura". Para READ no exige
explícitamente una confirmación, así que cabría enviar el mensaje y seguir.

### Decisión

El coordinador responde un ACK también al `release` de READ, y la biblioteca espera ese ACK antes
de devolver el control a la aplicación.

### Por qué

- `DSM_release` queda con un contrato uniforme: cuando retorna, el coordinador ya sabe que el
  acceso terminó. Sin eso, el valor de retorno de un release de READ no significaría nada.
- Hace verificable desde el log del coordinador que nadie entra a una barrera con un acceso vivo.
  Si el release fuera sin respuesta, una llegada a la barrera podría aparecer en el log antes del
  fin de la lectura que la precedió, y no habría forma de distinguir un error real de un reorden
  de registro.
- Simplifica el cierre: `DSM_finalize` solo se invoca sin páginas adquiridas, y con ACK esa
  precondición es comprobable localmente sin ambigüedad.

### Costo que se acepta

Un viaje de ida y vuelta por cada lectura. En el experimento de localidad de lectura se mide y se
reporta: es parte del costo del control que el enunciado pide distinguir de la transferencia de
datos.

---

## D4 — Identificadores en inglés, comentarios en español

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

Misma convención del Proyecto 2. Los identificadores del código y los nombres de archivo van en
inglés (`struct page`, `readers_active`, `wire.h`); los comentarios, los mensajes de error que ve
el usuario y toda la documentación van en español. El motivo es que el vocabulario técnico del
enunciado y de las páginas de manual es inglés, y traducirlo a medias produce nombres peores.

---

## D5 — Qué se reutiliza del Proyecto 2 y qué no

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

### Decisión

Se reutiliza de `SOA-P2-GrupoIE`, con la procedencia declarada en cada archivo:

- `clock.h` / `clock.c` — reloj monotónico. Tal cual, sin cambios de fondo.
- Los conversores estrictos de `cli` (`cli_parse_u32`, `cli_parse_u64`). El análisis de argumentos
  por rol es distinto en este proyecto y se escribe cuando existan los ejecutables (E3 y E4).
- El arnés de pruebas `tests/test_util.h` y la estructura del `Makefile`.
- Los scripts de experimentos y el esqueleto LaTeX del informe, cuando lleguen E8 y E9.

**No** se reutilizan `logger.c` ni `metrics.c`. Los dos existen en el P2 pero están atados a su
dominio: el logger escribe las columnas del CSV del buffer acotado y recibe `struct message` y
`struct op_snapshot`; las métricas calculan percentiles de latencia de ese experimento. El log de
este proyecto tiene otros campos (secuencia global, `request_id`, `page_id`, modo, versión,
lectores, invalidaciones pendientes, tráfico), así que copiarlos ahora significaría reescribirlos
dos veces. Se escriben donde se define su esquema: el del coordinador en E3 y el del trabajador
en E4.

---

## D6 — El repositorio está público, y eso hay que corregirlo antes de la entrega

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Estado:** **pendiente**

El enunciado exige un repositorio privado: *"Cada grupo creará un repositorio privado llamado
SOA-P3-GrupoXXX"* y *"El repositorio permanecerá privado"*. Al crearlo quedó público y el equipo
decidió dejarlo así por ahora para no interrumpir el arranque.

Queda anotado como pendiente explícito, con dos razones para no olvidarlo: es un requisito escrito
del enunciado, y mientras esté público cualquier otro grupo puede leer la solución. Se cambia con
`gh repo edit ebarrantes07/SOA-P3-GrupoIE --visibility private --accept-visibility-change-consequences`
o desde Settings → General → Danger Zone. Esta entrada se actualiza cuando se haga.

---

## D7 — La configuración de la región la define el coordinador

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, ambos acuerdan

### Contexto

`HELLO` declaraba `pages_expected`, pero `DSM_init` no recibía la cantidad de páginas, así que la
biblioteca no tenía de dónde sacar ese valor. Las dos salidas eran agregar un parámetro a la API o
que el coordinador sea la única autoridad.

### Decisión

El coordinador define la región. El trabajador manda `pages_expected = 0` ("sin opinión") y toma
el valor de `SESSION_START`. La aplicación lo consulta con `DSM_page_count`.

El campo se conserva en `HELLO`: si llega distinto de cero y no coincide con la sesión, el
coordinador rechaza el registro.

### Por qué

Con una sola autoridad, el trabajador no tiene cómo discrepar y desaparece toda una familia de
errores de configuración. El campo se mantiene porque el enunciado pide que el coordinador valide
la sesión; dejarlo con significado "0 = sin opinión" permite que una prueba afirme una expectativa
y verifique que un desajuste se rechaza, en vez de volverlo un relleno.

`DSM_page_count` hace falta porque la aplicación de comparación de granularidad usa una página por
nodo: sin accesor no tendría forma de saber cuántas hay.

---

## D8 — La generación de barrera la cuenta el trabajador y la valida el coordinador

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, ambos acuerdan

### Contexto

`dsm.h` decía que la generación la llevaba el coordinador, pero `BARRIER_ARRIVE` la manda el
trabajador. Con el campo en la trama sin dueño claro, no quedaba definido qué significa que los
dos números no coincidan.

### Decisión

Cada trabajador cuenta, por `barrier_id`, cuántas veces ya pasó por él, y manda ese número en su
llegada. El coordinador valida: rechaza una llegada repetida del mismo nodo en la misma
generación, y una generación distinta de la que está abierta.

### Por qué

Implementa literalmente la regla del enunciado de admitir una sola llegada por nodo y generación.
Y convierte en error detectable lo que de otro modo sería una desincronización silenciosa: un nodo
que se saltea un `barrier_id` o que los invoca en otro orden llega con una generación que no
corresponde, y eso se ve en el momento en vez de aparecer 80 generaciones después como un
resultado raro.

---

## D9 — Lo ajustable va en `struct dsm_options`, no en la firma de `DSM_init`

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, ambos acuerdan

### Contexto

El enunciado dice que el timeout de operación "puede ajustarse para instrumentación", y solo
existía la constante `DSM_DEFAULT_TIMEOUT_MS`. Lo mismo pasaba con la ruta del CSV del trabajador.

### Decisión

`DSM_init` recibe un `const struct dsm_options *` opcional con `timeout_ms` y `log_path`. `NULL`, o
un campo en cero o `NULL`, toma el valor por omisión. La estructura y la cadena se copian dentro.

### Por qué

Las cuatro aplicaciones obligatorias de #7 van a llamar a `DSM_init`. Agregar parámetros después
significa tocarlas todas; agregar un campo a la estructura, no. Es el momento más barato para
decidirlo, y es justo lo que este PR congela.

---

## D10 — Ciclo de vida del contexto: qué libera `DSM_finalize` y cuándo

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, ambos acuerdan

### Contexto

El contrato decía "si liberó el contexto, no se vuelve a usar", lo que no le dice a quien llama si
puede seguir usando `ctx` después de un error.

### Decisión

- Con una página adquirida: devuelve -1 con `DSM_ESTATE` y **no libera nada**; el contexto sigue
  válido.
- En cualquier otro caso **siempre libera**: devuelve 0 si el cierre fue limpio, -1 si la sesión ya
  estaba en error o falló la despedida, y en ambos casos `ctx` queda inválido.

Es decir: solo `DSM_ESTATE` deja el contexto vivo.

### Por qué

Una regla con una sola excepción se puede recordar y verificar. La consecuencia práctica queda
escrita en el encabezado: `DSM_last_error` se consulta antes de `DSM_finalize`, nunca después.

---

## D11 — Los códigos de error son contrato de cable, con valores explícitos

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, ambos acuerdan

### Decisión

`enum dsm_status` lleva valores explícitos (`DSM_EINVAL = 1`, `DSM_ESTATE = 2`, …) y no se
reordena. Un código desconocido recibido por la red se traduce a `DSM_EPROTO`.

### Por qué

Esos números son los que viajan en el campo `aux` de una trama `ERROR`. El riesgo **no** es el
compilador: los enumeradores de C sin inicializador valen 0, 1, 2… por norma, y lo que se
serializa es un `u32` que se escribe a mano, así que la representación del `enum` nunca viaja. El
riesgo es humano: que alguien inserte un código en medio de la lista dentro de unos días y cambie
el protocolo sin que nada avise. Con los valores escritos, moverlos es un acto deliberado y
visible en el diff.

---

## D12 — La clave de correlación es (nodo solicitante, `request_id`)

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac observa, se resuelve en la trama

### Contexto

Cada trabajador asigna sus propios `request_id`, así que un `request_id` por sí solo no identifica
una operación: dos nodos pueden emitir el mismo número. En las tramas que van del coordinador a un
trabajador, `node_id` es el destinatario, no el solicitante, así que en un `INVALIDATE` el nodo que
pidió la escritura no aparecía en ninguna parte.

### Decisión

La clave es `(node_id solicitante, request_id)`, y queda escrito en `wire.h`. `INVALIDATE` lleva el
`node_id` del solicitante en su campo `aux`, que para ese tipo estaba libre.

### Por qué

Isaac proponía documentarlo y que el log del coordinador registrara al solicitante. Resolverlo en
la trama es más barato y más robusto: no agrega un byte a la cabecera, la trama se explica sola, y
el log no depende de que la implementación cruce información aparte. Ese log es exactamente lo que
va a leer el verificador de invariantes de #8, que tiene que demostrar que la concesión de
escritura ocurrió después de los ACK de *esa* solicitud.

Para emparejar un `INVALIDATE_ACK` sigue alcanzando `(page_id, nodo que responde, request_id)`,
porque el ACK viaja en sentido contrario.

---

## D13 — `BYE`: salida ordenada, y cuándo es un error

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac observa la falta, se define aquí

### Decisión

Al recibir `BYE`, el coordinador saca al nodo del conjunto de copias retenidas de todas las
páginas y de los lectores activos, y cierra su conexión. Un `BYE` es legítimo solo si ese nodo no
tiene ninguna página adquirida y si ninguna barrera abierta queda sin poder completarse. Si el nodo
se despide mientras otros esperan una barrera que necesita su llegada, la sesión se aborta y los
que esperaban despiertan con `DSM_EABORT`.

### Por qué

Sin la primera parte, el coordinador podría mandarle un `INVALIDATE` a un nodo que ya se fue y
quedarse esperando un ACK que nunca llega: un bloqueo permanente por una despedida normal. La
segunda parte es la política del enunciado ante la pérdida de un participante — abortar, no seguir
con menos nodos.

Isaac ofreció tomarlo en #3, pero la definición va aquí porque es semántica de `wire.h` y `wire.h`
es lo que este PR congela. La implementación sí es de #4.

---

## D14 — `dsm.h` es el encabezado público; `wire.h` es interno

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac propone, se invierte la dirección del include

### Contexto

`dsm.h` incluía `wire.h`, así que una aplicación veía todo el protocolo solo para usar
`DSM_PAGE_SIZE`.

### Decisión

Los límites públicos (`DSM_PAGE_SIZE`, `DSM_MIN/MAX_NODES`, `DSM_MIN/MAX_PAGES`) se definen en
`dsm.h`. `wire.h` **incluye** `dsm.h` y se queda solo con lo del protocolo: magia, versión, layout
de la cabecera, códigos de tipo, banderas y centinelas. Una aplicación incluye únicamente `dsm.h`.

### Por qué

Isaac proponía definir el tamaño de página en `dsm.h` y dejar `wire.h` interno, lo que habría
dejado el mismo número en dos archivos — que es como se desincronizan. Invertir el include lo deja
en un solo lugar. Además `wire.h` ahora comprueba con `_Static_assert` que `DSM_READ`/`DSM_WRITE`
valgan lo mismo que `DSM_WIRE_MODE_READ`/`WRITE`: si alguien cambia uno, la compilación falla en
vez de romper el protocolo en silencio.

---

## D15 — `make test` con límite global, además del límite por binario

- **Fecha:** 2026-11-09
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1) · revisión del PR #11
- **Quién decide:** Isaac observa, ambos acuerdan

### Contexto

El criterio de aceptación pide un límite **global** de tiempo y el `Makefile` aplicaba uno por
binario: con N binarios, el peor caso era N veces el límite individual.

### Decisión

El ciclo de pruebas vive en `scripts/run_tests.sh` y el `Makefile` lo invoca entero bajo un
`timeout` (`TEST_TOTAL_TIMEOUT`, 900 s por omisión). Adentro cada binario conserva su propio límite
(`TEST_TIMEOUT`, 300 s), para poder atribuir un cuelgue a la prueba que lo causó.

### Por qué, y un detalle que costó dos intentos

Envolver el ciclo en un `timeout` tiene una trampa: al vencer, la señal llega al script y el
binario de prueba que estaba corriendo **sobrevive como huérfano**. El enunciado exige que no quede
ningún proceso del programa después de la suite, así que eso sería un incumplimiento introducido
por el propio arreglo.

El script atiende la señal y mata al grupo de procesos de la prueba en curso. Dos detalles
importan: el PID que se obtiene de poner `timeout` en segundo plano es el del `timeout`, no el del
binario, así que matarlo a él antes de que reenvíe la señal deja el binario suelto; y el trabajo se
lanza con `setsid` para que sea líder de su grupo, de modo que `kill -- -PID` alcance a los dos sin
depender de ninguna carrera. Verificado con una prueba que duerme 30 s, en tres corridas seguidas,
comprobando por nombre de proceso que no queda nada vivo.

---

## D16 — El directorio es una máquina de estados que recibe eventos y emite acciones

- **Fecha:** 2026-11-09
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/3)
- **Quién decide:** Isaac

### Contexto

El directorio es el núcleo de corrección: decide a quién conceder, a quién invalidar y cuándo. El
issue pide probarlo sin red, y el coordinador (#4) tiene que poder usarlo sin que el directorio
sepa nada de sockets ni de tramas.

### Decisión

`directory.h` expone un evento por cada mensaje que le importa (`dir_acquire`,
`dir_invalidate_ack`, `dir_release_write`, `dir_release_read`, `dir_node_leave`) y responde
emitiendo acciones por un callback (`GRANT`, `INVALIDATE`, `WRITE_PUBLISHED`, `READ_RELEASED`),
cada una con todo lo que necesita la trama: destinatario, página, `request_id`, solicitante, modo,
versión y, si corresponde, los datos. El coordinador traduce cada acción a una trama dentro del
callback; las pruebas la registran.

Detalles que quedan fijados:

- **Las acciones se emiten en el orden en que deben enviarse.** Un release WRITE publica y sube la
  versión antes de emitir su confirmación, y la confirmación sale antes que las concesiones que
  habilita. El coordinador no reordena nada.
- **Las copias retenidas son una máscara, sin versión por nodo.** La versión solo cambia al
  publicar un WRITE, y antes de concederlo se invalidaron todas las copias ajenas; por lo tanto
  toda copia retenida es de la versión vigente. Los datos se omiten solo si el nodo figura como
  copia retenida **y** declara esa versión: el directorio no confía en la declaración sola.
- **La ronda de invalidación se abre cuando el WRITE llega a la cabeza y no hay lectores activos**,
  no antes. Es lo que pide el enunciado ("se espera a que terminen los accesos activos
  anteriores") y deja un único momento en que se calcula a quién invalidar.
- **Un ACK repetido devuelve `DIR_DUPLICATE`** y no cambia nada. Se reconoce mientras su ronda sea
  la última de la página. El coordinador lo registra y lo descarta; no aborta.
- **La despedida cuenta como ACK** de cualquier ronda que esperaba a ese nodo (D13), porque su
  copia deja de existir.
- **Un evento rechazado no modifica nada ni emite nada.** El coordinador decide si responde
  `ERROR` o aborta.
- **`dir_check_invariants`** verifica SWMR, que lectores y ACK pendientes estén contenidos en las
  copias retenidas, que cada nodo ocupado aparezca en un solo lugar, y además una condición de
  progreso: después de cada evento la cola quedó tan adelantada como se podía. El coordinador
  puede llamarla en modo de depuración.

### Por qué

Con el callback, el directorio no reserva colas de acciones ni conoce tramas, y el puntero a los
datos de una concesión es válido mientras el coordinador los copia: como el coordinador tiene un
solo hilo (D1), nada puede cambiar la página entre la decisión y el envío.

### Cómo se validó

13 casos en `tests/test_directory.c`: uno por criterio de aceptación de #3, las validaciones, y
una prueba de estrés de 20 semillas × 5000 pasos con cuatro trabajadores simulados que confirman
invalidaciones en orden aleatorio. Después de cada paso se comprueban los invariantes, que la
versión concedida sea la última publicada y que los bytes que usa el trabajador, recibidos o de
su caché, sean los de esa versión. Para comprobar que las pruebas detectan errores reales se
inyectaron 11 bugs, uno a la vez (conceder WRITE sin esperar ACK o lectores, dejar que un READ se
cuele, contar un ACK duplicado, subir la versión después del ACK, confiar en la versión declarada,
no tratar la despedida como ACK, entre otros): todos hicieron fallar al menos una prueba.
