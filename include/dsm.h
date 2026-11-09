/* dsm.h — API pública de la biblioteca de memoria compartida distribuida.
 *
 * El acceso es explícito: la aplicación declara cuándo empieza y cuándo
 * termina de usar una página, y solo durante ese intervalo el puntero sirve.
 * No hay transparencia por fallos de página: no se intercepta SIGSEGV, no se
 * usa userfaultfd y no se fija una dirección virtual común entre nodos.
 *
 * Congelada en E0 (#1); la implementación es #5. Lo que sigue es el contrato
 * que el enunciado pide documentar, en tres partes: propiedad de la memoria,
 * tratamiento de errores, y la diferencia entre permiso activo y copia
 * retenida en caché.
 *
 *
 * PROPIEDAD DE LA MEMORIA
 *
 * La biblioteca es dueña de todo. DSM_acquire no entrega memoria nueva:
 * devuelve un puntero al marco de página que el contexto ya tiene reservado
 * para esa página. Quien llama nunca libera ese puntero, no lo guarda más
 * allá del release, y no lo pasa a otro hilo. El puntero puede repetirse
 * entre adquisiciones sucesivas de la misma página: que la dirección sea la
 * de la vez anterior no significa que el contenido siga siendo válido.
 *
 * Solo DSM_finalize libera el contexto. Después de llamarlo, el puntero al
 * contexto y cualquier puntero a página obtenido de él son inválidos.
 *
 * Las direcciones virtuales son distintas en cada proceso, así que un puntero
 * nunca se transmite ni se guarda dentro de una página como referencia: lo
 * que se comparte son valores y desplazamientos dentro de la página.
 *
 *
 * PERMISO ACTIVO Y COPIA EN CACHÉ
 *
 * Son dos cosas distintas y confundirlas es el error más fácil de cometer:
 *
 *   - El *permiso* es el derecho a leer o escribir ahora. Vive solo entre
 *     DSM_acquire y DSM_release, y lo concede el coordinador.
 *   - La *copia* son los bytes que quedaron en la caché local del nodo tras
 *     un acceso anterior. Puede seguir siendo correcta, puede estar
 *     invalidada, y en cualquier caso no autoriza nada.
 *
 * Tener la copia ahorra tráfico, no permisos: todo DSM_acquire consulta al
 * coordinador. Si la versión en caché coincide con la confirmada, la página
 * no se reenvía, pero el permiso se pide igual. Desreferenciar un puntero
 * conservado después del release es un error de uso del que la biblioteca no
 * protege: no hay detección por hardware de ese caso, y el enunciado no la
 * exige.
 *
 * Dentro del intervalo autorizado, las lecturas y escrituras son locales y no
 * generan un mensaje por palabra.
 *
 *
 * REGLAS DE USO QUE LA BIBLIOTECA SÍ VERIFICA
 *
 * Estas se rechazan localmente, antes de enviar nada por la red, y devuelven
 * DSM_EINVAL o DSM_ESTATE según el caso:
 *
 *   - Como máximo una página adquirida por nodo; no hay adquisiciones
 *     anidadas.
 *   - No se llama a DSM_release sin una adquisición en curso, ni se libera
 *     una página distinta de la adquirida.
 *   - No se entra a DSM_barrier con una página adquirida.
 *   - DSM_finalize solo se invoca sin páginas adquiridas.
 *   - page_id dentro de rango y modo conocido.
 *
 * Lo que la biblioteca no puede verificar: que en modo DSM_READ nadie escriba
 * a través del puntero. El contrato lo prohíbe y queda del lado de la
 * aplicación.
 *
 *
 * TRATAMIENTO DE ERRORES
 *
 * DSM_acquire devuelve NULL ante cualquier error; las demás funciones
 * devuelven 0 en éxito y -1 en error. En los dos casos el motivo se consulta
 * con DSM_last_error, que no se borra hasta la llamada siguiente que pueda
 * fallar. Un error de sesión (DSM_ECONN, DSM_ETIMEOUT, DSM_EPROTO,
 * DSM_EABORT) es terminal: la política ante la pérdida de un participante o
 * del coordinador es abortar la sesión, no reconectar ni recuperar escrituras
 * no publicadas. Tras un error terminal, toda llamada posterior falla con el
 * mismo código y lo único que queda por hacer es DSM_finalize. Los que
 * esperan en una barrera cuando la sesión cae despiertan con error, no se
 * quedan colgados.
 */
#ifndef SOA_P3_DSM_H
#define SOA_P3_DSM_H

#include <stdint.h>

#include "wire.h"

/* Contexto opaco de participación en una sesión. Un proceso trabajador tiene
 * uno y un solo hilo de aplicación que lo usa; los hilos internos del runtime
 * no cuentan como aplicaciones adicionales. El contexto no es seguro para
 * usarse desde varios hilos de aplicación a la vez. */
typedef struct dsm_ctx dsm_ctx;

/* Modo de acceso. El número que viaja por la red es DSM_WIRE_MODE_*, no este
 * valor (ver D2). */
enum dsm_mode {
    DSM_READ  = 1,
    DSM_WRITE = 2
};

/* Códigos de error. También son los valores que lleva el campo aux de una
 * trama ERROR, así que no se reordenan. */
enum dsm_status {
    DSM_OK = 0,
    DSM_EINVAL,     /* argumento inválido: page_id fuera de rango, modo
                     * desconocido, nodes/nid incoherentes */
    DSM_ESTATE,     /* la operación no corresponde al estado: adquisición
                     * anidada, release sin acquire, barrera con página
                     * adquirida, finalize con una página en la mano */
    DSM_ETIMEOUT,   /* venció el timeout de operación */
    DSM_ECONN,      /* se cerró la conexión o falló el socket */
    DSM_EPROTO,     /* trama inválida o incoherente con el protocolo */
    DSM_EABORT,     /* el coordinador abortó la sesión */
    DSM_ENOMEM,     /* no se pudo reservar memoria */
    DSM_EIO         /* error al escribir el registro local */
};

/* Timeout de operación por omisión, en milisegundos. El enunciado fija 10
 * segundos para las pruebas funcionales locales y permite ajustarlo para
 * instrumentación. */
#define DSM_DEFAULT_TIMEOUT_MS  10000u

/* Conecta con el coordinador, valida la sesión y espera a que estén
 * registrados los `nodes` nodos esperados. No admite incorporaciones tardías:
 * si la sesión ya arrancó, falla.
 *
 * `host` es una dirección IP o un nombre; `nid` identifica a este trabajador
 * y debe estar en [0, nodes-1]. Devuelve el contexto, o NULL y escribe el
 * motivo en *err si err no es NULL.
 *
 * Bloquea hasta que la sesión esté completa o venza el timeout. */
dsm_ctx *DSM_init(const char *host, uint16_t port, uint32_t nodes, uint32_t nid,
                  enum dsm_status *err);

/* Solicita permiso READ o WRITE sobre una página y bloquea hasta obtener la
 * autorización y datos vigentes.
 *
 * En DSM_READ pueden coexistir varios lectores. En DSM_WRITE la exclusividad
 * cubre todo el intervalo: mientras dure, ningún otro nodo tiene permiso ni
 * conserva una copia válida utilizable de esa página. Un leer-modificar-
 * escribir hecho dentro de un mismo intervalo WRITE es atómico respecto de
 * los demás nodos; partirlo en un READ y un WRITE separados no lo es, y
 * ninguna barrera posterior lo arregla.
 *
 * Devuelve el puntero local a la página (propiedad de la biblioteca, válido
 * solo hasta DSM_release), o NULL en error. */
void *DSM_acquire(dsm_ctx *ctx, uint32_t page_id, enum dsm_mode mode);

/* Termina el acceso a `page_id`, que debe ser el adquirido.
 *
 * En DSM_WRITE envía la página completa y espera la confirmación de su
 * publicación: cuando esta función retorna 0, los datos ya son la versión
 * confirmada y cualquier adquisición posterior, de cualquier nodo, la
 * observará. Si falla, el estado de la publicación es indeterminado y la
 * sesión queda en error.
 *
 * En DSM_READ registra el fin de la lectura y espera el ACK (D3). La copia
 * puede quedar en la caché como copia revocable, que no autoriza a leerla de
 * nuevo sin volver a adquirir.
 *
 * Devuelve 0 en éxito y -1 en error. */
int DSM_release(dsm_ctx *ctx, uint32_t page_id);

/* Barrera colectiva reutilizable, identificada por `barrier_id` y por una
 * generación que lleva el coordinador. Retorna cuando llegaron todos los
 * nodos de esa generación.
 *
 * No transfiere páginas y no hace visible nada por sí misma: la visibilidad
 * viene del release del escritor y de la siguiente adquisición del lector.
 * Todos los nodos deben invocar los mismos barrier_id en el mismo orden
 * lógico, y ninguno puede entrar con una página adquirida.
 *
 * Devuelve 0 en éxito y -1 en error. Si la sesión falla mientras se espera,
 * retorna -1 en vez de quedarse bloqueada. */
int DSM_barrier(dsm_ctx *ctx, uint32_t barrier_id);

/* Cierra la participación y libera sockets, memoria e hilos internos. Solo se
 * invoca sin páginas adquiridas; con una adquirida falla con DSM_ESTATE y no
 * libera nada.
 *
 * Espera a que terminen los hilos que creó. Después de un retorno exitoso,
 * `ctx` es inválido. Devuelve 0 en éxito y -1 en error; en los dos casos, si
 * liberó el contexto, no se vuelve a usar. */
int DSM_finalize(dsm_ctx *ctx);

/* Último error registrado en el contexto. DSM_OK si no hubo ninguno desde la
 * última llamada que pudiera fallar. */
enum dsm_status DSM_last_error(const dsm_ctx *ctx);

/* Texto de un código de error, en español y sin salto de línea final. Nunca
 * devuelve NULL. */
const char *DSM_strerror(enum dsm_status s);

#endif /* SOA_P3_DSM_H */
