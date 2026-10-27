# E555 data-driven beamer -- an experiment, built apart from the repo's Makefile.
#
#   make [ARCH=generic|v2|v3|native] [OPT=-O2] [CC=clang]
#   make clean
#
# Flags mirror the repo root Makefile exactly, so this binary is the same build
# as bin/E555_beamer but for the one source file that differs. ARCH=native is
# the default there and here; ARCH=generic is what a container wants.
#
# src/B_beam/E555_database.c is COMPILED, not copied and not modified: this tool
# links the same translation unit bin/E555_beamer and bin/E555_finalizer link.
# -I puts its headers on the include path, since our .c does not sit beside them.

ifeq ($(origin CC),default)
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

B := ../../src/B_beam

CFLAGS = -Wall -Wextra $(OPT) $(ARCHFLAGS) -fopenmp -I$(B)
LDLIBS = -lm

.PHONY: all clean
all: bin/E555_beamer_datadriven

bin:
	mkdir -p bin

bin/E555_beamer_datadriven: E555_beamer_datadriven.c $(B)/E555_database.c \
                            $(B)/E555_database.h $(B)/E555_beamer.h | bin
	$(CC) $(CFLAGS) $(B)/E555_database.c E555_beamer_datadriven.c -o $@ $(LDLIBS)

clean:
	rm -rf bin
