/*
 * E555_beamer.c -- Stage B of the E555 pipeline: 5-5-5 beam search.
 *
 * Wide-beam, level-by-level search over the Eternity II board (16x16, 256
 * pieces). Rows are 0-indexed bottom-up: row 0 = bottom border, inner rows
 * 1..14, row 15 = top border.
 *
 * WHAT IT DOES
 *   Reads the seed and ONE Stage A boundary arrangement, builds (or mmaps from
 *   the --db_file cache) the single 5-piece database DB_5pieces, enumerates the
 *   legal bottom-row and left-column orderings, ranks both by fan-out, and
 *   sweeps the most productive (bottom x left-column) configurations. With
 *   --random_edges there is no Stage A input at all: bottoms and left columns
 *   are sampled at random from the seed's edge pieces (best of 32 fan-out-ranked
 *   samples each; see E555_database.c), so the search can run indefinitely,
 *   generating partials over ever-fresh border combinations. Each configuration
 *   is searched by ONE beam of --beam_width partial boards, advanced one row at
 *   a time:
 *     EXPAND      every beam board fills its next row left-to-right as three
 *                 5-piece segments drawn from the ONE database -- A (cols 1-5)
 *                 keyed by the fixed left edge's right color, B (cols 6-10) keyed
 *                 by A's exposed right color, C (cols 11-15) keyed by B's, whose
 *                 5th bottom is an edge-interface color so the right edge appears
 *                 automatically. Exact disjointness + color-parity checks.
 *     SCORE       each child is ranked by the options it keeps open one row up
 *                 (segment-A exact cell + B/C fan-out table) plus the Mahalanobis
 *                 color-usage term and the center-139 bonus.
 *     SELECT      pooled children are deduplicated by frontier signature (keeping
 *                 the best score per signature), ranked, and pruned to the row's
 *                 effective width by a score band with a per-parent offspring cap
 *                 plus a random band whose share follows the frac_rand schedule.
 *     MATERIALIZE survivors become the next beam; their moves go to the ancestry
 *                 log from which emitted boards are reconstructed.
 *   A beam whose child pool comes up EMPTY proves the configuration dead below
 *   the current row and is abandoned. EVERY board reaching --stop_row (default
 *   12) is appended to the completions CSV, best-scored first -- no lookahead
 *   is applied at the stop row; whether a row fits above it is deliberately
 *   left to the next stage.
 *
 * WIDTH AND RANDOMNESS SCHEDULES
 *   The beam expands late, where extinction pressure is highest: half of the
 *   extra (--beam_expand - 1) x width arrives at --beam_expand_row - 1, the rest
 *   at --beam_expand_row. In the same region the random selection band shrinks
 *   (full --frac_rand early, half at expand_row-1, zero after) and the
 *   per-parent offspring cap doubles: early rows explore, late rows exploit the
 *   fan-out heuristic to thread the few remaining legal completions.
 *
 * RANDOMNESS
 *   Runs are intentionally NOT reproducible unless --seed is given: by default
 *   the master seed comes from the clock and the process id (and is printed),
 *   so repeated runs sample uncorrelated regions of the search space.
 *
 * COMPILE / RUN
 *   gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *       E555_database.c E555_beamer.c -o E555_beamer -lm
 *   ./E555_beamer --help
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
   real fan-out difference can overcome. It then persists in every descendant
   from row 6 while frac_rand_eff returns 0 from row 8, so the late beam became a
   near-monoculture of 139 lineages. The default is now 1.0 -- about one standard
   deviation of the color term -- so it breaks near-ties in favour of a 139
   lineage without overriding a board that is genuinely more continuable. */
#define SOFT_CENTER_RELEASE_ROW 6

/* Beam-row segment-B retries: if a conflict-free B chain admits no conflict-free
   C completion, try the next conflict-free B, up to this many, before giving up
   on the A record. Deep rows are conflict-dominated; without the fallback a
   viable A chain can be lost to one unlucky B pick. */
#define B_TRY 4

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
static uint32_t g_bc_nB           = 1;      /* --bc_window nB,nC (1,1 = legacy) */
static uint32_t g_bc_nC           = 1;
static uint32_t g_parent_cap      = 5;
static uint32_t g_pool_factor     = 8;
static uint32_t g_scan_factor     = 1024;
static const char *g_out_dir      = "beam_out";
static const char *g_db_file      = NULL;
static bool     g_random_edges    = false;

static uint32_t g_border_row_index = 0;
static uint32_t g_border_row_N     = 1;
static double   g_config_time_sec  = 600.0;
static double   g_max_wall_sec     = 0.0;
static uint64_t g_max_partials     = 0;   /* reported-board budget; 0 = unlimited */
static long     g_top_bottoms      = 300;   /* leading bottoms per border row (<1 = all) */
static long     g_top_columns      = 10;    /* leading left columns per bottom (<1 = all) */
/* Selection temperature for the border ranks themselves, the same Gumbel knob
   the beam rows use. 0 = off, i.e. the greedy head of the ranking as before. */
static double   g_tau_bottoms      = 0.0;
static double   g_tau_columns      = 0.0;
/* Abandon a bottom after this many consecutive columns that emitted nothing
   (0 = never bail, run all --top_columns). */
static uint32_t g_bail_columns     = 0;
static bool     g_seed_given       = false;  /* --seed passed explicitly */
static uint64_t g_resume_sol_idx   = 0;
static uint32_t g_resume_bi = 0, g_resume_li = 0;
static bool     g_resume_active = false;

static const BottomOrder *g_cur_bottom = NULL;
static const LeftOrder   *g_cur_left   = NULL;
static char     g_config_id_str[64] = "c0";

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

/* Random selection band: full early (the fan-out heuristic knows little when the
   board is mostly empty), half at expand_row-1, zero from expand_row on (pure
   fan-out selection where only a few legal completions remain). */
static double frac_rand_eff(int row) {
    if ((uint32_t)row < g_beam_expand_row - 1) return g_frac_rand;
    if ((uint32_t)row == g_beam_expand_row - 1) return g_frac_rand * 0.5;
    return 0.0;
}

/* Selection temperature (--gumbel_tau0/--gumbel_tau1), interpolated linearly
   from tau0 at row 1 to tau1 at --stop_row. 0 = off, which is the default and
   restores the legacy path exactly (frac_rand's uniform band plus greedy
   top-K). Because the score is already a log record count, tau = 1 samples
   boards in proportion to their estimated number of completions; tau -> 0 is
   pure greedy and large tau is near-uniform. */
static double gumbel_tau_eff(int row) {
    if (g_gumbel_tau0 <= 0.0 && g_gumbel_tau1 <= 0.0) return 0.0;
    if (g_stop_row <= 1) return g_gumbel_tau0;
    double f = (double)(row - 1) / (double)(g_stop_row - 1);
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    double tau = g_gumbel_tau0 + f * (g_gumbel_tau1 - g_gumbel_tau0);
    return tau > 0.0 ? tau : 0.0;
}

/* gumbel_noise() and gumbel_key() are shared with the border ranking and the
   side samplers -- see E555_database.h. */

/* Per-parent offspring cap in the score band: doubled once the beam widens, so
   successful parents can actually fill the extra slots (0 = uncapped). */
static uint32_t parent_cap_eff(int row) {
    if (g_parent_cap == 0) return 0;
    return (g_beam_expand > 1 && (uint32_t)row >= g_beam_expand_row - 1)
           ? g_parent_cap * 2 : g_parent_cap;
}

/* -- Board state operations ------------------------------------------------- */

/* Initialize a beam board to the bare border of (bottom, left) config. */
static void beam_init_border(BeamEntry *p, const BottomOrder *bot, const LeftOrder *lft) {
    memset(p, 0, sizeof *p);
    p->log_idx = UINT32_MAX;
    for (int k = 0; k < 4; k++) p->used[k] = bot->used[k] | lft->used[k];
    used_set(p->used, g_cTR.piece_id);          /* top-right corner: Stage C */

    for (int c = 0; c < PUZZLE_SIDE; c++) p->rtop[c] = (uint8_t)bot->rtop0[c];
    for (int c = 1; c <= EDGE_LEN; c++)         /* inner frontier (cols 1..14) */
        if (color_is_inner(p->rtop[c])) p->req_exposed[p->rtop[c]]++;

    if (!g_free_edges) {
        for (int r = 1; r <= EDGE_LEN; r++) {   /* left-column interfaces */
            int cr = lft->right[r];
            if (color_is_inner(cr)) p->req_exposed[cr]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {  /* right-edge interfaces */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[cl]++;
        }
        for (int c = COLOR_MIN; c <= COLOR_MAX; c++)
            p->req_exposed[c] = (int16_t)(p->req_exposed[c] + g_top_border_inner_count[c]);
    } else if (g_free_demand) {
        /* Free mode owes exactly the same demands; only the bookkeeping differs.
           An edge piece carries ONE inner color, and in either remaining role --
           frame-right (inner side faces left) or frame-up (inner side faces down)
           -- it exposes exactly that one inner half-edge into the interior. So
           which unplaced edge becomes a right edge and which becomes a top border
           piece does not change the demand multiset at all, and free mode's
           demands are computable exactly without enumerating the split.

           The left column is placed but its inner sides face rows 1..14, so it
           still owes 14 interfaces -- placed is not the same as satisfied. The
           edge pool then covers every remaining right edge AND the whole top
           border in one used-tested loop (free mode puts all 56 non-corner edges
           in g_edge_term, and g_top_border_inner_count is all-zero here).

           Count at depth r: 14 frontier + (14-r) left + (28-r) unplaced edges
           = 56-2r, which is exactly what the fixed-mode branch above produces.
           Fixed mode is that same rule with the roles pre-assigned. */
        for (int r = 1; r <= EDGE_LEN; r++) {
            int cr = lft->right[r];
            if (color_is_inner(cr)) p->req_exposed[cr]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {
            if (used_test(p->used, g_edge_term[t].piece_id)) continue;  /* already placed */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[cl]++;
        }
    }
    if (g_soft_center_139) used_set(p->used, SOFT_CENTER_139_PIECE);
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

    for (int c = 1; c < PUZZLE_SIDE; c++) {        /* frontier: cols 1..15 (col 0 edge) */
        int old_t = p->rtop[c], new_t = new_top[c];
        if (color_is_inner(old_t)) p->req_exposed[old_t]--;
        if (color_is_inner(new_t)) p->req_exposed[new_t]++;
    }
    for (int c = 0; c < PUZZLE_SIDE; c++) p->rtop[c] = new_top[c];

    if (!g_free_edges || g_free_demand) {
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
   --no_free_demand is not in force (see beam_init_border: the demands are exact
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
   can still serve only one column. */
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
   backwards; fills rows[1..p->depth] (rows[0] is the border, not stored). */
static void collect_rows(const BeamCtx *ctx, const BeamEntry *p, RowChoice rows[EDGE_LEN]) {
    uint32_t li = p->log_idx;
    for (int r = p->depth; r >= 1; r--) {
        rows[r] = ctx->log[r][li].mv;
        li = ctx->log[r][li].parent_log;
    }
}

/* Resolve the placed piece at (r,c): bottom row from the config's bottom order,
   col 0 from the config's left column, col 15 from the committed right edge,
   otherwise the committed inner piece. */
static inline void board_cell(const RowChoice rows[EDGE_LEN], int r, int c,
                              uint16_t *pid, uint8_t *rot) {
    const Oriented *o;
    if (r == 0)                      o = g_cur_bottom->p[c];
    else if (c == 0)                 o = g_cur_left->p[r];
    else if (c == PUZZLE_SIDE - 1)   o = &g_edge_term[rows[r].rterm];
    else                             o = &g_cat[rows[r].ci[c - 1]];
    *pid = o->piece_id; *rot = o->rotation;
}

static uint64_t board_fingerprint(const RowChoice rows[EDGE_LEN], int depth) {
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
static int format_board_tail(const RowChoice rows[EDGE_LEN], int row,
                             uint16_t colmask, char *out) {
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
    RowChoice rows[EDGE_LEN];
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
    int len = format_board_tail(rows, row, colmask, line);

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
 * Unlike maha_term this reads S and R off the board, so it needs no assumption
 * that n = 14*row matches the real piece count -- it is exact from a partial. */
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
 * score identically -- a mild overcount at row 3, a large and board-dependent
 * one at row 11 where most counted records are already unbuildable.
 *
 * alpha_c = R_c/tot_c is the fraction of color-c inner half-edges still in the
 * reservoir, so a chain sitting on colors b1..b5 survives with probability
 * ~ prod_k alpha_{b_k} to first order. Summed in log space over the 14 inner
 * frontier colors that is one term per segment-A/B/C bottom (segment C's fifth
 * bottom is an edge-interface color, whose terminals come from a different
 * pool). No record is decoded and no cell is scanned: this is a closed-form
 * discount on numbers already looked up, and it is self-scheduling -- alpha ~ 1
 * early, biting only as the reservoir empties. */
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
/* ...with the signature supplied, for the --bc_window path, which must remember
   a candidate's signature while it keeps looking for a better sibling (the
   scratch board it was computed from is overwritten by the next candidate). */
static inline void pool_append_sig(BeamCtx *ctx, Scratch *sc, uint64_t sig,
                                   uint32_t parent_idx, float score, const RowChoice *mv) {
    PoolEntry *pe = &sc->buf[sc->buf_n++];
    pe->score = score; pe->parent = parent_idx; pe->sig = sig; pe->mv = *mv;
    if (sc->buf_n == POOL_BATCH) pool_flush(ctx, sc);
}
static inline void pool_append(BeamCtx *ctx, Scratch *sc, const BeamEntry *t,
                               uint32_t parent_idx, float score, const RowChoice *mv) {
    pool_append_sig(ctx, sc, frontier_sig(t), parent_idx, score, mv);
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
                   next stage's problem, so --supply_check does not apply here
                   either. Rank by the heuristic terms only. */
                float score = (float)(color_term(t, e->row)
                              + ((t->flags & FLAG_BONUS_139) ? g_bonus_139 : 0.0));
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

    /* Beam row: up to B_TRY conflict-free B chains, first conflict-free C.
       With --bc_window nB,nC (default 1,1) the row's B and C segments -- 10 of
       its 14 pieces -- stop being whatever the database's global, board-blind
       fan-out sort offered first: up to nB workable B chains x nC C completions
       are scored and only the best is kept, so the objective steers generation
       instead of merely filtering it. At 1,1 the first accepted child ends the
       search immediately, exactly as before; the B_TRY retry on a B chain whose
       C fails is orthogonal and unchanged. */
    const bool window = (g_bc_nB > 1 || g_bc_nC > 1);
    const bool supply = (g_supply_check && (uint32_t)e->row >= g_supply_check);
    RowChoice best_mv; float best_score = 0.0f; uint64_t best_sig = 0;
    bool have_best = false;
    uint32_t nb_done = 0;
    int b_left = B_TRY;
    for (uint32_t jb = 0; jb < cB->n && b_left > 0 && nb_done < g_bc_nB; jb++) {
        uint16_t ciB[CHAIN_LEN]; int la_C;
        if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) continue;
        b_left--;
        if (!color_is_inner(la_C)) continue;
        const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                             [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
        if (!cC) continue;
        uint64_t maskB[4]; mask_of_chain(ciB, CHAIN_LEN, maskB);
        uint64_t forbidB[4] = { forbidA[0]|maskB[0], forbidA[1]|maskB[1],
                                forbidA[2]|maskB[2], forbidA[3]|maskB[3] };
        uint32_t nc_done = 0;
        for (uint32_t jc = 0; jc < cC->n && nc_done < g_bc_nC; jc++) {
            uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
            if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) continue;
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
            mv.rterm = rterm;
            *t = *p; commit_row(t, e->row, &mv);
            if (!parity_ok(t)) continue;
            if (supply && !supply_ok(t)) continue;
            float score;
            if (!score_child(t, e->row, &score)) continue;
            if (!window) {                      /* legacy: first hit wins */
                pool_append(ctx, sc, t, e->parent_idx, score, &mv);
                e->quota--;
                return;
            }
            nc_done++;
            if (!have_best || score > best_score) {
                best_score = score; best_mv = mv; best_sig = frontier_sig(t);
                have_best = true;
            }
        }
        if (nc_done) nb_done++;                 /* a B chain that produced a child */
    }
    if (have_best) {
        pool_append_sig(ctx, sc, best_sig, e->parent_idx, best_score, &best_mv);
        e->quota--;
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
        /* Before expand_prepare, whose guard would drop this parent when its
           segment-A cell is empty -- exactly the case a B+C partial records.
           Once per parent, not once per slice. */
        if (at_stop && g_incomplete_top && sl == 0) try_BC(ctx, &beam[pi], row);
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
            RowChoice rows[EDGE_LEN];
            rows[row] = pe->mv;
            collect_rows(ctx, &beam[pe->parent], rows);
            g_emit_fps[k]  = board_fingerprint(rows, row);
            g_emit_lens[k] = format_board_tail(rows, row, ROWMASK_FULL,
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
    beam_init_border(&cur[0], g_cur_bottom, g_cur_left);
    uint32_t beam_n = 1;
    memset(ctx->log_n, 0, sizeof ctx->log_n);
    g_stats.configs++;

    for (int row = 1; (uint32_t)row <= g_stop_row; row++) {
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

/* -- Checkpoint ------------------------------------------------------------- */

static void write_checkpoint(const char *path, uint32_t border_row, uint32_t bi, uint32_t li) {
    FILE *ck = fopen(path, "w");
    if (!ck) return;
    fprintf(ck, "%u %u %u %" PRIu64 "\n", border_row, bi, li, g_solution_idx);
    fclose(ck);
}
static void read_checkpoint(const char *path) {
    FILE *ck = fopen(path, "r");
    if (!ck) return;
    unsigned br=0, bi=0, li=0; unsigned long long sol=0;
    int nf = fscanf(ck, "%u %u %u %llu", &br, &bi, &li, &sol);
    fclose(ck);
    if (nf < 3) { printf("[resume] checkpoint %s unreadable; starting fresh\n", path); return; }
    g_border_row_index = br;
    g_resume_bi = bi;
    g_resume_li = li;
    if (nf >= 4) g_resume_sol_idx = (uint64_t)sol;
    g_resume_active = true;
    printf("[resume] border_row=%u bi=%u li=%u sol_idx=%" PRIu64 "\n",
           g_border_row_index, g_resume_bi, g_resume_li, g_resume_sol_idx);
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
        printf("[sum] incomplete-top partials: %zu  (A+B %zu, A+C %zu, B+C %zu)\n",
               g_partial_total, g_part_ab, g_part_ac, g_part_bc);
    fflush(stdout);
}

/* -- main ------------------------------------------------------------------- */

static void usage(const char *a0) {
    fprintf(stderr,
"Usage: %s seed.txt rotation.csv [options]\n"
"\n"
"Stage B 5-5-5 beam search over Eternity II boards. Reads the piece seed and one\n"
"Stage A border arrangement, then sweeps ranked (bottom-row x left-column)\n"
"configurations, each searched by a beam advanced one row at a time. Boards that\n"
"reach --stop_row are written to <out_dir>/beam_completions_<border>_<stop_row>.csv.\n"
"\n"
"Input / output:\n"
"  --out_dir DIR          output directory: completions + checkpoint (default beam_out)\n"
"  --border_row N         first data row of rotation.csv to use (default 0)\n"
"  --border_row_N N       number of consecutive border rows to sweep (default 1)\n"
"  --db_file PATH         on-disk cache of the seed-only inner database: built and\n"
"                         written on first run, mmapped in seconds on later runs\n"
"  --free_edges           any edge piece may terminate a row on the right (relaxed\n"
"                         parity; default: only the border row's own right edges)\n"
"  --random_edges         sample random borders from the seed's edge pieces instead\n"
"                         of reading a Stage A arrangement (implies --free_edges;\n"
"                         rotation.csv may be omitted and is ignored if given).\n"
"                         --border_row_N = number of random bottoms, --top_columns =\n"
"                         random left columns per bottom; each border is the best of\n"
"                         32 fan-out-ranked samples. --border_row/--top_bottoms and\n"
"                         --resume do not apply\n"
"  --BL N / --BR N        pin a seed piece index (0..255) to the Bottom-Left /\n"
"  --TL N / --TR N        Bottom-Right / Top-Left / Top-Right corner (--random_edges\n"
"                         only); unpinned corners are sampled, and with 3 pinned the\n"
"                         4th is forced. Each must be a genuine, distinct corner\n"
"  --incomplete_top       also emit boards that reach --stop_row with only TWO of its\n"
"                         three 5-piece segments -- 11 of the row's 16 pieces -- to a\n"
"                         separate <...>_<stop_row>_partial.csv. All three shapes are\n"
"                         kept: A+B (cols 11-15 unplaced), A+C (cols 6-10) and B+C\n"
"                         (cols 1-5). The two segments that lost their left-hand\n"
"                         neighbour are searched over all 17 inner colors, so expect\n"
"                         roughly 10x the partials of A+B alone; --max_partials caps it\n"
"                         Both CSVs are APPENDED to, never truncated: a fresh run\n"
"                         adds to whatever the out_dir already holds\n"
"\n"
"Beam shape:\n"
"  --beam_width K         boards kept per row (default 262144)\n"
"  --stop_row R           last row the beam fills, 1..13; reaching boards are emitted\n"
"                         (default 12; rows 14-15 belong to Stage C)\n"
"  --beam_expand E        late-search width multiplier (default 5; 1 = no expansion)\n"
"  --beam_expand_row R    row with the full ExK width; half of the extra width is\n"
"                         granted one row earlier (default 8)\n"
"\n"
"Scoring / selection:\n"
"  --score_model M        color-structure term: 'legacy' (the Mahalanobis bonus below)\n"
"                         or 'J', the objective derived from pairing combinatorics --\n"
"                         A_tot*KL(free-color mix || flat) plus a demand term, both in\n"
"                         nats like the fan-out terms, no schedule (default legacy)\n"
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
"                         score; halved at beam_expand_row-1, zero from\n"
"                         beam_expand_row on (default 0.75)\n"
"  --gumbel_tau0 T        selection temperature at row 1, falling linearly to\n"
"  --gumbel_tau1 T        --gumbel_tau1 at --stop_row. Above 0 the beam is chosen by\n"
"                         top-K of score/tau + Gumbel noise, which samples K distinct\n"
"                         boards with probability proportional to exp(score/tau); as\n"
"                         the score is a log record count, tau=1 samples in proportion\n"
"                         to estimated completions. Replaces --frac_rand's uniform\n"
"                         band, which is blind to the score and off from\n"
"                         beam_expand_row on. 0 = off, exact legacy (default 0;\n"
"                         0.25 flat measured best, above 1 hurts)\n"
"  --parent_cap N         max children per parent in the score-selected band;\n"
"                         doubled from beam_expand_row-1 on; 0 = uncapped (default 5)\n"
"\n"
"Feasibility certificates:\n"
"  --no_free_demand       DISABLE the free-mode demand accounting. On by default: an\n"
"                         edge piece owes one inner half-edge of its own color whether\n"
"                         it ends up a right edge or a top border piece, so free mode's\n"
"                         demands -- and hence the color parity test -- are exact\n"
"                         without knowing the split. Without this accounting free mode\n"
"                         carries no color certificate at all\n"
"  --supply_check R       from row R on, reject a board unless every frontier color is\n"
"                         wanted by no more columns than there are unused pieces\n"
"                         carrying it (Hall's condition on color classes; counts\n"
"                         pieces where parity counts half-edges). 0 = off (default 0)\n"
"\n"
"Expansion effort:\n"
"  --pool_factor N        candidate-pool target as a multiple of beam_width; more\n"
"                         pool = more children scored per row (default 8)\n"
"  --scan_factor N        decode-attempt budget per requested child; caps the time\n"
"                         spent on parents with nearly-impossible rows (default 1024)\n"
"  --bc_window nB,nC      per segment-A record, score up to nB workable B chains x nC\n"
"                         C completions and keep only the best child, instead of\n"
"                         taking the first that fits. This is where extra compute buys\n"
"                         objective rather than more candidates -- but note the\n"
"                         --scan_factor budget counts segment-A decodes only, so an\n"
"                         open window multiplies the work it does not see\n"
"                         (default 1,1 = the first fit, as before)\n"
"\n"
"Sweep control:\n"
"  --top_bottoms N        bottom-row orderings tried per border row, best-ranked\n"
"                         first (<1 = all; default 300)\n"
"  --top_columns N        left-column orderings tried per bottom (<1 = all; default 10)\n"
"  --gumbel_tau_bottoms T selection temperature for the bottom ranking: above 0 the\n"
"                         --top_bottoms tried are a sample without replacement with\n"
"                         probability proportional to exp(rank/tau) rather than the\n"
"                         greedy head. 0 = off, exact legacy (default 0; ~2 buys\n"
"                         variety at no measured yield cost)\n"
"  --gumbel_tau_columns T the same for the left-column ranking. That rank is a sum\n"
"                         over rows and so is symmetric in the row index, giving huge\n"
"                         tie classes that memcmp settles, so the top L are\n"
"                         lexicographically adjacent -- an argument for a high tau\n"
"                         that a single-seed A/B did NOT confirm (see PROJECT_E555).\n"
"                         Measure before raising it. 0 = off (default 0; ~4 buys\n"
"                         variety at no measured yield cost)\n"
"  --bail_columns N       abandon a bottom after N consecutive columns that emitted\n"
"                         nothing, instead of running all --top_columns. Most useful\n"
"                         with --incomplete_top, which emits below the stop row; on\n"
"                         completions alone at a high --stop_row emissions are rare\n"
"                         enough that this acts as a cap of N columns per bottom.\n"
"                         0 = never bail (default 0)\n"
"  --config_time_sec S    wall-time slice per (bottom x left) config; a per-row\n"
"                         deadline, so it only fires on a config that runs long\n"
"                         (default 600)\n"
"  --max_wall_sec S       total wall-time budget; 0 = unlimited (default 0)\n"
"  --max_partials N       stop once N boards have been reported, counting both\n"
"                         completions and --incomplete_top partials; the stop-row\n"
"                         beam in flight is always reported in full, so the final\n"
"                         count can overshoot N by up to one beam width\n"
"                         (0 = unlimited; default 0)\n"
"  --resume               continue from <out_dir>/sweep_checkpoint.txt\n"
"\n"
"Misc:\n"
"  --threads N            OpenMP threads (default: all cores)\n"
"  --seed S               RNG seed; omitted or 0 = randomized from clock+pid and\n"
"                         printed, so repeated runs are uncorrelated\n"
"  --soft_center_139      keep clue piece 139 out of rows 1..5 and add a score bonus\n"
"                         whenever it lands on one of the 4 center cells\n"
"  --verbose              per-row beam progress lines\n"
"  --help                 this text\n", a0);
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--random_edges")) g_random_edges = true;
    }
    /* Positional arguments: seed.txt always; rotation.csv only without
       --random_edges (it may still be given, but is ignored in random mode). */
    if (argc < 2 || argv[1][0] == '-') { usage(argv[0]); return 1; }
    const char *seed_path = argv[1], *csv_path = NULL;
    int opt_start = 2;
    if (argc > 2 && argv[2][0] != '-') { csv_path = argv[2]; opt_start = 3; }
    if (!g_random_edges && !csv_path) {
        fprintf(stderr, "rotation.csv is required unless --random_edges is given\n\n");
        usage(argv[0]); return 1;
    }
    bool resume = false;

    for (int i = opt_start; i < argc; i++) {
        if      (!strcmp(argv[i], "--random_edges"))              { /* handled above */ }
        else if (!strcmp(argv[i], "--BL")          && i+1 < argc) g_fixed_corner_pid[0] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--BR")          && i+1 < argc) g_fixed_corner_pid[1] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--TL")          && i+1 < argc) g_fixed_corner_pid[2] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--TR")          && i+1 < argc) g_fixed_corner_pid[3] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--incomplete_top"))            g_incomplete_top = true;
        else if (!strcmp(argv[i], "--out_dir")     && i+1 < argc) g_out_dir = argv[++i];
        else if (!strcmp(argv[i], "--border_row")  && i+1 < argc) g_border_row_index = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--border_row_N")&& i+1 < argc) g_border_row_N = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--db_file")     && i+1 < argc) g_db_file = argv[++i];
        else if (!strcmp(argv[i], "--free_edges"))                g_free_edges = true;
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
        else if (!strcmp(argv[i], "--bc_window")   && i+1 < argc) {
            unsigned nb = 0, nc = 0;
            if (sscanf(argv[++i], "%u,%u", &nb, &nc) != 2 || nb < 1 || nc < 1)
                fatal("--bc_window needs nB,nC with both >= 1 (e.g. 8,4)");
            g_bc_nB = nb; g_bc_nC = nc;
        }
        else if (!strcmp(argv[i], "--frac_rand")   && i+1 < argc) g_frac_rand = atof(argv[++i]);
        else if (!strcmp(argv[i], "--parent_cap")  && i+1 < argc) g_parent_cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool_factor") && i+1 < argc) g_pool_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scan_factor") && i+1 < argc) g_scan_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top_bottoms") && i+1 < argc) g_top_bottoms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--top_columns") && i+1 < argc) g_top_columns = atol(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau_bottoms") && i+1 < argc) g_tau_bottoms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gumbel_tau_columns") && i+1 < argc) g_tau_columns = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bail_columns") && i+1 < argc) g_bail_columns = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")        && i+1 < argc) { unsigned long long s; if (!parse_u64_token(argv[++i], &s)) fatal("--seed needs an integer"); g_master_seed = (uint64_t)s; g_seed_given = true; }
        else if (!strcmp(argv[i], "--config_time_sec") && i+1 < argc) g_config_time_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_wall_sec")    && i+1 < argc) g_max_wall_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_partials")    && i+1 < argc) g_max_partials = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--resume"))          resume = true;
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
    if (g_stop_row == 0 || g_stop_row > (uint32_t)MAX_DRILL_DEPTH)
        fatal("--stop_row must be in 1..%d (rows 14-15 belong to Stage C)", MAX_DRILL_DEPTH);
    if (!(fabs(g_lambda_maha) <= 1e6)) fatal("--lambda_Mahalanobis in [-1e6,1e6]");
    if (!(fabs(g_lambda_J) <= 1e6))    fatal("--lambda_J in [-1e6,1e6]");
    if (!(fabs(g_bonus_139) <= 1e6))   fatal("--bonus_139 in [-1e6,1e6]");
    if (!(g_gumbel_tau0 >= 0.0 && g_gumbel_tau0 <= 1e6)) fatal("--gumbel_tau0 in [0,1e6]");
    if (!(g_gumbel_tau1 >= 0.0 && g_gumbel_tau1 <= 1e6)) fatal("--gumbel_tau1 in [0,1e6]");
    if (!(g_tau_bottoms >= 0.0 && g_tau_bottoms <= 1e6)) fatal("--gumbel_tau_bottoms in [0,1e6]");
    if (!(g_tau_columns >= 0.0 && g_tau_columns <= 1e6)) fatal("--gumbel_tau_columns in [0,1e6]");
    if (g_supply_check > (uint32_t)MAX_DRILL_DEPTH)
        fatal("--supply_check must be in 0..%d (0 = off)", MAX_DRILL_DEPTH);
    if (g_frac_rand < 0.0 || g_frac_rand > 1.0) fatal("--frac_rand must be in [0,1]");
    if (g_pool_factor == 0) g_pool_factor = 1;
    if (g_scan_factor == 0) g_scan_factor = 1;
    bool any_fixed_corner = false;
    for (int r = 0; r < 4; r++) {
        if (g_fixed_corner_pid[r] < 0) continue;
        any_fixed_corner = true;
        if (g_fixed_corner_pid[r] >= (int)NUM_PIECES)
            fatal("--BL/--BR/--TL/--TR piece index must be in 0..%d", NUM_PIECES - 1);
    }
    if (any_fixed_corner && !g_random_edges)
        fatal("--BL/--BR/--TL/--TR are only valid with --random_edges");
    if (g_random_edges) {
        g_free_edges = true;               /* the border is not a fixed assignment */
        if (resume) fatal("--resume is not supported with --random_edges (borders are sampled fresh)");
    }
    /* The checkpoint stores (border_row, bottom index, column index) into the
       RANKED arrays. At tau > 0 that ranking is a Gumbel permutation drawn from
       the master seed, which defaults to a clock/PID mixture -- so resuming
       without pinning the seed would silently land on a different permutation,
       re-running some configs and skipping others. */
    if (resume && (g_tau_bottoms > 0.0 || g_tau_columns > 0.0) && !g_seed_given)
        fatal("--resume with --gumbel_tau_bottoms/--gumbel_tau_columns needs the "
              "original --seed (the checkpoint indexes a seed-dependent ordering)");

    printf("\n=== E555 beamer ===\n\n");
    printf("[cfg] seed_file=%s rotations_file=%s out_dir=%s\n",
           seed_path, csv_path ? csv_path : "(none: --random_edges)", g_out_dir);
    printf("[cfg] seed=%" PRIu64 " threads=%d random_edges=%d free_edges=%d border_row=%u border_row_N=%u\n",
           g_master_seed, g_nthreads, g_random_edges?1:0, g_free_edges?1:0,
           g_border_row_index, g_border_row_N);
    printf("[cfg] incomplete_top=%d resume=%d corners BL/BR/TL/TR=%d/%d/%d/%d\n",
           g_incomplete_top?1:0, resume?1:0, g_fixed_corner_pid[0], g_fixed_corner_pid[1],
           g_fixed_corner_pid[2], g_fixed_corner_pid[3]);
    printf("[cfg] beam_width=%u stop_row=%u expand=%ux@row%u lambda_Maha=%.3f\n",
           g_beam_width, g_stop_row, g_beam_expand, g_beam_expand_row, g_lambda_maha);
    printf("[cfg] frac_rand=%.2f parent_cap=%u pool_factor=%u scan_factor=%u soft_center_139=%d bonus_139=%.3f\n",
           g_frac_rand, g_parent_cap, g_pool_factor, g_scan_factor, g_soft_center_139?1:0,
           g_bonus_139);
    printf("[cfg] score_model=%s lambda_J=%.3f avail_correct=%d free_demand=%d supply_check=%u"
           " gumbel_tau=%.2f->%.2f bc_window=%u,%u\n",
           g_score_model_J ? "J" : "legacy", g_lambda_J, g_avail_correct?1:0,
           g_free_demand?1:0, g_supply_check, g_gumbel_tau0, g_gumbel_tau1,
           g_bc_nB, g_bc_nC);
    printf("[cfg] top_bottoms=%ld top_columns=%ld config_time=%.0fs max_wall=%.0fs max_partials=%" PRIu64 " db_file=%s\n",
           g_top_bottoms, g_top_columns, g_config_time_sec, g_max_wall_sec, g_max_partials,
           g_db_file ? g_db_file : "(none)");
    printf("[cfg] gumbel_tau_bottoms=%.2f gumbel_tau_columns=%.2f bail_columns=%u\n",
           g_tau_bottoms, g_tau_columns, g_bail_columns);
    fflush(stdout);

    double t_start = omp_get_wtime();
    ensure_dir(g_out_dir);
    load_seed_and_catalog(seed_path);
    build_catalog_indices();
    build_inner_color_totals();
    build_maha_tables();
    build_logtab();

    /* On a cache hit db_cache_load also fills the fan-out table and ranking
       totals from its index table, so no arena page is touched at startup. */
    bool cache_hit = g_db_file && db_cache_load(g_db_file);
    if (!cache_hit) {
        printf("[init] Building DB inner (2 passes)...\n"); fflush(stdout);
        build_db_inner();
        build_fanout_inner();
        sort_db_by_fanout();
        if (g_db_file) db_cache_save(g_db_file);
    }
    if (g_free_edges) {                       /* edge cells are border-independent */
        build_edge_terminal_pool();
        build_db_edge_and_sort();
    }

    char ckpath[1024];
    snprintf(ckpath, sizeof ckpath, "%s/sweep_checkpoint.txt", g_out_dir);
    if (resume) read_checkpoint(ckpath);

    BeamCtx ctx; beam_ctx_alloc(&ctx);
    Scratch **scratch = xmalloc((size_t)g_nthreads * sizeof(Scratch *));
    for (int t = 0; t < g_nthreads; t++) { scratch[t] = xmalloc(sizeof(Scratch)); memset(scratch[t], 0, sizeof(Scratch)); }

    signal(SIGINT, handle_stop); signal(SIGTERM, handle_stop);
    double t_sweep0 = omp_get_wtime();
    double init_s = t_sweep0 - t_start;

    if (g_random_edges) {
        /* Random-border sweep: no rotation CSV. --border_row_N random bottoms,
           --top_columns random left columns per bottom, each border the best of
           RANDOM_SIDE_SAMPLES fan-out-ranked samples (--top_bottoms is moot:
           every sampled bottom is used). No checkpoint: borders are not
           re-derivable, so a run is continued simply by starting another. */
        printf("\n========== random borders ==========\n");
        size_t run_b = g_border_row_N >= 1 ? g_border_row_N : 1;
        size_t run_l = g_top_columns  >= 1 ? (size_t)g_top_columns : 1;
        printf("[sweep] random mode: %zu bottoms x %zu lefts (best of %d samples each)\n",
               run_b, run_l, RANDOM_SIDE_SAMPLES); fflush(stdout);

        finalize_fixed_corners();          /* validate/resolve any pinned corners */
        htable_init();
        if (g_incomplete_top) partial_htable_init();
        char comp_path[1024];
        snprintf(comp_path, sizeof comp_path, "%s/beam_completions_random_%u.csv", g_out_dir, g_stop_row);
        g_completions_fp = fopen(comp_path, "a");
        if (!g_completions_fp) fatal("cannot open %s: %s", comp_path, strerror(errno));
        setvbuf(g_completions_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] completions -> %s (append)\n", comp_path);
        if (g_incomplete_top) {
            char part_path[1024];
            snprintf(part_path, sizeof part_path, "%s/beam_completions_random_%u_partial.csv", g_out_dir, g_stop_row);
            g_partial_fp = fopen(part_path, "a");
            if (!g_partial_fp) fatal("cannot open %s: %s", part_path, strerror(errno));
            setvbuf(g_partial_fp, NULL, _IOFBF, EMIT_FILE_BUF);
            printf("[out] incomplete-top partials -> %s (append)\n", part_path);
        }
        fflush(stdout);
        g_solution_idx = 0;

        RNG srng = rng_for(g_master_seed, 0xB07D0135u, 0, 0);
        BottomOrder bot; LeftOrder lft;
        for (size_t bi = 0; bi < run_b && !g_stop; bi++) {
            if (!sample_random_bottom(&srng, g_tau_bottoms, &bot))
                fatal("random bottom sampling failed; seed edge pool too constrained");
            validate_color_constants();
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = 0; li < run_l && !g_stop; li++) {
                if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) { printf("[sweep] max_wall reached.\n"); g_stop = 1; break; }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                if (!sample_random_left(&srng, g_tau_columns, &bot, &lft)) {
                    fprintf(stderr, "[warn] no left column fits bottom %zu; resampling bottom\n", bi);
                    break;
                }
                g_cur_bottom = &bot;
                g_cur_left   = &lft;
                snprintf(g_config_id_str, sizeof g_config_id_str, "rndb%zul%zu", bi, li);
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                if (g_incomplete_top) { g_partial_count = 0; memset(g_partial_htable, 0, g_partial_htable_sz*sizeof(uint64_t)); }
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                /* emitted/partials are this config's unique boards; sol_total and
                   part_total are the run totals written so far. */
                printf("[sweep] %s row=%u width=%u emitted=%zu partials=%zu reason=%s sol_total=%" PRIu64
                       " part_total=%zu wall=%.1fs\n",
                       g_config_id_str, br.row, br.width, g_emit_count, g_partial_count, br.reason,
                       g_solution_idx, g_partial_total, omp_get_wtime()-tc0);
                fflush(stdout);
                if (g_completions_fp) fflush(g_completions_fp);
                if (g_partial_fp)     fflush(g_partial_fp);
                partials_budget_announce();
                barren = (g_emit_count + g_partial_count > 0) ? 0 : barren + 1;
                if (g_bail_columns && barren >= g_bail_columns) {
                    printf("[bail] rndb%zu: %u column(s) in a row emitted nothing, "
                           "moving to the next bottom\n", bi, barren);
                    fflush(stdout);
                    break;
                }
            }
        }
    } else
    for (uint32_t cur_row = g_border_row_index; cur_row < g_border_row_index + g_border_row_N; cur_row++) {
        if (g_stop) break;
        printf("\n========== border row %u ==========\n", cur_row); fflush(stdout);

        uint8_t spins[NUM_PIECES];
        if (!read_one_border_row(csv_path, cur_row, spins)) { printf("[sweep] border row %u not found; stopping.\n", cur_row); break; }
        memcpy(g_spin, spins, sizeof g_spin);

        classify_deal_from_rotations();
        build_top_border_demands();
        if (!g_free_edges) { build_edge_terminal_pool(); build_db_edge_and_sort(); }
        validate_color_constants();

        enumerate_bottoms();
        enumerate_lefts();
        /* Separate streams per border row, so the bottoms drawn for one row do
           not depend on how many columns the previous row happened to enumerate. */
        RNG brng = rng_for(g_master_seed, cur_row, 0xB0770D15u, 0);
        RNG lrng = rng_for(g_master_seed, cur_row, 0x1EF7C015u, 0);
        rank_bottoms(g_tau_bottoms, &brng);
        rank_lefts(g_tau_columns, &lrng);

        size_t nb = g_bottom_n, nl = g_left_n;
        size_t run_b = (g_top_bottoms >= 1 && (size_t)g_top_bottoms < nb) ? (size_t)g_top_bottoms : nb;
        size_t run_l = (g_top_columns >= 1 && (size_t)g_top_columns < nl) ? (size_t)g_top_columns : nl;
        printf("[sweep] bottoms=%zu (run %zu)  left-cols=%zu (run %zu)  -> %zu configs\n",
               nb, run_b, nl, run_l, run_b*run_l); fflush(stdout);
        if (nb == 0 || nl == 0) fatal("no border configs for row %u", cur_row);

        htable_init();
        if (g_incomplete_top) partial_htable_init();
        if (g_completions_fp) fclose(g_completions_fp);
        if (g_partial_fp) fclose(g_partial_fp);
        char comp_path[1024];
        snprintf(comp_path, sizeof comp_path, "%s/beam_completions_%u_%u.csv", g_out_dir, cur_row, g_stop_row);
        g_completions_fp = fopen(comp_path, "a");
        if (!g_completions_fp) fatal("cannot open %s: %s", comp_path, strerror(errno));
        setvbuf(g_completions_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] completions -> %s (append)\n", comp_path);
        if (g_incomplete_top) {
            char part_path[1024];
            snprintf(part_path, sizeof part_path, "%s/beam_completions_%u_%u_partial.csv", g_out_dir, cur_row, g_stop_row);
            g_partial_fp = fopen(part_path, "a");
            if (!g_partial_fp) fatal("cannot open %s: %s", part_path, strerror(errno));
            setvbuf(g_partial_fp, NULL, _IOFBF, EMIT_FILE_BUF);
            printf("[out] incomplete-top partials -> %s (append)\n", part_path);
        }
        fflush(stdout);

        size_t start_bi = 0, start_li = 0;
        if (g_resume_active && cur_row == g_border_row_index) { start_bi = g_resume_bi; start_li = g_resume_li; g_solution_idx = g_resume_sol_idx; }
        else g_solution_idx = 0;

        for (size_t bi = start_bi; bi < run_b && !g_stop; bi++) {
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = (bi == start_bi ? start_li : 0); li < run_l && !g_stop; li++) {
                if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) { printf("[sweep] max_wall reached.\n"); g_stop = 1; break; }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                g_cur_bottom = &g_bottoms[bi];
                g_cur_left   = &g_lefts[li];
                snprintf(g_config_id_str, sizeof g_config_id_str, "r%ub%zul%zu", cur_row, bi, li);
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                if (g_incomplete_top) { g_partial_count = 0; memset(g_partial_htable, 0, g_partial_htable_sz*sizeof(uint64_t)); }
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                /* emitted/partials are this config's unique boards; sol_total and
                   part_total are the run totals written so far. */
                printf("[sweep] %s row=%u width=%u emitted=%zu partials=%zu reason=%s sol_total=%" PRIu64
                       " part_total=%zu wall=%.1fs\n",
                       g_config_id_str, br.row, br.width, g_emit_count, g_partial_count, br.reason,
                       g_solution_idx, g_partial_total, omp_get_wtime()-tc0);
                fflush(stdout);
                if (g_completions_fp) fflush(g_completions_fp);
                if (g_partial_fp)     fflush(g_partial_fp);
                partials_budget_announce();
                write_checkpoint(ckpath, cur_row, (uint32_t)bi, (uint32_t)(li+1));
                barren = (g_emit_count + g_partial_count > 0) ? 0 : barren + 1;
                if (g_bail_columns && barren >= g_bail_columns) {
                    printf("[bail] r%ub%zu: %u column(s) in a row emitted nothing, "
                           "moving to the next bottom\n", cur_row, bi, barren);
                    fflush(stdout);
                    /* Point the checkpoint at the next bottom, not at the column
                       we stopped on -- otherwise --resume walks back into the
                       bottom we just abandoned and undoes the saving. */
                    write_checkpoint(ckpath, cur_row, (uint32_t)(bi+1), 0);
                    break;
                }
            }
        }
        g_resume_active = false;
    }

    double wall = omp_get_wtime() - t_start;
    print_summary(wall, init_s, omp_get_wtime() - t_sweep0);
    if (g_completions_fp) fclose(g_completions_fp);
    if (g_partial_fp) fclose(g_partial_fp);
    return 0;
}
