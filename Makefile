# Makefile del Proyecto 3 (MC6004). Estructura heredada del Proyecto 2
# (SOA-P2-GrupoIE), ver docs/decisiones_diseno.md (D0.5).
#
# Se completa a medida que se agregan módulos: solo construye los ejecutables
# cuyo main ya existe, así que `make` funciona en cualquier punto del avance.

# make predefine CC = cc, así que `CC ?= gcc` nunca tendría efecto. Se usa
# gcc salvo que el usuario elija otro (make CC=clang o variable de entorno).
ifeq ($(origin CC),default)
CC       := gcc
endif

# _POSIX_C_SOURCE habilita sockets, poll(), getaddrinfo() y las extensiones
# de pthreads que usa el proyecto (pthread_condattr_setclock, entre otras).
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS  := -pthread
LDLIBS   := -lm

# Ejecutables: cada uno tiene su main en src/<nombre>.c. El coordinador y las
# cuatro aplicaciones obligatorias del enunciado.
BINS     := dsm_manager app_counter app_pi app_rw app_granularity
MAINS    := $(addprefix src/,$(addsuffix .c,$(BINS)))

# Módulos comunes: todo src/*.c que no sea un main. Las aplicaciones se
# enlazan contra ellos, que es lo que hace de libdsm una biblioteca propia.
COMMON   := $(filter-out $(MAINS),$(wildcard src/*.c))
OBJDIR   := build
COMMON_O := $(patsubst src/%.c,$(OBJDIR)/%.o,$(COMMON))

# Solo se construyen los ejecutables cuyo main ya existe.
PRESENT  := $(patsubst src/%.c,%,$(wildcard $(MAINS)))

# Pruebas unitarias por módulo: cada tests/test_*.c es su propio binario,
# enlazado contra los mismos módulos comunes. Las de integración de punta a
# punta (#6, #8) lanzan los ejecutables de BINS y viven en scripts/.
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(OBJDIR)/%,$(TEST_SRC))

# Las de integración son scripts ejecutables, no binarios que haya que
# compilar. run_tests.sh los corre igual que a los demás —hace exec de lo que
# le pasen— así que heredan el límite por prueba, el límite global y el matado
# por grupo ante una señal, sin duplicar nada de eso.
#
# Van DESPUÉS de las unitarias: si un módulo está roto, conviene verlo en su
# propia prueba antes de verlo como procesos que no se entienden. Y corren
# igual bajo ASan/UBSan y TSan, porque usan los ejecutables que acabe de
# construir la compilación instrumentada: eso es lo que cumple el criterio de
# #6 de que la suite COMPLETA corra bajo los sanitizers.
E2E_SH   := $(wildcard scripts/e2e_*.sh)
ALL_TESTS := $(TEST_BIN) $(E2E_SH)

# Límites de tiempo de la suite, en segundos. El enunciado pide un límite
# GLOBAL, así que hay dos: TEST_TIMEOUT acota cada binario, para poder decir
# cuál se colgó, y TEST_TOTAL_TIMEOUT acota la corrida completa, que es lo que
# el criterio exige — con N binarios, N veces el límite individual no es un
# límite global. El código 124 de timeout(1) se propaga como fallo en los dos
# niveles. Ajustables: make test TEST_TIMEOUT=60 TEST_TOTAL_TIMEOUT=600
TEST_TIMEOUT       ?= 300
TEST_TOTAL_TIMEOUT ?= 900

# ---- Sanitizers (#8) ---------------------------------------------------
#
# ASan/UBSan y TSan NO se pueden combinar en un mismo binario: son
# compilaciones separadas, como pide el enunciado. TSan analiza carreras
# entre los hilos locales de un proceso; los invariantes distribuidos
# necesitan las pruebas y las trazas propias del protocolo, no un sanitizer.

SANFLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer -g
TSANFLAGS := -fsanitize=thread -fno-omit-frame-pointer -g

# UBSan por defecto imprime y sigue, así que una corrida con comportamiento
# indefinido reportado terminaría en éxito. halt_on_error lo vuelve fatal,
# como ASan ya lo es. detect_leaks va explícito aunque sea el default en
# Linux: el objetivo declara que también busca fugas.
SANENV  := ASAN_OPTIONS=detect_leaks=1 \
           UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
TSANENV := TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1

.PHONY: all test test-san test-tsan test-robustez clean informe

# Sin esto make trata los .o como intermedios de las reglas de patrón y los
# borra al final de cada invocación, así que cada `make test` recompilaría
# todo desde cero. Con dos semanas de trabajo encima, eso se nota.
.SECONDARY: $(COMMON_O) $(patsubst tests/%.c,$(OBJDIR)/%.o,$(TEST_SRC))

all: $(PRESENT)
	@$(if $(PRESENT),true,echo "Aún no hay ejecutables en src/; nada que compilar.")

$(PRESENT): %: $(OBJDIR)/%.o $(COMMON_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/test_%.o: tests/test_%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/test_%: $(OBJDIR)/test_%.o $(COMMON_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR):
	mkdir -p $@

# El ciclo vive en scripts/run_tests.sh para poder envolverlo completo en un
# solo timeout: así el límite es global y no N veces el individual.
test: all $(TEST_BIN)
	@if [ -z "$(ALL_TESTS)" ]; then \
		echo "make test: aún no hay pruebas en tests/."; \
	else \
		timeout $(TEST_TOTAL_TIMEOUT) \
		    scripts/run_tests.sh $(TEST_TIMEOUT) $(ALL_TESTS); \
		rc=$$?; \
		if [ $$rc -eq 124 ]; then \
			echo "make test: TIEMPO GLOBAL AGOTADO tras $(TEST_TOTAL_TIMEOUT)s"; \
			exit 1; \
		fi; \
		exit $$rc; \
	fi

# Limpia antes y después a propósito, en vez de convivir con la compilación
# normal. Los objetos instrumentados comparten $(OBJDIR) y los ejecutables de
# BINS viven en la raíz, así que al quedar más nuevos que los .o normales make
# los daría por actualizados: un `make test` posterior seguiría corriendo los
# binarios instrumentados sin avisar. El clean final deja el árbol como
# estaba, y preserva el código de salida de la suite.
test-san:
	$(MAKE) clean
	$(SANENV) $(MAKE) test \
	    CFLAGS="$(CFLAGS) $(SANFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(SANFLAGS)"; \
	  rc=$$?; $(MAKE) clean; exit $$rc

test-tsan:
	$(MAKE) clean
	$(TSANENV) $(MAKE) test \
	    CFLAGS="$(CFLAGS) $(TSANFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSANFLAGS)"; \
	  rc=$$?; $(MAKE) clean; exit $$rc

# ---- Robustez: la suite repetida con fallos inyectados (#8) ------------
#
# Dos clases de inyección, en pasadas separadas para que un fallo diga cuál la
# provocó:
#
#   - FRAGMENTACION, los siete tamaños del criterio. DSM_IO_CHUNK_MAX recorta
#     cada send() y cada recv() a ese máximo, así que una página de 4096 bytes
#     cruza en miles de pedazos y cualquier supuesto de "llegó entero" se
#     rompe. Los resultados funcionales tienen que ser los mismos.
#   - RETARDO DEL TURNO DE REGISTRO, que ensancha una ventana de concurrencia
#     demasiado angosta para que una prueba la alcance sola. Sin él, el
#     mutante que quita el turno se detecta en 0 de 30 corridas; con él, en
#     20 de 20.
#
# Va aparte de `make test` porque cuesta minutos y no segundos: la
# fragmentación multiplica las llamadas al sistema por mil. Las pruebas que
# mueven volumen bajan su tamaño cuando detectan DSM_IO_CHUNK_MAX, para
# ejercer el CAMINO fragmentado sin multiplicar el trabajo (D5.2).
test-robustez: all $(TEST_BIN)
	@rc=0; \
	for n in 1 2 3 4 5 6 7; do \
		echo "===== fragmentos de $$n byte(s) ====="; \
		DSM_IO_CHUNK_MAX=$$n $(MAKE) --no-print-directory -s test || rc=1; \
	done; \
	echo "===== retardo en la ventana del turno de registro ====="; \
	DSM_LOG_TURN_DELAY_US=20000 timeout $(TEST_TIMEOUT) ./build/test_dsm \
		|| rc=1; \
	if [ $$rc -ne 0 ]; then \
		echo "make test-robustez: alguna pasada falló"; \
	else \
		echo "make test-robustez: las 8 pasadas pasaron"; \
	fi; \
	exit $$rc

# El informe. Fuera del camino de `make test` a propósito: compilarlo pide
# LaTeX, que no hace falta ni para construir el proyecto ni para probarlo.
# Deja el PDF en docs/informe.pdf y dice cuántas secciones siguen sin escribir.
informe:
	@command -v pdflatex >/dev/null || { echo "falta pdflatex; ver docs/informe/README.md" >&2; exit 1; }
	cd docs/informe && pdflatex -interaction=nonstopmode main.tex >/dev/null && bibtex main >/dev/null && pdflatex -interaction=nonstopmode main.tex >/dev/null && pdflatex -interaction=nonstopmode main.tex >/dev/null
	@cp docs/informe/main.pdf docs/informe.pdf
	@grep -a "Output written" docs/informe/main.log
	@echo "secciones sin escribir: $$(grep -a -c 'PENDIENTE:' docs/informe/main.log)"

clean:
	rm -rf $(OBJDIR) $(BINS)

-include $(wildcard $(OBJDIR)/*.d)
