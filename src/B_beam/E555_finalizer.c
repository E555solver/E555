/*
 * E555_finalizer.c -- E555 finalizer: 5-5-5 beam search from a partial.
 *
 * Derivative of the Stage B beamer (E555_beamer.c) that starts the beam
 * from a PARTIAL BOARD instead of the bottom border. The input is a
 * completions CSV in the exact format the beamer (and this program) emits, so
 * partials can be finalized, re-finalized, and chained. Rows are 0-indexed
 * bottom-up: row 0 = bottom border, inner rows 1..14, row 15 = top border.
 *
 * WHAT IT DOES
 *   Loads one or more partial boards (--border_row / --border_row_N select the
 *   CSV lines), LOCKS every piece at or below --finalize_from, and grows each
 *   board upward with the regular beam algorithm (expand / score / select /
 *   materialize -- identical machinery to the beamer; see the function
 *   comments). Boards reaching --stop_row are appended to
 *   <out_dir>/beam_completions_finalized_<stop_row>.csv, best-scored first; as in the
 *   beamer, no lookahead is applied at the stop row. Input lines seeding a
 *   search state already seen earlier in the file are skipped (see the
 *   "Input dedup" section) -- long partial lists are full of near-siblings
 *   that become identical once their top rows are freed.
 *
 * THE REDUCED DATABASE
 *   The 5-piece chain database is recomputed PER PARTIAL with the locked
 *   pieces excluded (g_db_exclude). With half the board fixed the chain DFS
 *   shrinks super-exponentially: the database builds in seconds, and --
 *   decisively for the high rows -- contains no chain that could be rejected
 *   for reusing a locked piece, so the per-row search is far faster than the
 *   beamer's at the same depth. Nothing is ever written to disk (no --db_file
 *   here).
 *
 * SIDE MODES
 *   Without --free_edges the borders placed in the partial are used as a fixed
 *   assignment (left column, right terminals, top-border demands) -- this
 *   requires ALL 60 border pieces to be placed in the CSV line. If any border
 *   piece is unplaced (pos 999), --free_edges is activated automatically:
 *   every edge and corner above finalize_from returns to a shared pool, and
 *   the left column above the locked rows is drawn by random sampling
 *   (--top_columns columns per partial, each the best of RANDOM_SIDE_SAMPLES
 *   fan-out-ranked chains), so repeated runs explore ever-fresh completions.
 *
 * ROW 14 AND THE TOP BORDER
 *   --stop_row may go up to 14 (the beamer stops at 13): committing row 14
 *   consumes the last inner pieces, so the frontier bookkeeping treats its
 *   tops as satisfying the top border rather than demanding a further inner
 *   row (see commit_row). Row 15 is deliberately NOT searchable: placing the
 *   final border is trivial for an external program once rows 0..14 stand.
 *
 * SCHEDULES AND RANDOMNESS
 *   The width/randomness schedules keep their ABSOLUTE row meaning from the
 *   beamer (expansion at --beam_expand_row, maha in rows 3..9), so a high
 *   finalize_from starts already expanded and fan-out-driven. One exception:
 *   the first searched row (finalize_from + 1) always uses the full
 *   --frac_rand random selection band, injecting variability into every run
 *   and repeat (--finalize_repeats sweeps each partial several times for
 *   exactly this reason). Runs are intentionally NOT reproducible unless
 *   --seed is given: by default the master seed comes from the clock and the
 *   process id (and is printed).
 *
 * COMPILE / RUN
 *   gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *       E555_database.c E555_finalizer.c -o E555_finalizer -lm
 *   ./E555_finalizer --help
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <omp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "E555_beamer.h"

/* Stop-row emission safety cap (lines per config; the CSV rows are ~2 KB). */
#define EMIT_MAX 1000000u

static uint64_t g_master_seed = 0;

static uint32_t g_beam_width      = 250000;
static uint32_t g_stop_row        = 11;
static uint32_t g_beam_expand     = 4;
static uint32_t g_beam_expand_row = 8;
static double   g_lambda_maha     = 0.6;   /* --lambda_Mahalanobis, in score-SD */
static double   g_frac_rand       = 0.30;  /* flat; deliberately above the beamer */
static double   g_lambda_J        = 1.0;    /* --lambda_J, the closure weight */
static bool     g_free_demand     = true;   /* --no_free_demand turns it off */
static uint32_t g_parent_cap      = 4;
static uint32_t g_pool_factor     = 8;
#define SCAN_FACTOR 1024u   /* was --scan_factor; a sweep found it inert */
static const char *g_out_dir      = "beam_out";

static uint32_t g_border_row_index = 0;   /* first data line of the partials CSV */
static uint32_t g_border_row_N     = 1;   /* number of consecutive lines to load */
static uint32_t g_finalize_from    = 5;   /* lock rows 0..N; search starts at N+1 */
static uint32_t g_finalize_repeats = 1;   /* full sweeps per input partial */
static double   g_config_time_sec  = 600.0;
static double   g_max_wall_sec     = 0.0;
static uint64_t g_max_partials     = 0;   /* reported-board budget; 0 = unlimited */
static long     g_top_columns      = 12;  /* sampled left columns per partial (free mode) */
static uint32_t g_bail_columns     = 0;   /* give up on a line after N barren columns */
/* Selection temperature for the left-column rank, as in the beamer. 0 = off,
   i.e. the published column is simply the best of the samples. */
static double   g_tau_columns      = 0.0;
static bool     g_opt_free_edges   = false;  /* --free_edges given (else auto per line) */

/* Optional Stage A rotations CSV (3rd positional): every data row is a candidate
   piece->side assignment. A partial whose LOCKED border agrees with one of them
   came from that border, so the row's assignment can be reimposed even though the
   partial's own border is incomplete -- see fin_rot_match. */
static uint8_t (*g_rot_rows)[NUM_PIECES] = NULL;
static uint32_t  g_rot_n       = 0;
static bool      g_sides_from_rot = false;  /* this line's sides came from a row */
static int       g_rot_matched    = -1;     /* which row, or -1 */

static const LeftOrder *g_cur_left = NULL;
static char     g_config_id_str[64] = "c0";

/* The loaded partial: oriented piece per filled cell, the locked-piece mask
   (rows 0..finalize_from), and the deepest fully-placed row. */
static Oriented g_grid[PUZZLE_SIDE][PUZZLE_SIDE];
static bool     g_grid_has[PUZZLE_SIDE][PUZZLE_SIDE];
static uint64_t g_lock_mask[4];
static int      g_partial_depth = -1;
static uint16_t g_tr_reserved   = 0xFFFF;   /* corner kept for the top-right cell */

/* Emit dedup table (open addressing, power-of-2; emission is serial). */
static uint64_t  *g_emit_htable = NULL;
static size_t     g_emit_htable_sz = 0, g_emit_count = 0;
static FILE      *g_completions_fp = NULL;
static uint64_t   g_solution_idx = 0;

/* --incomplete_top: A+B rows that reach --stop_row but have no segment C. They
   go to a separate partials file with their own dedup table; emission happens
   inside the parallel expansion, so it is guarded by an OpenMP critical. */
static bool       g_incomplete_top    = false;
static FILE      *g_partial_fp        = NULL;
static uint64_t  *g_partial_htable    = NULL;
static size_t     g_partial_htable_sz = 0, g_partial_count = 0;
static size_t     g_partial_total     = 0;
/* Run totals per partial kind, for the summary line. Indexed by ROWMASK_*. */
static size_t     g_part_ab = 0, g_part_ac = 0, g_part_bc = 0;

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* Run statistics for the end-of-run summary. */
static struct {
    uint64_t configs;
    uint64_t rows_advanced;
    uint64_t cands_total;
    uint64_t extinct_at[EDGE_LEN + 1];
    uint64_t reached_stop;
    uint64_t emitted_total;
    double   t_expand, t_select, t_mat, t_emit;
} g_stats;

/* --max_partials budget: stop-row completions plus --incomplete_top A+B partials,
   i.e. every board written to disk. Checked after each stop-row emission and at
   each config boundary, so the beam in flight is always reported in full and the
   final count may exceed N by up to one beam's worth. Called from serial code
   only (g_partial_total is mutated inside the emission critical section).
   The announcement is deferred to _announce() so that it never lands ahead of
   the [sweep] line of the config that triggered it. */
static bool g_budget_hit = false;
static bool partials_budget_spent(void) {
    if (g_max_partials == 0 || g_stop) return false;
    if (g_stats.emitted_total + (uint64_t)g_partial_total < g_max_partials) return false;
    g_budget_hit = true;
    g_stop = 1;
    return true;
}
static void partials_budget_announce(void) {
    if (!g_budget_hit) return;
    g_budget_hit = false;
    printf("[sweep] max_partials reached (%" PRIu64 " boards reported).\n",
           g_stats.emitted_total + (uint64_t)g_partial_total);
    fflush(stdout);
}

/* -- Row schedules ----------------------------------------------------------- */

/* Effective beam width: base K below the expansion, K*E/2 one row before
   --beam_expand_row (never below K), K*E from there on. The steps set the
   absolute width, so the combined final expansion is exactly E. */
static uint32_t beam_eff_K(int row) {
    const uint32_t K = g_beam_width, E = g_beam_expand;
    if (E <= 1 || (uint32_t)row < g_beam_expand_row - 1) return K;
    if ((uint32_t)row == g_beam_expand_row - 1) {
        uint32_t half = (uint32_t)(((uint64_t)K * E) / 2);
        return half > K ? half : K;
    }
    return K * E;
}

/* The random selection band is FLAT: --frac_rand applies at every row, as in the
   beamer, though at a deliberately higher default.
 
   It used to taper -- full up to expand_row-1, half there, ZERO from expand_row
   on -- with one exception carved out to keep the first searched row random.
   That schedule was written for a search running from row 1, and it does not
   survive being handed a board already filled to finalize_from. At the default
   finalize_from of 8 the searched rows are 9..12 and expand_row is 8, so exactly
   ONE row kept any randomness and the other three were purely fan-out selected.
   The old comment names the cost of that itself: "repeated runs over the same
   partial would retrace each other". Repeated runs are how this tool is used --
   --finalize_repeats exists for it -- so the band that makes them differ has to
   be alive on every row, not just the first.
 
   Why higher than the beamer's 0.10 rather than equal to it: the beamer gets one
   pass at a configuration and wants its budget spent on what the objective likes
   best. The finalizer can be re-run over the same partial as often as it is
   worth doing, so a wider random band is not a tax on one pass, it is coverage
   across many -- and it is the only thing keeping those passes from being copies
   of each other. */

/* gumbel_noise() and gumbel_key() are shared with the side samplers -- see
   E555_database.h. */

/* Per-parent offspring cap in the score band: doubled once the beam widens, so
   successful parents can actually fill the extra slots (0 = uncapped). */
static uint32_t parent_cap_eff(int row) {
    if (g_parent_cap == 0) return 0;
    return (g_beam_expand > 1 && (uint32_t)row >= g_beam_expand_row - 1)
           ? g_parent_cap * 2 : g_parent_cap;
}

/* -- Board state operations ------------------------------------------------- */

/* Oriented view of a seed piece under a spin (same convention as the database
   catalog: spin k exposes seed side (side+k)&3 on each board side). */
static Oriented fin_oriented(uint16_t pid, uint8_t spin) {
    const int e[4] = { g_seed_top[pid], g_seed_right[pid],
                       g_seed_bottom[pid], g_seed_left[pid] };
    Oriented o;
    o.piece_id = pid; o.rotation = spin;
    o.top    = (uint8_t)e[(0+spin)&3]; o.right = (uint8_t)e[(1+spin)&3];
    o.bottom = (uint8_t)e[(2+spin)&3]; o.left  = (uint8_t)e[(3+spin)&3];
    return o;
}

/* -- Eternity II clue pieces ------------------------------------------------
   Unlike the beamer, which builds a board from nothing and must therefore carry
   all four orientations through the beam, the finalizer is always handed a board
   that already committed to one. So the orientation is READ OFF the input line
   and a single one is pinned: no orientation bits in BeamEntry.flags, no term in
   the frontier signature, no per-orientation reserve pass in select_beam. That
   is most of the beamer's clue machinery gone.

   g_clue_ci[o][k] is the catalog index of clue k under orientation o, which is
   both what PIN_PIECE wants and where the clue's own bottom colour comes from.
   g_fin_orient is the orientation of the pass being searched, -1 when clues are
   off.

   READING an orientation and CHOOSING one are different things, and the
   difference is the whole of --clue_orient. A board carrying a clue has already
   committed: fin_clue_orient_of reads it off and it is never reconsidered. A
   board carrying none has committed to nothing -- which is every partial locked
   below row 7 under --clue_center, the centre clue being the only reachable one
   and sitting on row 7 or 8. Such a line is searched once per candidate
   orientation: four different pins against the same fixed lock, so four
   genuinely different searches, of which the old code ran none. */
static uint16_t g_clue_ci[4][CLUE_N];
static bool     g_clue_debug = false;   /* E555_CLUE_DEBUG=1 */
static int      g_fin_orient = -1;
static uint8_t  g_clue_orient_req = 0xF;  /* --clue_orient: allowed orientations */
static int      g_fin_orient_cand[4];     /* candidates for the current line */
static int      g_fin_orient_n = 0;       /* 0 = clues off -> one pass at -1 */

static uint16_t fin_cat_index_of(uint16_t pid, uint8_t spin) {
    for (int i = 0; i < g_cat_count; i++)
        if (g_cat[i].piece_id == pid && g_cat[i].rotation == spin) return (uint16_t)i;
    fatal("clue piece %u spin %u is not in the oriented catalog", pid, spin);
    return 0;
}

/* Is this clue entry active under the current flags? Entry 0 is the center. */
static inline bool clue_on(int k) {
    return (k == 0) ? (g_clue_mask & CLUE_CENTER) != 0 : (g_clue_mask & CLUE_CORNERS) != 0;
}

static void fin_init_clue_tables(void) {
    if (!g_clue_mask) return;
    g_clue_debug = getenv("E555_CLUE_DEBUG") != NULL;
    for (int o = 0; o < 4; o++)
        for (int k = 0; k < CLUE_N; k++)
            g_clue_ci[o][k] = fin_cat_index_of(g_clue[o][k].piece, g_clue[o][k].spin);
    for (int o = 0; o < 4; o++)
        if (g_clue[o][3].row != 13 || g_clue[o][4].row != 13)
            fatal("clue table: entries 3,4 must sit on row 13 (the reserve-and-attach pair)");
}

/* The orientation this input line committed to: the one satisfying the most
   enabled clues. Returns -1 when the line satisfies none, which is the only
   case a finalizer cannot resolve on its own -- an unclued board is equally
   compatible with all four, and there is nothing on it to choose from. */
static int fin_clue_orient_of(const int pos[NUM_PIECES], const int rot[NUM_PIECES]) {
    int best = -1, best_n = 0;
    for (int o = 0; o < 4; o++) {
        int n = 0;
        for (int k = 0; k < CLUE_N; k++) {
            if (!clue_on(k)) continue;
            const ClueCell *cc = &g_clue[o][k];
            if (pos[cc->piece] == cc->row * PUZZLE_SIDE + cc->col && rot[cc->piece] == cc->spin) n++;
        }
        if (n > best_n) { best = o; best_n = n; }
    }
    return best;
}

/* --clue_orient: "auto" (all four) or a comma list from 0..3, into a 4-bit
   mask. Rejects anything else rather than silently searching everything -- a
   typo here would quietly quadruple a run's cost. */
static uint8_t parse_clue_orient(const char *spec) {
    if (!strcmp(spec, "auto") || !strcmp(spec, "all")) return 0xF;
    uint8_t m = 0;
    for (const char *p = spec; *p; ) {
        if (*p < '0' || *p > '3' || (p[1] && p[1] != ','))
            fatal("--clue_orient wants 'auto' or a comma list from 0,1,2,3, not '%s'", spec);
        m |= (uint8_t)(1u << (*p - '0'));
        p += p[1] == ',' ? 2 : 1;
    }
    if (!m) fatal("--clue_orient names no orientation");
    return m;
}

/* Every enabled clue piece, over all four orientations. The four rows of
   g_clue permute the same five pieces -- {138, 180, 207, 248, 254} with
   --clue_corners, {138} without -- so this set does not depend on the
   orientation, which is exactly why the reduced database and the reserve can be
   shared by all four passes of one line. */
static void fin_clue_reserve(uint64_t used[4]) {
    for (int o = 0; o < 4; o++)
        for (int k = 0; k < CLUE_N; k++)
            if (clue_on(k)) used_set(used, g_clue[o][k].piece);
}

/* Frame sides of a seed piece: 0 = inner, 1 = edge, 2 = corner. */
static int fin_zero_count(int pid) {
    return (g_seed_top[pid]==0) + (g_seed_right[pid]==0)
         + (g_seed_bottom[pid]==0) + (g_seed_left[pid]==0);
}

/* Initialize the beam's single root board from the locked grid rows
   0..finalize_from plus the chosen left column above them. Equivalent to the
   beamer's border init followed by commit_row for every locked row: used
   covers the locked pieces, the column and the two reserved top corners;
   color_consumed counts the four sides of every locked inner piece (so
   maha_d2n's n = 14*depth stays exact); req_exposed carries the locked
   frontier's tops plus, in fixed mode, only the interfaces the REMAINING rows
   must still satisfy (the beamer's init demands minus what the locked rows
   already consumed -- the edge-terminal pool already excludes locked
   terminals via g_db_exclude). */
static void fin_init_entry(BeamEntry *p, const LeftOrder *lft) {
    memset(p, 0, sizeof *p);
    p->log_idx = UINT32_MAX;
    p->depth   = (uint16_t)g_finalize_from;

    for (int k = 0; k < 4; k++) p->used[k] = g_lock_mask[k] | lft->used[k];
    if (g_tr_reserved != 0xFFFF) used_set(p->used, g_tr_reserved);

    /* Reserve every enabled clue piece. The pinned walk is exempt from this
       mask, so a clue can still be placed on its own cell; everything else is
       barred from spending it. Reserving also keeps the supply and free-demand
       accounting exact for the row-13 pair, which is genuinely unavailable to
       any row this run searches. Idempotent for clues already inside the lock.
       Taken over all four orientations, which is not a widening: the four rows
       of g_clue name the same five pieces and only move them, so the mask is
       the one this pass would build anyway -- and it no longer depends on which
       orientation the pass happens to be searching. */
    if (g_clue_mask) fin_clue_reserve(p->used);

    for (int r = 1; r <= (int)g_finalize_from; r++)
        for (int c = 1; c <= EDGE_LEN; c++) {
            const Oriented *o = &g_grid[r][c];
            p->color_consumed[INNER_IDX(o->top)]++;    p->color_consumed[INNER_IDX(o->right)]++;
            p->color_consumed[INNER_IDX(o->bottom)]++; p->color_consumed[INNER_IDX(o->left)]++;
        }

    for (int c = 0; c < PUZZLE_SIDE; c++)
        p->rtop[c] = g_grid[g_finalize_from][c].top;
    for (int c = 1; c <= EDGE_LEN; c++)         /* inner frontier (cols 1..14) */
        if (color_is_inner(p->rtop[c])) p->req_exposed[INNER_IDX(p->rtop[c])]++;

    if (!g_free_edges) {
        /* Remaining left interfaces. Every unlocked left-side edge ends up in some
           row finalize_from+1..14, so the multiset of colors it owes the search is
           independent of the column's ordering -- and unlike lft->right[], the pool
           is complete even when the exhaustive enumerator fixed the column only up
           to --stop_row (fin_enum_rec). */
        for (int k = 0; k < g_left_count_pool; k++) {
            const Oriented *lp = &g_left_pool[k];
            if (used_test(g_lock_mask, lp->piece_id)) continue;
            if (color_is_inner(lp->right)) p->req_exposed[INNER_IDX(lp->right)]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {  /* remaining right edges */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[INNER_IDX(cl)]++;
        }
        for (int c = COLOR_MIN; c <= COLOR_MAX; c++)
            p->req_exposed[INNER_IDX(c)] = (int16_t)(p->req_exposed[INNER_IDX(c)] + g_top_border_inner_count[c]);
    } else if (g_free_demand) {
        /* Free mode owes exactly the same demands; only the bookkeeping differs.
           An edge piece carries ONE inner color, and in either remaining role --
           frame-right (inner side faces left) or frame-up (inner side faces
           down) -- it exposes exactly that one inner half-edge into the interior.
           So which unplaced edge becomes a right edge and which becomes a top
           border piece does not change the demand multiset, and free mode's
           demands are exact without enumerating the split.

           Two sources, disjoint by construction. The chosen column above the
           locked rows is PLACED but its inner sides face rows that are not, so
           it still owes an interface per row -- and p[r] is NULL for rows the
           column does not reach (fin_enum_rec fixes col 0 only up to
           --stop_row, where fin_sample_left goes all the way to row 14). Every
           edge the column did not take is then covered by the used-tested pool
           loop, which spans the remaining right edges AND the whole top border
           at once (free mode puts all 56 non-corner edges in g_edge_term, minus
           the locked ones via g_db_exclude, and g_top_border_inner_count is
           all-zero here). Corners never enter g_edge_term, and carry no inner
           color, so the two free corners need no handling. */
        for (int r = (int)g_finalize_from + 1; r <= EDGE_LEN; r++) {
            if (!lft->p[r]) continue;                  /* column stops here */
            int cr = lft->right[r];
            if (color_is_inner(cr)) p->req_exposed[INNER_IDX(cr)]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {
            if (used_test(p->used, g_edge_term[t].piece_id)) continue;  /* already placed */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[INNER_IDX(cl)]++;
        }
    }

}

/* Commit one inner row (cols 1..15) into the board (counters + frontier only;
   the move itself is logged by the caller). */
static void commit_row(BeamEntry *p, int row, const RowChoice *rc) {
    const Oriented *term = &g_edge_term[rc->rterm];

    for (int i = 0; i < EDGE_LEN; i++) {
        const Oriented *o = &g_cat[rc->ci[i]];
        used_set(p->used, o->piece_id);
        p->color_consumed[INNER_IDX(o->top)]++;    p->color_consumed[INNER_IDX(o->right)]++;
        p->color_consumed[INNER_IDX(o->bottom)]++; p->color_consumed[INNER_IDX(o->left)]++;
    }
    used_set(p->used, term->piece_id);

    uint8_t new_top[PUZZLE_SIDE];
    new_top[0]  = g_cur_left->p[row]->top;        /* col 0 (edge-iface, reconstruction) */
    for (int i = 0; i < EDGE_LEN; i++) new_top[1+i] = g_cat[rc->ci[i]].top;
    new_top[15] = term->top;                       /* col 15 (edge-iface) */

    /* Frontier update, cols 1..15 (col 0 is edge-iface). Rows 1..13: a new top
       is a demand on the NEXT inner row. Row 14 has no inner row above -- its
       tops face the top border, so they must not be counted as future demands:
       in fixed mode each one instead SATISFIES a top-border demand (decrement;
       any multiset mismatch leaves some demand unmet and parity_ok rejects the
       board, which is exactly the required row-14 top check); in free mode the
       top assignment is unconstrained, so the top simply drops out. Without
       this, every legal row-14 board would fail parity: all 196 inner pieces
       are consumed there, so any leftover req_exposed drives S negative. */
    const bool demand_exact = (!g_free_edges || g_free_demand);
    for (int c = 1; c < PUZZLE_SIDE; c++) {
        int old_t = p->rtop[c], new_t = new_top[c];
        if (color_is_inner(old_t)) p->req_exposed[INNER_IDX(old_t)]--;
        if (color_is_inner(new_t)) {
            if (row < EDGE_LEN)      p->req_exposed[INNER_IDX(new_t)]++;
            else if (demand_exact)   p->req_exposed[INNER_IDX(new_t)]--;
        }
    }
    for (int c = 0; c < PUZZLE_SIDE; c++) p->rtop[c] = new_top[c];

    if (demand_exact) {
        int la = g_cur_left->right[row];           /* segment-A interface satisfied */
        if (color_is_inner(la)) p->req_exposed[INNER_IDX(la)]--;
        int tl = term->left;                       /* right-edge interface satisfied */
        if (color_is_inner(tl)) p->req_exposed[INNER_IDX(tl)]--;
    }

}

/* Color parity: every inner color's surplus must be non-negative, and even
   whenever the demand vector is complete. S_c counts the free half-edges of
   color c -- those not spoken for by any committed interface -- and they can
   only be spent pairing with EACH OTHER, so an odd S_c proves the board cannot
   complete. That holds under fixed edges, and also in free mode once
   --no_free_demand is not in force (see fin_init_entry: the demands are exact
   there too). With an incomplete demand vector S is overstated and its parity
   is meaningless, which is why the test was originally skipped in free mode. */
static bool parity_ok(const BeamEntry *p) {
    const bool demand_exact = (!g_free_edges || g_free_demand);
    for (int c = COLOR_MIN; c <= COLOR_MAX; c++) {
        int S = g_inner_color_total[c] - p->color_consumed[INNER_IDX(c)] - p->req_exposed[INNER_IDX(c)];
        if (S < 0) return false;
        if (demand_exact && (S & 1)) return false;
    }
    return true;
}

/* -- Emit ------------------------------------------------------------------- */

static void htable_grow(void) {
    size_t new_sz = g_emit_htable_sz * 2;
    uint64_t *nt = xmalloc(new_sz * sizeof(uint64_t));
    memset(nt, 0, new_sz * sizeof(uint64_t));
    for (size_t i = 0; i < g_emit_htable_sz; i++) {
        uint64_t k = g_emit_htable[i];
        if (!k) continue;
        size_t h = k & (new_sz - 1);
        while (nt[h]) h = (h + 1) & (new_sz - 1);
        nt[h] = k;
    }
    free(g_emit_htable); g_emit_htable = nt; g_emit_htable_sz = new_sz;
}
static bool htable_insert(uint64_t key) {
    if (!key) key = 1;
    size_t h = key & (g_emit_htable_sz - 1);
    while (g_emit_htable[h]) {
        if (g_emit_htable[h] == key) return false;
        h = (h + 1) & (g_emit_htable_sz - 1);
    }
    g_emit_htable[h] = key; g_emit_count++;
    if (g_emit_count * 2 > g_emit_htable_sz) htable_grow();
    return true;
}
static void htable_init(void) {
    size_t sz = 1u << 16;
    if (g_emit_htable) free(g_emit_htable);
    g_emit_htable = xmalloc(sz * sizeof(uint64_t));
    memset(g_emit_htable, 0, sz * sizeof(uint64_t));
    g_emit_htable_sz = sz; g_emit_count = 0;
}

/* Dedup table for --incomplete_top partials (kept separate from the completions
   table so g_emit_count is unaffected). Same open-addressing scheme. */
static void partial_htable_grow(void) {
    size_t new_sz = g_partial_htable_sz << 1;
    uint64_t *nt = xmalloc(new_sz * sizeof(uint64_t));
    memset(nt, 0, new_sz * sizeof(uint64_t));
    for (size_t i = 0; i < g_partial_htable_sz; i++) {
        uint64_t k = g_partial_htable[i];
        if (!k) continue;
        size_t h = k & (new_sz - 1);
        while (nt[h]) h = (h + 1) & (new_sz - 1);
        nt[h] = k;
    }
    free(g_partial_htable); g_partial_htable = nt; g_partial_htable_sz = new_sz;
}
static bool partial_htable_insert(uint64_t key) {
    if (!key) key = 1;
    size_t h = key & (g_partial_htable_sz - 1);
    while (g_partial_htable[h]) {
        if (g_partial_htable[h] == key) return false;
        h = (h + 1) & (g_partial_htable_sz - 1);
    }
    g_partial_htable[h] = key; g_partial_count++;
    if (g_partial_count * 2 > g_partial_htable_sz) partial_htable_grow();
    return true;
}
static void partial_htable_init(void) {
    size_t sz = 1u << 16;
    if (g_partial_htable) free(g_partial_htable);
    g_partial_htable = xmalloc(sz * sizeof(uint64_t));
    memset(g_partial_htable, 0, sz * sizeof(uint64_t));
    g_partial_htable_sz = sz; g_partial_count = 0;
}

/* Reconstruct the move log of a beam board by walking its ancestry chain
   backwards; fills rows[finalize_from+1 .. p->depth] (the locked rows live in
   g_grid, not in the log). rows[] must hold EDGE_LEN+1 entries: the top inner
   row 14 is a valid stop row here. */
static void collect_rows(const BeamCtx *ctx, const BeamEntry *p,
                         RowChoice rows[EDGE_LEN + 1]) {
    uint32_t li = p->log_idx;
    for (int r = p->depth; r > (int)g_finalize_from; r--) {
        rows[r] = ctx->log[r][li].mv;
        li = ctx->log[r][li].parent_log;
    }
}

/* Resolve the placed piece at (r,c): locked rows straight from the loaded
   grid, col 0 from the config's left column, col 15 from the committed right
   edge, otherwise the committed inner piece. */
static inline void board_cell(const RowChoice rows[EDGE_LEN + 1], int r, int c,
                              uint16_t *pid, uint8_t *rot) {
    const Oriented *o;
    if (r <= (int)g_finalize_from)   o = &g_grid[r][c];
    else if (c == 0)                 o = g_cur_left->p[r];
    else if (c == PUZZLE_SIDE - 1)   o = &g_edge_term[rows[r].rterm];
    else                             o = &g_cat[rows[r].ci[c - 1]];
    *pid = o->piece_id; *rot = o->rotation;
}

static uint64_t board_fingerprint(const RowChoice rows[EDGE_LEN + 1], int depth) {
    const uint64_t prime = 1099511628211ULL;
    uint64_t fp = 14695981039346656037ULL;
    for (int r = 0; r <= depth; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            fp ^= pid; fp *= prime; fp ^= rot; fp *= prime;
        }
    return fp ? fp : 1;
}

/* -- Emission line formatting ------------------------------------------------ */

/* Longest tail an emitted line can carry: 256 position fields (0..255, or the
   999 unplaced sentinel -- 3 digits) and 256 rotation fields (1 digit), each
   preceded by ", ", plus the newline: 256*5 + 256*3 + 1 = 2049 bytes. The
   "<config>, <sol_idx>" prefix is written by the caller, which is the only place
   sol_idx is known. */
#define EMIT_LINE_MAX 3072

/* Completion files are block-buffered rather than line-buffered: a line is ~2 KB
   and line buffering spends one write() syscall per board. The trade is that a
   SIGKILL can now lose the tail of the buffer instead of nothing, so the file is
   flushed after every config, which bounds the loss to the config in flight. */
#define EMIT_FILE_BUF (1u << 20)

/* Decimal conversion. A board line is 512 of these and almost nothing else, so
   it is worth not going through fprintf's general machinery: measured ~2x on the
   exact emission pattern. */
static inline char *u32a(char *p, uint32_t v) {
    char t[10]; int k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) *p++ = t[--k];
    return p;
}

/* Flatten rows[0..row] into per-piece position/rotation vectors and write them
   as the 512 comma-separated fields that follow a line's prefix. colmask says
   which columns of the TOP row carry a piece (ROWMASK_FULL for a completed stop
   row, ROWMASK_AB/AC/BC for an --incomplete_top partial); every row below it is
   full either way. A mask rather than a last-placed column because the partial
   kinds leave a hole in the MIDDLE of the row, not only at its right end.
   Returns the byte count. */
static inline bool cell_is_placed(int r, int c, int row, uint16_t colmask) {
    if (r < 0 || r > row) return false;
    return (r < row) || ((colmask >> c) & 1u) != 0;
}

static int format_board_tail(const RowChoice rows[EDGE_LEN + 1], int row,
                             uint16_t colmask, int orient, char *out) {
    uint32_t pos[NUM_PIECES], rot_arr[NUM_PIECES];
    for (int i = 0; i < NUM_PIECES; i++) { pos[i] = 999; rot_arr[i] = 0; }
    for (int r = 0; r <= row; r++) {
        uint16_t m = (r == row) ? colmask : ROWMASK_FULL;
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!((m >> c) & 1u)) continue;
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            pos[pid] = (uint32_t)(r * PUZZLE_SIDE + c); rot_arr[pid] = rot;
        }
    }
    /* --clue_corners: also show the two clue pieces the search never reaches, but
       ONLY where they land in empty space. Isolated they cost nothing and say
       something. Touching a placed cell they would assert an edge the search
       never chose and never scored, so the attach yields to the board: a clue
       whose own cell or whose cell below is filled is left off, and the board is
       emitted as searched. There is no choice of orientation here -- the board
       committed to one when it placed its row-2 corners. */
    if (orient >= 0 && (g_clue_mask & CLUE_CORNERS))
        for (int k = 3; k < CLUE_N; k++) {
            const ClueCell *cc = &g_clue[orient][k];
            if (cell_is_placed(cc->row,     cc->col, row, colmask)) continue;
            if (cell_is_placed(cc->row - 1, cc->col, row, colmask)) continue;
            pos[cc->piece]     = (uint32_t)(cc->row * PUZZLE_SIDE + cc->col);
            rot_arr[cc->piece] = cc->spin;
        }

    char *p = out;
    for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, pos[i]); }
    for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, rot_arr[i]); }
    *p++ = '\n';
    return (int)(p - out);
}

/* Emit one --incomplete_top partial: the parent's ancestry plus the two stop-row
   segments named by colmask (ROWMASK_AB, ROWMASK_AC or ROWMASK_BC); the third
   segment's five columns are left unplaced (pos 999). Only the mv.ci[] slots
   under the mask are read -- the missing segment's are never filled in.
   Called from inside the parallel expansion, so the dedup + write are
   guarded by an OpenMP critical -- but the ancestry walk, the fingerprint and the
   512 field conversions are read-only over expansion-stable state and are nearly
   all of the cost, so they happen BEFORE the lock is taken. A board that then
   loses the dedup was formatted for nothing, which is cheap: that work is
   parallel, whereas everything inside the critical stalls every other thread. */
static void emit_incomplete(const BeamCtx *ctx, const BeamEntry *parent,
                            const RowChoice *mv, int row, uint16_t colmask) {
    if (!g_partial_fp) return;
    RowChoice rows[EDGE_LEN + 1];
    rows[row] = *mv;
    collect_rows(ctx, parent, rows);

    /* Fingerprint over placed cells only: full rows below, plus the masked
       columns of the partial stop row. The mask joins the hash so two kinds can
       never collide even if they somehow placed the same pieces. */
    const uint64_t prime = 1099511628211ULL;
    uint64_t fp = 14695981039346656037ULL;
    fp ^= colmask; fp *= prime;
    for (int r = 0; r <= row; r++) {
        uint16_t m = (r == row) ? colmask : ROWMASK_FULL;
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!((m >> c) & 1u)) continue;
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            fp ^= pid; fp *= prime; fp ^= rot; fp *= prime;
        }
    }
    if (!fp) fp = 1;

    char line[EMIT_LINE_MAX];
    /* The beamer carries an orientation per beam entry because it explores all
       four at once; here the whole line owns one, read off the input board. */
    int len = format_board_tail(rows, row, colmask, g_fin_orient, line);

    #pragma omp critical(e555_incomplete)
    {
        if (g_partial_fp && partial_htable_insert(fp)) {
            if      (colmask == ROWMASK_AB) g_part_ab++;
            else if (colmask == ROWMASK_AC) g_part_ac++;
            else                            g_part_bc++;
            uint64_t sol_idx = g_partial_total++;
            fprintf(g_partial_fp, "%s, %" PRIu64, g_config_id_str, sol_idx);
            fwrite(line, 1, (size_t)len, g_partial_fp);
        }
    }
}

/* BeamEntry.color_consumed is indexed by INNER_IDX and commit_row writes all four
   faces of every placed inner piece without a color_is_inner guard -- the guard
   would cost four branches per piece in the hottest loop in the program. That is
   sound only because an inner piece carries inner colors on every face: the seed
   loader keeps a piece as inner exactly when it has no frame side, and the
   canonical seed's 196 inner pieces have all 784 faces in COLOR_MIN..COLOR_MAX.
   It is a property of the seed file, not of the code, so check it once here
   rather than trusting it. */
static void init_check_inner_faces(void) {
    for (int i = 0; i < g_cat_count; i++) {
        const Oriented *o = &g_cat[i];
        int f[4] = { o->top, o->right, o->bottom, o->left };
        for (int k = 0; k < 4; k++)
            if (!color_is_inner(f[k]))
                fatal("inner piece %u presents non-inner color %d; the beam's "
                      "color counters are indexed over inner colors only",
                      o->piece_id, f[k]);
    }
}

/* -- Mahalanobis scoring (full finite-population covariance) ----------------- */
/* Measures how atypical the placed pieces' inner-color consumption vector is,
 * relative to drawing the same number of pieces uniformly without replacement
 * (hypergeometric): d2n = D^2 / E[D^2], where D^2 is the Mahalanobis distance in
 * the 16-dim Helmert contrast space of the 17 inner-color counts. d2n has
 * expectation 1 for a typical random sample; with lambda > 0 the score REWARDS
 * atypical usage (empirically, exhausting some colors early helps the endgame). */
#define MAHA_DIM 16

static double g_maha_W[MAHA_DIM][NUM_INNER_COLORS];
static double g_maha_Wtotal[MAHA_DIM];
static int    g_maha_M;

static void build_maha_tables(void) {
    int M = g_num_inner, NC = NUM_INNER_COLORS, DM = MAHA_DIM;
    g_maha_M = M;
    static double f[EXPECTED_INNER][NUM_INNER_COLORS];
    memset(f, 0, sizeof f);
    for (int ii = 0; ii < M; ii++) {
        int pid = g_inner_ids[ii];
        int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
        for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) f[ii][INNER_IDX(e[k])] += 1.0;
    }
    double total[NUM_INNER_COLORS] = {0};
    static double cross[NUM_INNER_COLORS][NUM_INNER_COLORS];
    memset(cross, 0, sizeof cross);
    for (int ii = 0; ii < M; ii++)
        for (int ci = 0; ci < NC; ci++) {
            total[ci] += f[ii][ci];
            for (int di = 0; di < NC; di++) cross[ci][di] += f[ii][ci]*f[ii][di];
        }
    static double A[NUM_INNER_COLORS][NUM_INNER_COLORS];
    for (int ci = 0; ci < NC; ci++) for (int di = 0; di < NC; di++)
        A[ci][di] = (double)M*cross[ci][di] - total[ci]*total[di];
    double B[MAHA_DIM][NUM_INNER_COLORS]; memset(B, 0, sizeof B);
    for (int k = 0; k < DM; k++) {
        double s = 1.0 / sqrt((double)(k+1)*(double)(k+2));
        for (int j = 0; j <= k; j++) B[k][j] = s;
        B[k][k+1] = -(double)(k+1)*s;
    }
    static double BA[MAHA_DIM][NUM_INNER_COLORS]; memset(BA, 0, sizeof BA);
    for (int k = 0; k < DM; k++) for (int ci = 0; ci < NC; ci++) for (int di = 0; di < NC; di++)
        BA[k][di] += B[k][ci]*A[ci][di];
    double G0[MAHA_DIM][MAHA_DIM]; memset(G0, 0, sizeof G0);
    for (int k1 = 0; k1 < DM; k1++) for (int k2 = 0; k2 < DM; k2++) for (int di = 0; di < NC; di++)
        G0[k1][k2] += BA[k1][di]*B[k2][di];
    double L[MAHA_DIM][MAHA_DIM]; memcpy(L, G0, sizeof L);
    for (int i = 0; i < DM; i++)
        for (int j = 0; j <= i; j++) {
            double s = L[i][j];
            for (int kk = 0; kk < j; kk++) s -= L[i][kk]*L[j][kk];
            if (i == j) { if (s < 1e-12) fatal("maha: covariance not positive-definite at pivot %d", i); L[i][j] = sqrt(s); }
            else { L[i][j] = s / L[j][j]; L[j][i] = 0.0; }
        }
    for (int c = 0; c < NC; c++) {
        double w[MAHA_DIM];
        for (int k = 0; k < DM; k++) { double s = B[k][c]; for (int j = 0; j < k; j++) s -= L[k][j]*w[j]; w[k] = s/L[k][k]; }
        for (int k = 0; k < DM; k++) g_maha_W[k][c] = w[k];
    }
    for (int k = 0; k < DM; k++) { double s = 0.0; for (int ci = 0; ci < NC; ci++) s += g_maha_W[k][ci]*total[ci]; g_maha_Wtotal[k] = s; }
}

/* The RAW statistic: d2n, with no weight and no row schedule.
 *
 * The schedule this replaced returned 0 outside rows 3..9 and peaked at row 6.
 * That was the wrong shape twice over. Selection only binds where the deduped
 * pool exceeds the row width -- measured at rows 4..7, since select_beam returns
 * every candidate untouched when kept <= K -- so the row-3 and row-8/9 tails
 * were weightless anyway, and killing the term at rows 10+ removed nothing that
 * was acting. What the term actually needed was not a schedule but a common
 * SCALE: d2n's spread varies by row (measured ~0.44 to ~0.73), so a fixed
 * coefficient means a different effective weight at every depth. color_term
 * divides that spread out instead. */
static inline double maha_d2n(const BeamEntry *t, int row) {
    int n = EDGE_LEN * row, M = g_maha_M;
    double fn = (double)n*(double)(M-n) / ((double)M*(double)M*(double)(M-1));
    if (fn <= 0.0) return 0.0;
    double n_over_M = (double)n/(double)M, d2 = 0.0;
    for (int k = 0; k < MAHA_DIM; k++) {
        double wx = -n_over_M*g_maha_Wtotal[k];
        for (int ci = 0; ci < NUM_INNER_COLORS; ci++) wx += g_maha_W[k][ci]*(double)t->color_consumed[ci];
        d2 += wx*wx;
    }
    return d2 / (fn*16.0);
}

/* Live per-row normalisation of d2n.
 *
 * The hybrid wants the Mahalanobis correction to carry a FIXED spread in score
 * units at every depth -- lambda_Mahalanobis is read as a score standard
 * deviation, not as a raw coefficient. That needs sigma_r, which is measured
 * rather than tabulated: every scored candidate feeds a padded per-thread
 * accumulator, and expand_row reduces them at the end of the row. Scoring row r
 * therefore uses row r-1's spread.
 *
 * Measuring beats freezing a table here. A frozen table is a calibration tied to
 * the seed, the borders and the beam shape, and it goes stale silently when any
 * of them move -- which they will, since the generation changes in this same
 * commit are meant to move exactly the rows this is measured over. The cost is
 * that r-1's sigma is an approximation to r's: adjacent rows differ by ~5-6% in
 * the reference table, well inside the useful band for lambda. Rows with no
 * usable measurement return 0, so the term simply stands down. */
#define MAX_ACC_THREADS 256
#define ACC_STRIDE      8               /* one cache line per thread, no sharing */
#define MAHA_MIN_SAMPLES 64.0           /* below this a spread is mostly noise */
static double g_d2n_acc[MAX_ACC_THREADS][ACC_STRIDE];   /* [0]=sum [1]=sumsq [2]=n */
static double g_maha_mean[EDGE_LEN + 2];
static double g_maha_sd[EDGE_LEN + 2];  /* 0 = row never measured */
static double g_maha_n[EDGE_LEN + 2];   /* samples behind the stored estimate */

static inline void maha_acc(double d2n) {
    int th = omp_get_thread_num();
    if (th >= MAX_ACC_THREADS) return;          /* absurd --threads: stand down */
    g_d2n_acc[th][0] += d2n;
    g_d2n_acc[th][1] += d2n * d2n;
    g_d2n_acc[th][2] += 1.0;
}

/* Reduce the row's accumulators and reset them. Summed in a FIXED thread order,
   so the result does not depend on how the row's work happened to be scheduled;
   it still depends on the thread COUNT, as every other float sum here does. */
static void maha_close_row(int row) {
    double sum = 0.0, sumsq = 0.0, n = 0.0;
    for (int th = 0; th < MAX_ACC_THREADS; th++) {
        sum += g_d2n_acc[th][0]; sumsq += g_d2n_acc[th][1]; n += g_d2n_acc[th][2];
        g_d2n_acc[th][0] = g_d2n_acc[th][1] = g_d2n_acc[th][2] = 0.0;
    }
    if (row < 0 || row > EDGE_LEN + 1) return;
    /* Too small a sample to RE-estimate the spread -- so keep the estimate that
       is already there. Zeroing it here was a bug, and a bad one for a tool that
       runs many small configurations over one database: a single narrow config
       wiped the calibration a wider one had measured, and every config after it
       scored with the correction stood down. An old estimate of this row's
       spread beats no estimate; the sample count travels with it so --verbose
       can show how much weight it carries. */
    if (n < MAHA_MIN_SAMPLES) return;
    double mean = sum / n;
    double var  = sumsq / n - mean * mean;
    g_maha_mean[row] = mean;
    g_maha_sd[row]   = (var > 1e-18) ? sqrt(var) : 0.0;
    g_maha_n[row]    = n;
}

/* Which row's spread normalises row `row`.
 *
 * The beamer can simply use row-1: it searches from row 1 upwards, so by the
 * time it scores row r it has just measured r-1. The finalizer cannot. Its
 * first searched row is finalize_from+1 and everything below is locked, so
 * row-1 was never searched and never measured -- and with the default
 * finalize_from that is row 9 normalising against row 8, which has no sample at
 * all. A run short enough to search only two rows therefore never applied the
 * correction once, whatever --lambda_Mahalanobis said.
 *
 * So: prefer THIS row's own spread, which a previous configuration over the same
 * database will have measured (the table deliberately outlives a config), then
 * the row below, then the nearest measured row in either direction. Same-row is
 * the better normaliser anyway -- it is the population being scored rather than
 * an adjacent approximation to it. Only the very first configuration of a run
 * scores its first row uncorrected, and it is the one row nothing can calibrate.
 *
 * Deterministic for a fixed run: the table changes only between configurations,
 * which execute in a fixed order, never within a row. */
static inline int maha_ref_row(int row) {
    if (row >= 1 && row <= EDGE_LEN + 1 && g_maha_sd[row] > 0.0) return row;
    for (int r = row - 1; r >= 1; r--)             if (g_maha_sd[r] > 0.0) return r;
    for (int r = row + 1; r <= EDGE_LEN + 1; r++)  if (g_maha_sd[r] > 0.0) return r;
    return -1;
}

/* -- The closure objective: exact pairing combinatorics (--lambda_J) --------- */
/* Every free inner half-edge must eventually meet another of the SAME color. Of
 * the (2A-1)!! ways to pair up 2A = sum_c S_c free half-edges, prod_c (S_c-1)!!
 * are color-consistent, so
 *
 *     P = prod_c (S_c-1)!! / (2A-1)!!
 *
 * which is zero exactly when some S_c is odd -- parity_ok's evenness test and
 * this objective are one formula, hard part and soft part. Stirling turns its
 * log into -A*H(pi), i.e. (up to a constant at fixed depth)
 *
 *     J_conc = A_tot * KL(pi || uniform),   pi_c = S_c / sum_d S_d
 *
 * "how far this board's remaining color mix has drifted from flat, weighted by
 * how many pairings are left to make". It is convex, so it rewards EXTREME
 * profiles -- exhausting some colors -- without ever naming which: at a balanced
 * start it is identically zero and has no preference to express. J_dem is the
 * companion term for demands already committed (frontier tops, side interfaces,
 * top border): each must find a half-edge of its color in the residual supply R,
 * which is a safety rail against burning a color the frontier still needs.
 *
 * Both are written centred (S_c/Sbar, R_c/Rbar). Both sums are constant across
 * siblings at a given row, so centring cannot change the ranking; it keeps the
 * value near 0 instead of near 1600 nats, so any swept weight keeps its
 * meaning when the weights are swept.
 *
 * This matters more here than in the beamer: maha_d2n infers the placed-piece
 * count from n = 14*row, whereas closure reads S and R straight off the board,
 * so it stays exact from a locked partial at any --finalize_from. */
#define LOGTAB_N 801                    /* > 784 = every inner half-edge */
static double g_logtab[LOGTAB_N];

static void build_logtab(void) {
    g_logtab[0] = 0.0;                  /* the x*log(x) -> 0 limit at x = 0 */
    for (int i = 1; i < LOGTAB_N; i++) g_logtab[i] = log((double)i);
}

static inline double closure_raw(const BeamEntry *t) {
    int S[NUM_INNER_COLORS], R[NUM_INNER_COLORS], D[NUM_INNER_COLORS];
    int sumS = 0, sumR = 0;
    for (int ci = 0; ci < NUM_INNER_COLORS; ci++) {
        int c = COLOR_MIN + ci;
        R[ci] = g_inner_color_total[c] - t->color_consumed[INNER_IDX(c)];
        D[ci] = t->req_exposed[INNER_IDX(c)];
        S[ci] = R[ci] - D[ci];
        sumR += R[ci]; sumS += S[ci];
    }
    const double logSbar = (sumS > 0) ? log((double)sumS / NUM_INNER_COLORS) : 0.0;
    const double logRbar = (sumR > 0) ? log((double)sumR / NUM_INNER_COLORS) : 0.0;
    double conc = 0.0, dem = 0.0;
    for (int ci = 0; ci < NUM_INNER_COLORS; ci++) {
        if (S[ci] > 0) conc += (double)S[ci] * (g_logtab[S[ci]] - logSbar);
        if (D[ci] > 0) dem  += (double)D[ci] * (g_logtab[R[ci]] - logRbar);
    }
    return 0.5 * conc + dem;
}

/* The colour part of the score: closure, plus a small piece-structure correction.
 *
 *     lambda_J * C(t)  +  lambda_Mahalanobis * (d2n - mu_r) / sigma_r
 *
 * Closure is the primary objective -- it is demand-aware, it is a log
 * probability so it adds to the fan-out log count in the same units, and a
 * sweep put it 2.64x ahead of no colour term on 20 of 20 paired configs.
 * Mahalanobis is the correction, not a co-equal term: the two correlate ~0.88,
 * so most of what it adds repeats closure, and the genuinely new signal -- the
 * part that knows half-edges arrive four at a time on indivisible pieces --
 * has a spread of only ~0.29 nats at the recommended weight.
 *
 * Both terms are always live. A model is chosen by zeroing a weight, which is
 * why there is no --score_model any more: --lambda_Mahalanobis 0 is closure
 * alone, --lambda_J 0 is Mahalanobis alone.
 *
 * The d2n sample is taken for EVERY scored candidate, whether or not the
 * correction can be normalised yet -- that is what gives the next row its
 * sigma. It is also taken before the --bc_window best child is chosen, so the
 * population measured is the population the score ranks.
 *
 * (--avail_correct used to be added here. It is gone: it lost 61 of 61 paired
 * configs, and cost 32% against closure and 13% against the hybrid. It rewards
 * holding abundant frontier colours, while closure often wants to spend a
 * colour whose demand is already covered -- they pull against each other.) */
static inline double color_term(const BeamEntry *t, int row) {
    double s = g_lambda_J * closure_raw(t);
    if (g_lambda_maha != 0.0) {
        double d2n = maha_d2n(t, row);
        maha_acc(d2n);
        int ref = maha_ref_row(row);
        if (ref >= 0) s += g_lambda_maha * (d2n - g_maha_mean[ref]) / g_maha_sd[ref];
    }
    return s;
}

/* -- Scoring (one-row lookahead + heuristic terms) --------------------------- */

/* The child's exposed tops key the next row. Segment A's next cell uses the
   FIXED next left-column color (exact); B and C use the left-agnostic fan-out
   table (their next left neighbour is unknown until A/B are chosen). Any
   NULL/zero is an exact one-row death -> the child is rejected. Applied on beam
   rows only: stop-row boards are emitted without this gate (see try_A). */
static bool score_child(const BeamEntry *t, int row, float *out) {
    const uint8_t *rt = t->rtop;
    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(rt[c])) return false;
    if (!color_is_edge_iface(rt[15])) return false;

    int la = g_cur_left->right[row + 1];
    if (!color_is_inner(la)) return false;
    const Cell *cA = g_db[INNER_IDX(la)][INNER_IDX(rt[1])][INNER_IDX(rt[2])]
                         [INNER_IDX(rt[3])][INNER_IDX(rt[4])][rt[5]];
    if (!cA) return false;
    uint64_t fB = db_seg_fanout(rt[6], rt[7], rt[8], rt[9], rt[10]);
    if (fB == 0) return false;
    uint64_t fC = db_seg_fanout(rt[11], rt[12], rt[13], rt[14], rt[15]);
    if (fC == 0) return false;

    /* log(a) + log1p(b) + log1p(c) = log(a*(1+b)*(1+c)) in R: one log instead of
       three, same objective. All three factors are positive integers here (a
       cell exists only if it holds records; both fan-outs are non-zero by the
       gates above), so the product is >= 4 and log is safely in range. */
    double s = log((double)cA->n * (1.0 + (double)fB) * (1.0 + (double)fC))
               + color_term(t, row);
    *out = (float)s;
    return true;
}

static inline uint64_t frontier_sig(const BeamEntry *b) {
    uint64_t h = 0x6BEA6BEA6BEA6BEAULL, w;
    memcpy(&w, &b->rtop[0], 8); h = splitmix64(h ^ w);
    memcpy(&w, &b->rtop[8], 8); h = splitmix64(h ^ w);
    for (int k = 0; k < 4; k++) h = splitmix64(h ^ b->used[k]);
    return h ? h : 1;
}

/* -- Beam expansion --------------------------------------------------------- */

static bool g_pool_clip_warned = false;

static bool expand_prepare(Expand *e, const BeamEntry *p, uint32_t pi, int row, bool at_stop) {
    e->parent = p; e->parent_idx = pi; e->row = row; e->at_stop = at_stop;

    /* The exact used-mask test (masks_intersect4) in try_A / pick_segB /
       pick_segC is the sole, authoritative availability check. */
    int la = g_cur_left->right[row];
    if (!color_is_inner(la)) return false;
    e->la_A = la;
    const uint8_t *rt = p->rtop;
    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(rt[c])) return false;
    if (!color_is_edge_iface(rt[15])) return false;

    e->cA = g_db[INNER_IDX(la)][INNER_IDX(rt[1])][INNER_IDX(rt[2])]
                [INNER_IDX(rt[3])][INNER_IDX(rt[4])][rt[5]];
    return e->cA != NULL;
}

/* Buffered pool append: one atomic reservation per POOL_BATCH children. */
static inline void pool_flush(BeamCtx *ctx, Scratch *sc) {
    if (!sc->buf_n) return;
    uint64_t pos;
    #pragma omp atomic capture
    { pos = ctx->pool_n; ctx->pool_n += sc->buf_n; }
    uint64_t nput = 0;
    if (pos < ctx->pool_cap) {
        nput = sc->buf_n;
        if (pos + nput > ctx->pool_cap) nput = ctx->pool_cap - pos;
        memcpy(&ctx->pool[pos], sc->buf, (size_t)nput * sizeof(PoolEntry));
    }
    if (nput < sc->buf_n && !g_pool_clip_warned) {
        g_pool_clip_warned = true;
        fprintf(stderr, "[warn] candidate pool full (cap %" PRIu64 "); surplus children dropped"
                        " -- consider a larger --pool_factor\n", ctx->pool_cap);
    }
    sc->buf_n = 0;
}
static inline void pool_append(BeamCtx *ctx, Scratch *sc, const BeamEntry *t,
                               uint32_t parent_idx, float score, const RowChoice *mv) {
    PoolEntry *pe = &sc->buf[sc->buf_n++];
    pe->score = score; pe->parent = parent_idx; pe->sig = frontier_sig(t); pe->mv = *mv;
    if (sc->buf_n == POOL_BATCH) pool_flush(ctx, sc);
}

/* Decode segment B record jb at cell cB; returns true and fills ciB/la_C for the
   first record that decodes and passes the exact forbid-mask test. bottoms = the
   five raw bottom colors of segment B (= the cell key, p->rtop[6..10]). */
static inline bool pick_segB(const Cell *cB, uint32_t jb, const uint8_t bottoms[],
                             const uint64_t forbid[4], uint16_t ciB[CHAIN_LEN],
                             int la_B, int *la_C_out) {
    uint32_t w = rec_load(cB->rec, jb, g_rec_bytes_inner);
    uint8_t f[CHAIN_LEN]; unpack_inner(w, f, g_lb_bits);
    uint64_t mask[4] = {0,0,0,0};
    if (!decode_inner_chain(f, CHAIN_LEN, la_B, bottoms, ciB, mask, forbid)) return false;
    if (masks_intersect4(forbid, mask)) return false;
    *la_C_out = g_cat[ciB[CHAIN_LEN-1]].right;
    return true;
}

/* Decode segment C record jc (4 inner + terminal) at cell cC. bottoms = the four
   inner bottom colors (p->rtop[11..14]); the terminal is recovered from its index
   within the per-left bucket of the 4th inner's exposed right color. */
static inline bool pick_segC(const Cell *cC, uint32_t jc, const uint8_t bottoms[],
                             const uint64_t forbid[4], uint16_t ciC[CHAIN_LEN-1],
                             int la_C, uint8_t *rterm_out) {
    uint32_t w = rec_load(cC->rec, jc, g_rec_bytes_edge);
    uint8_t f4[CHAIN_LEN-1]; int term_k; unpack_edge(w, f4, &term_k, g_lb_bits, g_term_bits);
    uint64_t mask[4] = {0,0,0,0};
    if (!decode_inner_chain(f4, CHAIN_LEN-1, la_C, bottoms, ciC, mask, forbid)) return false;
    int cl = g_cat[ciC[CHAIN_LEN-2]].right;          /* terminal's required left color */
    if (cl < 0 || cl >= NUM_COLORS_TOTAL || term_k >= g_edge_term_by_left_n[cl]) return false;
    int t = g_edge_term_by_left[cl][term_k];
    const Oriented *term = &g_edge_term[t];
    mask[term->piece_id >> 6] |= piece_bit(term->piece_id);
    if (masks_intersect4(forbid, mask)) return false;
    *rterm_out = (uint8_t)t;
    return true;
}

/* Segment-B cell for a decoded A chain (la_B = A's exposed right color). */
static inline const Cell *segB_cell(const BeamEntry *p, int la_B) {
    const uint8_t *rt = p->rtop;
    return g_db[INNER_IDX(la_B)][INNER_IDX(rt[6])][INNER_IDX(rt[7])]
               [INNER_IDX(rt[8])][INNER_IDX(rt[9])][rt[10]];
}

/* OR the inner pieces of a decoded chain into a mask. */
static inline void mask_of_chain(const uint16_t ci[CHAIN_LEN], int count, uint64_t mask[4]) {
    mask[0]=mask[1]=mask[2]=mask[3]=0;
    for (int i = 0; i < count; i++) { uint16_t pid = g_cat[ci[i]].piece_id; mask[pid>>6] |= piece_bit(pid); }
}

/* A+C partials (--incomplete_top): segment B is missing, so segment C has lost
   its left key -- B's exposed right color -- and every inner color is a
   candidate. Its bottom rt[11..15] is still pinned by the parent and the right
   edge still falls out of the cell, so this is the ordinary pick_segC scan run
   once per possible left color, against forbid = parent's used set plus segment
   A. One db_seg_fanout() load is the count over all 17 lefts, so a zero there
   skips the whole walk. */
static void emit_AC_partials(const BeamCtx *ctx, const BeamEntry *p, RowChoice *mv,
                             int row, const uint64_t forbid[4]) {
    const uint8_t *rt = p->rtop;
    if (db_seg_fanout(rt[11], rt[12], rt[13], rt[14], rt[15]) == 0) return;
    for (int lci = 0; lci < DIM_INNER && !g_stop; lci++) {
        const Cell *cC = g_db[lci][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                             [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
        if (!cC) continue;
        for (uint32_t jc = 0; jc < cC->n; jc++) {
            uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
            if (!pick_segC(cC, jc, rt + 11, forbid, ciC, lci + COLOR_MIN, &rterm)) continue;
            memcpy(&mv->ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
            mv->rterm = rterm;
            emit_incomplete(ctx, p, mv, row, ROWMASK_AC);
        }
    }
}

/* B+C partials (--incomplete_top): segment A is missing, so segment B has lost
   its left key -- the border column's exposed right color -- and every inner
   color is a candidate; C then chains off B exactly as it always does. This is
   the only partial kind that needs no segment A, so it is driven per PARENT
   rather than per A record: it is what rescues the boards whose segment-A cell
   is empty or wholly conflicted, which try_A never even reaches. */
static void try_BC(const BeamCtx *ctx, const BeamEntry *p, int row) {
    const uint8_t *rt = p->rtop;
    /* Only B's and C's bottoms constrain such a board: rt[1..5] lie under the
       hole and the border column's color is irrelevant once A is gone. */
    for (int c = 6; c <= 14; c++) if (!color_is_inner(rt[c])) return;
    if (!color_is_edge_iface(rt[15])) return;
    if (db_seg_fanout(rt[6], rt[7], rt[8], rt[9], rt[10]) == 0) return;

    RowChoice mv;
    for (int lbi = 0; lbi < DIM_INNER && !g_stop; lbi++) {
        const Cell *cB = g_db[lbi][INNER_IDX(rt[6])][INNER_IDX(rt[7])]
                             [INNER_IDX(rt[8])][INNER_IDX(rt[9])][rt[10]];
        if (!cB) continue;
        for (uint32_t jb = 0; jb < cB->n && !g_stop; jb++) {
            uint16_t ciB[CHAIN_LEN]; int la_C;
            if (!pick_segB(cB, jb, rt + 6, p->used, ciB, lbi + COLOR_MIN, &la_C)) continue;
            if (!color_is_inner(la_C)) continue;
            const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                 [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
            if (!cC) continue;
            uint64_t maskB[4], forbidB[4];
            mask_of_chain(ciB, CHAIN_LEN, maskB);
            for (int k = 0; k < 4; k++) forbidB[k] = p->used[k] | maskB[k];
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            for (uint32_t jc = 0; jc < cC->n; jc++) {
                uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
                if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) continue;
                memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
                mv.rterm = rterm;
                emit_incomplete(ctx, p, &mv, row, ROWMASK_BC);
            }
        }
    }
}

/* Expand one segment-A record into pool entries. Beam rows keep one child per A
   record: the first conflict-free C under the first workable of up to B_TRY
   conflict-free B chains. The stop row emits every conflict-free completion
   until the work item's quota is spent. */
static void try_A(Expand *e, BeamCtx *ctx, uint32_t j, Scratch *sc) {
    const BeamEntry *p = e->parent;
    const uint8_t *rt = p->rtop;
    uint32_t w = rec_load(e->cA->rec, j, g_rec_bytes_inner);
    uint8_t fA[CHAIN_LEN]; unpack_inner(w, fA, g_lb_bits);
    uint16_t ciA[CHAIN_LEN]; uint64_t maskA[4] = {0,0,0,0};
    if (!decode_inner_chain(fA, CHAIN_LEN, e->la_A, rt + 1, ciA, maskA, p->used)) return;  /* bottoms rt[1..5] */
    if (masks_intersect4(p->used, maskA)) return;

    e->budget--;                                   /* a real decode attempt */

    int la_B = g_cat[ciA[CHAIN_LEN-1]].right;
    const Cell *cB = color_is_inner(la_B) ? segB_cell(p, la_B) : NULL;
    /* No B cell at all. Below the stop row that kills the child; at the stop row
       a missing B is exactly what an A+C partial records, so fall through. */
    if (!cB && !(e->at_stop && g_incomplete_top)) return;

    uint64_t forbidA[4] = { p->used[0]|maskA[0], p->used[1]|maskA[1],
                            p->used[2]|maskA[2], p->used[3]|maskA[3] };
    BeamEntry *t = &sc->tmp;
    RowChoice mv;
    memcpy(&mv.ci[0], ciA, CHAIN_LEN * sizeof(uint16_t));

    if (e->at_stop) {
        for (uint32_t jb = 0; cB && jb < cB->n && e->quota > 0; jb++) {
            uint16_t ciB[CHAIN_LEN]; int la_C;
            if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) continue;
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            if (!color_is_inner(la_C)) {
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row, ROWMASK_AB);
                continue;
            }
            const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                 [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
            if (!cC) {
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row, ROWMASK_AB);
                continue;
            }
            uint64_t maskB[4], forbidB[4];
            mask_of_chain(ciB, CHAIN_LEN, maskB);
            for (int k = 0; k < 4; k++) forbidB[k] = forbidA[k] | maskB[k];
            bool ab_full = false;
            for (uint32_t jc = 0; jc < cC->n && e->quota > 0; jc++) {
                uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
                if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) continue;
                memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
                mv.rterm = rterm;
                *t = *p; commit_row(t, e->row, &mv);
                if (!parity_ok(t)) continue;
                /* No lookahead gate at the stop row: every board that completes
                   it is emitted -- whether a row fits above is deliberately the
                   next stage's problem. Rank by the heuristic terms only. */
                float score = (float)color_term(t, e->row);
                pool_append(ctx, sc, t, e->parent_idx, score, &mv);
                e->quota--;
                ab_full = true;
            }
            /* Valid A+B but no completable C: keep the A+B partial if requested. */
            if (g_incomplete_top && !ab_full) emit_incomplete(ctx, p, &mv, e->row, ROWMASK_AB);
        }
        if (g_incomplete_top) emit_AC_partials(ctx, p, &mv, e->row, forbidA);
        return;
    }
    if (!cB) return;                    /* stop-row-only fall-through ends above */

    /* Beam row: enumerate EVERY conflict-free (B, C) completion of this A
       record, quota permitting -- unlike the beamer, which keeps one child per
       record from the first few B chains. The beamer's economy is right for
       its dense cells (mean ~140 records) and wide beams; here the beam grows
       from a SINGLE locked board over a sparse reduced database (cell mean of
       a few records), so per-record exhaustiveness is cheap and decisive: it
       preserves every completion that actually exists, instead of letting an
       unlucky early B choice starve the pool to extinction. This is also why
       the beamer's --bc_window has no counterpart here: the window exists to
       stop taking the first (B, C) that fits, and this loop never did. */
    for (uint32_t jb = 0; jb < cB->n && e->quota > 0; jb++) {
        uint16_t ciB[CHAIN_LEN]; int la_C;
        if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) continue;
        if (!color_is_inner(la_C)) continue;
        const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                             [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
        if (!cC) continue;
        uint64_t maskB[4]; mask_of_chain(ciB, CHAIN_LEN, maskB);
        uint64_t forbidB[4] = { forbidA[0]|maskB[0], forbidA[1]|maskB[1],
                                forbidA[2]|maskB[2], forbidA[3]|maskB[3] };
        memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
        for (uint32_t jc = 0; jc < cC->n && e->quota > 0; jc++) {
            uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
            if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) continue;
            memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
            mv.rterm = rterm;
            *t = *p; commit_row(t, e->row, &mv);
            if (!parity_ok(t)) continue;
            float score;
            if (!score_child(t, e->row, &score)) continue;
            pool_append(ctx, sc, t, e->parent_idx, score, &mv);
            e->quota--;
        }
    }
}

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* Pins owed by `row` under the line's orientation. pin_idx[s] is the position
   within segment s (0=A, 1=B, 2=C) that is nailed down, or -1. Returns true if
   the row owes anything, in which case it must go through fin_expand_clued.

   Two rules, and the second is the one that is easy to miss: a clue ON this row
   fixes a piece, and a clue on the row ABOVE fixes a colour HERE, because a
   clue's bottom face has to meet whatever sits under it. Driving both off the
   table covers every clue the search actually places: row 1 under the row-2
   corners, and the centre clue's row 6-or-7 -- exactly the row the shipped
   pipelines free. Filtering afterwards is not an alternative: 22 surviving
   boards against 3328 when the beamer tried that on the two-colour case.

   Entries 3..4 pin NOTHING: they sit on row 13, which is never searched, so they
   are only reserved, and a clue that is never placed must not constrain the row
   below it. A pinned row goes through fin_expand_clued, which never reaches the
   --incomplete_top emitters, so pinning row 12 quietly discarded every 11-of-16
   board at the hardest row in the search.

   Only one pin per segment is representable. Within a single orientation no row
   ever wants two -- rows 2 and 13 put their pair in segments A and C, the centre
   is alone in B -- and init asserts the table shape that guarantees it. */
static bool fin_clue_pins_for(int row, int pin_idx[3], int pin_kind[3], uint16_t pin_val[3]) {
    pin_idx[0] = pin_idx[1] = pin_idx[2] = -1;
    pin_kind[0] = pin_kind[1] = pin_kind[2] = PIN_PIECE;
    pin_val[0] = pin_val[1] = pin_val[2] = 0;
    if (!g_clue_mask || g_fin_orient < 0) return false;

    bool any = false;
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_clue[g_fin_orient][k];
        int c = -1, kind = PIN_PIECE; uint16_t val = 0;
        if (k >= CLUE_N_REACHABLE) continue;         /* row 13: never searched */
        if (cc->row == row) {                       /* the clue itself */
            c = cc->col; kind = PIN_PIECE; val = g_clue_ci[g_fin_orient][k];
        } else if (cc->row == row + 1) {
            c = cc->col; kind = PIN_TOPCOLOR;       /* the colour it will sit on */
            val = g_cat[g_clue_ci[g_fin_orient][k]].bottom;
        }
        if (c < 1 || c > EDGE_LEN) continue;        /* cols 1..14 are the segments */
        int s = (c - 1) / CHAIN_LEN;
        if (pin_idx[s] >= 0) fatal("clue table: row %d wants two pins in segment %d", row, s);
        pin_idx[s] = (c - 1) % CHAIN_LEN; pin_kind[s] = kind; pin_val[s] = val;
        any = true;
    }
    return any;
}

/* E555_CLUE_DEBUG=1: what every searched row owes, once per input line. A row
   that owes a pin is generated by fin_expand_clued, which does NOT run the
   --incomplete_top emitters, so "does row 12 owe anything" decides whether the
   hardest row in the search can contribute partials at all. Printing it beats
   inferring it from a run that has to survive to row 12 first. */
static void fin_clue_dump_schedule(void) {
    if (!g_clue_mask || !g_clue_debug || g_fin_orient < 0) return;
    printf("[clue] orientation %d; rows %u..%u searched\n",
           g_fin_orient, g_finalize_from + 1, g_stop_row);
    for (uint32_t r = g_finalize_from + 1; r <= g_stop_row; r++) {
        int pi[3], pk[3]; uint16_t pv[3];
        if (!fin_clue_pins_for((int)r, pi, pk, pv)) { printf("[clue]   row %2u: free\n", r); continue; }
        printf("[clue]   row %2u:", r);
        for (int s = 0; s < 3; s++) {
            if (pi[s] < 0) continue;
            if (pk[s] == PIN_PIECE) printf("  seg%c[%d] = piece %u", 'A' + s, pi[s], g_cat[pv[s]].piece_id);
            else                    printf("  seg%c[%d] = colour %u below a clue", 'A' + s, pi[s], pv[s]);
        }
        printf("\n");
    }
    fflush(stdout);
}

/* One parent's expansion of a row that carries pins. Every segment goes through
   the pinned walk -- the same walk that provably reproduces a database cell when
   unpinned -- so pinned and free segments share one path, and a pinned segment A
   does not need the parent's A cell to exist at all (no stored chain holds a clue
   piece, they are excluded from the database). Clue rows are few and heavily
   constrained, so losing the cell's fan-out promise ordering here costs little.

   Every loop level spends budget, not just the outermost: quota only falls on
   ACCEPTED children, so a parent whose candidates all fail parity or the
   lookahead would otherwise walk the whole nA x nB x nC x terminals product. */
static void fin_expand_clued(BeamCtx *ctx, const BeamEntry *p, uint32_t pi, int row,
                             bool at_stop, Scratch *sc, uint32_t quota, uint64_t budget,
                             const int pin_idx[3], const int pin_kind[3],
                             const uint16_t pin_val[3]) {
    const uint8_t *rt = p->rtop;
    int la_A = g_cur_left->right[row];
    if (!color_is_inner(la_A)) return;
    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(rt[c])) return;
    if (!color_is_edge_iface(rt[15])) return;

    BeamEntry *t = &sc->tmp;
    RowChoice mv;

    int nA = enumerate_pinned_segment(la_A, rt + 1, CHAIN_LEN, pin_idx[0], pin_kind[0],
                                      pin_val[0], p->used, sc->seg[0], CLUE_SEG_CAP);
    if (g_clue_debug) fprintf(stderr, "[clue] row %d parent %u la_A=%d nA=%d\n", row, pi, la_A, nA);
    for (int ia = 0; ia < nA && quota > 0 && budget > 0; ia++) {
        const uint16_t *A = sc->seg[0][ia];
        uint64_t mA[4], fA[4];
        mask_of_chain(A, CHAIN_LEN, mA);
        for (int k = 0; k < 4; k++) fA[k] = p->used[k] | mA[k];
        int la_B = g_cat[A[CHAIN_LEN-1]].right;
        if (!color_is_inner(la_B)) continue;
        budget--;

        int nB = enumerate_pinned_segment(la_B, rt + 6, CHAIN_LEN, pin_idx[1], pin_kind[1],
                                          pin_val[1], fA, sc->seg[1], CLUE_SEG_CAP);
        if (g_clue_debug && ia < 4)
            fprintf(stderr, "[clue]   ia=%d la_B=%d nB=%d\n", ia, la_B, nB);
        for (int ib = 0; ib < nB && quota > 0 && budget > 0; ib++) {
            budget--;
            const uint16_t *B = sc->seg[1][ib];
            uint64_t mB[4], fB[4];
            mask_of_chain(B, CHAIN_LEN, mB);
            for (int k = 0; k < 4; k++) fB[k] = fA[k] | mB[k];
            int la_C = g_cat[B[CHAIN_LEN-1]].right;
            if (!color_is_inner(la_C)) continue;

            int nC = enumerate_pinned_segment(la_C, rt + 11, CHAIN_LEN-1, pin_idx[2], pin_kind[2],
                                              pin_val[2], fB, sc->seg[2], CLUE_SEG_CAP);
            if (g_clue_debug && ia < 3 && ib < 3)
                fprintf(stderr, "[clue]     ia=%d ib=%d la_C=%d nC=%d\n", ia, ib, la_C, nC);
            for (int ic = 0; ic < nC && quota > 0 && budget > 0; ic++) {
                budget--;
                const uint16_t *C = sc->seg[2][ic];
                uint64_t mC[4], fC[4];
                mask_of_chain(C, CHAIN_LEN-1, mC);
                for (int k = 0; k < 4; k++) fC[k] = fB[k] | mC[k];
                int cl = g_cat[C[CHAIN_LEN-2]].right;
                if (cl < 0 || cl >= NUM_COLORS_TOTAL) continue;
                /* The right edge is not searched: it falls out of whichever
                   terminal matches C's exposed right and the frontier below. */
                for (int kt = 0; kt < g_edge_term_by_left_n[cl] && quota > 0 && budget > 0; kt++) {
                    budget--;
                    int ti = g_edge_term_by_left[cl][kt];
                    const Oriented *term = &g_edge_term[ti];
                    if (term->bottom != rt[15]) continue;
                    uint16_t tp = term->piece_id;
                    if (fC[tp >> 6] & piece_bit(tp)) continue;

                    memcpy(&mv.ci[0],           A, CHAIN_LEN * sizeof(uint16_t));
                    memcpy(&mv.ci[CHAIN_LEN],   B, CHAIN_LEN * sizeof(uint16_t));
                    memcpy(&mv.ci[2*CHAIN_LEN], C, (CHAIN_LEN-1) * sizeof(uint16_t));
                    mv.rterm = (uint8_t)ti;

                    *t = *p; commit_row(t, row, &mv);
                    if (!parity_ok(t)) continue;
                    float score;
                    if (at_stop) {
                        score = (float)color_term(t, row);
                    } else if (!score_child(t, row, &score)) {
                        continue;
                    }
                    pool_append(ctx, sc, t, pi, score, &mv);
                    quota--;
                }
            }
        }
    }
}

static void expand_row(BeamCtx *ctx, const BeamEntry *beam, uint32_t beam_n,
                       int row, uint64_t cfg_hash, Scratch **scratch) {
    ctx->pool_n = 0;
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();

    uint32_t n_slices = 1;
    if (beam_n < 8u*(uint32_t)nt) {
        n_slices = (8u*(uint32_t)nt) / beam_n;
        if (n_slices == 0) n_slices = 1;
        if (n_slices > 1024u) n_slices = 1024u;
    }
    uint64_t pool_target = (uint64_t)g_pool_factor * g_beam_width;
    uint32_t quota_parent = (uint32_t)(pool_target / beam_n);
    if (quota_parent < 8) quota_parent = 8;
    uint64_t budget_parent = (uint64_t)quota_parent * SCAN_FACTOR;
    if (budget_parent < MIN_DECODE_BUDGET) budget_parent = MIN_DECODE_BUDGET;
    /* Slicing is a pure parallel decomposition: both scan phases below are
       strided by n_slices and the quota/budget are divided by it, so a parent is
       given the same total effort however many slices it is cut into -- and so
       however many threads the run has. Capping the slice count by the quota
       keeps the two floors just below from rounding that total up. */
    if (n_slices > quota_parent) n_slices = quota_parent;
    uint32_t quota_slice = quota_parent / n_slices; if (quota_slice == 0) quota_slice = 1;
    uint64_t budget_slice = budget_parent / n_slices; if (budget_slice < 64) budget_slice = 64;
    const bool at_stop = ((uint32_t)row == g_stop_row);

    /* Does this row owe a pin? Resolved once for the whole row: the line has a
       single orientation, so unlike the beamer there is nothing per-parent here. */
    int pin_idx[3], pin_kind[3]; uint16_t pin_val[3];
    const bool clue_row = fin_clue_pins_for(row, pin_idx, pin_kind, pin_val);

    uint64_t items = (uint64_t)beam_n * n_slices;
    #pragma omp parallel for schedule(dynamic, 8) num_threads(nt)
    for (uint64_t it = 0; it < items; it++) {
        if (g_stop) continue;
        uint32_t pi = (uint32_t)(it / n_slices);
        uint32_t sl = (uint32_t)(it % n_slices);
        Scratch *sc = scratch[omp_get_thread_num()];
        /* A pinned row is generated ONLY by the pinned walk, which enumerates a
           segment in full rather than sampling a database cell -- so it runs once
           per parent and the other slices sit this row out, exactly as the beamer
           does. Letting them fall through to the normal path would produce
           children that ignore the clue. */
        if (clue_row) {
            if (sl != 0) continue;
            fin_expand_clued(ctx, &beam[pi], pi, row, at_stop, sc,
                             quota_parent, budget_parent, pin_idx, pin_kind, pin_val);
            continue;
        }
        /* Before expand_prepare, whose guard would drop this parent when its
           segment-A cell is empty -- exactly the case a B+C partial records.
           Once per parent, not once per slice. */
        if (at_stop && g_incomplete_top && sl == 0) try_BC(ctx, &beam[pi], row);
        Expand e;
        if (!expand_prepare(&e, &beam[pi], pi, row, at_stop)) continue;
        /* Seeded per parent, not per slice. That used to be load-bearing: the
           slices shared ONE cycle over the whole cell and had to agree on it to
           divide it. Phase 2 now permutes only the slice's own untouched tail,
           so the shared key merely keeps a parent's randomness a function of the
           parent and the row rather than of where its work landed. */
        e.rng = rng_for(cfg_hash, (uint32_t)row, pi, 0xFFFFFFFFu);
        e.quota = quota_slice; e.budget = budget_slice;

        /* Phase 1: this slice's stride of the cell in promise order (the cell is
           fan-out sorted, so the prefix holds the most continuable chains),
           reserving a quarter of the budget for the randomized phase. */
        const uint64_t rand_budget = budget_slice / 4;
        const uint32_t n = e.cA->n;
        /* Slice sl owns the slice-local indices m = 0..n_sl-1, which are the
           global records sl, sl+n_slices, ... Phase 1 walks them in order; m_done
           records how far it got, so phase 2 can take the rest and ONLY the
           rest. */
        const uint32_t n_sl = (sl < n) ? ((n - sl + n_slices - 1) / n_slices) : 0;
        uint32_t m_done = 0;
        for (; m_done < n_sl; m_done++) {
            if (e.quota == 0 || e.budget <= rand_budget) break;
            try_A(&e, ctx, sl + m_done * n_slices, sc);
        }
        /* Phase 2: the records phase 1 did not reach, in one random full-cycle
           permutation (random start + stride coprime to the length visits every
           position exactly once).

           Phase 1 covers a PREFIX of the slice in the cell's fan-out order, so
           the tail is what is left to sample -- and permuting only the tail is
           what makes the two phases disjoint. Permuting the whole cell instead,
           as this did before, re-walked everything phase 1 had just done
           whenever quota was not the binding constraint; try_A is deterministic
           in its record, so every one of those was a bit-identical duplicate
           child. That is not a rare corner: quota only binds while the beam is
           full, so the waste was exactly 2x at the early rows AND at the
           collapsing rows 9-12, and near zero in between. Measured before this
           change, candidates/unique sat at 2.0000 for every row with quota
           headroom.

           When phase 1 exhausted the slice, L is 0 and phase 2 correctly does
           nothing. */
        const uint32_t L = n_sl - m_done;
        if (e.quota > 0 && e.budget > 0 && L > 1) {
            uint32_t start = rng_uniform(&e.rng, L);
            uint32_t step = 1;
            if (L > 2) { do step = 1 + rng_uniform(&e.rng, L - 1); while (gcd_u32(step, L) != 1); }
            uint32_t pos = start;
            for (uint32_t k = 0; k < L && e.quota > 0 && e.budget > 0; k++) {
                try_A(&e, ctx, sl + (m_done + pos) * n_slices, sc);
                pos += step; if (pos >= L) pos -= L;
            }
        } else if (e.quota > 0 && e.budget > 0 && L == 1) {
            try_A(&e, ctx, sl + m_done * n_slices, sc);
        }
        pool_flush(ctx, sc);
    }
    /* Every `continue` above skips the in-loop flush, so a thread whose LAST
       work item took one leaves children sitting in its buffer. They would then
       be flushed into the NEXT row's pool, where their parent indices point into
       the wrong beam -- boards that fail parity on rebuild. Drain every buffer
       here so no child can outlive the row that made it. Reachable before the
       clue rows existed, via a try_BC that appends and an expand_prepare that
       then fails; a clue row takes the `continue` for every parent, so there it
       is not a corner case but the whole row. */
    for (int t = 0; t < nt; t++) pool_flush(ctx, scratch[t]);
    maha_close_row(row);        /* this row's spread calibrates the next one */
}

/* -- Beam selection --------------------------------------------------------- */

static int cmp_sortrec_desc(const void *a, const void *b) {
    const SortRec *x = a, *y = b;
    if (x->score != y->score) return (x->score < y->score) ? 1 : -1;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

/* Sort n SortRecs descending by score: parallel chunk qsorts + one k-way merge. */
static void sort_recs_desc(SortRec *a, SortRec *tmp, uint32_t n, int nt) {
    if (n < (1u << 17) || nt <= 1) { qsort(a, n, sizeof(SortRec), cmp_sortrec_desc); return; }
    if (nt > 64) nt = 64;
    uint32_t bnd[65];
    for (int t = 0; t <= nt; t++) bnd[t] = (uint32_t)((uint64_t)n * t / nt);
    #pragma omp parallel for schedule(static, 1) num_threads(nt)
    for (int t = 0; t < nt; t++)
        qsort(a + bnd[t], bnd[t+1] - bnd[t], sizeof(SortRec), cmp_sortrec_desc);
    uint32_t head[64];
    for (int t = 0; t < nt; t++) head[t] = bnd[t];
    for (uint32_t o = 0; o < n; o++) {
        int best = -1;
        for (int t = 0; t < nt; t++) {
            if (head[t] == bnd[t+1]) continue;
            if (best < 0 || cmp_sortrec_desc(&a[head[t]], &a[head[best]]) < 0) best = t;
        }
        tmp[o] = a[head[best]++];
    }
    memcpy(a, tmp, (size_t)n * sizeof(SortRec));
}

/* Deduplicate the pool by frontier signature, keeping the BEST score per
   signature (two boards with the same used-set and exposed tops have identical
   futures, so only the top-scored representative needs to survive). The hash
   table is region-sharded:
   every signature belongs to exactly one thread's slot range, so threads insert
   without locks; probing wraps within the owner's range. Fills ctx->keep[]
   with the survivors' pool indices, best score first; returns the count. */
static int g_dedup_drop_warned = 0;

static uint32_t dedup_and_rank(BeamCtx *ctx, uint64_t pool_n, int nt) {
    /* Size the table to the pool actually in hand, not to the worst case the
       arena was reserved for. ctx->sig_sz is next_pow2(2*pool_cap) -- 33.5 M
       slots at default settings -- so using all of it every row costs a 268 MB
       memset and a 33.5 M-slot scan whether the row produced eleven million
       candidates or forty. On a locked board the small case IS the normal
       regime: a measured finalizer run spent 248.9s in here while its per-row
       pools held 31..1389 boards. Powers of two only (the probe masks), at
       least 64 slots per thread so every shard has room to probe, and never
       more than was allocated. Only the touched prefix of the arena ever faults
       in, so this lowers resident memory as well. */
    size_t sz = 4;
    while (sz < ctx->sig_sz && (uint64_t)sz < pool_n * 2) sz *= 2;
    while (sz < ctx->sig_sz && sz < (size_t)nt * 64)      sz *= 2;
    const size_t mask = sz - 1;
    memset(ctx->sig_key, 0, sz * sizeof(uint64_t));

    uint64_t dropped = 0;
    #pragma omp parallel num_threads(nt) reduction(+:dropped)
    {
        int t = omp_get_thread_num();
        int T = omp_get_num_threads();
        size_t lo = sz * (size_t)t / T, hi = sz * (size_t)(t+1) / T;
        if (hi > lo) {
            for (uint64_t i = 0; i < pool_n; i++) {
                uint64_t s = ctx->pool[i].sig;
                size_t h = (size_t)s & mask;
                if (h < lo || h >= hi) continue;
                float sc = ctx->pool[i].score;
                size_t probes = hi - lo;
                for (;;) {
                    if (probes-- == 0) { dropped++; break; }   /* shard full */
                    if (ctx->sig_key[h] == 0) {
                        ctx->sig_key[h] = s; ctx->sig_score[h] = sc; ctx->sig_idx[h] = (uint32_t)i;
                        break;
                    }
                    if (ctx->sig_key[h] == s) {
                        /* Lowest pool index breaks a score tie, so which of two
                           equally scored boards represents the signature cannot
                           depend on the order its shard happened to see them. */
                        if (sc > ctx->sig_score[h] ||
                            (sc == ctx->sig_score[h] && (uint32_t)i < ctx->sig_idx[h])) {
                            ctx->sig_score[h] = sc; ctx->sig_idx[h] = (uint32_t)i;
                        }
                        break;
                    }
                    h++; if (h == hi) h = lo;
                }
            }
        }
    }
    if (dropped && !g_dedup_drop_warned) {
        g_dedup_drop_warned = 1;
        fprintf(stderr, "[warn] dedup: %llu candidate(s) dropped, a signature shard "
                        "filled up (table %zu slots, pool %llu)\n",
                (unsigned long long)dropped, sz, (unsigned long long)pool_n);
    }

    uint32_t kept = 0;
    for (size_t h = 0; h < sz; h++) {
        if (!ctx->sig_key[h]) continue;
        ctx->srt[kept].score = ctx->sig_score[h];
        ctx->srt[kept].idx   = ctx->sig_idx[h];
        kept++;
    }
    sort_recs_desc(ctx->srt, ctx->srt_tmp, kept, nt);
    for (uint32_t i = 0; i < kept; i++) ctx->keep[i] = ctx->srt[i].idx;
    return kept;
}

/* Prune the ranked survivors to target_K: a score band (respecting the
   per-parent offspring cap) plus a random band of frac_rand_now * target_K,
   then best-first fill of any remainder. */
static uint32_t select_beam(BeamCtx *ctx, uint32_t kept, uint32_t beam_n,
                            uint32_t target_K, uint32_t cap, double frac_rand_now,
                            RNG *rng) {
    const uint32_t K = target_K;
    if (kept <= K) { for (uint32_t i = 0; i < kept; i++) ctx->sel[i] = i; return kept; }
    uint32_t k_rand = (uint32_t)(frac_rand_now * K);
    if (k_rand > K) k_rand = K;
    uint32_t k_top = K - k_rand;
    memset(ctx->taken, 0, kept);
    memset(ctx->offspring, 0, (size_t)beam_n*sizeof(uint32_t));
    uint32_t n_sel = 0, got = 0;
    for (uint32_t i = 0; i < kept && got < k_top; i++) {
        uint32_t par = ctx->pool[ctx->keep[i]].parent;
        if (cap && ctx->offspring[par] >= cap) continue;
        ctx->offspring[par]++; ctx->taken[i] = 1; ctx->sel[n_sel++] = i; got++;
    }
    got = 0;
    uint64_t draws = (uint64_t)k_rand*32 + 1024;
    while (got < k_rand && draws-- > 0) {
        uint32_t i = rng_uniform(rng, kept);
        if (ctx->taken[i]) continue;
        ctx->taken[i] = 1; ctx->sel[n_sel++] = i; got++;
    }
    for (uint32_t i = 0; i < kept && n_sel < K; i++) {
        if (ctx->taken[i]) continue;
        ctx->taken[i] = 1; ctx->sel[n_sel++] = i;
    }
    return n_sel;
}

static void materialize_beam(BeamCtx *ctx, const BeamEntry *src, BeamEntry *dst,
                             uint32_t n_sel, int row) {
    if (ctx->log_cap[row] < n_sel) {
        ctx->log[row] = xrealloc(ctx->log[row], (size_t)n_sel * sizeof(RowLog));
        ctx->log_cap[row] = n_sel;
    }
    ctx->log_n[row] = n_sel;
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    /* Static, not dynamic-64: the per-board work here is uniform, and a chunk of
       64 leaves a beam of a few hundred with fewer chunks than threads. */
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (uint32_t i = 0; i < n_sel; i++) {
        const PoolEntry *pe = &ctx->pool[ctx->keep[ctx->sel[i]]];
        BeamEntry *d = &dst[i];
        *d = src[pe->parent];
        ctx->log[row][i].parent_log = d->log_idx;   /* parent's own log entry */
        ctx->log[row][i].mv = pe->mv;
        commit_row(d, row, &pe->mv);
        d->depth = (uint16_t)row; d->score = pe->score; d->log_idx = i;
        assert(parity_ok(d));
    }
}

/* Stop-row emission buffers, allocated on first use (emission is entered from
   serial code). One tile is ~12.6 MB. */
#define EMIT_TILE 4096
static char     *g_emit_lines = NULL;    /* EMIT_TILE x EMIT_LINE_MAX */
static uint64_t *g_emit_fps   = NULL;
static int      *g_emit_lens  = NULL;

/* Emit the stop-row boards, best-scored first. Reconstructing a board, hashing
   it and converting its 512 fields is ~all of the cost and touches only
   read-only state, so a tile of boards is built in parallel; the dedup, the
   index draw and the write itself then run serially over that tile in rank
   order, so the file is byte for byte what emitting one board at a time
   produced. Boards losing the dedup were formatted for nothing -- parallel work
   traded against serial work, which is the point. */
static void emit_stop_row(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept, int row) {
    if (!g_completions_fp) return;
    uint32_t n_emit = kept < EMIT_MAX ? kept : EMIT_MAX;
    if (!g_emit_lines) {
        g_emit_lines = xmalloc((size_t)EMIT_TILE * EMIT_LINE_MAX);
        g_emit_fps   = xmalloc((size_t)EMIT_TILE * sizeof(uint64_t));
        g_emit_lens  = xmalloc((size_t)EMIT_TILE * sizeof(int));
    }
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    for (uint32_t base = 0; base < n_emit && !g_stop; base += EMIT_TILE) {
        uint32_t tile = n_emit - base < (uint32_t)EMIT_TILE
                      ? n_emit - base : (uint32_t)EMIT_TILE;
        #pragma omp parallel for schedule(static) num_threads(nt)
        for (uint32_t k = 0; k < tile; k++) {
            const PoolEntry *pe = &ctx->pool[ctx->keep[base + k]];
            RowChoice rows[EDGE_LEN + 1];
            rows[row] = pe->mv;
            collect_rows(ctx, &beam[pe->parent], rows);
            g_emit_fps[k]  = board_fingerprint(rows, row);
            g_emit_lens[k] = format_board_tail(rows, row, ROWMASK_FULL, g_fin_orient,
                                               g_emit_lines + (size_t)k * EMIT_LINE_MAX);
        }
        for (uint32_t k = 0; k < tile; k++) {
            if (!htable_insert(g_emit_fps[k])) continue;
            fprintf(g_completions_fp, "%s, %" PRIu64, g_config_id_str, g_solution_idx++);
            fwrite(g_emit_lines + (size_t)k * EMIT_LINE_MAX, 1,
                   (size_t)g_emit_lens[k], g_completions_fp);
            g_stats.emitted_total++;
        }
    }
}

/* -- Beam driver ------------------------------------------------------------ */

static BeamResult beam_search_config(BeamCtx *ctx, Scratch **scratch,
                                     uint64_t cfg_hash, double deadline) {
    BeamResult res = {0, 1, "stop_row"};
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    BeamEntry *cur = ctx->beam_a, *nxt = ctx->beam_b;
    fin_init_entry(&cur[0], g_cur_left);
    if (!parity_ok(&cur[0])) {
        printf("[beam] %s: locked board fails the color-parity invariant "
               "(provably dead); skipped\n", g_config_id_str);
        res.reason = "parity"; res.row = g_finalize_from;
        g_stats.configs++;
        return res;
    }
    uint32_t beam_n = 1;
    memset(ctx->log_n, 0, sizeof ctx->log_n);
    g_stats.configs++;

    for (int row = (int)g_finalize_from + 1; (uint32_t)row <= g_stop_row; row++) {
        if (g_stop)                      { res.reason = "interrupted"; break; }
        if (omp_get_wtime() >= deadline) { res.reason = "time";        break; }
        double t_row = omp_get_wtime();

        expand_row(ctx, cur, beam_n, row, cfg_hash, scratch);
        double t_exp = omp_get_wtime();
        g_stats.t_expand += t_exp - t_row;
        uint64_t pool_n = ctx->pool_n;
        if (pool_n > ctx->pool_cap) pool_n = ctx->pool_cap;
        g_stats.cands_total += pool_n;
        if (pool_n == 0) {
            res.reason = "extinct"; res.row = (uint32_t)row;
            g_stats.extinct_at[row]++;
            break;
        }

        /* ctx->keep drives emit_stop_row, so the stop row's emission order IS
           this ranking -- by the real score, and by nothing else. */
        uint32_t kept = dedup_and_rank(ctx, pool_n, nt);
        g_stats.t_select += omp_get_wtime() - t_exp;
        res.row = (uint32_t)row;
        g_stats.rows_advanced++;

        if ((uint32_t)row == g_stop_row) {
            res.width = kept;
            double t0 = omp_get_wtime();
            emit_stop_row(ctx, cur, kept, row);
            g_stats.t_emit += omp_get_wtime() - t0;
            g_stats.reached_stop++;
            partials_budget_spent();       /* the whole beam is written by now */
            if (g_verbose) {
                double dt = omp_get_wtime() - t_row;
                printf("[beam] %s row=%d cands=%" PRIu64 " uniq=%u emitted<=%u t=%.2fs\n",
                       g_config_id_str, row, pool_n, kept,
                       kept < EMIT_MAX ? kept : EMIT_MAX, dt);
                fflush(stdout);
            }
            break;
        }

        RNG sel_rng = rng_for(cfg_hash, (uint32_t)row, 0xFFFFFFFFu, 1u);
        uint32_t eff_K = beam_eff_K(row);
        uint32_t n_sel = select_beam(ctx, kept, beam_n, eff_K,
                                     parent_cap_eff(row), g_frac_rand, &sel_rng);
        double t0 = omp_get_wtime();
        materialize_beam(ctx, cur, nxt, n_sel, row);
        g_stats.t_mat += omp_get_wtime() - t0;
        BeamEntry *tmp = cur; cur = nxt; nxt = tmp;
        beam_n = n_sel; res.width = beam_n;

        if (g_verbose) {
            double dt = omp_get_wtime() - t_row;
            /* The row's best score, read from the pool. ctx->srt now carries
               the same number, nothing perturbing the sort key any more. */
            printf("[beam] %s row=%d cands=%" PRIu64 " uniq=%u beam=%u/%u smax=%.2f t=%.2fs (%.0f kc/s)\n",
                   g_config_id_str, row, pool_n, kept, beam_n, eff_K,
                   (double)ctx->pool[ctx->keep[0]].score, dt, (double)pool_n/dt/1e3);
            fflush(stdout);
        }
    }
    return res;
}

/* -- Workspace -------------------------------------------------------------- */

static BeamEntry *alloc_beam(uint64_t K) {
    BeamEntry *p = arena_map((size_t)K * sizeof(BeamEntry));
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (uint64_t i = 0; i < K; i++) memset(&p[i], 0, sizeof(BeamEntry));
    return p;
}

static void beam_ctx_alloc(BeamCtx *ctx) {
    const uint64_t KE = (uint64_t)g_beam_width * g_beam_expand;
    const uint32_t per = g_pool_factor < 8 ? 8 : g_pool_factor;
    memset(ctx, 0, sizeof *ctx);
    ctx->pool_cap = KE * (per + 1) + 16384;
    if (ctx->pool_cap > UINT32_MAX)
        fatal("beam_width x beam_expand x pool_factor too large (pool needs 32-bit indices)");
    ctx->pool = arena_map(ctx->pool_cap * sizeof(PoolEntry));
    ctx->sig_sz = 4; while (ctx->sig_sz < (size_t)ctx->pool_cap*2) ctx->sig_sz *= 2;
    ctx->sig_key   = arena_map(ctx->sig_sz * sizeof(uint64_t));
    ctx->sig_score = arena_map(ctx->sig_sz * sizeof(float));
    ctx->sig_idx   = arena_map(ctx->sig_sz * sizeof(uint32_t));
    ctx->keep = xmalloc(ctx->pool_cap * sizeof(uint32_t));
    ctx->srt     = arena_map(ctx->pool_cap * sizeof(SortRec));
    ctx->srt_tmp = arena_map(ctx->pool_cap * sizeof(SortRec));
    ctx->taken = xmalloc(ctx->pool_cap);
    ctx->offspring = xmalloc(KE * sizeof(uint32_t));
    ctx->sel = xmalloc(KE * sizeof(uint32_t));
    ctx->beam_a = alloc_beam(KE);
    ctx->beam_b = alloc_beam(KE);
    double gb = ((double)ctx->pool_cap*(sizeof(PoolEntry)+2*sizeof(SortRec)+sizeof(uint32_t)+1)
                 + (double)ctx->sig_sz*(sizeof(uint64_t)+sizeof(float)+sizeof(uint32_t))
                 + 2.0*(double)KE*sizeof(BeamEntry)
                 + 2.0*(double)KE*sizeof(uint32_t)) / 1e9;
    printf("[init] beam workspace: width=%u expand=%ux@row%u pool_cap=%" PRIu64 " (~%.2f GB), board=%zuB\n",
           g_beam_width, g_beam_expand, g_beam_expand_row, ctx->pool_cap, gb, sizeof(BeamEntry));
    fflush(stdout);
}

/* -- Partial input ------------------------------------------------------------ */

/* Tokenize one data line in place: config_id, sol_idx, pos[256], rot[256].
   id_out may be NULL when only the placements are needed. */
static bool fin_parse_fields(char *s, char id_out[64],
                             int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    char *tok = strtok(s, ",\r\n");
    if (!tok) return false;
    while (*tok == ' ') tok++;
    if (id_out) snprintf(id_out, 64, "%s", tok);
    tok = strtok(NULL, ",\r\n");                 /* sol_idx: not needed here */
    if (!tok) return false;
    for (int k = 0; k < 2 * NUM_PIECES; k++) {
        tok = strtok(NULL, ",\r\n");
        if (!tok) return false;
        long v = strtol(tok, NULL, 10);
        if (k < NUM_PIECES) pos[k] = (int)v;
        else                rot[k - NUM_PIECES] = (int)v;
    }
    return true;
}

/* Read the want-th data line of a completions CSV (blank and '#'/'%' comment
   lines are not counted): config_id, sol_idx, pos[256], rot[256]. Returns
   false when the file holds fewer data lines. */
static bool fin_read_partial_line(const char *path, uint32_t want,
                                  char id_out[64], int pos[NUM_PIECES],
                                  int rot[NUM_PIECES]) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open partials CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0;
    long data_idx = -1; bool found = false;
    while (getline(&line, &sz, f) > 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '\r' || *s == '#' || *s == '%') continue;
        if ((uint32_t)++data_idx != want) continue;
        found = fin_parse_fields(s, id_out, pos, rot);
        if (!found) fatal("partials CSV line %u is truncated (needs %d fields)",
                          want, 2 + 2 * NUM_PIECES);
        break;
    }
    free(line); fclose(f);
    return found;
}

/* -- Input dedup ---------------------------------------------------------- */
/* Long partial lists carry many near-siblings: boards from the same beam base
   that differ only in a few top-row pieces. Once rows above finalize_from are
   freed, such lines seed EXACTLY the same search, so every read line is
   hashed and any line matching an earlier one is skipped with a note.

   The hash covers precisely what the search will see: the exact placement
   (pos+rot) of row finalize_from itself (the frontier the beam grows from)
   plus, in fixed mode, of the border pieces above it (the fixed sides); and
   the SET of pieces left to the search (unplaced pieces, plus everything
   freed above finalize_from -- inner pieces in both modes, border pieces in
   free mode). The ARRANGEMENT of the locked rows below finalize_from is
   deliberately ignored -- two partials differing only there consume the same
   pieces and expose the same frontier.

   The whole file up to (and through) the selected window is hashed -- lines
   before --border_row too -- so chunked runs over one file skip a window
   line that repeats anything earlier. --finalize_repeats is the deliberate
   way to re-run an already-seen partial. */

/* Border completeness straight from a pos[] vector (the free/fixed decision
   the sweep will make for this line, needed before the grid is built). */
static bool fin_pos_border_complete(const int pos[NUM_PIECES]) {
    bool has[PUZZLE_SIDE][PUZZLE_SIDE];
    memset(has, 0, sizeof has);
    for (int pid = 0; pid < NUM_PIECES; pid++)
        if (pos[pid] != 999 && pos[pid] >= 0 && pos[pid] < NUM_PIECES)
            has[pos[pid] / PUZZLE_SIDE][pos[pid] % PUZZLE_SIDE] = true;
    for (int k = 0; k < PUZZLE_SIDE; k++)
        if (!has[0][k] || !has[PUZZLE_SIDE-1][k] ||
            !has[k][0] || !has[k][PUZZLE_SIDE-1]) return false;
    return true;
}

static uint64_t fin_partial_hash(const int pos[NUM_PIECES], const int rot[NUM_PIECES],
                                 int orient) {
    bool free_mode = g_opt_free_edges || !fin_pos_border_complete(pos);
    uint64_t h = 0xD0DEC5B0A0D5EEDULL;
    /* Everything freed above the lock folds to one bucket below, so two lines
       whose clues differ only above it would collide -- and the survivor would
       be pinned to the other one's orientation. Guarded so the no-clue hash,
       and hence the no-clue skip decisions, are bit-for-bit unchanged. */
    if (g_clue_mask) h = splitmix64(h ^ (uint64_t)(orient + 1));
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        int r = (pos[pid] == 999) ? -1 : pos[pid] / PUZZLE_SIDE;
        int c = (pos[pid] == 999) ? -1 : pos[pid] % PUZZLE_SIDE;
        bool border = (c == 0 || c == PUZZLE_SIDE-1 || r == PUZZLE_SIDE-1);
        uint64_t v;
        if (r < 0)                          v = 1;   /* unpositioned: free pool  */
        else if (r < (int)g_finalize_from)  v = 2;   /* locked below: order-free */
        else if ((uint32_t)r == g_finalize_from || (!free_mode && border))
            v = 4u + ((uint64_t)pos[pid] << 2) + (uint64_t)(rot[pid] & 3);
        else                                v = 1;   /* freed above: free pool   */
        h = splitmix64(h ^ ((uint64_t)pid << 40) ^ v);
    }
    return h ? h : 1;
}

/* Seen-hash set (open addressing, power-of-2; grows, never reset). */
static uint64_t *g_pdup_tab = NULL;
static size_t    g_pdup_sz  = 0, g_pdup_n = 0;

/* Insert key; returns false when it was already present. */
static bool pdup_insert(uint64_t key) {
    if (!key) key = 1;
    if (g_pdup_n * 2 + 2 > g_pdup_sz) {
        size_t new_sz = g_pdup_sz ? g_pdup_sz * 2 : (1u << 12);
        uint64_t *nt = xmalloc(new_sz * sizeof(uint64_t));
        memset(nt, 0, new_sz * sizeof(uint64_t));
        for (size_t i = 0; i < g_pdup_sz; i++) {
            uint64_t k = g_pdup_tab[i];
            if (!k) continue;
            size_t j = k & (new_sz - 1);
            while (nt[j]) j = (j + 1) & (new_sz - 1);
            nt[j] = k;
        }
        free(g_pdup_tab); g_pdup_tab = nt; g_pdup_sz = new_sz;
    }
    size_t j = key & (g_pdup_sz - 1);
    while (g_pdup_tab[j]) {
        if (g_pdup_tab[j] == key) return false;
        j = (j + 1) & (g_pdup_sz - 1);
    }
    g_pdup_tab[j] = key; g_pdup_n++;
    return true;
}

/* Data lines in a partials CSV, counted with the same convention the loader
   uses: blanks and # / % lines are not lines. Only called for
   --border_row_N 0, so an ordinary run opens the CSV exactly as before. */
static uint32_t fin_count_data_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open partials CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0; uint32_t n = 0;
    while (getline(&line, &sz, f) > 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s=='\0'||*s=='\n'||*s=='\r'||*s=='#'||*s=='%') continue;
        n++;
    }
    free(line); fclose(f);
    return n;
}

/* Hash every data line BEFORE the selected window into the seen set (one
   pass, no validation), so a window line duplicating anything earlier in the
   file is skipped even when the file is processed in --border_row chunks. */
static void fin_prescan_hashes(const char *path, uint32_t upto) {
    if (upto == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open partials CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0;
    long data_idx = -1, scanned = 0;
    int pos[NUM_PIECES], rot[NUM_PIECES];
    while (getline(&line, &sz, f) > 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '\r' || *s == '#' || *s == '%') continue;
        if ((uint32_t)++data_idx >= upto) break;
        if (fin_parse_fields(s, NULL, pos, rot)) {
            pdup_insert(fin_partial_hash(pos, rot,
                                         g_clue_mask ? fin_clue_orient_of(pos, rot) : -1));
            scanned++;
        }
    }
    free(line); fclose(f);
    printf("[init] dedup: prescanned %ld lines before the window (%zu distinct states)\n",
           scanned, g_pdup_n);
    fflush(stdout);
}

/* Can this line be searched at orientation `orient` at all? Reads g_grid and
   g_lock_mask, so it runs after the load's structural checks. `explain` prints
   the reason: an auto sweep tests four orientations and only wants to hear
   about them when every one has failed, otherwise three rejections would be
   printed for every ordinary line. */
static bool fin_clue_viable(int orient, const int pos[NUM_PIECES],
                            uint32_t want, bool explain) {
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_clue[orient][k];
        if ((uint32_t)cc->row <= g_finalize_from) {          /* locked: must be right already */
            const Oriented *o = &g_grid[cc->row][cc->col];
            if (o->piece_id != cc->piece || o->rotation != cc->spin) {
                if (explain)
                    printf("[skip] line %u: locked cell (%d,%d) holds piece %u spin %u, but clue %d\n"
                           "       of orientation %d needs piece %u spin %u there\n",
                           want, cc->row, cc->col, o->piece_id, o->rotation,
                           k, orient, cc->piece, cc->spin);
                return false;
            }
            continue;
        }
        if (used_test(g_lock_mask, cc->piece)) {             /* locked somewhere else */
            if (explain)
                printf("[skip] line %u: clue piece %u is locked at cell %d, but clue %d of\n"
                       "       orientation %d needs it at (%d,%d)\n",
                       want, cc->piece, pos[cc->piece], k, orient, cc->row, cc->col);
            return false;
        }
        /* A clue's bottom face must meet the piece below it. When that row is
           locked the colour is already decided, so check it now; when it is
           searched, fin_clue_pins_for demands the colour during generation. */
        if ((uint32_t)cc->row <= g_stop_row && (uint32_t)(cc->row - 1) <= g_finalize_from) {
            uint8_t need = g_cat[g_clue_ci[orient][k]].bottom;
            if (g_grid[cc->row - 1][cc->col].top != need) {
                if (explain)
                    printf("[skip] line %u: locked cell (%d,%d) exposes colour %u upward, but clue %d\n"
                           "       of orientation %d sits on it and needs %u\n",
                           want, cc->row - 1, cc->col, g_grid[cc->row-1][cc->col].top,
                           k, orient, need);
                return false;
            }
        }
    }
    return true;
}

/* Load and validate CSV line `want` into g_grid / g_lock_mask /
   g_partial_depth. Returns 1 when loaded, 0 with a printed reason when the
   line cannot seed a search (the sweep skips it), -1 when the file has no
   such line (the sweep stops). The structural checks make sure a corrupted or
   mismatched file cannot silently poison a long run. */
static int fin_load_partial(const char *path, uint32_t want, char id_out[64]) {
    int pos[NUM_PIECES], rot[NUM_PIECES];
    if (!fin_read_partial_line(path, want, id_out, pos, rot)) return -1;

    /* Which of the four orientations this line committed to, -1 for a line that
       carries no clue and has therefore committed to none. Resolved before the
       dedup because the hash folds it in: everything freed above the lock
       collapses to one bucket there, so two lines differing only in orientation
       would otherwise merge and the survivor be pinned to the wrong one. Two
       UNCLUED lines with the same state do still merge, which is right -- they
       would expand to the same candidate set and search it twice. */
    int orient = g_clue_mask ? fin_clue_orient_of(pos, rot) : -1;

    /* Input dedup: a line seeding a search state already seen (earlier in the
       file or in this window) is skipped -- see fin_partial_hash. */
    if (!pdup_insert(fin_partial_hash(pos, rot, orient))) {
        printf("[skip] line %u: duplicate search state at finalize_from %u "
               "(matches an earlier line)\n", want, g_finalize_from);
        return 0;
    }

    memset(g_grid_has, 0, sizeof g_grid_has);
    memset(g_lock_mask, 0, sizeof g_lock_mask);
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == 999) continue;
        if (pos[pid] < 0 || pos[pid] >= NUM_PIECES || rot[pid] < 0 || rot[pid] > 3) {
            printf("[skip] line %u: piece %d has pos %d / rot %d out of range\n",
                   want, pid, pos[pid], rot[pid]);
            return 0;
        }
        int r = pos[pid] / PUZZLE_SIDE, c = pos[pid] % PUZZLE_SIDE;
        if (g_grid_has[r][c]) {
            printf("[skip] line %u: two pieces on cell (%d,%d)\n", want, r, c);
            return 0;
        }
        g_grid[r][c] = fin_oriented((uint16_t)pid, (uint8_t)rot[pid]);
        g_grid_has[r][c] = true;
    }

    /* Piece type and frame orientation must match the cell everywhere. */
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!g_grid_has[r][c]) continue;
            const Oriented *o = &g_grid[r][c];
            bool fb = (r == 0), ft = (r == PUZZLE_SIDE-1);
            bool fl = (c == 0), fr = (c == PUZZLE_SIDE-1);
            if (fin_zero_count(o->piece_id) != (fb + ft + fl + fr) ||
                (fb && o->bottom != 0) || (ft && o->top   != 0) ||
                (fl && o->left   != 0) || (fr && o->right != 0)) {
                printf("[skip] line %u: piece %u does not fit cell (%d,%d)\n",
                       want, o->piece_id, r, c);
                return 0;
            }
        }

    /* The locked region must be complete: a 999 at or below finalize_from
       cannot be initialized. */
    for (int r = 0; r <= (int)g_finalize_from; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (!g_grid_has[r][c]) {
                printf("[skip] line %u: cell (%d,%d) unplaced at or below "
                       "finalize_from %u\n", want, r, c, g_finalize_from);
                return 0;
            }

    /* Every match inside (and between) the locked rows must hold. */
    for (int r = 0; r <= (int)g_finalize_from; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (c + 1 < PUZZLE_SIDE && g_grid[r][c].right != g_grid[r][c+1].left) {
                printf("[skip] line %u: color mismatch (%d,%d)-(%d,%d)\n",
                       want, r, c, r, c + 1);
                return 0;
            }
            if (r < (int)g_finalize_from && g_grid[r][c].top != g_grid[r+1][c].bottom) {
                printf("[skip] line %u: color mismatch (%d,%d)-(%d,%d)\n",
                       want, r, c, r + 1, c);
                return 0;
            }
        }

    for (int r = 0; r <= (int)g_finalize_from; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            used_set(g_lock_mask, g_grid[r][c].piece_id);

    /* Which orientations this line can be searched at. A line that CARRIES a
       clue has committed to one; a line that carries none is searched at every
       viable candidate --clue_orient allows. Either way the answer has to
       survive fin_clue_viable, which is decided here rather than discovered
       mid-search: a locked region contradicting a clue can never be repaired by
       growing rows above it. */
    g_fin_orient_n = 0;
    if (g_clue_mask) {
        if (orient >= 0) {
            if (!(g_clue_orient_req & (1u << orient))) {
                printf("[skip] line %u: carries clue orientation %d, which "
                       "--clue_orient excludes\n", want, orient);
                return 0;
            }
            if (!fin_clue_viable(orient, pos, want, true)) return 0;
            g_fin_orient_cand[g_fin_orient_n++] = orient;
        } else {
            for (int o = 0; o < 4; o++)
                if ((g_clue_orient_req & (1u << o)) &&
                    fin_clue_viable(o, pos, want, false))
                    g_fin_orient_cand[g_fin_orient_n++] = o;
            if (g_fin_orient_n == 0) {
                printf("[skip] line %u: carries no clue piece, and no orientation "
                       "--clue_orient allows survives the locked region:\n", want);
                for (int o = 0; o < 4; o++)
                    if (g_clue_orient_req & (1u << o))
                        (void)fin_clue_viable(o, pos, want, true);
                return 0;
            }
            printf("[note] line %u: carries no clue piece, so the orientation is "
                   "not read but chosen:\n"
                   "       searching %d of the 4 (", want, g_fin_orient_n);
            for (int i = 0; i < g_fin_orient_n; i++)
                printf("%s%d", i ? "," : "", g_fin_orient_cand[i]);
            printf("), one pass each over one database\n");
        }
    }
    /* The sweep's orientation loop is the only thing that selects a candidate.
       Cleared here so nothing between the load and that loop -- the database
       build above all -- can read the previous line's orientation. */
    g_fin_orient = -1;

    g_partial_depth = -1;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        bool full = true;
        for (int c = 0; c < PUZZLE_SIDE; c++) full = full && g_grid_has[r][c];
        if (!full) break;
        g_partial_depth = r;
    }
    return 1;
}

/* Fixed-sides mode needs the complete border in the partial. */
static bool fin_border_complete(void) {
    for (int k = 0; k < PUZZLE_SIDE; k++)
        if (!g_grid_has[0][k] || !g_grid_has[PUZZLE_SIDE-1][k] ||
            !g_grid_has[k][0] || !g_grid_has[k][PUZZLE_SIDE-1]) return false;
    return true;
}

/* -- Optional rotations CSV (Stage A borders) ---------------------------------- */

/* A usable row must partition the frame exactly as classify_deal_from_rotations
   demands: 14 edges per side and one corner per board corner. Rows that do not
   are dropped at load time, so a matched row can never fatal mid-sweep. */
static bool fin_rot_row_valid(const uint8_t spins[NUM_PIECES]) {
    int side_n[4] = {0,0,0,0}, corner_n[4] = {0,0,0,0};
    for (int i = 0; i < NUM_PIECES; i++) {
        int z = fin_zero_count(i);
        if (z == 0) continue;
        Oriented o = fin_oriented((uint16_t)i, spins[i]);
        if (z == 1) {
            if      (o.bottom == 0) side_n[0]++;
            else if (o.left   == 0) side_n[1]++;
            else if (o.right  == 0) side_n[2]++;
            else if (o.top    == 0) side_n[3]++;
            else return false;                       /* frame side not exposed */
        } else if (z == 2) {
            if      (o.bottom == 0 && o.left  == 0) corner_n[0]++;
            else if (o.bottom == 0 && o.right == 0) corner_n[1]++;
            else if (o.top    == 0 && o.left  == 0) corner_n[2]++;
            else if (o.top    == 0 && o.right == 0) corner_n[3]++;
            else return false;
        } else return false;                         /* 3+ frame sides */
    }
    for (int k = 0; k < 4; k++)
        if (side_n[k] != EDGE_LEN || corner_n[k] != 1) return false;
    return true;
}

/* Load every data row of a Stage A rotations CSV into memory (256 spins each).
   read_one_border_row rescans the file per row -- O(rows^2) line parses, trivial
   for the few-hundred-row files Stage A writes, and it keeps one parser. */
static void fin_load_rotations(const char *path) {
    uint32_t cap = 64, dropped = 0;
    g_rot_rows = xmalloc((size_t)cap * NUM_PIECES);
    uint8_t spins[NUM_PIECES];
    for (uint32_t i = 0; read_one_border_row(path, i, spins); i++) {
        if (!fin_rot_row_valid(spins)) { dropped++; continue; }
        if (g_rot_n == cap) {
            cap *= 2;
            g_rot_rows = xrealloc(g_rot_rows, (size_t)cap * NUM_PIECES);
        }
        memcpy(g_rot_rows[g_rot_n++], spins, NUM_PIECES);
    }
    if (dropped)
        printf("[warn] rotations '%s': %u row(s) are not a legal 14/14/14/14 border "
               "partition and were dropped\n", path, dropped);
    if (g_rot_n == 0) fatal("rotations CSV '%s' has no usable border row", path);
    printf("[cfg] rotations=%s rows=%u\n", path, g_rot_n);
}

/* Identify which rotations row this partial's border came from, and reimpose it.
   The evidence is the LOCKED border only -- rows 0..finalize_from, which
   fin_load_partial guarantees are fully placed (16 + 2*finalize_from pieces).
   Those pieces stay on the board, so their side membership MUST agree with the
   row or the fixed-mode demands would be wrong; pieces above the lock are
   returned to the pool and re-searched, and a --free_edges beamer would not have
   respected any row up there, so they are deliberately not compared.

   A border piece's rotation on a border cell is already validated as frame-facing
   (fin_load_partial), and for a given side exactly one rotation does that, so
   "same spin" and "same side" are the same test. First match wins. */
static bool fin_rot_match(uint32_t line) {
    for (uint32_t k = 0; k < g_rot_n; k++) {
        const uint8_t *sp = g_rot_rows[k];
        bool ok = true;
        for (int r = 0; r <= (int)g_finalize_from && ok; r++)
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                if (r != 0 && c != 0 && c != PUZZLE_SIDE - 1) continue;
                const Oriented *o = &g_grid[r][c];
                if (sp[o->piece_id] != o->rotation) { ok = false; break; }
            }
        if (!ok) continue;
        memcpy(g_spin, sp, sizeof g_spin);
        classify_deal_from_rotations();
        g_rot_matched = (int)k;
        printf("[note] line %u: locked border matches rotations row %u -> sides "
               "fixed from the annealer\n", line, k);
        return true;
    }
    printf("[note] line %u: no rotations row matches the locked border -> "
           "--free_edges\n", line);
    return false;
}

/* -- Per-partial database ------------------------------------------------------ */

/* (Re)build the reduced database for the current lock mask. Locked pieces are
   excluded outright (g_db_exclude), so the chain DFS shrinks
   super-exponentially with the locked inner count and no record can be
   rejected at search time for reusing a locked piece. The multi-GB inner
   phase is skipped when the lock mask matches the previous line's (repeats,
   or consecutive lines sharing their lower rows); the tiny edge phase is
   rebuilt every time (the mode and border assignment may differ). */
/* Raised whenever the inner database is rebuilt. The per-left_color inner
   totals further down are derived from it, and summing them means dereferencing
   every pointer in the multi-GB arena -- so they are recomputed only when this
   says the arena actually moved, instead of once per partial line. */
static bool g_fin_la_inner_stale = true;

static void fin_build_db(void) {
    static bool     have_db = false;
    static uint64_t built_mask[4];
    /* Clue pieces leave the chain database with the locked ones, so no chain can
       hold one and the search never rejects a chain for colliding with a clue --
       the pinned walk places them instead. This is only an optimisation: the
       `used` reservation in fin_init_entry is what makes it correct. Note the
       exclusion set, not g_lock_mask, is the cache key: two partials can share a
       locked region and still owe different clue pieces, and reusing the first
       one's arena for the second would leave a clue in the chains.

       The exclusion is taken over all four orientations (fin_clue_reserve), so
       one line's database serves every orientation it is searched at: the pins
       move between passes, the excluded pieces do not. */
    uint64_t excl[4];
    memcpy(excl, g_lock_mask, sizeof excl);
    if (g_clue_mask) fin_clue_reserve(excl);

    if (!have_db || memcmp(built_mask, excl, sizeof built_mask) != 0) {
        if (g_inner_arena) {
            munmap(g_inner_arena, g_inner_arena_size);
            g_inner_arena = NULL; g_inner_arena_size = 0;
        }
        if (g_edge_arena) {
            munmap(g_edge_arena, g_edge_arena_size);
            g_edge_arena = NULL; g_edge_arena_size = 0;
        }
        memset(g_db, 0, sizeof g_db);
        g_db_max_cell_n = 0;
        memcpy(g_db_exclude, excl, sizeof g_db_exclude);
        build_db_inner();
        build_fanout_inner();
        sort_db_by_fanout();
        memcpy(built_mask, excl, sizeof built_mask);
        have_db = true;
        g_fin_la_inner_stale = true;
    } else {
        memcpy(g_db_exclude, excl, sizeof g_db_exclude);
    }
    build_edge_terminal_pool();
    build_db_edge_and_sort();
}

/* -- Left columns above the locked rows ---------------------------------------- */

/* The candidate left-column pieces oriented frame-left, and the 4 corner ids.
   Without a matched rotations row that is all 56 edges; with one it is the 14 the
   annealer gave the left side (g_left_pool, already frame-left). Rebuilt whenever
   the source changes -- different partial lines may match different rows. */
static Oriented s_leftz[MAX_EDGE_TERMINALS];
static int      s_leftz_n = 0;
static int      s_corner_ids[4];
static int      s_corner_n = 0;
static int      s_pools_key = -2;           /* -1 = all 56 edges; else the row */

static void fin_build_side_pools(void) {
    const int key = g_sides_from_rot ? g_rot_matched : -1;
    if (s_leftz_n && s_pools_key == key) return;
    s_leftz_n = 0; s_corner_n = 0;

    for (int i = 0; i < NUM_PIECES; i++)
        if (fin_zero_count(i) == 2) {
            if (s_corner_n >= 4) fatal("seed has more than 4 corner pieces");
            s_corner_ids[s_corner_n++] = i;
        }

    if (g_sides_from_rot) {
        for (int k = 0; k < g_left_count_pool; k++) s_leftz[s_leftz_n++] = g_left_pool[k];
    } else {
        for (int i = 0; i < NUM_PIECES; i++) {
            if (fin_zero_count(i) != 1) continue;
            bool ok = false;
            for (uint8_t s = 0; s < 4 && !ok; s++) {
                Oriented o = fin_oriented((uint16_t)i, s);
                if (o.left == 0) { s_leftz[s_leftz_n++] = o; ok = true; }
            }
            if (!ok) fatal("edge piece %d cannot be oriented frame-left", i);
        }
    }
    s_pools_key = key;
}

/* Per-left_color record totals of the CURRENT reduced database (inner + edge
   cells), for ranking column samples -- same measure as the beamer's left
   ranking.

   Split by the b5 dimension, because the two halves have very different
   lifetimes. b5 is the LAST index of g_db, and the non-inner colors are exactly
   0..COLOR_MIN-1, so each half is a contiguous run inside every DIM_B5-slot
   group -- a comb through the array, not a scattered set. The inner half
   survives until the lock mask changes but summing it dereferences every
   pointer in the arena (a full multi-GB read); the edge half is rebuilt for
   every partial line but is the small half of the database. Recomputing only
   what actually changed turns a per-line arena sweep into a per-line walk of
   6 slots in every 23. The sums are unsigned integers, so splitting and
   reordering them cannot change the result. */
static uint64_t g_fin_la_total[DIM_INNER];   /* inner + edge: what callers read */
static uint64_t g_fin_la_inner[DIM_INNER];   /* the cached inner half */

static void fin_la_totals(void) {
    Cell **flat = &g_db[0][0][0][0][0][0];

    if (g_fin_la_inner_stale) {
        int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
        memset(g_fin_la_inner, 0, sizeof g_fin_la_inner);
        #pragma omp parallel num_threads(nt)
        {
            uint64_t loc[DIM_INNER] = {0};
            #pragma omp for schedule(static) nowait
            for (uint64_t base = 0; base < NCELL_DB; base += DIM_B5) {
                uint64_t la = base / FANOUT_N;      /* FANOUT_N is a multiple of
                                                       DIM_B5, so one division
                                                       serves the whole group */
                for (uint64_t b5 = COLOR_MIN; b5 < DIM_B5; b5++)
                    if (flat[base + b5]) loc[la] += flat[base + b5]->n;
            }
            #pragma omp critical
            for (int k = 0; k < DIM_INNER; k++) g_fin_la_inner[k] += loc[k];
        }
        g_fin_la_inner_stale = false;
    }

    memcpy(g_fin_la_total, g_fin_la_inner, sizeof g_fin_la_total);
    for (uint64_t base = 0; base < NCELL_DB; base += DIM_B5) {
        uint64_t la = base / FANOUT_N;
        for (uint64_t b5 = 0; b5 < COLOR_MIN; b5++)
            if (flat[base + b5]) g_fin_la_total[la] += flat[base + b5]->n;
    }

#ifdef E555_LA_VERIFY
    {   /* Recompute the pre-C8 way -- one flat pass over every cell -- and
           compare. Catches both a wrong comb (the b5 split) and a stale cache
           (the flag logic), on every call, whichever branch was taken. */
        uint64_t ref[DIM_INNER] = {0};
        for (uint64_t fi = 0; fi < NCELL_DB; fi++)
            if (flat[fi]) ref[fi / FANOUT_N] += flat[fi]->n;
        for (int k = 0; k < DIM_INNER; k++)
            if (ref[k] != g_fin_la_total[k])
                fatal("la_totals mismatch at left color %d: flat=%llu split=%llu",
                      k, (unsigned long long)ref[k], (unsigned long long)g_fin_la_total[k]);
        fprintf(stderr, "[verify] la_totals agrees with the flat scan\n");
    }
#endif
}

static double fin_left_rank(const int right[PUZZLE_SIDE]) {
    double s = 0.0;
    for (int r = 1; r <= EDGE_LEN; r++)
        if (color_is_inner(right[r]))
            s += log1p((double)g_fin_la_total[INNER_IDX(right[r])]);
    return s;
}

/* Fill a LeftOrder's locked rows (0..finalize_from) from the grid. */
static void fin_left_prefix(LeftOrder *lo) {
    memset(lo, 0, sizeof *lo);
    for (int r = 0; r <= (int)g_finalize_from; r++) {
        lo->p[r] = &g_grid[r][0];
        lo->right[r] = g_grid[r][0].right;
        used_set(lo->used, g_grid[r][0].piece_id);
    }
}

/* Fixed mode: the whole column, TL and TR come straight from the partial. */
static void fin_left_fixed(LeftOrder *lo) {
    fin_left_prefix(lo);
    for (int r = (int)g_finalize_from + 1; r < PUZZLE_SIDE; r++) {
        lo->p[r] = &g_grid[r][0];
        lo->right[r] = g_grid[r][0].right;
        used_set(lo->used, g_grid[r][0].piece_id);
    }
    g_tr_reserved = g_grid[PUZZLE_SIDE-1][PUZZLE_SIDE-1].piece_id;
    lo->rank = 0.0;
}

#define FIN_SAMPLE_RESTARTS 100000

/* Free mode: draw one left-column completion above the locked rows -- the
   best of RANDOM_SIDE_SAMPLES random legal chains of unlocked frame-left
   edges, each grown from the locked column's top piece to a TL corner picked
   at random among the two free corners (the other is reserved for TR). The
   winners are copied to static storage so the LeftOrder pointers stay valid
   for the duration of the configuration. */
static Oriented s_left_store[PUZZLE_SIDE];

/* Commit a chosen column (rows from..14 in seq, plus the TL corner at row 15,
   TR reserved) into a LeftOrder: copy into stable storage and set p/right/used.
   Shared by the random sampler and the exhaustive enumerator; the caller sets
   lo->rank (irrelevant when enumerating). */
static void fin_left_set(LeftOrder *lo, const Oriented seq[PUZZLE_SIDE],
                         Oriented TL, Oriented TR) {
    const int from = (int)g_finalize_from + 1;
    for (int r = from; r <= EDGE_LEN; r++) {
        s_left_store[r] = seq[r];
        lo->p[r] = &s_left_store[r];
        lo->right[r] = seq[r].right;
        used_set(lo->used, seq[r].piece_id);
    }
    s_left_store[PUZZLE_SIDE - 1] = TL;
    lo->p[PUZZLE_SIDE - 1] = &s_left_store[PUZZLE_SIDE - 1];
    lo->right[PUZZLE_SIDE - 1] = TL.right;
    used_set(lo->used, TL.piece_id);
    g_tr_reserved = TR.piece_id;
}

static bool fin_sample_left(RNG *rng, double tau, LeftOrder *lo) {
    fin_build_side_pools();
    fin_left_prefix(lo);
    const int from = (int)g_finalize_from + 1;

    int free_c[4], nfc = 0;
    for (int k = 0; k < s_corner_n; k++)
        if (!used_test(g_lock_mask, (uint16_t)s_corner_ids[k]))
            free_c[nfc++] = s_corner_ids[k];
    if (nfc != 2) fatal("expected exactly 2 unlocked corners, found %d", nfc);

    Oriented best[PUZZLE_SIDE], bestTL, bestTR;
    memset(&bestTL, 0, sizeof bestTL); memset(&bestTR, 0, sizeof bestTR);
    /* have_best, not a negative sentinel: at tau > 0 the key is
       rank/tau + Gumbel, which is frequently below zero. */
    double best_key = 0.0, best_rank = 0.0;
    bool have_best = false;
    uint64_t restarts = 0;
    for (int got = 0; got < RANDOM_SIDE_SAMPLES; ) {
        int tl_id, tr_id;
        if (g_sides_from_rot) {           /* the annealer already chose the corners */
            tl_id = (int)g_cTL.piece_id;  tr_id = (int)g_cTR.piece_id;
        } else {
            int pick = (int)rng_uniform(rng, (uint32_t)nfc);
            tl_id = free_c[pick];         tr_id = free_c[1 - pick];
        }
        Oriented TL, TR;
        bool okc = false, okc2 = false;
        for (uint8_t s = 0; s < 4 && !okc; s++) {
            Oriented o = fin_oriented((uint16_t)tl_id, s);
            if (o.top == 0 && o.left == 0) { TL = o; okc = true; }
        }
        for (uint8_t s = 0; s < 4 && !okc2; s++) {
            Oriented o = fin_oriented((uint16_t)tr_id, s);
            if (o.top == 0 && o.right == 0) { TR = o; okc2 = true; }
        }
        if (!okc || !okc2) return false;

        Oriented seq[PUZZLE_SIDE];
        uint64_t used[4];
        memcpy(used, g_lock_mask, sizeof used);
        used_set(used, (uint16_t)tl_id);
        used_set(used, (uint16_t)tr_id);
        int prev_top = g_grid[from - 1][0].top;
        bool ok = true;
        for (int r = from; r <= EDGE_LEN; r++) {
            int cand[MAX_EDGE_TERMINALS], nc = 0;
            for (int k = 0; k < s_leftz_n; k++) {
                const Oriented *o = &s_leftz[k];
                if (used_test(used, o->piece_id)) continue;
                if (o->bottom != prev_top) continue;
                if (r == EDGE_LEN && o->top != TL.bottom) continue;
                cand[nc++] = k;
            }
            if (nc == 0) { ok = false; break; }
            const Oriented *o = &s_leftz[cand[rng_uniform(rng, (uint32_t)nc)]];
            seq[r] = *o;
            used_set(used, o->piece_id);
            prev_top = o->top;
        }
        if (!ok) {
            if (++restarts > FIN_SAMPLE_RESTARTS) break;
            continue;
        }
        got++;
        int right[PUZZLE_SIDE];
        for (int r = 0; r < from; r++) right[r] = lo->right[r];
        for (int r = from; r <= EDGE_LEN; r++) right[r] = seq[r].right;
        right[PUZZLE_SIDE - 1] = TL.right;
        double rank = fin_left_rank(right);
        double key  = gumbel_key(rank, tau, rng);
        if (!have_best || key > best_key) {
            for (int r = from; r <= EDGE_LEN; r++) best[r] = seq[r];
            bestTL = TL; bestTR = TR;
            best_key = key; best_rank = rank; have_best = true;
        }
    }
    if (!have_best) return false;

    fin_left_set(lo, best, bestTL, bestTR);
    lo->rank = best_rank;
    return true;
}

/* Hash of a column's pieces above the locked rows (duplicate-sample skip). */
static uint64_t fin_left_hash(const LeftOrder *lo) {
    uint64_t h = 0xF17A11E5F17A11E5ULL;
    for (int r = (int)g_finalize_from + 1; r < PUZZLE_SIDE; r++)
        h = splitmix64(h ^ ((uint64_t)lo->p[r]->piece_id << 8) ^ lo->p[r]->rotation);
    return h ? h : 1;
}

/* Run one beam configuration for a chosen left column: reset the per-config emit
   state, search, and print the sweep line. Shared by the sampling loop and the
   exhaustive enumerator. */
/* -- The [sweep] line, and the run-length collapse behind it ----------------- */
/* Kept in step with the beamer's, which carries the full reasoning. In short: a
   configuration that emitted nothing prints only what is not trivially zero, and
   consecutive barren ones that died the same way under the same input line
   collapse into one counted line; and `filled` and `died` are separate fields
   because res.row is the last row COMPLETED for stop_row, time, interrupted and
   parity, but the row that FAILED for an extinction -- where res.width is the
   width the beam carried into that row rather than one it ever reached. */
#define SWEEP_QUIET_SEC 30.0
static char     g_sw_group[64] = "";   /* the line+repeat whose columns pend   */
static char     g_sw_key[96]   = "";   /* how they died; identical or no run   */
static bool     g_sw_armed = false;
static long     g_sw_first = -1, g_sw_last = -1;   /* the SUPPRESSED span      */
static uint32_t g_sw_n     = 0;
static double   g_sw_wall  = 0.0;

/* Does the committed clue orientation pin anything on this row? A clue pins the
   row it sits on and the one below it, to the colour it will stand on. */
static bool clue_row_pinned(int row) {
    if (!g_clue_mask) return false;
    int pi[3], pk[3]; uint16_t pv[3];
    return fin_clue_pins_for(row, pi, pk, pv);
}

static const char *sweep_reason(const BeamResult *br) {
    static char buf[64];
    if (strcmp(br->reason, "extinct") || !clue_row_pinned((int)br->row))
        return br->reason;
    snprintf(buf, sizeof buf, "%s(clue_row)", br->reason);
    return buf;
}

static void sweep_flush(void) {
    if (g_sw_n == 1)
        printf("[sweep] %sl%ld %s wall=%.1fs\n",
               g_sw_group, g_sw_first, g_sw_key, g_sw_wall);
    else if (g_sw_n > 1)
        printf("[sweep] %sl%ld-l%ld x%u %s wall=%.1fs\n",
               g_sw_group, g_sw_first, g_sw_last, g_sw_n, g_sw_key, g_sw_wall);
    if (g_sw_n) fflush(stdout);
    g_sw_group[0] = g_sw_key[0] = '\0';
    g_sw_armed = false; g_sw_first = g_sw_last = -1; g_sw_n = 0; g_sw_wall = 0.0;
}

static void sweep_report(const char *group, long li, const BeamResult *br, double wall) {
    const bool filled = strcmp(br->reason, "extinct") != 0;
    char key[96];
    snprintf(key, sizeof key, "%s=%u width=%u reason=%s",
             filled ? "filled" : "died", br->row, br->width, sweep_reason(br));

    if (g_emit_count + g_partial_count == 0) {
        if (g_sw_armed && !strcmp(g_sw_group, group) && !strcmp(g_sw_key, key)) {
            if (g_sw_n == 0) g_sw_first = li;
            g_sw_last = li; g_sw_n++; g_sw_wall += wall;
            if (g_sw_wall >= SWEEP_QUIET_SEC) sweep_flush();
            return;
        }
        sweep_flush();
        printf("[sweep] %sl%ld %s wall=%.1fs\n", group, li, key, wall);
        fflush(stdout);
        snprintf(g_sw_group, sizeof g_sw_group, "%s", group);
        snprintf(g_sw_key,   sizeof g_sw_key,   "%s", key);
        g_sw_armed = true; g_sw_first = g_sw_last = -1; g_sw_n = 0; g_sw_wall = 0.0;
        return;
    }

    sweep_flush();
    printf("[sweep] %sl%ld %s emitted=%zu", group, li, key, g_emit_count);
    if (g_incomplete_top) printf(" partials=%zu part_total=%zu", g_partial_count, g_partial_total);
    printf(" sol_total=%" PRIu64 " wall=%.1fs\n", g_solution_idx, wall);
    fflush(stdout);
}

static void run_left_config(BeamCtx *ctx, Scratch **scratch, double t_start,
                            uint32_t line, uint32_t rep, size_t idx,
                            const LeftOrder *lft) {
    g_cur_left = lft;
    snprintf(g_config_id_str, sizeof g_config_id_str, "p%ur%ul%zu", line, rep, idx);
    uint64_t cfg_hash = splitmix64(g_master_seed
                        ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

    g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
    if (g_incomplete_top) { g_partial_count = 0; memset(g_partial_htable, 0, g_partial_htable_sz*sizeof(uint64_t)); }
    double tc0 = omp_get_wtime();
    double slice_end = tc0 + g_config_time_sec;
    if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

    BeamResult br = beam_search_config(ctx, scratch, cfg_hash, slice_end);
    /* emitted/partials are this config's unique boards; sol_total/part_total are
       the run totals written so far (the CSVs accumulate across configs). */
    { char grp[64]; snprintf(grp, sizeof grp, "p%ur%u", line, rep);
      sweep_report(grp, (long)idx, &br, omp_get_wtime()-tc0); }
    if (g_completions_fp) fflush(g_completions_fp);
    if (g_partial_fp)     fflush(g_partial_fp);
    partials_budget_announce();
}

/* DFS over legal left columns up to the stop row: choose every unlocked
   frame-left candidate (s_leftz) whose bottom matches the row below,
   backtracking, for col 0 of rows finalize_from+1 .. stop_row only. Rows above
   the stop row are never filled by the beam, so their would-be left edges (and
   the two free corners) are left unreserved; without a matched rotations row the
   search may still spend them as right-edge terminals, with one they belong to
   the left side and only the interfaces they owe are booked (fin_init_entry).
   Each complete chain is a distinct board (col 0 differs), so no duplicates. */
static void fin_enum_rec(BeamCtx *ctx, Scratch **scratch, double t_start,
                         uint32_t line, LeftOrder *lo, Oriented seq[PUZZLE_SIDE],
                         int r, int prev_top, uint64_t used[4], size_t *count) {
    if (g_stop) return;
    if ((uint32_t)r > g_stop_row) {
        if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) {
            printf("[sweep] max_wall reached.\n"); g_stop = 1; return;
        }
        if (partials_budget_spent()) { partials_budget_announce(); return; }
        fin_left_prefix(lo);                 /* locked rows; lo->used = locked only */
        for (int rr = (int)g_finalize_from + 1; rr <= (int)g_stop_row; rr++) {
            s_left_store[rr] = seq[rr];
            lo->p[rr] = &s_left_store[rr];
            lo->right[rr] = seq[rr].right;
            used_set(lo->used, seq[rr].piece_id);
        }
        g_tr_reserved = 0xFFFF;              /* no top corner is placed or reserved */
        lo->rank = 0.0;
        run_left_config(ctx, scratch, t_start, line, 0, (*count)++, lo);
        return;
    }
    for (int k = 0; k < s_leftz_n && !g_stop; k++) {
        const Oriented *o = &s_leftz[k];
        if (used_test(used, o->piece_id)) continue;
        if (o->bottom != prev_top) continue;
        seq[r] = *o;
        used_set(used, o->piece_id);
        fin_enum_rec(ctx, scratch, t_start, line, lo, seq, r + 1, o->top, used, count);
        used_clear(used, o->piece_id);
    }
}

/* Exhaustive enumeration (--top_columns <= 0, whenever the column is not fixed by
   the partial): run the beam on EVERY legal left column above the locked rows, up
   to --stop_row. A matched rotations row shrinks the candidate pool from all 56
   edges to the annealer's 14, which is where most of the saving comes from.
   Returns the count. */
static size_t fin_enumerate_lefts(BeamCtx *ctx, Scratch **scratch, double t_start,
                                  uint32_t line) {
    fin_build_side_pools();
    const int from = (int)g_finalize_from + 1;
    LeftOrder lo;
    Oriented seq[PUZZLE_SIDE];
    uint64_t used[4];
    memcpy(used, g_lock_mask, sizeof used);
    const int prev0 = g_grid[from - 1][0].top;
    size_t count = 0;
    fin_enum_rec(ctx, scratch, t_start, line, &lo, seq, from, prev0, used, &count);
    return count;
}

/* -- Summary ------------------------------------------------------------------ */

static void print_summary(double wall_total, double init_s, double sweep_s) {
    printf("\n================= run summary =================\n");
    printf("[sum] wall %.1fs = init %.1fs (DB build %.1fs, sort %.1fs) + sweep %.1fs\n",
           wall_total, init_s, g_time_db_build, g_time_db_sort, sweep_s);
    if (g_stats.configs) {
        printf("[sum] configs: %" PRIu64 "  (%.1f s/config, %.1f configs/hour)\n",
               g_stats.configs, sweep_s / (double)g_stats.configs,
               3600.0 * (double)g_stats.configs / (sweep_s > 0 ? sweep_s : 1));
        printf("[sum] rows advanced: %" PRIu64 "   candidates: %" PRIu64 "  (%.2f Mcand/s in expand)\n",
               g_stats.rows_advanced, g_stats.cands_total,
               g_stats.t_expand > 0 ? (double)g_stats.cands_total / g_stats.t_expand / 1e6 : 0.0);
        printf("[sum] time split: expand %.1fs  select %.1fs  materialize %.1fs  emit %.1fs\n",
               g_stats.t_expand, g_stats.t_select, g_stats.t_mat, g_stats.t_emit);
        printf("[sum] extinctions by row:");
        bool any = false;
        for (int r = 1; r <= (int)g_stop_row; r++)
            if (g_stats.extinct_at[r]) { printf("  r%d:%" PRIu64, r, g_stats.extinct_at[r]); any = true; }
        if (!any) printf("  none");
        printf("   reached stop_row: %" PRIu64 "\n", g_stats.reached_stop);
    }
    if (g_verbose && g_lambda_maha != 0.0) {
        /* The measured Mahalanobis spread per row -- what --lambda_Mahalanobis
           is denominated in. Printed so the calibration is visible rather than
           implicit: a row reading 0 had too small a sample and stood the
           correction down, and a table far from the reference values means the
           regime moved and the weight no longer means what it did. */
        printf("[sum] maha sd by row:");
        bool anysd = false;
        for (int r = 1; r <= (int)g_stop_row; r++)
            if (g_maha_sd[r] > 0.0) { printf("  r%d:%.3f(n=%.0f)", r, g_maha_sd[r], g_maha_n[r]); anysd = true; }
        if (!anysd) printf("  none measured (every row under the %.0f-sample floor)", MAHA_MIN_SAMPLES);
        printf("\n");
    }
    printf("[sum] emitted unique boards: %" PRIu64 "\n", g_stats.emitted_total);
    if (g_incomplete_top)
        printf("[sum] incomplete-top partials: %zu  (A+B %zu, A+C %zu, B+C %zu)\n",
               g_partial_total, g_part_ab, g_part_ac, g_part_bc);
    fflush(stdout);
}


/* -- main ------------------------------------------------------------------- */

static void usage(const char *a0) {
    fprintf(stderr,
"Usage: %s seed.txt partials.csv [rotations.csv] [options]\n"
"\n"
"E555 finalizer: 5-5-5 beam search started from a partial board. Reads the\n"
"piece seed and one or more partials from a completions CSV (the beamer's own\n"
"output format), locks every piece at or below --finalize_from, rebuilds the\n"
"chain database without the locked pieces, and grows each board upward. Boards\n"
"reaching --stop_row are written to <out_dir>/beam_completions_finalized_<stop_row>.csv.\n"
"\n"
"The optional third positional is a Stage A rotations CSV -- normally the very\n"
"file the beamer was given. A beamer partial has no complete border, so the\n"
"finalizer would fall back to --free_edges and treat all 56 edges as candidates\n"
"for every side. Instead each partial's LOCKED border (rows 0..finalize_from) is\n"
"compared against every row of this file; the first row that assigns those same\n"
"pieces to those same sides is reimposed, so the left column is enumerated from\n"
"the annealer's 14 left edges instead of 56, and the right terminals and\n"
"top-border demands come from the annealer rather than a shared pool. A partial\n"
"that matches no row is searched exactly as before, with a note. Ignored when\n"
"--free_edges is given, or when the partial's border is already complete.\n"
"\n"
"Input lines are deduplicated on the fly: a line whose search state at\n"
"--finalize_from (frontier row, fixed sides, and the set of free pieces --\n"
"the arrangement of the locked rows below does not matter) matches any\n"
"earlier line of the file is skipped with a note; use --finalize_repeats to\n"
"re-run a partial deliberately.\n"
"\n"
"Input / output:\n"
"  --out_dir DIR          output directory for the completions CSV (default beam_out)\n"
"  --border_row N         first data line of partials.csv to load (default 0)\n"
"  --border_row_N N       number of consecutive lines to load (default 1; 0 = every\n"
"                         line from --border_row to the end of the file)\n"
"  --finalize_from N      lock rows 0..N and start the beam at row N+1 (default 5;\n"
"                         a line with an unplaced cell at or below N is skipped).\n"
"                         Low on purpose: the beam grows from ONE locked board, so it\n"
"                         needs rows to widen in before selection can do anything --\n"
"                         start too high and the pool never reaches --beam_width, the\n"
"                         score is never consulted (select_beam returns every\n"
"                         candidate untouched when the pool is under the row width)\n"
"                         and the run degenerates into exhaustive enumeration of\n"
"                         whatever the last few rows allow\n"
"  --finalize_repeats N   sweep each loaded partial N times, with fresh randomness\n"
"                         and (free mode) fresh left columns each time (default 1)\n"
"  --free_edges           free every edge piece above finalize_from into a shared\n"
"                         pool; activated automatically when the partial leaves a\n"
"                         border piece unplaced (fixed sides need all 60 placed)\n"
"  --clue_center          hold the published centre clue (piece 138) on its cell and\n"
"                         spin while the rows above the lock are rebuilt. Without it\n"
"                         a --finalize_from below 7 frees that cell and the search\n"
"                         quietly fills it with something else\n"
"  --clue_corners         likewise for the corner clues. The two on row 2 are usually\n"
"                         locked already; the two on row 13 are RESERVED, never pinned,\n"
"                         and attached to the emitted board only where they land on\n"
"                         empty cells -- they never constrain a searched row\n"
"                         Both flags read the orientation off each input board; a\n"
"                         board whose locked region already contradicts a clue is\n"
"                         skipped with the reason, since no row above can repair it\n"
"  --clue_orient LIST     which orientations a board that carries NO clue may be\n"
"                         searched at: 'auto' (default, all four) or a comma list\n"
"                         from 0,1,2,3. Every partial locked below row 7 is such a\n"
"                         board under --clue_center, the centre clue being the only\n"
"                         reachable one and sitting on row 7 or 8 -- it has committed\n"
"                         to no orientation, so one is CHOSEN rather than read, and\n"
"                         the line is searched once per candidate the lock does not\n"
"                         already contradict. Four different pins against the same\n"
"                         lock, so up to 4x the work of one pass, over one shared\n"
"                         database; name a single orientation to pin it back down.\n"
"                         The passes share the run's column-sampling stream, so each\n"
"                         one sees DIFFERENT left columns -- more coverage, but a\n"
"                         pinned re-run does not reproduce the columns the auto run\n"
"                         gave that orientation. A board that DOES carry a clue is\n"
"                         never re-oriented\n"
"  --incomplete_top       also emit boards that reach --stop_row with only TWO of its\n"
"                         three 5-piece segments -- 11 of the row's 16 pieces -- to a\n"
"                         separate <...>_<stop_row>_partial.csv. All three shapes are\n"
"                         kept: A+B (cols 11-15 unplaced), A+C (cols 6-10) and B+C\n"
"                         (cols 1-5). The two segments that lost their left-hand\n"
"                         neighbour are searched over all 17 inner colors, so expect\n"
"                         roughly 10x the partials of A+B alone; --max_partials caps it.\n"
"                         Both CSVs are APPENDED to, never truncated: a fresh run\n"
"                         adds to whatever the out_dir already holds\n"
"\n"
"Beam shape:\n"
"  --beam_width K         boards kept per row (default 250000, as the beamer)\n"
"  --stop_row R           last row the beam fills, finalize_from+1..14; reaching\n"
"                         boards are emitted (default 11, as the beamer; row 15 is\n"
"                         never searched: placing the top border is trivial for an\n"
"                         external tool)\n"
"  --beam_expand E        late-search width multiplier (default 4; 1 = no expansion)\n"
"  --beam_expand_row R    absolute board row with the full ExK width; half of the\n"
"                         extra width is granted one row earlier (default 8). This is\n"
"                         one of the few settings NOT matched to the beamer's: the row\n"
"                         number is absolute in both, but the search here starts at\n"
"                         finalize_from+1, so at the default it is already past this\n"
"                         threshold on its very first row\n"
"\n"
"Scoring / selection:\n"
"  --lambda_J F           weight of the CLOSURE term, the objective derived from\n"
"                         pairing combinatorics: A_tot*KL(free-color mix || flat) plus\n"
"                         a demand term, in nats like the fan-out terms. The primary\n"
"                         color objective; 0 turns it off (default 1.0, useful 0.5-1)\n"
"  --lambda_Mahalanobis F weight of the piece-structure CORRECTION, in units of its own\n"
"                         per-row standard deviation -- the spread is measured live and\n"
"                         divided out, so F means the same thing at every depth. Small\n"
"                         by design: it correlates ~0.88 with closure, so only the\n"
"                         residual is new (default 0.6, useful 0.3-0.7; 0 = off)\n"
"                         Both terms are always live. --lambda_Mahalanobis 0 is closure\n"
"                         alone and --lambda_J 0 is Mahalanobis alone, which is why\n"
"                         there is no --score_model.\n"
"  --frac_rand F          fraction of the beam selected at random instead of by score,\n"
"                         FLAT across every row (default 0.30). Deliberately well above\n"
"                         the beamer's 0.10 and the one place these two tools should\n"
"                         not agree: the beamer gets one pass at a configuration, while\n"
"                         this one is meant to be re-run over the same partial (see\n"
"                         --finalize_repeats), so the random band is not a tax on one\n"
"                         pass but coverage across many -- and it is the only thing\n"
"                         stopping repeated passes from retracing each other\n"
"  --parent_cap N         max children per parent in the score-selected band;\n"
"                         doubled from beam_expand_row-1 on; 0 = uncapped (default 4)\n"
"\n"
"Feasibility certificates:\n"
"  --no_free_demand       DISABLE the free-mode demand accounting. On by default: an\n"
"                         edge piece owes one inner half-edge of its own color whether\n"
"                         it ends up a right edge, a left edge or a top border piece,\n"
"                         so free mode's demands -- and hence the color parity test --\n"
"                         are exact without knowing which role each will take. Without\n"
"                         this accounting free mode carries no color certificate\n"
"\n"
"Expansion effort:\n"
"  --pool_factor N        candidate-pool target as a multiple of beam_width (default 8)\n"
"\n"
"Sweep control:\n"
"  --top_columns N        left-column completions sampled per partial per repeat\n"
"                         whenever the column is not fixed by the partial itself\n"
"                         (default 12; a complete border always uses exactly 1).\n"
"                         N<=0 enumerates EVERY legal left column exhaustively (no\n"
"                         sampling) -- for an exhaustive top-row search; pass a\n"
"                         rotations.csv to enumerate only the annealer's left edges\n"
"  --bail_columns N       abandon a partial line after N consecutive columns that\n"
"                         reported nothing (completions or --incomplete_top partials),\n"
"                         instead of running all --top_columns x --finalize_repeats.\n"
"                         0 = never bail (default 0), as in the beamer\n"
"  --gumbel_tau_columns T selection temperature for the sampled left column: above 0\n"
"                         the published column is drawn with probability proportional\n"
"                         to exp(rank/tau) instead of being the best of the samples,\n"
"                         which also makes the repeat-dedup exhaust the pool less\n"
"                         often. That rank is symmetric in the row index, an argument\n"
"                         for a high tau that is so far unmeasured on this side and\n"
"                         did not hold up in the beamer. No effect when --top_columns\n"
"                         <=0 enumerates exhaustively. 0 = off, legacy (default 0;\n"
"                         ~4 buys variety at no measured yield cost)\n"
"  --config_time_sec S    wall-time slice per configuration (default 600)\n"
"  --max_wall_sec S       total wall-time budget; 0 = unlimited (default 0)\n"
"  --max_partials N       stop once N boards have been reported, counting both\n"
"                         completions and --incomplete_top partials; the stop-row\n"
"                         beam in flight is always reported in full, so the final\n"
"                         count can overshoot N by up to one beam width\n"
"                         (0 = unlimited; default 0)\n"
"\n"
"Misc:\n"
"  --threads N            OpenMP threads (default: all cores)\n"
"  --seed S               RNG seed; omitted or 0 = randomized from clock+pid and\n"
"                         printed, so repeated runs are uncorrelated\n"
"  --verbose              per-row beam progress lines\n"
"  --help                 this text\n", a0);
}

/* -- --print-cmd ----------------------------------------------------------
 * The whole invocation with every flag carrying the value the run will really
 * use, from the command line or from a default. Copy the line and you have the
 * run. Prints and then continues, so an example can pass it every time.
 *
 * Every flag the parser accepts must appear here; tests/check_script_flags.py
 * enforces it, since a hand-written printer drifts from its parser fast. */
static void print_cmd(const char *a0, const char *seed_path, const char *csv_path,
                      const char *rot_path) {
    printf("[cmd] %s %s %s", a0, seed_path, csv_path);
    if (rot_path) printf(" %s", rot_path);
    if (g_opt_free_edges) printf(" --free_edges");
    if (g_incomplete_top) printf(" --incomplete_top");
    if (g_verbose)        printf(" --verbose");
    if (g_print_cmd)      printf(" --print-cmd");
    if (!g_free_demand)   printf(" --no_free_demand");
    if (g_clue_mask & CLUE_CENTER)  printf(" --clue_center");
    if (g_clue_mask & CLUE_CORNERS) printf(" --clue_corners");
    if (g_clue_orient_req != 0xF) {          /* 0xF is 'auto', the default */
        char oz[16]; size_t on = 0;
        for (int o = 0; o < 4; o++)
            if (g_clue_orient_req & (1u << o))
                on += (size_t)snprintf(oz + on, sizeof oz - on, "%s%d", on ? "," : "", o);
        printf(" --clue_orient %s", on ? oz : "auto");
    }
    printf(" --out_dir %s", g_out_dir);
    printf(" --border_row %u --border_row_N %u", g_border_row_index, g_border_row_N);
    printf(" --finalize_from %u --finalize_repeats %u", g_finalize_from, g_finalize_repeats);
    printf(" --beam_width %u --stop_row %u", g_beam_width, g_stop_row);
    printf(" --beam_expand %u --beam_expand_row %u", g_beam_expand, g_beam_expand_row);
    printf(" --lambda_J %g --lambda_Mahalanobis %g", g_lambda_J, g_lambda_maha);
    printf(" --frac_rand %g --parent_cap %u --pool_factor %u",
           g_frac_rand, g_parent_cap, g_pool_factor);
    printf(" --top_columns %ld --gumbel_tau_columns %g --bail_columns %u",
           g_top_columns, g_tau_columns, g_bail_columns);
    printf(" --threads %d --seed %" PRIu64, g_nthreads, g_master_seed);
    printf(" --config_time_sec %g --max_wall_sec %g --max_partials %" PRIu64 "\n",
           g_config_time_sec, g_max_wall_sec, g_max_partials);
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    if (argc < 3 || argv[1][0] == '-' || argv[2][0] == '-') { usage(argv[0]); return 1; }
    const char *seed_path = argv[1], *csv_path = argv[2], *rot_path = NULL;
    int opt_start = 3;
    if (argc > 3 && argv[3][0] != '-') { rot_path = argv[3]; opt_start = 4; }

    for (int i = opt_start; i < argc; i++) {
        if      (!strcmp(argv[i], "--out_dir")     && i+1 < argc) g_out_dir = argv[++i];
        else if (!strcmp(argv[i], "--border_row")  && i+1 < argc) g_border_row_index = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--border_row_N")&& i+1 < argc) g_border_row_N = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--finalize_from")    && i+1 < argc) g_finalize_from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--finalize_repeats") && i+1 < argc) g_finalize_repeats = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--free_edges"))                g_opt_free_edges = true;
        else if (!strcmp(argv[i], "--clue_center"))               g_clue_mask |= CLUE_CENTER;
        else if (!strcmp(argv[i], "--clue_corners"))              g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(argv[i], "--clue_orient")  && i+1 < argc) g_clue_orient_req = parse_clue_orient(argv[++i]);
        else if (!strcmp(argv[i], "--incomplete_top"))            g_incomplete_top = true;
        else if (!strcmp(argv[i], "--beam_width")  && i+1 < argc) g_beam_width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stop_row")    && i+1 < argc) g_stop_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand") && i+1 < argc) g_beam_expand = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand_row") && i+1 < argc) g_beam_expand_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_Mahalanobis") && i+1 < argc) g_lambda_maha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_J")    && i+1 < argc) g_lambda_J = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no_free_demand"))            g_free_demand = false;
        else if (!strcmp(argv[i], "--frac_rand")   && i+1 < argc) g_frac_rand = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bail_columns") && i+1 < argc) g_bail_columns = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--parent_cap")  && i+1 < argc) g_parent_cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool_factor") && i+1 < argc) g_pool_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top_columns") && i+1 < argc) g_top_columns = atol(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau_columns") && i+1 < argc) g_tau_columns = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")        && i+1 < argc) { unsigned long long s; if (!parse_u64_token(argv[++i], &s)) fatal("--seed needs an integer"); g_master_seed = (uint64_t)s; }
        else if (!strcmp(argv[i], "--config_time_sec") && i+1 < argc) g_config_time_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_wall_sec")    && i+1 < argc) g_max_wall_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_partials")    && i+1 < argc) g_max_partials = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--print-cmd"))       g_print_cmd = true;
        else if (!strcmp(argv[i], "--verbose"))         g_verbose = true;
        else { fprintf(stderr, "Unknown argument: %s\n\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (g_nthreads <= 0) g_nthreads = omp_get_max_threads();
    if (g_master_seed == 0) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        g_master_seed = splitmix64(((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec
                                   ^ ((uint64_t)getpid() << 44));
        if (g_master_seed == 0) g_master_seed = 1;
    }
    if (g_beam_width == 0) fatal("--beam_width must be positive");
    if (g_beam_width > (1u << 24)) fatal("--beam_width above 2^24 not supported");
    if (g_beam_expand < 1 || g_beam_expand > 64) fatal("--beam_expand must be in 1..64");
    if (g_beam_expand_row < 2) fatal("--beam_expand_row must be >= 2");
    if (g_finalize_from > (uint32_t)(EDGE_LEN - 1))
        fatal("--finalize_from must be in 0..%d (at least one row must remain searchable)",
              EDGE_LEN - 1);
    if (g_stop_row > (uint32_t)EDGE_LEN)
        fatal("--stop_row above %d rejected: the top border (row 15) is deliberately not\n"
              "in the database -- placing it on a finished row 14 is trivial externally", EDGE_LEN);
    if (g_stop_row <= g_finalize_from)
        fatal("--stop_row (%u) must exceed --finalize_from (%u)", g_stop_row, g_finalize_from);
    /* No clue cap on --stop_row or --finalize_from. Entries 3..4 sit on row 13,
       are RESERVED rather than pinned, and the attach in format_board_tail yields
       to whatever the search placed -- so searching row 13 builds it from other
       pieces and the clues are left off, instead of landing on top of them. */
    if (g_finalize_repeats < 1) g_finalize_repeats = 1;
    if (!(fabs(g_lambda_maha) <= 1e6)) fatal("--lambda_Mahalanobis in [-1e6,1e6]");
    if (!(fabs(g_lambda_J) <= 1e6))    fatal("--lambda_J in [-1e6,1e6]");
    if (!(g_tau_columns >= 0.0 && g_tau_columns <= 1e6)) fatal("--gumbel_tau_columns in [0,1e6]");
    if (g_frac_rand < 0.0 || g_frac_rand > 1.0) fatal("--frac_rand must be in [0,1]");
    if (g_pool_factor == 0) g_pool_factor = 1;

    /* --border_row_N 0 means "the rest of the file". Resolved before the banner so
       [cfg] and --print-cmd report the count the run will really use, and so a
       caller feeding a whole stage's output need not count the lines itself. */
    if (g_border_row_N == 0) {
        uint32_t csv_lines = fin_count_data_lines(csv_path);
        g_border_row_N = (csv_lines > g_border_row_index)
                       ? csv_lines - g_border_row_index : 0;
    }

    printf("\n=== E555 finalizer ===\n\n");
    if (g_print_cmd) print_cmd(argv[0], seed_path, csv_path, rot_path);
    printf("[cfg] seed_file=%s partials_file=%s out_dir=%s\n",
           seed_path, csv_path, g_out_dir);
    printf("[cfg] seed=%" PRIu64 " threads=%d verbose=%d lines=%u+%u finalize_from=%u repeats=%u incomplete_top=%d\n",
           g_master_seed, g_nthreads, g_verbose ? 1 : 0, g_border_row_index, g_border_row_N,
           g_finalize_from, g_finalize_repeats, g_incomplete_top ? 1 : 0);
    printf("[cfg] beam_width=%u stop_row=%u expand=%ux@row%u\n",
           g_beam_width, g_stop_row, g_beam_expand, g_beam_expand_row);
    printf("[cfg] frac_rand=%.2f (flat) parent_cap=%u pool_factor=%u\n",
           g_frac_rand, g_parent_cap, g_pool_factor);
    printf("[cfg] lambda_J=%.3f lambda_Maha=%.3f free_demand=%d\n",
           g_lambda_J, g_lambda_maha, g_free_demand ? 1 : 0);
    printf("[cfg] gumbel_tau_columns=%.2f\n", g_tau_columns);
    printf("[cfg] top_columns=%ld bail_columns=%u config_time=%.0fs max_wall=%.0fs max_partials=%" PRIu64 " free_edges=%s\n",
           g_top_columns, g_bail_columns, g_config_time_sec, g_max_wall_sec, g_max_partials,
           g_opt_free_edges ? "forced" : "auto (per line)");
    if (g_clue_mask) {
        char oz[16]; size_t on = 0;
        for (int o = 0; o < 4; o++)
            if (g_clue_orient_req & (1u << o)) on += (size_t)snprintf(oz + on, sizeof oz - on, "%s%d", on ? "," : "", o);
        printf("[cfg] clue_center=%d clue_corners=%d clue_orient=%s (read off a clued "
               "board, chosen from these when it carries none)\n",
               (g_clue_mask & CLUE_CENTER) ? 1 : 0, (g_clue_mask & CLUE_CORNERS) ? 1 : 0, oz);
    }
    if (g_clue_orient_req != 0xF && !g_clue_mask)
        fatal("--clue_orient needs --clue_center or --clue_corners: with clues off "
              "there is no orientation to choose");
    if (rot_path && g_opt_free_edges)
        printf("[note] --free_edges given: the rotations file '%s' is ignored\n", rot_path);
    fflush(stdout);

    double t_start = omp_get_wtime();
    ensure_dir(g_out_dir);
    load_seed_and_catalog(seed_path);
    build_catalog_indices();
    build_inner_color_totals();
    init_check_inner_faces();
    build_maha_tables();
    build_logtab();
    fin_init_clue_tables();          /* needs the oriented catalog, nothing else */
    if (rot_path && !g_opt_free_edges) fin_load_rotations(rot_path);

    BeamCtx ctx; beam_ctx_alloc(&ctx);
    Scratch **scratch = xmalloc((size_t)g_nthreads * sizeof(Scratch *));
    for (int t = 0; t < g_nthreads; t++) { scratch[t] = xmalloc(sizeof(Scratch)); memset(scratch[t], 0, sizeof(Scratch)); }

    signal(SIGINT, handle_stop); signal(SIGTERM, handle_stop);

    /* One completions file for the whole run, append mode: each emitted line is
       flushed as a single O_APPEND write, so several instances on the SAME
       machine can share one file. Instances on different machines should use
       separate --out_dir's (network filesystems do not guarantee atomic
       appends) and concatenate afterwards. */
    htable_init();
    if (g_incomplete_top) partial_htable_init();
    char comp_path[1024];
    snprintf(comp_path, sizeof comp_path, "%s/beam_completions_finalized_%u.csv", g_out_dir, g_stop_row);
    g_completions_fp = fopen(comp_path, "a");
    if (!g_completions_fp) fatal("cannot open %s: %s", comp_path, strerror(errno));
    setvbuf(g_completions_fp, NULL, _IOFBF, EMIT_FILE_BUF);
    printf("[out] completions -> %s (append)\n", comp_path);
    manifest_add(comp_path);
    char part_path[1024] = "";
    if (g_incomplete_top) {
        snprintf(part_path, sizeof part_path, "%s/beam_completions_finalized_%u_partial.csv", g_out_dir, g_stop_row);
        g_partial_fp = fopen(part_path, "a");
        if (!g_partial_fp) fatal("cannot open %s: %s", part_path, strerror(errno));
        setvbuf(g_partial_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] incomplete-top partials -> %s (append)\n", part_path);
        manifest_add(part_path);
    }
    fflush(stdout);
    g_solution_idx = 0;

    /* Input dedup: hash the lines before the window so a window line that
       repeats anything earlier in the file is skipped (see fin_partial_hash). */
    fin_prescan_hashes(csv_path, g_border_row_index);

    RNG srng = rng_for(g_master_seed, 0xF17A11E5u, 0, 0);
    double t_sweep0 = omp_get_wtime();
    double init_s = t_sweep0 - t_start;

    for (uint32_t line = g_border_row_index;
         line < g_border_row_index + g_border_row_N && !g_stop; line++) {
        printf("\n========== partial line %u ==========\n", line); fflush(stdout);

        char src_id[64] = "?";
        int got = fin_load_partial(csv_path, line, src_id);
        if (got < 0) { printf("[sweep] partials line %u not found; stopping.\n", line); break; }
        if (got == 0) continue;

        /* Side mode for this line. A complete border fixes the sides (and the
           column) from the partial itself; otherwise a rotations row that matches
           the locked border fixes the sides while the column is still searched;
           failing both, every edge is free. */
        g_sides_from_rot = false;
        g_rot_matched    = -1;
        if (g_opt_free_edges) g_free_edges = true;
        else if (fin_border_complete()) g_free_edges = false;
        else if (g_rot_n && fin_rot_match(line)) {
            g_free_edges     = false;
            g_sides_from_rot = true;
        } else {
            g_free_edges = true;
            printf("[note] line %u: unplaced border pieces -> --free_edges activated automatically\n", line);
        }
        char mode_str[32];
        if (g_sides_from_rot) snprintf(mode_str, sizeof mode_str, "fixed(rot %d)", g_rot_matched);
        else                  snprintf(mode_str, sizeof mode_str, "%s", g_free_edges ? "free" : "fixed");
        printf("[sweep] line %u: source '%s'  depth=%d  lock rows 0..%u  mode=%s\n",
               line, src_id, g_partial_depth, g_finalize_from, mode_str); fflush(stdout);

        if (!g_free_edges && !g_sides_from_rot) {  /* sides from the partial itself */
            for (int r = 0; r < PUZZLE_SIDE; r++)
                for (int c = 0; c < PUZZLE_SIDE; c++)
                    if (r == 0 || r == PUZZLE_SIDE-1 || c == 0 || c == PUZZLE_SIDE-1)
                        g_spin[g_grid[r][c].piece_id] = g_grid[r][c].rotation;
            classify_deal_from_rotations();
        }
        build_top_border_demands();   /* zero in free mode */

        fin_build_db();               /* reduced DB (inner skipped if mask unchanged) */
        fin_la_totals();
        g_cBL = g_grid[0][0];                g_has_cBL = true;
        g_cBR = g_grid[0][PUZZLE_SIDE-1];    g_has_cBR = true;
        validate_color_constants();

        /* One pass per candidate orientation. The loop starts AFTER
           fin_build_db so all four passes share one reduced database: the pins
           move between orientations, the excluded pieces do not (fin_clue_reserve).
           g_stop, --max_wall_sec and the partials budget are tested inside, so a
           budget still stops the line part-way through its orientations. */
        for (int oi = 0; oi < (g_fin_orient_n ? g_fin_orient_n : 1) && !g_stop; oi++) {
            g_fin_orient = g_fin_orient_n ? g_fin_orient_cand[oi] : -1;
            fin_clue_dump_schedule();
            if (g_fin_orient_n > 1)
                printf("[sweep] line %u: orientation %d (%d of %d)\n",
                       line, g_fin_orient, oi + 1, g_fin_orient_n);
            /* The left column is searched unless the partial fixes it outright: a
               matched rotations row constrains WHICH pieces may sit in it, not their
               order, so those lines still enumerate or sample columns. */
            const bool cols_free = g_free_edges || g_sides_from_rot;

            bool exhaustive = cols_free && g_top_columns <= 0;
            if (exhaustive) {
                /* Test EVERY legal left column up to the stop row (no sampling).
                   Deterministic, so a single pass -- repeats would only duplicate. */
                size_t n = fin_enumerate_lefts(&ctx, scratch, t_start, line);
                printf("[sweep] line %u: enumerated %zu left columns\n", line, n);
                if (n == 0 && !g_stop)
                    printf("[warn] line %u: no legal left column above row %u%s\n",
                           line, g_finalize_from,
                           g_sides_from_rot ? " from the matched rotations row's left edges" : "");
            } else {
                size_t run_l = (!cols_free) ? 1
                             : (g_top_columns >= 1 ? (size_t)g_top_columns : 1);
                uint64_t *tried = xmalloc(run_l * sizeof(uint64_t));

                uint32_t barren = 0;      /* consecutive columns that reported nothing */
                for (uint32_t rep = 0; rep < g_finalize_repeats && !g_stop; rep++) {
                    size_t tried_n = 0;
                    LeftOrder lft;
                    for (size_t li = 0; li < run_l && !g_stop; li++) {
                        if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) {
                            printf("[sweep] max_wall reached.\n"); g_stop = 1; break;
                        }
                        if (partials_budget_spent()) { partials_budget_announce(); break; }
                        if (cols_free) {
                            bool fresh = false, any = false;
                            uint64_t h = 0;
                            for (int attempt = 0; attempt < 16 && !fresh; attempt++) {
                                if (!fin_sample_left(&srng, g_tau_columns, &lft)) break;
                                any = true;
                                h = fin_left_hash(&lft);
                                fresh = true;
                                for (size_t q = 0; q < tried_n; q++)
                                    if (tried[q] == h) { fresh = false; break; }
                            }
                            if (!fresh) {
                                if (!any)
                                    printf("[warn] line %u: no legal left column above row %u\n",
                                           line, g_finalize_from);
                                else if (g_verbose)
                                    printf("[note] line %u rep %u: left columns exhausted after %zu\n",
                                           line, rep, tried_n);
                                break;
                            }
                            tried[tried_n++] = h;
                        } else {
                            fin_left_fixed(&lft);
                        }
                        uint64_t got0 = g_stats.emitted_total + g_partial_total;
                        run_left_config(&ctx, scratch, t_start, line, rep, li, &lft);
                        barren = (g_stats.emitted_total + g_partial_total > got0) ? 0 : barren + 1;
                        if (g_bail_columns && barren >= g_bail_columns) {
                            sweep_flush();
                            printf("[bail] line %u: %u column(s) in a row reported nothing, "
                                   "moving to the next line\n", line, barren);
                            fflush(stdout);
                            break;
                        }
                    }
                    sweep_flush();   /* never straddle a repeat */
                    if (g_bail_columns && barren >= g_bail_columns) break;
                }
                free(tried);
            }
        }
    }

    sweep_flush();
    double wall = omp_get_wtime() - t_start;
    print_summary(wall, init_s, omp_get_wtime() - t_sweep0);
    printf("[sum] completions file: %s\n", comp_path);
    if (g_incomplete_top) printf("[sum] partials file: %s\n", part_path);
    if (g_completions_fp) fclose(g_completions_fp);
    if (g_partial_fp) fclose(g_partial_fp);
    manifest_write(g_out_dir);          /* after the closes: it stats file sizes */
    return 0;
}
