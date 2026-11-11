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

.PHONY: all test test-san test-tsan clean

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
	@if [ -z "$(TEST_BIN)" ]; then \
		echo "make test: aún no hay pruebas en tests/."; \
	else \
		timeout $(TEST_TOTAL_TIMEOUT) \
		    scripts/run_tests.sh $(TEST_TIMEOUT) $(TEST_BIN); \
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

clean:
	rm -rf $(OBJDIR) $(BINS)

-include $(wildcard $(OBJDIR)/*.d)
