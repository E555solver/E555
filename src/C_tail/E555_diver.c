/*
 * E555_diver.c -- finish precomputed boards with the beamer's end dives and
 * polish, without the beam or the chain database.
 *
 *   E555_diver seed.txt boards.csv output.csv [options]
 *
 * Reads any board CSV (the last 512 fields of a row are pos[256] and rot[256];
 * a leading field is the config id; '#' and '%' lines are comments), keeps
 * every placed cell fixed, and fills the open cells exactly as
 * `E555_beamer --end_dive` does: the same engine (E555_dive.c), the same
 * stages, the same tuning. Boards whose best finish scores >= --emit_score are
 * written as canonical "config_id, score, pos[256], rot[256]" rows, best first
 * within each batch, and output.csv.outputs.txt lists the output.
 *
 * BATCHES. Consecutive rows with the same config id form a batch -- what the
 * beamer dives together for one configuration -- because stage 2 is chosen
 * relative to the batch (see E555_dive.h). A beamer output file therefore
 * replays configuration by configuration. Output is written after every batch.
 *
 * FRAME. Without --rotations every edge piece may take any open border cell.
 * With --rotations FILE, a beamer config id "r<N>b..." names the rotations row
 * its border came from, and the edge pieces are held to the sides that row
 * deals them, as in the beamer. A board that cannot be completed that way (its
 * edges do not sit where row N deals them) is dived with free edges instead, in
 * a run of its own under the same id -- which is how a beamer --free_edges
 * file replays, its corner seeds drawn from the row's pooled top/right catalog
 * as in the beamer. An id that names no row -- finalizer, random-border or
 * hand-made boards -- is dived with free edges.
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
 * broken ones (E555_dive.h, "COPIES, INCUMBENTS, ORDERS").
 *
 * EXTENDING. This file is only the front-end: input, batching, frame choice,
 * what to reopen, and output. New dive policies, moves and stages belong in
 * E555_dive.c's layers; new ways of choosing what to dive (holes masks,
 * reopened rows, alternative seeders) belong here, as preparation of the board
 * handed to dv_queue().
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
    uint64_t reopened, lifted, solved, better, moved;
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
   finalizer ids (p3r0l5) and random-border ids (rndb0l1) name none. */
static long dr_id_row(const char *id) {
    if (id[0] != 'r' || id[1] < '0' || id[1] > '9') return -1;
    char *e;
    const long v = strtol(id + 1, &e, 10);
    return (*e == 'b' && v >= 0) ? v : -1;
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

/* Write a complete board as a canonical row, outside the engine: a reopened
   board with nothing left to reopen (it scores 480). */
static void dr_write_complete(FILE *fp, const char *id, const uint16_t *pid, const uint8_t *rot) {
    bool brk[NUM_PIECES];
    const int score = 480 - dr_breaks(pid, rot, brk);
    uint32_t pos[NUM_PIECES], rr[NUM_PIECES];
    for (int x = 0; x < NUM_PIECES; x++) { pos[pid[x]] = (uint32_t)x; rr[pid[x]] = rot[x]; }
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
           (g_max_written && dv_written() >= g_max_written);
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
        "  --clue_corners      corner blocks use the 2x3 clue template\n"
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
        "  --print_cmd         echo the full command line\n"
        "  --verbose           one line per batch\n\n",
        DR_M_DEFAULT, DR_S_DEFAULT, DR_SEED_DEFAULT);
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
            if (v < 10 || v > 1000000000L) fatal("--end_dive must be in 10..1e9");
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
        else if (!strcmp(argv[i], "--print_cmd"))  g_print_cmd = true;
        else if (!strcmp(argv[i], "--verbose"))    g_verbose = true;
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); usage(argv[0]); return 1; }
    }
    if (g_nthreads <= 0) g_nthreads = omp_get_max_threads();
    omp_set_num_threads(g_nthreads);
    if (g_seeds && !g_rot_path) fatal("--corner_seeds needs --rotations: the corner blocks "
                                      "are built from the sides a rotations row deals");
    if ((g_clue_mask & CLUE_CORNERS) && !g_seeds)
        printf("[warn] --clue_corners only shapes --corner_seeds; ignored\n");
    if (!g_reopen && (g_prior > 0.0f || g_nogo > 0.0f || g_rounds > 1 || g_plateau))
        printf("[warn] --prior, --nogo, --rounds and --plateau act on reopened boards only "
               "(--reopen)\n");
    if (g_print_cmd) print_cmd(argv[0], seed_path, in_path, out_path);
    printf("[cfg] dives=%u polish=%d emit_score=%d frame=%s corner_seeds=%d%s threads=%d rng_seed=%" PRIu64 "\n",
           g_dives, g_polish, g_emit, g_rot_path ? g_rot_path : "(free edges)", g_seeds,
           (g_clue_mask & CLUE_CORNERS) ? " (clue 2x3)" : "", g_nthreads, g_rng);
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
    uint64_t lno = 0;
    DrBoard b;
    while (!dr_done(deadline) && getline(&line, &lcap, in) > 0) {
        lno++;
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '%' || *s == '\n' || *s == '\r' || !*s) continue;
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
    if (g_rot_path)
        printf("[sum] frame: %" PRIu64 " board(s) under an id naming no rotations row, %" PRIu64
               " that do not fit their row: both dived with free edges\n", g_st.unframed, g_st.misfit);
    dv_print_summary(wall);
    printf("[sum] %" PRIu64 " board(s) >= %d written to %s in %.1fs%s\n", dv_written(), g_emit,
           out_path, wall, g_stop ? " (interrupted)" :
           (deadline > 0.0 && omp_get_wtime() >= deadline) ? " (wall time reached)" : "");
    printf("  outputs_txt = %s\n", man);
    return 0;
}
