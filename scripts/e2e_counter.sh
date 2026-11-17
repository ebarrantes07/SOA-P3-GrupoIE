#!/usr/bin/env sh
# e2e_counter.sh — el punta a punta del contador compartido (#6).
#
# Lanza el coordinador y N procesos app_counter de verdad, los espera, y
# comprueba las DOS cosas: que todos terminen con código 0 y que el total sea
# exactamente N*K. Las dos hacen falta. Un nodo que muere a mitad podría dejar
# un total correcto por casualidad, y un total equivocado puede venir de
# procesos que terminaron bien.
#
# Vive en scripts/ y no en tests/ porque lanza los ejecutables de BINS, no
# módulos (ver el comentario del Makefile). `make test` se lo pasa a
# run_tests.sh como un ejecutable más, así que hereda de ahí el límite por
# prueba, el límite global de la suite y el matado por grupo ante una señal:
# nada de eso hay que reimplementarlo acá.
#
# Uso: e2e_counter.sh            los casos del criterio: N=1, 2 y 4 con K=1000
#      E2E_ITERS=50 e2e_counter.sh    el mismo recorrido, más rápido
#
# Código de salida: 0 si todos los casos pasan, 1 si alguno falla.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COORD="$ROOT/dsm_manager"
APP="$ROOT/app_counter"

# K del criterio de aceptación, con una excepción medida.
#
# Con la fragmentación forzada de DSM_IO_CHUNK_MAX cada lectura y cada
# escritura mueven ese tope de bytes, así que una página de 4096 cuesta miles
# de llamadas al sistema en vez de una o dos. Con K=1000 y cuatro nodos son
# 4000 transferencias de página en cada sentido: la corrida no termina en
# ningún tiempo razonable, y lo que se ve es el perro guardián matando a todos.
#
# En esa configuración se baja K. Lo que esa variable existe para ejercer es el
# CAMINO de la transferencia fragmentada de punta a punta, y veinte incrementos
# por nodo lo ejercen igual de bien que mil. El valor del criterio es el que
# corre en la configuración normal, que es la que lo tiene que cumplir.
#
# Un E2E_ITERS explícito gana sobre las dos cosas: si alguien pide un K, es ese.
if [ -n "${E2E_ITERS:-}" ]; then
    K=$E2E_ITERS
elif [ -n "${DSM_IO_CHUNK_MAX:-}" ]; then
    K=20
else
    K=1000
fi

# Plazo por operación de los procesos. Generoso a propósito: esta prueba
# verifica el RESULTADO, no la latencia, y una máquina cargada no debería
# hacerla fallar. Los plazos se prueban en test_e2e_manager y test_e2e_dsm.
TIMEOUT_MS=${E2E_TIMEOUT_MS:-20000}

PAGES=4

# Límite PROPIO de cada caso, en segundos. run_tests.sh ya acota este script
# entero, pero hace falta uno acá por dos razones: corrido a mano no hay nadie
# que lo acote, y sobre todo hay un cuelgue que el límite de afuera tardaría
# cinco minutos en delatar. Si ningún trabajador llega a conectarse, el
# coordinador espera para siempre a propósito: su reloj de registro arranca con
# la primera conexión aceptada y no al arrancar el proceso (D3.5), porque se
# lanza a mano y acotar eso sería acotar al operador. Visto desde acá, eso es
# una espera sin fondo, y el perro guardián es lo que la convierte en un fallo
# con nombre.
# Un cuelgue es infinito, así que cualquier tope finito lo caza: el único costo
# de un tope grande es cuánto se tarda en saberlo. Por eso hay margen de sobra
# sobre el costo medido —0,9 s en la configuración normal y 3 a 6 s con la
# fragmentación forzada— y queda bien por debajo del límite por prueba de
# run_tests.sh, que es lo que permite informar un fallo con nombre en vez de un
# "TIEMPO AGOTADO" genérico. Durante el desarrollo vi dos corridas de 67 s que
# no pude reproducir en nueve intentos y que atribuyo a carga del anfitrión;
# ajustar este tope a lo medido habría convertido eso en un fallo falso.
LIMITE_S=${E2E_LIMIT_S:-120}

TMP=$(mktemp -d) || exit 1

coord_pid=''
worker_pids=''
guard_pid=''
fallas=0

# Mata lo que haya quedado vivo y borra el temporal. Se llama en toda salida,
# incluida una señal: un coordinador huérfano se quedaría con su puerto y un
# trabajador huérfano violaría el criterio de que no sobreviva ningún proceso
# del programa a `make test`.
limpiar() {
    if [ -n "$guard_pid" ]; then
        kill -KILL "$guard_pid" 2>/dev/null
    fi
    for p in $worker_pids; do
        kill -KILL "$p" 2>/dev/null
    done
    if [ -n "$coord_pid" ]; then
        kill -KILL "$coord_pid" 2>/dev/null
    fi
    # Se recogen para no dejar zombis colgando de este script.
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap 'limpiar; exit 1' INT TERM HUP
trap 'limpiar' EXIT

decir() { echo "   $*"; }

# Un caso: N nodos, K incrementos cada uno, total esperado N*K.
caso() {
    n=$1
    esperado=$((n * K))
    fifo="$TMP/puerto"
    salida="$TMP/salida"
    mal=0

    echo "-- N=$n K=$K esperado=$esperado"
    rm -f "$fifo" "$salida"
    mkfifo "$fifo" || { decir "no se pudo crear el fifo"; return 1; }

    "$COORD" --nodes "$n" --pages "$PAGES" --port 0 \
             --timeout-ms "$TIMEOUT_MS" > "$fifo" 2> "$TMP/coord.err" &
    coord_pid=$!

    # El puerto se lee de un FIFO, y esa lectura es la que SINCRONIZA: se
    # bloquea hasta que el coordinador haya publicado su puerto, o sea hasta
    # que el socket esté escuchando. No hay espera a ciegas ni sondeo.
    #
    # Si el coordinador arranca y muere —argumentos malos, puerto ocupado—,
    # cierra el FIFO y la lectura devuelve fin de archivo en vez de colgarse,
    # que es lo que permite informar el motivo. El único caso que quedaría
    # bloqueado es que el ejecutable no exista, y eso se comprueba antes.
    port=''
    if ! { exec 3< "$fifo"; read -r port <&3; } then
        decir "FALLO: el coordinador no publicó un puerto"
        sed 's/^/     /' "$TMP/coord.err"
        return 1
    fi
    exec 3<&-
    decir "coordinador escuchando en el puerto $port"

    worker_pids=''
    i=0
    while [ "$i" -lt "$n" ]; do
        "$APP" --port "$port" --nodes "$n" --id "$i" --iters "$K" \
               >> "$salida" 2>> "$TMP/app.err" &
        worker_pids="$worker_pids $!"
        i=$((i + 1))
    done

    # El guardián: si el caso no termina dentro del límite, mata a todos. Eso
    # desbloquea los `wait` de abajo, que recogen el 137 de SIGKILL y lo
    # informan como falla. Un `wait` sin tope sería una espera sin fondo.
    ( sleep "$LIMITE_S"
      kill -KILL $worker_pids "$coord_pid" 2>/dev/null ) &
    guard_pid=$!

    # Códigos de salida de los trabajadores. Se esperan TODOS antes de juzgar,
    # para que un fallo temprano no esconda los demás en el informe.
    # El código se captura en una variable ANTES de probarlo: dentro de un
    # `if ! wait`, $? es el de la negación y siempre vale 0.
    for p in $worker_pids; do
        wait "$p"
        rc=$?
        if [ "$rc" -ne 0 ]; then
            decir "FALLO: un trabajador terminó con código $rc"
            mal=1
        fi
    done
    worker_pids=''

    # El coordinador sale solo cuando todos se despidieron: su código de
    # salida es lo que dice si la sesión terminó en orden o se abortó.
    wait "$coord_pid"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        decir "FALLO: el coordinador terminó con código $rc"
        mal=1
    fi
    coord_pid=''

    # Se desarma: ya no hay a quién matar, y dejarlo vivo haría que el script
    # tardara el límite entero en volver.
    kill -KILL "$guard_pid" 2>/dev/null
    wait "$guard_pid" 2>/dev/null
    guard_pid=''

    # El resultado, además de los códigos. Tienen que estar las N líneas: si
    # faltara una, algún nodo no llegó a verificar y eso no se ve en su
    # código de salida.
    lineas=$(grep -c "esperado=$esperado" "$salida" 2>/dev/null || true)
    if [ "${lineas:-0}" -ne "$n" ]; then
        decir "FALLO: $lineas de $n nodos informaron el total"
        mal=1
    fi
    if grep -qv "contador=$esperado esperado=$esperado" "$salida" 2>/dev/null; then
        decir "FALLO: algún nodo vio un total distinto de $esperado"
        sed 's/^/     /' "$salida"
        mal=1
    fi

    if [ "$mal" -eq 0 ]; then
        decir "OK: los $n nodos vieron $esperado"
    else
        sed 's/^/     /' "$TMP/app.err" 2>/dev/null
    fi
    return "$mal"
}

if [ ! -x "$COORD" ] || [ ! -x "$APP" ]; then
    echo "e2e_counter.sh: faltan ejecutables; corré make primero." >&2
    exit 1
fi

echo "e2e_counter.sh: contador compartido de punta a punta"

# Los tres tamaños del criterio de aceptación. N=1 verifica que el camino
# funcione sin contención ninguna; N=4 es el máximo del protocolo y el único
# que ejerce la cola de espera por página con tres nodos encolados.
for n in 1 2 4; do
    caso "$n" || fallas=$((fallas + 1))
done

if [ "$fallas" -ne 0 ]; then
    echo "e2e_counter.sh: $fallas de 3 casos fallaron"
    exit 1
fi
echo "e2e_counter.sh: los 3 casos pasaron"
exit 0
