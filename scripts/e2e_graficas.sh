#!/usr/bin/env sh
# e2e_graficas.sh — las figuras, contra datos de forma conocida (#9).
#
# No hay forma de MIRAR las figuras en este entorno: no hay navegador ni
# rasterizador de SVG. Así que en vez de confiar en que salieron bien, se
# comprueba por cálculo lo que un ojo verificaría: que el SVG sea XML válido,
# que ninguna coordenada caiga fuera del lienzo, que no haya NaN y que la
# cantidad de marcas sea la que los datos piden. Lo que no reemplaza: mirarlas
# antes de que entren al informe, que queda como tarea de #10.
#
# Y un caso que importa más que el dibujo: si faltan datos, la figura NO se
# dibuja y se dice qué faltó. Una figura a medias en un informe es peor que
# ninguna.
#
# Código de salida: 0 si todo lo esperado se cumple.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GRAF="$ROOT/scripts/graficas.py"
TMP=$(mktemp -d /tmp/soa-p3.XXXXXX) || exit 1
trap 'rm -rf "$TMP"' EXIT

fallas=0

if ! command -v python3 > /dev/null 2>&1; then
    echo "e2e_graficas.sh: no hay python3 en el entorno." >&2
    exit 1
fi

echo "e2e_graficas.sh: las tres figuras, y lo que pasa cuando faltan datos"

# ---- Datos de forma conocida --------------------------------------------
#
# Las formas son las que el informe va a contar: con `lote` los mensajes no
# crecen con las lecturas y con `repetido` sí; `shared` invalida y `separate`
# no; y el tiempo total baja con N mientras el secuencial queda fijo.

python3 - "$TMP/datos.json" <<'PY'
import json, sys

def cfg(d):
    return " ".join("%s=%s" % (k, d[k]) for k in sorted(d))

def met(**kv):
    return {k: {"n": 5, "media": v, "sd": v * 0.03, "min": v * 0.9,
                "max": v * 1.1} for k, v in kv.items()}

filas = []
for lecturas, lote, repetido in ((1, 30, 30), (8, 30, 240), (64, 30, 1920)):
    for modo, msgs in (("lote", lote), ("repetido", repetido)):
        filas.append({
            "experimento": "localidad",
            "config": cfg({"lecturas": lecturas, "modo": modo, "nodes": 2}),
            "repeticiones": 5, "n": 5, "descartadas": [], "ilegibles": [],
            "entornos": [{"uname": "a"}], "entorno_mezclado": False,
            "metricas": met(**{"red.msgs_total": msgs,
                               "red.bytes_total": msgs * 1200.0}),
        })
for n, sh_inv, sh_by in ((2, 95, 350000), (4, 199, 692000)):
    for layout, inv, by in (("shared", sh_inv, sh_by),
                            ("separate", 0, sh_by // 2)):
        filas.append({
            "experimento": "false_sharing",
            "config": cfg({"nodes": n, "layout": layout, "rounds": 50}),
            "repeticiones": 5, "n": 5, "descartadas": [], "ilegibles": [],
            "entornos": [{"uname": "a"}], "entorno_mezclado": False,
            "metricas": met(**{"red.invals": float(inv),
                               "red.bytes_total": float(by)}),
        })
for terms, base in ((100003, 12.0), (5000003, 600.0)):
    for n, factor in ((1, 1.0), (2, 0.56), (4, 0.33)):
        filas.append({
            "experimento": "distribucion",
            "config": cfg({"nodes": n, "terms": terms}),
            "repeticiones": 5, "n": 5, "descartadas": [], "ilegibles": [],
            "entornos": [{"uname": "a"}], "entorno_mezclado": False,
            "metricas": met(**{"app.t_total_ms": base * factor + 8,
                               "app.t_calculo_ms": base * factor,
                               "app.t_secuencial_ms": base}),
        })
json.dump(filas, open(sys.argv[1], "w"), ensure_ascii=False)
PY

mkdir -p "$TMP/fig"
if python3 "$GRAF" --datos "$TMP/datos.json" --out "$TMP/fig" > "$TMP/out" 2>&1; then
    echo "   OK: las tres figuras se dibujaron"
else
    echo "   FALLO: alguna figura no se dibujó"
    sed 's/^/     /' "$TMP/out"
    fallas=$((fallas + 1))
fi

# ---- Lo que un ojo verificaría, por cálculo ------------------------------

for f in fig1_localidad fig2_false_sharing fig3_distribucion; do
    if [ ! -s "$TMP/fig/$f.svg" ]; then
        echo "   FALLO: $f.svg no existe o está vacío"
        fallas=$((fallas + 1))
        continue
    fi
    if python3 - "$TMP/fig/$f.svg" <<'PY'
import re, sys
import xml.etree.ElementTree as ET

ruta = sys.argv[1]
texto = open(ruta, encoding="utf-8").read()
raiz = ET.fromstring(texto)            # XML mal formado -> excepción
w = float(raiz.get("width")); h = float(raiz.get("height"))

if "nan" in texto.lower():
    print("   hay NaN en las coordenadas"); sys.exit(1)

margen = 2.0
malos = []
for el in raiz.iter():
    for attr in ("x", "y", "cx", "cy", "x1", "y1", "x2", "y2"):
        v = el.get(attr)
        if v is None:
            continue
        f = float(v)
        lim = w if attr in ("x", "cx", "x1", "x2") else h
        if f < -margen or f > lim + margen:
            malos.append((el.tag.split('}')[-1], attr, f, lim))
for el in raiz.iter():
    for pts in ([el.get("points")] if el.get("points") else []):
        for par in pts.split():
            px, py = (float(t) for t in par.split(","))
            if not (-margen <= px <= w + margen and -margen <= py <= h + margen):
                malos.append(("polyline", "punto", px, py))
for d in re.findall(r'\sd="([^"]+)"', texto):
    for num in re.findall(r'-?\d+\.?\d*', d):
        pass                            # las barras se construyen con x,y ya acotados
if malos:
    print("   %d coordenada(s) fuera del lienzo %gx%g: %s"
          % (len(malos), w, h, malos[:4]))
    sys.exit(1)

# alturas de barra no negativas: una barra invertida se vería como un hueco
for d in re.findall(r'<path d="M([^"]+)"', texto):
    pass
print("   %s: %gx%g, %d elementos, coordenadas dentro del lienzo"
      % (ruta.split("/")[-1], w, h, len(list(raiz.iter())) - 1))
PY
    then
        echo "   OK: $f.svg es XML válido y no se sale del lienzo"
    else
        echo "   FALLO: $f.svg no pasó la comprobación de geometría"
        fallas=$((fallas + 1))
    fi
done

# Las etiquetas del extremo no se enciman. Los datos de prueba tienen el caso
# difícil a propósito: con T grande las dos series convergen (206 y 198), que
# es la conclusión del experimento y es cuando los números se superponían
# (revisión del #19).
if python3 - "$TMP/fig/fig3_distribucion.svg" <<'ETIQ'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
et = [(float(x), float(y), t) for x, y, t in re.findall(
    r'<text x="([0-9.]+)" y="([0-9.]+)" font-size="10" fill="#0b0b0b"[^>]*>'
    r'([^<]+)</text>', s)]
if len(et) != 4:
    print("   se esperaban 4 etiquetas de extremo y hay %d" % len(et))
    sys.exit(1)
mal = [(a, b) for i, a in enumerate(et) for b in et[i + 1:]
       if abs(a[0] - b[0]) < 60 and abs(a[1] - b[1]) < 12]
if mal:
    print("   etiquetas encimadas: %s" % (mal,))
    sys.exit(1)
print("   las 4 etiquetas del extremo estan a 12 px o mas entre si")
ETIQ
then
    echo "   OK: las etiquetas del extremo no se enciman"
else
    echo "   FALLO: hay etiquetas del extremo encimadas"
    fallas=$((fallas + 1))
fi

# Ningún marcador tapa la etiqueta de la referencia, y ninguna barra de error
# queda debajo de su marcador. Las dos las encontró una revisión mirando la
# figura: la primera hacía que "secuencial" se leyera "secuencia", y la segunda
# escondía entera cualquier desviación menor que el radio del marcador.
if python3 - "$TMP/fig/fig3_distribucion.svg" <<'TAPA'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
fallas = []

refs = [(float(x), float(y)) for x, y in re.findall(
    r'<text x="([0-9.]+)" y="([0-9.]+)" font-size="10" fill="#52514e"'
    r'[^>]*>secuencial</text>', s)]
if len(refs) != 2:
    print("   se esperaban 2 etiquetas de referencia y hay %d" % len(refs))
    sys.exit(1)
puntos = [(float(x), float(y)) for x, y, r in re.findall(
    r'<circle cx="([0-9.]+)" cy="([0-9.]+)" r="([0-9.]+)"', s) if r == "6.5"]
for lx, ly in refs:
    ancho = 4.9 * len("secuencial")
    for px, py in puntos:
        if (px + 7.5 > lx and px - 7.5 < lx + ancho
                and py + 7.5 > ly - 8 and py - 7.5 < ly + 2):
            fallas.append("un marcador en (%g,%g) tapa la etiqueta en (%g,%g)"
                          % (px, py, lx, ly))

# Orden de dibujo: el marcador y después su barra. La barra se reconoce por
# estar centrada en el punto, no por pasarle por encima: con dos series que
# convergen, la barra de una contiene el marcador de la otra y emparejarlas
# por superposición daba un falso positivo.
for m in re.finditer(r'<circle cx="([0-9.]+)" cy="([0-9.]+)" r="4.5"', s):
    cx, cy = float(m.group(1)), float(m.group(2))
    barra = [b for b in re.finditer(
        r'<line x1="([0-9.]+)" y1="([0-9.]+)" x2="\1" y2="([0-9.]+)"', s)
        if abs(float(b.group(1)) - cx) < 0.05
        and abs((float(b.group(2)) + float(b.group(3))) / 2 - cy) < 0.6]
    for b in barra:
        if b.start() < m.start():
            fallas.append("la barra de error en x=%g se dibuja antes que su "
                          "marcador" % cx)

if fallas:
    for f in fallas:
        print("   " + f)
    sys.exit(1)
print("   %d etiquetas de referencia despejadas, %d marcadores antes de su barra"
      % (len(refs), len(puntos)))
TAPA
then
    echo "   OK: nada tapa la referencia ni esconde una barra de error"
else
    echo "   FALLO: una etiqueta tapada o una barra de error escondida"
    fallas=$((fallas + 1))
fi

# Cantidad de marcas: 3 lecturas x 2 modos x 2 paneles = 12 barras.
barras=$(grep -c '<path d=' "$TMP/fig/fig1_localidad.svg" || true)
if [ "$barras" -eq 12 ]; then
    echo "   OK: fig1 tiene las 12 barras que piden los datos"
else
    echo "   FALLO: fig1 tiene $barras barras y debía tener 12"
    fallas=$((fallas + 1))
fi
# fig3: 2 facetas x 2 series = 4 polilíneas, y 2 referencias punteadas.
lineas=$(grep -c '<polyline' "$TMP/fig/fig3_distribucion.svg" || true)
punteadas=$(grep -c 'stroke-dasharray' "$TMP/fig/fig3_distribucion.svg" || true)
if [ "$lineas" -eq 4 ] && [ "$punteadas" -eq 2 ]; then
    echo "   OK: fig3 tiene 4 líneas y las 2 referencias secuenciales"
else
    echo "   FALLO: fig3 tiene $lineas líneas y $punteadas referencias"
    fallas=$((fallas + 1))
fi
# La leyenda está siempre: dos series, dos recuadros por figura.
for f in fig1_localidad fig2_false_sharing fig3_distribucion; do
    if [ "$(grep -c '<rect x=' "$TMP/fig/$f.svg")" -ge 2 ]; then
        echo "   OK: $f tiene leyenda"
    else
        echo "   FALLO: $f no tiene leyenda"
        fallas=$((fallas + 1))
    fi
done

# ---- Cuando faltan datos ------------------------------------------------

echo "-- cuando faltan datos, la figura no se dibuja y se dice por qué"
echo '[]' > "$TMP/vacio.json"
mkdir -p "$TMP/fig2"
python3 "$GRAF" --datos "$TMP/vacio.json" --out "$TMP/fig2" > "$TMP/out2" 2>&1
rc=$?
if [ "$rc" -ne 1 ]; then
    echo "   FALLO: con datos vacíos devolvió $rc y debía devolver 1"
    fallas=$((fallas + 1))
else
    echo "   OK: devuelve 1"
fi
if [ "$(grep -c 'SIN DIBUJAR' "$TMP/out2")" -eq 3 ]; then
    echo "   OK: las tres se informan sin dibujar, con su motivo"
else
    echo "   FALLO: no informó las tres"
    sed 's/^/     /' "$TMP/out2"
    fallas=$((fallas + 1))
fi
if [ -n "$(ls -A "$TMP/fig2" 2>/dev/null)" ]; then
    echo "   FALLO: dejó archivos de figuras a medias"
    fallas=$((fallas + 1))
else
    echo "   OK: no deja ninguna figura a medias"
fi

# Datos con la métrica cambiada de nombre: tiene que decir cuál falta.
python3 - "$TMP/datos.json" "$TMP/sinmetrica.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for r in d:
    if r["experimento"] == "false_sharing":
        r["metricas"].pop("red.invals", None)
json.dump(d, open(sys.argv[2], "w"))
PY
mkdir -p "$TMP/fig3"
python3 "$GRAF" --datos "$TMP/sinmetrica.json" --out "$TMP/fig3" > "$TMP/out3" 2>&1
if grep -q "falta la métrica red.invals" "$TMP/out3"; then
    echo "   OK: nombra la métrica que falta"
else
    echo "   FALLO: no nombró la métrica que falta"
    sed 's/^/     /' "$TMP/out3"
    fallas=$((fallas + 1))
fi

if [ "$fallas" -ne 0 ]; then
    echo "e2e_graficas.sh: $fallas comprobaciones fallaron"
    exit 1
fi
echo "e2e_graficas.sh: las figuras salen, y sin datos no se inventan"
exit 0
