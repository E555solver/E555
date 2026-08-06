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

/* -- soft-center-139 tuning -------------------------------------------------- */
/* Piece 139 (the published center clue) is barred from rows 1..RELEASE_ROW-1,
   and any board that happens to place it on one of the four true center cells
   -- (row,col) in {7,8}x{7,8} -- earns --bonus_139 on every subsequent score, so
   the lineage is promoted all the way up while it keeps finding matches.

   The bonus is additive on a score measured in nats of log record count, so its
   value IS a claimed factor in continuability: the historical 10.0 asserted that
   a center-139 board is worth e^10 ~ 2.2e4 times one without, which no amount of
   real fan-out difference can overcome. The default is now 1.0 -- about one
   standard deviation of the color term -- so it breaks near-ties in favour of a
   139 lineage without overriding a genuinely more continuable board. */
#define SOFT_CENTER_RELEASE_ROW 6

/* Stop-row emission safety cap (lines per config; the CSV rows are ~2 KB). */
#define EMIT_MAX 1000000u

static uint64_t g_master_seed = 0;

static uint32_t g_beam_width      = 262144;
static uint32_t g_stop_row        = 12;
static uint32_t g_beam_expand     = 5;
static uint32_t g_beam_expand_row = 8;
static double   g_lambda_maha     = 0.0;
static double   g_frac_rand       = 0.75;
static double   g_bonus_139       = 1.0;
static bool     g_score_model_J   = false;  /* --score_model J (default legacy) */
static double   g_lambda_J        = 1.0;
static bool     g_avail_correct   = false;
static bool     g_free_demand     = true;   /* --no_free_demand turns it off */
static uint32_t g_supply_check    = 0;      /* first row tested; 0 = off */
static double   g_gumbel_tau0     = 0.0;    /* 0 = off (exact legacy selection) */
static double   g_gumbel_tau1     = 0.0;
static uint32_t g_parent_cap      = 5;
static uint32_t g_pool_factor     = 8;
static uint32_t g_scan_factor     = 1024;
static const char *g_out_dir      = "beam_out";

static uint32_t g_border_row_index = 0;   /* first data line of the partials CSV */
static uint32_t g_border_row_N     = 1;   /* number of consecutive lines to load */
static uint32_t g_finalize_from    = 8;   /* lock rows 0..N; search starts at N+1 */
static uint32_t g_finalize_repeats = 1;   /* full sweeps per input partial */
static double   g_config_time_sec  = 600.0;
static double   g_max_wall_sec     = 0.0;
static uint64_t g_max_partials     = 0;   /* reported-board budget; 0 = unlimited */
static long     g_top_columns      = 10;  /* sampled left columns per partial (free mode) */
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

/* Random selection band: full early (the fan-out heuristic knows little when the
   board is mostly empty), half at expand_row-1, zero from expand_row on (pure
   fan-out selection where only a few legal completions remain). Finalizer
   exception: the FIRST searched row (finalize_from + 1) always keeps the full
   input fraction, whatever the expansion schedule says -- with a high
   finalize_from every searched row would otherwise be purely fan-out selected
   and repeated runs over the same partial would retrace each other. */
static double frac_rand_eff(int row) {
    if ((uint32_t)row <= g_finalize_from + 1) return g_frac_rand;
    if ((uint32_t)row < g_beam_expand_row - 1) return g_frac_rand;
    if ((uint32_t)row == g_beam_expand_row - 1) return g_frac_rand * 0.5;
    return 0.0;
}

/* Selection temperature (--gumbel_tau0/--gumbel_tau1), interpolated linearly
   from tau0 at row 1 to tau1 at --stop_row -- absolute row meaning, like every
   other schedule here, so a high --finalize_from starts already cooled. 0 = off,
   the default, which restores the legacy path exactly (frac_rand's uniform band
   plus greedy top-K). Because the score is already a log record count, tau = 1
   samples boards in proportion to their estimated number of completions. */
static double gumbel_tau_eff(int row) {
    if (g_gumbel_tau0 <= 0.0 && g_gumbel_tau1 <= 0.0) return 0.0;
    if (g_stop_row <= 1) return g_gumbel_tau0;
    double f = (double)(row - 1) / (double)(g_stop_row - 1);
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    double tau = g_gumbel_tau0 + f * (g_gumbel_tau1 - g_gumbel_tau0);
    return tau > 0.0 ? tau : 0.0;
}

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
   maha_term's n = 14*depth stays exact); req_exposed carries the locked
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

    for (int r = 1; r <= (int)g_finalize_from; r++)
        for (int c = 1; c <= EDGE_LEN; c++) {
            const Oriented *o = &g_grid[r][c];
            p->color_consumed[o->top]++;    p->color_consumed[o->right]++;
            p->color_consumed[o->bottom]++; p->color_consumed[o->left]++;
        }

    for (int c = 0; c < PUZZLE_SIDE; c++)
        p->rtop[c] = g_grid[g_finalize_from][c].top;
    for (int c = 1; c <= EDGE_LEN; c++)         /* inner frontier (cols 1..14) */
        if (color_is_inner(p->rtop[c])) p->req_exposed[p->rtop[c]]++;

    if (!g_free_edges) {
        /* Remaining left interfaces. Every unlocked left-side edge ends up in some
           row finalize_from+1..14, so the multiset of colors it owes the search is
           independent of the column's ordering -- and unlike lft->right[], the pool
           is complete even when the exhaustive enumerator fixed the column only up
           to --stop_row (fin_enum_rec). */
        for (int k = 0; k < g_left_count_pool; k++) {
            const Oriented *lp = &g_left_pool[k];
            if (used_test(g_lock_mask, lp->piece_id)) continue;
            if (color_is_inner(lp->right)) p->req_exposed[lp->right]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {  /* remaining right edges */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[cl]++;
        }
        for (int c = COLOR_MIN; c <= COLOR_MAX; c++)
            p->req_exposed[c] = (int16_t)(p->req_exposed[c] + g_top_border_inner_count[c]);
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
            if (color_is_inner(cr)) p->req_exposed[cr]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {
            if (used_test(p->used, g_edge_term[t].piece_id)) continue;  /* already placed */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[cl]++;
        }
    }

    /* Piece 139: bonus if it is already locked on a center cell; the rows-1..5
       reservation only applies while barred rows are still ahead of the search
       (released at SOFT_CENTER_RELEASE_ROW by the beam loop, as in the beamer). */
    if (g_soft_center_139) {
        for (int r = 7; r <= 8; r++)
            for (int c = 7; c <= 8; c++)
                if (r <= (int)g_finalize_from && g_grid_has[r][c] &&
                    g_grid[r][c].piece_id == SOFT_CENTER_139_PIECE)
                    p->flags |= FLAG_BONUS_139;
        if (!used_test(p->used, SOFT_CENTER_139_PIECE) &&
            g_finalize_from + 1 < SOFT_CENTER_RELEASE_ROW)
            used_set(p->used, SOFT_CENTER_139_PIECE);
    }
}

/* Commit one inner row (cols 1..15) into the board (counters + frontier only;
   the move itself is logged by the caller). */
static void commit_row(BeamEntry *p, int row, const RowChoice *rc) {
    const Oriented *term = &g_edge_term[rc->rterm];

    for (int i = 0; i < EDGE_LEN; i++) {
        const Oriented *o = &g_cat[rc->ci[i]];
        used_set(p->used, o->piece_id);
        p->color_consumed[o->top]++;    p->color_consumed[o->right]++;
        p->color_consumed[o->bottom]++; p->color_consumed[o->left]++;
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
        if (color_is_inner(old_t)) p->req_exposed[old_t]--;
        if (color_is_inner(new_t)) {
            if (row < EDGE_LEN)      p->req_exposed[new_t]++;
            else if (demand_exact)   p->req_exposed[new_t]--;
        }
    }
    for (int c = 0; c < PUZZLE_SIDE; c++) p->rtop[c] = new_top[c];

    if (demand_exact) {
        int la = g_cur_left->right[row];           /* segment-A interface satisfied */
        if (color_is_inner(la)) p->req_exposed[la]--;
        int tl = term->left;                       /* right-edge interface satisfied */
        if (color_is_inner(tl)) p->req_exposed[tl]--;
    }

    /* Center-139 bonus: rows 7/8, cols 7/8 are the board's four center cells
       (ci[i] holds column i+1). The flag persists in every descendant. */
    if (g_soft_center_139 && (row == 7 || row == 8) &&
        (g_cat[rc->ci[6]].piece_id == SOFT_CENTER_139_PIECE ||
         g_cat[rc->ci[7]].piece_id == SOFT_CENTER_139_PIECE))
        p->flags |= FLAG_BONUS_139;
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
        int S = g_inner_color_total[c] - p->color_consumed[c] - p->req_exposed[c];
        if (S < 0) return false;
        if (demand_exact && (S & 1)) return false;
    }
    return true;
}

/* Piece-supply certificate (Hall's condition, singleton case; --supply_check).
   Each of the 14 frontier columns needs a DISTINCT remaining inner piece
   carrying its exposed top color. Columns demanding the same color have
   identical candidate sets, so Hall's condition binds first on whole color
   classes: if a color is wanted by more columns than there are unused pieces
   carrying it anywhere, no perfect matching exists and the board is dead.
   This counts PIECES where parity_ok counts HALF-EDGES, so neither implies the
   other -- a piece with two sides of color c adds 2 to that color's surplus but
   can still serve only one column. Applied on beam rows only, where a further
   inner row is guaranteed to exist above the frontier. */
static bool supply_ok(const BeamEntry *p) {
    int need[NUM_COLORS_TOTAL] = {0};
    for (int c = 1; c <= EDGE_LEN; c++) need[p->rtop[c]]++;   /* always inner */
    for (int col = COLOR_MIN; col <= COLOR_MAX; col++) {
        if (!need[col]) continue;
        const uint64_t *m = g_color_pieces[col];
        int have = __builtin_popcountll(m[0] & ~p->used[0])
                 + __builtin_popcountll(m[1] & ~p->used[1])
                 + __builtin_popcountll(m[2] & ~p->used[2])
                 + __builtin_popcountll(m[3] & ~p->used[3]);
        if (have < need[col]) return false;
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
   as the 512 comma-separated fields that follow a line's prefix. cmax_stop is
   the last placed column of the TOP row -- PUZZLE_SIDE-1 for a completed stop
   row, 10 for an --incomplete_top A+B partial whose cols 11..15 stay unplaced;
   every row below it is full either way. Returns the byte count. */
static int format_board_tail(const RowChoice rows[EDGE_LEN + 1], int row,
                             int cmax_stop, char *out) {
    uint32_t pos[NUM_PIECES], rot_arr[NUM_PIECES];
    for (int i = 0; i < NUM_PIECES; i++) { pos[i] = 999; rot_arr[i] = 0; }
    for (int r = 0; r <= row; r++) {
        int cmax = (r == row) ? cmax_stop : PUZZLE_SIDE - 1;
        for (int c = 0; c <= cmax; c++) {
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            pos[pid] = (uint32_t)(r * PUZZLE_SIDE + c); rot_arr[pid] = rot;
        }
    }
    char *p = out;
    for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, pos[i]); }
    for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, rot_arr[i]); }
    *p++ = '\n';
    return (int)(p - out);
}

/* Emit one --incomplete_top partial: the parent's ancestry plus segments A and B
   of the stop row (mv.ci[0..9]); columns 11..15 of that row are left unplaced
   (pos 999). Called from inside the parallel expansion, so the dedup + write are
   guarded by an OpenMP critical -- but the ancestry walk, the fingerprint and the
   512 field conversions are read-only over expansion-stable state and are nearly
   all of the cost, so they happen BEFORE the lock is taken. A board that then
   loses the dedup was formatted for nothing, which is cheap: that work is
   parallel, whereas everything inside the critical stalls every other thread. */
static void emit_incomplete(const BeamCtx *ctx, const BeamEntry *parent,
                            const RowChoice *mv, int row) {
    if (!g_partial_fp) return;
    RowChoice rows[EDGE_LEN + 1];
    rows[row] = *mv;                        /* only ci[0..9] (A+B) are read below */
    collect_rows(ctx, parent, rows);

    /* Fingerprint over placed cells only: full rows below, plus cols 0..10 of the
       partial stop row (left edge + segment A + segment B). */
    const uint64_t prime = 1099511628211ULL;
    uint64_t fp = 14695981039346656037ULL;
    for (int r = 0; r <= row; r++) {
        int cmax = (r == row) ? 10 : PUZZLE_SIDE - 1;
        for (int c = 0; c <= cmax; c++) {
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            fp ^= pid; fp *= prime; fp ^= rot; fp *= prime;
        }
    }
    if (!fp) fp = 1;

    char line[EMIT_LINE_MAX];
    int len = format_board_tail(rows, row, 10, line);

    #pragma omp critical(e555_incomplete)
    {
        if (g_partial_fp && partial_htable_insert(fp)) {
            uint64_t sol_idx = g_partial_total++;
            fprintf(g_partial_fp, "%s, %" PRIu64, g_config_id_str, sol_idx);
            fwrite(line, 1, (size_t)len, g_partial_fp);
        }
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

/* The maha term is active in the mid-board (rows 3..9), peaking at row 6: early
   rows have too little signal, late rows should be pure fan-out. */
static inline double maha_schedule(int row) {
    if (row < 3 || row > 9) return 0.0;
    return 1.0 / (1.0 + (double)abs(6 - row));
}
static inline double maha_term(const BeamEntry *t, int row) {
    if (g_lambda_maha == 0.0) return 0.0;
    double sched = maha_schedule(row);
    if (sched == 0.0) return 0.0;
    int n = EDGE_LEN * row, M = g_maha_M;
    double fn = (double)n*(double)(M-n) / ((double)M*(double)M*(double)(M-1));
    if (fn <= 0.0) return 0.0;
    double n_over_M = (double)n/(double)M, d2 = 0.0;
    for (int k = 0; k < MAHA_DIM; k++) {
        double wx = -n_over_M*g_maha_Wtotal[k];
        for (int ci = 0; ci < NUM_INNER_COLORS; ci++) wx += g_maha_W[k][ci]*(double)t->color_consumed[COLOR_MIN+ci];
        d2 += wx*wx;
    }
    double d2n = d2 / (fn*16.0);
    return sched * g_lambda_maha * d2n;
}

/* -- The J objective: exact pairing combinatorics (--score_model J) ---------- */
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
 * value near 0 instead of near 1600 nats, so --bonus_139 and any swept weight
 * keep their meaning when --score_model is flipped.
 *
 * This matters more here than in the beamer: maha_term infers the placed-piece
 * count from n = 14*row, whereas J reads S and R straight off the board, so it
 * stays exact from a locked partial at any --finalize_from. */
#define LOGTAB_N 801                    /* > 784 = every inner half-edge */
static double g_logtab[LOGTAB_N];

static void build_logtab(void) {
    g_logtab[0] = 0.0;                  /* the x*log(x) -> 0 limit at x = 0 */
    for (int i = 1; i < LOGTAB_N; i++) g_logtab[i] = log((double)i);
}

static inline double j_term(const BeamEntry *t) {
    int S[NUM_INNER_COLORS], R[NUM_INNER_COLORS], D[NUM_INNER_COLORS];
    int sumS = 0, sumR = 0;
    for (int ci = 0; ci < NUM_INNER_COLORS; ci++) {
        int c = COLOR_MIN + ci;
        R[ci] = g_inner_color_total[c] - t->color_consumed[c];
        D[ci] = t->req_exposed[c];
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
    return g_lambda_J * (0.5 * conc + dem);
}

/* Availability correction (--avail_correct). fB and fC come from db_seg_fanout,
 * which sums record counts over all 17 left colors with NO reference to used[],
 * so two boards with identical exposed tops but disjoint remaining piece sets
 * score identically. alpha_c = R_c/tot_c is the fraction of color-c inner
 * half-edges still in the reservoir, so a chain sitting on colors b1..b5
 * survives with probability ~ prod_k alpha_{b_k} to first order; summed in log
 * space over the 14 inner frontier colors that is one term per segment-A/B/C
 * bottom (segment C's fifth bottom is an edge-interface color, whose terminals
 * come from a different pool). No record is decoded and no cell is scanned:
 * a closed-form discount on numbers already looked up, self-scheduling because
 * alpha ~ 1 until the reservoir actually empties. */
static inline double avail_term(const BeamEntry *t) {
    double s = 0.0;
    for (int c = 1; c <= EDGE_LEN; c++) {
        int col = t->rtop[c];                       /* inner: checked by the caller */
        int R = g_inner_color_total[col] - t->color_consumed[col];
        if (R <= 0) return -1000.0;                 /* unreachable: parity_ok rejects S<0 */
        s += g_logtab[R] - g_logtab[g_inner_color_total[col]];
    }
    return s;
}

/* The color-structure part of the score, under whichever model is selected. */
static inline double color_term(const BeamEntry *t, int row) {
    return g_score_model_J ? j_term(t) : maha_term(t, row);
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

    double s = log((double)cA->n) + log1p((double)fB) + log1p((double)fC)
               + color_term(t, row);
    if (g_avail_correct) s += avail_term(t);
    if (t->flags & FLAG_BONUS_139) s += g_bonus_139;
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
    if (!color_is_inner(la_B)) return;
    const Cell *cB = segB_cell(p, la_B);
    if (!cB) return;

    uint64_t forbidA[4] = { p->used[0]|maskA[0], p->used[1]|maskA[1],
                            p->used[2]|maskA[2], p->used[3]|maskA[3] };
    BeamEntry *t = &sc->tmp;
    RowChoice mv;
    memcpy(&mv.ci[0], ciA, CHAIN_LEN * sizeof(uint16_t));

    if (e->at_stop) {
        for (uint32_t jb = 0; jb < cB->n && e->quota > 0; jb++) {
            uint16_t ciB[CHAIN_LEN]; int la_C;
            if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) continue;
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            if (!color_is_inner(la_C)) {
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row);
                continue;
            }
            const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                 [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
            if (!cC) {
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row);
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
                   next stage's problem, so --supply_check does not apply here
                   either. Rank by the heuristic terms only. */
                float score = (float)(color_term(t, e->row)
                              + ((t->flags & FLAG_BONUS_139) ? g_bonus_139 : 0.0));
                pool_append(ctx, sc, t, e->parent_idx, score, &mv);
                e->quota--;
                ab_full = true;
            }
            /* Valid A+B but no completable C: keep the A+B partial if requested. */
            if (g_incomplete_top && !ab_full) emit_incomplete(ctx, p, &mv, e->row);
        }
        return;
    }

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
    const bool supply = (g_supply_check && (uint32_t)e->row >= g_supply_check);
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
            if (supply && !supply_ok(t)) continue;
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
    uint64_t budget_parent = (uint64_t)quota_parent * g_scan_factor;
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

    uint64_t items = (uint64_t)beam_n * n_slices;
    #pragma omp parallel for schedule(dynamic, 8) num_threads(nt)
    for (uint64_t it = 0; it < items; it++) {
        if (g_stop) continue;
        uint32_t pi = (uint32_t)(it / n_slices);
        uint32_t sl = (uint32_t)(it % n_slices);
        Scratch *sc = scratch[omp_get_thread_num()];
        Expand e;
        if (!expand_prepare(&e, &beam[pi], pi, row, at_stop)) continue;
        /* Seeded per parent, NOT per slice: the slices of one parent have to
           agree on the phase-2 permutation in order to divide it between them. */
        e.rng = rng_for(cfg_hash, (uint32_t)row, pi, 0xFFFFFFFFu);
        e.quota = quota_slice; e.budget = budget_slice;

        /* Phase 1: this slice's stride of the cell in promise order (the cell is
           fan-out sorted, so the prefix holds the most continuable chains),
           reserving a quarter of the budget for the randomized phase. */
        const uint64_t rand_budget = budget_slice / 4;
        const uint32_t n = e.cA->n;
        for (uint64_t j = sl; j < n; j += n_slices) {
            if (e.quota == 0 || e.budget <= rand_budget) break;
            try_A(&e, ctx, (uint32_t)j, sc);
        }
        /* Phase 2: one random full-cycle permutation of the cell (random start +
           stride coprime to n visits every record exactly once), DIVIDED among
           the parent's slices -- slice sl takes cycle positions sl, sl+n_slices,
           ... so between them they still visit each record exactly once. Giving
           every slice the whole cycle, as this did before, had two slices calling
           try_A on the same record; try_A is deterministic in that record, so the
           loser spent budget rebuilding a child the pool already held. It also
           made a parent's work grow with the slice count, and so with --threads. */
        if (e.quota > 0 && e.budget > 0 && n > 1) {
            uint32_t start = rng_uniform(&e.rng, n);
            uint32_t step = 1;
            do step = 1 + rng_uniform(&e.rng, n - 1); while (gcd_u32(step, n) != 1);
            uint64_t j = ((uint64_t)start + (uint64_t)sl * step) % n;
            const uint64_t jump = ((uint64_t)n_slices * step) % n;
            for (uint32_t m = sl; m < n && e.quota > 0 && e.budget > 0; m += n_slices) {
                try_A(&e, ctx, (uint32_t)j, sc);
                j += jump; if (j >= n) j -= n;
            }
        }
        pool_flush(ctx, sc);
    }
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
   futures, so only the top-scored representative -- e.g. the one holding the
   center-139 bonus -- needs to survive). The hash table is region-sharded:
   every signature belongs to exactly one thread's slot range, so threads insert
   without locks; probing wraps within the owner's range. Fills ctx->keep[]
   with the survivors' pool indices, best score first; returns the count. */
static int g_dedup_drop_warned = 0;

static uint32_t dedup_and_rank(BeamCtx *ctx, uint64_t pool_n, int nt,
                               double tau, RNG *rng) {
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

    /* The SORT KEY only. At tau > 0 each survivor's key is score/tau plus an
       independent Gumbel draw, whose top-K is provably a sample of K DISTINCT
       boards drawn without replacement with probability proportional to
       exp(score/tau) (Kool, van Hoof & Welling 2019). That replaces the uniform
       random band -- which is blind to the score and switched off entirely from
       beam_expand_row on -- with one score-aware, monotone knob. The pool's own
       scores are untouched, so the beam, the emission order and every reported
       score stay real. */
    uint32_t kept = 0;
    if (tau > 0.0) {
        const double inv_tau = 1.0 / tau;
        for (size_t h = 0; h < sz; h++) {
            if (!ctx->sig_key[h]) continue;
            ctx->srt[kept].score = (float)((double)ctx->sig_score[h] * inv_tau
                                           + gumbel_noise(rng));
            ctx->srt[kept].idx   = ctx->sig_idx[h];
            kept++;
        }
    } else {
        for (size_t h = 0; h < sz; h++) {
            if (!ctx->sig_key[h]) continue;
            ctx->srt[kept].score = ctx->sig_score[h];
            ctx->srt[kept].idx   = ctx->sig_idx[h];
            kept++;
        }
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
            g_emit_lens[k] = format_board_tail(rows, row, PUZZLE_SIDE - 1,
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

        if (g_soft_center_139 && row == SOFT_CENTER_RELEASE_ROW)
            for (uint32_t i = 0; i < beam_n; i++) used_clear(cur[i].used, SOFT_CENTER_139_PIECE);

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

        /* No perturbation at the stop row: ctx->keep drives emit_stop_row, and
           emission is deliberately ranked by the real score. */
        RNG gum_rng = rng_for(cfg_hash, (uint32_t)row, 0xFFFFFFFEu, 2u);
        double tau = ((uint32_t)row == g_stop_row) ? 0.0 : gumbel_tau_eff(row);
        uint32_t kept = dedup_and_rank(ctx, pool_n, nt, tau, &gum_rng);
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
        /* At tau > 0 the Gumbel perturbation IS the exploration, so the uniform
           band stands down rather than randomizing an already-random order. */
        double fr = (tau > 0.0) ? 0.0 : frac_rand_eff(row);
        uint32_t n_sel = select_beam(ctx, kept, beam_n, eff_K,
                                     parent_cap_eff(row), fr, &sel_rng);
        double t0 = omp_get_wtime();
        materialize_beam(ctx, cur, nxt, n_sel, row);
        g_stats.t_mat += omp_get_wtime() - t0;
        BeamEntry *tmp = cur; cur = nxt; nxt = tmp;
        beam_n = n_sel; res.width = beam_n;

        if (g_verbose) {
            double dt = omp_get_wtime() - t_row;
            /* The real best score of the row, read from the pool -- ctx->srt
               holds the Gumbel-perturbed sort key when --gumbel_tau0 is on. */
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

static uint64_t fin_partial_hash(const int pos[NUM_PIECES], const int rot[NUM_PIECES]) {
    bool free_mode = g_opt_free_edges || !fin_pos_border_complete(pos);
    uint64_t h = 0xD0DEC5B0A0D5EEDULL;
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
            pdup_insert(fin_partial_hash(pos, rot));
            scanned++;
        }
    }
    free(line); fclose(f);
    printf("[init] dedup: prescanned %ld lines before the window (%zu distinct states)\n",
           scanned, g_pdup_n);
    fflush(stdout);
}

/* Load and validate CSV line `want` into g_grid / g_lock_mask /
   g_partial_depth. Returns 1 when loaded, 0 with a printed reason when the
   line cannot seed a search (the sweep skips it), -1 when the file has no
   such line (the sweep stops). The structural checks make sure a corrupted or
   mismatched file cannot silently poison a long run. */
static int fin_load_partial(const char *path, uint32_t want, char id_out[64]) {
    int pos[NUM_PIECES], rot[NUM_PIECES];
    if (!fin_read_partial_line(path, want, id_out, pos, rot)) return -1;

    /* Input dedup: a line seeding a search state already seen (earlier in the
       file or in this window) is skipped -- see fin_partial_hash. */
    if (!pdup_insert(fin_partial_hash(pos, rot))) {
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
    if (!have_db || memcmp(built_mask, g_lock_mask, sizeof built_mask) != 0) {
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
        memcpy(g_db_exclude, g_lock_mask, sizeof g_db_exclude);
        build_db_inner();
        build_fanout_inner();
        sort_db_by_fanout();
        memcpy(built_mask, g_lock_mask, sizeof built_mask);
        have_db = true;
        g_fin_la_inner_stale = true;
    } else {
        memcpy(g_db_exclude, g_lock_mask, sizeof g_db_exclude);
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
    printf("[sweep] %s row=%u width=%u emitted=%zu partials=%zu reason=%s sol_total=%" PRIu64
           " part_total=%zu wall=%.1fs\n",
           g_config_id_str, br.row, br.width, g_emit_count, g_partial_count, br.reason,
           g_solution_idx, g_partial_total, omp_get_wtime()-tc0);
    fflush(stdout);
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
    printf("[sum] emitted unique boards: %" PRIu64 "\n", g_stats.emitted_total);
    if (g_incomplete_top)
        printf("[sum] incomplete-top A+B partials: %zu\n", g_partial_total);
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
"  --border_row_N N       number of consecutive lines to load (default 1)\n"
"  --finalize_from N      lock rows 0..N and start the beam at row N+1 (default 8;\n"
"                         a line with an unplaced cell at or below N is skipped)\n"
"  --finalize_repeats N   sweep each loaded partial N times, with fresh randomness\n"
"                         and (free mode) fresh left columns each time (default 1)\n"
"  --free_edges           free every edge piece above finalize_from into a shared\n"
"                         pool; activated automatically when the partial leaves a\n"
"                         border piece unplaced (fixed sides need all 60 placed)\n"
"  --incomplete_top       also emit boards that reach --stop_row with a valid segment\n"
"                         A and B but no segment C, to a separate\n"
"                         <...>_<stop_row>_partial.csv (cols 11-15 left unplaced).\n"
"                         Both CSVs are APPENDED to, never truncated: a fresh run\n"
"                         adds to whatever the out_dir already holds\n"
"\n"
"Beam shape:\n"
"  --beam_width K         boards kept per row (default 262144)\n"
"  --stop_row R           last row the beam fills, finalize_from+1..14; reaching\n"
"                         boards are emitted (default 12; row 15 is never searched:\n"
"                         placing the top border is trivial for an external tool)\n"
"  --beam_expand E        late-search width multiplier (default 5; 1 = no expansion)\n"
"  --beam_expand_row R    absolute board row with the full ExK width; half of the\n"
"                         extra width is granted one row earlier (default 8)\n"
"\n"
"Scoring / selection:\n"
"  --score_model M        color-structure term: 'legacy' (the Mahalanobis bonus below)\n"
"                         or 'J', the objective derived from pairing combinatorics --\n"
"                         A_tot*KL(free-color mix || flat) plus a demand term, both in\n"
"                         nats like the fan-out terms, no schedule. J reads the color\n"
"                         counts off the board, so unlike the Mahalanobis term it is\n"
"                         exact from a locked partial at any --finalize_from\n"
"                         (default legacy)\n"
"  --lambda_J F           weight of the J terms; 1 = as derived (default 1)\n"
"  --lambda_Mahalanobis F weight of the color-usage atypicality bonus, active in\n"
"                         rows 3..9 and peaking at row 6; legacy model only (default 0)\n"
"  --avail_correct        discount the B/C fan-out counts by the fraction of each\n"
"                         frontier color still unplaced, so the lookahead stops being\n"
"                         blind to which pieces are gone (default off)\n"
"  --bonus_139 F          score bonus while piece 139 sits on a center cell, added\n"
"                         to a log-count score (default 1; was 10 before, which is a\n"
"                         claimed factor of e^10 and swamped every other term)\n"
"  --frac_rand F          fraction of the beam selected at random instead of by\n"
"                         score; the FIRST searched row always uses the full value\n"
"                         (variability injection), later rows follow the beamer\n"
"                         schedule: halved at beam_expand_row-1, zero after\n"
"                         (default 0.75)\n"
"  --gumbel_tau0 T        selection temperature at row 1, falling linearly to\n"
"  --gumbel_tau1 T        --gumbel_tau1 at --stop_row. Above 0 the beam is chosen by\n"
"                         top-K of score/tau + Gumbel noise, which samples K distinct\n"
"                         boards with probability proportional to exp(score/tau); as\n"
"                         the score is a log record count, tau=1 samples in proportion\n"
"                         to estimated completions. Replaces --frac_rand's uniform\n"
"                         band, which is blind to the score. 0 = off, exact legacy\n"
"                         (default 0)\n"
"  --parent_cap N         max children per parent in the score-selected band;\n"
"                         doubled from beam_expand_row-1 on; 0 = uncapped (default 5)\n"
"\n"
"Feasibility certificates:\n"
"  --no_free_demand       DISABLE the free-mode demand accounting. On by default: an\n"
"                         edge piece owes one inner half-edge of its own color whether\n"
"                         it ends up a right edge, a left edge or a top border piece,\n"
"                         so free mode's demands -- and hence the color parity test --\n"
"                         are exact without knowing which role each will take. Without\n"
"                         this accounting free mode carries no color certificate\n"
"  --supply_check R       from row R on, reject a board unless every frontier color is\n"
"                         wanted by no more columns than there are unused pieces\n"
"                         carrying it (Hall's condition on color classes; counts\n"
"                         pieces where parity counts half-edges). 0 = off (default 0)\n"
"\n"
"Expansion effort:\n"
"  --pool_factor N        candidate-pool target as a multiple of beam_width (default 8)\n"
"  --scan_factor N        decode-attempt budget per requested child (default 1024)\n"
"\n"
"Sweep control:\n"
"  --top_columns N        left-column completions sampled per partial per repeat\n"
"                         whenever the column is not fixed by the partial itself\n"
"                         (default 10; a complete border always uses exactly 1).\n"
"                         N<=0 enumerates EVERY legal left column exhaustively (no\n"
"                         sampling) -- for an exhaustive top-row search; pass a\n"
"                         rotations.csv to enumerate only the annealer's left edges\n"
"  --gumbel_tau_columns T selection temperature for the sampled left column: above 0\n"
"                         the published column is drawn with probability proportional\n"
"                         to exp(rank/tau) instead of being the best of the samples,\n"
"                         which also makes the repeat-dedup exhaust the pool less\n"
"                         often. That rank is symmetric in the row index, so trust it\n"
"                         little and set tau high. No effect when --top_columns<=0\n"
"                         enumerates exhaustively. 0 = off, exact legacy (default 0)\n"
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
"  --soft_center_139      keep clue piece 139 out of rows 1..5 (when still ahead of\n"
"                         the search) and add a score bonus when it sits on one of\n"
"                         the 4 center cells\n"
"  --verbose              per-row beam progress lines\n"
"  --help                 this text\n", a0);
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
        else if (!strcmp(argv[i], "--incomplete_top"))            g_incomplete_top = true;
        else if (!strcmp(argv[i], "--beam_width")  && i+1 < argc) g_beam_width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stop_row")    && i+1 < argc) g_stop_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand") && i+1 < argc) g_beam_expand = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand_row") && i+1 < argc) g_beam_expand_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_Mahalanobis") && i+1 < argc) g_lambda_maha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--score_model") && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "legacy")) g_score_model_J = false;
            else if (!strcmp(m, "J"))      g_score_model_J = true;
            else fatal("--score_model must be 'legacy' or 'J'");
        }
        else if (!strcmp(argv[i], "--lambda_J")    && i+1 < argc) g_lambda_J = atof(argv[++i]);
        else if (!strcmp(argv[i], "--avail_correct"))             g_avail_correct = true;
        else if (!strcmp(argv[i], "--no_free_demand"))            g_free_demand = false;
        else if (!strcmp(argv[i], "--supply_check") && i+1 < argc) g_supply_check = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bonus_139")   && i+1 < argc) g_bonus_139 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau0") && i+1 < argc) g_gumbel_tau0 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau1") && i+1 < argc) g_gumbel_tau1 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frac_rand")   && i+1 < argc) g_frac_rand = atof(argv[++i]);
        else if (!strcmp(argv[i], "--parent_cap")  && i+1 < argc) g_parent_cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool_factor") && i+1 < argc) g_pool_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scan_factor") && i+1 < argc) g_scan_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top_columns") && i+1 < argc) g_top_columns = atol(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau_columns") && i+1 < argc) g_tau_columns = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")        && i+1 < argc) { unsigned long long s; if (!parse_u64_token(argv[++i], &s)) fatal("--seed needs an integer"); g_master_seed = (uint64_t)s; }
        else if (!strcmp(argv[i], "--config_time_sec") && i+1 < argc) g_config_time_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_wall_sec")    && i+1 < argc) g_max_wall_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_partials")    && i+1 < argc) g_max_partials = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--soft_center_139")) g_soft_center_139 = true;
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
    if (g_finalize_repeats < 1) g_finalize_repeats = 1;
    if (!(fabs(g_lambda_maha) <= 1e6)) fatal("--lambda_Mahalanobis in [-1e6,1e6]");
    if (!(fabs(g_lambda_J) <= 1e6))    fatal("--lambda_J in [-1e6,1e6]");
    if (!(fabs(g_bonus_139) <= 1e6))   fatal("--bonus_139 in [-1e6,1e6]");
    if (!(g_gumbel_tau0 >= 0.0 && g_gumbel_tau0 <= 1e6)) fatal("--gumbel_tau0 in [0,1e6]");
    if (!(g_gumbel_tau1 >= 0.0 && g_gumbel_tau1 <= 1e6)) fatal("--gumbel_tau1 in [0,1e6]");
    if (!(g_tau_columns >= 0.0 && g_tau_columns <= 1e6)) fatal("--gumbel_tau_columns in [0,1e6]");
    if (g_supply_check > (uint32_t)EDGE_LEN)
        fatal("--supply_check must be in 0..%d (0 = off)", EDGE_LEN);
    if (g_frac_rand < 0.0 || g_frac_rand > 1.0) fatal("--frac_rand must be in [0,1]");
    if (g_pool_factor == 0) g_pool_factor = 1;
    if (g_scan_factor == 0) g_scan_factor = 1;

    printf("\n=== E555 finalizer ===\n\n");
    printf("[cfg] seed_file=%s partials_file=%s out_dir=%s\n",
           seed_path, csv_path, g_out_dir);
    printf("[cfg] seed=%" PRIu64 " threads=%d lines=%u+%u finalize_from=%u repeats=%u incomplete_top=%d\n",
           g_master_seed, g_nthreads, g_border_row_index, g_border_row_N,
           g_finalize_from, g_finalize_repeats, g_incomplete_top ? 1 : 0);
    printf("[cfg] beam_width=%u stop_row=%u expand=%ux@row%u lambda_Maha=%.3f\n",
           g_beam_width, g_stop_row, g_beam_expand, g_beam_expand_row, g_lambda_maha);
    printf("[cfg] frac_rand=%.2f (full at row %u) parent_cap=%u pool_factor=%u scan_factor=%u soft_center_139=%d bonus_139=%.3f\n",
           g_frac_rand, g_finalize_from + 1, g_parent_cap, g_pool_factor, g_scan_factor,
           g_soft_center_139 ? 1 : 0, g_bonus_139);
    printf("[cfg] score_model=%s lambda_J=%.3f avail_correct=%d free_demand=%d supply_check=%u"
           " gumbel_tau=%.2f->%.2f\n",
           g_score_model_J ? "J" : "legacy", g_lambda_J, g_avail_correct ? 1 : 0,
           g_free_demand ? 1 : 0, g_supply_check, g_gumbel_tau0, g_gumbel_tau1);
    printf("[cfg] gumbel_tau_columns=%.2f\n", g_tau_columns);
    printf("[cfg] top_columns=%ld config_time=%.0fs max_wall=%.0fs max_partials=%" PRIu64 " free_edges=%s\n",
           g_top_columns, g_config_time_sec, g_max_wall_sec, g_max_partials,
           g_opt_free_edges ? "forced" : "auto (per line)");
    if (rot_path && g_opt_free_edges)
        printf("[note] --free_edges given: the rotations file '%s' is ignored\n", rot_path);
    fflush(stdout);

    double t_start = omp_get_wtime();
    ensure_dir(g_out_dir);
    load_seed_and_catalog(seed_path);
    build_catalog_indices();
    build_inner_color_totals();
    build_maha_tables();
    build_logtab();
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
    char part_path[1024] = "";
    if (g_incomplete_top) {
        snprintf(part_path, sizeof part_path, "%s/beam_completions_finalized_%u_partial.csv", g_out_dir, g_stop_row);
        g_partial_fp = fopen(part_path, "a");
        if (!g_partial_fp) fatal("cannot open %s: %s", part_path, strerror(errno));
        setvbuf(g_partial_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] incomplete-top partials -> %s (append)\n", part_path);
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
                    run_left_config(&ctx, scratch, t_start, line, rep, li, &lft);
                }
            }
            free(tried);
        }
    }

    double wall = omp_get_wtime() - t_start;
    print_summary(wall, init_s, omp_get_wtime() - t_sweep0);
    printf("[sum] completions file: %s\n", comp_path);
    if (g_incomplete_top) printf("[sum] partials file: %s\n", part_path);
    if (g_completions_fp) fclose(g_completions_fp);
    if (g_partial_fp) fclose(g_partial_fp);
    return 0;
}
