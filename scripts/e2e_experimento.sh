#!/usr/bin/env sh
# e2e_experimento.sh — el lanzador de #9 cumple la costura con el análisis.
#
# Corre experimento.py con la matriz mínima (--rapido, 2 repeticiones) y
# comprueba lo que analisis.py da por hecho, sin depender de él:
#
#   - un .json y un .csv por corrida, con el nombre <config>_r<n>;
#   - los campos del contrato en cada JSON, códigos de salida en 0 y el
#     registro nombrado en `log` presente y con el encabezado del proyecto;
#   - un calentamiento por configuración, declarado, y fuera de los .json;
#   - que la localidad distinga de verdad las dos formas: con `repetido` el
#     lector pide una adquisición por lectura, con `lote` una sola, y en las
#     dos hace las mismas lecturas locales.
#
# Código de salida: 0 si todo cuadra.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1
trap 'rm -rf "$TMP"' EXIT
trap 'rm -rf "$TMP"; exit 1' INT TERM HUP

if ! command -v python3 > /dev/null 2>&1; then
    echo "e2e_experimento.sh: no hay python3 en el entorno." >&2
    exit 1
fi

echo "e2e_experimento.sh: el lanzador de la matriz de #9"

if ! python3 "$ROOT/scripts/experimento.py" --rapido --reps 2 \
        --out "$TMP/r" > "$TMP/out" 2>&1; then
    echo "   FALLO: experimento.py no terminó en 0"
    sed 's/^/     /' "$TMP/out"
    exit 1
fi
echo "   OK: $(tail -1 "$TMP/out")"

python3 -I - "$TMP/r" <<'PY'
import glob, json, os, sys

raiz = sys.argv[1]
fallas = []
CAMPOS = ("experimento", "config", "repeticion", "salida", "salida_por_nodo",
          "codigos_salida", "log", "entorno", "comando", "inicio_utc",
          "duracion_s", "calentamiento_descartado")
# --rapido: 4 configuraciones de localidad, 2 de false sharing, 2 de π.
ESPERADAS = {"localidad": 4, "false_sharing": 2, "distribucion": 2}
REPS = 2

for exp, configs in ESPERADAS.items():
    d = os.path.join(raiz, exp)
    jsons = sorted(glob.glob(os.path.join(d, "*.json")))
    if len(jsons) != configs * REPS:
        fallas.append("%s: %d JSON, se esperaban %d"
                      % (exp, len(jsons), configs * REPS))
    cal = glob.glob(os.path.join(d, "calentamiento", "*.txt"))
    cal = [c for c in cal if not c.endswith(".log.txt")]
    if len(cal) != configs:
        fallas.append("%s: %d calentamientos, se esperaban %d"
                      % (exp, len(cal), configs))
    if glob.glob(os.path.join(d, "calentamiento", "*.json")):
        fallas.append("%s: un calentamiento quedó como .json y el análisis "
                      "lo contaría" % exp)
    for j in jsons:
        base = os.path.basename(j)[:-5]
        doc = json.load(open(j, encoding="utf-8"))
        faltan = [c for c in CAMPOS if c not in doc]
        if faltan:
            fallas.append("%s: faltan %s" % (base, ", ".join(faltan)))
            continue
        if not base.endswith("_r%d" % doc["repeticion"]):
            fallas.append("%s: el nombre no termina en _r%d"
                          % (base, doc["repeticion"]))
        cod = doc["codigos_salida"]
        if cod["coordinador"] != 0 or any(c != 0 for c in cod["nodos"]):
            fallas.append("%s: códigos %s" % (base, cod))
        if len(doc["salida_por_nodo"]) != doc["config"]["nodes"]:
            fallas.append("%s: salida de %d nodos, la sesión tenía %d"
                          % (base, len(doc["salida_por_nodo"]),
                             doc["config"]["nodes"]))
        log = os.path.join(d, doc["log"])
        try:
            primera = open(log, encoding="utf-8").readline()
        except OSError:
            fallas.append("%s: falta el registro %s" % (base, doc["log"]))
            continue
        if not primera.startswith("run_id,component,"):
            fallas.append("%s: el registro no tiene el encabezado" % base)
        cal = os.path.join(d, doc["calentamiento_descartado"])
        if not os.path.isfile(cal):
            fallas.append("%s: el calentamiento declarado no existe" % base)
        if exp == "localidad":
            lector = doc["salida_por_nodo"][1]
            m = doc["config"]["lecturas"]
            adq = 1 if doc["config"]["modo"] == "lote" else m
            if (lector.get("accesos_locales") != str(m)
                    or lector.get("adquisiciones") != str(adq)):
                fallas.append("%s: el lector informó %s accesos y %s "
                              "adquisiciones, se esperaban %d y %d"
                              % (base, lector.get("accesos_locales"),
                                 lector.get("adquisiciones"), m, adq))

for f in fallas:
    print("   FALLO: " + f)
if not fallas:
    print("   OK: contrato completo en las %d corridas, calentamientos "
          "declarados, localidad con 1 y M adquisiciones"
          % (sum(ESPERADAS.values()) * REPS))
sys.exit(1 if fallas else 0)
PY
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "e2e_experimento.sh: el lanzador no cumple la costura"
    exit 1
fi
echo "e2e_experimento.sh: el lanzador cumple la costura con el análisis"
exit 0
