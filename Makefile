# Makefile for E555 -- an open toolkit for the Eternity II puzzle.
#
# Four executables are built into bin/:
#   bin/E555_beamer      -- Stage B beam search from the bottom border
#   bin/E555_finalizer   -- resume the beam from a partial board
#   bin/E555_roundhouse  -- Stage B strip solver (rotates the board, refills a
#                           W-wide border strip from the chain database)
#   bin/E555_backtracker -- Stage C exact / bounded-mismatch DFS tail solver
#
# The beamer, finalizer and roundhouse share the database module
# (src/B_beam/E555_database.c);
# the backtracker is self-contained. Requires GCC (or Clang) with OpenMP on a
# 64-bit POSIX system; -O3 and -march=native are load-bearing for the multi-GB
# database build and the DFS hot loops.
#
#   make               # build all four
#   make beamer        # bin/E555_beamer only
#   make finalizer     # bin/E555_finalizer only
#   make roundhouse    # bin/E555_roundhouse only
#   make backtracker   # bin/E555_backtracker only
#   make clean         # remove bin/

CC     = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -fopenmp
LDLIBS = -lm

B  := src/B_beam
C  := src/C_tail
DB := $(B)/E555_database.c

.PHONY: all beamer finalizer roundhouse backtracker clean
all: beamer finalizer roundhouse backtracker
beamer:      bin/E555_beamer
finalizer:   bin/E555_finalizer
roundhouse:  bin/E555_roundhouse
backtracker: bin/E555_backtracker

bin:
	mkdir -p bin

bin/E555_beamer: $(DB) $(B)/E555_beamer.c $(B)/E555_database.h $(B)/E555_beamer.h | bin
	$(CC) $(CFLAGS) $(DB) $(B)/E555_beamer.c -o $@ $(LDLIBS)

bin/E555_finalizer: $(DB) $(B)/E555_finalizer.c $(B)/E555_database.h $(B)/E555_beamer.h | bin
	$(CC) $(CFLAGS) $(DB) $(B)/E555_finalizer.c -o $@ $(LDLIBS)

bin/E555_roundhouse: $(DB) $(B)/E555_roundhouse.c $(B)/E555_database.h | bin
	$(CC) $(CFLAGS) $(DB) $(B)/E555_roundhouse.c -o $@ $(LDLIBS)

bin/E555_backtracker: $(C)/E555_backtracker.c | bin
	$(CC) $(CFLAGS) $(C)/E555_backtracker.c -o $@ $(LDLIBS)

clean:
	rm -rf bin
