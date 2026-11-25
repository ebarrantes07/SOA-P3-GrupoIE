#!/usr/bin/env sh
# e2e_analisis.sh — analisis.py sobre resultados de forma conocida (#9).
#
# El lanzador de la matriz es de #9 y todavía no existe, pero el análisis no
# tiene por qué esperarlo: lo que se prueba acá es la ARITMÉTICA y el trato de
# las corridas malas, y para eso alcanzan resultados construidos a propósito,
# con valores cuyo promedio y desviación se conocen de antemano.
#
# Los valores son 10, 12, 14, 16 y 18: media 14 y desviación muestral
# sqrt(40/4) = 3.1623 (no 2.83, que sería la poblacional). Por nodo se
# escriben t y t+1, así el máximo por corrida es t+1 y su media 15: eso
# comprueba que de los valores por nodo se tome el máximo y no el promedio.
#
# Cuatro corridas malas, una por cada forma de serlo, y todas tienen que
# quedar FUERA de las estadísticas y aparecer NOMBRADAS: el criterio de #9 es
# que ninguna ejecución se descarte sin explicación.
#
# Código de salida: 0 si todo lo esperado se cumple.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
GRAN="$ROOT/app_granularity"
ANA="$ROOT/scripts/analisis.py"

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
    echo "e2e_analisis.sh: faltan ejecutables; corré make primero." >&2
    exit 1
fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "e2e_analisis.sh: no hay python3 en el entorno." >&2
    exit 1
fi

echo "e2e_analisis.sh: media, desviación y corridas descartadas"

# ---- Un registro real, que es el que acompaña a cada corrida -------------

real="$TMP/base.csv"
rm -f "$TMP/puerto"
mkfifo "$TMP/puerto" || exit 1
timeout --foreground "$LIMITE_S" \
    "$COORD" --nodes 2 --pages 2 --port 0 --timeout-ms 20000 --log "$real" \
             > "$TMP/puerto" 2> "$TMP/coord.err" &
coord=$!
pids="$coord"
port=''
if ! { exec 3< "$TMP/puerto"; read -r port <&3; } then
    echo "   FALLO: el coordinador no publicó un puerto"
    exit 1
fi
exec 3<&-
i=0
trabajadores=''
while [ "$i" -lt 2 ]; do
    timeout --foreground "$LIMITE_S" \
        "$GRAN" --port "$port" --nodes 2 --id "$i" --layout shared --rounds 3 \
                > /dev/null 2>> "$TMP/app.err" &
    trabajadores="$trabajadores $!"
    pids="$pids $!"
    i=$((i + 1))
done
malos=''
for p in $trabajadores; do
    rc=0; wait "$p" || rc=$?
    [ "$rc" -eq 0 ] || malos="$malos nodo($rc)"
done
rc=0; wait "$coord" || rc=$?
[ "$rc" -eq 0 ] || malos="$malos coordinador($rc)"
pids=''
if [ -n "$malos" ]; then
    echo "   FALLO: la corrida base no terminó limpia:$malos"
    sed 's/^/     /' "$TMP/coord.err" "$TMP/app.err" 2>/dev/null
    exit 1
fi

# ---- Resultados de forma conocida ---------------------------------------

DIR="$TMP/results/prueba"
mkdir -p "$DIR"

# corrida: archivo, repeticion, valor, codigo del nodo 0, csv, uname,
# experimento. El experimento va como parámetro porque el análisis agrupa por
# (experimento, config) y no por carpeta: con el nombre fijo, las tres
# configuraciones de esta prueba caerían en el mismo grupo.
corrida() {
    cp "$real" "$DIR/$5"
    cat > "$DIR/$1" <<JSON
{
  "experimento": "$7",
  "config": {"nodes": 2, "layout": "shared"},
  "repeticion": $2,
  "salida": {"t_trabajo_ms": $3, "diseno": "shared"},
  "salida_por_nodo": [{"t_trabajo_ms": $3}, {"t_trabajo_ms": $(($3 + 1))}],
  "codigos_salida": {"coordinador": 0, "nodos": [$4, 0]},
  "log": "$5",
  "entorno": {"uname": "$6"},
  "duracion_s": 1
}
JSON
}

corrida c_r1.json 1 10 0 c_r1.csv maquina-a prueba
corrida c_r2.json 2 12 0 c_r2.csv maquina-a prueba
corrida c_r3.json 3 14 0 c_r3.csv maquina-a prueba
corrida c_r4.json 4 16 0 c_r4.csv maquina-a prueba
corrida c_r5.json 5 18 0 c_r5.csv maquina-a prueba

# Mala 1: un nodo no terminó en 0. Su valor es enorme a propósito: si entrara
# en las estadísticas, la media saltaría de 14 a 181.
corrida c_r6.json 6 1000 3 c_r6.csv maquina-a prueba

# Mala 2: el registro no cuadra. Se le infla bytes_in en una fila, igual que
# en e2e_metricas.sh.
corrida c_r7.json 7 1000 0 c_r7.csv maquina-a prueba
awk -F, -v OFS=, 'NR == 4 { $17 = $17 + 1000 } { print }' "$real" \
    > "$DIR/c_r7.csv.tmp" && mv "$DIR/c_r7.csv.tmp" "$DIR/c_r7.csv"

# Mala 3: el registro no está.
corrida c_r8.json 8 1000 0 c_r8.csv maquina-a prueba
rm -f "$DIR/c_r8.csv"

# Mala 4: JSON ilegible.
echo '{ esto no es json' > "$DIR/c_r9.json"

# Otra configuración, con una sola repetición: la desviación no existe.
mkdir -p "$TMP/results/sola"
DIR="$TMP/results/sola"
corrida s_r1.json 1 20 0 s_r1.csv maquina-a sola

# Y otra con el entorno mezclado entre repeticiones.
mkdir -p "$TMP/results/mezcla"
DIR="$TMP/results/mezcla"
corrida m_r1.json 1 10 0 m_r1.csv maquina-a mezcla
corrida m_r2.json 2 12 0 m_r2.csv maquina-b mezcla

# ---- Lo que tiene que informar ------------------------------------------

python3 "$ANA" "$TMP/results" > "$TMP/out" 2>&1
rc=$?

exige() {
    if grep -Eq -- "$2" "$TMP/out"; then
        echo "   OK: $1"
    else
        echo "   FALLO: $1"
        fallas=$((fallas + 1))
    fi
}
niega() {
    if grep -Eq -- "$2" "$TMP/out"; then
        echo "   FALLO: $1"
        fallas=$((fallas + 1))
    else
        echo "   OK: $1"
    fi
}

echo "-- la aritmética"
# Las cinco buenas y nada más: n=5 de 9 repeticiones.
# Ocho y no nueve: el JSON ilegible no se puede atribuir a ninguna
# configuracion --no se pudo leer su config-- y se informa aparte.
exige "cinco corridas buenas de ocho leidas"  "8 repeticiones, n=5"
exige "media 14 y desviación muestral 3.162"  "app\.t_trabajo_ms +14 ± 3\.162"
exige "por nodo se toma el máximo: media 15"  "nodo_max\.t_trabajo_ms +15 ± 3\.162"
exige "el rango va de 10 a 18"                "\[10, 18\]"
niega "ninguna mala contaminó la media"       "181"

echo "-- las corridas descartadas, todas nombradas"
exige "el nodo que no terminó en 0" \
      "DESCARTADA r6 \(c_r6\.json\): procesos que no terminaron en 0: nodo0=3"
exige "el registro que no cuadra" \
      "DESCARTADA r7 \(c_r7\.json\): el tráfico del registro no cuadra"
exige "el registro que falta" \
      "DESCARTADA r8 \(c_r8\.json\): no se pudo medir el registro"
exige "el JSON ilegible"                 "ILEGIBLE c_r9.json"

echo "-- los otros dos casos"
exige "con n=1 la desviación no existe"  "app\.t_trabajo_ms +20 ± —"
exige "el entorno mezclado se avisa"     "AVISO: 2 entornos distintos"

echo "-- el código de salida"
if [ "$rc" -eq 1 ]; then
    echo "   OK: devuelve 1 cuando hubo corridas con problemas"
else
    echo "   FALLO: devolvió $rc y debía devolver 1"
    fallas=$((fallas + 1))
fi

# Y sin problemas, 0: las cinco buenas solas.
mkdir -p "$TMP/limpio"
cp "$TMP/results/prueba/c_r1.json" "$TMP/results/prueba/c_r1.csv" \
   "$TMP/results/prueba/c_r2.json" "$TMP/results/prueba/c_r2.csv" "$TMP/limpio/"
if python3 "$ANA" "$TMP/limpio" > /dev/null 2>&1; then
    echo "   OK: devuelve 0 cuando no hay ninguna corrida con problemas"
else
    echo "   FALLO: devolvió distinto de 0 sobre resultados limpios"
    fallas=$((fallas + 1))
fi

if [ "$fallas" -ne 0 ]; then
    echo "e2e_analisis.sh: $fallas comprobaciones fallaron"
    sed 's/^/     /' "$TMP/out"
    exit 1
fi
echo "e2e_analisis.sh: la aritmética cuadra y nada se descarta en silencio"
exit 0
