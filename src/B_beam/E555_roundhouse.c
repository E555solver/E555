/*
 * E555_roundhouse.c -- deterministic width-W border-strip search.
 *
 * The solver keeps a validated interior structure, frees selected border bands,
 * and rebuilds one W-piece level at a time.  In every internal frame a round
 * searches the W rightmost columns from frame bottom to frame top; rotation and
 * mirroring map that one implementation onto the requested input-board sides.
 *
 * A normal level contains W-1 inner pieces and one edge terminal.  Its exposed
 * top colors form a signature with 17^(W-1)*5 states.  Before the final side,
 * and when an explicit final endpoint is requested, a backward color oracle
 * rejects signatures that cannot reach that endpoint.  An unrestricted final
 * side instead uses a maximum-reachable-depth oracle: branches need not reach
 * the far border, but branches that cannot tie the deepest board already found
 * can be skipped safely.  DFS always enforces actual piece uniqueness.  W=5
 * has about 17 times as many signatures as W=4 and many more chain records, so
 * successor signatures are cached by default.
 *
 * Geometry
 *   rounds 1..3 use nested right/top/left bands around a rectangular core.
 *   rounds 4 keeps a centered core, the complete first wall column, and the
 *   bottom-right W-piece anchor chain.  The four rotations then cover all sides;
 *   complete near/far overlap levels are treated as fixed prefixes/suffixes.
 *
 * Direction
 *   --ccw is the native traversal. --cw mirrors seed and board, runs the same
 *   frame search, then mirrors output back.  One name each, no aliases.
 *
 * Hold mode
 *   For rounds 1..3, --hold_band retains occupied cells in the half of the
 *   final side opposite the current traversal and searches the other half.
 *   The retained half is a ceiling, not a required endpoint: the final search
 *   keeps the deepest exact prefix even when it cannot reach the open seam.
 *   This definition is symmetric: CW and CCW map to opposite physical halves
 *   through the same frame transform.  Held cells are checked again at output;
 *   an internal change is a fatal invariant failure, never a silent result.
 *   A mismatched open seam is still emitted to the user-named output file and
 *   identified as class hold-join in stdout and by a `j` provenance tag.
 *
 * Stops and ties
 *   --stop_row is an absolute frame level on the final side only.
 *   --stop_after is rotation-independent and limits the number of new final-side
 *   levels.  --ties keeps equal-depth boards only when they differ at least
 *   --tie_depth complete W-piece levels behind the newest placement.
 *   --target_ties stops one input as soon as N such boards reach that endpoint.
 *
 * Parallelism and budgets
 *   Database construction and each oracle level are OpenMP-parallel.  The exact
 *   recursive DFS is serial because it mutates one board through nested rotations.
 *   --shard_count/--shard_index split a corpus across independent processes, the
 *   efficient form of parallelism for large board collections. --max_nodes counts
 *   DFS chain records for one input; database and oracle work are not included.
 *
 * Input/output
 *   CSV readers use the final 512 fields as pos[256],rot[256], so leading legacy
 *   metadata is allowed.  The third positional argument is the output CSV file;
 *   it is replaced at startup and receives every emitted puzzle board.  Output is
 *   restored to the input orientation and written as canonical 514-field rows.
 *   The output file's directory also receives the usual outputs.txt manifest;
 *   it contains the absolute output path when this invocation wrote boards and
 *   is empty otherwise.  Normal stdout has one physical [board] line per input.
 *
 * Build
 *   gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *       E555_database.c E555_roundhouse.c -o E555_roundhouse -lm
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <omp.h>

#include "E555_database.h"

#define MAX_W        5
#define MAX_LEVEL    16                 /* frame rows 0..15 */
#define MAX_ROUNDS   4
#define HOLD_SPLIT   8                  /* keep the far half of the final side */

/* -- Tunables (the file header explains what each one is for) --------------- */

static const char *g_out_path  = NULL;
static char     g_out_dir[PATH_MAX];   /* parent directory; holds outputs.txt */
static char     g_manifest_path[PATH_MAX];
static uint32_t g_line_first   = 0;     /* --start_row */
static uint32_t g_line_count   = 0;     /* --num_rows; 0 = every remaining line */
static int      g_opt_W        = 5;     /* --strip_width, 0 = narrowest usable */
static int      g_rounds       = 3;
static int      g_rotate       = 1;
static bool     g_reverse      = false; /* --reverse: mirror the board left-right */
static int      g_stop_level   = -1;    /* --stop_row, -1 = the strip's last */
static int      g_stop_after   = -1;    /* explicit new-level endpoint, final side */
static bool     g_hold_band    = false; /* --hold_band: keep the last band's pieces */
static int      g_pin_corner[4] = { -1, -1, -1, -1 };   /* BL BR TL TR (input) */
static uint64_t g_max_nodes    = 0;
static uint32_t g_ties         = 1;     /* boards to emit at the deepest reach */
static uint32_t g_tie_depth    = 2;     /* changed chain levels behind the frontier */
static uint32_t g_target_ties  = 0;     /* stop this input after N endpoint ties */
static uint32_t g_shard_count  = 1;     /* process every count-th row in the window */
static uint32_t g_shard_index  = 0;     /* zero-based member of the shard set */
static int      g_max_breaks   = 0;     /* --breaks: greedy fill budget */
static double   g_config_time_sec = 600.0;
static double   g_max_wall_sec = 0.0;
static uint64_t g_max_boards   = 0;
static bool     g_transition_cache = true;

/* Corner roles, indexed as g_pin_corner: 0=BL 1=BR 2=TL 3=TR, in the INPUT
   board's coordinates (cell (0,0), (0,15), (15,0), (15,15)). */
static const char *k_corner_name[4] = { "BL", "BR", "TL", "TR" };

/* -- Board (frame coordinates) --------------------------------------------- */

static Oriented g_grid[PUZZLE_SIDE][PUZZLE_SIDE];
static bool     g_has[PUZZLE_SIDE][PUZZLE_SIDE];
static uint16_t g_order[PUZZLE_SIDE][PUZZLE_SIDE]; /* path-relative placement order */
static uint64_t g_placed[4];            /* pieces currently on the board */
static int      g_rot_applied;          /* CW quarter-turns vs the input board */
static char     g_in_id[96];            /* the input board's config_id, kept on output */
static int      g_W;                    /* strip width in force */
static int      g_n_placed;             /* pieces on the board: the depth metric */
static int      g_avail[NUM_COLORS_TOTAL];   /* free inner-color sides, for parity */
static int      g_input_placed;
static int      g_base_kept;
static int      g_hold_kept;
static bool     g_hold_active;
static bool     g_hold_mask[PUZZLE_SIDE][PUZZLE_SIDE];
/* Expected held placements in the unrotated search space.  Under --cw that
   space is mirrored, as is g_input_snap; output is un-mirrored only after this
   invariant has been checked. */
static bool     g_hold_expected[NUM_PIECES];
static int      g_hold_expected_pos[NUM_PIECES];
static uint8_t  g_hold_expected_rot[NUM_PIECES];
static int      g_hold_near_occupied;
static uint64_t g_db_reusable[4];       /* fixed edge anchors allowed in DB records */

typedef enum {
    HOLD_REPORT_OFF,
    HOLD_REPORT_ACTIVE,
    HOLD_REPORT_EMPTY,
    HOLD_REPORT_FALLBACK
} HoldReport;

static HoldReport g_hold_report = HOLD_REPORT_OFF;
static char       g_hold_reason[128];
static char       g_board_reason[192];

/* -- Run state ------------------------------------------------------------- */

/* One explicit output file.  The row's provenance tag and stdout class say
   whether it is clean, an open-seam hold join, or a greedy break fill. */
static FILE    *g_out_fp = NULL;
static uint32_t g_line_id  = 0;         /* input CSV line being searched */
static uint64_t g_emitted  = 0;
static double   g_t_start  = 0.0;
static double   g_t_config = 0.0;
static uint64_t g_nodes;                /* nodes spent on the current input board */
static bool     g_truncated;            /* did a budget bite on this input board? */
static bool     g_target_stop;          /* requested endpoint quota reached */
static uint32_t g_endpoint_hits;          /* diverse final-endpoint boards retained */
static int      g_endpoint_depth;         /* depth associated with endpoint_hits */
static uint64_t g_first_endpoint_node;    /* DFS node at the first endpoint hit */
static uint32_t g_verbose_seen[MAX_ROUNDS+2];
static volatile sig_atomic_t g_stop = 0;
/* SIGINT/SIGTERM: stop at the next budget check so the output file stays valid. */
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* -- Run statistics -------------------------------------------------------- */
/* A run that emits nothing but refutations is the normal case on the real seed,
   so the counters that explain WHY are the important ones: refuted[] holds the
   level at which colour alone killed a strip before any piece was tried. */
static struct {
    uint32_t lines_read, lines_used, lines_dup, lines_bad;
    uint32_t lines_exhausted, lines_truncated, lines_target;
    uint32_t strips, strips_refuted;
    uint32_t refuted[MAX_LEVEL+2];
    uint64_t nodes;
    uint64_t emit_solved, emit_deepest, emit_joined, emit_filled;
} g_stats;

/* Input-board side corresponding to frame-right in each rotation. */
static const char *k_side_name[4] = { "RIGHT", "TOP", "LEFT", "BOTTOM" };
/* Under --reverse the frame is a mirror image of the input, and a left-right
   mirror swaps exactly those two sides. */
static const char *k_side_name_rev[4] = { "LEFT", "TOP", "RIGHT", "BOTTOM" };
static const char *round_side(int round) {
    return (g_reverse ? k_side_name_rev : k_side_name)[(g_rotate + round - 1) & 3];
}
static const char *direction_name(void) { return g_reverse ? "CW" : "CCW"; }

/* Every search stopping condition in one place. */
static bool budget_spent(void) {
    if (g_stop) return true;
    if (g_max_wall_sec > 0.0 && omp_get_wtime() - g_t_start >= g_max_wall_sec) return true;
    if (g_max_boards > 0 && g_emitted >= g_max_boards) return true;
    if (g_config_time_sec > 0.0 && omp_get_wtime() - g_t_config >= g_config_time_sec) return true;
    if (g_max_nodes > 0 && g_nodes >= g_max_nodes) return true;
    return false;
}

/* -- Small helpers --------------------------------------------------------- */

static Oriented rh_oriented(uint16_t pid, uint8_t spin) {
    const int e[4] = { g_seed_top[pid], g_seed_right[pid],
                       g_seed_bottom[pid], g_seed_left[pid] };
    Oriented o;
    o.piece_id = pid; o.rotation = spin;
    o.top    = (uint8_t)e[(0 + spin) & 3]; o.right = (uint8_t)e[(1 + spin) & 3];
    o.bottom = (uint8_t)e[(2 + spin) & 3]; o.left  = (uint8_t)e[(3 + spin) & 3];
    return o;
}

/* Frame sides of a piece in its canonical orientation: 0 = inner, 1 = edge,
   2 = corner. The shared module's copy of this is static, hence the local one. */
static int rh_zero_count(int pid) {
    return (g_seed_top[pid] == 0) + (g_seed_right[pid] == 0) +
           (g_seed_bottom[pid] == 0) + (g_seed_left[pid] == 0);
}

/* -- CW direction: left-right mirror ----------------------------------------
   Mirroring reverses traversal while preserving piece IDs:

       (r,c,spin) -> (r,15-c,(4-spin)&3)

   Seed, clues, input and output cross this boundary; the search itself remains
   unchanged in mirror space. */

/* Swap every piece's left and right colours, then rebuild the oriented catalog
   the way load_seed_and_catalog does. Called between that and
   build_catalog_indices(), so nothing downstream ever sees the real seed. */
static void mirror_seed(void) {
    for (int i = 0; i < NUM_PIECES; i++) {
        int t = g_seed_left[i]; g_seed_left[i] = g_seed_right[i]; g_seed_right[i] = t;
    }
    g_cat_count = 0;
    for (int k = 0; k < g_num_inner; k++)
        for (uint8_t spin = 0; spin < 4; spin++)
            g_cat[g_cat_count++] = rh_oriented((uint16_t)g_inner_ids[k], spin);
}

/* Mirror one board in the canonical pos[]/rot[] layout, in place. Used on the
   way in (before anything reads the board) and on the way out (on a copy). */
static void mirror_line(int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == 999) continue;
        int r = pos[pid] / PUZZLE_SIDE, c = pos[pid] % PUZZLE_SIDE;
        pos[pid] = r * PUZZLE_SIDE + (PUZZLE_SIDE - 1 - c);
        rot[pid] = (4 - rot[pid]) & 3;
    }
}

/* Side indices as in rh_oriented: 0=top 1=right 2=bottom 3=left. */
static int side_color(const Oriented *o, int side) {
    return side == 0 ? o->top : side == 1 ? o->right : side == 2 ? o->bottom : o->left;
}

/* The spin that puts color 0 on `side` (edges), or on both sides (corners). */
static bool orient_frame(uint16_t pid, int sideA, int sideB, Oriented *out) {
    for (uint8_t spin = 0; spin < 4; spin++) {
        Oriented o = rh_oriented(pid, spin);
        if (side_color(&o, sideA) != 0) continue;
        if (sideB >= 0 && side_color(&o, sideB) != 0) continue;
        *out = o; return true;
    }
    return false;
}

/* Put a piece on / take it off the board's bookkeeping. The colour tally the
   parity prune reads is maintained here so the hot loop never rescans. */
static void take_piece(uint16_t pid) {
    used_set(g_placed, pid);
    g_n_placed++;
    int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
    for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]--;
}
static void drop_piece(uint16_t pid) {
    used_clear(g_placed, pid);
    g_n_placed--;
    int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
    for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
}

/* -- Frame rotation -------------------------------------------------------- */

/* One clockwise quarter-turn of the whole board: cell (r,c) -> (15-c, r), and a
   piece's spin advances by 3 (its old left face becomes its new top face). */
static void rotate_cw(void) {
    static Oriented ng[PUZZLE_SIDE][PUZZLE_SIDE];
    static bool     nh[PUZZLE_SIDE][PUZZLE_SIDE];
    static uint16_t no[PUZZLE_SIDE][PUZZLE_SIDE];
    memset(nh, 0, sizeof nh);
    memset(no, 0, sizeof no);
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!g_has[r][c]) continue;
            ng[PUZZLE_SIDE-1-c][r] = rh_oriented(g_grid[r][c].piece_id,
                                                 (uint8_t)((g_grid[r][c].rotation + 3) & 3));
            nh[PUZZLE_SIDE-1-c][r] = true;
            no[PUZZLE_SIDE-1-c][r] = g_order[r][c];
        }
    memcpy(g_grid, ng, sizeof g_grid);
    memcpy(g_has,  nh, sizeof g_has);
    memcpy(g_order, no, sizeof g_order);
    g_rot_applied = (g_rot_applied + 1) & 3;
}

/* Map a frame cell back out of the turns, undoing `k` of them. One turn back is
   (R,C) -> (C, 15-R). This lands in the space the SEARCH calls the board, which
   under --reverse is still mirrored -- frame_to_input below goes the last step. */
static void frame_to_orig(int R, int C, int k, int *r, int *c) {
    for (int i = 0; i < (k & 3); i++) { int nr = C, nc = PUZZLE_SIDE-1-R; R = nr; C = nc; }
    *r = R; *c = C;
}

/* The same map carried one step further, to the board the user actually handed
   in: undo the turns, then the --reverse mirror. Messages, corner roles and the
   emitted boards go through here; the search itself never does, because it
   works in mirror space throughout. Identical to frame_to_orig without
   --reverse. */
static void frame_to_input(int R, int C, int k, int *r, int *c) {
    frame_to_orig(R, C, k, r, c);
    if (g_reverse) *c = PUZZLE_SIDE - 1 - *c;
}

/* -- Eternity II clue pieces ------------------------------------------------
   Every retained shape keeps the center, so --clue_center verifies it. The four
   off-center clues may be searched; record and border-chain filters pin their
   piece, cell and spin. */
static int g_rh_orient = -1;              /* orientation of the board being searched */

/* One clockwise quarter-turn applied to a cell and a spin, matching rotate_cw
   exactly: (r,c) -> (15-c, r) and spin -> (spin+3)&3. */
static void orig_to_frame(int r, int c, int spin, int k, int *R, int *C, int *S) {
    for (int i = 0; i < (k & 3); i++) {
        int nr = PUZZLE_SIDE-1-c, nc = r;
        r = nr; c = nc; spin = (spin + 3) & 3;
    }
    *R = r; *C = c; *S = spin;
}

/* orig_to_frame must invert frame_to_orig for every cell and turn count. A wrong
   spin direction here would pin the right piece the wrong way round and quietly
   produce a board that satisfies every edge but not the clue, so it is checked
   rather than commented. Runs once, only when a clue flag is set. */
static void check_frame_maps(void) {
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < PUZZLE_SIDE; r++)
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                int R, C, S, br, bc;
                orig_to_frame(r, c, 0, k, &R, &C, &S);
                frame_to_orig(R, C, k, &br, &bc);
                if (br != r || bc != c)
                    fatal("orig_to_frame/frame_to_orig disagree at k=%d (%d,%d) -> (%d,%d) -> (%d,%d)",
                          k, r, c, R, C, br, bc);
            }
}

/* Is this clue entry active under the current flags? Entry 0 is the center. */
static inline bool clue_on(int k) {
    return (k == 0) ? (g_clue_mask & CLUE_CENTER) != 0 : (g_clue_mask & CLUE_CORNERS) != 0;
}

/* The clue table this run searches against. It is g_clue, and under --reverse it
   is g_clue mirrored: a reflected board's clues are not any of the four
   quarter-turns g_clue holds, so without this every clued board would be
   skipped for "carries none of the enabled clue pieces". The row index keeps its
   meaning -- the input board is mirrored too, so a board that was orientation o
   still matches at row o -- and g_clue itself stays untouched, so a message that
   names a cell on the user's board reads it from there. */
static ClueCell g_rh_clue[4][CLUE_N];

static void init_clue_table(void) {
    memcpy(g_rh_clue, g_clue, sizeof g_rh_clue);
    if (!g_reverse) return;
    for (int o = 0; o < 4; o++)
        for (int k = 0; k < CLUE_N; k++) {
            ClueCell *cc = &g_rh_clue[o][k];
            cc->col  = (uint8_t)(PUZZLE_SIDE - 1 - cc->col);
            cc->spin = (uint8_t)((4 - cc->spin) & 3);
        }
}

/* The orientation this board committed to: the one satisfying the most enabled
   clues. -1 when it satisfies none, which is the only case this tool cannot
   resolve on its own. */
static int rh_clue_orient_of(const int pos[NUM_PIECES], const int rot[NUM_PIECES]) {
    int best = -1, best_n = 0;
    for (int o = 0; o < 4; o++) {
        int n = 0;
        for (int k = 0; k < CLUE_N; k++) {
            if (!clue_on(k)) continue;
            const ClueCell *cc = &g_rh_clue[o][k];
            if (pos[cc->piece] == cc->row * PUZZLE_SIDE + cc->col && rot[cc->piece] == cc->spin) n++;
        }
        if (n > best_n) { best = o; best_n = n; }
    }
    return best;
}

/* Is `pid` one of the enabled clue pieces? Five entries, scanned linearly --
   this sits in the record loop, so it stays a straight-line test over a table
   that is in cache. */
static inline bool rh_is_clue_piece(uint16_t pid) {
    if (!g_clue_mask || g_rh_orient < 0) return false;
    for (int k = 0; k < CLUE_N; k++)
        if (clue_on(k) && g_rh_clue[g_rh_orient][k].piece == pid) return true;
    return false;
}

/* The clue owed by frame cell (R,C) under the current rotation, or NULL. */
static const ClueCell *rh_clue_at_frame(int R, int C, int *spin_out) {
    if (!g_clue_mask || g_rh_orient < 0) return NULL;
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_rh_clue[g_rh_orient][k];
        int fr, fc, fs;
        orig_to_frame(cc->row, cc->col, cc->spin, g_rot_applied, &fr, &fc, &fs);
        if (fr == R && fc == C) { *spin_out = fs; return cc; }
    }
    return NULL;
}

/* The same map on a whole frame rectangle, for the log. A quarter-turn sends a
   rectangle to a rectangle and so does the mirror, so mapping the two opposite
   corners and taking the min/max is exact -- no scan needed. Used to say where
   the core and the strip sit on the board the user handed in, which is the one
   thing no reader can work out from frame coordinates. */
static void box_to_orig(int R0, int R1, int C0, int C1, int k,
                        int *r0, int *r1, int *c0, int *c1) {
    int a_r, a_c, b_r, b_c;
    frame_to_input(R0, C0, k, &a_r, &a_c);
    frame_to_input(R1, C1, k, &b_r, &b_c);
    *r0 = a_r < b_r ? a_r : b_r; *r1 = a_r < b_r ? b_r : a_r;
    *c0 = a_c < b_c ? a_c : b_c; *c1 = a_c < b_c ? b_c : a_c;
}

/* -- The width-W chain database -------------------------------------------- */
/* A chain is (W-1) inner pieces plus one frame-right edge terminal -- exactly a
 * segment-C record, shortened. A cell is keyed by the color exposed to the
 * chain's left plus the W colors it presents downward, the last of which is a
 * frame-interface color (1..5) because the right border column perpetuates one
 * upward. Records pack to (W-1)*lb_bits + term_bits, as in E555_database.c.
 *
 * A signature is the same W-color tuple: (W-1) inner + 1 frame-interface, so a
 * chain's exposed tops index the next level's cell directly. */

static uint32_t s_nsig;                 /* 17^(W-1) * 5 */
static uint64_t s_ncell;                /* 17 * s_nsig */
static Cell   **s_db      = NULL;
static uint8_t *s_arena   = NULL;
static size_t   s_arena_bytes = 0;
static uint32_t *s_rec_off = NULL;       /* cell -> first entry in s_succ */
static uint32_t *s_succ    = NULL;       /* successor signature per record */
static size_t    s_succ_bytes = 0;
static int      s_lb_bits, s_term_bits, s_rec_bytes;
static uint64_t s_records = 0;
static uint64_t s_cells = 0;
static uint64_t s_db_bytes = 0;
static uint32_t s_max_cell_n = 0;
static uint64_t s_exclude_key[4];
static int      s_db_W = -1;
static bool     s_db_ready = false;

/* Smallest field width (>= 1 bit) that can index n items. */
static int bits_for(int n) { int b = 1; while ((1 << b) < n) b++; return b; }

/* The chain width, with its range visible to the optimizer. g_W is validated to
   2..MAX_W when a board is loaded; taking the clamped copy into a local lets the
   bounds checker see that every [MAX_W] index below is in range. */
static inline int chain_w(void) {
    int w = g_W;
    if (w < 2) w = 2;
    if (w > MAX_W) w = MAX_W;
    return w;
}

/* Pack W colors into a signature: (W-1) inner-color INDICES (0..16) and one raw
   frame-interface color (1..5). Mixed-radix, interface color innermost. */
static uint32_t sig_make(const int inner_idx[], int b_iface) {
    const int W = chain_w();
    uint32_t s = 0;
    for (int i = 0; i < W - 1; i++) s = s*DIM_INNER + (uint32_t)inner_idx[i];
    return s*MAX_EDGE_SIDE_COLOR + (uint32_t)(b_iface - 1);
}

/* Inverse of sig_make, but writing RAW colors: bot[] is what the record codec
   and decode_inner_chain() expect as the cell's bottom colors. */
static void sig_bottoms(uint32_t sig, uint8_t bot[]) {
    const int W = chain_w();
    bot[W-1] = (uint8_t)(sig % MAX_EDGE_SIDE_COLOR + 1);
    uint32_t q = sig / MAX_EDGE_SIDE_COLOR;
    for (int i = W - 2; i >= 0; i--) { bot[i] = (uint8_t)(q % DIM_INNER + COLOR_MIN); q /= DIM_INNER; }
}

/* Record codec. A record is (W-1) inner fields of s_lb_bits -- each piece's
   index inside its tiny (left,bottom) catalog bucket, both colors being known at
   decode time -- plus one terminal field of s_term_bits, its index inside the
   per-left edge-terminal bucket. The whole record fits in 32 bits (asserted at
   build time) and is stored in s_rec_bytes bytes. */
static inline uint32_t rh_pack(const uint8_t f[], int term) {
    const int W = chain_w();
    uint32_t w = 0;
    for (int i = 0; i < W - 1; i++) w |= (uint32_t)f[i] << (s_lb_bits * i);
    return w | ((uint32_t)term << (s_lb_bits * (W - 1)));
}

/* Undo rh_pack. */
static inline void rh_unpack(uint32_t w, uint8_t f[], int *term) {
    const int W = chain_w();
    uint32_t m = (1u << s_lb_bits) - 1u, tm = (1u << s_term_bits) - 1u;
    for (int i = 0; i < W - 1; i++) f[i] = (uint8_t)((w >> (s_lb_bits * i)) & m);
    *term = (int)((w >> (s_lb_bits * (W - 1))) & tm);
}

/* Decode record j of a cell keyed by (la_color, bot[0..W-1]). Fills the catalog
   indices of the inner pieces, the terminal's index into g_edge_term, and the
   signature of the chain's exposed tops. */
static inline bool rh_decode(const Cell *cell, uint32_t j, int la_color,
                             const uint8_t bot[], uint16_t ci[], int *term,
                             uint32_t *succ) {
    const int W = chain_w();
    uint8_t loc[MAX_W]; int t_idx;
    rh_unpack(rec_load(cell->rec, j, s_rec_bytes), loc, &t_idx);
    uint64_t ignored[4] = {0,0,0,0};
    if (!decode_inner_chain(loc, W - 1, la_color, bot, ci, ignored, ignored)) return false;
    int cl = g_cat[ci[W-2]].right;
    if (t_idx >= g_edge_term_by_left_n[cl]) return false;
    *term = g_edge_term_by_left[cl][t_idx];
    uint32_t s = 0;
    for (int i = 0; i < W - 1; i++) s = s*DIM_INNER + (uint32_t)INNER_IDX(g_cat[ci[i]].top);
    *succ = s*MAX_EDGE_SIDE_COLOR + (uint32_t)(g_edge_term[*term].top - 1);
    return true;
}

/* Grow one chain: walk (W-1) inner pieces left to right, chaining each piece's
   right color into the next one's left, then attach every legal frame-right edge
   terminal. `used` carries the excluded set down, so a record can never contain
   an excluded piece NOR repeat a piece within itself -- an invariant the search
   relies on. Two passes: store=false counts records per cell, store=true writes
   them into the laid-out arena. */
static void db_dfs(int depth, int la, int next_left, int bidx[], uint8_t locs[],
                   uint64_t used[4], uint32_t *work, bool store) {
    if (depth == chain_w() - 1) {
        int cl = next_left;
        if (!color_is_inner(cl)) return;
        for (int k = 0; k < g_edge_term_by_left_n[cl]; k++) {
            int t = g_edge_term_by_left[cl][k];
            if (used_test(used, g_edge_term[t].piece_id)) continue;
            uint64_t fi = (uint64_t)la * s_nsig + sig_make(bidx, g_edge_term[t].bottom);
            if (!store) {
                #pragma omp atomic
                work[fi]++;
            } else {
                uint32_t pos;
                #pragma omp atomic capture
                pos = work[fi]++;
                rec_store(s_db[fi]->rec, pos, s_rec_bytes, rh_pack(locs, k));
            }
        }
        return;
    }
    for (int i = 0; i < g_left_count[next_left]; i++) {
        int ci = g_left_bucket[next_left][i];
        uint16_t pid = g_cat[ci].piece_id;
        if (used_test(used, pid)) continue;
        bidx[depth] = INNER_IDX(g_cat[ci].bottom);
        locs[depth] = (uint8_t)g_cat_to_lb_local[ci];
        used_set(used, pid);
        db_dfs(depth+1, la, g_cat[ci].right, bidx, locs, used, work, store);
        used_clear(used, pid);
    }
}

/* One full pass of the build, parallel over the first piece of the chain. */
static void db_pass(uint32_t *work, bool store) {
    #pragma omp parallel for schedule(dynamic, 4)
    for (int ci0 = 0; ci0 < g_cat_count; ci0++) {
        uint16_t pid0 = g_cat[ci0].piece_id;
        if (used_test(g_db_exclude, pid0)) continue;
        uint64_t used[4];
        memcpy(used, g_db_exclude, sizeof used);
        uint8_t locs[MAX_W]; int bidx[MAX_W];
        int la = INNER_IDX(g_cat[ci0].left);
        bidx[0] = INNER_IDX(g_cat[ci0].bottom);
        locs[0] = (uint8_t)g_cat_to_lb_local[ci0];
        used_set(used, pid0);
        db_dfs(1, la, g_cat[ci0].right, bidx, locs, used, work, store);
    }
}

static int cmp_chain_record(const void *a, const void *b) {
    return memcmp(a, b, (size_t)s_rec_bytes);
}

/* Build the chain database for the current g_db_exclude. Two passes (count, then
   store) into one arena, as the shared module does. */
static void build_chain_db(void) {
    double t0 = omp_get_wtime();
    if (s_arena) { munmap(s_arena, s_arena_bytes); s_arena = NULL; s_arena_bytes = 0; }
    if (s_succ) { munmap(s_succ, s_succ_bytes); s_succ = NULL; s_succ_bytes = 0; }
    free(s_rec_off); s_rec_off = NULL;
    if (!s_db) s_db = xmalloc(s_ncell * sizeof(Cell *));
    memset(s_db, 0, s_ncell * sizeof(Cell *));

    build_edge_terminal_pool();          /* honours g_db_exclude in free mode */
    int maxt = 1;
    for (int c = 0; c < NUM_COLORS_TOTAL; c++)
        if (g_edge_term_by_left_n[c] > maxt) maxt = g_edge_term_by_left_n[c];
    s_lb_bits = g_lb_bits; s_term_bits = bits_for(maxt);
    if ((g_W - 1) * s_lb_bits + s_term_bits > 32)
        fatal("chain record needs %d bits (>32)", (g_W-1)*s_lb_bits + s_term_bits);
    s_rec_bytes = ((g_W - 1) * s_lb_bits + s_term_bits + 7) / 8;

    uint32_t *cnt = calloc(s_ncell, sizeof(uint32_t));
    if (!cnt) fatal("calloc for chain count array (%" PRIu64 " cells) failed", s_ncell);
    db_pass(cnt, false);

    uint64_t bytes = 0; s_records = 0; s_max_cell_n = 0;
    uint64_t cells = 0;
    for (uint64_t fi = 0; fi < s_ncell; fi++) {
        if (!cnt[fi]) continue;
        bytes += cell_stride(cnt[fi], s_rec_bytes);
        s_records += cnt[fi]; cells++;
        if (cnt[fi] > s_max_cell_n) s_max_cell_n = cnt[fi];
    }
    if (!cells) fatal("chain database is empty: no legal width-%d chain from this pool", g_W);
    s_arena = arena_map(bytes); s_arena_bytes = bytes;
    uint64_t off = 0;
    for (uint64_t fi = 0; fi < s_ncell; fi++) {
        if (!cnt[fi]) continue;
        Cell *c = (Cell *)(s_arena + off);
        c->n = cnt[fi]; c->_pad = 0;
        s_db[fi] = c;
        off += cell_stride(cnt[fi], s_rec_bytes);
    }
    memset(cnt, 0, s_ncell * sizeof(uint32_t));
    db_pass(cnt, true);

    /* Parallel insertion uses atomic slots, so scheduler timing would otherwise
       change DFS order and make --max_nodes comparisons noisy. */
    #pragma omp parallel for schedule(dynamic, 512)
    for (long long ffi = 0; ffi < (long long)s_ncell; ffi++) {
        Cell *cell = s_db[(uint64_t)ffi];
        if (cell && cell->n > 1)
            qsort(cell->rec, cell->n, (size_t)s_rec_bytes, cmp_chain_record);
    }

    /* Cache each record's exposed-top signature. Oracle sweeps revisit every
       record many times, especially at W=5; decoding once here trades modest
       memory for substantially cheaper sweeps and lets DFS reject dead-color
       records before recovering their piece identities. */
    double tc0 = omp_get_wtime();
    const uint64_t cache_limit = 768ULL << 20;
    uint64_t need = s_records * sizeof(uint32_t);
    if (g_transition_cache && need <= cache_limit) {
        if (s_records > UINT32_MAX)
            fatal("transition cache has too many records (%" PRIu64 ")", s_records);
        s_rec_off = xmalloc(((size_t)s_ncell + 1u) * sizeof(uint32_t));
        uint64_t roff = 0;
        for (uint64_t fi = 0; fi < s_ncell; fi++) {
            s_rec_off[fi] = (uint32_t)roff;
            roff += cnt[fi];
        }
        s_rec_off[s_ncell] = (uint32_t)roff;
        s_succ_bytes = (size_t)need;
        s_succ = arena_map(s_succ_bytes);
        #pragma omp parallel for schedule(dynamic, 256)
        for (long long ffi = 0; ffi < (long long)s_ncell; ffi++) {
            uint64_t fi = (uint64_t)ffi;
            const Cell *cell = s_db[fi];
            if (!cell) continue;
            int wall = (int)(fi / s_nsig) + COLOR_MIN;
            uint32_t sig = (uint32_t)(fi % s_nsig);
            uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
            uint32_t base = s_rec_off[fi];
            for (uint32_t j = 0; j < cell->n; j++) {
                uint16_t ci[MAX_W]; int term; uint32_t succ;
                s_succ[base+j] = rh_decode(cell, j, wall, bot, ci, &term, &succ)
                                   ? succ : UINT32_MAX;
            }
        }
    }
    double cache_sec = omp_get_wtime() - tc0;
    free(cnt);

    s_cells = cells;
    s_db_bytes = bytes;
    memcpy(s_exclude_key, g_db_exclude, sizeof s_exclude_key);
    s_db_W = g_W;
    s_db_ready = true;

    const char *cache_state = !g_transition_cache ? "off" :
                              need > cache_limit ? "skipped-limit" : "on";
    if (g_verbose)
        printf("[db] W=%d records=%" PRIu64 " cells=%" PRIu64 " db=%.3fGB "
               "cache=%s/%.3fGB max_cell=%u total=%.2fs cache_build=%.2fs\n",
               g_W, s_records, cells, (double)bytes/1e9, cache_state,
               (double)s_succ_bytes/1e9, s_max_cell_n, omp_get_wtime()-t0, cache_sec);
}

/* -- Border chains (level 0 and the top closure) --------------------------- */
/* W pieces along a horizontal frame row: (W-1) edges with their frame side out,
 * then one corner in the last column with two frame sides. They chain on
 * frame-interface colors, starting from the color the placed cell at
 * (row, 15-W) exposes to its right. */

typedef struct {
    uint16_t pid[MAX_W];
    uint8_t  spin[MAX_W];
    uint32_t sig;                       /* tops (level 0) or bottoms (closure) */
    uint64_t mask[4];
    uint8_t  reach;                     /* relaxed levels including level 0 */
} BorderChain;

/* Per round, because round 2 enumerates its own chains while round 1 is still
   walking its list. Nothing here is shared between rounds. */
static BorderChain *g_bc[MAX_ROUNDS+2];
static size_t       g_bc_n[MAX_ROUNDS+2], g_bc_cap[MAX_ROUNDS+2];
static BorderChain *g_close[MAX_ROUNDS+2];
static size_t       g_close_n[MAX_ROUNDS+2], g_close_cap[MAX_ROUNDS+2];

static void bc_push(BorderChain **arr, size_t *n, size_t *cap, const BorderChain *b) {
    if (*n == *cap) { *cap = *cap ? *cap*2 : 256; *arr = xrealloc(*arr, *cap * sizeof(BorderChain)); }
    (*arr)[(*n)++] = *b;
}

/* frame_side: 2 for the bottom row (frame points down), 0 for the top row. The
   corner also carries frame on its right (column 15). */
static void bc_dfs(int depth, int left_need, int frame_side, int pin_corner,
                   uint64_t used[4], BorderChain *cur, int inner[],
                   BorderChain **out, size_t *n, size_t *cap) {
    const int W = chain_w();
    if (depth == W - 1) {
        for (int pid = 0; pid < NUM_PIECES; pid++) {
            if (rh_zero_count(pid) != 2) continue;
            if (pin_corner >= 0 && pid != pin_corner) continue;
            if (used_test(used, (uint16_t)pid)) continue;
            Oriented o;
            if (!orient_frame((uint16_t)pid, frame_side, 1, &o)) continue;
            if (o.left != left_need) continue;
            int expose = (frame_side == 2) ? o.top : o.bottom;
            if (!color_is_edge_iface(expose)) continue;
            cur->pid[depth] = (uint16_t)pid; cur->spin[depth] = o.rotation;
            cur->sig = sig_make(inner, expose);
            memcpy(cur->mask, used, sizeof cur->mask);
            used_set(cur->mask, (uint16_t)pid);
            bc_push(out, n, cap, cur);
        }
        return;
    }
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (rh_zero_count(pid) != 1) continue;
        if (used_test(used, (uint16_t)pid)) continue;
        Oriented o;
        if (!orient_frame((uint16_t)pid, frame_side, -1, &o)) continue;
        if (o.left != left_need) continue;
        int expose = (frame_side == 2) ? o.top : o.bottom;
        if (!color_is_inner(expose)) continue;
        cur->pid[depth] = (uint16_t)pid; cur->spin[depth] = o.rotation;
        inner[depth] = INNER_IDX(expose);
        used_set(used, (uint16_t)pid);
        bc_dfs(depth+1, o.right, frame_side, pin_corner, used, cur, inner, out, n, cap);
        used_clear(used, (uint16_t)pid);
    }
}

/* Which corner role of the INPUT board sits at frame cell (R,15)? Reading the
   role in input coordinates is what keeps --BL/--BR/--TL/--TR naming the board
   the user handed in under --reverse, with no pin table to swap. */
static int corner_role_at(int R) {
    int r, c;
    frame_to_input(R, PUZZLE_SIDE-1, g_rot_applied, &r, &c);
    return (r == 0 ? 0 : 2) + (c == 0 ? 0 : 1);
}

/* All legal border chains for one horizontal frame row of the strip, starting
   from the color the placed wall cell exposes to its right. Pieces already on
   the board are unavailable. frame_side is 2 for the strip's bottom row (frame
   pointing down) and 0 for its top row. */
static void enumerate_border_row(int row, int frame_side,
                                 BorderChain **out, size_t *n, size_t *cap) {
    *n = 0;
    int left_need = g_grid[row][PUZZLE_SIDE-1-g_W].right;
    if (!color_is_edge_iface(left_need)) return;
    uint64_t used[4]; memcpy(used, g_placed, sizeof used);
    int role = corner_role_at(row);
    int pin_corner = g_pin_corner[role];
    int fixed_spin = -1;
    if (g_has[row][PUZZLE_SIDE-1]) {
        const Oriented *fixed = &g_grid[row][PUZZLE_SIDE-1];
        if (pin_corner >= 0 && pin_corner != fixed->piece_id) return;
        pin_corner = fixed->piece_id;
        fixed_spin = fixed->rotation;
        used_clear(used, fixed->piece_id);
    }
    BorderChain cur; memset(&cur, 0, sizeof cur);
    int inner[MAX_W];
    bc_dfs(0, left_need, frame_side, pin_corner,
           used, &cur, inner, out, n, cap);
    if (fixed_spin >= 0) {
        size_t keep = 0;
        for (size_t i = 0; i < *n; i++)
            if ((*out)[i].spin[chain_w()-1] == (uint8_t)fixed_spin)
                (*out)[keep++] = (*out)[i];
        *n = keep;
    }
}

/* Closure chains are sorted by signature so try_close() can binary-search them. */
static int cmp_bc_sig(const void *a, const void *b) {
    uint32_t x = ((const BorderChain *)a)->sig, y = ((const BorderChain *)b)->sig;
    return (x > y) - (x < y);
}

static int cmp_bc_reach(const void *a, const void *b) {
    const BorderChain *x = a, *y = b;
    if (x->reach != y->reach) return (x->reach < y->reach) ? 1 : -1;
    return cmp_bc_sig(a, b);
}

/* -- The oracle ------------------------------------------------------------ */
/* Backward color reachability. Piece identities are ignored here and enforced
 * by DFS, so false states are safe to prune. */

static uint64_t *g_live[MAX_ROUNDS+2][MAX_LEVEL+2];
static uint8_t  *g_reach[MAX_ROUNDS+2][MAX_LEVEL+2];
static int       g_wall[MAX_ROUNDS+2][MAX_LEVEL+2];
static int       g_start_level[MAX_ROUNDS+2];         /* first DB level searched */
static uint32_t  g_start_sig[MAX_ROUNDS+2];           /* top colors of fixed prefix */
static int       g_prefix_n[MAX_ROUNDS+2];             /* complete fixed levels */
static int       g_suffix_n[MAX_ROUNDS+2];             /* complete fixed far levels */
static int       g_top_level[MAX_ROUNDS+2];            /* last DB level */
static bool      g_close_top[MAX_ROUNDS+2];            /* closure row follows */
static bool      g_relaxed_hold[MAX_ROUNDS+2];         /* open seam before held half */
static bool      g_end_fixed[MAX_ROUNDS+2];            /* exact signature at far seam */
static uint32_t  g_end_sig[MAX_ROUNDS+2];
static bool      g_depth_oracle[MAX_ROUNDS+2];          /* final side: maximize prefix */
static int16_t   g_pin_term_pid[MAX_ROUNDS+2][MAX_LEVEL+2];
static int8_t    g_pin_term_spin[MAX_ROUNDS+2][MAX_LEVEL+2];
static uint64_t  g_live_n[MAX_ROUNDS+2][MAX_LEVEL+2];
static size_t    g_live_words = 0;

static inline bool live_test(const uint64_t *bs, uint32_t s) {
    return (bs[s >> 6] >> (s & 63)) & 1u;
}

/* (Re)size the per-level bitsets for the current signature space. Called once
   per strip; W (and so s_nsig) can change between input boards. */
static void oracle_alloc(int round) {
    g_live_words = (s_nsig + 63) / 64;
    for (int rd = 0; rd <= MAX_ROUNDS+1; rd++)
        for (int r = 0; r <= MAX_LEVEL+1; r++)
            g_live[rd][r] = xrealloc(g_live[rd][r], g_live_words * sizeof(uint64_t));

    /* Only the unrestricted final side uses depth values.  Unlike the endpoint
       bitsets, no earlier recursion level needs to preserve this table. */
    if (g_depth_oracle[round])
        for (int r = 0; r <= MAX_LEVEL+1; r++)
            g_reach[round][r] = xrealloc(g_reach[round][r],
                                          (size_t)s_nsig * sizeof(uint8_t));
}

/* Sweep levels lo..hi. Threads own complete bitset words, avoiding atomics. */
static void oracle_backward(int round, int lo, int hi) {
    if (lo > hi) return;
    uint64_t level_count = 0;

    /* One persistent team for the whole backward sweep.  The implicit barrier at
       the end of each omp-for is the level dependency: r reads the completed
       bitset at r+1.  This avoids creating a thread team once per level. */
    #pragma omp parallel num_threads(g_nthreads) shared(level_count)
    {
        for (int r = hi; r >= lo; r--) {
            uint64_t *live = g_live[round][r];
            const uint64_t *above = g_live[round][r+1];
            const int wall = g_wall[round][r];

            #pragma omp single
            level_count = 0;

            if (!color_is_inner(wall)) {
                #pragma omp for schedule(static)
                for (long w = 0; w < (long)g_live_words; w++) live[w] = 0;
                #pragma omp single
                g_live_n[round][r] = 0;
                continue;
            }

            const uint64_t base = (uint64_t)INNER_IDX(wall) * s_nsig;
            const int pin_pid = g_pin_term_pid[round][r];
            const int pin_spin = g_pin_term_spin[round][r];
            /* Dead signatures scan whole cells while live ones stop at their first
               survivor, so dynamic word scheduling balances the irregular work. */
            #pragma omp for schedule(dynamic, 8) reduction(+:level_count)
            for (long w = 0; w < (long)g_live_words; w++) {
                uint64_t bits = 0;
                for (int b = 0; b < 64; b++) {
                    uint32_t sig = (uint32_t)(w*64 + b);
                    if (sig >= s_nsig) break;
                    uint64_t fi = base + sig;
                    const Cell *cell = s_db[fi];
                    if (!cell) continue;
                    uint8_t bot[MAX_W];
                    if (!s_succ || pin_pid >= 0) sig_bottoms(sig, bot);
                    uint32_t off = s_succ ? s_rec_off[fi] : 0;
                    for (uint32_t j = 0; j < cell->n; j++) {
                        uint16_t ci[MAX_W]; int term = -1; uint32_t succ;
                        if (s_succ) {
                            succ = s_succ[off+j];
                            if (succ == UINT32_MAX || !live_test(above, succ)) continue;
                        }
                        if (!s_succ || pin_pid >= 0) {
                            if (!rh_decode(cell, j, wall, bot, ci, &term, &succ)) continue;
                            if (!live_test(above, succ)) continue;
                        }
                        if (pin_pid >= 0 &&
                            (g_edge_term[term].piece_id != (uint16_t)pin_pid ||
                             g_edge_term[term].rotation != (uint8_t)pin_spin))
                            continue;
                        bits |= 1ULL << b;
                        break;
                    }
                }
                live[w] = bits;
                level_count += (uint64_t)__builtin_popcountll(bits);
            }
            #pragma omp single
            g_live_n[round][r] = level_count;
        }
    }
}

/* Seed the level above the last database level: the closure chains' required
   signatures, or "anything goes" when the strip stops below the top border. */
static void oracle_seed_top(int round) {
    int t = g_top_level[round] + 1;
    uint64_t *live = g_live[round][t];
    if (g_end_fixed[round]) {
        memset(live, 0, g_live_words * sizeof(uint64_t));
        uint32_t s = g_end_sig[round];
        live[s >> 6] |= 1ULL << (s & 63);
        return;
    }
    if (!g_close_top[round]) {
        for (size_t i = 0; i < g_live_words; i++) live[i] = ~0ULL;
        return;
    }
    memset(live, 0, g_live_words * sizeof(uint64_t));
    for (size_t i = 0; i < g_close_n[round]; i++) {
        uint32_t s = g_close[round][i].sig;
        live[s >> 6] |= 1ULL << (s & 63);
    }
}

/* Maximum relaxed depth for an unrestricted final side.  Unlike the endpoint
   bitset above, this table never requires a branch to reach the far border.
   g_reach[r][sig] is the maximum number of additional whole strip levels that
   can be placed from (r,sig) when pieces may be reused.  A top closure counts
   as one level; a fixed far suffix must match if the search reaches it. */
static void depth_seed_top(int round) {
    int t = g_top_level[round] + 1;
    uint8_t *reach = g_reach[round][t];
    memset(reach, 0, (size_t)s_nsig * sizeof(uint8_t));
    if (!g_close_top[round]) return;
    for (size_t i = 0; i < g_close_n[round]; i++)
        reach[g_close[round][i].sig] = 1;
}

static void depth_backward(int round, int lo, int hi) {
    if (lo > hi) return;
    #pragma omp parallel num_threads(g_nthreads)
    {
        for (int r = hi; r >= lo; r--) {
            uint8_t *reach = g_reach[round][r];
            const uint8_t *above = g_reach[round][r+1];
            const int wall = g_wall[round][r];

            if (!color_is_inner(wall)) {
                #pragma omp for schedule(static)
                for (long s = 0; s < (long)s_nsig; s++) reach[s] = 0;
                continue;
            }

            const uint64_t base = (uint64_t)INNER_IDX(wall) * s_nsig;
            const int pin_pid = g_pin_term_pid[round][r];
            const int pin_spin = g_pin_term_spin[round][r];
            const uint8_t cap = (uint8_t)(hi - r + 1 + (g_close_top[round] ? 1 : 0));

            #pragma omp for schedule(dynamic, 64)
            for (long ss = 0; ss < (long)s_nsig; ss++) {
                uint32_t sig = (uint32_t)ss;
                uint64_t fi = base + sig;
                const Cell *cell = s_db[fi];
                uint8_t best = 0;
                if (!cell) { reach[sig] = 0; continue; }

                uint8_t bot[MAX_W];
                if (!s_succ || pin_pid >= 0) sig_bottoms(sig, bot);
                uint32_t off = s_succ ? s_rec_off[fi] : 0;
                for (uint32_t j = 0; j < cell->n; j++) {
                    uint16_t ci[MAX_W]; int term = -1; uint32_t succ;
                    if (s_succ) {
                        succ = s_succ[off+j];
                        if (succ == UINT32_MAX) continue;
                    }
                    if (!s_succ || pin_pid >= 0) {
                        if (!rh_decode(cell, j, wall, bot, ci, &term, &succ)) continue;
                    }
                    if (pin_pid >= 0 &&
                        (g_edge_term[term].piece_id != (uint16_t)pin_pid ||
                         g_edge_term[term].rotation != (uint8_t)pin_spin))
                        continue;
                    if (r == hi && g_end_fixed[round] && succ != g_end_sig[round])
                        continue;
                    uint8_t cand = (uint8_t)(1 + above[succ]);
                    if (cand > best) best = cand;
                    if (best == cap) break;
                }
                reach[sig] = best;
            }
        }
    }
}

/* Live signatures at one level, for the log. */
static uint64_t live_count(int round, int level) {
    return g_live_n[round][level];
}

/* -- Parity / supply prune ------------------------------------------------- */
/* Exact whenever the strip is the last unfilled region: then every side of every
 * unplaced piece either faces a known boundary -- the frontier below, one wall
 * color per remaining level, or the frame -- or pairs with another unplaced
 * piece inside the strip. So for every inner color the surplus must be
 * non-negative, and even. */

static int g_wall_suffix[MAX_ROUNDS+2][MAX_LEVEL+2][NUM_COLORS_TOTAL];
static int g_cells_above[MAX_ROUNDS+2][MAX_LEVEL+2];

static void parity_prepare(int round) {
    memset(g_wall_suffix[round], 0, sizeof g_wall_suffix[round]);
    for (int r = g_top_level[round]; r >= 0; r--) {
        memcpy(g_wall_suffix[round][r], g_wall_suffix[round][r+1],
               sizeof g_wall_suffix[round][r]);
        if (color_is_inner(g_wall[round][r])) g_wall_suffix[round][r][g_wall[round][r]]++;
    }
    for (int r = 0; r <= MAX_LEVEL+1; r++) {
        int lv = g_top_level[round] - r + 1; if (lv < 0) lv = 0;
        g_cells_above[round][r] = lv * chain_w() + (g_close_top[round] ? chain_w() : 0);
    }
}

/* Sound only when the strip is the last unfilled region, which is exactly the
   `cells remaining == pieces unplaced` test on the first line -- otherwise the
   unplaced pool also has to feed regions this strip knows nothing about, and
   nothing can be concluded. */
static bool parity_ok(int round, int level, uint32_t sig) {
    /* This certificate assumes the whole remaining strip must be filled.  An
       unrestricted final side is optimizing prefix depth, so failure to close
       the far end says nothing about whether another exact level is useful. */
    if (g_depth_oracle[round] || g_relaxed_hold[round] || g_rounds == 4) return true;
    if (g_cells_above[round][level] != NUM_PIECES - g_n_placed) return true;
    /* Nothing above still to place: there are no sides left to pair off, and the
       junction `sig` names is between two levels that are already down. Only a
       held block reaches this level with the strip complete -- an open top means
       a later round still has cells, so the test above has already returned. */
    if (!g_cells_above[round][level]) return true;
    const int W = chain_w();
    uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
    int need[NUM_COLORS_TOTAL] = {0};
    for (int i = 0; i < W - 1; i++) need[bot[i]]++;
    for (int c = COLOR_MIN; c <= COLOR_MAX; c++) {
        int s = g_avail[c] - need[c] - g_wall_suffix[round][level][c];
        if (s < 0 || (s & 1)) return false;
    }
    return true;
}

/* -- The furthest it got --------------------------------------------------- */
/* The whole output of a run. Depth is PIECES ON THE BOARD -- the one measure
 * comparable across rounds, unlike a per-strip level, which restarts each time.
 * Boards are kept in the input's orientation, so taking a snapshot never has to
 * disturb the live grid. */

typedef struct {
    int pos[NUM_PIECES], rot[NUM_PIECES];
    uint16_t order[NUM_PIECES];
    int placed;
} Snap;

static Snap g_input_snap;

static Snap    *g_best      = NULL;
static uint32_t g_best_n    = 0, g_best_cap = 0;
static int      g_best_depth = -1;

static void best_reset(void) { g_best_n = 0; g_best_depth = -1; }

/* Snapshot in the input orientation. order[] is zero for retained pieces and
   otherwise records when the piece entered the current DFS path. */
static void snapshot(Snap *s) {
    for (int i = 0; i < NUM_PIECES; i++) {
        s->pos[i] = 999; s->rot[i] = 0; s->order[i] = 0;
    }
    s->placed = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!g_has[r][c]) continue;
            int ar, ac; frame_to_orig(r, c, g_rot_applied, &ar, &ac);
            uint16_t pid = g_grid[r][c].piece_id;
            s->pos[pid] = ar*PUZZLE_SIDE + ac;
            s->rot[pid] = (g_grid[r][c].rotation + g_rot_applied) & 3;
            s->order[pid] = g_order[r][c];
            s->placed++;
        }
}

static bool snap_same(const Snap *a, const Snap *b) {
    return memcmp(a->pos, b->pos, sizeof a->pos) == 0 &&
           memcmp(a->rot, b->rot, sizeof a->rot) == 0;
}

/* Keep ties only when they differ behind the last g_tie_depth complete chain
   levels. This removes cosmetic last-level variants while retaining boards that
   changed earlier rounds or genuinely deep pieces. */
static bool best_is_new(const Snap *s) {
    uint16_t newest_s = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (s->order[p] > newest_s) newest_s = s->order[p];
    uint64_t separation = (uint64_t)g_tie_depth * (uint64_t)g_W;
    int cut_s = separation > (uint64_t)INT_MAX ? INT_MIN :
                (int)newest_s - (int)separation;

    for (uint32_t k = 0; k < g_best_n; k++) {
        const Snap *b = &g_best[k];
        if (snap_same(s, b)) return false;
        uint16_t newest_b = 0;
        for (int p = 0; p < NUM_PIECES; p++)
            if (b->order[p] > newest_b) newest_b = b->order[p];
        int cut_b = separation > (uint64_t)INT_MAX ? INT_MIN :
                    (int)newest_b - (int)separation;
        bool deep = false;
        for (int p = 0; p < NUM_PIECES && !deep; p++) {
            if (s->pos[p] == b->pos[p] && s->rot[p] == b->rot[p]) continue;
            if ((s->pos[p] != 999 && (int)s->order[p] <= cut_s) ||
                (b->pos[p] != 999 && (int)b->order[p] <= cut_b))
                deep = true;
        }
        if (!deep) return false;
    }
    return true;
}

/* Offer the current board as the furthest reached. Cheap enough to call from
   every dead end: in the common case it compares two ints and returns. */
static bool offer_board(void) {
    if (g_n_placed < g_best_depth) return false;
    if (g_n_placed > g_best_depth) { best_reset(); g_best_depth = g_n_placed; }
    if (g_best_n >= g_ties) return false;
    if (g_best_n == g_best_cap) {
        g_best_cap = g_best_cap ? g_best_cap*2 : 8;
        g_best = xrealloc(g_best, g_best_cap * sizeof(Snap));
    }
    snapshot(&g_best[g_best_n]);
    if (!best_is_new(&g_best[g_best_n])) return false;
    g_best_n++;
    return true;
}

/* -- Emission -------------------------------------------------------------- */

/* Matched junctions out of 480, and separately the mismatched ones. Both count
   only placed-placed pairs, so an empty cell costs score without ever counting
   as a break. */
static void score_snap(const Snap *s, int *matched, int *breaks) {
    Oriented o[NUM_PIECES]; bool has[NUM_PIECES];
    memset(has, 0, sizeof has);
    for (int p = 0; p < NUM_PIECES; p++)
        if (s->pos[p] != 999) {
            o[s->pos[p]] = rh_oriented((uint16_t)p, (uint8_t)s->rot[p]);
            has[s->pos[p]] = true;
        }
    *matched = 0; *breaks = 0;
    for (int cell = 0; cell < NUM_PIECES; cell++) {
        int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
        if (c+1 < PUZZLE_SIDE && has[cell] && has[cell+1])
            ((o[cell].right == o[cell+1].left) ? (*matched)++ : (*breaks)++);
        if (r+1 < PUZZLE_SIDE && has[cell] && has[cell+PUZZLE_SIDE])
            ((o[cell].top == o[cell+PUZZLE_SIDE].bottom) ? (*matched)++ : (*breaks)++);
    }
}

/* Changes in the physical top W rows of the input board.  A piece is "new" when
   its id was not in that band initially; it is "removed" when an initial top-band
   piece no longer belongs to the band.  This measures fresh piece flow from the
   lower board rather than merely counting newly occupied cells. */
typedef struct {
    int before, after, entered, removed;
} TopBandChange;

/* Normal mode prints exactly one physical line per input board.  Emission may
   still write several deepest ties, so keep one representative (best score,
   then fewest mismatches) plus the number of rows written. */
typedef struct {
    uint32_t search_written;
    uint32_t fill_written;
    bool     have_search;
    bool     have_fill;
    char     search_class[16];
    int      search_placed;
    int      search_matched;
    int      search_breaks;
    TopBandChange search_top;
    int      fill_placed;
    int      fill_matched;
    int      fill_breaks;
    TopBandChange fill_top;
    int      fill_rejected_breaks;
} BoardReport;

static BoardReport g_board_report;

static void board_report_reset(void) {
    memset(&g_board_report, 0, sizeof g_board_report);
    g_board_report.fill_rejected_breaks = -1;
}

static TopBandChange top_band_change(const Snap *s) {
    TopBandChange d = {0, 0, 0, 0};
    const int first_top_row = PUZZLE_SIDE - g_W;
    for (int p = 0; p < NUM_PIECES; p++) {
        bool before = g_input_snap.pos[p] != 999 &&
                      g_input_snap.pos[p] / PUZZLE_SIDE >= first_top_row;
        bool after  = s->pos[p] != 999 &&
                      s->pos[p] / PUZZLE_SIDE >= first_top_row;
        d.before += before;
        d.after += after;
        d.entered += after && !before;
        d.removed += before && !after;
    }
    return d;
}

/* --hold_band is an immutability promise, not a search preference.  Verify it
   on every emitted snapshot so a coordinate error in either direction cannot
   silently produce a board that discarded the previous pass. */
static void verify_hold_snapshot(const Snap *s) {
    if (!g_hold_active) return;
    for (int p = 0; p < NUM_PIECES; p++) {
        if (!g_hold_expected[p]) continue;
        if (s->pos[p] == g_hold_expected_pos[p] &&
            s->rot[p] == (int)g_hold_expected_rot[p]) continue;
        fatal("--hold_band invariant failed for piece %d: expected cell=%d spin=%u, "
              "found cell=%d spin=%d", p, g_hold_expected_pos[p],
              g_hold_expected_rot[p], s->pos[p], s->rot[p]);
    }
}

/* Write one canonical puzzle row to the explicit output file.  Clean search
   boards, open-seam hold joins and greedy fills share the file; their provenance
   tags and the concise stdout class keep them distinguishable. */
static void emit_snap(const Snap *snap_in, const char *tag) {
    verify_hold_snapshot(snap_in);

    /* score_snap must use the same seed space as the search.  Under --cw both
       the input snapshot and the seed are mirrored; the CSV is un-mirrored only
       after scoring and after the hold invariant has been checked. */
    int matched, breaks;
    score_snap(snap_in, &matched, &breaks);

    const bool joined = breaks > 0 && g_hold_active && tag[0] != 'f';
    const char *class_name = joined ? "hold-join" :
                             tag[0] == 'f' ? "break-fill" : "clean";
    const char *id_tag = joined ? "j" : tag;

    Snap mirrored;
    const Snap *s = snap_in;
    if (g_reverse) {
        mirrored = *snap_in;
        mirror_line(mirrored.pos, mirrored.rot);
        s = &mirrored;
    }

    char id[128];
    snprintf(id, sizeof id, "%s_%u%s%" PRIu64,
             g_in_id, g_line_id, id_tag, g_emitted);

    fprintf(g_out_fp, "%s, %d", id, matched);
    for (int i = 0; i < NUM_PIECES; i++) fprintf(g_out_fp, ", %d", s->pos[i]);
    for (int i = 0; i < NUM_PIECES; i++) fprintf(g_out_fp, ", %d", s->rot[i]);
    fputc('\n', g_out_fp);
    fflush(g_out_fp);

    TopBandChange top = top_band_change(snap_in);
    int changed = 0;
    for (int p = 0; p < NUM_PIECES; p++) {
        bool moved = snap_in->pos[p] != g_input_snap.pos[p];
        bool respun = snap_in->pos[p] != 999 && g_input_snap.pos[p] != 999 &&
                      snap_in->rot[p] != g_input_snap.rot[p];
        if (moved || respun) changed++;
    }

    if (tag[0] == 'f') {
        g_board_report.fill_written++;
        if (!g_board_report.have_fill || breaks < g_board_report.fill_breaks ||
            (breaks == g_board_report.fill_breaks && matched > g_board_report.fill_matched)) {
            g_board_report.have_fill = true;
            g_board_report.fill_placed = s->placed;
            g_board_report.fill_matched = matched;
            g_board_report.fill_breaks = breaks;
            g_board_report.fill_top = top;
        }
    } else {
        g_board_report.search_written++;
        if (!g_board_report.have_search ||
            s->placed > g_board_report.search_placed ||
            (s->placed == g_board_report.search_placed &&
             (breaks < g_board_report.search_breaks ||
              (breaks == g_board_report.search_breaks &&
               matched > g_board_report.search_matched)))) {
            g_board_report.have_search = true;
            snprintf(g_board_report.search_class, sizeof g_board_report.search_class,
                     "%s", class_name);
            g_board_report.search_placed = s->placed;
            g_board_report.search_matched = matched;
            g_board_report.search_breaks = breaks;
            g_board_report.search_top = top;
        }
    }

    if (g_verbose)
        printf("[emit] line=%u id=%s placed=%d net=%+d top=%d->%d new=%d "
               "removed=%d score=%d breaks=%d class=%s added_after_cut=%+d "
               "changed=%d held=%d\n",
               g_line_id, id, s->placed, s->placed - g_input_placed,
               top.before, top.after, top.entered, top.removed,
               matched, breaks, class_name, s->placed - g_base_kept, changed,
               g_hold_active ? g_hold_kept : 0);

    g_emitted++;
    if      (tag[0] == 'f')       g_stats.emit_filled++;
    else if (joined)              g_stats.emit_joined++;
    else if (s->placed == NUM_PIECES) g_stats.emit_solved++;
    else                          g_stats.emit_deepest++;
}

/* -- Strip search ---------------------------------------------------------- */
/* Serial DFS across all requested sides. Oracle/database work remains parallel;
 * keeping board mutation serial makes nested rotations deterministic. */

static void run_round(int round);

/* Place only the empty cells of a chain level. The returned bit mask makes
   backtracking leave fixed prefixes, terminal anchors and held cells untouched. */
static uint16_t level_place(int level, const uint16_t pid[], const uint8_t spin[]) {
    const int W = chain_w();
    uint16_t added = 0;
    for (int k = 0; k < W; k++) {
        int c = PUZZLE_SIDE - W + k;
        if (g_has[level][c]) continue;
        g_grid[level][c] = rh_oriented(pid[k], spin[k]);
        g_has[level][c] = true;
        g_order[level][c] = (uint16_t)(g_n_placed - g_base_kept + 1);
        take_piece(pid[k]);
        added |= (uint16_t)(1u << k);
    }
    return added;
}
static void level_remove(int level, uint16_t added) {
    const int W = chain_w();
    for (int k = 0; k < W; k++) {
        if (!(added & (uint16_t)(1u << k))) continue;
        int c = PUZZLE_SIDE - W + k;
        g_has[level][c] = false;
        g_order[level][c] = 0;
        drop_piece(g_grid[level][c].piece_id);
    }
}

/* A strip has reached its target: either the spiral is done, or the board is
   rotated and handed to the next round. Recorded either way, so a board is never
   lost because the NEXT round happened to be refuted at once. */
static void strip_done(int round) {
    bool accepted = offer_board();
    if (round >= g_rounds) {
        if (accepted) {
            if (g_n_placed > g_endpoint_depth) {
                g_endpoint_depth = g_n_placed;
                g_endpoint_hits = 0;
                g_first_endpoint_node = g_nodes;
            }
            if (g_n_placed == g_endpoint_depth) {
                if (g_endpoint_hits == 0) g_first_endpoint_node = g_nodes;
                g_endpoint_hits++;
                if (g_target_ties > 0 && g_endpoint_hits >= g_target_ties)
                    g_target_stop = true;
            }
        }
        return;
    }
    Oriented sg[PUZZLE_SIDE][PUZZLE_SIDE]; bool sh[PUZZLE_SIDE][PUZZLE_SIDE];
    uint16_t so[PUZZLE_SIDE][PUZZLE_SIDE];
    int srot = g_rot_applied;
    memcpy(sg, g_grid, sizeof sg); memcpy(sh, g_has, sizeof sh);
    memcpy(so, g_order, sizeof so);
    rotate_cw();
    run_round(round+1);
    memcpy(g_grid, sg, sizeof g_grid); memcpy(g_has, sh, sizeof g_has);
    memcpy(g_order, so, sizeof g_order);
    g_rot_applied = srot;
}

/* Close the strip's top row from the precomputed chains (sorted by signature). */
static void try_close(int round, uint32_t sig) {
    const int W = chain_w();
    const BorderChain *cl = g_close[round];
    size_t lo = 0, hi = g_close_n[round];
    while (lo < hi) { size_t m = (lo+hi)/2; if (cl[m].sig < sig) lo = m+1; else hi = m; }
    bool any = false;
    for (size_t i = lo; i < g_close_n[round] && cl[i].sig == sig; i++) {
        bool clash = false;
        for (int k = 0; k < W && !clash; k++) {
            int c = PUZZLE_SIDE - W + k;
            if (g_has[g_top_level[round]+1][c]) {
                const Oriented *o = &g_grid[g_top_level[round]+1][c];
                clash = o->piece_id != cl[i].pid[k] || o->rotation != cl[i].spin[k];
            } else {
                clash = used_test(g_placed, cl[i].pid[k]);
            }
        }
        if (clash) continue;
        uint16_t added = level_place(g_top_level[round]+1, cl[i].pid, cl[i].spin);
        strip_done(round);
        level_remove(g_top_level[round]+1, added);
        any = true;
        if (g_target_stop) break;
        if (budget_spent()) break;
    }
    if (!any) offer_board();
}

/* Fill levels bottom-up. Per record, in the order that rejects most cheaply:
   decode, oracle bitset test (dead on colour -- one lookup), piece disjointness,
   parity/supply. `advanced` records whether any child survived, so a node that
   rejects everything reports itself as a dead end. */
static void strip_dfs(int round, int level, uint32_t sig) {
    const int W = chain_w();
    if (g_target_stop) return;
    if (budget_spent()) { g_truncated = true; return; }
    if (level > g_top_level[round]) {
        if (g_close_top[round]) try_close(round, sig);
        else strip_done(round);
        return;
    }

    uint64_t fi = (uint64_t)INNER_IDX(g_wall[round][level]) * s_nsig + sig;
    const Cell *cell = s_db[fi];
    if (!cell) { offer_board(); return; }
    uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
    bool advanced = false;
    uint32_t rec_base = s_succ ? s_rec_off[fi] : 0;

    for (uint32_t j = 0; j < cell->n; j++) {
        if (g_target_stop) break;
        if (budget_spent()) { g_truncated = true; break; }
        g_nodes++;
        if (s_succ) {
            uint32_t cached = s_succ[rec_base+j];
            if (cached == UINT32_MAX) continue;
            if (!g_depth_oracle[round] &&
                !live_test(g_live[round][level+1], cached)) continue;
        }
        uint16_t ci[MAX_W]; int term; uint32_t succ;
        if (!rh_decode(cell, j, g_wall[round][level], bot, ci, &term, &succ)) continue;
        if (!s_succ && !g_depth_oracle[round] &&
            !live_test(g_live[round][level+1], succ)) continue;
        if (g_depth_oracle[round]) {
            /* A fixed suffix may be approached without promising to reach it,
               but the last adjacent level is legal only when its seam matches. */
            if (level == g_top_level[round] && g_end_fixed[round] &&
                succ != g_end_sig[round])
                continue;
            int max_levels = 1 + (int)g_reach[round][level+1][succ];
            if (g_n_placed + max_levels * W < g_best_depth) continue;
        }

        /* A record never repeats a piece within itself -- db_dfs carries a used
           mask -- so only the levels below have to be tested. */
        uint16_t pids[MAX_W]; uint8_t spins[MAX_W];
        bool clash = false;
        for (int i = 0; i < W - 1 && !clash; i++) {
            pids[i]  = g_cat[ci[i]].piece_id;
            spins[i] = g_cat[ci[i]].rotation;
            clash = g_has[level][PUZZLE_SIDE-W+i] || used_test(g_placed, pids[i]);
        }
        if (clash) continue;
        pids[W-1]  = g_edge_term[term].piece_id;
        spins[W-1] = g_edge_term[term].rotation;
        if (g_pin_term_pid[round][level] >= 0) {
            if (pids[W-1] != (uint16_t)g_pin_term_pid[round][level] ||
                spins[W-1] != (uint8_t)g_pin_term_spin[round][level]) continue;
            const Oriented *fixed = &g_grid[level][PUZZLE_SIDE-1];
            if (!g_has[level][PUZZLE_SIDE-1] || fixed->piece_id != pids[W-1] ||
                fixed->rotation != spins[W-1]) continue;
        } else if (g_has[level][PUZZLE_SIDE-1] || used_test(g_placed, pids[W-1])) {
            continue;
        }

        /* Clue guards, two of them and both needed. A cell that owes a clue must
           receive exactly that piece at exactly that spin; and a clue piece may
           not be laid anywhere but its own cell -- the analogue of the beamer's
           database exclusion, without which round 1 could spend a piece round 3
           still needs and refute a strip that is actually fine. */
        if (g_clue_mask && g_rh_orient >= 0) {
            bool bad = false;
            for (int i = 0; i < W && !bad; i++) {
                int spin_need;
                const ClueCell *cc = rh_clue_at_frame(level, PUZZLE_SIDE - W + i, &spin_need);
                if (cc) bad = (pids[i] != cc->piece || spins[i] != (uint8_t)spin_need);
                else    bad = rh_is_clue_piece(pids[i]);
            }
            if (bad) continue;
        }

        uint16_t added = level_place(level, pids, spins);
        if (parity_ok(round, level+1, succ)) {
            advanced = true;
            strip_dfs(round, level+1, succ);
        }
        level_remove(level, added);
        if (g_target_stop) break;
        if (budget_spent()) { g_truncated = true; break; }
    }
    if (!advanced) offer_board();
}

/* -- Strip geometry -------------------------------------------------------- */

static int level_occupancy(int level) {
    int n = 0;
    for (int c = PUZZLE_SIDE-g_W; c < PUZZLE_SIDE; c++) n += g_has[level][c] ? 1 : 0;
    return n;
}

/* Signature exposed above an already-complete strip level. */
static bool fixed_level_signature(int level, uint32_t *out) {
    int inner[MAX_W];
    for (int k = 0; k < chain_w()-1; k++) {
        int color = g_grid[level][PUZZLE_SIDE-g_W+k].top;
        if (!color_is_inner(color)) return false;
        inner[k] = INNER_IDX(color);
    }
    int iface = g_grid[level][PUZZLE_SIDE-1].top;
    if (!color_is_edge_iface(iface)) return false;
    *out = sig_make(inner, iface);
    return true;
}

/* Signature required immediately below an already-complete far level. */
static bool fixed_level_bottom_signature(int level, uint32_t *out) {
    int inner[MAX_W];
    for (int k = 0; k < chain_w()-1; k++) {
        int color = g_grid[level][PUZZLE_SIDE-g_W+k].bottom;
        if (!color_is_inner(color)) return false;
        inner[k] = INNER_IDX(color);
    }
    int iface = g_grid[level][PUZZLE_SIDE-1].bottom;
    if (!color_is_edge_iface(iface)) return false;
    *out = sig_make(inner, iface);
    return true;
}

/* Derive the active right strip in the current frame. Ordinary rounds start
   with a free border chain. --rounds 4 may instead inherit complete prefix
   levels and, on its final side, fixed edge terminals. */
static bool strip_geometry(int round) {
    const int W = chain_w();
    const int wall_col = PUZZLE_SIDE - 1 - W;
    memset(g_pin_term_pid[round], 0xFF, sizeof g_pin_term_pid[round]);
    memset(g_pin_term_spin[round], 0xFF, sizeof g_pin_term_spin[round]);
    g_relaxed_hold[round] = false;
    g_prefix_n[round] = 0;
    g_suffix_n[round] = 0;
    g_start_sig[round] = 0;
    g_end_fixed[round] = false;
    g_end_sig[round] = 0;
    g_depth_oracle[round] = false;

    int wall_top = -1;
    for (int r = 0; r < PUZZLE_SIDE && g_has[r][wall_col]; r++) wall_top = r;
    if (wall_top < 1) return false;

    int prefix = 0;
    while (prefix <= wall_top && level_occupancy(prefix) == W) prefix++;
    if (g_rounds == 4) {
        /* Side 1 starts on the retained bottom anchor. Later sides may inherit
           a complete corner prefix or start from a fresh border chain,
           depending on whether the preceding side reached that corner. */
        if (round == 1 && prefix != 1) return false;
    } else if (prefix != 0) {
        return false;
    }

    int suffix_start = PUZZLE_SIDE;
    if (g_rounds == 4 && round == 4) {
        while (suffix_start > prefix && level_occupancy(suffix_start-1) == W)
            suffix_start--;
        g_suffix_n[round] = PUZZLE_SIDE - suffix_start;
    }

    /* Validate occupied cells between the fixed blocks. Held opposite-half
       cells are outside the active search by construction. */
    for (int r = prefix; r <= wall_top; r++) {
        int n = level_occupancy(r);
        if (g_hold_active && round == g_rounds && r >= HOLD_SPLIT) continue;
        if (g_rounds == 4 && round == 4 && r >= suffix_start) continue;
        if (g_rounds == 4 && round == 4 && n == 1 && g_has[r][PUZZLE_SIDE-1]) {
            g_pin_term_pid[round][r] = (int16_t)g_grid[r][PUZZLE_SIDE-1].piece_id;
            g_pin_term_spin[round][r] = (int8_t)g_grid[r][PUZZLE_SIDE-1].rotation;
            continue;
        }
        if (n != 0) return false;
    }

    int natural_last = (g_rounds == 4 && round == 4 && g_suffix_n[round])
                         ? suffix_start - 1 : wall_top;
    int last = natural_last;
    if (g_hold_active && round == g_rounds) {
        if (HOLD_SPLIT > natural_last) return false;
        last = HOLD_SPLIT - 1;
        g_relaxed_hold[round] = true;
    }

    /* Stop controls apply only to the final requested side. Earlier sides must
       finish or they cannot build the wall/prefix needed by the next rotation. */
    if (round == g_rounds) {
        if (g_stop_level >= 0 && g_stop_level < last) last = g_stop_level;
        if (g_stop_after >= 0) {
            int first_new = prefix ? prefix : 0; /* ordinary level 0 is newly chosen */
            int by_count = first_new + g_stop_after - 1;
            if (by_count < last) last = by_count;
        }
    }
    if (last < (prefix ? prefix : 0)) return false;

    g_prefix_n[round] = prefix;
    g_start_level[round] = prefix ? prefix : 1;
    if (prefix && !fixed_level_signature(prefix-1, &g_start_sig[round])) return false;
    if (g_suffix_n[round] && last == natural_last) {
        if (!fixed_level_bottom_signature(suffix_start, &g_end_sig[round])) return false;
        g_end_fixed[round] = true;
    }
    g_close_top[round] = (!g_end_fixed[round] && natural_last == PUZZLE_SIDE-1 &&
                          last == PUZZLE_SIDE-1);
    g_top_level[round] = g_close_top[round] ? PUZZLE_SIDE-2 : last;
    /* Earlier sides and explicit stop endpoints must be completed.  Otherwise
       the final side maximizes exact prefix depth.  In hold mode the retained
       half is a ceiling, not a required destination: a useful shorter prefix
       must not be rejected merely because it cannot reach the open seam. */
    g_depth_oracle[round] = (round == g_rounds &&
                             g_stop_level < 0 && g_stop_after < 0);

    for (int r = 0; r <= g_top_level[round]; r++) {
        if (!g_has[r][wall_col]) return false;
        g_wall[round][r] = g_grid[r][wall_col].right;
    }
    for (int r = g_top_level[round]+1; r <= MAX_LEVEL+1; r++) g_wall[round][r] = 0;
    return true;
}

/* -- One round ------------------------------------------------------------- */

static void do_strip(int round) {
    bool log = g_verbose && g_verbose_seen[round]++ == 0;
    if (!strip_geometry(round)) {
        if (log)
            printf("[round] n=%d side=%s status=NO_GEOMETRY placed=%d\n",
                   round, round_side(round), g_n_placed);
        offer_board();
        return;
    }
    oracle_alloc(round);
    g_stats.strips++;

    int first_new = g_prefix_n[round] ? g_prefix_n[round] : 0;
    int last = g_close_top[round] ? g_top_level[round] + 1 : g_top_level[round];
    if (log) {
        int sr0, sr1, sc0, sc1;
        box_to_orig(first_new, last, PUZZLE_SIDE-g_W, PUZZLE_SIDE-1,
                    g_rot_applied, &sr0, &sr1, &sc0, &sc1);
        printf("[round] n=%d side=%s levels=%d..%d prefix=%d suffix=%d closure=%d "
               "oracle=%s input_box=%d..%d,%d..%d hold=%d\n",
               round, round_side(round), first_new, last, g_prefix_n[round],
               g_suffix_n[round], g_close_top[round] ? 1 : 0,
               g_depth_oracle[round] ? "depth" : "endpoint", sr0, sr1, sc0, sc1,
               g_relaxed_hold[round] ? 1 : 0);
    }

    if (g_close_top[round]) {
        enumerate_border_row(PUZZLE_SIDE-1, 0,
                             &g_close[round], &g_close_n[round], &g_close_cap[round]);
        qsort(g_close[round], g_close_n[round], sizeof(BorderChain), cmp_bc_sig);
        if (!g_close_n[round] && !g_depth_oracle[round]) {
            if (log)
                printf("[round] n=%d side=%s status=NO_CLOSURE placed=%d\n",
                       round, round_side(round), g_n_placed);
            g_stats.strips_refuted++;
            g_stats.refuted[g_top_level[round]+1]++;
            offer_board();
            return;
        }
        if (!g_close_n[round] && log)
            printf("[round] n=%d side=%s closure=unavailable; prefixes remain searchable\n",
                   round, round_side(round));
    } else {
        g_close_n[round] = 0;
    }

    double t0 = omp_get_wtime();
    int collapse = 0;
    uint64_t min_live = UINT64_MAX;

    if (g_depth_oracle[round]) {
        depth_seed_top(round);
        if (g_start_level[round] <= g_top_level[round])
            depth_backward(round, g_start_level[round], g_top_level[round]);
        parity_prepare(round);

        if (g_prefix_n[round]) {
            int reach = g_start_level[round] > g_top_level[round]
                          ? 0 : g_reach[round][g_start_level[round]][g_start_sig[round]];
            if (log)
                printf("[oracle] round=%d mode=depth start=fixed max_levels=%d sec=%.3f\n",
                       round, reach, omp_get_wtime()-t0);
            strip_dfs(round, g_start_level[round], g_start_sig[round]);
            return;
        }

        enumerate_border_row(0, 2, &g_bc[round], &g_bc_n[round], &g_bc_cap[round]);
        size_t raw = g_bc_n[round];
        uint8_t max_reach = 0;
        for (size_t i = 0; i < raw; i++) {
            int after = g_reach[round][1][g_bc[round][i].sig];
            g_bc[round][i].reach = (uint8_t)(1 + after);
            if (g_bc[round][i].reach > max_reach) max_reach = g_bc[round][i].reach;
        }
        qsort(g_bc[round], raw, sizeof(BorderChain), cmp_bc_reach);
        size_t alive = raw;             /* every legal level-0 prefix is useful */

        if (log)
            printf("[oracle] round=%d mode=depth starts=%zu max_levels=%u sec=%.3f\n",
                   round, raw, max_reach, omp_get_wtime()-t0);
        if (!alive) {
            if (log)
                printf("[round] n=%d side=%s status=NO_START placed=%d\n",
                       round, round_side(round), g_n_placed);
            offer_board();
            return;
        }

        for (size_t b = 0; b < alive; b++) {
            if (g_target_stop) break;
            if (budget_spent()) { g_truncated = true; break; }
            const BorderChain *bc = &g_bc[round][b];
            if (g_n_placed + (int)bc->reach * chain_w() < g_best_depth) continue;
            uint16_t added = level_place(0, bc->pid, bc->spin);
            strip_dfs(round, 1, bc->sig);
            level_remove(0, added);
        }
        return;
    }

    oracle_seed_top(round);
    if (g_start_level[round] <= g_top_level[round])
        oracle_backward(round, g_start_level[round], g_top_level[round]);
    parity_prepare(round);

    for (int r = g_start_level[round]; r <= g_top_level[round]; r++) {
        uint64_t n = live_count(round, r);
        if (n < min_live) min_live = n;
        if (!n && !collapse) collapse = r;
    }
    if (min_live == UINT64_MAX) min_live = s_nsig;

    if (g_prefix_n[round]) {
        bool live = g_start_level[round] > g_top_level[round]
                      ? (!g_end_fixed[round] || g_start_sig[round] == g_end_sig[round])
                      : live_test(g_live[round][g_start_level[round]], g_start_sig[round]);
        if (log)
            printf("[oracle] round=%d mode=endpoint start=fixed live=%d min_states=%" PRIu64
                   " sec=%.3f\n", round, live ? 1 : 0, min_live,
                   omp_get_wtime()-t0);
        if (!live) {
            if (log)
                printf("[round] n=%d side=%s status=COLOR_DEAD level=%d placed=%d\n",
                       round, round_side(round), collapse ? collapse : g_start_level[round],
                       g_n_placed);
            g_stats.strips_refuted++;
            g_stats.refuted[collapse ? collapse : g_start_level[round]]++;
            offer_board();
            return;
        }
        strip_dfs(round, g_start_level[round], g_start_sig[round]);
        return;
    }

    enumerate_border_row(0, 2, &g_bc[round], &g_bc_n[round], &g_bc_cap[round]);
    size_t raw = g_bc_n[round], alive = 0;
    for (size_t i = 0; i < raw; i++)
        if (live_test(g_live[round][1], g_bc[round][i].sig))
            g_bc[round][alive++] = g_bc[round][i];
    g_bc_n[round] = alive;

    if (log)
        printf("[oracle] round=%d mode=endpoint starts=%zu/%zu min_states=%" PRIu64
               " sec=%.3f\n", round, alive, raw, min_live, omp_get_wtime()-t0);
    if (!alive) {
        if (log)
            printf("[round] n=%d side=%s status=COLOR_DEAD level=%d starts=0/%zu placed=%d\n",
                   round, round_side(round), collapse ? collapse : 1, raw, g_n_placed);
        g_stats.strips_refuted++;
        g_stats.refuted[collapse ? collapse : 1]++;
        offer_board();
        return;
    }

    for (size_t b = 0; b < alive; b++) {
        if (g_target_stop) break;
        if (budget_spent()) { g_truncated = true; break; }
        const BorderChain *bc = &g_bc[round][b];
        uint16_t added = level_place(0, bc->pid, bc->spin);
        strip_dfs(round, 1, bc->sig);
        level_remove(0, added);
    }
}

/* One round: bail on the budget, otherwise search this rotation's strip. */
static void run_round(int round) {
    if (round > g_rounds || g_target_stop) return;
    if (budget_spent()) { g_truncated = true; return; }
    do_strip(round);
}

/* -- Greedy break fill (--breaks) ------------------------------------------ */
/* No backtracking. Choose the cell with the cheapest available placement,
   breaking ties by the fewest equal-cost choices. Piece/cell frame-type counts
   stay balanced, so a legal seating always exists. */

/* Does piece `pid` at spin `spin` seat legally in cell (r,c)? Grey faces must
   fall exactly on the board's rim and nowhere else. */
static bool fits_frame(int pid, int spin, int r, int c) {
    Oriented o = rh_oriented((uint16_t)pid, (uint8_t)spin);
    return (o.top    == 0) == (r == PUZZLE_SIDE-1) &&
           (o.bottom == 0) == (r == 0) &&
           (o.left   == 0) == (c == 0) &&
           (o.right  == 0) == (c == PUZZLE_SIDE-1);
}

/* Mismatches a placement would create against the neighbours already down. */
static int placement_cost(const Oriented *o, int r, int c) {
    int cost = 0;
    if (c+1 < PUZZLE_SIDE && g_has[r][c+1] && o->right  != g_grid[r][c+1].left)   cost++;
    if (c   > 0           && g_has[r][c-1] && o->left   != g_grid[r][c-1].right)  cost++;
    if (r+1 < PUZZLE_SIDE && g_has[r+1][c] && o->top    != g_grid[r+1][c].bottom) cost++;
    if (r   > 0           && g_has[r-1][c] && o->bottom != g_grid[r-1][c].top)    cost++;
    return cost;
}

/* Fill every empty cell of the board in place and return the breaks it cost.
   It always runs to completion, even past --breaks: the caller wants the
   number either way, because "this board needs 61 breaks" is a far more useful
   answer than "more than 40". */
static int greedy_fill(void) {
    int spent = 0;
    for (;;) {
        int best_r = -1, best_c = -1, best_pid = -1, best_spin = 0;
        int best_cost = 5, best_ways = 0;
        for (int r = 0; r < PUZZLE_SIDE; r++)
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                if (g_has[r][c]) continue;
                int need = (r == 0) + (r == PUZZLE_SIDE-1) + (c == 0) + (c == PUZZLE_SIDE-1);
                int cell_cost = 5, ways = 0, pid_at = -1, spin_at = 0;
                /* A clue cell takes only its
                   own piece at its own spin; every other cell refuses clue pieces
                   outright, which also keeps one available for its cell. */
                int clue_spin = 0;
                const ClueCell *want_clue = rh_clue_at_frame(r, c, &clue_spin);
                for (int pid = 0; pid < NUM_PIECES; pid++) {
                    if (used_test(g_placed, (uint16_t)pid)) continue;
                    if (rh_zero_count(pid) != need) continue;
                    if (want_clue ? (pid != want_clue->piece) : rh_is_clue_piece((uint16_t)pid))
                        continue;
                    for (int spin = 0; spin < 4; spin++) {
                        if (want_clue && spin != clue_spin) continue;
                        if (!fits_frame(pid, spin, r, c)) continue;
                        Oriented o = rh_oriented((uint16_t)pid, (uint8_t)spin);
                        int cost = placement_cost(&o, r, c);
                        if (cost < cell_cost) {
                            cell_cost = cost; ways = 1; pid_at = pid; spin_at = spin;
                        } else if (cost == cell_cost) ways++;
                    }
                }
                if (pid_at < 0) continue;                  /* cannot happen: see above */
                if (cell_cost < best_cost ||
                    (cell_cost == best_cost && ways < best_ways)) {
                    best_cost = cell_cost; best_ways = ways;
                    best_r = r; best_c = c; best_pid = pid_at; best_spin = spin_at;
                }
            }
        if (best_r < 0) return spent;                      /* the board is full */
        g_grid[best_r][best_c] = rh_oriented((uint16_t)best_pid, (uint8_t)best_spin);
        g_has[best_r][best_c] = true;
        take_piece((uint16_t)best_pid);
        spent += best_cost;
    }
}

/* Load a snapshot back onto the grid (snapshots are in the input's orientation,
   so the fill works there and no rotation is involved). */
static void restore_snap(const Snap *s) {
    memset(g_has, 0, sizeof g_has);
    memset(g_order, 0, sizeof g_order);
    memset(g_placed, 0, sizeof g_placed);
    g_n_placed = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (s->pos[pid] == 999) continue;
        int r = s->pos[pid]/PUZZLE_SIDE, c = s->pos[pid]%PUZZLE_SIDE;
        g_grid[r][c] = rh_oriented((uint16_t)pid, (uint8_t)s->rot[pid]);
        g_has[r][c] = true;
        g_order[r][c] = s->order[pid];
        used_set(g_placed, (uint16_t)pid);
        g_n_placed++;
    }
    for (int c = 0; c < NUM_COLORS_TOTAL; c++) g_avail[c] = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (used_test(g_placed, (uint16_t)pid)) continue;
        int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
        for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
    }
    g_rot_applied = 0;
}

/* -- Input ----------------------------------------------------------------- */

static char *trim_field(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static bool parse_int_field(char *s, int *out) {
    s = trim_field(s);
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) end++;
    if (errno || end == s || !end || *end || v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

static bool parse_fields(char *s, char id_out[96], int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    size_t nf = 0, cap = 520;
    char **field = xmalloc(cap * sizeof(*field));
    char *p = s, *tok;
    while ((tok = strsep(&p, ",")) != NULL) {
        if (nf == cap) { cap *= 2; field = xrealloc(field, cap * sizeof(*field)); }
        field[nf++] = tok;
    }
    if (nf < 1u + 2u*NUM_PIECES) { free(field); return false; }
    if (id_out) snprintf(id_out, 96, "%s", trim_field(field[0]));

    size_t first = nf - 2u*NUM_PIECES;
    bool ok = true;
    for (int k = 0; k < 2*NUM_PIECES && ok; k++) {
        int v = 0;
        ok = parse_int_field(field[first + (size_t)k], &v);
        if (!ok) break;
        if (k < NUM_PIECES) pos[k] = v;
        else rot[k-NUM_PIECES] = v;
    }
    free(field);
    return ok;
}

/* Stream the next data row; blank/comment lines do not count. Any leading
   metadata is skipped because parse_fields uses only the final 512 fields. */
static bool read_next_board(FILE *f, char **line, size_t *cap,
                            uint32_t *data_index, char id_out[96],
                            int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    while (getline(line, cap, f) > 0) {
        char *q = *line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q=='\0'||*q=='\n'||*q=='\r'||*q=='#'||*q=='%') continue;
        uint32_t idx = (*data_index)++;
        if (!parse_fields(q, id_out, pos, rot))
            fatal("boards CSV data row %u has an invalid numeric tail (needs at "
                  "least %d fields including id)", idx, 1 + 2*NUM_PIECES);
        return true;
    }
    return false;
}

/* Data lines in a board CSV, so the banner can say what --start_row may ask
   for. Cheap next to everything else, and it turns "0 searched, 0 emitted" into
   a sentence the reader can act on. */
static uint32_t count_data_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open boards CSV %s: %s", path, strerror(errno));
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

/* Core dedup: two inputs agreeing on the retained core seed an identical search,
   whatever they do outside it. Hashed after the cut and the rotation, so the key
   covers exactly what the search will see. */
static uint64_t *g_dup = NULL; static size_t g_dup_sz = 0, g_dup_n = 0;

static bool dup_insert(uint64_t key) {
    if (!key) key = 1;
    if (g_dup_n*2 >= g_dup_sz) {
        size_t ns = g_dup_sz ? g_dup_sz*2 : 4096;
        uint64_t *nt = xmalloc(ns*sizeof(uint64_t));
        memset(nt, 0, ns*sizeof(uint64_t));
        for (size_t i = 0; i < g_dup_sz; i++) {
            if (!g_dup[i]) continue;
            size_t h = (size_t)(g_dup[i]*0x9E3779B97F4A7C15ULL) & (ns-1);
            while (nt[h]) h = (h+1) & (ns-1);
            nt[h] = g_dup[i];
        }
        free(g_dup); g_dup = nt; g_dup_sz = ns;
    }
    size_t h = (size_t)(key*0x9E3779B97F4A7C15ULL) & (g_dup_sz-1);
    while (g_dup[h]) { if (g_dup[h] == key) return false; h = (h+1) & (g_dup_sz-1); }
    g_dup[h] = key; g_dup_n++;
    return true;
}

/* Fingerprint of the cut board: piece and rotation per kept cell, plus W. */
static uint64_t core_hash(void) {
    uint64_t fp = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    fp ^= (uint64_t)g_W; fp *= prime;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint32_t v = g_has[r][c] ? (uint32_t)g_grid[r][c].piece_id*4u + g_grid[r][c].rotation : 0xFFFFu;
            fp ^= v; fp *= prime;
        }
    return fp;
}

/* Cells retained before the spiral starts, in frame coordinates. Rounds 1..3
   keep the original nested core. Round 4 keeps a centered core, the complete
   wall column for side 1, and the W-piece bottom-right anchor chain. */
static bool cell_kept(int r, int c, int W) {
    if (g_rounds == 4) {
        int hi = PUZZLE_SIDE - 1 - W;
        bool core = r >= W && r <= hi && c >= W && c <= hi;
        bool wall = c == hi;
        bool anchor = r == 0 && c >= PUZZLE_SIDE - W;
        return core || wall || anchor;
    }
    bool keep = c <= PUZZLE_SIDE-1-W;
    if (g_rounds >= 2) keep = keep && r <= PUZZLE_SIDE-1-W;
    if (g_rounds >= 3) keep = keep && c >= W;
    return keep;
}

static void turn_cell_cw(int r, int c, int turns, int *R, int *C) {
    turns &= 3;
    while (turns--) {
        int nr = PUZZLE_SIDE - 1 - c;
        c = r; r = nr;
    }
    *R = r; *C = c;
}

/* Position of an initial-frame cell on the FINAL side.  The input has already
   been mirrored for --cw and rotated by --rotate before this is called, so the
   same transform serves both directions.  Return -1 outside the final strip,
   0 in the half searched by this pass, and 1 in the opposite half retained by
   --hold_band. */
static int hold_half(int r, int c, int W) {
    if (g_rounds == 4 || cell_kept(r, c, W)) return -1;
    int R, C;
    turn_cell_cw(r, c, g_rounds-1, &R, &C);
    if (C < PUZZLE_SIDE-W) return -1;
    return R >= HOLD_SPLIT ? 1 : 0;
}

static void check_hold_partition(int W) {
    if (!g_hold_band || g_rounds == 4) return;
    int n[2] = {0,0};
    for (int r=0; r<PUZZLE_SIDE; r++)
        for (int c=0; c<PUZZLE_SIDE; c++) {
            int h = hold_half(r,c,W);
            if (h >= 0) n[h]++;
        }
    const int want = HOLD_SPLIT * W;
    if (n[0] != want || n[1] != want)
        fatal("internal hold mapping is asymmetric: searched=%d retained=%d expected=%d",
              n[0], n[1], want);
}

/* Bounding box of the retained shape in input coordinates, used only in logs. */
static void core_box_orig(int W, int *r0, int *r1, int *c0, int *c1) {
    *r0 = *c0 = PUZZLE_SIDE; *r1 = *c1 = -1;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!cell_kept(r, c, W)) continue;
            int ar, ac; frame_to_input(r, c, g_rot_applied, &ar, &ac);
            if (ar < *r0) *r0 = ar;
            if (ar > *r1) *r1 = ar;
            if (ac < *c0) *c0 = ac;
            if (ac > *c1) *c1 = ac;
        }
}

static bool piece_seats(const Oriented *o, int r, int c) {
    bool fb = r==0, ft = r==PUZZLE_SIDE-1, fl = c==0, fr = c==PUZZLE_SIDE-1;
    return rh_zero_count(o->piece_id) == (fb+ft+fl+fr) &&
           (!fb || o->bottom == 0) && (!ft || o->top == 0) &&
           (!fl || o->left == 0) && (!fr || o->right == 0);
}

/* The retained shape must be complete, legally seated and internally matched. */
static bool core_usable(int W, char why[128]) {
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!cell_kept(r, c, W)) continue;
            int ar, ac; frame_to_input(r, c, g_rot_applied, &ar, &ac);
            if (!g_has[r][c]) {
                if (why) snprintf(why, 128, "kept cell (%d,%d) is empty", ar, ac);
                return false;
            }
            const Oriented *o = &g_grid[r][c];
            if (!piece_seats(o, r, c)) {
                if (why) snprintf(why, 128, "piece %u does not fit kept cell (%d,%d)",
                                  o->piece_id, ar, ac);
                return false;
            }
            /* Both neighbours must be PLACED before their colours mean
               anything: g_grid is not cleared between boards, so comparing
               against a kept-but-empty cell reads the previous board and
               reports a break in a region that is merely unfilled. The scan
               reaches that cell on a later pass and names it correctly. */
            int nr = -1, nc = -1;
            if (c+1 < PUZZLE_SIDE && cell_kept(r,c+1,W) && g_has[r][c+1] &&
                o->right != g_grid[r][c+1].left) { nr=r; nc=c+1; }
            else if (r+1 < PUZZLE_SIDE && cell_kept(r+1,c,W) && g_has[r+1][c] &&
                     o->top != g_grid[r+1][c].bottom) { nr=r+1; nc=c; }
            if (nr >= 0) {
                if (why) {
                    int br, bc; frame_to_input(nr,nc,g_rot_applied,&br,&bc);
                    snprintf(why,128,"break in kept shape at (%d,%d)-(%d,%d)",
                             ar,ac,br,bc);
                }
                return false;
            }
        }
    return true;
}

static bool retained_for_hold_check(int r, int c, int W) {
    return cell_kept(r,c,W) || g_hold_mask[r][c];
}

/* Select and validate the occupied cells in the far half of the final side.
   The seam itself is intentionally open: searched and held halves need not
   color-match, and such outputs are routed to the hold-join file. */
static void hold_band_prepare(uint32_t line) {
    memset(g_hold_mask, 0, sizeof g_hold_mask);
    memset(g_hold_expected, 0, sizeof g_hold_expected);
    g_hold_active = false; g_hold_kept = 0;
    g_hold_near_occupied = 0;
    g_hold_report = g_hold_band ? HOLD_REPORT_EMPTY : HOLD_REPORT_OFF;
    g_hold_reason[0] = '\0';
    if (!g_hold_band) return;

    int hr0=PUZZLE_SIDE, hr1=-1, hc0=PUZZLE_SIDE, hc1=-1;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            int half = hold_half(r,c,g_W);
            if (half == 0 && g_has[r][c]) g_hold_near_occupied++;
            if (half == 1 && g_has[r][c]) {
                g_hold_mask[r][c] = true;
                g_hold_kept++;
                uint16_t pid = g_grid[r][c].piece_id;
                int ar, ac;
                frame_to_orig(r,c,g_rot_applied,&ar,&ac);
                g_hold_expected[pid] = true;
                g_hold_expected_pos[pid] = ar*PUZZLE_SIDE + ac;
                g_hold_expected_rot[pid] =
                    (uint8_t)((g_grid[r][c].rotation + g_rot_applied) & 3);
                int ir, ic;
                frame_to_input(r,c,g_rot_applied,&ir,&ic);
                if (ir < hr0) hr0 = ir;
                if (ir > hr1) hr1 = ir;
                if (ic < hc0) hc0 = ic;
                if (ic > hc1) hc1 = ic;
            }
        }
    if (!g_hold_kept) {
        snprintf(g_hold_reason, sizeof g_hold_reason,
                 "no pieces in far half of %s", round_side(g_rounds));
        if (g_verbose)
            printf("[hold] line=%u status=EMPTY side=%s near_half_occupied=%d\n",
                   line, round_side(g_rounds), g_hold_near_occupied);
        return;
    }

    int bad_r=-1, bad_c=-1; const char *why=NULL;
    for (int r=0; r<PUZZLE_SIDE && !why; r++)
        for (int c=0; c<PUZZLE_SIDE && !why; c++) {
            if (!g_hold_mask[r][c]) continue;
            if (!piece_seats(&g_grid[r][c],r,c)) {
                why="illegal seating"; bad_r=r; bad_c=c;
            }
        }
    /* Scan pairs from either endpoint. This includes held-to-core interfaces
       on the left/bottom, which a held-cell-only right/up scan would miss. */
    for (int r=0; r<PUZZLE_SIDE && !why; r++)
        for (int c=0; c<PUZZLE_SIDE && !why; c++) {
            if (!retained_for_hold_check(r,c,g_W)) continue;
            const Oriented *o=&g_grid[r][c];
            if (c+1<PUZZLE_SIDE && retained_for_hold_check(r,c+1,g_W) &&
                (g_hold_mask[r][c] || g_hold_mask[r][c+1]) &&
                o->right!=g_grid[r][c+1].left) {
                why="retained break"; bad_r=r; bad_c=c;
            } else if (r+1<PUZZLE_SIDE && retained_for_hold_check(r+1,c,g_W) &&
                       (g_hold_mask[r][c] || g_hold_mask[r+1][c]) &&
                       o->top!=g_grid[r+1][c].bottom) {
                why="retained break"; bad_r=r; bad_c=c;
            }
        }
    if (why) {
        int ar,ac; frame_to_input(bad_r,bad_c,g_rot_applied,&ar,&ac);
        g_hold_report = HOLD_REPORT_FALLBACK;
        snprintf(g_hold_reason, sizeof g_hold_reason, "%s at %d,%d", why, ar, ac);
        if (g_verbose)
            printf("[hold] line=%u status=FALLBACK side=%s reason=\"%s\"\n",
                   line, round_side(g_rounds), g_hold_reason);
        memset(g_hold_mask,0,sizeof g_hold_mask);
        memset(g_hold_expected,0,sizeof g_hold_expected);
        g_hold_kept=0;
        return;
    }
    g_hold_active = true;
    g_hold_report = HOLD_REPORT_ACTIVE;
    if (g_verbose)
        printf("[hold] line=%u status=ACTIVE side=%s kept=%d box=%d..%d,%d..%d "
               "near_half_occupied=%d seam=open\n",
               line, round_side(g_rounds), g_hold_kept,
               hr0,hr1,hc0,hc1,g_hold_near_occupied);
}

/* Load one CSV line into the frame, apply --rotate, choose the strip width and
   free everything outside the core. */
static bool load_and_cut(uint32_t line, const char *input_id,
                         const int pos_in[NUM_PIECES],
                         const int rot_in[NUM_PIECES], int forced_W) {
    int pos[NUM_PIECES], rot[NUM_PIECES];
    g_board_reason[0] = '\0';
    g_hold_report = g_hold_band ? HOLD_REPORT_EMPTY : HOLD_REPORT_OFF;
    g_hold_reason[0] = '\0';
    memcpy(pos, pos_in, sizeof pos);
    memcpy(rot, rot_in, sizeof rot);
    snprintf(g_in_id, sizeof g_in_id, "%s", input_id ? input_id : "");
    /* A board with no id of its own still needs one to carry. */
    if (!g_in_id[0]) snprintf(g_in_id, sizeof g_in_id, "p%u", line);

    /* --reverse: into mirror space first, so everything below -- the clue
       orientation included -- reads a board that agrees with the mirrored seed
       and the mirrored clue table. */
    if (g_reverse) mirror_line(pos, rot);

    g_input_snap.placed = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        g_input_snap.pos[pid] = pos[pid];
        g_input_snap.rot[pid] = rot[pid];
        g_input_snap.order[pid] = 0;
        if (pos[pid] != 999) g_input_snap.placed++;
    }
    g_input_placed = g_input_snap.placed;

    /* Read the board's committed orientation before anything is rotated or
       freed: the clue table is in unturned coordinates, and this is the only
       moment the board is still in them. */
    g_rh_orient = g_clue_mask ? rh_clue_orient_of(pos, rot) : -1;
    if (g_clue_mask && g_rh_orient < 0) {
        snprintf(g_board_reason, sizeof g_board_reason,
                 "no enabled clue orientation");
        return false;
    }

    memset(g_has, 0, sizeof g_has);
    memset(g_order, 0, sizeof g_order);
    memset(g_placed, 0, sizeof g_placed);
    g_rot_applied = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == 999) continue;
        if (pos[pid] < 0 || pos[pid] >= NUM_PIECES || rot[pid] < 0 || rot[pid] > 3) {
            snprintf(g_board_reason, sizeof g_board_reason,
                     "piece %d has pos=%d rot=%d", pid, pos[pid], rot[pid]);
            return false;
        }
        int r = pos[pid]/PUZZLE_SIDE, c = pos[pid]%PUZZLE_SIDE;
        if (g_has[r][c]) {
            snprintf(g_board_reason, sizeof g_board_reason,
                     "duplicate cell %d,%d", r, c);
            return false;
        }
        g_grid[r][c] = rh_oriented((uint16_t)pid, (uint8_t)rot[pid]);
        g_has[r][c] = true;
    }

    for (int i = 0; i < (g_rotate & 3); i++) rotate_cw();
    g_rot_applied = g_rotate & 3;

    /* Width. An explicit --strip_width is validated as given; --strip_width 0
       takes the narrowest usable one, which keeps the most proven structure. */
    char why[128] = "";
    if (forced_W) {
        g_W = forced_W;
        if (!core_usable(g_W, why)) {
            snprintf(g_board_reason, sizeof g_board_reason,
                     "W=%d unusable: %s", g_W, why);
            return false;
        }
    } else {
        int chosen = 0;
        for (int W = 2; W <= MAX_W && !chosen; W++)
            if (core_usable(W, NULL)) chosen = W;
        if (!chosen) {
            core_usable(MAX_W, why);
            snprintf(g_board_reason, sizeof g_board_reason,
                     "no usable W in 2..%d: %s", MAX_W, why);
            return false;
        }
        g_W = chosen;
    }

    check_hold_partition(g_W);
    hold_band_prepare(line);

    /* Keep the core/anchors and, when valid, occupied cells in the far half of
       the final side. Everything else is made available to the strip search. */
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (cell_kept(r, c, g_W)) continue;
            if (g_hold_mask[r][c]) continue;
            g_has[r][c] = false;
            g_order[r][c] = 0;
        }

    /* Clue viability. A clue whose cell survives the cut is never re-placed by
       any strip, so if it is wrong there it is wrong for good and searching this
       board is wasted work. The centre clue is always in this case -- the core
       spans the middle of the board at every W -- which is why --clue_center
       verifies here rather than pinning anything. */
    for (int k = 0; k < CLUE_N && g_clue_mask; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_rh_clue[g_rh_orient][k];
        int R, C, S;
        orig_to_frame(cc->row, cc->col, cc->spin, g_rot_applied, &R, &C, &S);
        if (!g_has[R][C]) continue;                 /* freed: a strip will pin it */
        if (g_grid[R][C].piece_id != cc->piece || g_grid[R][C].rotation != (uint8_t)S) {
            /* The cell is named from the UNMIRRORED table, so the message points
               at the board the user handed in rather than at mirror space. */
            const ClueCell *say = &g_clue[g_rh_orient][k];
            snprintf(g_board_reason, sizeof g_board_reason,
                     "retained clue %d conflict at %d,%d: expected %u found %u",
                     k, say->row, say->col, cc->piece, g_grid[R][C].piece_id);
            return false;
        }
    }

    /* A retained corner must honor an explicit input-corner pin. Searched
       corners are constrained later by border-chain enumeration. */
    const int cr[4] = {0,0,PUZZLE_SIDE-1,PUZZLE_SIDE-1};
    const int cc[4] = {0,PUZZLE_SIDE-1,0,PUZZLE_SIDE-1};
    for (int k = 0; k < 4; k++) {
        int ir, ic; frame_to_input(cr[k],cc[k],g_rot_applied,&ir,&ic);
        int role = (ir == 0 ? 0 : 2) + (ic == 0 ? 0 : 1);
        if (g_pin_corner[role] >= 0 && g_has[cr[k]][cc[k]] &&
            g_grid[cr[k]][cc[k]].piece_id != (uint16_t)g_pin_corner[role]) {
            snprintf(g_board_reason, sizeof g_board_reason,
                     "%s corner expected %d found %u", k_corner_name[role],
                     g_pin_corner[role], g_grid[cr[k]][cc[k]].piece_id);
            return false;
        }
    }

    g_n_placed = 0;
    memset(g_placed, 0, sizeof g_placed);
    memset(g_db_reusable, 0, sizeof g_db_reusable);
    for (int c = 0; c < NUM_COLORS_TOTAL; c++) g_avail[c] = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (g_has[r][c]) {
                used_set(g_placed, g_grid[r][c].piece_id); g_n_placed++;
                if (g_rounds == 4 && r == 0 && c >= PUZZLE_SIDE-g_W)
                    used_set(g_db_reusable, g_grid[r][c].piece_id);
            }
    g_base_kept = g_n_placed;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (used_test(g_placed, (uint16_t)pid)) continue;
        int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
        for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
    }
    return true;
}

/* -- Compact normal-mode reporting ----------------------------------------- */

static const char *hold_report_text(char buf[32]) {
    switch (g_hold_report) {
        case HOLD_REPORT_ACTIVE:
            snprintf(buf, 32, "%d", g_hold_kept);
            break;
        case HOLD_REPORT_EMPTY:
            snprintf(buf, 32, "empty");
            break;
        case HOLD_REPORT_FALLBACK:
            snprintf(buf, 32, "fallback");
            break;
        default:
            snprintf(buf, 32, "off");
            break;
    }
    return buf;
}

/* One newline per selected input board.  Long records may wrap in a terminal,
   but remain one physical line in a redirected or Slurm log. */
static void print_board_result(uint32_t line, const char *status,
                               int deepest, double sec, const char *reason) {
    char hold[32];
    const char *id = g_in_id[0] ? g_in_id : "?";

    if (reason && *reason) {
        printf("[board] line=%u id=%s status=%s reason=\"%s\"\n",
               line, id, status, reason);
        return;
    }

    if (g_board_report.have_search) {
        const TopBandChange *top = &g_board_report.search_top;
        printf("[board] line=%u id=%s status=%s pieces=%d->%d added=%+d "
               "top=%d->%d top_new=%d top_removed=%d outputs=%u score=%d "
               "breaks=%d class=%s hold=%s nodes=%" PRIu64 " sec=%.3f",
               line, id, status, g_input_placed, g_board_report.search_placed,
               g_board_report.search_placed - g_input_placed,
               top->before, top->after, top->entered, top->removed,
               g_board_report.search_written + g_board_report.fill_written,
               g_board_report.search_matched,
               g_board_report.search_breaks, g_board_report.search_class,
               hold_report_text(hold), g_nodes, sec);
    } else if (g_board_report.have_fill) {
        const TopBandChange *top = &g_board_report.fill_top;
        printf("[board] line=%u id=%s status=%s pieces=%d->%d added=%+d "
               "top=%d->%d top_new=%d top_removed=%d outputs=%u score=%d "
               "breaks=%d class=break-fill hold=%s nodes=%" PRIu64 " sec=%.3f",
               line, id, status, g_input_placed, g_board_report.fill_placed,
               g_board_report.fill_placed - g_input_placed,
               top->before, top->after, top->entered, top->removed,
               g_board_report.fill_written, g_board_report.fill_matched,
               g_board_report.fill_breaks, hold_report_text(hold), g_nodes, sec);
    } else {
        printf("[board] line=%u id=%s status=%s input=%d deepest=%d added=%+d "
               "outputs=0 reason=no-new-level hold=%s nodes=%" PRIu64 " sec=%.3f",
               line, id, status, g_input_placed, deepest,
               deepest - g_input_placed, hold_report_text(hold), g_nodes, sec);
    }

    if (g_board_report.have_search && g_board_report.have_fill)
        printf(" fill=%d score=%d breaks=%d",
               g_board_report.fill_placed, g_board_report.fill_matched,
               g_board_report.fill_breaks);
    else if (g_board_report.fill_rejected_breaks >= 0)
        printf(" fill=rejected(%d>%d)",
               g_board_report.fill_rejected_breaks, g_max_breaks);

    if (g_hold_report == HOLD_REPORT_FALLBACK && g_hold_reason[0])
        printf(" hold_reason=\"%s\"", g_hold_reason);
    putchar('\n');
}

/* Parent directory of the explicit output file.  The shared manifest writer
   then creates the usual <directory>/outputs.txt used by the beamer tools. */
static void output_parent_dir(const char *path, char out[PATH_MAX]) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, PATH_MAX, ".");
    } else if (slash == path) {
        snprintf(out, PATH_MAX, "/");
    } else {
        size_t n = (size_t)(slash - path);
        if (n >= PATH_MAX) fatal("output directory path is too long");
        memcpy(out, path, n);
        out[n] = '\0';
    }
}

/* -- Summary ---------------------------------------------------------------- */

static void print_summary(double wall) {
    if (g_verbose) {
        uint64_t clean_search = g_stats.emit_solved + g_stats.emit_deepest;
        printf("[sum-detail] status_done=%u status_target=%u status_budget=%u "
               "strips=%u color_dead=%u nodes=%" PRIu64 "\n",
               g_stats.lines_exhausted, g_stats.lines_target,
               g_stats.lines_truncated, g_stats.strips,
               g_stats.strips_refuted, g_stats.nodes);
        printf("[sum-detail] clean_search=%" PRIu64 " solved=%" PRIu64
               " deepest=%" PRIu64 " hold_join=%" PRIu64
               " break_fill=%" PRIu64 "\n",
               clean_search, g_stats.emit_solved, g_stats.emit_deepest,
               g_stats.emit_joined, g_stats.emit_filled);
    }

    printf("\n=== Finished ===\n");
    printf("Processed: %u board(s); searched %u; duplicate %u; unusable %u\n",
           g_stats.lines_read, g_stats.lines_used,
           g_stats.lines_dup, g_stats.lines_bad);
    printf("Written:   %" PRIu64 " board(s) to %s\n", g_emitted, g_out_path);
    printf("Elapsed:   %.2f s\n", wall);
    fflush(stdout);
}

/* -- Usage and entry point -------------------------------------------------- */

static const char *k_usage =
"E555_roundhouse -- deterministic width-W spiral strip search\n"
"\n"
"  bin/E555_roundhouse seed.txt boards.csv output.csv [options]\n"
"\n"
"GEOMETRY\n"
"  --rounds N             1..4 sides (default 3)\n"
"  --strip_width W        2..5; 0 chooses the narrowest usable W (default 5)\n"
"  --rotate K             -3..3 quarter-turns before the cut (default 1)\n"
"  --ccw | --cw           spiral direction (default CCW)\n"
"  --hold_band            rounds 1..3: retain occupied cells in the half\n"
"                         opposite the current final-side traversal, search\n"
"                         the other half to its deepest reachable prefix, and\n"
"                         leave the join unconstrained\n"
"  --stop_row R           final-side frame level 0..15\n"
"  --stop_after N         require N new levels on the final side, then stop\n"
"  --BL/--BR/--TL/--TR P  pin an input-board corner piece\n"
"\n"
"SEARCH\n"
"  --ties N               keep up to N deepest boards (default 1)\n"
"  --tie_depth N          ties must differ at least N chain levels behind\n"
"                         the newest placement (default 2)\n"
"  --target_ties N        stop each input after N diverse boards reach the\n"
"                         requested final-side endpoint (default 0=off)\n"
"  --max_nodes N          DFS chain records examined per input board; 0=unlimited\n"
"                         This is a deterministic search-work limit; database\n"
"                         construction and oracle sweeps are not counted.\n"
"  --time_limit S         search seconds per input board after DB build\n"
"                         (default 600; 0=unlimited)\n"
"  --wall_time S          seconds for the full invocation (default 0=unlimited)\n"
"  --max_emitted N        stop after N written boards (default 0=unlimited)\n"
"  --breaks B             greedily complete the deepest board with <=B breaks\n"
"  --clue_center          require the published center clue\n"
"  --clue_corners         require the four published clue pieces\n"
"  --no_transition_cache  save memory; slower repeated oracle sweeps, notably W=5\n"
"\n"
"INPUT / OUTPUT\n"
"  output.csv             required output file; replaced at startup\n"
"                         outputs.txt is written beside it, in beamer format\n"
"  --start_row N          first data row (default 0)\n"
"  --num_rows N           rows to process; 0 means through EOF\n"
"  --shard_count N        split that row window into N process shards\n"
"  --shard_index I        zero-based shard to process (default 0 of 1)\n"
"                         concurrent shards must use different output files\n"
"  --threads N            OpenMP threads for database/oracle work; DFS is serial\n"
"  --verbose              add database, geometry, oracle and emission details\n"
"  --print_cmd            print the normalized invocation\n"
"\n"
"NOTES\n"
"  Earlier sides always use an endpoint oracle because they must finish before\n"
"  the next rotation. With no explicit final stop, the last side uses a depth\n"
"  oracle, including under --hold_band: it explores exact prefixes without\n"
"  requiring the far border or held-half seam, and returns the deepest found.\n"
"  --rounds 4 retains a centered core, the first wall column, and the W-piece\n"
"  bottom-right anchor chain; it then traverses all four sides. It is currently\n"
"  separate from --hold_band. Every emitted puzzle board goes to output.csv;\n"
"  stdout and the id tag identify clean, hold-join and break-fill rows.\n";

/* --BL/--BR/--TL/--TR -> g_pin_corner role. */
static int corner_index(const char *flag) {
    if (!strcmp(flag, "--BL")) return 0;
    if (!strcmp(flag, "--BR")) return 1;
    if (!strcmp(flag, "--TL")) return 2;
    if (!strcmp(flag, "--TR")) return 3;
    return -1;
}

/* Options this tool no longer has. A removed flag is a hard error like any
   other unknown one -- silently accepting and ignoring it would let a stale
   caller keep running while doing something different, which is the failure
   tests/check_script_flags.py exists to catch. Only the output move earns a
   message of its own, because it relocates an argument rather than dropping a
   control. The name lives in an array so that the check's parser scan does not
   harvest it as a flag this binary accepts. */
static void reject_removed_flag(const char *a) {
    static const char *moved[] = { "--out_dir", NULL };
    for (int k = 0; moved[k]; k++)
        if (!strcmp(a, moved[k]))
            fatal("%s was removed; give output.csv as the third positional "
                  "argument", a);
}

static void print_cmd(const char *a0, const char *seed_path, const char *csv_path,
                      const char *out_path, int nthreads) {
    static const char *corner[4] = { "--BL", "--BR", "--TL", "--TR" };
    printf("[cmd] %s %s %s %s %s", a0, seed_path, csv_path, out_path,
           g_reverse ? "--cw" : "--ccw");
    if (g_hold_band) printf(" --hold_band");
    if (g_verbose) printf(" --verbose");
    if (g_print_cmd) printf(" --print_cmd");
    if (!g_transition_cache) printf(" --no_transition_cache");
    if (g_clue_mask & CLUE_CENTER) printf(" --clue_center");
    if (g_clue_mask & CLUE_CORNERS) printf(" --clue_corners");
    for (int k = 0; k < 4; k++)
        if (g_pin_corner[k] >= 0) printf(" %s %d", corner[k], g_pin_corner[k]);
    printf(" --start_row %u --num_rows %u", g_line_first, g_line_count);
    if (g_shard_count > 1)
        printf(" --shard_count %u --shard_index %u", g_shard_count, g_shard_index);
    printf(" --rounds %d --rotate %d --strip_width %d", g_rounds, g_rotate, g_opt_W);
    if (g_stop_level >= 0) printf(" --stop_row %d", g_stop_level);
    if (g_stop_after >= 0) printf(" --stop_after %d", g_stop_after);
    printf(" --ties %u --tie_depth %u --target_ties %u --breaks %d",
           g_ties, g_tie_depth, g_target_ties, g_max_breaks);
    printf(" --max_nodes %" PRIu64 " --max_emitted %" PRIu64,
           g_max_nodes, g_max_boards);
    printf(" --time_limit %g --wall_time %g --threads %d\n",
           g_config_time_sec, g_max_wall_sec,
           nthreads > 0 ? nthreads : g_nthreads);
}

static int cli_int(const char *opt, const char *s) {
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX)
        fatal("%s expects an integer, got '%s'", opt, s);
    return (int)v;
}

static uint64_t cli_u64(const char *opt, const char *s) {
    if (*s == '-') fatal("%s expects a non-negative integer, got '%s'", opt, s);
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0')
        fatal("%s expects a non-negative integer, got '%s'", opt, s);
    return (uint64_t)v;
}

static uint32_t cli_u32(const char *opt, const char *s) {
    uint64_t v = cli_u64(opt, s);
    if (v > UINT32_MAX) fatal("%s value is too large: '%s'", opt, s);
    return (uint32_t)v;
}

static double cli_double(const char *opt, const char *s) {
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno || end == s || *end != '\0' || !isfinite(v))
        fatal("%s expects a finite number, got '%s'", opt, s);
    return v;
}

static bool same_existing_file(const char *a, const char *b) {
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        fputs(k_usage, stdout);
        return 0;
    }
    if (argc < 4) {
        fputs(k_usage, stderr);
        return 1;
    }
    const char *seed_path = argv[1];
    const char *csv_path  = argv[2];
    g_out_path = argv[3];
    if (!g_out_path[0] || g_out_path[0] == '-')
        fatal("the third positional argument must be output.csv");
    int nthreads = 0;

    for (int i = 4; i < argc; i++) {
        const char *a = argv[i];
        reject_removed_flag(a);
        int ci = corner_index(a);
        if (ci >= 0 && i+1 < argc) g_pin_corner[ci] = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--start_row") && i+1 < argc) g_line_first = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--num_rows") && i+1 < argc) g_line_count = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--shard_count") && i+1 < argc) g_shard_count = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--shard_index") && i+1 < argc) g_shard_index = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--strip_width") && i+1 < argc) g_opt_W = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--rounds") && i+1 < argc) g_rounds = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--rotate") && i+1 < argc) g_rotate = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--cw")) g_reverse = true;
        else if (!strcmp(a, "--ccw")) g_reverse = false;
        else if (!strcmp(a, "--stop_row") && i+1 < argc)
            g_stop_level = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--stop_after") && i+1 < argc) g_stop_after = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--hold_band")) g_hold_band = true;
        else if (!strcmp(a, "--breaks") && i+1 < argc) g_max_breaks = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--clue_center")) g_clue_mask |= CLUE_CENTER;
        else if (!strcmp(a, "--clue_corners")) g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(a, "--max_nodes") && i+1 < argc) g_max_nodes = cli_u64(a, argv[++i]);
        else if (!strcmp(a, "--ties") && i+1 < argc) g_ties = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--tie_depth") && i+1 < argc) g_tie_depth = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--target_ties") && i+1 < argc) g_target_ties = cli_u32(a, argv[++i]);
        else if (!strcmp(a, "--time_limit") && i+1 < argc) g_config_time_sec = cli_double(a, argv[++i]);
        else if (!strcmp(a, "--wall_time") && i+1 < argc) g_max_wall_sec = cli_double(a, argv[++i]);
        else if (!strcmp(a, "--max_emitted") && i+1 < argc) g_max_boards = cli_u64(a, argv[++i]);
        else if (!strcmp(a, "--threads") && i+1 < argc) nthreads = cli_int(a, argv[++i]);
        else if (!strcmp(a, "--no_transition_cache")) g_transition_cache = false;
        else if (!strcmp(a, "--print_cmd")) g_print_cmd = true;
        else if (!strcmp(a, "--verbose")) g_verbose = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { fputs(k_usage, stdout); return 0; }
        else fatal("unknown or incomplete option %s (try --help)", a);
    }

    if (g_rounds < 1 || g_rounds > MAX_ROUNDS) fatal("--rounds must be 1..%d", MAX_ROUNDS);
    if (g_opt_W && (g_opt_W < 2 || g_opt_W > MAX_W))
        fatal("--strip_width must be 0 or 2..%d", MAX_W);
    if (g_rotate < -3 || g_rotate > 3) fatal("--rotate must be -3..3");
    g_rotate = (g_rotate + 4) & 3;
    if (g_ties < 1) fatal("--ties must be >= 1");
    if (g_target_ties > g_ties) {
        printf("[warn] target_ties=%u raises ties from %u to %u\n",
               g_target_ties, g_ties, g_target_ties);
        g_ties = g_target_ties;
    }
    if (g_shard_count < 1) fatal("--shard_count must be >= 1");
    if (g_shard_index >= g_shard_count)
        fatal("--shard_index must be in 0..shard_count-1");
    if (g_stop_level < -1 || g_stop_level > 15) fatal("--stop_row must be 0..15");
    if (g_stop_after == 0 || g_stop_after < -1) fatal("--stop_after must be >= 1");
    if (g_stop_level >= 0 && g_stop_after >= 0)
        fatal("use either --stop_row or --stop_after, not both");
    if (g_hold_band && (g_stop_level >= 0 || g_stop_after >= 0))
        fatal("--hold_band already fixes the final-side endpoint; remove the stop option");
    if (g_hold_band && g_rounds == 4)
        fatal("--hold_band is currently for --rounds 1..3; round 4 already uses fixed anchors");
    if (g_max_breaks < 0) fatal("--breaks must be >= 0");
    if (g_config_time_sec < 0.0 || g_max_wall_sec < 0.0)
        fatal("time limits must be non-negative");
    if (nthreads < 0) fatal("--threads must be >= 0");

    omp_set_dynamic(0);
    omp_set_max_active_levels(1);
    if (nthreads > 0) { omp_set_num_threads(nthreads); g_nthreads = nthreads; }
    else g_nthreads = omp_get_max_threads();
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);

    /* The output is a fresh file for this invocation.  Protect the input and seed
       from accidental truncation, including aliases through hard links/symlinks. */
    if (same_existing_file(g_out_path, csv_path) || !strcmp(g_out_path, csv_path))
        fatal("output file must differ from the input boards file");
    if (same_existing_file(g_out_path, seed_path) || !strcmp(g_out_path, seed_path))
        fatal("output file must differ from the seed file");
    output_parent_dir(g_out_path, g_out_dir);
    int mn = snprintf(g_manifest_path, sizeof g_manifest_path, "%s%soutputs.txt",
                      g_out_dir, !strcmp(g_out_dir, "/") ? "" : "/");
    if (mn < 0 || (size_t)mn >= sizeof g_manifest_path)
        fatal("manifest path is too long");
    if (!strcmp(g_out_path, g_manifest_path) ||
        same_existing_file(g_out_path, g_manifest_path))
        fatal("output file cannot be the manifest %s", g_manifest_path);

    char wtag[8];
    if (g_opt_W) snprintf(wtag, sizeof wtag, "%d", g_opt_W);
    else snprintf(wtag, sizeof wtag, "auto");

    uint32_t csv_lines = count_data_lines(csv_path);
    if (g_line_count == 0)
        g_line_count = csv_lines > g_line_first ? csv_lines - g_line_first : 0;

    printf("=== E555 roundhouse ===\n\n");
    if (g_print_cmd) print_cmd(argv[0], seed_path, csv_path, g_out_path, nthreads);
    printf("Input:  %s\n", csv_path);
    printf("Output: %s  (replaced)\n", g_out_path);
    printf("Manifest: %s\n", g_manifest_path);
    printf("Rows:   %u..%u", g_line_first,
           g_line_count ? g_line_first + g_line_count - 1 : g_line_first);
    if (g_shard_count > 1)
        printf("  shard %u/%u", g_shard_index, g_shard_count);
    printf("\nSearch: %s; ", direction_name());
    for (int rd = 1; rd <= g_rounds; rd++)
        printf("%s%s", rd == 1 ? "" : " > ", round_side(rd));
    printf("; rounds=%d; W=%s; hold=%s; ties=%u; tie_depth=%u\n",
           g_rounds, wtag, g_hold_band ? "on" : "off", g_ties, g_tie_depth);
    if (g_verbose) {
        printf("[cfg-detail] rotate=%d stop_row=%d stop_after=%d target_ties=%u "
               "max_nodes=%" PRIu64 " time_limit=%.3g wall_time=%.3g "
               "breaks=%d max_emitted=%" PRIu64 " threads=%d dfs_threads=1 "
               "transition_cache=%d\n",
               g_rotate, g_stop_level, g_stop_after, g_target_ties,
               g_max_nodes, g_config_time_sec, g_max_wall_sec,
               g_max_breaks, g_max_boards, g_nthreads,
               g_transition_cache ? 1 : 0);
        if (g_clue_mask || g_pin_corner[0] >= 0 || g_pin_corner[1] >= 0 ||
            g_pin_corner[2] >= 0 || g_pin_corner[3] >= 0)
            printf("[cfg-detail] clues=center:%d,corners:%d "
                   "pins=BL:%d,BR:%d,TL:%d,TR:%d\n",
                   !!(g_clue_mask & CLUE_CENTER),
                   !!(g_clue_mask & CLUE_CORNERS),
                   g_pin_corner[0], g_pin_corner[1],
                   g_pin_corner[2], g_pin_corner[3]);
        if (g_rounds == 4)
            printf("[cfg-detail] round4=inner-core+wall-column+bottom-right-W-anchor\n");
    }
    if (g_line_first >= csv_lines)
        printf("[warn] start_row=%u data_rows=%u action=none\n", g_line_first, csv_lines);
    printf("\n");
    fflush(stdout);

    g_free_edges = true;
    load_seed_and_catalog(seed_path);
    if (g_reverse) mirror_seed();
    init_clue_table();
    build_catalog_indices();
    build_inner_color_totals();
    if (g_clue_mask) check_frame_maps();

    for (int k = 0; k < 4; k++)
        if (g_pin_corner[k] >= 0 &&
            (g_pin_corner[k] >= NUM_PIECES || rh_zero_count(g_pin_corner[k]) != 2))
            fatal("--%s %d is not a corner piece", k_corner_name[k], g_pin_corner[k]);

    g_out_fp = fopen(g_out_path, "w");
    if (!g_out_fp)
        fatal("cannot create output file %s: %s", g_out_path, strerror(errno));
    /* Register after fopen("w") has truncated the file, so the shared growth
       test lists it exactly when this invocation writes at least one board. */
    manifest_add(g_out_path);

    g_t_start = omp_get_wtime();

    FILE *csv_fp = fopen(csv_path, "r");
    if (!csv_fp) fatal("cannot open boards CSV %s: %s", csv_path, strerror(errno));
    char *csv_line = NULL; size_t csv_cap = 0;
    uint32_t next_data_index = 0, li = 0;
    char input_id[96]; int input_pos[NUM_PIECES], input_rot[NUM_PIECES];

    while (read_next_board(csv_fp, &csv_line, &csv_cap, &next_data_index,
                           input_id, input_pos, input_rot)) {
        li = next_data_index - 1;
        if (li < g_line_first) continue;
        if (li >= g_line_first + g_line_count || g_stop) break;
        if (((li - g_line_first) % g_shard_count) != g_shard_index) continue;
        if (g_max_wall_sec > 0.0 && omp_get_wtime() - g_t_start >= g_max_wall_sec) break;
        if (g_max_boards > 0 && g_emitted >= g_max_boards) break;
        g_stats.lines_read++;
        g_line_id = li;
        g_nodes = 0;
        board_report_reset();
        if (!load_and_cut(li, input_id, input_pos, input_rot, g_opt_W)) {
            g_stats.lines_bad++;
            print_board_result(li, "SKIP", g_input_placed, 0.0,
                               g_board_reason[0] ? g_board_reason : "unusable input");
            continue;
        }
        uint64_t retained_hash = core_hash();
        if (!dup_insert(retained_hash)) {
            g_stats.lines_dup++;
            snprintf(g_board_reason, sizeof g_board_reason,
                     "duplicate retained shape %016" PRIx64, retained_hash);
            print_board_result(li, "DUPLICATE", g_input_placed, 0.0, g_board_reason);
            continue;
        }
        g_stats.lines_used++;

        int anchors = g_rounds == 4 ? g_W : 0;
        if (g_verbose)
            printf("[line] n=%u core=%016" PRIx64 " input=%d kept=%d fixed=%d "
                   "anchors=%d hold=%d free=%d W=%d\n",
                   li, retained_hash, g_input_placed, g_base_kept,
                   g_base_kept-g_hold_kept-anchors, anchors, g_hold_kept,
                   NUM_PIECES-g_base_kept, g_W);
        if (NUM_PIECES - g_n_placed < g_W) {
            snprintf(g_board_reason, sizeof g_board_reason,
                     "fewer than W=%d cells are open", g_W);
            print_board_result(li, "SKIP", g_n_placed, 0.0, g_board_reason);
            continue;
        }

        s_nsig = MAX_EDGE_SIDE_COLOR;
        for (int i = 0; i < chain_w() - 1; i++) s_nsig *= DIM_INNER;
        s_ncell = (uint64_t)DIM_INNER * s_nsig;

        for (int k = 0; k < 4; k++)
            g_db_exclude[k] = g_placed[k] & ~g_db_reusable[k];
        bool reuse_db = s_db_ready && s_db && s_db_W == g_W &&
                        !memcmp(s_exclude_key, g_db_exclude, sizeof s_exclude_key);
        if (reuse_db) {
            if (g_verbose)
                printf("[db] W=%d reuse=1 records=%" PRIu64 " cells=%" PRIu64
                       " db=%.3fGB cache=%.3fGB\n",
                       g_W, s_records, s_cells, (double)s_db_bytes/1e9,
                       (double)s_succ_bytes/1e9);
        } else {
            if (s_db) { free(s_db); s_db = NULL; }
            build_chain_db();
        }

        if (g_verbose) {
            int kr0, kr1, kc0, kc1;
            core_box_orig(g_W, &kr0, &kr1, &kc0, &kc1);
            printf("[shape] line=%u retained_box=%d..%d,%d..%d reusable_anchors=%d\n",
                   li, kr0, kr1, kc0, kc1, g_rounds == 4 ? g_W : 0);
        }

        Snap core; snapshot(&core);
        best_reset();
        memset(g_verbose_seen, 0, sizeof g_verbose_seen);
        g_nodes = 0; g_truncated = false; g_target_stop = false;
        g_endpoint_hits = 0; g_endpoint_depth = -1; g_first_endpoint_node = 0;
        g_t_config = omp_get_wtime();
        run_round(1);
        double sec = omp_get_wtime() - g_t_config;
        g_stats.nodes += g_nodes;
        if (g_target_stop) g_stats.lines_target++;
        else if (g_truncated) g_stats.lines_truncated++;
        else g_stats.lines_exhausted++;

        int deepest = g_best_depth < 0 ? core.placed : g_best_depth;
        const char *status = g_target_stop ? "TARGET" : g_truncated ? "BUDGET" : "DONE";
        if (g_verbose)
            printf("[result] line=%u status=%s deepest=%d added=%+d net=%+d ties=%u "
                   "endpoint_hits=%u target_ties=%u first_endpoint_node=%" PRIu64
                   " nodes=%" PRIu64 " sec=%.3f\n",
                   li, status, deepest, deepest-core.placed, deepest-g_input_placed,
                   g_best_n, g_endpoint_hits, g_target_ties, g_first_endpoint_node,
                   g_nodes, sec);

        for (uint32_t k = 0; k < g_best_n; k++) {
            if (g_max_boards > 0 && g_emitted >= g_max_boards) break;
            if (g_best[k].placed <= core.placed) continue;
            emit_snap(&g_best[k], g_best[k].placed == NUM_PIECES ? "s" : "d");
        }

        if (g_max_breaks > 0 && (!g_max_boards || g_emitted < g_max_boards)) {
            restore_snap(g_best_n ? &g_best[0] : &core);
            int spent = greedy_fill();
            if (spent > g_max_breaks) {
                g_board_report.fill_rejected_breaks = spent;
            } else {
                Snap filled; snapshot(&filled);
                emit_snap(&filled, "f");
            }
        }
        print_board_result(li, status, deepest, sec, NULL);
    }

    free(csv_line);
    fclose(csv_fp);
    if (fclose(g_out_fp) != 0)
        fatal("cannot close output file %s: %s", g_out_path, strerror(errno));
    g_out_fp = NULL;
    print_summary(omp_get_wtime()-g_t_start);
    manifest_write(g_out_dir);
    return 0;
}
