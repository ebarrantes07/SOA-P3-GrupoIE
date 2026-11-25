#!/usr/bin/env python3
"""analisis.py — media, desviación y n por configuración (#9).

Lee los resultados que deja el lanzador en `results/<experimento>/`, agrupa
las repeticiones de cada configuración y resume cada métrica. Adaptado del
analisis.py de SOA-P2-GrupoIE.

Por cada corrida espera dos archivos con el mismo nombre base:

    <config>_r<n>.json   lo que imprimió la aplicación, más entorno y códigos
    <config>_r<n>.csv    el registro del coordinador de ESA corrida

El JSON no trae métricas de tráfico: esas se derivan del CSV con metricas.py,
que es el único lugar donde se decide qué cuenta cada columna. Así un cambio
en la contabilidad no obliga a tocar el lanzador ni este archivo.

LO QUE SE DECIDIÓ ACÁ, y que el informe tiene que poder justificar:

  - Desviación estándar MUESTRAL (n−1), no poblacional: cinco repeticiones son
    una muestra de las corridas posibles, no la población. Con n=1 no existe y
    se informa vacía, nunca 0, que se leería como "sin variación".

  - Ninguna corrida se descarta en silencio (criterio de #9). Una corrida sale
    de las estadísticas solo por dos motivos, y los dos se informan con su
    nombre: que algún proceso no haya terminado en 0, o que su tráfico no
    cuadre según metricas.py. Si no queda ninguna corrida buena, la
    configuración se informa igual, con n=0.

  - Los campos numéricos se descubren solos. El lanzador parsea los pares
    clave=valor que imprime cada aplicación y los copia tal cual; acá se
    agrega todo lo que se pueda leer como número. Si una aplicación agrega un
    campo, aparece sin tocar este archivo.

  - De los valores POR NODO se toma el máximo antes de promediar entre
    repeticiones, porque una configuración tarda lo que tarda el nodo más
    lento: promediar los cuatro nodos escondería justamente al que define el
    tiempo de la corrida.

  - El entorno tiene que ser el mismo en todas las repeticiones de una
    configuración. Si no lo es, se avisa: mezclar máquinas vuelve
    incomparables los tiempos, y el criterio pide declarar el hardware.

Uso:
    analisis.py results/                    resumen legible
    analisis.py --json results/             para graficas.py
    analisis.py results/false_sharing/      una sola familia
"""
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metricas                                            # noqa: E402


def numero(v):
    """float si el valor se lee como número, None si no."""
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    try:
        return float(str(v).strip())
    except (TypeError, ValueError):
        return None


def resumen(valores):
    """n, media y desviación muestral. La desviación es None con n < 2."""
    n = len(valores)
    if n == 0:
        return {"n": 0, "media": None, "sd": None, "min": None, "max": None}
    media = sum(valores) / n
    sd = None
    if n > 1:
        var = sum((v - media) ** 2 for v in valores) / (n - 1)
        sd = math.sqrt(var)
    return {"n": n, "media": media, "sd": sd,
            "min": min(valores), "max": max(valores)}


def clave_config(cfg):
    """Identidad de una configuración, estable y legible."""
    if not isinstance(cfg, dict):
        return str(cfg)
    return " ".join("%s=%s" % (k, cfg[k]) for k in sorted(cfg))


def cargar_corrida(ruta):
    """Lee un JSON de corrida y le agrega el tráfico de su CSV.

    Devuelve (corrida, motivo_de_descarte). El motivo es None si sirve."""
    try:
        with open(ruta, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        return None, "no se pudo leer el JSON: %s" % e

    for campo in ("experimento", "config", "repeticion"):
        if campo not in d:
            return None, "al JSON le falta el campo %s" % campo

    cod = d.get("codigos_salida", {})
    malos = []
    if cod.get("coordinador", 0) != 0:
        malos.append("coordinador=%s" % cod["coordinador"])
    for i, c in enumerate(cod.get("nodos", [])):
        if c != 0:
            malos.append("nodo%d=%s" % (i, c))

    tr = None
    nombre_log = d.get("log")
    if nombre_log:
        csv_path = os.path.join(os.path.dirname(ruta), nombre_log)
        try:
            tr = metricas.metricas(csv_path)
        except (OSError, ValueError) as e:
            return d, "no se pudo medir el registro: %s" % e
        if not tr["verificacion"]["todo_cuadra"]:
            return d, "el tráfico del registro no cuadra"
    d["_trafico"] = tr

    if malos:
        return d, "procesos que no terminaron en 0: " + ", ".join(malos)
    return d, None


def metricas_de(c):
    """Todas las métricas numéricas de una corrida, con su prefijo de origen."""
    m = {}
    for k, v in (c.get("salida") or {}).items():
        x = numero(v)
        if x is not None:
            m["app." + k] = x
    por_nodo = c.get("salida_por_nodo") or []
    campos = {}
    for nodo in por_nodo:
        for k, v in (nodo or {}).items():
            x = numero(v)
            if x is not None:
                campos.setdefault(k, []).append(x)
    for k, vs in campos.items():
        # El máximo, no el promedio: la corrida tarda lo que el nodo más lento.
        m["nodo_max." + k] = max(vs)
    tr = c.get("_trafico")
    if tr:
        for k in ("msgs_in", "msgs_out", "bytes_in", "bytes_out", "bytes_total",
                  "page_xfers", "invals", "grants_con_pagina",
                  "grants_sin_pagina", "filas"):
            m["red." + k] = float(tr[k])
        for k, v in tr["bytes_por_grupo"].items():
            m["red.bytes_" + k] = float(v)
        for k, v in tr["adquisiciones"].items():
            m["red.adq_" + k] = float(v)
    if "duracion_s" in c:
        x = numero(c["duracion_s"])
        if x is not None:
            m["corrida.duracion_s"] = x
    return m


def analizar(rutas):
    grupos = {}
    for ruta in sorted(rutas):
        c, motivo = cargar_corrida(ruta)
        if c is None:
            # Ni siquiera se pudo leer: se informa aparte, sin configuración.
            grupos.setdefault(("?", "?"), nuevo_grupo())["ilegibles"].append(
                {"archivo": os.path.basename(ruta), "motivo": motivo})
            continue
        clave = (c["experimento"], clave_config(c["config"]))
        g = grupos.setdefault(clave, nuevo_grupo())
        g["corridas"].append(c)
        if motivo is not None:
            g["descartadas"].append(
                {"archivo": os.path.basename(ruta),
                 "repeticion": c.get("repeticion"), "motivo": motivo})
        else:
            g["buenas"].append(c)
        ent = c.get("entorno")
        if ent is not None:
            firma = json.dumps(ent, sort_keys=True)
            if firma not in g["entornos"]:
                g["entornos"][firma] = ent

    salida = []
    for (exp, cfg), g in sorted(grupos.items()):
        campos = {}
        for c in g["buenas"]:
            for k, v in metricas_de(c).items():
                campos.setdefault(k, []).append(v)
        salida.append({
            "experimento": exp,
            "config": cfg,
            "repeticiones": len(g["corridas"]),
            "n": len(g["buenas"]),
            "descartadas": g["descartadas"],
            "ilegibles": g["ilegibles"],
            "entornos": list(g["entornos"].values()),
            "entorno_mezclado": len(g["entornos"]) > 1,
            "metricas": {k: resumen(v) for k, v in sorted(campos.items())},
        })
    return salida


def nuevo_grupo():
    return {"corridas": [], "buenas": [], "descartadas": [], "ilegibles": [],
            "entornos": {}}


def fmt(x):
    if x is None:
        return "—"
    if abs(x - round(x)) < 1e-9 and abs(x) < 1e15:
        return "%d" % round(x)
    return "%.3f" % x


def imprimir(res):
    problemas = 0
    for r in res:
        if r["experimento"] == "?":
            print("== archivos que no se pudieron leer")
            for d in r["ilegibles"]:
                print("     ILEGIBLE %s: %s" % (d["archivo"], d["motivo"]))
                problemas += 1
            continue
        print("== %s · %s" % (r["experimento"], r["config"]))
        linea = "   %d repeticiones, n=%d" % (r["repeticiones"], r["n"])
        if r["descartadas"]:
            linea += ", %d descartadas" % len(r["descartadas"])
        print(linea)
        for d in r["descartadas"]:
            print("     DESCARTADA r%s (%s): %s"
                  % (d["repeticion"], d["archivo"], d["motivo"]))
            problemas += 1
        for d in r["ilegibles"]:
            print("     ILEGIBLE %s: %s" % (d["archivo"], d["motivo"]))
            problemas += 1
        if r["entorno_mezclado"]:
            print("     AVISO: %d entornos distintos entre repeticiones; los "
                  "tiempos no son comparables" % len(r["entornos"]))
            problemas += 1
        if r["n"] == 0:
            print("     sin ninguna corrida utilizable")
            continue
        anchos = max(len(k) for k in r["metricas"])
        for k, s in r["metricas"].items():
            print("   %-*s  %12s ± %-10s  [%s, %s]  n=%d"
                  % (anchos, k, fmt(s["media"]), fmt(s["sd"]),
                     fmt(s["min"]), fmt(s["max"]), s["n"]))
    return problemas


def rutas_de(args):
    rutas = []
    for a in args:
        if os.path.isdir(a):
            rutas += glob.glob(os.path.join(a, "**", "*.json"), recursive=True)
        else:
            rutas.append(a)
    return rutas


def main(argv):
    como_json = "--json" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        print("uso: analisis.py [--json] results/ | corrida.json ...",
              file=sys.stderr)
        return 2
    rutas = rutas_de(args)
    if not rutas:
        print("analisis.py: no hay ningún .json en %s" % ", ".join(args),
              file=sys.stderr)
        return 2
    res = analizar(rutas)
    if como_json:
        json.dump(res, sys.stdout, indent=2, ensure_ascii=False)
        print()
        return 0
    problemas = imprimir(res)
    if problemas:
        print("\nanalisis.py: %d corrida(s) o configuración(es) con problemas, "
              "informadas arriba" % problemas)
    return 1 if problemas else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
