/*
 * E555_diver.c -- finish board files with the beamer's end dives and polish,
 * without the beam or the chain database, after optionally rebuilding their
 * top the way the beamer's --backtrack_row trajectory does.
 *
 *   E555_diver seed.txt boards.csv output.csv [options]
 *
 * INPUT. Any board CSV: the last 512 fields of a row are pos[256] and rot[256]
 * (999 = unplaced), a leading field is the config id, '#' and '%' lines are
 * comments. --start_row R / --num_rows N read data rows R..R+N-1 only.
 *
 * PER BOARD, in this order:
 *   holes      --holes SPEC lifts cells: a 16x16 0/1 mask file (first data line
 *              = row 0), top:K, bottom:K, left:K, right:K or box:R0-R1,C0-C1.
 *   clues      --pin_clue N places the centre clue of frame N (the beamer's
 *              quadrant numbering), --clue_corners also its four corner clues;
 *              a board whose clue cell or clue piece is taken otherwise is
 *              dropped.
 *   backtrack  --backtrack fills the open cells of rows 0..14 in row-major order
 *              with zero breaks and keeps the deepest prefix. --stop_row S
 *              searches rows up to S exhaustively; every board completing them
 *              is extended column by column over the open cells of rows
 *              S+1..14, cols 0..14 (--extend_nodes per board), and the board
 *              whose extension goes deepest is kept, the first found on ties; a
 *              full region ends the search. One board in, at most one out. The
 *              budget is DR_BT_NODES nodes a board, extensions included; a board
 *              that never reaches S keeps its deepest row prefix.
 *   dives      open cells are filled exactly as `E555_beamer --end_dive` does:
 *              the same engine (E555_dive.c), stages and tuning. Boards whose
 *              best finish scores >= --emit_score are written as canonical
 *              "config_id, score, pos[256], rot[256]" rows, best first within
 *              each batch. --end_dive 0 writes the prepared boards instead
 *              (score = matched edges between placed cells).
 *
 * THE SEARCH. A cell takes an unused piece whose sides match every placed
 * neighbour and whose frame colour 0 faces exactly the board's edge. Inner
 * cells draw from g_lb_bucket[left][bottom] in the beamer's order, so a
 * beamer stop-row board is extended exactly as the beamer extends it; border
 * cells draw from the edge and corner orientations, at their dealt spin when
 * the batch is framed. Top supply: while (15,c) is open, a row-14 piece's top
 * colour must be the inner colour of an unused top-border candidate not yet
 * promised to another row-14 cell, and a candidate placed elsewhere spends one
 * (framed: pieces dealt to the top; free: any edge piece). Boards are searched
 * in parallel, each serially, so the output does not depend on --threads.
 *
 * BATCHES. Consecutive rows with the same config id form a batch -- what the
 * beamer dives together for one configuration -- because stage 2 is chosen
 * relative to the batch (see E555_dive.h). A beamer output file therefore
 * replays configuration by configuration. Output is written after every batch.
 *
 * FRAME. Without --rotations every edge piece may take any open border cell.
 * With --rotations FILE, a beamer config id "r<N>b..." (or "NAME_r<N>b..."
 * from a beamer --prefix run) names the rotations row its border came from, and
 * the edge pieces are held to the sides that row deals them, as in the beamer,
 * in the backtrack as in the dives. A board that cannot be completed that way
 * (its edges do not sit where row N deals them) is prepared and dived with
 * free edges instead, in a run of its own under the same id -- which is how a
 * beamer --free_edges file replays, its corner seeds drawn from the row's
 * pooled top/right catalog as in the beamer. An id that names no row --
 * finalizer, random-border or hand-made boards -- is dived with free edges.
 *
 * Every dive's random stream is keyed by its board and --rng_seed, so output
 * does not depend on --threads or on where a batch starts.
 *
 * REOPEN. A complete board is skipped, unless --reopen names cells to lift
 * from it: the band of rows (or columns) holding its damage (auto), a fixed
 * band, or a mask. The lifted board is dived with the complete one as its
 * incumbent, so the board written is the incumbent itself unless a dive beats
 * it -- never worse. --rounds N re-dives each board's best N times, the auto
 * band re-chosen every round (--reopen given n times: round r reopens the
 * (r mod n)-th spec). --copies K dives every board K times on streams
 * of its own and keeps the best copy; --orders gives the copies different
 * places to start filling from; --prior / --nogo start the dives of a reopened
 * board pulled toward the incumbent's clean placements and away from its
 * broken ones (E555_dive.h, "COPIES, INCUMBENTS, ORDERS"). --holes and --reopen
 * do not combine, nor do --reopen and --backtrack: --holes and the backtrack
 * turn every board into a partial with no incumbent.
 *
 * SCOPE. The diver finishes boards for score; its search is the beamer's
 * zero-break trajectory and nothing else. Break-tolerant, reordered or
 * enumerating searches (all solutions of a band, bounded mismatch, Hall
 * pruning) are E555_backtracker's. New dive policies, moves and stages belong
 * in E555_dive.c's layers; new ways of preparing the board handed to
 * dv_queue() belong here.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <omp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "../B_beam/E555_database.h"
#include "E555_dive.h"

#define DR_M_DEFAULT      10000    /* --end_dive */
#define DR_S_DEFAULT      450      /* --emit_score */
#define DR_SEED_DEFAULT   1        /* --rng_seed */
#define DR_MAX_FIELDS     1024
#define DR_MAX_COPIES     1024     /* --copies */
#define DR_AUTO_COVER     0.9      /* --reopen auto: share of its side's damage the band holds */
#define DR_AUTO_CAP       5        /* ... and at most this deep before the extra rows */
#define DR_MAX_SPECS      16       /* --reopen A --reopen B ...: round r reopens spec r mod n */

/* One --reopen spec: the auto band, a side's band, or a set of cells. */
typedef struct {
    bool auto_band;
    int  side;                     /* 0 top, 1 bottom, 2 left, 3 right; -1 = auto or cells */
    int  depth;                    /* side:K's K, or auto+E's E */
    bool cell[NUM_PIECES];         /* box: or a mask file */
} DrSpec;
static DrSpec      g_spec[DR_MAX_SPECS];
static const char *g_spec_arg[DR_MAX_SPECS];
static int         g_nspec = 0;

/* -- Options ---------------------------------------------------------------- */
static const char *g_rot_path     = NULL;    /* --rotations FILE */
static uint32_t    g_dives        = DR_M_DEFAULT;
static int         g_polish       = -1;      /* --end_polish R; -1 = off */
static int         g_emit         = DR_S_DEFAULT;
static int         g_seeds        = 0;       /* --corner_seeds N; 0 = off */
static uint64_t    g_rng          = DR_SEED_DEFAULT;
static double      g_wall         = 0.0;     /* --wall_time S; 0 = none */
static uint64_t    g_max_written  = 0;       /* --max_emitted N; 0 = none */
static uint32_t    g_copies       = 1;       /* --copies K */
static uint8_t     g_orders[64];             /* --orders LIST */
static int         g_norders      = 0;
static float       g_order_weight = 0.0f;    /* --order_weight W; 0 = the engine's */
static const char *g_reopen       = NULL;    /* --reopen SPEC, the first one given */
static int         g_rounds       = 1;       /* --rounds N */
static bool        g_plateau      = false;   /* --plateau */
static float       g_prior        = 0.0f;    /* --prior A */
static float       g_nogo         = 0.0f;    /* --nogo B */
static uint64_t    g_rec_start    = 0;       /* --start_row R: first record read */
static uint64_t    g_rec_count    = 0;       /* --num_rows N; 0 = the rest */

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* -- Input ------------------------------------------------------------------ */
typedef struct {
    uint16_t pid[NUM_PIECES];      /* per cell; DV_EMPTY = open */
    uint8_t  rot[NUM_PIECES];
    uint64_t line;
    bool     cut;                  /* reopened: inc holds the complete board */
    bool     done;                 /* reopened and written: out of the rounds */
    int      orig;                 /* the score it was read with (reopened) */
    uint64_t orig_fp;              /* ... and its fingerprint */
    uint16_t inc_pid[NUM_PIECES];
    uint8_t  inc_rot[NUM_PIECES];
} DrBoard;

static struct {
    char     id[256];
    DrBoard *b;
    size_t   n, cap;
} g_batch;

static struct {
    uint64_t rows, batches, complete, malformed, misfit, unframed, dived;
    uint64_t reopened, lifted, solved, better, moved, raw;
    int64_t  gain;
} g_st;

/* Split a data line into trimmed comma-separated fields, in place. */
static int dr_split(char *s, char **f, int max) {
    int n = 0;
    for (char *p = s; ; ) {
        while (*p == ' ' || *p == '\t') p++;
        char *start = p;
        while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
        char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        const char c = *p;
        *end = '\0';
        if (n < max) f[n] = start;
        n++;
        if (c != ',') break;
        p++;
    }
    return n;
}

/* One row into a board. False (with the reason) when it does not describe a
   board: a field that is not a number, a cell or rotation out of range, or two
   pieces on one cell. */
static bool dr_parse(char **f, int n, DrBoard *b, const char **why) {
    if (n < 2 * NUM_PIECES) { *why = "fewer than 512 fields"; return false; }
    char **pos = f + n - 2 * NUM_PIECES, **rot = pos + NUM_PIECES;
    for (int x = 0; x < NUM_PIECES; x++) { b->pid[x] = DV_EMPTY; b->rot[x] = 0; }
    for (int p = 0; p < NUM_PIECES; p++) {
        char *ev, *er;
        errno = 0;
        const unsigned long v = strtoul(pos[p], &ev, 10);
        const unsigned long r = strtoul(rot[p], &er, 10);
        if (errno || ev == pos[p] || *ev || er == rot[p] || *er) {
            *why = "a field is not a number"; return false;
        }
        if (v == 999) continue;
        if (v >= NUM_PIECES) { *why = "a cell outside 0..255"; return false; }
        if (r > 3)           { *why = "a rotation outside 0..3"; return false; }
        if (b->pid[v] != DV_EMPTY) { *why = "two pieces on one cell"; return false; }
        b->pid[v] = (uint16_t)p;
        b->rot[v] = (uint8_t)r;
    }
    return true;
}

/* The rotations row a beamer config id "r<N>b..." names, or -1. Anchored, so
   finalizer ids (p3r0l5) and random-border ids (rndb0l1) name none; a beamer
   --prefix run's id "NAME_r<N>b..." is read after its last '_'. */
static long dr_id_row_at(const char *id) {
    if (id[0] != 'r' || id[1] < '0' || id[1] > '9') return -1;
    char *e;
    const long v = strtol(id + 1, &e, 10);
    return (*e == 'b' && v >= 0) ? v : -1;
}

static long dr_id_row(const char *id) {
    const long v = dr_id_row_at(id);
    const char *u = strrchr(id, '_');
    return (v >= 0 || !u) ? v : dr_id_row_at(u + 1);
}

/* -- Reopen: the cells lifted from a complete board ------------------------- */

/* Side d (0 top, 1 right, 2 bottom, 3 left) of piece p at CCW spin s. */
static inline int dr_side(int p, int s, int d) {
    const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
    return e[(d + s) & 3];
}

/* Broken edges of a complete board; brk[x] marks the cells they touch. */
static int dr_breaks(const uint16_t *pid, const uint8_t *rot, bool brk[NUM_PIECES]) {
    int n = 0;
    memset(brk, 0, NUM_PIECES * sizeof *brk);
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        if (c < PUZZLE_SIDE - 1 &&
            dr_side(pid[x], rot[x], 1) != dr_side(pid[x + 1], rot[x + 1], 3)) {
            n++; brk[x] = brk[x + 1] = true;
        }
        if (r < PUZZLE_SIDE - 1 &&
            dr_side(pid[x], rot[x], 0) != dr_side(pid[x + PUZZLE_SIDE], rot[x + PUZZLE_SIDE], 2)) {
            n++; brk[x] = brk[x + PUZZLE_SIDE] = true;
        }
    }
    return n;
}

/* Distance of cell x from side k (0 top, 1 bottom, 2 left, 3 right). */
static inline int dr_dist(int x, int k) {
    const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    return k == 0 ? PUZZLE_SIDE - 1 - r : k == 1 ? r : k == 2 ? c : PUZZLE_SIDE - 1 - c;
}

static int dr_cmp_int(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* The cells --reopen lifts from a complete board; returns how many, 0 when
   auto finds no damage. auto: of the four sides, the one whose outer band
   holds the most damaged cells (cells with a broken edge), that band deep
   enough to hold DR_AUTO_COVER of the damaged cells on its side's half of the
   board, at most DR_AUTO_CAP, plus auto+E's extra rows. Measured on dived and
   polished boards (damage in rows 12-15): re-diving rows 12-15 gained where
   rows 13-15 did not, and rows 11-15 gained less -- one stray deep break must
   not deepen the band (PROJECT_E555.md, E555_ender.py). */
static int dr_lift(const uint16_t *pid, const uint8_t *rot, const DrSpec *sp,
                   bool lift[NUM_PIECES]) {
    memset(lift, 0, NUM_PIECES * sizeof *lift);
    int side = sp->side, depth = sp->depth, n = 0;
    if (!sp->auto_band && side < 0) {                            /* box or mask */
        for (int x = 0; x < NUM_PIECES; x++) { lift[x] = sp->cell[x]; n += lift[x]; }
        return n;
    }
    if (sp->auto_band) {
        bool brk[NUM_PIECES];
        if (!dr_breaks(pid, rot, brk)) return 0;
        int best_hits = -1;
        side = -1;
        for (int k = 0; k < 4; k++) {
            int near[NUM_PIECES], nn = 0;
            for (int x = 0; x < NUM_PIECES; x++)
                if (brk[x] && dr_dist(x, k) < PUZZLE_SIDE / 2) near[nn++] = dr_dist(x, k);
            if (!nn) continue;
            qsort(near, (size_t)nn, sizeof *near, dr_cmp_int);
            int need = (int)ceil((double)nn * DR_AUTO_COVER);
            if (need < 1) need = 1;
            int d = near[need - 1] + 1;
            if (d > DR_AUTO_CAP) d = DR_AUTO_CAP;
            d += sp->depth;
            int hits = 0;
            for (int x = 0; x < NUM_PIECES; x++) hits += brk[x] && dr_dist(x, k) < d;
            if (hits > best_hits) { best_hits = hits; side = k; depth = d; }
        }
        if (side < 0) return 0;
    }
    for (int x = 0; x < NUM_PIECES; x++)
        if (dr_dist(x, side) < depth) { lift[x] = true; n++; }
    return n;
}

/* Lift the cells of `lift` from b's incumbent into b's board. */
static void dr_apply(DrBoard *b, const bool lift[NUM_PIECES]) {
    for (int x = 0; x < NUM_PIECES; x++) {
        b->pid[x] = lift[x] ? DV_EMPTY : b->inc_pid[x];
        b->rot[x] = lift[x] ? 0 : b->inc_rot[x];
    }
}

/* Write a board as a canonical row, outside the engine: a reopened board with
   nothing left to reopen, or a prepared board under --end_dive 0. The score is
   its matched edges between placed cells (480 for a solved board). */
static void dr_write_complete(FILE *fp, const char *id, const uint16_t *pid, const uint8_t *rot) {
    int score = 0;
    uint32_t pos[NUM_PIECES], rr[NUM_PIECES];
    for (int p = 0; p < NUM_PIECES; p++) { pos[p] = 999; rr[p] = 0; }
    for (int x = 0; x < NUM_PIECES; x++) {
        if (pid[x] == DV_EMPTY) continue;
        pos[pid[x]] = (uint32_t)x; rr[pid[x]] = rot[x];
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        if (c < PUZZLE_SIDE - 1 && pid[x + 1] != DV_EMPTY)
            score += dr_side(pid[x], rot[x], 1) == dr_side(pid[x + 1], rot[x + 1], 3);
        if (r < PUZZLE_SIDE - 1 && pid[x + PUZZLE_SIDE] != DV_EMPTY)
            score += dr_side(pid[x], rot[x], 0) == dr_side(pid[x + PUZZLE_SIDE], rot[x + PUZZLE_SIDE], 2);
    }
    fprintf(fp, "%s, %d", id, score);
    for (int p = 0; p < NUM_PIECES; p++) fprintf(fp, ", %u", pos[p]);
    for (int p = 0; p < NUM_PIECES; p++) fprintf(fp, ", %u", rr[p]);
    fprintf(fp, "\n");
    fflush(fp);
}

static uint64_t dr_fp(const uint16_t *pid, const uint8_t *rot) {
    uint64_t h = 14695981039346656037ULL;
    for (int x = 0; x < NUM_PIECES; x++) {
        h ^= pid[x]; h *= 1099511628211ULL;
        h ^= rot[x]; h *= 1099511628211ULL;
    }
    return h;
}

/* Parse one --reopen spec: auto, auto+E, top:K, bottom:K, left:K, right:K,
   box:R0-R1,C0-C1 (rows R0..R1 of columns C0..C1), or a 16x16 0/1 mask file
   (the --holes format: '#' comments, first data line row 0). */
static void dr_parse_spec(const char *spec, DrSpec *sp) {
    static const char *const sides[4] = { "top:", "bottom:", "left:", "right:" };
    memset(sp, 0, sizeof *sp);
    sp->side = -1;
    if (!strcmp(spec, "auto") || !strncmp(spec, "auto+", 5)) {
        sp->auto_band = true;
        sp->depth = spec[4] ? atoi(spec + 5) : 0;
        if (sp->depth < 0 || sp->depth > PUZZLE_SIDE / 2)
            fatal("--reopen auto+E: E must be in 0..%d", PUZZLE_SIDE / 2);
        return;
    }
    for (int k = 0; k < 4; k++) {
        const size_t n = strlen(sides[k]);
        if (strncmp(spec, sides[k], n)) continue;
        sp->side = k;
        sp->depth = atoi(spec + n);
        if (sp->depth < 1 || sp->depth > PUZZLE_SIDE)
            fatal("--reopen %s: K must be in 1..%d", spec, PUZZLE_SIDE);
        return;
    }
    if (!strncmp(spec, "box:", 4)) {
        int r0, r1, c0, c1;
        if (sscanf(spec + 4, "%d-%d,%d-%d", &r0, &r1, &c0, &c1) != 4 || r0 < 0 || r1 < r0 ||
            r1 >= PUZZLE_SIDE || c0 < 0 || c1 < c0 || c1 >= PUZZLE_SIDE)
            fatal("--reopen %s: want box:R0-R1,C0-C1 with 0 <= R0 <= R1 <= 15 and "
                  "0 <= C0 <= C1 <= 15", spec);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) sp->cell[r * PUZZLE_SIDE + c] = true;
        return;
    }
    FILE *fp = fopen(spec, "r");
    if (!fp) fatal("--reopen %s: not auto, auto+E, top:K, bottom:K, left:K, right:K, "
                   "or a readable mask file (%s)", spec, strerror(errno));
    char *line = NULL;
    size_t cap = 0;
    int row = 0, n = 0;
    while (row < PUZZLE_SIDE && getline(&line, &cap, fp) > 0) {
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '#' || *q == '%' || *q == '\n' || *q == '\r' || !*q) continue;
        int c = 0;
        for (; *q && c < PUZZLE_SIDE; q++) {
            if (*q != '0' && *q != '1') continue;
            sp->cell[row * PUZZLE_SIDE + c] = *q == '1';
            n += *q == '1';
            c++;
        }
        if (c != PUZZLE_SIDE) fatal("--reopen %s: data line %d has %d cells, not %d",
                                    spec, row + 1, c, PUZZLE_SIDE);
        row++;
    }
    free(line);
    fclose(fp);
    if (row != PUZZLE_SIDE) fatal("--reopen %s: %d data lines, not %d", spec, row, PUZZLE_SIDE);
    if (!n) fatal("--reopen %s: the mask opens no cell", spec);
}

/* --reopen, given n times: round r of a board reopens the (r mod n)-th spec. */
static void dr_parse_reopen(const char *arg) {
    if (g_nspec == DR_MAX_SPECS) fatal("--reopen: at most %d specs", DR_MAX_SPECS);
    if (!g_nspec) g_reopen = arg;
    g_spec_arg[g_nspec] = arg;
    dr_parse_spec(arg, &g_spec[g_nspec++]);
}

/* -- Prepare: holes, clue pins, backtrack ----------------------------------- */
static DrSpec      g_holes;
static const char *g_holes_arg = NULL;       /* --holes SPEC */
static int         g_pin       = 0;          /* --pin_clue N; 0 = off */
static int         g_orient    = -1;         /* its clue frame (g_clue row) */
static bool        g_backtrack = false;      /* --backtrack */
static int         g_stop_row  = -1;         /* --stop_row S; -1 = off */
static uint32_t    g_ext_nodes = 100000;     /* --extend_nodes N */
static bool        g_ext_set   = false;

#define DR_BT_NODES 10000000ULL              /* per board: rows and extensions together */

static struct {
    uint64_t holed, holes_lifted, pinned, clue_drop;
    uint64_t bt, reached, shortb, capped, ext_cells, ext_cols;
    int      ext_max, ext_cols_max, row_max;
} g_pr;

/* Lift the --holes cells from a board. */
static void dr_holes(DrBoard *b) {
    bool lift[NUM_PIECES];
    dr_lift(b->pid, b->rot, &g_holes, lift);
    int n = 0;
    for (int x = 0; x < NUM_PIECES; x++)
        if (lift[x] && b->pid[x] != DV_EMPTY) { b->pid[x] = DV_EMPTY; b->rot[x] = 0; n++; }
    g_pr.holed++;
    g_pr.holes_lifted += (uint64_t)n;
}

/* --pin_clue: put the frame's centre clue (and, with --clue_corners, its four
   corner clues) on the board. False when a clue cell holds another piece or a
   clue piece stands elsewhere or at another spin: the board cannot carry this
   frame. */
static bool dr_clue(DrBoard *b) {
    const int last = (g_clue_mask & CLUE_CORNERS) ? CLUE_N : 1;
    for (int k = 0; k < last; k++) {
        const ClueCell *cc = &g_clue[g_orient][k];
        const int x = cc->row * PUZZLE_SIDE + cc->col;
        if (b->pid[x] == cc->piece) { if (b->rot[x] != cc->spin) return false; continue; }
        if (b->pid[x] != DV_EMPTY) return false;
        for (int y = 0; y < NUM_PIECES; y++) if (b->pid[y] == cc->piece) return false;
    }
    for (int k = 0; k < last; k++) {
        const ClueCell *cc = &g_clue[g_orient][k];
        const int x = cc->row * PUZZLE_SIDE + cc->col;
        if (b->pid[x] == DV_EMPTY) { b->pid[x] = cc->piece; b->rot[x] = cc->spin; g_pr.pinned++; }
    }
    return true;
}

/* The zero-break search of --backtrack / --stop_row. A cell takes an unused
   piece whose sides match every placed neighbour and whose frame colour (0)
   faces exactly the board's edge: an inner cell draws from the catalog bucket
   g_lb_bucket[left][bottom] (the beamer's order), a border cell from the edge
   and corner orientations below, at their dealt spin under a rotations row.
   Top supply: while (15,c) is open, the top colour of a row-14 piece must be
   the inner colour of an unused top-border candidate not yet promised to
   another row-14 cell (avail[]); a candidate placed elsewhere spends one. */
typedef struct { uint16_t pid; uint8_t rot, t, r, b, l; } DrOr;
static DrOr g_eo[4 * NUM_PIECES];            /* edge and corner orientations, by (l, b) */
static int  g_eo_at[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL + 1];

static void dr_bt_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    build_catalog_indices();                 /* g_lb_bucket, without a database */
    int n = 0;
    for (int l = 0; l < NUM_COLORS_TOTAL; l++)
        for (int b = 0; b < NUM_COLORS_TOTAL; b++) {
            g_eo_at[l][b] = n;
            for (int p = 0; p < NUM_PIECES; p++) {
                const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
                if (e[0] && e[1] && e[2] && e[3]) continue;          /* inner */
                for (int s = 0; s < 4; s++)
                    if (dr_side(p, s, 3) == l && dr_side(p, s, 2) == b)
                        g_eo[n++] = (DrOr){ (uint16_t)p, (uint8_t)s, (uint8_t)dr_side(p, s, 0),
                                            (uint8_t)dr_side(p, s, 1), (uint8_t)b, (uint8_t)l };
            }
        }
    for (int l = 0; l < NUM_COLORS_TOTAL; l++) g_eo_at[l][NUM_COLORS_TOTAL] =
        l + 1 < NUM_COLORS_TOTAL ? g_eo_at[l + 1][0] : n;
}

#define DR_OPEN 0xFF
typedef struct {
    uint16_t pid[NUM_PIECES];
    uint8_t  rot[NUM_PIECES];
    uint8_t  side[NUM_PIECES][4];            /* placed colours; DR_OPEN = open */
    uint64_t used[4];
    int      avail[NUM_COLORS_TOTAL];
    int8_t   topc[NUM_PIECES];               /* inner colour a top candidate owes; -1 = none */
    bool     framed;
    int      nrow, next;                     /* row-phase and extension cells */
    uint8_t  rcell[NUM_PIECES], ecell[NUM_PIECES];
    uint16_t rp[NUM_PIECES], ep[NUM_PIECES]; /* the current path: piece, rotation */
    uint8_t  rr[NUM_PIECES], er[NUM_PIECES];
    int      rbest, ebest, leaf_best;        /* deepest row prefix, extension, best leaf's */
    uint16_t bp[NUM_PIECES];                 /* the board kept, per cell */
    uint8_t  br[NUM_PIECES];
    uint64_t nodes, enodes;
    bool     capped, done;
} DrBt;

static inline void bt_put(DrBt *t, int x, uint16_t p, uint8_t s, int tc, int rc, int bc, int lc) {
    t->pid[x] = p; t->rot[x] = s;
    t->side[x][0] = (uint8_t)tc; t->side[x][1] = (uint8_t)rc;
    t->side[x][2] = (uint8_t)bc; t->side[x][3] = (uint8_t)lc;
    used_set(t->used, p);
}
static inline void bt_take(DrBt *t, int x) {
    used_clear(t->used, t->pid[x]);
    t->pid[x] = DV_EMPTY;
    memset(t->side[x], DR_OPEN, 4);
}

/* Try every candidate of cell x; next(t, d+1) after each placement. Returns
   false when the search must stop (budget, or a full result). */
typedef bool (*DrNext)(DrBt *t, int d);
static bool bt_cell(DrBt *t, int x, int d, uint64_t *nodes, uint64_t cap, DrNext next) {
    const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    const int L = c ? t->side[x - 1][1] : 0, B = r ? t->side[x - PUZZLE_SIDE][0] : 0;
    if (L == DR_OPEN || B == DR_OPEN) return true;
    const int up = r < PUZZLE_SIDE - 1 ? t->side[x + PUZZLE_SIDE][2] : 0;
    const int rt = c < PUZZLE_SIDE - 1 ? t->side[x + 1][3] : 0;
    const bool supply = r == EDGE_LEN && up == DR_OPEN;
    if (r && r < PUZZLE_SIDE - 1 && c && c < PUZZLE_SIDE - 1) {
        if (!color_is_inner(L) || !color_is_inner(B)) return true;
        const int nb = g_lb_count[L][B];
        for (int k = 0; k < nb; k++) {
            const Oriented *o = &g_cat[g_lb_bucket[L][B][k]];
            if (up != DR_OPEN && o->top != up) continue;
            if (rt != DR_OPEN && o->right != rt) continue;
            if (supply && t->avail[o->top] <= 0) continue;
            if (used_test(t->used, o->piece_id)) continue;
            if (*nodes >= cap) return false;
            (*nodes)++;
            bt_put(t, x, o->piece_id, o->rotation, o->top, o->right, o->bottom, o->left);
            if (supply) t->avail[o->top]--;
            const bool go = next(t, d + 1);
            if (supply) t->avail[o->top]++;
            bt_take(t, x);
            if (!go) return false;
        }
        return true;
    }
    for (int k = g_eo_at[L][B]; k < g_eo_at[L][B + 1]; k++) {
        const DrOr *o = &g_eo[k];
        if ((o->r == 0) != (c == PUZZLE_SIDE - 1) || (o->t == 0) != (r == PUZZLE_SIDE - 1)) continue;
        if (up != DR_OPEN && o->t != up) continue;
        if (rt != DR_OPEN && o->r != rt) continue;
        if (t->framed && o->rot != g_spin[o->pid]) continue;
        if (used_test(t->used, o->pid)) continue;
        const int tc = t->topc[o->pid];
        if (tc >= 0 && t->avail[tc] <= 0) continue;
        if (*nodes >= cap) return false;
        (*nodes)++;
        bt_put(t, x, o->pid, o->rot, o->t, o->r, o->b, o->l);
        if (tc >= 0) t->avail[tc]--;
        const bool go = next(t, d + 1);
        if (tc >= 0) t->avail[tc]++;
        bt_take(t, x);
        if (!go) return false;
    }
    return true;
}

/* Extension: column-major over the open cells of rows S+1..14, cols 0..14;
   the deepest prefix, first found. */
static bool bt_ext(DrBt *t, int d) {
    if (d > t->ebest) {
        t->ebest = d;
        for (int k = 0; k < d; k++) {
            t->ep[k] = t->pid[t->ecell[k]]; t->er[k] = t->rot[t->ecell[k]];
        }
    }
    if (d == t->next) return false;
    return bt_cell(t, t->ecell[d], d, &t->enodes, g_ext_nodes, bt_ext);
}

/* Keep the board as it stands plus its extension prefix. */
static void bt_keep(DrBt *t) {
    memcpy(t->bp, t->pid, sizeof t->bp);
    memcpy(t->br, t->rot, sizeof t->br);
    for (int k = 0; k < t->ebest; k++) { t->bp[t->ecell[k]] = t->ep[k]; t->br[t->ecell[k]] = t->er[k]; }
}

/* Row phase: row-major over the open cells of rows 0..S (0..14 without S). */
static bool bt_row(DrBt *t, int d) {
    if (d > t->rbest) {
        t->rbest = d;
        for (int k = 0; k < d; k++) { t->rp[k] = t->pid[t->rcell[k]]; t->rr[k] = t->rot[t->rcell[k]]; }
    }
    if (d < t->nrow)
        return bt_cell(t, t->rcell[d], d, &t->nodes, DR_BT_NODES, bt_row);
    if (g_stop_row < 0) return false;                  /* every open cell filled */
    t->ebest = 0; t->enodes = 0;
    if (g_ext_nodes) bt_ext(t, 0);
    t->nodes += t->enodes;                             /* one budget for both phases */
    if (t->ebest > t->leaf_best) { t->leaf_best = t->ebest; bt_keep(t); }
    if (t->enodes >= g_ext_nodes && t->ebest < t->next) t->capped = true;
    return t->leaf_best < t->next && g_ext_nodes > 0;  /* a full region, or the first leaf, ends it */
}

/* Prepare one board in place: the best leaf at S, else the deepest row prefix. */
static void dr_backtrack(DrBoard *b, bool framed, DrBt *t) {
    memset(t, 0, sizeof *t);
    t->framed = framed;
    for (int x = 0; x < NUM_PIECES; x++) {
        t->pid[x] = b->pid[x]; t->rot[x] = b->rot[x];
        if (b->pid[x] == DV_EMPTY) { memset(t->side[x], DR_OPEN, 4); continue; }
        for (int s = 0; s < 4; s++) t->side[x][s] = (uint8_t)dr_side(b->pid[x], b->rot[x], s);
        used_set(t->used, b->pid[x]);
    }
    /* Top-border candidates: dealt to the top under a rotations row, any
       edge piece otherwise; each owes the top border its inner colour. */
    for (int p = 0; p < NUM_PIECES; p++) {
        t->topc[p] = -1;
        const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
        const int z = !e[0] + !e[1] + !e[2] + !e[3];
        if (z != 1 || used_test(t->used, (uint16_t)p)) continue;
        if (framed && dr_side(p, g_spin[p], 0) != 0) continue;
        for (int k = 0; k < 4; k++) if (!e[k]) t->topc[p] = (int8_t)e[(k + 2) & 3];
        t->avail[t->topc[p]]++;
    }
    const int top = g_stop_row >= 0 ? g_stop_row : EDGE_LEN;
    for (int x = 0; x < (top + 1) * PUZZLE_SIDE; x++)
        if (b->pid[x] == DV_EMPTY) t->rcell[t->nrow++] = (uint8_t)x;
    if (g_stop_row >= 0)
        for (int c = 0; c < EDGE_LEN + 1; c++)
            for (int r = g_stop_row + 1; r <= EDGE_LEN; r++)
                if (b->pid[r * PUZZLE_SIDE + c] == DV_EMPTY) t->ecell[t->next++] = (uint8_t)(r * PUZZLE_SIDE + c);
    t->leaf_best = -1;
    bt_row(t, 0);
    if (t->nodes >= DR_BT_NODES) t->capped = true;
    if (t->leaf_best >= 0) {                           /* reached S: the best leaf */
        memcpy(b->pid, t->bp, sizeof t->bp);
        memcpy(b->rot, t->br, sizeof t->br);
        return;
    }
    for (int k = 0; k < t->rbest; k++) {               /* the deepest row prefix */
        b->pid[t->rcell[k]] = t->rp[k]; b->rot[t->rcell[k]] = t->rr[k];
    }
}

/* Whole columns of the region above S a board fills, from column 1. */
static int dr_ext_cols(const DrBoard *b) {
    int n = 0;
    for (int c = 1; c <= EDGE_LEN; c++) {
        for (int r = g_stop_row + 1; r <= EDGE_LEN; r++)
            if (b->pid[r * PUZZLE_SIDE + c] == DV_EMPTY) return n;
        n++;
    }
    return n;
}

/* Backtrack boards idx[0..n) of the batch under one frame, in parallel; each
   board's search is serial, so the result does not depend on --threads. */
static void dr_prepare(const size_t *idx, size_t n, bool framed) {
    if (!n || !g_backtrack) return;
    dr_bt_init();
    int *reach = xmalloc(3 * n * sizeof *reach), *cap = reach + n, *filled = cap + n;
    #pragma omp parallel
    {
        DrBt *t = xmalloc(sizeof *t);
        #pragma omp for schedule(dynamic, 1)
        for (size_t i = 0; i < n; i++) {
            DrBoard *b = &g_batch.b[idx[i]];
            dr_backtrack(b, framed, t);
            reach[i] = t->leaf_best >= 0;
            cap[i]   = t->capped;
            filled[i] = t->leaf_best >= 0 ? t->leaf_best : t->rbest;
        }
        free(t);
    }
    for (size_t i = 0; i < n; i++) {
        const DrBoard *b = &g_batch.b[idx[i]];
        g_pr.bt++;
        g_pr.capped += cap[i];
        int row = -1;                                 /* highest full row */
        for (int r = 0; r < PUZZLE_SIDE; r++) {
            int x = 0;
            while (x < PUZZLE_SIDE && b->pid[r * PUZZLE_SIDE + x] != DV_EMPTY) x++;
            if (x < PUZZLE_SIDE) break;
            row = r;
        }
        if (row > g_pr.row_max) g_pr.row_max = row;
        if (g_stop_row < 0) continue;
        if (!reach[i]) { g_pr.shortb++; continue; }
        g_pr.reached++;
        g_pr.ext_cells += (uint64_t)filled[i];
        if (filled[i] > g_pr.ext_max) g_pr.ext_max = filled[i];
        const int cols = dr_ext_cols(b);
        g_pr.ext_cols += (uint64_t)cols;
        if (cols > g_pr.ext_cols_max) g_pr.ext_cols_max = cols;
    }
    free(reach);
}

/* -- Rotations rows and the corner catalog, cached per row ----------------- */
static long g_row = -2;            /* the row g_spin holds; -2 = none yet */
static bool g_row_ok = false;      /* found in the file */
static bool g_row_seed = false;    /* its corner catalog can seed */
static bool g_row_pooled = false;  /* the catalog pools the top and right edges */

/* The corner catalog of the loaded row. Pooled is the beamer's --free_edges
   catalog: any unused top or right edge may fill a TR side cell or witness the
   top border, as a free-edge dive may put it there. */
static void dr_build_catalog(bool pooled) {
    char lab[64];
    snprintf(lab, sizeof lab, "rotations row %ld%s", g_row, pooled ? " (free edges)" : "");
    g_tc_pool_top_right = pooled;
    g_row_pooled = pooled;
    g_row_seed = tc_build(g_clue_orients, (g_clue_mask & CLUE_CORNERS) != 0, lab) != 0;
}

static void dr_load_row(long row) {
    if (row == g_row) return;
    g_row = row;
    g_row_ok = g_row_seed = false;
    uint8_t spins[NUM_PIECES];
    if (!read_one_border_row(g_rot_path, (uint32_t)row, spins)) {
        printf("[diver] rotations row %ld not in %s: its boards are dived with free edges\n",
               row, g_rot_path);
        fflush(stdout);
        return;
    }
    memcpy(g_spin, spins, sizeof g_spin);
    classify_deal_from_rotations();
    g_row_ok = true;
    if (g_seeds > 0) dr_build_catalog(false);
}

/* tc_config for the board's column 0. Its live lists depend only on the pieces
   at (13,0) and (14,0), so the last call is reused while those stay the same. */
static void dr_corner_config(const DrBoard *b) {
    static int last_hi = -2, last_lo = -2;
    static long last_row = -2;
    static bool last_pooled = false;
    const int xh = (PUZZLE_SIDE - 2) * PUZZLE_SIDE, xl = (PUZZLE_SIDE - 3) * PUZZLE_SIDE;
    const int hi = b->pid[xh] == DV_EMPTY ? -1 : b->pid[xh];
    const int lo = b->pid[xl] == DV_EMPTY ? -1 : b->pid[xl];
    if (hi == last_hi && lo == last_lo && last_row == g_row && last_pooled == g_row_pooled) return;
    last_hi = hi; last_lo = lo; last_row = g_row; last_pooled = g_row_pooled;
    static Oriented col[PUZZLE_SIDE];
    LeftOrder lft;
    memset(&lft, 0, sizeof lft);
    uint64_t used[4] = { 0, 0, 0, 0 };
    for (int x = 0; x < NUM_PIECES; x++)
        if (b->pid[x] != DV_EMPTY) used_set(used, b->pid[x]);
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        const int x = r * PUZZLE_SIDE;
        if (b->pid[x] == DV_EMPTY) continue;
        col[r].piece_id = b->pid[x];
        col[r].rotation = b->rot[x];
        lft.p[r] = &col[r];
    }
    (void)tc_config(&lft, used);
}

/* -- Batches ---------------------------------------------------------------- */
static FILE  *g_out;
static double g_deadline = 0.0;

static bool dr_time_up(void) {
    return g_stop || (g_deadline > 0.0 && omp_get_wtime() >= g_deadline);
}

/* Queue boards idx[0..n) of the batch, a reopened one with its incumbent;
   `salt` gives a round's copies streams of their own. */
static void dr_queue(const size_t *idx, size_t n, bool seed, uint64_t salt) {
    for (size_t i = 0; i < n; i++) {
        const DrBoard *b = &g_batch.b[idx[i]];
        if (seed) dr_corner_config(b);
        DvQueue o = { .copies = g_copies, .orders = g_orders, .norders = g_norders, .salt = salt };
        if (b->cut) { o.inc_pid = b->inc_pid; o.inc_rot = b->inc_rot; }
        dv_queue(b->pid, b->rot, &o);
    }
}

/* Dive boards idx[0..n) of the batch under one frame, and write the result:
   first the boards dived as they came, then the reopened ones, round after
   round, each round re-cutting every board from its best so far. */
static void dr_run(const size_t *idx, size_t n, bool by_side, bool seed) {
    if (!n) return;
    dv_frame(by_side);
    dv_seeding(seed);
    size_t *plain = xmalloc(2 * n * sizeof *plain), *cut = plain + n, np = 0, nc = 0;
    for (size_t i = 0; i < n; i++) {
        if (g_batch.b[idx[i]].cut) cut[nc++] = idx[i];
        else                       plain[np++] = idx[i];
    }
    if (g_verbose) {
        printf("[batch] %s: %zu board(s), %s edges%s", g_batch.id, n,
               by_side ? "dealt" : "free", seed ? ", corner seeding" : "");
        if (nc) printf(", %zu reopened x %d round(s)", nc, g_rounds);
        if (g_copies > 1) printf(", %u copies each", g_copies);
        printf("\n");
        fflush(stdout);
    }
    if (np) {
        dr_queue(plain, np, seed, 0);
        dv_run(g_batch.id);
        dv_flush(g_out);
    }
    if (nc && !g_stop) {
        dv_keeping(false);
        size_t live = nc;
        for (int round = 0; round < g_rounds && live; round++) {
            if (round > 0) {
                /* The next round dives each board's best so far, the auto band
                   chosen afresh; a board with nothing left to reopen is done. */
                size_t k = 0;
                for (size_t i = 0; i < live; i++) {
                    DrBoard *b = &g_batch.b[cut[i]];
                    bool lift[NUM_PIECES];
                    if (!dr_lift(b->inc_pid, b->inc_rot, &g_spec[round % g_nspec], lift)) {
                        dr_write_complete(g_out, g_batch.id, b->inc_pid, b->inc_rot);
                        g_st.solved++;
                        b->done = true;
                        continue;
                    }
                    bool prev[NUM_PIECES];
                    for (int x = 0; x < NUM_PIECES; x++) prev[x] = b->pid[x] == DV_EMPTY;
                    dr_apply(b, lift);
                    if (!dv_fits(b->pid)) dr_apply(b, prev);   /* the last cut always fits */
                    cut[k++] = cut[i];
                }
                live = k;
                if (!live) break;
            }
            dr_queue(cut, live, seed, (uint64_t)round);
            dv_run(g_batch.id);
            for (size_t i = 0; i < live; i++) {
                DrBoard *b = &g_batch.b[cut[i]];
                if (dv_result(i, b->inc_pid, b->inc_rot) < 0)
                    fatal("internal error: a reopened board lost its incumbent");
            }
            if (g_verbose && g_rounds > 1) {
                int best = -1;
                for (size_t i = 0; i < live; i++) {
                    bool brk[NUM_PIECES];
                    const DrBoard *b = &g_batch.b[cut[i]];
                    const int sc = 480 - dr_breaks(b->inc_pid, b->inc_rot, brk);
                    if (sc > best) best = sc;
                }
                printf("[round] %s: round %d of %d, %zu board(s), best %d\n",
                       g_batch.id, round + 1, g_rounds, live, best);
                fflush(stdout);
            }
            if (dr_time_up()) break;
        }
        if (live) dv_keep_last();          /* the last round holds every live board's best */
        dv_keeping(true);
        dv_flush(g_out);
        for (size_t i = 0; i < n; i++) {
            const DrBoard *b = &g_batch.b[idx[i]];
            if (!b->cut) continue;
            bool brk[NUM_PIECES];
            const int sc = 480 - dr_breaks(b->inc_pid, b->inc_rot, brk);
            if (sc > b->orig) { g_st.better++; g_st.gain += sc - b->orig; }
            else if (dr_fp(b->inc_pid, b->inc_rot) != b->orig_fp) g_st.moved++;
        }
    }
    free(plain);
    g_st.dived += n;
}

static void dr_batch(void) {
    if (!g_batch.n) return;
    g_st.batches++;
    size_t *dealt = xmalloc(2 * g_batch.n * sizeof *dealt), *freed = dealt + g_batch.n;
    size_t nd = 0, nf = 0;
    const long row = g_rot_path ? dr_id_row(g_batch.id) : -1;
    if (row >= 0) dr_load_row(row);
    const bool framed = row >= 0 && g_row_ok;
    if (framed) {
        dv_frame(true);
        for (size_t i = 0; i < g_batch.n; i++)
            if (dv_fits(g_batch.b[i].pid)) dealt[nd++] = i;
            else                           freed[nf++] = i;
        g_st.misfit += nf;
    } else {
        for (size_t i = 0; i < g_batch.n; i++) freed[nf++] = i;
        g_st.unframed += nf;
    }
    if (nf) {                                /* can they complete at all? */
        dv_frame(false);
        size_t k = 0;
        for (size_t i = 0; i < nf; i++) {
            if (dv_fits(g_batch.b[freed[i]].pid)) { freed[k++] = freed[i]; continue; }
            printf("[diver] line %" PRIu64 " (%s): the open cells cannot take the unused "
                   "pieces (corners, edges and inner pieces do not balance); skipped\n",
                   g_batch.b[freed[i]].line, g_batch.id);
            g_st.malformed++;
        }
        if (framed && k)
            printf("[diver] %s: %zu board(s) do not fit rotations row %ld; dived with free "
                   "edges\n", g_batch.id, k, row);
        nf = k;
    }
    dr_prepare(dealt, nd, true);
    dr_prepare(freed, nf, false);
    if (!g_dives) {                          /* --end_dive 0: the prepared boards */
        if (g_verbose) {
            if (nd) printf("[batch] %s: %zu board(s), dealt edges, prepared only\n", g_batch.id, nd);
            if (nf) printf("[batch] %s: %zu board(s), free edges, prepared only\n", g_batch.id, nf);
            fflush(stdout);
        }
        bool *keep = xmalloc(g_batch.n * sizeof *keep);
        memset(keep, 0, g_batch.n * sizeof *keep);
        for (size_t i = 0; i < nd; i++) keep[dealt[i]] = true;
        for (size_t i = 0; i < nf; i++) keep[freed[i]] = true;
        for (size_t i = 0; i < g_batch.n; i++) {
            if (!keep[i] || (g_max_written && g_st.raw >= g_max_written)) continue;
            dr_write_complete(g_out, g_batch.id, g_batch.b[i].pid, g_batch.b[i].rot);
            g_st.raw++;
        }
        g_st.dived += nd + nf;
        free(keep); free(dealt);
        g_batch.n = 0;
        return;
    }
    if (nd && g_seeds > 0 && g_row_pooled) dr_build_catalog(false);
    dr_run(dealt, nd, true, framed && g_row_seed);
    /* Free-edge boards of a named row -- a beamer --free_edges run -- seed from
       the row's pooled catalog, as the beamer did. */
    if (nf && framed && g_seeds > 0 && !g_row_pooled) dr_build_catalog(true);
    if (!g_stop) dr_run(freed, nf, false, framed && g_row_seed);
    free(dealt);
    g_batch.n = 0;
}

static void dr_take(const char *id, const DrBoard *b) {
    if (g_batch.n && strcmp(id, g_batch.id) != 0) dr_batch();
    if (!g_batch.n) snprintf(g_batch.id, sizeof g_batch.id, "%s", id);
    if (g_batch.n == g_batch.cap) {
        g_batch.cap = g_batch.cap ? 2 * g_batch.cap : 64;
        g_batch.b = xrealloc(g_batch.b, g_batch.cap * sizeof *g_batch.b);
    }
    g_batch.b[g_batch.n++] = *b;
}

static bool dr_done(double deadline) {
    return g_stop || (deadline > 0.0 && omp_get_wtime() >= deadline) ||
           (g_max_written && dv_written() + g_st.raw >= g_max_written);
}

/* -- Command line ----------------------------------------------------------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "\nUsage: %s seed.txt boards.csv output.csv [options]\n\n"
        "Finish every board of boards.csv with the beamer's end dives: placed cells stay,\n"
        "open cells are filled by greedy random dives that allow broken edges, the best\n"
        "boards learn from their own best dives, and --end_polish hill-climbs the winners.\n"
        "Consecutive rows with the same config id are one batch. Output: canonical rows\n"
        "\"config_id, score, pos[256], rot[256]\", best first per batch, and\n"
        "output.csv.outputs.txt.\n\n", prog);
    fprintf(stderr,
        "Options:\n"
        "  --end_dive M        dives per board (default %d)\n"
        "  --end_polish R      polish the winners with R kick-and-polish rounds (default off)\n"
        "  --emit_score S      write a board when its best finish is >= S (default %d)\n"
        "  --rotations FILE    Stage A rotations: a beamer id \"r<N>b...\" holds edge pieces to\n"
        "                      the sides row N deals them (default: any edge on any border cell)\n"
        "  --corner_seeds N    also dive N copies of each board with an alive top-corner\n"
        "                      block placed (needs --rotations; default 0 = off)\n"
        "  --clue_corners      corner blocks use the 2x3 clue template; with --pin_clue the\n"
        "                      frame's four corner clues are also placed (see below)\n"
        "  --threads N         worker threads (default: all)\n"
        "  --rng_seed S        keys every dive (default %d)\n"
        "  --wall_time S       stop after S seconds; the current batch is still written\n"
        "  --max_emitted N     stop after writing N boards\n"
        "\nImproving complete boards (a dived and polished board, say):\n"
        "  --reopen SPEC       lift cells from every complete board and dive them again; the\n"
        "                      board written is the old one unless a dive beats it. SPEC:\n"
        "                      auto (the rows or columns of the side holding the damage,\n"
        "                      deep enough for 90%% of it, at most 5), auto+E (E rows more),\n"
        "                      top:K, bottom:K, left:K, right:K, box:R0-R1,C0-C1, or a\n"
        "                      16x16 0/1 mask file. Given n times, round r reopens the\n"
        "                      (r mod n)-th\n"
        "  --rounds N          re-dive each board's best N times (default 1)\n"
        "  --plateau           a round may also move a board to a different one of the\n"
        "                      same score (never to a worse one)\n"
        "  --copies K          dive every board K times, each copy on streams of its own;\n"
        "                      the best copy is written (default 1)\n"
        "  --orders LIST       copy k starts filling at LIST[k mod n]: mrv (the stock dive),\n"
        "                      left, right, centre, ends, top, bottom (default mrv)\n"
        "  --prior A           dives of a reopened board start pulled toward its clean\n"
        "                      placements (weight A, 0..2; default 0 = off)\n"
        "  --nogo B            ... and pushed off its placements on broken cells (0..2)\n"
        "\nPreparing boards before the dives (in this order):\n"
        "  --start_row R       read input records R.. only (0-based data rows, not a board row)\n"
        "  --num_rows N        ... and at most N of them (default 0 = the rest)\n"
        "  --holes SPEC        lift these cells from every board: a 16x16 0/1 mask file (first\n"
        "                      data line = row 0), top:K, bottom:K, left:K, right:K or\n"
        "                      box:R0-R1,C0-C1. Not with --reopen\n"
        "  --pin_clue N        place the centre clue of frame N (1 lower-left (7,7), 2 lower-\n"
        "                      right (7,8), 3 upper-right (8,8), 4 upper-left (8,7)); with\n"
        "                      --clue_corners also its four corner clues. A board whose clue\n"
        "                      cell or clue piece is taken otherwise is dropped\n"
        "  --backtrack         fill open cells of rows 0..14 row by row with zero breaks, as\n"
        "                      deep as the search gets (%llu nodes a board), then dive\n"
        "  --stop_row S        backtrack rows up to S (1..14); every board that completes them\n"
        "                      is extended column by column over rows S+1..14, cols 0..14, and\n"
        "                      the one whose extension goes deepest is dived. Implies\n"
        "                      --backtrack\n"
        "  --extend_nodes N    node budget of each extension (default 100000; 0 = keep the\n"
        "                      first board to reach S)\n"
        "  --end_dive 0        write the prepared boards (score = matched edges) without diving\n"
        "\n"
        "  --print_cmd         echo the full command line\n"
        "  --verbose           one line per batch\n\n",
        DR_M_DEFAULT, DR_S_DEFAULT, DR_SEED_DEFAULT, (unsigned long long)DR_BT_NODES);
}

static void print_cmd(const char *a0, const char *seed, const char *in, const char *out) {
    printf("[cmd] %s %s %s %s --end_dive %u --emit_score %d", a0, seed, in, out, g_dives, g_emit);
    if (g_polish >= 0)                printf(" --end_polish %d", g_polish);
    if (g_rot_path)                   printf(" --rotations %s", g_rot_path);
    if (g_seeds)                      printf(" --corner_seeds %d", g_seeds);
    if (g_clue_mask & CLUE_CORNERS)   printf(" --clue_corners");
    printf(" --threads %d --rng_seed %" PRIu64, g_nthreads, g_rng);
    if (g_wall > 0.0)                 printf(" --wall_time %g", g_wall);
    if (g_max_written)                printf(" --max_emitted %" PRIu64, g_max_written);
    for (int k = 0; k < g_nspec; k++) printf(" --reopen %s", g_spec_arg[k]);
    if (g_reopen)                     printf(" --rounds %d", g_rounds);
    if (g_plateau)                    printf(" --plateau");
    if (g_copies > 1)                 printf(" --copies %u", g_copies);
    if (g_norders) {
        printf(" --orders ");
        for (int k = 0; k < g_norders; k++) printf("%s%s", k ? "," : "", dv_order_names[g_orders[k]]);
    }
    if (g_order_weight > 0.0f)        printf(" --order_weight %g", g_order_weight);
    if (g_prior > 0.0f)               printf(" --prior %g", g_prior);
    if (g_nogo > 0.0f)                printf(" --nogo %g", g_nogo);
    if (g_rec_start)                  printf(" --start_row %" PRIu64, g_rec_start);
    if (g_rec_count)                  printf(" --num_rows %" PRIu64, g_rec_count);
    if (g_holes_arg)                  printf(" --holes %s", g_holes_arg);
    if (g_pin)                        printf(" --pin_clue %d", g_pin);
    if (g_stop_row >= 0)              printf(" --stop_row %d --extend_nodes %u", g_stop_row, g_ext_nodes);
    else if (g_backtrack)             printf(" --backtrack");
    if (g_print_cmd)                  printf(" --print_cmd");
    if (g_verbose)                    printf(" --verbose");
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }
    const char *seed_path = argv[1], *in_path = argv[2], *out_path = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--end_dive") && i + 1 < argc) {
            const long v = atol(argv[++i]);
            if ((v < 10 && v != 0) || v > 1000000000L)
                fatal("--end_dive must be 0 (no dives) or in 10..1e9");
            g_dives = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "--end_polish") && i + 1 < argc) {
            g_polish = atoi(argv[++i]);
            if (g_polish < 0) fatal("--end_polish must be >= 0");
        }
        else if (!strcmp(argv[i], "--emit_score")  && i + 1 < argc) g_emit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rotations")   && i + 1 < argc) g_rot_path = argv[++i];
        else if (!strcmp(argv[i], "--corner_seeds") && i + 1 < argc) {
            g_seeds = atoi(argv[++i]);
            if (g_seeds < 0 || g_seeds > 64) fatal("--corner_seeds must be in 0..64");
        }
        else if (!strcmp(argv[i], "--clue_corners")) g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(argv[i], "--threads")     && i + 1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rng_seed")    && i + 1 < argc) {
            unsigned long long s;
            if (!parse_u64_token(argv[++i], &s)) fatal("--rng_seed needs an integer");
            g_rng = (uint64_t)s;
        }
        else if (!strcmp(argv[i], "--wall_time")   && i + 1 < argc) g_wall = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max_emitted") && i + 1 < argc) g_max_written = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--copies")      && i + 1 < argc) {
            const long v = atol(argv[++i]);
            if (v < 1 || v > DR_MAX_COPIES) fatal("--copies must be in 1..%d", DR_MAX_COPIES);
            g_copies = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "--orders")      && i + 1 < argc) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            g_norders = 0;
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
                int k = 0;
                while (k < DV_NORDERS && strcmp(t, dv_order_names[k])) k++;
                if (k == DV_NORDERS) fatal("--orders: unknown order '%s' (mrv, left, right, "
                                           "centre, ends, top, bottom)", t);
                if (g_norders == (int)sizeof g_orders) fatal("--orders: at most %d entries",
                                                             (int)sizeof g_orders);
                g_orders[g_norders++] = (uint8_t)k;
            }
            if (!g_norders) fatal("--orders needs at least one order");
        }
        else if (!strcmp(argv[i], "--order_weight") && i + 1 < argc) {
            g_order_weight = (float)atof(argv[++i]);
            if (g_order_weight <= 0.0f) fatal("--order_weight must be > 0");
        }
        else if (!strcmp(argv[i], "--reopen")      && i + 1 < argc) dr_parse_reopen(argv[++i]);
        else if (!strcmp(argv[i], "--plateau"))    g_plateau = true;
        else if (!strcmp(argv[i], "--rounds")      && i + 1 < argc) {
            g_rounds = atoi(argv[++i]);
            if (g_rounds < 1) fatal("--rounds must be >= 1");
        }
        else if (!strcmp(argv[i], "--prior")       && i + 1 < argc) {
            g_prior = (float)atof(argv[++i]);
            if (g_prior < 0.0f || g_prior > 2.0f) fatal("--prior must be in 0..2");
        }
        else if (!strcmp(argv[i], "--nogo")        && i + 1 < argc) {
            g_nogo = (float)atof(argv[++i]);
            if (g_nogo < 0.0f || g_nogo > 2.0f) fatal("--nogo must be in 0..2");
        }
        else if (!strcmp(argv[i], "--start_row")   && i + 1 < argc) {
            unsigned long long v;
            if (!parse_u64_token(argv[++i], &v))
                fatal("--start_row expects a non-negative integer (0-based record index), got '%s'", argv[i]);
            g_rec_start = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "--num_rows")    && i + 1 < argc) {
            unsigned long long v;
            if (!parse_u64_token(argv[++i], &v))
                fatal("--num_rows expects a non-negative integer (0 = every remaining record), got '%s'", argv[i]);
            g_rec_count = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "--holes")       && i + 1 < argc) {
            g_holes_arg = argv[++i];
            if (!strncmp(g_holes_arg, "auto", 4))
                fatal("--holes takes a mask file, top:K, bottom:K, left:K, right:K or "
                      "box:R0-R1,C0-C1 (auto belongs to --reopen)");
            dr_parse_spec(g_holes_arg, &g_holes);
        }
        else if (!strcmp(argv[i], "--pin_clue")    && i + 1 < argc) {
            g_pin = atoi(argv[++i]);
            if (g_pin < 1 || g_pin > 4)
                fatal("--pin_clue takes 1..4 (1 lower-left, 2 lower-right, 3 upper-right, 4 upper-left)");
        }
        else if (!strcmp(argv[i], "--backtrack"))  g_backtrack = true;
        else if (!strcmp(argv[i], "--stop_row")    && i + 1 < argc) {
            char *e; errno = 0;
            const long v = strtol(argv[++i], &e, 10);
            if (errno || e == argv[i] || *e || v < 1 || v > EDGE_LEN)
                fatal("--stop_row expects a board row in 1..%d, got '%s'", EDGE_LEN, argv[i]);
            g_stop_row = (int)v;
            g_backtrack = true;
        }
        else if (!strcmp(argv[i], "--extend_nodes") && i + 1 < argc) {
            unsigned long long v;
            if (!parse_u64_token(argv[++i], &v) || v > 1000000000ULL)
                fatal("--extend_nodes expects an integer in 0..1e9, got '%s'", argv[i]);
            g_ext_nodes = (uint32_t)v;
            g_ext_set = true;
        }
        else if (!strcmp(argv[i], "--print_cmd"))  g_print_cmd = true;
        else if (!strcmp(argv[i], "--verbose"))    g_verbose = true;
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); usage(argv[0]); return 1; }
    }
    if (g_nthreads <= 0) g_nthreads = omp_get_max_threads();
    omp_set_num_threads(g_nthreads);
    if (g_rot_path) {                       /* read per batch later: check it now */
        FILE *rf = fopen(g_rot_path, "r");
        if (!rf) fatal("cannot open rotation CSV %s: %s", g_rot_path, strerror(errno));
        fclose(rf);
    }
    if (g_seeds && !g_rot_path) fatal("--corner_seeds needs --rotations: the corner blocks "
                                      "are built from the sides a rotations row deals");
    if ((g_clue_mask & CLUE_CORNERS) && !g_seeds && !g_pin)
        printf("[warn] --clue_corners pins clues only with --pin_clue, and shapes "
               "--corner_seeds; ignored\n");
    if (!g_reopen && (g_prior > 0.0f || g_nogo > 0.0f || g_rounds > 1 || g_plateau))
        printf("[warn] --prior, --nogo, --rounds and --plateau act on reopened boards only "
               "(--reopen)\n");
    if (g_holes_arg && g_reopen)
        fatal("--holes and --reopen do not combine: --holes makes every board a partial to "
              "finish, --reopen improves complete boards against themselves");
    if (!g_dives && g_reopen) fatal("--end_dive 0 writes prepared boards; --reopen needs dives");
    if (g_backtrack && g_reopen)
        fatal("--backtrack/--stop_row rebuild partial boards; --reopen keeps a complete board's "
              "cells as its incumbent -- use one or the other");
    if (g_ext_set && g_stop_row < 0) fatal("--extend_nodes needs --stop_row");
    if (g_pin) {
        g_orient = clue_orient_for_pin(g_pin);
        g_clue_mask |= CLUE_CENTER;
        g_clue_orients = (uint8_t)(1u << g_orient);
    }
    if (g_print_cmd) print_cmd(argv[0], seed_path, in_path, out_path);
    printf("[cfg] dives=%u polish=%d emit_score=%d frame=%s corner_seeds=%d%s threads=%d rng_seed=%" PRIu64 "\n",
           g_dives, g_polish, g_emit, g_rot_path ? g_rot_path : "(free edges)", g_seeds,
           (g_clue_mask & CLUE_CORNERS) ? " (clue 2x3)" : "", g_nthreads, g_rng);
    if (g_holes_arg || g_pin || g_backtrack || g_rec_start || g_rec_count) {
        printf("[cfg] records=%" PRIu64 "..", g_rec_start);
        if (g_rec_count) printf("%" PRIu64, g_rec_start + g_rec_count - 1);
        printf(" holes=%s clues=", g_holes_arg ? g_holes_arg : "none");
        if (g_pin) printf("pin %d (%s)%s", g_pin, clue_pin_quadrant_name(g_pin),
                          (g_clue_mask & CLUE_CORNERS) ? " + corners" : "");
        else       printf("as read");
        if (g_stop_row >= 0) printf(" backtrack=rows to %d, then columns (extend_nodes=%u)",
                                    g_stop_row, g_ext_nodes);
        else if (g_backtrack) printf(" backtrack=rows");
        printf("\n");
    }
    if (g_reopen || g_copies > 1 || g_norders) {
        printf("[cfg] reopen=");
        if (!g_nspec) printf("off");
        for (int k = 0; k < g_nspec; k++) printf("%s%s", k ? " then " : "", g_spec_arg[k]);
        printf(" rounds=%d copies=%u orders=", g_rounds, g_copies);
        if (!g_norders) printf("mrv");
        for (int k = 0; k < g_norders; k++) printf("%s%s", k ? "," : "", dv_order_names[g_orders[k]]);
        printf(" prior=%g nogo=%g%s\n", g_prior, g_nogo, g_plateau ? " plateau" : "");
    }
    fflush(stdout);

    const double t0 = omp_get_wtime();
    load_seed_and_catalog(seed_path);
    FILE *in = fopen(in_path, "r");
    if (!in) fatal("cannot open %s: %s", in_path, strerror(errno));
    g_out = fopen(out_path, "w");
    if (!g_out) fatal("cannot open %s: %s", out_path, strerror(errno));

    signal(SIGINT, handle_stop); signal(SIGTERM, handle_stop);
    const double deadline = g_wall > 0.0 ? t0 + g_wall : 0.0;
    g_deadline = deadline;
    DvParams dp = {
        .dives = g_dives, .emit_score = g_emit, .polish = g_polish,
        .corner_seeds = g_seeds, .seed_corners = g_seeds > 0,
        .master_seed = g_rng, .max_written = g_max_written,
        .deadline = deadline, .threads = g_nthreads, .stop = &g_stop,
        .prior = g_prior, .nogo = g_nogo, .order_weight = g_order_weight,
        .plateau = g_plateau,
    };
    dv_init(&dp);

    char *line = NULL, **f = xmalloc(DR_MAX_FIELDS * sizeof *f);
    size_t lcap = 0;
    uint64_t lno = 0, nrec = 0;
    DrBoard b;
    while (!dr_done(deadline) && getline(&line, &lcap, in) > 0) {
        lno++;
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '%' || *s == '\n' || *s == '\r' || !*s) continue;
        const uint64_t rec = nrec++;
        if (rec < g_rec_start) continue;
        if (g_rec_count && rec >= g_rec_start + g_rec_count) break;
        const int n = dr_split(line, f, DR_MAX_FIELDS);
        const char *why = "more fields than any board row";
        if (n > DR_MAX_FIELDS || !dr_parse(f, n, &b, &why)) {
            printf("[diver] line %" PRIu64 ": not a board (%s); skipped\n", lno, why);
            g_st.malformed++;
            continue;
        }
        g_st.rows++;
        b.line = lno;
        b.cut = b.done = false;
        if (g_holes_arg) dr_holes(&b);
        if (g_pin && !dr_clue(&b)) { g_pr.clue_drop++; continue; }
        bool open = false;
        for (int x = 0; x < NUM_PIECES && !open; x++) open = b.pid[x] == DV_EMPTY;
        const char *id = n > 2 * NUM_PIECES ? f[0] : "board";
        if (!open) {
            if (!g_reopen) { g_st.complete++; continue; }
            bool lift[NUM_PIECES], brk[NUM_PIECES];
            const int nl = dr_lift(b.pid, b.rot, &g_spec[0], lift);
            if (!nl) {                          /* auto, and nothing is broken */
                if (strcmp(id, g_batch.id) != 0) dr_batch();
                dr_write_complete(g_out, id, b.pid, b.rot);
                g_st.solved++;
                continue;
            }
            memcpy(b.inc_pid, b.pid, sizeof b.pid);
            memcpy(b.inc_rot, b.rot, sizeof b.rot);
            b.orig = 480 - dr_breaks(b.pid, b.rot, brk);
            b.orig_fp = dr_fp(b.pid, b.rot);
            dr_apply(&b, lift);
            b.cut = true;
            g_st.reopened++;
            g_st.lifted += (uint64_t)nl;
        }
        dr_take(id, &b);
    }
    if (!dr_done(deadline)) dr_batch();
    fclose(in);
    free(line); free(f); free(g_batch.b);
    if (fclose(g_out) != 0) fatal("writing %s failed", out_path);

    char man[PATH_MAX];
    snprintf(man, sizeof man, "%s.outputs.txt", out_path);
    FILE *mf = fopen(man, "w");
    if (mf) { fprintf(mf, "%s\n", out_path); fclose(mf); }

    const double wall = omp_get_wtime() - t0;
    printf("[sum] diver: %" PRIu64 " board row(s) read, %" PRIu64 " dived in %" PRIu64 " batch(es)",
           g_st.rows, g_st.dived, g_st.batches);
    if (g_st.complete)  printf(", %" PRIu64 " already complete (nothing to dive; see --reopen)",
                               g_st.complete);
    if (g_st.malformed) printf(", %" PRIu64 " skipped as malformed", g_st.malformed);
    printf("\n");
    if (g_reopen)
        printf("[sum] reopen %s%s: %" PRIu64 " complete board(s) reopened (%.1f cells lifted on "
               "average), %d round(s) x %u copies; %" PRIu64 " improved (%+" PRId64 " edges in "
               "all), the rest written %s%s\n", g_reopen,
               g_nspec > 1 ? " (and the other specs in turn)" : "", g_st.reopened,
               g_st.reopened ? (double)g_st.lifted / (double)g_st.reopened : 0.0, g_rounds,
               g_copies, g_st.better, g_st.gain,
               g_plateau ? "at their old score" : "as they were",
               g_st.solved ? "; SOLVED boards written as they are" : "");
    if (g_plateau && g_reopen)
        printf("[sum] plateau: %" PRIu64 " board(s) written at their old score as a different "
               "board\n", g_st.moved);
    if (g_st.solved)
        printf("[sum] %" PRIu64 " board(s) score 480/480: nothing is broken\n", g_st.solved);
    if (g_holes_arg)
        printf("[sum] holes %s: %" PRIu64 " board(s), %.1f cells lifted on average\n", g_holes_arg,
               g_pr.holed, g_pr.holed ? (double)g_pr.holes_lifted / (double)g_pr.holed : 0.0);
    if (g_pin)
        printf("[sum] clues (--pin_clue %d%s): %" PRIu64 " piece(s) placed, %" PRIu64 " board(s) "
               "dropped (a clue cell or piece taken otherwise)\n", g_pin,
               (g_clue_mask & CLUE_CORNERS) ? " --clue_corners" : "", g_pr.pinned, g_pr.clue_drop);
    if (g_backtrack) {
        printf("[sum] backtrack: %" PRIu64 " board(s), highest full row reached %d",
               g_pr.bt, g_pr.row_max);
        if (g_stop_row >= 0)
            printf("; %" PRIu64 " reached row %d, %" PRIu64 " did not (dived from their deepest "
                   "row prefix)", g_pr.reached, g_stop_row, g_pr.shortb);
        printf("; node cap hit on %" PRIu64 "\n", g_pr.capped);
        if (g_stop_row >= 0 && g_pr.reached)
            printf("[sum] extension (--extend_nodes %u), over the %" PRIu64 " board(s) at row %d: "
                   "cells mean %.1f, max %d of rows %d..14; whole columns mean %.2f, max %d\n",
                   g_ext_nodes, g_pr.reached, g_stop_row,
                   (double)g_pr.ext_cells / (double)g_pr.reached, g_pr.ext_max, g_stop_row + 1,
                   (double)g_pr.ext_cols / (double)g_pr.reached, g_pr.ext_cols_max);
    }
    if (g_rot_path)
        printf("[sum] frame: %" PRIu64 " board(s) under an id naming no rotations row, %" PRIu64
               " that do not fit their row: both dived with free edges\n", g_st.unframed, g_st.misfit);
    if (g_dives) dv_print_summary(wall);
    if (g_dives) printf("[sum] %" PRIu64 " board(s) >= %d", dv_written(), g_emit);
    else         printf("[sum] %" PRIu64 " prepared board(s), not dived,", g_st.raw);
    printf(" written to %s in %.1fs%s\n", out_path, wall, g_stop ? " (interrupted)" :
           (deadline > 0.0 && omp_get_wtime() >= deadline) ? " (wall time reached)" : "");
    printf("  outputs_txt = %s\n", man);
    return 0;
}
