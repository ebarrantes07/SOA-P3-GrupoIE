#!/usr/bin/env sh
# Corre los binarios de prueba que le pasen como argumentos.
#
# Existe para que `make test` pueda aplicar un límite GLOBAL de tiempo, como
# pide el enunciado, y no solo uno por binario: el Makefile invoca este script
# entero bajo un timeout. Adentro cada binario lleva además su propio límite,
# así un cuelgue se atribuye a la prueba que lo causó en vez de matar la suite
# sin decir cuál fue.
#
# Uso: run_tests.sh <segundos por binario> <binario>...
# Código de salida: 0 si todas pasan, 1 si alguna falla, se le agota el tiempo
# o la suite entera es interrumpida.
set -u

if [ "$#" -lt 1 ]; then
    echo "uso: $0 <segundos por binario> <binario>..." >&2
    exit 2
fi

per_test="$1"
shift

if [ "$#" -eq 0 ]; then
    echo "run_tests.sh: no se recibió ningún binario de prueba."
    exit 0
fi

# La prueba en curso corre en segundo plano y se espera con wait, para que
# este script pueda atender una señal mientras ella trabaja. Sin esto, el
# timeout global del Makefile mata al script y la prueba sigue viva como
# huérfana: el enunciado exige que no quede ningún proceso del programa
# después de la suite, y un binario abandonado escribiendo a la terminal es
# justo lo que no debe pasar.
child=''

on_signal() {
    if [ -n "$child" ]; then
        # Al GRUPO, no al PID: el grupo incluye al timeout y al binario que
        # supervisa, así que la señal llega al binario directamente y no
        # depende de que el timeout la reenvíe antes de morir. Esa dependencia
        # era justamente la que dejaba huérfana la prueba en curso.
        kill -TERM "-$child" 2>/dev/null
        kill -KILL "-$child" 2>/dev/null
    fi
    echo "run_tests.sh: interrumpido, se terminó la prueba en curso." >&2
    exit 1
}
trap on_signal TERM INT HUP

fail=0
for t in "$@"; do
    echo "-- $t --"
    # setsid, y sin job control: el trabajo en segundo plano no es líder de
    # grupo, así que setsid hace exec en su lugar y el PID que devuelve $!
    # es además el PGID. Eso vuelve determinista el kill al grupo del trap:
    # no depende de que el timeout alcance a reenviar la señal antes de
    # morir, que es lo que dejaba la prueba huérfana.
    setsid timeout "$per_test" "$t" &
    child=$!
    wait "$child"
    rc=$?
    child=''
    if [ "$rc" -ne 0 ]; then
        fail=1
        if [ "$rc" -eq 124 ]; then
            echo "  TIEMPO AGOTADO tras ${per_test}s"
        fi
    fi
done

exit $fail
