/* test_cli.c — pruebas de los conversores estrictos de argumentos. */
#include "cli.h"
#include "test_util.h"

#include <stdint.h>

static bool test_u64_acepta_validos(void) {
    uint64_t v = 0;
    CHECK(cli_parse_u64("0", 0, 10, &v) && v == 0);
    CHECK(cli_parse_u64("10", 0, 10, &v) && v == 10);
    CHECK(cli_parse_u64("7", 7, 7, &v) && v == 7);
    CHECK(cli_parse_u64("18446744073709551615", 0, UINT64_MAX, &v) && v == UINT64_MAX);
    return true;
}

static bool test_u64_rechaza_basura(void) {
    uint64_t v = 123;
    CHECK(!cli_parse_u64(NULL, 0, 10, &v));
    CHECK(!cli_parse_u64("", 0, 10, &v));
    CHECK(!cli_parse_u64(" 5", 0, 10, &v));       /* espacio inicial */
    CHECK(!cli_parse_u64("5 ", 0, 10, &v));       /* espacio final */
    CHECK(!cli_parse_u64("+5", 0, 10, &v));       /* signo explícito */
    CHECK(!cli_parse_u64("-1", 0, 10, &v));       /* strtoull lo haría enorme */
    CHECK(!cli_parse_u64("5x", 0, 10, &v));       /* basura al final */
    CHECK(!cli_parse_u64("0x10", 0, 100, &v));    /* no se acepta hexadecimal */
    CHECK(!cli_parse_u64("5.0", 0, 10, &v));
    CHECK(v == 123);                              /* no escribe en error */
    return true;
}

static bool test_u64_rango_y_overflow(void) {
    uint64_t v = 0;
    CHECK(!cli_parse_u64("11", 0, 10, &v));       /* arriba del máximo */
    CHECK(!cli_parse_u64("2", 3, 10, &v));        /* abajo del mínimo */
    /* Un dígito más que UINT64_MAX: ERANGE, no un valor truncado. */
    CHECK(!cli_parse_u64("18446744073709551616", 0, UINT64_MAX, &v));
    CHECK(!cli_parse_u64("99999999999999999999999", 0, UINT64_MAX, &v));
    return true;
}

static bool test_u32_y_u16_acotan(void) {
    uint32_t a = 0;
    uint16_t b = 0;
    CHECK(cli_parse_u32("4294967295", 0, UINT32_MAX, &a) && a == UINT32_MAX);
    CHECK(!cli_parse_u32("4294967296", 0, UINT32_MAX, &a));
    CHECK(cli_parse_u16("65535", 1, UINT16_MAX, &b) && b == UINT16_MAX);
    CHECK(!cli_parse_u16("65536", 1, UINT16_MAX, &b));
    CHECK(!cli_parse_u16("0", 1, UINT16_MAX, &b)); /* puerto 0 no sirve */
    return true;
}

static bool test_rangos_del_protocolo(void) {
    /* Los rangos que de verdad va a validar el proyecto: 1..4 nodos,
     * 1..256 páginas, identificador de nodo en 0..nodes-1. */
    uint32_t v = 0;
    CHECK(cli_parse_u32("4", 1, 4, &v) && v == 4);
    CHECK(!cli_parse_u32("5", 1, 4, &v));
    CHECK(!cli_parse_u32("0", 1, 4, &v));
    CHECK(cli_parse_u32("256", 1, 256, &v) && v == 256);
    CHECK(!cli_parse_u32("257", 1, 256, &v));
    CHECK(cli_parse_u32("0", 0, 3, &v) && v == 0);  /* nid del nodo 0 */
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"u64_acepta_validos", test_u64_acepta_validos},
        {"u64_rechaza_basura", test_u64_rechaza_basura},
        {"u64_rango_y_overflow", test_u64_rango_y_overflow},
        {"u32_y_u16_acotan", test_u32_y_u16_acotan},
        {"rangos_del_protocolo", test_rangos_del_protocolo},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
