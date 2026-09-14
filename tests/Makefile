# tests/Makefile -- builds the fixed-frame experiment's beamer, and nothing else.
#
#   make -C tests              # -> bin/E555_beamer_FixedFrame
#   make -C tests ARCH=v3      # for a mixed x86-64 cluster
#   make -C tests clean
#
# Deliberately separate from the repository's root Makefile. The experiment lives
# entirely under tests/ and must not require an edit anywhere else, so it carries
# its own build rather than adding a target to the production one. It links the
# UNMODIFIED src/B_beam/E555_database.c -- only the beamer itself is a copy.
#
# Flag conventions mirror the root Makefile so a binary built here behaves like
# the production ones: wrong-CPU binaries die with SIGILL, v2/v3 need GCC 11+ or
# Clang 12+, and changing a flag forces a rebuild.

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

CFLAGS = -Wall -Wextra $(OPT) $(ARCHFLAGS) -fopenmp
LDLIBS = -lm

ROOT := ..
B    := $(ROOT)/src/B_beam
BIN  := $(ROOT)/bin

.PHONY: all clean FORCE
all: $(BIN)/E555_beamer_FixedFrame

$(BIN):
	mkdir -p $(BIN)

# Same trick the root Makefile uses: without it, `make ARCH=x` straight after a
# plain `make` is a silent no-op.
STAMP := $(BIN)/.buildflags_fixedframe
$(STAMP): FORCE | $(BIN)
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
FORCE:

# -I$(B) so the copy here finds E555_beamer.h where it actually lives, rather
# than carrying a ../ include path that breaks if either file moves.
$(BIN)/E555_beamer_FixedFrame: E555_beamer_FixedFrame.c \
                               $(B)/E555_database.c $(B)/E555_database.h \
                               $(B)/E555_beamer.h $(STAMP) | $(BIN)
	$(CC) $(CFLAGS) -I$(B) $(B)/E555_database.c E555_beamer_FixedFrame.c \
	    -o $@ $(LDLIBS)

clean:
	rm -f $(BIN)/E555_beamer_FixedFrame $(STAMP)
