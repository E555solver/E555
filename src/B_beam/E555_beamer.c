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
 *   are sampled at random from the seed's edge pieces (the best of 32 random
 *   draws each by fan-out rank, drawn biased and filtered under
 *   --exhaust_border_color), so the search can run indefinitely,
 *   generating partials over ever-fresh border combinations;
 *   --exhaust_border_color makes each such border use up one frame colour
 *   within the rows it writes. Each configuration
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
 *     SELECT      pooled children are deduplicated by frontier signature (keeping
 *                 the best score per signature), ranked, and pruned to the row's
 *                 effective width by a score band with a per-parent offspring cap
 *                 plus a random band whose share follows the frac_rand schedule.
 *     MATERIALIZE survivors become the next beam; their moves go to the ancestry
 *                 log from which emitted boards are reconstructed.
 *   A beam whose child pool comes up EMPTY is abandoned. That is a HEURISTIC
 *   extinction, not a proof: the generator keeps a bounded number of children
 *   per segment-A record, spends a bounded quota per parent, and starts from an
 *   already-pruned beam, so an empty pool means only that this bounded search
 *   found no child from the states it still held. Boards reaching --stop_row
 *   (default 11) are appended to the completions CSV, best-scored first, up to
 *   EMIT_MAX per config -- no lookahead is applied at the stop row; whether a
 *   row fits above it is deliberately left to the next stage.
 *
 * BACKTRACKING (--backtrack_row N)
 *   The beam stops at row N, whose best --backtrack_row_factor x width boards
 *   (0: every candidate, raw) are then searched exhaustively, cell by cell in
 *   row-major order, up to --stop_row: parity at every row and clue pins, and
 *   under --lambda_corners at least one top corner still closable (a TL or TR
 *   block alive) at every row. Every completing board is emitted, exact
 *   duplicates aside and without the EMIT_MAX cap -- first extended column by
 *   column above the stop row, from its deepest zero-break prefix
 *   (--extend_nodes, --backtrack_min_col). See
 *   backtrack_emit and extend_board. A stop-row board whose top row nearly
 *   repeats the previous one's is dropped first (--no_top_dedup keeps it); under
 *   --backtrack_min_col K a partial row whose column 1 can no longer be
 *   stacked to row 14 is cut on the spot (col_reach), which drops no written
 *   board.
 *
 * END DIVES (--end_dive M --emit_score S)
 *   Instead of being written, each stop-row board is completed to 256 pieces
 *   by M greedy random dives that allow broken edges (the backtracker's stuck
 *   mode), in two stages, the second learning from each board's best dives;
 *   --end_polish R then hill-climbs and kicks the best ones. The best
 *   completion per board is written if it has >= S connected edges, sorted
 *   by score. The engine is src/C_tail/E555_dive.c.
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
 *       E555_database.c ../C_tail/E555_dive.c E555_beamer.c -o E555_beamer -lm
 *   ./E555_beamer --help
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <omp.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "E555_beamer.h"
#include "../C_tail/E555_dive.h"

/* Beam-row segment-B retries: if a conflict-free B chain admits no conflict-free
   C completion, try the next conflict-free B, up to this many, before giving up
   on the A record. Deep rows are conflict-dominated; without the fallback a
   viable A chain can be lost to one unlucky B pick. */
#define B_TRY 4

/* Largest --bc_window side. Nothing is sized by it -- the window keeps one
   running best, not an array -- but b_left is B_TRY + nB - 1 in an int, so an
   unbounded nB would overflow it negative and the B loop would never run, which
   would empty the beam silently instead of merely slowing it. */
#define BC_WINDOW_MAX 128

/* Stop-row emission safety cap (lines per config; the CSV rows are ~2 KB). */
#define EMIT_MAX 1000000u

static uint64_t g_master_seed = 0;

static uint32_t g_beam_width      = 250000;
static uint32_t g_stop_row        = 11;
/* --backtrack_row N (0 = off): the beam stops at row N, and its roots are
   searched exhaustively, cell by cell, up to --stop_row. */
static uint32_t g_backtrack_row   = 0;
/* --backtrack_row_factor M: row N is an ordinary beam row and its best
   M x (row width) boards are the roots; 0 = every row-N candidate, expanded
   as a stop row and kept raw (the behaviour before the flag). */
static uint32_t g_bt_factor       = 2;
/* --extend_nodes N: every board the backtracker completes at --stop_row is
   extended by an exhaustive column-major search over rows stop+1..14, cols
   1..14, of at most N nodes, and written from its deepest zero-break prefix
   (0 = off). --backtrack_min_col K writes only boards whose extension fills
   at least K whole columns. See extend_board. */
static uint32_t g_extend_nodes    = 100000;
static bool     g_cap_top         = true;    /* --cap_top: close the top-left border exactly */
static uint32_t g_bt_min_col      = 0;
/* --free_top_clue (with --clue_corners): the extension treats the two row-13
   clue cells as ordinary ones and may place the top-LEFT clue (13,2) on any
   cell above the stop row. The top-RIGHT clue (13,13) stays held through the
   extension, so it never lands in the left columns, and is then left off the
   board for the dives to place among the open top-right cells. Rows up to the
   stop row hold both in reserve. */
static bool     g_free_top_clue   = false;
/* The row-13 clue entries of g_clue: top-left on (13,2), top-right on (13,13). */
#define CLUE_TOP_LEFT  3
#define CLUE_TOP_RIGHT 4
/* A backtracker pin beside PIN_PIECE/PIN_TOPCOLOR (E555_database.h): the
   colour the cell must show to its RIGHT (bt_band_pins). */
#define PIN_RIGHTCOLOR 2
/* The top-row near-duplicate filter (--no_top_dedup turns it off): a stop-row
   board whose top row repeats the previous one's columns 1..K and differs from
   it in at most one other cell is dropped before its extension. See
   bt_close_row. */
static bool     g_top_dedup       = true;
/* Under --backtrack_min_col K the backtracker cuts a partial row as soon as
   its column 1 can no longer be stacked to row 14, and a stop-row cell of
   columns 2..K whose top nothing can sit on. Set from K; see col_reach. */
static bool     g_col_check       = false;
/* --backtrack_unlock_right R (0 = off): the backtracker's board is the block
   rows 0..S x columns 0..W, W = 15-R. Each root gives back columns W+1..15 of
   rows 1..N, rows N+1..S are searched over columns 1..W only, and the layered
   extension (layer_extend) grows the band around the block. g_last_inner is
   the last column a searched row fills (W; 14 when off) and g_block_mask the
   columns a written board carries up to the stop row (all of them when off),
   so the default path reads the values it always did. */
static uint32_t g_unlock_right    = 0;
static int      g_last_inner      = EDGE_LEN;
static uint16_t g_block_mask      = ROWMASK_FULL;
/* --clue_corners: the backtracker pins the row-13 clues too (bt_band_pins). */
static bool     g_band_pins       = false;
/* E555_ROOT_DEDUP=0 (test only): search every root, repeats included. */
static bool     g_root_dedup      = true;
static bool     g_cap_top_set     = false;
/* --prefix: written in front of the configuration id in every board name. */
static char     g_prefix[40]      = "";
/* Boards found at the stop row and written, this configuration's and the
   run's (cfg_count_written). */
static uint64_t g_cfg_found = 0, g_cfg_written = 0, g_found_total = 0, g_written_total = 0;
static char     g_board_id_str[140] = "c0";

static bool     g_backtrack_row_set = false;
static bool     g_bt_factor_set = false;
static bool     g_extend_set = false, g_min_col_set = false;
/* The row the beam treats as its last: the stop row, or the backtrack row,
   whose candidates (or, under --backtrack_row_factor M > 0, its best M x width
   boards) are the backtracker's roots. */
static inline uint32_t gen_stop_row(void) { return g_backtrack_row ? g_backtrack_row : g_stop_row; }
/* Is `row` expanded as a stop row (every completion, no lookahead, raw
   ranking)? The stop row always is; the backtrack row only under
   --backtrack_row_factor 0, otherwise it is an ordinary beam row whose
   selection gives the roots. */
static inline bool row_is_stop(uint32_t row) {
    return row == g_stop_row || (g_backtrack_row && row == g_backtrack_row && g_bt_factor == 0);
}
/* --end_dive M (0 = off): stop-row boards are completed by M random dives
   each and emitted by score (>= --emit_score S) instead of written as found. */
#define DV_M_DEFAULT 10000         /* --end_dive without a value */
static uint32_t g_end_dive       = 0;
static int      g_emit_score     = 450;
static int      g_end_polish     = -1;    /* --end_polish R; -1 = off */
static int      g_corner_seeds   = 4;     /* --corner_seeds N; 0 = off */
static bool     g_emit_score_set = false;
/* Does --backtrack_min_col drop boards? Under --backtrack_unlock_right it counts
   the band columns allowed to stay open, so K = R (its default there) drops
   none. */
static inline bool mincol_cuts(void) {
    return g_unlock_right ? g_bt_min_col < g_unlock_right : g_bt_min_col > 0;
}
/* --emit_score as a floor on written boards without --end_dive (unlock only). */
static inline bool score_cuts(void) {
    return g_unlock_right && g_emit_score_set && !g_end_dive;
}
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
static double   g_bc_accept       = 1.0;    /* --bc_window_accept F: P(keep the best) */
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
static bool     g_exhaust          = false;   /* --exhaust_border_color */
static uint64_t g_exhaust_cfgs[MAX_EDGE_SIDE_COLOR + 1];  /* configs run, by colour used up (0 = plain) */
static int      g_exhaust_ok[MAX_EDGE_SIDE_COLOR];        /* colours that can be used up at all */
static int      g_exhaust_ok_n = 0;
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
   ("r<row>b<bottom>l<column>"), which -Wformat-truncation checks. */
static char     g_config_id_str[96] = "c0";

/* --exhaust_border_color: a random bottom for a frame colour drawn per bottom
   (g_exhaust_colour), kept only if a left column can then use the colour up.
   The colour is drawn from those that can be used up at all (decided once at
   startup); colour 2 is accepted only 1 time in 5, because on seed_Edge5 it
   admits just two corner placements and its borders repeat the same edge set.
   After EXHAUST_TRIES failed colours the bottom is a plain random one
   (g_exhaust_colour = 0), so sampling luck never stops a run. The colour stays
   set for every left column sampled for this bottom. */
#define EXHAUST_TRIES 4
/* --random_edges: repeated (bottom, column) draws tolerated before the exhaust
   colour is dropped for the bottom, or, with none to drop, the bottom is left. */
#define BORDER_REPEAT_TRIES 16
static bool sample_exhaust_bottom(RNG *rng, BottomOrder *bot) {
    for (int t = 0; t < EXHAUST_TRIES && g_exhaust_ok_n; t++) {
        int c;
        do c = g_exhaust_ok[rng_uniform(rng, (uint32_t)g_exhaust_ok_n)];
        while (c == 2 && rng_uniform(rng, 5) != 0);
        g_exhaust_colour = c;
        if (!sample_random_bottom(rng, g_tau_bottoms, bot)) continue;
        LeftOrder probe;
        if (sample_random_left(rng, g_tau_columns, bot, &probe)) return true;
    }
    g_exhaust_colour = 0;
    return sample_random_bottom(rng, g_tau_bottoms, bot);
}

/* A run code for a bare --prefix: 6 characters of [A-Z0-9] from the kernel's
   random source (clock and pid if that fails), never from the master RNG, so
   runs under --rng_seed stay reproducible. */
static void run_code(char out[7]) {
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint8_t b[6];
    if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) {
        uint64_t x = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL ^ (uint64_t)getpid() << 17
                   ^ (uint64_t)clock();
        for (int k = 0; k < 6; k++) { x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ULL; b[k] = (uint8_t)(x >> 56); }
    }
    for (int k = 0; k < 6; k++) out[k] = al[b[k] % 36];
    out[6] = '\0';
}

/* The name a written board carries in its first cell: the configuration id,
   behind the --prefix when one is set. */
static void set_board_id(void) {
    if (g_prefix[0]) snprintf(g_board_id_str, sizeof g_board_id_str, "%s_%s", g_prefix, g_config_id_str);
    else             snprintf(g_board_id_str, sizeof g_board_id_str, "%s", g_config_id_str);
}

/* Emit dedup table (open addressing, power-of-2; emission is serial). */
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
/* The first SIGINT/SIGTERM finishes the configuration and the summary; the
   second kills at once. */
static void handle_stop(int sig) { g_stop = 1; signal(sig, SIG_DFL); }

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
    uint64_t columns_repeated;      /* --random_edges: a run border drawn again */
    uint64_t plain_fallbacks;       /* ... so often the exhaust colour was dropped */
    uint64_t bottoms_left;          /* ... so often the bottom was abandoned */
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
    /* With --end_dive the budget caps the sorted dived output instead. */
    if (g_max_partials == 0 || g_stop || g_end_dive) return false;
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

/* --backtrack_unlock_right: commit only columns 1..W of a row. Columns W+1..15
   stay empty, so their tops keep the values below them (the row-0 tops under
   the band stay owed), the column-W piece's right face is owed to the band, and
   no right edge is taken: the edges given back stay owed through the pool count
   of beam_init_border. Every colour still moves by an even amount (that right
   face is consumed and owed at once), so parity_ok applies as it is. */
static void commit_row_narrow(BeamEntry *p, int row, const RowChoice *rc) {
    const int W = g_last_inner;
    for (int i = 0; i < W; i++) {
        const Oriented *o = &g_cat[rc->ci[i]];
        used_set(p->used, o->piece_id);
        p->color_consumed[INNER_IDX(o->top)]++;    p->color_consumed[INNER_IDX(o->right)]++;
        p->color_consumed[INNER_IDX(o->bottom)]++; p->color_consumed[INNER_IDX(o->left)]++;
        const int old_t = p->rtop[i + 1];
        if (color_is_inner(old_t)) p->req_exposed[INNER_IDX(old_t)]--;
        p->req_exposed[INNER_IDX(o->top)]++;
        p->rtop[i + 1] = o->top;
    }
    p->rtop[0] = g_cur_left->p[row]->top;
    p->req_exposed[INNER_IDX(g_cat[rc->ci[W - 1]].right)]++;
    if (!g_free_edges || g_free_demand) {
        const int la = g_cur_left->right[row];     /* segment-A interface satisfied */
        if (color_is_inner(la)) p->req_exposed[INNER_IDX(la)]--;
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

/* A backtracker row: the whole row, or columns 1..W under --backtrack_unlock_right. */
static inline void bt_commit(BeamEntry *p, int row, const RowChoice *rc) {
    if (g_unlock_right) commit_row_narrow(p, row, rc);
    else                commit_row(p, row, rc);
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

/* --random_edges: every (bottom, left column) pair run so far, keyed by
   border_key, so that a run never searches the same border twice. */
static uint64_t *g_border_seen = NULL;
static size_t    g_border_seen_sz = 0, g_border_seen_n = 0;

static uint64_t border_key(const BottomOrder *b, const LeftOrder *l) {
    uint64_t h = 0xB0DE5EE4C0FFEE77ULL;
    for (int i = 0; i < PUZZLE_SIDE; i++)
        h = splitmix64(h ^ ((uint64_t)b->p[i]->piece_id << 2 | b->p[i]->rotation));
    for (int i = 0; i < PUZZLE_SIDE; i++)
        h = splitmix64(h ^ ((uint64_t)l->p[i]->piece_id << 2 | l->p[i]->rotation) ^ 0x10000);
    return h ? h : 1;
}
static bool border_seen(uint64_t k) {
    if (!g_border_seen_sz) return false;
    size_t h = (size_t)k & (g_border_seen_sz - 1);
    while (g_border_seen[h]) {
        if (g_border_seen[h] == k) return true;
        h = (h + 1) & (g_border_seen_sz - 1);
    }
    return false;
}
static void border_seen_add(uint64_t k) {
    if ((g_border_seen_n + 1) * 2 > g_border_seen_sz) {
        size_t nsz = g_border_seen_sz ? g_border_seen_sz * 2 : 1024;
        uint64_t *nt = xmalloc(nsz * sizeof(uint64_t));
        memset(nt, 0, nsz * sizeof(uint64_t));
        for (size_t i = 0; i < g_border_seen_sz; i++) {
            uint64_t x = g_border_seen[i];
            if (!x) continue;
            size_t h = (size_t)x & (nsz - 1);
            while (nt[h]) h = (h + 1) & (nsz - 1);
            nt[h] = x;
        }
        free(g_border_seen); g_border_seen = nt; g_border_seen_sz = nsz;
    }
    size_t h = (size_t)k & (g_border_seen_sz - 1);
    while (g_border_seen[h]) {
        if (g_border_seen[h] == k) return;
        h = (h + 1) & (g_border_seen_sz - 1);
    }
    g_border_seen[h] = k; g_border_seen_n++;
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
            if (!((g_block_mask >> c) & 1u)) continue;   /* a band column: never read */
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
   top row is empty, which is what makes an attached row-13 clue isolated, and
   so is every band column under --backtrack_unlock_right. */
static inline bool cell_is_placed(int r, int c, int row, uint16_t colmask) {
    if (r < 0 || r > row || !((g_block_mask >> c) & 1u)) return false;
    return (r < row) || ((colmask >> c) & 1u) != 0;
}

/* A stop-row board's extension above its top row (extend_board): n catalog
   orientations, the k-th on cell[k]. Under --random_edges the extension also
   chooses column 0 above the stop row: an index ci >= CATALOG_SIZE is then the
   frame-left edge g_edge_left[ci - CATALOG_SIZE]. */
#define EXT_MAX (EDGE_LEN * (EDGE_LEN + 1))
/* --cap_top: up to the TL corner and 14 top edges on row 15. */
typedef struct {
    uint8_t  n;
    uint8_t  cell[PUZZLE_SIDE], rot[PUZZLE_SIDE];
    uint16_t pid[PUZZLE_SIDE];
} Cap;
/* raw: layer_extend's result, which may reach any open cell, frame included:
   ci[k] is then a piece id and rot[k] its spin. */
typedef struct {
    uint16_t n;
    bool     raw;
    uint8_t  cell[NUM_PIECES];
    uint16_t ci[NUM_PIECES];
    uint8_t  rot[NUM_PIECES];
    Cap      cap;
} Ext;
static const Oriented *g_edge_left = NULL;    /* --random_edges: every edge, frame left */
static int             g_edge_left_n = 0;
static const Oriented *g_edge_up = NULL;      /* every edge, frame up (--cap_top) */
static uint8_t         g_up_by_in[NUM_COLORS_TOTAL][MAX_EDGE_TERMINALS];   /* by inner colour */
static uint8_t         g_up_by_in_n[NUM_COLORS_TOTAL];
static inline const Oriented *ext_or(uint16_t ci) {
    return ci < CATALOG_SIZE ? &g_cat[ci] : &g_edge_left[ci - CATALOG_SIZE];
}

/* Colour a piece at spin s shows on side d (0 top, 1 right, 2 bottom, 3 left). */
static inline int seed_side(uint32_t pid, uint32_t s, int d) {
    const int k = (int)((d + s) & 3u);
    return k == 0 ? g_seed_top[pid] : k == 1 ? g_seed_right[pid]
         : k == 2 ? g_seed_bottom[pid] : g_seed_left[pid];
}

/* Matched edges of a board given as per-piece position/rotation vectors: the
   junctions between two placed cells whose colours agree, out of 480. */
static int board_score(const uint32_t pos[NUM_PIECES], const uint32_t rot[NUM_PIECES]) {
    int16_t at[NUM_PIECES];
    for (int x = 0; x < NUM_PIECES; x++) at[x] = -1;
    for (int p = 0; p < NUM_PIECES; p++) if (pos[p] < NUM_PIECES) at[pos[p]] = (int16_t)p;
    int n = 0;
    for (int x = 0; x < NUM_PIECES; x++) {
        if (at[x] < 0) continue;
        const uint32_t p = (uint32_t)at[x];
        if (x % PUZZLE_SIDE < PUZZLE_SIDE - 1 && at[x + 1] >= 0) {
            const uint32_t q = (uint32_t)at[x + 1];
            n += seed_side(p, rot[p], 1) == seed_side(q, rot[q], 3);
        }
        if (x + PUZZLE_SIDE < NUM_PIECES && at[x + PUZZLE_SIDE] >= 0) {
            const uint32_t q = (uint32_t)at[x + PUZZLE_SIDE];
            n += seed_side(p, rot[p], 0) == seed_side(q, rot[q], 2);
        }
    }
    return n;
}

/* Flatten rows[0..row] into per-piece position/rotation vectors and write them
   as the 512 comma-separated fields that follow a line's prefix. colmask says
   which columns of the TOP row carry a piece (ROWMASK_FULL for a completed stop
   row, or AB/AC/BC/B for an --incomplete_top partial); every row below it is
   full either way. A mask rather than a last-placed column because the partial
   kinds leave a hole in the MIDDLE of the row, not only at its right end.
   board_arrays fills the per-piece vectors; format_board_tail writes them and
   returns the byte count. */
static void board_arrays(const RowChoice rows[EDGE_LEN], int row, uint16_t colmask,
                         int orient, const Ext *ext,
                         uint32_t pos[NUM_PIECES], uint32_t rot_arr[NUM_PIECES]) {
    for (int i = 0; i < NUM_PIECES; i++) { pos[i] = 999; rot_arr[i] = 0; }
    for (int r = 0; r <= row; r++) {
        uint16_t m = ((r == row) ? colmask : ROWMASK_FULL) & g_block_mask;
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!((m >> c) & 1u)) continue;
            uint16_t pid; uint8_t rot; board_cell(rows, r, c, &pid, &rot);
            pos[pid] = (uint32_t)(r * PUZZLE_SIDE + c); rot_arr[pid] = rot;
        }
    }
    bool ext_cell[NUM_PIECES] = { false };
    if (ext) {
        for (int k = 0; k < ext->n; k++) {
            if (ext->raw) {
                pos[ext->ci[k]] = ext->cell[k]; rot_arr[ext->ci[k]] = ext->rot[k];
            } else {
                const Oriented *o = ext_or(ext->ci[k]);
                pos[o->piece_id] = ext->cell[k]; rot_arr[o->piece_id] = o->rotation;
            }
            ext_cell[ext->cell[k]] = true;
        }
        for (int k = 0; k < ext->cap.n; k++) {          /* --cap_top, on row 15 */
            pos[ext->cap.pid[k]] = ext->cap.cell[k]; rot_arr[ext->cap.pid[k]] = ext->cap.rot[k];
        }
    }
    /* --clue_corners: also place the two row-13 clue pieces the beam never
       reaches, so every emitted board carries all the clues its orientation
       holds -- the viewer shows the corners pinned, and Stage C (and the end
       dives) must build around them. On a row-12 board each touches row 12
       with a junction the search did not choose; that edge may break, and it
       is scored like any other. Only a searched row 13 (--stop_row 13) fills
       their cells, and then they are left off. There is no choice of
       orientation here -- the board committed to one when it placed its row-2
       corners.

       Under --free_top_clue the extension treats those cells as ordinary ones
       and may have put the top-left clue anywhere above the stop row, or
       another piece on its cell. If it did not place it, the clue goes home
       only onto an empty cell whose placed neighbours it matches, so the board
       stays break-free; otherwise it is left for the dives. The top-right clue
       was held through the extension and is always left for the dives, which
       place it among the open cells -- the top-right corner the column-major
       extension leaves -- rather than on its own cell.

       Under --backtrack_unlock_right every corner clue whose cell is in the
       band goes home the same way, (2,13) included: the layered extension
       builds around it, and the search pins the stop-row cells under it. */
    if (orient >= 0 && (g_clue_mask & CLUE_CORNERS))
        for (int k = g_unlock_right ? 1 : CLUE_TOP_LEFT; k <= CLUE_TOP_RIGHT; k++) {
            const ClueCell *cc = &g_clue[orient][k];
            const int x = cc->row * PUZZLE_SIDE + cc->col;
            if (cell_is_placed(cc->row, cc->col, row, colmask)) continue;
            if (g_free_top_clue && k == CLUE_TOP_RIGHT) continue;   /* left for the dives */
            if (pos[cc->piece] != 999) continue;          /* the extension placed it */
            if (ext_cell[x]) continue;                     /* another piece holds the cell */
            if (g_free_top_clue) {
                const Oriented *o = &g_cat[g_clue_ci[orient][k]];
                bool fits = true;
                for (int d = 0; d < 4 && fits; d++) {
                    const int rr = cc->row + (d == 0) - (d == 2), cc2 = cc->col + (d == 1) - (d == 3);
                    const int y = rr * PUZZLE_SIDE + cc2;
                    int face = -1;                         /* the neighbour's facing colour */
                    if (cell_is_placed(rr, cc2, row, colmask) && cc2 > 0 && cc2 < PUZZLE_SIDE - 1) {
                        uint16_t pid; uint8_t rt; board_cell(rows, rr, cc2, &pid, &rt);
                        face = seed_side(pid, rt, (d + 2) & 3);
                    } else if (ext_cell[y]) {
                        for (int j = 0; j < ext->n; j++)
                            if (ext->cell[j] == y) { face = seed_side(ext_or(ext->ci[j])->piece_id,
                                                                     ext_or(ext->ci[j])->rotation, (d + 2) & 3); break; }
                    }
                    const int mine = d == 0 ? o->top : d == 1 ? o->right : d == 2 ? o->bottom : o->left;
                    if (face >= 0 && face != mine) fits = false;
                }
                if (!fits) continue;
            }
            pos[cc->piece]     = (uint32_t)x;
            rot_arr[cc->piece] = cc->spin;
        }
    /* Rotations mode: the rest of the fixed frame the search held -- the whole
       left column (cBL..cTL, one matched Euler trail) and the top-right corner.
       The config chose them before the first row and kept them out of the beam
       (beam_init_border puts all 17 in the used set), so no searched cell can
       hold them and none of them touches a searched cell with an edge the
       search did not score: above the top row their only placed neighbours are
       each other. Written only where the board is still empty, so rows the
       search placed are emitted exactly as before. Not under --random_edges,
       where the frame is a sample rather than an input, and not under
       --backtrack_unlock_right, whose band leaves that frame and the three
       corners free. */
    if (!g_random_edges && !g_unlock_right) {
        for (int r = 0; r < PUZZLE_SIDE; r++) {
            if (cell_is_placed(r, 0, row, colmask)) continue;
            const Oriented *o = g_cur_left->p[r];
            if (pos[o->piece_id] != 999)
                fatal("left-column piece %u (row %d) is already on the board",
                      o->piece_id, r);
            pos[o->piece_id]     = (uint32_t)(r * PUZZLE_SIDE);
            rot_arr[o->piece_id] = o->rotation;
        }
        const int tr = PUZZLE_SIDE * PUZZLE_SIDE - 1;
        if (!cell_is_placed(PUZZLE_SIDE - 1, PUZZLE_SIDE - 1, row, colmask)) {
            if (pos[g_cTR.piece_id] != 999)
                fatal("top-right corner %u is already on the board", g_cTR.piece_id);
            pos[g_cTR.piece_id]     = (uint32_t)tr;
            rot_arr[g_cTR.piece_id] = g_cTR.rotation;
        }
    }
}

static int format_board_tail(const RowChoice rows[EDGE_LEN], int row,
                             uint16_t colmask, int orient, const Ext *ext, char *out) {
    uint32_t pos[NUM_PIECES], rot_arr[NUM_PIECES];
    board_arrays(rows, row, colmask, orient, ext, pos, rot_arr);
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
    int len = format_board_tail(rows, row, colmask, orient, NULL, line);

    #pragma omp critical(e555_incomplete)
    {
        if (*fp && partial_dedup_accept(keys, n)) {
            g_part_total[kind]++;
            g_part_count[kind]++;
            g_partial_count++;
            uint64_t sol_idx = g_partial_total++;
            fprintf(*fp, "%s, %" PRIu64, g_board_id_str, sol_idx);
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

static double g_d2n_acc[MAX_ACC_THREADS][ACC_STRIDE];   /* [0]=sum [1]=sumsq [2]=n */
/* The current configuration's estimate of a row drives the next row. Where a
   row is too thin to measure, scoring falls back to the run-pooled estimate of
   that row rather than to whichever earlier configuration happened to run
   last: a configuration is its own regime (its border fixes the colour supply),
   and a stale spread from another one scales the correction wrongly. */
static double g_maha_local_mean[EDGE_LEN + 2];
static double g_maha_local_sd[EDGE_LEN + 2];
static double g_maha_local_n[EDGE_LEN + 2];
static double g_maha_pool_sum[EDGE_LEN + 2];
static double g_maha_pool_sumsq[EDGE_LEN + 2];
static double g_maha_pool_n[EDGE_LEN + 2];

static inline void maha_acc(double d2n) {
    int th = omp_get_thread_num();
    if (th >= MAX_ACC_THREADS) return;          /* absurd --threads: stand down */
    g_d2n_acc[th][0] += d2n;
    g_d2n_acc[th][1] += d2n * d2n;
    g_d2n_acc[th][2] += 1.0;
}

/* A new configuration starts without a local estimate of any row. */
static void maha_reset_config(void) {
    memset(g_maha_local_mean, 0, sizeof g_maha_local_mean);
    memset(g_maha_local_sd,   0, sizeof g_maha_local_sd);
    memset(g_maha_local_n,    0, sizeof g_maha_local_n);
}

/* Reduce the row's accumulators and reset them. Summed in a FIXED thread order,
   so the result does not depend on how the row's work happened to be scheduled;
   it still depends on the thread COUNT, as every other float sum here does. The
   row's samples always join the run pool; the configuration's own estimate is
   replaced only when the row had enough of them. */
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
static inline double color_term(const BeamEntry *t, int row) {
    double s = g_lambda_J * closure_raw(t);
    if (g_lambda_maha != 0.0) {
        double d2n = maha_d2n(t, row);
        maha_acc(d2n);
        double mean = 0.0, sd = 0.0;
        if (row >= 2 && maha_calibration(row - 1, &mean, &sd))
            s += g_lambda_maha * (d2n - mean) / sd;
    }
    return s;
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

/* -- Rotations-row echo ------------------------------------------------------ */

/* Rotations row `want`, verbatim, and the comment line nearest above it (the
   Stage A "#  TOP=.. RIGHT=.. BOTTOM=.. LEFT=..  Score=.." line), so the log
   carries the border it was run on: if the rotations file is lost, the row can
   be rebuilt from the log. Nothing is recomputed -- the trail counts and score
   are echoed as the file states them.

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

/* ===========================================================================
 * Top-corner supply  (--lambda_corners)
 * ===========================================================================
 * The model, the exact block catalogs, the alive counting and the stop-row
 * report are the shared tc_* code in E555_database.c/.h (see the header for
 * the templates and the step score). The beamer keeps what is its own: the
 * flag and the per-configuration calibration of u_row.
 *
 * u_row is the standard deviation of the rest of the score, so lambda is in
 * score-SD units like --lambda_Mahalanobis. Here it is taken from this
 * configuration's previous row first, then that row pooled over the run, then
 * this row pooled over earlier configurations, else one nat.
 * =========================================================================== */

static double g_lambda_corners = 0.0;   /* --lambda_corners [X]; 0 = off */
static bool   g_corners_on     = false;
static double g_corner_u = 1.0;          /* u_row for the row being expanded */

/* --lambda_reserve F: keep the double-decker TOP reserve for last. Stage A
   --double_decker marks the pieces it reserves for row 14 in the rotations row
   (spin 1 = TOP, 2 = BOTTOM, 3 = LEFT or RIGHT); every TOP piece a board has placed costs F
   score-SDs, in the same u_row unit as --lambda_corners, so the two terms add
   and either can run alone. Nothing is held: a board may still spend a reserve
   piece when nothing else fits, and the dives treat the reserve as free. The
   mask is filled per border row by reserve_row, also with F = 0, so the
   stop-row report can say how much of the reserve an unpenalised run spends. */
static double   g_lambda_reserve = 0.0;
static bool     g_aux_on = false;       /* either term on: u_row is measured */
static uint64_t g_top_mask[4];          /* this border row's TOP reserve */
static int      g_top_n = 0;
static uint64_t g_rsv_boards = 0, g_rsv_free_sum = 0, g_rsv_marked_sum = 0;

/* TOP reserve pieces a board has NOT placed yet. */
static inline int top_free(const uint64_t used[4]) {
    int n = 0;
    for (int k = 0; k < 4; k++) n += __builtin_popcountll(used[k] & g_top_mask[k]);
    return g_top_n - n;
}

/* The TOP reserve of one border row: the inner pieces with spin 1, read only
   when the Stage A comment's `Decker=a/b/c/d` (TOP/RIGHT/BOTTOM/LEFT, `-` for
   a classic side) shows TOP two tall. Another two-tall side marks its pieces
   2 (BOTTOM) or 3 (LEFT/RIGHT); an older row marked every side with 1, so a
   row with another two-tall side but no piece coded 2 or 3 cannot be split,
   and gets no mask, with a note. */
static void reserve_row(uint32_t row, const char *comment, const uint8_t spins[NUM_PIECES]) {
    memset(g_top_mask, 0, sizeof g_top_mask);
    g_top_n = 0;
    const char *d = NULL;
    for (const char *q = comment ? strstr(comment, "Decker=") : NULL; q; q = strstr(q + 1, "Decker="))
        if (q == comment || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '#') { d = q + 7; break; }
    if (!d) return;
    bool two[4] = { false, false, false, false };
    int n = 0;
    for (; n < 4 && *d && *d != ' ' && *d != '\t' && *d != '\n' && *d != '\r'; n++) {
        two[n] = *d != '-';
        while (*d && *d != '/' && *d != ' ' && *d != '\t' && *d != '\n' && *d != '\r') d++;
        if (*d == '/') d++;
    }
    if (n != 4 || !two[0]) return;
    bool coded = false;
    for (int i = 0; i < EXPECTED_INNER; i++) coded |= spins[g_inner_ids[i]] >= 2;
    if ((two[1] || two[2] || two[3]) && !coded) {
        printf("[reserve] border row %u: marks from an older --double_decker with another "
               "two-tall side -- the TOP reserve cannot be told apart, so it is not tracked\n", row);
        return;
    }
    for (int i = 0; i < EXPECTED_INNER; i++) {
        const int pid = g_inner_ids[i];
        if (spins[pid] != 1) continue;
        used_set(g_top_mask, (uint16_t)pid);
        g_top_n++;
    }
    printf("[reserve] border row %u: %d TOP reserve piece(s)%s\n", row, g_top_n,
           g_lambda_reserve > 0.0 ? ", each placed one costs --lambda_reserve" : ", tracked only");
    fflush(stdout);
}

static inline void rsv_tally(int free_n) {
    if (!g_top_n) return;
    g_rsv_boards++; g_rsv_free_sum += (uint64_t)free_n; g_rsv_marked_sum += (uint64_t)g_top_n;
}

static double g_cu_acc[MAX_ACC_THREADS][ACC_STRIDE];
static double g_cu_local_sd[EDGE_LEN + 2], g_cu_local_n[EDGE_LEN + 2];
static double g_cu_pool_sum[EDGE_LEN + 2], g_cu_pool_sumsq[EDGE_LEN + 2],
              g_cu_pool_n[EDGE_LEN + 2];

static void corner_reset_config(void)
{
    memset(g_cu_local_sd, 0, sizeof g_cu_local_sd);
    memset(g_cu_local_n,  0, sizeof g_cu_local_n);
}

static void corner_close_row(int row)
{
    if (!g_aux_on) return;
    double sum = 0.0, sumsq = 0.0, n = 0.0;
    for (int th = 0; th < MAX_ACC_THREADS; th++) {
        sum += g_cu_acc[th][0]; sumsq += g_cu_acc[th][1]; n += g_cu_acc[th][2];
        g_cu_acc[th][0] = g_cu_acc[th][1] = g_cu_acc[th][2] = 0.0;
    }
    if (row < 0 || row > EDGE_LEN + 1 || n <= 0.0) return;
    g_cu_pool_sum[row] += sum; g_cu_pool_sumsq[row] += sumsq; g_cu_pool_n[row] += n;
    if (n < MAHA_MIN_SAMPLES) return;
    double m = sum / n, v = sumsq / n - m * m;
    g_cu_local_sd[row] = (v > 1e-18) ? sqrt(v) : 0.0;
    g_cu_local_n[row]  = n;
}

static double corner_pooled_sd(int row)
{
    if (row < 0 || row > EDGE_LEN + 1) return 0.0;
    double n = g_cu_pool_n[row];
    if (n < MAHA_MIN_SAMPLES) return 0.0;
    double m = g_cu_pool_sum[row] / n, v = g_cu_pool_sumsq[row] / n - m * m;
    return (v > 1e-18) ? sqrt(v) : 0.0;
}

static double corner_unit(int row)
{
    int src = row - 1;
    if (src >= 0 && g_cu_local_n[src] >= MAHA_MIN_SAMPLES && g_cu_local_sd[src] > 0.0)
        return g_cu_local_sd[src];
    double sd = corner_pooled_sd(src);
    if (sd > 0.0) return sd;
    sd = corner_pooled_sd(row);
    return sd > 0.0 ? sd : 1.0;
}

/* The corner and reserve terms. `pre` is the rest of the child's score, which
   is what u_row measures the spread of -- sampled once here, before either
   term, so each keeps the unit it would have alone. The board's clue frame
   picks the corner catalog. */
static inline double aux_term(const BeamEntry *t, int row, bool at_stop, double pre)
{
    int th = omp_get_thread_num();
    if (th >= 0 && th < MAX_ACC_THREADS) {
        g_cu_acc[th][0] += pre; g_cu_acc[th][1] += pre * pre; g_cu_acc[th][2] += 1.0;
    }
    double s = 0.0;
    if (g_corners_on) {
        const bool at12 = at_stop && row == PUZZLE_SIDE - 4;
        const int orient = ENTRY_HAS_ORIENT(t) ? (int)ENTRY_ORIENT(t) : -1;
        s += g_lambda_corners * g_corner_u
           * (double)tc_step_sum(orient, t->used, t->rtop, at12);
    }
    if (g_lambda_reserve > 0.0 && g_top_n)
        s -= g_lambda_reserve * g_corner_u * (double)(g_top_n - top_free(t->used));
    return s;
}

/* The emitted board's used set and exposed tops, for the stop-row report. */
static uint8_t corner_emit_code(const BeamEntry *parent, const RowChoice *mv,
                                int row, int orient)
{
    uint64_t used[4];
    uint8_t rtop[PUZZLE_SIDE];
    memcpy(used, parent->used, sizeof used);
    rtop[0] = g_cur_left->p[row]->top;
    for (int i = 0; i < EDGE_LEN; i++) {
        used_set(used, g_cat[mv->ci[i]].piece_id);
        rtop[1 + i] = g_cat[mv->ci[i]].top;
    }
    used_set(used, g_edge_term[mv->rterm].piece_id);
    rtop[PUZZLE_SIDE - 1] = g_edge_term[mv->rterm].top;
    return tc_board_code(orient, used, rtop, row == PUZZLE_SIDE - 4);
}

/* ====================== end top-corner supply ============================== */

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
                                  uint32_t nA, uint64_t fB, uint64_t fC) {
    double s = log((double)nA * (1.0 + (double)fB) * (1.0 + (double)fC))
               + color_term(t, row);
    if (g_aux_on) s += aux_term(t, row, false, s);
    return (float)s;
}

/* The stop row has no lookahead, so the colour term alone ranks it. */
static inline float score_stop(const BeamEntry *t, int row) {
    double s = color_term(t, row);
    if (g_aux_on) s += aux_term(t, row, true, s);
    return (float)s;
}

/* Full scoring for a child that already exists: the same gate as
   child_lookahead, read off the materialized frontier instead of the chunks. */
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

    *out = score_scanned(t, row, cA->n, fB, fC);
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

/* g_lefts is already sorted by its fan-out rank.  Stable
   compaction retains that order while dropping columns that cannot satisfy both
   row-1 corner-clue colors.  A bottom with no survivors is thereby eliminated
   before a beam workspace is touched. */
static size_t clue_filter_ranked_lefts(size_t b_idx, const BottomOrder *bot,
                                       size_t ordinary_viable) {
    g_border_stats.bottoms_ranked++;
    g_border_stats.columns_ordinary_viable += ordinary_viable;
    const bool clues = (g_clue_mask & CLUE_CORNERS) != 0;
    if (!clues && !g_corners_on) {
        g_border_stats.columns_clue_compatible += ordinary_viable;
        return ordinary_viable;
    }

    if (g_left_filter_cap < g_left_n) {
        g_left_filter_buf = xrealloc(g_left_filter_buf,
                                     g_left_n * sizeof(*g_left_filter_buf));
        g_left_filter_cap = g_left_n;
    }

    size_t out = 0, corner_dead = 0;
    /* Compatible ordinary-viable entries go first.  Rejected viable entries are
       retained after them, and the ordinary-dead suffix follows unchanged.  The
       next bottom therefore still ranks the complete, uncorrupted left-order set.
       Under --lambda_corners a column must also end in a pair some TL block can
       use: the column fixes (0,14) and (0,13), so any other column can never
       place the (13,2) clue legally. */
    for (size_t i = 0; i < ordinary_viable; i++) {
        if (clues && !row1_corner_compatible_cached(b_idx, bot, &g_lefts[i])) continue;
        if (!g_corners_on || tc_left_ok(&g_lefts[i])) g_left_filter_buf[out++] = g_lefts[i];
        else corner_dead++;
    }
    size_t tail = out;
    for (size_t i = 0; i < ordinary_viable; i++)
        if ((clues && !row1_corner_compatible_cached(b_idx, bot, &g_lefts[i]))
            || (g_corners_on && !tc_left_ok(&g_lefts[i])))
            g_left_filter_buf[tail++] = g_lefts[i];
    g_tc_columns_dead += corner_dead;
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
                    if (!parity_ok(t)) continue;
                    float score;
                    if (at_stop) {
                        score = score_stop(t, row);
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
                float score = score_stop(t, e->row);
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
    /* The runner-up, for --bc_window_accept F < 1. */
    RowChoice sec_mv; float sec_score = 0.0f; uint64_t sec_sig = 0;
    uint8_t sec_flags = 0;
    bool have_sec = false;
    const bool track_sec = window && g_bc_accept < 1.0;
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
        if (!pick_segB(cB, jb, rt + 6, forbidA, ciB, la_B, &la_C)) continue;
        b_left--;
        if (!color_is_inner(la_C)) continue;
        const Cell *cC = g_db[INNER_IDX(la_C)][INNER_IDX(rt[11])][INNER_IDX(rt[12])]
                             [INNER_IDX(rt[13])][INNER_IDX(rt[14])][rt[15]];
        if (!cC) continue;
        uint64_t maskB[4]; mask_of_chain(ciB, CHAIN_LEN, maskB);
        uint64_t forbidB[4] = { forbidA[0]|maskB[0], forbidA[1]|maskB[1],
                                forbidA[2]|maskB[2], forbidA[3]|maskB[3] };
        memcpy(&mv.ci[CHAIN_LEN], ciB, CHAIN_LEN * sizeof(uint16_t));  /* B is fixed here */
        uint32_t nc_done = 0;
        const uint32_t nC_lim = e->keep_all ? cC->n : g_bc_nC;
        for (uint32_t jc = 0; jc < cC->n && nc_done < nC_lim && e->quota > 0; jc++) {
            uint16_t ciC[CHAIN_LEN-1]; uint8_t rterm;
            if (!pick_segC(cC, jc, rt + 11, forbidB, ciC, la_C, &rterm)) continue;
            memcpy(&mv.ci[2*CHAIN_LEN], ciC, (CHAIN_LEN-1) * sizeof(uint16_t));
            mv.rterm = rterm;
            /* Lookahead first: an exact one-row death costs nothing to find here,
               where it used to cost a board copy, commit_row and a parity scan. */
            uint32_t nA; uint64_t fB, fC;
            if (!child_lookahead(mv.ci, rterm, e->row, &nA, &fB, &fC)) continue;
            *t = *p; commit_row(t, e->row, &mv);
            if (!parity_ok(t)) continue;
            float score = score_scanned(t, e->row, nA, fB, fC);
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
                if (track_sec && have_best) {
                    sec_score = best_score; sec_mv = best_mv; sec_sig = best_sig;
                    sec_flags = best_flags; have_sec = true;
                }
                best_score = score; best_mv = mv; best_sig = frontier_sig(t);
                best_flags = t->flags;
                have_best = true;
            } else if (track_sec && (!have_sec || score > sec_score)) {
                sec_score = score; sec_mv = mv; sec_sig = frontier_sig(t);
                sec_flags = t->flags; have_sec = true;
            }
        }
        if (nc_done) nb_done++;                 /* a B chain that produced a child */
    }
    if (have_sec) {
        /* Keyed on the record, not drawn from e->rng: the choice is the same
           however the parent is sliced over threads, and the phase-2 stream is
           untouched. */
        RNG r = rng_for(e->cfg_hash ^ 0xACCE97ULL, (uint32_t)e->row, e->parent_idx, j);
        double u = (double)(rng_next(&r) >> 11) * (1.0 / 9007199254740992.0);
        if (u >= g_bc_accept) {
            best_score = sec_score; best_mv = sec_mv; best_sig = sec_sig;
            best_flags = sec_flags;
        }
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
    /* At least 8 children per parent (the pool target uses the base width, so
       the floor is what keeps the expanded rows' pool at 8 per board), but
       never more than --pool_factor asks: --pool_factor 1 keeps 1. */
    const uint32_t quota_floor = g_pool_factor < 8 ? g_pool_factor : 8;
    if (quota_parent < quota_floor) quota_parent = quota_floor;
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
    const bool at_stop = row_is_stop((uint32_t)row);
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
        e.cfg_hash = cfg_hash;

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
    corner_close_row(row);      /* likewise u_row, under --lambda_corners */
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

/* The backtrack row's candidates bypass FRONTIER deduplication: each is the
   root of an exhaustive search, and two roots sharing a frontier reach the
   same upper rows under different lower ones -- distinct boards, both wanted.
   The raw pool is score-sorted and becomes ctx->keep, the roots' order. */
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

static int cmp_u32_asc(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* --backtrack_row_factor M > 0: the backtrack row's deduplicated, ranked
   candidates are cut to M x the row's width by the beam's own selection (same
   per-parent cap, random band and selection seed as a beam row), and the
   survivors are left in ctx->keep[0..n), still in rank order, as the roots. */
static uint32_t select_roots(BeamCtx *ctx, uint32_t kept, uint32_t beam_n, int row,
                             uint64_t cfg_hash) {
    uint64_t target = (uint64_t)g_bt_factor * beam_eff_K(row);
    if (target >= kept) return kept;
    RNG sel_rng = rng_for(cfg_hash, (uint32_t)row, 0xFFFFFFFFu, 1u);
    uint32_t n = select_beam(ctx, kept, beam_n, (uint32_t)target,
                             parent_cap_eff(row), g_frac_rand, &sel_rng);
    qsort(ctx->sel, n, sizeof *ctx->sel, cmp_u32_asc);
    for (uint32_t i = 0; i < n; i++) ctx->keep[i] = ctx->keep[ctx->sel[i]];  /* sel[i] >= i */
    return n;
}

/* --backtrack_unlock_right: candidates that differed only in the columns their
   rows give back are now the same board, and would repeat the same search. The
   first of each, in rank order, stays in ctx->keep[0..kept) (compacted in
   place); the count left is returned and the repeats go to *repeats. The
   fingerprints are computed in parallel and inserted serially in rank order,
   so which candidate survives cannot depend on the thread count; a matching
   fingerprint is confirmed cell by cell. ctx->sig_key/sig_idx are free here:
   the next dedup_and_rank clears them before use. */
static uint64_t *g_root_fp = NULL;
static uint32_t  g_root_fp_cap = 0;

static void root_rows(const BeamCtx *ctx, const BeamEntry *beam, uint32_t pi, int N,
                      RowChoice rows[EDGE_LEN]) {
    const PoolEntry *pe = &ctx->pool[pi];
    collect_rows(ctx, &beam[pe->parent], rows);
    rows[N] = pe->mv;
}

static bool root_same(const BeamCtx *ctx, const BeamEntry *beam, uint32_t a, uint32_t b, int N) {
    if (ctx->pool[a].flags != ctx->pool[b].flags) return false;
    RowChoice ra[EDGE_LEN], rb[EDGE_LEN];
    root_rows(ctx, beam, a, N, ra);
    root_rows(ctx, beam, b, N, rb);
    for (int r = 1; r <= N; r++)
        if (memcmp(ra[r].ci, rb[r].ci, (size_t)g_last_inner * sizeof ra[r].ci[0])) return false;
    return true;
}

static uint32_t unique_roots(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept, int N,
                             int nt, uint64_t *repeats) {
    if (g_root_fp_cap < kept) {
        g_root_fp = xrealloc(g_root_fp, (size_t)kept * sizeof *g_root_fp);
        g_root_fp_cap = kept;
    }
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (uint32_t i = 0; i < kept; i++) {
        const uint32_t pi = ctx->keep[i];
        RowChoice rows[EDGE_LEN];
        root_rows(ctx, beam, pi, N, rows);
        uint64_t fp = 14695981039346656037ULL ^ ctx->pool[pi].flags;
        for (int r = 1; r <= N; r++)
            for (int c = 0; c < g_last_inner; c++) { fp ^= rows[r].ci[c]; fp *= 1099511628211ULL; }
        g_root_fp[i] = fp ? fp : 1;
    }
    size_t sz = 4;
    while (sz < ctx->sig_sz && (uint64_t)sz < (uint64_t)kept * 2) sz *= 2;
    const size_t mask = sz - 1;
    memset(ctx->sig_key, 0, sz * sizeof *ctx->sig_key);
    uint32_t n = 0;
    for (uint32_t i = 0; i < kept; i++) {
        const uint64_t fp = g_root_fp[i];
        const uint32_t pi = ctx->keep[i];
        size_t h = (size_t)splitmix64(fp) & mask;
        bool repeat = false;
        for (; ctx->sig_key[h]; h = (h + 1) & mask)
            if (ctx->sig_key[h] == fp && root_same(ctx, beam, ctx->sig_idx[h], pi, N)) { repeat = true; break; }
        if (repeat) continue;
        ctx->sig_key[h] = fp; ctx->sig_idx[h] = pi;
        ctx->keep[n++] = pi;
    }
    *repeats = kept - n;
    return n;
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
        d->flags = pe->flags;
        assert(parity_ok(d));
    }
}

/* A stop-row board is prepared in one of two forms. Written as always, it is
   the formatted line tail; under --end_dive it is queued for the dives instead,
   as the board itself -- per cell, a piece (DV_EMPTY = open) and a rotation --
   so nothing is formatted only to be parsed back. Either form fits one
   EMIT_LINE_MAX slot; the preparation is parallel, emit_board_line serial. */
#define DIVE_SLOT_BYTES (NUM_PIECES * (sizeof(uint16_t) + 1))
_Static_assert(DIVE_SLOT_BYTES <= EMIT_LINE_MAX, "dive slot fits an emission slot");

/* Written boards by matched edges, for the summary's score line when nothing
   is dived (under --end_dive the dive engine keeps that tally). */
static uint64_t g_score_hist[DV_EDGES + 1];
/* Extension cells above the stop row (--cap_top cells excluded) of every
   written board, or of every dived one under --end_dive. */
static uint64_t g_ext_hist[PUZZLE_SIDE * PUZZLE_SIDE + 1];

/* Where the summary's best boards are: each output CSV this run appended to,
   and the data row (0-based, comments not counted -- the index
   tools/E555_viewer.py --row takes) the next written board lands on. The
   files are opened for append, so the rows earlier runs left are counted at
   open. */
static char   **g_out_files = NULL;
static int      g_out_file_n = 0, g_out_file_cur = -1;
static uint64_t g_file_rows = 0;

/* Data rows already in a board CSV: lines not starting with '#' or '%' that
   hold at least 512 fields, as the viewer counts them. */
static uint64_t count_board_rows(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char buf[1 << 20];
    uint64_t rows = 0;
    size_t commas = 0;
    bool at_start = true, comment = false;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) {
            const char ch = buf[i];
            if (at_start) { comment = ch == '#' || ch == '%'; at_start = false; }
            if (ch == ',') commas++;
            else if (ch == '\n') {
                if (!comment && commas >= 2 * NUM_PIECES - 1) rows++;
                commas = 0; at_start = true;
            }
        }
    if (!at_start && !comment && commas >= 2 * NUM_PIECES - 1) rows++;
    fclose(f);
    return rows;
}

/* Open a completions CSV for append and make it the file new rows are
   credited to. */
static FILE *open_completions(const char *path) {
    FILE *fp = fopen(path, "a");
    if (!fp) fatal("cannot open %s: %s", path, strerror(errno));
    setvbuf(fp, NULL, _IOFBF, EMIT_FILE_BUF);
    g_file_rows = (fseek(fp, 0, SEEK_END) == 0 && ftell(fp) > 0) ? count_board_rows(path) : 0;
    g_out_files = xrealloc(g_out_files, (size_t)(g_out_file_n + 1) * sizeof *g_out_files);
    g_out_files[g_out_file_n] = xmalloc(strlen(path) + 1);
    strcpy(g_out_files[g_out_file_n], path);
    g_out_file_cur = g_out_file_n++;
    return fp;
}

/* The five best values seen, each with the file and row it was written to;
   ties keep the earliest. */
#define TOP_KEEP 5
typedef struct { int v, file; uint64_t row; } TopRec;
typedef struct { int n; TopRec r[TOP_KEEP]; } TopList;
static TopList g_top_score, g_top_ext;
static void top_add(TopList *t, int v, uint64_t row) {
    if (t->n == TOP_KEEP && v <= t->r[TOP_KEEP - 1].v) return;
    int i = t->n < TOP_KEEP ? t->n++ : TOP_KEEP - 1;
    while (i > 0 && t->r[i - 1].v < v) { t->r[i] = t->r[i - 1]; i--; }
    t->r[i] = (TopRec){ v, g_out_file_cur, row };
}

/* --end_dive: dv_flush writes the rows; it reports each one's score here. */
static void dive_row_written(int score) { top_add(&g_top_score, score, g_file_rows++); }

static int prepare_board(const RowChoice rows[EDGE_LEN], int row, int orient,
                         const Ext *ext, char *slot, uint16_t *score) {
    uint32_t pos[NUM_PIECES], rot[NUM_PIECES];
    board_arrays(rows, row, ROWMASK_FULL, orient, ext, pos, rot);
    *score = 0;
    if (!g_end_dive) {
        *score = (uint16_t)board_score(pos, rot);
        char *p = slot;
        for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, pos[i]); }
        for (int i = 0; i < NUM_PIECES; i++) { *p++ = ','; *p++ = ' '; p = u32a(p, rot[i]); }
        *p++ = '\n';
        return (int)(p - slot);
    }
    uint16_t pid[NUM_PIECES];
    uint8_t *cr = (uint8_t *)slot + NUM_PIECES * sizeof(uint16_t);
    for (int x = 0; x < NUM_PIECES; x++) { pid[x] = DV_EMPTY; cr[x] = 0; }
    for (int p = 0; p < NUM_PIECES; p++)
        if (pos[p] < NUM_PIECES) { pid[pos[p]] = (uint16_t)p; cr[pos[p]] = (uint8_t)rot[p]; }
    memcpy(slot, pid, sizeof pid);
    return (int)DIVE_SLOT_BYTES;
}

/* Write one board, or queue it for the dives; returns the data row it was
   written to (UINT64_MAX when queued). */
static uint64_t emit_board_line(const char *slot, size_t len) {
    if (g_end_dive) {
        uint16_t pid[NUM_PIECES];
        memcpy(pid, slot, sizeof pid);
        dv_add(pid, (const uint8_t *)slot + sizeof pid);
        return UINT64_MAX;
    }
    fprintf(g_completions_fp, "%s, %" PRIu64, g_board_id_str, g_solution_idx++);
    fwrite(slot, 1, len, g_completions_fp);
    return g_file_rows++;
}

/* Stop-row emission buffers, allocated on first use (emission is entered from
   serial code). One tile is ~12.6 MB. */
#define EMIT_TILE 4096
static char *g_emit_lines = NULL;    /* EMIT_TILE x EMIT_LINE_MAX */
static uint64_t *g_emit_fps = NULL;
static int  *g_emit_lens  = NULL;
static uint8_t *g_emit_corner = NULL;   /* --lambda_corners: corner_emit_code */
static uint8_t *g_emit_rsv    = NULL;   /* TOP reserve pieces left free */
static uint16_t *g_emit_sc    = NULL;   /* matched edges (no --end_dive) */

/* Emit the stop-row boards, best-scored first. Reconstructing a board, hashing
   it and converting its 512 fields is ~all of the cost and touches only
   read-only state, so a tile of boards is built in parallel; the dedup, the
   index draw and the write itself then run serially over that tile in rank
   order, so the file is byte for byte what emitting one board at a time
   produced. Boards losing the dedup were formatted for nothing -- parallel work
   traded against serial work, which is the point. */
static void emit_stop_row(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept, int row) {
    if (!g_completions_fp) return;
    if (!g_emit_lines) {
        g_emit_lines = xmalloc((size_t)EMIT_TILE * EMIT_LINE_MAX);
        g_emit_fps   = xmalloc((size_t)EMIT_TILE * sizeof(uint64_t));
        g_emit_lens  = xmalloc((size_t)EMIT_TILE * sizeof(int));
        g_emit_corner = xmalloc((size_t)EMIT_TILE);
        g_emit_rsv    = xmalloc((size_t)EMIT_TILE);
        g_emit_sc     = xmalloc((size_t)EMIT_TILE * sizeof *g_emit_sc);
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
            g_emit_lens[k] = prepare_board(rows, row,
                                           (pe->flags & FLAG_ORIENT_SET)
                                               ? (int)(pe->flags & FLAG_ORIENT_MASK) : -1,
                                           NULL, g_emit_lines + (size_t)k * EMIT_LINE_MAX,
                                           &g_emit_sc[k]);
            if (g_corners_on)
                g_emit_corner[k] = corner_emit_code(&beam[pe->parent], &pe->mv, row,
                    (pe->flags & FLAG_ORIENT_SET) ? (int)(pe->flags & FLAG_ORIENT_MASK) : -1);
            if (g_top_n) {
                uint64_t u[4];
                memcpy(u, beam[pe->parent].used, sizeof u);
                for (int i = 0; i < EDGE_LEN; i++) used_set(u, g_cat[pe->mv.ci[i]].piece_id);
                g_emit_rsv[k] = (uint8_t)top_free(u);
            }
        }
        for (uint32_t k = 0; k < tile && g_emit_count < EMIT_MAX; k++) {
            if (!htable_insert(g_emit_fps[k])) continue;
            const uint64_t at = emit_board_line(g_emit_lines + (size_t)k * EMIT_LINE_MAX,
                                                (size_t)g_emit_lens[k]);
            g_stats.emitted_total++;
            if (!g_end_dive) { g_score_hist[g_emit_sc[k]]++; top_add(&g_top_score, g_emit_sc[k], at); }
            if (g_corners_on) tc_tally(g_emit_corner[k]);
            if (g_top_n) rsv_tally(g_emit_rsv[k]);
        }
    }
}

/* -- --backtrack_row: exhaustive search from the row-N candidates ----------- */

/* The beam stops at row N = --backtrack_row, expanded as a stop row, and every
   row-N candidate becomes the root of an exhaustive depth-first search that
   fills rows N+1..--stop_row one cell at a time in row-major order:

     col 0      the configuration's fixed left column (g_cur_left);
     cols 1-14  every inner catalog orientation matching the left and bottom
                colours (g_lb_bucket), not yet used;
     col 15     every terminal of the border's own right-edge pool
                (g_edge_term, so --free_edges widens it exactly as it widens the
                database) matching the left and bottom colours.

   Clues the search reaches follow expand_clue_row / expand_clued: a committed
   board takes its orientation's pins (the clue piece on its cell, the colour
   it will sit on one row below); an uncommitted board branches once per
   enabled orientation pinned on the row and may also go on unpinned only while
   row < g_clue_last_assign. The row-13 corner clues are never pinned, so
   nothing about them constrains row 12. Each completed row is committed with
   commit_row and must pass parity_ok, the beam's own exact test.

   Under --lambda_corners every committed row (and the root) must also keep at
   least one top corner buildable: in the board's clue frame, some TL block or
   some TR block is still alive (none of its pieces used, a top-border witness
   pair free under --free_edges, the row-12 junction met at row 12). Only a row
   that kills BOTH corners ends the path; the beam's corner term is unchanged,
   and every emitted board keeps at least one corner closable. Each level
   keeps the alive blocks of its parent's lists only (a used piece never comes
   back), so the test gets cheaper as the search climbs.

   Nothing is scored and nothing is selected: every board that completes the
   stop row is emitted, in root rank order and, within a root, in the order
   the search finds them -- so the file does not depend on the thread count.

   PARALLELISM. The roots' subtrees are wildly uneven: most die within a few
   cells and a few run to hundreds of thousands of boards. So there is no
   barrier anywhere. Threads claim roots in rank order, a window of them ahead
   of the oldest root not yet written; a root's boards are written as soon as it
   and every root before it are done. A thread that finds nothing to claim --
   the roots are exhausted, or the window waits on one slow root -- is HUNGRY,
   and a search that reaches a row boundary while any thread is hungry hands
   the subtree above that row to the job queue instead of descending itself,
   leaving a placeholder in its output. Outputs are flattened placeholder by
   placeholder, so the boards come out in exactly the serial search's order and
   the counts are the serial search's counts. */

/* -- Extending a stop-row board (--extend_nodes) ----------------------------- *

   Every board the backtracker completes at the stop row S goes on, before it is
   written or dived, with an exhaustive zero-break search over the inner cells
   above it -- rows S+1..14, cols 1..14 -- in COLUMN-major order: column 1 bottom
   up, then column 2, and so on. The board is written from the deepest prefix
   the search reaches (the first found at that depth, so the output does not
   depend on the thread count), still one line per stop-row board. Growing by
   columns leaves the unfilled cells, and so the dives' breaks, in the top-right
   corner; the right column and the top border above S are left to the dives.

   Every cell is checked on its own: an unused piece matching the left and
   bottom colours (g_lb_bucket); on row 14, a top colour still carried by an
   unplaced piece of the top-border pool (the rotations row's top border, or
   every unused edge under --free_edges), so no cell forces a break at the top
   border; next to a clue cell, the colour the clue will show. A clue cell in
   the region takes its piece at its spin when the board's orientation is
   known, and ends the extension when it is not. At most --extend_nodes
   placements per board.

   --free_top_clue turns the two row-13 clue cells into ordinary cells and lets
   the top-left clue go anywhere in the region; the top-right clue stays held
   (it is left for the dives, see board_arrays). --random_edges samples the left
   column as part of the configuration, but the board is written without it
   above the stop row, so there the extension chooses column 0 too: the
   sampled edges above S are released, and column 0 and column 1 are filled
   together, row by row (a frame-left edge chained on the column-0 top below,
   then the inner piece next to it), before columns 2..14. An edge placed in
   column 0 leaves the top-border pool.

   The region's clue cells, the pieces it may use although the board holds
   them, and its top-border pool are defined once below (g_ext_fix,
   ext_release_mask, ext_avail) and shared with the strip check and the
   stop-row test, so the three always agree on what the extension can do.

   --cap_top (on by default) closes the top-left border exactly over the whole
   columns the extension fills: the TL corner (rotations mode: the fixed one;
   --random_edges: either corner not on the board, its bottom on column 0's
   row-14 top), then top edges (15,1), (15,2), ... left to right, each unused,
   frame up, its inner colour the row-14 top below it and its left frame colour
   its neighbour's right one. The cap is only a tie-break: the search, its
   order and its node cap are unchanged and the most cells still win, so every
   board keeps its extension length and whole columns (--backtrack_min_col
   writes exactly the same boards); among fillings of that length the one with
   the longest cap is kept, and the cap is written with it. A cap is worked out
   only for a path that reaches the best length, once per whole column. */

/* Per orientation + 1 (0 = not yet known), per cell above the stop row: the
   catalog index of the clue the extension must place there, -2 for a clue
   cell of unknown orientation (the extension stops there), -1 otherwise. Rows
   up to the stop row are -1. Built once by ext_fix_init. */
static int16_t g_ext_fix[5][NUM_PIECES];

static void ext_fix_init(void) {
    for (int oi = 0; oi < 5; oi++) {
        const int orient = oi - 1;
        int16_t *fix = g_ext_fix[oi];
        for (int i = 0; i < NUM_PIECES; i++) fix[i] = -1;
        if (!g_clue_mask) continue;
        for (int k = 0; k < CLUE_N; k++) {
            if (!clue_on(k)) continue;
            if (g_free_top_clue && k >= CLUE_N_REACHABLE) continue;
            for (int o = 0; o < 4; o++) {
                if (!(g_clue_orients & (1u << o))) continue;
                if (orient >= 0 && o != orient) continue;
                const ClueCell *cc = &g_clue[o][k];
                if (cc->row <= g_stop_row || cc->row > EDGE_LEN) continue;
                fix[cc->row * PUZZLE_SIDE + cc->col] =
                    orient >= 0 ? (int16_t)g_clue_ci[o][k] : (int16_t)-2;
            }
        }
    }
}

static inline const int16_t *ext_fix_of(int orient) { return g_ext_fix[orient + 1]; }

/* Pieces a board holds that the extension may still place: the sampled left
   column above the stop row (--random_edges) and the orientation's top-left
   row-13 clue (--free_top_clue; the top-right one stays held). */
static void ext_release_mask(int orient, uint64_t rel[4]) {
    rel[0] = rel[1] = rel[2] = rel[3] = 0;
    if (g_random_edges)
        for (int r = (int)g_stop_row + 1; r <= EDGE_LEN; r++) used_set(rel, g_cur_left->p[r]->piece_id);
    if (g_free_top_clue && orient >= 0) used_set(rel, g_clue[orient][CLUE_TOP_LEFT].piece);
}

/* The top-border pool by inner colour, for a board whose unavailable pieces
   are `used` (releases already taken out). */
static void ext_avail(const uint64_t used[4], int avail[NUM_COLORS_TOTAL]) {
    memset(avail, 0, NUM_COLORS_TOTAL * sizeof avail[0]);
    if (g_free_edges) {
        for (int t = 0; t < g_edge_term_count; t++)
            if (!used_test(used, g_edge_term[t].piece_id) && color_is_inner(g_edge_term[t].left))
                avail[g_edge_term[t].left]++;
    } else {
        for (int c = COLOR_MIN; c <= COLOR_MAX; c++) avail[c] = g_top_border_inner_count[c];
    }
}

static inline int ext_cols_n(int n, int stop);

typedef struct {
    uint64_t used[4];
    int      avail[NUM_COLORS_TOTAL];     /* top-border pool, by inner colour */
    const int16_t *fix;                   /* per cell: clue catalog index, -1 free, -2 stop */
    uint8_t  topc[NUM_PIECES], rgtc[NUM_PIECES];   /* colours a placed region cell shows */
    uint8_t  cell[EXT_MAX];               /* the region, in visit order */
    uint16_t cur[EXT_MAX];
    const uint8_t *rtop;
    uint8_t  top0;                        /* --random_edges: column 0's top at the stop row */
    int      nreg, best, stop;
    uint64_t nodes, cap;
    Ext     *out;
    /* --cap_top: the best cap over whole columns 1..m of the current path
       (worked out when first needed after the path completes column m), the
       recorded filling's cap length, the longest a cap can be, and the corners
       that may start it (--random_edges). */
    Cap      cap_at[EDGE_LEN + 1];
    bool     cap_ok[EDGE_LEN + 1];
    int      best_cap, cap_full, ncorner;
    uint16_t corner_pid[2];
    uint8_t  corner_rot[2], corner_bot[2], corner_rgt[2];
} ExtCtx;

/* The longest run of top edges on (15,c..m), c's left frame colour `frame`;
   *cur holds the cap so far, *best the longest found, `full` the longest
   possible (the search stops once it is reached). */
static void cap_edges(const ExtCtx *e, uint64_t used[4], int c, int m, int frame,
                      int full, Cap *cur, Cap *best) {
    if (cur->n > best->n) *best = *cur;
    if (c > m || best->n == full) return;
    const int in = e->topc[EDGE_LEN * PUZZLE_SIDE + c];
    for (int j = 0; j < g_up_by_in_n[in]; j++) {
        const Oriented *o = &g_edge_up[g_up_by_in[in][j]];
        if (o->left != frame || used_test(used, o->piece_id)) continue;
        if (!g_free_edges && g_spin[o->piece_id] != o->rotation) continue;  /* not a top edge here */
        used_set(used, o->piece_id);
        cur->cell[cur->n] = (uint8_t)((PUZZLE_SIDE - 1) * PUZZLE_SIDE + c);
        cur->pid[cur->n] = o->piece_id; cur->rot[cur->n] = o->rotation; cur->n++;
        cap_edges(e, used, c + 1, m, o->right, full, cur, best);
        cur->n--;
        used_clear(used, o->piece_id);
        if (best->n == full) return;
    }
}

/* The best cap over whole columns 1..m of the current path into cap_at[m]. */
static const Cap *ext_cap(ExtCtx *e, int m) {
    Cap *best = &e->cap_at[m];
    if (e->cap_ok[m]) return best;
    e->cap_ok[m] = true;
    best->n = 0;
    uint64_t used[4];
    memcpy(used, e->used, sizeof used);
    Cap cur; cur.n = 0;
    if (!g_random_edges) {
        cap_edges(e, used, 1, m, g_cur_left->p[PUZZLE_SIDE - 1]->right, m, &cur, best);
        return best;
    }
    const int b0 = e->topc[EDGE_LEN * PUZZLE_SIDE];
    for (int j = 0; j < e->ncorner && best->n < m + 1; j++) {
        if (e->corner_bot[j] != b0) continue;
        cur.cell[0] = (uint8_t)((PUZZLE_SIDE - 1) * PUZZLE_SIDE);
        cur.pid[0] = e->corner_pid[j]; cur.rot[0] = e->corner_rot[j]; cur.n = 1;
        cap_edges(e, used, 1, m, e->corner_rgt[j], m + 1, &cur, best);
    }
    return best;
}

/* Done: every region cell filled and, under --cap_top, its cap complete. */
static inline bool ext_done(const ExtCtx *e) {
    return e->best == e->nreg && (!g_cap_top || e->best_cap == e->cap_full);
}

static void ext_dfs(ExtCtx *e, int d);

static inline void ext_place(ExtCtx *e, int d, int cell, uint16_t ci) {
    const Oriented *o = ext_or(ci);
    e->cur[d] = ci;
    e->topc[cell] = o->top;
    e->rgtc[cell] = o->right;
    ext_dfs(e, d + 1);
}

/* Column 0 above the stop row (--random_edges): a frame-left edge whose bottom
   meets the column-0 top below it. */
static void ext_dfs_col0(ExtCtx *e, int d, int cell, int r) {
    const int B = (r == e->stop + 1) ? e->top0 : e->topc[cell - PUZZLE_SIDE];
    for (int k = 0; k < g_edge_left_n; k++) {
        const Oriented *o = &g_edge_left[k];
        if (o->bottom != B || !color_is_inner(o->right)) continue;
        if (used_test(e->used, o->piece_id)) continue;
        if (e->nodes >= e->cap) return;
        e->nodes++;
        used_set(e->used, o->piece_id);
        e->avail[o->right]--;
        ext_place(e, d, cell, (uint16_t)(CATALOG_SIZE + k));
        e->avail[o->right]++;
        used_clear(e->used, o->piece_id);
        if (ext_done(e)) return;
    }
}

static void ext_dfs(ExtCtx *e, int d) {
    int m = 0;
    if (g_cap_top && d > 0) {
        m = ext_cols_n(d, e->stop);
        if (m != ext_cols_n(d - 1, e->stop)) e->cap_ok[m] = false;   /* column m just completed */
    }
    if (d > 0 && d >= e->best) {
        const Cap *cap = m > 0 ? ext_cap(e, m) : NULL;
        const int cn = cap ? cap->n : 0;
        if (d > e->best || cn > e->best_cap) {
            e->best = d; e->best_cap = cn;
            memcpy(e->out->ci, e->cur, (size_t)d * sizeof e->cur[0]);
            if (cap) e->out->cap = *cap; else e->out->cap.n = 0;
        }
    }
    if (d == e->nreg || e->nodes >= e->cap) return;
    const int cell = e->cell[d], r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
    if (c == 0) { ext_dfs_col0(e, d, cell, r); return; }
    const int L = (c == 1 && !g_random_edges) ? g_cur_left->right[r] : e->rgtc[cell - 1];
    const int B = (r == e->stop + 1) ? e->rtop[c] : e->topc[cell - PUZZLE_SIDE];
    if (!color_is_inner(L) || !color_is_inner(B)) return;
    const int f = e->fix[cell];
    if (f == -2) return;
    const int up = (r < EDGE_LEN) ? e->fix[cell + PUZZLE_SIDE] : -1;
    const int rt = (c < EDGE_LEN) ? e->fix[cell + 1] : -1;
    const int need_t = up >= 0 ? g_cat[up].bottom : -1;
    const int need_r = rt >= 0 ? g_cat[rt].left : -1;
    if (f >= 0) {                     /* the clue: reserved in used, placed as is */
        const Oriented *o = &g_cat[f];
        if (o->left != L || o->bottom != B) return;
        if ((need_t >= 0 && o->top != need_t) || (need_r >= 0 && o->right != need_r)) return;
        e->nodes++;
        ext_place(e, d, cell, (uint16_t)f);
        return;
    }
    const bool top_row = (r == EDGE_LEN);
    const int nb = g_lb_count[L][B];
    for (int k = 0; k < nb; k++) {
        const int ci = g_lb_bucket[L][B][k];
        const Oriented *o = &g_cat[ci];
        if (need_t >= 0 && o->top != need_t) continue;
        if (need_r >= 0 && o->right != need_r) continue;
        if (top_row && e->avail[o->top] <= 0) continue;
        if (used_test(e->used, o->piece_id)) continue;
        if (e->nodes >= e->cap) return;
        e->nodes++;
        used_set(e->used, o->piece_id);
        if (top_row) e->avail[o->top]--;
        ext_place(e, d, cell, (uint16_t)ci);
        if (top_row) e->avail[o->top]++;
        used_clear(e->used, o->piece_id);
        if (ext_done(e)) return;
    }
}

/* Extend the stop-row board (used, rtop, orientation) above row `stop` into
   out; returns whether the node cap cut the search short. */
static bool extend_board(const uint64_t used[4], const uint8_t rtop[PUZZLE_SIDE],
                         int stop, int orient, Ext *out) {
    out->n = 0; out->raw = false; out->cap.n = 0;
    if (g_extend_nodes == 0 || stop >= EDGE_LEN) return false;
    ExtCtx e;
    uint64_t rel[4];
    ext_release_mask(orient, rel);
    for (int k = 0; k < 4; k++) e.used[k] = used[k] & ~rel[k];
    e.rtop = rtop; e.stop = stop; e.out = out;
    e.top0 = g_random_edges ? g_cur_left->p[stop]->top : 0;
    e.best = 0; e.nodes = 0; e.cap = g_extend_nodes;
    e.nreg = 0;
    for (int c = g_random_edges ? 0 : 1; c <= EDGE_LEN; c++) {
        if (g_random_edges && c == 1) continue;               /* filled with column 0 */
        for (int r = stop + 1; r <= EDGE_LEN; r++) {
            e.cell[e.nreg++] = (uint8_t)(r * PUZZLE_SIDE + c);
            if (g_random_edges && c == 0) e.cell[e.nreg++] = (uint8_t)(r * PUZZLE_SIDE + 1);
        }
    }
    e.fix = ext_fix_of(orient);
    ext_avail(e.used, e.avail);
    e.best_cap = 0;
    e.cap_full = EDGE_LEN + (g_random_edges ? 1 : 0);
    memset(e.cap_ok, 0, sizeof e.cap_ok);
    e.ncorner = 0;
    if (g_cap_top && g_random_edges) {
        /* The two corners off the board; a pinned --TL takes the corner, a
           pinned --TR keeps its piece for (15,15). */
        const bool pinned = g_fixed_corner_pid[2] >= 0 || g_fixed_corner_pid[3] >= 0;
        const uint16_t cand[2] = { g_cTL.piece_id, g_cTR.piece_id };
        for (int j = 0; j < (pinned ? 1 : 2); j++)
            for (uint8_t sp = 0; sp < 4; sp++)
                if (seed_side(cand[j], sp, 0) == 0 && seed_side(cand[j], sp, 3) == 0) {
                    e.corner_pid[e.ncorner] = cand[j]; e.corner_rot[e.ncorner] = sp;
                    e.corner_bot[e.ncorner] = (uint8_t)seed_side(cand[j], sp, 2);
                    e.corner_rgt[e.ncorner] = (uint8_t)seed_side(cand[j], sp, 1);
                    e.ncorner++;
                    break;
                }
    }
    ext_dfs(&e, 0);
    out->n = (uint16_t)e.best;
    for (int k = 0; k < e.best; k++) out->cell[k] = e.cell[k];
    return e.best < e.nreg && e.nodes >= e.cap;
}

/* Whole columns an extension of n cells fills, counted from column 1. Under
   --random_edges column 0 is filled alongside column 1, row by row. */
static inline int ext_cols_n(int n, int stop) {
    const int h = EDGE_LEN - stop;
    if (h <= 0) return 0;
    if (!g_random_edges) return n / h;
    return n >= 2 * h ? 1 + (n - 2 * h) / h : 0;
}

static inline int ext_columns(const Ext *x, int stop) { return ext_cols_n(x->n, stop); }

/* -- The layered extension (--backtrack_unlock_right) ------------------------ *

   The stop-row board is the block rows 0..S x columns 0..W; everything else is
   the band. The extension grows the band in L-shaped layers hugging the block,
   along one fixed path: layer k goes up column W+k from row 0 to row S+k, then
   left along row S+k to column 0 (an arm past column or row 15 is left out),
   and the next layer starts again at row 0. Every cell takes an unused piece
   that matches each placed neighbour -- an inner piece inside, an edge with its
   grey side out on the frame, a corner with both grey sides out on a corner
   cell -- so the band never breaks. Any edge may go on any side and any corner
   on any open corner cell: the band gives the configuration's frame back.

   Corner clues whose cell is in the band are on the board before it starts
   (board_arrays writes them home) and the path steps over them. An exact DFS of
   at most --extend_nodes placements keeps the deepest prefix of the path, the
   first found at that depth: a prefix is a fixed set of cells, so the deepest
   is also the best scoring, and the result does not depend on the thread
   count. It never changes the block: other blocks are the backtracker's job. */

#define LX_EMPTY  0xFFu                 /* a face nobody shows yet */
#define LX_INNER  0
#define LX_EDGE   1                     /* + d: an edge cell whose outer side is d */
#define LX_CORNER 5                     /* + j: corner cell j (0 BL, 1 BR, 2 TL, 3 TR) */

typedef struct { uint16_t pid; uint8_t rot; uint8_t f[4]; } LxPiece;   /* faces top, right, bottom, left */
typedef struct {
    int     n, nlayers;
    uint8_t cell[NUM_PIECES];
    int     done[PUZZLE_SIDE + 1];      /* path cells that complete layers 1..k */
} LxPath;
static LxPath   g_lx_path[5];           /* per orientation + 1 (0 = not known) */
static uint8_t  g_lx_kind[NUM_PIECES];
static LxPiece  g_lx_edge[4][NUM_PIECES / 4];
static int      g_lx_edge_n[4];
static LxPiece  g_lx_corner[4][4];
static int      g_lx_corner_n[4];
/* The inner catalog by (right, bottom): the cells the path fills leftwards know
   those two sides, as g_lb_bucket serves the ones it fills upwards. */
static uint16_t g_rb_bucket[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL][MAX_LB_BUCKET];
static uint8_t  g_rb_count[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL];

static void lx_path_build(LxPath *P, int orient) {
    const int S = (int)g_stop_row, W = g_last_inner;
    bool clue[NUM_PIECES] = { false };
    if (orient >= 0 && (g_clue_mask & CLUE_CORNERS))
        for (int k = 1; k < CLUE_N; k++) {
            const ClueCell *cc = &g_clue[orient][k];
            if (cc->row > S || cc->col > W) clue[cc->row * PUZZLE_SIDE + cc->col] = true;
        }
    P->n = 0; P->nlayers = 0; P->done[0] = 0;
    for (int k = 1; W + k <= PUZZLE_SIDE - 1 || S + k <= PUZZLE_SIDE - 1; k++) {
        const int c = W + k, r = S + k;
        if (c <= PUZZLE_SIDE - 1)
            for (int y = 0; y <= (r < PUZZLE_SIDE - 1 ? r : PUZZLE_SIDE - 1); y++)
                if (!clue[y * PUZZLE_SIDE + c]) P->cell[P->n++] = (uint8_t)(y * PUZZLE_SIDE + c);
        if (r <= PUZZLE_SIDE - 1)
            for (int x = (c - 1 < PUZZLE_SIDE - 1 ? c - 1 : PUZZLE_SIDE - 1); x >= 0; x--)
                if (!clue[r * PUZZLE_SIDE + x]) P->cell[P->n++] = (uint8_t)(r * PUZZLE_SIDE + x);
        P->done[k] = P->n; P->nlayers = k;
    }
}

/* The frame pieces by the cells they may take, the (right, bottom) buckets, the
   cell kinds and the paths. Once, after the clue tables and the catalog. */
static void lx_init(void) {
    static const int corner_side[4][2] = { { 2, 3 }, { 2, 1 }, { 0, 3 }, { 0, 1 } };
    for (int p = 0; p < NUM_PIECES; p++) {
        int grey = 0;
        for (int d = 0; d < 4; d++) grey += seed_side((uint32_t)p, 0, d) == 0;
        for (int s = 0; s < 4; s++) {
            LxPiece q = { (uint16_t)p, (uint8_t)s, { 0, 0, 0, 0 } };
            for (int d = 0; d < 4; d++) q.f[d] = (uint8_t)seed_side((uint32_t)p, (uint32_t)s, d);
            if (grey == 1)
                for (int d = 0; d < 4; d++)
                    if (q.f[d] == 0) g_lx_edge[d][g_lx_edge_n[d]++] = q;
            if (grey == 2)
                for (int j = 0; j < 4; j++)
                    if (q.f[corner_side[j][0]] == 0 && q.f[corner_side[j][1]] == 0)
                        g_lx_corner[j][g_lx_corner_n[j]++] = q;
        }
    }
    for (int i = 0; i < g_cat_count; i++) {
        const Oriented *o = &g_cat[i];
        uint8_t *n = &g_rb_count[o->right][o->bottom];
        if (*n >= MAX_LB_BUCKET) fatal("internal: a (right, bottom) bucket holds over %d pieces", MAX_LB_BUCKET);
        g_rb_bucket[o->right][o->bottom][(*n)++] = (uint16_t)i;
    }
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        const bool rb = r == 0 || r == PUZZLE_SIDE - 1, cb = c == 0 || c == PUZZLE_SIDE - 1;
        if (rb && cb)  g_lx_kind[x] = (uint8_t)(LX_CORNER + (r ? 2 : 0) + (c ? 1 : 0));
        else if (r == PUZZLE_SIDE - 1) g_lx_kind[x] = LX_EDGE + 0;
        else if (c == PUZZLE_SIDE - 1) g_lx_kind[x] = LX_EDGE + 1;
        else if (r == 0)               g_lx_kind[x] = LX_EDGE + 2;
        else if (c == 0)               g_lx_kind[x] = LX_EDGE + 3;
        else                           g_lx_kind[x] = LX_INNER;
    }
    for (int oi = 0; oi < 5; oi++) lx_path_build(&g_lx_path[oi], oi - 1);
}

typedef struct {
    uint8_t  face[NUM_PIECES][4];       /* the colour each placed cell shows on each side */
    uint64_t used[4];
    const LxPath *path;
    uint16_t pid[NUM_PIECES];           /* the current path's pieces */
    uint8_t  rot[NUM_PIECES];
    int      best;
    uint64_t nodes, cap;
    Ext     *out;
} LxCtx;

static void lx_dfs(LxCtx *e, int d);

/* Place piece pid at spin rot (faces f) on path cell d (board cell x) if it is
   unused and shows every colour `need` asks for, and search on from there. */
static inline void lx_try(LxCtx *e, int d, int x, const uint8_t need[4],
                          uint16_t pid, uint8_t rot, const uint8_t f[4]) {
    for (int s = 0; s < 4; s++)
        if (need[s] != LX_EMPTY && need[s] != f[s]) return;
    if (used_test(e->used, pid) || e->nodes >= e->cap) return;
    e->nodes++;
    used_set(e->used, pid);
    memcpy(e->face[x], f, 4);
    e->pid[d] = pid; e->rot[d] = rot;
    lx_dfs(e, d + 1);
    memset(e->face[x], LX_EMPTY, 4);
    used_clear(e->used, pid);
}

static inline void lx_try_cat(LxCtx *e, int d, int x, const uint8_t need[4], int ci) {
    const Oriented *o = &g_cat[ci];
    const uint8_t f[4] = { o->top, o->right, o->bottom, o->left };
    lx_try(e, d, x, need, o->piece_id, o->rotation, f);
}

static void lx_dfs(LxCtx *e, int d) {
    if (d > e->best) {
        e->best = d;
        memcpy(e->out->ci, e->pid, (size_t)d * sizeof e->pid[0]);
        memcpy(e->out->rot, e->rot, (size_t)d);
    }
    if (d == e->path->n || e->nodes >= e->cap) return;
    const int x = e->path->cell[d], r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    const uint8_t need[4] = {
        r == PUZZLE_SIDE - 1 ? 0 : e->face[x + PUZZLE_SIDE][2],
        c == PUZZLE_SIDE - 1 ? 0 : e->face[x + 1][3],
        r == 0 ? 0 : e->face[x - PUZZLE_SIDE][0],
        c == 0 ? 0 : e->face[x - 1][1] };
    const int kind = g_lx_kind[x];
    const int full = e->path->n;
    if (kind >= LX_CORNER) {
        const int j = kind - LX_CORNER;
        for (int k = 0; k < g_lx_corner_n[j] && e->best < full; k++)
            lx_try(e, d, x, need, g_lx_corner[j][k].pid, g_lx_corner[j][k].rot, g_lx_corner[j][k].f);
    } else if (kind >= LX_EDGE) {
        const int s = kind - LX_EDGE;
        for (int k = 0; k < g_lx_edge_n[s] && e->best < full; k++)
            lx_try(e, d, x, need, g_lx_edge[s][k].pid, g_lx_edge[s][k].rot, g_lx_edge[s][k].f);
    } else if (need[3] != LX_EMPTY && need[2] != LX_EMPTY) {           /* going up */
        for (int k = 0; k < g_lb_count[need[3]][need[2]] && e->best < full; k++)
            lx_try_cat(e, d, x, need, g_lb_bucket[need[3]][need[2]][k]);
    } else if (need[1] != LX_EMPTY && need[2] != LX_EMPTY) {           /* going left */
        for (int k = 0; k < g_rb_count[need[1]][need[2]] && e->best < full; k++)
            lx_try_cat(e, d, x, need, g_rb_bucket[need[1]][need[2]][k]);
    } else if (need[2] != LX_EMPTY) {                                  /* a layer's turn */
        for (int L = COLOR_MIN; L <= COLOR_MAX && e->best < full; L++)
            for (int k = 0; k < g_lb_count[L][need[2]] && e->best < full; k++)
                lx_try_cat(e, d, x, need, g_lb_bucket[L][need[2]][k]);
    } else {
        for (int i = 0; i < g_cat_count && e->best < full; i++) lx_try_cat(e, d, x, need, i);
    }
}

/* Extend the stop-row board rows[0..stop] (orientation orient) along its path
   into out; *layers gets the whole layers it fills. Returns whether the node
   cap cut the search short. */
static bool layer_extend(const RowChoice rows[EDGE_LEN], int stop, int orient,
                         Ext *out, int *layers) {
    out->n = 0; out->raw = true; out->cap.n = 0;
    *layers = 0;
    const LxPath *P = &g_lx_path[orient + 1];
    if (g_extend_nodes == 0 || P->n == 0) return false;
    uint32_t pos[NUM_PIECES], rot[NUM_PIECES];
    board_arrays(rows, stop, ROWMASK_FULL, orient, NULL, pos, rot);
    LxCtx e;
    memset(e.face, LX_EMPTY, sizeof e.face);
    memset(e.used, 0, sizeof e.used);
    for (int p = 0; p < NUM_PIECES; p++) {
        if (pos[p] >= NUM_PIECES) continue;
        used_set(e.used, (uint16_t)p);
        for (int d = 0; d < 4; d++) e.face[pos[p]][d] = (uint8_t)seed_side((uint32_t)p, rot[p], d);
    }
    e.path = P; e.best = 0; e.nodes = 0; e.cap = g_extend_nodes; e.out = out;
    lx_dfs(&e, 0);
    out->n = (uint16_t)e.best;
    for (int k = 0; k < e.best; k++) out->cell[k] = P->cell[k];
    while (*layers < P->nlayers && P->done[*layers + 1] <= e.best) (*layers)++;
    return e.best < P->n && e.nodes >= e.cap;
}

/* -- The column check (--backtrack_min_col K >= 1) --------------------------- *

   Under --backtrack_min_col K a board is written only if its extension fills
   columns 1..K of rows S+1..14, so its column 1 runs whole from row 1 to row
   14. The row-major search fills every column of the rows up to S; so a partial
   board whose row r has its column-1 piece placed can lead to a written board
   only if column 1 can still be stacked from row r+1 to row 14. The check
   searches for one such stack, exactly as the board will have to build it:

     - each piece fits the left column's colour beside it and the top of the
       piece below; no piece is used twice, and none the board already holds;
     - under --random_edges, column 0 above the stop row is chosen too, as the
       extension chooses it: a frame-left edge on the column-0 top below, its
       inner colour the left colour of column 1 (rows up to S keep the
       configuration's sampled column);
     - row 14's top must be a colour the top-border pool still carries, the
       column-0 edges of the stack taken out of that pool;
     - the column-1 cell next to a clue cell the extension will fill shows the
       clue the colour it needs.

   Nothing else is asked (not the pins of rows <= S, not the columns to the
   right), and the pieces the extension may release (ext_release_mask) count as
   free, so every board the extension would keep has such a stack: a partial
   row without one is cut and no written board is lost. Checks that exceed
   COL_BUDGET steps answer "unknown" and cut nothing.

   Two caches keep the search short. A state (row, column-0 top, column-1 top)
   that failed with some pieces taken fails with any more taken, so a dead
   state is stored with the pieces taken then and recognised whenever they are
   all taken again; a stack found is stored with its pieces and stands again
   while none of them is taken (and its row-14 top is still in the pool). Both
   are emptied at the start of every job and at the rows where a subtree may be
   handed to another thread, so a check's answer -- budget included -- never
   depends on the thread count.

   Measured, the check halves the backtracker's nodes or better and cuts its
   time by about 40% (PROJECT_E555.md 5.10); wider strips (columns 1..W,
   pieces reused between rows) cut more boards but cost more than they saved,
   and so did building strip rows from the chain database's records. */
#define COL_DBITS  16
#define COL_LBITS  14
#define COL_BUDGET 50000u

typedef struct { uint64_t key; uint64_t taken[4]; uint32_t epoch; } ColDead;
typedef struct { uint64_t key; uint64_t mask[4]; uint32_t epoch; uint8_t t14, own; } ColLive;

typedef struct {
    ColDead  *dead;                       /* direct-mapped, lossy */
    ColLive  *live;
    uint32_t  epoch;
    uint64_t  used[4];                    /* pieces taken: the board's, less releases, and the stack's */
    int       avail[NUM_COLORS_TOTAL];    /* top-border pool, less the stack's column-0 edges */
    const int16_t *fix;                   /* the extension's clue cells */
    uint64_t  kbits;                      /* orientation bits of every key */
    int       stop;
    uint64_t  steps;
    bool      unknown;                    /* the budget ran out */
    uint64_t  wit[4];                     /* the stack found, from the current state up */
    uint8_t   t14, own;                   /* its row-14 top; its column-0 edges of that colour */
} ColCheck;

static ColCheck *col_new(void) {
    ColCheck *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->dead = xmalloc(((size_t)1 << COL_DBITS) * sizeof *s->dead);
    memset(s->dead, 0, ((size_t)1 << COL_DBITS) * sizeof *s->dead);
    s->live = xmalloc(((size_t)1 << COL_LBITS) * sizeof *s->live);
    memset(s->live, 0, ((size_t)1 << COL_LBITS) * sizeof *s->live);
    s->epoch = 1;
    return s;
}

/* Empty both caches: at the start of a job and at the rows where a subtree may
   be handed to another thread. */
static void col_forget(ColCheck *s) {
    if (++s->epoch == 0) {
        memset(s->dead, 0, ((size_t)1 << COL_DBITS) * sizeof *s->dead);
        memset(s->live, 0, ((size_t)1 << COL_LBITS) * sizeof *s->live);
        s->epoch = 1;
    }
}

static inline uint64_t col_key(const ColCheck *s, int row, int c0, int b) {
    return (uint64_t)row | ((uint64_t)(c0 & 7) << 4) | ((uint64_t)(b & 31) << 7) | s->kbits;
}

static inline bool col_dead(const ColCheck *s, uint64_t key) {
    const ColDead *e = &s->dead[splitmix64(key) & (((uint64_t)1 << COL_DBITS) - 1)];
    if (e->key != key || e->epoch != s->epoch) return false;
    return !((e->taken[0] & ~s->used[0]) | (e->taken[1] & ~s->used[1])
           | (e->taken[2] & ~s->used[2]) | (e->taken[3] & ~s->used[3]));
}

static inline void col_bury(ColCheck *s, uint64_t key) {
    ColDead *e = &s->dead[splitmix64(key) & (((uint64_t)1 << COL_DBITS) - 1)];
    e->key = key; e->epoch = s->epoch;
    memcpy(e->taken, s->used, sizeof e->taken);
}

static bool col_alive(ColCheck *s, uint64_t key) {
    const ColLive *e = &s->live[splitmix64(key) & (((uint64_t)1 << COL_LBITS) - 1)];
    if (e->key != key || e->epoch != s->epoch) return false;
    if ((e->mask[0] & s->used[0]) | (e->mask[1] & s->used[1])
        | (e->mask[2] & s->used[2]) | (e->mask[3] & s->used[3])) return false;
    if (s->avail[e->t14] - e->own < 1) return false;
    for (int k = 0; k < 4; k++) s->wit[k] |= e->mask[k];
    s->t14 = e->t14; s->own = e->own;
    return true;
}

static inline void col_remember(ColCheck *s, uint64_t key) {
    ColLive *e = &s->live[splitmix64(key) & (((uint64_t)1 << COL_LBITS) - 1)];
    e->key = key; e->epoch = s->epoch;
    memcpy(e->mask, s->wit, sizeof e->mask);
    e->t14 = s->t14; e->own = s->own;
}

static bool col_reach(ColCheck *s, int row, int c0, int b);

/* Column 1 of `row`, its left neighbour showing L, the piece below showing B;
   c0n is the column-0 top this row hands up. */
static bool col_cell(ColCheck *s, int row, int L, int B, int c0n) {
    if (++s->steps > COL_BUDGET) { s->unknown = true; return true; }
    if (!color_is_inner(L) || !color_is_inner(B)) return false;
    const int x = row * PUZZLE_SIDE + 1;
    const int rf = s->fix[x + 1];
    const int need_r = rf >= 0 ? g_cat[rf].left : -1;
    const bool top = (row == EDGE_LEN);
    const int nb = g_lb_count[L][B];
    for (int k = 0; k < nb; k++) {
        const Oriented *o = &g_cat[g_lb_bucket[L][B][k]];
        const uint16_t pid = o->piece_id;
        if (need_r >= 0 && o->right != need_r) continue;
        if (used_test(s->used, pid)) continue;
        if (top && s->avail[o->top] <= 0) continue;
        bool ok = true;
        if (top) { s->t14 = o->top; s->own = 0; }
        else {
            used_set(s->used, pid);
            ok = col_reach(s, row + 1, c0n, o->top);
            used_clear(s->used, pid);
        }
        if (s->unknown) return true;
        if (ok) { used_set(s->wit, pid); return true; }
    }
    return false;
}

/* Can column 1 (and, above the stop row under --random_edges, column 0) be
   stacked from `row` -- column-1 bottom b, column-0 bottom c0 -- to row 14? */
static bool col_reach(ColCheck *s, int row, int c0, int b) {
    const uint64_t key = col_key(s, row, c0, b);
    if (col_dead(s, key)) return false;
    if (col_alive(s, key)) return true;
    bool ok = false;
    if (g_random_edges && row > s->stop) {
        for (int k = 0; k < g_edge_left_n && !ok; k++) {     /* column 0 above S */
            const Oriented *o = &g_edge_left[k];
            if (o->bottom != c0 || !color_is_inner(o->right)) continue;
            if (used_test(s->used, o->piece_id)) continue;
            used_set(s->used, o->piece_id); s->avail[o->right]--;
            ok = col_cell(s, row, o->right, b, o->top);
            s->avail[o->right]++; used_clear(s->used, o->piece_id);
            if (s->unknown) return true;
            if (ok) { used_set(s->wit, o->piece_id); s->own += (o->right == s->t14); }
        }
    } else {
        const int c0n = (g_random_edges && row == s->stop) ? g_cur_left->p[row]->top : 0;
        ok = col_cell(s, row, g_cur_left->right[row], b, c0n);
        if (s->unknown) return true;
    }
    if (ok) { col_remember(s, key); return true; }
    col_bury(s, key);
    return false;
}

/* One search job's output: formatted boards and, between them, the outputs of
   the subtrees the job handed to the queue (kid[i] goes before board kid_at[i]). */
typedef struct BtNode {
    char     *buf; size_t len, cap;       /* board slots, back to back */
    size_t   *off;                        /* start of board i in buf */
    uint64_t *fp;                         /* exact-board fingerprint */
    uint8_t  *cc;                         /* corner code (--lambda_corners) */
    uint8_t  *rs;                         /* TOP reserve pieces left free */
    uint8_t  *ex;                         /* extension cells (--extend_nodes) */
    uint8_t  *cp;                         /* --cap_top: cap cells on row 15 */
    uint8_t  *ly;                         /* --backtrack_unlock_right: whole layers */
    uint16_t *sc;                         /* matched edges (no --end_dive) */
    uint32_t  n, ncap;
    struct BtNode **kid;
    uint32_t *kid_at;
    uint32_t  nkid, kidcap;
} BtNode;

typedef struct {
    BeamEntry lvl[EDGE_LEN + 1];          /* lvl[r]: the board with row r committed */
    RowChoice rows[EDGE_LEN];             /* the full move log, rows 1..stop */
    uint64_t  used[EDGE_LEN + 1][4];      /* pieces taken, while row r is being filled */
    int8_t    pin_kind[EDGE_LEN + 1][PUZZLE_SIDE];   /* -1 or PIN_PIECE / PIN_TOPCOLOR */
    uint16_t  pin_val[EDGE_LEN + 1][PUZZLE_SIDE];
    uint8_t   flags[EDGE_LEN + 1];        /* orientation tag row r commits with */
    uint64_t  nodes, fill[EDGE_LEN + 1], cut_parity, cut_corner;
    uint64_t  ext_nodes_cap, cut_mincol;  /* extensions the node cap cut; under --backtrack_min_col */
    uint64_t  cut_score;                  /* --backtrack_unlock_right: under --emit_score */
    /* --lambda_corners: alive live-block indices per level, slot and corner. */
    uint16_t  cn[EDGE_LEN + 1][4][2];
    uint16_t  cl[EDGE_LEN + 1][4][2][TC_MAX_BLOCKS];
    int       root_row;                   /* the level whose corner lists come from the catalog */
    int       split_max;                  /* last row after which a subtree may be handed off */
    uint32_t  root;                       /* the root being searched */
    double    deadline;
    bool      abort;
    BtNode  **out;                        /* where this job's output goes, made on first use */
    /* The column check: partial rows cut, checks whose budget ran out. */
    uint64_t  cut_col, strip_unknown;
    ColCheck *col;
    /* The top-row near-duplicate filter: the last stop row not rejected. */
    RowChoice ref;
    bool      ref_ok;
    uint64_t  near_dup;
} BtCtx;

static struct {
    uint64_t roots, roots_emitting, nodes, cut_parity, cut_corner, emitted;
    /* --extend_nodes, over the boards written: extension cells and whole
       columns (sum, max), extensions the node cap cut, boards dropped by
       --backtrack_min_col, and per configuration the deepest extension. */
    uint64_t ext_cells, ext_cols, ext_cap, cut_mincol, dup;
    int      ext_max, ext_cols_max, cfg_ext_max;
    uint64_t cfg_found, cfg_dup, cfg_mincol;   /* this configuration's stop-row boards,
                                           exact repeats, under --backtrack_min_col */
    uint64_t cut_col, strip_unknown, near_dup, cfg_near_dup;   /* the column check, the
                                           near-duplicate filter */
    uint64_t cap_cells, cap_boards, cap_closed;   /* --cap_top over the written boards:
                                           cells, boards with a whole column, of
                                           those closed over all of them */
    uint64_t root_rep, cfg_root_rep;      /* --backtrack_unlock_right: roots dropped as
                                           repeats, the run's and this configuration's */
    uint64_t cut_score, cfg_cut_score;    /* boards under --emit_score, not written */
    uint64_t fill[EDGE_LEN + 1];
    double   t;
} g_bt_run;

static BtCtx **g_bt_ctx = NULL;
static int     g_bt_nctx = 0;

/* A subtree handed off at a row boundary: the root, the rows it had filled
   above the root (N+1..k) with their orientation tags, and where its output
   goes. k = N is a root as claimed. */
#define BT_SPLIT_ROWS 3                   /* hand-offs happen at rows N+1..N+3 */
#define BT_CLAIM      8                   /* roots claimed at a time */
typedef struct {
    uint32_t  root;
    int       k;
    RowChoice rows[BT_SPLIT_ROWS];
    uint8_t   flags[BT_SPLIT_ROWS];
    BtNode   *out;                        /* a hand-off's node, made when it is handed off */
    BtNode  **out_slot;                   /* a root's: its slot's node, made on first use */
} BtJob;

/* A claimed root: its output and the jobs of its tree still to finish. */
typedef struct { BtNode *node; int pending; } BtRoot;

static struct {
    const BeamCtx   *ctx;
    const BeamEntry *beam;
    int       N;
    uint32_t  kept, win;
    double    deadline;
    BtJob    *q;                          /* hand-off queue */
    size_t    qhead, qn, qcap;
    omp_lock_t qlock;
    BtRoot   *slot;                       /* root r lives in slot[r % win] */
    uint32_t  next;                       /* next root to claim */
    uint32_t  cursor;                     /* next root to write */
    omp_lock_t commit;
    bool      abort;
    uint64_t  emitted, roots_emitting, dup;
    BeamEntry border;                     /* --backtrack_unlock_right: the bare border */
} g_bt;

/* Is some thread out of work? Read without locks: a stale answer only moves a
   hand-off one row boundary earlier or later. */
static inline bool bt_hungry(void) {
    size_t qn; uint32_t next, cursor;
    #pragma omp atomic read
    qn = g_bt.qn;
    #pragma omp atomic read
    next = g_bt.next;
    #pragma omp atomic read
    cursor = g_bt.cursor;
    return qn == 0 && (next >= g_bt.kept || next >= cursor + g_bt.win);
}

static BtNode *bt_node_new(void) {
    BtNode *o = xmalloc(sizeof *o);
    memset(o, 0, sizeof *o);
    return o;
}

/* Most roots emit nothing, so a job's output node is made when first needed. */
static inline BtNode *bt_out(BtCtx *x) {
    if (!*x->out) *x->out = bt_node_new();
    return *x->out;
}

static void bt_node_free(BtNode *o) {
    if (!o) return;
    for (uint32_t i = 0; i < o->nkid; i++) bt_node_free(o->kid[i]);
    free(o->buf); free(o->off); free(o->fp); free(o->cc); free(o->rs); free(o->ex); free(o->cp); free(o->ly); free(o->sc);
    free(o->kid); free(o->kid_at);
    free(o);
}

static void bt_job_push(const BtJob *j) {
    omp_set_lock(&g_bt.qlock);
    if (g_bt.qn == g_bt.qcap) {
        const size_t nc = g_bt.qcap ? g_bt.qcap * 2 : 256;
        BtJob *q = xmalloc(nc * sizeof *q);
        for (size_t i = 0; i < g_bt.qn; i++) q[i] = g_bt.q[(g_bt.qhead + i) % g_bt.qcap];
        free(g_bt.q);
        g_bt.q = q; g_bt.qcap = nc; g_bt.qhead = 0;
    }
    g_bt.q[(g_bt.qhead + g_bt.qn) % g_bt.qcap] = *j;
    #pragma omp atomic write
    g_bt.qn = g_bt.qn + 1;
    omp_unset_lock(&g_bt.qlock);
}

static bool bt_job_pop(BtJob *j) {
    size_t qn;
    #pragma omp atomic read
    qn = g_bt.qn;
    if (!qn) return false;
    bool got = false;
    omp_set_lock(&g_bt.qlock);
    if (g_bt.qn) {
        *j = g_bt.q[g_bt.qhead];
        g_bt.qhead = (g_bt.qhead + 1) % g_bt.qcap;
        #pragma omp atomic write
        g_bt.qn = g_bt.qn - 1;
        got = true;
    }
    omp_unset_lock(&g_bt.qlock);
    return got;
}

/* Hand the subtree above row `row` (just completed in x) to the queue, and
   leave its placeholder at the current end of this job's output. */
static void bt_spawn(BtCtx *x, int row) {
    BtJob j;
    j.root = x->root;
    j.k = row;
    for (int r = g_bt.N + 1; r <= row; r++) {
        j.rows[r - g_bt.N - 1]  = x->rows[r];
        j.flags[r - g_bt.N - 1] = x->lvl[r].flags;
    }
    j.out = bt_node_new();
    BtNode *o = bt_out(x);
    if (o->nkid == o->kidcap) {
        o->kidcap = o->kidcap ? o->kidcap * 2 : 8;
        o->kid    = xrealloc(o->kid, o->kidcap * sizeof *o->kid);
        o->kid_at = xrealloc(o->kid_at, o->kidcap * sizeof *o->kid_at);
    }
    o->kid[o->nkid] = j.out; o->kid_at[o->nkid] = o->n; o->nkid++;
    j.out_slot = NULL;
    BtRoot *br = &g_bt.slot[x->root % g_bt.win];
    #pragma omp atomic
    br->pending++;
    bt_job_push(&j);
}

static void bt_out_push(BtNode *o, BtCtx *x, int stop) {
    if (o->len + EMIT_LINE_MAX > o->cap) {
        o->cap = o->cap ? o->cap * 2 : (size_t)64 * EMIT_LINE_MAX;
        while (o->len + EMIT_LINE_MAX > o->cap) o->cap *= 2;
        o->buf = xrealloc(o->buf, o->cap);
    }
    if (o->n == o->ncap) {
        o->ncap = o->ncap ? o->ncap * 2 : 64;
        o->off = xrealloc(o->off, (size_t)o->ncap * sizeof *o->off);
        o->fp  = xrealloc(o->fp,  (size_t)o->ncap * sizeof *o->fp);
        o->cc  = xrealloc(o->cc,  (size_t)o->ncap * sizeof *o->cc);
        o->rs  = xrealloc(o->rs,  (size_t)o->ncap * sizeof *o->rs);
        o->ex  = xrealloc(o->ex,  (size_t)o->ncap * sizeof *o->ex);
        o->cp  = xrealloc(o->cp,  (size_t)o->ncap * sizeof *o->cp);
        o->ly  = xrealloc(o->ly,  (size_t)o->ncap * sizeof *o->ly);
        o->sc  = xrealloc(o->sc,  (size_t)o->ncap * sizeof *o->sc);
    }
    const BeamEntry *t = &x->lvl[stop];
    const int orient = ENTRY_HAS_ORIENT(t) ? (int)ENTRY_ORIENT(t) : -1;
    Ext ext;
    int layers = 0;
    if (g_unlock_right) {
        /* K = --backtrack_min_col band columns may stay open: layers 1..R-K must be whole. */
        if (layer_extend(x->rows, stop, orient, &ext, &layers)) x->ext_nodes_cap++;
        if (layers < (int)(g_unlock_right - g_bt_min_col)) { x->cut_mincol++; return; }
    } else {
        if (extend_board(t->used, t->rtop, stop, orient, &ext)) x->ext_nodes_cap++;
        if (g_bt_min_col && ext_columns(&ext, stop) < (int)g_bt_min_col) { x->cut_mincol++; return; }
    }
    o->off[o->n] = o->len;
    o->fp[o->n]  = board_fingerprint(x->rows, stop);
    o->cc[o->n]  = g_corners_on
                 ? tc_board_code(orient, t->used, t->rtop, stop == PUZZLE_SIDE - 4) : 0;
    o->rs[o->n]  = (uint8_t)(g_top_n ? top_free(t->used) : 0);
    o->ex[o->n]  = (uint8_t)ext.n;
    o->cp[o->n]  = ext.cap.n;
    o->ly[o->n]  = (uint8_t)layers;
    const int len = prepare_board(x->rows, stop, orient, &ext, o->buf + o->len, &o->sc[o->n]);
    /* --backtrack_unlock_right without dives: --emit_score, when given, is a
       floor on the extended board's matched edges. */
    if (score_cuts() && o->sc[o->n] < g_emit_score) {
        x->cut_score++;
        return;
    }
    o->len += (size_t)len;
    o->n++;
}

static void bt_row(BtCtx *x, int row);

/* The corner cut. Filters the parent level's alive lists (every live block at
   the root) against board t, stores them at `row`, and returns whether some
   slot the board may still use keeps at least one corner (TL or TR) alive. A
   path ends only when both corners are dead; one dead corner is left to the
   dives. */
static bool bt_corners_ok(BtCtx *x, int row, const BeamEntry *t) {
    const bool at12 = (row == PUZZLE_SIDE - 4);
    const bool root = (row == x->root_row);
    const int orient = ENTRY_HAS_ORIENT(t) ? (int)ENTRY_ORIENT(t) : -1;
    bool any = false;
    for (int s = 0; s < 4; s++) {
        x->cn[row][s][TC_TL] = x->cn[row][s][TC_TR] = 0;
        if (!(g_tc_slots & (1u << s))) continue;
        if (g_tc_clued && orient >= 0 && s != orient) continue;
        for (int k = 0; k < 2; k++) {
            if (at12 && g_tc_clued && t->rtop[k ? PUZZLE_SIDE - 3 : 2] != g_tc_clue_bottom[s][k])
                continue;
            const int n_src = root ? g_tc_live_n[s][k] : x->cn[row - 1][s][k];
            const uint16_t *src = x->cl[row - 1][s][k];
            uint16_t *dst = x->cl[row][s][k];
            int n = 0;
            for (int q = 0; q < n_src; q++) {
                const int i = root ? q : src[q];
                if (tc_alive(s, k, i, t->used, t->rtop, at12)) dst[n++] = (uint16_t)i;
            }
            x->cn[row][s][k] = (uint16_t)n;
        }
        if (x->cn[row][s][TC_TL] || x->cn[row][s][TC_TR]) any = true;
    }
    return any;
}

/* The top-row near-duplicate test: does stop row b repeat a's columns 1..K
   exactly and differ from it in at most one other cell (columns K+1..15)? A
   board with a new piece in columns 1..K is never a near duplicate -- those
   columns are where the extension starts from. Under --backtrack_unlock_right
   the row ends at column W and K counts band columns instead: all of columns
   1..W are compared. */
static bool top_near_dup(const RowChoice *a, const RowChoice *b) {
    const int K = g_unlock_right ? 0 : (int)g_bt_min_col;
    for (int c = 1; c <= K; c++) if (a->ci[c - 1] != b->ci[c - 1]) return false;
    int diff = g_unlock_right ? 0 : (a->rterm != b->rterm);
    for (int c = K + 1; c <= g_last_inner && diff <= 1; c++) diff += (a->ci[c - 1] != b->ci[c - 1]);
    return diff <= 1;
}

/* Row `row` is complete in x->rows[row]: commit, test, then go up or emit.

   At the stop row, the near-duplicate filter compares the row with the last
   stop row that was not itself rejected, emitted or not: if the previous board
   could not fill the columns, its near twin will not either, and if it could,
   the twin's extension and dives would only repeat its own. The reference is
   cleared wherever the search may hand a subtree to another thread (rows up to
   split_max, and at the start of every job), so each subtree is judged the
   same way whichever thread runs it, and the output does not depend on the
   thread count. */
static void bt_close_row(BtCtx *x, int row) {
    BeamEntry *t = &x->lvl[row];
    *t = x->lvl[row - 1];
    bt_commit(t, row, &x->rows[row]);
    t->depth = (uint16_t)row;
    t->flags = x->flags[row];
    if (!parity_ok(t)) { x->cut_parity++; return; }
    if (g_corners_on && !bt_corners_ok(x, row, t)) { x->cut_corner++; return; }
    x->fill[row]++;
    if ((uint32_t)row == g_stop_row) {
        if (g_top_dedup) {
            if (x->ref_ok && top_near_dup(&x->ref, &x->rows[row])) { x->near_dup++; return; }
            x->ref = x->rows[row]; x->ref_ok = true;
        }
        bt_out_push(bt_out(x), x, row);
        return;
    }
    if (row <= x->split_max) {
        x->ref_ok = false;
        if (x->col) col_forget(x->col);
        if (bt_hungry()) { bt_spawn(x, row); return; }
    }
    bt_row(x, row + 1);
}

/* -- The min-col checks, as the row-major search meets them ------------------ */

static inline int bt_orient(const BtCtx *x, int row) {
    return (x->flags[row] & FLAG_ORIENT_SET) ? (int)(x->flags[row] & FLAG_ORIENT_MASK) : -1;
}

/* Can column 1 still be stacked to row 14 above row `row`, whose column-1
   piece is placed? Cuts and unknowns are counted here. */
static bool bt_col_ok(BtCtx *x, int row) {
    const int orient = bt_orient(x, row);
    if (!x->col) x->col = col_new();
    ColCheck *s = x->col;
    uint64_t rel[4];
    ext_release_mask(orient, rel);
    for (int k = 0; k < 4; k++) s->used[k] = x->used[row][k] & ~rel[k];
    ext_avail(s->used, s->avail);
    s->fix = ext_fix_of(orient);
    s->kbits = (uint64_t)(orient + 1) << 12;
    s->stop = (int)g_stop_row;
    s->steps = 0; s->unknown = false;
    s->wit[0] = s->wit[1] = s->wit[2] = s->wit[3] = 0;
    const int c0 = (g_random_edges && (uint32_t)row == g_stop_row) ? g_cur_left->p[row]->top : 0;
    const bool ok = col_reach(s, row + 1, c0, g_cat[x->rows[row].ci[0]].top);
    if (s->unknown) { x->strip_unknown++; return true; }
    if (!ok) { x->cut_col++; return false; }
    return true;
}

/* Stop row S, column c (<= K) just placed with top colour B: the extension
   must put a piece on (S+1, c), so some piece it may use has to sit on B --
   with the left column's colour beside it in column 1 (rotations mode), the
   colour a clue cell next to it or above it wants, and a top the border pool
   still carries on row 14. A clue cell takes only its clue; a clue cell of
   unknown orientation ends the extension, so the board cannot fill column c. */
static bool bt_top_fits(BtCtx *x, int row, int c, int B) {
    const int orient = bt_orient(x, row);
    const int16_t *fix = ext_fix_of(orient);
    const int y = (row + 1) * PUZZLE_SIDE + c;
    const int f = fix[y];
    if (f == -2) return false;
    if (f >= 0) return g_cat[f].bottom == B;
    uint64_t u[4], rel[4];
    ext_release_mask(orient, rel);
    for (int k = 0; k < 4; k++) u[k] = x->used[row][k] & ~rel[k];
    const bool top = (row + 1 == EDGE_LEN);
    int avail[NUM_COLORS_TOTAL];
    if (top) ext_avail(u, avail);
    const int rf = c < EDGE_LEN ? fix[y + 1] : -1, uf = !top ? fix[y + PUZZLE_SIDE] : -1;
    const int need_r = rf >= 0 ? g_cat[rf].left : -1, need_t = uf >= 0 ? g_cat[uf].bottom : -1;
    const int Lfix = (c == 1 && !g_random_edges) ? g_cur_left->right[row + 1] : -1;
    const int L0 = Lfix >= 0 ? Lfix : COLOR_MIN, L1 = Lfix >= 0 ? Lfix : COLOR_MAX;
    for (int L = L0; L <= L1; L++)
        for (int k = 0; k < g_lb_count[L][B]; k++) {
            const Oriented *o = &g_cat[g_lb_bucket[L][B][k]];
            if (need_r >= 0 && o->right != need_r) continue;
            if (need_t >= 0 && o->top != need_t) continue;
            if (top && avail[o->top] <= 0) continue;
            if (used_test(u, o->piece_id)) continue;
            return true;
        }
    return false;
}

/* Fill column `col` of row `row`, whose left neighbour exposes colour L. */
static void bt_cell(BtCtx *x, int row, int col, int L) {
    if (x->abort) return;
    if ((++x->nodes & 0xFFFFu) == 0) {
        bool other;
        #pragma omp atomic read
        other = g_bt.abort;
        if (other || g_stop || omp_get_wtime() >= x->deadline) {
            x->abort = true;
            #pragma omp atomic write
            g_bt.abort = true;
            return;
        }
    }
    /* --backtrack_min_col: the column before this one was just placed. */
    if (g_col_check) {
        if (col == 2 && !bt_col_ok(x, row)) return;
        if ((uint32_t)row == g_stop_row && col >= 3 && col - 1 <= (int)g_bt_min_col
            && !bt_top_fits(x, row, col - 1, g_cat[x->rows[row].ci[col - 2]].top)) {
            x->cut_col++;
            return;
        }
    }
    const int B = x->lvl[row - 1].rtop[col];
    RowChoice *mv = &x->rows[row];
    uint64_t *used = x->used[row];
    if (col > g_last_inner) {
        /* --backtrack_unlock_right: the row ends at column W, before the band. */
        if (col != PUZZLE_SIDE - 1) { bt_close_row(x, row); return; }
        if (L < 0 || L >= NUM_COLORS_TOTAL) return;
        for (int kt = 0; kt < g_edge_term_by_left_n[L]; kt++) {
            int ti = g_edge_term_by_left[L][kt];
            const Oriented *term = &g_edge_term[ti];
            if (term->bottom != B || used_test(used, term->piece_id)) continue;
            mv->rterm = (uint8_t)ti;
            bt_close_row(x, row);
            if (x->abort) return;
        }
        return;
    }
    if (!color_is_inner(L) || !color_is_inner(B)) return;
    const int kind = x->pin_kind[row][col];
    if (kind == PIN_PIECE) {
        /* The clue piece is reserved in the used mask from the start, so it is
           placed here without the used test -- it can sit nowhere else. */
        const uint16_t ci = x->pin_val[row][col];
        const Oriented *o = &g_cat[ci];
        if (o->left != L || o->bottom != B) return;
        mv->ci[col - 1] = ci;
        bt_cell(x, row, col + 1, o->right);
        return;
    }
    const bool top_pin = (kind == PIN_TOPCOLOR), right_pin = (kind == PIN_RIGHTCOLOR);
    const int nb = g_lb_count[L][B];
    for (int k = 0; k < nb; k++) {
        const int ci = g_lb_bucket[L][B][k];
        const Oriented *o = &g_cat[ci];
        if (top_pin && o->top != x->pin_val[row][col]) continue;
        if (right_pin && o->right != x->pin_val[row][col]) continue;
        const uint16_t pid = o->piece_id;
        if (used_test(used, pid)) continue;
        used_set(used, pid);
        mv->ci[col - 1] = (uint16_t)ci;
        bt_cell(x, row, col + 1, o->right);
        used_clear(used, pid);
        if (x->abort) return;
    }
}

static void bt_set_pins(BtCtx *x, int row, const int pin_idx[3], const int pin_kind[3],
                        const uint16_t pin_val[3]) {
    memset(x->pin_kind[row], -1, sizeof x->pin_kind[row]);
    if (!pin_idx) return;
    for (int s = 0; s < 3; s++) {
        if (pin_idx[s] < 0) continue;
        int c = 1 + s * CHAIN_LEN + pin_idx[s];
        if (c > g_last_inner) continue;       /* a band column: never searched */
        x->pin_kind[row][c] = (int8_t)pin_kind[s];
        x->pin_val[row][c]  = pin_val[s];
    }
}

/* --backtrack_unlock_right with --clue_corners: the row-13 clues, which the
   beam's pin schedule leaves out. A clue whose cell is in the block (a stop row
   of 13) is placed there, as the row-2 ones are; a block cell right under a
   clue shows its bottom colour, and a block cell right left of one, the clue
   being in the band, shows its left colour. So a clue on the board never breaks
   against the block. The clue piece is reserved from the start, as every clue
   is, so a pinned cell is its only place. */
static void bt_band_pins(BtCtx *x, int row, int orient) {
    if (!g_band_pins || orient < 0) return;
    const int S = (int)g_stop_row, W = g_last_inner;
    for (int k = CLUE_TOP_LEFT; k <= CLUE_TOP_RIGHT; k++) {
        const ClueCell *cc = &g_clue[orient][k];
        const int r = cc->row, c = cc->col;
        const bool in_block = r <= S && c <= W;
        const Oriented *o = &g_cat[g_clue_ci[orient][k]];
        if (in_block && row == r) {
            x->pin_kind[row][c] = PIN_PIECE;
            x->pin_val[row][c]  = g_clue_ci[orient][k];
        } else if (row == r - 1 && c <= W) {
            x->pin_kind[row][c] = PIN_TOPCOLOR;
            x->pin_val[row][c]  = o->bottom;
        } else if (!in_block && row == r && c == W + 1) {
            x->pin_kind[row][c - 1] = PIN_RIGHTCOLOR;
            x->pin_val[row][c - 1]  = o->left;
        }
    }
}

/* Start row `row` on top of x->lvl[row - 1], once per clue branch it owes. */
static void bt_row(BtCtx *x, int row) {
    const BeamEntry *p = &x->lvl[row - 1];
    const int L = g_cur_left->right[row];
    if (!color_is_inner(L)) return;
    if (g_clue_mask) {
        int pi[3], pk[3]; uint16_t pv[3];
        if (ENTRY_HAS_ORIENT(p)) {
            if (clue_pins_for(row, (int)ENTRY_ORIENT(p), pi, pk, pv)) {
                bt_set_pins(x, row, pi, pk, pv);
                bt_band_pins(x, row, (int)ENTRY_ORIENT(p));
                memcpy(x->used[row], p->used, sizeof x->used[row]);
                x->flags[row] = p->flags;
                bt_cell(x, row, 1, L);
                return;
            }
        } else {
            /* A branch that pins only the colour a clue will sit on is a subset
               of the deferred branch whenever deferring is allowed: the same
               filling stays unassigned and commits one row up, where the same
               orientation pins the clue piece itself. The beam needs that early
               commit to rank and prune; an exhaustive search would only find
               every such board twice. */
            const bool may_defer = row < g_clue_last_assign;
            bool any = false;
            for (int o = 0; o < 4 && !x->abort; o++) {
                if (!(g_clue_orients & (1u << o))) continue;
                if (!clue_pins_for(row, o, pi, pk, pv)) continue;
                any = true;
                bool places_piece = false;
                for (int sg = 0; sg < 3; sg++)
                    if (pi[sg] >= 0 && pk[sg] == PIN_PIECE) places_piece = true;
                if (may_defer && !places_piece) continue;
                bt_set_pins(x, row, pi, pk, pv);
                bt_band_pins(x, row, o);
                memcpy(x->used[row], p->used, sizeof x->used[row]);
                x->flags[row] = (uint8_t)(o | FLAG_ORIENT_SET);
                bt_cell(x, row, 1, L);
            }
            if (any && !may_defer) return;
        }
    }
    bt_set_pins(x, row, NULL, NULL, NULL);
    bt_band_pins(x, row, ENTRY_HAS_ORIENT(p) ? (int)ENTRY_ORIENT(p) : -1);
    memcpy(x->used[row], p->used, sizeof x->used[row]);
    x->flags[row] = p->flags;
    bt_cell(x, row, 1, L);
}

/* Write one finished root: its boards and its hand-offs' boards, in DFS order.
   Called under g_bt.commit, so dedup, the file and the dive queue see the roots
   one at a time and in rank order. */
static void bt_emit_node(const BtNode *o, uint64_t *boards) {
    if (!o) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i <= o->n; i++) {
        while (k < o->nkid && o->kid_at[k] == i) bt_emit_node(o->kid[k++], boards);
        if (i == o->n) break;
        (*boards)++;
        if (!htable_insert(o->fp[i])) { g_bt.dup++; continue; }
        const size_t end = (i + 1 < o->n) ? o->off[i + 1] : o->len;
        const uint64_t at = emit_board_line(o->buf + o->off[i], end - o->off[i]);
        g_stats.emitted_total++; g_bt.emitted++;
        if (!g_end_dive) { g_score_hist[o->sc[i]]++; top_add(&g_top_score, o->sc[i], at); }
        if (g_extend_nodes) {
            /* --backtrack_unlock_right: whole layers rather than whole columns */
            const int n = o->ex[i], cols = g_unlock_right ? o->ly[i] : ext_cols_n(n, (int)g_stop_row);
            g_ext_hist[n]++;
            if (!g_end_dive) top_add(&g_top_ext, n, at);
            g_bt_run.ext_cells += (uint64_t)n; g_bt_run.ext_cols += (uint64_t)cols;
            if (n > g_bt_run.ext_max) g_bt_run.ext_max = n;
            if (cols > g_bt_run.ext_cols_max) g_bt_run.ext_cols_max = cols;
            if (n > g_bt_run.cfg_ext_max) g_bt_run.cfg_ext_max = n;
            if (g_cap_top && cols > 0) {
                g_bt_run.cap_cells += o->cp[i]; g_bt_run.cap_boards++;
                g_bt_run.cap_closed += o->cp[i] == cols + (g_random_edges ? 1 : 0);
            }
        }
        if (g_corners_on) tc_tally(o->cc[i]);
        rsv_tally(o->rs[i]);
    }
}

/* --max_emitted counts written boards; under --end_dive it caps the dived
   output instead and never stops the search. */
static inline bool bt_budget_spent(void) {
    return g_max_partials && !g_end_dive
        && g_stats.emitted_total + (uint64_t)g_partial_total >= g_max_partials;
}

/* Write root r -- or, once the budget is spent, drop it: the written count then
   overshoots --max_emitted by at most one root's boards. */
static void bt_commit_root(uint32_t r) {
    BtRoot *br = &g_bt.slot[r % g_bt.win];
    if (!bt_budget_spent()) {
        uint64_t boards = 0;
        bt_emit_node(br->node, &boards);
        if (boards) g_bt.roots_emitting++;
    }
    bt_node_free(br->node);
    br->node = NULL;
}

/* Write every root that is done and has no unwritten root before it. Whoever
   holds the lock does the writing; the others go back to searching. */
static void bt_try_commit(void) {
    if (!omp_test_lock(&g_bt.commit)) return;
    for (;;) {
        uint32_t c, next;
        #pragma omp atomic read
        c = g_bt.cursor;
        #pragma omp atomic read
        next = g_bt.next;
        if (c >= next) break;
        int pending;
        #pragma omp atomic read
        pending = g_bt.slot[c % g_bt.win].pending;
        if (pending) break;
        #pragma omp flush
        bt_commit_root(c);
        #pragma omp atomic write
        g_bt.cursor = c + 1;
        if (bt_budget_spent()) {             /* stop claiming and searching */
            #pragma omp atomic write
            g_bt.abort = true;
        }
    }
    omp_unset_lock(&g_bt.commit);
}

/* One job of the search: rebuild the board it starts from and search above. */
static void bt_run_job(BtCtx *x, const BtJob *j) {
    const int N = g_bt.N;
    BtNode *handoff = j->out;             /* lives through this call; never NULL for a hand-off */
    const PoolEntry *pe = &g_bt.ctx->pool[g_bt.ctx->keep[j->root]];
    collect_rows(g_bt.ctx, &g_bt.beam[pe->parent], x->rows);
    x->rows[N] = pe->mv;
    BeamEntry *r = &x->lvl[N];
    if (g_unlock_right) {                 /* rows 1..N give back columns W+1..15 */
        *r = g_bt.border;
        for (int row = 1; row <= N; row++) commit_row_narrow(r, row, &x->rows[row]);
    } else {
        *r = g_bt.beam[pe->parent];
        commit_row(r, N, &pe->mv);
    }
    r->depth = (uint16_t)N; r->flags = pe->flags;
    x->out = j->out_slot ? j->out_slot : &handoff;
    x->deadline = g_bt.deadline; x->root = j->root;
    x->ref_ok = false;
    if (x->col) col_forget(x->col);
    #pragma omp atomic read
    x->abort = g_bt.abort;
    if (x->abort) return;
    if (j->k == N) {
        x->root_row = N;
        if (g_corners_on && !bt_corners_ok(x, N, r)) { x->cut_corner++; return; }
        bt_row(x, N + 1);
        return;
    }
    /* A hand-off: its rows were complete and legal when they were handed off.
       Its corner lists are rebuilt from the catalog rather than from the levels
       below -- the same lists, since a block dead at one row stays dead above
       it (used pieces never come back). */
    for (int row = N + 1; row <= j->k; row++) {
        x->rows[row] = j->rows[row - N - 1];
        BeamEntry *t = &x->lvl[row];
        *t = x->lvl[row - 1];
        bt_commit(t, row, &x->rows[row]);
        t->depth = (uint16_t)row;
        t->flags = j->flags[row - N - 1];
    }
    x->root_row = j->k;
    if (g_corners_on) (void)bt_corners_ok(x, j->k, &x->lvl[j->k]);
    bt_row(x, j->k + 1);
}

static void bt_job_done(uint32_t root) {
    BtRoot *br = &g_bt.slot[root % g_bt.win];
    int left;
    #pragma omp flush
    #pragma omp atomic capture seq_cst
    left = --br->pending;
    if (left == 0) bt_try_commit();
}

/* Search every row-N candidate (ctx->keep[0..kept), rank order) to the stop row
   and emit what completes it. Fills in the configuration's result. */
static void backtrack_emit(BeamCtx *ctx, const BeamEntry *beam, uint32_t kept,
                           int N, double deadline, BeamResult *res) {
    const int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    if (g_bt_nctx < nt) {
        g_bt_ctx = xrealloc(g_bt_ctx, (size_t)nt * sizeof *g_bt_ctx);
        /* Each thread allocates and zeroes its own context, so first touch puts
           it in the memory of the socket that thread runs on (two-socket
           machines; pair with OMP_PROC_BIND=close OMP_PLACES=cores). */
        const int have = g_bt_nctx;
        for (int t = have; t < nt; t++) g_bt_ctx[t] = NULL;
        #pragma omp parallel num_threads(nt)
        {
            const int t = omp_get_thread_num();
            if (t >= have && t < nt) {
                BtCtx *c = xmalloc(sizeof(BtCtx));
                memset(c, 0, sizeof(BtCtx));
                g_bt_ctx[t] = c;
            }
        }
        for (int t = have; t < nt; t++)            /* fewer threads than asked */
            if (!g_bt_ctx[t]) { g_bt_ctx[t] = xmalloc(sizeof(BtCtx)); memset(g_bt_ctx[t], 0, sizeof(BtCtx)); }
        g_bt_nctx = nt;
        omp_init_lock(&g_bt.qlock);
        omp_init_lock(&g_bt.commit);
    }
    const uint32_t win = 1024u + 64u * (uint32_t)nt;
    if (g_bt.win < win) {
        free(g_bt.slot);
        g_bt.slot = xmalloc((size_t)win * sizeof *g_bt.slot);
        g_bt.win = win;
    }
    memset(g_bt.slot, 0, (size_t)g_bt.win * sizeof *g_bt.slot);
    g_bt.ctx = ctx; g_bt.beam = beam; g_bt.N = N; g_bt.kept = kept;
    g_bt.deadline = deadline;
    if (g_unlock_right) beam_init_border(&g_bt.border, g_cur_bottom, g_cur_left);
    g_bt.qhead = g_bt.qn = 0;
    g_bt.next = g_bt.cursor = 0;
    g_bt.abort = false;
    g_bt.emitted = g_bt.roots_emitting = g_bt.dup = 0;
    const int split_max = N + BT_SPLIT_ROWS < (int)g_stop_row - 1
                        ? N + BT_SPLIT_ROWS : (int)g_stop_row - 1;
    for (int t = 0; t < nt; t++) {
        BtCtx *x = g_bt_ctx[t];
        x->nodes = x->cut_parity = x->cut_corner = 0;
        x->ext_nodes_cap = x->cut_mincol = x->cut_score = 0;
        x->cut_col = x->strip_unknown = x->near_dup = 0;
        memset(x->fill, 0, sizeof x->fill);
        x->abort = false;
        x->split_max = split_max;
    }
    const double t0 = omp_get_wtime();

    #pragma omp parallel num_threads(nt)
    {
        BtCtx *x = g_bt_ctx[omp_get_thread_num()];
        int idle = 0;
        for (;;) {
            BtJob j;
            if (bt_job_pop(&j)) { idle = 0; bt_run_job(x, &j); bt_job_done(j.root); continue; }
            /* Claim the next roots, a few at a time, if the window has room. */
            uint32_t r0 = 0, rn = 0;
            bool stop_claims;
            #pragma omp atomic read
            stop_claims = g_bt.abort;
            omp_set_lock(&g_bt.qlock);
            if (!stop_claims && !g_stop && g_bt.next < g_bt.kept
                && g_bt.next < g_bt.cursor + g_bt.win) {
                r0 = g_bt.next;
                rn = BT_CLAIM;
                if (rn > g_bt.kept - r0) rn = g_bt.kept - r0;
                if (rn > g_bt.cursor + g_bt.win - r0) rn = g_bt.cursor + g_bt.win - r0;
                for (uint32_t r = r0; r < r0 + rn; r++) {
                    BtRoot *br = &g_bt.slot[r % g_bt.win];
                    br->node = NULL;
                    br->pending = 1;
                }
                #pragma omp atomic write
                g_bt.next = r0 + rn;
            }
            omp_unset_lock(&g_bt.qlock);
            if (rn) {
                idle = 0;
                for (uint32_t r = r0; r < r0 + rn; r++) {
                    BtJob rj = { .root = r, .k = N, .out = NULL,
                                 .out_slot = &g_bt.slot[r % g_bt.win].node };
                    bt_run_job(x, &rj);
                    bt_job_done(r);
                }
                continue;
            }
            /* Nothing to do: done when every claimed root is written and no
               more will be claimed; otherwise help write, then wait. */
            bt_try_commit();
            uint32_t c, next; size_t qn;
            #pragma omp atomic read
            c = g_bt.cursor;
            #pragma omp atomic read
            next = g_bt.next;
            #pragma omp atomic read
            qn = g_bt.qn;
            #pragma omp atomic read
            stop_claims = g_bt.abort;
            if (qn == 0 && c == next && (next >= g_bt.kept || stop_claims || g_stop)) break;
            if (++idle < 64) sched_yield();
            else { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }
        }
    }

    uint64_t fill[EDGE_LEN + 1] = {0}, nodes = 0, cut_p = 0, cut_c = 0, ecap = 0, cut_m = 0;
    uint64_t cut_col = 0, unknown = 0, near_dup = 0, cut_s = 0;
    bool aborted = g_bt.abort;
    for (int t = 0; t < nt; t++) {
        BtCtx *x = g_bt_ctx[t];
        nodes += x->nodes; cut_p += x->cut_parity; cut_c += x->cut_corner;
        ecap += x->ext_nodes_cap; cut_m += x->cut_mincol;
        cut_col += x->cut_col; unknown += x->strip_unknown; near_dup += x->near_dup;
        cut_s += x->cut_score;
        for (int r = 0; r <= EDGE_LEN; r++) fill[r] += x->fill[r];
        if (x->abort) aborted = true;
    }
    const uint64_t roots = g_bt.next, roots_emitting = g_bt.roots_emitting, emitted = g_bt.emitted;
    /* A budget stop is reported as the budget, not as an interrupted search. */
    const bool budget = bt_budget_spent();
    if (budget) partials_budget_spent();

    const double dt = omp_get_wtime() - t0;
    int deepest = N;
    for (int r = N + 1; r <= (int)g_stop_row; r++) if (fill[r]) deepest = r;
    g_bt_run.roots += roots; g_bt_run.roots_emitting += roots_emitting;
    g_bt_run.nodes += nodes; g_bt_run.cut_parity += cut_p; g_bt_run.cut_corner += cut_c;
    g_bt_run.emitted += emitted; g_bt_run.t += dt;
    g_bt_run.ext_cap += ecap; g_bt_run.cut_mincol += cut_m;
    g_bt_run.dup += g_bt.dup;
    g_bt_run.cfg_found = fill[g_stop_row];
    g_bt_run.cfg_dup = g_bt.dup; g_bt_run.cfg_mincol = cut_m;
    g_bt_run.cut_col += cut_col; g_bt_run.strip_unknown += unknown;
    g_bt_run.near_dup += near_dup; g_bt_run.cfg_near_dup = near_dup;
    g_bt_run.cut_score += cut_s; g_bt_run.cfg_cut_score = cut_s;
    for (int r = 0; r <= EDGE_LEN; r++) g_bt_run.fill[r] += fill[r];

    if (fill[g_stop_row]) {
        res->row = g_stop_row; res->width = (uint32_t)emitted;
        g_stats.reached_stop++;
    } else if (aborted) {
        res->row = (uint32_t)deepest;
        res->width = (uint32_t)(deepest > N ? fill[deepest] : kept);
    } else {
        res->reason = "extinct";
        res->row = (uint32_t)deepest + 1;
        res->width = (uint32_t)(deepest > N ? fill[deepest] : kept);
        g_stats.extinct_at[deepest + 1]++;
    }
    if (aborted && !budget)
        res->reason = g_stop ? "interrupted" : "time";

    if (g_verbose) {
        printf("[dfs] %s roots=%" PRIu64 " emitting=%" PRIu64 " nodes=%" PRIu64
               " cut_parity=%" PRIu64, g_config_id_str, roots, roots_emitting, nodes, cut_p);
        if (g_corners_on) printf(" cut_corner=%" PRIu64, cut_c);
        if (g_extend_nodes) printf(" ext_max=%d", g_bt_run.cfg_ext_max);
        if (mincol_cuts())  printf(" cut_mincol=%" PRIu64, cut_m);
        if (g_unlock_right) printf(" root_repeats=%" PRIu64, g_bt_run.cfg_root_rep);
        if (score_cuts())   printf(" cut_score=%" PRIu64, cut_s);
        if (g_col_check)    printf(" cut_col=%" PRIu64 " unknown=%" PRIu64, cut_col, unknown);
        if (g_top_dedup)    printf(" near_dups=%" PRIu64, near_dup);
        printf(" reached");
        for (int r = N + 1; r <= (int)g_stop_row; r++) printf(" r%d:%" PRIu64, r, fill[r]);
        printf(" emitted=%" PRIu64 "%s t=%.2fs\n", emitted,
               aborted && !budget ? " (stopped early)" : "", dt);
        fflush(stdout);
    }
}

/* -- Beam driver ------------------------------------------------------------ */

/* The compact log writes a configuration's [sweep] line in stages, so a long
   backtrack is not silent: the id (at the start of the beam under
   --random_edges; in rotations mode, where the configurations that die in the
   beam collapse into one line, only once the beam has candidates), then what
   the beam handed on, then sweep_report's counts. */
static bool g_sweep_open = false;
static void barren_flush(void);
static void sweep_head(void) {
    if (g_verbose || g_sweep_open) return;
    barren_flush();
    printf("[sweep] %s", g_config_id_str);
    fflush(stdout);
    g_sweep_open = true;
}
static void sweep_beam_done(uint32_t kept) {
    if (g_verbose) return;
    sweep_head();
    printf(" beam: %u %s |", kept, g_backtrack_row ? "roots" : "boards");
    fflush(stdout);
}

static BeamResult beam_search_config(BeamCtx *ctx, Scratch **scratch,
                                     uint64_t cfg_hash, double deadline) {
    BeamResult res = {0, 1, "stop_row"};
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    BeamEntry *cur = ctx->beam_a, *nxt = ctx->beam_b;
    beam_init_border(&cur[0], g_cur_bottom, g_cur_left);
    uint32_t beam_n = 1;
    g_beam_unpruned = false;          /* row 1 keeps the one-child economy */
    memset(ctx->log_n, 0, sizeof ctx->log_n);
    maha_reset_config();
    if (g_aux_on) corner_reset_config();
    if (g_corners_on) (void)tc_config(g_cur_left, cur[0].used);
    g_stats.configs++;

    const uint32_t last_row = gen_stop_row();
    for (int row = 1; (uint32_t)row <= last_row; row++) {
        if (g_stop)                      { res.reason = "interrupted"; break; }
        if (omp_get_wtime() >= deadline) { res.reason = "time";        break; }
        double t_row = omp_get_wtime();

        if (g_aux_on) g_corner_u = corner_unit(row);   /* read by every child */
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

        /* Under --backtrack_row_factor 0 the backtrack row keeps its candidates
           raw: every one is a root of the exhaustive search, and two roots
           sharing a frontier still differ below it. Every other row, the
           backtrack row included otherwise, deduplicates by frontier. ctx->keep
           drives the emission, so the stop row's emission order IS this
           ranking. */
        const bool bt_row = g_backtrack_row && (uint32_t)row == last_row;
        const bool raw = bt_row && g_bt_factor == 0;
        uint32_t kept = raw ? rank_pool_raw(ctx, pool_n, nt)
                            : dedup_and_rank(ctx, pool_n, nt);
        g_stats.row_retained[row] += kept;
        uint64_t root_rep = 0;
        if (bt_row && g_unlock_right && g_root_dedup) {
            kept = unique_roots(ctx, cur, kept, row, nt, &root_rep);
            g_bt_run.root_rep += root_rep; g_bt_run.cfg_root_rep = root_rep;
        }
        uint32_t roots = kept;
        if (bt_row && !raw) roots = select_roots(ctx, kept, beam_n, row, cfg_hash);
        g_stats.t_select += omp_get_wtime() - t_exp;
        res.row = (uint32_t)row;
        g_stats.rows_advanced++;

        if (bt_row) {
            res.width = roots;
            g_stats.row_selected[row] += roots;
            if (g_verbose) {
                printf("[beam] %s row=%d cands=%" PRIu64 " %s=%u roots=%u",
                       g_config_id_str, row, pool_n, raw ? "ranked" : "uniq", kept, roots);
                if (g_unlock_right) printf(" root_repeats=%" PRIu64, root_rep);
                printf(" t=%.2fs\n", omp_get_wtime() - t_row);
                fflush(stdout);
            }
            sweep_beam_done(roots);
            double t0 = omp_get_wtime();
            backtrack_emit(ctx, cur, roots, row, deadline, &res);
            g_stats.t_emit += omp_get_wtime() - t0;
            partials_budget_spent();
            break;
        }
        if ((uint32_t)row == g_stop_row) {
            res.width = kept;
            g_stats.row_selected[row] += kept;
            sweep_beam_done(kept);
            double t0 = omp_get_wtime();
            emit_stop_row(ctx, cur, kept, row);
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
    /* select_beam fills up to its target: the row width, or for the roots of
       --backtrack_row_factor M up to M x the width, never more than the pool. */
    uint64_t sel_n = KE;
    if (g_backtrack_row && g_bt_factor > 1)
        sel_n = KE * g_bt_factor < ctx->pool_cap ? KE * g_bt_factor : ctx->pool_cap;
    ctx->sel = xmalloc(sel_n * sizeof(uint32_t));
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
    /* The resumed run keeps the range it was given: it starts at the
       checkpoint's border row and still ends where --start_row + --num_rows
       ends, so a job that owns a slice of the rotations file never spills into
       the next job's rows. A checkpoint outside that slice belongs to another
       run. */
    if (br < g_start_row || br >= g_start_row + g_num_rows)
        fatal("--resume: %s is at border row %u, outside this run's rows %u..%u "
              "(--start_row/--num_rows must be the original run's)",
              path, br, g_start_row, g_start_row + g_num_rows - 1);
    g_num_rows -= br - g_start_row;
    g_start_row = br;
    g_resume_bi = bi;
    g_resume_li = li;
    if (nf >= 4) g_resume_sol_idx = (uint64_t)sol;
    g_resume_active = true;
    printf("[resume] start_row=%u bi=%u li=%u sol_idx=%" PRIu64 "\n",
           g_start_row, g_resume_bi, g_resume_li, g_resume_sol_idx);
}

/* -- Summary ------------------------------------------------------------------ */

static void print_time_stamp(const char *what, double wall) {
    char buf[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z", &tmv);
    printf("[time] run %s %s", what, buf);
    if (wall >= 0.0) {
        const long w = (long)(wall + 0.5);
        printf(" (wall %ld:%02ld:%02ld)", w / 3600, (w / 60) % 60, w % 60);
    }
    printf("\n");
}

/* --verbose: every detail line of the run, ahead of the short summary. */
static void print_summary_detail(double wall_total, double init_s, double sweep_s) {
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
    if (g_backtrack_row && g_bt_run.roots) {
        printf("[sum] backtrack from row %u: roots=%" PRIu64 " roots_with_boards=%" PRIu64
               " nodes=%" PRIu64 " (%.1f Mnodes/s) cut_parity=%" PRIu64,
               g_backtrack_row, g_bt_run.roots, g_bt_run.roots_emitting, g_bt_run.nodes,
               g_bt_run.t > 0 ? (double)g_bt_run.nodes / g_bt_run.t / 1e6 : 0.0,
               g_bt_run.cut_parity);
        if (g_corners_on) printf(" cut_corner=%" PRIu64, g_bt_run.cut_corner);
        printf(" found=%" PRIu64 " time=%.1fs\n", g_bt_run.fill[g_stop_row], g_bt_run.t);
        printf("[sum] backtrack boards completing each row:");
        for (int r = (int)g_backtrack_row + 1; r <= (int)g_stop_row; r++)
            printf("  r%d:%" PRIu64, r, g_bt_run.fill[r]);
        printf("\n");
        if (g_extend_nodes && g_unlock_right) {
            const uint64_t b = g_bt_run.emitted;
            printf("[sum] layered extension (--extend_nodes %u), over %" PRIu64 " board(s) kept: "
                   "cells mean %.1f, max %d of %d in the band; whole layers mean %.2f, max %d of "
                   "%d; node cap hit %" PRIu64 "\n",
                   g_extend_nodes, b, b ? (double)g_bt_run.ext_cells / (double)b : 0.0,
                   g_bt_run.ext_max, g_lx_path[0].n,
                   b ? (double)g_bt_run.ext_cols / (double)b : 0.0, g_bt_run.ext_cols_max,
                   g_lx_path[0].nlayers, g_bt_run.ext_cap);
        } else if (g_extend_nodes) {
            const uint64_t b = g_bt_run.emitted;
            const int h = EDGE_LEN - (int)g_stop_row;
            printf("[sum] extension (--extend_nodes %u), over %" PRIu64 " board(s) kept: "
                   "cells mean %.1f, max %d of %d; whole columns mean %.2f, max %d of %d; "
                   "node cap hit %" PRIu64 "\n",
                   g_extend_nodes, b, b ? (double)g_bt_run.ext_cells / (double)b : 0.0,
                   g_bt_run.ext_max, h * EDGE_LEN,
                   b ? (double)g_bt_run.ext_cols / (double)b : 0.0, g_bt_run.ext_cols_max, EDGE_LEN,
                   g_bt_run.ext_cap);
        }
    }
    if (g_end_dive) dv_print_summary(wall_total);
    if (g_rsv_boards)
        printf("[sum] reserve at stop row: TOP pieces still free, mean %.2f of %.2f over %" PRIu64
               " board(s) (--lambda_reserve %.3g)\n",
               (double)g_rsv_free_sum / (double)g_rsv_boards,
               (double)g_rsv_marked_sum / (double)g_rsv_boards, g_rsv_boards, g_lambda_reserve);
    if (g_border_stats.bottoms_ranked) {
        printf("[sum] border prefilter: bottom-slots=%" PRIu64
               " no-clue-column=%" PRIu64
               " columns ordinary=%" PRIu64 " clue-compatible=%" PRIu64
               " run=%" PRIu64 " time=%.1fs",
               g_border_stats.bottoms_ranked,
               g_border_stats.bottoms_no_clue_column,
               g_border_stats.columns_ordinary_viable,
               g_border_stats.columns_clue_compatible,
               g_border_stats.columns_run,
               g_border_stats.clue_seconds);
        if (g_border_stats.columns_repeated)
            printf(" repeats=%" PRIu64 " plain-fallbacks=%" PRIu64 " bottoms-left=%" PRIu64,
                   g_border_stats.columns_repeated, g_border_stats.plain_fallbacks,
                   g_border_stats.bottoms_left);
        printf("\n");
    }
    if (g_corners_on) {
        tc_print_summary(g_stop_row, false);
        printf("[sum] corner unit u_row (pooled SD of the rest of the score):");
        for (int r = 1; r <= (int)g_stop_row; r++) {
            double sd = corner_pooled_sd(r);
            if (sd > 0.0) printf("  r%d:%.3f", r, sd);
        }
        printf("\n");
    }
    if (g_lambda_maha != 0.0) {
        /* The measured Mahalanobis spread per row, pooled over the run -- what
           --lambda_Mahalanobis is denominated in. A row below the sample floor
           stood the correction down. */
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
    if (g_clue_debug)
        for (int r = 1; r <= EDGE_LEN; r++)
            if (g_dbg_calls[r])
                printf("[clue] row %2d: parents=%llu sumA=%llu sumB=%llu sumC=%llu\n", r,
                       (unsigned long long)g_dbg_calls[r], (unsigned long long)g_dbg_nA[r],
                       (unsigned long long)g_dbg_nB[r], (unsigned long long)g_dbg_nC[r]);
}

/* "45s", "11m08s", "1h02m03s". */
static const char *fmt_dur(char *b, size_t n, double sec) {
    const long t = (long)(sec + 0.5);
    if (t < 60)        snprintf(b, n, "%lds", t);
    else if (t < 3600) snprintf(b, n, "%ldm%02lds", t / 60, t % 60);
    else               snprintf(b, n, "%ldh%02ldm%02lds", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

/* "12345", "1.2M", "3.4G": counts that stay short. */
static const char *fmt_cnt(char *b, size_t n, uint64_t v) {
    if (v < 1000000ULL)         snprintf(b, n, "%" PRIu64, v);
    else if (v < 1000000000ULL) snprintf(b, n, "%.1fM", (double)v / 1e6);
    else                        snprintf(b, n, "%.2fG", (double)v / 1e9);
    return b;
}

#define SCORE_LINE_MAX 6                  /* values shown on the summary's score lines */

/* "[sum] best boards: 388 row 120, 386 row 7, ... in FILE": where the listed
   boards were written, as the 0-based data row tools/E555_viewer.py --row
   takes. The file is named once when all of them share it. Extensions also
   give their whole columns. */
static void print_top_list(const char *what, const TopList *t, bool ext) {
    if (!t->n) return;
    bool one_file = true;
    for (int i = 1; i < t->n; i++) one_file &= t->r[i].file == t->r[0].file;
    printf("[sum] %s:", what);
    for (int i = 0; i < t->n; i++) {
        const TopRec *r = &t->r[i];
        printf("%s %d", i ? "," : "", r->v);
        if (ext && !g_unlock_right) printf(" (%d cols)", ext_cols_n(r->v, (int)g_stop_row));
        if (!one_file) printf(" %s", g_out_files[r->file]);
        printf(" row %" PRIu64, r->row);
    }
    if (one_file) printf(" in %s", g_out_files[t->r[0].file]);
    printf("\n");
}

/* The run summary: one short line per topic, only those that apply, nothing
   said twice; --verbose prints every detail line first. It ends with the
   written boards by score and where the best are, then, with the extension,
   the same two for its length. */
static void print_summary(double wall_total, double init_s, double sweep_s) {
    char b1[32], b2[32];
    printf("\n================= run summary =================\n");
    if (g_verbose) print_summary_detail(wall_total, init_s, sweep_s);

    const double bt_t = g_backtrack_row ? g_bt_run.t : 0.0;
    const double dv_t = g_end_dive ? dv_seconds() : 0.0;
    const double beam_t = sweep_s - bt_t - dv_t > 0.0 ? sweep_s - bt_t - dv_t : 0.0;
    printf("[sum] time: init %s, beam %s", fmt_dur(b1, sizeof b1, init_s), fmt_dur(b2, sizeof b2, beam_t));
    if (g_backtrack_row) printf(", backtrack %s", fmt_dur(b1, sizeof b1, bt_t));
    if (g_end_dive)      printf(", dives %s", fmt_dur(b1, sizeof b1, dv_t));
    printf("\n");
    if (g_stats.configs) {
        printf("[sum] configs: %" PRIu64 ", %.1f s each; %" PRIu64 " reached row %u\n",
               g_stats.configs, sweep_s / (double)g_stats.configs, g_stats.reached_stop, g_stop_row);
        bool any = false;
        for (int r = 1; r <= (int)g_stop_row; r++) {
            if (!g_stats.extinct_at[r]) continue;
            printf("%s r%d:%" PRIu64, any ? "" : "[sum] extinct:", r, g_stats.extinct_at[r]);
            any = true;
        }
        if (any) printf("\n");
    }
    if (g_backtrack_row && g_bt_run.roots) {
        printf("[sum] backtrack: %s nodes, %.0f M/s;", fmt_cnt(b1, sizeof b1, g_bt_run.nodes),
               g_bt_run.t > 0 ? (double)g_bt_run.nodes / g_bt_run.t / 1e6 : 0.0);
        for (int r = (int)g_backtrack_row + 1; r <= (int)g_stop_row; r++)
            printf(" r%d:%s", r, fmt_cnt(b1, sizeof b1, g_bt_run.fill[r]));
        printf("\n");
        if (g_extend_nodes) {
            const uint64_t b = g_bt_run.emitted;
            printf("[sum] extension: mean %.2f %s, max %d", b ? (double)g_bt_run.ext_cols / (double)b : 0.0,
                   g_unlock_right ? "layers" : "cols", g_bt_run.ext_cols_max);
            if (g_bt_run.ext_cap) printf("; node cap hit %" PRIu64, g_bt_run.ext_cap);
            printf("\n");
            if (g_cap_top && g_bt_run.cap_boards)
                printf("[sum] top cap: %.2f cells per board; %.0f%% of the boards with a whole "
                       "column closed over all of them\n",
                       (double)g_bt_run.cap_cells / (double)g_bt_run.cap_boards,
                       100.0 * (double)g_bt_run.cap_closed / (double)g_bt_run.cap_boards);
        }
        if (g_unlock_right) {
            printf("[sum] unlock_right %u: block rows 0..%u x columns 0..%d; %s roots searched, ",
                   g_unlock_right, g_stop_row, g_last_inner, fmt_cnt(b1, sizeof b1, g_bt_run.roots));
            printf("%s repeats dropped", fmt_cnt(b1, sizeof b1, g_bt_run.root_rep));
            if (mincol_cuts())
                printf("; %s boards short of layer %u", fmt_cnt(b1, sizeof b1, g_bt_run.cut_mincol),
                       g_unlock_right - g_bt_min_col);
            if (score_cuts())
                printf("; %s boards under --emit_score %d", fmt_cnt(b1, sizeof b1, g_bt_run.cut_score),
                       g_emit_score);
            printf("\n");
        }
        if (g_col_check) {
            printf("[sum] min_col %u: ", g_bt_min_col);
            printf("column check cut %s partial rows", fmt_cnt(b1, sizeof b1, g_bt_run.cut_col));
            if (g_bt_run.strip_unknown)
                printf(", %s undecided", fmt_cnt(b1, sizeof b1, g_bt_run.strip_unknown));
            printf("\n");
        }
    }
    if (g_corners_on) {
        uint64_t nb, tl0, tr0, both, joint;
        tc_hist_get(&nb, &tl0, &tr0, &both, &joint);
        if (nb) {
            const double pc = 100.0 / (double)nb;
            printf("[sum] corners: TL %.0f%%, TR %.0f%%, both %.0f%%, pair %.0f%%\n",
                   (double)(nb - tl0) * pc, (double)(nb - tr0) * pc, (double)both * pc, (double)joint * pc);
        }
    }
    if (g_rsv_boards)
        printf("[sum] reserve: %.1f of %.0f TOP pieces free\n",
               (double)g_rsv_free_sum / (double)g_rsv_boards,
               (double)g_rsv_marked_sum / (double)g_rsv_boards);
    printf("[sum] boards: found %" PRIu64, g_found_total);
    if (g_backtrack_row && g_bt_run.near_dup) printf(", near-dups %" PRIu64, g_bt_run.near_dup);
    if (g_backtrack_row && g_bt_run.dup)      printf(", repeats %" PRIu64, g_bt_run.dup);
    if (mincol_cuts())                        printf(", below min_col %" PRIu64, g_bt_run.cut_mincol);
    if (score_cuts())                         printf(", below emit_score %" PRIu64, g_bt_run.cut_score);
    if (g_end_dive)                           printf(", dived %" PRIu64, dv_boards());
    printf("\n");
    if (g_incomplete_top) {
        printf("[sum] partials: %zu (", g_partial_total);
        for (int k = 0; k < PART_N; k++)
            printf("%s%s %zu", k ? ", " : "", g_part_name[k], g_part_total[k]);
        printf(")\n");
    }
    if (g_exhaust) {
        printf("[sum] exhausted colour:");
        int k = 0;
        for (int c = 1; c <= MAX_EDGE_SIDE_COLOR; c++)
            if (g_exhaust_cfgs[c]) printf("%s c%d %" PRIu64, k++ ? "," : "", c, g_exhaust_cfgs[c]);
        if (g_exhaust_cfgs[0]) printf("%s plain %" PRIu64, k ? "," : "", g_exhaust_cfgs[0]);
        printf(" configs\n");
    }
    if (g_prefix[0]) printf("[sum] run prefix: %s -> %s\n", g_prefix, g_out_dir);
    printf("[sum] *** Output boards score:");
    int shown = 0;
    bool more = false;
    for (int sc = DV_EDGES; sc >= 0; sc--) {
        const uint64_t n = g_end_dive ? dv_score_count(sc) : g_score_hist[sc];
        if (!n) continue;
        if (shown == SCORE_LINE_MAX) { more = true; break; }
        printf("%s%d:%" PRIu64, shown ? "  " : " ", sc, n);
        shown++;
    }
    printf("%s%s (%" PRIu64 " written)\n", shown ? "" : " none", more ? " ..." : "", g_written_total);
    print_top_list("best boards", &g_top_score, false);
    if (g_backtrack_row && g_extend_nodes) {
        if (g_unlock_right) printf("[sum] *** Output boards extension (band cells):");
        else printf("[sum] *** Output boards extension (cells above row %u, top cap not counted):",
                    g_stop_row);
        shown = 0; more = false;
        for (int n = (int)(sizeof g_ext_hist / sizeof g_ext_hist[0]) - 1; n >= 0; n--) {
            if (!g_ext_hist[n]) continue;
            if (shown == SCORE_LINE_MAX) { more = true; break; }
            printf("%s%d:%" PRIu64, shown ? "  " : " ", n, g_ext_hist[n]);
            shown++;
        }
        printf("%s%s\n", shown ? "" : " none", more ? " ..." : "");
        print_top_list("longest extensions", &g_top_ext, true);
    }
    print_time_stamp("ended", wall_total);
    fflush(stdout);
}

/* -- main ------------------------------------------------------------------- */

/* Finish the configuration's queued stop-row boards and write the kept ones
   now, so a killed run loses at most the configuration in flight. Edge pieces
   keep the side their rotations row deals them, unless edges are free or the
   --backtrack_unlock_right band has given the frame back. */
static void dive_config(void) {
    dv_frame(!g_free_edges && !g_random_edges && !g_unlock_right);
    dv_run(g_board_id_str);
    dv_flush(g_completions_fp);
}

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
"  --prefix [NAME]        write every board as NAME_<config> in its first CSV cell,\n"
"                         to tell runs apart. NAME: letters, digits, '_' '.' '-'\n"
"                         (no quotes needed). Bare: a random 6-character code for\n"
"                         the run, printed in [cfg] and the summary; pass that code\n"
"                         again with --resume to keep the names\n"
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
"                         you), --top_columns = random left columns per bottom.\n"
"                         Each bottom is a random legal chain of edges between\n"
"                         randomly placed corners, the best of 32 such draws by\n"
"                         fan-out rank (--tau_bottoms softens the choice); each\n"
"                         left column is drawn the same way from the edges the\n"
"                         bottom left over (--tau_columns). --exhaust_border_color\n"
"                         changes how both are drawn: see there. A (bottom,\n"
"                         column) pair never runs twice in a run: a repeated\n"
"                         column is redrawn, and after 16 repeats the bottom's\n"
"                         columns are drawn plain (no exhaust colour), then the\n"
"                         bottom is left.\n"
"                         --start_row/--num_rows, --top_bottoms and --resume do\n"
"                         not apply\n"
"  --samples N            --random_edges: random bottoms to try (default 1; 0 = until\n"
"                         --wall_time/--max_emitted); --top_columns lefts each\n"
"  --exhaust_border_color --random_edges: every border uses up one frame colour\n"
"                         (drawn per bottom) within the bottom row and column 0\n"
"                         rows 1..stop_row, so the right column, the top row and\n"
"                         the left column above the stop row see only the other\n"
"                         four. The draws are no longer uniform: a colour is\n"
"                         picked per bottom (only colours that can be used up;\n"
"                         colour 2 rarely), edges carrying it are drawn 16x as\n"
"                         often, and draws are rejected and redrawn when the\n"
"                         corners break the rule, when a bottom leaves more of the\n"
"                         colour than column 0 rows 1..stop_row can hold, or when\n"
"                         a left column leaves any unplaced or puts one above the\n"
"                         stop row. A bottom must admit at least one such column.\n"
"                         The best of 32 ACCEPTED draws is kept, as above. After 4\n"
"                         colours fail, the bottom is a plain random one\n"
"  --BL N / --BR N        pin a seed piece index (0..255) to the Bottom-Left /\n"
"  --TL N / --TR N        Bottom-Right / Top-Left / Top-Right corner (--random_edges\n"
"                         only); unpinned corners are sampled, and with 3 pinned the\n"
"                         4th is forced. Each must be a genuine, distinct corner\n"
"  --incomplete_top       also emit stop-row boards with only part of the stop row:\n"
"                         two segments (A+B, A+C, B+C) to <...>_partial.csv and\n"
"                         segment B alone to <...>_partial_B.csv. A partial is dropped\n"
"                         when an earlier one of the same kind has the same pieces\n"
"                         below the stop row and differs in at most one stop-row\n"
"                         piece. All CSVs are APPENDED, never truncated. In\n"
"                         rotations mode every emitted board also carries the\n"
"                         configuration's fixed left column and top-right corner\n"
"\n"
"Beam shape:\n"
"  --beam_width K         boards kept per row (default 250000)\n"
"  --stop_row R           last row the beam fills, 1..13; reaching boards are emitted\n"
"                         (default 11: the beam reliably FILLS row 11 and reliably\n"
"                         dies attempting 12, so stopping at 11 emits that material\n"
"                         instead of discarding it -- hand row 12 to the finalizer)\n"
"  --backtrack_row N      stop the BEAM at row N (1..stop_row-1), then search every\n"
"                         root (--backtrack_row_factor) EXHAUSTIVELY, cell by cell\n"
"                         in row-major order, up to --stop_row: the fixed left column,\n"
"                         right edges from the border's own pool, clue pins enforced\n"
"                         and colour parity checked at every row; with\n"
"                         --lambda_corners a path also ends once NEITHER top corner\n"
"                         can be closed (no TL and no TR block alive). EVERY board\n"
"                         completing the stop row is emitted (exact duplicates\n"
"                         dropped); pair with --max_emitted. --incomplete_top is\n"
"                         ignored with a warning (default 0 = off)\n"
"  --backtrack_row_factor M\n"
"                         with --backtrack_row: row N is an ordinary beam row, and\n"
"                         its best M x (row width) boards -- same dedup, per-parent\n"
"                         cap and random band as any row -- are the roots, so\n"
"                         --pool_factor can grow without growing the search. 0 = row\n"
"                         N expanded as a stop row and EVERY candidate a root, raw\n"
"                         (up to pool_factor x width near-twins; the behaviour\n"
"                         before this flag) (default 2)\n"
"  --extend_nodes N       with --backtrack_row: every stop-row board goes on,\n"
"                         cell by cell in COLUMN-major order (column 1 bottom up,\n"
"                         then column 2, ...) over rows stop+1..14, cols 1..14, with\n"
"                         no breaks, and is written (or dived) from the deepest\n"
"                         prefix found -- still one line per stop-row board, its\n"
"                         open cells in the top-right corner. Row-14 cells must\n"
"                         show a colour the unplaced top border still carries.\n"
"                         N caps the search per board (default 100000; 0 = off)\n"
"  --cap_top [N]          with the extension: of its longest fillings, keep the one\n"
"                         whose top-left border closes exactly -- the TL corner, then\n"
"                         top edges over the whole columns, matching the row-14\n"
"                         colours below and each other -- and write that closure\n"
"                         on row 15. Never changes which boards are written or how\n"
"                         far they extend. N = 0 turns it off; bare or nonzero = on\n"
"                         (default 1)\n"
"  --backtrack_min_col K  with --backtrack_row: write only boards whose extension\n"
"                         fills columns 1..K of rows stop+1..14 whole; lets a lower\n"
"                         --stop_row run without flooding the output (default 0).\n"
"                         The search then cuts a partial row as soon as its column 1\n"
"                         can no longer be stacked to row 14 with unused pieces (and,\n"
"                         under --random_edges, column 0 above the stop row), and a\n"
"                         stop-row cell of columns 2..K whose top nothing can sit on:\n"
"                         the same boards are written, fewer are found, sooner\n"
"  --no_top_dedup         with --backtrack_row: keep stop-row boards whose top row\n"
"                         repeats the previous board's columns 1..K and differs from\n"
"                         it in at most one other cell. By default such a near twin\n"
"                         is dropped before its extension and dives, whether the\n"
"                         previous board was written or not\n"
"  --backtrack_unlock_right R\n"
"                         with --backtrack_row N (R in 2..13; default 0 = off): the\n"
"                         board is the block rows 0..S x columns 0..15-R and the rest\n"
"                         is the band, frame and the three free corners included.\n"
"                         Each root gives back columns 16-R..15 of rows 1..N, rows\n"
"                         N+1..S are searched over columns 1..15-R, and the\n"
"                         extension grows the band in layers hugging the block (up\n"
"                         column 15-R+k from row 0, then left along row S+k to\n"
"                         column 0), any edge on any side. --backtrack_min_col K then\n"
"                         counts the band columns allowed to stay open (default R:\n"
"                         every board is written with its longest extension), and\n"
"                         without --end_dive --emit_score S drops boards that\n"
"                         match fewer edges. Corner clues in the band go home.\n"
"                         Not with --lambda_corners, --free_top_clue or --cap_top\n"
"  --beam_expand E      late-search width multiplier (default 4; 1 = no expansion)\n"
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
"                         residual is new. The spread is measured per configuration,\n"
"                         falling back to the run's pooled spread for a thin row\n"
"                         (default 1.0, useful 0.5-1.5; 0 = off)\n"
"                         Both terms are always live. --lambda_Mahalanobis 0 is closure\n"
"                         alone and --lambda_J 0 is Mahalanobis alone, which is why\n"
"                         there is no --score_model.\n"
"  --lambda_corners [F]   TOP-CORNER SUPPLY. Per border row,\n"
"                         enumerates every legal filling of a small block against\n"
"                         each top corner -- with --clue_corners the 2x3 block the\n"
"                         row-13 clue closes (one catalog per clue frame), else the\n"
"                         3 cells next to the corner -- never runs a left column no\n"
"                         TL block can use, and scores each child by the blocks per\n"
"                         corner still buildable from unused pieces: -3/+1/+2/+3 for\n"
"                         0/1/2/3+, times F in units of the row's score SD. Bare flag\n"
"                         = 0.5; absent = 0 (off). Needs a rotations file (not\n"
"                         --random_edges) and --stop_row <= 12. With --free_edges\n"
"                         the top and right edge pieces are pooled: either may fill\n"
"                         the TR block or witness the top border\n"
"  --lambda_reserve F     keep Stage A's double-decker TOP reserve for last: every\n"
"                         inner piece the rotations row marks with spin 1 (the pieces\n"
"                         --double_decker TOP reserved for row 14) costs F, in units\n"
"                         of the row's score SD like --lambda_corners, for each one a\n"
"                         board has placed. Nothing is held: a reserve piece can still\n"
"                         be used when nothing else fits, and the dives and the\n"
"                         --backtrack_row search ignore it. Works with or without\n"
"                         --lambda_corners. The stop-row summary reports how many\n"
"                         reserve pieces the boards left free, also at F = 0\n"
"                         (default 0 = off; not with --random_edges)\n"
"  --clue_center          force the published center clue piece onto its cell, at its\n"
"                         orientation's spin (piece 138; one of the 4 center cells)\n"
"  --clue_corners         force the two published corner clues the beam can reach,\n"
"                         both on row 2; the row-13 pair is only reserved, never pinned,\n"
"                         and constrains no searched row. Clue pieces leave the database\n"
"  --free_top_clue        with --clue_corners and --backtrack_row: the extension above\n"
"                         the stop row fills both row-13 clue cells like any other and\n"
"                         may place the top-left clue on any cell (if it does not, the\n"
"                         clue goes home only where it matches its neighbours). The\n"
"                         top-right clue stays held through the extension and is then\n"
"                         left off the board, for the dives to place\n"
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
"Finishing boards (end dives):\n"
"  --end_dive [M]         complete every stop-row board (beam or --backtrack_row) to\n"
"                         256 pieces with greedy random dives that allow broken edges\n"
"                         (most-constrained cell, exact fit before a break, fewest\n"
"                         breaks, least-constraining value) and write the best\n"
"                         completion instead of the stop-row board. Stage 1: M/10\n"
"                         dives per board. Stage 2: the other M-M/10 in 18\n"
"                         cross-entropy rounds that learn from the board's own best\n"
"                         dives, for boards whose stage-1 best is >= S-4 (or the top\n"
"                         10%% of a configuration when that is under 20%%). Written\n"
"                         after each configuration, best first, as config_id,\n"
"                         connected edges, pos, rot; --max_emitted then caps the\n"
"                         written boards instead of stopping the search. Bare flag =\n"
"                         10000; absent = off\n"
"  --end_polish R         with --end_dive: on boards whose best dive is within 6 of S,\n"
"                         hill-climb the 32 best distinct dives (best swap or\n"
"                         re-rotation, repeated), then run R kick-and-polish rounds\n"
"                         (4 sampled swaps, re-polish, annealed accept) over 16\n"
"                         walks. 0 = polish only; absent = off\n"
"  --emit_score S         connected edges (of 480) a finished board needs to be\n"
"                         written (default 450); without --end_dive, under\n"
"                         --backtrack_unlock_right only, the extended board's\n"
"                         edges when S is given\n"
"  --corner_seeds N       with --end_dive and --lambda_corners: every stop-row board\n"
"                         with an alive top-corner block also dives up to N copies\n"
"                         with a block and a free pair of top witnesses fixed on\n"
"                         their cells, so that corner is clean; the unseeded board\n"
"                         is dived too (default 4, 0 = off)\n"
"\n"
"Selection:\n"
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
"  --bc_window_accept F   full-beam regime: keep the window's best child with\n"
"                         probability F, else its second best (default 1)\n"
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
"                         greedy head. The ranks of the leading bottoms differ by\n"
"                         well under 1 nat, so the scale is small: 0.1 keeps about\n"
"                         half the picks in the greedy top 10 and measured no yield\n"
"                         cost; 2 is close to a uniform draw over every bottom and\n"
"                         measured ~10%% fewer row-11 configurations. 0 = off, the\n"
"                         greedy head (default 0)\n"
"  --tau_columns T        the same for the left-column ranking (0.1 measured no\n"
"                         yield cost; default 0)\n"
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
"  --resume               continue from <out_dir>/sweep_checkpoint.txt. Give the\n"
"                         original --start_row/--num_rows: the run ends where the\n"
"                         original would have. A configuration cut by --wall_time or\n"
"                         a signal before it wrote anything is run again. SIGINT/\n"
"                         SIGTERM finish the current configuration and print the\n"
"                         summary; a second signal kills\n"
"\n"
"Misc:\n"
"  --threads N            OpenMP threads (default: all cores)\n"
"  --rng_seed S           RNG seed; omitted = randomized from clock+pid. Explicit\n"
"                         0 is a valid deterministic seed, like any other integer\n"
"  --verbose              per-row [beam] traces, every [sweep] line, [rank]/[bail]\n"
"                         lines and the [dfs]/[dive] lines per configuration\n"
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
    /* Grouped, most important first: borders, clues, beam, score, the top of the
       board, the finish, budget, I/O. A flag at its default or inert without its
       parent flag is left out; everything else is printed, so the line replays. */
    if (g_random_edges)  printf(" --random_edges --samples %u", g_samples);
    else                 printf(" --start_row %u --num_rows %u", g_start_row, g_num_rows);
    if (g_exhaust)       printf(" --exhaust_border_color");
    if (g_free_edges)    printf(" --free_edges");
    for (int k = 0; k < 4; k++)
        if (g_fixed_corner_pid[k] >= 0) printf(" %s %d", corner[k], g_fixed_corner_pid[k]);
    printf(" --top_bottoms %ld --top_columns %ld", g_top_bottoms, g_top_columns);
    printf(" --tau_bottoms %g --tau_columns %g", g_tau_bottoms, g_tau_columns);
    printf(" --bail_columns %u", g_bail_columns);
    printf(" --rng_seed %" PRIu64, g_master_seed);
    /* --pin_clue turns CLUE_CENTER on itself, so printing both is not a
       contradiction -- the line stays correct if the implication ever changes. */
    if (g_clue_mask & CLUE_CENTER)  printf(" --clue_center");
    if (g_clue_mask & CLUE_CORNERS) printf(" --clue_corners");
    if (g_pin_clue)      printf(" --pin_clue %d", g_pin_clue);
    if (g_free_top_clue) printf(" --free_top_clue");
    printf(" --beam_width %u --beam_expand %u --beam_expand_row %u",
           g_beam_width, g_beam_expand, g_beam_expand_row);
    printf(" --pool_factor %u --bc_window %u,%u", g_pool_factor, g_bc_nB, g_bc_nC);
    if (g_bc_accept < 1.0) printf(" --bc_window_accept %g", g_bc_accept);
    printf(" --parent_cap %u --frac_rand %g", g_parent_cap, g_frac_rand);
    printf(" --lambda_J %g --lambda_Mahalanobis %g", g_lambda_J, g_lambda_maha);
    if (g_lambda_corners > 0.0) printf(" --lambda_corners %g", g_lambda_corners);
    if (g_lambda_reserve > 0.0) printf(" --lambda_reserve %g", g_lambda_reserve);
    if (!g_free_demand)  printf(" --no_free_demand");
    printf(" --stop_row %u", g_stop_row);
    if (g_incomplete_top) printf(" --incomplete_top");
    if (!g_cap_top && !g_unlock_right) printf(" --cap_top 0");
    if (g_backtrack_row) printf(" --backtrack_row %u --backtrack_row_factor %u --extend_nodes %u",
                                g_backtrack_row, g_bt_factor, g_extend_nodes);
    if (g_unlock_right)  printf(" --backtrack_unlock_right %u", g_unlock_right);
    if (g_bt_min_col || g_unlock_right) printf(" --backtrack_min_col %u", g_bt_min_col);
    if (g_backtrack_row && !g_top_dedup) printf(" --no_top_dedup");
    if (g_end_dive) printf(" --end_dive %u", g_end_dive);
    if (g_end_dive && g_end_polish >= 0) printf(" --end_polish %d", g_end_polish);
    if (g_end_dive || score_cuts()) printf(" --emit_score %d", g_emit_score);
    if (g_end_dive && g_corners_on && g_corner_seeds != 4) printf(" --corner_seeds %d", g_corner_seeds);
    printf(" --time_limit %g --wall_time %g --max_emitted %" PRIu64 " --threads %d",
           g_config_time_sec, g_max_wall_sec, g_max_partials, g_nthreads);
    printf(" --out_dir %s", g_out_dir);
    if (g_prefix[0])     printf(" --prefix %s", g_prefix);
    if (g_db_file)       printf(" --db_file %s", g_db_file);
    if (resume)          printf(" --resume");
    if (g_verbose)       printf(" --verbose");
    if (g_print_cmd)     printf(" --print_cmd");
    printf("\n");
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
/* Compact log: the run of consecutive barren configurations (nothing found,
   ordinary end) of one bottom, printed as one line when the run ends. */
static char     g_cb_group[64] = "";
static long     g_cb_first = -1, g_cb_last = -1;
static uint32_t g_cb_n = 0;
static double   g_cb_wall = 0.0;
static uint32_t g_cb_died[EDGE_LEN + 2];

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

/* This configuration's boards: found at the stop row (the backtracker's
   completions, or the beam's unique stop-row boards) and written to the CSV
   (under --end_dive, the dived boards kept at >= --emit_score), with the run
   totals of both. Set after the configuration's dives by cfg_count_written. */
static void cfg_count_written(uint64_t dv_written_before) {
    g_cfg_found   = g_backtrack_row ? g_bt_run.cfg_found : (uint64_t)g_emit_count;
    g_cfg_written = g_end_dive ? dv_written() - dv_written_before : (uint64_t)g_emit_count;
    g_found_total += g_cfg_found; g_written_total += g_cfg_written;
}

static void barren_flush(void) {
    if (!g_cb_n) return;
    if (g_cb_n == 1) printf("[sweep] %sl%ld found=0 died", g_cb_group, g_cb_first);
    else printf("[sweep] %sl%ld-l%ld x%u found=0 died", g_cb_group, g_cb_first, g_cb_last, g_cb_n);
    for (int r = 0; r < EDGE_LEN + 2; r++)
        if (g_cb_died[r]) printf(" r%d:%u", r, g_cb_died[r]);
    printf(" wall=%.1fs\n", g_cb_wall);
    fflush(stdout);
    g_cb_group[0] = '\0'; g_cb_first = g_cb_last = -1; g_cb_n = 0; g_cb_wall = 0.0;
    memset(g_cb_died, 0, sizeof g_cb_died);
}

static void sweep_flush(void) {
    if (!g_verbose) {
        barren_flush();
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
        /* Compact log: a line for configurations that found boards or
           two-segment partials, or ended for an unusual reason; the barren
           rest of a bottom collapse into one line per run of them, with the
           rows they died at. Every count is this configuration's (or the
           run's); run totals are in the summary. */
        const size_t strong_partials = g_part_count[PART_AB]
                                     + g_part_count[PART_AC]
                                     + g_part_count[PART_BC];
        const bool productive = g_cfg_found != 0 || strong_partials != 0;
        const bool exceptional = strcmp(br->reason, "extinct") != 0
                              && strcmp(br->reason, "stop_row") != 0;
        const bool open = g_sweep_open;   /* the head is out: always finish the line */
        g_sweep_open = false;
        if (open && !productive && !exceptional && !filled) {
            printf(" died r%u wall=%.1fs\n", br->row, wall);
            fflush(stdout);
            return;
        }
        if (!open && (productive || exceptional)) {
            barren_flush();
            printf("[sweep] %sl%ld", group, li);
        }
        if (open || productive || exceptional) {
            printf(" found=%" PRIu64, g_cfg_found);
            if (g_backtrack_row && g_bt_run.cfg_dup) printf(" repeats=%" PRIu64, g_bt_run.cfg_dup);
            if (g_backtrack_row && g_bt_run.cfg_near_dup)
                printf(" dups=%" PRIu64, g_bt_run.cfg_near_dup);
            if (mincol_cuts()) printf(" below_min_col=%" PRIu64, g_bt_run.cfg_mincol);
            if (score_cuts()) printf(" below_score=%" PRIu64, g_bt_run.cfg_cut_score);
            if (g_unlock_right && g_bt_run.cfg_root_rep)
                printf(" root_repeats=%" PRIu64, g_bt_run.cfg_root_rep);
            printf(" written=%" PRIu64, g_cfg_written);
            if (g_end_dive && dv_last_best() >= 0) printf(" best=%d", dv_last_best());
            if (g_incomplete_top && g_partial_count) printf(" partials=%zu", g_partial_count);
            if (exceptional) printf(" stopped=%s at row %u", sweep_reason(br), br->row);
            printf(" wall=%.1fs\n", wall);
            fflush(stdout);
            return;
        }

        if (g_cb_n && strcmp(g_cb_group, group)) barren_flush();
        if (!g_cb_n) { snprintf(g_cb_group, sizeof g_cb_group, "%s", group); g_cb_first = li; }
        g_cb_last = li; g_cb_n++; g_cb_wall += wall;
        if (br->row < (uint32_t)(EDGE_LEN + 2)) g_cb_died[br->row]++;
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
    if (g_end_dive) printf(" written=%" PRIu64 " wall=%.1fs\n", dv_written(), wall);
    else            printf(" sol_total=%" PRIu64 " wall=%.1fs\n", g_solution_idx, wall);
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
        else if (!strcmp(argv[i], "--backtrack_row") && i+1 < argc) {
            int v = atoi(argv[++i]);
            g_backtrack_row = v > 0 ? (uint32_t)v : 0; g_backtrack_row_set = true;
        }
        else if (!strcmp(argv[i], "--backtrack_row_factor") && i+1 < argc) {
            char *end; long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > 1000000L)
                fatal("--backtrack_row_factor needs an integer in 0..1000000, got '%s'", argv[i]);
            g_bt_factor = (uint32_t)v; g_bt_factor_set = true;
        }
        else if (!strcmp(argv[i], "--extend_nodes") && i+1 < argc) {
            char *end; long long v = strtoll(argv[++i], &end, 10);
            if (*end || v < 0 || v > 1000000000LL)
                fatal("--extend_nodes expects an integer in 0..1000000000, got '%s'", argv[i]);
            g_extend_nodes = (uint32_t)v; g_extend_set = true;
        }
        else if (!strcmp(argv[i], "--backtrack_min_col") && i+1 < argc) {
            char *end; long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > EDGE_LEN)
                fatal("--backtrack_min_col expects an integer in 0..%d, got '%s'", EDGE_LEN, argv[i]);
            g_bt_min_col = (uint32_t)v; g_min_col_set = true;
        }
        else if (!strcmp(argv[i], "--backtrack_unlock_right") && i+1 < argc) {
            char *end; long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v == 1 || v > EDGE_LEN - 1)
                fatal("--backtrack_unlock_right expects 0 (off) or an integer in 2..%d, got '%s'",
                      EDGE_LEN - 1, argv[i]);
            g_unlock_right = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "--free_top_clue"))             g_free_top_clue = true;
        else if (!strcmp(argv[i], "--no_top_dedup"))              g_top_dedup = false;
        else if (!strcmp(argv[i], "--exhaust_border_color"))      g_exhaust = true;
        else if (!strcmp(argv[i], "--cap_top")) {
            /* Bare or nonzero = on, 0 = off; the value is taken only when the
               next token is an integer in its entirety. */
            g_cap_top = true; g_cap_top_set = true;
            if (i + 1 < argc) {
                char *end = NULL;
                long v = strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0') { g_cap_top = v != 0; i++; }
            }
        }
        else if (!strcmp(argv[i], "--prefix")) {
            /* The name is optional: bare, the run draws a 6-character code. */
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) {
                const char *v = argv[++i];
                size_t n = strlen(v);
                if (n == 0 || n > 32) fatal("--prefix must be 1..32 characters, got '%s'", v);
                for (size_t k = 0; k < n; k++)
                    if (!isalnum((unsigned char)v[k]) && v[k] != '_' && v[k] != '.' && v[k] != '-')
                        fatal("--prefix may hold only letters, digits, '_', '.' and '-' "
                              "(no spaces or commas: it starts a CSV cell), got '%s'", v);
                snprintf(g_prefix, sizeof g_prefix, "%s", v);
            } else {
                run_code(g_prefix);
            }
        }
        else if (!strcmp(argv[i], "--end_dive")) {
            /* The value is optional, as for --lambda_corners: a bare --end_dive
               means DV_M_DEFAULT dives per board. */
            long v = DV_M_DEFAULT;
            if (i + 1 < argc) {
                char *end = NULL;
                long w = strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0') { v = w; i++; }
            }
            if (v < 10 || v > 1000000000L) fatal("--end_dive must be in 10..1000000000");
            g_end_dive = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "--corner_seeds") && i+1 < argc) {
            g_corner_seeds = atoi(argv[++i]);
            if (g_corner_seeds < 0 || g_corner_seeds > 64) fatal("--corner_seeds must be in 0..64");
        }
        else if (!strcmp(argv[i], "--end_polish") && i+1 < argc) {
            g_end_polish = atoi(argv[++i]);
            if (g_end_polish < 0) fatal("--end_polish must be >= 0");
        }
        else if (!strcmp(argv[i], "--emit_score") && i+1 < argc) {
            g_emit_score = atoi(argv[++i]); g_emit_score_set = true;
        }
        else if (!strcmp(argv[i], "--beam_expand") && i+1 < argc) g_beam_expand = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beam_expand_row") && i+1 < argc) g_beam_expand_row = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_Mahalanobis") && i+1 < argc) g_lambda_maha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_J")    && i+1 < argc) g_lambda_J = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lambda_corners")) {
            /* The value is optional: taken only when the next token is a number
               in its entirety, so `--lambda_corners --stop_row 12` is the bare
               flag followed by another flag, not a parse error. */
            g_lambda_corners = TC_LAMBDA_DEFAULT;
            if (i + 1 < argc) {
                char *end = NULL;
                double v = strtod(argv[i + 1], &end);
                if (end != argv[i + 1] && *end == '\0') { g_lambda_corners = v; i++; }
            }
        }
        else if (!strcmp(argv[i], "--no_free_demand"))            g_free_demand = false;
        else if (!strcmp(argv[i], "--clue_center"))               g_clue_mask |= CLUE_CENTER;
        else if (!strcmp(argv[i], "--clue_corners"))              g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(argv[i], "--lambda_reserve") && i+1 < argc) g_lambda_reserve = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pin_clue")    && i+1 < argc) g_pin_clue = atoi(argv[++i]);
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
        else if (!strcmp(argv[i], "--bc_window_accept") && i+1 < argc) {
            char *end; g_bc_accept = strtod(argv[++i], &end);
            if (*end || !(g_bc_accept >= 0.0 && g_bc_accept <= 1.0))
                fatal("--bc_window_accept needs F in [0,1]");
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
    if (!(g_lambda_corners >= 0.0 && g_lambda_corners <= 1e6))
        fatal("--lambda_corners must be in [0,1e6] (0 = off)");
    if (!(g_lambda_reserve >= 0.0 && g_lambda_reserve <= 1e6))
        fatal("--lambda_reserve must be in [0,1e6]");
    if (g_lambda_reserve > 0.0 && g_random_edges)
        fatal("--lambda_reserve reads Stage A's reserve marks from a rotations row; "
              "--random_edges has none");
    g_corners_on = (g_lambda_corners > 0.0);
    g_aux_on = g_corners_on || g_lambda_reserve > 0.0;
    if (g_corners_on) {
        if (g_random_edges)
            fatal("--lambda_corners needs a rotations file, not --random_edges: the "
                  "catalog is built from the sides it deals");
        /* --free_edges: any unused edge may end a row, so the top border and the
           right column above the stop row are one pool. The catalog then lets
           each edge dealt to the top or to the right fill a TR block side cell
           or witness the top border (E555_database.c). */
        g_tc_pool_top_right = g_free_edges;
        if (g_stop_row > (uint32_t)(PUZZLE_SIDE - 4))
            fatal("--lambda_corners needs --stop_row 12 or below: rows 13-14 hold the "
                  "corner blocks it protects");
    }
    if (g_beam_width == 0) fatal("--beam_width must be positive");
    if (g_beam_width > (1u << 24)) fatal("--beam_width above 2^24 not supported");
    if (g_beam_expand < 1 || g_beam_expand > 64) fatal("--beam_expand must be in 1..64");
    if (g_beam_expand_row < 2) fatal("--beam_expand_row must be >= 2");
    if (g_stop_row == 0 || g_stop_row > (uint32_t)MAX_DRILL_DEPTH)
        fatal("--stop_row must be in 1..%d (rows 14-15 belong to Stage C)", MAX_DRILL_DEPTH);
    if (g_unlock_right) {
        const int W = PUZZLE_SIDE - 1 - (int)g_unlock_right;
        if (!g_backtrack_row_set)
            fatal("--backtrack_unlock_right gives back columns from the backtrack row on: "
                  "add --backtrack_row N");
        if (g_min_col_set && g_bt_min_col > g_unlock_right)
            fatal("--backtrack_min_col counts the band columns left open under "
                  "--backtrack_unlock_right %u: it must be in 0..%u", g_unlock_right, g_unlock_right);
        if (!g_min_col_set) g_bt_min_col = g_unlock_right;
        if (g_bt_min_col < g_unlock_right && !g_extend_nodes)
            fatal("--backtrack_min_col %u needs %u whole layer(s) of the extension: "
                  "--extend_nodes 0 turns it off", g_bt_min_col, g_unlock_right - g_bt_min_col);
        if (g_corners_on)
            fatal("--lambda_corners protects top-corner blocks built on the configuration's "
                  "frame, which --backtrack_unlock_right leaves open: drop one of the two");
        if (g_free_top_clue)
            fatal("--free_top_clue acts in the column-major extension, which "
                  "--backtrack_unlock_right replaces: drop one of the two");
        if (g_cap_top_set && g_cap_top)
            fatal("--cap_top closes the top border over the column-major extension; under "
                  "--backtrack_unlock_right the layered extension places the frame itself");
        g_cap_top = false;
        if (g_clue_mask & CLUE_CORNERS) {
            if (W < 13 && g_backtrack_row < 2)
                fatal("--backtrack_unlock_right %u with --clue_corners needs --backtrack_row 2 or "
                      "more: the (2,13) clue goes home in the band, so the beam must place the "
                      "cell beside it", g_unlock_right);
            g_band_pins = true;
        }
        g_last_inner = W;
        g_block_mask = (uint16_t)((1u << (W + 1)) - 1u);
        if (getenv("E555_ROOT_DEDUP") && !strcmp(getenv("E555_ROOT_DEDUP"), "0")) g_root_dedup = false;
    }
    if (g_backtrack_row_set) {
        if (g_backtrack_row == 0 || g_backtrack_row >= g_stop_row)
            fatal("--backtrack_row must be in 1..%u (below --stop_row %u)",
                  g_stop_row - 1, g_stop_row);
        if (g_bt_min_col && !g_extend_nodes && !g_unlock_right)
            fatal("--backtrack_min_col %u needs the extension: --extend_nodes 0 turns it off",
                  g_bt_min_col);
        if (g_incomplete_top) {
            fprintf(stderr, "[warn] --incomplete_top has no effect with --backtrack_row: "
                            "the backtracker emits only complete stop-row boards\n");
            g_incomplete_top = false;
        }
    }
    if (g_bt_factor_set && !g_backtrack_row_set)
        fatal("--backtrack_row_factor sets how many roots the backtracker gets: "
              "add --backtrack_row N");
    if ((g_extend_set || g_min_col_set) && !g_backtrack_row_set)
        fatal("--extend_nodes and --backtrack_min_col extend the backtracker's stop-row "
              "boards: add --backtrack_row N");
    if (!g_top_dedup && !g_backtrack_row_set)
        fatal("--no_top_dedup turns off a filter of the backtracker's stop-row boards: "
              "add --backtrack_row N");
    if (g_free_top_clue) {
        if (!(g_clue_mask & CLUE_CORNERS))
            fatal("--free_top_clue frees the row-13 corner clues: add --clue_corners");
        if (!g_backtrack_row_set || !g_extend_nodes)
            fatal("--free_top_clue acts in the extension above the stop row: it needs "
                  "--backtrack_row N and --extend_nodes > 0");
    }
    g_col_check = g_bt_min_col > 0 && !g_unlock_right;
    if (g_emit_score < 0 || g_emit_score > 480) fatal("--emit_score must be in 0..480");
    if (g_end_polish >= 0 && !g_end_dive)
        fprintf(stderr, "[warn] --end_polish has no effect without --end_dive\n");
    if (g_emit_score_set && !g_end_dive && !g_unlock_right)
        fprintf(stderr, "[warn] --emit_score has no effect without --end_dive\n");
    if (!(fabs(g_lambda_maha) <= 1e6)) fatal("--lambda_Mahalanobis in [-1e6,1e6]");
    if (!(fabs(g_lambda_J) <= 1e6))    fatal("--lambda_J in [-1e6,1e6]");
    /* No clue cap on --stop_row. Row 13's two clues are reserved, never pinned,
       and the attach in board_arrays yields to a searched row 13 -- which is
       built from other pieces, so the clues are left off rather than written on
       top of them. */
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
    if (g_exhaust && !g_random_edges)
        fatal("--exhaust_border_color needs --random_edges (a rotations file fixes the frame)");
    g_exhaust_rows = (int)g_stop_row;
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
    /* The rotations file is checked here, before anything slow: with an
       explicit --num_rows it used to be opened only after the chain database
       was built or loaded, so a missing file surfaced minutes into the run. */
    if (!g_random_edges) {
        uint32_t rot_lines = count_border_rows(csv_path);       /* fatal if unreadable */
        if (rot_lines == 0) fatal("rotation CSV %s holds no border row", csv_path);
        if (g_start_row >= rot_lines)
            fatal("--start_row %u is past the last border row of %s (%u rows)",
                  g_start_row, csv_path, rot_lines);
        if (g_num_rows == 0) g_num_rows = rot_lines - g_start_row;
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
    if (g_unlock_right && (g_clue_mask & CLUE_CENTER))
        for (int o = 0; o < 4; o++) {
            if (!(g_clue_orients & (1u << o))) continue;
            const ClueCell *cc = &g_clue[o][0];
            if (cc->row > g_stop_row || cc->col > (uint32_t)g_last_inner)
                fatal("--backtrack_unlock_right %u leaves the centre clue's cell (%u,%u) outside "
                      "the block rows 0..%u x columns 0..%d: lower R, raise --stop_row, or pin "
                      "another frame with --pin_clue", g_unlock_right, cc->row, cc->col,
                      g_stop_row, g_last_inner);
        }

    printf("\n=== E555 beamer ===\n\n");
    print_time_stamp("started", -1.0);
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
    if (g_backtrack_row && g_bt_factor)
        printf("[cfg] backtrack_row=%u factor=%u (beam through row %u, its best %u x width "
               "boards the roots of an exhaustive row-major search to row %u)\n",
               g_backtrack_row, g_bt_factor, g_backtrack_row, g_bt_factor, g_stop_row);
    else if (g_backtrack_row)
        printf("[cfg] backtrack_row=%u factor=0 (beam through row %u, then an exhaustive "
               "row-major search of every row-%u candidate to row %u)\n",
               g_backtrack_row, g_backtrack_row, g_backtrack_row, g_stop_row);
    if (g_unlock_right) {
        const int W = g_last_inner;
        printf("[cfg] backtrack_unlock_right=%u (the roots give back columns %d..15 of rows 1..%u; "
               "rows %u..%u are searched over columns 1..%d; the board is the block rows 0..%u x "
               "columns 0..%d, the rest is the band, frame and corners included)\n",
               g_unlock_right, W + 1, g_backtrack_row, g_backtrack_row + 1, g_stop_row, W,
               g_stop_row, W);
        if (g_extend_nodes)
            printf("[cfg] extend_nodes=%u (each row-%u board grows the band in layers hugging the "
                   "block: up column %d+k from row 0, then left along row %u+k to column 0; any "
                   "edge on any side, up to %u nodes; written from its deepest zero-break "
                   "prefix)\n", g_extend_nodes, g_stop_row, W, g_stop_row, g_extend_nodes);
        if (mincol_cuts())
            printf("[cfg] backtrack_min_col=%u (band columns left open: only boards whose "
                   "extension completes layers 1..%u, out to column %u, are written)\n",
                   g_bt_min_col, g_unlock_right - g_bt_min_col, 15 - g_bt_min_col);
        else
            printf("[cfg] backtrack_min_col=%u (band columns left open: every stop-row board is "
                   "written with its extension)\n", g_bt_min_col);
        if (score_cuts())
            printf("[cfg] emit_score=%d (a board whose extended board matches fewer edges is not "
                   "written)\n", g_emit_score);
    } else {
        if (g_backtrack_row && g_extend_nodes)
            printf("[cfg] extend_nodes=%u (each row-%u board continues column by column, rows %u..14, "
                   "up to %u nodes; written from its deepest zero-break prefix)\n",
                   g_extend_nodes, g_stop_row, g_stop_row + 1, g_extend_nodes);
        if (g_bt_min_col)
            printf("[cfg] backtrack_min_col=%u (only boards whose extension fills columns 1..%u "
                   "of rows %u..14 are written; a partial row whose column 1 cannot reach row 14 is "
                   "cut)\n", g_bt_min_col, g_bt_min_col, g_stop_row + 1);
    }
    if (g_backtrack_row)
        printf("[cfg] top_dedup=%s\n", !g_top_dedup ? "off" : g_unlock_right
               ? "on (a stop-row board within one cell of the previous one's top row is "
                 "dropped before its extension)"
               : "on (a stop-row board within one top-row cell of the previous one, its "
                 "columns 1..K the same, is dropped before its extension)");
    if (g_free_top_clue)
        printf("[cfg] free_top_clue=1 (the extension may place the top-left row-13 clue on any "
               "cell; the top-right one is left for the dives)\n");
    if (g_exhaust)
        printf("[cfg] exhaust_border_color=1 (each random border uses up one frame colour, drawn "
               "per bottom, within the bottom row and column 0 rows 1..%u; a bottom that "
               "cannot is a plain random one)\n", g_stop_row);
    if (g_random_edges && g_backtrack_row && g_extend_nodes)
        printf("[cfg] random edges: the extension chooses column 0 above row %u\n", g_stop_row);
    if (g_backtrack_row && g_extend_nodes && !g_unlock_right)
        printf("[cfg] cap_top=%d%s\n", g_cap_top ? 1 : 0,
               g_cap_top ? " (of its longest fillings, the extension keeps the one whose top-left "
                           "border closes exactly over the most whole columns, and writes that cap)" : "");
    if (g_prefix[0])
        printf("[cfg] run prefix: %s (boards are named %s_<config>) -> %s\n",
               g_prefix, g_prefix, g_out_dir);
    if (g_end_dive) {
        printf("[cfg] end_dive=%u (stage 1 %u dives per stop-row board, stage 2 %u more for "
               "boards >= S-4, or the top 10%% if that is under 20%%) emit_score=%d\n",
               g_end_dive, g_end_dive / 10, g_end_dive - g_end_dive / 10, g_emit_score);
        if (g_end_polish >= 0)
            printf("[cfg] end_polish=%d (polish the 32 best dives of each board within 6 "
                   "of S, then %d kick-and-polish rounds over 16 walks)\n",
                   g_end_polish, g_end_polish);
        if (g_corners_on)
            printf("[cfg] corner_seeds=%d (%s)\n", g_corner_seeds, g_corner_seeds
                   ? "each stop-row board with an alive top-corner block also dives up to "
                     "that many copies with a block and its top witnesses fixed in place"
                   : "off");
    }
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
    printf("[cfg] lambda_J=%.3f lambda_Maha=%.3f free_demand=%d bc_window=%u,%u",
           g_lambda_J, g_lambda_maha, g_free_demand?1:0, g_bc_nB, g_bc_nC);
    if (g_bc_accept < 1.0) printf(" accept=%.2f", g_bc_accept);
    printf("\n");
    if (g_corners_on)
        printf("[cfg] lambda_corners=%.3f (score-SD units; %s top-corner blocks)\n",
               g_lambda_corners, (g_clue_mask & CLUE_CORNERS) ? "clue 2x3" : "3-cell");
    if (g_lambda_reserve > 0.0)
        printf("[cfg] lambda_reserve=%.3f (score-SD units per TOP reserve piece placed)\n",
               g_lambda_reserve);
    printf("[cfg] top_bottoms=%ld top_columns=%ld time_limit=%.0fs wall_time=%.0fs max_emitted=%" PRIu64 " db_file=%s\n",
           g_top_bottoms, g_top_columns, g_config_time_sec, g_max_wall_sec, g_max_partials,
           g_db_file ? g_db_file : "(none)");
    printf("[cfg] tau_bottoms=%.2f tau_columns=%.2f bail_columns=%u\n",
           g_tau_bottoms, g_tau_columns, g_bail_columns);
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
    ext_fix_init();
    if (g_unlock_right) lx_init();
    if (g_random_edges) g_edge_left_n = edge_left_pool(&g_edge_left);
    {   /* --cap_top: the frame-up edges, by the inner colour they show down */
        const int n = edge_up_pool(&g_edge_up);
        for (int k = 0; k < n; k++) {
            const int c = g_edge_up[k].bottom;
            g_up_by_in[c][g_up_by_in_n[c]++] = (uint8_t)k;
        }
    }
    if (g_exhaust) {
        /* Before the database: a pin set that rules every colour out is caught
           in milliseconds, and no draw is ever wasted on an impossible colour. */
        finalize_fixed_corners();
        printf("[cfg] exhaust colours possible:");
        for (int c = 1; c <= MAX_EDGE_SIDE_COLOR; c++)
            if (exhaust_colour_possible(c, g_exhaust_rows)) {
                g_exhaust_ok[g_exhaust_ok_n++] = c;
                printf(" c%d%s", c, c == 2 ? " (accepted 1 in 5)" : "");
            }
        printf("%s\n", g_exhaust_ok_n ? "" : " none");
        if (!g_exhaust_ok_n)
            fatal("--exhaust_border_color: no frame colour can be used up with these corners "
                  "within the bottom row and left rows 1..%u", g_stop_row);
    }

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
    if (g_end_dive) {
        DvParams dp = {
            .dives = g_end_dive, .emit_score = g_emit_score, .polish = g_end_polish,
            .corner_seeds = g_corner_seeds, .seed_corners = g_corners_on,
            .master_seed = g_master_seed,
            .max_written = g_max_partials,
            .deadline = g_max_wall_sec > 0.0 ? t_start + g_max_wall_sec : 0.0,
            .threads = g_nthreads, .stop = &g_stop,
        };
        dv_init(&dp);
        dv_set_row_hook(dive_row_written);
    }
    double t_sweep0 = omp_get_wtime();
    double init_s = t_sweep0 - t_start;

    if (g_random_edges) {
        /* Random-border sweep: no rotation CSV. --samples random bottoms,
           --top_columns random left columns per bottom, each border the best of
           RANDOM_SIDE_SAMPLES fan-out-ranked samples (--top_bottoms is moot:
           every sampled bottom is used). No (bottom, column) pair runs twice
           (border_seen). No checkpoint: borders are not re-derivable, so a run
           is continued simply by starting another. */
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
        g_completions_fp = open_completions(comp_path);
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
            if (g_exhaust) {
                if (!sample_exhaust_bottom(&srng, &bot))
                    fatal("random bottom sampling failed; seed edge pool too constrained");
            } else if (!sample_random_bottom(&srng, g_tau_bottoms, &bot))
                fatal("random bottom sampling failed; seed edge pool too constrained");
            validate_color_constants();
            g_border_stats.bottoms_ranked++;
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = 0; li < run_l && !g_stop; li++) {
                if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) { printf("[sweep] max_wall reached.\n"); g_stop = 1; break; }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                bool got_left = false, out_of_columns = false;
                double clue_t0 = omp_get_wtime();
                unsigned left_tries = (g_clue_mask & CLUE_CORNERS) ? 64u : 1u;
                unsigned dup = 0;
                uint64_t bkey = 0;
                for (unsigned tr = 0; tr < left_tries; ) {
                    if (!sample_random_left(&srng, g_tau_columns, &bot, &lft)) break;
                    bkey = border_key(&bot, &lft);
                    if (border_seen(bkey)) {
                        /* A border this run already searched: draw again. After
                           BORDER_REPEAT_TRIES repeats the exhaust colour's columns
                           are spent for this bottom, so its columns are drawn
                           plain from here; with nothing left to drop, the bottom
                           is left. */
                        g_border_stats.columns_repeated++;
                        if (++dup < BORDER_REPEAT_TRIES) continue;
                        if (g_exhaust_colour) {
                            g_exhaust_colour = 0;
                            g_border_stats.plain_fallbacks++;
                            dup = 0;
                            continue;
                        }
                        out_of_columns = true;
                        break;
                    }
                    tr++;
                    g_border_stats.columns_ordinary_viable++;
                    if (row1_corner_compatible(&bot, &lft)) {
                        g_border_stats.columns_clue_compatible++;
                        got_left = true;
                        break;
                    }
                }
                g_border_stats.clue_seconds += omp_get_wtime() - clue_t0;
                if (out_of_columns) { g_border_stats.bottoms_left++; break; }
                if (!got_left) {
                    g_border_stats.bottoms_no_clue_column++;
                    if (g_verbose)
                        fprintf(stderr, "[warn] no clue-compatible left column sampled "
                                "for random bottom %zu\n", bi);
                    break;
                }
                g_border_stats.columns_run++;
                border_seen_add(bkey);
                if (getenv("E555_BORDER_KEYS")) fprintf(stderr, "[bkey] %016" PRIx64 "\n", bkey);
                if (g_exhaust) g_exhaust_cfgs[g_exhaust_colour]++;
                g_cur_bottom = &bot;
                g_cur_left   = &lft;
                snprintf(g_config_id_str, sizeof g_config_id_str, "rndb%zul%zu", bi, li);
                set_board_id();
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                g_bt_run.cfg_found = g_bt_run.cfg_dup = g_bt_run.cfg_mincol = 0;
                g_bt_run.cfg_near_dup = g_bt_run.cfg_root_rep = g_bt_run.cfg_cut_score = 0;
                g_bt_run.cfg_ext_max = 0;
                if (g_incomplete_top) partial_config_reset();
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

                sweep_head();
                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                const uint64_t w0 = dv_written();
                if (g_end_dive) dive_config();
                cfg_count_written(w0);
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
    for (uint32_t cur_row = g_start_row; cur_row < g_start_row + g_num_rows; cur_row++) {
        if (g_stop) break;
        {
            const double el = omp_get_wtime() - t_start;
            printf("\n========== border row %u   run %.0f s = %.1f min ==========\n",
                   cur_row, el, el / 60.0);
            fflush(stdout);
        }

        uint8_t spins[NUM_PIECES];
        if (!read_one_border_row(csv_path, cur_row, spins)) { printf("[sweep] border row %u not found; stopping.\n", cur_row); break; }
        /* The row as the file has it, with its Stage A comment, so the log alone
           is enough to rebuild the rotations file. */
        {
            char *row_txt, *cmt_txt;
            if (read_border_row_text(csv_path, cur_row, &row_txt, &cmt_txt)) {
                printf("[border] %s row %u, as in the file:\n", csv_path, cur_row);
                if (cmt_txt) printf("%s\n", cmt_txt);
                printf("%s\n", row_txt);
                fflush(stdout);
            }
            reserve_row(cur_row, cmt_txt, spins);
            free(row_txt); free(cmt_txt);
        }
        memcpy(g_spin, spins, sizeof g_spin);

        classify_deal_from_rotations();
        build_top_border_demands();
        if (!g_free_edges) { build_edge_terminal_pool(); build_db_edge_and_sort(); }
        validate_color_constants();
        if (g_corners_on) {
            /* A clue frame whose corner has no legal block can never be closed;
               a border row with no such frame at all is skipped outright. */
            char lab[48]; snprintf(lab, sizeof lab, "border row %u", cur_row);
            if (!tc_build(g_clue_orients, (g_clue_mask & CLUE_CORNERS) != 0, lab)) {
                printf("[corner] border row %u: a top corner has no legal block in any "
                       "allowed clue frame -- no board from it can close, skipping the row\n",
                       cur_row);
                fflush(stdout);
                g_tc_skipped++;
                continue;
            }
        }

        enumerate_bottoms();
        enumerate_lefts();
        /* Separate streams per border row, so the bottoms drawn for one row do
           not depend on how many columns the previous row happened to enumerate. */
        RNG brng = rng_for(g_master_seed, cur_row, 0xB0770D15u, 0);
        rank_bottoms(g_tau_bottoms, &brng);

        size_t nb = g_bottom_n, nl = g_left_n;
        corner_compat_cache_init(nb);
        size_t run_b = (g_top_bottoms >= 1 && (size_t)g_top_bottoms < nb) ? (size_t)g_top_bottoms : nb;
        size_t cap_l = (g_top_columns >= 1 && (size_t)g_top_columns < nl) ? (size_t)g_top_columns : nl;
        printf("[sweep] bottoms=%zu (run %zu)  left-cols=%zu enumerated, up to %zu per bottom\n",
               nb, run_b, nl, cap_l); fflush(stdout);
        if (nb == 0 || nl == 0) fatal("no border configs for row %u", cur_row);

        htable_init();
        if (g_incomplete_top) partial_dedup_init();
        if (g_completions_fp) { dv_flush(g_completions_fp); fclose(g_completions_fp); }
        partial_outputs_close();
        char comp_path[1024];
        snprintf(comp_path, sizeof comp_path, "%s/beam_completions_%u_%u.csv", g_out_dir, cur_row, g_stop_row);
        g_completions_fp = open_completions(comp_path);
        printf("[out] completions -> %s (append)\n", comp_path);
        manifest_add(comp_path);
        if (g_incomplete_top) {
            char part_base[1024];
            snprintf(part_base, sizeof part_base,
                     "%s/beam_completions_%u_%u", g_out_dir, cur_row, g_stop_row);
            partial_outputs_open_base(part_base);
        }
        fflush(stdout);

        size_t start_bi = 0, start_li = 0;
        if (g_resume_active && cur_row == g_start_row) { start_bi = g_resume_bi; start_li = g_resume_li; g_solution_idx = g_resume_sol_idx; }
        else g_solution_idx = 0;

        for (size_t bi = start_bi; bi < run_b && !g_stop; bi++) {
            char blab[56];
            snprintf(blab, sizeof blab, "r%ub%zu", cur_row, bi);
            /* A column's rank is conditional on the bottom (left_rank_of), so the
               ranking belongs here, not once per border row. Its stream is keyed
               by bi so each bottom draws its own and --resume re-derives the same
               ordering for the bottom it re-enters. */
            RNG lrng = rng_for(g_master_seed, cur_row, 0x1EF7C015u, (uint32_t)bi);
            size_t distinct = 0;
            size_t ordinary_viable = rank_lefts(&g_bottoms[bi], g_tau_columns, &lrng, &distinct);
            double clue_t0 = omp_get_wtime();
            size_t viable = clue_filter_ranked_lefts(bi, &g_bottoms[bi], ordinary_viable);
            g_border_stats.clue_seconds += omp_get_wtime() - clue_t0;
            size_t run_l = viable < cap_l ? viable : cap_l;
            g_border_stats.columns_run += run_l;
            if (g_verbose) {
                printf("[rank] %s: columns %zu -> %zu ordinary -> %zu clue-compatible, "
                       "%zu distinct ordinary rank(s), run %zu\n",
                       blab, nl, ordinary_viable, viable, distinct, run_l);
                fflush(stdout);
            }
            if (run_l == 0) {                /* no column completes row 1 with it */
                write_checkpoint(ckpath, cur_row, (uint32_t)(bi+1), 0);
                continue;
            }
            uint32_t barren = 0;            /* consecutive columns that emitted nothing */
            for (size_t li = (bi == start_bi ? start_li : 0); li < run_l && !g_stop; li++) {
                if (g_max_wall_sec > 0.0 && omp_get_wtime() - t_start >= g_max_wall_sec) { printf("[sweep] max_wall reached.\n"); g_stop = 1; break; }
                if (partials_budget_spent()) { partials_budget_announce(); break; }
                g_cur_bottom = &g_bottoms[bi];
                g_cur_left   = &g_lefts[li];
                snprintf(g_config_id_str, sizeof g_config_id_str, "%sl%zu", blab, li);
                set_board_id();
                uint64_t cfg_hash = splitmix64(g_master_seed
                                    ^ (fnv1a_str(g_config_id_str) * 0x9E3779B97F4A7C15ULL));

                g_emit_count = 0; memset(g_emit_htable, 0, g_emit_htable_sz*sizeof(uint64_t));
                g_bt_run.cfg_found = g_bt_run.cfg_dup = g_bt_run.cfg_mincol = 0;
                g_bt_run.cfg_near_dup = g_bt_run.cfg_root_rep = g_bt_run.cfg_cut_score = 0;
                g_bt_run.cfg_ext_max = 0;
                if (g_incomplete_top) partial_config_reset();
                double tc0 = omp_get_wtime();
                double slice_end = tc0 + g_config_time_sec;
                if (g_max_wall_sec > 0.0) { double ge = t_start + g_max_wall_sec; if (ge < slice_end) slice_end = ge; }

                BeamResult br = beam_search_config(&ctx, scratch, cfg_hash, slice_end);
                const uint64_t w0 = dv_written();
                if (g_end_dive) dive_config();
                cfg_count_written(w0);
                sweep_report(blab, (long)li, &br, omp_get_wtime()-tc0);
                if (g_completions_fp) fflush(g_completions_fp);
                partial_outputs_flush();
                partials_budget_announce();
                /* A config cut short by --wall_time or a signal before it wrote
                   anything is not done: the checkpoint stays on it, so --resume
                   runs it again. One that did write is kept as done, so a resume
                   never appends its boards twice. */
                const bool cut = !strcmp(br.reason, "interrupted") ||
                                 (!strcmp(br.reason, "time") && g_max_wall_sec > 0.0 &&
                                  omp_get_wtime() - t_start >= g_max_wall_sec);
                size_t wrote = g_emit_count;
                for (int k = 0; k < PART_N; k++) wrote += g_part_count[k];
                const bool redo = cut && wrote == 0;
                write_checkpoint(ckpath, cur_row, (uint32_t)bi, (uint32_t)(redo ? li : li + 1));
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
                    write_checkpoint(ckpath, cur_row, (uint32_t)(bi+1), 0);
                    break;
                }
            }
            sweep_flush();      /* never straddle a bottom, or the next [rank] */
        }
        g_resume_active = false;
    }

    sweep_flush();
    if (g_completions_fp) dv_flush(g_completions_fp);
    double wall = omp_get_wtime() - t_start;
    print_summary(wall, init_s, omp_get_wtime() - t_sweep0);
    if (g_completions_fp) fclose(g_completions_fp);
    partial_outputs_close();
    /* After the closes: the manifest reports which files GREW, so their buffers
       have to have reached the disk before it stats them. */
    manifest_write(g_out_dir);
    return 0;
}
