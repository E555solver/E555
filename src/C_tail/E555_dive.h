/*
 * E555_dive.h -- the end-dive engine: finish partial boards with greedy random
 * dives that allow broken edges, learn from the best of them, and polish the
 * winners by local search.
 *
 * Shared by bin/E555_beamer (--end_dive) and any tool that wants to finish
 * boards the same way. The engine owns no command line: every option is parsed
 * by the front-end's main() and handed over in a DvParams.
 *
 * LAYERS (E555_dive.c keeps one section per layer)
 *   problem     orientations, fit masks, the nine frame classes of a border
 *   dive        forward checking and one stuck-mode dive (MRV cell, exact fit
 *               before a break, minimal-break class, least-constraining value)
 *   learning    cross-entropy weights w(piece, cell) learned from elite dives
 *   local search  re-rotation / swap moves, hill-climb polish, kick-and-polish walks
 *   batch       the queued boards of one configuration run through the stages
 *               below as parallel jobs
 *   seeders     corner seeding: extra copies of a board with an alive top-corner
 *               block placed (--lambda_corners catalog, E555_database.c tc_*)
 *   reporting   run totals for the summary
 *
 * STAGES (per batch)
 *   stage 1  M/10 plain dives per board; its best is kept
 *   stage 2  boards whose stage-1 best is >= S-4 (or the top 10% when that is
 *            under 20% of the batch) get the other M - M/10 dives, in 18
 *            cross-entropy rounds learning from the board's own best dives
 *   polish   (R >= 0) boards within 6 of S: their 16 best distinct dives are
 *            hill-climbed, then R kick-and-polish rounds run over 8 walks
 *   keep     boards whose best is >= S, written by dv_flush sorted by score
 *
 * Every dive's random stream is keyed by its board and its index, so the result
 * does not depend on the thread count.
 */
#ifndef E555_DIVE_H
#define E555_DIVE_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../B_beam/E555_database.h"

#define DV_EMPTY  0xFFFFu          /* an open cell in a queued board */
#define DV_EDGES  480              /* matched edges of a solved board */

typedef struct {
    uint32_t dives;            /* M: dives per board (stage 1 + stage 2) */
    int      emit_score;       /* S: a finished board is kept when its best >= S */
    int      polish;           /* R: kick-and-polish rounds; < 0 = no polish */
    int      corner_seeds;     /* seeded copies per board with an alive corner block */
    bool     seed_corners;     /* the tc_* catalog is built and seeding may use it */
    uint64_t master_seed;      /* keys every board's random streams */
    uint64_t max_written;      /* cap on boards written by dv_flush; 0 = none */
    double   deadline;         /* absolute omp_get_wtime(); 0 = none */
    int      threads;
    volatile sig_atomic_t *stop;   /* raised by a signal handler; may be NULL */
} DvParams;

/* Once, after the seed is loaded. */
void     dv_init(const DvParams *p);
/* Per border: which frame cells each edge piece may take. by_side holds an
   edge piece to the side its g_spin gives it; otherwise any edge may take any
   border cell. */
void     dv_frame(bool by_side);
/* Turn corner seeding on or off for the boards queued next (it starts as
   DvParams.seed_corners). A front-end turns it off for a batch the corner
   catalog does not describe. */
void     dv_seeding(bool on);
/* Queue one board of the current batch: per cell, the piece (DV_EMPTY = open)
   and its rotation. Placed cells never move. With seeding on, seeded copies of
   the board are queued right after it (for the corner catalog of tc_config's
   last call). The board's top row -- the highest row with every row below it
   full -- is where seeding reads the exposed tops. */
void     dv_add(const uint16_t pid[NUM_PIECES], const uint8_t rot[NUM_PIECES]);
/* Under the current dv_frame, can a board's dives always complete? False when
   a piece is placed twice or a class of open cells lacks candidates (a board
   whose edges do not sit on the sides the frame deals them). */
bool     dv_fits(const uint16_t pid[NUM_PIECES]);
/* Dive, learn and polish the queued batch; keep the boards >= S under `id`. */
void     dv_run(const char *id);
/* Write the kept boards, best first, as "id, score, pos[256], rot[256]", and
   forget them. Called after every batch, so a killed run loses at most one. */
void     dv_flush(FILE *fp);
uint64_t dv_written(void);
uint64_t dv_boards(void);
void     dv_print_summary(double wall_total);

#endif /* E555_DIVE_H */
