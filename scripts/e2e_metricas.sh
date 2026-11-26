#!/usr/bin/env sh
# e2e_metricas.sh — metricas.py contra un registro real y contra registros
# alterados (#9).
#
# Mismo criterio que e2e_verificador.sh: un módulo que nunca vio un registro
# malo no está verificado. Acá lo que se pone a prueba no son los invariantes
# del protocolo sino la CONTABILIDAD: que las tres identidades que metricas.py
# comprueba —páginas derivadas contra la columna, invalidaciones derivadas
# contra la columna, y bytes por grupo contra el total— detecten un registro
# cuyo tráfico no cierra.
#
# Importa porque de esas cifras sale la tabla de tráfico del informe: si el
# módulo acepta cualquier cosa, el informe también.
#
# Código de salida: 0 si el registro real pasa y las alteraciones fallan.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
GRAN="$ROOT/app_granularity"
MET="$ROOT/scripts/metricas.py"

NODOS=2
RONDAS=${E2E_RONDAS:-5}
LIMITE_S=${E2E_LIMIT_S:-120}
TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1

pids=''
fallas=0

limpiar() {
    for p in $pids; do
        kill -TERM "$p" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap 'limpiar; exit 1' INT TERM HUP
trap 'limpiar' EXIT

if [ ! -x "$COORD" ] || [ ! -x "$GRAN" ]; then
    echo "e2e_metricas.sh: faltan ejecutables; corré make primero." >&2
    exit 1
fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "e2e_metricas.sh: no hay python3 en el entorno." >&2
    exit 1
fi

echo "e2e_metricas.sh: la contabilidad del tráfico, leída del registro"

# ---- Una corrida real -----------------------------------------------------

real="$TMP/real.csv"
rm -f "$TMP/puerto"
mkfifo "$TMP/puerto" || exit 1
timeout --foreground "$LIMITE_S" \
    "$COORD" --nodes "$NODOS" --pages "$NODOS" --port 0 --timeout-ms 20000 \
             --log "$real" > "$TMP/puerto" 2> "$TMP/coord.err" &
coord=$!
pids="$coord"

port=''
if ! { exec 3< "$TMP/puerto"; read -r port <&3; } then
    echo "   FALLO: el coordinador no publicó un puerto"
    sed 's/^/     /' "$TMP/coord.err"
    exit 1
fi
exec 3<&-

trabajadores=''
i=0
while [ "$i" -lt "$NODOS" ]; do
    timeout --foreground "$LIMITE_S" \
        "$GRAN" --port "$port" --nodes "$NODOS" --id "$i" \
                --layout shared --rounds "$RONDAS" \
                > /dev/null 2>> "$TMP/app.err" &
    trabajadores="$trabajadores $!"
    pids="$pids $!"
    i=$((i + 1))
done

# Cada proceso por su PID y con su código: un registro parcial haría pasar
# todo lo de abajo sin haber medido nada (misma lección que e2e_verificador).
malos=''
i=0
for p in $trabajadores; do
    rc=0
    wait "$p" || rc=$?
    [ "$rc" -eq 0 ] || malos="$malos trabajador-$i($rc)"
    i=$((i + 1))
done
rc=0
wait "$coord" || rc=$?
[ "$rc" -eq 0 ] || malos="$malos coordinador($rc)"
pids=''

if [ -n "$malos" ]; then
    echo "   FALLO: la corrida real no terminó limpia:$malos"
    sed 's/^/     /' "$TMP/coord.err" "$TMP/app.err" 2>/dev/null
    exit 1
fi

echo "-- un registro real tiene que cuadrar"
if python3 "$MET" "$real" > "$TMP/out" 2>&1; then
    filas=$(sed -n 's/.*(N=[0-9]*, \([0-9]*\) filas).*/\1/p' "$TMP/out")
    echo "   OK: la corrida cuadra, $filas filas"
else
    echo "   FALLO: metricas.py no cuadra sobre una corrida real"
    sed 's/^/     /' "$TMP/out"
    fallas=$((fallas + 1))
fi

# ---- Registros alterados --------------------------------------------------
#
# Cada uno rompe UNA de las tres identidades, y se exige que la delate.

echo "-- cada registro alterado tiene que descuadrar"

alterado="$TMP/mal.csv"

# Alteración, programa de awk que la aplica, y el motivo que DEBE informar.
# Exigir el motivo es lo que impide que esto quede verde con un módulo que
# rechace todo: cada una rompe una sola de las tres identidades, y se
# comprobó que no rompe las otras dos.
altera() {
    desc=$1; prog=$2; motivo=$3
    awk -F, -v OFS=, "$prog { print }" "$real" > "$alterado"
    if python3 "$MET" "$alterado" > "$TMP/out" 2>&1; then
        echo "   FALLO: $desc: lo aceptó"
        fallas=$((fallas + 1))
    elif grep -q "$motivo" "$TMP/out"; then
        echo "   OK ($motivo): $desc"
    else
        echo "   FALLO: $desc: descuadró por otro motivo, no por $motivo"
        sed 's/^/     /' "$TMP/out"
        fallas=$((fallas + 1))
    fi
}

# Un byte de más en una fila: su delta deja de ser un tamaño de trama, y
# también el de la fila siguiente, porque el delta es una diferencia.
altera "bytes_in inflado en una fila" 'NR == 4 { $17 = $17 + 1000 }' \
       "deltas_sin_explicar=2"
# El total de páginas ya no coincide con las concesiones con página más los
# releases.
altera "page_xfers alterado" 'NR > 1 { $19 = $19 + 7 }' "paginas=MAL"
# El total de invalidaciones ya no coincide con las filas INVALIDATE.
altera "invals alterado" 'NR > 1 { $20 = $20 + 3 }' "invals=MAL"

# ---- Lo que no es un registro de coordinador ------------------------------

echo "-- lo que no es un registro de coordinador"
: > "$TMP/vacio.csv"
if python3 "$MET" "$TMP/vacio.csv" > /dev/null 2>&1; then
    echo "   FALLO: archivo vacío aceptado"
    fallas=$((fallas + 1))
else
    echo "   OK: archivo vacío"
fi
sed 's/,coordinador,/,trabajador,/' "$real" > "$TMP/trabajador.csv"
if python3 "$MET" "$TMP/trabajador.csv" > /dev/null 2>&1; then
    echo "   FALLO: registro de trabajador aceptado"
    fallas=$((fallas + 1))
else
    echo "   OK: registro de trabajador rechazado"
fi

if [ "$fallas" -ne 0 ]; then
    echo "e2e_metricas.sh: $fallas comprobaciones fallaron"
    exit 1
fi
echo "e2e_metricas.sh: la contabilidad cuadra y los descuadres se detectan"
exit 0
