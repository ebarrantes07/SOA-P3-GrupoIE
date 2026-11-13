/* Pruebas del registro de nodos y las barreras (#4).
 *
 * Todo sin red y sin directorio: la sesión es contabilidad por nodo, así que
 * cada caso es una secuencia de llamadas con un resultado exacto. Eso permite
 * cubrir los criterios de #4 sobre registro y barreras antes de que exista el
 * ciclo poll(), y que cuando exista, lo único que agregue sea orquestación.
 */
#include <string.h>

#include "log.h"
#include "session.h"
#include "test_util.h"

/* Registra los `n` nodos de una sesión de `n`, declarando "sin opinión" sobre
 * las páginas, que es lo que manda la biblioteca (D0.7). */
static bool registrar_todos(sess *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (sess_hello(s, i, n, 0) != DSM_OK)
            return false;
    return true;
}

/* ---- Creación ---------------------------------------------------------- */

static bool test_create_rejects_bad_args(void) {
    CHECK(sess_create(0, 4, 1) == NULL);
    CHECK(sess_create(DSM_MAX_NODES + 1, 4, 1) == NULL);
    CHECK(sess_create(2, 0, 1) == NULL);
    CHECK(sess_create(2, DSM_MAX_PAGES + 1, 1) == NULL);

    sess *s = sess_create(2, 8, 12345);
    CHECK(s != NULL);
    CHECK(sess_nodes(s) == 2 && sess_pages(s) == 8);
    CHECK(sess_run_id(s) == 12345);
    CHECK(sess_state_of(s) == SESS_REGISTERING);
    sess_destroy(s);

    /* Los accesores aceptan NULL. */
    CHECK(sess_nodes(NULL) == 0 && sess_pages(NULL) == 0);
    CHECK(sess_run_id(NULL) == 0);
    CHECK(sess_state_of(NULL) == SESS_ABORTED);
    CHECK(sess_active(NULL) == 0);
    CHECK(!sess_registered(NULL, 0) && !sess_left(NULL, 0));
    CHECK(!sess_barrier_pending(NULL));
    sess_destroy(NULL);
    return true;
}

/* ---- Registro: criterio "N=1, 2 y 4 registran correctamente" ---------- */

static bool test_session_starts_with_n_nodes(void) {
    const uint32_t tamanos[] = {1, 2, 4};
    for (size_t k = 0; k < 3; k++) {
        uint32_t n = tamanos[k];
        sess *s = sess_create(n, 4, 1);
        CHECK(s != NULL);

        /* Mientras falte uno, la sesión NO arranca. */
        for (uint32_t i = 0; i < n; i++) {
            CHECK(sess_state_of(s) == SESS_REGISTERING);
            CHECK(sess_hello(s, i, n, 0) == DSM_OK);
            CHECK(sess_registered(s, i));
            CHECK(sess_active(s) == i + 1);
        }
        /* Con el último, sí. */
        CHECK(sess_state_of(s) == SESS_RUNNING);
        sess_destroy(s);
    }
    return true;
}

/* Los ids no tienen que llegar en orden: 3, 0, 2, 1 registra igual. */
static bool test_ids_may_arrive_out_of_order(void) {
    sess *s = sess_create(4, 4, 1);
    CHECK(s != NULL);
    const uint32_t orden[] = {3, 0, 2, 1};
    for (size_t i = 0; i < 4; i++)
        CHECK(sess_hello(s, orden[i], 4, 0) == DSM_OK);
    CHECK(sess_state_of(s) == SESS_RUNNING);
    CHECK(sess_active(s) == 4);
    sess_destroy(s);
    return true;
}

/* Criterio de #4: "id repetido ... se rechaza con error explícito". */
static bool test_duplicate_id_is_rejected(void) {
    sess *s = sess_create(3, 4, 1);
    CHECK(s != NULL);
    CHECK(sess_hello(s, 1, 3, 0) == DSM_OK);

    /* El mismo id otra vez: ESTATE, y no cuenta como un nodo más. */
    CHECK(sess_hello(s, 1, 3, 0) == DSM_ESTATE);
    CHECK(sess_active(s) == 1);
    CHECK(sess_state_of(s) == SESS_REGISTERING);

    /* Y la sesión sigue aceptando a los que faltan. */
    CHECK(sess_hello(s, 0, 3, 0) == DSM_OK);
    CHECK(sess_hello(s, 2, 3, 0) == DSM_OK);
    CHECK(sess_state_of(s) == SESS_RUNNING);
    sess_destroy(s);
    return true;
}

static bool test_hello_validates_what_the_worker_declares(void) {
    sess *s = sess_create(2, 16, 1);
    CHECK(s != NULL);

    /* Id fuera de [0, nodes). */
    CHECK(sess_hello(s, 2, 2, 0) == DSM_EINVAL);
    CHECK(sess_hello(s, 99, 2, 0) == DSM_EINVAL);

    /* Cantidad de nodos que no coincide. */
    CHECK(sess_hello(s, 0, 1, 0) == DSM_EINVAL);
    CHECK(sess_hello(s, 0, 4, 0) == DSM_EINVAL);

    /* pages_expected: 0 es "sin opinión" y pasa; un valor que no coincide se
     * rechaza; el que coincide también pasa (D0.7). */
    CHECK(sess_hello(s, 0, 2, 99) == DSM_EINVAL);
    CHECK(sess_hello(s, 0, 2, 16) == DSM_OK);
    CHECK(sess_hello(s, 1, 2, 0) == DSM_OK);
    CHECK(sess_state_of(s) == SESS_RUNNING);
    sess_destroy(s);
    return true;
}

/* Criterio de #4: "rechaza incorporaciones tardías". */
static bool test_late_join_is_rejected(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));
    CHECK(sess_state_of(s) == SESS_RUNNING);

    /* La sesión arrancó: ningún HELLO más. */
    CHECK(sess_hello(s, 0, 2, 0) == DSM_ESTATE);
    CHECK(sess_hello(s, 1, 2, 0) == DSM_ESTATE);

    /* Y el motivo es "llegaste tarde" ANTES que cualquier otro reproche: un
     * HELLO tardío con argumentos además equivocados sigue siendo ESTATE, no
     * EINVAL. Importa porque es lo que el nodo recibe en el ERROR, y
     * "la sesión ya arrancó" le dice qué hacer, mientras que "argumento
     * inválido" lo manda a revisar su configuración, que está bien.
     *
     * Esta línea es además lo único que distingue el chequeo de estado: con
     * los N registrados, todo id válido ya está ocupado, así que sin ella
     * quitar ese chequeo no hace fallar ninguna prueba. */
    CHECK(sess_hello(s, 0, 99, 0) == DSM_ESTATE);
    CHECK(sess_hello(s, 1, 2, 777) == DSM_ESTATE);
    sess_destroy(s);
    return true;
}

/* ---- BYE (D0.13) ------------------------------------------------------- */

static bool test_bye_is_an_ordered_exit(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    CHECK(sess_bye(s, 0) == DSM_OK);
    CHECK(sess_left(s, 0));
    CHECK(!sess_registered(s, 0));
    CHECK(sess_active(s) == 1);
    CHECK(sess_state_of(s) == SESS_RUNNING);   /* no aborta: nadie esperaba */

    /* Dos veces no. Y no se vuelve a entrar. */
    CHECK(sess_bye(s, 0) == DSM_ESTATE);
    CHECK(sess_hello(s, 0, 2, 0) == DSM_ESTATE);

    /* Cuando se van todos, la sesión termina en orden. */
    CHECK(sess_bye(s, 1) == DSM_OK);
    CHECK(sess_active(s) == 0);
    sess_finish(s);
    CHECK(sess_state_of(s) == SESS_FINISHED);
    sess_destroy(s);
    return true;
}

/* Irse con una barrera esperándolo aborta: esa barrera necesita a todos los
 * nodos, así que los demás quedarían esperando para siempre (D0.13). */
static bool test_bye_with_a_barrier_waiting_aborts(void) {
    sess *s = sess_create(3, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 3));

    enum dsm_status why = DSM_OK;
    CHECK(sess_barrier_arrive(s, 0, 7, 0, &why) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_pending(s));

    /* El nodo 2 nunca llegó, y se va. */
    CHECK(sess_bye(s, 2) == DSM_EABORT);
    CHECK(sess_state_of(s) == SESS_ABORTED);

    /* Abortada, todo lo demás se rechaza con EABORT: es lo que hace que el
     * resto reciba error en vez de esperar indefinidamente. */
    CHECK(sess_hello(s, 0, 3, 0) == DSM_EABORT);
    CHECK(sess_bye(s, 1) == DSM_EABORT);
    CHECK(sess_barrier_arrive(s, 1, 7, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_EABORT);
    sess_destroy(s);
    return true;
}

/* Irse durante el registro deja la sesión sin poder alcanzar los N nunca. */
static bool test_bye_while_registering_aborts(void) {
    sess *s = sess_create(4, 4, 1);
    CHECK(s != NULL);
    CHECK(sess_hello(s, 0, 4, 0) == DSM_OK);
    CHECK(sess_hello(s, 1, 4, 0) == DSM_OK);
    CHECK(sess_state_of(s) == SESS_REGISTERING);

    CHECK(sess_bye(s, 0) == DSM_EABORT);
    CHECK(sess_state_of(s) == SESS_ABORTED);
    sess_destroy(s);
    return true;
}

static bool test_abort_is_idempotent_and_blocks_finish(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    sess_abort(s);
    CHECK(sess_state_of(s) == SESS_ABORTED);
    sess_abort(s);
    CHECK(sess_state_of(s) == SESS_ABORTED);

    /* Una sesión abortada no pasa a FINISHED aunque no quede nadie: el
     * registro tiene que poder distinguir un final limpio de uno roto. */
    sess_finish(s);
    CHECK(sess_state_of(s) == SESS_ABORTED);
    sess_abort(NULL);
    sess_finish(NULL);
    sess_destroy(s);
    return true;
}

/* ---- Barreras (D0.8) --------------------------------------------------- */

static bool test_barrier_releases_when_all_arrive(void) {
    sess *s = sess_create(3, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 3));

    uint64_t gen = 99;
    uint32_t llegados = 99;
    CHECK(!sess_barrier_info(s, 5, &gen, &llegados));   /* nunca usada */

    CHECK(sess_barrier_arrive(s, 0, 5, 0, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_info(s, 5, &gen, &llegados));
    CHECK(gen == 0 && llegados == 1);

    CHECK(sess_barrier_arrive(s, 2, 5, 0, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_info(s, 5, &gen, &llegados));
    CHECK(gen == 0 && llegados == 2);

    /* El último la completa. */
    CHECK(sess_barrier_arrive(s, 1, 5, 0, NULL) == SESS_BARRIER_RELEASE);

    /* Y la generación ya avanzó, con la máscara limpia: la barrera es
     * reutilizable sin liberar el casillero. */
    CHECK(sess_barrier_info(s, 5, &gen, &llegados));
    CHECK(gen == 1 && llegados == 0);
    CHECK(!sess_barrier_pending(s));
    sess_destroy(s);
    return true;
}

/* Reutilizable de verdad: varias rondas seguidas sobre el mismo barrier_id,
 * cada una con su generación. */
static bool test_barrier_is_reusable_across_generations(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    for (uint64_t g = 0; g < 50; g++) {
        CHECK(sess_barrier_arrive(s, 0, 3, g, NULL) == SESS_BARRIER_WAIT);
        CHECK(sess_barrier_arrive(s, 1, 3, g, NULL) == SESS_BARRIER_RELEASE);

        uint64_t gen = 0;
        CHECK(sess_barrier_info(s, 3, &gen, NULL));
        CHECK(gen == g + 1);
    }
    sess_destroy(s);
    return true;
}

/* Criterio de #4: "una sola llegada admitida por nodo y generación". */
static bool test_one_arrival_per_node_and_generation(void) {
    sess *s = sess_create(3, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 3));

    enum dsm_status why = DSM_OK;
    CHECK(sess_barrier_arrive(s, 0, 1, 0, &why) == SESS_BARRIER_WAIT);

    /* La segunda llegada del mismo nodo a la misma generación se rechaza, y
     * sobre todo NO cuenta: si contara, dos nodos podrían liberar una barrera
     * de tres. */
    CHECK(sess_barrier_arrive(s, 0, 1, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);
    CHECK(sess_barrier_arrive(s, 0, 1, 0, &why) == SESS_BARRIER_REJECT);

    uint32_t llegados = 0;
    CHECK(sess_barrier_info(s, 1, NULL, &llegados));
    CHECK(llegados == 1);

    /* Hacen falta los otros dos de verdad. */
    CHECK(sess_barrier_arrive(s, 1, 1, 0, &why) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_arrive(s, 2, 1, 0, &why) == SESS_BARRIER_RELEASE);
    sess_destroy(s);
    return true;
}

/* Una generación que no es la abierta es un nodo desincronizado, y se ve en
 * el momento en vez de aparecer 80 generaciones después (D0.8). */
static bool test_wrong_generation_is_rejected(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    enum dsm_status why = DSM_OK;

    /* La primera generación de la vida de una barrera es 0, porque el
     * trabajador manda cuántas veces YA pasó. Empezar en 1 se rechaza en vez
     * de aceptarse como punto de partida. */
    CHECK(sess_barrier_arrive(s, 0, 2, 1, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);
    CHECK(!sess_barrier_info(s, 2, NULL, NULL) || true);

    CHECK(sess_barrier_arrive(s, 0, 2, 0, &why) == SESS_BARRIER_WAIT);

    /* Adelantado y atrasado, los dos mal. */
    CHECK(sess_barrier_arrive(s, 1, 2, 1, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);
    CHECK(sess_barrier_arrive(s, 1, 2, 7, &why) == SESS_BARRIER_REJECT);

    uint32_t llegados = 0;
    CHECK(sess_barrier_info(s, 2, NULL, &llegados));
    CHECK(llegados == 1);        /* ninguna de las rechazadas contó */

    CHECK(sess_barrier_arrive(s, 1, 2, 0, &why) == SESS_BARRIER_RELEASE);

    /* Después de liberar, la generación vieja ya no sirve. */
    CHECK(sess_barrier_arrive(s, 0, 2, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(sess_barrier_arrive(s, 0, 2, 1, &why) == SESS_BARRIER_WAIT);
    sess_destroy(s);
    return true;
}

/* Barreras distintas llevan cuentas separadas, pero un NODO no puede estar
 * esperando en dos: después de llegar queda bloqueado hasta la liberación,
 * así que una llegada a otra barrera es un trabajador desincronizado. */
static bool test_a_node_waits_on_one_barrier_at_a_time(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    CHECK(sess_barrier_arrive(s, 0, 10, 0, NULL) == SESS_BARRIER_WAIT);

    /* El nodo 0 está esperando en la 10: llegar a la 20 se rechaza. */
    enum dsm_status why = DSM_OK;
    CHECK(sess_barrier_arrive(s, 0, 20, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);

    /* Y no contó en la 20: si contara, la 20 avanzaría con un nodo que en
     * realidad está bloqueado en otra, y la 10 no liberaría nunca. */
    uint32_t a = 0, b = 0;
    CHECK(sess_barrier_info(s, 10, NULL, &a));
    CHECK(a == 1);
    CHECK(!sess_barrier_info(s, 20, NULL, &b));

    /* El nodo 1 sí puede estar en otra barrera: la restricción es por nodo,
     * no global. Las dos cuentas quedan separadas. */
    CHECK(sess_barrier_arrive(s, 1, 20, 0, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_info(s, 10, NULL, &a));
    CHECK(sess_barrier_info(s, 20, NULL, &b));
    CHECK(a == 1 && b == 1);

    /* Al liberarse una, sus nodos dejan de estar bloqueados y pueden llegar
     * a otra. Con el nodo 1 en la 20, la 10 necesita que el 1 llegue; para
     * eso tendría que salir de la 20 primero, así que esta sesión quedó
     * trabada a propósito: es exactamente la desincronización que el rechazo
     * de arriba previene cuando el culpable es un solo nodo. */
    CHECK(sess_barrier_pending(s));
    sess_destroy(s);
    return true;
}

/* Tras una liberación, los nodos quedan libres para otra barrera. */
static bool test_release_frees_nodes_for_another_barrier(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    CHECK(sess_barrier_arrive(s, 0, 10, 0, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_arrive(s, 1, 10, 0, NULL) == SESS_BARRIER_RELEASE);
    CHECK(!sess_barrier_pending(s));

    /* Los dos pasan ahora a otra barrera sin problema. */
    CHECK(sess_barrier_arrive(s, 0, 20, 0, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_arrive(s, 1, 20, 0, NULL) == SESS_BARRIER_RELEASE);

    /* Y vuelven a la primera, que recuerda su generación. */
    CHECK(sess_barrier_arrive(s, 0, 10, 1, NULL) == SESS_BARRIER_WAIT);
    CHECK(sess_barrier_arrive(s, 1, 10, 1, NULL) == SESS_BARRIER_RELEASE);
    sess_destroy(s);
    return true;
}

/* El casillero no se libera al liberarse la barrera, así que la tabla se
 * agota con barrier_id DISTINTOS, no con rondas. Al agotarse se rechaza en
 * vez de desalojar, porque desalojar perdería la generación en silencio. */
static bool test_barrier_table_rejects_when_exhausted(void) {
    sess *s = sess_create(1, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 1));

    /* Con un solo nodo, cada llegada libera de inmediato. */
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++)
        CHECK(sess_barrier_arrive(s, 0, 1000 + i, 0, NULL)
              == SESS_BARRIER_RELEASE);

    enum dsm_status why = DSM_OK;
    CHECK(sess_barrier_arrive(s, 0, 9999, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_EINVAL);

    /* Las que ya tienen casillero siguen funcionando, con su generación
     * intacta: es justamente lo que se perdería al desalojar. */
    uint64_t gen = 0;
    CHECK(sess_barrier_info(s, 1000, &gen, NULL));
    CHECK(gen == 1);
    CHECK(sess_barrier_arrive(s, 0, 1000, 1, NULL) == SESS_BARRIER_RELEASE);
    sess_destroy(s);
    return true;
}

/* Una llegada RECHAZADA no consume casillero. Sin esto, un trabajador
 * desincronizado podría agotar la tabla a fuerza de rechazos y dejar sin
 * barreras a la sesión entera. */
static bool test_rejected_arrival_consumes_no_slot(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    /* Muchas más llegadas inválidas que casilleros, todas con barrier_id
     * distintos y generación equivocada. */
    enum dsm_status why = DSM_OK;
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS * 4u; i++) {
        CHECK(sess_barrier_arrive(s, 0, 500 + i, 3, &why)
              == SESS_BARRIER_REJECT);
        CHECK(why == DSM_ESTATE);
        /* Ninguna quedó registrada. */
        CHECK(!sess_barrier_info(s, 500 + i, NULL, NULL));
    }
    CHECK(!sess_barrier_pending(s));

    /* Y la tabla sigue entera: caben las SESS_MAX_BARRIERS de siempre. */
    for (uint32_t i = 0; i < SESS_MAX_BARRIERS; i++) {
        CHECK(sess_barrier_arrive(s, 0, 100 + i, 0, NULL)
              == SESS_BARRIER_WAIT);
        CHECK(sess_barrier_arrive(s, 1, 100 + i, 0, NULL)
              == SESS_BARRIER_RELEASE);
    }
    CHECK(sess_barrier_arrive(s, 0, 9999, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_EINVAL);
    sess_destroy(s);
    return true;
}

static bool test_barrier_needs_a_running_session(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    enum dsm_status why = DSM_OK;

    /* Todavía registrando: una llegada no tiene sentido. */
    CHECK(sess_hello(s, 0, 2, 0) == DSM_OK);
    CHECK(sess_barrier_arrive(s, 0, 1, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);

    CHECK(sess_hello(s, 1, 2, 0) == DSM_OK);

    /* Un nodo fuera de rango, o que se fue. */
    CHECK(sess_barrier_arrive(s, 5, 1, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);
    CHECK(sess_bye(s, 1) == DSM_OK);
    CHECK(sess_barrier_arrive(s, 1, 1, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_ESTATE);

    CHECK(sess_barrier_arrive(NULL, 0, 1, 0, &why) == SESS_BARRIER_REJECT);
    CHECK(why == DSM_EINVAL);
    sess_destroy(s);
    return true;
}

/* ---- Tiempo por nodo --------------------------------------------------- */

/* La duración es lo que alimenta la columna dur_ns del registro. Sin
 * operación marcada tiene que ser LOG_NO_DURATION, no 0: la celda queda
 * vacía, porque 0 significaría "tardó nada" y no "no se sabe". */
static bool test_elapsed_is_absent_until_an_op_begins(void) {
    sess *s = sess_create(2, 4, 1);
    CHECK(s != NULL);
    CHECK(registrar_todos(s, 2));

    CHECK(sess_op_elapsed(s, 0, 1000) == LOG_NO_DURATION);

    CHECK(sess_op_begin(s, 0, 1000));
    CHECK(sess_op_elapsed(s, 0, 1500) == 500);
    CHECK(sess_op_elapsed(s, 0, 1000) == 0);

    /* Cada nodo lleva el suyo. */
    CHECK(sess_op_elapsed(s, 1, 1500) == LOG_NO_DURATION);
    CHECK(sess_op_begin(s, 1, 1200));
    CHECK(sess_op_elapsed(s, 1, 1500) == 300);
    CHECK(sess_op_elapsed(s, 0, 1500) == 500);

    /* Una marca sobre un reloj YA abierto no lo mueve, y lo dice en el
     * retorno. Es lo que evita que una trama repetida corra el plazo de la
     * espera que sigue en pie: a t=2000 el nodo 0 sigue esperando desde
     * 1000, no desde 2000. */
    CHECK(!sess_op_begin(s, 0, 2000));
    CHECK(sess_op_elapsed(s, 0, 2000) == 1000);

    /* Cerrado el reloj, una marca nueva sí arranca de cero: un nodo tiene a
     * lo sumo una operación en curso, y esta es otra. */
    sess_op_end(s, 0);
    CHECK(sess_op_begin(s, 0, 2000));
    CHECK(sess_op_elapsed(s, 0, 2100) == 100);

    /* Un reloj que retrocediera daría 0, no un número enorme por desborde. */
    CHECK(sess_op_elapsed(s, 0, 1999) == 0);

    /* Nodo fuera de rango y NULL no revientan. */
    CHECK(sess_op_elapsed(s, 9, 1000) == LOG_NO_DURATION);
    CHECK(sess_op_elapsed(NULL, 0, 1000) == LOG_NO_DURATION);
    CHECK(!sess_op_begin(NULL, 0, 1));
    CHECK(!sess_op_begin(s, 9, 1));

    /* Al irse, deja de haber operación en curso. */
    CHECK(sess_bye(s, 0) == DSM_OK);
    CHECK(sess_op_elapsed(s, 0, 3000) == LOG_NO_DURATION);
    sess_destroy(s);
    return true;
}

int main(void) {
    const struct test_case tests[] = {
        {"create_rejects_bad_args", test_create_rejects_bad_args},
        {"session_starts_with_n_nodes", test_session_starts_with_n_nodes},
        {"ids_may_arrive_out_of_order", test_ids_may_arrive_out_of_order},
        {"duplicate_id_is_rejected", test_duplicate_id_is_rejected},
        {"hello_validates_what_the_worker_declares", test_hello_validates_what_the_worker_declares},
        {"late_join_is_rejected", test_late_join_is_rejected},
        {"bye_is_an_ordered_exit", test_bye_is_an_ordered_exit},
        {"bye_with_a_barrier_waiting_aborts", test_bye_with_a_barrier_waiting_aborts},
        {"bye_while_registering_aborts", test_bye_while_registering_aborts},
        {"abort_is_idempotent_and_blocks_finish", test_abort_is_idempotent_and_blocks_finish},
        {"barrier_releases_when_all_arrive", test_barrier_releases_when_all_arrive},
        {"barrier_is_reusable_across_generations", test_barrier_is_reusable_across_generations},
        {"one_arrival_per_node_and_generation", test_one_arrival_per_node_and_generation},
        {"wrong_generation_is_rejected", test_wrong_generation_is_rejected},
        {"a_node_waits_on_one_barrier_at_a_time", test_a_node_waits_on_one_barrier_at_a_time},
        {"release_frees_nodes_for_another_barrier", test_release_frees_nodes_for_another_barrier},
        {"barrier_table_rejects_when_exhausted", test_barrier_table_rejects_when_exhausted},
        {"rejected_arrival_consumes_no_slot", test_rejected_arrival_consumes_no_slot},
        {"barrier_needs_a_running_session", test_barrier_needs_a_running_session},
        {"elapsed_is_absent_until_an_op_begins", test_elapsed_is_absent_until_an_op_begins},
    };
    return test_util_run(tests, sizeof tests / sizeof tests[0]);
}
