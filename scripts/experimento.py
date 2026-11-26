#!/usr/bin/env python3
"""experimento.py — la matriz de experimentos de #9, desatendida.

Corre cada configuración de los tres experimentos del enunciado con R
repeticiones (5 por omisión), cada una con su propio coordinador, y deja por
corrida los dos archivos que lee analisis.py (la costura de #9):

    results/<experimento>/<config>_r<n>.json   lo que imprimió cada nodo
    results/<experimento>/<config>_r<n>.csv    el registro del coordinador

Este lanzador NO calcula ninguna métrica de tráfico: deja el CSV crudo y
metricas.py decide qué cuenta cada columna. De la salida de las aplicaciones
toma, sin esquema, todos los pares clave=valor que imprime cada nodo.

LO QUE SE DECIDIÓ ACÁ (D8.1):

  - Una corrida de CALENTAMIENTO por configuración, antes de las medidas, que
    no entra a las estadísticas y sí queda declarada: su salida se guarda en
    results/<experimento>/calentamiento/<config>.txt (no es .json, así que el
    análisis no la levanta) y cada JSON medido la nombra en
    `calentamiento_descartado`. La primera corrida después de compilar llegó
    a medir la mitad que las siguientes; descartarla en silencio violaría el
    criterio de #9.
  - Una corrida que falla NO detiene la matriz: queda escrita con sus códigos
    de salida, y es el análisis el que la excluye con su motivo. Al final se
    listan todas las que fallaron y el código de salida es 1.
  - Cada proceso corre en su propio grupo y con tope de tiempo; al vencer, se
    mata el grupo entero. Nada queda vivo entre corridas.
  - El experimento de distribución corre app_pi con --barriers 0: las 100
    generaciones de barrera son una prueba de corrección, no parte del costo
    de calcular π, y entrarían en t_total_ms.

Uso:
    experimento.py                          la matriz completa, 5 repeticiones
    experimento.py --solo false_sharing     una sola familia (separadas por coma)
    experimento.py --reps 3 --out /tmp/r    otra cantidad u otro destino
    experimento.py --rapido                 matriz mínima, para probar el lanzador
"""
import json
import os
import platform
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COORD = os.path.join(ROOT, "dsm_manager")
TIMEOUT_MS = int(os.environ.get("EXPERIMENTO_TIMEOUT_MS", "30000"))  # plazo por operación
LIMITE_S = 300              # tope de una corrida entera

PAR = re.compile(r"^([a-z_][a-z0-9_]*)=(\S+)$")


# ---- La matriz -------------------------------------------------------------
#
# Cada configuración: (nombre, dict de configuración, ejecutable, nodos,
# páginas, argumentos). El dict usa las claves que esperan analisis.py y
# graficas.py: lecturas/modo/nodes, nodes/layout/rounds y nodes/terms.

def matriz(rapido):
    lecturas = (1, 8) if rapido else (1, 8, 64)
    nodos_fs = (2,) if rapido else (2, 4)
    rondas = 5 if rapido else 50
    terminos = (1003,) if rapido else (100003, 200000003)
    nodos_pi = (1, 2) if rapido else (1, 2, 4)

    exps = {"localidad": [], "false_sharing": [], "distribucion": []}
    for modo, acceso in (("lote", "batch"), ("repetido", "repeated")):
        for m in lecturas:
            exps["localidad"].append((
                "n2_%s_lec%d" % (modo, m),
                {"nodes": 2, "modo": modo, "lecturas": m},
                "app_rw", 2, 1, ["--reads", str(m), "--access", acceso]))
    for n in nodos_fs:
        for layout in ("shared", "separate"):
            exps["false_sharing"].append((
                "n%d_%s_r%d" % (n, layout, rondas),
                {"nodes": n, "layout": layout, "rounds": rondas},
                "app_granularity", n, n,
                ["--layout", layout, "--rounds", str(rondas)]))
    for t in terminos:
        for n in nodos_pi:
            exps["distribucion"].append((
                "n%d_t%d" % (n, t), {"nodes": n, "terms": t},
                "app_pi", n, 2, ["--terms", str(t), "--barriers", "0"]))
    return exps


# ---- El entorno ------------------------------------------------------------

def primera_linea(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return (out.stdout or out.stderr).splitlines()[0].strip()
    except (OSError, subprocess.SubprocessError, IndexError):
        return None


def entorno():
    cpu = None
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for linea in f:
                if linea.startswith("model name"):
                    cpu = linea.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    glibc = None
    try:
        glibc = os.confstr("CS_GNU_LIBC_VERSION")
    except (ValueError, OSError, AttributeError):
        pass
    return {
        "uname": " ".join(platform.uname()),
        "cpu": cpu,
        "nproc": os.cpu_count(),
        "gcc": primera_linea(["gcc", "--version"]),
        "glibc": glibc,
        "python": platform.python_version(),
    }


# ---- Una corrida -----------------------------------------------------------

def pares(texto):
    """Todos los clave=valor de la salida de un nodo, sin esquema."""
    d = {}
    for linea in texto.splitlines():
        for tok in linea.split():
            m = PAR.match(tok)
            if m:
                d[m.group(1)] = m.group(2)
    return d


def matar(p):
    """Al grupo entero: así no queda ningún hijo de timeout huérfano."""
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def correr(app, nodos, paginas, args, log_path, run_id):
    """Una sesión: coordinador y N nodos. Devuelve un dict con las salidas,
    los códigos y la duración. Nunca lanza por un fallo de los procesos.

    Cada proceso va en su propia sesión para poder matarlo con su grupo, y
    eso lo saca del grupo de este script: un Ctrl-C ya no le llegaría. Por eso
    el `finally` mata lo que quede vivo, salga como salga esta función."""
    vivos = []
    try:
        return _correr(app, nodos, paginas, args, log_path, run_id, vivos)
    finally:
        for p in vivos:
            if p.poll() is None:
                matar(p)
                p.wait()


def _correr(app, nodos, paginas, args, log_path, run_id, vivos):
    t0 = time.monotonic()
    coord = subprocess.Popen(
        [COORD, "--nodes", str(nodos), "--pages", str(paginas), "--port", "0",
         "--timeout-ms", str(TIMEOUT_MS), "--log", log_path,
         "--run-id", str(run_id)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True)
    vivos.append(coord)
    # La primera línea es el puerto: leerla sincroniza con el socket ya
    # escuchando. Fin de archivo = el coordinador murió al arrancar.
    puerto = coord.stdout.readline().strip()
    if not puerto.isdigit():
        # Si cerró stdout pero sigue vivo, no se le espera para siempre ni se
        # deja escapar la excepción: la matriz seguiría con un traceback.
        try:
            coord.wait(timeout=10)
        except subprocess.TimeoutExpired:
            matar(coord)
            coord.wait()
        return {"codigos": {"coordinador": coord.returncode, "nodos": []},
                "salidas": [], "errores": [coord.stderr.read()],
                "comando": [], "duracion_s": time.monotonic() - t0}

    cmd = [os.path.join(ROOT, app), "--port", puerto, "--nodes", str(nodos),
           "--timeout-ms", str(TIMEOUT_MS)] + args
    procs = [subprocess.Popen(cmd + ["--id", str(i)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True,
                              start_new_session=True)
             for i in range(nodos)]
    vivos.extend(procs)

    limite = t0 + LIMITE_S
    salidas, errores, codigos = [], [], []
    for p in procs:
        try:
            out, err = p.communicate(timeout=max(1.0, limite - time.monotonic()))
        except subprocess.TimeoutExpired:
            matar(p)
            out, err = p.communicate()
            err += "\n[experimento.py: vencido el tope de %d s]" % LIMITE_S
        salidas.append(out)
        errores.append(err)
        codigos.append(p.returncode)
    # Con todos los nodos afuera, al coordinador le queda a lo sumo un plazo
    # para cerrar. No se le espera el tope entero: si los nodos murieron antes
    # de conectarse, espera indefinidamente a propósito (D3.5) y cada corrida
    # fallida costaría cinco minutos. Si hay que matarlo, sale con -9 y la
    # corrida queda marcada como fallida, que es lo que es.
    #
    # El plazo completo solo tiene sentido si todos los nodos salieron en 0:
    # ahí el coordinador está cerrando en orden y puede necesitar su margen.
    # Si alguno falló, la corrida ya está perdida y el coordinador quizá
    # nunca cierre (nodos muertos antes de registrarse), así que esperar el
    # plazo entero solo costaría 35 s por corrida.
    if all(c == 0 for c in codigos):
        gracia = TIMEOUT_MS / 1000.0 + 5.0
    else:
        gracia = 2.0
    try:
        _, cerr = coord.communicate(
            timeout=max(1.0, min(gracia, limite - time.monotonic())))
    except subprocess.TimeoutExpired:
        matar(coord)
        _, cerr = coord.communicate()
        cerr += "\n[experimento.py: el coordinador no cerró %.0f s después " \
                "de que salieron todos los nodos]" % gracia
    errores.append(cerr)

    return {"codigos": {"coordinador": coord.returncode, "nodos": codigos},
            "salidas": salidas, "errores": errores,
            "comando": cmd + ["--id", "<i>"],
            "duracion_s": time.monotonic() - t0}


def nodo_representativo(por_nodo):
    """De qué nodo es `salida`: el nodo 0, salvo que haya un lector.

    En localidad el nodo 0 es el escritor y publica una sola vez, así que sus
    `adquisiciones` y `accesos_locales` no dicen nada del experimento. Lo que
    se mide es el lector, y es el primero con rol=lector. El JSON lo nombra
    en `salida_nodo` para que nadie tenga que adivinarlo."""
    for i, d in enumerate(por_nodo):
        if d.get("rol") == "lector":
            return i
    return 0


def ok(r):
    return r["codigos"]["coordinador"] == 0 and r["codigos"]["nodos"] and \
        all(c == 0 for c in r["codigos"]["nodos"])


# ---- La matriz entera ------------------------------------------------------

def al_terminar(signum, _frame):
    """SIGTERM (el tope global de run_tests.sh, por ejemplo) se vuelve una
    salida normal, para que corran los `finally` que matan a los hijos."""
    raise SystemExit(128 + signum)


def main(argv):
    signal.signal(signal.SIGTERM, al_terminar)
    reps, out_dir, solo, rapido = 5, os.path.join(ROOT, "results"), None, False
    args = argv[1:]
    try:
        while args:
            a = args.pop(0)
            if a == "--reps":
                reps = int(args.pop(0))
                if reps < 1:
                    raise ValueError
            elif a == "--out":
                out_dir = args.pop(0)
            elif a == "--solo":
                solo = set(args.pop(0).split(","))
            elif a == "--rapido":
                rapido = True
            elif a in ("-h", "--help"):
                print(__doc__)
                return 0
            else:
                raise ValueError
    except (IndexError, ValueError):
        print("experimento.py: argumentos inválidos (ver --help)",
              file=sys.stderr)
        return 2

    if not os.access(COORD, os.X_OK):
        print("experimento.py: falta %s; corré make primero." % COORD,
              file=sys.stderr)
        return 2

    exps = matriz(rapido)
    if solo is not None:
        desconocidos = solo - set(exps)
        if desconocidos:
            print("experimento.py: experimentos desconocidos: %s"
                  % ", ".join(sorted(desconocidos)), file=sys.stderr)
            return 2
        exps = {k: v for k, v in exps.items() if k in solo}

    env = entorno()
    run_id = int(time.time()) * 1000
    fallidas = []
    total = sum(len(v) for v in exps.values()) * (reps + 1)
    hechas = 0

    for exp, configs in exps.items():
        d = os.path.join(out_dir, exp)
        dcal = os.path.join(d, "calentamiento")
        os.makedirs(dcal, exist_ok=True)
        for nombre, cfg, app, nodos, paginas, extra in configs:
            # Calentamiento: corre igual que una medida, se guarda como texto
            # y no entra a las estadísticas.
            run_id += 1
            cal_log = os.path.join(dcal, nombre + ".log.txt")
            r = correr(app, nodos, paginas, extra, cal_log, run_id)
            cal = os.path.join(dcal, nombre + ".txt")
            with open(cal, "w", encoding="utf-8") as f:
                f.write("calentamiento de %s/%s: descartado antes de medir\n"
                        "códigos: %s\nduración: %.3f s\n\n"
                        % (exp, nombre, json.dumps(r["codigos"]),
                           r["duracion_s"]))
                for i, s in enumerate(r["salidas"]):
                    f.write("-- nodo %d\n%s" % (i, s))
            hechas += 1

            for rep in range(1, reps + 1):
                run_id += 1
                base = "%s_r%d" % (nombre, rep)
                log = os.path.join(d, base + ".csv")
                inicio = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                r = correr(app, nodos, paginas, extra, log, run_id)
                por_nodo = [pares(s) for s in r["salidas"]]
                nodo_salida = nodo_representativo(por_nodo)
                doc = {
                    "experimento": exp,
                    "config": cfg,
                    "repeticion": rep,
                    "salida": por_nodo[nodo_salida] if por_nodo else {},
                    "salida_nodo": nodo_salida,
                    "salida_por_nodo": por_nodo,
                    "codigos_salida": r["codigos"],
                    "log": base + ".csv",
                    "entorno": env,
                    "comando": r["comando"],
                    "inicio_utc": inicio,
                    "duracion_s": round(r["duracion_s"], 4),
                    "calentamiento_descartado":
                        os.path.relpath(cal, d),
                }
                if not ok(r):
                    doc["errores"] = [e for e in r["errores"] if e]
                    fallidas.append("%s/%s" % (exp, base))
                with open(os.path.join(d, base + ".json"), "w",
                          encoding="utf-8") as f:
                    json.dump(doc, f, ensure_ascii=False, indent=1)
                hechas += 1
                print("[%3d/%d] %-14s %-22s %s  %.2f s"
                      % (hechas, total, exp, base,
                         "ok" if ok(r) else "FALLÓ %s" % r["codigos"],
                         r["duracion_s"]), flush=True)

    if fallidas:
        print("experimento.py: %d corridas fallaron (quedan escritas, el "
              "análisis las excluye con su motivo):" % len(fallidas))
        for f in fallidas:
            print("   " + f)
        return 1
    print("experimento.py: %d corridas medidas, todas terminaron en 0"
          % (total - sum(len(v) for v in exps.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
