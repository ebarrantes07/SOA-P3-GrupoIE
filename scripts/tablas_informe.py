#!/usr/bin/env python3
"""tablas_informe.py — las tablas del informe, derivadas del agregado (#10).

Escribe los cuerpos de las tres tablas de resultados como archivos .tex que
las secciones del informe incluyen con \\input. El informe no transcribe
ningún número: si la matriz se vuelve a correr, se regeneran las tablas y el
texto que las cita se revisa contra ellas.

    python3 scripts/analisis.py --json results/<matriz> > agregados.json
    python3 scripts/tablas_informe.py agregados.json docs/informe/tablas

Las cifras se escriben con coma decimal, que es la del documento, y los miles
con el espacio fino de LaTeX. La desviación es la muestral que calcula
analisis.py; con n=1 sería vacía, y acá se escribiría como una raya.
"""

import json
import os
import sys


def coma(s):
    return s.replace(".", ",")


def num(v, dec=2):
    """Un número con coma decimal, o una raya si no hay valor."""
    if v is None:
        return "---"
    return coma("%.*f" % (dec, v))


def miles(v):
    """Un entero con separador de miles fino, que LaTeX entiende en modo texto."""
    if v is None:
        return "---"
    s = "{:,}".format(int(round(v))).replace(",", "\\,")
    return s


def val(g, clave, campo="media"):
    m = g["metricas"].get(clave)
    return None if m is None else m[campo]


def pm(g, clave, dec=2):
    """media ± desviación, o solo la media si la desviación no existe.

    Un valor entero que no varió en ninguna repetición se escribe solo, sin
    «± 0,0»: es una cuenta exacta y no una medición con incertidumbre, y
    escribirla con decimales invita a leer ruido donde no lo hay.
    """
    m, s = val(g, clave), val(g, clave, "sd")
    if m is None:
        return "---"
    if s is None:
        return num(m, dec)
    if s == 0 and abs(m - round(m)) < 1e-9:
        return miles(m)
    return "$%s \\pm %s$" % (num(m, dec), num(s, dec))


def busca(datos, experimento, **filtros):
    for g in datos:
        if g["experimento"] != experimento:
            continue
        cfg = dict(p.split("=", 1) for p in g["config"].split())
        if all(cfg.get(k) == str(v) for k, v in filtros.items()):
            return g
    raise KeyError("no está la configuración %s %s" % (experimento, filtros))


def tabla_localidad(datos):
    filas = []
    for lecturas in (1, 8, 64):
        for modo, etiqueta in (("lote", "una sola"), ("repetido", "una por lectura")):
            g = busca(datos, "localidad", lecturas=lecturas, modo=modo)
            filas.append("%d & %s & %s & %s & %s & %s & %s \\\\" % (
                lecturas, etiqueta,
                pm(g, "app.t_lectura_ms", 3),
                miles(val(g, "app.adquisiciones")),
                miles(val(g, "red.msgs_total")),
                miles(val(g, "red.bytes_total")),
                miles(val(g, "red.page_xfers"))))
    return filas


def tabla_false_sharing(datos):
    filas = []
    for nodos in (2, 4):
        for layout, etiqueta in (("shared", "misma página"),
                                 ("separate", "una por nodo")):
            g = busca(datos, "false_sharing", nodes=nodos, layout=layout)
            filas.append("%d & %s & %s & %s & %s & %s & %s \\\\" % (
                nodos, etiqueta,
                pm(g, "nodo_max.t_trabajo_ms"),
                pm(g, "red.invals", 1),
                miles(val(g, "red.bytes_coherencia")),
                miles(val(g, "red.page_xfers")),
                miles(val(g, "red.bytes_coordinacion"))))
    return filas


def tabla_distribucion(datos):
    filas = []
    for terminos in (100003, 200000003):
        base = val(busca(datos, "distribucion", nodes=1, terms=terminos),
                   "app.t_calculo_ms")
        for nodos in (1, 2, 4):
            g = busca(datos, "distribucion", nodes=nodos, terms=terminos)
            calc = val(g, "app.t_calculo_ms")
            escala = "---" if not calc or not base else num(base / calc) + "$\\times$"
            filas.append("%s & %d & %s & %s & %s & %s & %s \\\\" % (
                miles(terminos), nodos,
                pm(g, "app.t_calculo_ms"),
                pm(g, "app.t_total_ms"),
                pm(g, "app.t_secuencial_ms"),
                miles(val(g, "red.bytes_total")),
                escala))
    return filas


# Cada tabla se escribe entera, con su preámbulo y su encabezado, y la sección
# la incluye entre el \caption y el fin del flotante. Incluir solo las filas,
# con el \input dentro del propio tabular, hace que LaTeX responda "Misplaced
# \noalign" en el \bottomrule.
TABLAS = {
    "localidad.tex": (
        tabla_localidad, "@{}rlrrrrr@{}",
        "lect. & permiso & tiempo (ms) & adq. & msjs. & bytes & págs."),
    "false_sharing.tex": (
        tabla_false_sharing, "@{}rlrrrrr@{}",
        "$N$ & contadores & tiempo (ms) & inval. & b.\\ coher. & págs. "
        "& b.\\ coord."),
    "distribucion.tex": (
        tabla_distribucion, "@{}rrrrrrr@{}",
        "$T$ & $N$ & cálculo (ms) & total (ms) & secuencial (ms) & bytes "
        "& escala"),
}


def main(argv):
    if len(argv) != 3:
        print("uso: tablas_informe.py agregados.json docs/informe/tablas",
              file=sys.stderr)
        return 2
    try:
        datos = json.load(open(argv[1], encoding="utf-8"))
    except (OSError, ValueError) as e:
        print("tablas_informe.py: no se pudo leer %s: %s" % (argv[1], e),
              file=sys.stderr)
        return 2

    ns = sorted({g["n"] for g in datos})
    if ns != [ns[0]]:
        print("tablas_informe.py: las configuraciones no comparten n (%s); "
              "el informe declara uno solo" % ns, file=sys.stderr)
        return 1
    malas = [g["config"] for g in datos if g["descartadas"]]
    if malas:
        print("tablas_informe.py: hay corridas descartadas en %s; el informe "
              "tiene que decirlo" % ", ".join(malas), file=sys.stderr)

    os.makedirs(argv[2], exist_ok=True)
    for nombre, (hacer, columnas, encabezado) in sorted(TABLAS.items()):
        try:
            filas = hacer(datos)
        except KeyError as e:
            print("tablas_informe.py: %s" % e, file=sys.stderr)
            return 1
        ruta = os.path.join(argv[2], nombre)
        with open(ruta, "w", encoding="utf-8") as f:
            f.write("%% Generado por scripts/tablas_informe.py. No editar.\n")
            f.write("\\begin{tabular}{%s}\n\\toprule\n%s \\\\\n\\midrule\n"
                    % (columnas, encabezado))
            f.write("\n".join(filas) + "\n")
            f.write("\\bottomrule\n\\end{tabular}\n")
        print("%s: %d filas" % (ruta, len(filas)))

    resumen = {"corridas": sum(g["repeticiones"] for g in datos),
               "configuraciones": len(datos), "n": ns[0],
               "descartadas": sum(len(g["descartadas"]) for g in datos),
               "entorno": next(g["entornos"][0] for g in datos if g["entornos"])}
    ruta = os.path.join(argv[2], "resumen.json")
    with open(ruta, "w", encoding="utf-8") as f:
        json.dump(resumen, f, ensure_ascii=False, indent=1)
    print("%s: %d corridas, n=%d, %d descartadas"
          % (ruta, resumen["corridas"], resumen["n"], resumen["descartadas"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
