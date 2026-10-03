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
 *
 * COPIES, INCUMBENTS, ORDERS (dv_queue)
 *   copies     a board queued K times, each copy on random streams of its own,
 *              is dived K times as K boards of the batch; only its best copy is
 *              kept (the earliest on a tie)
 *   incumbent  the complete board a queued board was cut from: every copy's
 *              best starts as the incumbent, so the board kept is never worse
 *              than it, and is the incumbent itself unless a dive beats it
 *              (DvParams.plateau: or ties it with a different board, which is
 *              then kept -- a walk along the plateau when batches are chained).
 *              With DvParams.prior / .nogo the dives start pulled toward the
 *              incumbent's placements on its clean cells and pushed off them on
 *              its broken cells: stage 1 is guided by those weights, and stage 2
 *              learns from them instead of from zero
 *   orders     copy k fills its open cells starting where orders[k % n] says:
 *              still the most constrained cell first, the order only breaking
 *              ties among the most constrained, so copies that start at
 *              different places leave their breaks in different places
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
    float    prior;            /* starting weight on an incumbent's clean placements; 0 = off */
    float    nogo;             /* ... and minus this on its broken ones; 0 = off */
    float    order_weight;     /* how strongly a copy's order breaks MRV ties; 0 = 6 */
    bool     plateau;          /* a board with an incumbent keeps a different board that ties
                                  it, when a dive or the polish finds one */
} DvParams;

/* Where a copy starts filling (DvQueue.orders): MRV is the stock dive, the
   others break MRV's ties toward one place of the open cells. */
typedef enum {
    DV_ORDER_MRV, DV_ORDER_LEFT, DV_ORDER_RIGHT, DV_ORDER_CENTRE, DV_ORDER_ENDS,
    DV_ORDER_TOP, DV_ORDER_BOTTOM, DV_NORDERS
} DvOrder;
extern const char *const dv_order_names[DV_NORDERS];   /* "mrv", "left", ... */

/* How dv_queue() queues one board; all zero = dv_add(). */
typedef struct {
    const uint16_t *inc_pid;   /* the complete board it was cut from, per cell; NULL = none */
    const uint8_t  *inc_rot;
    uint32_t        copies;    /* dive it this many times, each copy on streams of its own; 0 = 1 */
    const uint8_t  *orders;    /* copy k fills in order orders[k % norders] (DvOrder) */
    int             norders;   /* 0 = every copy MRV */
    uint64_t        salt;      /* keys the copies' streams too; 0 with one copy = dv_add's */
} DvQueue;

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
/* dv_add() with copies, an incumbent and fill orders (see COPIES above). The
   incumbent must hold every placed cell of the board as the board does. With
   seeding on, the seeded copies are made from the first copy only. */
void     dv_queue(const uint16_t pid[NUM_PIECES], const uint8_t rot[NUM_PIECES],
                  const DvQueue *o);
/* Under the current dv_frame, can a board's dives always complete? False when
   a piece is placed twice or a class of open cells lacks candidates (a board
   whose edges do not sit on the sides the frame deals them). */
bool     dv_fits(const uint16_t pid[NUM_PIECES]);
/* Dive, learn and polish the queued batch; keep the boards >= S under `id`. */
void     dv_run(const char *id);
/* Write the kept boards, best first, as "id, score, pos[256], rot[256]", and
   forget them. Called after every batch, so a killed run loses at most one. */
void     dv_flush(FILE *fp);
/* Optional: called with the score of each row dv_flush writes, in order
   (NULL, the default, for none). */
void     dv_set_row_hook(void (*hook)(int score));
uint64_t dv_written(void);
/* The best score of the last dv_run batch, -1 when it dived nothing. */
int      dv_last_best(void);
uint64_t dv_boards(void);
/* Written boards at a score (matched edges), and the time spent diving. */
uint64_t dv_score_count(int score);
double   dv_seconds(void);
/* After dv_run: the best board of queued board i of that batch (i counts
   dv_add/dv_queue calls, not copies) -- its best copy, or its incumbent -- and
   its score, whether or not it reached S; -1 when it has none. */
int      dv_result(size_t i, uint16_t pid[NUM_PIECES], uint8_t rot[NUM_PIECES]);
/* Keep each batch's boards for dv_flush (on, the default), or not (off): a
   front-end chaining batches reads them with dv_result, and keeps the last
   batch's with dv_keep_last (returns how many were >= S). */
void     dv_keeping(bool on);
uint64_t dv_keep_last(void);
void     dv_print_summary(double wall_total);

#endif /* E555_DIVE_H */
