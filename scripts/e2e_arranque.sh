#!/usr/bin/env sh
# e2e_arranque.sh — los casos de inicio y validación del enunciado (#6).
#
# Todos estos casos DEBEN FALLAR, y por eso están en su propio script: mezclar
# los que deben pasar con los que deben fallar hace que un archivo se lea al
# revés en la mitad de sus líneas. Acá "pasar la prueba" significa que el
# programa rechazó lo que tenía que rechazar, con un código distinto de cero y
# sin quedarse colgado ni dejar un socket escuchando.
#
# Tres grupos, de menos a más involucrados:
#
#   1. Argumentos inválidos, que se rechazan sin abrir un socket.
#   2. Una página que no existe en la sesión, que solo se sabe después de que
#      el coordinador diga cuántas hay (D0.7).
#   3. Identificadores repetidos, que requiere dos nodos y un coordinador.
#
# Código de salida: 0 si todos los casos se comportaron como se espera.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
APP="$ROOT/app_counter"
LIMITE_S=${E2E_LIMIT_S:-120}   # ver el comentario de e2e_counter.sh
TMP=$(mktemp -d) || exit 1

fallas=0
pids=''

limpiar() {
    for p in $pids; do
        kill -KILL "$p" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap 'limpiar; exit 1' INT TERM HUP
trap 'limpiar' EXIT

# Un caso simple: el comando tiene que terminar con código distinto de cero.
# El tope lo pone `timeout`, porque un rechazo que se cuelga no es un rechazo:
# el enunciado pide que los argumentos inválidos se rechacen, y quedarse
# esperando sería lo contrario.
rechaza() {
    desc=$1
    shift
    if timeout 10 "$@" > "$TMP/out" 2>&1; then
        echo "   FALLO: $desc: se aceptó (código 0)"
        sed 's/^/     /' "$TMP/out"
        fallas=$((fallas + 1))
    else
        rc=$?
        if [ "$rc" -eq 124 ]; then
            echo "   FALLO: $desc: no respondió, se colgó"
            fallas=$((fallas + 1))
        else
            echo "   OK ($rc): $desc"
        fi
    fi
}

# Arranca un coordinador con --port 0 y deja su puerto en $port. La lectura del
# FIFO es la que sincroniza: se desbloquea cuando el socket ya escucha.
coord_arranca() {
    n=$1
    pages=$2
    rm -f "$TMP/puerto"
    mkfifo "$TMP/puerto" || return 1
    "$COORD" --nodes "$n" --pages "$pages" --port 0 --timeout-ms 5000 \
             > "$TMP/puerto" 2> "$TMP/coord.err" &
    coord_pid=$!
    pids="$pids $coord_pid"
    port=''
    if ! { exec 3< "$TMP/puerto"; read -r port <&3; } then
        return 1
    fi
    exec 3<&-
    # Red de seguridad: si el caso no avanza, esto lo desbloquea y el `wait`
    # recoge un 137 en vez de esperar para siempre. Hace falta porque el reloj
    # de registro del coordinador arranca con la primera conexión y no al
    # arrancar el proceso (D3.5): sin ningún nodo, espera indefinidamente.
    ( sleep "$LIMITE_S"; kill -KILL "$coord_pid" 2>/dev/null ) &
    guard_pid=$!
    pids="$pids $guard_pid"
    return 0
}

coord_termina() {
    wait "$coord_pid" 2>/dev/null
    rc_coord=$?
    kill -KILL "$guard_pid" 2>/dev/null
    wait "$guard_pid" 2>/dev/null
    pids=''
}

if [ ! -x "$COORD" ] || [ ! -x "$APP" ]; then
    echo "e2e_arranque.sh: faltan ejecutables; corré make primero." >&2
    exit 1
fi

echo "e2e_arranque.sh: casos de inicio y validación"

echo "-- argumentos inválidos, sin abrir un socket"
rechaza "coordinador sin --nodes"            "$COORD"
rechaza "coordinador con --nodes 9"          "$COORD" --nodes 9
rechaza "coordinador con --pages 0"          "$COORD" --nodes 2 --pages 0
rechaza "coordinador con opción inventada"   "$COORD" --nodes 2 --inventada x
rechaza "trabajador sin --port"              "$APP" --nodes 2 --id 0
rechaza "trabajador sin --id"                "$APP" --port 9 --nodes 2
rechaza "trabajador con --id 3 de 2 nodos"   "$APP" --port 9 --nodes 2 --id 3
rechaza "trabajador con --iters 0"           "$APP" --port 9 --nodes 2 --id 0 --iters 0
rechaza "trabajador con --page 256"          "$APP" --port 9 --nodes 2 --id 0 --page 256
rechaza "trabajador con desborde de N*K" \
        "$APP" --port 9 --nodes 4 --id 0 --iters 18446744073709551615

echo "-- una página que no existe en esta sesión"
# Distinto de --page 256: la 99 es válida para el PROTOCOLO y no para una
# región de 4 páginas, así que solo se puede rechazar después del arranque.
# Con un solo nodo no hay nadie más a quien arruinarle la corrida.
if coord_arranca 1 4; then
    if "$APP" --port "$port" --nodes 1 --id 0 --iters 1 --page 99 \
              > "$TMP/out" 2>&1; then
        echo "   FALLO: se aceptó la página 99 en una región de 4"
        fallas=$((fallas + 1))
    else
        echo "   OK ($?): la página 99 se rechazó en una región de 4"
        grep -q "la región tiene 4" "$TMP/out" || {
            echo "   FALLO: el mensaje no dice cuántas páginas hay"
            sed 's/^/     /' "$TMP/out"
            fallas=$((fallas + 1))
        }
    fi
    coord_termina
    # El coordinador sale en 0: el nodo se despidió en orden pese al rechazo,
    # así que la sesión no se abortó. Un rechazo local no es una caída.
    if [ "$rc_coord" -ne 0 ]; then
        echo "   FALLO: el coordinador terminó con $rc_coord y debía salir en 0"
        fallas=$((fallas + 1))
    else
        echo "   OK: el coordinador terminó en orden tras el rechazo"
    fi
else
    echo "   FALLO: el coordinador no arrancó"
    fallas=$((fallas + 1))
fi

echo "-- identificadores repetidos"
# Dos nodos que se presentan con el mismo id. El coordinador acepta al primero
# y rechaza al segundo sin tocar la sesión del primero (D0.13), así que nunca
# junta los 2 nodos que espera: vence su plazo de registro y la sesión muere.
# Lo que se verifica es que NINGUNO termine en 0 y que nada se cuelgue; que la
# sesión no siga adelante con menos nodos de los pedidos es el punto.
if coord_arranca 2 4; then
    "$APP" --port "$port" --nodes 2 --id 0 --iters 1 > "$TMP/a" 2>&1 &
    a=$!
    "$APP" --port "$port" --nodes 2 --id 0 --iters 1 > "$TMP/b" 2>&1 &
    b=$!
    pids="$pids $a $b"
    wait "$a"; ra=$?
    wait "$b"; rb=$?
    if [ "$ra" -eq 0 ] || [ "$rb" -eq 0 ]; then
        echo "   FALLO: un nodo terminó en 0 ($ra y $rb) con el id repetido"
        sed 's/^/     /' "$TMP/a" "$TMP/b"
        fallas=$((fallas + 1))
    else
        echo "   OK ($ra y $rb): ninguno de los dos completó la sesión"
    fi
    coord_termina
else
    echo "   FALLO: el coordinador no arrancó"
    fallas=$((fallas + 1))
fi

if [ "$fallas" -ne 0 ]; then
    echo "e2e_arranque.sh: $fallas casos no se comportaron como se espera"
    exit 1
fi
echo "e2e_arranque.sh: todos los casos rechazaron lo que debían"
exit 0
