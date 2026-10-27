# E555_soft -- an experiment, built apart from the repo's Makefile. Needs NLopt.
#
#   make [ARCH=generic|v2|v3|native] [OPT=-O2] [CC=clang] [NLOPT=/prefix]
#   make clean
#
# Flags mirror the repo root Makefile. src/B_beam/E555_database.c and
# src/C_tail/E555_dive.c are compiled, not copied: the same translation units
# bin/E555_diver links. NLOPT names an install prefix when NLopt is not on the
# system paths (libnlopt-dev on Debian/Ubuntu).

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
C := ../../src/C_tail

NLOPT ?=
ifneq ($(NLOPT),)
NLOPT_CFLAGS := -I$(NLOPT)/include
NLOPT_LIBS   := -L$(NLOPT)/lib -L$(NLOPT)/lib64 -Wl,-rpath,$(NLOPT)/lib -Wl,-rpath,$(NLOPT)/lib64
endif

CFLAGS = -Wall -Wextra $(OPT) $(ARCHFLAGS) -fopenmp $(NLOPT_CFLAGS)
LDLIBS = $(NLOPT_LIBS) -lnlopt -lm

.PHONY: all clean
all: bin/E555_soft

bin:
	mkdir -p bin

bin/E555_soft: E555_soft.c $(B)/E555_database.c $(B)/E555_database.h \
               $(C)/E555_dive.c $(C)/E555_dive.h | bin
	$(CC) $(CFLAGS) $(B)/E555_database.c $(C)/E555_dive.c E555_soft.c -o $@ $(LDLIBS)

clean:
	rm -rf bin
