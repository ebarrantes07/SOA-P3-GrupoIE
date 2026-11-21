#!/usr/bin/env python3
"""Altera a mano un registro del coordinador, para probar el verificador.

Uso:
    scripts/mutar_registro.py <mutacion> entrada.csv salida.csv
    scripts/mutar_registro.py --lista

Existe por el criterio de #8: "el verificador falla a proposito contra un log
alterado a mano, para demostrar que realmente verifica". Un verificador que
nunca vio un registro malo no es un verificador, es un lector de archivos.

Cada mutacion esta nombrada por la propiedad que debe romper, y altera lo
MINIMO: una fila cambiada, agregada o quitada. Despues se renumera la columna
`seq` para que quede consecutiva, asi la unica anomalia del archivo es la
semantica que se quiso introducir y no un salto en la numeracion que el
verificador podria delatar por otro motivo.
"""

import csv
import sys

MUTACIONES = {
    "dos_escritores":
        "P1: concede WRITE a otro nodo mientras el primero todavia escribe",
    "publica_ajeno":
        "P1: un nodo publica una pagina de la que no es el escritor",
    "lector_con_escritor":
        "P2: concede READ mientras hay un escritor activo",
    "grant_sin_ack":
        "P3: borra un INVALIDATE_ACK, asi el grant de WRITE llega con la "
        "ronda abierta",
    "version_retrocede":
        "P4: baja la version de una publicacion",
    "ack_repetido_cierra":
        "P5: agrega un segundo INVALIDATE y repite el ACK del primero, asi la "
        "ronda solo cerraria contando el repetido dos veces",
}


def cargar(path):
    with open(path, newline="", encoding="utf-8") as f:
        filas = list(csv.reader(f))
    if not filas:
        sys.exit("el registro esta vacio")
    cab = [c.strip() for c in filas[0]]
    idx = {name: i for i, name in enumerate(cab)}
    for c in ("seq", "node_id", "page_id", "event", "mode", "version"):
        if c not in idx:
            sys.exit(f"al encabezado le falta la columna {c}")
    return filas[0], filas[1:], idx


def buscar(cuerpo, idx, event, mode=None, desde=0):
    """Indice de la primera fila con ese evento (y modo), o None."""
    for i in range(desde, len(cuerpo)):
        f = cuerpo[i]
        if f[idx["event"]].strip() != event:
            continue
        if mode is not None and f[idx["mode"]].strip() != mode:
            continue
        return i
    return None


def mutar(nombre, cuerpo, idx):
    """Aplica la mutacion sobre `cuerpo` (lista de filas). Devuelve un texto
    que describe lo que cambio, para que el informe lo diga."""

    if nombre == "dos_escritores":
        i = buscar(cuerpo, idx, "ACQ_GRANT", "WRITE")
        if i is None:
            sys.exit("el registro no tiene ningun grant de WRITE")
        dueno = cuerpo[i][idx["node_id"]].strip()
        otro = "1" if dueno != "1" else "0"
        fila = list(cuerpo[i])
        fila[idx["node_id"]] = otro
        cuerpo.insert(i + 1, fila)
        return (f"se duplico el grant de WRITE de la fila {i + 2} para el "
                f"nodo {otro}, sin que el nodo {dueno} liberara")

    if nombre == "publica_ajeno":
        i = buscar(cuerpo, idx, "WRITE_PUBLISHED")
        if i is None:
            sys.exit("el registro no tiene ninguna publicacion")
        dueno = cuerpo[i][idx["node_id"]].strip()
        cuerpo[i][idx["node_id"]] = "1" if dueno != "1" else "0"
        return (f"la publicacion de la fila {i + 2} pasa del nodo {dueno} al "
                f"nodo {cuerpo[i][idx['node_id']]}")

    if nombre == "lector_con_escritor":
        i = buscar(cuerpo, idx, "ACQ_GRANT", "WRITE")
        if i is None:
            sys.exit("el registro no tiene ningun grant de WRITE")
        dueno = cuerpo[i][idx["node_id"]].strip()
        otro = "1" if dueno != "1" else "0"
        fila = list(cuerpo[i])
        fila[idx["node_id"]] = otro
        fila[idx["mode"]] = "READ"
        cuerpo.insert(i + 1, fila)
        return (f"se agrego un grant de READ al nodo {otro} justo despues del "
                f"grant de WRITE al nodo {dueno}")

    if nombre == "grant_sin_ack":
        i = buscar(cuerpo, idx, "INVALIDATE_ACK")
        if i is None:
            sys.exit("el registro no tiene ningun ACK de invalidacion")
        quitada = cuerpo.pop(i)
        return (f"se borro el ACK de la fila {i + 2}, del nodo "
                f"{quitada[idx['node_id']].strip()}")

    if nombre == "version_retrocede":
        i = buscar(cuerpo, idx, "WRITE_PUBLISHED")
        # La primera publicacion sube 0 -> 1; para que BAJE hace falta una
        # posterior, asi que se busca la segunda.
        j = buscar(cuerpo, idx, "WRITE_PUBLISHED", desde=(i or 0) + 1)
        if j is None:
            sys.exit("el registro necesita dos publicaciones para esta mutacion")
        antes = cuerpo[j][idx["version"]].strip()
        cuerpo[j][idx["version"]] = "0"
        return f"la version de la fila {j + 2} baja de {antes} a 0"

    if nombre == "ack_repetido_cierra":
        i = buscar(cuerpo, idx, "INVALIDATE")
        if i is None:
            sys.exit("el registro no tiene ninguna invalidacion")
        ack = buscar(cuerpo, idx, "INVALIDATE_ACK", desde=i)
        if ack is None:
            sys.exit("el registro no tiene ningun ACK despues de la invalidacion")
        dueno = cuerpo[i][idx["node_id"]].strip()
        otro = "1" if dueno != "1" else "0"
        # Un segundo nodo invalidado...
        inval2 = list(cuerpo[i])
        inval2[idx["node_id"]] = otro
        cuerpo.insert(i + 1, inval2)
        # ...y en lugar de su ACK, el del primero otra vez.
        repetido = list(cuerpo[ack + 1])
        cuerpo.insert(ack + 2, repetido)
        return (f"se invalido tambien al nodo {otro} y se repitio el ACK del "
                f"nodo {dueno} en su lugar")

    sys.exit(f"mutacion desconocida: {nombre}")


def main(argv):
    if len(argv) == 2 and argv[1] == "--lista":
        for n, d in MUTACIONES.items():
            print(f"{n}\t{d}")
        return 0
    if len(argv) != 4:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2

    nombre, entrada, salida = argv[1], argv[2], argv[3]
    if nombre not in MUTACIONES:
        print(f"mutacion desconocida: {nombre}; probá --lista", file=sys.stderr)
        return 2

    cab, cuerpo, idx = cargar(entrada)
    detalle = mutar(nombre, cuerpo, idx)

    # Se renumera `seq` para que la unica anomalia sea la semantica.
    for n, fila in enumerate(cuerpo, start=1):
        if fila[idx["seq"]].strip() != "":
            fila[idx["seq"]] = str(n)

    with open(salida, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cab)
        w.writerows(cuerpo)
    print(f"{nombre}: {detalle}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
