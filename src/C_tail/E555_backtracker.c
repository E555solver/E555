/*
 * E555_backtracker.c -- Stage C exact / bounded-mismatch DFS tail solver.
 *
 * Parallel backtracking tail solver for Eternity II partial boards.  It reads a
 * CSV of partial 16x16 boards (Stage-B beam output, or this program's own
 * output) and fills the remaining empty cells of each by a constrained DFS,
 * writing the best board found for every processed record back out as a
 * re-feedable CSV line.
 *
 * ALGORITHM
 *   - DFS fills one cell at a time.  Cell order is selectable (--order); the
 *     default mrv chooses the most-constrained cell for fail-first search.  The
 *     static orders (rowmajor/spiral/spiralout/...) remain selectable, and
 *     --reverse flips any of them; spiralout places the top-left corner last.
 *     A piece fits a cell when its oriented edges match
 *     the already-placed neighbours and the gray-0 frame rule (color 0 only on
 *     the outer border, facing out) holds.
 *   - Forward-checking engine: a static (side,color) orientation-bitset
 *     index plus per-cell exact-fit domains (FcState) make MRV counts and
 *     candidate enumeration a few word ops per cell instead of a 256x4 rescan,
 *     and give an immediate cutoff when a neighbour's domain empties.
 *   - Classic mode (--jump off, except 2sides/4sides) applies three sound
 *     completion prunes: a global empty-domain lower bound, incremental
 *     color/type accounting, and a Hall/deficiency bipartite-matching bound.
 *     Hall is always checked at the root and, by default, adaptively below it;
 *     --hall controls the policy.  With --jump on, or with an exact side-growth
 *     order, an impossible cell is deferred so independent gaps can keep
 *     growing and completion-only prunes are disabled.
 *   - --max-mismatch K allows up to K broken internal edges in the finished
 *     board (default 0 = exact).  K is an ABSOLUTE ceiling: breaks already in
 *     the input count against it, and an input that already exceeds K is
 *     dropped.  Iterative deepening over the budget finds the fewest breaks
 *     when each level is exhaustive (break-mode any, or uncapped lds); stuck
 *     is a faster heuristic.  With --all-solutions, retained ties prefer fewer
 *     distinct pieces touched.  --break-mode selects where breaks may be
 *     spent: stuck (dead-end cells only), any (everywhere; complete but
 *     exponential), or lds (discrepancy-limited widening from stuck toward
 *     any, --lds-max caps the allowance).
 *
 * OUTPUT
 *   - The mandatory output.csv gets ONE line per processed record: the best
 *     board reached for that record (a completion when found, else the deepest
 *     partial), streamed at end-of-record, thread-safe.  Layout is the
 *     canonical E555 board row config_id,score,pos[256],rot[256] (514 fields;
 *     score = matched internal edges) so the file can be fed straight back in
 *     as input for an improvement loop.
 *   - <output>.checkpoint.csv: append-only crash-recovery file in the same
 *     re-feedable layout; a record's best board is appended when it improves
 *     (>=10 s throttle per record).  Removed on clean completion.
 *   - ~30 s heartbeat progress lines per in-flight record (k, D, nodes, best).
 *   - --status optionally writes <output>.status.csv with one diagnostic row
 *     per parsed input record.  --best-output N additionally writes global
 *     top-N pure and mismatch partial-board CSVs.
 *
 * PARALLELISM (OpenMP)
 *   - Automatic, resolved once the input window is known: with no more records
 *     than threads, every worker attacks one record's search together; otherwise
 *     each worker takes its own record.  --all-for-one forces the former.
 *   - stuck mode ignores all of that: --stuck_restarts dives share nothing, so
 *     they are simply split across threads.
 *   - There is no knob for the frontier split.  It was measured and it does not
 *     matter: see the note on SPLIT_TASKS_PER_THREAD, where an independent-process
 *     control shows this search is limited by the memory system rather than by
 *     scheduling.
 *
 * COMPILE
 *   gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *       E555_backtracker.c -o E555_backtracker -lm
 *   (debug self-checks: -DVERIFY_AVAIL for the availability counters,
 *    -DVERIFY_INDEX for the bitset-index candidate enumeration.)
 *
 * RUN
 *   ./E555_backtracker seed.txt completions.csv output.csv [options]
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#define E555_BUILD_TAG "backtracker-20260719"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <omp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -- Board geometry ----------------------------------------------------------- */

#define PUZZLE_SIDE  16
#define NUM_PIECES   (PUZZLE_SIDE * PUZZLE_SIDE)
#define USED_WORDS   ((NUM_PIECES + 63) / 64)
#define MAX_SEQ_LEN  NUM_PIECES

/* -- Other constants ---------------------------------------------------------- */

#define MAX_COLOR     22
#define NUM_COLORS    (MAX_COLOR + 1)
#define EMPTY_PIECE   ((int16_t)-1)
#define CSV_UNPLACED  999
#define CSV_BUF_BYTES 32768
#define REC_BUF_BYTES 262144   /* verbose boards + top-N at end */
#define SPLIT_DEPTH 4           /* max prefix decisions before search-parallel workers start */
/*
 * Frontier tasks per worker.  Sized automatically from the thread count; there is
 * deliberately no CLI knob for it, because it was measured and it does not matter.
 *
 * MEASUREMENT (2026-07-21, 4 physical / 8 logical cores, 48-cell tail,
 * --break-mode lds --lds-max 0 --max-mismatch 12, 3 reps):
 *   tasks/thread    16      64     128     512    2048
 *   wall @4 thr   5.88s   5.78s  5.76s   5.96s  11.19s
 *   wall @8 thr   4.92s   5.05s  4.99s   5.57s  23.95s
 * Overdecomposition buys nothing here, and large values are actively harmful
 * (the serial frontier build grows).  The reason is that load imbalance is not
 * the bottleneck: four INDEPENDENT single-threaded processes on this machine take
 * 23.7s each versus 14.4s for one alone, i.e. the hardware itself yields only
 * 2.44x aggregate throughput on 4 cores.  The threaded search already achieves
 * 2.37x, so essentially all available parallelism is being extracted and the
 * remaining gap is memory system contention, not scheduling.
 *
 * Do not "optimize" this without re-running that independent-process control:
 * on a machine with more memory bandwidth per core the picture may differ, but
 * on a laptop the scheduler is not what is limiting you.
 */
#define SPLIT_TASKS_PER_THREAD 16
#define CONFIG_ID_LEN 64       /* max length of config-id string */

/* -- Inline RNG (xoroshiro128++ + splitmix64) ---------------------------------- *
 * Mirrors the Stage B generator in E555_database.h so the two halves of the
 * pipeline randomize identically.  The backtracker is standalone and does not
 * include that header, so the three primitives are repeated here.
 *
 * Streams are derived, never shared: each greedy dive builds its own RNG from
 * (master seed, record index, restart index) via splitmix64, so parallel dives
 * need no locking and no per-thread generator state. */

typedef struct { uint64_t s[2]; } RNG;

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline uint64_t rng_next(RNG *r) {
    uint64_t s0 = r->s[0], s1 = r->s[1];
    uint64_t result = rotl64(s0 + s1, 17) + s0;
    s1 ^= s0;
    r->s[0] = rotl64(s0, 49) ^ s1 ^ (s1 << 21);
    r->s[1] = rotl64(s1, 28);
    return result;
}

/* Unbiased uniform draw in [0, n) (Lemire's method). */
static inline uint32_t rng_uniform(RNG *r, uint32_t n) {
    if (n <= 1) return 0;
    uint64_t m = (uint64_t)(uint32_t)rng_next(r) * (uint64_t)n;
    uint32_t leftover = (uint32_t)m;
    if (leftover < n) {
        uint32_t threshold = (uint32_t)(0u - n) % n;
        while (leftover < threshold) {
            m = (uint64_t)(uint32_t)rng_next(r) * (uint64_t)n;
            leftover = (uint32_t)m;
        }
    }
    return (uint32_t)(m >> 32);
}

static inline RNG rng_for(uint64_t master, uint64_t a, uint64_t b) {
    uint64_t h = splitmix64(master
        ^ (a * 0x9E3779B97F4A7C15ULL)
        ^ (b * 0xBF58476D1CE4E5B9ULL));
    RNG r; r.s[0] = h; r.s[1] = splitmix64(h);
    return r;
}

/* -- Seed data ---------------------------------------------------------------- */

static int g_seed_top[NUM_PIECES];
static int g_seed_right[NUM_PIECES];
static int g_seed_bottom[NUM_PIECES];
static int g_seed_left[NUM_PIECES];
static int g_piece_zero_count[NUM_PIECES]; /* 0=inner, 1=edge, 2=corner */
static uint8_t g_piece_spin_mask[NUM_PIECES]; /* one bit per distinct orientation */

/* Pre-classified by frame-degree: g_pieces_by_zero[d][i] = pid for degree d */
static int g_pieces_by_zero[3][NUM_PIECES];
static int g_pieces_by_zero_n[3];

/* -- Holes -------------------------------------------------------------------- */

static int  g_holes[NUM_PIECES];
static bool g_holes_active = false;

/* -- Global configuration ----------------------------------------------------- */

static int         g_nthreads           = 0;
static long long   g_row_start          = -1;   /* -1 = no --row (start at record 0) */
static long long   g_count              = -1;   /* -1 = unset; resolved in main() */
static bool        g_verbose            = false;
static int         g_best_output        = 0;    /* 0 = off: no global top-N partial tracks */

typedef enum { PAR_AUTO = 0, PAR_RECORDS, PAR_SEARCH } ParallelMode;
static ParallelMode g_parallel_mode     = PAR_AUTO;
static bool        g_search_parallel    = false; /* resolved after the CSV window is known */
static bool        g_dedup              = true;  /* default: exact post-holes duplicate skipping */
static bool        g_write_status       = false; /* --status: write output.csv.status.csv */

static const char *parallel_mode_name(ParallelMode m) {
    switch (m) {
        case PAR_AUTO:    return "auto";
        case PAR_RECORDS: return "records";
        case PAR_SEARCH:  return "search";
        default:          return "?";
    }
}
/* Jumping policy.  false (default): classic backtracker - a cell with no legal
 * candidate ends the branch (backtrack).  true (--jump): the impossible cell is
 * skipped and independent gaps elsewhere keep filling (best-partial mode).
 * 2sides/4sides use a dedicated exact side-growth DFS while still
 * requiring the user-facing --jump option to remain off. */
static bool        g_jump              = false;
static int         g_total_records      = 0;
static double      g_t_start_wall       = 0.0;
static int         g_rotation           = 0;    /* --rotate K */
static uint64_t    g_solution_limit     = 1;    /* per record; 0 = enumerate all */

/* -- Mismatch budget ---------------------------------------------------------- */
/*
 * g_max_mismatch is the ABSOLUTE ceiling on broken internal edges in a completed
 * board (default 0 = exact).  A "broken edge" is an adjacency between two placed
 * pieces whose touching colors differ.  Border (gray-0) edges are structural and
 * always enforced exactly (never counted as a mismatch).
 *
 * The INPUT partial may itself carry broken edges (e.g. when re-reading this
 * program's own output).  Those input breaks count against the ceiling: an input
 * that already has more than g_max_mismatch breaks is dropped without searching.
 * Iterative deepening runs k = input_breaks .. g_max_mismatch.  With an
 * exhaustive break policy and no cutoff, the first completed level has the
 * minimum number of breaks.  Exhaustive ranking within that level (including
 * the clustering tie-break) additionally requires --all-solutions.
 */
static int      g_max_mismatch     = 0;
/* Per-record wall-clock budget for the mismatch search (seconds).  -1 -> unset:
 * defaults to TIME_LIMIT_DEFAULT when --max-mismatch>0, else unlimited.  An
 * explicit 0 means unlimited.  When the budget elapses the search stops and the
 * best board found so far (complete if any, else deepest partial) is reported.
 * Proving a board cannot be completed within K breaks can be exponential, so a
 * budget keeps the run bounded; K=0 (strict) ignores this and is unchanged. */
static double   g_time_limit_sec   = -1.0;
#define TIME_LIMIT_DEFAULT 30.0

/* Break placement policy.  The three modes answer different questions, and the
 * split between them is the whole design: one is a triage filter, two are
 * proof engines.
 *
 *   stuck (default): GREEDY DESCENT, no backtracking - see greedy_dive().
 *     Takes an exact fit where one exists, spends a minimal break where none
 *     does, never reconsiders, and therefore always reaches 256 pieces in one
 *     pass over the empty cells.  Cost is O(cells) per dive, so --stuck_restarts
 *     runs many randomized dives and keeps the best.  This is a fast filter for
 *     ranking candidate partials, NOT a proof: it never establishes that a
 *     board cannot be completed with fewer breaks.
 *
 *   any: breaks are tried at every cell (in addition to exact fits) - complete
 *     within the budget but exponentially wide, so it must be paired with
 *     --time-limit.
 *
 *   lds (E555): limited discrepancy on VOLUNTARY breaks.  At a cell with no
 *     exact fit, all break classes within the budget are tried.  Taking a break
 *     candidate where an exact fit also exists costs one discrepancy; a search
 *     pass with allowance D permits at most D such choices per path.  D iterates
 *     0,1,2,... inside each budget level's time window (capped by --lds-max),
 *     and each D+1 widens toward any-mode completeness.
 *
 * any and lds keep the iterative-deepening ladder (k = input_breaks .. budget),
 * so an exhausted level is a theorem: no completion exists with <= k broken
 * edges.  Use them for overnight runs.  stuck never enters that loop.
 *
 * Exact search (--max-mismatch 0) is independent of all three: with a zero
 * budget collect_candidates() never gathers break placements, so the mode
 * cannot influence it. */
typedef enum { BREAK_STUCK = 0, BREAK_ANY, BREAK_LDS } BreakMode;
static BreakMode g_break_mode = BREAK_STUCK;
static int       g_lds_max    = -1;   /* -1 = no cap beyond the break budget */

/* Greedy-dive controls (stuck mode only). */
/* Dives run at roughly 9k-18k/s on four cores depending on region size, so the
 * default is about 5-10 s of work per record.  Raise it freely; the cost is
 * linear and the boards keep coming out distinct. */
static long long g_stuck_restarts = 100000; /* independent randomized dives per record */
static uint64_t  g_rng_master     = 0;      /* seeded from the clock + pid in main() */

static const char *break_mode_name(BreakMode m) {
    switch (m) {
        case BREAK_STUCK: return "stuck";
        case BREAK_ANY:   return "any";
        case BREAK_LDS:   return "lds";
        default:          return "?";
    }
}

/* Hall is exact but relatively expensive.  E555 always computes the root
 * deficiency unless disabled; below the root the default adaptive policy runs
 * it periodically and near the leaves instead of at almost every child. */
typedef enum { HALL_OFF = 0, HALL_ROOT, HALL_ADAPTIVE, HALL_ALWAYS } HallMode;
static HallMode g_hall_mode = HALL_ADAPTIVE;
static int      g_hall_stride = 8;       /* adaptive: every N placed tail cells */
static int      g_hall_small  = 32;      /* adaptive: always when <= this many cells remain */
#define HALL_TIGHT_DOMAIN 4
#define HALL_TIGHT_CELLS  4

static const char *hall_mode_name(HallMode m) {
    switch (m) {
        case HALL_OFF:      return "off";
        case HALL_ROOT:     return "root";
        case HALL_ADAPTIVE: return "adaptive";
        case HALL_ALWAYS:   return "always";
        default:            return "?";
    }
}

/* -- v6: traversal order ------------------------------------------------------ */
typedef enum {
    ORD_ROWMAJOR = 0,  /* (r asc, c asc) - identical to v5 default */
    ORD_COLMAJOR,      /* (c asc, r asc) */
    ORD_SNAKE,         /* boustrophedon by row */
    ORD_SPIRAL,        /* outside-in (border rings first) */
    ORD_CENTEROUT,     /* inside-out (center first) */
    ORD_SPIRALOUT,     /* inside-out ring path ending at the top-left corner */
    ORD_MRV,           /* dynamic: most-constrained cell; row-major tie break,
                        * or column-major when --reverse is given */
    ORD_2SIDES,        /* exact side-growth: left/right layers, one cell per row */
    ORD_4SIDES         /* exact side-growth: left/right/bottom/top layers */
} OrderMode;
static OrderMode g_order_mode = ORD_MRV;
static bool g_reverse = false;   /* --reverse: reversed traversal; mrv tie-break -> column-major */

/* -- Stop band (--stop_row / --stop_column) ----------------------------------- *
 * The beamer stops at --stop_row R and emits every board that fills rows 0..R,
 * which the finalizer then resumes from with --finalize_from R.  These options
 * give the backtracker the same shape: restrict the search to a band of rows or
 * columns and emit every distinct way to fill it.
 *
 * The band is the CLOSED range 0..g_stop_n, matching the beamer (--stop_row 12
 * there means rows 0..12, thirteen rows).  --reverse anchors it at the far side
 * instead -- rows 15-N..15, columns 15-N..15 -- so with a stop option on,
 * --reverse both flips static traversal (its older meaning) and picks the side.
 *
 * Membership is decided in the ORIGINAL frame, the one the CSV is written in,
 * so "row 12" means row 12 as the user reads it.  Under --rotate the DFS works
 * in a rotated frame, so callers there must map back first (rotated_to_orig). */
static int  g_stop_n      = -1;      /* band size - 1; -1 = no stop option */
static bool g_stop_isrow  = true;    /* true: --stop_row, false: --stop_column */
static bool g_stop_active = false;

static inline bool cell_in_band(int r, int c) {
    if (!g_stop_active) return true;
    int v = g_stop_isrow ? r : c;
    return g_reverse ? (v >= PUZZLE_SIDE - 1 - g_stop_n) : (v <= g_stop_n);
}

/* Band membership for a cell of the SEARCH board, which under --rotate K sits
 * in the rotated frame.  apply_rotation_k turns (r,c) into (c, SIDE-1-r) once
 * per quarter turn, so undoing K turns means applying that same step (4-K)&3
 * times -- the coordinate half of inverse_rotation_k, which cannot be reused
 * here because it moves whole boards, not single cells. */
static inline bool cell_in_band_search(int r, int c) {
    if (!g_stop_active) return true;
    if (g_rotation > 0) {
        for (int q = (4 - g_rotation) & 3; q > 0; q--) {
            int tr = c, tc = PUZZLE_SIDE - 1 - r;
            r = tr; c = tc;
        }
    }
    return cell_in_band(r, c);
}

static const char *order_name(OrderMode m) {
    switch (m) {
        case ORD_ROWMAJOR:  return "rowmajor";
        case ORD_COLMAJOR:  return "colmajor";
        case ORD_SNAKE:     return "snake";
        case ORD_SPIRAL:    return "spiral";
        case ORD_CENTEROUT: return "centerout";
        case ORD_SPIRALOUT:  return "spiralout";
        case ORD_MRV:        return "mrv";
        case ORD_2SIDES:     return "2sides";
        case ORD_4SIDES:    return "4sides";
        default:            return "?";
    }
}

static inline bool order_is_side_growth(OrderMode m) {
    return m == ORD_2SIDES || m == ORD_4SIDES;
}

/* Side-growth modes are deliberately best-partial searches even though the
 * user-facing --jump switch remains off: they place exact pieces only, defer a
 * side cell that has no exact candidate, and do not let a completion-only
 * proof elsewhere on the board terminate lateral growth. */
static inline bool order_uses_soft_completion(OrderMode m) {
    return g_jump || order_is_side_growth(m);
}

static const char *effective_hall_name(void) {
    if (g_jump) return "off_jump";
    if (order_is_side_growth(g_order_mode)) return "off_side_growth";
    return hall_mode_name(g_hall_mode);
}

/* -- Global counters ---------------------------------------------------------- */

static uint64_t g_total_solutions    = 0;
static uint64_t g_records_processed  = 0;
static uint64_t g_cnt_invalid        = 0;
static uint64_t g_cnt_dropped        = 0;   /* input_breaks already exceed --max-mismatch */
static uint64_t g_cnt_duplicate      = 0;   /* post-holes board repeats an earlier row */
static uint64_t g_cnt_infeasible     = 0;
static uint64_t g_cnt_hall_infeasible= 0;
static uint64_t g_cnt_solution       = 0;
static uint64_t g_cnt_solution_cutoff= 0;
static uint64_t g_cnt_no_solution    = 0;
static uint64_t g_cnt_cutoff         = 0;
static uint64_t g_feas_code_cnt[5]   = {0};

/* -- Best-partial-board tracking ---------------------------------------------- */

typedef struct {
    /* piece/board state at deepest point (rotated frame for display) */
    struct {
        struct {
            int16_t piece_id;
            uint8_t rotation;
            uint8_t top, right, bottom, left;
        } cell[PUZZLE_SIDE][PUZZLE_SIDE];
        uint64_t used[USED_WORDS];
    } board;
    /* initial board for this record (after holes, rotated frame) */
    struct {
        struct {
            int16_t piece_id;
            uint8_t rotation;
            uint8_t top, right, bottom, left;
        } cell[PUZZLE_SIDE][PUZZLE_SIDE];
        uint64_t used[USED_WORDS];
    } initial_board;
    int       n_total;           /* total pieces on best board */
    int       initial_n_placed;  /* pieces before DFS */
    int       n_added;           /* n_total - initial_n_placed */
    int       n_connected;       /* matched internal edges on this board */
    int       n_broken;          /* broken internal edges on this board */
    int       n_break_pieces;    /* distinct pieces incident to >=1 broken edge */
    int       deepest_row;       /* (row,col) of last DFS-placed piece */
    int       deepest_col;
    char      record_id[CONFIG_ID_LEN];
    long long sol_id_num;
} BestPartial;

/*
 * Two separate top-N tracks: PURE (boards with 0 broken edges) and MISMATCH
 * (boards with >=1 broken edge).  Both rank by pieces first, then by connected
 * edges, then (mismatch) by fewer broken edges.  A pure input fills only the
 * pure track; a break-carrying input fills only the mismatch track.
 */
static BestPartial *g_best_pure       = NULL;
static int          g_best_pure_count = 0;
static BestPartial *g_best_mm         = NULL;
static int          g_best_mm_count   = 0;
/* Race-free fast-path floors for the global top-N tracks.  A value of 0 means
 * the track is not full yet; once full, it is the weakest retained n_total. */
static atomic_int   g_best_pure_floor;
static atomic_int   g_best_mm_floor;
static omp_lock_t   g_best_lock;

/* Per-record running best (pieces, then connected edges).  best_board holds the
 * full board for that best, so it can be streamed to output.csv at end-of-record.
 * Protected by g_best_lock when updated from parallel workers. */
struct Board; /* forward: defined below */
typedef struct {
    atomic_int best_total; /* max pieces reached on this record; worker-read */
    int best_connected;  /* matched internal edges at that board */
    int best_broken;     /* broken internal edges at that board */
    int best_break_pieces;/* pieces touched by broken edges at that board */
    int deepest_row;     /* (row,col) of the last piece placed on that board */
    int deepest_col;
    struct Board *best_board; /* full board copy of the best (rotated frame) */
    /* E555 crash-recovery checkpoint state (protected by g_best_lock): next
     * wall-clock time a checkpoint line may be appended for this record, and
     * whether an improvement is still waiting to be flushed. */
    double ckpt_next;
    bool   ckpt_dirty;
} RecStat;
#define CKPT_MIN_INTERVAL_SEC 10.0

/* -- Output files ------------------------------------------------------------- */

static FILE *g_stream_csv = NULL;   /* mandatory output.csv: best line per record */
static FILE *g_status_csv = NULL;   /* optional --status per-record diagnostics */
static FILE *g_ckpt_csv   = NULL;   /* E555: append-only crash-recovery checkpoint;
                                     * last line per record = its best so far.
                                     * Removed on clean completion. */
static FILE *g_band_csv   = NULL;   /* --stop_row/--stop_column: one line per band
                                     * filling.  Unlike output.csv, which holds a
                                     * single best board per record, this streams
                                     * every emission as the search finds it. */
static uint64_t g_band_emitted  = 0; /* bands written */
static uint64_t g_band_rejected = 0; /* bands dropped for carrying a broken edge */


/* -- Utilities ---------------------------------------------------------------- */

static void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "Fatal: "); vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n"); va_end(ap);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p && n) fatal("malloc(%zu) failed", n);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) fatal("realloc(%zu) failed", n);
    return q;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) fatal("strdup failed");
    return p;
}

static double elapsed_wall(void) { return omp_get_wtime() - g_t_start_wall; }

static void appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap) fatal("output buffer overflow (increase REC_BUF_BYTES)");
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off)
        fatal("output buffer too small (increase REC_BUF_BYTES)");
    *off += (size_t)n;
}

static void checked_fwrite(const void *ptr, size_t sz, size_t cnt, FILE *f, const char *path) {
    if (!sz || !cnt) return;
    if (fwrite(ptr, sz, cnt, f) != cnt)
        fatal("write failed for %s: %s", path, strerror(errno ? errno : EIO));
}

static void checked_fflush(FILE *f, const char *path) {
    if (fflush(f) != 0) fatal("fflush failed for %s: %s", path, strerror(errno));
}

static void checked_fclose(FILE *f, const char *path) {
    if (!f) return;
    if (fflush(f) != 0) { int e = errno; fclose(f); fatal("fflush failed for %s: %s", path, strerror(e)); }
    if (fclose(f) != 0) fatal("fclose failed for %s: %s", path, strerror(errno));
}

/* -- Piece / board structures ------------------------------------------------- */

typedef struct {
    int16_t piece_id;
    uint8_t rotation;
    uint8_t top, right, bottom, left;
} Oriented;

/* Immutable orientation cache.  v9 reconstructed these six bytes in every
 * break-candidate scan and placement; E555 builds the 1024 entries once. */
static Oriented g_oriented[NUM_PIECES][4];

typedef struct Board {
    Oriented cell[PUZZLE_SIDE][PUZZLE_SIDE];
    uint64_t used[USED_WORDS];
    /* Incremental piece-availability, maintained by board_place/board_unplace so
     * the remaining-subproblem feasibility prune is O(NUM_COLORS) instead of a
     * full 256-piece rescan.  avail_color[k] = occurrences of color k across the
     * four sides of all still-UNUSED pieces; avail_type[z] = count of unused
     * pieces with frame-degree z (0=inner,1=edge,2=corner).  These trail `used`
     * so copying only the cell+used prefix (BestPartial) stays correct. */
    int16_t  avail_color[NUM_COLORS];
    int16_t  avail_type[3];
} Board;

typedef struct { int8_t row, col; } Cell;
typedef struct {
    uint16_t pid;
    uint8_t  spin;
    uint8_t  breaks;
} Candidate;

/* Shared live state for one record search: the cancellation flag (C11 atomics
 * so time-limit cancellation is race-free when search-parallel workers are
 * active), an approximate node counter (each thread adds its batch when its
 * local counter wraps), and the next-heartbeat timestamp for the periodic
 * progress line (CAS-claimed so exactly one thread prints per interval). */
enum { STOP_NONE = 0, STOP_SOLUTION_LIMIT = 1, STOP_TIMEOUT = 2 };
typedef struct {
    atomic_int    requested;   /* bitmask of STOP_* reasons */
    atomic_llong  hb_next_ms;  /* next heartbeat, ms since g_t_start_wall */
    atomic_ullong nodes;       /* approximate nodes visited on this record */
    atomic_ullong solutions;   /* complete leaves found on this record */
} SearchLive;
#define HEARTBEAT_SEC 30.0
static inline void search_stop_init(SearchLive *s) {
    atomic_store_explicit(&s->requested, STOP_NONE, memory_order_relaxed);
}
static inline void search_live_init(SearchLive *s) {
    search_stop_init(s);
    atomic_store_explicit(&s->nodes, 0, memory_order_relaxed);
    atomic_store_explicit(&s->solutions, 0, memory_order_relaxed);
    long long first = (long long)((elapsed_wall() + HEARTBEAT_SEC) * 1000.0);
    atomic_store_explicit(&s->hb_next_ms, first, memory_order_relaxed);
}
static inline int search_stop_reason(const SearchLive *s) {
    return atomic_load_explicit(&s->requested, memory_order_relaxed);
}
static inline bool search_stop_requested(const SearchLive *s) {
    return search_stop_reason(s) != STOP_NONE;
}
static inline void search_request_stop(SearchLive *s, int reason) {
    atomic_fetch_or_explicit(&s->requested, reason, memory_order_relaxed);
}
static inline bool claim_complete_solution(SearchLive *s) {
    if (g_solution_limit == 0) {
        atomic_fetch_add_explicit(&s->solutions, 1, memory_order_relaxed);
        return true;
    }
    unsigned long long old =
        atomic_load_explicit(&s->solutions, memory_order_relaxed);
    for (;;) {
        if (old >= g_solution_limit) {
            search_request_stop(s, STOP_SOLUTION_LIMIT);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&s->solutions, &old, old + 1ULL,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            if (old + 1ULL >= g_solution_limit)
                search_request_stop(s, STOP_SOLUTION_LIMIT);
            return true;
        }
    }
}

/* E555: read-only per-search context.  Bundles the invariants that v8 threaded
 * through tail_dfs()'s parameter list, one instance per (record, budget level,
 * lds allowance).  rstat and live point at shared per-record state (lock- and
 * atomic-protected respectively); everything else is immutable during the
 * search, so the struct is safe to share across search-parallel workers. */
typedef struct {
    const char  *config_id;
    long long    sol_id;
    int          initial_n_placed;
    int          initial_connected;
    const Board *initial_board;
    RecStat     *rstat;
    SearchLive  *live;
    OrderMode    mode;
    int          k_budget;       /* absolute break ceiling at this level */
    int          input_breaks;   /* breaks already in the input partial */
    int          lds_D;          /* voluntary-break allowance (BREAK_LDS) */
    double       record_start;   /* absolute omp_get_wtime at record entry */
    double       deadline;       /* wall-clock deadline; 0 = none */
    bool         collect;        /* gather complete boards in MismatchResult */
} SearchCtx;
static inline uint64_t piece_bit(int pid) { return 1ULL << (pid & 63); }
static inline bool used_test(const uint64_t u[], int pid) { return (u[pid>>6] & piece_bit(pid)) != 0; }
static inline void used_set  (uint64_t u[], int pid) { u[pid>>6] |=  piece_bit(pid); }
static inline void used_clear(uint64_t u[], int pid) { u[pid>>6] &= ~piece_bit(pid); }

/* CCW quarter-turn rotation: top=e[s], right=e[s+1], bottom=e[s+2], left=e[s+3] */
static Oriented make_oriented_uncached(int piece_id, int spin) {
    const int e[4] = { g_seed_top[piece_id], g_seed_right[piece_id],
                       g_seed_bottom[piece_id], g_seed_left[piece_id] };
    Oriented o;
    o.piece_id = (int16_t)piece_id;
    o.rotation = (uint8_t)spin;
    o.top    = (uint8_t)e[(0 + spin) & 3];
    o.right  = (uint8_t)e[(1 + spin) & 3];
    o.bottom = (uint8_t)e[(2 + spin) & 3];
    o.left   = (uint8_t)e[(3 + spin) & 3];
    return o;
}

static void build_oriented_cache(void) {
    int distinct_total = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        uint8_t mask = 0;
        for (int spin = 0; spin < 4; spin++) {
            g_oriented[pid][spin] = make_oriented_uncached(pid, spin);
            bool duplicate = false;
            for (int prev = 0; prev < spin; prev++) {
                const Oriented *a = &g_oriented[pid][spin];
                const Oriented *b = &g_oriented[pid][prev];
                if (a->top == b->top && a->right == b->right &&
                    a->bottom == b->bottom && a->left == b->left) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                mask |= (uint8_t)(1u << spin);
                distinct_total++;
            }
        }
        g_piece_spin_mask[pid] = mask;
    }
    printf("[seed] distinct oriented pieces=%d/%d (rotational duplicates removed)\n",
           distinct_total, NUM_PIECES * 4);
}

static inline Oriented make_oriented(int piece_id, int spin) {
    return g_oriented[piece_id][spin & 3];
}

static void board_init(Board *b) {
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            b->cell[r][c].piece_id = EMPTY_PIECE;
    memset(b->used, 0, sizeof(b->used));
    /* All pieces start unused: seed the availability counters with the full
     * totals (cheap, runs once per board construction, never in the DFS). */
    memset(b->avail_color, 0, sizeof(b->avail_color));
    b->avail_type[0] = b->avail_type[1] = b->avail_type[2] = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        b->avail_color[g_seed_top[pid]]++;
        b->avail_color[g_seed_right[pid]]++;
        b->avail_color[g_seed_bottom[pid]]++;
        b->avail_color[g_seed_left[pid]]++;
        int z = g_piece_zero_count[pid];
        if (z >= 0 && z <= 2) b->avail_type[z]++;
    }
}

static inline void board_place(Board *b, int r, int c, int pid, int spin) {
    b->cell[r][c] = g_oriented[pid][spin & 3];
    used_set(b->used, pid);
    b->avail_color[g_seed_top[pid]]--;
    b->avail_color[g_seed_right[pid]]--;
    b->avail_color[g_seed_bottom[pid]]--;
    b->avail_color[g_seed_left[pid]]--;
    int z = g_piece_zero_count[pid];
    if (z >= 0 && z <= 2) b->avail_type[z]--;
}

static inline void board_unplace(Board *b, int r, int c) {
    int pid = (int)b->cell[r][c].piece_id;
    if (pid >= 0) {
        used_clear(b->used, pid);
        b->avail_color[g_seed_top[pid]]++;
        b->avail_color[g_seed_right[pid]]++;
        b->avail_color[g_seed_bottom[pid]]++;
        b->avail_color[g_seed_left[pid]]++;
        int z = g_piece_zero_count[pid];
        if (z >= 0 && z <= 2) b->avail_type[z]++;
    }
    b->cell[r][c].piece_id = EMPTY_PIECE;
}

/* O(1) count via popcnt */
static int board_count_placed(const Board *b) {
    int n = 0;
    for (int w = 0; w < USED_WORDS; w++)
        n += __builtin_popcountll(b->used[w]);
    return n;
}

/* Empty cells the search is actually allowed to fill.  Without a stop band that
 * is every empty cell, so this reduces to NUM_PIECES - board_count_placed(). */
static int board_count_searchable(const Board *b) {
    if (!g_stop_active) return NUM_PIECES - board_count_placed(b);
    int n = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (b->cell[r][c].piece_id == EMPTY_PIECE && cell_in_band_search(r, c))
                n++;
    return n;
}

/* Every band cell placed?  The DFS leaf tests this instead of "all 256 placed",
 * which a band search never reaches. */
static bool band_is_complete(const Board *b) {
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (cell_in_band_search(r, c) && b->cell[r][c].piece_id == EMPTY_PIECE)
                return false;
    return true;
}

static int cell_frame_degree(int row, int col) {
    return (row == 0) + (row == PUZZLE_SIDE-1) + (col == 0) + (col == PUZZLE_SIDE-1);
}

/* -- Board rotation ----------------------------------------------------------- */

/*
 * One CCW 90 deg rotation maps (r,c) -> (c, N-1-r) and increments piece spin by 1.
 * apply_rotation_k applies this k times.
 * Positions and spins of all placed pieces are remapped.
 */
static void apply_rotation_k(Board *dst, const Board *src, int k) {
    board_init(dst);
    if (k == 0) { *dst = *src; return; }
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &src->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) continue;
            int nr = r, nc = c;
            for (int q = 0; q < k; q++) {
                int tr = nc, tc = PUZZLE_SIDE - 1 - nr;
                nr = tr; nc = tc;
            }
            int new_spin = ((int)o->rotation + k) & 3;
            board_place(dst, nr, nc, (int)o->piece_id, new_spin);
        }
    }
}

/* Inverse rotation: (k CCW) -> (4-k CCW) */
static void inverse_rotation_k(Board *dst, const Board *src, int k) {
    apply_rotation_k(dst, src, (4 - k) & 3);
}

/* -- Seed loading ------------------------------------------------------------- */

static void load_seed(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open seed '%s': %s", path, strerror(errno));

    int n_inner = 0, n_edge = 0, n_corner = 0, n_bad = 0;
    int color_count[NUM_COLORS] = {0};
    memset(g_pieces_by_zero_n, 0, sizeof(g_pieces_by_zero_n));

    for (int i = 0; i < NUM_PIECES; i++) {
        if (fscanf(f, "%d %d %d %d",
                   &g_seed_top[i], &g_seed_right[i],
                   &g_seed_bottom[i], &g_seed_left[i]) != 4)
            fatal("seed: need %d rows of 4 integers; failed at row %d", NUM_PIECES, i);
        int e[4] = { g_seed_top[i], g_seed_right[i], g_seed_bottom[i], g_seed_left[i] };
        for (int k = 0; k < 4; k++) {
            if (e[k] < 0 || e[k] > MAX_COLOR)
                fatal("seed row %d has color %d outside 0..%d", i, e[k], MAX_COLOR);
            color_count[e[k]]++;
        }
        int z = (g_seed_top[i]==0) + (g_seed_right[i]==0)
              + (g_seed_bottom[i]==0) + (g_seed_left[i]==0);
        g_piece_zero_count[i] = z;
        if      (z == 0) { n_inner++; g_pieces_by_zero[0][g_pieces_by_zero_n[0]++] = i; }
        else if (z == 1) { n_edge++;  g_pieces_by_zero[1][g_pieces_by_zero_n[1]++] = i; }
        else if (z == 2) { n_corner++; g_pieces_by_zero[2][g_pieces_by_zero_n[2]++] = i; }
        else n_bad++;
    }
    int ch;
    do { ch = fgetc(f); } while (ch != EOF && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'));
    if (ch != EOF)
        fatal("seed: extra data after row %d", NUM_PIECES - 1);
    fclose(f);

    printf("[seed] %d pieces: %d inner, %d edge, %d corner",
           NUM_PIECES, n_inner, n_edge, n_corner);
    if (n_bad) printf(", %d invalid", n_bad);
    printf("\n");
    if (n_inner != 196 || n_edge != 56 || n_corner != 4 || n_bad != 0)
        fatal("seed frame types are %d inner / %d edge / %d corner / %d invalid; "
              "expected 196/56/4/0", n_inner, n_edge, n_corner, n_bad);
    if (color_count[0] != 4 * PUZZLE_SIDE)
        fatal("seed color 0 occurs %d times, expected %d",
              color_count[0], 4 * PUZZLE_SIDE);
    fflush(stdout);
}

/* -- Holes loading (newline-aware) -------------------------------------------- */

/*
 * Reads NUM_PIECES integers from holes file.
 * Newlines, commas, and tabs all act as delimiters - the file can be written
 * as a 16x16 matrix with one row per line.
 * Comment lines (# prefix) are skipped.
 */
static bool load_holes(const char *path, int holes[NUM_PIECES], char *why, size_t why_sz) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(why, why_sz, "cannot open holes file '%s': %s", path, strerror(errno)); return false; }

    /* Read entire file into a buffer */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f); snprintf(why, why_sz, "cannot seek holes file: %s", strerror(errno)); return false;
    }
    long fsz = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); snprintf(why, why_sz, "cannot rewind holes file: %s", strerror(errno)); return false;
    }
    if (fsz < 0) { fclose(f); snprintf(why, why_sz, "ftell failed for holes file"); return false; }
    char *raw = xmalloc((size_t)fsz + 1);
    size_t got = fread(raw, 1, (size_t)fsz, f);
    if (got != (size_t)fsz && ferror(f)) {
        int e = errno ? errno : EIO;
        free(raw); fclose(f);
        snprintf(why, why_sz, "cannot read holes file: %s", strerror(e));
        return false;
    }
    raw[got] = '\0';
    fclose(f);

    /* Strip comment lines (replace from # to \n with spaces) */
    for (char *p = raw; *p; p++) {
        if (*p == '#') {
            while (*p && *p != '\n') *p++ = ' ';
        }
    }

    int count = 0;
    char *p = raw;
    char *tok = strtok(p, ", \t\r\n");
    while (tok) {
        if (count >= NUM_PIECES) {
            free(raw);
            snprintf(why, why_sz, "holes: more than %d values", NUM_PIECES);
            return false;
        }
        char *end = NULL; errno = 0;
        long long v = strtoll(tok, &end, 10);
        if (errno || end == tok || *end) {
            free(raw);
            snprintf(why, why_sz, "holes: bad integer at value %d: '%s'", count, tok);
            return false;
        }
        if (v != 0 && v != 1) {
            free(raw);
            snprintf(why, why_sz, "holes: value %d is %lld; expected 0 or 1", count, v);
            return false;
        }
        holes[count++] = (int)v;
        tok = strtok(NULL, ", \t\r\n");
    }
    free(raw);

    if (count != NUM_PIECES) {
        snprintf(why, why_sz, "holes: got %d values, expected %d", count, NUM_PIECES);
        return false;
    }
    return true;
}

/* -- CSV parsing -------------------------------------------------------------- */

static bool parse_ll_token(const char *tok, long long *out) {
    if (!tok) return false;
    while (*tok == ' ' || *tok == '\t') tok++;
    if (!*tok) return false;
    char *end = NULL; errno = 0;
    long long v = strtoll(tok, &end, 10);
    if (errno || end == tok) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return false;
    *out = v;
    return true;
}

/* Count comma-separated fields in a CSV line (trailing newline ignored). */
static int count_csv_fields(const char *line) {
    int n = 1;
    for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++)
        if (*p == ',') n++;
    return n;
}

static bool parse_board_fields(char *const fields[], int n_fields, int first_pos,
                               int pos[NUM_PIECES], int rot[NUM_PIECES],
                               char *why, size_t why_sz) {
    if (first_pos < 0 || n_fields - first_pos != 2 * NUM_PIECES) {
        snprintf(why, why_sz, "layout has %d board fields, need exactly %d",
                 n_fields - first_pos, 2 * NUM_PIECES);
        return false;
    }
    long long v = 0;
    bool occupied[NUM_PIECES] = { false };
    for (int i = 0; i < NUM_PIECES; i++) {
        if (!parse_ll_token(fields[first_pos + i], &v)) {
            snprintf(why, why_sz, "bad pos[%d]", i);
            return false;
        }
        if (v != CSV_UNPLACED && (v < 0 || v >= NUM_PIECES)) {
            snprintf(why, why_sz, "pos[%d]=%lld out of range", i, v);
            return false;
        }
        if (v != CSV_UNPLACED) {
            if (occupied[v]) {
                snprintf(why, why_sz, "cell %lld occurs more than once in pos[]", v);
                return false;
            }
            occupied[v] = true;
        }
        pos[i] = (int)v;
    }
    for (int i = 0; i < NUM_PIECES; i++) {
        if (!parse_ll_token(fields[first_pos + NUM_PIECES + i], &v)) {
            snprintf(why, why_sz, "bad rot[%d]", i);
            return false;
        }
        if (v < 0 || v > 3) {
            snprintf(why, why_sz, "rot[%d]=%lld out of range", i, v);
            return false;
        }
        rot[i] = (int)v;
    }
    return true;
}

/*
 * Parse one of the two documented, exact CSV layouts:
 *   canonical / Stage-B : config_id,meta,pos[256],rot[256]       (514 fields)
 *                         (meta is the score of a canonical row, or the
 *                          sol_id of a Stage-B beamer/finalizer row)
 *   legacy tailsolver   : config_id,sol_id,rank,pos[256],rot[256] (515 fields)
 *
 * v9 guessed the layout from a single field-count special case and then allowed
 * arbitrary trailing fields.  A one-column shift silently corrupts every piece
 * position, so E555 accepts only these two unambiguous schemas.
 */
static bool parse_csv_line(const char *line,
                           char config_id_out[CONFIG_ID_LEN], long long *sol_id,
                           int pos[NUM_PIECES], int rot[NUM_PIECES],
                           char *why, size_t why_sz) {
    const int beam_fields = 2 + 2 * NUM_PIECES;
    const int tail_fields = 3 + 2 * NUM_PIECES;
    int n_fields = count_csv_fields(line);
    if (n_fields != beam_fields && n_fields != tail_fields) {
        snprintf(why, why_sz, "got %d fields; expected %d (beam) or %d (tail)",
                 n_fields, beam_fields, tail_fields);
        return false;
    }

    char *buf = xstrdup(line);
    char *end = buf + strlen(buf);
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';

    char **fields = xmalloc((size_t)n_fields * sizeof(*fields));
    char *pcur = buf;
    int got = 0;
    while (got < n_fields) {
        fields[got++] = strsep(&pcur, ",");
        if (!pcur) break;
    }
    if (got != n_fields) {
        snprintf(why, why_sz, "internal field-count mismatch (%d vs %d)", got, n_fields);
        free(fields); free(buf); return false;
    }

    char *tok = fields[0];
    while (*tok == ' ' || *tok == '\t') tok++;
    char *tok_end = tok + strlen(tok);
    while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) tok_end--;
    *tok_end = '\0';
    if (!*tok) {
        snprintf(why, why_sz, "empty config_id field");
        free(fields); free(buf); return false;
    }
    size_t cid_len = strlen(tok);
    if (cid_len >= CONFIG_ID_LEN) {
        snprintf(why, why_sz, "config_id length %zu exceeds limit %d",
                 cid_len, CONFIG_ID_LEN - 1);
        free(fields); free(buf); return false;
    }
    memcpy(config_id_out, tok, cid_len + 1);

    if (!parse_ll_token(fields[1], sol_id)) {
        snprintf(why, why_sz, "bad solution_id field");
        free(fields); free(buf); return false;
    }

    int first_pos = 2;
    if (n_fields == tail_fields) {
        long long rank = 0;
        if (!parse_ll_token(fields[2], &rank)) {
            snprintf(why, why_sz, "bad rank/meta field");
            free(fields); free(buf); return false;
        }
        first_pos = 3;
    }

    bool ok = parse_board_fields(fields, n_fields, first_pos,
                                 pos, rot, why, why_sz);
    free(fields);
    free(buf);
    return ok;
}

/*
 * Reconstruct board from pos[]/rot[] arrays, then apply g_rotation CCW.
 * Holes are applied AFTER rotation (in the rotated frame) by the caller.
 */
static bool board_from_csv(Board *b,
                            const int pos[NUM_PIECES], const int rot[NUM_PIECES],
                            int *n_placed, char *why, size_t why_sz) {
    Board raw;
    board_init(&raw);
    *n_placed = 0;
    bool occupied[NUM_PIECES];
    memset(occupied, 0, sizeof(occupied));

    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == CSV_UNPLACED) continue;
        if (pos[pid] < 0 || pos[pid] >= NUM_PIECES)
            { snprintf(why, why_sz, "piece %d: invalid pos %d", pid, pos[pid]); return false; }
        if (rot[pid] < 0 || rot[pid] > 3)
            { snprintf(why, why_sz, "piece %d: invalid rot %d", pid, rot[pid]); return false; }
        if (occupied[pos[pid]])
            { snprintf(why, why_sz, "cell %d occupied twice", pos[pid]); return false; }
        occupied[pos[pid]] = true;
        int r = pos[pid] / PUZZLE_SIDE;
        int c = pos[pid] % PUZZLE_SIDE;
        board_place(&raw, r, c, pid, rot[pid]);
        (*n_placed)++;
    }

    if (g_rotation > 0)
        apply_rotation_k(b, &raw, g_rotation);
    else
        *b = raw;

    return true;
}

/*
 * E555: reconstruct the SEARCH-INITIAL board of a record: CSV pos/rot, then
 * rotation (inside board_from_csv), then holes (in the rotated frame).  Used
 * by process_line and by the duplicate-detection pass in main, so the state
 * that is hashed for deduplication is exactly the state that gets searched.
 */
static bool build_initial_board(const int pos[NUM_PIECES], const int rot[NUM_PIECES],
                                Board *b, int *n_placed, int *holes_applied,
                                char *why, size_t why_sz) {
    *holes_applied = 0;
    if (!board_from_csv(b, pos, rot, n_placed, why, why_sz))
        return false;
    if (g_holes_active) {
        for (int i = 0; i < NUM_PIECES; i++) {
            if (g_holes[i] == 0) continue;
            int r = i / PUZZLE_SIDE, c = i % PUZZLE_SIDE;
            if (b->cell[r][c].piece_id != EMPTY_PIECE) {
                board_unplace(b, r, c);
                (*n_placed)--;
                (*holes_applied)++;
            }
        }
    }
    /* With a stop band, everything outside it is cleared and its pieces go back
     * to the pool.  Without this a beamer partial that already carries rows
     * above the band would starve the band of pieces it should be free to use,
     * and the enumeration would silently be of a smaller set than asked for.
     * Runs after holes and before the state hash, so deduplication still sees
     * exactly the board that gets searched. */
    if (g_stop_active) {
        for (int r = 0; r < PUZZLE_SIDE; r++) {
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                if (cell_in_band_search(r, c)) continue;
                if (b->cell[r][c].piece_id != EMPTY_PIECE) {
                    board_unplace(b, r, c);
                    (*n_placed)--;
                }
            }
        }
    }
    return true;
}

/* FNV-1a over the cell contents (piece id + rotation; empty cells fold in a
 * sentinel).  Characterizes the post-holes initial board for deduplication. */
static uint64_t board_state_hash(const Board *b) {
    uint64_t h = 1469598103934665603ULL;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            uint32_t v = (o->piece_id == EMPTY_PIECE)
                         ? 0xFFFFFFFFu
                         : (((uint32_t)(uint16_t)o->piece_id << 2) | o->rotation);
            for (int k = 0; k < 4; k++) {
                h ^= (v >> (8 * k)) & 0xFFu;
                h *= 1099511628211ULL;
            }
        }
    }
    return h;
}

/* Exact equality of two boards' placements (piece + rotation per cell). */
static bool board_cells_equal(const Board *a, const Board *b) {
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *oa = &a->cell[r][c], *ob = &b->cell[r][c];
            if (oa->piece_id != ob->piece_id) return false;
            if (oa->piece_id != EMPTY_PIECE && oa->rotation != ob->rotation) return false;
        }
    }
    return true;
}

/* -- Board printing ----------------------------------------------------------- */

/*
 * ASCII board.  Mismatched internal edges between two placed pieces are marked
 * with '$':
 *   - a horizontal mismatch (left/right neighbours) replaces the vertical '|'
 *     separator between the two cells with '$';
 *   - a vertical mismatch (up/down neighbours) replaces the '------' border
 *     segment between the two cells with '$$$$$$'.
 * Matched edges and frame edges keep the normal '|' / '-' characters.
 */
static void print_board_ascii(FILE *f, const Board *b, const char *title) {
    if (title && title[0]) fprintf(f, "\n%s\n", title);
    fprintf(f, "\n         ");
    for (int c = 0; c < PUZZLE_SIDE; c++) fprintf(f, " (%2d) ", c+1);
    fprintf(f, "\n         ");
    for (int c = 0; c < PUZZLE_SIDE; c++) fprintf(f, "+------");
    fprintf(f, "+\n");
    for (int r = PUZZLE_SIDE-1; r >= 0; r--) {
        fprintf(f, "  (%2d)   ", r+1);
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            /* separator to the left of cell c: '$' if it breaks a placed pair */
            char sep = '|';
            if (c > 0) {
                const Oriented *l = &b->cell[r][c-1], *o = &b->cell[r][c];
                if (l->piece_id != EMPTY_PIECE && o->piece_id != EMPTY_PIECE &&
                    l->right != o->left) sep = '$';
            }
            fprintf(f, "%c", sep);
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) fprintf(f, " ***  ");
            else fprintf(f, "%3u:%1u ", (unsigned)((int)o->piece_id + 1),
                         (unsigned)o->rotation);
        }
        fprintf(f, "|\n         ");
        /* border line below row r: '$$$$$$' under a broken vertical pair (r,r-1) */
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            fprintf(f, "+");
            bool vbreak = false;
            if (r > 0) {
                const Oriented *o = &b->cell[r][c], *d = &b->cell[r-1][c];
                if (o->piece_id != EMPTY_PIECE && d->piece_id != EMPTY_PIECE &&
                    o->bottom != d->top) vbreak = true;
            }
            fprintf(f, vbreak ? "$$$$$$" : "------");
        }
        fprintf(f, "+\n");
    }
    fprintf(f, "\n");
}

/* -- Placement validation ----------------------------------------------------- */

typedef enum {
    FIT_OK=0,
    FIT_REJECT_ZERO_RULE,
    FIT_REJECT_LEFT,
    FIT_REJECT_BOTTOM,
    FIT_REJECT_TOP,
    FIT_REJECT_RIGHT
} FitResult;

static bool frame_zero_rule_ok(int row, int col, const Oriented *o) {
    if ((row == PUZZLE_SIDE-1) != (o->top    == 0)) return false;
    if ((col == PUZZLE_SIDE-1) != (o->right  == 0)) return false;
    if ((row == 0)             != (o->bottom == 0)) return false;
    if ((col == 0)             != (o->left   == 0)) return false;
    return true;
}

/*
 * v6 mismatch-aware fit.  Returns the number of *broken internal edges* that
 * placing `o` at (row,col) would create against ALREADY-PLACED neighbours, or
 * -1 if the gray-0 frame rule is violated (a hard structural reject that is
 * never treated as a mismatch).
 *
 * Each internal edge is shared by two cells and is evaluated exactly once - at
 * the moment the SECOND of the two cells is filled - so summing this count over
 * a DFS path yields the exact number of broken edges with no double-counting.
 * An empty neighbour contributes nothing (its edge is decided later).
 */
static int piece_break_count(const Board *b, int row, int col, const Oriented *o) {
    if (!frame_zero_rule_ok(row, col, o)) return -1;
    int breaks = 0;
    if (col > 0) {
        const Oriented *n = &b->cell[row][col-1];
        if (n->piece_id != EMPTY_PIECE && n->right != o->left) breaks++;
    }
    if (row > 0) {
        const Oriented *n = &b->cell[row-1][col];
        if (n->piece_id != EMPTY_PIECE && n->top != o->bottom) breaks++;
    }
    if (row < PUZZLE_SIDE-1) {
        const Oriented *n = &b->cell[row+1][col];
        if (n->piece_id != EMPTY_PIECE && n->bottom != o->top) breaks++;
    }
    if (col < PUZZLE_SIDE-1) {
        const Oriented *n = &b->cell[row][col+1];
        if (n->piece_id != EMPTY_PIECE && n->left != o->right) breaks++;
    }
    return breaks;
}

/* -- E555: bitset forward-checking engine ---------------------------------------- */
/*
 * Orientation bitsets: bit index = pid*4 + spin over all 1024 (piece,rotation)
 * placements.  Two static tables are built once after the seed loads:
 *   g_fit_side[s][k] = orientations whose side s (0=top,1=right,2=bottom,3=left)
 *                      shows color k;
 *   g_cellbase[r][c] = orientations satisfying the gray-0 frame rule at (r,c)
 *                      (this implicitly encodes the inner/edge/corner type).
 * The per-search FcState then keeps, for every EMPTY cell, the AND of its base
 * mask with the fit masks demanded by its already-placed neighbours (nbmask),
 * plus a mask of the orientations of still-unused pieces (unused4).  The exact-
 * fit domain of a cell is nbmask & unused4: candidate enumeration, MRV counts,
 * and empty-domain cutoffs all become a few dozen word operations instead of a
 * 256-piece x 4-spin rescan.  Break (mismatch) placements are NOT in the index;
 * they are enumerated by the classic scan only when a mode actually wants them.
 */

#define OMASK_WORDS ((NUM_PIECES * 4 + 63) / 64)

typedef struct { uint64_t w[OMASK_WORDS]; } OMask;

static OMask g_fit_side[4][NUM_COLORS];
static OMask g_cellbase[PUZZLE_SIDE][PUZZLE_SIDE];

static inline void omask_set(OMask *m, int bit) { m->w[bit >> 6] |= 1ULL << (bit & 63); }
static inline void omask_and(OMask *dst, const OMask *src) {
    for (int i = 0; i < OMASK_WORDS; i++) dst->w[i] &= src->w[i];
}

static void build_fit_index(void) {
    memset(g_fit_side, 0, sizeof(g_fit_side));
    memset(g_cellbase, 0, sizeof(g_cellbase));
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        for (int spin = 0; spin < 4; spin++) {
            if ((g_piece_spin_mask[pid] & (1u << spin)) == 0) continue;
            Oriented o = make_oriented(pid, spin);
            int bit = pid * 4 + spin;
            omask_set(&g_fit_side[0][o.top],    bit);
            omask_set(&g_fit_side[1][o.right],  bit);
            omask_set(&g_fit_side[2][o.bottom], bit);
            omask_set(&g_fit_side[3][o.left],   bit);
            for (int r = 0; r < PUZZLE_SIDE; r++)
                for (int c = 0; c < PUZZLE_SIDE; c++)
                    if (frame_zero_rule_ok(r, c, &o))
                        omask_set(&g_cellbase[r][c], bit);
        }
    }
}

/* Per-search forward-checking state.  In addition to exact-fit masks, E555
 * maintains the remaining colour demand incrementally.  v9 recomputed that
 * demand by scanning all 256 cells after almost every accepted placement; on
 * the supplied run that happened roughly one billion times per hard record. */
typedef struct {
    OMask nbmask[PUZZLE_SIDE][PUZZLE_SIDE];
    OMask unused4;
    uint16_t dom_count[PUZZLE_SIDE][PUZZLE_SIDE];
    int16_t zero_domains;       /* empty cells with no exact-fit orientation */
    int16_t required_color[NUM_COLORS];
    int16_t need_type[3];
} FcState;

#define DOM_PLACED UINT16_MAX

static const int g_dr[4] = { 1, 0,-1, 0 };
static const int g_dc[4] = { 0, 1, 0,-1 };

/* Direction is from the empty cell toward its placed neighbour. */
static inline int nb_edge_toward(const Oriented *nb, int dir_from_empty) {
    switch (dir_from_empty) {
        case 0: return nb->bottom;
        case 1: return nb->left;
        case 2: return nb->top;
        case 3: return nb->right;
        default: return -1;
    }
}

/* Edge of a newly placed piece facing direction dir. */
static inline int own_edge_toward(const Oriented *o, int dir) {
    switch (dir) {
        case 0: return o->top;
        case 1: return o->right;
        case 2: return o->bottom;
        case 3: return o->left;
        default: return -1;
    }
}

static inline int omask_intersection_count(const OMask *a, const OMask *b) {
    int n = 0;
    for (int i = 0; i < OMASK_WORDS; i++)
        n += __builtin_popcountll(a->w[i] & b->w[i]);
    return n;
}

static void fc_compute_cell(const Board *b, FcState *fc, int r, int c) {
    uint16_t old = fc->dom_count[r][c];
    if (old != DOM_PLACED && old == 0) fc->zero_domains--;

    OMask m = g_cellbase[r][c];
    if (r+1 < PUZZLE_SIDE) {
        const Oriented *nb = &b->cell[r+1][c];
        if (nb->piece_id != EMPTY_PIECE) omask_and(&m, &g_fit_side[0][nb->bottom]);
    }
    if (c+1 < PUZZLE_SIDE) {
        const Oriented *nb = &b->cell[r][c+1];
        if (nb->piece_id != EMPTY_PIECE) omask_and(&m, &g_fit_side[1][nb->left]);
    }
    if (r > 0) {
        const Oriented *nb = &b->cell[r-1][c];
        if (nb->piece_id != EMPTY_PIECE) omask_and(&m, &g_fit_side[2][nb->top]);
    }
    if (c > 0) {
        const Oriented *nb = &b->cell[r][c-1];
        if (nb->piece_id != EMPTY_PIECE) omask_and(&m, &g_fit_side[3][nb->right]);
    }
    fc->nbmask[r][c] = m;
    int n = omask_intersection_count(&m, &fc->unused4);
    fc->dom_count[r][c] = (uint16_t)n;
    if (n == 0) fc->zero_domains++;
}

#if defined(VERIFY_INDEX) || defined(VERIFY_AVAIL)
static void fc_verify_state(const Board *b, const FcState *fc, const char *where);
#else
#define fc_verify_state(b,fc,where) ((void)0)
#endif

static void fc_init(FcState *fc, const Board *b) {
    memset(fc, 0, sizeof(*fc));
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            fc->dom_count[r][c] = DOM_PLACED;
    for (int pid = 0; pid < NUM_PIECES; pid++)
        if (!used_test(b->used, pid))
            fc->unused4.w[(pid * 4) >> 6] |=
                (uint64_t)g_piece_spin_mask[pid] << ((pid * 4) & 63);

    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (b->cell[r][c].piece_id != EMPTY_PIECE) continue;
            fc_compute_cell(b, fc, r, c);
            fc->need_type[cell_frame_degree(r, c)]++;
            for (int dir = 0; dir < 4; dir++) {
                int nr = r + g_dr[dir], nc = c + g_dc[dir];
                if (nr < 0 || nr >= PUZZLE_SIDE || nc < 0 || nc >= PUZZLE_SIDE) {
                    fc->required_color[0]++;
                } else {
                    const Oriented *nb = &b->cell[nr][nc];
                    if (nb->piece_id != EMPTY_PIECE)
                        fc->required_color[nb_edge_toward(nb, dir)]++;
                }
            }
        }
    }
    fc_verify_state(b, fc, "fc_init");
}

/* Removing or restoring one piece changes every exact domain only in the one
 * 64-bit word containing that piece's four orientation bits.  Maintaining the
 * counts this way makes MRV O(cells), not O(cells*16 words), and gives a global
 * zero-domain forward check essentially for free. */
static inline void fc_adjust_piece_support(const Board *b, FcState *fc,
                                           int pid, int sign) {
    int wi = (pid * 4) >> 6;
    uint64_t bits = (uint64_t)g_piece_spin_mask[pid] << ((pid * 4) & 63);
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (b->cell[r][c].piece_id != EMPTY_PIECE) continue;
            uint16_t old = fc->dom_count[r][c];
            if (old == DOM_PLACED) continue; /* newly emptied/selected cell */
            int delta = __builtin_popcountll(fc->nbmask[r][c].w[wi] & bits);
            if (delta == 0) continue;
            int next = (int)old + sign * delta;
            if (next < 0 || next > NUM_PIECES * 4)
                fatal("FC domain-count corruption at (r=%d,c=%d): %u %+d",
                      r, c, (unsigned)old, sign * delta);
            if (old == 0) fc->zero_domains--;
            fc->dom_count[r][c] = (uint16_t)next;
            if (next == 0) fc->zero_domains++;
        }
    }
}

#if defined(VERIFY_INDEX) || defined(VERIFY_AVAIL)
static void fc_verify_state(const Board *b, const FcState *fc, const char *where) {
    int zeros = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (b->cell[r][c].piece_id != EMPTY_PIECE) {
                if (fc->dom_count[r][c] != DOM_PLACED)
                    fatal("%s: placed cell (%d,%d) has domain count %u", where, r, c,
                          (unsigned)fc->dom_count[r][c]);
                continue;
            }
            int n = omask_intersection_count(&fc->nbmask[r][c], &fc->unused4);
            if (n != (int)fc->dom_count[r][c])
                fatal("%s: domain count (%d,%d) cached=%u actual=%d", where, r, c,
                      (unsigned)fc->dom_count[r][c], n);
            if (n == 0) zeros++;
        }
    }
    if (zeros != fc->zero_domains)
        fatal("%s: zero_domains cached=%d actual=%d", where, fc->zero_domains, zeros);
}
#endif

static inline void fc_refresh_neighbors(const Board *b, FcState *fc, int r, int c) {
    if (r+1 < PUZZLE_SIDE && b->cell[r+1][c].piece_id == EMPTY_PIECE) fc_compute_cell(b, fc, r+1, c);
    if (c+1 < PUZZLE_SIDE && b->cell[r][c+1].piece_id == EMPTY_PIECE) fc_compute_cell(b, fc, r, c+1);
    if (r > 0             && b->cell[r-1][c].piece_id == EMPTY_PIECE) fc_compute_cell(b, fc, r-1, c);
    if (c > 0             && b->cell[r][c-1].piece_id == EMPTY_PIECE) fc_compute_cell(b, fc, r, c-1);
}

/* DFS-side place/unplace: keep board, domains, availability and colour demand
 * in lockstep.  The demand update touches only the four incident edges. */
static inline void dfs_place(Board *b, FcState *fc, int r, int c, int pid, int spin) {
    const Oriented *o = &g_oriented[pid][spin & 3];

    if (fc->dom_count[r][c] == 0) fc->zero_domains--;
    fc->dom_count[r][c] = DOM_PLACED;

    fc->need_type[cell_frame_degree(r, c)]--;
    for (int dir = 0; dir < 4; dir++) {
        int nr = r + g_dr[dir], nc = c + g_dc[dir];
        if (nr < 0 || nr >= PUZZLE_SIDE || nc < 0 || nc >= PUZZLE_SIDE) {
            fc->required_color[0]--;          /* remove this empty border demand */
        } else {
            const Oriented *nb = &b->cell[nr][nc];
            if (nb->piece_id != EMPTY_PIECE)
                fc->required_color[nb_edge_toward(nb, dir)]--;
        }
    }

    board_place(b, r, c, pid, spin);
    fc->unused4.w[(pid * 4) >> 6] &= ~(0xFULL << ((pid * 4) & 63));
    fc_adjust_piece_support(b, fc, pid, -1);

    for (int dir = 0; dir < 4; dir++) {
        int nr = r + g_dr[dir], nc = c + g_dc[dir];
        if (nr >= 0 && nr < PUZZLE_SIDE && nc >= 0 && nc < PUZZLE_SIDE &&
            b->cell[nr][nc].piece_id == EMPTY_PIECE)
            fc->required_color[own_edge_toward(o, dir)]++;
    }
    fc_refresh_neighbors(b, fc, r, c);
    fc_verify_state(b, fc, "dfs_place");
}

static inline void dfs_unplace(Board *b, FcState *fc, int r, int c) {
    Oriented o = b->cell[r][c];
    int pid = (int)o.piece_id;

    for (int dir = 0; dir < 4; dir++) {
        int nr = r + g_dr[dir], nc = c + g_dc[dir];
        if (nr >= 0 && nr < PUZZLE_SIDE && nc >= 0 && nc < PUZZLE_SIDE &&
            b->cell[nr][nc].piece_id == EMPTY_PIECE)
            fc->required_color[own_edge_toward(&o, dir)]--;
    }

    board_unplace(b, r, c);
    if (pid >= 0)
        fc->unused4.w[(pid * 4) >> 6] |=
            (uint64_t)g_piece_spin_mask[pid] << ((pid * 4) & 63);
    if (pid >= 0) fc_adjust_piece_support(b, fc, pid, +1);

    fc->need_type[cell_frame_degree(r, c)]++;
    for (int dir = 0; dir < 4; dir++) {
        int nr = r + g_dr[dir], nc = c + g_dc[dir];
        if (nr < 0 || nr >= PUZZLE_SIDE || nc < 0 || nc >= PUZZLE_SIDE) {
            fc->required_color[0]++;
        } else {
            const Oriented *nb = &b->cell[nr][nc];
            if (nb->piece_id != EMPTY_PIECE)
                fc->required_color[nb_edge_toward(nb, dir)]++;
        }
    }

    fc_compute_cell(b, fc, r, c);       /* (r,c) is empty again */
    fc_refresh_neighbors(b, fc, r, c);
    fc_verify_state(b, fc, "dfs_unplace");
}

/* Number of exact fits (0-break placements of unused pieces) at empty (r,c). */
static inline int fc_exact_count(const FcState *fc, int r, int c) {
    return (int)fc->dom_count[r][c];
}

static inline bool fc_dom_nonzero(const FcState *fc, int r, int c) {
    return fc->dom_count[r][c] != 0;
}


/* -- E555: Hall / deficiency prune ----------------------------------------------- */
/*
 * Bipartite lower bound on the broken edges any completion must still add.
 *
 * Left vertices: the empty cells.  Right vertices: the unused pieces.  Edge
 * (cell, piece) iff SOME rotation of the piece satisfies the frame rule and
 * every color already fixed around the cell - i.e. the piece appears in the
 * cell's exact-fit FC domain.  By Koenig/Ore, a maximum matching leaves exactly
 * d = max over cell subsets S of (|S| - |N(S)|) cells unmatched (the graph's
 * deficiency).  Any completion assigns distinct pieces to cells, so at least d
 * cells receive a NON-adjacent piece; a frame-legal placement of a non-adjacent
 * piece mismatches at least one edge against a neighbour that was already fixed
 * when the domain was computed, and those violated edges are pairwise distinct
 * (each touches exactly one empty cell).  Hence
 *
 *     additional broken edges >= deficiency d,
 *
 * valid at ANY break budget - unlike the per-color marginal count in
 * remaining_feasibility_ex, which treats the four faces of one piece as
 * independently available and therefore misses cross-color contention (one
 * bi-colored piece "promised" to two cells at once).
 *
 * Implementation: greedy pre-match (cells in ascending domain size), then
 * Kuhn augmenting paths for the leftovers, with piece-bitset adjacency.  Early
 * exit as soon as the unmatched count exceeds the budget.  Typical cost is a
 * few thousand word ops per call because the greedy pass matches almost every
 * cell whenever domains are wide.
 */

#define PMASK_WORDS USED_WORDS   /* 256 pieces -> 4 x u64 */

/* Compact every 4th bit (positions 0,4,...,60) into the low 16 bits. */
static inline uint64_t nibble_flags_to_16(uint64_t x) {
    x &= 0x1111111111111111ULL;
    x = (x | (x >> 3))  & 0x0303030303030303ULL;
    x = (x | (x >> 6))  & 0x000F000F000F000FULL;
    x = (x | (x >> 12)) & 0x000000FF000000FFULL;
    x = (x | (x >> 24)) & 0x000000000000FFFFULL;
    return x;
}

/* Collapse an orientation domain (1024 bits) to a piece mask (256 bits). */
static inline void collapse_to_pieces(const OMask *nbm, const OMask *unused4,
                                      uint64_t out[PMASK_WORDS]) {
    for (int pw = 0; pw < PMASK_WORDS; pw++) {
        uint64_t r = 0;
        for (int k = 0; k < 4; k++) {
            uint64_t w = nbm->w[pw*4 + k] & unused4->w[pw*4 + k];
            w |= w >> 1;
            w |= w >> 2;                       /* any bit in nibble -> bit 4i */
            r |= nibble_flags_to_16(w) << (16 * k);
        }
        out[pw] = r;
    }
}

static bool hall_augment(int ci,
                         uint64_t adj[][PMASK_WORDS],
                         int16_t match_cell[NUM_PIECES],
                         uint64_t visited[PMASK_WORDS]) {
    for (int w = 0; w < PMASK_WORDS; w++) {
        uint64_t cand = adj[ci][w] & ~visited[w];
        while (cand) {
            int bit = __builtin_ctzll(cand);
            cand &= cand - 1;
            visited[w] |= 1ULL << bit;
            int p = w * 64 + bit;
            if (match_cell[p] < 0 ||
                hall_augment(match_cell[p], adj, match_cell, visited)) {
                match_cell[p] = (int16_t)ci;
                return true;
            }
        }
    }
    return false;
}

/*
 * Returns true when the deficiency of (cells x unused pieces) provably exceeds
 * `budget` (prune the node).  With out_deficiency non-NULL the full matching is
 * computed and the exact deficiency reported (used at the record root).
 */
static bool hall_prune(const FcState *fc, const Cell *cells, int n_cells,
                       int budget, int *out_deficiency) {
    if (out_deficiency) *out_deficiency = 0;
    if (n_cells <= 0) return false;
    if (n_cells > MAX_SEQ_LEN) fatal("hall_prune: %d cells", n_cells);

    static _Thread_local uint64_t adj[MAX_SEQ_LEN][PMASK_WORDS];
    int cnt[MAX_SEQ_LEN];
    for (int i = 0; i < n_cells; i++) {
        collapse_to_pieces(&fc->nbmask[cells[i].row][cells[i].col],
                           &fc->unused4, adj[i]);
        int n = 0;
        for (int w = 0; w < PMASK_WORDS; w++) n += __builtin_popcountll(adj[i][w]);
        cnt[i] = n;
    }

    /* Cells in ascending domain size (counting sort; domains are <= 256). */
    int order[MAX_SEQ_LEN];
    {
        int bucket[NUM_PIECES + 1] = {0};
        for (int i = 0; i < n_cells; i++) bucket[cnt[i]]++;
        int acc = 0;
        for (int k = 0; k <= NUM_PIECES; k++) { int t = bucket[k]; bucket[k] = acc; acc += t; }
        for (int i = 0; i < n_cells; i++) order[bucket[cnt[i]]++] = i;
    }

    int16_t match_cell[NUM_PIECES];
    memset(match_cell, -1, sizeof(match_cell));
    uint64_t piece_taken[PMASK_WORDS] = {0};

    int deferred[MAX_SEQ_LEN];
    int n_deferred = 0;
    int unmatched = 0;

    /* Greedy pass: first free piece in the cell's domain. */
    for (int oi = 0; oi < n_cells; oi++) {
        int ci = order[oi];
        if (cnt[ci] == 0) {                    /* empty domain: never matchable */
            if (++unmatched > budget) return true;
            continue;
        }
        bool got = false;
        for (int w = 0; w < PMASK_WORDS && !got; w++) {
            uint64_t freep = adj[ci][w] & ~piece_taken[w];
            if (freep) {
                int bit = __builtin_ctzll(freep);
                piece_taken[w] |= 1ULL << bit;
                match_cell[w * 64 + bit] = (int16_t)ci;
                got = true;
            }
        }
        if (!got) deferred[n_deferred++] = ci;
    }

    /* Augmenting paths for the leftovers. */
    for (int di = 0; di < n_deferred; di++) {
        uint64_t visited[PMASK_WORDS] = {0};
        if (!hall_augment(deferred[di], adj, match_cell, visited)) {
            if (++unmatched > budget) return true;
        }
    }

    if (out_deficiency) *out_deficiency = unmatched;
    return unmatched > budget;
}

/*
 * Count broken internal edges on a COMPLETE board and how many distinct pieces
 * are incident to at least one broken edge (clustering metric).  Each internal
 * edge is counted once (right edge of (r,c) vs left of (r,c+1); top of (r,c) vs
 * bottom of (r+1,c)).  Border edges are not internal and are ignored here.
 */
static void board_break_stats(const Board *b, int *out_breaks, int *out_pieces) {
    int breaks = 0;
    bool touched[NUM_PIECES];
    memset(touched, 0, sizeof(touched));
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) continue;
            if (c+1 < PUZZLE_SIDE) {
                const Oriented *e = &b->cell[r][c+1];
                if (e->piece_id != EMPTY_PIECE && o->right != e->left) {
                    breaks++;
                    touched[o->piece_id] = true;
                    touched[e->piece_id] = true;
                }
            }
            if (r+1 < PUZZLE_SIDE) {
                const Oriented *n = &b->cell[r+1][c];
                if (n->piece_id != EMPTY_PIECE && o->top != n->bottom) {
                    breaks++;
                    touched[o->piece_id] = true;
                    touched[n->piece_id] = true;
                }
            }
        }
    }
    int npieces = 0;
    for (int i = 0; i < NUM_PIECES; i++) if (touched[i]) npieces++;
    if (out_breaks) *out_breaks = breaks;
    if (out_pieces) *out_pieces = npieces;
}

/*
 * Count matched (connected) and broken internal edges on any board (partial or
 * complete).  Each internal adjacency between two placed pieces is one edge:
 * matched if the touching colours are equal, broken otherwise.  Empty cells
 * contribute nothing.  Border (gray-0) edges are not internal and are ignored.
 */
static void board_edge_counts(const Board *b, int *out_connected, int *out_broken) {
    int conn = 0, brk = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) continue;
            if (c+1 < PUZZLE_SIDE) {
                const Oriented *e = &b->cell[r][c+1];
                if (e->piece_id != EMPTY_PIECE) { if (o->right == e->left) conn++; else brk++; }
            }
            if (r+1 < PUZZLE_SIDE) {
                const Oriented *n = &b->cell[r+1][c];
                if (n->piece_id != EMPTY_PIECE) { if (o->top == n->bottom) conn++; else brk++; }
            }
        }
    }
    if (out_connected) *out_connected = conn;
    if (out_broken)    *out_broken    = brk;
}

/* Count occupied cells on the four exterior sides.  Corners count once in
 * border_total and once in each incident side count.  This diagnostic is
 * especially useful for 2sides/4sides, whose purpose is visible side growth. */
static void board_border_counts(const Board *b,
                                int *border_total,
                                int *left, int *right,
                                int *bottom, int *top) {
    int nb = 0, nl = 0, nr = 0, nd = 0, nu = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (b->cell[r][c].piece_id == EMPTY_PIECE) continue;
            bool on_left   = (c == 0);
            bool on_right  = (c == PUZZLE_SIDE - 1);
            bool on_bottom = (r == 0);
            bool on_top    = (r == PUZZLE_SIDE - 1);
            if (on_left)   nl++;
            if (on_right)  nr++;
            if (on_bottom) nd++;
            if (on_top)    nu++;
            if (on_left || on_right || on_bottom || on_top) nb++;
        }
    }
    if (border_total) *border_total = nb;
    if (left)         *left         = nl;
    if (right)        *right        = nr;
    if (bottom)       *bottom       = nd;
    if (top)          *top          = nu;
}

/*
 * Validate a partial board.  `max_breaks` internal adjacency mismatches are
 * tolerated (for mismatch mode and for re-reading this program's own break-
 * carrying best-partial output); the gray-0 frame rule and the placed-count are
 * always strict.  When max_breaks==0 this is exactly the v5 check.
 */
static bool validate_partial_board_ex(const Board *b, int n_placed, int max_breaks,
                                      char *why, size_t why_sz) {
    int placed = 0;
    int breaks = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) continue;
            placed++;
            if (!frame_zero_rule_ok(r, c, o)) {
                snprintf(why, why_sz,
                         "frame-zero violation at (r=%d,c=%d) piece=%d rot=%u",
                         r, c, (int)o->piece_id, (unsigned)o->rotation);
                return false;
            }
            if (c+1 < PUZZLE_SIDE && b->cell[r][c+1].piece_id != EMPTY_PIECE &&
                o->right != b->cell[r][c+1].left) {
                if (++breaks > max_breaks) {
                    snprintf(why, why_sz, "horizontal mismatch at (r=%d,c=%d)", r, c);
                    return false;
                }
            }
            if (r+1 < PUZZLE_SIDE && b->cell[r+1][c].piece_id != EMPTY_PIECE &&
                o->top != b->cell[r+1][c].bottom) {
                if (++breaks > max_breaks) {
                    snprintf(why, why_sz, "vertical mismatch at (r=%d,c=%d)", r, c);
                    return false;
                }
            }
        }
    }
    if (placed != n_placed) {
        snprintf(why, why_sz, "count mismatch: placed=%d expected=%d", placed, n_placed);
        return false;
    }
    return true;
}

/*
 * v6 lenient full-board check for mismatch mode: every cell must be filled and
 * obey the gray-0 frame rule, but up to `max_breaks` broken internal edges are
 * tolerated.  The actual broken-edge count is returned via *out_breaks.  Fails
 * only on a structural defect (empty cell, frame violation) or if breaks exceed
 * max_breaks (an internal-consistency guard - should never happen).
 */
static bool validate_complete_board_lenient(const Board *b, int max_breaks,
                                            int *out_breaks, int *out_pieces,
                                            char *why, size_t why_sz) {
    int placed = board_count_placed(b);
    if (placed != NUM_PIECES) {
        snprintf(why, why_sz, "full board has %d pieces, expected %d", placed, NUM_PIECES);
        return false;
    }
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) {
                snprintf(why, why_sz, "empty cell at (r=%d,c=%d)", r, c); return false;
            }
            if (!frame_zero_rule_ok(r, c, o)) {
                snprintf(why, why_sz, "frame-zero violation at (r=%d,c=%d)", r, c); return false;
            }
        }
    }
    int breaks = 0, pieces = 0;
    board_break_stats(b, &breaks, &pieces);
    if (out_breaks) *out_breaks = breaks;
    if (out_pieces) *out_pieces = pieces;
    if (breaks > max_breaks) {
        snprintf(why, why_sz, "broken edges %d exceed allowed %d", breaks, max_breaks);
        return false;
    }
    return true;
}

/* -- Search sequence ---------------------------------------------------------- */

/*
 * E555: spiralout -- inside-out rectilinear ring path.  Unlike centerout (sorted
 * by distance, ragged frontier) this walks contiguous rings with fixed
 * chirality, so every new cell touches the previous ring and the previous cell
 * (typically 2 fixed sides).  Ring k (k=7 innermost 2x2 .. 0 border) is walked
 *   left column (top-1 down to bottom), bottom row (left+1 .. right),
 *   right column (bottom+1 .. top), top row (right-1 down to left),
 * so ring k ends at its own top-left corner, adjacent to ring k-1's entry.
 * First cell placed is (7,7); the final border ring passes the lower-left
 * corner (0,0), the lower-right (0,15), the upper-right (15,15), and ends at
 * the TOP-LEFT corner (r=15,c=0) -- the last piece placed.
 */
static int g_spiralout_key[PUZZLE_SIDE][PUZZLE_SIDE];

static void build_spiralout_table(void) {
    int idx = 0;
    bool seen[PUZZLE_SIDE][PUZZLE_SIDE];
    memset(seen, 0, sizeof(seen));
    for (int k = PUZZLE_SIDE/2 - 1; k >= 0; k--) {
        int lo = k, hi = PUZZLE_SIDE - 1 - k;
        for (int r = hi-1; r >= lo; r--) { g_spiralout_key[r][lo] = idx++; seen[r][lo] = true; }
        for (int c = lo+1; c <= hi; c++) { g_spiralout_key[lo][c] = idx++; seen[lo][c] = true; }
        for (int r = lo+1; r <= hi; r++) { g_spiralout_key[r][hi] = idx++; seen[r][hi] = true; }
        for (int c = hi-1; c >= lo; c--) { g_spiralout_key[hi][c] = idx++; seen[hi][c] = true; }
    }
    if (idx != NUM_PIECES)
        fatal("spiralout table covers %d cells, expected %d", idx, NUM_PIECES);
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (!seen[r][c]) fatal("spiralout table misses cell (r=%d,c=%d)", r, c);
    if (g_spiralout_key[PUZZLE_SIDE-1][0] != NUM_PIECES - 1)
        fatal("spiralout table does not end at the top-left corner");
}

/*
 * Static-order sort key for an empty cell (r,c).  Lower key is filled first.
 * For ORD_MRV the static key is irrelevant (cells are chosen dynamically); we
 * fall back to row-major so the initial array is deterministic.
 *
 * Note rows are indexed with r=0 at the bottom; "rowmajor" therefore fills
 * bottom-to-top, matching v5.
 */
static long order_key(OrderMode m, int r, int c) {
    const int S = PUZZLE_SIDE, N = PUZZLE_SIDE - 1;
    /* --reverse gives each order its natural opposite direction.  Linear orders
     * mirror their scan; ring/distance orders keep their shell bucket (so
     * "border-first" stays border-first) and TRANSPOSE the within-shell walk,
     * which flips the traversal chirality.  MRV is unaffected: its key is only a
     * deterministic fallback, so --reverse is deliberately ignored there. */
    switch (m) {
        case ORD_COLMAJOR:
            /* fwd: columns L->R, each col bottom->top.
             * rev: columns R->L, each col top->bottom (180 deg). */
            return g_reverse ? (long)(N - c) * S + (N - r)
                             : (long)c * S + r;
        case ORD_SNAKE: {
            /* rev flips each row's scan direction (rows stay bottom->top). */
            int cc = g_reverse ? ((r & 1) ? c : (N - c))
                               : ((r & 1) ? (N - c) : c);
            return (long)r * S + cc;
        }
        case ORD_SPIRAL: {
            /* outside-in: smaller ring (closer to border) first.
             * rev transposes the within-ring walk -> up the left column first. */
            int ring = r; int t;
            t = N - r; if (t < ring) ring = t;
            t = c;     if (t < ring) ring = t;
            t = N - c; if (t < ring) ring = t;
            long sec = g_reverse ? (long)c * S + r : (long)r * S + c;
            return (long)ring * (S * S) + sec;
        }
        case ORD_CENTEROUT: {
            /* inside-out: smaller squared distance to centre (7.5,7.5) first.
             * Use doubled coords to keep integers: (2r-15)^2 + (2c-15)^2.
             * rev transposes the within-shell walk (chirality flip). */
            int dr = 2*r - N;
            int dc = 2*c - N;
            long sec = g_reverse ? (long)c * S + r : (long)r * S + c;
            return (long)(dr*dr + dc*dc) * (S * S) + sec;
        }
        case ORD_SPIRALOUT:
            /* inside-out ring path; rings are symmetric under transpose, so
             * looking up the transposed cell flips chirality while keeping the
             * inside-out progression. */
            return g_reverse ? (long)g_spiralout_key[c][r]
                             : (long)g_spiralout_key[r][c];
        case ORD_ROWMAJOR:
            /* rev: each row R->L, rows stay bottom->top. */
            return g_reverse ? (long)r * S + (N - c)
                             : (long)r * S + c;
        case ORD_MRV:
        default:
            return (long)r * S + c;
    }
}

typedef struct { long key; Cell cell; } OrderItem;

static int order_item_cmp(const void *pa, const void *pb) {
    const OrderItem *a = pa, *b = pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return 0;
}

static void side_sequence_add(const Board *b,
                              bool seen[PUZZLE_SIDE][PUZZLE_SIDE],
                              Cell *seq, int *n, int r, int c) {
    if (r < 0 || r >= PUZZLE_SIDE || c < 0 || c >= PUZZLE_SIDE) return;
    if (seen[r][c] || b->cell[r][c].piece_id != EMPTY_PIECE) return;
    if (!cell_in_band_search(r, c)) return;
    if (*n >= MAX_SEQ_LEN)
        fatal("search sequence overflow (MAX_SEQ_LEN=%d)", MAX_SEQ_LEN);
    seen[r][c] = true;
    seq[(*n)++] = (Cell){(int8_t)r, (int8_t)c};
}

/* Literal side-growth orders.  There is no MRV and no candidate-count tie
 * break here.
 *
 * 2sides, for every horizontal inset d:
 *   row 0 left, row 0 right, row 1 left, row 1 right, ...
 * This gives every row one cell from each lateral side before moving the two
 * fronts one column toward the centre.
 *
 * 4sides uses concentric insets and cycles left, right, bottom, top for each
 * coordinate along the current ring.  Duplicate corner visits are suppressed.
 * Thus every row receives its lateral cells and every column receives its
 * bottom/top cells before the search moves to the next inner ring. */
static void build_side_sequence(const Board *b, Cell *seq, int *n_seq,
                                OrderMode mode) {
    bool seen[PUZZLE_SIDE][PUZZLE_SIDE];
    memset(seen, 0, sizeof(seen));
    int n = 0;

    if (mode == ORD_2SIDES) {
        for (int d = 0; d < (PUZZLE_SIDE + 1) / 2; d++) {
            int left = d;
            int right = PUZZLE_SIDE - 1 - d;
            for (int r = 0; r < PUZZLE_SIDE; r++) {
                side_sequence_add(b, seen, seq, &n, r, left);
                side_sequence_add(b, seen, seq, &n, r, right);
            }
        }
    } else {
        for (int d = 0; d < (PUZZLE_SIDE + 1) / 2; d++) {
            int lo = d;
            int hi = PUZZLE_SIDE - 1 - d;
            for (int i = lo; i <= hi; i++) {
                side_sequence_add(b, seen, seq, &n, i,  lo); /* left   */
                side_sequence_add(b, seen, seq, &n, i,  hi); /* right  */
                side_sequence_add(b, seen, seq, &n, lo, i ); /* bottom */
                side_sequence_add(b, seen, seq, &n, hi, i ); /* top    */
            }
        }
    }

    int expected = board_count_searchable(b);
    if (n != expected)
        fatal("%s sequence covers %d empty cells, expected %d",
              order_name(mode), n, expected);
    *n_seq = n;
}

/* Build the list of empty cells ordered by the given static pattern. */
static void build_search_sequence_ordered(const Board *b, Cell *seq, int *n_seq,
                                          OrderMode mode) {
    if (order_is_side_growth(mode)) {
        build_side_sequence(b, seq, n_seq, mode);
        return;
    }

    OrderItem items[MAX_SEQ_LEN];
    int n = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (b->cell[r][c].piece_id == EMPTY_PIECE && cell_in_band_search(r, c)) {
                if (n >= MAX_SEQ_LEN)
                    fatal("search sequence overflow (MAX_SEQ_LEN=%d)", MAX_SEQ_LEN);
                items[n].key  = order_key(mode, r, c);
                items[n].cell = (Cell){(int8_t)r, (int8_t)c};
                n++;
            }
    qsort(items, (size_t)n, sizeof(OrderItem), order_item_cmp);
    for (int i = 0; i < n; i++) seq[i] = items[i].cell;
    *n_seq = n;
}

/* A short runtime trace makes stale binaries and accidental MRV routing
 * immediately obvious.  For 2sides the first layer must be the two lateral
 * border cells of each row; for 4sides it must be the outer L/R/B/T ring. */
static void print_side_sequence_preview(const Cell *seq, int n_seq) {
    int show = n_seq < 32 ? n_seq : 32;
    printf("[side-order] exact deferred-growth sequence; first %d/%d empty targets:",
           show, n_seq);
    for (int i = 0; i < show; i++)
        printf(" (%d,%d)", (int)seq[i].row, (int)seq[i].col);
    if (show < n_seq) printf(" ...");
    printf("\n");
}


/* -- Remaining-subproblem feasibility ----------------------------------------- */

typedef enum {
    FEAS_OK=0,
    FEAS_TYPE_MISMATCH,
    FEAS_COLOR_DEFICIT,
    FEAS_COLOR_ODD,
    FEAS_EMPTY_DOMAIN
} FeasCode;

typedef struct {
    FeasCode code;
    int color;
    int available, required, remainder;
    int need_type[3], avail_type[3];
} FeasIssue;

static const char *feas_code_name(FeasCode c) {
    switch (c) {
        case FEAS_OK:            return "ok";
        case FEAS_TYPE_MISMATCH: return "type_mismatch";
        case FEAS_COLOR_DEFICIT: return "color_deficit";
        case FEAS_COLOR_ODD:     return "color_odd_surplus";
        case FEAS_EMPTY_DOMAIN:  return "empty_exact_domain";
        default:                 return "unknown";
    }
}

static void describe_feas(const FeasIssue *x, char *buf, size_t cap) {
    if (!x || x->code == FEAS_OK) { snprintf(buf, cap, "ok"); return; }
    if (x->code == FEAS_TYPE_MISMATCH) {
        snprintf(buf, cap, "type_mismatch need(inner/edge/corner)=%d/%d/%d avail=%d/%d/%d",
                 x->need_type[0], x->need_type[1], x->need_type[2],
                 x->avail_type[0], x->avail_type[1], x->avail_type[2]);
        return;
    }
    if (x->code == FEAS_EMPTY_DOMAIN) {
        snprintf(buf, cap, "empty_exact_domain cells=%d remaining_break_budget=%d",
                 x->required, x->available);
        return;
    }
    snprintf(buf, cap, "%s color=%d available=%d required=%d surplus=%d",
             feas_code_name(x->code), x->color, x->available, x->required, x->remainder);
}

/*
 * v6: `budget` is the number of broken internal edges still allowed in the
 * remaining subproblem.  The colour accounting adapts to it soundly:
 *
 *   budget == 0  -> every remaining edge must match exactly: a colour needs a
 *                  non-negative, EVEN surplus (the classic v5 prune).
 *   budget  > 0  -> up to `budget` edges may break.  Breaking one edge frees
 *                  exactly one unmet colour demand, so completing the board
 *                  requires AT LEAST (sum of positive colour deficits) broken
 *                  edges.  Hence if deficit_total > budget the node is provably
 *                  infeasible and is pruned.  The evenness test is not sound
 *                  once breaks are permitted, so it is skipped.
 *
 * The type-count prune and the colour-range sanity check are always applied
 * (mismatches change neither piece types nor the gray-0 frame structure).
 */
static bool remaining_feasibility_ex(const Board *b, const FcState *fc,
                                     FeasIssue *issue, int budget) {
    const int16_t *required  = fc->required_color;
    const int16_t *need_type = fc->need_type;
    const int16_t *available = b->avail_color;
    const int16_t *avail_type= b->avail_type;

    if (issue) { memset(issue, 0, sizeof(*issue)); issue->code = FEAS_OK; issue->color = -1; }

#ifdef VERIFY_AVAIL
    {
        int chk_c[NUM_COLORS] = {0}, chk_t[3] = {0,0,0};
        int chk_req[NUM_COLORS] = {0}, chk_need[3] = {0,0,0};
        for (int pid = 0; pid < NUM_PIECES; pid++) {
            if (used_test(b->used, pid)) continue;
            chk_c[g_seed_top[pid]]++; chk_c[g_seed_right[pid]]++;
            chk_c[g_seed_bottom[pid]]++; chk_c[g_seed_left[pid]]++;
            int z = g_piece_zero_count[pid]; if (z>=0&&z<=2) chk_t[z]++;
        }
        for (int r = 0; r < PUZZLE_SIDE; r++) for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (b->cell[r][c].piece_id != EMPTY_PIECE) continue;
            chk_need[cell_frame_degree(r,c)]++;
            for (int dir = 0; dir < 4; dir++) {
                int nr=r+g_dr[dir], nc=c+g_dc[dir];
                if (nr<0||nr>=PUZZLE_SIDE||nc<0||nc>=PUZZLE_SIDE) chk_req[0]++;
                else if (b->cell[nr][nc].piece_id != EMPTY_PIECE)
                    chk_req[nb_edge_toward(&b->cell[nr][nc],dir)]++;
            }
        }
        for (int k = 0; k < NUM_COLORS; k++) {
            if (chk_c[k] != available[k]) fatal("avail_color[%d] %d != %d", k, available[k], chk_c[k]);
            if (chk_req[k] != required[k]) fatal("required_color[%d] %d != %d", k, required[k], chk_req[k]);
        }
        for (int z = 0; z < 3; z++) {
            if (chk_t[z] != avail_type[z]) fatal("avail_type[%d] %d != %d", z, avail_type[z], chk_t[z]);
            if (chk_need[z] != need_type[z]) fatal("need_type[%d] %d != %d", z, need_type[z], chk_need[z]);
        }
    }
#endif

    for (int z = 0; z <= 2; z++) {
        if (need_type[z] != avail_type[z]) {
            if (issue) {
                issue->code = FEAS_TYPE_MISMATCH; issue->color = z;
                for (int k = 0; k <= 2; k++) {
                    issue->need_type[k] = need_type[k];
                    issue->avail_type[k] = avail_type[k];
                }
            }
            return false;
        }
    }

    if (budget <= 0) {
        for (int col = 0; col < NUM_COLORS; col++) {
            int rem = available[col] - required[col];
            if (rem < 0) {
                if (issue) {
                    issue->code = FEAS_COLOR_DEFICIT; issue->color = col;
                    issue->available = available[col]; issue->required = required[col]; issue->remainder = rem;
                }
                return false;
            }
            if (rem & 1) {
                if (issue) {
                    issue->code = FEAS_COLOR_ODD; issue->color = col;
                    issue->available = available[col]; issue->required = required[col]; issue->remainder = rem;
                }
                return false;
            }
        }
    } else {
        int deficit_total = 0;
        int worst_col = -1, worst_def = 0;
        for (int col = 0; col < NUM_COLORS; col++) {
            int def = required[col] - available[col];
            if (def > 0) {
                deficit_total += def;
                if (def > worst_def) { worst_def = def; worst_col = col; }
            }
        }
        if (deficit_total > budget) {
            if (issue) {
                issue->code = FEAS_COLOR_DEFICIT; issue->color = worst_col;
                issue->available = worst_col >= 0 ? available[worst_col] : 0;
                issue->required  = worst_col >= 0 ? required[worst_col]  : 0;
                issue->remainder = -deficit_total;
            }
            return false;
        }
    }

    if (issue) {
        for (int k = 0; k <= 2; k++) {
            issue->need_type[k] = need_type[k];
            issue->avail_type[k] = avail_type[k];
        }
    }
    return true;
}


/* -- DFS statistics ----------------------------------------------------------- */

typedef struct {
    uint64_t visits[MAX_SEQ_LEN+1];
    uint64_t piece_tests[MAX_SEQ_LEN+1];
    uint64_t type_rejects[MAX_SEQ_LEN+1];
    uint64_t rotation_tests[MAX_SEQ_LEN+1];
    uint64_t fit_ok[MAX_SEQ_LEN+1];
    uint64_t reject_zero[MAX_SEQ_LEN+1];
    uint64_t reject_left[MAX_SEQ_LEN+1];
    uint64_t reject_bottom[MAX_SEQ_LEN+1];
    uint64_t reject_top[MAX_SEQ_LEN+1];
    uint64_t reject_right[MAX_SEQ_LEN+1];
    uint64_t parity_rejects[MAX_SEQ_LEN+1];
    uint64_t fc_rejects[MAX_SEQ_LEN+1];    /* global empty-domain lower bound */
    uint64_t hall_rejects[MAX_SEQ_LEN+1];  /* Hall/deficiency matching prune */
    uint64_t hall_calls[MAX_SEQ_LEN+1];
    uint64_t recursed[MAX_SEQ_LEN+1];
    uint64_t dead_nodes[MAX_SEQ_LEN+1];
    uint64_t completed_leaves;
    int max_depth_entered;       /* deepest recursive node entered */
    int max_placed_attempted;    /* deepest placement, including immediately-pruned children */
    int first_dead_depth_seen;   /* shallowest dead selected cell, deterministic after merge */
    int first_dead_row;
    int first_dead_col;
} DfsStats;

static void stats_init(DfsStats *st) {
    memset(st, 0, sizeof(*st));
    st->first_dead_depth_seen = -1;
    st->first_dead_row = -1;
    st->first_dead_col = -1;
}

static void stats_note_dead(DfsStats *st, int depth, int row, int col) {
    st->dead_nodes[depth]++;
    if (st->first_dead_depth_seen < 0 || depth < st->first_dead_depth_seen ||
        (depth == st->first_dead_depth_seen &&
         (row < st->first_dead_row ||
          (row == st->first_dead_row && col < st->first_dead_col)))) {
        st->first_dead_depth_seen = depth;
        st->first_dead_row = row;
        st->first_dead_col = col;
    }
}

static void stats_merge(DfsStats *dst, const DfsStats *src) {
#define MERGE_FIELD(field) \
    for (int _i = 0; _i <= MAX_SEQ_LEN; _i++) dst->field[_i] += src->field[_i]
    MERGE_FIELD(visits);
    MERGE_FIELD(piece_tests);
    MERGE_FIELD(type_rejects);
    MERGE_FIELD(rotation_tests);
    MERGE_FIELD(fit_ok);
    MERGE_FIELD(reject_zero);
    MERGE_FIELD(reject_left);
    MERGE_FIELD(reject_bottom);
    MERGE_FIELD(reject_top);
    MERGE_FIELD(reject_right);
    MERGE_FIELD(parity_rejects);
    MERGE_FIELD(fc_rejects);
    MERGE_FIELD(hall_rejects);
    MERGE_FIELD(hall_calls);
    MERGE_FIELD(recursed);
    MERGE_FIELD(dead_nodes);
#undef MERGE_FIELD
    dst->completed_leaves += src->completed_leaves;
    if (src->max_depth_entered > dst->max_depth_entered)
        dst->max_depth_entered = src->max_depth_entered;
    if (src->max_placed_attempted > dst->max_placed_attempted)
        dst->max_placed_attempted = src->max_placed_attempted;
    if (src->first_dead_depth_seen >= 0 &&
        (dst->first_dead_depth_seen < 0 ||
         src->first_dead_depth_seen < dst->first_dead_depth_seen ||
         (src->first_dead_depth_seen == dst->first_dead_depth_seen &&
          (src->first_dead_row < dst->first_dead_row ||
           (src->first_dead_row == dst->first_dead_row &&
            src->first_dead_col < dst->first_dead_col))))) {
        dst->first_dead_depth_seen = src->first_dead_depth_seen;
        dst->first_dead_row = src->first_dead_row;
        dst->first_dead_col = src->first_dead_col;
    }
}

static uint64_t sum_array(const uint64_t a[], int n) {
    uint64_t s = 0;
    int lim = n < MAX_SEQ_LEN ? n : MAX_SEQ_LEN;
    for (int i = 0; i <= lim; i++) s += a[i];
    return s;
}

#define TOTAL(field, st, ns)  sum_array((st)->field, (ns))

static int first_dead_depth(const DfsStats *st, int n_seq) {
    (void)n_seq;
    return st->first_dead_depth_seen;
}

/* -- Solution output ---------------------------------------------------------- */

static void board_to_pos_rot(const Board *b, int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    for (int i = 0; i < NUM_PIECES; i++) { pos[i] = CSV_UNPLACED; rot[i] = 0; }
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *o = &b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE) continue;
            int pid = (int)o->piece_id;
            pos[pid] = r * PUZZLE_SIDE + c;
            rot[pid] = (int)o->rotation;
        }
    }
}

static void write_band_partial(const Board *b,
                               const char *config_id_str, long long sol_id);

/*
 * Write a full solution. Board b is in rotated frame.
 * ASCII display uses rotated frame; CSV/txt output uses original frame.
 *
 * With a stop band the board is deliberately incomplete, so the full-board
 * validator below -- which treats any empty cell as a defect -- cannot run and
 * the band writer takes over.  Routing both through here means the four DFS
 * leaves need no knowledge of which mode they are in.
 */
static uint64_t write_solution(const Board *b,
                                const char *config_id_str, long long input_sol_id) {
    char why[256];
    if (g_stop_active) {
        write_band_partial(b, config_id_str, input_sol_id);
        uint64_t n;
        #pragma omp atomic capture
        n = ++g_total_solutions;
        return n;
    }
    /* For validation: use original frame */
    Board orig;
    const Board *out_b;
    if (g_rotation > 0) {
        inverse_rotation_k(&orig, b, g_rotation);
        out_b = &orig;
    } else {
        out_b = b;
    }

    int sol_breaks = 0, sol_break_pieces = 0;
    /* Tolerate up to the theoretical maximum of internal breaks: the board may
     * legitimately carry breaks (from a break-carrying input and/or the budget).
     * Structural defects (empty cell, frame rule) still abort. */
    if (!validate_complete_board_lenient(out_b, 2*PUZZLE_SIDE*(PUZZLE_SIDE-1),
                                         &sol_breaks, &sol_break_pieces,
                                         why, sizeof(why)))
        fatal("internal error: invalid full solution from sol_id=%lld: %s", input_sol_id, why);
    int sol_connected = 2*PUZZLE_SIDE*(PUZZLE_SIDE-1) - sol_breaks;

    uint64_t solnum;
    #pragma omp atomic capture
    solnum = ++g_total_solutions;

    /* The completed board is not written here: each record's best board (a
     * completion when one is found) is streamed once to output.csv at
     * end-of-record.  This only validates, counts, and -- under --verbose --
     * announces the completion. */
    if (g_verbose) {
        #pragma omp critical(stdout_print)
        {
            printf("*** SOLUTION %" PRIu64 " from config=%s sol_id=%lld  t=%.3fs"
                   "  connected_edges=%d  broken_edges=%d  pieces_with_breaks=%d ***\n",
                   solnum, config_id_str, input_sol_id, elapsed_wall(),
                   sol_connected, sol_breaks, sol_break_pieces);
            print_board_ascii(stdout, b, "=== FULL SOLUTION (rotated frame) ===");
            fflush(stdout);
        }
    }

    return solnum;
}

/*
 * Stream the single best board for a processed record to output.csv.  Layout
 * is the canonical E555 board row config_id_solid,score,pos[256],rot[256]
 * (514 fields; score = matched internal edges) in the original frame, and the
 * file can be fed straight back in as input.  One line per non-rejected
 * record; thread-safe.
 */
static void write_stream_best(const Board *b,
                              const char *config_id_str, long long sol_id) {
    if (!g_stream_csv) return;
    Board orig;
    const Board *out_b = b;
    if (g_rotation > 0) { inverse_rotation_k(&orig, b, g_rotation); out_b = &orig; }

    int conn = 0, brk = 0;
    board_edge_counts(out_b, &conn, &brk);
    int pos[NUM_PIECES], rot_arr[NUM_PIECES];
    board_to_pos_rot(out_b, pos, rot_arr);
    char buf[CSV_BUF_BYTES]; size_t off = 0;
    appendf(buf, sizeof(buf), &off, "%s_%lld,%d", config_id_str, sol_id, conn);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", pos[i]);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", rot_arr[i]);
    appendf(buf, sizeof(buf), &off, "\n");
    #pragma omp critical(stream_csv)
    { checked_fwrite(buf, 1, off, g_stream_csv, "output.csv");
      checked_fflush(g_stream_csv, "output.csv"); }
}

/*
 * E555: write one completed stop band.
 *
 * Same canonical 514-field layout as output.csv, so the finalizer and every
 * other reader take it unchanged; pieces outside the band are simply unplaced,
 * which board_to_pos_rot already renders as CSV_UNPLACED.  Field 2 is the
 * matched-edge count of the band alone -- board_edge_counts skips empty cells,
 * so adjacencies leaving the band are not counted either way.
 *
 * Two failure modes, treated differently on purpose.  A structural defect (an
 * unfilled band cell, a frame-zero violation) is an internal error and aborts,
 * exactly as write_solution does for a full board.  A BROKEN EDGE only drops
 * the board: --break-mode stuck takes a minimal break wherever no exact fit
 * exists, so it produces such bands legitimately, and they are useless
 * downstream because the finalizer validates every color match inside its
 * locked region.  Rejections are counted so a run that emits nothing is
 * diagnosable from the summary rather than mysterious.
 */
static void write_band_partial(const Board *b,
                               const char *config_id_str, long long sol_id) {
    if (!g_band_csv) return;
    Board orig;
    const Board *out_b = b;
    if (g_rotation > 0) { inverse_rotation_k(&orig, b, g_rotation); out_b = &orig; }

    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!cell_in_band(r, c)) continue;
            const Oriented *o = &out_b->cell[r][c];
            if (o->piece_id == EMPTY_PIECE)
                fatal("internal error: band cell (r=%d,c=%d) empty at emission "
                      "from sol_id=%lld", r, c, sol_id);
            if (!frame_zero_rule_ok(r, c, o))
                fatal("internal error: frame-zero violation at (r=%d,c=%d) in band "
                      "from sol_id=%lld", r, c, sol_id);
        }
    }

    int conn = 0, brk = 0;
    board_edge_counts(out_b, &conn, &brk);
    if (brk > 0) {
        #pragma omp atomic update
        g_band_rejected++;
        return;
    }

    int pos[NUM_PIECES], rot_arr[NUM_PIECES];
    board_to_pos_rot(out_b, pos, rot_arr);
    char buf[CSV_BUF_BYTES]; size_t off = 0;
    appendf(buf, sizeof(buf), &off, "%s_%lld,%d", config_id_str, sol_id, conn);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", pos[i]);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", rot_arr[i]);
    appendf(buf, sizeof(buf), &off, "\n");
    /* One fwrite per line keeps the append atomic, so several processes may
     * share one output file the way the beamer's emitters do. */
    #pragma omp critical(band_csv)
    { checked_fwrite(buf, 1, off, g_band_csv, "stop-band csv");
      checked_fflush(g_band_csv, "stop-band csv");
      g_band_emitted++; }
}

/* -- E555: crash-recovery checkpoint + progress heartbeat ------------------------ */

/*
 * Append one re-feedable line (same canonical 514-field layout as output.csv)
 * with a record's CURRENT best board to <output>.checkpoint.csv.  output.csv
 * itself is written only at end-of-record, so without this a killed multi-day
 * run would lose every in-flight record's best.  Bests improve monotonically,
 * so on crash recovery the LAST checkpoint line per record is its best; the
 * file is removed on clean completion (output.csv then holds something at
 * least as good).
 */
static void write_checkpoint_line(const Board *b,
                                  const char *config_id_str, long long sol_id) {
    if (!g_ckpt_csv) return;
    Board orig;
    const Board *out_b = b;
    if (g_rotation > 0) { inverse_rotation_k(&orig, b, g_rotation); out_b = &orig; }

    int conn = 0, brk = 0;
    board_edge_counts(out_b, &conn, &brk);
    int pos[NUM_PIECES], rot_arr[NUM_PIECES];
    board_to_pos_rot(out_b, pos, rot_arr);
    char buf[CSV_BUF_BYTES]; size_t off = 0;
    appendf(buf, sizeof(buf), &off, "%s_%lld,%d", config_id_str, sol_id, conn);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", pos[i]);
    for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", rot_arr[i]);
    appendf(buf, sizeof(buf), &off, "\n");
    #pragma omp critical(ckpt_csv)
    { checked_fwrite(buf, 1, off, g_ckpt_csv, "checkpoint.csv");
      checked_fflush(g_ckpt_csv, "checkpoint.csv"); }
}

/*
 * Periodic progress line (~every HEARTBEAT_SEC per record) so long searches are
 * observable: current budget level k, lds allowance D, elapsed time, node count
 * and the record's best board so far.  The next-heartbeat slot is claimed with
 * a CAS so exactly one thread prints per interval, even under search-parallel execution.
 * Also flushes a pending ("dirty") checkpoint whose 10 s throttle has expired.
 */
static void heartbeat_maybe(const SearchCtx *cx, double now) {
    long long now_ms = (long long)((now - g_t_start_wall) * 1000.0);
    long long nxt = atomic_load_explicit(&cx->live->hb_next_ms, memory_order_relaxed);
    if (now_ms < nxt) return;
    long long claim = now_ms + (long long)(HEARTBEAT_SEC * 1000.0);
    if (!atomic_compare_exchange_strong_explicit(&cx->live->hb_next_ms, &nxt, claim,
                                                 memory_order_relaxed, memory_order_relaxed))
        return;

    int bt = 0, bc = 0, bb = 0;
    if (cx->rstat) {
        omp_set_lock(&g_best_lock);
        bt = atomic_load_explicit(&cx->rstat->best_total, memory_order_relaxed);
        bc = cx->rstat->best_connected;
        bb = cx->rstat->best_broken;
        omp_unset_lock(&g_best_lock);
    }

    unsigned long long nodes =
        atomic_load_explicit(&cx->live->nodes, memory_order_relaxed);
    char dstr[32] = "";
    if (g_break_mode == BREAK_LDS && cx->k_budget > cx->input_breaks)
        snprintf(dstr, sizeof(dstr), " D=%d", cx->lds_D);
    #pragma omp critical(stdout_print)
    {
        printf("[cfg=%s sol=%lld] progress k=%d%s t=%.0fs nodes=%.1fM "
               "best=%d pieces connected=%d broken=%d\n",
               cx->config_id, cx->sol_id, cx->k_budget, dstr,
               now - cx->record_start, (double)nodes / 1e6, bt, bc, bb);
        fflush(stdout);
    }
}

/*
 * Flush a pending ("dirty") checkpoint whose throttle window has expired.  The
 * Shared checkpoint state is protected by g_best_lock.
 */
static void ckpt_flush_maybe(const SearchCtx *cx, double now) {
    RecStat *rs = cx->rstat;
    if (!g_ckpt_csv || !rs) return;
    bool  do_ckpt = false;
    Board ckpt_copy;
    /* ckpt_dirty and ckpt_next are shared by search workers.  Read them only
     * under the same lock used by try_update_best; the old unlocked fast path
     * was a C data race.  This runs only once per NODE_TICK_BATCH nodes. */
    omp_set_lock(&g_best_lock);
    if (rs->ckpt_dirty && now >= rs->ckpt_next && rs->best_board) {
        ckpt_copy = *rs->best_board;
        rs->ckpt_dirty = false;
        rs->ckpt_next  = now + CKPT_MIN_INTERVAL_SEC;
        do_ckpt = true;
    }
    omp_unset_lock(&g_best_lock);
    if (do_ckpt) write_checkpoint_line(&ckpt_copy, cx->config_id, cx->sol_id);
}

/* Batched per-node bookkeeping: node counter, deadline check, checkpoint
 * flush, heartbeat. */
#define NODE_TICK_BATCH 4096ULL
static void search_node_tick(const SearchCtx *cx) {
    atomic_fetch_add_explicit(&cx->live->nodes, NODE_TICK_BATCH, memory_order_relaxed);
    double now = omp_get_wtime();
    if (cx->deadline > 0.0 && now > cx->deadline) {
        search_request_stop(cx->live, STOP_TIMEOUT);
        return;
    }
    ckpt_flush_maybe(cx, now);
    heartbeat_maybe(cx, now);
}

/* -- Per-record status CSV ---------------------------------------------------- */

/*
 * One row is emitted for every parsed record selected for processing.
 */
static void write_status_csv(const char *config_id_str, long long sol_id,
                                    bool valid, const char *status,
                                    int n_placed, int n_empty, int holes_applied,
                                    double elapsed, uint64_t local_solutions,
                                    const FeasIssue *init_issue,
                                    const DfsStats *st, const Cell *seq, int n_seq,
                                    int best_total, int best_connected, int best_broken,
                                    int best_deep_row, int best_deep_col,
                                    int root_deficiency) {
    if (!g_status_csv) return;
    (void)seq;

    int fd = first_dead_depth(st, n_seq);
    int fd_row = st->first_dead_row;
    int fd_col = st->first_dead_col;
    int n_added_best = best_total > n_placed ? best_total - n_placed : 0;

    const char *fcode = (init_issue && init_issue->code != FEAS_OK)
                        ? feas_code_name(init_issue->code) : "ok";

    char buf[CSV_BUF_BYTES]; size_t off = 0;
    appendf(buf, sizeof(buf), &off,
            "%s,%lld,%s,%s,%s,%s,%s,%d,%d,%d,%.6f,%" PRIu64 ","
            "%d,%d,%d,%d,%d,%s,"
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d\n",
            config_id_str, sol_id,
            valid ? "yes" : "no",
            status,
            order_name(g_order_mode),
            parallel_mode_name(g_parallel_mode),
            effective_hall_name(),
            n_placed, n_empty, holes_applied,
            elapsed, local_solutions,
            st->max_depth_entered, st->max_placed_attempted,
            fd, fd_row, fd_col,
            fcode,
            TOTAL(visits,        st, n_seq),
            TOTAL(piece_tests,   st, n_seq),
            TOTAL(fit_ok,        st, n_seq),
            TOTAL(parity_rejects,st, n_seq),
            TOTAL(fc_rejects,    st, n_seq),
            TOTAL(hall_calls,    st, n_seq),
            TOTAL(hall_rejects,  st, n_seq),
            best_deep_row, best_deep_col, n_added_best,
            best_total, best_connected, best_broken,
            root_deficiency);

    #pragma omp critical(status_csv_write)
    { checked_fwrite(buf,1,off,g_status_csv,"status.csv");
      checked_fflush(g_status_csv,"status.csv"); }
}

/* -- Best partial board tracking (pure + mismatch tracks, per-record stat) ---- */

/* Strict "a is a better partial than b" ordering: more pieces, then more
 * connected edges, then fewer broken edges, then fewer pieces touched by those
 * breaks.  The final tie-break implements the clustering objective: two breaks
 * on one piece are preferred over two breaks spread across four pieces. */
static bool partial_better(int a_tot, int a_conn, int a_brk, int a_bpieces,
                           int b_tot, int b_conn, int b_brk, int b_bpieces) {
    if (a_tot     != b_tot)     return a_tot     > b_tot;
    if (a_conn    != b_conn)    return a_conn    > b_conn;
    if (a_brk     != b_brk)     return a_brk     < b_brk;
    return a_bpieces < b_bpieces;
}

/*
 * Called after each successful board_place (and at completion).  Routes the
 * board to the PURE track (broken==0) or the MISMATCH track (broken>0), keeping
 * each sorted best-first.  Also updates the per-record running best `rstat`.
 *
 * pure_track_ok is true only at the budget-0 deepening level.  A 0-break board
 * found while a break budget is active is NOT added to the pure track, so the
 * pure result is exactly what a --max-mismatch 0 run produces -- independent of
 * the requested budget.  rstat (the streamed overall best) is always updated.
 */
static void try_update_best(const Board *cur_board,
                            const Board *init_board,
                            int initial_n_placed,
                            int n_total, int n_added,
                            int connected, int breaks,
                            int last_row, int last_col,
                            const char *config_id_str, long long sol_id,
                            RecStat *rstat, bool pure_track_ok) {
    BestPartial *track = NULL; int *count = NULL; atomic_int *floorp = NULL;
    if (breaks > 0) {
        track = g_best_mm; count = &g_best_mm_count; floorp = &g_best_mm_floor;
    } else if (pure_track_ok) {
        track = g_best_pure; count = &g_best_pure_count; floorp = &g_best_pure_floor;
    }

    int rbest = rstat ? atomic_load_explicit(&rstat->best_total, memory_order_relaxed) : INT_MAX;
    int floor_snapshot = floorp ? atomic_load_explicit(floorp, memory_order_relaxed) : INT_MAX;
    bool rstat_cand = (rstat && n_total >= rbest);
    bool track_cand = (g_best_output > 0 && track && n_total >= floor_snapshot);
    if (!rstat_cand && !track_cand) return;

    /* Exact search now reaches this function with all metrics already known in
     * O(1).  v9 rescanned all 256 cells whenever a dead child tied the current
     * depth; that was billions of full-board scans in the supplied run. */
    int break_pieces = 0;
    if (breaks > 0) board_break_stats(cur_board, NULL, &break_pieces);

    bool  do_ckpt = false;
    Board ckpt_copy;

    omp_set_lock(&g_best_lock);

    if (rstat && partial_better(n_total, connected, breaks, break_pieces,
                                atomic_load_explicit(&rstat->best_total, memory_order_relaxed),
                                rstat->best_connected,
                                rstat->best_broken, rstat->best_break_pieces)) {
        rstat->best_connected    = connected;
        rstat->best_broken       = breaks;
        rstat->best_break_pieces = break_pieces;
        rstat->deepest_row       = last_row;
        rstat->deepest_col       = last_col;
        if (rstat->best_board) *rstat->best_board = *cur_board;
        atomic_store_explicit(&rstat->best_total, n_total, memory_order_relaxed);
        if (g_ckpt_csv) {
            double now = omp_get_wtime();
            if (now >= rstat->ckpt_next) {
                rstat->ckpt_next  = now + CKPT_MIN_INTERVAL_SEC;
                rstat->ckpt_dirty = false;
                ckpt_copy = *cur_board;
                do_ckpt = true;
            } else {
                rstat->ckpt_dirty = true;
            }
        }
    }

    if (g_best_output <= 0 || !track) {
        omp_unset_lock(&g_best_lock);
        if (do_ckpt) write_checkpoint_line(&ckpt_copy, config_id_str, sol_id);
        return;
    }

    int ins = *count;
    for (int i = 0; i < *count; i++) {
        if (partial_better(n_total, connected, breaks, break_pieces,
                           track[i].n_total, track[i].n_connected,
                           track[i].n_broken, track[i].n_break_pieces)) {
            ins = i; break;
        }
    }
    if (ins < g_best_output) {
        int new_count = (*count < g_best_output) ? *count + 1 : g_best_output;
        for (int i = new_count - 1; i > ins; i--) track[i] = track[i-1];
        memcpy(&track[ins].board,         cur_board,  sizeof(track[ins].board));
        memcpy(&track[ins].initial_board, init_board, sizeof(track[ins].initial_board));
        track[ins].n_total          = n_total;
        track[ins].initial_n_placed = initial_n_placed;
        track[ins].n_added          = n_added;
        track[ins].n_connected      = connected;
        track[ins].n_broken         = breaks;
        track[ins].n_break_pieces   = break_pieces;
        track[ins].deepest_row      = last_row;
        track[ins].deepest_col      = last_col;
        snprintf(track[ins].record_id, CONFIG_ID_LEN, "%s", config_id_str);
        track[ins].sol_id_num       = sol_id;
        *count = new_count;

        int new_floor = (*count < g_best_output) ? 0 : track[g_best_output-1].n_total;
        atomic_store_explicit(floorp, new_floor, memory_order_relaxed);
    }

    omp_unset_lock(&g_best_lock);
    if (do_ckpt) write_checkpoint_line(&ckpt_copy, config_id_str, sol_id);
}

/* -- v8: mismatch-solution collector ------------------------------------------ */

typedef struct {
    Board board;     /* rotated frame */
    int   breaks;    /* total broken internal edges */
    int   pieces;    /* distinct pieces incident to broken edges */
} MismatchSolution;

/* In mismatch mode, complete leaves are not written immediately.  The solver
 * keeps the top --best-output complete boards at the first budget level that
 * succeeds.  Ranking is exact: fewer broken edges, then fewer break-touched
 * pieces, then deterministic board order for ties. */
typedef struct {
    bool              found;
    MismatchSolution *items;
    int               count;
    int               cap;
    uint64_t          seen;
} MismatchResult;

static int board_lex_compare(const Board *a, const Board *b) {
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            const Oriented *oa = &a->cell[r][c];
            const Oriented *ob = &b->cell[r][c];
            if (oa->piece_id != ob->piece_id) return (int)oa->piece_id - (int)ob->piece_id;
            if (oa->rotation != ob->rotation) return (int)oa->rotation - (int)ob->rotation;
        }
    }
    return 0;
}

static bool mismatch_solution_better_than_item(const Board *a_board,
                                               int a_breaks, int a_pieces,
                                               const MismatchSolution *b) {
    if (a_breaks != b->breaks) return a_breaks < b->breaks;
    if (a_pieces != b->pieces) return a_pieces < b->pieces;
    return board_lex_compare(a_board, &b->board) < 0;
}

static void mres_init(MismatchResult *m) {
    if (!m->items) {
        m->cap = (g_best_output > 0) ? g_best_output : 1;
        m->items = xmalloc((size_t)m->cap * sizeof(MismatchSolution));
    }
    m->found = false;
    m->count = 0;
    m->seen  = 0;
}

static void mres_free(MismatchResult *m) {
    free(m->items);
    m->items = NULL;
    m->count = m->cap = 0;
    m->found = false;
    m->seen = 0;
}

static void mres_insert_unlocked(MismatchResult *m, const Board *b,
                                   int breaks, int pieces) {
    m->found = true;

    int ins = m->count;
    for (int i = 0; i < m->count; i++) {
        if (mismatch_solution_better_than_item(b, breaks, pieces, &m->items[i])) {
            ins = i;
            break;
        }
    }
    if (ins >= m->cap) return;

    int new_count = (m->count < m->cap) ? m->count + 1 : m->cap;
    for (int i = new_count - 1; i > ins; i--) m->items[i] = m->items[i-1];
    m->items[ins].board  = *b;
    m->items[ins].breaks = breaks;
    m->items[ins].pieces = pieces;
    m->count = new_count;
}

static void mres_add(MismatchResult *m, const Board *b, int breaks, int pieces) {
    m->seen++;
    mres_insert_unlocked(m, b, breaks, pieces);
}

static void mres_merge(MismatchResult *dst, const MismatchResult *src) {
    dst->seen += src->seen;
    for (int i = 0; i < src->count; i++)
        mres_insert_unlocked(dst, &src->items[i].board,
                             src->items[i].breaks, src->items[i].pieces);
}

/*
 * Candidate-aware MRV support.
 *
 * v6's MRV used only a cheap proxy: number of already-fixed sides.  That was
 * better than row-major, but it still selected cells with hundreds of legal
 * rotations in mismatch-any mode.  v8 keeps v7 candidate-aware MRV: it counts the actual candidate placements
 * that fit the current board and remaining break budget, then chooses the cell
 * with the fewest viable continuations.  This is the main reason --break-mode
 * any becomes usable for small mismatch budgets.
 */
#define MAX_CANDIDATES (NUM_PIECES * 4)

static int fixed_side_score(const Board *b, int r, int c) {
    int score = 0;
    if (c == 0 || b->cell[r][c-1].piece_id != EMPTY_PIECE) score++;
    if (c == PUZZLE_SIDE-1 || b->cell[r][c+1].piece_id != EMPTY_PIECE) score++;
    if (r == 0 || b->cell[r-1][c].piece_id != EMPTY_PIECE) score++;
    if (r == PUZZLE_SIDE-1 || b->cell[r+1][c].piece_id != EMPTY_PIECE) score++;
    return score;
}

static int candidate_cmp(const void *pa, const void *pb) {
    const Candidate *a = (const Candidate *)pa;
    const Candidate *b = (const Candidate *)pb;
    if (a->breaks != b->breaks) return (int)a->breaks - (int)b->breaks;
    if (a->pid    != b->pid)    return (int)a->pid    - (int)b->pid;
    return (int)a->spin - (int)b->spin;
}

/* Count frame-legal BREAK placements (1..budget) at a cell.  The cell-base
 * bitset already removes wrong piece types, wrong border rotations and used
 * pieces, so mismatch modes no longer rescan type lists and reject three of
 * four edge-piece rotations. */
static int count_break_candidates_scan(const Board *b, const FcState *fc,
                                       int row, int col, int budget) {
    (void)b;
    int n = 0;
    for (int wi = 0; wi < OMASK_WORDS; wi++) {
        uint64_t w = g_cellbase[row][col].w[wi] & fc->unused4.w[wi];
        while (w) {
            int bit = wi * 64 + __builtin_ctzll(w);
            w &= w - 1;
            int pid = bit >> 2, spin = bit & 3;
            int br = piece_break_count(b, row, col, &g_oriented[pid][spin]);
            if (br >= 1 && br <= budget) n++;
        }
    }
    return n;
}

/* Exact MRV in strict mode.  In BREAK_ANY mode the break candidates are part
 * of the branching factor and must be counted too; v9 ignored them whenever
 * one exact fit existed, which could select a nominally 1-way cell having
 * hundreds of actual children. */
static int mrv_cell_count(const Board *b, const FcState *fc, int r, int c, int budget) {
    int ex = fc_exact_count(fc, r, c);
    if (budget <= 0) return ex;
    if (g_break_mode == BREAK_ANY)
        return ex + count_break_candidates_scan(b, fc, r, c, budget);
    if (ex > 0) return ex;
    return count_break_candidates_scan(b, fc, r, c, budget);
}

/*
 * Enumerate candidate placements at (row,col), best-first: exact fits from the
 * FC index (breaks==0, ascending pid/spin - identical to v8's sorted order),
 * then, when a mode wants them, break placements from the classic scan sorted
 * by (breaks,pid,spin).  Break candidates are gathered when the budget allows
 * AND (the cell has no exact fit, or want_breaks_if_exact - i.e. any-mode, or
 * lds-mode with discrepancies left).
 */
static int collect_candidates(const Board *b, const FcState *fc,
                              int row, int col, int remaining_budget,
                              bool want_breaks_if_exact,
                              Candidate *out, int out_cap, int *exact_count,
                              DfsStats *st, int depth) {
    int n = 0, exact = 0;

    for (int wi = 0; wi < OMASK_WORDS; wi++) {
        uint64_t w = fc->nbmask[row][col].w[wi] & fc->unused4.w[wi];
        while (w) {
            int bit = wi * 64 + __builtin_ctzll(w);
            w &= w - 1;
            if (n < out_cap) {
                out[n].pid    = (uint16_t)(bit >> 2);
                out[n].spin   = (uint8_t)(bit & 3);
                out[n].breaks = 0;
                n++;
            }
            exact++;
        }
    }

#ifdef VERIFY_INDEX
    {   /* cross-check the index-enumerated exact fits against a fresh scan */
        int chk = 0;
        int deg = cell_frame_degree(row, col);
        for (int ti = 0; ti < g_pieces_by_zero_n[deg]; ti++) {
            int pid = g_pieces_by_zero[deg][ti];
            if (used_test(b->used, pid)) continue;
            for (int spin = 0; spin < 4; spin++) {
                if ((g_piece_spin_mask[pid] & (1u << spin)) == 0) continue;
                Oriented o = make_oriented(pid, spin);
                if (piece_break_count(b, row, col, &o) == 0) {
                    bool found = false;
                    for (int i = 0; i < n && out[i].breaks == 0; i++)
                        if (out[i].pid == pid && out[i].spin == spin) { found = true; break; }
                    if (!found)
                        fatal("VERIFY_INDEX: scan fit (pid=%d,spin=%d) at (r=%d,c=%d) missing from index",
                              pid, spin, row, col);
                    chk++;
                }
            }
        }
        if (chk != exact)
            fatal("VERIFY_INDEX: (r=%d,c=%d) index has %d exact fits, scan %d",
                  row, col, exact, chk);
    }
#endif

    if (remaining_budget > 0 && (exact == 0 || want_breaks_if_exact)) {
        int first_break = n;
        for (int wi = 0; wi < OMASK_WORDS; wi++) {
            uint64_t w = g_cellbase[row][col].w[wi] & fc->unused4.w[wi];
            while (w) {
                int bit = wi * 64 + __builtin_ctzll(w);
                w &= w - 1;
                int pid = bit >> 2, spin = bit & 3;
                if (st) { st->piece_tests[depth]++; st->rotation_tests[depth]++; }
                int br = piece_break_count(b, row, col, &g_oriented[pid][spin]);
                if (br < 1 || br > remaining_budget) continue;
                if (n < out_cap) {
                    out[n].pid = (uint16_t)pid;
                    out[n].spin = (uint8_t)spin;
                    out[n].breaks = (uint8_t)br;
                    n++;
                }
            }
        }
        if (n - first_break > 1)
            qsort(out + first_break, (size_t)(n - first_break), sizeof(out[0]),
                  candidate_cmp);
    }

    if (exact_count) *exact_count = exact;
    return n;
}

/* -- Dynamic cell choice ----------------------------------------------------- *
 * MRV modes choose dynamically.  Every other order, including the literal
 * 2sides/4sides sweeps built above, consumes its precomputed sequence exactly
 * as written. */

/* Choose which remaining cell (index in rem[pos..n_rem-1]) to process next. */
static int pick_next_cell(const Board *b, const FcState *fc,
                          const Cell *rem, int n_rem, int pos,
                          OrderMode mode, int remaining_budget) {
    if (mode != ORD_MRV) return pos;

    int best = pos;
    int best_eff = INT_MAX;
    int best_fixed = -1;
    long best_key = LONG_MAX;

    for (int j = pos; j < n_rem; j++) {
        int r = (int)rem[j].row, c = (int)rem[j].col;
        int effective = mrv_cell_count(b, fc, r, c, remaining_budget);
        int fixed = fixed_side_score(b, r, c);
        /* MRV tie-break axis: row-major by default, column-major with --reverse
         * (this folds in the former --order mrv-colmajor). */
        long key = g_reverse ? (long)c * PUZZLE_SIDE + r
                             : (long)r * PUZZLE_SIDE + c;

        if (effective < best_eff ||
            (effective == best_eff && fixed > best_fixed) ||
            (effective == best_eff && fixed == best_fixed && key < best_key)) {
            best = j;
            best_eff = effective;
            best_fixed = fixed;
            best_key = key;
            if (best_eff == 0 && best_fixed == 4) break;
        }
    }
    return best;
}

static inline int placed_neighbor_count(const Board *b, int r, int c) {
    int n = 0;
    if (r+1 < PUZZLE_SIDE && b->cell[r+1][c].piece_id != EMPTY_PIECE) n++;
    if (c+1 < PUZZLE_SIDE && b->cell[r][c+1].piece_id != EMPTY_PIECE) n++;
    if (r > 0             && b->cell[r-1][c].piece_id != EMPTY_PIECE) n++;
    if (c > 0             && b->cell[r][c-1].piece_id != EMPTY_PIECE) n++;
    return n;
}

static inline bool hall_should_run(const FcState *fc, const Cell *cells,
                                   int placed_depth, int n_cells) {
    switch (g_hall_mode) {
        case HALL_ALWAYS:   return true;
        case HALL_ADAPTIVE:
            if (n_cells <= g_hall_small) return true;
            if (g_hall_stride <= 0 || placed_depth % g_hall_stride != 0) return false;
            {
                int tight = 0;
                for (int i = 0; i < n_cells; i++) {
                    int n = fc_exact_count(fc, cells[i].row, cells[i].col);
                    if (n <= HALL_TIGHT_DOMAIN && ++tight >= HALL_TIGHT_CELLS)
                        return true;
                }
            }
            return false;
        case HALL_ROOT:
        case HALL_OFF:
        default:            return false;
    }
}

/* -- Greedy dive (stuck mode) -------------------------------------------------- *
 *
 * One descent, no backtracking, no recursion: pick a cell, place a piece, never
 * reconsider.  It is deliberately kept out of tail_dfs() - it shares no control
 * flow with the exhaustive search, and keeping the two apart means a change here
 * can never perturb the proof engines.
 *
 * WHY IT ALWAYS COMPLETES.  g_cellbase[r][c] restricts a cell to pieces of its
 * required frame type, and the type counts are exactly balanced (4 corners, 56
 * edges, 196 inner).  Every placement consumes one piece of the cell's own type,
 * so the number of unused pieces of type T always equals the number of unfilled
 * cells needing T.  The candidate set at the chosen cell is therefore never
 * empty - at worst every option breaks edges.  A dive is O(cells) and cannot
 * fail, which is what makes restarts affordable in bulk.
 *
 * NONE of the completion prunes (zero-domain bound, colour/parity accounting,
 * Hall deficiency) are called.  They exist to prove a branch cannot complete,
 * and here every branch completes by construction; running them would be pure
 * overhead and could only reject a board we intend to keep.
 *
 * Cell choice defers breaking as long as possible: prefer cells that still have
 * an exact fit, most-constrained first.  Only when EVERY remaining cell is stuck
 * do we break, at the cell with the fewest break candidates - the narrowest
 * commitment available.  This ordering matters more than anything else here; a
 * static order lands on stuck cells roughly three times as often.
 *
 * Divergence between dives comes entirely from random tie-breaking - both in the
 * cell choice and in the piece choice.  That is measurably enough on its own: a
 * 200,000-dive batch produced 200,000 distinct boards.  An earlier version also
 * injected voluntary breaks at cells that had an exact fit, on the theory that
 * ties alone might be too rare; measurement showed diversity was already total
 * and the extra breaks only made the boards worse, so it was removed.
 */
#define DIVE_BREAK_CAP 4   /* a cell has at most 4 neighbours, so no placement can
                            * break more than 4 edges: this is "unlimited" here */

static void greedy_dive(Board *b, FcState *fc, const Cell *cells, int n_cells,
                        RNG *rng) {
    Cell rem[MAX_SEQ_LEN];
    memcpy(rem, cells, (size_t)n_cells * sizeof(Cell));

    /* Reservoir sampling over ties: every candidate cell tying the current best
     * key replaces the incumbent with probability 1/seen, giving a uniform pick
     * in one pass and no scratch array. */
    for (int pos = 0; pos < n_cells; pos++) {
        int sel = -1, best_ex = INT_MAX, ties = 0;

        for (int j = pos; j < n_cells; j++) {
            int ex = fc_exact_count(fc, rem[j].row, rem[j].col);
            if (ex == 0) continue;                    /* stuck: consider only if nothing else */
            if (ex < best_ex) { best_ex = ex; sel = j; ties = 1; }
            else if (ex == best_ex && rng_uniform(rng, (uint32_t)++ties) == 0) sel = j;
        }

        if (sel < 0) {
            /* Every remaining cell is stuck.  Break where the commitment is
             * narrowest; the scan is expensive, so it runs only in this case. */
            int best_n = INT_MAX;
            ties = 0;
            for (int j = pos; j < n_cells; j++) {
                int n = count_break_candidates_scan(b, fc, rem[j].row, rem[j].col,
                                                    DIVE_BREAK_CAP);
                if (n <= 0) continue;
                if (n < best_n) { best_n = n; sel = j; ties = 1; }
                else if (n == best_n && rng_uniform(rng, (uint32_t)++ties) == 0) sel = j;
            }
            if (sel < 0) sel = pos;   /* unreachable by the type-balance argument */
        }

        if (sel != pos) { Cell t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }
        int row = rem[pos].row, col = rem[pos].col;

        /* The cell selector only lands on a stuck cell when every remaining cell
         * is stuck, so a nonzero budget here is requested exactly when breaking
         * is forced.  A zero budget keeps collect_candidates() on its cheap
         * exact-fit path. */
        const bool must_break = (fc_exact_count(fc, row, col) == 0);

        Candidate cand[MAX_CANDIDATES];
        int exact_count = 0;
        int n_cand = collect_candidates(b, fc, row, col,
                                        must_break ? DIVE_BREAK_CAP : 0,
                                        false, cand, MAX_CANDIDATES,
                                        &exact_count, NULL, 0);

        /* collect_candidates() returns exact fits first, then break candidates
         * sorted ascending by break count.  Choose uniformly inside the best
         * class: the exact fits when there are any, otherwise the minimal-break
         * group. */
        int lo = 0, hi = 0;
        if (!must_break && exact_count > 0) {
            lo = 0; hi = exact_count;
        } else {
            lo = exact_count;                       /* first break candidate */
            if (lo >= n_cand) { lo = 0; hi = n_cand; }   /* nothing broke: take what we have */
            else {
                int min_br = cand[lo].breaks;
                hi = lo;
                while (hi < n_cand && cand[hi].breaks == min_br) hi++;
            }
        }
        if (hi <= lo) { lo = 0; hi = n_cand; }
        if (hi <= lo) return;    /* type balance says this cannot happen */

        int pick = lo + (int)rng_uniform(rng, (uint32_t)(hi - lo));
        dfs_place(b, fc, row, col, (int)cand[pick].pid, (int)cand[pick].spin);
    }
}

/* Cheap order-independent fingerprint of a finished board, used only to count how
 * many DISTINCT boards a batch of dives produced.  Collisions would understate
 * diversity slightly; at 64 bits over a few million dives that is negligible. */
static uint64_t dive_fingerprint(const Board *b) {
    uint64_t h = 1469598103934665603ULL;          /* FNV-1a offset basis */
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            h ^= (uint64_t)(uint16_t)b->cell[r][c].piece_id;
            h *= 1099511628211ULL;
            h ^= (uint64_t)b->cell[r][c].rotation;
            h *= 1099511628211ULL;
        }
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* Summary of one record's batch of dives, reported on the [dive] line. */
typedef struct {
    long long dives;        /* dives actually run (may be < requested on timeout) */
    long long distinct;     /* distinct finished boards - the diversity check */
    int  best_breaks;
    int  median_breaks;
    int  worst_breaks;
    double seconds;
} DiveStats;

/*
 * Run --stuck_restarts independent greedy dives and keep the best boards.
 *
 * Parallelism here is the safe kind: dives share nothing.  Each thread owns its
 * Board, FcState, RNG and MismatchResult, and the per-thread results are merged
 * once at the end.  Nothing is written to rstat or the global top-N tracks from
 * inside the loop - try_update_best() takes the process-wide g_best_lock, and
 * calling it a million times would serialize the whole run.  Only the winning
 * board goes through it, from the caller.
 */
static void run_greedy_restarts(const Board *base, const Cell *seq, int n_seq,
                                uint64_t rec_index, double deadline,
                                MismatchResult *mres, DiveStats *ds) {
    const long long n_want = g_stuck_restarts > 0 ? g_stuck_restarts : 1;
    double t_start = omp_get_wtime();

    uint64_t *fp = xmalloc((size_t)n_want * sizeof(uint64_t));
    int      *br = xmalloc((size_t)n_want * sizeof(int));

    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    if ((long long)nt > n_want) nt = (int)n_want;

    #pragma omp parallel num_threads(nt)
    {
        Board b;
        FcState fc;
        MismatchResult tmres = {0};
        mres_init(&tmres);

        #pragma omp for schedule(static)
        for (long long i = 0; i < n_want; i++) {
            /* One shared absolute deadline; a dive is sub-millisecond, so
             * checking once per dive is fine-grained enough. */
            if (deadline > 0.0 && omp_get_wtime() > deadline) { fp[i] = 0; br[i] = -1; continue; }

            RNG rng = rng_for(g_rng_master, rec_index, (uint64_t)i);
            b = *base;
            fc_init(&fc, &b);
            greedy_dive(&b, &fc, seq, n_seq, &rng);

            /* The type-balance argument says a dive cannot fail to fill every
             * cell.  Check it rather than trust it: the cost is trivial next to
             * the dive, and a silent short board would corrupt the statistics
             * and be written out as if it were a completion. */
            if (board_count_placed(&b) != NUM_PIECES)
                fatal("internal error: greedy dive %lld left %d/%d cells empty",
                      i, NUM_PIECES - board_count_placed(&b), NUM_PIECES);

            int breaks = 0, pieces = 0;
            board_break_stats(&b, &breaks, &pieces);
            fp[i] = dive_fingerprint(&b);
            br[i] = breaks;
            mres_add(&tmres, &b, breaks, pieces);
        }

        #pragma omp critical(dive_mres_merge)
        { mres_merge(mres, &tmres); }
        mres_free(&tmres);
    }

    /* Compact out any dives skipped by the deadline, then summarize. */
    long long n = 0;
    for (long long i = 0; i < n_want; i++)
        if (br[i] >= 0) { br[n] = br[i]; fp[n] = fp[i]; n++; }

    ds->dives = n;
    ds->seconds = omp_get_wtime() - t_start;
    if (n > 0) {
        qsort(br, (size_t)n, sizeof(int), cmp_int_asc);
        ds->best_breaks   = br[0];
        ds->median_breaks = br[n / 2];
        ds->worst_breaks  = br[n - 1];
        qsort(fp, (size_t)n, sizeof(uint64_t), cmp_u64);
        long long uniq = 1;
        for (long long i = 1; i < n; i++) if (fp[i] != fp[i-1]) uniq++;
        ds->distinct = uniq;
    } else {
        ds->best_breaks = ds->median_breaks = ds->worst_breaks = -1;
        ds->distinct = 0;
    }
    free(fp); free(br);
}

/* -- Recursive DFS ------------------------------------------------------------ */

static _Thread_local uint64_t g_tls_nodes = 0;  /* per-thread node counter for time checks */

/*
 * Dedicated exact side-growth DFS.
 *
 * This is intentionally separate from tail_dfs(): 2sides/4sides are partial-
 * construction experiments, not completion-feasibility searches.  They use the
 * fixed geometric sequence produced by build_side_sequence(), accept only exact
 * placements, never invoke FC zero-domain, colour/parity, or Hall completion
 * prunes, and defer a selected cell when its exact domain is empty.  Keeping the
 * implementation separate prevents a future change to the ordinary solver from
 * silently reintroducing the four-piece hard-prune failure.
 */
static void side_dfs(Board *b, FcState *fc, Cell *seq, int n_seq, int pos,
                     int base_depth, int connected_so_far, int added_so_far,
                     const SearchCtx *cx, uint64_t *local_solutions,
                     DfsStats *st, MismatchResult *mres) {
    if (!order_is_side_growth(cx->mode))
        fatal("internal error: dedicated side DFS called for order %s",
              order_name(cx->mode));
    if (cx->k_budget != 0 || cx->input_breaks != 0)
        fatal("internal error: side growth requires an exact zero-break board");
    if (search_stop_requested(cx->live)) return;
    if (((++g_tls_nodes) & (NODE_TICK_BATCH - 1ULL)) == 0) {
        search_node_tick(cx);
        if (search_stop_requested(cx->live)) return;
    }

    int depth = base_depth + pos;
    if (depth > MAX_SEQ_LEN) fatal("side DFS depth exceeds MAX_SEQ_LEN=%d", MAX_SEQ_LEN);

    if (cx->rstat) {
        int upper = cx->initial_n_placed + added_so_far + (n_seq - pos);
        int best_snapshot = atomic_load_explicit(&cx->rstat->best_total,
                                                 memory_order_relaxed);
        if (upper < best_snapshot) return;
    }

    st->visits[depth]++;
    if (depth > st->max_depth_entered) st->max_depth_entered = depth;

    if (pos == n_seq) {
        if (g_stop_active ? band_is_complete(b)
                          : board_count_placed(b) == NUM_PIECES) {
            if (!claim_complete_solution(cx->live)) return;
            st->completed_leaves++;
            (*local_solutions)++;
            write_solution(b, cx->config_id, cx->sol_id);
            if (cx->collect) {
                int br = 0, pcs = 0;
                board_break_stats(b, &br, &pcs);
                mres_add(mres, b, br, pcs);
            }
        }
        return;
    }

    const int row = (int)seq[pos].row;
    const int col = (int)seq[pos].col;
    Candidate cand[MAX_CANDIDATES];
    int exact_count = 0;
    int n_cand = collect_candidates(b, fc, row, col, 0, false,
                                    cand, MAX_CANDIDATES, &exact_count,
                                    st, depth);
    if (n_cand != exact_count)
        fatal("internal error: side search generated mismatch candidates");

    uint64_t children = 0;
    for (int i = 0; i < n_cand && !search_stop_requested(cx->live); i++) {
        if (cand[i].breaks != 0)
            fatal("internal error: nonzero-break candidate in side search");

        const int adj = placed_neighbor_count(b, row, col);
        const int new_connected = connected_so_far + adj;
        dfs_place(b, fc, row, col, (int)cand[i].pid, (int)cand[i].spin);
        st->fit_ok[depth]++;
        if (depth + 1 > st->max_placed_attempted)
            st->max_placed_attempted = depth + 1;

        const int new_added = added_so_far + 1;
        const int n_total = cx->initial_n_placed + new_added;
        try_update_best(b, cx->initial_board, cx->initial_n_placed,
                        n_total, new_added, new_connected, 0,
                        row, col, cx->config_id, cx->sol_id, cx->rstat, true);

        st->recursed[depth]++;
        children++;
        side_dfs(b, fc, seq, n_seq, pos + 1, base_depth,
                 new_connected, new_added, cx,
                 local_solutions, st, mres);
        dfs_unplace(b, fc, row, col);
    }

    if (children == 0 && !search_stop_requested(cx->live)) {
        /* The selected geometric target is blocked.  It is deliberately left
         * empty while the sweep proceeds to the next row/side target. */
        stats_note_dead(st, depth, row, col);
        side_dfs(b, fc, seq, n_seq, pos + 1, base_depth,
                 connected_so_far, added_so_far, cx,
                 local_solutions, st, mres);
    }
}

/*
 * Generic tail DFS for every order except 2sides/4sides.  In --jump mode, a
 * selected cell with no legal candidate is deferred and search continues in
 * independent gaps.  The dedicated side_dfs() above handles the two geometric
 * side-growth experiments.  FcState mirrors the board through
 * dfs_place/dfs_unplace.
 */
static void tail_dfs(Board *b, FcState *fc, Cell *rem, int n_rem, int pos,
                     int base_depth, int breaks_so_far, int connected_so_far,
                     int added_so_far, int disc_used, const SearchCtx *cx,
                     uint64_t *local_solutions, DfsStats *st,
                     MismatchResult *mres) {
    if (order_is_side_growth(cx->mode))
        fatal("internal error: side-growth order entered generic DFS");
    const bool soft_completion = g_jump;
    if (search_stop_requested(cx->live)) return;
    if (((++g_tls_nodes) & (NODE_TICK_BATCH - 1ULL)) == 0) {
        search_node_tick(cx);
        if (search_stop_requested(cx->live)) return;
    }

    int depth = base_depth + pos;
    if (depth > MAX_SEQ_LEN) fatal("DFS depth exceeds MAX_SEQ_LEN=%d", MAX_SEQ_LEN);

    if (cx->rstat) {
        int upper = cx->initial_n_placed + added_so_far + (n_rem - pos);
        int best_snapshot = atomic_load_explicit(&cx->rstat->best_total, memory_order_relaxed);
        if (upper < best_snapshot) return;
    }

    st->visits[depth]++;
    if (depth > st->max_depth_entered) st->max_depth_entered = depth;

    if (pos == n_rem) {
        if (g_stop_active ? band_is_complete(b)
                          : board_count_placed(b) == NUM_PIECES) {
            if (!claim_complete_solution(cx->live)) return;
            st->completed_leaves++;
            (*local_solutions)++;
            write_solution(b, cx->config_id, cx->sol_id);
            if (cx->collect) {
                int br = 0, pcs = 0;
                board_break_stats(b, &br, &pcs);
                mres_add(mres, b, br, pcs);
            }
        }
        return;
    }

    int remaining_budget = cx->k_budget - breaks_so_far;
    if (remaining_budget < 0) return;

    int sel = pick_next_cell(b, fc, rem, n_rem, pos, cx->mode, remaining_budget);
    if (sel < 0) {
        stats_note_dead(st, depth, -1, -1);
        return;
    }
    if (sel != pos) { Cell t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }

    int row = (int)rem[pos].row;
    int col = (int)rem[pos].col;
    Candidate cand[MAX_CANDIDATES];
    int exact_count = 0;
    bool want_breaks_if_exact =
        (g_break_mode == BREAK_ANY) ||
        (g_break_mode == BREAK_LDS && disc_used < cx->lds_D);
    int n_cand = collect_candidates(b, fc, row, col, remaining_budget,
                                    want_breaks_if_exact,
                                    cand, MAX_CANDIDATES, &exact_count, st, depth);

    int required_break = -1;
    if (g_break_mode == BREAK_STUCK) {
        if (exact_count > 0) {
            required_break = 0;
        } else {
            for (int i = 0; i < n_cand; i++) {
                if (cand[i].breaks > 0) { required_break = cand[i].breaks; break; }
            }
        }
    }

    uint64_t children = 0;
    for (int i = 0; i < n_cand && !search_stop_requested(cx->live); i++) {
        if ((int)cand[i].breaks > remaining_budget) continue;
        int new_disc = disc_used;
        if (g_break_mode == BREAK_STUCK) {
            if ((int)cand[i].breaks != required_break) continue;
        } else if (g_break_mode == BREAK_LDS &&
                   cand[i].breaks > 0 && exact_count > 0) {
            if (disc_used >= cx->lds_D) break;
            new_disc = disc_used + 1;
        }

        int adj = placed_neighbor_count(b, row, col);
        int new_breaks = breaks_so_far + (int)cand[i].breaks;
        int new_connected = connected_so_far + adj - (int)cand[i].breaks;
        dfs_place(b, fc, row, col, (int)cand[i].pid, (int)cand[i].spin);
        st->fit_ok[depth]++;
        if (depth + 1 > st->max_placed_attempted) st->max_placed_attempted = depth + 1;

        int new_added = added_so_far + 1;
        int n_total = cx->initial_n_placed + new_added;
        bool viable = true;

        /* In classic mode a saved partial must itself survive every sound
         * completion prune.  v9 saved the child before these tests, so the
         * output could contain a placement already proved impossible. */
        if (!soft_completion) {
            if (fc->zero_domains > cx->k_budget - new_breaks) {
                st->fc_rejects[depth]++;
                viable = false;
            }
            if (viable) {
                FeasIssue issue;
                if (!remaining_feasibility_ex(b, fc, &issue,
                                              cx->k_budget - new_breaks)) {
                    st->parity_rejects[depth]++;
                    viable = false;
                }
            }
            int n_left = n_rem - pos - 1;
            if (viable && hall_should_run(fc, rem + pos + 1, depth + 1, n_left)) {
                st->hall_calls[depth]++;
                if (hall_prune(fc, rem + pos + 1, n_left,
                               cx->k_budget - new_breaks, NULL)) {
                    st->hall_rejects[depth]++;
                    viable = false;
                }
            }
        }

        if (viable || soft_completion) {
            try_update_best(b, cx->initial_board, cx->initial_n_placed,
                            n_total, new_added, new_connected, new_breaks,
                            row, col, cx->config_id, cx->sol_id, cx->rstat,
                            cx->k_budget == 0);
            st->recursed[depth]++;
            children++;
            tail_dfs(b, fc, rem, n_rem, pos + 1, base_depth,
                     new_breaks, new_connected, new_added, new_disc,
                     cx, local_solutions, st, mres);
        }
        dfs_unplace(b, fc, row, col);
    }

    if (children == 0 && !search_stop_requested(cx->live)) {
        stats_note_dead(st, depth, row, col);
        if (soft_completion) {
            tail_dfs(b, fc, rem, n_rem, pos + 1, base_depth,
                     breaks_so_far, connected_so_far, added_so_far, disc_used,
                     cx, local_solutions, st, mres);
        }
    }

    if (sel != pos) { Cell t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }
}

/* -- Split-frontier search-parallel DFS --------------------------------------- */

typedef struct {
    Board b;
    Cell  rem[MAX_SEQ_LEN];
    int   n_rem;
    int   pos;
    int   base_depth;
    int   breaks;
    int   connected;
    int   added;
    int   disc;    /* E555: lds discrepancies spent along the prefix */
} AfoWorkItem;

typedef struct {
    AfoWorkItem *items;
    int count;
    int cap;
    int target;
    int max_pos;
} AfoFrontier;

static void afo_frontier_init(AfoFrontier *fr, int target) {
    fr->items = NULL;
    fr->count = 0;
    fr->cap = 0;
    fr->target = target > 0 ? target : 1;
    fr->max_pos = 0;
}

static void afo_frontier_free(AfoFrontier *fr) {
    free(fr->items);
    fr->items = NULL;
    fr->count = fr->cap = fr->target = fr->max_pos = 0;
}

static void afo_frontier_emit(AfoFrontier *fr, const Board *b, const Cell *rem,
                              int n_rem, int pos, int base_depth,
                              int breaks, int connected, int added, int disc) {
    if (fr->count == fr->cap) {
        int new_cap = fr->cap ? fr->cap * 2 : 256;
        fr->items = xrealloc(fr->items, (size_t)new_cap * sizeof(AfoWorkItem));
        fr->cap = new_cap;
    }
    AfoWorkItem *it = &fr->items[fr->count++];
    it->b = *b;
    if (n_rem > 0) memcpy(it->rem, rem, (size_t)n_rem * sizeof(Cell));
    it->n_rem = n_rem;
    it->pos = pos;
    it->base_depth = base_depth;
    it->breaks = breaks;
    it->connected = connected;
    it->added = added;
    it->disc = disc;
    if (pos > fr->max_pos) fr->max_pos = pos;
}

/* Dedicated serial prefix expansion for 2sides/4sides.  Like side_dfs(), this
 * contains no completion-feasibility pruning.  Empty exact domains are emitted
 * as deferred steps, so frontier creation itself cannot reproduce the old
 * zero-task/four-piece failure. */
static void side_build_frontier(Board *b, FcState *fc, Cell *seq, int n_seq,
                                int pos, int base_depth, int connected_so_far,
                                int added_so_far, int emit_pos,
                                const SearchCtx *cx,
                                uint64_t *local_solutions, DfsStats *st,
                                MismatchResult *mres, AfoFrontier *fr) {
    if (!order_is_side_growth(cx->mode))
        fatal("internal error: side frontier called for order %s",
              order_name(cx->mode));
    if (cx->k_budget != 0 || cx->input_breaks != 0)
        fatal("internal error: side frontier requires an exact zero-break board");
    if (search_stop_requested(cx->live)) return;
    if (((++g_tls_nodes) & (NODE_TICK_BATCH - 1ULL)) == 0) {
        search_node_tick(cx);
        if (search_stop_requested(cx->live)) return;
    }

    if (pos > 0 && (pos >= emit_pos || fr->count >= fr->target)) {
        afo_frontier_emit(fr, b, seq, n_seq, pos, base_depth,
                          0, connected_so_far, added_so_far, 0);
        return;
    }

    int depth = base_depth + pos;
    if (depth > MAX_SEQ_LEN)
        fatal("side frontier depth exceeds MAX_SEQ_LEN=%d", MAX_SEQ_LEN);

    if (cx->rstat) {
        int upper = cx->initial_n_placed + added_so_far + (n_seq - pos);
        int best_snapshot = atomic_load_explicit(&cx->rstat->best_total,
                                                 memory_order_relaxed);
        if (upper < best_snapshot) return;
    }

    st->visits[depth]++;
    if (depth > st->max_depth_entered) st->max_depth_entered = depth;

    if (pos == n_seq) {
        if (g_stop_active ? band_is_complete(b)
                          : board_count_placed(b) == NUM_PIECES) {
            if (!claim_complete_solution(cx->live)) return;
            st->completed_leaves++;
            (*local_solutions)++;
            write_solution(b, cx->config_id, cx->sol_id);
            if (cx->collect) {
                int br = 0, pcs = 0;
                board_break_stats(b, &br, &pcs);
                mres_add(mres, b, br, pcs);
            }
        }
        return;
    }

    const int row = (int)seq[pos].row;
    const int col = (int)seq[pos].col;
    Candidate cand[MAX_CANDIDATES];
    int exact_count = 0;
    int n_cand = collect_candidates(b, fc, row, col, 0, false,
                                    cand, MAX_CANDIDATES, &exact_count,
                                    st, depth);
    if (n_cand != exact_count)
        fatal("internal error: side frontier generated mismatch candidates");

    uint64_t children = 0;
    for (int i = 0; i < n_cand && !search_stop_requested(cx->live); i++) {
        if (cand[i].breaks != 0)
            fatal("internal error: nonzero-break candidate in side frontier");

        const int adj = placed_neighbor_count(b, row, col);
        const int new_connected = connected_so_far + adj;
        dfs_place(b, fc, row, col, (int)cand[i].pid, (int)cand[i].spin);
        st->fit_ok[depth]++;
        if (depth + 1 > st->max_placed_attempted)
            st->max_placed_attempted = depth + 1;

        const int new_added = added_so_far + 1;
        const int n_total = cx->initial_n_placed + new_added;
        try_update_best(b, cx->initial_board, cx->initial_n_placed,
                        n_total, new_added, new_connected, 0,
                        row, col, cx->config_id, cx->sol_id, cx->rstat, true);

        st->recursed[depth]++;
        children++;
        side_build_frontier(b, fc, seq, n_seq, pos + 1, base_depth,
                            new_connected, new_added, emit_pos, cx,
                            local_solutions, st, mres, fr);
        dfs_unplace(b, fc, row, col);
    }

    if (children == 0 && !search_stop_requested(cx->live)) {
        stats_note_dead(st, depth, row, col);
        side_build_frontier(b, fc, seq, n_seq, pos + 1, base_depth,
                            connected_so_far, added_so_far, emit_pos, cx,
                            local_solutions, st, mres, fr);
    }
}

/*
 * Serial prefix expansion used only by search-parallel mode.  It mirrors tail_dfs()
 * for MRV selection, mismatch policy, feasibility accounting, and skip-on-zero-
 * candidate behavior.  A frontier item stores rem[] and pos so workers resume
 * exactly the same subtree; skipped cells are not reconstructed from the Board.
 * emit_pos is the depth at which subtrees are handed to the frontier (E555: a
 * parameter instead of the fixed SPLIT_DEPTH, so the adaptive refinement
 * loop can expand an existing item exactly one more level).
 */
static void afo_build_frontier(Board *b, FcState *fc, Cell *rem, int n_rem, int pos,
                               int base_depth, int breaks_so_far, int connected_so_far,
                               int added_so_far, int disc_used, int emit_pos,
                               const SearchCtx *cx, uint64_t *local_solutions,
                               DfsStats *st, MismatchResult *mres, AfoFrontier *fr) {
    if (order_is_side_growth(cx->mode))
        fatal("internal error: side-growth order entered generic frontier builder");
    const bool soft_completion = g_jump;
    if (search_stop_requested(cx->live)) return;
    if (((++g_tls_nodes) & (NODE_TICK_BATCH - 1ULL)) == 0) {
        search_node_tick(cx);
        if (search_stop_requested(cx->live)) return;
    }

    if (pos > 0 && (pos >= emit_pos || fr->count >= fr->target)) {
        afo_frontier_emit(fr, b, rem, n_rem, pos, base_depth,
                          breaks_so_far, connected_so_far, added_so_far, disc_used);
        return;
    }

    int depth = base_depth + pos;
    if (depth > MAX_SEQ_LEN) fatal("DFS depth exceeds MAX_SEQ_LEN=%d", MAX_SEQ_LEN);

    if (cx->rstat) {
        int upper = cx->initial_n_placed + added_so_far + (n_rem - pos);
        int best_snapshot = atomic_load_explicit(&cx->rstat->best_total, memory_order_relaxed);
        if (upper < best_snapshot) return;
    }

    st->visits[depth]++;
    if (depth > st->max_depth_entered) st->max_depth_entered = depth;

    if (pos == n_rem) {
        if (g_stop_active ? band_is_complete(b)
                          : board_count_placed(b) == NUM_PIECES) {
            if (!claim_complete_solution(cx->live)) return;
            st->completed_leaves++;
            (*local_solutions)++;
            write_solution(b, cx->config_id, cx->sol_id);
            if (cx->collect) {
                int br = 0, pcs = 0;
                board_break_stats(b, &br, &pcs);
                mres_add(mres, b, br, pcs);
            }
        }
        return;
    }

    int remaining_budget = cx->k_budget - breaks_so_far;
    if (remaining_budget < 0) return;

    int sel = pick_next_cell(b, fc, rem, n_rem, pos, cx->mode, remaining_budget);
    if (sel < 0) {
        stats_note_dead(st, depth, -1, -1);
        return;
    }
    if (sel != pos) { Cell t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }

    int row = (int)rem[pos].row;
    int col = (int)rem[pos].col;
    Candidate cand[MAX_CANDIDATES];
    int exact_count = 0;
    bool want_breaks_if_exact =
        (g_break_mode == BREAK_ANY) ||
        (g_break_mode == BREAK_LDS && disc_used < cx->lds_D);
    int n_cand = collect_candidates(b, fc, row, col, remaining_budget,
                                    want_breaks_if_exact,
                                    cand, MAX_CANDIDATES, &exact_count, st, depth);

    int required_break = -1;
    if (g_break_mode == BREAK_STUCK) {
        if (exact_count > 0) {
            required_break = 0;
        } else {
            for (int i = 0; i < n_cand; i++) {
                if (cand[i].breaks > 0) { required_break = cand[i].breaks; break; }
            }
        }
    }

    uint64_t children = 0;
    for (int i = 0; i < n_cand && !search_stop_requested(cx->live); i++) {
        if ((int)cand[i].breaks > remaining_budget) continue;
        int new_disc = disc_used;
        if (g_break_mode == BREAK_STUCK) {
            if ((int)cand[i].breaks != required_break) continue;
        } else if (g_break_mode == BREAK_LDS &&
                   cand[i].breaks > 0 && exact_count > 0) {
            if (disc_used >= cx->lds_D) break;
            new_disc = disc_used + 1;
        }

        int adj = placed_neighbor_count(b, row, col);
        int new_breaks = breaks_so_far + (int)cand[i].breaks;
        int new_connected = connected_so_far + adj - (int)cand[i].breaks;
        dfs_place(b, fc, row, col, (int)cand[i].pid, (int)cand[i].spin);
        st->fit_ok[depth]++;
        if (depth + 1 > st->max_placed_attempted) st->max_placed_attempted = depth + 1;

        int new_added = added_so_far + 1;
        int n_total = cx->initial_n_placed + new_added;
        bool viable = true;
        if (!soft_completion) {
            if (fc->zero_domains > cx->k_budget - new_breaks) {
                st->fc_rejects[depth]++;
                viable = false;
            }
            if (viable) {
                FeasIssue issue;
                if (!remaining_feasibility_ex(b, fc, &issue,
                                              cx->k_budget - new_breaks)) {
                    st->parity_rejects[depth]++;
                    viable = false;
                }
            }
            int n_left = n_rem - pos - 1;
            if (viable && hall_should_run(fc, rem + pos + 1, depth + 1, n_left)) {
                st->hall_calls[depth]++;
                if (hall_prune(fc, rem + pos + 1, n_left,
                               cx->k_budget - new_breaks, NULL)) {
                    st->hall_rejects[depth]++;
                    viable = false;
                }
            }
        }

        if (viable || soft_completion) {
            try_update_best(b, cx->initial_board, cx->initial_n_placed,
                            n_total, new_added, new_connected, new_breaks,
                            row, col, cx->config_id, cx->sol_id, cx->rstat,
                            cx->k_budget == 0);
            st->recursed[depth]++;
            children++;
            afo_build_frontier(b, fc, rem, n_rem, pos + 1,
                               base_depth, new_breaks, new_connected, new_added,
                               new_disc, emit_pos, cx, local_solutions, st, mres, fr);
        }
        dfs_unplace(b, fc, row, col);
    }

    if (children == 0 && !search_stop_requested(cx->live)) {
        stats_note_dead(st, depth, row, col);
        if (soft_completion) {
            afo_build_frontier(b, fc, rem, n_rem, pos + 1,
                               base_depth, breaks_so_far, connected_so_far,
                               added_so_far, disc_used, emit_pos,
                               cx, local_solutions, st, mres, fr);
        }
    }

    if (sel != pos) { Cell t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }
}

static void run_search_at_k_search_parallel(Board *base, const SearchCtx *cx,
                                        uint64_t *local_solutions,
                                        DfsStats *stats, MismatchResult *mres) {
    const bool side_search = order_is_side_growth(cx->mode);
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    if (nt <= 1) {
        Cell rem[MAX_SEQ_LEN]; int n_rem = 0;
        build_search_sequence_ordered(base, rem, &n_rem, cx->mode);
        FcState fc;
        fc_init(&fc, base);
        if (side_search) {
            side_dfs(base, &fc, rem, n_rem, 0, 0,
                     cx->initial_connected, 0, cx,
                     local_solutions, stats, mres);
        } else {
            tail_dfs(base, &fc, rem, n_rem, 0, 0, cx->input_breaks,
                     cx->initial_connected, 0, 0, cx,
                     local_solutions, stats, mres);
        }
        return;
    }

    Cell rem[MAX_SEQ_LEN]; int n_rem = 0;
    build_search_sequence_ordered(base, rem, &n_rem, cx->mode);

    /* Overdecomposition.  Subtree sizes in a backtracking search differ by orders
     * of magnitude, so with only a few tasks per worker one oversized task decides
     * the wall time while everyone else waits at the barrier.  Building many small
     * tasks instead lets schedule(dynamic,1) even the load out on its own.  This
     * changes only the granularity of the work, never which subtrees are visited -
     * the exhaustive modes stay exhaustive. */
    long long target_ll = (long long)nt * (long long)SPLIT_TASKS_PER_THREAD;
    if (target_ll < nt) target_ll = nt;
    if (target_ll > INT_MAX) target_ll = INT_MAX;

    AfoFrontier fr;
    afo_frontier_init(&fr, (int)target_ll);

    /* Initial expansion to SPLIT_DEPTH (the v8 behaviour). */
    uint64_t prefix_solutions = 0;
    {
        Board prefix = *base;
        FcState pfc;
        fc_init(&pfc, &prefix);
        if (side_search) {
            side_build_frontier(&prefix, &pfc, rem, n_rem, 0, 0,
                                cx->initial_connected, 0,
                                SPLIT_DEPTH, cx, &prefix_solutions,
                                stats, mres, &fr);
        } else {
            afo_build_frontier(&prefix, &pfc, rem, n_rem, 0, 0,
                               cx->input_breaks, cx->initial_connected, 0, 0,
                               SPLIT_DEPTH, cx, &prefix_solutions,
                               stats, mres, &fr);
        }
    }

    /* v9 adaptive refinement: with low branching near the root a fixed split
     * depth yields far fewer tasks than threads and starves large machines.
     * While below the task target, consume the SHALLOWEST frontier item and
     * expand it exactly one more level.  Each subtree is expanded at most once
     * (items are consumed), so no node is re-walked and no solution or best-
     * partial update is ever emitted twice. */
    int refine_depth = fr.max_pos;
    {
        int depth_cap = n_rem - 1;
        long long max_exp_ll = 16LL * (long long)fr.target;
        int max_expansions = (max_exp_ll > INT_MAX) ? INT_MAX : (int)max_exp_ll;
        int expansions = 0;
        while (fr.count > 0 && fr.count < fr.target &&
               expansions < max_expansions && !search_stop_requested(cx->live)) {
            int pick = -1;
            for (int i = 0; i < fr.count; i++) {
                const AfoWorkItem *it = &fr.items[i];
                if (it->pos >= it->n_rem || it->pos >= depth_cap) continue;
                if (pick < 0 || it->pos < fr.items[pick].pos) pick = i;
            }
            if (pick < 0) break;                      /* nothing refinable left */
            AfoWorkItem item = fr.items[pick];        /* consume the item */
            fr.items[pick] = fr.items[--fr.count];
            FcState ifc;
            fc_init(&ifc, &item.b);
            if (side_search) {
                side_build_frontier(&item.b, &ifc, item.rem, item.n_rem,
                                    item.pos, item.base_depth, item.connected,
                                    item.added, item.pos + 1, cx,
                                    &prefix_solutions, stats, mres, &fr);
            } else {
                afo_build_frontier(&item.b, &ifc, item.rem, item.n_rem,
                                   item.pos, item.base_depth, item.breaks,
                                   item.connected, item.added, item.disc,
                                   item.pos + 1, cx, &prefix_solutions,
                                   stats, mres, &fr);
            }
            expansions++;
            if (item.pos + 1 > refine_depth) refine_depth = item.pos + 1;
        }
    }
    *local_solutions += prefix_solutions;

    if (g_verbose || fr.count < nt) {
        #pragma omp critical(stdout_print)
        {
            printf("[cfg=%s sol=%lld] search_parallel k=%d frontier_tasks=%d target=%d "
                   "split_depth=%d refined_to=%d\n",
                   cx->config_id, cx->sol_id, cx->k_budget, fr.count, fr.target,
                   SPLIT_DEPTH, refine_depth);
            fflush(stdout);
        }
    }

    if (fr.count == 0 || search_stop_requested(cx->live)) {
        afo_frontier_free(&fr);
        return;
    }

    uint64_t worker_solutions = 0;
    #pragma omp parallel num_threads(nt) reduction(+:worker_solutions)
    {
        DfsStats thread_stats;
        stats_init(&thread_stats);

        /* Complete mismatch boards are collected per worker and merged after
         * the dynamic work loop.  Workers never mutate the shared MismatchResult
         * during DFS, so search-parallel mode has no collector data race. */
        MismatchResult thread_mres = {0};
        if (cx->collect) mres_init(&thread_mres);

        #pragma omp for schedule(dynamic,1)
        for (int wi = 0; wi < fr.count; wi++) {
            if (search_stop_requested(cx->live)) continue;
            const AfoWorkItem *it = &fr.items[wi];
            Board b = it->b;
            FcState wfc;
            fc_init(&wfc, &b);
            Cell local_rem[MAX_SEQ_LEN];
            if (it->n_rem > 0) memcpy(local_rem, it->rem, (size_t)it->n_rem * sizeof(Cell));
            uint64_t ls = 0;
            DfsStats lst;
            stats_init(&lst);
            if (side_search) {
                side_dfs(&b, &wfc, local_rem, it->n_rem, it->pos,
                         it->base_depth, it->connected, it->added, cx,
                         &ls, &lst, cx->collect ? &thread_mres : mres);
            } else {
                tail_dfs(&b, &wfc, local_rem, it->n_rem, it->pos,
                         it->base_depth, it->breaks, it->connected,
                         it->added, it->disc, cx,
                         &ls, &lst, cx->collect ? &thread_mres : mres);
            }
            worker_solutions += ls;
            stats_merge(&thread_stats, &lst);
        }

        #pragma omp critical(afo_stats_merge)
        { stats_merge(stats, &thread_stats); }

        if (cx->collect) {
            #pragma omp critical(afo_mres_merge)
            { mres_merge(mres, &thread_mres); }
            mres_free(&thread_mres);
        }
    }

    *local_solutions += worker_solutions;
    afo_frontier_free(&fr);
}

/* -- Search dispatch for one budget level k ----------------------------------- */

static void run_search_at_k(Board *base, const SearchCtx *cx,
                            uint64_t *local_solutions, DfsStats *stats,
                            MismatchResult *mres) {
    /* E555 makes the side-growth contract an executable invariant, not merely
     * an option convention.  These modes must never enter mismatch search or
     * any completion-only Hall/FC/color pruning path. */
    if (order_is_side_growth(cx->mode)) {
        if (g_jump || g_hall_mode != HALL_OFF || cx->k_budget != 0 ||
            cx->input_breaks != 0 || g_max_mismatch != 0)
            fatal("internal error: %s side-growth invariants violated",
                  order_name(cx->mode));
    }

    if (g_search_parallel) {
        run_search_at_k_search_parallel(base, cx, local_solutions, stats, mres);
        return;
    }

    Cell rem[MAX_SEQ_LEN]; int n_rem = 0;
    build_search_sequence_ordered(base, rem, &n_rem, cx->mode);
    FcState fc;
    fc_init(&fc, base);
    if (order_is_side_growth(cx->mode)) {
        side_dfs(base, &fc, rem, n_rem, 0, 0,
                 cx->initial_connected, 0, cx,
                 local_solutions, stats, mres);
    } else {
        tail_dfs(base, &fc, rem, n_rem, 0, 0, cx->input_breaks,
                 cx->initial_connected, 0, 0, cx,
                 local_solutions, stats, mres);
    }
}

/* -- Per-record processing ---------------------------------------------------- */

static void process_line(const char *config_id_str, long long sol_id,
                         const int pos[NUM_PIECES], const int rot[NUM_PIECES]) {
    double t0 = omp_get_wtime();

    uint64_t line_num;
    #pragma omp atomic capture
    line_num = ++g_records_processed;

    char rec_buf[REC_BUF_BYTES];
    size_t rec_off = 0;

    /* -- Reconstruct board from CSV (rotation applied), then holes (in the
     *    rotated frame, per user specification) -- */
    Board base;
    int n_placed = 0;
    int holes_applied = 0;
    char why[512]; why[0] = '\0';

    if (!build_initial_board(pos, rot, &base, &n_placed, &holes_applied, why, sizeof(why))) {
        #pragma omp atomic
        g_cnt_invalid++;
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[%4" PRIu64 "/%d] cfg=%s sol=%lld  INVALID: %s\n",
                line_num, g_total_records, config_id_str, sol_id, why);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); }
        DfsStats st0; stats_init(&st0);
        FeasIssue fi0; memset(&fi0,0,sizeof(fi0)); fi0.code=FEAS_OK;
        write_status_csv(config_id_str, sol_id, false, "invalid",
                                n_placed, 0, 0, omp_get_wtime()-t0, 0,
                                &fi0, &st0, NULL, 0, n_placed, 0, 0,
                                -1, -1, 0);
        return;
    }

    /* -- Save initial board (after holes, in rotated frame) for verbose display -- */
    Board initial_board = base;
    int initial_n_placed = n_placed;

    /* Edges already present in the input partial (broken==0 for exact/beam
     * input; may be >0 when re-reading mismatch output or imperfect Stage-B
     * partials). */
    int input_breaks = 0, input_connected = 0;
    board_edge_counts(&base, &input_connected, &input_breaks);

    /* -- Validate resulting partial board.  Input internal mismatches are
     *    ALWAYS accepted (only the gray-0 frame rule and the placed-count are
     *    strict); a warning is emitted below so they are never silent. -- */
    if (!validate_partial_board_ex(&base, n_placed, 480, why, sizeof(why))) {
        #pragma omp atomic
        g_cnt_invalid++;
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[%4" PRIu64 "/%d] cfg=%s sol=%lld  INVALID (post-holes): %s\n",
                line_num, g_total_records, config_id_str, sol_id, why);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); }
        DfsStats st0; stats_init(&st0);
        FeasIssue fi0; memset(&fi0,0,sizeof(fi0)); fi0.code=FEAS_OK;
        write_status_csv(config_id_str, sol_id, false, "invalid",
                                n_placed, 0, holes_applied, omp_get_wtime()-t0, 0,
                                &fi0, &st0, NULL, 0, n_placed, input_connected, input_breaks,
                                -1, -1, 0);
        return;
    }

    /* -- Drop records whose input already exceeds the TOTAL break budget --
     * --max-mismatch is the absolute ceiling on broken edges in the finished
     * board; input breaks count against it.  If the input alone already has more
     * breaks than allowed, no completion can satisfy the budget, so skip it
     * without searching (its own status/counter, never a silent no_solution). */
    if (input_breaks > g_max_mismatch) {
        #pragma omp atomic
        g_cnt_dropped++;
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] DROPPED: input has %d broken edge(s) > "
                "--max-mismatch %d (total budget); not searched.\n",
                config_id_str, sol_id, input_breaks, g_max_mismatch);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); }
        write_stream_best(&base, config_id_str, sol_id);
        DfsStats st0; stats_init(&st0);
        FeasIssue fi0; memset(&fi0,0,sizeof(fi0)); fi0.code=FEAS_OK;
        write_status_csv(config_id_str, sol_id, true, "dropped",
                                n_placed, NUM_PIECES - n_placed, holes_applied,
                                omp_get_wtime()-t0, 0, &fi0, &st0, NULL, 0,
                                n_placed, input_connected, input_breaks,
                                -1, -1, 0);
        return;
    }

    /* -- Warn if the input partial already carries broken edges -- */
    if (input_breaks > 0) {
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] WARNING: input partial has %d broken edge(s) "
                "(connected=%d); accepted as sunk, counted toward the total budget.\n",
                config_id_str, sol_id, input_breaks, input_connected);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); rec_off = 0; rec_buf[0] = '\0'; }
    }

    /* -- Build search sequence (reporting copy, in the chosen static order) -- */
    Cell seq[MAX_SEQ_LEN];
    int  n_seq = 0;
    build_search_sequence_ordered(&base, seq, &n_seq, g_order_mode);
    const bool soft_completion = order_uses_soft_completion(g_order_mode);

    if (g_verbose && order_is_side_growth(g_order_mode)) {
        #pragma omp critical(stdout_print)
        { print_side_sequence_preview(seq, n_seq); fflush(stdout); }
    }

    /* -- Initial-board display is explicit: --verbose only.  v9 printed one
     * large board even in quiet mode, which obscured batch diagnostics. -- */
    if (g_verbose) {
        char title[200];
        snprintf(title, sizeof(title),
                 "=== INITIAL BOARD  cfg=%s sol=%lld  placed=%d/%d  empty=%d  holes=%d%s ===",
                 config_id_str, sol_id, n_placed, NUM_PIECES, n_seq, holes_applied,
                 g_rotation > 0 ? " (rotated frame)" : "");
        #pragma omp critical(stdout_print)
        { print_board_ascii(stdout, &initial_board, title); fflush(stdout); }
    }

    int  total_budget = g_max_mismatch;     /* absolute ceiling; input breaks count in */
    bool collect = (g_max_mismatch > 0 || input_breaks > 0);

    /* stuck + a nonzero budget selects the greedy dive engine.  A zero budget is
     * an exact search, which is independent of break mode and must keep the full
     * backtracking DFS. */
    const bool greedy_mode = (g_break_mode == BREAK_STUCK && g_max_mismatch > 0);

    /* Root FC state supplies both exact domains and the incrementally maintained
     * colour/type demand used by every later node. */
    FcState root_fc;
    fc_init(&root_fc, &base);
    FeasIssue init_issue; memset(&init_issue,0,sizeof(init_issue)); init_issue.code=FEAS_OK;
    bool initial_feasible = remaining_feasibility_ex(&base, &root_fc, &init_issue,
                                                     total_budget - input_breaks);
    if (initial_feasible && root_fc.zero_domains > total_budget - input_breaks) {
        memset(&init_issue, 0, sizeof(init_issue));
        init_issue.code = FEAS_EMPTY_DOMAIN;
        init_issue.required = root_fc.zero_domains;
        init_issue.available = total_budget - input_breaks;
        init_issue.remainder = init_issue.available - init_issue.required;
        initial_feasible = false;
    }

    if (!initial_feasible) {
        char desc[256]; describe_feas(&init_issue, desc, sizeof(desc));
        #pragma omp atomic
        g_cnt_infeasible++;
        #pragma omp critical(feas_cnt_update)
        { if (init_issue.code < 5) g_feas_code_cnt[(int)init_issue.code]++; }

        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] completion-infeasible within the requested budget%s: "
                "placed=%d/%d empty=%d holes=%d reason=%s\n",
                config_id_str, sol_id,
                soft_completion ? ", continuing exact best-partial growth" : "",
                n_placed, NUM_PIECES, n_seq, holes_applied, desc);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); rec_off = 0; rec_buf[0] = '\0'; }
    }

    /* -- Search: iterative deepening over the TOTAL broken-edge budget --
     * Levels run k = input_breaks .. g_max_mismatch; the search stops at the
     * first level that completes the board, so the broken-edge count is minimal.
     * `collect` is true whenever the board may carry breaks (g_max_mismatch>0, or
     * the input already has breaks); then complete boards are gathered for the
     * summary.  When false (pure input, g_max_mismatch==0) the strict path
     * enumerates solutions directly.
     *
     * All levels share the one per-record deadline.  A timed-out lower level may
     * still widen to a higher mismatch budget only while that same deadline has
     * time remaining. */
    /* E555 interprets --time-limit as one total per-record budget, not one fresh
     * window per mismatch level.  Unset means unlimited for exact search and the
     * conservative default only for mismatch search. */
    double eff_limit = g_time_limit_sec;
    if (eff_limit < 0.0) eff_limit = (g_max_mismatch > 0) ? TIME_LIMIT_DEFAULT : 0.0;
    double record_deadline = (eff_limit > 0.0) ? (t0 + eff_limit) : 0.0;

    /* Per-record running best, seeded with the input board's own edge stats and
     * board, so a record whose search adds nothing streams back its input line. */
    int input_break_pieces = 0;
    if (input_breaks > 0) board_break_stats(&base, NULL, &input_break_pieces);
    Board rstat_best = base;
    RecStat rstat;
    atomic_init(&rstat.best_total, initial_n_placed);
    rstat.best_connected = input_connected;
    rstat.best_broken = input_breaks;
    rstat.best_break_pieces = input_break_pieces;
    rstat.deepest_row = -1;
    rstat.deepest_col = -1;
    rstat.best_board = &rstat_best;
    rstat.ckpt_next  = omp_get_wtime() + CKPT_MIN_INTERVAL_SEC;
    rstat.ckpt_dirty = false;

    /* E555: Hall/deficiency lower bound at the record root.  root_deficiency
     * additional broken edges are provably forced (see hall_prune), so budget
     * levels below input_breaks + root_deficiency cannot complete: start the
     * iterative deepening there instead of proving each hopeless level empty.
     * Classic mode only; --jump ignores completion-death by design. */
    int root_deficiency = 0;
    if (initial_feasible && !soft_completion &&
        g_hall_mode != HALL_OFF && n_seq > 0)
        hall_prune(&root_fc, seq, n_seq, INT_MAX, &root_deficiency);
    int k_start = input_breaks + root_deficiency;
    bool hall_impossible = (!soft_completion && k_start > total_budget);
    if (root_deficiency > 0) {
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] root Hall deficiency=%d: any completion needs "
                ">= %d additional broken edge(s); first admissible total k=%d%s.\n",
                config_id_str, sol_id, root_deficiency, root_deficiency, k_start,
                hall_impossible ? " (outside the requested budget)" : "");
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); rec_off = 0; rec_buf[0] = '\0'; }
    }

    /* In classic mode these are proofs about every completion.  Do not spend a
     * DFS level manufacturing a dead "best partial" as v9 did; stream the input
     * board unchanged and report the proof. */
    if (!soft_completion && (!initial_feasible || hall_impossible)) {
        if (hall_impossible) {
            #pragma omp atomic
            g_cnt_hall_infeasible++;
        }
        #pragma omp atomic
        g_cnt_no_solution++;
        write_stream_best(&base, config_id_str, sol_id);
        double elapsed0 = omp_get_wtime() - t0;
        DfsStats st0; stats_init(&st0);
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] ROOT-INFEASIBLE: no DFS run; best remains %d/%d "
                "connected=%d broken=%d t=%.6fs  no_solution\n",
                config_id_str, sol_id, n_placed, NUM_PIECES,
                input_connected, input_breaks, elapsed0);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); }
        write_status_csv(config_id_str, sol_id, true, "no_solution",
                                n_placed, n_seq, holes_applied, elapsed0, 0,
                                &init_issue, &st0, seq, n_seq,
                                n_placed, input_connected, input_breaks,
                                -1, -1, root_deficiency);
        return;
    }

    uint64_t local_solutions = 0;
    DfsStats stats, total_stats;
    stats_init(&stats);
    stats_init(&total_stats);
    SearchLive live; search_live_init(&live);
    MismatchResult mres = {0}; mres_init(&mres);
    int  solved_k = -1;
    bool timed_out = false;

    /* -- stuck mode: greedy dives instead of the deepening ladder -------------- *
     * Every dive completes, so there is no budget to deepen through and no proof
     * to construct.  Run the batch, keep the best boards, and fall through to the
     * shared reporting path with mres populated exactly as a mismatch level would
     * have left it. */
    if (greedy_mode) {
        DiveStats ds;
        memset(&ds, 0, sizeof(ds));
        run_greedy_restarts(&base, seq, n_seq, line_num,
                            record_deadline, &mres, &ds);
        local_solutions = (uint64_t)ds.dives;
        solved_k = mres.found ? mres.items[0].breaks : -1;
        #pragma omp atomic
        g_total_solutions += (uint64_t)ds.dives;   /* every dive is a completion */

        /* Feed only the winner to the shared best-partial machinery: one
         * g_best_lock acquisition per record instead of one per dive. */
        if (mres.found) {
            const Board *w = &mres.items[0].board;
            int conn = 0, brk = 0;
            board_edge_counts(w, &conn, &brk);
            try_update_best(w, &initial_board, initial_n_placed,
                            NUM_PIECES, NUM_PIECES - initial_n_placed,
                            conn, brk, -1, -1,
                            config_id_str, sol_id, &rstat, false);
        }

        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] [dive] %lld dives in %.2fs (%.0f/s)  "
                "breaks best=%d median=%d worst=%d  distinct_boards=%lld\n",
                config_id_str, sol_id, ds.dives, ds.seconds,
                ds.seconds > 0.0 ? (double)ds.dives / ds.seconds : 0.0,
                ds.best_breaks, ds.median_breaks, ds.worst_breaks, ds.distinct);
        #pragma omp critical(stdout_print)
        { fputs(rec_buf, stdout); fflush(stdout); rec_off = 0; rec_buf[0] = '\0'; }

        if (record_deadline > 0.0 && omp_get_wtime() > record_deadline)
            search_request_stop(&live, STOP_TIMEOUT);
    }

    for (int k = greedy_mode ? total_budget + 1 : k_start; k <= total_budget; k++) {
        stats_init(&stats);
        search_stop_init(&live);
        mres_init(&mres);
        uint64_t lsol = 0;

        double level_deadline = record_deadline;

        /* lds: iterate the voluntary-break allowance D inside this level's one
         * time window.  D=0 first (~stuck speed); each D+1 widens toward the
         * any-mode tree.  Other modes run a single pass with D unused. */
        int d_hi = 0;
        if (g_break_mode == BREAK_LDS && k > input_breaks) {
            d_hi = k - input_breaks;
            if (g_lds_max >= 0 && g_lds_max < d_hi) d_hi = g_lds_max;
        }

        bool found = false;
        for (int D = 0; D <= d_hi; D++) {
            SearchCtx cx;
            memset(&cx, 0, sizeof(cx));
            cx.config_id        = config_id_str;
            cx.sol_id           = sol_id;
            cx.initial_n_placed = initial_n_placed;
            cx.initial_connected= input_connected;
            cx.initial_board    = &initial_board;
            cx.rstat            = &rstat;
            cx.live             = &live;
            cx.mode             = g_order_mode;
            cx.k_budget         = k;
            cx.input_breaks     = input_breaks;
            cx.lds_D            = D;
            cx.record_start     = t0;
            cx.deadline         = level_deadline;
            cx.collect          = collect;

            run_search_at_k(&base, &cx, &lsol, &stats, &mres);

            found = collect ? mres.found : (lsol > 0);
            if (found || search_stop_requested(&live)) break;
            if (g_break_mode == BREAK_LDS && D < d_hi) {
                unsigned long long nds =
                    atomic_load_explicit(&live.nodes, memory_order_relaxed);
                #pragma omp critical(stdout_print)
                {
                    printf("[cfg=%s sol=%lld] lds k=%d: D=%d exhausted, widening to "
                           "D=%d  t=%.1fs nodes=%.1fM best=%d pieces broken=%d\n",
                           config_id_str, sol_id, k, D, D + 1,
                           omp_get_wtime() - t0, (double)nds / 1e6,
                           atomic_load_explicit(&rstat.best_total, memory_order_relaxed),
                           rstat.best_broken);
                    fflush(stdout);
                }
            }
        }
        local_solutions = lsol;
        stats_merge(&total_stats, &stats);

        if (found) { solved_k = k; timed_out = false; break; }

        /* All mismatch levels share one record deadline.  Once it expires there
         * is no fresh window to spend at a wider k. */
        timed_out = (search_stop_reason(&live) & STOP_TIMEOUT) != 0;
        if (timed_out) break;

        if (k < total_budget) {
            #pragma omp critical(stdout_print)
            {
                printf("[cfg=%s sol=%lld] level k=%d %s  t=%.1fs best=%d pieces "
                       "connected=%d broken=%d -> deepening to k=%d\n",
                       config_id_str, sol_id, k,
                       timed_out ? "timed out" : "exhausted (no completion)",
                       omp_get_wtime() - t0,
                       atomic_load_explicit(&rstat.best_total, memory_order_relaxed),
                       rstat.best_connected, rstat.best_broken, k + 1);
                fflush(stdout);
            }
        }
    }

    stats = total_stats;

    double elapsed = omp_get_wtime() - t0;

    /* Stream this record's best board (a completion when found, else the deepest
     * partial) as one re-feedable line to output.csv. */
    write_stream_best(rstat.best_board, config_id_str, sol_id);

    /* -- Determine status.  In collect mode success is mres.found; the broken-
     * edge count of the reported board is solved_k. -- */
    bool any_solution = collect ? mres.found : (local_solutions > 0);
    int stop_reason = search_stop_reason(&live);
    bool limit_reached = (stop_reason & STOP_SOLUTION_LIMIT) != 0;
    timed_out = (stop_reason & STOP_TIMEOUT) != 0;
    const char *status;
    if (timed_out && !limit_reached) {
        if (any_solution) {
            status = "solution_cutoff";
            #pragma omp atomic
            g_cnt_solution_cutoff++;
        } else {
            status = "cutoff";
            #pragma omp atomic
            g_cnt_cutoff++;
        }
    } else if (any_solution) {
        status = "solution";
        #pragma omp atomic
        g_cnt_solution++;
    } else {
        status = "no_solution";
        #pragma omp atomic
        g_cnt_no_solution++;
    }

    /* -- Depth info: report the record's BEST board (rstat), which persists
     *    across all k-levels (stats.max_depth_entered is reset each level). -- */
    int fd = first_dead_depth(&stats, n_seq);
    char stuck_str[64];
    int deep_row     = rstat.deepest_row;
    int deep_col     = rstat.deepest_col;
    int rstat_best_total = atomic_load_explicit(&rstat.best_total, memory_order_relaxed);
    int n_added_dfs  = rstat_best_total - initial_n_placed;

    if (any_solution)
        snprintf(stuck_str, sizeof(stuck_str), "solved");
    else if (timed_out)
        snprintf(stuck_str, sizeof(stuck_str), "timed_out@%.0fs(best+%d)",
                 eff_limit, n_added_dfs);
    else if (fd >= 0)
        snprintf(stuck_str, sizeof(stuck_str), "stuck@(r=%d,c=%d)",
                 stats.first_dead_row, stats.first_dead_col);
    else
        snprintf(stuck_str, sizeof(stuck_str), "no_dead_recorded");

    /* -- Compact per-record line (with connected/broken edge counts) -- */
    const int total_internal = 2 * PUZZLE_SIDE * (PUZZLE_SIDE - 1);  /* 480 */
    if (strcmp(status, "solution") == 0 || strcmp(status, "solution_cutoff") == 0) {
        int sbroken    = collect ? (mres.found ? mres.items[0].breaks : solved_k) : 0;
        int sconnected = total_internal - sbroken;  /* complete board: all 480 edges exist */
        char brk[96];
        snprintf(brk, sizeof(brk), " connected_edges=%d broken_edges=%d pieces_with_breaks=%d kept=%d",
                 sconnected, sbroken, collect && mres.found ? mres.items[0].pieces : 0,
                 collect ? mres.count : 0);
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] %s found=%" PRIu64
                " placed=%d/%d empty=%d holes=%d"
                " deepest=(r=%d,c=%d) added=%d%s t=%.3fs\n",
                config_id_str, sol_id, status, local_solutions,
                n_placed, NUM_PIECES, n_seq, holes_applied,
                deep_row, deep_col, n_added_dfs, brk, elapsed);
    } else {
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "[cfg=%s sol=%lld] placed=%d/%d empty=%d holes=%d"
                " best=%d connected_edges=%d broken_edges=%d"
                " deepest=(r=%d,c=%d) added=%d %s t=%.3fs  %s\n",
                config_id_str, sol_id, n_placed, NUM_PIECES, n_seq, holes_applied,
                rstat_best_total, rstat.best_connected, rstat.best_broken,
                deep_row, deep_col, n_added_dfs, stuck_str, elapsed, status);
    }

    if (order_is_side_growth(g_order_mode)) {
        int border = 0, left = 0, right = 0, bottom = 0, top = 0;
        board_border_counts(rstat.best_board, &border, &left, &right, &bottom, &top);
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "  SIDE RESULT: border=%d/%d  left=%d/%d  right=%d/%d  "
                "bottom=%d/%d  top=%d/%d  broken=%d\n",
                border, 4 * (PUZZLE_SIDE - 1),
                left, PUZZLE_SIDE, right, PUZZLE_SIDE,
                bottom, PUZZLE_SIDE, top, PUZZLE_SIDE,
                rstat.best_broken);
    }

    /* -- Verbose: DFS stats -- */
    if (g_verbose) {
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "  DFS visits=%" PRIu64 " piece_tests=%" PRIu64
                " type_rejects=%" PRIu64 " fit_ok=%" PRIu64
                " parity_rejects=%" PRIu64 " fc_rejects=%" PRIu64
                " hall_rejects=%" PRIu64 " recursed=%" PRIu64 "\n",
                TOTAL(visits,       &stats, n_seq),
                TOTAL(piece_tests,  &stats, n_seq),
                TOTAL(type_rejects, &stats, n_seq),
                TOTAL(fit_ok,       &stats, n_seq),
                TOTAL(parity_rejects,&stats,n_seq),
                TOTAL(fc_rejects,   &stats, n_seq),
                TOTAL(hall_rejects, &stats, n_seq),
                TOTAL(recursed,     &stats, n_seq));
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "  Fit rejects: zero_rule=%" PRIu64 " left=%" PRIu64
                " bottom=%" PRIu64 " top=%" PRIu64 " right=%" PRIu64 "\n",
                TOTAL(reject_zero,   &stats, n_seq),
                TOTAL(reject_left,   &stats, n_seq),
                TOTAL(reject_bottom, &stats, n_seq),
                TOTAL(reject_top,    &stats, n_seq),
                TOTAL(reject_right,  &stats, n_seq));
        appendf(rec_buf, sizeof(rec_buf), &rec_off,
                "  max_depth_seq=%d/%d  first_dead=%s  deepest=(r=%d,c=%d)  n_added=%d",
                stats.max_depth_entered, n_seq, stuck_str,
                deep_row, deep_col, n_added_dfs);
        if (order_is_side_growth(g_order_mode))
            appendf(rec_buf, sizeof(rec_buf), &rec_off,
                    "  defer_events=%" PRIu64,
                    TOTAL(dead_nodes, &stats, n_seq));
        appendf(rec_buf, sizeof(rec_buf), &rec_off, "\n");
    }

    #pragma omp critical(stdout_print)
    { fputs(rec_buf, stdout); fflush(stdout); }

    write_status_csv(config_id_str, sol_id, true, status,
                            n_placed, n_seq, holes_applied, elapsed,
                            local_solutions, &init_issue, &stats, seq, n_seq,
                            rstat_best_total, rstat.best_connected, rstat.best_broken,
                            deep_row, deep_col, root_deficiency);
    mres_free(&mres);
}

/* -- CSV record store --------------------------------------------------------- */

typedef struct {
    char      config_id[CONFIG_ID_LEN];
    long long sol_id;
    int       pos[NUM_PIECES];
    int       rot[NUM_PIECES];
} CsvRecord;

/* -- E555: post-holes duplicate detection ---------------------------------------- */
/*
 * Many Stage-B partials share a common base and differ only in cells that an
 * aggressive --holes file removes; after the holes they are the SAME initial
 * board, and searching the later copies would exactly repeat the earlier
 * search.  Before processing, every record with row index < proc_end (i.e.
 * rows that will be processed PLUS rows skipped because they lie below --row,
 * which belong to an earlier block of a split run) is hashed on its
 * post-holes initial board.  Records whose board exactly matches an earlier
 * row are marked as duplicates and skipped with a message naming that row.
 * Hash buckets are confirmed by exact cell-by-cell comparison, so a hash
 * collision can never cause a false skip.  First occurrence in file order is
 * canonical.  Invalid records are never deduplicated (they report as invalid).
 */

typedef struct { uint64_t h; int idx; } RecHash;

static int rechash_cmp(const void *pa, const void *pb) {
    const RecHash *a = pa, *b = pb;
    if (a->h != b->h) return (a->h < b->h) ? -1 : 1;
    return a->idx - b->idx;
}

/* Fills dup_of[i] with the earliest row index whose post-holes board equals
 * row i's (or -1).  Rows [0, n_hash) are hashed; duplicates are only marked
 * for rows >= proc_start (earlier rows are other blocks' work). */
static void build_duplicate_map(const CsvRecord *records, int n_hash,
                                int proc_start, int *dup_of) {
    RecHash *rh = xmalloc((size_t)(n_hash > 0 ? n_hash : 1) * sizeof(RecHash));
    int n_valid = 0;
    for (int i = 0; i < n_hash; i++) {
        Board b; int npl = 0, ha = 0; char why[256];
        if (!build_initial_board(records[i].pos, records[i].rot,
                                 &b, &npl, &ha, why, sizeof(why)))
            continue;
        rh[n_valid].h   = board_state_hash(&b);
        rh[n_valid].idx = i;
        n_valid++;
    }
    qsort(rh, (size_t)n_valid, sizeof(RecHash), rechash_cmp);

    int n_dups_in_window = 0;
    for (int s = 0; s < n_valid; ) {
        int e = s;
        while (e < n_valid && rh[e].h == rh[s].h) e++;
        if (e - s > 1) {
            /* Exact confirmation inside the hash bucket: keep one board per
             * distinct representative (usually 1), match later rows against
             * them in file order. */
            int    n_reps  = 0;
            int   *rep_idx = xmalloc((size_t)(e - s) * sizeof(int));
            Board *rep_brd = xmalloc((size_t)(e - s) * sizeof(Board));
            for (int t = s; t < e; t++) {
                Board b; int npl = 0, ha = 0; char why[256];
                if (!build_initial_board(records[rh[t].idx].pos, records[rh[t].idx].rot,
                                         &b, &npl, &ha, why, sizeof(why)))
                    continue;   /* cannot happen: hashed above */
                int match = -1;
                for (int r = 0; r < n_reps; r++)
                    if (board_cells_equal(&b, &rep_brd[r])) { match = r; break; }
                if (match >= 0) {
                    dup_of[rh[t].idx] = rep_idx[match];
                    if (rh[t].idx >= proc_start) n_dups_in_window++;
                } else {
                    rep_idx[n_reps] = rh[t].idx;
                    rep_brd[n_reps] = b;
                    n_reps++;
                }
            }
            free(rep_idx);
            free(rep_brd);
        }
        s = e;
    }
    free(rh);
    printf("[dedup] hashed %d record(s) post-holes (rows 0..%d): "
           "%d duplicate(s) in the processing window will be skipped\n",
           n_valid, n_hash - 1, n_dups_in_window);
    fflush(stdout);
}

/* Report and account one skipped duplicate (thread-safe). */
static void report_duplicate(const CsvRecord *rec, int row,
                             const CsvRecord *orig, int orig_row) {
    #pragma omp atomic
    g_records_processed++;
    #pragma omp atomic
    g_cnt_duplicate++;
    #pragma omp critical(stdout_print)
    {
        printf("[cfg=%s sol=%lld] DUPLICATE: initial board after holes is identical "
               "to row %d (cfg=%s sol=%lld); skipped.\n",
               rec->config_id, rec->sol_id, orig_row, orig->config_id, orig->sol_id);
        fflush(stdout);
    }
    Board b;
    int n_placed = 0, holes_applied = 0;
    int connected = 0, broken = 0;
    char why[256];
    bool valid = build_initial_board(rec->pos, rec->rot, &b, &n_placed,
                                     &holes_applied, why, sizeof(why));
    if (valid) {
        board_edge_counts(&b, &connected, &broken);
        write_stream_best(&b, rec->config_id, rec->sol_id);
    }
    (void)row;
    DfsStats st0; stats_init(&st0);
    FeasIssue fi0; memset(&fi0, 0, sizeof(fi0)); fi0.code = FEAS_OK;
    write_status_csv(rec->config_id, rec->sol_id, valid, "duplicate_unsearched",
                            n_placed, valid ? NUM_PIECES - n_placed : 0,
                            holes_applied, 0.0, 0, &fi0, &st0, NULL, 0,
                            n_placed, connected, broken,
                            -1, -1, 0);
}

/* -- Usage -------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "\nUsage: %s seed.txt completions.csv output.csv [options]\n\n"
            "Arguments:\n"
            "  seed.txt         %d rows of: top right bottom left.\n"
            "  completions.csv  Canonical/Stage-B (514-field) or legacy (515-field) CSV.\n"
            "  output.csv       One re-feedable best-board row per processed record.\n\n",
            prog, NUM_PIECES);
    fputs(
        "Core options:\n"
        "  --version              Print the exact build tag and exit.\n"
        "  --threads N            OpenMP worker count (default: all available).\n"
        "  --all-for-one          Put every thread on one record's search at a time,\n"
        "                         instead of one record per thread.  Chosen\n"
        "                         automatically when there are no more records than\n"
        "                         threads; use this to force it.\n"
        "  --row N                First data-record index (0 based).\n"
        "  --count N              Number of records; default 1 with --row, else all.\n"
        "  --holes PATH           256 position-indexed 0/1 entries; matrix CSV accepted.\n"
        "  --rotate K             Rotate K quarter turns CCW before search (0..3).\n"
        "  --verbose              Print every initial board and detailed statistics.\n"
        "  --best-output N        Also retain global top-N pure/mismatch partials.\n"
        "  --status               Write output.csv.status.csv (off by default).\n"
        "  --dedup                Enable exact post-hole duplicate skipping (default).\n"
        "  --no-dedup             Search every selected input row independently.\n\n",
        stderr);
    fputs(
        "Search order and pruning:\n"
        "  --order MODE           mrv (default), rowmajor, colmajor, snake, spiral,\n"
        "                         centerout, spiralout, 2sides, 4sides.\n"
        "                         mrv picks the most-constrained cell (fail-first);\n"
        "                         static orders land on cells with no exact fit far\n"
        "                         more often once breaks are allowed.\n"
        "                         2sides: exact left/right layers, one cell from each\n"
        "                         side of every row before moving inward.\n"
        "                         4sides: exact left/right/bottom/top ring layers.\n"
        "                         Side modes defer dead cells and optimize a large\n"
        "                         zero-mismatch partial; they require --max-mismatch 0.\n"
        "  --reverse              Reverse the --order traversal: rowmajor right-to-left,\n"
        "                         colmajor top-to-bottom, spiral/centerout/spiralout\n"
        "                         opposite chirality (border-first preserved); mrv uses\n"
        "                         a column-major tie-break instead of row-major.  No\n"
        "                         effect on 2sides/4sides.\n"
        "  --jump                 Best-partial mode: skip a dead cell and continue.\n"
        "  --hall MODE            off, root, adaptive (default), or always.\n"
        "  --no-hall              Alias for --hall off.\n"
        "  --hall-stride N        Adaptive Hall period in placements (default 8; 0=off).\n"
        "  --hall-small N         Adaptive Hall always-on threshold (default 32 cells).\n\n",
        stderr);
    fputs(
        "Mismatch search:\n"
        "  --max-mismatch K       Absolute completed-board broken-edge ceiling (0..480).\n"
        "                         0 = exact search.  In stuck mode every dive completes\n"
        "                         regardless, so K only filters what counts as a result.\n"
        "  --break-mode MODE      stuck (default), any, or lds.\n"
        "                         stuck: greedy dives, no backtracking, always completes.\n"
        "                           Fast triage for ranking partials; proves nothing.\n"
        "                         any/lds: exhaustive, iterative deepening.  An exhausted\n"
        "                           level proves no completion exists at that break count.\n"
        "  --stuck_restarts N     Independent randomized dives per record (default 100000,\n"
        "                         roughly 5-10 s on four cores).\n"
        "                         Dives are sub-millisecond; N in the millions is fine.\n"
        "  --lds-max N            With --break-mode lds, cap voluntary mismatch\n"
        "                         placements per root-to-leaf path.  Forced repairs\n"
        "                         at cells with no exact fit do not consume this count.\n"
        "  --time-limit SEC       Total wall-clock budget per input record; 0=unlimited.\n"
        "                         Unset: unlimited for exact search, 30 s for mismatch.\n\n"
        "  --solution-limit N     Stop after N completions per record (default 1; 0=all).\n"
        "                         Ignored in stuck mode (--stuck_restarts governs).\n"
        "  --all-solutions        Alias for --solution-limit 0.\n\n"
        "Stop band (emit partials for the finalizer):\n"
        "  --stop_row N           Search ONLY rows 0..N and emit every way to fill\n"
        "                         them, as the beamer's --stop_row does.  Cells above\n"
        "                         the band are cleared first (their pieces return to\n"
        "                         the pool) and written unplaced, so each line feeds\n"
        "                         E555_finalizer --finalize_from N directly.\n"
        "  --stop_column N        The same over columns 0..N.\n"
        "                         --reverse anchors either band at the far side\n"
        "                         instead (rows 15-N..15, columns 15-N..15) as well as\n"
        "                         flipping static traversal.  A band completion IS the\n"
        "                         solution here, so --solution-limit caps how many are\n"
        "                         emitted -- it defaults to 1, use --all-solutions to\n"
        "                         enumerate.  Requires --max-mismatch 0 and --jump off.\n\n"
        "Auxiliary output:\n"
        "  output.csv.status.csv          Per-record diagnostics, with --status.\n"
        "  output.csv.checkpoint.csv      Crash recovery; removed on clean completion.\n"
        "  output.csv.stop_row<N>.csv     Every completed band, with --stop_row\n"
        "                                 (stop_col<N> for columns, _rev when reversed).\n"
        "  output.csv.best_pure.csv       With --best-output.\n"
        "  output.csv.best_mismatch.csv   With --best-output.\n\n",
        stderr);
}

/* -- Best-N partial boards CSV (one track) ------------------------------------ */

/*
 * Write one best-partial track.  Data-row layout is intentionally identical to
 * output.csv (the canonical config_id,score,pos[256],rot[256] row) so the file
 * can be fed straight back in.  The rank and per-rank metrics stay in
 * stdout/header comments rather than data columns to preserve that contract.
 */
static void write_partials_track(const char *path, const char *kind,
                                  const BestPartial *track, int count) {
    if (count == 0) return;

    FILE *f = fopen(path, "w");
    if (!f) fatal("cannot create %s: %s", path, strerror(errno));
    fprintf(f, "# E555 backtracker -- best %s partial boards (original frame)\n", kind);
    fprintf(f, "# fields: config_id,score,pos_0,...,pos_255,rot_0,...,rot_255\n");
    fprintf(f, "# per-rank pieces/connected/broken/break_pieces are listed on stdout at write time\n");
    if (ferror(f)) fatal("cannot write header to %s", path);

    printf("\n  Best %s partial boards -> %s\n", kind, path);
    printf("  %-5s  %-22s  %-10s  %-14s  %-10s  %-8s  %-12s  %s\n",
           "rank", "config_id", "sol_id", "pieces(tot/add)", "connected", "broken", "break_pieces", "deepest");

    for (int k = 0; k < count; k++) {
        const BestPartial *bp = &track[k];

        Board orig;
        const Board *out_b;
        if (g_rotation > 0) {
            inverse_rotation_k(&orig, (const Board *)&bp->board, g_rotation);
            out_b = &orig;
        } else {
            out_b = (const Board *)&bp->board;
        }

        int pos[NUM_PIECES], rot_arr[NUM_PIECES];
        board_to_pos_rot(out_b, pos, rot_arr);

        char buf[CSV_BUF_BYTES]; size_t off = 0;
        appendf(buf, sizeof(buf), &off, "%s_%lld,%d",
                bp->record_id, bp->sol_id_num, bp->n_connected);
        for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", pos[i]);
        for (int i = 0; i < NUM_PIECES; i++) appendf(buf, sizeof(buf), &off, ",%d", rot_arr[i]);
        appendf(buf, sizeof(buf), &off, "\n");
        checked_fwrite(buf, 1, off, f, path);

        printf("  %-5d  %-22s  %-10lld  %5d/%-8d  %-10d  %-8d  %-12d  (r=%d,c=%d)\n",
               k + 1, bp->record_id, bp->sol_id_num,
               bp->n_total, bp->n_added, bp->n_connected, bp->n_broken,
               bp->n_break_pieces, bp->deepest_row, bp->deepest_col);
    }

    checked_fclose(f, path);
    printf("  %d %s board(s) written.\n", count, kind);
    fflush(stdout);
}

/* -- Main --------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc == 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("E555 backtracker build %s\n", E555_BUILD_TAG);
        return EXIT_SUCCESS;
    }
    if (argc < 4) { usage(argv[0]); return EXIT_FAILURE; }

    const char *seed_path  = argv[1];
    const char *csv_path   = argv[2];
    const char *out_path   = argv[3];
    const char *holes_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else if (strcmp(argv[i], "--threads") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v <= 0 || v > INT_MAX)
                fatal("--threads expects a positive integer, got '%s'", argv[i]);
            g_nthreads = (int)v;
        } else if (strcmp(argv[i], "--all-for-one") == 0) {
            g_parallel_mode = PAR_SEARCH;
        } else if (strcmp(argv[i], "--reverse") == 0) {
            g_reverse = true;
        } else if (strcmp(argv[i], "--jump") == 0) {
            g_jump = true;
        } else if (strcmp(argv[i], "--stuck_restarts") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long long v = strtoll(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 1)
                fatal("--stuck_restarts expects a positive integer, got '%s'", argv[i]);
            g_stuck_restarts = v;
        } else if (strcmp(argv[i], "--row") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long long v = strtoll(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0)
                fatal("--row expects a non-negative integer (0-based record index), got '%s'", argv[i]);
            g_row_start = v;
        } else if (strcmp(argv[i], "--count") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long long v = strtoll(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 1)
                fatal("--count expects a positive integer, got '%s'", argv[i]);
            g_count = v;
        } else if (strcmp(argv[i], "--holes") == 0 && i+1 < argc) {
            holes_path = argv[++i];
        } else if (strcmp(argv[i], "--best-output") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v <= 0 || v > INT_MAX)
                fatal("--best-output expects a positive integer, got '%s'", argv[i]);
            g_best_output = (int)v;
        } else if (strcmp(argv[i], "--dedup") == 0) {
            g_dedup = true;
        } else if (strcmp(argv[i], "--no-dedup") == 0) {
            g_dedup = false;
        } else if (strcmp(argv[i], "--status") == 0) {
            g_write_status = true;
        } else if (strcmp(argv[i], "--rotate") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > 3)
                fatal("--rotate expects 0, 1, 2, or 3; got '%s'", argv[i]);
            g_rotation = (int)v;
        } else if (strcmp(argv[i], "--stop_row") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > PUZZLE_SIDE - 1)
                fatal("--stop_row expects an integer in [0,%d], got '%s'",
                      PUZZLE_SIDE - 1, argv[i]);
            if (g_stop_active)
                fatal("--stop_row and --stop_column are mutually exclusive");
            g_stop_n = (int)v; g_stop_isrow = true; g_stop_active = true;
        } else if (strcmp(argv[i], "--stop_column") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > PUZZLE_SIDE - 1)
                fatal("--stop_column expects an integer in [0,%d], got '%s'",
                      PUZZLE_SIDE - 1, argv[i]);
            if (g_stop_active)
                fatal("--stop_row and --stop_column are mutually exclusive");
            g_stop_n = (int)v; g_stop_isrow = false; g_stop_active = true;
        } else if (strcmp(argv[i], "--max-mismatch") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            /* 480 = 2*16*15 internal edges on the board (theoretical maximum). */
            if (errno || end == argv[i] || *end || v < 0 || v > 480)
                fatal("--max-mismatch expects an integer in [0,480], got '%s'", argv[i]);
            g_max_mismatch = (int)v;
        } else if (strcmp(argv[i], "--time-limit") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            double v = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end || v < 0.0)
                fatal("--time-limit expects a non-negative number of seconds, got '%s'", argv[i]);
            g_time_limit_sec = v;
        } else if (strcmp(argv[i], "--solution-limit") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            const char *arg = argv[++i];
            if (arg[0] == '-')
                fatal("--solution-limit expects a non-negative integer, got '%s'", arg);
            unsigned long long v = strtoull(arg, &end, 10);
            if (errno || end == arg || *end)
                fatal("--solution-limit expects a non-negative integer, got '%s'", arg);
            g_solution_limit = (uint64_t)v;
        } else if (strcmp(argv[i], "--all-solutions") == 0) {
            g_solution_limit = 0;
        } else if (strcmp(argv[i], "--order") == 0 && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "rowmajor"))  g_order_mode = ORD_ROWMAJOR;
            else if (!strcmp(m, "colmajor"))  g_order_mode = ORD_COLMAJOR;
            else if (!strcmp(m, "snake"))     g_order_mode = ORD_SNAKE;
            else if (!strcmp(m, "spiral"))    g_order_mode = ORD_SPIRAL;
            else if (!strcmp(m, "centerout")) g_order_mode = ORD_CENTEROUT;
            else if (!strcmp(m, "spiralout")) g_order_mode = ORD_SPIRALOUT;
            else if (!strcmp(m, "mrv"))       g_order_mode = ORD_MRV;
            else if (!strcmp(m, "2sides"))    g_order_mode = ORD_2SIDES;
            else if (!strcmp(m, "4sides"))    g_order_mode = ORD_4SIDES;
            else fatal("--order expects mrv|rowmajor|colmajor|snake|spiral|centerout|spiralout|2sides|4sides, got '%s'", m);
        } else if (strcmp(argv[i], "--break-mode") == 0 && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "stuck")) g_break_mode = BREAK_STUCK;
            else if (!strcmp(m, "any"))   g_break_mode = BREAK_ANY;
            else if (!strcmp(m, "lds"))   g_break_mode = BREAK_LDS;
            else fatal("--break-mode expects stuck|any|lds, got '%s'", m);
        } else if (strcmp(argv[i], "--lds-max") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > 480)
                fatal("--lds-max expects an integer in [0,480], got '%s'", argv[i]);
            g_lds_max = (int)v;
        } else if (strcmp(argv[i], "--hall") == 0 && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "off"))      g_hall_mode = HALL_OFF;
            else if (!strcmp(m, "root"))     g_hall_mode = HALL_ROOT;
            else if (!strcmp(m, "adaptive")) g_hall_mode = HALL_ADAPTIVE;
            else if (!strcmp(m, "always"))   g_hall_mode = HALL_ALWAYS;
            else fatal("--hall expects off|root|adaptive|always, got '%s'", m);
        } else if (strcmp(argv[i], "--no-hall") == 0) {
            g_hall_mode = HALL_OFF;
        } else if (strcmp(argv[i], "--hall-stride") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > MAX_SEQ_LEN)
                fatal("--hall-stride expects an integer in [0,%d], got '%s'", MAX_SEQ_LEN, argv[i]);
            g_hall_stride = (int)v;
        } else if (strcmp(argv[i], "--hall-small") == 0 && i+1 < argc) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || v < 0 || v > MAX_SEQ_LEN)
                fatal("--hall-small expects an integer in [0,%d], got '%s'", MAX_SEQ_LEN, argv[i]);
            g_hall_small = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            usage(argv[0]); return EXIT_FAILURE;
        }
    }

    /* A stop band exists to feed the finalizer, which structurally validates
     * every color match inside its locked region -- so a band carrying a broken
     * edge would be rejected there rather than here, silently, one stage later.
     * Requiring exactness up front turns that into an argument error.  --jump is
     * refused for a different reason: it skips a dead cell and keeps going, so
     * the band would never be complete and the run could only ever emit nothing. */
    if (g_stop_active) {
        if (g_max_mismatch != 0)
            fatal("--stop_%s requires --max-mismatch 0: an emitted band must be "
                  "exactly matched or the finalizer will reject it",
                  g_stop_isrow ? "row" : "column");
        if (g_jump)
            fatal("--stop_%s requires --jump off: jumping leaves dead cells empty, "
                  "so the band can never complete", g_stop_isrow ? "row" : "column");
    }

    /* Literal side-growth orders use exact placements only.  They have their
     * own dead-cell deferral semantics, so the generic --jump switch remains
     * off and a mismatch budget is not meaningful. */
    if (g_order_mode == ORD_2SIDES || g_order_mode == ORD_4SIDES) {
        if (g_jump)
            fatal("--order %s requires --jump off", order_name(g_order_mode));
        if (g_max_mismatch != 0)
            fatal("--order %s requires --max-mismatch 0", order_name(g_order_mode));
        /* Side growth maximizes an exact partial.  Hall/color/FC completion
         * proofs are intentionally irrelevant here and must never stop the
         * sweep, even when the caller supplied --hall adaptive/always. */
        g_hall_mode = HALL_OFF;
    }
    if (g_lds_max >= 0 && g_break_mode != BREAK_LDS)
        fprintf(stderr, "[warn] --lds-max has no effect unless --break-mode lds is selected.\n");

    /* mrv is the default order (see g_order_mode).  It is the right default
     * across the board: once breaks are allowed a static order lands on cells
     * with no exact fit ~3x as often as MRV, each costing a full unused-pool
     * scan (measured on a 48-cell tail: 232 M piece tests under spiralout vs
     * 1.6 M under mrv, for a worse board).  A user-supplied --order is used
     * as given. */

    const bool greedy_run = (g_break_mode == BREAK_STUCK && g_max_mismatch > 0);
    if (greedy_run) {
        if (g_jump)
            fprintf(stderr, "[warn] --jump has no effect in stuck mode: a greedy dive "
                            "never leaves a cell empty.\n");
        if (g_solution_limit != 1)
            fprintf(stderr, "[warn] --solution-limit has no effect in stuck mode; "
                            "--stuck_restarts governs how many boards are produced.\n");
    }

    /* Dive randomness is seeded from system noise only - there is deliberately no
     * --rng-seed, so runs are not reproducible by design. */
    {
        struct timespec ts_m, ts_r;
        clock_gettime(CLOCK_MONOTONIC, &ts_m);
        clock_gettime(CLOCK_REALTIME,  &ts_r);
        g_rng_master = splitmix64((uint64_t)ts_m.tv_nsec * 0x9E3779B97F4A7C15ULL
                                ^ (uint64_t)ts_r.tv_sec
                                ^ ((uint64_t)ts_r.tv_nsec << 17)
                                ^ ((uint64_t)getpid() << 32));
    }

    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    g_t_start_wall = omp_get_wtime();
    omp_init_lock(&g_best_lock);
    atomic_init(&g_best_pure_floor, 0);
    atomic_init(&g_best_mm_floor, 0);
    if (g_best_output > 0) {
        g_best_pure = xmalloc((size_t)g_best_output * sizeof(BestPartial));
        g_best_mm   = xmalloc((size_t)g_best_output * sizeof(BestPartial));
    }

    printf("=== E555 backtracker [build %s] ===\n",
           E555_BUILD_TAG);
    printf("  build=%s\n", E555_BUILD_TAG);
    printf("  seed=%s\n  csv=%s\n  out=%s\n", seed_path, csv_path, out_path);
    printf("  threads=%d  verbose=%s\n", nt, g_verbose ? "yes" : "no");
    printf("  board_size=%dx%d  pieces=%d\n", PUZZLE_SIDE, PUZZLE_SIDE, NUM_PIECES);
    printf("  rotate=%d CCW  (output_frame=%s)\n",
           g_rotation * 90, g_rotation > 0 ? "original" : "unchanged");
    if (g_row_start >= 0)
        printf("  start: row=%lld (0-based record index)\n", g_row_start);
    if (holes_path)
        printf("  holes=%s (interpreted in rotated frame)\n", holes_path);
    if (g_best_output > 0) printf("  best_output=%d\n", g_best_output);
    else                   printf("  best_output=off\n");
    printf("  parallel_requested=%s", parallel_mode_name(g_parallel_mode));
    if (g_parallel_mode != PAR_RECORDS)
        printf("  target_frontier=%d (auto)", nt * SPLIT_TASKS_PER_THREAD);
    printf("\n");
    const bool side_growth = order_is_side_growth(g_order_mode);
    printf("  order=%s%s  jump=%s  completion_feasibility=%s\n",
           order_name(g_order_mode), g_reverse ? " (reverse)" : "", g_jump ? "on" : "off",
           g_jump ? "soft-for-partials" :
           (side_growth ? "soft-side-growth" : "hard-prune"));
    if (g_order_mode == ORD_2SIDES) {
        printf("  side_engine=E555 literal static sequence; no MRV; exact DFS; dead cells deferred\n");
        printf("  side_sweep=left/right layers; every row advances one cell per side before the next inset\n");
        printf("  side_policy=exact-only; FC/color/Hall completion prunes are bypassed by invariant\n");
    } else if (g_order_mode == ORD_4SIDES) {
        printf("  side_engine=E555 literal static sequence; no MRV; exact DFS; dead cells deferred\n");
        printf("  side_sweep=left/right/bottom/top ring layers; the exterior ring is processed first\n");
        printf("  side_policy=exact-only; FC/color/Hall completion prunes are bypassed by invariant\n");
    }
    printf("  hall=%s", effective_hall_name());
    if (!order_uses_soft_completion(g_order_mode) && g_hall_mode == HALL_ADAPTIVE)
        printf("  stride=%d  small=%d", g_hall_stride, g_hall_small);
    printf("\n");
    printf("  max_mismatch=%d (total break ceiling)", g_max_mismatch);
    if (g_max_mismatch > 0) {
        printf("  break_mode=%s", break_mode_name(g_break_mode));
        if (g_break_mode == BREAK_LDS) {
            if (g_lds_max >= 0) printf("  lds_max=%d", g_lds_max);
            else                printf("  lds_max=auto(up to break budget)");
        }
    }
    printf("\n");
    if (greedy_run)
        printf("  engine=GREEDY DIVES (no backtracking, always completes)"
               "  restarts=%lld  -- fast triage, proves nothing\n",
               g_stuck_restarts);
    else if (g_max_mismatch > 0)
        printf("  engine=exhaustive iterative deepening -- an exhausted level is a "
               "proof that no completion exists at that break count\n");
    {
        double el = g_time_limit_sec;
        if (el < 0.0) el = (g_max_mismatch > 0) ? TIME_LIMIT_DEFAULT : 0.0;
        printf("  time_limit=");
        if (el > 0.0) printf("%.0fs/record", el); else printf("unlimited");
    }
    printf("\n");
    printf("  solution_limit=");
    if (g_solution_limit > 0) printf("%" PRIu64 " per record\n", g_solution_limit);
    else                      printf("all\n");
    printf("  dedup=%s  status_report=%s\n",
           g_dedup ? "on (default)" : "off",
           g_write_status ? "on" : "off (use --status)");
    fflush(stdout);

    /* [1/5] Load seed */
    printf("\n[1/5] Loading seed...\n"); fflush(stdout);
    load_seed(seed_path);
    build_oriented_cache();
    build_spiralout_table();
    build_fit_index();

    /* [2/5] Load holes */
    if (holes_path) {
        printf("[2/5] Loading holes file...\n"); fflush(stdout);
        char why[256];
        memset(g_holes, 0, sizeof(g_holes));
        if (!load_holes(holes_path, g_holes, why, sizeof(why)))
            fatal("holes file error: %s", why);
        int nholes = 0;
        for (int i = 0; i < NUM_PIECES; i++) if (g_holes[i]) nholes++;
        printf("[holes] %d positions marked forced-empty (in rotated frame)\n", nholes);
        g_holes_active = true;
        /* No stop-band interaction to guard here: a hole UNPLACES a cell so the
         * search can refill it, rather than forcing it to stay empty, so holes
         * inside the band are the ordinary way to open one for enumeration. */
    } else {
        printf("[2/5] No holes file.\n"); fflush(stdout);
    }

    /* [3/5] Prepare output.  output.csv is always re-feedable.  The status CSV
     * is opt-in via --status.  The checkpoint is removed only after a clean
     * completion. */
    printf("[3/5] Preparing output files...\n"); fflush(stdout);

    char status_csv_path[PATH_MAX], pure_csv_path[PATH_MAX], mm_csv_path[PATH_MAX];
    char ckpt_csv_path[PATH_MAX];
    snprintf(status_csv_path,  sizeof(status_csv_path),  "%s.status.csv",         out_path);
    snprintf(pure_csv_path, sizeof(pure_csv_path), "%s.best_pure.csv",      out_path);
    snprintf(mm_csv_path,   sizeof(mm_csv_path),   "%s.best_mismatch.csv",  out_path);
    snprintf(ckpt_csv_path, sizeof(ckpt_csv_path), "%s.checkpoint.csv",     out_path);
    /* Sidecar naming as elsewhere here, carrying the band size the way the
     * beamer's beam_completions_<border>_<stop_row>.csv does, so a directory of
     * results says which row or column each file stopped at. */
    char band_csv_path[PATH_MAX];
    if (g_stop_active)
        snprintf(band_csv_path, sizeof(band_csv_path), "%s.stop_%s%d%s.csv",
                 out_path, g_stop_isrow ? "row" : "col", g_stop_n,
                 g_reverse ? "_rev" : "");

    const char *stream_hdr =
        "# E555 backtracker -- best board per record (original frame)\n"
        "# fields: config_id,score,pos_0,...,pos_255,rot_0,...,rot_255\n";
    const char *status_hdr =
        "config_id,sol_id,valid,status,order,parallel_mode,hall_mode,"
        "n_placed,n_empty,holes_applied,elapsed_sec,local_solutions,"
        "max_recursive_depth,max_placement_attempted,first_dead_depth,"
        "first_dead_row,first_dead_col,init_feasibility_code,"
        "dfs_visits_total,dfs_piece_tests_total,dfs_fit_ok_total,"
        "dfs_color_type_rejects_total,dfs_fc_rejects_total,"
        "dfs_hall_calls_total,dfs_hall_rejects_total,"
        "best_deepest_row,best_deepest_col,best_n_added,"
        "best_pieces,best_connected_edges,best_broken_edges,root_deficiency\n";

    g_stream_csv = fopen(out_path, "w");
    if (!g_stream_csv) fatal("cannot create %s: %s", out_path, strerror(errno));
    if (fputs(stream_hdr, g_stream_csv) == EOF)
        fatal("cannot write header to %s", out_path);
    checked_fflush(g_stream_csv, out_path);
    printf("  best-per-record -> %s\n", out_path);
    g_ckpt_csv = fopen(ckpt_csv_path, "w");
    if (!g_ckpt_csv) fatal("cannot create %s: %s", ckpt_csv_path, strerror(errno));
    if (fputs("# E555 backtracker -- crash-recovery checkpoint "
              "(append-only; LAST line per record = its best so far; removed on "
              "clean completion)\n"
              "# fields: config_id,score,pos_0,...,pos_255,rot_0,...,rot_255\n",
              g_ckpt_csv) == EOF)
        fatal("cannot write header to %s", ckpt_csv_path);
    checked_fflush(g_ckpt_csv, ckpt_csv_path);
    printf("  checkpoint      -> %s (crash recovery; removed on clean exit)\n",
           ckpt_csv_path);
    if (g_stop_active) {
        g_band_csv = fopen(band_csv_path, "w");
        if (!g_band_csv) fatal("cannot create %s: %s", band_csv_path, strerror(errno));
        if (fprintf(g_band_csv,
                    "# E555 backtracker -- every completed --stop_%s %d band%s "
                    "(original frame)\n"
                    "# cells outside the band are unplaced (%d); feed to "
                    "E555_finalizer --finalize_from %d\n"
                    "# fields: config_id,score,pos_0,...,pos_255,rot_0,...,rot_255\n",
                    g_stop_isrow ? "row" : "column", g_stop_n,
                    g_reverse ? " (anchored at the far side)" : "",
                    CSV_UNPLACED, g_stop_n) < 0)
            fatal("cannot write header to %s", band_csv_path);
        checked_fflush(g_band_csv, band_csv_path);
        printf("  stop band       -> %s\n", band_csv_path);
    }
    if (g_write_status) {
        g_status_csv = fopen(status_csv_path, "w");
        if (!g_status_csv) fatal("cannot create %s: %s", status_csv_path, strerror(errno));
        if (fputs(status_hdr, g_status_csv) == EOF)
            fatal("cannot write header to %s", status_csv_path);
        checked_fflush(g_status_csv, status_csv_path);
        printf("  status          -> %s\n", status_csv_path);
    } else {
        printf("  status          -> disabled (add --status to write %s)\n", status_csv_path);
    }
    fflush(stdout);

    /* [4/5] Read CSV */
    printf("\n[4/5] Reading CSV...\n"); fflush(stdout);
    bool use_stdin = (strcmp(csv_path, "-") == 0);
    FILE *csv_f = use_stdin ? stdin : fopen(csv_path, "r");
    if (!csv_f) fatal("cannot open CSV '%s': %s", csv_path, strerror(errno));

    CsvRecord *records = NULL;
    int n_records = 0, cap = 0;
    long long n_comment = 0, n_unparseable = 0, n_filtered = 0, physical_line = 0;

    char *csvline = NULL; size_t csvline_cap = 0; ssize_t csvlen;
    while ((csvlen = getline(&csvline, &csvline_cap, csv_f)) >= 0) {
        physical_line++;
        const char *trim = csvline;
        while (*trim == ' ' || *trim == '\t') trim++;
        if (*trim == '#' || *trim == '\0' || *trim == '\n' || *trim == '\r') {
            n_comment++; continue;
        }

        char cid[CONFIG_ID_LEN] = {0};
        long long sid = 0;
        int pos[NUM_PIECES], rot[NUM_PIECES];
        char why[256];

        if (!parse_csv_line(csvline, cid, &sid, pos, rot, why, sizeof(why))) {
            fprintf(stderr, "[warn] line %lld skipped: %s\n", physical_line, why);
            n_unparseable++; continue;
        }
        if (n_records == cap) {
            cap = cap ? cap * 2 : 1024;
            records = xrealloc(records, (size_t)cap * sizeof(CsvRecord));
        }
        snprintf(records[n_records].config_id, CONFIG_ID_LEN, "%s", cid);
        records[n_records].sol_id = sid;
        memcpy(records[n_records].pos, pos, sizeof(pos));
        memcpy(records[n_records].rot, rot, sizeof(rot));
        n_records++;
    }
    free(csvline);
    if (!use_stdin) fclose(csv_f);

    printf("[csv] loaded=%d  comment_or_blank=%lld  unparseable=%lld  filtered=%lld\n",
           n_records, n_comment, n_unparseable, n_filtered);
    fflush(stdout);

    if (n_records == 0) {
        printf("[done] No records to process.\n");
        checked_fclose(g_stream_csv, out_path);
        if (g_status_csv) checked_fclose(g_status_csv, status_csv_path);
        if (g_ckpt_csv) { checked_fclose(g_ckpt_csv, ckpt_csv_path); remove(ckpt_csv_path); }
        free(records);
        omp_destroy_lock(&g_best_lock);
        return EXIT_SUCCESS;
    }

    g_total_records = n_records;

    /* -- Select the processing window [proc_start, proc_start+proc_count) --
     * --row N is the 0-based index of the first data record to process (comment
     * and blank lines do not count); default 0.  --count caps how many records
     * (file order).  Default (no --count): one record when --row is given,
     * otherwise all records from the start.  Blocks for parallel machines:
     * --row 0 --count 100, --row 100 --count 100, ... */
    int proc_start = 0;
    if (g_row_start >= 0) {
        if (g_row_start >= n_records) {
            printf("[done] --row %lld is past the last record (%d loaded).\n",
                   g_row_start, n_records);
            checked_fclose(g_stream_csv, out_path);
            if (g_status_csv) checked_fclose(g_status_csv, status_csv_path);
            if (g_ckpt_csv) { checked_fclose(g_ckpt_csv, ckpt_csv_path); remove(ckpt_csv_path); }
            free(records);
            omp_destroy_lock(&g_best_lock);
            return EXIT_SUCCESS;
        }
        proc_start = (int)g_row_start;
    }
    int avail = n_records - proc_start;
    long long want = (g_count >= 1) ? g_count
                   : (g_row_start >= 0 ? 1 : (long long)avail);
    int proc_count = (want < avail) ? (int)want : avail;
    int proc_end = proc_start + proc_count;        /* exclusive */

    /* Resolve automatic parallelism only after the selected input window is
     * known.  For a small batch, parallelizing one DFS avoids the v9 failure
     * mode where early-dead records release workers while the remaining hard
     * records continue serially. */
    ParallelMode requested_parallel = g_parallel_mode;
    if (g_parallel_mode == PAR_AUTO)
        g_parallel_mode = (proc_count <= nt) ? PAR_SEARCH : PAR_RECORDS;
    g_search_parallel = (g_parallel_mode == PAR_SEARCH);
    printf("[parallel] requested=%s resolved=%s (records=%d workers=%d)\n",
           parallel_mode_name(requested_parallel), parallel_mode_name(g_parallel_mode),
           proc_count, nt);

    /* -- E555: mark records whose post-holes initial board repeats an earlier
     *    row (processed here or skipped below --row); they would exactly
     *    repeat that row's search. -- */
    int *dup_of = xmalloc((size_t)n_records * sizeof(int));
    for (int i = 0; i < n_records; i++) dup_of[i] = -1;
    if (g_dedup)
        build_duplicate_map(records, proc_end, proc_start, dup_of);
    else
        printf("[dedup] off by --no-dedup; every selected input row will be searched\n");

    /* [5/5] Search */
    printf("\n[5/5] Searching %d of %d record(s) (window [%d,%d))...\n",
           proc_count, n_records, proc_start, proc_end);
    if (g_search_parallel) {
        printf("  search-parallel: records sequential; each DFS may use all %d worker(s).\n", nt);
    } else {
        printf("  record-parallel: up to %d records concurrently; each DFS is serial.\n", nt);
    }
    fflush(stdout);

    if (g_search_parallel) {
        for (int i = proc_start; i < proc_end; i++) {
            if (dup_of[i] >= 0) {
                report_duplicate(&records[i], i, &records[dup_of[i]], dup_of[i]);
                continue;
            }
            process_line(records[i].config_id, records[i].sol_id,
                         records[i].pos, records[i].rot);
        }
    } else {
        #pragma omp parallel for schedule(dynamic,1) num_threads(nt)
        for (int i = proc_start; i < proc_end; i++) {
            if (dup_of[i] >= 0) {
                report_duplicate(&records[i], i, &records[dup_of[i]], dup_of[i]);
                continue;
            }
            process_line(records[i].config_id, records[i].sol_id,
                         records[i].pos, records[i].rot);
        }
    }

    double wall = elapsed_wall();

    /* -- End-of-run summary -- */
    printf("\n=== E555 backtracker complete ===\n");
    printf("  wall_time_sec          = %.3f\n", wall);
    printf("  records_loaded         = %d\n",   n_records);
    printf("  records_processed      = %" PRIu64 "\n", g_records_processed);
    printf("\n  By status:\n");
    printf("    invalid              = %" PRIu64 "\n", g_cnt_invalid);
    printf("    dropped(input>budget)= %" PRIu64 "\n", g_cnt_dropped);
    printf("    duplicate(post-holes)= %" PRIu64 "\n", g_cnt_duplicate);
    printf("    initially_infeasible = %" PRIu64 "\n", g_cnt_infeasible);
    printf("    hall_root_infeasible = %" PRIu64 "\n", g_cnt_hall_infeasible);
    printf("    solution             = %" PRIu64 "\n", g_cnt_solution);
    printf("    solution_cutoff      = %" PRIu64 "\n", g_cnt_solution_cutoff);
    printf("    no_solution          = %" PRIu64 "\n", g_cnt_no_solution);
    printf("    cutoff               = %" PRIu64 "\n", g_cnt_cutoff);
    printf("    full_solutions       = %" PRIu64 "\n", g_total_solutions);
    if (g_stop_active) {
        /* The accept rate is the diagnostic that matters under --break-mode
         * stuck, whose dives take a minimal break when no exact fit exists and
         * so can reject every band they produce.  Without it a run that emitted
         * nothing looks like a bug rather than the mode working as designed. */
        uint64_t seen = g_band_emitted + g_band_rejected;
        printf("    bands_emitted        = %" PRIu64 "\n", g_band_emitted);
        printf("    bands_rejected_broken= %" PRIu64 "%s\n", g_band_rejected,
               seen ? "" : "  (no band was ever completed)");
        if (seen)
            printf("    band_accept_rate     = %.1f%%\n",
                   100.0 * (double)g_band_emitted / (double)seen);
    }

    if (g_cnt_infeasible > 0) {
        printf("\n  Initial infeasibility breakdown:\n");
        const char *fnames[5] = { "ok", "type_mismatch", "color_deficit",
                                  "color_odd_surplus", "empty_exact_domain" };
        for (int k = 0; k < 5; k++) {
            if (g_feas_code_cnt[k])
                printf("    %-22s = %" PRIu64 "\n", fnames[k], g_feas_code_cnt[k]);
        }
    }

    /* -- Best partial boards (only with --best-output): PURE and MISMATCH -- */
    if (g_best_output > 0) {
        printf("\n  Best partial boards reached (top-%d per track):\n", g_best_output);
        if (g_best_pure_count == 0 && g_best_mm_count == 0)
            printf("    (none -- no search was performed)\n");
        if (g_best_pure_count > 0) {
            const BestPartial *bp = &g_best_pure[0];
            printf("    PURE     #1: cfg=%s sol=%lld  pieces=%d (added %d)  connected=%d  broken=0  deepest=(r=%d,c=%d)\n",
                   bp->record_id, bp->sol_id_num, bp->n_total, bp->n_added,
                   bp->n_connected, bp->deepest_row, bp->deepest_col);
            char title[220];
            snprintf(title, sizeof(title),
                     "=== Best PURE #1: cfg=%s sol=%lld  pieces=%d (added %d)  connected=%d  broken=0 ===",
                     bp->record_id, bp->sol_id_num, bp->n_total, bp->n_added, bp->n_connected);
            print_board_ascii(stdout, (const Board *)&bp->board, title);
        }
        if (g_best_mm_count > 0) {
            const BestPartial *bp = &g_best_mm[0];
            printf("    MISMATCH #1: cfg=%s sol=%lld  pieces=%d (added %d)  connected=%d  broken=%d  deepest=(r=%d,c=%d)\n",
                   bp->record_id, bp->sol_id_num, bp->n_total, bp->n_added,
                   bp->n_connected, bp->n_broken, bp->deepest_row, bp->deepest_col);
            char title[220];
            snprintf(title, sizeof(title),
                     "=== Best MISMATCH #1: cfg=%s sol=%lld  pieces=%d (added %d)  connected=%d  broken=%d "
                     "($ marks broken edges) ===",
                     bp->record_id, bp->sol_id_num, bp->n_total, bp->n_added,
                     bp->n_connected, bp->n_broken);
            print_board_ascii(stdout, (const Board *)&bp->board, title);
        }
        write_partials_track(pure_csv_path, "pure",     g_best_pure, g_best_pure_count);
        write_partials_track(mm_csv_path,   "mismatch", g_best_mm,   g_best_mm_count);
    }

    printf("\n  best_per_record_csv      = %s\n", out_path);
    if (g_status_csv)          printf("  status_csv               = %s\n", status_csv_path);
    if (g_best_pure_count > 0) printf("  best_partials_pure_csv   = %s\n", pure_csv_path);
    if (g_best_mm_count   > 0) printf("  best_partials_mm_csv     = %s\n", mm_csv_path);
    fflush(stdout);

    checked_fclose(g_stream_csv, out_path);
    if (g_status_csv) checked_fclose(g_status_csv, status_csv_path);
    if (g_ckpt_csv) {
        FILE *f = g_ckpt_csv;
        g_ckpt_csv = NULL;             /* no writers past this point */
        checked_fclose(f, ckpt_csv_path);
        if (remove(ckpt_csv_path) != 0)
            fprintf(stderr, "[warn] could not remove %s: %s\n",
                    ckpt_csv_path, strerror(errno));
    }
    free(g_best_pure);
    free(g_best_mm);
    free(dup_of);
    free(records);
    omp_destroy_lock(&g_best_lock);
    return EXIT_SUCCESS;
}
