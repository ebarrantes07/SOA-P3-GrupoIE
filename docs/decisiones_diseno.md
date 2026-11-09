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
