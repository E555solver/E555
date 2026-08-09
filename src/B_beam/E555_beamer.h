/*
 * E555_beamer.h -- Stage B 5-5-5 beam search: data structures.
 *
 * The beam search is the solver layer on top of the single-database module
 * (E555_database.h). It reads the read-only DB_5pieces and drives a wide,
 * fixed-width frontier of partial boards up the puzzle one row at a time. Each
 * inner row is filled left-to-right as three 5-piece segments (A: cols 1-5,
 * B: cols 6-10, C: cols 11-15 ending in the emergent right edge); the col-0 left
 * edge is fixed by the border configuration. See E555_beamer.c for the full
 * architecture (expand / score / select / materialize / emit).
 */

#ifndef E555_BEAMER_V1_H
#define E555_BEAMER_V1_H

#include <stdint.h>
#include <stdbool.h>

#include "E555_database.h"

/* Per-parent decode-budget floor (see expand_row): deep in the board most
   candidate chains fail the exact disjointness test, so the budget never starves
   a parent below this many full decode attempts. */
#define MIN_DECODE_BUDGET   2048

/* -- Beam state ------------------------------------------------------------ */

/* One committed inner row: the 14 inner catalog indices for columns 1..14
   (ci[0..4] seg A, ci[5..9] seg B, ci[10..13] seg C inner) and rterm, the
   edge-terminal pool index placed at column 15. Column 0 is the fixed left edge
   (from the border config), so it is not stored here. */
typedef struct {
    uint16_t ci[EDGE_LEN];   /* cols 1..14 */
    uint8_t  rterm;          /* col 15 right edge (g_edge_term index) */
} RowChoice;

/* Which columns of the emitted TOP row carry a piece (bit c = column c). Rows
   below the top are always full. A completed row uses ROWMASK_FULL; the three
   --incomplete_top partials each place the col-0 left edge plus two of the three
   segments -- 11 of the row's 16 pieces -- and leave the third segment's five
   columns unplaced (pos 999). The mask is also what keeps the RowChoice.ci[]
   slots of the missing segment, which are never filled in, from being read. */
#define ROWMASK_FULL  0xFFFFu   /* cols 0..15                         */
#define ROWMASK_AB    0x07FFu   /* cols 0..10   -- C (11..15) missing */
#define ROWMASK_AC    0xF83Fu   /* cols 0..5,11..15 -- B (6..10) missing */
#define ROWMASK_BC    0xFFC1u   /* cols 0,6..15 -- A (1..5) missing    */

/* One beam board: only the resumable frontier state (counters + exposed tops).
   The per-row move history lives OUTSIDE the entry, in the beam context's
   ancestry log (RowLog); log_idx points at this board's own entry in the log of
   its depth row, from which the full board is reconstructed at emission time.
   Keeping the hot struct small (~150 B vs ~570 B with an inline move log) makes
   the per-candidate scratch copy and the per-row materialization ~4x cheaper. */
typedef struct {
    uint64_t used[4];
    int16_t  color_consumed[NUM_COLORS_TOTAL];
    int16_t  req_exposed[NUM_COLORS_TOTAL];
    uint8_t  rtop[PUZZLE_SIDE];     /* exposed tops; [1..14] inner, [15] edge-iface */
    uint16_t depth;                 /* last committed row (0 = only border) */
    uint8_t  flags;                 /* FLAG_* */
    uint8_t  _pad;
    uint32_t log_idx;               /* own entry in ctx->log[depth]; unused at depth 0 */
    float    score;
} BeamEntry;

/* One candidate child in the per-row pool: which beam board it extends, the move
   that extends it, its score, and its frontier signature for the dedup. */
typedef struct {
    float     score;
    uint32_t  parent;
    uint64_t  sig;
    RowChoice mv;
} PoolEntry;

/* One ancestry-log entry: the move that created a materialized beam board, plus
   its parent's log index one row below. Emission walks this chain backwards. */
typedef struct {
    uint32_t  parent_log;
    RowChoice mv;
} RowLog;

/* Survivor sort key: pool index + score, ranked descending after the dedup. */
typedef struct { float score; uint32_t idx; } SortRec;

/* Per-thread scratch: a tentative child board for commit + score, and a local
   append buffer so pool insertion costs one atomic per batch, not per child. */
#define POOL_BATCH 256
typedef struct {
    BeamEntry tmp;
    PoolEntry buf[POOL_BATCH];
    uint32_t  buf_n;
} Scratch;

/* Shared per-run beam workspace (sized once from --beam_width x --beam_expand). */
typedef struct {
    BeamEntry *beam_a, *beam_b;
    PoolEntry *pool;
    uint64_t   pool_cap;
    uint64_t   pool_n;            /* atomic append cursor */
    uint64_t  *sig_key;           /* dedup: open addressing, region-sharded */
    float     *sig_score;         /*   best score seen for the slot's signature */
    uint32_t  *sig_idx;           /*   pool index holding that best score */
    size_t     sig_sz;
    uint32_t  *keep;              /* surviving pool indices, ranked best-first */
    SortRec   *srt, *srt_tmp;     /* survivor ranking buffers */
    uint8_t   *taken;
    uint32_t  *offspring;
    uint32_t  *sel;
    RowLog    *log[EDGE_LEN];     /* ancestry log, rows 1..13; reset per config */
    uint32_t   log_n[EDGE_LEN];
    uint32_t   log_cap[EDGE_LEN];
} BeamCtx;

/* Everything one (parent, slice) work item needs to scan its share of the
   parent's segment-A cell. The B and C cells depend on the chosen A (and B)
   chains, so they are resolved inside the scan, not here. */
typedef struct {
    const BeamEntry *parent;
    uint32_t  parent_idx;
    int       row;
    bool      at_stop;
    int       la_A;              /* segment-A left color (from the fixed left col) */
    const Cell *cA;              /* segment-A cell for this parent/row */
    RNG       rng;
    uint64_t  budget;            /* full decode attempts remaining */
    uint32_t  quota;             /* accepted children remaining */
} Expand;

typedef struct { uint32_t row, width; const char *reason; } BeamResult;

#endif /* E555_BEAMER_V1_H */
