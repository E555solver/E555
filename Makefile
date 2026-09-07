# Makefile for E555 -- an open toolkit for the Eternity II puzzle.
#
# Four executables are built into bin/:
#   bin/E555_beamer      -- Stage B beam search from the bottom border
#   bin/E555_finalizer   -- resume the beam from a partial board
#   bin/E555_roundhouse  -- Stage B strip solver (rotates the board, refills a
#                           W-wide border strip from the chain database)
#   bin/E555_backtracker -- Stage C exact / bounded-mismatch DFS tail solver
#
# The beamer, finalizer and roundhouse share one source module
# (src/B_beam/E555_database.c); the backtracker is self-contained. Requires GCC
# (or Clang) with OpenMP on a 64-bit POSIX system.
#
#   make                 # build all four, tuned for THIS machine
#   make beamer          # one tool only: beamer finalizer roundhouse backtracker
#   make ARCH=v3         # one binary for a cluster of mixed modern x86-64
#   make ARCH=generic    # runs anywhere; what CI and containers want
#   make OPT=-O0         # fast compile, slow binary, for a quick syntax check
#   make clean           # remove bin/
#
# ---------------------------------------------------------------------------
# ARCH -- what the binary is allowed to assume about the CPU that runs it
# ---------------------------------------------------------------------------
#
#   native   (default)  -march=native. Fastest, and valid ONLY on the machine
#                       that compiled it. A binary copied elsewhere -- or run
#                       after a cloud VM migrates to different silicon -- dies
#                       with SIGILL, "Illegal instruction", usually with no
#                       other clue as to why. If that happens, rebuild, or use
#                       one of the levels below.
#   v3                  -march=x86-64-v3 -mtune=native. AVX2, BMI2, FMA: Intel
#                       Haswell (2013) and later, AMD Zen and later. THE
#                       SETTING FOR A CLUSTER of mixed but reasonably modern
#                       Intel CPUs -- one binary every node can run, still
#                       instruction-scheduled for the node that built it.
#   v2                  -march=x86-64-v2 -mtune=native. SSE4.2 and POPCNT:
#                       Intel Nehalem (2008) and later. Use when some node in
#                       the cluster predates Haswell.
#   generic             No -march at all. Any x86-64, and any architecture at
#                       all -- Apple Silicon included. The right choice for CI,
#                       containers and cloud sandboxes, none of which own the
#                       CPU their output will eventually run on.
#
# Pick the LOWEST level every machine in the pool supports; `lscpu` lists the
# flags, and avx2 is the one that separates v3 from v2. Expect a few percent,
# not a few times: -O3 does most of the work here, and -march mainly decides
# whether the binary starts at all.
#
# v2 and v3 need GCC 11+ or Clang 12+. On an older toolchain, or for a CPU
# worth naming exactly, set the flags directly and ignore ARCH:
#
#   make ARCHFLAGS='-march=skylake-avx512'
#
# OPT is separate from ARCH so the two trade independently. -O3 is the default
# and is load-bearing: the multi-GB database build and the DFS hot loops are
# what this project spends its time in.

# Make defines CC itself, so `CC ?= gcc` would be a silent no-op and build with
# `cc`. Overriding only the built-in default leaves `make CC=clang` and an
# exported CC working as expected.
ifeq ($(origin CC),default)
CC := gcc
endif

ARCH ?= native
OPT  ?= -O3

ARCHFLAGS_native  := -march=native
ARCHFLAGS_v3      := -march=x86-64-v3 -mtune=native
ARCHFLAGS_v2      := -march=x86-64-v2 -mtune=native
ARCHFLAGS_generic :=

# A typo must not quietly build `generic`: that is a slower binary nobody asked
# for, and the mistake would only ever surface as a benchmark that got worse.
ifeq ($(filter $(ARCH),native v3 v2 generic),)
$(error unknown ARCH '$(ARCH)' -- use native, v3, v2 or generic, \
        or set ARCHFLAGS directly)
endif
ARCHFLAGS ?= $(ARCHFLAGS_$(ARCH))

CFLAGS = -Wall -Wextra $(OPT) $(ARCHFLAGS) -fopenmp
LDLIBS = -lm

B := src/B_beam
C := src/C_tail

# The three Stage B tools share one compilation unit. This is a SOURCE file --
# it is not the 6.5 GB chain database the beamer builds at runtime and caches
# with --db_file, and the two have nothing to do with each other. The old name
# for this variable was `DB`, which suggested otherwise.
SHARED_SRC := $(B)/E555_database.c
SHARED_HDR := $(B)/E555_database.h $(B)/E555_beamer.h

STAGE_B := beamer finalizer roundhouse

.PHONY: all beamer finalizer roundhouse backtracker clean FORCE
all: beamer finalizer roundhouse backtracker
beamer:      bin/E555_beamer
finalizer:   bin/E555_finalizer
roundhouse:  bin/E555_roundhouse
backtracker: bin/E555_backtracker

bin:
	mkdir -p bin

# Changing ARCH or OPT has to force a rebuild. Without this, `make ARCH=generic`
# straight after a plain `make` finds the binaries up to date, does nothing, and
# leaves the native ones in place -- which is precisely the failure ARCH exists
# to prevent, now wearing a clean build log. The stamp holds the flags actually
# used and is only rewritten when they change, so an unchanged setting still
# rebuilds nothing.
FLAGSTAMP := bin/.buildflags
$(FLAGSTAMP): FORCE | bin
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || { echo '$(CC) $(CFLAGS)' > $@; \
	    echo "[make] build flags: $(CC) $(CFLAGS)"; }
FORCE:

# One rule for the three Stage B tools: they differ only in which .c is compiled
# alongside the shared module, and three copies of one command line is how the
# copies drift apart.
$(addprefix bin/E555_,$(STAGE_B)): bin/E555_%: $(B)/E555_%.c $(SHARED_SRC) $(SHARED_HDR) $(FLAGSTAMP) | bin
	$(CC) $(CFLAGS) $(SHARED_SRC) $< -o $@ $(LDLIBS)

bin/E555_backtracker: $(C)/E555_backtracker.c $(FLAGSTAMP) | bin
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf bin
