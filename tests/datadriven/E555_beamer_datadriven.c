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
 *                 color-usage term.
 *     SELECT      below the stop row, pooled children are deduplicated by frontier
 *                 signature (keeping the best score per signature), ranked, and
 *                 pruned to the effective width. At the stop row the raw candidates
 *                 are score-sorted, then exact duplicate boards are removed while
 *                 distinct complete boards -- even one-piece variants -- survive.
 *     MATERIALIZE survivors become the next beam; their moves go to the ancestry
 *                 log from which emitted boards are reconstructed.
 *   A beam whose child pool comes up EMPTY is abandoned. That is a HEURISTIC
 *   extinction, not a proof: the generator keeps a bounded number of children
 *   per segment-A record, spends a bounded quota per parent, and starts from an
 *   already-pruned beam, so an empty pool means only that this bounded search
 *   found no child from the states it still held. Boards reaching --stop_row
 *   (default 12) are appended to the completions CSV, best-scored first and with
 *   exact-board deduplication only, up to EMIT_MAX per config -- no lookahead is
 *   applied there; whether a row fits above is deliberately left to Stage C.
 *
 * WIDTH AND RANDOMNESS SCHEDULES
 *   The beam expands late, where extinction pressure is highest: half of the
 *   extra (--beam_expand - 1) x width arrives at --beam_expand_row - 1, the rest
 *   at --beam_expand_row, and the per-parent offspring cap doubles there: late
 *   rows get more room to thread the few remaining legal completions. The
 *   random selection band does NOT taper -- --frac_rand is flat across rows.
 *
 * RANDOMNESS
 *   Runs are intentionally NOT reproducible unless --rng_seed is given: by default
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

/* Beam-row segment-B retries: if a conflict-free B chain admits no conflict-free
   C completion, try the next conflict-free B, up to this many, before giving up
   on the A record. Deep rows are conflict-dominated; without the fallback a
   viable A chain can be lost to one unlucky B pick. */
#define B_TRY 4

/* Largest --bc_window side. Nothing is sized by it -- the window keeps one
   running best, not an array -- but b_left is B_TRY + nB - 1 in an int, so an
   unbounded nB would overflow it negative and the B loop would never run, which
   would empty the beam silently instead of merely slowing it. */
#define BC_WINDOW_MAX 64

/* Stop-row emission safety cap (lines per config; the CSV rows are ~2 KB). */
#define EMIT_MAX 1000000u

static uint64_t g_master_seed = 0;

static uint32_t g_beam_width      = 250000;
static uint32_t g_stop_row        = 11;
static uint32_t g_beam_expand     = 4;
static uint32_t g_beam_expand_row = 7;
static double   g_lambda_maha     = 1.0;   /* --lambda_Mahalanobis, in score-SD */
static double   g_frac_rand       = 0.10;  /* flat across rows; see below */
/* Share of the beam reserved as a per-orientation floor, split evenly over the
   four orientations (so K/8 each). Not a CLI knob: it exists to stop one
   orientation crowding the others out, and unused floor is handed straight
   back to the score ranking, so there is nothing to tune. */
#define CLUE_FLOOR_FRAC 0.5
static uint16_t g_clue_ci[4][CLUE_N];      /* catalog index of each oriented clue */
static double   g_lambda_J        = 1.0;    /* --lambda_J, the closure weight */
static bool     g_free_demand     = true;   /* --no_free_demand turns it off */
static uint32_t g_bc_nB           = 3;      /* --bc_window nB,nC; 1,1 = legacy */
static uint32_t g_bc_nC           = 3;
static uint32_t g_parent_cap      = 4;
static uint32_t g_pool_factor     = 8;
/* Segment-A decode budget per requested child. Was --scan_factor; a sweep found
   8192 BYTE-identical to 1024 over 219 beam rows, because the per-parent quota
   is what actually binds (and at a starved row neither does), so it is a fixed
   safety valve against a pathological cell rather than a knob. */
#define SCAN_FACTOR 1024u
static const char *g_out_dir      = "beam_out";
static const char *g_db_file      = NULL;
static bool     g_random_edges    = false;

static uint32_t g_start_row        = 0;
static uint32_t g_num_rows         = 0;   /* rows to sweep; 0 = every remaining row of rotation.csv */
static uint32_t g_samples          = 1;   /* --samples: random borders to try under --random_edges;
                                              0 = uncapped, run until --wall_time/--max_emitted stops you */
static double   g_config_time_sec  = 600.0;
static double   g_max_wall_sec     = 0.0;
static uint64_t g_max_partials     = 0;   /* reported-board budget; 0 = unlimited */
static long     g_top_bottoms      = 10;    /* leading bottoms per border row (<1 = all) */
static long     g_top_columns      = 12;    /* leading left columns per bottom (<1 = all) */
/* Selection temperature for the border ranks themselves, the same Gumbel knob
   the beam rows use. 0 = off, i.e. the greedy head of the ranking as before. */
static double   g_tau_bottoms      = 0.0;
static double   g_tau_columns      = 0.0;
/* Abandon a bottom after this many consecutive columns that emitted nothing
   (0 = never bail, run all --top_columns). */
static uint32_t g_bail_columns     = 0;
static bool     g_seed_given       = false;  /* --rng_seed passed explicitly */
static uint64_t g_resume_sol_idx   = 0;
static uint32_t g_resume_bi = 0, g_resume_li = 0;
static bool     g_resume_active = false;

static const BottomOrder *g_cur_bottom = NULL;
static const LeftOrder   *g_cur_left   = NULL;
/* Wide enough that snprintf cannot truncate the longest id the sweep can build
   ("r<row>b<bottom>#<lap>l<column>"), which -Wformat-truncation checks. */
static char     g_config_id_str[96] = "c0";

/* Exact-board dedup table (open addressing, power of two). Learning uses it
   before counting; search uses it only while writing the complete stop row.
   Frontier dedup remains disabled there, so distinct histories/boards are kept. */
static uint64_t  *g_emit_htable = NULL;
static size_t     g_emit_htable_sz = 0, g_emit_count = 0;
static FILE      *g_completions_fp = NULL;
static uint64_t   g_solution_idx = 0;

/* --incomplete_top: stop-row boards that fill only part of the stop row. Two
   segments (A+B, A+C, B+C) go to <...>_partial.csv, segment B alone to
   <...>_partial_B.csv. Column 0 is always present. */
static bool g_incomplete_top = false;

#define PARTMASK_B ((uint16_t)0x07C1u)  /* col 0 + cols 6..10 */
_Static_assert((ROWMASK_AB & PARTMASK_B) == PARTMASK_B
            && (ROWMASK_BC & PARTMASK_B) == PARTMASK_B, "B mask");

typedef enum { PART_AB = 0, PART_AC, PART_BC, PART_B, PART_N } PartialKind;
static const char *const g_part_name[PART_N] = { "AB", "AC", "BC", "B" };
static const uint16_t g_part_mask[PART_N] = {
    ROWMASK_AB, ROWMASK_AC, ROWMASK_BC, PARTMASK_B
};
enum { PFILE_TWO = 0, PFILE_B, PFILE_N };
static FILE *g_partial_fp[PFILE_N];
static inline int partial_file_of(PartialKind k) { return k == PART_B ? PFILE_B : PFILE_TWO; }

/* Partial dedup. Two partials of one kind are duplicates when the pieces below
   the stop row are the same SET (the parent's used mask) and their stop-row
   pieces differ in at most one cell (rotations ignored). Each partial with n
   occupied stop-row cells stores n keys, key i hashing everything but cell i;
   two sequences share a key iff their Hamming distance is <= 1. The key set
   lives for one border row. */
static uint64_t *g_pdedup = NULL;
static size_t    g_pdedup_sz = 0, g_pdedup_n = 0;

static size_t g_partial_count = 0;      /* this configuration, all kinds */
static size_t g_part_count[PART_N];     /* this configuration by kind */
static size_t g_partial_total = 0;      /* whole run, all kinds */
static size_t g_part_total[PART_N];     /* whole run by kind */

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* Run statistics for the end-of-run summary. */
static struct {
    uint64_t configs;
    uint64_t rows_advanced;
    uint64_t cands_total;
    uint64_t extinct_at[EDGE_LEN + 1];
    uint64_t row_attempts[EDGE_LEN + 1];
    uint64_t row_candidates[EDGE_LEN + 1];
    uint64_t row_retained[EDGE_LEN + 1];
    uint64_t row_selected[EDGE_LEN + 1];
    uint64_t reached_stop;
    uint64_t emitted_total;
    double   t_expand, t_select, t_mat, t_emit;
} g_stats;

static struct {
    uint64_t bottoms_ranked;
    uint64_t bottoms_no_clue_column;
    uint64_t columns_ordinary_viable;
    uint64_t columns_clue_compatible;
    uint64_t columns_run;
    double   clue_seconds;
} g_border_stats;

/* Exact row-1 corner-clue compatibility, cached per ranked bottom and segment-A
   left color for the current border row.  -1 unknown, 0 impossible, 1 possible. */
static int8_t *g_corner_compat_cache = NULL;
static size_t  g_corner_compat_nb = 0;
static LeftOrder *g_left_filter_buf = NULL;
static size_t     g_left_filter_cap = 0;


/* --max_emitted budget: stop-row completions plus every --incomplete_top
   partial, i.e. every board written to disk. Checked after
   each stop-row emission and at each config boundary, so the beam in flight is
   always reported in full and the final count may exceed N by up to one
   configuration's reported output. Called from serial code only
   (g_partial_total is mutated inside the emission critical section).
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
    printf("[sweep] max_emitted reached (%" PRIu64 " boards reported).\n",
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

/* The random selection band is FLAT: --frac_rand applies at every row. It used
   to taper to zero from --beam_expand_row on, which made sense when the band
   was 0.75 and the late rows needed protecting from it. At 0.10 the taper buys
   nothing and costs the late rows their only hedge against a biased objective.
   It is also very nearly a no-op either way: the band is split off inside
   select_beam, which only runs when the pool EXCEEDS the row width, and at the
   expanded rows it rarely does -- measured mean occupancy at row 8 was 700k of
   1.31M slots. Where selection does not bind, every candidate survives and the
   fraction is moot. */

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
        if (color_is_inner(p->rtop[c])) p->req_exposed[INNER_IDX(p->rtop[c])]++;

    if (!g_free_edges) {
        for (int r = 1; r <= EDGE_LEN; r++) {   /* left-column interfaces */
            int cr = lft->right[r];
            if (color_is_inner(cr)) p->req_exposed[INNER_IDX(cr)]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {  /* right-edge interfaces */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[INNER_IDX(cl)]++;
        }
        for (int c = COLOR_MIN; c <= COLOR_MAX; c++)
            p->req_exposed[INNER_IDX(c)] = (int16_t)(p->req_exposed[INNER_IDX(c)] + g_top_border_inner_count[c]);
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
            if (color_is_inner(cr)) p->req_exposed[INNER_IDX(cr)]++;
        }
        for (int t = 0; t < g_edge_term_count; t++) {
            if (used_test(p->used, g_edge_term[t].piece_id)) continue;  /* already placed */
            int cl = g_edge_term[t].left;
            if (color_is_inner(cl)) p->req_exposed[INNER_IDX(cl)]++;
        }
    }
    /* Reserve every enabled clue piece. Two of the corner clues sit on row 13
       and are never placed, so holding them makes the free-demand accounting
       exact rather than optimistic. The pinned walk is exempt from this mask,
       so a clue can still be placed on its own cell. */
    for (int k = 0; k < CLUE_N; k++) {
        if ((k == 0) ? !(g_clue_mask & CLUE_CENTER) : !(g_clue_mask & CLUE_CORNERS)) continue;
        for (int o = 0; o < 4; o++)
            if (g_clue_orients & (1u << o)) used_set(p->used, g_clue[o][k].piece);
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

    for (int c = 1; c < PUZZLE_SIDE; c++) {        /* frontier: cols 1..15 (col 0 edge) */
        int old_t = p->rtop[c], new_t = new_top[c];
        if (color_is_inner(old_t)) p->req_exposed[INNER_IDX(old_t)]--;
        if (color_is_inner(new_t)) p->req_exposed[INNER_IDX(new_t)]++;
    }
    for (int c = 0; c < PUZZLE_SIDE; c++) p->rtop[c] = new_top[c];

    if (!g_free_edges || g_free_demand) {
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
   --no_free_demand is not in force (see beam_init_border: the demands are exact
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

static bool pdedup_has(uint64_t k) {
    size_t h = (size_t)k & (g_pdedup_sz - 1);
    while (g_pdedup[h]) {
        if (g_pdedup[h] == k) return true;
        h = (h + 1) & (g_pdedup_sz - 1);
    }
    return false;
}

static void pdedup_put(uint64_t k) {
    size_t h = (size_t)k & (g_pdedup_sz - 1);
    while (g_pdedup[h]) {
        if (g_pdedup[h] == k) return;
        h = (h + 1) & (g_pdedup_sz - 1);
    }
    g_pdedup[h] = k;
    g_pdedup_n++;
}

static void pdedup_grow(void) {
    uint64_t *old = g_pdedup; size_t old_sz = g_pdedup_sz;
    g_pdedup_sz <<= 1;
    g_pdedup = xmalloc(g_pdedup_sz * sizeof *g_pdedup);
    memset(g_pdedup, 0, g_pdedup_sz * sizeof *g_pdedup);
    g_pdedup_n = 0;
    for (size_t i = 0; i < old_sz; i++) if (old[i]) pdedup_put(old[i]);
    free(old);
}

/* The n masked keys of one partial. Cell contributions are summed, so masking
   cell i is a subtraction. 0 marks an empty slot and is never a key. */
static void partial_keys(const uint64_t used[4], PartialKind kind, int orient,
                         const uint16_t *seq, int n, uint64_t *keys) {
    uint64_t base = splitmix64(0x5A17D3D0C0FFEE11ULL ^ (uint64_t)kind
                               ^ ((uint64_t)(orient + 2) << 8));
    for (int k = 0; k < 4; k++) base = splitmix64(base ^ used[k]);
    uint64_t part[PUZZLE_SIDE], sum = 0;
    for (int i = 0; i < n; i++) {
        part[i] = splitmix64(((uint64_t)i << 16) ^ seq[i] ^ 0x9E3779B97F4A7C15ULL);
        sum += part[i];
    }
    for (int i = 0; i < n; i++) {
        uint64_t k = splitmix64(base ^ (sum - part[i]) ^ (uint64_t)i);
        keys[i] = k ? k : 1;
    }
}

/* True, and the keys are recorded, iff no earlier partial is a duplicate.
   Called inside the e555_incomplete critical section. */
static bool partial_dedup_accept(const uint64_t *keys, int n) {
    for (int i = 0; i < n; i++) if (pdedup_has(keys[i])) return false;
    while ((g_pdedup_n + (size_t)n) * 2 > g_pdedup_sz) pdedup_grow();
    for (int i = 0; i < n; i++) pdedup_put(keys[i]);
    return true;
}

static void partial_dedup_init(void) {
    free(g_pdedup);
    g_pdedup_sz = (size_t)1 << 16;
    g_pdedup = xmalloc(g_pdedup_sz * sizeof *g_pdedup);
    memset(g_pdedup, 0, g_pdedup_sz * sizeof *g_pdedup);
    g_pdedup_n = 0;
    g_partial_count = 0;
    memset(g_part_count, 0, sizeof g_part_count);
}

static void partial_config_reset(void) {
    g_partial_count = 0;
    memset(g_part_count, 0, sizeof g_part_count);
}

static PartialKind partial_kind_from_mask(uint16_t colmask) {
    for (int k = 0; k < PART_N; k++)
        if (g_part_mask[k] == colmask) return (PartialKind)k;
    fatal("internal: unknown partial mask 0x%04x", (unsigned)colmask);
    return PART_AB;
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

static void partial_outputs_flush(void) {
    for (int k = 0; k < PFILE_N; k++)
        if (g_partial_fp[k]) fflush(g_partial_fp[k]);
}

static void partial_outputs_close(void) {
    for (int k = 0; k < PFILE_N; k++) {
        if (g_partial_fp[k]) fclose(g_partial_fp[k]);
        g_partial_fp[k] = NULL;
    }
}

static void partial_outputs_open_base(const char *base) {
    static const char *const suffix[PFILE_N] = { "_partial", "_partial_B" };
    char path[1152];
    for (int k = 0; k < PFILE_N; k++) {
        snprintf(path, sizeof path, "%s%s.csv", base, suffix[k]);
        g_partial_fp[k] = fopen(path, "a");
        if (!g_partial_fp[k]) fatal("cannot open %s: %s", path, strerror(errno));
        setvbuf(g_partial_fp[k], NULL, _IOFBF, EMIT_FILE_BUF);
        manifest_add(path);
    }
    printf("[out] incomplete-top partials -> %s_partial.csv (AB/AC/BC), "
           "%s_partial_B.csv (append)\n", base, base);
}

/* Decimal conversion. A board line is 512 of these and almost nothing else, so
   it is worth not going through fprintf's general machinery: measured ~2x on the
   exact emission pattern. */
static inline char *u32a(char *p, uint32_t v) {
    char t[10]; int k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) *p++ = t[--k];
    return p;
}

/* Does the board being formatted carry a piece at (r, c)? Rows below the top one
   are full; the top row carries only the columns in colmask. Anything above the
   top row is empty, which is what makes an attached row-13 clue isolated. */
static inline bool cell_is_placed(int r, int c, int row, uint16_t colmask) {
    if (r < 0 || r > row) return false;
    return (r < row) || ((colmask >> c) & 1u) != 0;
}

/* Flatten rows[0..row] into per-piece position/rotation vectors and write them
   as the 512 comma-separated fields that follow a line's prefix. colmask says
   which columns of the TOP row carry a piece (ROWMASK_FULL for a completed stop
   row, or AB/AC/BC/B for an --incomplete_top partial); every row below it is
   full either way. A mask rather than a last-placed column because the partial
   kinds leave a hole in the MIDDLE of the row, not only at its right end.
   Returns the byte count. */
static int format_board_tail(const RowChoice rows[EDGE_LEN], int row,
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
    /* --clue_corners: also show the two clue pieces the beam never reaches, but
       ONLY where they land in empty space. Isolated they cost nothing and say
       something -- the viewer makes it obvious the corners were pinned, and a
       hole-free Stage C solve has to build around them. Touching a placed cell
       they would assert an edge the search never chose and never scored, which
       on a row-12 board is two near-certain breaks bolted onto the best partial
       the run produced. So the attach yields to the board: a clue whose own cell
       or whose cell below is filled is simply left off, and the board is emitted
       as searched. There is no choice of orientation here -- the board committed
       to one when it placed its row-2 corners. */
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

/* Emit one --incomplete_top partial: the parent's ancestry plus the stop-row
   segments named by colmask. Reconstruction, keys and formatting are parallel;
   only the dedup and the write are serialised. */
static void emit_incomplete(const BeamCtx *ctx, const BeamEntry *parent,
                            const RowChoice *mv, int row, uint16_t colmask) {
    PartialKind kind = partial_kind_from_mask(colmask);
    FILE **fp = &g_partial_fp[partial_file_of(kind)];
    if (!*fp) return;

    RowChoice rows[EDGE_LEN];
    rows[row] = *mv;
    collect_rows(ctx, parent, rows);

    const int orient = ENTRY_HAS_ORIENT(parent) ? (int)ENTRY_ORIENT(parent) : -1;
    uint16_t seq[PUZZLE_SIDE];
    int n = 0;
    for (int c = 1; c < PUZZLE_SIDE; c++) {
        if (!((colmask >> c) & 1u)) continue;
        uint8_t rot;
        board_cell(rows, row, c, &seq[n++], &rot);
    }
    uint64_t keys[PUZZLE_SIDE];
    partial_keys(parent->used, kind, orient, seq, n, keys);

    char line[EMIT_LINE_MAX];
    int len = format_board_tail(rows, row, colmask, orient, line);

    #pragma omp critical(e555_incomplete)
    {
        if (*fp && partial_dedup_accept(keys, n)) {
            g_part_total[kind]++;
            g_part_count[kind]++;
            g_partial_count++;
            uint64_t sol_idx = g_partial_total++;
            fprintf(*fp, "%s, %" PRIu64, g_config_id_str, sol_idx);
            fwrite(line, 1, (size_t)len, *fp);
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

typedef enum {
    REJ_A_CELL = 0,
    REJ_A_DECODE,
    REJ_B_CELL,
    REJ_B_DECODE,
    REJ_C_CELL,
    REJ_C_DECODE,
    REJ_LOOKAHEAD,
    REJ_PARITY,
    REJ_N
} RejectKind;

static const char *const g_rej_name[REJ_N] = {
    "A-cell", "A-conflict", "B-cell", "B-conflict",
    "C-cell", "C-conflict", "lookahead", "parity"
};
static uint64_t g_reject[MAX_ACC_THREADS][EDGE_LEN + 2][REJ_N];

static inline void reject_count(int row, RejectKind kind) {
    int th = omp_get_thread_num();
    if (th >= 0 && th < MAX_ACC_THREADS && row >= 0 && row <= EDGE_LEN + 1)
        g_reject[th][row][kind]++;
}
static double g_d2n_acc[MAX_ACC_THREADS][ACC_STRIDE];   /* [0]=sum [1]=sumsq [2]=n */
/* Current-configuration estimates drive the next row.  If a row is too thin,
   scoring falls back to the stable run-pooled estimate instead of whichever
   earlier configuration happened to run last. */
static double g_maha_local_mean[EDGE_LEN + 2];
static double g_maha_local_sd[EDGE_LEN + 2];
static double g_maha_local_n[EDGE_LEN + 2];
static double g_maha_pool_sum[EDGE_LEN + 2];
static double g_maha_pool_sumsq[EDGE_LEN + 2];
static double g_maha_pool_n[EDGE_LEN + 2];

/* Verbose-only score diagnostics.  These are descriptive candidate-population
   moments, not independent statistical samples.  They quantify actual weighted
   contributions, so their SDs show which terms can move a ranking and their
   correlations show overlap. */
typedef enum {
    SD_FANOUT = 0, SD_CLOSURE, SD_MAHA, SD_TABLE_INC, SD_TABLE_ACC, SD_N
} ScoreDiagKind;
typedef struct {
    double n, sum[SD_N], sumsq[SD_N];
    double cross_table_fan, cross_table_J, cross_J_maha;
} ScoreDiag;
static ScoreDiag g_score_diag[MAX_ACC_THREADS][EDGE_LEN + 2];

static inline void score_diag_acc(int row, double fanout, double closure,
                                  double maha, double table_inc,
                                  double table_acc) {
    if (!g_verbose) return;
    int th = omp_get_thread_num();
    if (th < 0 || th >= MAX_ACC_THREADS || row < 0 || row > EDGE_LEN + 1) return;
    ScoreDiag *d = &g_score_diag[th][row];
    const double v[SD_N] = { fanout, closure, maha, table_inc, table_acc };
    d->n += 1.0;
    for (int k = 0; k < SD_N; k++) {
        d->sum[k] += v[k];
        d->sumsq[k] += v[k] * v[k];
    }
    d->cross_table_fan += table_acc * fanout;
    d->cross_table_J   += table_acc * closure;
    d->cross_J_maha    += closure * maha;
}

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
static void maha_reset_config(void) {
    memset(g_maha_local_mean, 0, sizeof g_maha_local_mean);
    memset(g_maha_local_sd,   0, sizeof g_maha_local_sd);
    memset(g_maha_local_n,    0, sizeof g_maha_local_n);
}

static void maha_close_row(int row) {
    double sum = 0.0, sumsq = 0.0, n = 0.0;
    for (int th = 0; th < MAX_ACC_THREADS; th++) {
        sum += g_d2n_acc[th][0]; sumsq += g_d2n_acc[th][1]; n += g_d2n_acc[th][2];
        g_d2n_acc[th][0] = g_d2n_acc[th][1] = g_d2n_acc[th][2] = 0.0;
    }
    if (row < 0 || row > EDGE_LEN + 1 || n <= 0.0) return;

    g_maha_pool_sum[row]   += sum;
    g_maha_pool_sumsq[row] += sumsq;
    g_maha_pool_n[row]     += n;

    if (n < MAHA_MIN_SAMPLES) return;
    double mean = sum / n;
    double var  = sumsq / n - mean * mean;
    g_maha_local_mean[row] = mean;
    g_maha_local_sd[row]   = (var > 1e-18) ? sqrt(var) : 0.0;
    g_maha_local_n[row]    = n;
}

static bool maha_calibration(int source_row, double *mean, double *sd) {
    if (source_row < 0 || source_row > EDGE_LEN + 1) return false;
    if (g_maha_local_n[source_row] >= MAHA_MIN_SAMPLES
        && g_maha_local_sd[source_row] > 0.0) {
        *mean = g_maha_local_mean[source_row];
        *sd   = g_maha_local_sd[source_row];
        return true;
    }
    double n = g_maha_pool_n[source_row];
    if (n < MAHA_MIN_SAMPLES) return false;
    double m = g_maha_pool_sum[source_row] / n;
    double v = g_maha_pool_sumsq[source_row] / n - m * m;
    if (!(v > 1e-18)) return false;
    *mean = m;
    *sd = sqrt(v);
    return true;
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
 * Unlike maha_d2n this reads S and R off the board, so it needs no assumption
 * that n = 14*row matches the real piece count -- it is exact from a partial. */
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
static inline double color_term_parts(const BeamEntry *t, int row,
                                      double *closure_out, double *maha_out) {
    double closure = g_lambda_J * closure_raw(t);
    double maha = 0.0;
    if (g_lambda_maha != 0.0) {
        double d2n = maha_d2n(t, row);
        maha_acc(d2n);
        double mean = 0.0, sd = 0.0;
        if (row >= 2 && maha_calibration(row - 1, &mean, &sd))
            maha = g_lambda_maha * (d2n - mean) / sd;
    }
    if (closure_out) *closure_out = closure;
    if (maha_out) *maha_out = maha;
    return closure + maha;
}


/* -- Scoring (one-row lookahead + heuristic terms) --------------------------- */

/* The child's exposed tops key the next row. Segment A's next cell uses the
   FIXED next left-column color (exact); B and C use the left-agnostic fan-out
   table (their next left neighbour is unknown until A/B are chosen). Any
   NULL/zero is an exact one-row death -> the child is rejected. Applied on beam
   rows only: stop-row boards are emitted without this gate (see try_A). */
/* -- Clue plumbing ---------------------------------------------------------- */

static bool g_clue_debug  = false;   /* E555_CLUE_DEBUG=1 */
/* --pin_clue N: 0 = off (hedge over all four frames, the default), 1..4 = pin
   the frame whose center clue sits in that quadrant. Kept as the raw N rather
   than the resolved orientation so --print_cmd can echo back what was typed. */
static int  g_pin_clue    = 0;
static uint64_t g_dbg_nA[EDGE_LEN+2], g_dbg_nB[EDGE_LEN+2], g_dbg_nC[EDGE_LEN+2], g_dbg_calls[EDGE_LEN+2];
static int  g_clue_first[4];         /* lowest row that owes a pin, per orientation */
static int  g_clue_last_assign = -1; /* last row at which a board may still commit */

static uint16_t cat_index_of(uint16_t pid, uint8_t spin) {
    for (int i = 0; i < g_cat_count; i++)
        if (g_cat[i].piece_id == pid && g_cat[i].rotation == spin) return (uint16_t)i;
    fatal("clue piece %u spin %u is not in the oriented catalog", pid, spin);
    return 0;
}

/* Is this clue entry active under the current flags? Entry 0 is the center. */
static inline bool clue_on(int k) {
    return (k == 0) ? (g_clue_mask & CLUE_CENTER) != 0 : (g_clue_mask & CLUE_CORNERS) != 0;
}

static void init_clue_tables(void) {
    if (!g_clue_mask) return;
    g_clue_debug = getenv("E555_CLUE_DEBUG") != NULL;
    for (int o = 0; o < 4; o++) {
        for (int k = 0; k < CLUE_N; k++)
            g_clue_ci[o][k] = cat_index_of(g_clue[o][k].piece, g_clue[o][k].spin);
        /* The table's shape is load-bearing everywhere below; assert it once. */
        if (g_clue[o][1].row != 2 || g_clue[o][1].col != 2 ||
            g_clue[o][2].row != 2 || g_clue[o][2].col != 13)
            fatal("clue table: entries 1,2 must be the row-2 corners");
        if (g_clue[o][3].row != 13 || g_clue[o][4].row != 13)
            fatal("clue table: entries 3,4 must sit on row 13");
        /* The first row at which o demands anything is the PUSH-DOWN row, one
           below its lowest clue -- that is where a board commits, not the clue's
           own row. Corners put it at row 1, the center at 6 or 7. */
        g_clue_first[o] = 99;
        for (int k = 0; k < CLUE_N_REACHABLE; k++) {     /* row 13 pins nothing */
            if (!clue_on(k)) continue;
            int r = g_clue[o][k].row - 1;
            if (r < 1) r = 1;
            if (r < g_clue_first[o]) g_clue_first[o] = r;
        }
    }
    for (int o = 0; o < 4; o++)
        if ((g_clue_orients & (1u << o)) && g_clue_first[o] < 99 &&
            g_clue_first[o] > g_clue_last_assign) g_clue_last_assign = g_clue_first[o];
}

/* Pins owed by `row` under orientation o: pin_idx[s] is the position within
   segment s (0=A,1=B,2=C) that is nailed down, or -1. Returns true if o owes
   anything, in which case the row must go through expand_clued.

   Two rules, and the second is the one that is easy to miss: a clue ON this row
   fixes a piece, and a clue on the row ABOVE fixes a COLOR here, because a
   clue's bottom face has to meet whatever sits under it. Demanding that color
   while the segments are being generated is what keeps the row alive; filtering
   finished children instead is not equivalent in effect, because the beam keeps
   about one child per A record, so rejecting the ~94% that miss starves the row
   rather than reshaping it. Measured on the row-2 corners: 22 surviving boards
   filtering against 3328 pinning.

   Driving both rules off the table rather than hard-coding the row-1 case makes
   them cover every clue the search actually places: the row-2 corners and the
   center's row 6-or-7 push-down, which is what the beam pays for most (without
   it the center row kept 1075 boards of 104833).

   Entries 3..4 pin NOTHING. They sit on row 13, which no legal --stop_row
   reaches, so they are only ever reserved -- and a clue that is never placed has
   no business constraining the row below it. Pinning row 12 under them bought a
   matching junction for two optional pieces and cost the whole row: a pinned row
   goes through expand_clued, which skips the --incomplete_top emitters, so every
   11-of-16 board at the hardest row in the search was silently thrown away. Rows
   11 and 12 are where the search nearly dies; nothing there is worth spending on
   a clue this program does not enforce.

   Only one pin per segment is representable. Within a single orientation no row
   ever wants two -- row 2 puts its pair in segments A and C, the center is alone
   in B -- and init asserts the table shape that guarantees it. */
static bool clue_pins_for(int row, int o, int pin_idx[3], int pin_kind[3],
                          uint16_t pin_val[3]) {
    pin_idx[0] = pin_idx[1] = pin_idx[2] = -1;
    pin_kind[0] = pin_kind[1] = pin_kind[2] = PIN_PIECE;
    pin_val[0] = pin_val[1] = pin_val[2] = 0;

    bool any = false;
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_clue[o][k];
        int c = -1, kind = PIN_PIECE; uint16_t val = 0;
        if (k >= CLUE_N_REACHABLE) continue;              /* row 13: never searched */
        if (cc->row == row) {                            /* the clue itself */
            c = cc->col; kind = PIN_PIECE; val = g_clue_ci[o][k];
        } else if (cc->row == row + 1) {                 /* the color it will sit on */
            c = cc->col; kind = PIN_TOPCOLOR; val = g_cat[g_clue_ci[o][k]].bottom;
        }
        if (c < 1 || c > EDGE_LEN) continue;             /* cols 1..14 are the segments */
        int s = (c - 1) / CHAIN_LEN;
        if (pin_idx[s] >= 0) fatal("clue table: row %d wants two pins in segment %d", row, s);
        pin_idx[s] = (c - 1) % CHAIN_LEN; pin_kind[s] = kind; pin_val[s] = val;
        any = true;
    }
    return any;
}

/* E555_CLUE_DEBUG=1: the whole pin schedule, once, before any search runs.
   Rows 11 and 12 are reached so rarely that a live run is a poor way to check
   what they owe, and the schedule is entirely determined by the table -- so
   print it instead of hunting for a board that gets there. */
static void clue_dump_schedule(void) {
    if (!g_clue_mask || !g_clue_debug) return;
    for (int o = 0; o < 4; o++) {
        if (!(g_clue_orients & (1u << o))) continue;
        printf("[clue] orientation %d commits at row %d\n", o, g_clue_first[o]);
        for (int r = 1; r <= EDGE_LEN; r++) {
            int pi[3], pk[3]; uint16_t pv[3];
            if (!clue_pins_for(r, o, pi, pk, pv)) continue;
            printf("[clue]   row %2d:", r);
            for (int s = 0; s < 3; s++) {
                if (pi[s] < 0) continue;
                if (pk[s] == PIN_PIECE)
                    printf("  seg%c[%d] = piece %u", 'A' + s, pi[s], g_cat[pv[s]].piece_id);
                else
                    printf("  seg%c[%d] = color %u below a clue", 'A' + s, pi[s], pv[s]);
            }
            printf("\n");
        }
    }
    fflush(stdout);
}

/* ===========================================================================
 * Data-driven frequency table  (this fork only)
 * ===========================================================================
 * Piece-by-cell and piece-by-5-5-5-segment distributions, measured in a
 * LEARNING phase and then used as the beam's ranking objective in a SEARCH
 * phase.
 *
 * The stock objective is local: a colour ledger plus a one-row fan-out
 * lookahead, with no opinion about WHERE a piece belongs. With the clues pinned
 * and a rotations row fixing the corners the puzzle is anchored, so "piece p
 * tends to sit at cell x" is a geometric statement that can be measured.
 *
 * FOUR PASSES. Pass j turns the rotations row j quarter-turns clockwise and pins
 * the clue frame to match -- the same anchored puzzle, relabelled -- and every
 * board is folded back to the canonical frame. At --stop_row S, pass 0 covers
 * canonical rows 1..S, pass 2 rows 15-S..14, passes 1 and 3 the columns, so the
 * top rows are measured by pass 2's first rows.
 * =========================================================================== */

#define FREQ_NPASS           4
#define LEARN_CAP_PER_CONFIG 10000 /* boards counted per configuration */
#define FREQ_KT_HALF         0.5   /* Krichevsky-Trofimov: 1/2 count/category */
#define SINKHORN_ITERS       400
#define SINKHORN_TOL         1e-13
#define FREQ_CENTRE_CELL     119   /* (7,7): where canonicalising must land 138 */
#define FREQ_TABLE_MAGIC     "E555 datadriven counts v4"
#define FREQ_TABLE_VERSION   4

/* --freq_model: the beam's spatial resolution. `segment` pools counts over the
   generator's A/B/C bins, `cell` keeps the exact piece-by-cell matrix. Both are
   KT-smoothed, Sinkhorn-balanced and conditioned on the rows still unfilled, so
   only the resolution differs. Border ranking always uses exact cells: choosing
   a bottom row or left column is an ordering problem. */
typedef enum { FREQ_MODEL_SEGMENT = 0, FREQ_MODEL_CELL = 1 } FreqModel;
static FreqModel g_freq_model = FREQ_MODEL_SEGMENT;
static const char *g_learn_path = NULL;   /* --learn PATH */
static const char *g_table_path = NULL;   /* --table PATH */
static bool        g_learning   = false;  /* accumulating, not emitting */
static bool        g_freq_on    = false;  /* a table is loaded and steering */
static bool        g_freq_border_ok = true;  /* border block in use */

/* In learning, --wall_time is split evenly across the four passes, so a
   time-boxed run still measures the top rows (passes 1..3). */
static double      g_pass_deadline = 0.0;    /* absolute; 0 = no limit */

static inline bool pass_time_spent(void)
{
    return g_pass_deadline > 0.0 && omp_get_wtime() >= g_pass_deadline;
}
static bool        g_freq_debug = false;  /* E555_FREQ_DEBUG=1 */
/* Guided score = stock row-local score (fan-out lookahead + colour terms) +
   the learned statistic accumulated over every committed row. The two answer
   different questions: where a piece belongs, and whether any row fits above.
   BeamEntry.score carries ONLY the learned accumulator; the stock terms stay
   row-local exactly as in the stock beamer.

   E555_FREQ_PURE=1 ranks by the learned statistic alone; E555_FREQ_DEBUG=1 then
   asserts that the accumulator equals a from-scratch sum over the placed cells.
   E555_FREQ_NOBORDER=1 drops the border block, leaving the bottom row and left
   column to the library's ranking. */
static bool        g_freq_pure  = false;

#define FREQ_TSZ ((size_t)NUM_PIECES * NUM_PIECES)   /* [piece*256 + cell] */
#define FREQ_RSZ ((size_t)NUM_PIECES * PUZZLE_SIDE)  /* [piece*16 + row] */
#define FREQ_SEG_N (PUZZLE_SIDE * NUM_SEG)            /* [row*3 + A/B/C] */
#define FREQ_SSZ ((size_t)NUM_PIECES * FREQ_SEG_N)   /* [piece*48 + segment] */

static float  *g_fw = NULL;          /* active beam weights: segment or exact cell */
static float  *g_fw_segment = NULL;  /* future-conditioned 5-5-5 preservation */
static float  *g_fw_cell = NULL;     /* future-conditioned exact-cell preservation */
static float  *g_fw_location = NULL; /* unconditional exact location for border rank */
static double *g_pcnt[FREQ_NPASS];    /* per-pass weighted cell counts */
static double *g_pwcell[FREQ_NPASS];  /* per-pass weight covering each cell */
static double *g_pwsq[FREQ_NPASS];    /* per-pass sum of squared weight (ESS) */
static double *g_psegcnt[FREQ_NPASS]; /* per-pass inner piece-by-segment counts */
static double *g_pwseg[FREQ_NPASS];   /* per-pass total cell weight per segment */
static size_t  g_pconfigs[FREQ_NPASS];   /* configurations that contributed */
static size_t  g_ppool[FREQ_NPASS];      /* bottoms the pass had to draw from */
static size_t  g_preps[FREQ_NPASS];      /* times it cycled that pool */
static double *g_cfgcnt = NULL;      /* one configuration's raw counts */
static size_t  g_cfg_boards = 0;     /* boards counted for the live config */
static int     g_pass = 0;           /* which pass is running */
static int     g_orient_base = 0;    /* clue orientation of the INPUT rotations row */
static int     g_centre_cell = FREQ_CENTRE_CELL;  /* where it puts the centre clue */
static uint8_t g_spin0[NUM_PIECES];  /* the canonical (unturned) rotations row */

/* Prequential lift, accumulated online. The current configuration is
   scored before its counts are folded in, using the same 5-5-5 resolution as the
   beam (exact cells for border pieces). The processed search table also applies
   Sinkhorn and future conditioning; this cheaper statistic answers the basic
   question "does the segment table predict unseen configurations at all?" */
static double g_lift_sum = 0.0;
static double g_lift_cells = 0.0;
static size_t g_lift_boards = 0;

/* -- Geometry --------------------------------------------------------------- */

/* How many of a cell's four sides face out of the board: 2 corner, 1 border,
   0 interior. The same partition the frame rule fixes on pieces, so a piece can
   only ever occupy a cell of its own kind. */
static inline int cell_kind(int cell)
{
    int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
    return (r == 0) + (r == PUZZLE_SIDE - 1) + (c == 0) + (c == PUZZLE_SIDE - 1);
}

static inline int piece_kind(int p)
{
    return (g_seed_top[p] == 0) + (g_seed_right[p] == 0)
         + (g_seed_bottom[p] == 0) + (g_seed_left[p] == 0);
}

/* One clockwise quarter-turn sends (r,c) -> (15-c, r); n of them, applied n
   times. Identical to rotate_cell in tools/E555_rotate.py and to rotate_cw in
   E555_roundhouse.c, which is the convention the whole repo turns boards by. */
static inline int cell_turn_cw(int cell, int n)
{
    int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
    for (int i = 0; i < (n & 3); i++) { int nr = PUZZLE_SIDE - 1 - c; c = r; r = nr; }
    return r * PUZZLE_SIDE + c;
}

/* Turn a rotations row n quarter-turns clockwise.
 *
 * A border piece's spin IS its side: its grey face points at the frame edge it
 * belongs to, which is why a rotations row carries no side field and
 * classify_deal_from_rotations reads the assignment straight back out of the
 * spins. So the turn is spin += 3n on exactly the pieces that have a grey face,
 * and on nothing else -- an inner piece's spin in a rotations row is never read
 * by anything downstream. This is the same map tools/E555_rotate.py --rotations
 * applies, which the release gate checks against the spins a turned BOARD
 * carries on its 60 frame cells. */
static void rot_row_turn(uint8_t spins[NUM_PIECES], int n)
{
    if (!(n &= 3)) return;
    for (int p = 0; p < NUM_PIECES; p++)
        if (piece_kind(p) > 0) spins[p] = (uint8_t)((spins[p] + 3 * n) & 3);
}

/* -- The table -------------------------------------------------------------- */

/* Which (piece, cell) pairs the table is allowed to hold. Corners are fixed by
   the rotations row and carry no information; an enabled clue piece is pinned,
   not learned, and is already barred from the chain database. Everything else is
   in, matched by kind. */
static bool g_piece_free[NUM_PIECES];
static bool g_cell_free[NUM_PIECES];
static int  g_n_free_inner_pieces = 0;
static int  g_seg_cap[FREQ_SEG_N];              /* free inner slots per A/B/C bin */
static int  g_edge_side_of_piece[NUM_PIECES];   /* 0=top 1=right 2=bottom 3=left */

/* Segment numbering follows the live row generator exactly. Segment C has four
   free INNER cells (11..14); its fifth database position is the right-edge piece
   at column 15 and is modelled in the side-specific edge block. */
static inline int freq_segment_of_cell(int cell)
{
    int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
    if (r <= 0 || r >= PUZZLE_SIDE - 1 || c <= 0 || c >= PUZZLE_SIDE - 1)
        return -1;
    int s = (c <= CHAIN_LEN) ? 0 : (c <= 2 * CHAIN_LEN) ? 1 : 2;
    return r * NUM_SEG + s;
}

static void freq_mark_free(void)
{
    for (int p = 0; p < NUM_PIECES; p++) g_piece_free[p] = (piece_kind(p) < 2);
    for (int x = 0; x < NUM_PIECES; x++) g_cell_free[x] = (cell_kind(x) < 2);
    for (int k = 0; k < CLUE_N; k++) {
        if ((k == 0) ? !(g_clue_mask & CLUE_CENTER) : !(g_clue_mask & CLUE_CORNERS))
            continue;
        const ClueCell *cc = &g_clue[g_orient_base][k];   /* the input row's frame */
        g_piece_free[cc->piece] = false;
        g_cell_free[cc->row * PUZZLE_SIDE + cc->col] = false;
    }
    g_n_free_inner_pieces = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (g_piece_free[p] && piece_kind(p) == 0) g_n_free_inner_pieces++;
    memset(g_seg_cap, 0, sizeof g_seg_cap);
    for (int x = 0; x < NUM_PIECES; x++) {
        int z = g_cell_free[x] ? freq_segment_of_cell(x) : -1;
        if (z >= 0) g_seg_cap[z]++;
    }

    /* An edge piece's side in the CANONICAL frame, read off the canonical
       rotations row. Turning the row moves every edge to the next side, so in
       canonical coordinates a piece keeps this side in all four passes -- which
       is what makes the border block four independent per-side blocks. */
    for (int p = 0; p < NUM_PIECES; p++) {
        g_edge_side_of_piece[p] = -1;
        if (piece_kind(p) != 1) continue;
        const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
        for (int d = 0; d < 4; d++)
            if (e[(d + g_spin0[p]) & 3] == 0) { g_edge_side_of_piece[p] = d; break; }
    }
}

static int edge_side_of_cell(int cell)
{
    int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
    if (r == PUZZLE_SIDE - 1) return 0;          /* top */
    if (c == PUZZLE_SIDE - 1) return 1;          /* right */
    if (r == 0)               return 2;          /* bottom */
    if (c == 0)               return 3;          /* left */
    return -1;
}

static void freq_alloc(void)
{
    for (int j = 0; j < FREQ_NPASS; j++) {
        g_pcnt[j]    = xmalloc(FREQ_TSZ * sizeof(double));
        g_pwcell[j]  = xmalloc(NUM_PIECES * sizeof(double));
        g_pwsq[j]    = xmalloc(NUM_PIECES * sizeof(double));
        g_psegcnt[j] = xmalloc(FREQ_SSZ * sizeof(double));
        g_pwseg[j]   = xmalloc(FREQ_SEG_N * sizeof(double));
        memset(g_pcnt[j],    0, FREQ_TSZ * sizeof(double));
        memset(g_pwcell[j],  0, NUM_PIECES * sizeof(double));
        memset(g_pwsq[j],    0, NUM_PIECES * sizeof(double));
        memset(g_psegcnt[j], 0, FREQ_SSZ * sizeof(double));
        memset(g_pwseg[j],   0, FREQ_SEG_N * sizeof(double));
    }
    g_cfgcnt = xmalloc(FREQ_TSZ * sizeof(double));
    memset(g_cfgcnt, 0, FREQ_TSZ * sizeof(double));
}

/* -- Learning: accumulate one configuration --------------------------------- */

/* Add one canonicalised board. Every cell it has placed is counted once, at
   weight 1; the configuration's boards are divided by their own number when the
   configuration closes, which is what gives each CONFIGURATION one vote rather
   than each board. */
static void freq_add_board(const RowChoice rows[EDGE_LEN], int row, int turn)
{
    for (int r = 0; r <= row; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint16_t pid; uint8_t rot;
            board_cell(rows, r, c, &pid, &rot);
            int x = cell_turn_cw(r * PUZZLE_SIDE + c, turn);
            if (!g_piece_free[pid] || !g_cell_free[x]) continue;
            g_cfgcnt[(size_t)pid * NUM_PIECES + x] += 1.0;
        }
    g_cfg_boards++;
}

/* Score one board against configurations closed before its own. Inner pieces use
   the A/B/C segment containing the cell; border pieces retain exact-cell scoring.
   KT adds one half-count to every eligible piece, the classical parameter-free
   finite-log estimator. */
static void freq_score_board(const RowChoice rows[EDGE_LEN], int row, int turn)
{
    double sum = 0.0; int n = 0;
    for (int r = 0; r <= row; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint16_t pid; uint8_t rot;
            board_cell(rows, r, c, &pid, &rot);
            int x = cell_turn_cw(r * PUZZLE_SIDE + c, turn);
            if (!g_piece_free[pid] || !g_cell_free[x]) continue;
            int kind = cell_kind(x);
            double K = (kind == 0) ? (double)g_n_free_inner_pieces : (double)EDGE_LEN;
            double nn = 0.0, w = 0.0;
            if (kind == 0) {
                int z = freq_segment_of_cell(x), cap = z >= 0 ? g_seg_cap[z] : 0;
                if (cap <= 0) continue;
                for (int j = 0; j < FREQ_NPASS; j++) {
                    nn += g_psegcnt[j][(size_t)pid * FREQ_SEG_N + z];
                    w  += g_pwseg[j][z];
                }
                nn /= (double)cap; w /= (double)cap;  /* evidence per physical slot */
            } else {
                for (int j = 0; j < FREQ_NPASS; j++) {
                    nn += g_pcnt[j][(size_t)pid * NUM_PIECES + x];
                    w  += g_pwcell[j][x];
                }
            }
            double prob = (nn + FREQ_KT_HALF) / (w + FREQ_KT_HALF * K);
            sum += log(prob * K);
            n++;
        }
    if (!n) return;
    g_lift_sum += sum; g_lift_cells += n; g_lift_boards++;
}

/* Close the live configuration: fold its counts in at 1/n_boards, so the
   configuration contributes one unit of weight to every cell it covered however
   many near-identical boards it emitted. That is the whole answer to the
   correlation between boards grown from one border, and it needs no model. */
static void freq_close_config(void)
{
    if (!g_cfg_boards) return;
    const double board_w = 1.0 / (double)g_cfg_boards;
    double *pc = g_pcnt[g_pass], *pw = g_pwcell[g_pass], *pq = g_pwsq[g_pass];
    double *ps = g_psegcnt[g_pass], *pws = g_pwseg[g_pass];
    for (int x = 0; x < NUM_PIECES; x++) {
        double tot = 0.0;
        int z = (cell_kind(x) == 0) ? freq_segment_of_cell(x) : -1;
        for (int p = 0; p < NUM_PIECES; p++) {
            size_t i = (size_t)p * NUM_PIECES + x;
            if (g_cfgcnt[i] == 0.0) continue;
            double add = g_cfgcnt[i] * board_w;
            pc[i] += add;
            if (z >= 0) ps[(size_t)p * FREQ_SEG_N + z] += add;
            tot += add;
            g_cfgcnt[i] = 0.0;
        }
        /* tot is 1 for a cell every board of the configuration placed, so the
           squared weights and the segment totals remain in CONFIGURATION units. */
        if (tot > 0.0) {
            pw[x] += tot; pq[x] += tot * tot;
            if (z >= 0) pws[z] += tot;
        }
    }
    g_pconfigs[g_pass]++;
    g_cfg_boards = 0;
}

/* Learning's replacement for emit_stop_row: boards are reconstructed, counted
   and dropped, never formatted or written.

   A configuration's vote is estimated from at most LEARN_CAP_PER_CONFIG of its
   distinct stop-row states, drawn uniformly without replacement: each state gets
   the key splitmix64(frontier signature ^ cfg_hash) and the smallest keys are
   counted. The draw follows board content, not pool order or score order; the
   latter carries no information when every stop-row score ties (e.g. --lambda_J 0
   --lambda_Mahalanobis 0 and no table). Serial: entered from serial code. */
typedef struct { uint64_t key; uint32_t idx; } LearnPick;
static LearnPick *g_learn_pick = NULL;
static size_t     g_learn_pick_cap = 0;

static int cmp_learn_pick(const void *a, const void *b)
{
    const LearnPick *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

static void learn_stop_row(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept,
                           int row, uint64_t cfg_hash)
{
    const int turn = (FREQ_NPASS - g_pass) & 3;   /* back to the canonical frame */
    if (g_learn_pick_cap < kept) {
        g_learn_pick = xrealloc(g_learn_pick, (size_t)kept * sizeof *g_learn_pick);
        g_learn_pick_cap = kept;
    }
    for (uint32_t k = 0; k < kept; k++) {
        uint32_t i = ctx->keep[k];
        g_learn_pick[k].key = splitmix64(ctx->pool[i].sig ^ cfg_hash);
        g_learn_pick[k].idx = i;
    }
    if (kept > LEARN_CAP_PER_CONFIG)
        qsort(g_learn_pick, kept, sizeof *g_learn_pick, cmp_learn_pick);

    uint32_t counted = 0;
    for (uint32_t k = 0; k < kept && counted < LEARN_CAP_PER_CONFIG && !g_stop; k++) {
        const PoolEntry *pe = &ctx->pool[g_learn_pick[k].idx];
        RowChoice rows[EDGE_LEN];
        rows[row] = pe->mv;
        collect_rows(ctx, &beam[pe->parent], rows);
        if (!htable_insert(board_fingerprint(rows, row))) continue;
        counted++;

        /* The one assertion that proves the whole rotation chain: the centre clue
           is pinned to this pass's frame, so folding the board back by (4-j)
           quarter-turns must land piece 138 on (7,7) -- the cell map, the spin
           map and the pin map all at once. */
        if (g_clue_mask & CLUE_CENTER) {
            int found = -1;
            for (int r = 0; r <= row && found < 0; r++)
                for (int c = 0; c < PUZZLE_SIDE; c++) {
                    uint16_t pid; uint8_t rot;
                    board_cell(rows, r, c, &pid, &rot);
                    if (pid == g_clue[g_orient_base][0].piece) {
                        found = cell_turn_cw(r * PUZZLE_SIDE + c, turn); break;
                    }
                }
            if (found >= 0 && found != g_centre_cell)
                fatal("pass %d: centre clue canonicalises to cell %d, not %d -- "
                      "the rotations turn and the clue pin disagree",
                      g_pass, found, g_centre_cell);
        }
        freq_score_board(rows, row, turn);
        freq_add_board(rows, row, turn);
        g_stats.emitted_total++;
    }
}

/* Piece-by-row and piece-by-segment sums of the raw counts. Written to the
   table for grep-level auditing and re-derived on load as a consistency check. */
static void freq_make_rowcnt(const double *cnt, double *rowcnt)
{
    memset(rowcnt, 0, FREQ_RSZ * sizeof(double));
    for (int p = 0; p < NUM_PIECES; p++) {
        if (!g_piece_free[p]) continue;
        for (int x = 0; x < NUM_PIECES; x++) {
            if (!g_cell_free[x]) continue;
            double v = cnt[(size_t)p * NUM_PIECES + x];
            if (v != 0.0) rowcnt[(size_t)p * PUZZLE_SIDE + x / PUZZLE_SIDE] += v;
        }
    }
}

static void freq_make_segcnt(const double *cnt, double *segcnt)
{
    memset(segcnt, 0, FREQ_SSZ * sizeof(double));
    for (int p = 0; p < NUM_PIECES; p++) {
        if (!g_piece_free[p] || piece_kind(p) != 0) continue;
        for (int x = 0; x < NUM_PIECES; x++) {
            int z = g_cell_free[x] ? freq_segment_of_cell(x) : -1;
            if (z < 0) continue;
            double v = cnt[(size_t)p * NUM_PIECES + x];
            if (v != 0.0) segcnt[(size_t)p * FREQ_SEG_N + z] += v;
        }
    }
}

/* -- The estimator ---------------------------------------------------------- */

/* Sinkhorn (IPF) to doubly stochastic over one square block.
 *
 * A row of the board is a PERMUTATION, so a beam maximising an unbalanced
 * sum log P(p|x) spends globally popular pieces on the low rows it commits first
 * and strands the top -- which is the disease this whole table is meant to treat.
 * Balancing makes every piece's mass and every cell's mass 1, which removes both
 * that and the coverage bias (cells seen by 4 passes against 2) in one step, and
 * leaves pure affinity. Smoothing guarantees strict positivity, so it converges. */
static void sinkhorn(double *m, const int *idx, int n)
{
    double *ru = xmalloc((size_t)n * sizeof(double));
    double *cu = xmalloc((size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) ru[i] = cu[i] = 1.0;
    for (int it = 0; it < SINKHORN_ITERS; it++) {
        double worst = 0.0;
        for (int i = 0; i < n; i++) {                      /* rows to 1 */
            double s = 0.0;
            for (int k = 0; k < n; k++) s += m[(size_t)i * n + k] * ru[i] * cu[k];
            if (s > 0.0) ru[i] /= s;
            double d = fabs(s - 1.0); if (d > worst) worst = d;
        }
        for (int k = 0; k < n; k++) {                      /* columns to 1 */
            double s = 0.0;
            for (int i = 0; i < n; i++) s += m[(size_t)i * n + k] * ru[i] * cu[k];
            if (s > 0.0) cu[k] /= s;
            double d = fabs(s - 1.0); if (d > worst) worst = d;
        }
        if (worst < SINKHORN_TOL) break;
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) m[(size_t)i * n + k] *= ru[i] * cu[k];
    (void)idx; free(ru); free(cu);
}

/* Largest deviation of any row or column sum from 1 -- the check that the
   balance actually converged, reported rather than assumed. */
static double block_balance_error(const double *m, int n)
{
    double worst = 0.0;
    for (int i = 0; i < n; i++) {
        double s = 0.0;
        for (int k = 0; k < n; k++) s += m[(size_t)i * n + k];
        if (fabs(s - 1.0) > worst) worst = fabs(s - 1.0);
    }
    for (int k = 0; k < n; k++) {
        double s = 0.0;
        for (int i = 0; i < n; i++) s += m[(size_t)i * n + k];
        if (fabs(s - 1.0) > worst) worst = fabs(s - 1.0);
    }
    return worst;
}

/* Merge the four passes: one configuration, one vote, no per-pass rescaling.
 * Unequal coverage is real information -- a region two thin passes cover is
 * less well known -- and KT smoothing plus Sinkhorn handle finite counts and
 * coverage bias. An uneven pass count is cured by more --top_bottoms. */
static void freq_merge(double *cnt, double *wcell, double *wsq)
{
    int live = 0;
    for (int j = 0; j < FREQ_NPASS; j++) if (g_pconfigs[j]) live++;
    if (!live) fatal("--learn: no configuration produced a board -- nothing to learn from");

    memset(cnt, 0, FREQ_TSZ * sizeof(double));
    memset(wcell, 0, NUM_PIECES * sizeof(double));
    memset(wsq, 0, NUM_PIECES * sizeof(double));
    for (int j = 0; j < FREQ_NPASS; j++) {
        if (!g_pconfigs[j]) continue;
        for (size_t i = 0; i < FREQ_TSZ; i++) cnt[i] += g_pcnt[j][i];
        for (int x = 0; x < NUM_PIECES; x++) {
            wcell[x] += g_pwcell[j][x];
            wsq[x]   += g_pwsq[j][x];
        }
    }
}

/* Turn merged counts into the statistics the search needs.
 *
 * BEAM: SEGMENT OR EXACT-CELL PRESERVATION
 *   Segment mode collapses inner counts to the generator's A/B/C bins. This SUMS raw
 *   occurrences before any logarithm is taken: a piece concentrated in one
 *   clue-adjacent cell contributes its full evidence to the segment rather than
 *   being cancelled by neutral cells elsewhere in it. Each bin is then converted
 *   back to evidence per physical slot, KT-smoothed, expanded over its free cells
 *   and Sinkhorn-balanced. Let q[p,x] be that balanced per-slot mass.
 *   When row r is being committed, only rows r..14 are still available, so
 *
 *       score(p,x at row r) = log( q[p,x] * C_r / R_p(r) )
 *
 *   where C_r is the number of free slots in rows r..14 and R_p(r) is piece p's
 *   total learned mass over those slots. This is the log lift over a uniform
 *   remaining slot. In log space R_p is a log-sum-exp over all future segments:
 *   one sharp top/corner preference dominates naturally, but no single noisy
 *   cell becomes a hard max. Spending a top-loving piece low is penalised now;
 *   a piece whose useful region is current is rewarded now.
 *
 *   Cell mode applies the identical remaining-opportunity normalization to the
 *   Sinkhorn-balanced piece-by-cell matrix. It preserves sharp clue-adjacent
 *   structure while remaining directly comparable to segment mode.
 *
 * BORDER: EXACT LOCATION
 *   Bottom and left orderings are ranked by a KT-smoothed, Sinkhorn-
 *   balanced piece-by-cell table. Exact position is the information that
 *   distinguishes permutations near the lower clues.
 *
 * Edge pieces on the vertical sides use the same future-conditioned form in the
 * beam, because their row is sequential too. Horizontal edge weights are zero
 * in the beam (the bottom is already fixed; the top is Stage C), while their
 * exact location weights remain live for border ranking. */
static double freq_build(const double *cnt, const double *wcell, const double *segcnt)
{
    if (!g_fw_segment)  g_fw_segment  = xmalloc(FREQ_TSZ * sizeof *g_fw_segment);
    if (!g_fw_cell)     g_fw_cell     = xmalloc(FREQ_TSZ * sizeof *g_fw_cell);
    if (!g_fw_location) g_fw_location = xmalloc(FREQ_TSZ * sizeof *g_fw_location);
    memset(g_fw_segment,  0, FREQ_TSZ * sizeof *g_fw_segment);
    memset(g_fw_cell,     0, FREQ_TSZ * sizeof *g_fw_cell);
    memset(g_fw_location, 0, FREQ_TSZ * sizeof *g_fw_location);

    int pl[NUM_PIECES], cl[NUM_PIECES], np = 0, nc = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (g_piece_free[p] && piece_kind(p) == 0) pl[np++] = p;
    for (int x = 0; x < NUM_PIECES; x++)
        if (g_cell_free[x] && cell_kind(x) == 0) cl[nc++] = x;
    if (np != nc) fatal("interior block is %d pieces x %d cells -- not square", np, nc);

    double seg_total[FREQ_SEG_N];
    memset(seg_total, 0, sizeof seg_total);
    for (int i = 0; i < np; i++)
        for (int z = 0; z < FREQ_SEG_N; z++)
            seg_total[z] += segcnt[(size_t)pl[i] * FREQ_SEG_N + z];

    double *mloc = xmalloc((size_t)np * np * sizeof(double));
    double *mseg = xmalloc((size_t)np * np * sizeof(double));
    for (int i = 0; i < np; i++)
        for (int k = 0; k < nc; k++) {
            int x = cl[k], z = freq_segment_of_cell(x), cap = z >= 0 ? g_seg_cap[z] : 0;
            if (cap <= 0) fatal("free interior cell %d has no 5-5-5 segment", x);
            mloc[(size_t)i * np + k] =
                (cnt[(size_t)pl[i] * NUM_PIECES + x] + FREQ_KT_HALF)
                / (wcell[x] + FREQ_KT_HALF * np);
            double mean_n = segcnt[(size_t)pl[i] * FREQ_SEG_N + z] / (double)cap;
            double mean_w = seg_total[z] / (double)cap;
            mseg[(size_t)i * np + k] =
                (mean_n + FREQ_KT_HALF) / (mean_w + FREQ_KT_HALF * np);
        }
    sinkhorn(mloc, NULL, np);
    sinkhorn(mseg, NULL, np);
    double err = block_balance_error(mloc, np);
    double e = block_balance_error(mseg, np); if (e > err) err = e;

    int row_slots[PUZZLE_SIDE] = {0}, rem_slots[PUZZLE_SIDE] = {0};
    for (int k = 0; k < nc; k++) row_slots[cl[k] / PUZZLE_SIDE]++;
    for (int r = PUZZLE_SIDE - 2; r >= 1; r--)
        rem_slots[r] = rem_slots[r + 1] + row_slots[r];

    double *rem_seg = xmalloc((size_t)np * PUZZLE_SIDE * sizeof(double));
    double *rem_loc = xmalloc((size_t)np * PUZZLE_SIDE * sizeof(double));
    memset(rem_seg, 0, (size_t)np * PUZZLE_SIDE * sizeof(double));
    memset(rem_loc, 0, (size_t)np * PUZZLE_SIDE * sizeof(double));
    for (int i = 0; i < np; i++) {
        double acc_seg = 0.0, acc_loc = 0.0;
        for (int r = PUZZLE_SIDE - 2; r >= 1; r--) {
            for (int k = 0; k < nc; k++) if (cl[k] / PUZZLE_SIDE == r) {
                acc_seg += mseg[(size_t)i * np + k];
                acc_loc += mloc[(size_t)i * np + k];
            }
            rem_seg[(size_t)i * PUZZLE_SIDE + r] = acc_seg;
            rem_loc[(size_t)i * PUZZLE_SIDE + r] = acc_loc;
        }
    }

    for (int i = 0; i < np; i++)
        for (int k = 0; k < nc; k++) {
            int x = cl[k], r = x / PUZZLE_SIDE;
            double rs = rem_seg[(size_t)i * PUZZLE_SIDE + r];
            double rl = rem_loc[(size_t)i * PUZZLE_SIDE + r];
            if (!(rs > 0.0) || !(rl > 0.0) || rem_slots[r] <= 0)
                fatal("frequency model has no remaining mass for piece %d row %d", pl[i], r);
            size_t fi = (size_t)pl[i] * NUM_PIECES + x;
            g_fw_location[fi] = (float)log((double)np * mloc[(size_t)i * np + k]);
            g_fw_segment[fi] = (float)log(mseg[(size_t)i * np + k]
                                           * (double)rem_slots[r] / rs);
            g_fw_cell[fi] = (float)log(mloc[(size_t)i * np + k]
                                        * (double)rem_slots[r] / rl);
        }
    free(mloc); free(mseg); free(rem_seg); free(rem_loc);

    /* Edge pieces have no A/B/C pooling.  On vertical sides both beam models use
       the same future-conditioned exact-location distribution; on horizontal
       sides the active beam weight is zero (bottom already fixed, top is Stage C).
       The unconditional exact-location table remains live for border ranking. */
    for (int side = 0; side < 4; side++) {
        int bp[PUZZLE_SIDE], bc[PUZZLE_SIDE], n1 = 0, n2 = 0;
        for (int p = 0; p < NUM_PIECES; p++)
            if (g_piece_free[p] && piece_kind(p) == 1 && g_edge_side_of_piece[p] == side)
                bp[n1++] = p;
        for (int x = 0; x < NUM_PIECES; x++)
            if (g_cell_free[x] && cell_kind(x) == 1 && edge_side_of_cell(x) == side)
                bc[n2++] = x;
        if (n1 != n2 || !n1) continue;

        double *b = xmalloc((size_t)n1 * n1 * sizeof(double));
        for (int i = 0; i < n1; i++)
            for (int k = 0; k < n1; k++) {
                int x = bc[k];
                b[(size_t)i * n1 + k] =
                    (cnt[(size_t)bp[i] * NUM_PIECES + x] + FREQ_KT_HALF)
                    / (wcell[x] + FREQ_KT_HALF * n1);
            }
        sinkhorn(b, NULL, n1);
        e = block_balance_error(b, n1); if (e > err) err = e;

        int brow_slots[PUZZLE_SIDE] = {0}, brem_slots[PUZZLE_SIDE] = {0};
        double *brem_mass = NULL;
        if (side == 1 || side == 3) {
            for (int k = 0; k < n1; k++) brow_slots[bc[k] / PUZZLE_SIDE]++;
            for (int r = PUZZLE_SIDE - 2; r >= 1; r--)
                brem_slots[r] = brem_slots[r + 1] + brow_slots[r];
            brem_mass = xmalloc((size_t)n1 * PUZZLE_SIDE * sizeof(double));
            memset(brem_mass, 0, (size_t)n1 * PUZZLE_SIDE * sizeof(double));
            for (int i = 0; i < n1; i++) {
                double acc = 0.0;
                for (int r = PUZZLE_SIDE - 2; r >= 1; r--) {
                    for (int k = 0; k < n1; k++)
                        if (bc[k] / PUZZLE_SIDE == r) acc += b[(size_t)i * n1 + k];
                    brem_mass[(size_t)i * PUZZLE_SIDE + r] = acc;
                }
            }
        }
        for (int i = 0; i < n1; i++)
            for (int k = 0; k < n1; k++) {
                int x = bc[k], r = x / PUZZLE_SIDE;
                double wl = log((double)n1 * b[(size_t)i * n1 + k]);
                double wa = 0.0;
                if (brem_mass) {
                    double rm = brem_mass[(size_t)i * PUZZLE_SIDE + r];
                    if (rm > 0.0 && brem_slots[r] > 0)
                        wa = log(b[(size_t)i * n1 + k]
                                 * (double)brem_slots[r] / rm);
                }
                size_t fi = (size_t)bp[i] * NUM_PIECES + x;
                g_fw_location[fi] = g_freq_border_ok ? (float)wl : 0.0f;
                g_fw_segment[fi] = g_freq_border_ok ? (float)wa : 0.0f;
                g_fw_cell[fi] = g_freq_border_ok ? (float)wa : 0.0f;
            }
        free(brem_mass); free(b);
    }

    g_fw = (g_freq_model == FREQ_MODEL_CELL) ? g_fw_cell : g_fw_segment;
    return err;
}

/* -- Table file ------------------------------------------------------------- */

/* Line-oriented and greppable rather than binary or JSON, for the same reason
   E555_extract_consensus.py gives: being able to read one line of it with grep is
   worth more than loading it in one call. The file holds RAW counts, not the
   processed weights, so the estimator can change without re-learning. */
static uint64_t seed_hash(void)
{
    uint64_t h = 14695981039346656037ULL;
    for (int p = 0; p < NUM_PIECES; p++) {
        const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
        for (int d = 0; d < 4; d++) { h ^= (uint64_t)e[d]; h *= 1099511628211ULL; }
    }
    return h;
}

/* The border a table belongs to: every edge and corner piece with its spin in
   the canonical rotations row. That fixes each edge's side and the corners,
   which is all a rotations row decides. */
static uint64_t border_hash(void)
{
    uint64_t h = 14695981039346656037ULL;
    for (int p = 0; p < NUM_PIECES; p++) {
        if (piece_kind(p) < 1) continue;
        h ^= (uint64_t)p;        h *= 1099511628211ULL;
        h ^= (uint64_t)g_spin0[p]; h *= 1099511628211ULL;
    }
    return h;
}

/* Rotations row `want`, verbatim, and the comment line nearest above it (the
   Stage A "#  TOP=.. RIGHT=.. BOTTOM=.. LEFT=..  Score=.." line), so the log and
   the table can carry the border they were run on: if the rotations file is
   lost, the row can be rebuilt from either. Nothing is recomputed -- the trail
   counts and score are echoed as the file states them.

   Rows are counted exactly as read_one_border_row counts them: a line whose
   first non-blank character is '#' or '%' is a comment, a blank line is
   skipped, anything else is a data row. The comment is the last comment line
   after the previous data row, or NULL when there is none. Both strings are
   malloc'd without the line ending; the caller frees them. */
static bool read_border_row_text(const char *path, uint32_t want,
                                 char **row_out, char **comment_out)
{
    *row_out = NULL; *comment_out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open rotation CSV %s: %s", path, strerror(errno));
    char *line = NULL, *comment = NULL; size_t sz = 0; ssize_t len;
    uint32_t data_idx = 0; bool found = false;
    while ((len = getline(&line, &sz, f)) >= 0) {
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') continue;
        if (*s == '#' || *s == '%') { free(comment); comment = strdup(line); continue; }
        if (data_idx++ == want) {
            *row_out = strdup(line); *comment_out = comment; comment = NULL;
            found = true; break;
        }
        free(comment); comment = NULL;
    }
    free(comment); free(line); fclose(f);
    return found;
}

static void freq_write(const char *path, const char *seed_path, const char *rot_path,
                       const double *cnt, const double *wcell, const double *wsq)
{
    FILE *f = fopen(path, "w");
    if (!f) fatal("cannot write --learn table %s", path);
    fprintf(f, "# %s\n", FREQ_TABLE_MAGIC);
    fprintf(f, "# canonical frame: clue orientation 0, centre clue on (7,7) = --pin_clue 1\n");
    fprintf(f, "# rows are 0-indexed bottom-up; cell = row*16 + col\n");
    fprintf(f, "# rowcnt p r is the sum of cnt p x over free cells in row r\n");
    fprintf(f, "# segcnt p r s is the sum over free inner cells in 5-5-5 segment s\n");
    fprintf(f, "version %d\n", FREQ_TABLE_VERSION);
    fprintf(f, "seed %s %" PRIu64 "\n", seed_path, seed_hash());
    fprintf(f, "rotations %s %u\n", rot_path ? rot_path : "-", g_start_row);
    /* The rotations row itself, untouched (unturned: pass 0's border), and its
       Stage A comment when the file has one. Provenance only -- the reader
       checks the border by hash -- but it lets the rotations file be rebuilt:
         grep -E '^rotations_(comment|row) ' TABLE | cut -d' ' -f2- */
    if (rot_path) {
        char *row_txt, *cmt_txt;
        if (read_border_row_text(rot_path, g_start_row, &row_txt, &cmt_txt)) {
            if (cmt_txt) fprintf(f, "rotations_comment %s\n", cmt_txt);
            fprintf(f, "rotations_row %s\n", row_txt);
        }
        free(row_txt); free(cmt_txt);
    }
    fprintf(f, "border %" PRIu64 "\n", border_hash());
    fprintf(f, "clue_mask %u pin %d\n", g_clue_mask, g_pin_clue);
    fprintf(f, "stop_row %u beam_width %u threads %d rng_seed %" PRIu64 "\n",
            g_stop_row, g_beam_width, g_nthreads, g_master_seed);
    fprintf(f, "top_bottoms %ld top_columns %ld cap_per_config %d\n",
            g_top_bottoms, g_top_columns, LEARN_CAP_PER_CONFIG);
    /* pool and reps are provenance: 400 configurations drawn from 432 distinct
       bottoms is not the same evidence as 400 from 25920, and the file has to say
       which it was. Readers that only want the count still parse the first four
       fields. */
    for (int j = 0; j < FREQ_NPASS; j++)
        fprintf(f, "pass %d configs %zu pool %zu reps %zu\n",
                j, g_pconfigs[j], g_ppool[j], g_preps[j]);
    /* The free sets, so the file describes its own blocks. Coverage alone cannot
       recover them: a free cell no pass ever reached carries no count, yet it is
       still part of the square block the estimator balances, holding pure
       backoff. Without these lines a reader would balance a different, smaller
       matrix and get different weights. */
    for (int p = 0; p < NUM_PIECES; p++)
        if (g_piece_free[p]) fprintf(f, "freep %d %d %d\n", p, piece_kind(p),
                                     g_edge_side_of_piece[p]);
    for (int x = 0; x < NUM_PIECES; x++)
        if (g_cell_free[x]) fprintf(f, "freec %d\n", x);
    fprintf(f, "lift_kind segment_kt_prequential\n");
    fprintf(f, "lift %.6f %zu\n",
            g_lift_cells > 0.0 ? g_lift_sum / g_lift_cells : 0.0, g_lift_boards);
    for (int x = 0; x < NUM_PIECES; x++)
        if (wcell[x] > 0.0) fprintf(f, "wcell %d %.10g %.10g\n", x, wcell[x], wsq[x]);
    double *rowcnt = xmalloc(FREQ_RSZ * sizeof(double));
    double *segcnt = xmalloc(FREQ_SSZ * sizeof(double));
    freq_make_rowcnt(cnt, rowcnt);
    freq_make_segcnt(cnt, segcnt);
    for (int p = 0; p < NUM_PIECES; p++)
        for (int r = 0; r < PUZZLE_SIDE; r++) {
            double v = rowcnt[(size_t)p * PUZZLE_SIDE + r];
            if (v > 0.0) fprintf(f, "rowcnt %d %d %.10g\n", p, r, v);
        }
    for (int p = 0; p < NUM_PIECES; p++)
        for (int z = 0; z < FREQ_SEG_N; z++) {
            double v = segcnt[(size_t)p * FREQ_SEG_N + z];
            if (v > 0.0) fprintf(f, "segcnt %d %d %d %.10g\n",
                                 p, z / NUM_SEG, z % NUM_SEG, v);
        }
    free(rowcnt); free(segcnt);
    for (int p = 0; p < NUM_PIECES; p++)
        for (int x = 0; x < NUM_PIECES; x++) {
            double v = cnt[(size_t)p * NUM_PIECES + x];
            if (v > 0.0) fprintf(f, "cnt %d %d %.10g\n", p, x, v);
        }
    if (fclose(f)) fatal("error closing --learn table %s", path);
}

/* Read a table and build the weights. Seed, clue frame and border are checked
   against this run; any mismatch is fatal (the border one can be overridden). */
static void freq_load(const char *path, const char *seed_path, const char *rot_path)
{
    (void)seed_path;                         /* the hash is what is checked */
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot read --table %s", path);
    double *cnt     = xmalloc(FREQ_TSZ * sizeof(double));
    double *wcell   = xmalloc(NUM_PIECES * sizeof(double));
    double *wsq     = xmalloc(NUM_PIECES * sizeof(double));
    double *rowcnt  = xmalloc(FREQ_RSZ * sizeof(double));
    double *segcnt  = xmalloc(FREQ_SSZ * sizeof(double));
    memset(cnt, 0, FREQ_TSZ * sizeof(double));
    memset(wcell, 0, NUM_PIECES * sizeof(double));
    memset(wsq, 0, NUM_PIECES * sizeof(double));
    memset(rowcnt, 0, FREQ_RSZ * sizeof(double));
    memset(segcnt, 0, FREQ_SSZ * sizeof(double));

    /* getline, not a fixed buffer: a rotations_row line runs past 512 bytes, and
       fgets would hand its tail back as a line of its own. */
    char *line = NULL; size_t line_sz = 0;
    bool seen_version = false, seen_rowcnt = false, seen_segcnt = false;
    uint64_t want_seed = 0, want_border = 0; bool seen_border = false;
    unsigned rot_row = 0, clue_mask = 0; int pin = 0;
    int tab_side[NUM_PIECES];
    for (int k = 0; k < NUM_PIECES; k++) tab_side[k] = -2;   /* -2 = no freep line */
    char rot_name[256] = "-", seed_name[256] = "-", lift_kind[64] = "";
    double lift = 0.0; size_t lift_n = 0;
    while (getline(&line, &line_sz, f) >= 0) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int p, x, j, r, sg; double v, w; size_t n;
        if (!strncmp(line, "version ", 8)) {
            int version = atoi(line + 8);
            if (version != FREQ_TABLE_VERSION)
                fatal("--table %s: version %d, this build reads %d -- "
                      "re-run the learning phase", path, version, FREQ_TABLE_VERSION);
            seen_version = true;
            continue;
        }
        if (sscanf(line, "seed %255s %" SCNu64, seed_name, &want_seed) == 2) continue;
        if (sscanf(line, "rotations %255s %u", rot_name, &rot_row) == 2) continue;
        if (sscanf(line, "border %" SCNu64, &want_border) == 1) {
            seen_border = true; continue;
        }
        if (sscanf(line, "freep %d %d %d", &p, &x, &j) == 3) {
            if (p >= 0 && p < NUM_PIECES && x == 1) tab_side[p] = j;
            continue;
        }
        if (sscanf(line, "clue_mask %u pin %d", &clue_mask, &pin) == 2) continue;
        if (sscanf(line, "lift_kind %63s", lift_kind) == 1) continue;
        if (sscanf(line, "lift %lf %zu", &lift, &lift_n) == 2) continue;
        if (sscanf(line, "wcell %d %lf %lf", &x, &v, &w) == 3) {
            if (x >= 0 && x < NUM_PIECES) { wcell[x] = v; wsq[x] = w; }
            continue;
        }
        if (sscanf(line, "rowcnt %d %d %lf", &p, &r, &v) == 3) {
            if (p >= 0 && p < NUM_PIECES && r >= 0 && r < PUZZLE_SIDE) {
                rowcnt[(size_t)p * PUZZLE_SIDE + r] = v;
                seen_rowcnt = true;
            }
            continue;
        }
        if (sscanf(line, "segcnt %d %d %d %lf", &p, &r, &sg, &v) == 4) {
            if (p >= 0 && p < NUM_PIECES && r >= 0 && r < PUZZLE_SIDE
                && sg >= 0 && sg < NUM_SEG) {
                segcnt[(size_t)p * FREQ_SEG_N + r * NUM_SEG + sg] = v;
                seen_segcnt = true;
            }
            continue;
        }
        if (sscanf(line, "cnt %d %d %lf", &p, &x, &v) == 3) {
            if (p >= 0 && p < NUM_PIECES && x >= 0 && x < NUM_PIECES)
                cnt[(size_t)p * NUM_PIECES + x] = v;
            continue;
        }
        if (sscanf(line, "pass %d configs %zu", &j, &n) == 2) {
            if (j >= 0 && j < FREQ_NPASS) g_pconfigs[j] = n;
        }
    }
    free(line); fclose(f);
    if (!seen_version) fatal("--table %s: not an E555 datadriven table", path);
    if (strcmp(lift_kind, "segment_kt_prequential"))
        fatal("--table %s: lift_kind is '%s', expected segment_kt_prequential",
              path, *lift_kind ? lift_kind : "(missing)");
    if (want_seed != seed_hash())
        fatal("--table %s was learned on a different seed (%s) -- refusing", path, seed_name);
    if (clue_mask != g_clue_mask)
        fatal("--table %s was learned with clue_mask %u, this run has %u -- refusing",
              path, clue_mask, g_clue_mask);
    if (pin != g_pin_clue)
        fatal("--table %s was learned at --pin_clue %d, this run has %d -- refusing",
              path, pin, g_pin_clue);
    /* The border is compared by content, not by file name: the table's own
       border hash when it has one, otherwise the side it records for every free
       edge piece. The interior counts depend on the colours the border turns
       inward, so a different border is refused; E555_FREQ_ANY_BORDER=1 accepts
       it deliberately, steering the interior and dropping the border block. */
    bool border_same = true;
    if (seen_border) {
        border_same = (want_border == border_hash());
    } else {
        for (int k = 0; k < NUM_PIECES; k++) {
            bool free_edge = g_piece_free[k] && piece_kind(k) == 1;
            if (free_edge ? tab_side[k] != g_edge_side_of_piece[k] : tab_side[k] != -2)
                { border_same = false; break; }
        }
    }
    if (!border_same) {
        if (!getenv("E555_FREQ_ANY_BORDER"))
            fatal("--table %s was learned on a different border (%s row %u) than "
                  "%s row %u -- refusing (E555_FREQ_ANY_BORDER=1 overrides)",
                  path, rot_name, rot_row, rot_path ? rot_path : "-", g_start_row);
        g_freq_border_ok = false;
        fprintf(stderr, "[freq] E555_FREQ_ANY_BORDER: table border differs; border "
                "block dropped, interior table still steers\n");
    }

    if (getenv("E555_FREQ_NOBORDER")) g_freq_border_ok = false;

    /* rowcnt and segcnt are redundant with cnt; a disagreement means a
       damaged or hand-edited file. */
    if (!seen_rowcnt || !seen_segcnt)
        fatal("--table %s: missing rowcnt/segcnt statistics", path);
    double *derived_rowcnt = xmalloc(FREQ_RSZ * sizeof(double));
    double *derived_segcnt = xmalloc(FREQ_SSZ * sizeof(double));
    freq_make_rowcnt(cnt, derived_rowcnt);
    freq_make_segcnt(cnt, derived_segcnt);
    for (size_t i = 0; i < FREQ_RSZ; i++)
        if (fabs(rowcnt[i] - derived_rowcnt[i]) > 1e-7 * (1.0 + fabs(derived_rowcnt[i])))
            fatal("--table %s: rowcnt disagrees with cnt at piece %zu row %zu",
                  path, i / PUZZLE_SIDE, i % PUZZLE_SIDE);
    for (size_t i = 0; i < FREQ_SSZ; i++)
        if (fabs(segcnt[i] - derived_segcnt[i]) > 1e-7 * (1.0 + fabs(derived_segcnt[i])))
            fatal("--table %s: segcnt disagrees with cnt at piece %zu row %zu segment %zu",
                  path, i / FREQ_SEG_N, (i % FREQ_SEG_N) / NUM_SEG, i % NUM_SEG);
    free(derived_rowcnt); free(derived_segcnt);
    double err = freq_build(cnt, wcell, segcnt);
    /* E555_FREQ_DUMP=PATH writes the processed weights out so freq_view.py
       --check can compare them against its own, independent implementation of
       this estimator. Two implementations of one formula disagreeing is the
       cheapest way to learn that one of them is wrong. Env-gated rather than a
       flag: it is a debugging tap, not part of the interface. */
    const char *dump = getenv("E555_FREQ_DUMP");
    if (dump) {
        FILE *df = fopen(dump, "w");
        if (!df) fatal("E555_FREQ_DUMP: cannot write %s", dump);
        fprintf(df, "# beam_model %s-preservation border_model location "
                    "smoothing KT-1/2\n",
                    g_freq_model == FREQ_MODEL_CELL ? "cell" : "segment");
        for (int pp = 0; pp < NUM_PIECES; pp++)
            for (int xx = 0; xx < NUM_PIECES; xx++) {
                size_t fi = (size_t)pp * NUM_PIECES + xx;
                if (g_fw[fi] != 0.0f)
                    fprintf(df, "fw %d %d %.7g\n", pp, xx, (double)g_fw[fi]);
                if (g_fw_segment[fi] != 0.0f)
                    fprintf(df, "fwseg %d %d %.7g\n", pp, xx, (double)g_fw_segment[fi]);
                if (g_fw_cell[fi] != 0.0f)
                    fprintf(df, "fwcell %d %d %.7g\n", pp, xx, (double)g_fw_cell[fi]);
                if (piece_kind(pp) == 1 && g_fw_location[fi] != 0.0f)
                    fprintf(df, "fwb %d %d %.7g\n", pp, xx,
                            (double)g_fw_location[fi]);
            }
        if (fclose(df)) fatal("E555_FREQ_DUMP: error closing %s", dump);
        printf("[freq] weights dumped to %s\n", dump);
    }
    size_t covered = 0; double ess = 0.0;
    for (int x = 0; x < NUM_PIECES; x++)
        if (wcell[x] > 0.0) { covered++; ess += wcell[x] * wcell[x] / wsq[x]; }
    printf("[freq] table %s: beam_model=%s-preservation "
           "border_model=location smoothing=KT-1/2 cells=%zu mean_ess=%.1f "
           "balance_err=%.2g segment_lift=%.4f nats/cell over %zu boards%s\n",
           path, g_freq_model == FREQ_MODEL_CELL ? "cell" : "segment",
           covered, covered ? ess / covered : 0.0, err, lift, lift_n,
           g_freq_border_ok ? "" : " (border block off)");
    free(cnt); free(wcell); free(wsq); free(rowcnt); free(segcnt);
    g_freq_on = true;
}

/* -- Scoring ---------------------------------------------------------------- */

/* One committed row's learned weight: future-conditioned segment or exact-cell
   scores for the 14 inner pieces plus vertical-side scores for the two edge pieces. The bottom
   row carries zero active weight (its exact location score already selected the
   border), so every decision-relevant placed piece contributes once. */
static inline double freq_row_term(const RowChoice *mv, int row)
{
    const int base = row * PUZZLE_SIDE;
    double s = g_fw[(size_t)g_cur_left->p[row]->piece_id * NUM_PIECES + base];
    for (int i = 0; i < EDGE_LEN; i++)
        s += g_fw[(size_t)g_cat[mv->ci[i]].piece_id * NUM_PIECES + base + 1 + i];
    s += g_fw[(size_t)g_edge_term[mv->rterm].piece_id * NUM_PIECES + base + PUZZLE_SIDE - 1];
    return s;
}

static inline double freq_bottom_term(const BottomOrder *bot, const float *weights)
{
    double s = 0.0;
    for (int c = 0; c < PUZZLE_SIDE; c++)
        s += weights[(size_t)bot->p[c]->piece_id * NUM_PIECES + c];
    return s;
}

/* The beam accumulator uses segment preservation. Border enumeration does not:
   it always uses exact location, because two border orderings contain the same
   pieces and differ only by their cells. */
static inline double freq_beam_bottom_term(const BottomOrder *bot)
{
    return freq_bottom_term(bot, g_fw);
}

static inline double freq_location_bottom_term(const BottomOrder *bot)
{
    return freq_bottom_term(bot, g_fw_location);
}

/* The whole board, recomputed from every placed cell. Only E555_FREQ_DEBUG uses
   it: the accumulated score must equal this, or the claim that the beam ranks on
   whole-board fit is not true. */
static double freq_board_term(const RowChoice rows[EDGE_LEN], int row)
{
    double s = 0.0;
    for (int r = 0; r <= row; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint16_t pid; uint8_t rot;
            board_cell(rows, r, c, &pid, &rot);
            s += g_fw[(size_t)pid * NUM_PIECES + r * PUZZLE_SIDE + c];
        }
    return s;
}

/* -- Data-driven border ranking --------------------------------------------- */

/* bottom_rank_of and left_rank_of are static inside E555_database.c, so the two
   measures are rebuilt here from the same public primitives and the frequency
   term is added to them. Both are in nats, and the spread of the fan-out measure
   (order 10) and of the frequency term (order 4 over 16 cells) are comparable,
   so they add without a weight to tune.
 *
 * The perturbation is gumbel_key, exactly as the library uses it, so
 * --tau_bottoms and --tau_columns keep their meaning precisely: taking the first
 * --top_bottoms entries draws that many DISTINCT orderings in proportion to
 * exp(rank/tau). With the frequency term inside the rank, that is the border
 * being sampled by the learned frequencies, within the input rotations set.
 *
 * E555_database.c:1348 makes the case for why a positional term belongs here:
 * every colour-multiset functional is CONSTANT across a border row's columns, so
 * only positional information can tell them apart. Border ranking therefore
 * ALWAYS uses g_fw_location. The beam's A/B/C statistic intentionally removes
 * within-segment noise; the border is an ordering decision and needs the full
 * piece-by-cell resolution. */
static double freq_bottom_rank(const BottomOrder *b)
{
    const int *rt = b->rtop0;
    double s = log1p((double)db_seg_fanout(rt[1], rt[2], rt[3], rt[4], rt[5]))
             + log1p((double)db_seg_fanout(rt[6], rt[7], rt[8], rt[9], rt[10]))
             + log1p((double)db_seg_fanout(rt[11], rt[12], rt[13], rt[14], rt[15]));
    return s + freq_location_bottom_term(b);
}

/* A column the bottom cannot start row 1 with: -inf sorts last at any tau, since
   -inf/tau is still -inf and finite Gumbel noise leaves it there. */
#define FREQ_LEFT_DEAD (-HUGE_VAL)

static double freq_left_rank(const LeftOrder *l, const BottomOrder *bot)
{
    const int *rt = bot->rtop0;
    uint64_t joint = db_seg_count(l->right[1], rt[1], rt[2], rt[3], rt[4], rt[5]);
    if (joint == 0) return FREQ_LEFT_DEAD;
    double s = log1p((double)joint);
    for (int r = 1; r + CHAIN_LEN - 1 <= EDGE_LEN; r++)
        s += left_window_logfanout(l->right, r);
    for (int r = 1; r <= EDGE_LEN; r++)
        s += g_fw_location[(size_t)l->p[r]->piece_id * NUM_PIECES
                           + r * PUZZLE_SIDE];
    return s;
}

static int cmp_rank_asc(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int freq_cmp_bottom(const void *a, const void *b)
{
    const BottomOrder *x = a, *y = b;
    if (x->rank != y->rank) return (x->rank < y->rank) ? 1 : -1;
    return memcmp(x->rtop0, y->rtop0, sizeof x->rtop0);
}

static int freq_cmp_left(const void *a, const void *b)
{
    const LeftOrder *x = a, *y = b;
    if (x->rank != y->rank) return (x->rank < y->rank) ? 1 : -1;
    return memcmp(x->right, y->right, sizeof x->right);
}

static void freq_rank_bottoms(double tau, RNG *rng)
{
    for (size_t i = 0; i < g_bottom_n; i++)
        g_bottoms[i].rank = gumbel_key(freq_bottom_rank(&g_bottoms[i]), tau, rng);
    qsort(g_bottoms, g_bottom_n, sizeof(BottomOrder), freq_cmp_bottom);
}

static size_t freq_rank_lefts(const BottomOrder *bot, double tau, RNG *rng, size_t *distinct)
{
    size_t viable = 0;
    double *raw = distinct ? xmalloc(g_left_n * sizeof(double)) : NULL;
    for (size_t i = 0; i < g_left_n; i++) {
        double r = freq_left_rank(&g_lefts[i], bot);
        if (r != FREQ_LEFT_DEAD) viable++;
        if (raw) raw[i] = r;
        g_lefts[i].rank = gumbel_key(r, tau, rng);
    }
    if (raw) {
        /* Sort and count runs. The obvious nested scan is quadratic, and
           g_left_n reaches 46080 on a trail-rich side, which is 1.1e9 compares
           per bottom row for a number that only appears in a progress line. */
        qsort(raw, g_left_n, sizeof(double), cmp_rank_asc);
        size_t d = g_left_n ? 1 : 0;
        for (size_t i = 1; i < g_left_n; i++) if (raw[i] != raw[i-1]) d++;
        *distinct = d;
        free(raw);
    }
    qsort(g_lefts, g_left_n, sizeof(LeftOrder), freq_cmp_left);
    return viable;
}

/* ====================== end data-driven frequency table ==================== */

/* Score a child whose one-row lookahead already passed, from the three counts
   the lookahead returned. The only formula in the program: every caller reaches
   the score through here.

       log(nA) + log1p(fB) + log1p(fC) = log(nA * (1+fB) * (1+fC))

   is an identity in R, so the product form is the same objective with one log
   instead of three. All three factors are positive integers -- nA >= 1 because a
   cell exists only if it holds records, and both fan-outs are non-zero by the
   gate -- so the product is >= 4, and even the absurd upper bound of the whole
   database squared is 9.2e21, nowhere near overflowing a double.

   The two forms differ by a few ulp: measured over 400 k triples drawn from the
   live ranges, at most 7.1e-15 absolute (2.8e-16 relative), and all 400 k round
   to the SAME float -- which is what the score is stored as. So this is not a
   numerical change in any sense the search can see; it is only formally not
   bit-identical, since a double difference of 1e-15 can in principle land the
   sum on the far side of a float rounding boundary. */
static inline float score_scanned(const BeamEntry *t, int row,
                                  uint32_t nA, uint64_t fB, uint64_t fC,
                                  const RowChoice *mv, float parent_learned) {
    /* BeamEntry.score is the learned accumulator and nothing else. The stock
       objective remains row-local, as it is without a table. This isolates the
       experiment: loading a zero learned table cannot turn the stock heuristic
       into a cumulative path score. */
    double learned_inc = 0.0, learned = 0.0;
    if (g_freq_on) {
        learned_inc = freq_row_term(mv, row);
        learned = (double)parent_learned + learned_inc;
        if (g_freq_pure) {
            score_diag_acc(row, 0.0, 0.0, 0.0, learned_inc, learned);
            return (float)learned;
        }
    }
    double closure = 0.0, maha = 0.0;
    double fanout = log((double)nA * (1.0 + (double)fB) * (1.0 + (double)fC));
    double colors = color_term_parts(t, row, &closure, &maha);
    score_diag_acc(row, fanout, closure, maha, learned_inc, learned);
    return (float)(fanout + colors + learned);
}

/* The stop row drops the fan-out lookahead (whether a row fits above is the next
   stage's problem). Guided search still adds the complete learned accumulator,
   but only the CURRENT row's colour term. */
static inline float score_stop(const BeamEntry *t, const BeamEntry *parent,
                               int row, const RowChoice *mv) {
    double learned_inc = g_freq_on ? freq_row_term(mv, row) : 0.0;
    double learned = g_freq_on ? (double)parent->score + learned_inc : 0.0;
    if (g_freq_on && g_freq_pure) {
        score_diag_acc(row, 0.0, 0.0, 0.0, learned_inc, learned);
        return (float)learned;
    }
    double closure = 0.0, maha = 0.0;
    double colors = color_term_parts(t, row, &closure, &maha);
    score_diag_acc(row, 0.0, closure, maha, learned_inc, learned);
    return (float)(learned + colors);
}

/* Full scoring for a child that already exists: the same gate as
   child_lookahead, read off the materialized frontier instead of the chunks. */
static bool score_child(const BeamEntry *t, int row, float *out,
                        const RowChoice *mv, float parent_acc) {
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

    *out = score_scanned(t, row, cA->n, fB, fC, mv, parent_acc);
    return true;
}

/* score_child's lookahead, run BEFORE the child exists.
 *
 * The tops a row exposes upward follow from the chosen chunks alone -- commit_row
 * sets new_top[1+i] = g_cat[ci[i]].top and new_top[15] = term->top, and reads
 * nothing else from the parent -- so every one of score_child's early returns can
 * be decided without materializing anything. That is worth hoisting: the gate is
 * an exact one-row death test, and a candidate dying on it used to pay a whole
 * BeamEntry copy, commit_row's counter updates and a parity scan first.
 *
 * The three counts the score needs come back with it, so the caller does not
 * repeat the lookups -- one g_db probe into a 261 MB pointer array plus two
 * g_fanout probes, which are the expensive part of scoring a child. */
static inline bool child_lookahead(const uint16_t ci[EDGE_LEN], uint8_t rterm, int row,
                                   uint32_t *nA_out, uint64_t *fB_out, uint64_t *fC_out) {
    uint8_t tp[PUZZLE_SIDE];
    tp[0] = 0;                                   /* col 0 is the border; unread here */
    for (int i = 0; i < EDGE_LEN; i++) tp[1+i] = g_cat[ci[i]].top;
    tp[15] = g_edge_term[rterm].top;

    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(tp[c])) return false;
    if (!color_is_edge_iface(tp[15])) return false;

    int la = g_cur_left->right[row + 1];
    if (!color_is_inner(la)) return false;
    const Cell *cA = g_db[INNER_IDX(la)][INNER_IDX(tp[1])][INNER_IDX(tp[2])]
                         [INNER_IDX(tp[3])][INNER_IDX(tp[4])][tp[5]];
    if (!cA) return false;
    uint64_t fB = db_seg_fanout(tp[6], tp[7], tp[8], tp[9], tp[10]);
    if (fB == 0) return false;
    uint64_t fC = db_seg_fanout(tp[11], tp[12], tp[13], tp[14], tp[15]);
    if (fC == 0) return false;

    *nA_out = cA->n; *fB_out = fB; *fC_out = fC;
    return true;
}

static inline uint64_t frontier_sig(const BeamEntry *b) {
    uint64_t h = 0x6BEA6BEA6BEA6BEAULL, w;
    /* Orientation joins the hash, or two boards that differ only in which clue
       set they owe would dedup into one and the loser's lineage would vanish
       silently. Guarded so the no-clue signature is unchanged. */
    if (g_clue_mask) h = splitmix64(h ^ (uint64_t)b->flags);
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
    if (!e->cA) { reject_count(row, REJ_A_CELL); return false; }
    return true;
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
                                   uint32_t parent_idx, float score, const RowChoice *mv,
                                   uint8_t flags) {
    PoolEntry *pe = &sc->buf[sc->buf_n++];
    pe->score = score; pe->parent = parent_idx; pe->sig = sig; pe->mv = *mv;
    pe->flags = flags;
    if (sc->buf_n == POOL_BATCH) pool_flush(ctx, sc);
}
static inline void pool_append(BeamCtx *ctx, Scratch *sc, const BeamEntry *t,
                               uint32_t parent_idx, float score, const RowChoice *mv) {
    pool_append_sig(ctx, sc, frontier_sig(t), parent_idx, score, mv, t->flags);
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

/* Exact startup feasibility for the two lower corner clues.  The row-1 pins are
   TOPCOLOR constraints in segment A (col 2) and C (col 13).  This walk uses the
   same complete database cells and exact used masks as the beam, but stops at the
   first full A+B+C row.  It therefore has no false negatives from quotas, promise
   ordering, or CLUE_SEG_CAP. */
static inline bool chain_pin_ok(const uint16_t ci[], int count, int pin_idx,
                                int pin_kind, uint16_t pin_val) {
    if (pin_idx < 0) return true;
    if (pin_idx >= count) return false;
    if (pin_kind == PIN_PIECE) return ci[pin_idx] == pin_val;
    return g_cat[ci[pin_idx]].top == pin_val;
}

static void border_used_mask(const BottomOrder *bot, const LeftOrder *lft,
                             uint64_t used[4]) {
    for (int k = 0; k < 4; k++) used[k] = bot->used[k] | lft->used[k];
    used_set(used, g_cTR.piece_id);
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        for (int o = 0; o < 4; o++)
            if (g_clue_orients & (1u << o)) used_set(used, g_clue[o][k].piece);
    }
}

static bool row1_corner_compatible(const BottomOrder *bot, const LeftOrder *lft) {
    if (!(g_clue_mask & CLUE_CORNERS)) return true;

    const int la_A = lft->right[1];
    const int *rt = bot->rtop0;
    uint8_t bt[PUZZLE_SIDE];
    for (int c = 0; c < PUZZLE_SIDE; c++) bt[c] = (uint8_t)rt[c];
    if (!color_is_inner(la_A)) return false;
    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(rt[c])) return false;
    if (!color_is_edge_iface(rt[15])) return false;

    const Cell *cA = g_db[INNER_IDX(la_A)][INNER_IDX(rt[1])][INNER_IDX(rt[2])]
                         [INNER_IDX(rt[3])][INNER_IDX(rt[4])][rt[5]];
    if (!cA) return false;

    uint64_t used0[4];
    border_used_mask(bot, lft, used0);

    for (int o = 0; o < 4; o++) {
        if (!(g_clue_orients & (1u << o))) continue;
        int pin_idx[3], pin_kind[3]; uint16_t pin_val[3];
        if (!clue_pins_for(1, o, pin_idx, pin_kind, pin_val)) continue;

        for (uint32_t ja = 0; ja < cA->n; ja++) {
            uint32_t wa = rec_load(cA->rec, ja, g_rec_bytes_inner);
            uint8_t fa[CHAIN_LEN]; unpack_inner(wa, fa, g_lb_bits);
            uint16_t A[CHAIN_LEN]; uint64_t mA[4] = {0,0,0,0};
            if (!decode_inner_chain(fa, CHAIN_LEN, la_A, bt + 1,
                                    A, mA, used0)) continue;
            if (masks_intersect4(used0, mA)
                || !chain_pin_ok(A, CHAIN_LEN, pin_idx[0], pin_kind[0], pin_val[0]))
                continue;

            uint64_t fA[4] = { used0[0]|mA[0], used0[1]|mA[1],
                               used0[2]|mA[2], used0[3]|mA[3] };
            int la_B = g_cat[A[CHAIN_LEN-1]].right;
            if (!color_is_inner(la_B)) continue;
            const Cell *cB = g_db[INNER_IDX(la_B)][INNER_IDX(rt[6])][INNER_IDX(rt[7])]
                                 [INNER_IDX(rt[8])][INNER_IDX(rt[9])][rt[10]];
            if (!cB) continue;

            for (uint32_t jb = 0; jb < cB->n; jb++) {
                uint16_t B[CHAIN_LEN]; int la_C;
                if (!pick_segB(cB, jb, bt + 6, fA, B, la_B, &la_C))
                    continue;
                if (!chain_pin_ok(B, CHAIN_LEN, pin_idx[1], pin_kind[1], pin_val[1])
                    || !color_is_inner(la_C)) continue;

                uint64_t mB[4], fB[4];
                mask_of_chain(B, CHAIN_LEN, mB);
                for (int k = 0; k < 4; k++) fB[k] = fA[k] | mB[k];
                const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                     [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
                if (!cC) continue;

                for (uint32_t jc = 0; jc < cC->n; jc++) {
                    uint16_t C[CHAIN_LEN-1]; uint8_t rterm;
                    if (!pick_segC(cC, jc, bt + 11, fB,
                                   C, la_C, &rterm)) continue;
                    if (!chain_pin_ok(C, CHAIN_LEN-1, pin_idx[2],
                                      pin_kind[2], pin_val[2])) continue;
                    (void)rterm;
                    return true;
                }
            }
        }
    }
    return false;
}

static void corner_compat_cache_init(size_t nb) {
    free(g_corner_compat_cache);
    g_corner_compat_cache = NULL;
    g_corner_compat_nb = nb;
    if (!nb || !(g_clue_mask & CLUE_CORNERS)) return;
    if (g_left_n) {
        for (size_t i = 1; i < g_left_n; i++)
            if (memcmp(g_lefts[i].used, g_lefts[0].used,
                       sizeof g_lefts[i].used) != 0)
                fatal("corner-clue cache assumes fixed-mode left orders are "
                      "permutations of one piece set");
    }
    if (nb > SIZE_MAX / NUM_COLORS_TOTAL)
        fatal("corner-clue compatibility cache size overflow");
    g_corner_compat_cache = xmalloc(nb * NUM_COLORS_TOTAL);
    memset(g_corner_compat_cache, -1, nb * NUM_COLORS_TOTAL);
}

static bool row1_corner_compatible_cached(size_t b_idx, const BottomOrder *bot,
                                          const LeftOrder *lft) {
    if (!g_corner_compat_cache) return true;
    if (b_idx >= g_corner_compat_nb) fatal("internal: corner cache bottom index");
    int la = lft->right[1];
    if (la < 0 || la >= NUM_COLORS_TOTAL) return false;
    int8_t *slot = &g_corner_compat_cache[b_idx * NUM_COLORS_TOTAL + (size_t)la];
    if (*slot < 0) *slot = row1_corner_compatible(bot, lft) ? 1 : 0;
    return *slot != 0;
}

/* g_lefts is already sorted by its ordinary/database+table rank.  Stable
   compaction retains that order while dropping columns that cannot satisfy both
   row-1 corner-clue colors.  A bottom with no survivors is thereby eliminated
   before a beam workspace is touched. */
static size_t clue_filter_ranked_lefts(size_t b_idx, const BottomOrder *bot,
                                       size_t ordinary_viable) {
    g_border_stats.bottoms_ranked++;
    g_border_stats.columns_ordinary_viable += ordinary_viable;
    if (!(g_clue_mask & CLUE_CORNERS)) {
        g_border_stats.columns_clue_compatible += ordinary_viable;
        return ordinary_viable;
    }

    if (g_left_filter_cap < g_left_n) {
        g_left_filter_buf = xrealloc(g_left_filter_buf,
                                     g_left_n * sizeof(*g_left_filter_buf));
        g_left_filter_cap = g_left_n;
    }

    size_t out = 0;
    /* Compatible ordinary-viable entries go first.  Rejected viable entries are
       retained after them, and the ordinary-dead suffix follows unchanged.  The
       next bottom therefore still ranks the complete, uncorrupted left-order set. */
    for (size_t i = 0; i < ordinary_viable; i++)
        if (row1_corner_compatible_cached(b_idx, bot, &g_lefts[i]))
            g_left_filter_buf[out++] = g_lefts[i];
    size_t tail = out;
    for (size_t i = 0; i < ordinary_viable; i++)
        if (!row1_corner_compatible_cached(b_idx, bot, &g_lefts[i]))
            g_left_filter_buf[tail++] = g_lefts[i];
    for (size_t i = ordinary_viable; i < g_left_n; i++)
        g_left_filter_buf[tail++] = g_lefts[i];
    if (tail != g_left_n) fatal("internal: clue column partition lost entries");
    memcpy(g_lefts, g_left_filter_buf, g_left_n * sizeof(*g_lefts));

    g_border_stats.columns_clue_compatible += out;
    if (!out) g_border_stats.bottoms_no_clue_column++;
    return out;
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

/* B+C and B-only partials (--incomplete_top): segment A is missing, so B has
   lost its left key and every inner colour is a candidate; C chains off B as
   usual. Driven per PARENT rather than per A record, so it also covers parents
   whose segment-A cell is empty or wholly conflicted. */
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
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            emit_incomplete(ctx, p, &mv, row, PARTMASK_B);
            const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                 [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
            if (!cC) continue;
            uint64_t maskB[4], forbidB[4];
            mask_of_chain(ciB, CHAIN_LEN, maskB);
            for (int k = 0; k < 4; k++) forbidB[k] = p->used[k] | maskB[k];
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

/* One (parent, orientation) expansion of a row that carries clues. Every segment
   goes through the pinned walk -- the same walk that provably reproduces a
   database cell when unpinned -- so pinned and free segments share one path and
   a pinned segment A does not need the parent's A cell to exist at all (no
   stored chain holds a clue piece). Clue rows are few and heavily constrained,
   so giving up the cell's fan-out promise ordering here costs little.

   Every loop level spends budget, not just the outermost: quota only falls on
   ACCEPTED children, so a parent whose candidates all fail parity or the
   lookahead would otherwise walk the full nA x nB x nC x terminals product --
   up to 10^9 with a free segment at the 1024 cap. */
static void expand_clued(BeamCtx *ctx, const BeamEntry *p, uint32_t pi, int row,
                         bool at_stop, Scratch *sc, uint32_t quota, uint64_t budget,
                         int orient, const int pin_idx[3], const int pin_kind[3],
                         const uint16_t pin_val[3]) {
    const uint8_t *rt = p->rtop;
    int la_A = g_cur_left->right[row];
    if (!color_is_inner(la_A)) return;
    for (int c = 1; c <= EDGE_LEN; c++) if (!color_is_inner(rt[c])) return;
    if (!color_is_edge_iface(rt[15])) return;

    BeamEntry *t = &sc->tmp;
    RowChoice mv;
    const uint8_t oflags = (uint8_t)(orient | FLAG_ORIENT_SET);

    int nA = enumerate_pinned_segment(la_A, rt + 1, CHAIN_LEN, pin_idx[0], pin_kind[0], pin_val[0],
                                      p->used, sc->seg[0], CLUE_SEG_CAP);
    if (g_clue_debug) {
        #pragma omp atomic
        g_dbg_nA[row] += nA;
        #pragma omp atomic
        g_dbg_calls[row] += 1;
    }
    for (int ia = 0; ia < nA && quota > 0 && budget > 0; ia++) {
        const uint16_t *A = sc->seg[0][ia];
        uint64_t mA[4], fA[4];
        mask_of_chain(A, CHAIN_LEN, mA);
        for (int k = 0; k < 4; k++) fA[k] = p->used[k] | mA[k];
        int la_B = g_cat[A[CHAIN_LEN-1]].right;
        if (!color_is_inner(la_B)) continue;
        budget--;

        int nB = enumerate_pinned_segment(la_B, rt + 6, CHAIN_LEN, pin_idx[1], pin_kind[1], pin_val[1],
                                          fA, sc->seg[1], CLUE_SEG_CAP);
        if (g_clue_debug) { 
            #pragma omp atomic
            g_dbg_nB[row] += nB; }
        for (int ib = 0; ib < nB && quota > 0 && budget > 0; ib++) {
            budget--;
            const uint16_t *B = sc->seg[1][ib];
            uint64_t mB[4], fB[4];
            mask_of_chain(B, CHAIN_LEN, mB);
            for (int k = 0; k < 4; k++) fB[k] = fA[k] | mB[k];
            int la_C = g_cat[B[CHAIN_LEN-1]].right;
            if (!color_is_inner(la_C)) continue;

            int nC = enumerate_pinned_segment(la_C, rt + 11, CHAIN_LEN-1, pin_idx[2], pin_kind[2], pin_val[2],
                                              fB, sc->seg[2], CLUE_SEG_CAP);
            if (g_clue_debug) {
                #pragma omp atomic
                g_dbg_nC[row] += nC;
            }
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
                    t->flags = oflags;          /* before pool_append: it hashes flags */
                    if (!parity_ok(t)) {
                        reject_count(row, REJ_PARITY); continue;
                    }
                    float score;
                    if (at_stop) {
                        score = score_stop(t, p, row, &mv);
                    } else if (!score_child(t, row, &score, &mv, p->score)) {
                        reject_count(row, REJ_LOOKAHEAD);
                        continue;
                    }
                    pool_append(ctx, sc, t, pi, score, &mv);
                    quota--;
                }
            }
        }
    }
}

/* Clue handling for one parent on a row that carries clues; returns whether the
   parent may ALSO expand normally.

   A board is unassigned until it places its first clue, then owes that
   orientation's remaining clues for life. An unassigned board may defer only
   while some enabled orientation still has its first clue ahead of this row;
   past that point it can never satisfy any orientation, so it stops producing
   children and falls out of the beam. */
static bool expand_clue_row(BeamCtx *ctx, const BeamEntry *p, uint32_t pi, int row,
                            bool at_stop, Scratch *sc, uint32_t quota, uint64_t budget) {
    int pin_idx[3], pin_kind[3]; uint16_t pin_val[3];
    if (ENTRY_HAS_ORIENT(p)) {
        int o = ENTRY_ORIENT(p);
        if (!clue_pins_for(row, o, pin_idx, pin_kind, pin_val)) return true;   /* nothing due */
        expand_clued(ctx, p, pi, row, at_stop, sc, quota, budget, o, pin_idx, pin_kind, pin_val);
        return false;
    }
    for (int o = 0; o < 4; o++) {
        if (!(g_clue_orients & (1u << o))) continue;
        if (!clue_pins_for(row, o, pin_idx, pin_kind, pin_val)) continue;
        expand_clued(ctx, p, pi, row, at_stop, sc, quota, budget, o, pin_idx, pin_kind, pin_val);
    }
    return row < g_clue_last_assign;
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
    if (!decode_inner_chain(fA, CHAIN_LEN, e->la_A, rt + 1, ciA, maskA, p->used)) {
        reject_count(e->row, REJ_A_DECODE); return;
    }  /* bottoms rt[1..5] */
    if (masks_intersect4(p->used, maskA)) {
        reject_count(e->row, REJ_A_DECODE); return;
    }

    e->budget--;                                   /* a real decode attempt */

    int la_B = g_cat[ciA[CHAIN_LEN-1]].right;
    const Cell *cB = color_is_inner(la_B) ? segB_cell(p, la_B) : NULL;
    /* No B cell at all. Below the stop row that kills the child; at the stop row
       a missing B is exactly what an A+C partial records, so fall through. */
    if (!cB && !(e->at_stop && g_incomplete_top)) {
        reject_count(e->row, REJ_B_CELL);
        return;
    }

    uint64_t forbidA[4] = { p->used[0]|maskA[0], p->used[1]|maskA[1],
                            p->used[2]|maskA[2], p->used[3]|maskA[3] };
    BeamEntry *t = &sc->tmp;
    RowChoice mv;
    memcpy(&mv.ci[0], ciA, CHAIN_LEN * sizeof(uint16_t));

    if (e->at_stop) {
        for (uint32_t jb = 0; cB && jb < cB->n && e->quota > 0; jb++) {
            uint16_t ciB[CHAIN_LEN]; int la_C;
            if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) {
                reject_count(e->row, REJ_B_DECODE); continue;
            }
            memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));
            if (!color_is_inner(la_C)) {
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row, ROWMASK_AB);
                continue;
            }
            const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                                 [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
            if (!cC) {
                reject_count(e->row, REJ_C_CELL);
                if (g_incomplete_top) emit_incomplete(ctx, p, &mv, e->row, ROWMASK_AB);
                continue;
            }
            uint64_t maskB[4], forbidB[4];
            mask_of_chain(ciB, CHAIN_LEN, maskB);
            for (int k = 0; k < 4; k++) forbidB[k] = forbidA[k] | maskB[k];
            bool ab_full = false;
            for (uint32_t jc = 0; jc < cC->n && e->quota > 0; jc++) {
                uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
                if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) {
                    reject_count(e->row, REJ_C_DECODE); continue;
                }
                memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
                mv.rterm = rterm;
                *t = *p; commit_row(t, e->row, &mv);
                if (!parity_ok(t)) { reject_count(e->row, REJ_PARITY); continue; }
                /* No lookahead gate at the stop row: every board that completes
                   it is emitted -- whether a row fits above is deliberately the
                   next stage's problem. Rank by the heuristic terms only. */
                float score = score_stop(t, p, e->row, &mv);
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

    /* Beam row, two regimes.

       BEAM AT CAPACITY -- up to B_TRY conflict-free B chains, and with
       --bc_window nB,nC (default 3,3) up to nB workable B chains x nC C
       completions are scored, of which the best is kept. The row's B and C
       segments -- 10 of its 14 pieces -- then stop being whatever the database's
       global, board-blind fan-out sort offered first, and the objective steers
       generation instead of merely filtering it. At 1,1 the first accepted child
       ends the search immediately; the B_TRY retry on a B chain whose C fails is
       orthogonal and unchanged.

       BEAM BELOW CAPACITY (e->keep_all) -- enumerate the B and C cells and keep
       every child, bounded by quota alone. Picking the best of a window is only
       worth anything while something is being discarded downstream, and
       select_beam discards nothing once the pool stops filling the row width.
       There the other candidates of the window are not losers, they are
       completions being thrown away at exactly the rows the search dies on. The
       finalizer's expansion has always worked this way, over sparse cells grown
       from a single locked board, which is the same regime a collapsing beam is
       in by row 10. Quota still bounds the total, so this buys depth per A
       record with breadth across A records rather than with more work. */
    const bool window = (g_bc_nB > 1 || g_bc_nC > 1) && !e->keep_all;
    const uint32_t nB_lim = e->keep_all ? cB->n : g_bc_nB;
    RowChoice best_mv; float best_score = 0.0f; uint64_t best_sig = 0;
    uint8_t best_flags = 0;
    bool have_best = false;
    uint32_t nb_done = 0;
    /* The retry budget has to grow with the window, or the window cannot fill.
       b_left is spent on every conflict-free B chain, while nb_done counts only a
       B chain that PRODUCED a child -- so at nB = 3 the loop must find three
       productive B chains inside B_TRY conflict-free tries. Deep rows are
       conflict-dominated, so a fixed B_TRY starves any nB > 1 and the window
       would look ineffective for a reason unrelated to its merit. Exactly B_TRY
       at nB = 1, so the legacy path is untouched. */
    int b_left = B_TRY + (int)g_bc_nB - 1;
    for (uint32_t jb = 0; jb < cB->n && nb_done < nB_lim && e->quota > 0; jb++) {
        if (!e->keep_all && b_left <= 0) break;
        uint16_t ciB[CHAIN_LEN]; int la_C;
        if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) {
            reject_count(e->row, REJ_B_DECODE); continue;
        }
        b_left--;
        if (!color_is_inner(la_C)) continue;
        const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                             [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
        if (!cC) { reject_count(e->row, REJ_C_CELL); continue; }
        uint64_t maskB[4]; mask_of_chain(ciB, CHAIN_LEN, maskB);
        uint64_t forbidB[4] = { forbidA[0]|maskB[0], forbidA[1]|maskB[1],
                                forbidA[2]|maskB[2], forbidA[3]|maskB[3] };
        memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));  /* B is fixed here */
        uint32_t nc_done = 0;
        const uint32_t nC_lim = e->keep_all ? cC->n : g_bc_nC;
        for (uint32_t jc = 0; jc < cC->n && nc_done < nC_lim && e->quota > 0; jc++) {
            uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
            if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) {
                reject_count(e->row, REJ_C_DECODE); continue;
            }
            memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
            mv.rterm = rterm;
            /* Lookahead first: an exact one-row death costs nothing to find here,
               where it used to cost a board copy, commit_row and a parity scan. */
            uint32_t nA; uint64_t fB, fC;
            if (!child_lookahead(mv.ci, rterm, e->row, &nA, &fB, &fC)) {
                reject_count(e->row, REJ_LOOKAHEAD); continue;
            }
            *t = *p; commit_row(t, e->row, &mv);
            if (!parity_ok(t)) { reject_count(e->row, REJ_PARITY); continue; }
            float score = score_scanned(t, e->row, nA, fB, fC, &mv, p->score);
            if (e->keep_all) {                  /* starved beam: keep them all */
                pool_append(ctx, sc, t, e->parent_idx, score, &mv);
                e->quota--;
                nc_done++;
                continue;
            }
            if (!window) {                      /* legacy: first hit wins */
                pool_append(ctx, sc, t, e->parent_idx, score, &mv);
                e->quota--;
                return;
            }
            nc_done++;
            if (!have_best || score > best_score) {
                best_score = score; best_mv = mv; best_sig = frontier_sig(t);
                best_flags = t->flags;
                have_best = true;
            }
        }
        if (nc_done) nb_done++;                 /* a B chain that produced a child */
    }
    if (have_best) {
        pool_append_sig(ctx, sc, best_sig, e->parent_idx, best_score, &best_mv, best_flags);
        e->quota--;
    }
}

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* Set after each row's selection: did every deduplicated candidate survive?
   Read by the NEXT row's expansion -- see keep_all in expand_row. */
static bool g_beam_unpruned = false;

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
    /* Did the PREVIOUS row keep everything it generated? Then selection is not
       discarding anything -- select_beam early-returns on kept <= K -- and this
       row is very likely in the same regime. There, keeping one child per
       segment-A record throws away completions for no gain, so try_A keeps them
       all instead: exactly what the finalizer's expansion already does, and for
       the same reason.

       This is the previous row's MEASURED outcome, not a prediction from the
       beam width. `beam_n < beam_eff_K(row)` looks like the same test and is
       not: at row 1 a single parent branches wide enough to overfill any beam,
       so the width comparison says "starved" while the pool prunes 400:1. The
       measurement cannot make that mistake, and it starts false, so row 1 keeps
       the one-child economy.

       It cannot inflate the pool either: e->quota still falls once per accepted
       child and quota_parent is pool_target/beam_n, so the total is bounded by
       pool_factor x beam_width either way. keep_all REALLOCATES that quota from
       breadth across A records to depth within one. Where the beam is full that
       trade would cost A-diversity, which is why it is off there. */
    const bool keep_all = g_beam_unpruned;
    /* Does any enabled orientation place a clue on this row? */
    bool clue_row = false;
    if (g_clue_mask) {
        int pi_[3], pk_[3]; uint16_t pv_[3];
        for (int o = 0; o < 4 && !clue_row; o++)
            if ((g_clue_orients & (1u << o)) && clue_pins_for(row, o, pi_, pk_, pv_)) clue_row = true;
    }

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
        if (at_stop && g_incomplete_top && sl == 0 && !clue_row)
            try_BC(ctx, &beam[pi], row);
        if (clue_row) {
            /* A clue row is expanded ONCE per parent, not once per slice: the
               pinned walk already enumerates the parent's whole candidate set.
               Letting the other slices fall through would run the ordinary
               unclued expansion beside it and quietly fill the row with boards
               that ignore the clue. */
            if (sl != 0) continue;
            if (!expand_clue_row(ctx, &beam[pi], pi, row, at_stop, sc,
                                 quota_parent, budget_parent))
                continue;                  /* this parent owes a clue it just placed */
        }
        Expand e;
        if (!expand_prepare(&e, &beam[pi], pi, row, at_stop)) continue;
        /* Seeded per parent, not per slice. That used to be load-bearing: the
           slices shared ONE cycle over the whole cell and had to agree on it to
           divide it. Phase 2 now permutes only the slice's own untouched tail,
           so the shared key merely keeps a parent's randomness a function of the
           parent and the row rather than of where its work landed. */
        e.rng = rng_for(cfg_hash, (uint32_t)row, pi, 0xFFFFFFFFu);
        e.quota = quota_slice; e.budget = budget_slice;
        e.keep_all = keep_all;

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
       here so no child can outlive the row that made it. */
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

/* Complete stop-row candidates bypass FRONTIER deduplication. Stage C may reopen
   lower rows, so two complete boards with the same future frontier are not
   interchangeable there. The raw pool is score-sorted; emit_stop_row removes
   only exact duplicate boards. Near-duplicate suppression applies only to the
   --incomplete_top partials. */
static uint32_t rank_pool_raw(BeamCtx *ctx, uint64_t pool_n, int nt) {
    if (pool_n > UINT32_MAX) fatal("stop-row pool exceeds 32-bit ranking index");
    uint32_t n = (uint32_t)pool_n;
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (uint32_t i = 0; i < n; i++) {
        ctx->srt[i].score = ctx->pool[i].score;
        ctx->srt[i].idx = i;
    }
    sort_recs_desc(ctx->srt, ctx->srt_tmp, n, nt);
    for (uint32_t i = 0; i < n; i++) ctx->keep[i] = ctx->srt[i].idx;
    return n;
}

/* Prune the ranked survivors to target_K: a score band (respecting the
   per-parent offspring cap) plus a random band of frac_rand_now * target_K,
   then best-first fill of any remainder. */
static uint32_t select_beam(BeamCtx *ctx, uint32_t kept, uint32_t beam_n,
                            uint32_t target_K, uint32_t cap, double frac_rand_now,
                            RNG *rng) {
    const uint32_t K = target_K;
    if (kept <= K) { for (uint32_t i = 0; i < kept; i++) ctx->sel[i] = i; return kept; }
    memset(ctx->taken, 0, kept);
    memset(ctx->offspring, 0, (size_t)beam_n*sizeof(uint32_t));
    uint32_t n_sel = 0, got = 0;

    /* Per-orientation floor. Each orientation is a different hypothesis about
       which puzzle side is our row 0, and the score cannot compare them -- left
       to pure merit one orientation crowds the rest out and the search silently
       stops being complete. So each gets a guaranteed min(n_o, K/8) taken in its
       own score order, and whatever a thin orientation cannot fill simply stays
       unclaimed: the passes below run unchanged over the same taken[] array and
       spend the remainder on merit. Clues off leaves n_sel at 0 and every count
       below identical to what it was before this existed. */
    if (g_clue_mask) {
        const uint32_t floor_o = (uint32_t)(CLUE_FLOOR_FRAC * (double)K / 4.0);
        for (int o = 0; o < 4 && floor_o; o++) {
            uint32_t got_o = 0;
            for (uint32_t i = 0; i < kept && got_o < floor_o; i++) {
                if (ctx->taken[i]) continue;
                const PoolEntry *pe = &ctx->pool[ctx->keep[i]];
                if (!(pe->flags & FLAG_ORIENT_SET)) continue;
                if ((pe->flags & FLAG_ORIENT_MASK) != (uint8_t)o) continue;
                if (cap && ctx->offspring[pe->parent] >= cap) continue;
                ctx->offspring[pe->parent]++;
                ctx->taken[i] = 1; ctx->sel[n_sel++] = i; got_o++;
            }
        }
    }

    /* Split what is left the way the whole beam was split before. */
    const uint32_t rem = (n_sel < K) ? K - n_sel : 0;
    uint32_t k_rand = (uint32_t)(frac_rand_now * rem);
    if (k_rand > rem) k_rand = rem;
    uint32_t k_top = rem - k_rand;
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
        d->depth = (uint16_t)row;
        /* PoolEntry.score is the ranking used on THIS row. BeamEntry.score must
           carry only the learned path statistic into the NEXT row. */
        d->score = g_freq_on
            ? (float)((double)src[pe->parent].score + freq_row_term(&pe->mv, row))
            : pe->score;
        d->log_idx = i;
        d->flags = pe->flags;
        assert(parity_ok(d));
    }
}

/* Stop-row emission buffers, allocated on first use (emission is entered from
   serial code). One tile is ~12.6 MB. */
#define EMIT_TILE 4096
static char *g_emit_lines = NULL;    /* EMIT_TILE x EMIT_LINE_MAX */
static uint64_t *g_emit_fps = NULL;
static int  *g_emit_lens  = NULL;

/* Emit complete stop-row boards best-scored first. Formatting and exact-board
   fingerprints are tiled in parallel; writes and the fingerprint table stay
   serial so file order is the score order. Distinct complete boards are never
   suppressed merely because they differ in only one piece. */
static void emit_stop_row(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept, int row) {
    if (!g_completions_fp) return;
    if (!g_emit_lines) {
        g_emit_lines = xmalloc((size_t)EMIT_TILE * EMIT_LINE_MAX);
        g_emit_fps   = xmalloc((size_t)EMIT_TILE * sizeof(uint64_t));
        g_emit_lens  = xmalloc((size_t)EMIT_TILE * sizeof(int));
    }
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    for (uint32_t base = 0; base < kept && g_emit_count < EMIT_MAX && !g_stop;
         base += EMIT_TILE) {
        uint32_t tile = kept - base < (uint32_t)EMIT_TILE
                      ? kept - base : (uint32_t)EMIT_TILE;
        #pragma omp parallel for schedule(static) num_threads(nt)
        for (uint32_t k = 0; k < tile; k++) {
            const PoolEntry *pe = &ctx->pool[ctx->keep[base + k]];
            RowChoice rows[EDGE_LEN];
            rows[row] = pe->mv;
            collect_rows(ctx, &beam[pe->parent], rows);
            g_emit_fps[k] = board_fingerprint(rows, row);
            g_emit_lens[k] = format_board_tail(rows, row, ROWMASK_FULL,
                                               (pe->flags & FLAG_ORIENT_SET)
                                                   ? (int)(pe->flags & FLAG_ORIENT_MASK) : -1,
                                               g_emit_lines + (size_t)k * EMIT_LINE_MAX);
        }
        if (g_freq_debug && g_freq_on && g_freq_pure)
            for (uint32_t k = 0; k < tile; k++) {
                const PoolEntry *pe = &ctx->pool[ctx->keep[base + k]];
                RowChoice rows[EDGE_LEN];
                rows[row] = pe->mv;
                collect_rows(ctx, &beam[pe->parent], rows);
                double full = freq_board_term(rows, row);
                if (fabs(full - (double)pe->score) > 1e-2 * (1.0 + fabs(full)))
                    fatal("E555_FREQ_DEBUG: accumulated score %.6f but the whole "
                          "board sums to %.6f -- the running total is not the board",
                          (double)pe->score, full);
            }
        for (uint32_t k = 0; k < tile && g_emit_count < EMIT_MAX; k++) {
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
    /* The root carries row 0 of the learned statistic. Stock lookahead/colour
       scores are deliberately not stored here: they remain local to each row. */
    if (g_freq_on) cur[0].score = (float)freq_beam_bottom_term(g_cur_bottom);
    uint32_t beam_n = 1;
    g_beam_unpruned = false;          /* row 1 keeps the one-child economy */
    memset(ctx->log_n, 0, sizeof ctx->log_n);
    maha_reset_config();
    g_stats.configs++;

    for (int row = 1; (uint32_t)row <= g_stop_row; row++) {
        if (g_stop)                      { res.reason = "interrupted"; break; }
        if (omp_get_wtime() >= deadline) { res.reason = "time";        break; }
        double t_row = omp_get_wtime();

        expand_row(ctx, cur, beam_n, row, cfg_hash, scratch);
        double t_exp = omp_get_wtime();
        g_stats.t_expand += t_exp - t_row;
        uint64_t pool_n = ctx->pool_n;
        if (pool_n > ctx->pool_cap) pool_n = ctx->pool_cap;
        g_stats.cands_total += pool_n;
        g_stats.row_attempts[row]++;
        g_stats.row_candidates[row] += pool_n;
        if (pool_n == 0) {
            res.reason = "extinct"; res.row = (uint32_t)row;
            g_stats.extinct_at[row]++;
            break;
        }

        /* Search-mode stop-row candidates remain raw through ranking so frontier
           equivalence cannot collapse distinct complete boards. emit_stop_row
           then removes exact duplicate boards only. Learning still frontier-
           deduplicates before counting, and all earlier rows must deduplicate
           because equivalent futures do not both belong in the beam. */
        const bool raw_stop = ((uint32_t)row == g_stop_row) && !g_learning;
        uint32_t kept = raw_stop ? rank_pool_raw(ctx, pool_n, nt)
                                 : dedup_and_rank(ctx, pool_n, nt);
        g_stats.row_retained[row] += kept;
        g_stats.t_select += omp_get_wtime() - t_exp;
        res.row = (uint32_t)row;
        g_stats.rows_advanced++;

        if ((uint32_t)row == g_stop_row) {
            res.width = kept;
            g_stats.row_selected[row] += kept;
            double t0 = omp_get_wtime();
            if (g_learning) learn_stop_row(ctx, cur, kept, row, cfg_hash);
            else             emit_stop_row(ctx, cur, kept, row);
            g_stats.t_emit += omp_get_wtime() - t0;
            g_stats.reached_stop++;
            partials_budget_spent();       /* the whole beam is written by now */
            if (g_verbose) {
                double dt = omp_get_wtime() - t_row;
                printf("[beam] %s row=%d cands=%" PRIu64 " ranked=%u emitted<=%u t=%.2fs\n",
                       g_config_id_str, row, pool_n, kept,
                       kept < EMIT_MAX ? kept : EMIT_MAX, dt);
                fflush(stdout);
            }
            break;
        }

        RNG sel_rng = rng_for(cfg_hash, (uint32_t)row, 0xFFFFFFFFu, 1u);
        uint32_t eff_K = beam_eff_K(row);
        g_beam_unpruned = (kept <= eff_K);
        uint32_t n_sel = select_beam(ctx, kept, beam_n, eff_K,
                                     parent_cap_eff(row), g_frac_rand, &sel_rng);
        g_stats.row_selected[row] += n_sel;
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
    g_start_row = br;
    g_resume_bi = bi;
    g_resume_li = li;
    if (nf >= 4) g_resume_sol_idx = (uint64_t)sol;
    g_resume_active = true;
    printf("[resume] start_row=%u bi=%u li=%u sol_idx=%" PRIu64 "\n",
           g_start_row, g_resume_bi, g_resume_li, g_resume_sol_idx);
}

/* -- Summary ------------------------------------------------------------------ */

static double score_diag_sd(double n, double sum, double sumsq) {
    if (n <= 0.0) return 0.0;
    double v = sumsq / n - (sum / n) * (sum / n);
    return v > 0.0 ? sqrt(v) : 0.0;
}

static bool score_diag_corr(double n, double sx, double sxx,
                            double sy, double syy, double sxy,
                            double *out) {
    if (n <= 1.0) return false;
    double vx = sxx - sx * sx / n;
    double vy = syy - sy * sy / n;
    if (!(vx > 1e-18) || !(vy > 1e-18)) return false;
    *out = (sxy - sx * sy / n) / sqrt(vx * vy);
    return true;
}

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
        printf("[sum] row flow (attempts:candidates/retained/selected):");
        for (int r = 1; r <= (int)g_stop_row; r++)
            if (g_stats.row_attempts[r])
                printf("  r%d=%" PRIu64 ":%" PRIu64 "/%" PRIu64 "/%" PRIu64,
                       r, g_stats.row_attempts[r], g_stats.row_candidates[r],
                       g_stats.row_retained[r], g_stats.row_selected[r]);
        printf("\n");
    }
    if (g_border_stats.bottoms_ranked) {
        printf("[sum] border prefilter: bottom-slots=%" PRIu64
               " no-clue-column=%" PRIu64
               " columns ordinary=%" PRIu64 " clue-compatible=%" PRIu64
               " run=%" PRIu64 " time=%.1fs\n",
               g_border_stats.bottoms_ranked,
               g_border_stats.bottoms_no_clue_column,
               g_border_stats.columns_ordinary_viable,
               g_border_stats.columns_clue_compatible,
               g_border_stats.columns_run,
               g_border_stats.clue_seconds);
    }
    if (g_lambda_maha != 0.0) {
        printf("[sum] Mahalanobis pooled raw SD by row:");
        bool anysd = false;
        for (int r = 1; r <= (int)g_stop_row; r++) {
            double n = g_maha_pool_n[r];
            if (n < MAHA_MIN_SAMPLES) continue;
            double m = g_maha_pool_sum[r] / n;
            double v = g_maha_pool_sumsq[r] / n - m * m;
            if (!(v > 1e-18)) continue;
            printf("  r%d:%.3f(n=%.0f)", r, sqrt(v), n);
            anysd = true;
        }
        if (!anysd)
            printf("  none measured (every row under the %.0f-sample floor)",
                   MAHA_MIN_SAMPLES);
        printf("\n");
    }
    if (g_verbose) {
        for (int r = 1; r <= (int)g_stop_row; r++) {
            ScoreDiag a = {0};
            for (int th = 0; th < MAX_ACC_THREADS; th++) {
                const ScoreDiag *d = &g_score_diag[th][r];
                a.n += d->n;
                for (int k = 0; k < SD_N; k++) {
                    a.sum[k] += d->sum[k]; a.sumsq[k] += d->sumsq[k];
                }
                a.cross_table_fan += d->cross_table_fan;
                a.cross_table_J   += d->cross_table_J;
                a.cross_J_maha    += d->cross_J_maha;
            }
            if (a.n <= 0.0) continue;
            printf("[score] r%d n=%.0f SD fan=%.3f J=%.3f Maha=%.3f "
                   "table_inc=%.3f table_acc=%.3f",
                   r, a.n,
                   score_diag_sd(a.n, a.sum[SD_FANOUT], a.sumsq[SD_FANOUT]),
                   score_diag_sd(a.n, a.sum[SD_CLOSURE], a.sumsq[SD_CLOSURE]),
                   score_diag_sd(a.n, a.sum[SD_MAHA], a.sumsq[SD_MAHA]),
                   score_diag_sd(a.n, a.sum[SD_TABLE_INC], a.sumsq[SD_TABLE_INC]),
                   score_diag_sd(a.n, a.sum[SD_TABLE_ACC], a.sumsq[SD_TABLE_ACC]));
            double c;
            if (score_diag_corr(a.n,
                                a.sum[SD_TABLE_ACC], a.sumsq[SD_TABLE_ACC],
                                a.sum[SD_FANOUT], a.sumsq[SD_FANOUT],
                                a.cross_table_fan, &c))
                printf(" corr(T,F)=%.3f", c);
            if (score_diag_corr(a.n,
                                a.sum[SD_TABLE_ACC], a.sumsq[SD_TABLE_ACC],
                                a.sum[SD_CLOSURE], a.sumsq[SD_CLOSURE],
                                a.cross_table_J, &c))
                printf(" corr(T,J)=%.3f", c);
            if (score_diag_corr(a.n,
                                a.sum[SD_CLOSURE], a.sumsq[SD_CLOSURE],
                                a.sum[SD_MAHA], a.sumsq[SD_MAHA],
                                a.cross_J_maha, &c))
                printf(" corr(J,M)=%.3f", c);
            printf("\n");
        }
    }
    {
        int first = (int)g_stop_row - 2;
        if (first < 1) first = 1;
        for (int r = first; r <= (int)g_stop_row; r++) {
            uint64_t tot[REJ_N] = {0};
            uint64_t all = 0;
            for (int th = 0; th < MAX_ACC_THREADS; th++)
                for (int k = 0; k < REJ_N; k++) {
                    tot[k] += g_reject[th][r][k];
                }
            for (int k = 0; k < REJ_N; k++) all += tot[k];
            if (!all) continue;
            printf("[late] row %d rejection attempts:", r);
            for (int k = 0; k < REJ_N; k++)
                if (tot[k]) printf(" %s=%" PRIu64, g_rej_name[k], tot[k]);
            printf("\n");
        }
    }
    if (g_clue_debug)
        for (int r = 1; r <= EDGE_LEN; r++)
            if (g_dbg_calls[r])
                printf("[clue] row %2d: parents=%llu sumA=%llu sumB=%llu sumC=%llu\n", r,
                       (unsigned long long)g_dbg_calls[r], (unsigned long long)g_dbg_nA[r],
                       (unsigned long long)g_dbg_nB[r], (unsigned long long)g_dbg_nC[r]);
    if (g_learning)
        printf("[sum] learned unique boards counted: %" PRIu64 "\n",
               g_stats.emitted_total);
    else
        printf("[sum] emitted complete boards: %" PRIu64 "\n",
               g_stats.emitted_total);
    if (g_incomplete_top) {
        printf("[sum] incomplete-top partials: %zu  (", g_partial_total);
        for (int k = 0; k < PART_N; k++)
            printf("%s%s %zu", k ? ", " : "", g_part_name[k], g_part_total[k]);
        printf(")\n");
    }
    fflush(stdout);
}

/* -- main ------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Does a left column's window score read the board the right way up?
 *
 * left_window_logfanout claims that rotating the column a quarter-turn CCW turns
 * a vertical 5-strip of col-1 pieces into an ordinary DB chain, so that the
 * strip standing against rows r..r+4 is counted by feeding right[] DOWNWARD.
 * The direction is the whole claim and it fails silently: the database is dense
 * enough that a reversed read still returns a large, plausible-looking number
 * for every column -- just the count of a tuple no column ever presents -- so
 * the ranking would quietly become noise. Hence a check rather than trust.
 *
 * Build a real strip out of the database instead of a board file: decode a
 * record, giving a horizontal chain q0..q4 with q_i.bottom = b_i and
 * q_i.left = q_{i-1}.right. Rotate each piece CW (the inverse turn) and stack
 * them, q0 on top. Then v_i.left = q_i.bottom = b_i and v_i.bottom = q_i.right
 * = q_{i+1}.left = v_{i+1}.top, so the pieces really do stack -- that assertion
 * is what catches a wrong face map. Laying v_i at row r+4-i makes right[r+4-i]
 * = b_i, so a correct downward read must return EXACTLY the cell we decoded
 * from, and the test is that equality rather than a mere non-zero: the database
 * is dense enough (about two thirds of cells occupied, and the fan-out sums 17
 * of them) that almost any 5-tuple of inner colours scores above zero, so "the
 * reversed order also scores" is true and proves nothing. The reversed count is
 * reported only to show the test has teeth -- it is a different tuple, so it
 * should usually differ. The board-level direction is settled independently:
 * on data/synth_solution_480.csv the downward read is a legal chain for 10 of
 * 10 windows of the true left column, the upward read for 0 of 10. */
static void verify_column_rotation(void) {
    if (!getenv("E555_COL_VERIFY")) return;
    RNG r = rng_for(0xC01C01C0u, 0, 0, 0);
    const uint64_t zero[4] = {0,0,0,0};
    int checked = 0, tried = 0, bad = 0, fwd_hit = 0, rev_hit = 0;
    while (checked < 300 && tried < 200000) {
        tried++;
        int la = COLOR_MIN + (int)rng_uniform(&r, DIM_INNER);
        uint8_t b[CHAIN_LEN];
        for (int i = 0; i < CHAIN_LEN; i++) b[i] = (uint8_t)(COLOR_MIN + rng_uniform(&r, DIM_INNER));
        const Cell *c = g_db[INNER_IDX(la)][INNER_IDX(b[0])][INNER_IDX(b[1])]
                            [INNER_IDX(b[2])][INNER_IDX(b[3])][b[4]];
        if (!c || c->n == 0) continue;

        uint32_t w = rec_load(c->rec, 0, g_rec_bytes_inner);
        uint8_t f[CHAIN_LEN]; unpack_inner(w, f, g_lb_bits);
        uint16_t ci[CHAIN_LEN]; uint64_t m[4] = {0,0,0,0};
        if (!decode_inner_chain(f, CHAIN_LEN, la, b, ci, m, zero)) { bad++; continue; }
        checked++;

        /* Turn the chain back a quarter-turn and stand it up, q0 on top. */
        Oriented v[CHAIN_LEN];
        for (int i = 0; i < CHAIN_LEN; i++)
            v[i] = g_cat[cat_index_of(g_cat[ci[i]].piece_id, (uint8_t)((g_cat[ci[i]].rotation + 3) & 3))];
        for (int i = 0; i < CHAIN_LEN; i++)
            if (v[i].left != b[i]) { printf("[colv] FACE MAP: v[%d].left=%u b=%u\n", i, v[i].left, b[i]); bad++; }
        for (int i = 0; i + 1 < CHAIN_LEN; i++)
            if (v[i].bottom != v[i+1].top) { printf("[colv] STACK BREAK at %d\n", i); bad++; }

        /* Lay it against rows 1..5 of a column and score it both ways round. */
        int right[PUZZLE_SIDE], rev[PUZZLE_SIDE];
        memset(right, 0, sizeof right); memset(rev, 0, sizeof rev);
        for (int i = 0; i < CHAIN_LEN; i++) { right[CHAIN_LEN - i] = v[i].left; rev[1 + i] = v[i].left; }
        double want = log1p((double)db_seg_fanout(b[0], b[1], b[2], b[3], b[4]));
        double got  = left_window_logfanout(right, 1);
        if (got == want) fwd_hit++;
        else { printf("[colv] WRONG WINDOW: got %.6f want %.6f\n", got, want); bad++; }
        if (left_window_logfanout(rev, 1) != want) rev_hit++;
    }
    printf("[colv] %d strips: downward %d/%d exact, reversed differs %d/%d, %d error(s)\n",
           checked, fwd_hit, checked, rev_hit, checked, bad);
    if (bad) fatal("column rotation self-check failed");
}

/* E555_SEG_VERIFY=1: assert that the pinned-segment walk with NO pin reproduces
   the database cell for the same key, chain for chain. The walk reads the raw
   (left,bottom) buckets while the cell holds packed records built by a separate
   DFS, so agreeing on both the count and the exact multiset of chains is strong
   evidence the walk enumerates the same object the beam already trusts.
   Only meaningful with clues off: with them on the walk deliberately sees pieces
   the database excludes, so it would legitimately find more. */
static void verify_segment_enumerator(void) {
    if (!getenv("E555_SEG_VERIFY")) return;
    if (g_clue_mask) { printf("[segv] skipped (clues on: the walk sees excluded pieces)\n"); return; }
    enum { CAP = 2048 };
    static uint16_t out[CAP][CHAIN_LEN];
    static uint64_t ha[CAP], hb[CAP];
    RNG r = rng_for(0x5E6C0DE5u, 0, 0, 0);
    const uint64_t zero[4] = {0,0,0,0};
    int checked = 0, tried = 0, bad = 0; uint64_t chains = 0;
    while (checked < 300 && tried < 200000) {
        tried++;
        int la = COLOR_MIN + (int)rng_uniform(&r, DIM_INNER);
        uint8_t b[CHAIN_LEN];
        for (int i = 0; i < CHAIN_LEN; i++) b[i] = (uint8_t)(COLOR_MIN + rng_uniform(&r, DIM_INNER));
        const Cell *c = g_db[INNER_IDX(la)][INNER_IDX(b[0])][INNER_IDX(b[1])]
                            [INNER_IDX(b[2])][INNER_IDX(b[3])][b[4]];
        if (!c || c->n == 0 || c->n > CAP) continue;
        checked++; chains += c->n;
        for (uint32_t j = 0; j < c->n; j++) {
            uint32_t w = rec_load(c->rec, j, g_rec_bytes_inner);
            uint8_t f[CHAIN_LEN]; unpack_inner(w, f, g_lb_bits);
            uint16_t ci[CHAIN_LEN]; uint64_t m[4] = {0,0,0,0};
            if (!decode_inner_chain(f, CHAIN_LEN, la, b, ci, m, zero)) { bad++; break; }
            uint64_t h = 1469598103934665603ULL;
            for (int i = 0; i < CHAIN_LEN; i++) { h ^= ci[i]; h *= 1099511628211ULL; }
            ha[j] = h;
        }
        int n = enumerate_pinned_segment(la, b, CHAIN_LEN, -1, PIN_PIECE, 0, zero, out, CAP);
        if (n != (int)c->n) {
            printf("[segv] COUNT MISMATCH la=%d db=%u walk=%d\n", la, c->n, n); bad++; continue;
        }
        for (int j = 0; j < n; j++) {
            uint64_t h = 1469598103934665603ULL;
            for (int i = 0; i < CHAIN_LEN; i++) { h ^= out[j][i]; h *= 1099511628211ULL; }
            hb[j] = h;
        }
        qsort(ha, (size_t)n, sizeof ha[0], cmp_u64);
        qsort(hb, (size_t)n, sizeof hb[0], cmp_u64);
        if (memcmp(ha, hb, (size_t)n * sizeof ha[0]) != 0) {
            printf("[segv] SET MISMATCH la=%d n=%d\n", la, n); bad++;
        }
    }
    printf("[segv] %d cells, %llu chains: %s\n", checked, (unsigned long long)chains,
           bad ? "MISMATCHES FOUND" : "walk == database, exactly");
    if (bad) fatal("segment enumerator disagrees with the database");
}

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
"  --start_row N          first data row of rotation.csv to use (default 0)\n"
"  --num_rows N           number of consecutive border rows to sweep\n"
"                         (default 0 = every remaining row of rotation.csv)\n"
"  --db_file PATH         on-disk cache of the seed-only inner database: built and\n"
"                         written on first run, mmapped in seconds on later runs\n"
"  --free_edges           any edge piece may terminate a row on the right (relaxed\n"
"                         parity; default: only the border row's own right edges)\n"
"  --random_edges         sample random borders from the seed's edge pieces instead\n"
"                         of reading a Stage A arrangement (implies --free_edges;\n"
"                         rotation.csv may be omitted and is ignored if given).\n"
"                         --samples = number of random bottoms to try (default 1;\n"
"                         0 = uncapped, run until --wall_time/--max_emitted stops\n"
"                         you), --top_columns = random left columns per bottom;\n"
"                         each border is the best of 32 fan-out-ranked samples.\n"
"                         --start_row/--num_rows, --top_bottoms and --resume do\n"
"                         not apply\n"
"  --BL N / --BR N        pin a seed piece index (0..255) to the Bottom-Left /\n"
"  --TL N / --TR N        Bottom-Right / Top-Left / Top-Right corner (--random_edges\n"
"                         only); unpinned corners are sampled, and with 3 pinned the\n"
"                         4th is forced. Each must be a genuine, distinct corner\n"
"  --incomplete_top       also emit stop-row boards with only part of the stop row:\n"
"                         two segments (A+B, A+C, B+C) to <...>_partial.csv and\n"
"                         segment B alone to <...>_partial_B.csv. A partial is dropped\n"
"                         when an earlier one of the same kind has the same pieces\n"
"                         below the stop row and differs in at most one stop-row\n"
"                         piece. Complete rows drop exact duplicates only. All CSVs\n"
"                         are APPENDED, never truncated\n"
"\n"
"Beam shape:\n"
"  --beam_width K         boards kept per row (default 250000)\n"
"  --stop_row R           last row the beam fills, 1..13; reaching boards are emitted\n"
"                         (default 11: the beam reliably FILLS row 11 and reliably\n"
"                         dies attempting 12, so stopping at 11 emits that material\n"
"                         instead of discarding it -- hand row 12 to the finalizer)\n"
"  --beam_expand E        late-search width multiplier (default 4; 1 = no expansion)\n"
"  --beam_expand_row R    row with the full ExK width; half of the extra width is\n"
"                         granted one row earlier (default 7)\n"
"\n"
"Scoring / selection:\n"
"  --lambda_J F           weight of the CLOSURE term, the objective derived from\n"
"                         pairing combinatorics: A_tot*KL(free-color mix || flat) plus\n"
"                         a demand term, in nats like the fan-out terms. The primary\n"
"                         color objective; 0 turns it off (default 1.0, useful 0.5-1.5)\n"
"  --lambda_Mahalanobis F weight of the piece-structure CORRECTION, in units of its own\n"
"                         per-row standard deviation -- the spread is measured live and\n"
"                         divided out, so F means the same thing at every depth. Small\n"
"                         by design: it correlates ~0.88 with closure, so only the\n"
"                         residual is new (default 1.0; 0 = off)\n"
"                         Both terms are always live. --lambda_Mahalanobis 0 is closure\n"
"                         alone and --lambda_J 0 is Mahalanobis alone, which is why\n"
"                         there is no --score_model.\n"
"  --clue_center          force the published center clue piece onto its cell, at its\n"
"                         orientation's spin (piece 138; one of the 4 center cells)\n"
"  --clue_corners         force the two published corner clues the beam can reach,\n"
"                         both on row 2; the row-13 pair is only reserved, never pinned,\n"
"                         and constrains no searched row. Clue pieces leave the database\n"
"  --pin_clue N           search ONE of the four clue frames instead of hedging over all\n"
"                         four. The five clues are one rigid body, so naming where the\n"
"                         CENTRE clue sits names the whole set. Rows are 0-indexed\n"
"                         bottom-up, and N runs anticlockwise from the lower left:\n"
"                           0  not pinned -- all four frames at once (default)\n"
"                           1  centre clue lower-left   (7,7)\n"
"                           2  centre clue lower-right  (7,8)\n"
"                           3  centre clue upper-right  (8,8)\n"
"                           4  centre clue upper-left   (8,7)\n"
"                         This is a simplification, not extra constraint machinery:\n"
"                         unpinned, every clue row expands each parent once per frame\n"
"                         and the beam carries boards from four frames at once. Pinned,\n"
"                         it expands once and the beam is one frame throughout.\n"
"                         Implies --clue_center and NOTHING else -- add --clue_corners\n"
"                         yourself if you want it, since it costs four more pieces out\n"
"                         of the chain database. Pinning alone does not, so a pinned run\n"
"                         reuses an unpinned run's --db_file cache\n"
"\n"
"DATA-DRIVEN STEERING (this fork only; --learn and --table are separate runs)\n"
"  --learn PATH           learning phase: grow the board from all four sides in turn,\n"
"                         counting where each piece lands in the canonical frame, and\n"
"                         write the table to PATH. Emits nothing. Needs a rotations\n"
"                         file and --pin_clue.\n"
"  --table PATH           search phase: use the learned table in the beam and\n"
"                         ALWAYS rank bottom rows and left columns by exact location.\n"
"  --freq_model M         beam statistic: segment (default, pooled A/B/C) or cell\n"
"                         (exact piece-cell). Both use the same future-preservation\n"
"                         normalization, KT half-count smoothing and Sinkhorn.\n"
"  --frac_rand F          fraction of the beam selected at random instead of by\n"
"                         score, FLAT across rows. Both bands are drawn from the\n"
"                         same deduplicated pool and sum to the row width, so a\n"
"                         lower value does not send more states forward -- it sends\n"
"                         better-chosen ones. Only bites where the pool exceeds the\n"
"                         width; where it does not, every candidate survives anyway\n"
"                         (default 0.10; 0 trusts the objective completely)\n"
"  --parent_cap N         max children per parent in the score-selected band;\n"
"                         doubled from beam_expand_row-1 on; 0 = uncapped (default 4)\n"
"\n"
"Feasibility certificates:\n"
"  --no_free_demand       DISABLE the free-mode demand accounting. On by default: an\n"
"                         edge piece owes one inner half-edge of its own color whether\n"
"                         it ends up a right edge or a top border piece, so free mode's\n"
"                         demands -- and hence the color parity test -- are exact\n"
"                         without knowing the split. Without this accounting free mode\n"
"                         carries no color certificate at all\n"
"\n"
"Expansion effort:\n"
"  --pool_factor N        candidate-pool target as a multiple of beam_width; more\n"
"                         pool = more children scored per row (default 8)\n"
"  --bc_window nB,nC      per segment-A record, how many B/C completions to consider.\n"
"                         Two regimes, chosen per row by whether the beam is full:\n"
"                           beam AT capacity -- score up to nB workable B chains x nC\n"
"                             C completions and keep the BEST child, so the objective\n"
"                             picks it instead of the database\'s board-blind sort;\n"
"                           beam BELOW capacity -- enumerate the B and C cells and keep\n"
"                             EVERY child, bounded by the per-parent quota alone.\n"
"                         Selection only discards states while the pool exceeds the row\n"
"                         width, so where it does not, an extra child costs nothing and\n"
"                         a discarded sibling is a completion thrown away. 1,1 makes\n"
"                         the full-beam regime take the first fit (default 3,3)\n"
"\n"
"Sweep control:\n"
"  --top_bottoms N        bottom-row orderings tried per border row, best-ranked\n"
"                         first (<1 = all; default 10). A bottom on a NEW border is\n"
"                         worth more than another bottom on one already tried, so\n"
"                         spend the budget on --num_rows before raising this\n"
"  --top_columns N        left-column orderings tried per bottom (<1 = all; default 12).\n"
"                         Ranked SEPARATELY FOR EACH BOTTOM, since a column's rank is\n"
"                         conditional on the bottom it shares the board with, so l0\n"
"                         means the best column for THIS bottom. Columns that cannot\n"
"                         complete row 1 with it are dropped, which is why the [rank]\n"
"                         line can show fewer run than asked for\n"
"  --tau_bottoms T        selection temperature for the bottom ranking: above 0 the\n"
"                         --top_bottoms tried are a sample without replacement with\n"
"                         probability proportional to exp(rank/tau) rather than the\n"
"                         greedy head. 0 = off, exact legacy (default 0; ~2 buys\n"
"                         variety at no measured yield cost)\n"
"  --tau_columns T        the same for the left-column ranking. EVERY MEASUREMENT OF\n"
"                         THIS FLAG BEFORE THE COLUMN RANK WAS REWRITTEN IS VOID: the\n"
"                         old rank was one constant across all columns of a border\n"
"                         row (see left_rank_of), so rank/tau cancelled and tau chose\n"
"                         nothing -- tau 2 and tau 4 picked identical columns run for\n"
"                         run, while the same test on --tau_bottoms did not.\n"
"                         There is now a real rank to perturb. 0 = off (default 0);\n"
"                         re-measure before raising it\n"
"  --bail_columns N       abandon a bottom after N consecutive columns that emitted\n"
"                         nothing, instead of running all --top_columns. Most useful\n"
"                         with --incomplete_top: completions and two-segment partials\n"
"                         reset the streak, B-only partials do not.\n"
"                         On completions alone at a high --stop_row emissions are rare\n"
"                         enough that this acts as a cap of N columns per bottom.\n"
"                         0 = never bail (default 0)\n"
"  --time_limit S         wall-time slice per (bottom x left) config; a per-row\n"
"                         deadline, so it only fires on a config that runs long\n"
"                         (default 600)\n"
"  --wall_time S          total wall-time budget; 0 = unlimited (default 0)\n"
"  --max_emitted N        stop once N boards have been reported, counting both\n"
"                         completions and --incomplete_top partials; the\n"
"                         configuration in flight is always reported in full, so the\n"
"                         final count can overshoot N by that configuration's output\n"
"                         (0 = unlimited; default 0)\n"
"  --resume               continue from <out_dir>/sweep_checkpoint.txt\n"
"\n"
"Misc:\n"
"  --threads N            OpenMP threads (default: all cores)\n"
"  --rng_seed S           RNG seed; omitted = randomized from clock+pid. Explicit\n"
"                         0 is a valid deterministic seed, like any other integer\n"
"  --verbose              per-row beam traces plus score-component SD/correlations\n"
"  --help                 this text\n", a0);
}

/* -- --print_cmd ----------------------------------------------------------
 * The whole invocation, every flag carrying the value the run will actually
 * use, whether that came from the command line or from a default. Copy the
 * line and you have the run: no scrolling back through a log, no guessing
 * which defaults were in force on the day.
 *
 * It prints and then the run continues, so an example can pass it on every run
 * and teach while it works.
 *
 * Every flag the parser accepts must appear here. tests/check_script_flags.py
 * enforces that, because a hand-written printer drifts from its parser within
 * about two commits. */
static void print_cmd(const char *a0, const char *seed_path, const char *csv_path,
                      bool resume) {
    static const char *corner[4] = { "--BL", "--BR", "--TL", "--TR" };
    printf("[cmd] %s %s", a0, seed_path);
    if (csv_path) printf(" %s", csv_path);
    if (g_random_edges)   printf(" --random_edges");
    if (g_free_edges)     printf(" --free_edges");
    if (g_incomplete_top) printf(" --incomplete_top");
    if (resume)           printf(" --resume");
    if (g_verbose)        printf(" --verbose");
    if (g_print_cmd)      printf(" --print_cmd");
    if (g_pin_clue)                 printf(" --pin_clue %d", g_pin_clue);
    /* --pin_clue turns CLUE_CENTER on itself, so printing both is not a
       contradiction -- the line stays correct if the implication ever changes. */
    if (g_clue_mask & CLUE_CENTER)  printf(" --clue_center");
    if (g_clue_mask & CLUE_CORNERS) printf(" --clue_corners");
    if (!g_free_demand)   printf(" --no_free_demand");
    for (int k = 0; k < 4; k++)
        if (g_fixed_corner_pid[k] >= 0) printf(" %s %d", corner[k], g_fixed_corner_pid[k]);
    printf(" --out_dir %s", g_out_dir);
    if (g_random_edges) printf(" --samples %u", g_samples);
    else                 printf(" --start_row %u --num_rows %u", g_start_row, g_num_rows);
    if (g_db_file) printf(" --db_file %s", g_db_file);
    printf(" --beam_width %u --stop_row %u", g_beam_width, g_stop_row);
    printf(" --beam_expand %u --beam_expand_row %u", g_beam_expand, g_beam_expand_row);
    printf(" --lambda_J %g --lambda_Mahalanobis %g", g_lambda_J, g_lambda_maha);
    printf(" --frac_rand %g --parent_cap %u --pool_factor %u",
           g_frac_rand, g_parent_cap, g_pool_factor);
    printf(" --bc_window %u,%u", g_bc_nB, g_bc_nC);
    printf(" --top_bottoms %ld --top_columns %ld", g_top_bottoms, g_top_columns);
    if (g_learn_path) printf(" --learn %s", g_learn_path);
    if (g_table_path) printf(" --table %s", g_table_path);
    if (g_learn_path || g_table_path)
        printf(" --freq_model %s",
               g_freq_model == FREQ_MODEL_CELL ? "cell" : "segment");
    printf(" --tau_bottoms %g --tau_columns %g", g_tau_bottoms, g_tau_columns);
    printf(" --bail_columns %u", g_bail_columns);
    printf(" --threads %d --rng_seed %" PRIu64, g_nthreads, g_master_seed);
    printf(" --time_limit %g --wall_time %g --max_emitted %" PRIu64 "\n",
           g_config_time_sec, g_max_wall_sec, g_max_partials);
}

/* -- The [sweep] line, and the run-length collapse behind it ----------------- */
/* One configuration used to print 130 characters of mostly zeros whether or not
   it found anything, and a sweep that loses a whole CLASS of columns at one row
   prints hundreds of those in a row: in one clued production log, 474 of 653
   lines were byte-identical but for the column index. So a configuration that
   emitted nothing prints only what is not trivially zero, and consecutive
   barren ones that died the same way under the same bottom collapse into one
   counted line.
 
   The first of a run always prints in full, and a run is flushed once its
   configurations have cost SWEEP_QUIET_SEC between them, so a slow sequence of
   identical deaths still reports progress rather than going silent.
 
   `filled` and `died` are separate fields because one number cannot be both.
   res.row is the last row COMPLETED for stop_row, time and interrupted, and the
   row that FAILED for an extinction -- where res.width is then the width the
   beam carried into that row, not a width it ever reached. Printing both under
   one name is what made "row=11 width=1292" read as though row 11 held 1292
   boards, when row 11 held none and 1292 is what row 10 handed it. */
#define SWEEP_QUIET_SEC 30.0
static char     g_sw_group[64] = "";   /* the bottom whose columns are pending */
static char     g_sw_key[96]   = "";   /* how they died; identical or no run   */
static bool     g_sw_armed = false;    /* a run is open, even at zero suppressed */
static long     g_sw_first = -1, g_sw_last = -1;   /* the SUPPRESSED span      */
static uint32_t g_sw_n     = 0;        /* suppressed since the printed one     */
static double   g_sw_wall  = 0.0;      /* their combined wall time             */
static double   g_progress_last = 0.0;

/* Does any enabled orientation pin something on this row? A row is pinned by a
   clue ON it and, one row earlier, by the colour that clue will sit on -- which
   is why --clue_corners bites at row 1, two rows below the cells it names. */
static bool clue_row_pinned(int row) {
    if (!g_clue_mask) return false;
    for (int o = 0; o < 4; o++) {
        if (!(g_clue_orients & (1u << o))) continue;
        int pi[3], pk[3]; uint16_t pv[3];
        if (clue_pins_for(row, o, pi, pk, pv)) return true;
    }
    return false;
}

/* reason=, with the one qualifier that explains an otherwise baffling death. */
static const char *sweep_reason(const BeamResult *br) {
    static char buf[64];
    if (strcmp(br->reason, "extinct") || !clue_row_pinned((int)br->row))
        return br->reason;
    snprintf(buf, sizeof buf, "%s(clue_row)", br->reason);
    return buf;
}

static void sweep_flush(void) {
    if (!g_verbose) {
        g_sw_group[0] = g_sw_key[0] = '\0';
        g_sw_armed = false; g_sw_first = g_sw_last = -1; g_sw_n = 0; g_sw_wall = 0.0;
        return;
    }
    if (g_sw_n == 1)                    /* one held back is not worth a range */
        printf("[sweep] %sl%ld %s wall=%.1fs\n",
               g_sw_group, g_sw_first, g_sw_key, g_sw_wall);
    else if (g_sw_n > 1)
        printf("[sweep] %sl%ld-l%ld x%u %s wall=%.1fs\n",
               g_sw_group, g_sw_first, g_sw_last, g_sw_n, g_sw_key, g_sw_wall);
    if (g_sw_n) fflush(stdout);
    g_sw_group[0] = g_sw_key[0] = '\0';
    g_sw_armed = false; g_sw_first = g_sw_last = -1; g_sw_n = 0; g_sw_wall = 0.0;
}

/* Report one finished configuration. `group` is the id without its column
   suffix ("r0b0", "rndb0"), `li` that suffix, so a collapsed run can name the
   span it covers. */
static void sweep_report(const char *group, long li, const BeamResult *br, double wall) {
    const bool filled = strcmp(br->reason, "extinct") != 0;
    char key[96];
    snprintf(key, sizeof key, "%s=%u width=%u reason=%s",
             filled ? "filled" : "died", br->row, br->width, sweep_reason(br));

    if (!g_verbose) {
        /* Compact log: a line for completions and two-segment partials; B-only
           partials are counted in the periodic progress line and the summary. */
        const size_t strong_partials = g_part_count[PART_AB]
                                     + g_part_count[PART_AC]
                                     + g_part_count[PART_BC];
        const bool productive = g_emit_count != 0 || strong_partials != 0;
        const bool exceptional = strcmp(br->reason, "extinct") != 0
                              && strcmp(br->reason, "stop_row") != 0;
        if (productive || exceptional) {
            printf("[sweep] %sl%ld %s", group, li, key);
            if (g_emit_count) printf(" emitted=%zu", g_emit_count);
            if (g_incomplete_top && g_partial_count)
                printf(" partials=%zu part_total=%zu", g_partial_count, g_partial_total);
            if (g_emit_count) printf(" sol_total=%" PRIu64, g_solution_idx);
            printf(" wall=%.1fs\n", wall);
            fflush(stdout);
            return;
        }

        double now = omp_get_wtime();
        if (g_progress_last == 0.0) g_progress_last = now;
        if (now - g_progress_last >= SWEEP_QUIET_SEC) {
            printf("[progress] %sl%ld configs=%" PRIu64
                   " stop=%" PRIu64 " boards=%" PRIu64,
                   group, li, g_stats.configs, g_stats.reached_stop,
                   g_stats.emitted_total);
            if (g_incomplete_top) printf(" partials=%zu", g_partial_total);
            printf(" deaths");
            for (int r = 1; r <= (int)g_stop_row; r++)
                if (g_stats.extinct_at[r])
                    printf(" r%d=%" PRIu64, r, g_stats.extinct_at[r]);
            printf("\n");
            fflush(stdout);
            g_progress_last = now;
        }
        return;
    }

    if (g_emit_count + g_partial_count == 0) {          /* nothing to report */
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

/* Data lines in a rotations CSV, counted with the same blank/comment convention
   read_one_border_row uses, but in one pass and without validating any field.
   Only called for --num_rows 0, so an ordinary run with an explicit count opens
   the CSV exactly as before. */
static uint32_t count_border_rows(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open rotation CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0; uint32_t n = 0; ssize_t len;
    while ((len = getline(&line, &sz, f)) >= 0) {
        bool nonempty = false;
        for (ssize_t i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '#' || ch == '%') break;
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') { nonempty = true; break; }
        }
        if (nonempty) n++;
    }
    free(line); fclose(f);
    return n;
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
        else if (!strcmp(argv[i], "--start_row")   && i+1 < argc) g_start_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--num_rows")    && i+1 < argc) g_num_rows = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples")     && i+1 < argc) g_samples = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--db_file")     && i+1 < argc) g_db_file = argv[++i];
        else if (!strcmp(argv[i], "--free_edges"))                g_free_edges = true;
        else if (!strcmp(argv[i], "--beam_width")  && i+1 < argc) g_beam_width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stop_row")    && i+1 < argc) g_stop_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand") && i+1 < argc) g_beam_expand = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand_row") && i+1 < argc) g_beam_expand_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_Mahalanobis") && i+1 < argc) g_lambda_maha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_J")    && i+1 < argc) g_lambda_J = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no_free_demand"))            g_free_demand = false;
        else if (!strcmp(argv[i], "--clue_center"))               g_clue_mask |= CLUE_CENTER;
        else if (!strcmp(argv[i], "--clue_corners"))              g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(argv[i], "--pin_clue")    && i+1 < argc) g_pin_clue = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--learn")       && i+1 < argc) g_learn_path = argv[++i];
        else if (!strcmp(argv[i], "--table")       && i+1 < argc) g_table_path = argv[++i];
        else if (!strcmp(argv[i], "--freq_model") && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "segment")) g_freq_model = FREQ_MODEL_SEGMENT;
            else if (!strcmp(m, "cell"))    g_freq_model = FREQ_MODEL_CELL;
            else fatal("--freq_model must be segment or cell");
        }
        else if (!strcmp(argv[i], "--bc_window")   && i+1 < argc) {
            unsigned nb = 0, nc = 0;
            /* Bounded above as well: b_left is B_TRY + nB - 1 as an int, so an
               absurd nB would overflow it negative and the B loop would never
               run -- an empty beam instead of a slow one. */
            if (sscanf(argv[++i], "%u,%u", &nb, &nc) != 2
                || nb < 1 || nc < 1 || nb > BC_WINDOW_MAX || nc > BC_WINDOW_MAX)
                fatal("--bc_window needs nB,nC in 1..%d (e.g. 3,2)", BC_WINDOW_MAX);
            g_bc_nB = nb; g_bc_nC = nc;
        }
        else if (!strcmp(argv[i], "--frac_rand")   && i+1 < argc) g_frac_rand = atof(argv[++i]);
        else if (!strcmp(argv[i], "--parent_cap")  && i+1 < argc) g_parent_cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool_factor") && i+1 < argc) g_pool_factor = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top_bottoms") && i+1 < argc) g_top_bottoms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--top_columns") && i+1 < argc) g_top_columns = atol(argv[++i]);
        else if (!strcmp(argv[i], "--tau_bottoms") && i+1 < argc) g_tau_bottoms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tau_columns") && i+1 < argc) g_tau_columns = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bail_columns") && i+1 < argc) g_bail_columns = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rng_seed")    && i+1 < argc) { unsigned long long s; if (!parse_u64_token(argv[++i], &s)) fatal("--rng_seed needs an integer"); g_master_seed = (uint64_t)s; g_seed_given = true; }
        else if (!strcmp(argv[i], "--time_limit")  && i+1 < argc) g_config_time_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--wall_time")       && i+1 < argc) g_max_wall_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_emitted")     && i+1 < argc) g_max_partials = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--resume"))          resume = true;
        else if (!strcmp(argv[i], "--print_cmd"))       g_print_cmd = true;
        else if (!strcmp(argv[i], "--verbose"))         g_verbose = true;
        else { fprintf(stderr, "Unknown argument: %s\n\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (g_nthreads <= 0) g_nthreads = omp_get_max_threads();
    /* An explicit --rng_seed 0 is a SUPPLIED seed, not an absent one. Keying the
       clock fallback off the value meant `--rng_seed 0` set g_seed_given and then
       got a clock/PID seed anyway -- which passes the --resume guard below
       while the border ranking it indexes is different on every run. */
    if (!g_seed_given) {
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
    /* No clue cap on --stop_row. Row 13's two clues are reserved, never pinned,
       and the attach in format_board_tail now yields to whatever the search
       placed -- so searching row 13 simply builds it from other pieces and the
       clues are left off, rather than being written on top of them. */
    if (!(g_tau_bottoms >= 0.0 && g_tau_bottoms <= 1e6)) fatal("--tau_bottoms in [0,1e6]");
    if (!(g_tau_columns >= 0.0 && g_tau_columns <= 1e6)) fatal("--tau_columns in [0,1e6]");
    if (g_frac_rand < 0.0 || g_frac_rand > 1.0) fatal("--frac_rand must be in [0,1]");
    if (g_pool_factor == 0) g_pool_factor = 1;
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
        fatal("--resume with --tau_bottoms/--tau_columns needs the "
              "original --rng_seed (the checkpoint indexes a seed-dependent ordering)");

    /* --num_rows 0 means "the rest of the file" (fixed/rotation mode only; under
       --random_edges there is no file to exhaust, and --samples governs that
       mode's count instead). Resolved before the banner so [cfg] and --print_cmd
       report the count the run will really use. */
    if (!g_random_edges && g_num_rows == 0) {
        uint32_t rot_lines = count_border_rows(csv_path);
        g_num_rows = (rot_lines > g_start_row) ? rot_lines - g_start_row : 0;
    }

    /* --pin_clue: narrow the search to ONE of the four clue frames.
     *
     * g_clue_orients has always been read by the search -- the reserve loop in
     * beam_init_border, init_clue_tables, clue_dump_schedule, expand_clue_row,
     * expand_row and clue_row_pinned all gate on it -- but nothing ever assigned
     * it, so every clued run hedged over all four. Pinning is therefore a
     * SIMPLIFICATION and not a new mechanism: expand_clue_row stops expanding
     * each parent once per enabled orientation, and the beam stops holding
     * boards from four different frames for the dedup hash to keep apart.
     *
     * It turns on the CENTER clue and nothing else. Whether to also demand the
     * corner clues is a separate question with a separate price -- g_db_exclude
     * is gated per clue, so --clue_corners takes four more pieces out of the
     * chain database -- so that stays an explicit flag.
     *
     * Resolved here for the same reason --num_rows is, just above: the banner
     * and --print_cmd must report the frame the run will really use. */
    if (g_pin_clue) {
        g_clue_mask   |= CLUE_CENTER;
        g_clue_orients = (uint8_t)(1u << clue_orient_for_pin(g_pin_clue));
    }

    /* --learn and --table are separate runs on purpose: that is what lets every
       other flag keep the meaning it already has in whichever run it is passed
       to, instead of needing a phase-qualified twin. */
    if (g_learn_path && g_table_path)
        fatal("--learn and --table are separate runs: pass one or the other");
    /* A table is a calibration for ONE border. These three would each corrupt it
       silently rather than loudly, which is the only reason they are checked
       here instead of being left to the user. */
    if (g_learn_path && g_num_rows != 1)
        fatal("--learn needs --num_rows 1: the table records one rotations row as "
              "its provenance and its border block is keyed by which side that row "
              "deals each edge to, so folding several rows into one table would "
              "mix incompatible side assignments (--num_rows 0, the default, means "
              "every remaining row of the file)");
    if (g_table_path && g_num_rows != 1)
        fatal("--table needs --num_rows 1: a table belongs to one border");
    if (g_learn_path && resume)
        fatal("--resume cannot resume a --learn run: the checkpoint stores "
              "(border row, bottom, column) but not which of the four passes wrote "
              "it, so resuming would restart at pass 0 and re-skip bottoms in every "
              "pass. Re-run the learning phase from the start; it writes no boards, "
              "so it is cheap to repeat");
    if (g_learn_path && g_free_edges)
        fatal("--learn cannot use --free_edges: the border block assumes each edge "
              "piece has a fixed side, which is exactly what a rotations row "
              "decides and --free_edges releases");
    g_learning   = (g_learn_path != NULL);
    g_freq_debug = (getenv("E555_FREQ_DEBUG") != NULL);
    g_freq_pure  = (getenv("E555_FREQ_PURE") != NULL);

    printf("\n=== E555 beamer (data-driven fork) ===\n\n");
    if (g_print_cmd) print_cmd(argv[0], seed_path, csv_path, resume);
    printf("[cfg] seed_file=%s rotations_file=%s out_dir=%s\n",
           seed_path, csv_path ? csv_path : "(none: --random_edges)", g_out_dir);
    if (g_random_edges)
        printf("[cfg] rng_seed=%" PRIu64 " threads=%d verbose=%d random_edges=%d free_edges=%d samples=%u\n",
               g_master_seed, g_nthreads, g_verbose?1:0, g_random_edges?1:0, g_free_edges?1:0,
               g_samples);
    else
        printf("[cfg] rng_seed=%" PRIu64 " threads=%d verbose=%d random_edges=%d free_edges=%d start_row=%u num_rows=%u\n",
               g_master_seed, g_nthreads, g_verbose?1:0, g_random_edges?1:0, g_free_edges?1:0,
               g_start_row, g_num_rows);
    printf("[cfg] incomplete_top=%d resume=%d corners BL/BR/TL/TR=%d/%d/%d/%d\n",
           g_incomplete_top?1:0, resume?1:0, g_fixed_corner_pid[0], g_fixed_corner_pid[1],
           g_fixed_corner_pid[2], g_fixed_corner_pid[3]);
    printf("[cfg] beam_width=%u stop_row=%u expand=%ux@row%u\n",
           g_beam_width, g_stop_row, g_beam_expand, g_beam_expand_row);
    printf("[cfg] frac_rand=%.2f parent_cap=%u pool_factor=%u\n",
           g_frac_rand, g_parent_cap, g_pool_factor);
    if (g_clue_mask) {
        /* Which frame, not just which clues. This used to read "(all 4
           orientations)" unconditionally, which --pin_clue would have turned
           into a lie -- and a run in the wrong frame looks exactly like a run
           in the right one, so the banner is the only place it shows. */
        printf("[cfg] clue_center=%d clue_corners=%d ",
               (g_clue_mask & CLUE_CENTER) ? 1 : 0, (g_clue_mask & CLUE_CORNERS) ? 1 : 0);
        if (g_pin_clue) {
            int o = clue_orient_for_pin(g_pin_clue);
            printf("(pin_clue %d: centre clue at %s, row %u col %u; orientation %d of 4) ",
                   g_pin_clue, clue_pin_quadrant_name(g_pin_clue),
                   g_clue[o][0].row, g_clue[o][0].col, o);
        } else {
            printf("(all 4 orientations) ");
        }
        printf("pinned_rows=");
        /* Which rows the clues actually constrain, printed because it is not
           the rows they sit on: a clue pins the row BELOW it too, to the colour
           it will stand on. --clue_corners names cells on row 2 and bites at
           row 1, and a sweep that dies there is otherwise a mystery. */
        const char *sep = "";
        for (int r = 1; r <= (int)g_stop_row; r++)
            if (clue_row_pinned(r)) { printf("%s%d", sep, r); sep = ","; }
        printf("%s (a clue pins its own row and the one below it)\n", *sep ? "" : "none");
    }
    printf("[cfg] lambda_J=%.3f lambda_Maha=%.3f free_demand=%d bc_window=%u,%u\n",
           g_lambda_J, g_lambda_maha, g_free_demand?1:0, g_bc_nB, g_bc_nC);
    printf("[cfg] top_bottoms=%ld top_columns=%ld time_limit=%.0fs wall_time=%.0fs max_emitted=%" PRIu64 " db_file=%s\n",
           g_top_bottoms, g_top_columns, g_config_time_sec, g_max_wall_sec, g_max_partials,
           g_db_file ? g_db_file : "(none)");
    printf("[cfg] tau_bottoms=%.2f tau_columns=%.2f bail_columns=%u\n",
           g_tau_bottoms, g_tau_columns, g_bail_columns);
    if (g_learn_path || g_table_path)
        printf("[cfg] beam_freq_model=%s-preservation "
               "border_freq_model=location smoothing=KT-1/2\n",
               g_freq_model == FREQ_MODEL_CELL ? "cell" : "segment");
    fflush(stdout);

    double t_start = omp_get_wtime();
    ensure_dir(g_out_dir);
    load_seed_and_catalog(seed_path);
    build_catalog_indices();
    build_inner_color_totals();
    init_check_inner_faces();
    build_maha_tables();
    build_logtab();
    /* Needs the oriented catalog and nothing else, so it runs BEFORE the build:
       a malformed clue table is then caught in milliseconds rather than after
       eighty seconds of database. */
    init_clue_tables();
    clue_dump_schedule();

    /* ---- data-driven setup ------------------------------------------------ */
    if (g_learn_path || g_table_path) {
        if (g_random_edges)
            fatal("--learn/--table need a rotations file: the table is tied to one "
                  "border's corner assignment, which --random_edges resamples");
        if (!(g_clue_mask & CLUE_CENTER) || g_pin_clue < 1 || g_pin_clue > 4)
            fatal("--learn/--table need --clue_center and --pin_clue 1..4: without a "
                  "pinned clue frame a board's orientation is not readable, and the "
                  "four passes cannot be folded into one table");
        g_orient_base = clue_orient_for_pin(g_pin_clue);
        g_centre_cell = g_clue[g_orient_base][0].row * PUZZLE_SIDE
                      + g_clue[g_orient_base][0].col;

        /* The canonical rotations row, and the turn checked before it is used:
           four clockwise quarter-turns must be the identity. */
        if (!read_one_border_row(csv_path, g_start_row, g_spin0))
            fatal("--learn/--table: rotations row %u not found in %s", g_start_row, csv_path);
        uint8_t chk[NUM_PIECES];
        memcpy(chk, g_spin0, sizeof chk);
        for (int t = 0; t < 4; t++) rot_row_turn(chk, 1);
        if (memcmp(chk, g_spin0, sizeof chk) != 0)
            fatal("the rotations turn is not of order 4 -- refusing to learn from it");
        for (int t = 1; t < 4; t++) {       /* and each turn really moves the row */
            memcpy(chk, g_spin0, sizeof chk);
            rot_row_turn(chk, t);
            if (memcmp(chk, g_spin0, sizeof chk) == 0)
                fatal("turning the rotations row %d quarter-turn(s) changed nothing", t);
        }
        freq_mark_free();
        printf("[freq] canonical frame: orientation %d (--pin_clue %d), centre clue "
               "on cell %d (row %d col %d); %d free interior pieces\n",
               g_orient_base, g_pin_clue, g_centre_cell,
               g_centre_cell / PUZZLE_SIDE, g_centre_cell % PUZZLE_SIDE,
               g_n_free_inner_pieces);
    }
    if (g_learn_path) { freq_alloc(); manifest_add(g_learn_path); }
    if (g_table_path) freq_load(g_table_path, seed_path, csv_path);

    /* Clue pieces leave the DATABASE, so no chain can hold one and the search
       never rejects a chain for colliding with a clue.

       Set AFTER build_inner_color_totals: g_inner_color_total must stay the
       WHOLE inner set. A clue piece is still placed on the board and consumed
       there, so subtracting it from the totals leaves parity_ok's
       S = total - consumed - required short by that piece's half-edges, which
       flips S's parity and rejects perfectly sound boards.

       Set BEFORE the cache is consulted: the exclude hash is what stops a clue
       cache being reused for a normal run, and back. */
    if (g_clue_mask) {
        for (int k = 0; k < CLUE_N; k++) {
            bool is_center = (k == 0);
            if (is_center ? !(g_clue_mask & CLUE_CENTER) : !(g_clue_mask & CLUE_CORNERS)) continue;
            for (int o = 0; o < 4; o++) used_set(g_db_exclude, g_clue[o][k].piece);
        }
    }

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
    verify_segment_enumerator();
    verify_column_rotation();

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
        /* Random-border sweep: no rotation CSV. --samples random bottoms,
           --top_columns random left columns per bottom, each border the best of
           RANDOM_SIDE_SAMPLES fan-out-ranked samples (--top_bottoms is moot:
           every sampled bottom is used). No checkpoint: borders are not
           re-derivable, so a run is continued simply by starting another. */
        printf("\n========== random borders ==========\n");
        size_t run_b = (g_samples == 0) ? SIZE_MAX : g_samples;
        size_t run_l = g_top_columns  >= 1 ? (size_t)g_top_columns : 1;
        printf("[sweep] random mode: %zu bottoms x %zu lefts (best of %d samples each)\n",
               run_b, run_l, RANDOM_SIDE_SAMPLES); fflush(stdout);

        finalize_fixed_corners();          /* validate/resolve any pinned corners */
        htable_init();
        if (g_incomplete_top) partial_dedup_init();
        char comp_path[1024];
        snprintf(comp_path, sizeof comp_path, "%s/beam_completions_random_%u.csv", g_out_dir, g_stop_row);
        g_completions_fp = fopen(comp_path, "a");
        if (!g_completions_fp) fatal("cannot open %s: %s", comp_path, strerror(errno));
        setvbuf(g_completions_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] completions -> %s (append)\n", comp_path);
        manifest_add(comp_path);
        if (g_incomplete_top) {
            char part_base[1024];
            snprintf(part_base, sizeof part_base,
                     "%s/beam_completions_random_%u", g_out_dir, g_stop_row);
            partial_outputs_open_base(part_base);
        }
        fflush(stdout);
        g_solution_idx = 0;

        RNG srng = rng_for(g_master_seed, 0xB07D0135u, 0, 0);
        BottomOrder bot; LeftOrder lft;
        for (size_t bi = 0; bi < run_b && !g_stop; bi++) {
            if (!sample_random_bottom(&srng, g_tau_bottoms, &bot))
                fatal("random bottom sampling failed; seed edge pool too constrained");
            validate_color_constants();
            g_border_stats.bottoms_ranked++;
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = 0; li < run_l && !g_stop; li++) {
                if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) { printf("[sweep] max_wall reached.\n"); g_stop = 1; break; }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                bool got_left = false;
                double clue_t0 = omp_get_wtime();
                unsigned left_tries = (g_clue_mask & CLUE_CORNERS) ? 64u : 1u;
                for (unsigned tr = 0; tr < left_tries; tr++) {
                    if (!sample_random_left(&srng, g_tau_columns, &bot, &lft)) break;
                    g_border_stats.columns_ordinary_viable++;
                    if (row1_corner_compatible(&bot, &lft)) {
                        g_border_stats.columns_clue_compatible++;
                        got_left = true;
                        break;
                    }
                }
                g_border_stats.clue_seconds += omp_get_wtime() - clue_t0;
                if (!got_left) {
                    g_border_stats.bottoms_no_clue_column++;
                    if (g_verbose)
                        fprintf(stderr, "[warn] no clue-compatible left column sampled "
                                "for random bottom %zu\n", bi);
                    break;
                }
                g_border_stats.columns_run++;
                g_cur_bottom = &bot;
                g_cur_left   = &lft;
                snprintf(g_config_id_str, sizeof g_config_id_str, "rndb%zul%zu", bi, li);
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                if (g_incomplete_top) partial_config_reset();
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                /* emitted/partials are this config's unique boards; sol_total and
                   part_total are the run totals written so far. */
                { char grp[64]; snprintf(grp, sizeof grp, "rndb%zu", bi);
                  sweep_report(grp, (long)li, &br, omp_get_wtime()-tc0); }
                if (g_completions_fp) fflush(g_completions_fp);
                partial_outputs_flush();
                partials_budget_announce();
                barren = (g_emit_count + g_part_count[PART_AB]
                          + g_part_count[PART_AC] + g_part_count[PART_BC] > 0)
                         ? 0 : barren + 1;
                if (g_bail_columns && barren >= g_bail_columns) {
                    sweep_flush();
                    if (g_verbose) {
                        printf("[bail] rndb%zu: %u column(s) in a row emitted nothing, "
                               "moving to the next bottom\n", bi, barren);
                        fflush(stdout);
                    }
                    break;
                }
            }
            sweep_flush();      /* never straddle a bottom */
        }
    } else
    for (g_pass = 0; g_pass < (g_learning ? FREQ_NPASS : 1) && !g_stop; g_pass++)
    for (uint32_t cur_row = g_start_row; cur_row < g_start_row + g_num_rows; cur_row++) {
        if (g_stop) break;
        if (g_learning)
            printf("\n========== learning pass %d of %d (rotations row turned %d "
                   "quarter-turn(s) clockwise, clue orientation %d) ==========\n",
                   g_pass + 1, FREQ_NPASS, g_pass, (g_orient_base + g_pass) & 3);
        printf("\n========== border row %u ==========\n", cur_row); fflush(stdout);

        uint8_t spins[NUM_PIECES];
        if (!read_one_border_row(csv_path, cur_row, spins)) { printf("[sweep] border row %u not found; stopping.\n", cur_row); break; }
        /* The row as the file has it (before this pass's turn), with its Stage A
           comment, so the log alone is enough to rebuild the rotations file. */
        {
            char *row_txt, *cmt_txt;
            if (read_border_row_text(csv_path, cur_row, &row_txt, &cmt_txt)) {
                printf("[border] %s row %u, as in the file%s:\n", csv_path, cur_row,
                       g_pass ? " (unturned)" : "");
                if (cmt_txt) printf("%s\n", cmt_txt);
                printf("%s\n", row_txt);
                fflush(stdout);
            }
            free(row_txt); free(cmt_txt);
        }
        /* Turn the border and the clue frame together: the same anchored puzzle,
           relabelled, so the pass grows from what was another side of it. */
        rot_row_turn(spins, g_pass);
        if (g_clue_mask) g_clue_orients = (uint8_t)(1u << ((g_orient_base + g_pass) & 3));
        memcpy(g_spin, spins, sizeof g_spin);
        /* Search mode keeps the stock meaning exactly -- measured from t_start,
           so the database build counts against it as it always has. */
        g_pass_deadline = (g_max_wall_sec <= 0.0) ? 0.0
            : (g_learning ? omp_get_wtime() + g_max_wall_sec / FREQ_NPASS
                          : t_start + g_max_wall_sec);
        if (g_learning && g_max_wall_sec > 0.0)
            printf("[sweep] pass %d has %.0fs of the %.0fs budget\n",
                   g_pass, g_max_wall_sec / FREQ_NPASS, g_max_wall_sec);

        classify_deal_from_rotations();
        build_top_border_demands();
        if (!g_free_edges) { build_edge_terminal_pool(); build_db_edge_and_sort(); }
        validate_color_constants();

        enumerate_bottoms();
        enumerate_lefts();
        /* Separate streams per border row, so the bottoms drawn for one row do
           not depend on how many columns the previous row happened to enumerate. */
        RNG brng = rng_for(g_master_seed, cur_row + (uint32_t)g_pass * 65536u, 0xB0770D15u, 0);
        if (g_freq_on && g_freq_border_ok) freq_rank_bottoms(g_tau_bottoms, &brng);
        else                               rank_bottoms(g_tau_bottoms, &brng);

        size_t nb = g_bottom_n, nl = g_left_n;
        corner_compat_cache_init(nb);
        size_t run_b = (g_top_bottoms >= 1 && (size_t)g_top_bottoms < nb) ? (size_t)g_top_bottoms : nb;
        size_t cap_l = (g_top_columns >= 1 && (size_t)g_top_columns < nl) ? (size_t)g_top_columns : nl;
        /* LEARNING TAKES THE NUMBER OF SAMPLES IT ASKED FOR, EVEN FROM A SMALL POOL.
         *
         * An annealed border is rich in Euler trails on some sides and poor on
         * others, and each pass puts a different side at the bottom: 480 / 25920 /
         * 46080 / 432 on borders_annealed_fix12.csv row 1. Stopping at the pool
         * therefore hands the two poor sides a fraction of the evidence of the two
         * rich ones -- and the poor sides are the interesting ones, because a piece
         * that fits where there is little freedom is a piece that really has to go
         * there. So when the pool is smaller than --top_bottoms, go round it again.
         *
         * A second lap is not a re-run. Everything that makes one configuration's
         * beam differ from another's comes from cfg_hash, which is derived from the
         * configuration id string, and from lrng, which is keyed by the loop counter
         * -- and the id carries the lap number while the counter keeps rising. So
         * each lap draws a fresh --frac_rand band and, at --tau_columns > 0, a fresh
         * column ordering, and explores a different interior from the same border.
         * Without that the repeat would be bit-identical, every board would be
         * dropped by the per-row dedup, and the configuration would not even be
         * counted -- which is exactly what makes the fresh stream the whole point.
         * Search mode stops at the pool. */
        if (g_learning && g_top_bottoms >= 1 && (size_t)g_top_bottoms > nb)
            run_b = (size_t)g_top_bottoms;
        if (g_learning) {
            g_ppool[g_pass] = nb;
            g_preps[g_pass] = nb ? (run_b + nb - 1) / nb : 0;
        }
        printf("[sweep] bottoms=%zu (run %zu%s)  left-cols=%zu enumerated, up to %zu per bottom\n",
               nb, run_b,
               run_b > nb ? " -- pool cycled with fresh random streams" : "",
               nl, cap_l); fflush(stdout);
        if (nb == 0 || nl == 0) fatal("no border configs for row %u", cur_row);

        htable_init();
        if (g_incomplete_top) partial_dedup_init();
        if (g_completions_fp) fclose(g_completions_fp);
        g_completions_fp = NULL;
        partial_outputs_close();
        if (!g_learning) {
        char comp_path[1024];
        snprintf(comp_path, sizeof comp_path, "%s/beam_completions_%u_%u.csv", g_out_dir, cur_row, g_stop_row);
        g_completions_fp = fopen(comp_path, "a");
        if (!g_completions_fp) fatal("cannot open %s: %s", comp_path, strerror(errno));
        setvbuf(g_completions_fp, NULL, _IOFBF, EMIT_FILE_BUF);
        printf("[out] completions -> %s (append)\n", comp_path);
        manifest_add(comp_path);
        if (g_incomplete_top) {
            char part_base[1024];
            snprintf(part_base, sizeof part_base,
                     "%s/beam_completions_%u_%u", g_out_dir, cur_row, g_stop_row);
            partial_outputs_open_base(part_base);
        }
        }   /* !g_learning: the learning phase counts boards, it writes none */
        fflush(stdout);

        size_t start_bi = 0, start_li = 0;
        if (g_resume_active && cur_row == g_start_row) { start_bi = g_resume_bi; start_li = g_resume_li; g_solution_idx = g_resume_sol_idx; }
        else g_solution_idx = 0;

        bool pass_done = false;
        for (size_t bi = start_bi; bi < run_b && !g_stop && !pass_done; bi++) {
            /* bi is the sample SLOT, which may exceed the pool; b_idx is the
               bottom it selects and b_rep the lap. Keying the streams on bi rather
               than b_idx is what gives each lap its own randomness, and costs
               nothing on the first lap, where the two are equal. */
            const size_t b_idx = bi % nb;
            const size_t b_rep = bi / nb;
            char blab[56];
            if (b_rep) snprintf(blab, sizeof blab, "r%ub%zu#%zu", cur_row, b_idx, b_rep);
            else       snprintf(blab, sizeof blab, "r%ub%zu", cur_row, b_idx);

            /* A column's rank is conditional on the bottom (left_rank_of), so the
               ranking belongs here, not once per border row. Its stream is keyed
               by bi so each sample draws its own and --resume re-derives the same
               ordering for the bottom it re-enters. */
            RNG lrng = rng_for(g_master_seed, cur_row + (uint32_t)g_pass * 65536u, 0x1EF7C015u, (uint32_t)bi);
            size_t distinct = 0;
            size_t ordinary_viable = (g_freq_on && g_freq_border_ok)
                ? freq_rank_lefts(&g_bottoms[b_idx], g_tau_columns, &lrng, &distinct)
                : rank_lefts(&g_bottoms[b_idx], g_tau_columns, &lrng, &distinct);
            double clue_t0 = omp_get_wtime();
            size_t viable = clue_filter_ranked_lefts(b_idx, &g_bottoms[b_idx],
                                                      ordinary_viable);
            g_border_stats.clue_seconds += omp_get_wtime() - clue_t0;
            size_t run_l = viable < cap_l ? viable : cap_l;
            g_border_stats.columns_run += run_l;
            if (g_verbose) {
                printf("[rank] %s: columns %zu -> %zu ordinary -> %zu clue-compatible, "
                       "%zu distinct ordinary rank(s), run %zu\n",
                       blab, nl, ordinary_viable, viable, distinct, run_l);
                fflush(stdout);
            }
            if (run_l == 0) {                /* no clue-compatible row 1 */
                if (!g_learning) write_checkpoint(ckpath, cur_row, (uint32_t)(bi+1), 0);
                continue;
            }
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = (bi == start_bi ? start_li : 0); li < run_l && !g_stop; li++) {
                if (pass_time_spent()) {
                    printf("[sweep] %s reached.\n",
                           g_learning ? "pass wall budget" : "max_wall");
                    if (!g_learning) g_stop = 1;
                    pass_done = true; break;
                }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                g_cur_bottom = &g_bottoms[b_idx];
                g_cur_left   = &g_lefts[li];
                snprintf(g_config_id_str, sizeof g_config_id_str, "%sl%zu", blab, li);
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));
                if (g_learning)     /* the four passes must not share a stream */
                    cfg_hash = splitmix64(cfg_hash ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(g_pass + 1)));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                if (g_incomplete_top) partial_config_reset();
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_pass_deadline > 0.0 && g_pass_deadline < slice_end) slice_end = g_pass_deadline;

                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                if (g_learning) freq_close_config();
                /* emitted/partials are this config's unique boards; sol_total and
                   part_total are the run totals written so far. */
                { char grp[64]; snprintf(grp, sizeof grp, "%s", blab);
                  sweep_report(grp, (long)li, &br, omp_get_wtime()-tc0); }
                if (g_completions_fp) fflush(g_completions_fp);
                partial_outputs_flush();
                partials_budget_announce();
                if (!g_learning) write_checkpoint(ckpath, cur_row, (uint32_t)bi, (uint32_t)(li+1));
                barren = (g_emit_count + g_part_count[PART_AB]
                          + g_part_count[PART_AC] + g_part_count[PART_BC] > 0)
                         ? 0 : barren + 1;
                if (g_bail_columns && barren >= g_bail_columns) {
                    sweep_flush();
                    if (g_verbose) {
                        printf("[bail] %s: %u column(s) in a row emitted nothing, "
                               "moving to the next bottom\n", blab, barren);
                        fflush(stdout);
                    }
                    /* Point the checkpoint at the next bottom, not at the column
                       we stopped on -- otherwise --resume walks back into the
                       bottom we just abandoned and undoes the saving. */
                    if (!g_learning) write_checkpoint(ckpath, cur_row, (uint32_t)(bi+1), 0);
                    break;
                }
            }
            sweep_flush();      /* never straddle a bottom, or the next [rank] */
        }
        g_resume_active = false;
    }

    sweep_flush();
    if (g_learning) {
        double *cnt   = xmalloc(FREQ_TSZ * sizeof(double));
        double *wcell = xmalloc(NUM_PIECES * sizeof(double));
        double *wsq   = xmalloc(NUM_PIECES * sizeof(double));
        freq_merge(cnt, wcell, wsq);
        freq_write(g_learn_path, seed_path, csv_path, cnt, wcell, wsq);
        size_t covered = 0, thin = 0, nfree = 0;
        size_t cmin = (size_t)-1, cmax = 0;
        for (int j = 0; j < FREQ_NPASS; j++) {
            if (g_pconfigs[j] < cmin) cmin = g_pconfigs[j];
            if (g_pconfigs[j] > cmax) cmax = g_pconfigs[j];
        }
        if (!cmax) cmax = 1;
        for (int x = 0; x < NUM_PIECES; x++) nfree += g_cell_free[x] ? 1 : 0;
        for (int x = 0; x < NUM_PIECES; x++) {
            if (wcell[x] <= 0.0) continue;
            covered++;
            if (wcell[x] * wcell[x] / wsq[x] < 30.0) thin++;   /* configurations */
        }
        printf("\n[learn] passes:");
        for (int j = 0; j < FREQ_NPASS; j++) printf(" p%d=%zu", j, g_pconfigs[j]);
        printf(" configurations\n");
        for (int j = 0; j < FREQ_NPASS; j++) {
            if (!g_ppool[j]) continue;
            printf("[learn]   pass %d: %zu configurations from a pool of %zu bottoms",
                   j, g_pconfigs[j], g_ppool[j]);
            if (g_preps[j] > 1)
                printf(", cycled %zu times -- laps share a border, so they sharpen "
                       "each border's estimate rather than adding independent ones",
                       g_preps[j]);
            printf("\n");
        }
        printf("[learn] cells covered %zu of %d free (%zu backed by fewer than 30 "
               "independent configurations)\n", covered, (int)nfree, thin);
        printf("[learn] pass balance: the thinnest pass carries %.0f%% of the "
               "configurations of the fattest%s\n", 100.0 * (double)cmin / (double)cmax,
               (cmin * 4 < cmax) ? "  <-- raise --top_bottoms" : "");
        /* The one number that says whether any of this carries signal. Scored
           prequentially: a board is measured against a table that holds only
           configurations finished before its own. */
        printf("[learn] prequential segment lift %.4f nats/cell over %zu boards%s\n",
               g_lift_cells > 0.0 ? g_lift_sum / g_lift_cells : 0.0, g_lift_boards,
               g_lift_cells > 0.0 && g_lift_sum / g_lift_cells <= 0.0
                   ? "  <-- NO SIGNAL: the table is not worth searching with" : "");
        printf("[learn] table -> %s\n", g_learn_path);
        free(cnt); free(wcell); free(wsq);
    }
    double wall = omp_get_wtime() - t_start;
    print_summary(wall, init_s, omp_get_wtime() - t_sweep0);
    if (g_completions_fp) fclose(g_completions_fp);
    partial_outputs_close();
    /* After the closes: the manifest reports which files GREW, so their buffers
       have to have reached the disk before it stats them. */
    manifest_write(g_out_dir);
    return 0;
}
