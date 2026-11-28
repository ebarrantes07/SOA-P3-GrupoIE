# Resultados del informe

Las corridas de las que salen todas las cifras, las tablas y las figuras del
informe. Se versionan completas para que el informe se pueda reproducir sin
volver a medir, que es lo que pide el enunciado al exigir que los resultados
individuales se conserven.

```
matriz/<experimento>/                           distribucion, false_sharing, localidad
  <config>_r<n>.json                            lo que imprimió cada nodo
  <config>_r<n>.csv                             el registro del coordinador
  calentamiento/<config>.txt                    la corrida descartada
agregados.json                                  medias, desviaciones y n
```

El agregado vive **fuera** de `matriz/` a propósito: `analisis.py` recorre
todos los `.json` del directorio que se le pase, y si encontrara el agregado
ahí dentro lo informaría como una corrida ilegible —correctamente, porque no
tiene los campos de una corrida— y agregaría un grupo vacío a la salida.

**160 corridas, 16 configuraciones, 10 repeticiones cada una, ninguna
descartada.** Antes de cada configuración se corrió una repetición de
calentamiento que se descarta a propósito y queda conservada aparte: la
primera corrida después de compilar llega a medir la mitad que las siguientes
para el mismo trabajo.

El entorno de cada corrida está dentro de su `.json`, en el campo `entorno`.
Todas comparten el mismo, y `analisis.py` avisa si dos repeticiones de una
configuración no lo compartieran.

## Reproducir el informe desde acá

Sin volver a correr la matriz:

```bash
python3 scripts/analisis.py --json results/seleccionados/matriz > agregados.json
python3 scripts/tablas_informe.py agregados.json docs/informe/tablas
python3 scripts/graficas.py --datos agregados.json --out results/figuras
make informe
```

El `agregados.json` que está acá es el que produjo el primer comando sobre
estas mismas corridas, así que sirve de comprobación: el que se genere tiene
que coincidir.

## Volver a medir

```bash
make
python3 scripts/experimento.py --reps 10 --out results/<nombre>
```

Los números van a ser otros —son tiempos de una máquina concreta— pero las
cantidades de mensajes, páginas e invalidaciones deberían repetirse, porque no
dependen del reloj.

## Qué significa cada métrica

Las de tráfico no las informa ninguna aplicación: se derivan del `.csv` con
`scripts/metricas.py`, que comprueba tres identidades antes de dar un número y
se niega a informar si alguna no cierra. Los bytes incluyen los 32 de cabecera
de cada trama, y la página se cuenta una sola vez por transmisión.
