#!/usr/bin/env sh
# e2e_verificador.sh — el verificador de invariantes, contra registros reales
# y contra registros alterados a mano (#8).
#
# Dos mitades, y las dos hacen falta:
#
#   1. Un registro de una corrida REAL tiene que pasar. Si no, el verificador
#      es inútil: nadie va a creerle un fallo a algo que falla siempre.
#   2. Cada registro ALTERADO tiene que fallar, y con la propiedad correcta.
#      Es el criterio del issue: "falla a propósito contra un log alterado a
#      mano, para demostrar que realmente verifica". Un verificador que nunca
#      vio un registro malo no es un verificador, es un lector de archivos.
#
# Que se exija la propiedad correcta y no solo "falló" es lo que impide que el
# script quede verde con un verificador que rechace todo.
#
# Código de salida: 0 si el registro real pasa y todas las alteraciones fallan
# como se espera.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
APP="$ROOT/app_counter"
VERIF="$ROOT/scripts/verificar_invariantes.py"
MUTAR="$ROOT/scripts/mutar_registro.py"

NODOS=3
PAGINAS=2
ITERS=${E2E_ITERS:-20}
LIMITE_S=${E2E_LIMIT_S:-120}    # ver el comentario de e2e_counter.sh
# Prefijo propio: asi run_tests.sh puede comprobar que no quedo
# ninguno sin confundirlo con el temporal de otra herramienta.
TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1

pids=''
fallas=0

limpiar() {
    for p in $pids; do
        kill -KILL "$p" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap 'limpiar; exit 1' INT TERM HUP
trap 'limpiar' EXIT

# Mutación y propiedad que DEBE delatar. `ack_repetido_cierra` se espera como
# P3 a propósito y no como P5: P5 está implementada estructuralmente —un ACK
# repetido no descuenta de la ronda— así que la ronda queda abierta y lo que
# salta es el grant prematuro. Esa es la demostración de que no se cuenta dos
# veces; si se contara, el registro pasaría.
CASOS="dos_escritores:P1
publica_ajeno:P1
lector_con_escritor:P2
grant_sin_ack:P3
version_retrocede:P4
ack_repetido_cierra:P3"

if [ ! -x "$COORD" ] || [ ! -x "$APP" ]; then
    echo "e2e_verificador.sh: faltan ejecutables; corré make primero." >&2
    exit 1
fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "e2e_verificador.sh: no hay python3 en el entorno." >&2
    exit 1
fi

echo "e2e_verificador.sh: invariantes del protocolo, leídos del registro"

# ---- Una corrida real -----------------------------------------------------

real="$TMP/real.csv"
rm -f "$TMP/puerto"
mkfifo "$TMP/puerto" || exit 1
timeout "$LIMITE_S" \
    "$COORD" --nodes "$NODOS" --pages "$PAGINAS" --port 0 \
             --timeout-ms 10000 --log "$real" --run-id 808 \
             > "$TMP/puerto" 2> "$TMP/coord.err" &
coord=$!
pids="$pids $coord"

port=''
if ! { exec 3< "$TMP/puerto"; read -r port <&3; } then
    echo "   FALLO: el coordinador no publicó un puerto"
    sed 's/^/     /' "$TMP/coord.err"
    exit 1
fi
exec 3<&-

i=0
while [ "$i" -lt "$NODOS" ]; do
    timeout "$LIMITE_S" \
        "$APP" --port "$port" --nodes "$NODOS" --id "$i" --iters "$ITERS" \
               > /dev/null 2>> "$TMP/app.err" &
    pids="$pids $!"
    i=$((i + 1))
done
wait 2>/dev/null
pids=''

if [ ! -s "$real" ]; then
    echo "   FALLO: la corrida no dejó registro"
    sed 's/^/     /' "$TMP/coord.err" "$TMP/app.err" 2>/dev/null
    exit 1
fi

echo "-- un registro de una corrida real tiene que pasar"
if python3 "$VERIF" "$real" > "$TMP/out" 2>&1; then
    echo "   OK: $(grep -c '^   OK' "$TMP/out") propiedades, $(sed -n 's/^   \([0-9]*\) filas.*/\1/p' "$TMP/out") filas"
else
    echo "   FALLO: el verificador rechaza una corrida real"
    sed 's/^/     /' "$TMP/out"
    fallas=$((fallas + 1))
fi

# ---- Registros alterados a mano -------------------------------------------

echo "-- cada registro alterado tiene que fallar, y con su propiedad"
for caso in $CASOS; do
    mut=$(echo "$caso" | cut -d: -f1)
    prop=$(echo "$caso" | cut -d: -f2)
    alterado="$TMP/$mut.csv"

    if ! python3 "$MUTAR" "$mut" "$real" "$alterado" > "$TMP/mut.out" 2>&1; then
        echo "   FALLO: no se pudo aplicar la mutación $mut"
        sed 's/^/     /' "$TMP/mut.out"
        fallas=$((fallas + 1))
        continue
    fi

    if python3 "$VERIF" "$alterado" > "$TMP/out" 2>&1; then
        echo "   FALLO: $mut pasó la verificación y debía violar $prop"
        fallas=$((fallas + 1))
        continue
    fi
    if grep -q "VIOLA $prop" "$TMP/out"; then
        echo "   OK ($prop): $mut"
    else
        echo "   FALLO: $mut falló, pero no por $prop"
        sed 's/^/     /' "$TMP/out"
        fallas=$((fallas + 1))
    fi
done

# ---- Lo que el verificador tiene que RECHAZAR leer ------------------------

echo "-- un archivo que no es un registro de coordinador"
: > "$TMP/vacio.csv"
if python3 "$VERIF" "$TMP/vacio.csv" > "$TMP/out" 2>&1; then
    echo "   FALLO: aceptó un archivo vacío"
    fallas=$((fallas + 1))
else
    echo "   OK ($?): archivo vacío"
fi
if python3 "$VERIF" "$TMP/no_existe.csv" > "$TMP/out" 2>&1; then
    echo "   FALLO: aceptó un archivo que no existe"
    fallas=$((fallas + 1))
else
    echo "   OK ($?): archivo inexistente"
fi
# Un registro de trabajador no sirve: no ve el estado de las páginas, así que
# aceptarlo daría un "todo bien" que no significa nada.
sed 's/,coordinador,/,trabajador,/' "$real" > "$TMP/trabajador.csv"
if python3 "$VERIF" "$TMP/trabajador.csv" > "$TMP/out" 2>&1; then
    echo "   FALLO: aceptó un registro de trabajador como si fuera del coordinador"
    fallas=$((fallas + 1))
else
    echo "   OK ($?): registro de trabajador rechazado"
fi

if [ "$fallas" -ne 0 ]; then
    echo "e2e_verificador.sh: $fallas comprobaciones fallaron"
    exit 1
fi
echo "e2e_verificador.sh: el verificador acepta lo bueno y rechaza lo malo"
exit 0
