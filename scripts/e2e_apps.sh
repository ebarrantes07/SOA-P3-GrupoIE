#!/usr/bin/env sh
# e2e_apps.sh — las tres aplicaciones de #7, de punta a punta.
#
# Cada aplicación se verifica a sí misma y sale con 3 si el resultado está
# mal (D5.1), así que la mitad del trabajo es exigir código 0 en todos los
# procesos. La otra mitad está en el registro del coordinador, que es lo que
# ningún nodo ve desde adentro:
#
#   app_pi           N=1, 2 y 4 con T=100003 y 100 generaciones de barrera.
#                    El nodo 0 compara con la serie secuencial (≤ 1e-9) y
#                    cada nodo comprueba en cada generación que nadie salió
#                    antes de tiempo.
#   app_rw           N=3 y 4 con varias versiones. Cada lector verifica la
#                    página byte a byte; acá se exige además que el
#                    coordinador haya invalidado las copias de los lectores
#                    antes de cada versión nueva: (N-1)·(V-1) INVALIDATE.
#   app_granularity  N=2 y 4, los dos diseños. Se exige el MISMO número de
#                    adquisiciones WRITE en los dos, invalidaciones con la
#                    página compartida y ninguna con una página por nodo.
#
# Uso: e2e_apps.sh
# Código de salida: 0 si todos los casos pasan, 1 si alguno falla.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
PI="$ROOT/app_pi"
RW="$ROOT/app_rw"
GRAN="$ROOT/app_granularity"

# Con la fragmentación forzada cada página cruza en miles de llamadas al
# sistema; se baja el VOLUMEN y se conserva el camino (mismo criterio que
# e2e_counter.sh, D5.2). T no mueve páginas, así que no cambia.
if [ -n "${DSM_IO_CHUNK_MAX:-}" ]; then
    GENS=10; ROUNDS=10
else
    GENS=100; ROUNDS=50
fi
TERMS=100003
VERSIONS=3
TIMEOUT_MS=${E2E_TIMEOUT_MS:-20000}
LIMITE_S=${E2E_LIMIT_S:-120}

TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1
pids=''
fallas=0

# `timeout --foreground` deja cada proceso en el grupo de este script, así el
# matado por grupo de run_tests.sh lo alcanza; y TERM, no KILL, para que
# timeout alcance a reenviarlo a su hijo antes de morir. Con KILL al timeout,
# el programa quedaría huérfano.
limpiar() {
    for p in $pids; do
        kill -TERM "$p" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap 'limpiar; exit 1' INT TERM HUP
trap 'limpiar' EXIT

decir() { echo "   $*"; }

# Corre una sesión: coordinador con N nodos y P páginas, y N procesos del
# programa dado con los argumentos extra. Deja la salida de los nodos en
# $TMP/out, el registro del coordinador en $TMP/coord.csv y devuelve 1 si
# algún proceso no terminó en 0.
sesion() {
    # Todo con prefijo s_: en sh las variables son globales, y n, i, rc o p
    # son los nombres de los bucles que llaman a esta función. Sin el prefijo,
    # una llamada con un N fijo dentro de `for n in ...` le cambiaría el N al
    # bucle sin ningún aviso.
    s_n=$1; s_pages=$2; s_app=$3; shift 3
    s_mal=0
    rm -f "$TMP/puerto" "$TMP/out" "$TMP/err" "$TMP/coord.csv"
    mkfifo "$TMP/puerto" || return 1

    timeout --foreground "$LIMITE_S" \
        "$COORD" --nodes "$s_n" --pages "$s_pages" --port 0 \
                 --timeout-ms "$TIMEOUT_MS" --log "$TMP/coord.csv" \
                 > "$TMP/puerto" 2> "$TMP/coord.err" &
    s_coord=$!
    pids="$s_coord"

    # La lectura del FIFO sincroniza: vuelve cuando el socket ya escucha, o
    # con fin de archivo si el coordinador murió al arrancar.
    s_port=''
    if ! { exec 3< "$TMP/puerto"; read -r s_port <&3; } then
        exec 3<&-
        decir "FALLO: el coordinador no publicó un puerto"
        sed 's/^/     /' "$TMP/coord.err"
        return 1
    fi
    exec 3<&-

    s_workers=''
    s_i=0
    while [ "$s_i" -lt "$s_n" ]; do
        timeout --foreground "$LIMITE_S" \
            "$s_app" --port "$s_port" --nodes "$s_n" --id "$s_i" "$@" \
                   >> "$TMP/out" 2>> "$TMP/err" &
        s_workers="$s_workers $!"
        pids="$pids $!"
        s_i=$((s_i + 1))
    done

    # Se esperan todos antes de juzgar, para no esconder fallos detrás del
    # primero. El código se guarda antes de probarlo.
    for s_p in $s_workers; do
        wait "$s_p"
        s_rc=$?
        if [ "$s_rc" -ne 0 ]; then
            decir "FALLO: un nodo terminó con código $s_rc"
            s_mal=1
        fi
    done
    wait "$s_coord"
    s_rc=$?
    if [ "$s_rc" -ne 0 ]; then
        decir "FALLO: el coordinador terminó con código $s_rc"
        s_mal=1
    fi
    pids=''
    if [ "$s_mal" -ne 0 ]; then
        sed 's/^/     /' "$TMP/err" "$TMP/coord.err" 2>/dev/null
    fi
    return "$s_mal"
}

# Filas del registro del coordinador con ese evento (y ese modo, si se da).
# Columnas de log.h: event es la 7 y mode la 9.
contar() {
    awk -F, -v ev="$1" -v mo="${2:-}" \
        'NR > 1 && $7 == ev && (mo == "" || $9 == mo) { n++ }
         END { print n + 0 }' "$TMP/coord.csv"
}

for b in "$COORD" "$PI" "$RW" "$GRAN"; do
    if [ ! -x "$b" ]; then
        echo "e2e_apps.sh: falta $b; corré make primero." >&2
        exit 1
    fi
done

echo "e2e_apps.sh: π, lectores/escritor y granularidad"

echo "-- app_pi: T=$TERMS, $GENS generaciones de barrera"
for n in 1 2 4; do
    if sesion "$n" 2 "$PI" --terms "$TERMS" --barriers "$GENS"; then
        linea=$(grep '^pi:' "$TMP/out")
        llegadas=$(contar BARRIER_ARRIVE)
        # Arranque y sumas, más las G generaciones: una llegada por nodo cada
        # vez. Menos llegadas querrían decir que alguna barrera no se usó.
        esperadas=$((n * (GENS + 2)))
        if [ -z "$linea" ]; then
            decir "FALLO: N=$n: el nodo 0 no informó el resultado"
            fallas=$((fallas + 1))
        elif [ "$llegadas" -ne "$esperadas" ]; then
            decir "FALLO: N=$n: $llegadas llegadas a barrera, se esperaban $esperadas"
            fallas=$((fallas + 1))
        else
            decir "OK: N=$n $(echo "$linea" | tr ' ' '\n' \
                | grep -E '^(dif_orden|err_truncamiento)=' | tr '\n' ' ')"
        fi
    else
        fallas=$((fallas + 1))
    fi
done

echo "-- app_rw: $VERSIONS versiones"
for n in 3 4; do
    if sesion "$n" 1 "$RW" --versions "$VERSIONS"; then
        inval=$(contar INVALIDATE)
        acks=$(contar INVALIDATE_ACK)
        esperadas=$(((n - 1) * (VERSIONS - 1)))
        if [ "$inval" -ne "$esperadas" ] || [ "$acks" -ne "$esperadas" ]; then
            decir "FALLO: N=$n: $inval INVALIDATE y $acks ACK, se esperaban $esperadas"
            fallas=$((fallas + 1))
        else
            decir "OK: N=$n: $((n - 1)) lectores verificaron $VERSIONS versiones; $inval copias invalidadas y confirmadas"
        fi
    else
        fallas=$((fallas + 1))
    fi
done

echo "-- app_granularity: $ROUNDS rondas por nodo"
for n in 2 4; do
    w_shared=''
    for diseno in shared separate; do
        if ! sesion "$n" "$n" "$GRAN" --layout "$diseno" --rounds "$ROUNDS"; then
            fallas=$((fallas + 1))
            continue
        fi
        w=$(contar ACQ_REQ WRITE)
        inv=$(contar INVALIDATE)
        if [ "$w" -ne $((n * ROUNDS)) ]; then
            decir "FALLO: N=$n $diseno: $w adquisiciones WRITE, se esperaban $((n * ROUNDS))"
            fallas=$((fallas + 1))
        elif [ "$diseno" = shared ] && [ "$inv" -eq 0 ]; then
            decir "FALLO: N=$n shared: ninguna invalidación con la página compartida"
            fallas=$((fallas + 1))
        elif [ "$diseno" = separate ] && [ "$inv" -ne 0 ]; then
            decir "FALLO: N=$n separate: $inv invalidaciones con una página por nodo"
            fallas=$((fallas + 1))
        else
            decir "OK: N=$n $diseno: $w adquisiciones WRITE, $inv invalidaciones, $(contar BARRIER_ARRIVE) llegadas a barrera"
        fi
        if [ "$diseno" = shared ]; then
            w_shared=$w
        elif [ -n "$w_shared" ] && [ "$w" -ne "$w_shared" ]; then
            decir "FALLO: N=$n: $w_shared y $w adquisiciones; el trabajo no es el mismo"
            fallas=$((fallas + 1))
        fi
    done
done

if [ "$fallas" -ne 0 ]; then
    echo "e2e_apps.sh: $fallas casos fallaron"
    exit 1
fi
echo "e2e_apps.sh: todos los casos pasaron"
exit 0
