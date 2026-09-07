# E555 -- Eternity II toolkit. Builds four executables into bin/.
# Needs GCC or Clang with OpenMP, 64-bit POSIX.
#
#   make [all|beamer|finalizer|roundhouse|backtracker|clean]
#   make ARCH=v3 | OPT=-O2 | ARCHFLAGS='-march=skylake-avx512' | CC=clang
#
#   ARCH     native (default)  -march=native                  build host only
#            v3                -march=x86-64-v3 -mtune=native AVX2, Haswell+/Zen+
#            v2                -march=x86-64-v2 -mtune=native SSE4.2, Nehalem+
#            generic           (none)                         any arch
#   OPT      -O3 (default)
#
# Wrong-CPU binaries die with SIGILL. v2/v3 need GCC 11+ or Clang 12+.
# ARCHFLAGS overrides ARCH. Changing any flag forces a rebuild.

ifeq ($(origin CC),default)     # make predefines CC=cc; real overrides still win
CC := gcc
endif

ARCH ?= native
OPT  ?= -O3

ARCHFLAGS_native  := -march=native
ARCHFLAGS_v3      := -march=x86-64-v3 -mtune=native
ARCHFLAGS_v2      := -march=x86-64-v2 -mtune=native
ARCHFLAGS_generic :=

ifeq ($(filter $(ARCH),native v3 v2 generic),)
$(error unknown ARCH '$(ARCH)': use native, v3, v2, generic, or set ARCHFLAGS)
endif
ARCHFLAGS ?= $(ARCHFLAGS_$(ARCH))

CFLAGS = -Wall -Wextra $(OPT) $(ARCHFLAGS) -fopenmp
LDLIBS = -lm

B := src/B_beam
C := src/C_tail

.PHONY: all beamer finalizer roundhouse backtracker clean FORCE
all: beamer finalizer roundhouse backtracker
beamer:      bin/E555_beamer
finalizer:   bin/E555_finalizer
roundhouse:  bin/E555_roundhouse
backtracker: bin/E555_backtracker

bin:
	mkdir -p bin

# Without this, `make ARCH=x` straight after `make` is a silent no-op.
STAMP := bin/.buildflags
$(STAMP): FORCE | bin
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
FORCE:

bin/E555_beamer: $(B)/E555_beamer.c $(B)/E555_database.c $(B)/E555_database.h $(B)/E555_beamer.h $(STAMP) | bin
	$(CC) $(CFLAGS) $(B)/E555_database.c $(B)/E555_beamer.c -o $@ $(LDLIBS)

bin/E555_finalizer: $(B)/E555_finalizer.c $(B)/E555_database.c $(B)/E555_database.h $(B)/E555_beamer.h $(STAMP) | bin
	$(CC) $(CFLAGS) $(B)/E555_database.c $(B)/E555_finalizer.c -o $@ $(LDLIBS)

bin/E555_roundhouse: $(B)/E555_roundhouse.c $(B)/E555_database.c $(B)/E555_database.h $(STAMP) | bin
	$(CC) $(CFLAGS) $(B)/E555_database.c $(B)/E555_roundhouse.c -o $@ $(LDLIBS)

bin/E555_backtracker: $(C)/E555_backtracker.c $(STAMP) | bin
	$(CC) $(CFLAGS) $(C)/E555_backtracker.c -o $@ $(LDLIBS)

clean:
	rm -rf bin
