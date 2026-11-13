# Decisiones de diseño

Bitácora viva de decisiones de diseño del Proyecto 3 (MC6004 SOA). Ninguna entrada se borra aunque
una decisión posterior la reemplace: en ese caso la entrada nueva dice explícitamente a cuál
reemplaza y por qué. El objetivo es que cualquiera (incluido el profesor) pueda reconstruir por qué
el código quedó como quedó sin tener que preguntar.

Las entradas se numeran **`Di.j`**, donde `i` es la épica que originó la decisión (E0 a E9, las
mismas de los issues) y `j` es un consecutivo dentro de esa épica: `D1.3` es la tercera decisión
que salió de E1. Es un índice, no un orden cronológico. La razón es práctica: con dos personas
trabajando en ramas paralelas, una numeración corrida hace que las dos tomen el mismo número
siguiente y haya que renumerar al fusionar. Con `Di.j` cada épica tiene su propio espacio y la
colisión no puede ocurrir.

---

## D0.1 — El coordinador es un proceso de un solo hilo con `poll()`

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

## D0.2 — Formato de trama: cabecera fija de 32 bytes, campo por campo, en orden de red

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
- `reserved` deja espacio para un campo chico futuro sin cambiar `DSM_HEADER_SIZE`, sin inventar un
  tipo de mensaje y sin tocar esa regla de recepción. Para que eso sirva de algo, `wire_check` exige
  que hoy valga cero: un receptor que lo ignorara seguiría adelante interpretando mal una trama que
  no entiende, mientras que rechazarla hace que el cambio se note el día que ocurra. Un byte
  reservado que nadie verifica no está reservado, está sin usar.
- De paso, el byte deja la cabecera en 32 y todos los campos multibyte en múltiplos de 4 (`length`
  en 8, `request_id` en 16, `aux` en 28), lo que hace legible un volcado hexadecimal de 16 bytes por
  línea. Sin él serían 31 bytes y offsets impares. No es por alineación de acceso: los enteros se
  arman byte por byte con corrimientos (D1.1), así que ningún campo se lee como entero desde el buffer.

### Alternativa descartada

Cabecera de longitud variable con campos opcionales por tipo. Ahorra algunos bytes y complica
todas las validaciones; con 4 nodos y páginas de 4 KB, 32 bytes de cabecera son irrelevantes
frente a la carga. El byte reservado cuesta 1 de 4136 en una trama con página: 0,02 %.

### Una trampa que conviene dejar escrita

`sizeof(struct wire_header)` también da 32 en x86-64, pero es casualidad y los offsets **no**
coinciden con el cable: en memoria `length` cae en 4 y `request_id` en 24, mientras que en el cable
van en 8 y en 16. Quien vea los dos 32 iguales y se tiente con un `memcpy` de la estructura al
buffer produce una trama con los campos corridos. Es exactamente lo que esta decisión prohíbe al
exigir serializar campo por campo.

---

## D0.3 — El `release` de READ también se confirma con ACK

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

## D0.4 — Identificadores en inglés, comentarios en español

- **Fecha:** 2026-11-08
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/1)
- **Quién decide:** ambos

Misma convención del Proyecto 2. Los identificadores del código y los nombres de archivo van en
inglés (`struct page`, `readers_active`, `wire.h`); los comentarios, los mensajes de error que ve
el usuario y toda la documentación van en español. El motivo es que el vocabulario técnico del
enunciado y de las páginas de manual es inglés, y traducirlo a medias produce nombres peores.

---

## D0.5 — Qué se reutiliza del Proyecto 2 y qué no

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

## D0.6 — El repositorio está público, y eso hay que corregirlo antes de la entrega

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

## D0.7 — La configuración de la región la define el coordinador

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

## D0.8 — La generación de barrera la cuenta el trabajador y la valida el coordinador

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

## D0.9 — Lo ajustable va en `struct dsm_options`, no en la firma de `DSM_init`

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

## D0.10 — Ciclo de vida del contexto: qué libera `DSM_finalize` y cuándo

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

## D0.11 — Los códigos de error son contrato de cable, con valores explícitos

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

## D0.12 — La clave de correlación es (nodo solicitante, `request_id`)

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

## D0.13 — `BYE`: salida ordenada, y cuándo es un error

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

## D0.14 — `dsm.h` es el encabezado público; `wire.h` es interno

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

## D0.15 — `make test` con límite global, además del límite por binario

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

## D1.1 — El transporte tiene dos niveles, y los plazos son absolutos

- **Fecha:** 2026-11-09
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel propone, ambos acuerdan al planear el issue

### Contexto

Los dos lados del sistema necesitan mover bytes, pero con restricciones opuestas. El coordinador es
un ciclo `poll()` de un solo hilo (D0.1) y **no puede bloquearse nunca**: si se queda esperando a
que un trabajador lea, deja de atender a los otros tres. El hilo receptor del trabajador, en cambio,
quiere justamente bloquearse hasta que llegue la respuesta que espera.

### Decisión

`io.h` expone los dos niveles:

- **Un intento:** `io_read_some` / `io_write_some` mueven lo que se pueda ahora y devuelven
  `IO_AGAIN` si no se puede nada. El coordinador hace un intento, guarda lo pendiente en su buffer
  y sigue con los demás.
- **El ciclo completo:** `io_recv_all` / `io_send_all` insisten hasta mover todos los bytes o hasta
  que venza el plazo, esperando en `poll` cuando no hay nada que hacer.

Los plazos son **instantes absolutos** del reloj monotónico, no duraciones por llamada.

### Por qué

Una sola implementación sirve a los dos frentes, así que el manejo de transferencias parciales,
`EINTR` y `EAGAIN` vive en un lugar y no en dos. La alternativa —que E1 entregara solo el nivel
bloqueante y E3 escribiera sus primitivas— habría dejado dos capas de I/O distintas en el mismo
proyecto.

Los plazos absolutos resuelven un problema concreto: una trama de 4136 bytes puede necesitar varias
escrituras parciales, y con un timeout por llamada cada una recibiría los 10 segundos completos. El
timeout total sería el producto, no el límite. Con un instante límite calculado una vez, las tres
esperas comparten el mismo presupuesto.

### Lo que esto obliga

Para que el plazo se respete, el descriptor debe ser **no bloqueante** (ver D1.3): con uno
bloqueante, `recv` duerme dentro del sistema sin mirar el plazo, y este solo acotaría las esperas
que llegan a ver `EAGAIN`. Queda documentado como precondición en `io.h`.

---

## D1.2 — `TCP_NODELAY` activado en todas las conexiones

- **Fecha:** 2026-11-09
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** ambos

### Decisión

Todo socket conectado que sale de `net.c` lleva `TCP_NODELAY`.

### Por qué

El algoritmo de Nagle retiene un segmento pequeño esperando juntarlo con el siguiente, para no
desperdiciar red con paquetes de pocos bytes. El protocolo de este proyecto es casi todo mensajes
de control chicos: una solicitud son 40 bytes, un ACK de invalidación son 32. Con Nagle activo,
esos mensajes pueden quedar retenidos decenas de milisegundos, y entonces el experimento de
localidad de lectura y el de costo de distribución **medirían el buffer de TCP en vez del
protocolo**.

Queda anotado acá y no solo en el código porque afecta los números del informe: cualquiera que
intente reproducir las mediciones necesita saber que esta opción está puesta.

### Lo que se pierde

Más paquetes en la red para el mismo tráfico útil. Con 4 nodos por loopback es irrelevante, y en la
corrida complementaria en dos equipos tampoco domina frente a los 4 KB de una página.

---

## D1.3 — Todos los sockets son no bloqueantes, y `MSG_NOSIGNAL` en vez de `SIGPIPE`

- **Fecha:** 2026-11-09
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel

### Decisión

`net.c` pone `O_NONBLOCK` en todo descriptor que crea, incluido el que escucha y los que vienen de
`accept`. El modo no bloqueante se activa **antes** de `connect`. Y `io.c` usa `send`/`recv` con
`MSG_NOSIGNAL`, no `read`/`write`.

### Por qué el no bloqueante

Es la precondición de los plazos de D1.1. Y en `connect` es más que eso: un `connect` bloqueante se
queda dentro del sistema el tiempo que el sistema decida —en Linux, del orden de dos minutos de
reintentos de SYN— así que un trabajador apuntando a un coordinador que no está se colgaría, y el
timeout de 10 segundos del enunciado no existiría. No bloqueante, `connect` vuelve con `EINPROGRESS`
y la espera la controla el plazo.

Un detalle que no es evidente: un socket a medio conectar se vuelve escribible **tanto si la
conexión funcionó como si falló**. `poll` no distingue; hay que preguntar `SO_ERROR`. Saltarse ese
paso hace que un rechazo parezca un éxito.

### Por qué `MSG_NOSIGNAL`

Escribir a un socket cuyo par ya cerró levanta `SIGPIPE`, cuya acción por omisión es **terminar el
proceso**. Sin la bandera, la prueba de desconexión que el enunciado exige no fallaría: el
trabajador desaparecería sin dejar registro ni código de salida útil, y la suite se caería sin
diagnóstico. Con la bandera, el mismo caso devuelve `EPIPE`, que `io.c` traduce a `IO_CLOSED`.

El costo es que el módulo trabaja sobre sockets y no sobre tuberías ni archivos, porque
`MSG_NOSIGNAL` es de `send`/`recv`. El proyecto solo usa TCP, así que no se pierde nada.

---

## D1.4 — El límite de fragmentación se configura por variable de entorno

- **Fecha:** 2026-11-09
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel

### Contexto

El enunciado exige una prueba de robustez que limite cada envío y cada recepción a entre 1 y 7
bytes, y pida los mismos resultados funcionales.

### Decisión

`io.c` acota cada operación a `DSM_IO_CHUNK_MAX`, leída del entorno una sola vez. Un valor fuera de
[1, 65536] se ignora; 0 significa sin límite. `io_set_chunk_limit` existe para las pruebas
unitarias.

### Por qué del entorno y no de un parámetro

La prueba de punta a punta de #8 tiene que correr **los binarios reales**, con sus procesos
separados, sin recompilar nada ni pasar banderas por toda la cadena de llamadas. Una variable de
entorno se hereda sola.

Se lee con `pthread_once` porque el hilo de aplicación y el hilo receptor pueden llegar a la vez, y
una inicialización perezosa con una bandera común sería una carrera que ThreadSanitizer reportaría
con razón. El valor se guarda en un `_Atomic` porque las pruebas lo cambian.

Un valor inválido se ignora en silencio en vez de escribir a `stderr`: esto es una biblioteca y no
le corresponde contaminar la salida del programa. Quien quiera verificar qué quedó configurado
llama a `io_chunk_limit`, y el registro del trabajador lo anota.

---

## D1.5 — Tres capas, y el coordinador no usa la de más arriba

- **Fecha:** 2026-11-09
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel

### Decisión

El transporte queda en tres módulos con una sola dirección de dependencia:

| Módulo | Traduce | Depende de |
|---|---|---|
| `wire` | bytes ↔ campos | nada (ni sockets) |
| `io` | bytes ↔ socket | nada del protocolo |
| `frame` | tramas ↔ socket | `wire` y `io` |

El hilo receptor del trabajador usa `frame`. El coordinador **no**: arma las tramas con su propio
buffer de entrada y `wire_decode_header`, porque su ciclo `poll()` no puede bloquearse (D0.1).

### Por qué

`wire` sin red se prueba completo sin abrir un socket, igual que el directorio de #3 se prueba sin
red: las dos piezas donde vive la corrección se verifican con secuencias de datos, no con
escenarios. Y `io` sin protocolo se puede ejercitar con patrones de bytes cualquiera, lo que hace
que la prueba de fragmentación sea independiente de qué mensajes existan.

Que el coordinador no use `frame` no es una omisión: es la consecuencia de D0.1. Vale dejarlo escrito
para que nadie "arregle" después la duplicación aparente metiendo `frame_recv` en el ciclo `poll()`,
que es exactamente el error que bloquearía al coordinador esperando a un nodo lento.

---

## D1.6 — Cada trama sale en una sola escritura, y un solo emisor por descriptor

- **Fecha:** 2026-11-11
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel, a partir de la revisión de Isaac en el PR #13

### Contexto

`frame_send` enviaba la cabecera y la carga con dos llamadas a `io_send_all`. Con `TCP_NODELAY`
activado (D1.2), eso produce dos segmentos por cada trama con carga: los 32 bytes de cabecera viajan
solos y la página de 4096 va aparte. D1.2 existe para que los experimentos midan el protocolo y no
el algoritmo de Nagle, y partir cada mensaje en dos trabaja contra ese mismo objetivo.

Hay un segundo problema en la misma función. Dos `io_send_all` seguidos no son atómicos, y en el
trabajador hay dos hilos que envían por el mismo socket: el de aplicación (`ACQ_REQ`, los releases,
las barreras) y el receptor (`INVALIDATE_ACK`). Si se intercalan, el flujo queda corrupto sin
remedio, porque el receptor usa `length` como única autoridad de dónde termina cada trama.

### Decisión

`frame_send` arma la trama completa en un buffer local de `DSM_MAX_FRAME` (4136 bytes) y la entrega
en **una sola** llamada a `io_send_all`.

Y el contrato de `frame_send` declara la precondición: **un solo emisor por descriptor a la vez**;
si varios hilos envían por el mismo socket, quien llama serializa. La función no lleva candado. La
serialización vive en `libdsm` (#5), con un mutex de envío por contexto.

### Por qué

Medido con un shim de `LD_PRELOAD` sobre `send`, enviando un `ACQ_GRANT` con página:

| | llamadas a `send` |
|---|---|
| Antes | 2: una de 32 bytes, otra de 4104 |
| Después | 1: de 4136 bytes |

La alternativa era `sendmsg` con dos `iovec`, que evita copiar la página. Se descartó por una razón
concreta: ante una escritura parcial habría que avanzar sobre el arreglo de `iovec` recalculando
cuánto se consumió de cada uno, o sea un **segundo** ciclo de transferencia parcial además del que
`io_send_all` ya tiene probado. El buffer local deja todo el manejo de `EINTR`, `EAGAIN` y plazos en
el único camino que las pruebas ya ejercitan, y no cambia `io.h`. El costo es un `memcpy` de hasta
4096 bytes por trama, despreciable frente a la llamada al sistema y a la red.

Los 4136 bytes van en la pila: con dos hilos que envían y 8 MB de pila por hilo, no es un problema.
Queda además un chequeo redundante de `length > DSM_MAX_PAYLOAD` antes de copiar. Hoy no puede
dispararse, porque `wire_check` ya acota la longitud al máximo de cada tipo; se queda porque es lo
único que separa un tipo nuevo mal declarado en la tabla de `wire.c` de un desborde de esa pila.

### Una sola llamada a `io_send_all` no es una sola llamada al sistema

Es importante no confundir las dos cosas, porque de eso depende que la precondición siga siendo
necesaria. Con el límite de fragmentación en 1000 (D1.4), la misma trama sale así:

```
send #1: 1000 bytes    send #4: 1000 bytes
send #2: 1000 bytes    send #5:  136 bytes
send #3: 1000 bytes
```

Cinco llamadas al sistema, y entre cualquiera de ellas otro hilo podría meter sus bytes en el medio.
Enviar en una sola operación lógica reduce los segmentos en el caso normal; **no** vuelve atómico el
envío. Por eso la precondición es parte del contrato y no una consecuencia de este cambio.

### Cómo se validó

Las 62 pruebas de la suite pasan, y también con `DSM_IO_CHUNK_MAX` en 1, en 7 y en 1000: los
round-trip de `test_frame` recorren el camino nuevo con la trama partida en pedazos de un byte, que
es el caso que atraparía un error en el offset de la copia. El conteo de `send` se midió con el shim
descrito arriba, antes y después.

---

## D1.7 — El plazo de `net_connect` es para la llamada completa y se reparte entre las direcciones

- **Fecha:** 2026-11-11
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/2)
- **Quién decide:** Emmanuel, a partir de la revisión de Isaac en el PR #13

### Contexto

`net_connect` resuelve con `AF_UNSPEC`, así que un nombre puede dar varias direcciones —típicamente
una IPv6 y una IPv4— y se prueban en orden. El plazo que recibe es absoluto, y se le pasaba entero a
cada intento. La primera dirección que no respondiera agotaba el plazo completo y la función
devolvía `NET_TIMEOUT` sin llegar a probar las siguientes.

Por loopback no se nota, porque hay una sola dirección y responde al instante. En la corrida en dos
equipos que pide el enunciado sí: un IPv6 configurado pero sin ruta se queda esperando hasta el
plazo, y la IPv4 que sí funciona nunca se intenta.

### Decisión

El plazo es para la llamada completa y se reparte entre las direcciones que faltan por probar,
recalculándolo en cada vuelta con lo que queda. La aritmética vive en `net_deadline_share`:

- sin plazo (0) devuelve 0;
- un plazo ya vencido se devuelve tal cual, para que `io_wait` lo reporte de inmediato;
- si no, `ahora + (plazo − ahora) / faltantes`.

Si una dirección agota su parte, se abandona **esa dirección** y se sigue con la siguiente. El
estado final distingue los dos motivos: `NET_TIMEOUT` si alguna agotó su parte, `NET_ERROR` si todas
fallaron antes de eso.

Se corrigen dos cosas más de la misma función. La variable que acumulaba el estado de salida solo
tomaba el valor `NET_ERROR`, así que se eliminó. Y el `errno` del último fallo se guarda en una
variable y se restaura antes de retornar: `net.h` promete que en `NET_ERROR` el `errno` queda con el
motivo, y asignarlo dentro del ciclo no lo cumplía, porque el `socket()` de la vuelta siguiente lo
pisa.

### Por qué repartir y no otra cosa

La alternativa era dejarlo como estaba y documentarlo ("el plazo es para el primer intento"). Se
descartó porque el enunciado pide la corrida en dos equipos, que es exactamente el escenario donde
el orden de las direcciones decide si la conexión funciona, y porque un plazo que no significa lo
mismo para la primera dirección que para las demás es difícil de usar bien desde afuera.

Repartir en partes iguales y recalcular con lo que queda tiene una propiedad útil: si una dirección
falla rápido, la siguiente hereda más plazo del que le tocaba al principio, y la última siempre
recibe todo lo que sobre.

### Cómo se validó, y qué quedó sin validar

Esto hay que decirlo con precisión, porque las dos primeras pruebas que se escribieron **no
servían**: el código anterior las pasaba igual, y se comprobó corriéndolas contra él.

- `deadline_share` prueba la aritmética directamente, con un "ahora" fijo en vez del reloj: sin
  plazo, una sola dirección, dos (mitad y mitad), tres (el redondeo queda para la última), una
  primera que falla rápido y le deja más a la siguiente, un plazo vencido y `left` en 0. Quitando el
  divisor correcto o la guarda del plazo vencido, la prueba falla.
- `connect_respects_deadline` comprueba que el plazo se respeta contra una dirección de TEST-NET-2
  (RFC 5737), que no está enrutada. Eso prueba el plazo, **no** el reparto.
- `error_keeps_errno` fija el contrato de `errno`, pero **no** atrapa el defecto original: ese
  necesita que el nombre resuelva a dos direcciones o más y que la primera falle. En la máquina
  donde se desarrolló, `localhost` resuelve solo a `127.0.0.1`. La prueba deja el caso escrito con
  `localhost` para que cubra de verdad en una máquina que lo resuelva a `::1` y `127.0.0.1`.

O sea: el reparto está probado en su aritmética y el camino de varias direcciones no se recorre en
esta máquina. Por eso la aritmética se sacó a una función propia en vez de quedar embebida en el
ciclo — es la única parte que se puede fijar con una prueba.

### Un detalle para no confundir al que lea el código

La salida temprana `if (left <= 1u) return deadline_ns;` **no** es lo que hace que la última
dirección se quede con todo: con `left == 1` la división general ya da exactamente el plazo. Lo
único que agrega esa rama es no dividir por cero si `left` llegara en 0. Se escribió `<= 1` en vez
de `== 0` para que el caso de la última dirección quede visible al leer, y está anotado en los tres
lugares porque un mutante que cambie `<= 1` por `== 0` no hace fallar ninguna prueba, y eso podría
leerse como que la rama no sirve.

---

## D2.1 — El directorio es una máquina de estados que recibe eventos y emite acciones

- **Fecha:** 2026-11-09, corregida el 2026-11-10 con la revisión del PR #12
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/3)
- **Quién decide:** Isaac; la corrección de la correlación sale de un hallazgo de Emmanuel

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
- **Un acceso completo lleva un solo `request_id`**, el de su `ACQ_REQ`, que se repite en la
  concesión, en el release y en su confirmación. Los `request_id` de un nodo **crecen
  estrictamente** (pueden saltar, nunca repetirse) y el directorio rechaza un `ACQ_REQ` que no
  crezca y un release con otro número.
- **Una ronda de invalidación se identifica por (solicitante, `request_id`)**, y el
  `INVALIDATE_ACK` repite las dos partes: el `request_id` y, en `aux`, el solicitante que traía el
  `INVALIDATE`. Con la regla anterior esa clave es única en toda la sesión.
- **Un ACK repetido devuelve `DIR_DUPLICATE`** y no cambia nada. Se reconoce mientras su ronda sea
  la última de la página; uno de una ronda anterior se rechaza con `DIR_ESTATE`. El coordinador
  registra el repetido y lo descarta; no aborta.
- **La despedida cuenta como ACK** de cualquier ronda que esperaba a ese nodo (D0.13), porque su
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
solo hilo (D0.1), nada puede cambiar la página entre la decisión y el envío.

### La correlación, y por qué el primer diseño no alcanzaba

La primera versión emparejaba un `INVALIDATE_ACK` solo por `request_id`. Emmanuel mostró en la
revisión una secuencia en la que un nodo repite un `request_id` en dos WRITE sobre la misma
página: el ACK rezagado de la primera ronda se cuenta en la segunda y el WRITE se concede antes de
que la invalidación real se confirme. Lo grave es que `dir_check_invariants` no lo ve, porque
desde dentro la ronda se completó de forma legítima, y ninguna semilla de la prueba de estrés lo
encuentra, porque los trabajadores simulados nunca repiten números.

Su propuesta era exigir `request_id` crecientes por nodo. Eso cierra su secuencia, pero no
alcanza solo: los `request_id` son únicos **por nodo**, y dos solicitantes distintos pueden usar
el mismo número. Un ACK rezagado de la ronda del nodo 0 con `request_id` 5 encajaría en una ronda
posterior del nodo 2 con `request_id` 5. Por eso el ACK repite también al solicitante. Las dos
reglas juntas vuelven única la clave, y el enunciado pide justamente que el coordinador valide la
"correspondencia de ACK antes de modificar su estado".

De la misma familia: un release tiene que llevar el `request_id` de su acquire. Si no, un
`REL_WRITE` con un número equivocado pasaba sin detectarse y el trabajador no podría emparejar la
confirmación con su operación pendiente.

Costo: el trabajador repite en el ACK un campo que ya recibió, sin bytes extra en la cabecera, y
`libdsm` (#5) lleva un contador de `request_id` por contexto. Del lado del transporte, `wire_check`
puede validar el `aux` del `INVALIDATE_ACK` con la misma regla que el del `INVALIDATE`.

### Cómo se validó

17 casos en `tests/test_directory.c`: uno por criterio de aceptación de #3, las validaciones,
cuatro de correlación (`request_id` creciente, release con su `request_id`, el ACK rezagado del
mismo solicitante que encontró Emmanuel y el de otro solicitante con el mismo número), y una
prueba de estrés de 20 semillas × 5000 pasos con cuatro trabajadores simulados que confirman
invalidaciones en orden aleatorio. Los simulados usan `request_id` con saltos y que se repiten
entre nodos. Después de cada paso se comprueban los invariantes, que la versión concedida sea la
última publicada, que los bytes que usa el trabajador (recibidos o de su caché) sean los de esa
versión y que cada confirmación repita el `request_id` del acceso.

Para comprobar que las pruebas detectan errores reales se inyectaron 15 bugs, uno a la vez, y
todos hicieron fallar al menos una prueba. Entre ellos: conceder WRITE sin esperar ACK o
lectores, dejar que un READ se cuele, contar un ACK duplicado, subir la versión después del ACK,
confiar en la versión declarada, no tratar la despedida como ACK, permitir `request_id`
repetidos, no verificar el `request_id` del release y, el que motivó la corrección, emparejar el
ACK sin el solicitante: con solo la regla de `request_id` creciente,
`stale_ack_other_requester_same_id` falla.

---

## D3.1 — El registro es un CSV de 20 columnas, y es un formateador sin reloj ni estado propio

- **Fecha:** 2026-11-11
- **Issue:** [#4](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/4)
- **Quién decide:** Emmanuel

### Contexto

El enunciado pide un registro con cuatro grupos de campos —identificación, estado de la página,
tráfico y tiempo— y una secuencia global creciente. D0.5 ya había decidido no reutilizar el
`logger.c` del Proyecto 2, porque sus columnas son las del buffer acotado y recibe tipos de ese
dominio; el esquema se define donde se usa, y el del coordinador es este issue.

### Decisión

Un CSV con estas 20 columnas, en este orden:

```
run_id, component, seq, node_id, request_id, page_id, event, status,   <- identificación
mode, version, writer, readers, readers_mask, inval_pending,           <- estado
msgs_in, msgs_out, bytes_in, bytes_out, page_xfers, invals,            <- tráfico
dur_ns                                                                 <- tiempo
```

Cuatro propiedades fijan el diseño:

**No lleva reloj.** La duración la calcula quien registra y llega en la fila ya hecha. El módulo
no llama a `now_monotonic_ns` nunca. Es lo que vuelve sus pruebas deterministas: la misma
secuencia de llamadas produce el mismo archivo byte por byte, así que se comparan archivos
completos en vez de verificar rangos.

**No duplica el estado.** Las cinco columnas de estado salen de un `struct dir_page_info`, la
instantánea que el directorio ya produce (D2.1). No hay copia que mantener al día. Una copia
podría desincronizarse del directorio, y entonces el registro mentiría justo cuando más importa,
que es al auditar un invariante roto.

**Lo ausente es celda vacía, no centinela.** Un `BARRIER_RELEASE` no tiene página y un
`SESSION_START` no tiene nodo: esas celdas quedan vacías. No pueden quedar en 0 porque el nodo 0 y
la página 0 son reales, y no se escribe `0xFFFFFFFF` porque los scripts de E8 leerían la columna
como número y un promedio quedaría contaminado con 4294967295. Una celda vacía es un valor
faltante en cualquier herramienta de análisis.

**El tráfico lo cuenta el módulo, no quien registra.** `log_count_sent` y `log_count_recv` reciben
el tipo de trama y la longitud de carga, y derivan solos los bytes totales, si llevaba página y si
era una invalidación. Quien llama no recibe ningún contador, así que **no puede contar dos veces**:
el criterio de #4 que pide que el tamaño de página se cuente una sola vez por transmisión queda
satisfecho por la forma de la interfaz, no por disciplina al usarla. Se invocan desde el único
lugar donde una trama cruza de verdad el socket.

### Dos detalles que podrían parecer descuidos

**`status` es columna propia, no va en `event`.** La primera versión escribía el estado de error en
la columna de evento. Con dos columnas, una fila de error conserva **qué** se intentaba y **por
qué** falló; con una sola se perdía lo primero.

**El token del estado no es `DSM_strerror`.** Por dos razones. La práctica: `DSM_strerror` lo
implementa `libdsm` (#5) y el coordinador no se enlaza contra la biblioteca del trabajador — al
intentar usarlo, el enlazado falló. Y la de fondo: sirven a lectores distintos. `DSM_strerror` da
una frase en español para que una aplicación la muestre; esta columna necesita un token corto,
estable y sin comas que se pueda agrupar. Que las dos tablas no coincidan no es una
inconsistencia: no describen lo mismo.

### Cómo se validó

13 casos en `tests/test_log.c`. Al no haber reloj, nueve comparan el archivo completo contra el
texto esperado e informan la primera línea que difiere. Los otros cubren los rechazos de apertura
(nombre con coma, salto de línea o comilla; nombre demasiado largo; ruta imposible), que un
registro nulo acepte todas las llamadas sin hacer nada, que el nodo 0 salga como `0` y no como
ausente, que la secuencia sea una sola para todos los nodos y páginas, y que los contadores de una
fila sean los del momento de escribirla.

Se inyectaron cuatro errores, uno a la vez, y cada uno hace fallar su prueba: contar como
transferencia una concesión sin datos, contar el ACK de invalidación como otra invalidación, no
incrementar la secuencia y olvidar la cabecera en el conteo de bytes.

Un quinto —quitar el chequeo de tipo en la derivación de página— **no hacía fallar nada**, porque
con tramas válidas es inalcanzable: solo `ACQ_GRANT` y `REL_WRITE` admiten carga de 4096 o más, y
la mayor de las demás es `ERROR` con 256. Se agregó un caso que le pasa a propósito un tipo chico
con longitud de página, porque estas funciones no validan tramas y el chequeo de tipo es lo único
que las separa de contar una transferencia inventada. Con ese caso, el quinto mutante también cae.

---

## D3.2 — Cada conexión tiene dos buffers fijos, y la salida llena significa un nodo que no lee

- **Fecha:** 2026-11-11
- **Issue:** [#4](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/4)
- **Quién decide:** Emmanuel

### Contexto

El issue pide un buffer de entrada y otro de salida por conexión *"de modo que una escritura
parcial no bloquee la recepción de nada"*. Con un solo hilo (D0.1), bloquearse en un nodo detiene
a los otros tres, que es justo lo que el enunciado prohíbe.

Ya existe `frame.h` (D1.5), pero no sirve acá: `frame_recv` lee hasta completar la trama y
`frame_send` escribe hasta vaciarla. Las dos son correctas para el hilo receptor del trabajador,
que no tiene nada mejor que hacer mientras espera.

### Decisión

El ciclo es el inverso al de `frame`: se leen los bytes que haya, se extraen las tramas
**completas** que ya estén, y lo que quedó a medias espera en el buffer hasta la próxima vuelta del
`poll()`. Igual del lado de la salida: se encolan tramas enteras y se escribe lo que el socket
acepte.

**El núcleo no toca sockets.** `conn_feed`, `conn_next`, `conn_queue` y `conn_take` son funciones
puras sobre los dos buffers; el socket aparece solo en `conn_recv` y `conn_send`, que son un
intento de `io_read_some`/`io_write_some` y nada más. Es lo que permite forzar de forma
determinista los casos que en una red real aparecen por azar.

**Una cabecera inválida no se consume.** Se reporta el motivo y los bytes se quedan donde están.
Podría parecer que conviene saltarse los 32 bytes y resincronizar, pero si la cabecera es basura su
`length` también lo es, así que no se sabe dónde empezaría la trama siguiente: no hay nada a qué
resincronizar. El flujo murió y quien llama aborta esa sesión, que es lo mismo que ya decía D1.5.

### El buffer de salida, y por qué puede fallar al encolar

Lo que un nodo puede tener encolado está acotado por el protocolo: a lo sumo una concesión con
página (4136 bytes), porque un nodo tiene a lo sumo un acceso en curso; a lo sumo N−1
invalidaciones, porque cada ronda abierta corresponde a una solicitud WRITE en la cabeza de alguna
página y cada una es de un nodo distinto (D2.1); y unas pocas confirmaciones. Menos de 5 KB.

Pero esa cota vale para un nodo que se comporta. Uno que deje de leer su socket y siga enviando
—por ejemplo, llegadas a barrera de varias generaciones sin esperar ninguna liberación— puede
acumular salida sin tope. Por eso `conn_queue` puede devolver false, y cuando lo hace **no es un
error del coordinador**: es un nodo que no respeta el protocolo, y se trata como una desconexión.

Se descartó un buffer creciente: solo cambiaría el síntoma de "se llenó el buffer de un nodo" a
"se agotó la memoria del coordinador", que es peor porque se lleva la sesión entera en vez de una
conexión. La capacidad quedó en 16 KB, cómodamente por encima de los 5 KB del caso legítimo, para
que llenarla signifique de verdad que el par dejó de leer y no que la cota estaba justa.

### Cómo se validó

13 casos, casi todos sin red: una trama partida en 4136 pedazos de un byte (ninguna trama hasta el
último), dos tramas en una sola lectura, una entera más el principio de la siguiente, una cabecera
inválida que no se consume, los bytes encolados comparados contra lo que serializa el emisor, el
rechazo de una cabecera inválida sin tocar el buffer, la salida llena que rechaza sin truncar, y
una escritura parcial reanudada en pedazos de 100, 1 y 500 bytes que reconstruye los bytes exactos.

El último caso sí usa TCP: un hilo emisor con el límite de fragmentación en 1 byte manda tres
tramas con página, para que lo anterior no quede verificado solo contra mi propio modelo de cómo
llegan los bytes.

Se inyectaron cinco errores. Cuatro caen de inmediato: consumir la cabecera inválida, entregar la
trama sin esperar la carga completa, encolar sin validar y no avanzar la cabeza en `conn_take`.

El quinto —no compactar nunca el buffer de entrada— **pasaba las 12 pruebas**. Los dos casos
fáciles no lo ejercitan: si se consume todo, `conn_next` devuelve la cabeza a cero por su cuenta, y
si el sobrante es chico lo que llega entra al final sin mover nada. Hace falta que la cabeza esté
avanzada **y** que lo que llega no quepa al final aunque sí quepa en el total. Se agregó ese caso,
construido con la aritmética exacta del buffer, y con él el mutante revienta con SEGV en vez de
pasar.

---

## D3.3 — La sesión lleva nodos y barreras, sin conocer sockets ni páginas

- **Fecha:** 2026-11-12
- **Issue:** [#4](https://github.com/ebarrantes07/SOA-P3-GrupoIE/issues/4)
- **Quién decide:** Emmanuel

### Contexto

El coordinador tiene tres clases de estado: el de las páginas (el directorio, D2.1), el de los
bytes en vuelo (las conexiones, D3.2) y el de los participantes — quién se registró, quién se fue,
y en qué generación va cada barrera. Esto último es lo que falta.

### Decisión

Un módulo que habla solo de `node_id`. **No mapea nodos a conexiones**: qué descriptor le
corresponde a cada uno lo sabe el coordinador. Si guardara descriptores, sus pruebas necesitarían
sockets para nada, y los criterios de #4 sobre registro y barreras no se podrían verificar hasta
que exista el ciclo `poll()`.

Tampoco verifica si un nodo tiene páginas adquiridas: eso es estado del directorio y lo responde
`dir_node_leave`. Ante un `BYE`, quien llama hace las dos comprobaciones (D0.13).

### Barreras: tabla fija, y el casillero no se libera

El `barrier_id` viene en `aux` y es un u32, así que no se puede indexar un arreglo con él. Hay una
tabla de 16 casilleros, y cada uno recuerda la generación **abierta** de su barrera.

El casillero **no se libera al liberarse la barrera**. Si se liberara, se perdería la generación, y
la ronda siguiente llegaría con un número que no se podría validar contra nada — que es
exactamente lo que D0.8 quiere detectar. Una barrera, una vez usada, se queda con su casillero.

Al agotarse la tabla, una barrera nueva se rechaza con `DSM_EINVAL` en vez de desalojar a otra.
Desalojar sería silencioso y volvería a perder la generación; rechazar se ve. Las cuatro
aplicaciones del enunciado usan un puñado de barreras, pero el protocolo admite cualquier u32 y el
coordinador no puede confiar en que el otro lado se porte.

### Tres reglas que no estaban escritas en ningún lado

**La primera generación de la vida de una barrera es la 0.** El trabajador manda cuántas veces *ya*
pasó (D0.8), así que la primera vez son cero. Un casillero nuevo arranca con la 0 abierta, y una
primera llegada con otro número se rechaza en vez de aceptarse como punto de partida: un nodo
desincronizado se ve en su primera barrera, no ochenta generaciones después.

**Un nodo espera en una sola barrera a la vez.** Después de llegar queda bloqueado hasta la
liberación, así que una llegada a otra barrera solo puede venir de un trabajador desincronizado. Se
rechaza para que se vea ahí, en vez de aparecer como una barrera que no libera nunca. Es el mismo
invariante que el directorio tiene para los accesos.

**Un `BYE` durante el registro aborta.** Irse antes de que estén los N deja a la sesión sin poder
alcanzarlos nunca. Se aborta en el momento en vez de esperar al plazo, que solo podría decir "nadie
llegó". Es la misma política del enunciado ante la pérdida de un participante que ya aplicaba D0.13
para las barreras.

### Dos cosas que salieron de que la prueba no coincidiera con el código

**Un nodo esperando en dos barreras.** La prueba afirmaba que llegar a una segunda barrera se
rechaza, y el módulo lo aceptaba. Ganó la prueba: era la regla correcta y faltaba implementarla.

**Una llegada rechazada reservaba casillero.** Al arreglar lo anterior, la prueba siguió fallando
por otro motivo: la reserva ocurría antes de validar, así que una llegada inválida consumía un
casillero. Con 16 casilleros, un trabajador desincronizado podía agotar la tabla a fuerza de
rechazos y dejar sin barreras a la sesión entera. Ahora se valida todo primero y se reserva solo al
aceptar; una barrera que no existe tiene su generación abierta en 0 de forma implícita, que es lo
que permite validar antes de crear.

### Cómo se validó

20 casos, todos sin red y sin directorio. Cubren los criterios de #4 sobre registro (N = 1, 2 y 4;
ids fuera de orden; id repetido; incorporación tardía; lo que el trabajador declara, incluido el
"sin opinión" de D0.7) y sobre barreras (liberación con todos, reutilización a lo largo de 50
generaciones, una sola llegada por nodo y generación, generación adelantada y atrasada, tabla
agotada, y que una rechazada no consuma casillero).

Seis errores inyectados, cada uno hace fallar su prueba: contar la llegada duplicada, no validar la
generación, no avanzarla al liberar, no abortar el `BYE` con una barrera esperando, reservar
casillero antes de validar y aceptar una incorporación tardía.

El último merece una nota, porque al principio **no fallaba**. Con los N registrados, todo id
válido ya está ocupado, así que un `HELLO` tardío cae igual en "id repetido" y devuelve el mismo
`DSM_ESTATE`. Lo único que distingue al chequeo es el **motivo** cuando el `HELLO` tardío trae
además argumentos equivocados: con el chequeo es `ESTATE` ("llegaste tarde"), y sin él es `EINVAL`
("revisá tu configuración"), que manda al nodo a buscar un problema que no tiene. Se agregó ese
caso, y con él el mutante cae.
