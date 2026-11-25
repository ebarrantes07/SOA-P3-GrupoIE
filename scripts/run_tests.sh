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

# ---- Lo que no debe sobrevivir a la suite -------------------------------
#
# El enunciado pide que no quede ningun proceso del programa ni recurso tomado
# despues de las pruebas, y hasta ahora eso se comprobaba a mano cada vez. Dos
# defectos de esa familia los encontro una persona leyendo, no la suite: un
# `sleep` huerfano del guardian del punta a punta, y una prueba que al fallar
# dejaba vivos a los procesos que no habia esperado.
#
# Se toma una FOTO antes y se compara despues: asi un dsm_manager que el
# usuario tenga corriendo a mano, o un temporal ajeno, no cuentan como fuga.
# Solo lo que aparecio durante la suite.
BINARIOS='dsm_manager app_counter app_pi app_rw app_granularity'
PATRONES='/tmp/e2e_* /tmp/test_dsm_* /tmp/soa-p3.*'

foto_procesos() {
    for b in $BINARIOS; do
        pgrep -x "$b" 2>/dev/null
    done | sort -n
}

foto_temporales() {
    # shellcheck disable=SC2086
    ls -d $PATRONES 2>/dev/null | sort
}

if command -v pgrep > /dev/null 2>&1; then
    antes_proc=$(foto_procesos)
    hay_pgrep=1
else
    antes_proc=''
    hay_pgrep=0
fi
antes_tmp=$(foto_temporales)

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

# ---- El chequeo de fugas, despues de todas las pruebas ------------------

fugas=0

if [ "$hay_pgrep" -eq 1 ]; then
    nuevos=$(foto_procesos | grep -vxF "${antes_proc:-__nada__}" || true)
    if [ -n "$nuevos" ]; then
        echo "-- FUGA: procesos del programa que sobrevivieron a la suite:"
        for p in $nuevos; do
            echo "     pid $p: $(ps -o comm= -p "$p" 2>/dev/null)"
        done
        fugas=1
    fi
else
    # Criterio de #8: si una herramienta no esta, se dice, y no se afirma que
    # el chequeo paso.
    echo "-- AVISO: no hay pgrep; NO se comprobo si quedaron procesos vivos."
fi

nuevos_tmp=$(foto_temporales | grep -vxF "${antes_tmp:-__nada__}" || true)
if [ -n "$nuevos_tmp" ]; then
    echo "-- FUGA: temporales que la suite no borro:"
    for f in $nuevos_tmp; do
        echo "     $f"
    done
    fugas=1
fi

if [ "$fugas" -ne 0 ]; then
    echo "run_tests.sh: la suite dejo recursos tomados."
    exit 1
fi

exit $fail
