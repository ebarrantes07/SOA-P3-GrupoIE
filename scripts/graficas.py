#!/usr/bin/env python3
"""graficas.py — las figuras de los experimentos, en SVG (#9).

Lee la salida de `analisis.py --json` y deja una figura por experimento.
Adaptado del graficas.py de SOA-P2-GrupoIE: de ahí vienen la clase Svg, el
cálculo de topes de eje y la decisión de escribir SVG a mano.

SIN DEPENDENCIAS, A PROPÓSITO. El README promete que los scripts usan solo la
biblioteca estándar, y el criterio de #10 exige reconstruir desde una copia
limpia. Una figura para un informe impreso no necesita matplotlib, y pedirlo
agregaría una instalación a la cadena de reproducción.

LOS COLORES NO SE INVENTAN. Son los slots 1 y 2 de la paleta categórica de
referencia y sus tintas, en el orden fijo: azul para el caso base y naranja
para el que se compara. Están validados en esa referencia (banda de luminosidad,
piso de croma, separación para visión con deficiencia de color y contraste
sobre la superficie clara); el validador del método necesita node, que no está
en este entorno, así que se usan los valores ya validados en vez de improvisar
hexadecimales nuevos. El mismo criterio que en P2.

Las figuras son para un PDF impreso: no llevan capa de interacción, y la
"vista de tabla" que pide la accesibilidad del método es la tabla de medias y
desviaciones que el informe imprime al lado.

QUÉ DIBUJA CADA UNA

  fig1_localidad.svg     barras agrupadas. Para cada cantidad de lecturas, el
                         costo de hacerlas dentro de un intervalo contra el de
                         volver a adquirir cada vez.
  fig2_false_sharing.svg barras agrupadas, dos paneles. Invalidaciones y bytes
                         con los contadores en una página y en una por nodo,
                         con N=2 y N=4.
  fig3_distribucion.svg  líneas. Tiempo contra N, con el tiempo de la fase de
                         cálculo aparte y el secuencial como referencia.

Cada figura declara de qué clave de configuración sale su eje x y de cuál sus
series. Si los datos no traen esa clave o esa métrica, la figura no se dibuja y
se dice qué faltó: una figura a medias en un informe es peor que ninguna.

Uso:
    analisis.py --json results/ | graficas.py --out results/
    graficas.py --datos agregados.json --out results/
"""
import json
import math
import os
import sys

# ---- Paleta (ver el encabezado: slots 1 y 2 ya validados) ---------------

SERIES = ["#2a78d6", "#eb6834"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SOFT = "#52514e"
GRID = "#e5e4e0"
AXIS = "#b9b8b2"

FONT = ("-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, "
        "'Helvetica Neue', Arial, sans-serif")

# ---- Utilidades de SVG (de P2) ------------------------------------------


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


class Svg:
    """Acumulador de elementos SVG. Sin dependencias y sin estado oculto."""

    def __init__(self, width, height):
        self.w = width
        self.h = height
        self.parts = []

    def add(self, markup):
        self.parts.append(markup)

    def text(self, x, y, s, size=11, fill=INK_SOFT, anchor="start",
             weight="normal"):
        self.add('<text x="%.1f" y="%.1f" font-size="%s" fill="%s" '
                 'text-anchor="%s" font-weight="%s">%s</text>'
                 % (x, y, size, fill, anchor, weight, esc(s)))

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1, dash=None):
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        self.add('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                 'stroke-width="%s"%s/>' % (x1, y1, x2, y2, stroke, width, d))

    def bar(self, x, y, w, h, fill, radius=4):
        """Punta redondeada y anclada a la base: la punta es el dato."""
        r = min(radius, w / 2, h) if h > 0 else 0
        self.add('<path d="M%.1f,%.1f L%.1f,%.1f Q%.1f,%.1f %.1f,%.1f '
                 'L%.1f,%.1f Q%.1f,%.1f %.1f,%.1f L%.1f,%.1f Z" fill="%s"/>'
                 % (x, y + h, x, y + r, x, y, x + r, y, x + w - r, y,
                    x + w, y, x + w, y + r, x + w, y + h, fill))

    def circle(self, cx, cy, r, fill, ring=None):
        if ring:
            self.add('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>'
                     % (cx, cy, r + 2, ring))
        self.add('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>'
                 % (cx, cy, r, fill))

    def polyline(self, points, stroke, width=2, dash=None):
        pts = " ".join("%.1f,%.1f" % (x, y) for x, y in points)
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        self.add('<polyline points="%s" fill="none" stroke="%s" '
                 'stroke-width="%s" stroke-linecap="round"%s/>'
                 % (pts, stroke, width, d))

    def barra_error(self, cx, y_lo, y_hi, ancho=3):
        """La desviación, en tinta secundaria: es incertidumbre, no una serie."""
        if abs(y_hi - y_lo) < 0.6:
            return
        self.line(cx, y_lo, cx, y_hi, stroke=INK_SOFT, width=1.2)
        self.line(cx - ancho, y_lo, cx + ancho, y_lo, stroke=INK_SOFT,
                  width=1.2)
        self.line(cx - ancho, y_hi, cx + ancho, y_hi, stroke=INK_SOFT,
                  width=1.2)

    def render(self):
        body = "\n  ".join(self.parts)
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" '
                'height="%d" viewBox="0 0 %d %d" font-family="%s">\n'
                '  <rect width="%d" height="%d" fill="%s"/>\n  %s\n</svg>\n'
                % (self.w, self.h, self.w, self.h, FONT, self.w, self.h,
                   SURFACE, body))


def tope_lindo(valor, marcas=5):
    """Tope de eje redondo y >= valor, para que las marcas no queden feas."""
    if valor <= 0:
        return 1.0, [0.0]
    crudo = valor / marcas
    mag = 10 ** math.floor(math.log10(crudo))
    paso = mag
    for mult in (1, 2, 2.5, 5, 10):
        paso = mult * mag
        if paso >= crudo:
            break
    top = paso * marcas
    while top < valor:
        top += paso
    return top, [i * paso for i in range(int(round(top / paso)) + 1)]


def fmt_num(x):
    if x is None:
        return "—"
    if abs(x) >= 10000:
        return "%.0fk" % (x / 1000.0)
    if abs(x - round(x)) < 1e-9:
        return "%d" % round(x)
    return "%.2f" % x


# ---- Los datos del análisis ---------------------------------------------


def config_dict(texto):
    """"layout=shared nodes=2" -> {"layout": "shared", "nodes": "2"}"""
    d = {}
    for parte in str(texto).split():
        if "=" in parte:
            k, v = parte.split("=", 1)
            d[k] = v
    return d


def orden_clave(v):
    try:
        return (0, float(v))
    except (TypeError, ValueError):
        return (1, str(v))


def ordenar_series(valores, orden):
    """El orden de las series lo fija la figura, no el alfabeto: el caso base
    va primero y se queda con el slot 1 de la paleta en todas las figuras."""
    if not orden:
        return sorted(valores, key=orden_clave)
    conocidos = [v for v in orden if v in valores]
    return conocidos + sorted(set(valores) - set(conocidos), key=orden_clave)


def filas_de(datos, experimento):
    """Las configuraciones de un experimento, con su config desarmada."""
    out = []
    for r in datos:
        if r.get("experimento") != experimento or r.get("n", 0) == 0:
            continue
        out.append((config_dict(r["config"]), r))
    return out


def valor(r, metrica):
    m = r["metricas"].get(metrica)
    if m is None:
        return None, None
    return m["media"], m["sd"]


# ---- Marco común de un panel --------------------------------------------


def marco(svg, x0, y0, w, h, maximo, titulo_y):
    """Ejes y grilla recesivos. Devuelve (tope, función y)."""
    top, marcas = tope_lindo(maximo)
    for t in marcas:
        y = y0 + h - (t / top) * h
        svg.line(x0, y, x0 + w, y, stroke=GRID, width=1)
        svg.text(x0 - 6, y + 3.5, fmt_num(t), size=10, anchor="end")
    svg.line(x0, y0 + h, x0 + w, y0 + h, stroke=AXIS, width=1)
    svg.text(x0 - 6, y0 - 8, titulo_y, size=10, fill=INK_SOFT, anchor="start")
    return top, (lambda v: y0 + h - (min(v, top) / top) * h)


def colocar_extremos(svg, extremos, y_tope, y_piso, obstaculos, minimo=12.0):
    """Coloca las etiquetas del extremo sin que se encimen entre ellas ni con
    la línea de referencia.

    Hace falta porque con datos reales las dos series CONVERGEN —con T grande
    el total y el cálculo casi coinciden, que es justamente la conclusión del
    experimento— y los dos números quedaban uno sobre otro, ilegibles
    (hallazgo de la revisión del #19, que las rasterizó). Cuando una etiqueta
    se corre de su punto, se dibuja una guía fina hasta él para que no se
    pierda de qué serie es."""
    if not extremos:
        return
    puestas = [dict(e) for e in extremos]
    for ob in obstaculos:
        for e in puestas:
            if abs(e["y"] - ob) < minimo:
                e["y"] = ob - minimo if e["y"] <= ob else ob + minimo
    puestas.sort(key=lambda e: e["y"])
    for i in range(1, len(puestas)):
        if puestas[i]["y"] - puestas[i - 1]["y"] < minimo:
            puestas[i]["y"] = puestas[i - 1]["y"] + minimo
    exceso = puestas[-1]["y"] - y_piso
    if exceso > 0:
        for e in puestas:
            e["y"] -= exceso
    falta = y_tope - puestas[0]["y"]
    if falta > 0:
        for e in puestas:
            e["y"] += falta
    for e in puestas:
        # El corrimiento se mide contra la posición SIN mover, no contra el
        # punto: el desplazamiento de 3.5 que centra el texto en su línea de
        # base no es un corrimiento y no merece guía.
        if abs(e["y"] - e["y_ini"]) > 3.0:
            svg.line(e["x"] - 5, e["y0"], e["x"] - 1, e["y"] - 3.5,
                     stroke=AXIS, width=1)
        svg.text(e["x"], e["y"], e["texto"], size=10, fill=INK)


def leyenda(svg, x, y, etiquetas):
    """Siempre presente con dos series o más: la identidad no es solo color."""
    cx = x
    for i, et in enumerate(etiquetas):
        color = SERIES[i % len(SERIES)]
        svg.add('<rect x="%.1f" y="%.1f" width="10" height="10" rx="2" '
                'fill="%s"/>' % (cx, y - 8, color))
        svg.text(cx + 14, y, et, size=11, fill=INK_SOFT)
        cx += 16 + 7.2 * len(et) + 18
    return cx


# ---- Figuras ------------------------------------------------------------


def barras_agrupadas(datos, spec, out_dir):
    """Un panel por métrica; en cada uno, un grupo por valor del eje x y una
    barra por serie."""
    filas = filas_de(datos, spec["experimento"])
    if not filas:
        return "no hay ninguna configuración del experimento '%s'" % spec["experimento"]
    for clave in (spec["x"], spec["series"]):
        if any(clave not in cfg for cfg, _ in filas):
            return "a la configuración le falta la clave '%s'" % clave

    xs = sorted({cfg[spec["x"]] for cfg, _ in filas}, key=orden_clave)
    ss = ordenar_series({cfg[spec["series"]] for cfg, _ in filas},
                        spec.get("orden_series"))
    paneles = spec["paneles"]
    faltan = [m for m, _ in paneles
              if all(valor(r, m)[0] is None for _, r in filas)]
    if faltan:
        return "falta la métrica %s en los datos" % ", ".join(faltan)

    # Presupuesto vertical: título 34, subtítulo 54, panel de 86 a 296,
    # marcas del eje 312, nombre del eje 330, leyenda 358.
    pw, ph = 330, 210
    sep = 56
    w = 60 + len(paneles) * pw + (len(paneles) - 1) * sep + 20
    h = 372
    svg = Svg(w, h)
    svg.text(28, 34, spec["titulo"], size=15, fill=INK, weight="600")
    svg.text(28, 54, spec["subtitulo"], size=11.5, fill=INK_SOFT)

    for pi, (metrica, etiqueta) in enumerate(paneles):
        x0 = 60 + pi * (pw + sep)
        y0 = 86
        maximo = 0
        for cfg, r in filas:
            med, sd = valor(r, metrica)
            if med is not None:
                maximo = max(maximo, med + (sd or 0))
        top, ey = marco(svg, x0, y0, pw, ph, maximo, etiqueta)

        gw = pw / len(xs)
        ancho = min(44, (gw - 16) / len(ss))
        for gi, xv in enumerate(xs):
            cx = x0 + gi * gw + gw / 2
            for si, sv in enumerate(ss):
                r = next((r for cfg, r in filas
                          if cfg[spec["x"]] == xv and cfg[spec["series"]] == sv),
                         None)
                if r is None:
                    continue
                med, sd = valor(r, metrica)
                if med is None:
                    continue
                # 2 px de superficie entre barras vecinas
                bx = cx - (len(ss) * (ancho + 2)) / 2 + si * (ancho + 2) + 1
                y = ey(med)
                svg.bar(bx, y, ancho, y0 + ph - y, SERIES[si % len(SERIES)])
                if sd:
                    svg.barra_error(bx + ancho / 2, ey(med - sd), ey(med + sd))
                svg.text(bx + ancho / 2, y - 7, fmt_num(med), size=10,
                         fill=INK, anchor="middle")
            svg.text(cx, y0 + ph + 16, str(xv), size=10.5, fill=INK_SOFT,
                     anchor="middle")
        svg.text(x0 + pw / 2, y0 + ph + 34, spec["x_titulo"], size=11,
                 fill=INK_SOFT, anchor="middle")

    leyenda(svg, 60, h - 14,
            [(spec.get("serie_formato", "%s") % s).strip() for s in ss])
    ruta = os.path.join(out_dir, spec["archivo"])
    with open(ruta, "w", encoding="utf-8") as f:
        f.write(svg.render())
    return None


def lineas(datos, spec, out_dir):
    """Una línea por métrica contra el eje x, con la referencia punteada."""
    filas = filas_de(datos, spec["experimento"])
    if not filas:
        return "no hay ninguna configuración del experimento '%s'" % spec["experimento"]
    if any(spec["x"] not in cfg for cfg, _ in filas):
        return "a la configuración le falta la clave '%s'" % spec["x"]
    faceta = spec.get("faceta")
    if faceta and any(faceta not in cfg for cfg, _ in filas):
        return "a la configuración le falta la clave '%s'" % faceta
    series = spec["series_metricas"]
    faltan = [m for m, _ in series
              if all(valor(r, m)[0] is None for _, r in filas)]
    if faltan:
        return "falta la métrica %s en los datos" % ", ".join(faltan)

    facetas = (sorted({cfg[faceta] for cfg, _ in filas}, key=orden_clave)
               if faceta else [None])
    # Igual que las barras, más una línea para el nombre de la faceta.
    pw, ph = 330, 210
    sep = 56
    w = 60 + len(facetas) * pw + (len(facetas) - 1) * sep + 20
    h = 392
    svg = Svg(w, h)
    svg.text(28, 34, spec["titulo"], size=15, fill=INK, weight="600")
    svg.text(28, 54, spec["subtitulo"], size=11.5, fill=INK_SOFT)

    for fi, fv in enumerate(facetas):
        grupo = [(cfg, r) for cfg, r in filas
                 if faceta is None or cfg[faceta] == fv]
        xs = sorted({cfg[spec["x"]] for cfg, _ in grupo}, key=orden_clave)
        x0 = 60 + fi * (pw + sep)
        y0 = 86
        maximo = 0
        for _, r in grupo:
            for metrica, _et in series:
                med, sd = valor(r, metrica)
                if med is not None:
                    maximo = max(maximo, med + (sd or 0))
            if spec.get("referencia"):
                med, _ = valor(r, spec["referencia"][0])
                if med is not None:
                    maximo = max(maximo, med)
        top, ey = marco(svg, x0, y0, pw, ph, maximo, spec["y_etiqueta"])
        paso = pw / max(len(xs), 1)

        def ex(i):
            return x0 + i * paso + paso / 2

        # La referencia va primero: su altura es un obstáculo para las
        # etiquetas del extremo, así que hay que conocerla antes de colocarlas.
        obstaculos = []
        if spec.get("referencia"):
            metrica, et = spec["referencia"]
            vals = [valor(r, metrica)[0] for _, r in grupo
                    if valor(r, metrica)[0] is not None]
            if vals:
                yr = ey(sum(vals) / len(vals))
                svg.line(x0, yr, x0 + pw, yr, stroke=INK_SOFT, width=1.4,
                         dash="5 4")
                svg.text(x0 + 6, yr - 7, et, size=10, fill=INK_SOFT)
                obstaculos.append(yr)

        extremos = []
        for si, (metrica, _et) in enumerate(series):
            puntos = []
            for i, xv in enumerate(xs):
                r = next((r for cfg, r in grupo if cfg[spec["x"]] == xv), None)
                if r is None:
                    continue
                med, sd = valor(r, metrica)
                if med is None:
                    continue
                puntos.append((ex(i), ey(med), med, sd))
            if not puntos:
                continue
            svg.polyline([(p[0], p[1]) for p in puntos],
                         SERIES[si % len(SERIES)])
            for px, py, med, sd in puntos:
                if sd:
                    svg.barra_error(px, ey(med - sd), ey(med + sd))
                svg.circle(px, py, 4.5, SERIES[si % len(SERIES)], ring=SURFACE)
            px, py, med, _ = puntos[-1]
            extremos.append({"x": px + 9, "y": py + 3.5, "y_ini": py + 3.5,
                             "y0": py, "texto": fmt_num(med)})

        colocar_extremos(svg, extremos, y0 + 8, y0 + ph - 2, obstaculos)

        for i, xv in enumerate(xs):
            svg.text(ex(i), y0 + ph + 16, str(xv), size=10.5, fill=INK_SOFT,
                     anchor="middle")
        svg.text(x0 + pw / 2, y0 + ph + 34, spec["x_titulo"], size=11,
                 fill=INK_SOFT, anchor="middle")
        if fv is not None:
            svg.text(x0 + pw / 2, y0 + ph + 56,
                     spec["faceta_formato"] % fv, size=11.5,
                     fill=INK, anchor="middle", weight="600")

    leyenda(svg, 60, h - 14, [et for _, et in series])
    ruta = os.path.join(out_dir, spec["archivo"])
    with open(ruta, "w", encoding="utf-8") as f:
        f.write(svg.render())
    return None


FIGURAS = [
    {
        "archivo": "fig1_localidad.svg",
        "experimento": "localidad",
        "titulo": "Localidad de lectura",
        "subtitulo": "El mismo trabajo de lectura, hecho dentro de un "
                     "intervalo o volviendo a adquirir cada vez.",
        "x": "lecturas", "x_titulo": "lecturas por intervalo",
        "series": "modo", "orden_series": ["lote", "repetido"],
        "serie_formato": "%s",
        "paneles": [("red.msgs_total", "mensajes"),
                    ("red.bytes_total", "bytes")],
        "dibuja": barras_agrupadas,
    },
    {
        "archivo": "fig2_false_sharing.svg",
        "experimento": "false_sharing",
        "titulo": "Falso compartimiento",
        "subtitulo": "Contadores independientes, en una misma página o en una "
                     "por nodo. Las adquisiciones y los incrementos son los "
                     "mismos en los dos diseños.",
        "x": "nodes", "x_titulo": "nodos (N)",
        "series": "layout", "orden_series": ["shared", "separate"],
        "serie_formato": "%s",
        "paneles": [("red.invals", "invalidaciones"),
                    ("red.bytes_total", "bytes")],
        "dibuja": barras_agrupadas,
    },
    {
        "archivo": "fig3_distribucion.svg",
        "experimento": "distribucion",
        "titulo": "Costo de distribución",
        "subtitulo": "π con el mismo T repartido entre N nodos. El total "
                     "incluye la inicialización de la sesión y la serie de "
                     "referencia. Cada panel tiene su propia escala.",
        "x": "nodes", "x_titulo": "nodos (N)",
        "y_etiqueta": "ms",
        "series_metricas": [("app.t_total_ms", "total, con inicialización"),
                            ("app.t_calculo_ms", "cálculo y publicación")],
        "referencia": ("app.t_secuencial_ms", "secuencial"),
        "faceta": "terms", "faceta_formato": "T = %s",
        "dibuja": lineas,
    },
]


def main(argv):
    out_dir = "results"
    datos_path = None
    args = argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out_dir = args[i + 1]
            i += 2
        elif args[i] == "--datos" and i + 1 < len(args):
            datos_path = args[i + 1]
            i += 2
        else:
            print("uso: graficas.py [--datos agregados.json] [--out dir]",
                  file=sys.stderr)
            return 2
    try:
        if datos_path:
            with open(datos_path, encoding="utf-8") as f:
                datos = json.load(f)
        else:
            datos = json.load(sys.stdin)
    except (OSError, ValueError) as e:
        print("graficas.py: no se pudieron leer los datos: %s" % e,
              file=sys.stderr)
        return 2
    if not isinstance(datos, list):
        print("graficas.py: se esperaba la lista que imprime "
              "`analisis.py --json`", file=sys.stderr)
        return 2
    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError as e:
        print("graficas.py: no se pudo crear %s: %s" % (out_dir, e),
              file=sys.stderr)
        return 2

    faltantes = 0
    for spec in FIGURAS:
        motivo = spec["dibuja"](datos, spec, out_dir)
        if motivo is None:
            print("   %s" % os.path.join(out_dir, spec["archivo"]))
        else:
            print("   SIN DIBUJAR %s: %s" % (spec["archivo"], motivo))
            faltantes += 1
    if faltantes:
        print("graficas.py: %d figura(s) sin dibujar, por lo dicho arriba"
              % faltantes)
    return 1 if faltantes else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
