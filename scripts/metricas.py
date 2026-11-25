#!/usr/bin/env python3
"""metricas.py — las métricas de una corrida, leídas del registro del coordinador (#9).

Lo que esto resuelve es el criterio fino de #9: que las métricas de tráfico
digan si cuentan cabeceras y cargas útiles, y que el tamaño de página se
cuente una sola vez por transmisión. Las dos respuestas, que vienen de
`src/log.c` y no de una suposición:

  - Los bytes INCLUYEN la cabecera. `log_count_sent` y `log_count_recv` suman
    `DSM_HEADER_SIZE + payload_len`, que son 32 bytes más la carga.
  - La página se cuenta UNA VEZ POR TRAMA que la lleva: `carries_page` es
    cierto solo para REL_WRITE y para un ACQ_GRANT cuya carga llega a 4096
    bytes. Un ACQ_GRANT sin datos no cuenta como transferencia.

Las columnas de tráfico del CSV son ACUMULATIVAS, no por fila: el total de una
corrida es el valor de la ÚLTIMA fila. Sumar la columna multiplicaría todo.

ATRIBUCIÓN POR EVENTO. Además del total, acá se reparte el tráfico por tipo de
evento, restando filas consecutivas. Que eso sea legítimo no es obvio y se
comprobó en el código: en `manager.c`, la recepción cuenta la trama antes de
procesar nada (`on_frame`), y cada caso de `on_action` llama a `send_to`
—que cuenta— ANTES de escribir su fila. Así el delta de una fila es el de su
propia trama.

La atribución da algo que las columnas no dan: si un ACQ_GRANT llevó página,
porque su delta es 4136 en vez de 40. El directorio decide eso según la
versión que el nodo declara en caché, y el registro no tiene una columna para
ello.

DOS FILAS QUE VALEN VARIAS TRAMAS. El coordinador escribe UNA fila por barrera
liberada y una por sesión arrancada, pero manda una trama a cada nodo. Por eso
BARRIER_RELEASE vale N×40 y SESSION_START vale N×56. Verificado en cuatro
corridas reales (granularidad en los dos diseños, π y lectores/escritor): con
esas dos reglas, el 100% de los deltas coincide con un tamaño de trama del
protocolo.

Uso:
    metricas.py registro.csv [registro2.csv ...]      resumen legible
    metricas.py --json registro.csv                   un objeto por corrida
"""
import csv
import json
import sys

HEADER_SIZE = 32
PAGE_SIZE = 4096

# Bytes de la trama de cada evento, cabecera incluida. `None` = depende de N.
TRAMA = {
    "ACQ_REQ":          HEADER_SIZE + 8,
    "INVALIDATE":       HEADER_SIZE,
    "INVALIDATE_ACK":   HEADER_SIZE,
    "REL_WRITE":        HEADER_SIZE + PAGE_SIZE,
    "WRITE_PUBLISHED":  HEADER_SIZE + 8,
    "REL_READ":         HEADER_SIZE,
    "READ_RELEASED":    HEADER_SIZE,
    "BARRIER_ARRIVE":   HEADER_SIZE + 8,
    "NODE_JOIN":        HEADER_SIZE + 8,      # el HELLO que se recibió
    "NODE_LEAVE":       HEADER_SIZE,          # el BYE
    "SESSION_END":      0,                    # no manda nada
    "TIMEOUT":          0,
}
# ACQ_GRANT vale 40 sin datos y 4136 con la página.
GRANT_SIN = HEADER_SIZE + 8
GRANT_CON = HEADER_SIZE + 8 + PAGE_SIZE

# Tres grupos: el tráfico del protocolo de coherencia, el de la coordinación
# experimental (las barreras, que #9 pide contabilizar aparte) y el del
# arranque y cierre de la sesión.
COHERENCIA = {"ACQ_REQ", "ACQ_GRANT", "INVALIDATE", "INVALIDATE_ACK",
              "REL_WRITE", "WRITE_PUBLISHED", "REL_READ", "READ_RELEASED"}
COORDINACION = {"BARRIER_ARRIVE", "BARRIER_RELEASE"}
SESION = {"NODE_JOIN", "SESSION_START", "SESSION_END", "NODE_LEAVE"}


def cargar(ruta):
    with open(ruta, newline="", encoding="utf-8") as f:
        filas = list(csv.DictReader(f))
    if not filas:
        raise ValueError("el registro no tiene ninguna fila")
    if "component" not in filas[0] or "bytes_in" not in filas[0]:
        raise ValueError("no parece un registro de este proyecto")
    ajenas = {f["component"] for f in filas} - {"coordinador"}
    if ajenas:
        raise ValueError("no es un registro de coordinador: %s"
                         % ", ".join(sorted(ajenas)))
    return filas


def nodos_de(filas):
    """N de la sesión: una fila NODE_JOIN por nodo registrado."""
    n = sum(1 for f in filas if f["event"] == "NODE_JOIN")
    vistos = {int(f["node_id"]) for f in filas if f["node_id"] != ""}
    return max(n, (max(vistos) + 1) if vistos else 0)


def esperado(evento, delta, n):
    """Bytes que debía valer esa fila, o None si el evento no se modela."""
    if evento == "ACQ_GRANT":
        return GRANT_CON if delta == GRANT_CON else GRANT_SIN
    if evento == "BARRIER_RELEASE":
        return n * (HEADER_SIZE + 8)
    if evento == "SESSION_START":
        return n * (HEADER_SIZE + 24)
    return TRAMA.get(evento)


def metricas(ruta):
    filas = cargar(ruta)
    n = nodos_de(filas)
    ult = filas[-1]

    eventos = {}
    bytes_por_evento = {}
    grupos = {"coherencia": 0, "coordinacion": 0, "sesion": 0, "otro": 0}
    grants_con_pagina = 0
    descuadres = []
    previo = 0

    for f in filas:
        ev = f["event"]
        total = int(f["bytes_in"]) + int(f["bytes_out"])
        delta = total - previo
        previo = total

        eventos[ev] = eventos.get(ev, 0) + 1
        bytes_por_evento[ev] = bytes_por_evento.get(ev, 0) + delta
        if ev == "ACQ_GRANT" and delta == GRANT_CON:
            grants_con_pagina += 1

        esp = esperado(ev, delta, n)
        if esp is not None and delta != esp:
            descuadres.append({"seq": f["seq"], "event": ev,
                               "delta": delta, "esperado": esp})

        if ev in COHERENCIA:
            grupos["coherencia"] += delta
        elif ev in COORDINACION:
            grupos["coordinacion"] += delta
        elif ev in SESION:
            grupos["sesion"] += delta
        else:
            grupos["otro"] += delta

    adq = {"READ": 0, "WRITE": 0}
    for f in filas:
        if f["event"] == "ACQ_REQ" and f["mode"] in adq:
            adq[f["mode"]] += 1

    m = {
        "registro": ruta,
        "run_id": ult["run_id"],
        "nodos": n,
        "filas": len(filas),
        "msgs_in": int(ult["msgs_in"]),
        "msgs_out": int(ult["msgs_out"]),
        "bytes_in": int(ult["bytes_in"]),
        "bytes_out": int(ult["bytes_out"]),
        "bytes_total": int(ult["bytes_in"]) + int(ult["bytes_out"]),
        "page_xfers": int(ult["page_xfers"]),
        "invals": int(ult["invals"]),
        "adquisiciones": adq,
        "grants_con_pagina": grants_con_pagina,
        "grants_sin_pagina": eventos.get("ACQ_GRANT", 0) - grants_con_pagina,
        "eventos": eventos,
        "bytes_por_evento": bytes_por_evento,
        "bytes_por_grupo": grupos,
    }
    m["verificacion"] = verificar(m, filas, descuadres)
    return m


def verificar(m, filas, descuadres):
    """Tres identidades que tienen que cerrar. Si alguna no cierra, la métrica
    se informa igual pero marcada: una corrida con tráfico que no cuadra no se
    descarta en silencio (criterio de #9)."""
    xfers = m["grants_con_pagina"] + m["eventos"].get("REL_WRITE", 0)
    invs = m["eventos"].get("INVALIDATE", 0)
    suma = sum(m["bytes_por_grupo"].values())
    return {
        "deltas_sin_explicar": descuadres,
        "page_xfers_cuadra": xfers == m["page_xfers"],
        "page_xfers_derivado": xfers,
        "invals_cuadra": invs == m["invals"],
        "invals_derivado": invs,
        "bytes_por_grupo_cuadra": suma == m["bytes_total"],
        "todo_cuadra": (not descuadres and xfers == m["page_xfers"]
                        and invs == m["invals"] and suma == m["bytes_total"]),
    }


def imprimir(m):
    v = m["verificacion"]
    print("== %s  (N=%d, %d filas)" % (m["registro"], m["nodos"], m["filas"]))
    print("   mensajes      %d recibidos, %d enviados" % (m["msgs_in"], m["msgs_out"]))
    print("   bytes         %d (cabeceras incluidas): %d entran, %d salen"
          % (m["bytes_total"], m["bytes_in"], m["bytes_out"]))
    print("   páginas       %d transferidas (%d en concesiones, %d en releases)"
          % (m["page_xfers"], m["grants_con_pagina"],
             m["eventos"].get("REL_WRITE", 0)))
    print("   invalidaciones %d" % m["invals"])
    print("   adquisiciones  %d READ, %d WRITE"
          % (m["adquisiciones"]["READ"], m["adquisiciones"]["WRITE"]))
    print("   concesiones    %d con página, %d sin datos (la copia servía)"
          % (m["grants_con_pagina"], m["grants_sin_pagina"]))
    g = m["bytes_por_grupo"]
    print("   por grupo      coherencia %d, coordinación %d, sesión %d"
          % (g["coherencia"], g["coordinacion"], g["sesion"]))
    if v["todo_cuadra"]:
        print("   verificación   todo cuadra")
        return
    # Se nombra la identidad que falló, no solo que algo falló: es lo que
    # permite a e2e_metricas.sh exigir el motivo correcto en cada alteración,
    # como e2e_verificador.sh exige la propiedad correcta.
    print("   verificación   DESCUADRA  deltas_sin_explicar=%d  paginas=%s  "
          "invals=%s  grupos=%s"
          % (len(v["deltas_sin_explicar"]),
             "cuadra" if v["page_xfers_cuadra"] else "MAL",
             "cuadra" if v["invals_cuadra"] else "MAL",
             "cuadra" if v["bytes_por_grupo_cuadra"] else "MAL"))
    for d in v["deltas_sin_explicar"][:5]:
        print("     seq %s %s: %d bytes, se esperaban %d"
              % (d["seq"], d["event"], d["delta"], d["esperado"]))
    if not v["page_xfers_cuadra"]:
        print("     páginas: la columna dice %d y los eventos dan %d"
              % (m["page_xfers"], v["page_xfers_derivado"]))
    if not v["invals_cuadra"]:
        print("     invalidaciones: la columna dice %d y las filas dan %d"
              % (m["invals"], v["invals_derivado"]))


def main(argv):
    como_json = "--json" in argv
    rutas = [a for a in argv[1:] if not a.startswith("--")]
    if not rutas:
        print(__doc__.strip().splitlines()[-3], file=sys.stderr)
        return 2
    salida = []
    malas = 0
    for r in rutas:
        try:
            m = metricas(r)
        except (OSError, ValueError) as e:
            print("%s: NO SE PUDO LEER: %s" % (r, e), file=sys.stderr)
            malas += 1
            continue
        if not m["verificacion"]["todo_cuadra"]:
            malas += 1
        salida.append(m)
        if not como_json:
            imprimir(m)
    if como_json:
        json.dump(salida if len(salida) != 1 else salida[0],
                  sys.stdout, indent=2, ensure_ascii=False)
        print()
    return 1 if malas else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
