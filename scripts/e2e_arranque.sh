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
PI="$ROOT/app_pi"
RW="$ROOT/app_rw"
GRAN="$ROOT/app_granularity"
LIMITE_S=${E2E_LIMIT_S:-120}   # ver el comentario de e2e_counter.sh
# Prefijo propio: asi run_tests.sh puede comprobar que no quedo
# ninguno sin confundirlo con el temporal de otra herramienta.
TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1

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
    # Bajo `timeout`, que es la red de seguridad: hace falta porque el reloj de
    # registro del coordinador arranca con la primera conexión y no al arrancar
    # el proceso (D3.5), así que sin ningún nodo espera indefinidamente. Un
    # subproceso guardián con `sleep` haría lo mismo pero dejaría el sleep
    # huérfano con el stdout del script tomado (ver e2e_counter.sh).
    timeout "$LIMITE_S" \
        "$COORD" --nodes "$n" --pages "$pages" --port 0 --timeout-ms 5000 \
                 > "$TMP/puerto" 2> "$TMP/coord.err" &
    coord_pid=$!
    pids="$pids $coord_pid"
    port=''
    if ! { exec 3< "$TMP/puerto"; read -r port <&3; } then
        return 1
    fi
    exec 3<&-
    return 0
}

# Más estricto que `rechaza`: exige el código 1 de las aplicaciones
# (argumentos inválidos, D5.1). Con `rechaza` alcanzaría cualquier código
# distinto de cero, y un analizador roto que dejara pasar la opción y se
# pusiera a conectar al puerto 9 saldría con 2 (falló el DSM) y pasaría igual.
# El 1 es lo que prueba que se rechazó ANTES de abrir un socket.
rechaza_args() {
    desc=$1
    shift
    rc=0
    timeout 10 "$@" > "$TMP/out" 2>&1 || rc=$?
    if [ "$rc" -eq 1 ]; then
        echo "   OK (1): $desc"
    else
        echo "   FALLO: $desc: salió con $rc y debía salir con 1"
        sed 's/^/     /' "$TMP/out"
        fallas=$((fallas + 1))
    fi
}

# --help no es un error: imprime el uso y sale con 0.
ayuda() {
    rc=0
    timeout 10 "$1" --help > "$TMP/out" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ] && grep -q "uso:" "$TMP/out"; then
        echo "   OK (0): $(basename "$1") --help"
    else
        echo "   FALLO: $(basename "$1") --help salió con $rc"
        fallas=$((fallas + 1))
    fi
}

coord_termina() {
    wait "$coord_pid" 2>/dev/null
    rc_coord=$?
    pids=''
}

for b in "$COORD" "$APP" "$PI" "$RW" "$GRAN"; do
    if [ ! -x "$b" ]; then
        echo "e2e_arranque.sh: falta $b; corré make primero." >&2
        exit 1
    fi
done

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

echo "-- argumentos inválidos de las aplicaciones de #7, con código 1"
# Las opciones de conexión las lee app_common.c para las tres, así que esos
# casos van una vez con cada aplicación; las propias, con la suya.
for b in "$PI" "$RW" "$GRAN"; do
    nombre=$(basename "$b")
    rechaza_args "$nombre sin --port"           "$b" --nodes 2 --id 0
    rechaza_args "$nombre con --id 2 de 2"      "$b" --port 9 --nodes 2 --id 2
    rechaza_args "$nombre con opción inventada" "$b" --port 9 --nodes 2 --id 0 --inventada x
    rechaza_args "$nombre con opción sin valor" "$b" --port 9 --nodes 2 --id
    ayuda "$b"
done
rechaza_args "app_pi con --terms 0"           "$PI" --port 9 --nodes 2 --id 0 --terms 0
rechaza_args "app_pi con --terms 2^52+1" \
             "$PI" --port 9 --nodes 2 --id 0 --terms 4503599627370497
rechaza_args "app_rw con --versions 0"        "$RW" --port 9 --nodes 2 --id 0 --versions 0
rechaza_args "app_rw con --page 256"          "$RW" --port 9 --nodes 2 --id 0 --page 256
rechaza_args "app_granularity con --layout zzz" \
             "$GRAN" --port 9 --nodes 2 --id 0 --layout zzz
rechaza_args "app_granularity con --rounds 0" \
             "$GRAN" --port 9 --nodes 2 --id 0 --rounds 0

echo "-- una página que no existe en esta sesión"
# Distinto de --page 256: la 99 es válida para el PROTOCOLO y no para una
# región de 4 páginas, así que solo se puede rechazar después del arranque.
# Con un solo nodo no hay nadie más a quien arruinarle la corrida.
if coord_arranca 1 4; then
    if timeout "$LIMITE_S" \
           "$APP" --port "$port" --nodes 1 --id 0 --iters 1 --page 99 \
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
    timeout "$LIMITE_S" "$APP" --port "$port" --nodes 2 --id 0 --iters 1 \
            > "$TMP/a" 2>&1 &
    a=$!
    timeout "$LIMITE_S" "$APP" --port "$port" --nodes 2 --id 0 --iters 1 \
            > "$TMP/b" 2>&1 &
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
