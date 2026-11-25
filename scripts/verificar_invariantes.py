#!/usr/bin/env python3
"""Comprueba los invariantes del protocolo leyendo el registro del coordinador.

Uso:
    scripts/verificar_invariantes.py registro.csv [registro2.csv ...]

Codigo de salida: 0 si todos los registros cumplen, 1 si alguno viola algo,
2 si un archivo no se puede leer o no es un registro de coordinador.

POR QUE EXISTE, Y POR QUE NO LEE LAS COLUMNAS DE ESTADO
=======================================================

Todo lo que las pruebas comprueban es codigo nuestro verificando codigo
nuestro. Si el directorio tuviera un defecto y las pruebas compartieran el
mismo supuesto equivocado, las dos estarian de acuerdo y nadie se enteraria.
Este script existe para romper esa circularidad: lee el registro y comprueba
las propiedades desde afuera.

Eso impone una regla que es la decision central del script: **reconstruye su
propio modelo del estado a partir de los EVENTOS, y no lee las columnas de
estado del directorio** (writer, readers, readers_mask, inval_pending). Esas
columnas son la opinion del directorio sobre si mismo: si concediera dos
escritores a la vez, tambien las escribiria mal, de forma coherente con su
propio defecto. Un verificador que las leyera estaria repitiendo esa opinion
en vez de comprobarla.

Lo que si hace, al final, es COMPARAR su modelo contra esas columnas y avisar
si difieren. Un desacuerdo significa que uno de los dos esta mal, y eso
tambien vale saberlo.

LAS CINCO PROPIEDADES (criterio de #8)
======================================

  P1  un solo escritor activo por pagina
  P2  mientras hay escritor, ningun lector y ninguna copia valida ajena
  P3  el grant de WRITE llega despues de TODOS los ACK que su ronda requeria
  P4  la version de una pagina nunca retrocede
  P5  un ACK repetido no cuenta dos veces para cerrar una ronda
"""

import csv
import sys

# Columnas que el script necesita. Se buscan POR NOMBRE en el encabezado, no
# por posicion: agregar una columna al registro no deberia romper esto.
REQUERIDAS = ("component", "seq", "node_id", "request_id", "page_id", "event",
              "status", "mode", "version")

# Columnas de estado del directorio. NO se usan para verificar; solo para el
# contraste final (ver el encabezado del archivo).
ESTADO = ("writer", "readers", "inval_pending")


class Violacion(Exception):
    """Un invariante que no se cumple, con la fila que lo delata."""

    def __init__(self, prop, fila, detalle):
        super().__init__(detalle)
        self.prop = prop
        self.fila = fila
        self.detalle = detalle


class Pagina:
    """El modelo PROPIO del script sobre una pagina.

    `copias` son los nodos cuya copia retenida sigue vigente, sin permiso.
    Un nodo con permiso esta en `lectores` o es `escritor`, no en `copias`:
    son estados distintos y mezclarlos haria que P2 no distinguiera "un
    lector activo" de "una copia que quedo guardada".
    """

    def __init__(self):
        self.escritor = None
        self.lectores = set()
        self.copias = set()
        self.version = None          # None = todavia no se publico nada
        self.pendientes = set()      # nodos a los que se pidio invalidar
        self.ronda = None            # request_id de la ronda abierta
        self.acks_repetidos = 0


class Verificador:
    def __init__(self, nombre):
        self.nombre = nombre
        self.paginas = {}
        self.vivos = set()
        self.violaciones = []
        self.desacuerdos = []
        self.observaciones = {"acks_repetidos": 0, "nodos_perdidos": 0}
        self.filas = 0

    def pagina(self, pid):
        if pid not in self.paginas:
            self.paginas[pid] = Pagina()
        return self.paginas[pid]

    # ---- Invariantes ---------------------------------------------------
    #
    # Se comprueban DESPUES de aplicar cada evento, sobre el modelo propio.
    # Hacerlo despues y no solo en el grant es lo que convierte P1 y P2 en
    # invariantes de verdad: tienen que valer en todo momento, no solo en el
    # instante en que se concede algo.

    def comprobar(self, pid, fila):
        p = self.paginas[pid]

        if isinstance(p.escritor, set):          # defensivo, nunca deberia
            raise Violacion("P1", fila, "escritor multiple")

        if p.escritor is not None:
            # P2: con escritor no puede haber lectores ni copias ajenas.
            if p.lectores:
                raise Violacion(
                    "P2", fila,
                    f"la pagina {pid} tiene escritor {p.escritor} y ademas "
                    f"lectores {sorted(p.lectores)}")
            ajenas = p.copias - {p.escritor}
            if ajenas:
                raise Violacion(
                    "P2", fila,
                    f"la pagina {pid} tiene escritor {p.escritor} y copias "
                    f"validas ajenas en {sorted(ajenas)}")

    # ---- Transiciones --------------------------------------------------

    def aplicar(self, fila):
        ev = fila["event"]
        nodo = fila["node_id"]
        pid = fila["page_id"]

        if ev == "NODE_JOIN":
            if nodo is not None:
                self.vivos.add(nodo)
            return
        if ev == "NODE_LEAVE":
            # Un nodo que se va deja de contar en todas las paginas: si no, su
            # copia retenida seguiria contando como "copia valida ajena" para
            # siempre y P2 fallaria por un nodo que ya no existe.
            if nodo is not None:
                self.vivos.discard(nodo)
                self.observaciones["nodos_perdidos"] += 1
                for p in self.paginas.values():
                    if p.escritor == nodo:
                        p.escritor = None
                    p.lectores.discard(nodo)
                    p.copias.discard(nodo)
                    p.pendientes.discard(nodo)
            return

        if pid is None or nodo is None:
            return        # barreras, sesion, errores: no tocan una pagina

        p = self.pagina(pid)

        if ev == "ACQ_GRANT":
            if fila["mode"] == "WRITE":
                # P1 antes de aplicar: si ya hay escritor, conceder a otro es
                # la violacion, y hay que delatarla antes de pisarla.
                if p.escritor is not None and p.escritor != nodo:
                    raise Violacion(
                        "P1", fila,
                        f"se concede WRITE de la pagina {pid} al nodo {nodo} "
                        f"con el nodo {p.escritor} ya escribiendo")
                # P3: la ronda de invalidacion tiene que estar cerrada.
                if p.pendientes:
                    raise Violacion(
                        "P3", fila,
                        f"se concede WRITE de la pagina {pid} al nodo {nodo} "
                        f"con ACK pendientes de {sorted(p.pendientes)}")
                p.escritor = nodo
                p.copias.discard(nodo)
                p.lectores.discard(nodo)
            else:
                if p.escritor is not None:
                    raise Violacion(
                        "P2", fila,
                        f"se concede READ de la pagina {pid} al nodo {nodo} "
                        f"con el nodo {p.escritor} escribiendo")
                p.lectores.add(nodo)
                p.copias.discard(nodo)
            self.version_de(p, fila, estricta=False)

        elif ev == "WRITE_PUBLISHED":
            if p.escritor != nodo:
                raise Violacion(
                    "P1", fila,
                    f"el nodo {nodo} publica la pagina {pid} sin ser el "
                    f"escritor (lo es {p.escritor})")
            p.escritor = None
            p.copias.add(nodo)        # el escritor conserva su copia
            self.version_de(p, fila, estricta=True)

        elif ev == "READ_RELEASED":
            p.lectores.discard(nodo)
            p.copias.add(nodo)        # queda retenida, sin permiso

        elif ev == "INVALIDATE":
            p.pendientes.add(nodo)
            p.ronda = fila["request_id"]

        elif ev == "INVALIDATE_ACK":
            if nodo in p.pendientes:
                p.pendientes.discard(nodo)
                p.copias.discard(nodo)
            else:
                # P5: un ACK que nadie pidio, o repetido, NO cierra la ronda.
                # El modelo simplemente no lo cuenta, y si el grant llegara
                # igual con pendientes abiertos, P3 lo delata. Se registra
                # para poder decir cuantos hubo.
                p.acks_repetidos += 1
                self.observaciones["acks_repetidos"] += 1

        self.comprobar(pid, fila)

    # ---- P4: la version nunca retrocede --------------------------------

    def version_de(self, p, fila, estricta):
        v = fila["version"]
        if v is None:
            return
        if p.version is None:
            p.version = v
            return
        if v < p.version or (estricta and v <= p.version):
            cmp = "no crecio" if estricta else "retrocedio"
            raise Violacion(
                "P4", fila,
                f"la version de la pagina {fila['page_id']} {cmp}: "
                f"{p.version} -> {v}")
        p.version = v

    # ---- Contraste con las columnas del directorio ---------------------

    def contrastar(self, fila):
        """Compara el modelo propio con lo que el directorio dice de si mismo.

        No verifica: avisa. Un desacuerdo no dice cual de los dos esta mal,
        pero decirlo es mas util que callarlo.
        """
        pid = fila["page_id"]
        if pid is None or pid not in self.paginas:
            return
        p = self.paginas[pid]
        w = fila.get("writer")
        if w is not None:
            esperado = -1 if p.escritor is None else p.escritor
            if w != esperado:
                self.desacuerdos.append(
                    (fila["seq"], f"writer: el registro dice {w}, el modelo "
                                  f"{esperado}"))
        r = fila.get("readers")
        if r is not None and r != len(p.lectores):
            self.desacuerdos.append(
                (fila["seq"], f"readers: el registro dice {r}, el modelo "
                              f"{len(p.lectores)}"))


# ---- Lectura del registro ----------------------------------------------

def entero(s):
    """Celda a entero, o None si esta vacia. Una celda vacia significa 'no
    aplica' en este esquema (D3.1), no cero."""
    s = s.strip()
    if s == "":
        return None
    try:
        return int(s)
    except ValueError:
        return None


def leer(path):
    """Devuelve (filas, error). Cada fila es un dict con los campos ya
    convertidos; `error` es un texto si el archivo no sirve."""
    try:
        with open(path, newline="", encoding="utf-8") as f:
            r = csv.reader(f)
            try:
                cab = next(r)
            except StopIteration:
                return None, "el archivo esta vacio"
            idx = {name: i for i, name in enumerate(c.strip() for c in cab)}
            faltan = [c for c in REQUERIDAS if c not in idx]
            if faltan:
                return None, f"al encabezado le faltan columnas: {faltan}"

            filas = []
            for n, raw in enumerate(r, start=2):
                if len(raw) < len(cab):
                    return None, f"la linea {n} tiene menos columnas que el encabezado"
                fila = {"linea": n}
                for c in REQUERIDAS + ESTADO:
                    if c not in idx:
                        continue
                    celda = raw[idx[c]]
                    fila[c] = celda.strip() if c in ("component", "event",
                                                     "mode", "status") \
                        else entero(celda)
                filas.append(fila)
            return filas, None
    except OSError as e:
        return None, f"no se pudo leer: {e}"


def verificar(path):
    """Devuelve (ok, Verificador | None, error)."""
    filas, err = leer(path)
    if err is not None:
        return False, None, err

    comp = {f["component"] for f in filas if f["component"]}
    if comp and "coordinador" not in comp:
        return False, None, (f"esto no es el registro del coordinador "
                             f"(component = {sorted(comp)}); los invariantes "
                             f"de pagina solo se pueden ver desde ahi")

    v = Verificador(path)
    for fila in filas:
        v.filas += 1
        try:
            v.aplicar(fila)
        except Violacion as viol:
            v.violaciones.append(viol)
            break        # el estado ya no es confiable: seguir inventaria
        v.contrastar(fila)
    return not v.violaciones, v, None


def informar(path, ok, v, err):
    print(f"== {path}")
    if err is not None:
        print(f"   NO SE PUDO VERIFICAR: {err}")
        return
    print(f"   {v.filas} filas, {len(v.paginas)} paginas con actividad")

    if ok:
        for prop, texto in (
                ("P1", "un solo escritor activo por pagina"),
                ("P2", "sin lectores ni copias ajenas mientras hay escritor"),
                ("P3", "el grant de WRITE despues de todos los ACK"),
                ("P4", "la version nunca retrocede"),
                ("P5", "un ACK repetido no cierra una ronda")):
            print(f"   OK  {prop}  {texto}")
    else:
        for viol in v.violaciones:
            print(f"   VIOLA {viol.prop} (linea {viol.fila['linea']}, "
                  f"seq {viol.fila['seq']}, evento {viol.fila['event']})")
            print(f"         {viol.detalle}")

    if v.observaciones["acks_repetidos"]:
        print(f"   nota: {v.observaciones['acks_repetidos']} ACK no pedidos o "
              f"repetidos, ninguno contado para cerrar su ronda")
    if v.observaciones["nodos_perdidos"]:
        print(f"   nota: {v.observaciones['nodos_perdidos']} nodos salieron "
              f"de la sesion")
    if v.desacuerdos:
        print(f"   DESACUERDO con las columnas del directorio "
              f"({len(v.desacuerdos)}); las primeras:")
        for seq, texto in v.desacuerdos[:5]:
            print(f"         seq {seq}: {texto}")


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2

    peor = 0
    for path in argv[1:]:
        ok, v, err = verificar(path)
        informar(path, ok, v, err)
        if err is not None:
            peor = max(peor, 2)
        elif not ok or v.desacuerdos:
            peor = max(peor, 1)
    return peor


if __name__ == "__main__":
    sys.exit(main(sys.argv))
