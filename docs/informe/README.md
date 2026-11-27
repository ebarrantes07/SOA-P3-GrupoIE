# Informe (fuente LaTeX)

Fuente del informe del proyecto (`docs/informe.pdf`, de 8 a 12 páginas sin
anexos según el enunciado). Formato `IEEEtran`, igual que el Proyecto 2: una
sección por archivo `.tex`, referencias en `referencias.bib`.

| Archivo | Sección |
|---|---|
| `introduccion.tex` | Introducción |
| `arquitectura.tex` | Arquitectura y representación de la memoria |
| `protocolo.tex` | Protocolo de coherencia y sincronización |
| `pruebas.tex` | Pruebas y verificación |
| `resultados.tex` | Evaluación experimental |
| `analisis.tex` | Preguntas de análisis |
| `comparacion.tex` | Relación con la DSM transparente |
| `decisiones.tex` | Decisiones de diseño |
| `limitaciones.tex` | Límites de alcance |
| `conclusiones.tex` | Conclusiones |
| `declaracion_transparencia.tex` | Uso de IA y material reutilizado |
| `entrega.tex` | URL, etiqueta y SHA del commit demostrado |

La gestión del proyecto y la trazabilidad —responsabilidades, issues, pull
requests y revisiones— no van en el informe: están en el README del
repositorio.

Lo que falta escribir aparece en el PDF como **[PENDIENTE — …]** y en el log
de compilación como `PENDIENTE:`. El PDF que se entrega no debe tener ninguno.

## Compilar

```bash
make informe          # desde la raíz del repositorio
```

Imprime la cantidad de páginas y cuántas secciones siguen sin escribir. Pide
`pdflatex` y `bibtex` (TeX Live); no hace falta para construir ni para probar
el proyecto, y por eso el objetivo está fuera del camino de `make test`.

A mano, si se prefiere:

```bash
cd docs/informe
pdflatex -interaction=nonstopmode main.tex
bibtex main
pdflatex -interaction=nonstopmode main.tex
pdflatex -interaction=nonstopmode main.tex
grep -a -c PENDIENTE main.log   # -a: el log no es UTF-8 puro
```

## Figuras

Las tres figuras salen de `scripts/graficas.py`, que las escribe en SVG a
partir del agregado de `scripts/analisis.py`. **Los PDF de `figuras/` se
versionan**, para que compilar el informe no dependa de tener un convertidor
de SVG instalado.

Para regenerarlas hace falta uno. Con Inkscape:

```bash
python3 scripts/analisis.py --json results/<matriz> > agregados.json
python3 scripts/graficas.py --datos agregados.json --out results/figuras
inkscape --export-type=pdf \
         --export-filename=docs/informe/figuras/fig1_localidad.pdf \
         results/figuras/fig1_localidad.svg
```

Una sola fuente de datos y un solo generador: el SVG que se mira en pantalla
y el PDF que entra al informe son el mismo dibujo.
