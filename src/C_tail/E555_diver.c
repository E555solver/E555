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
 * EXTENDING. This file is only the front-end: input, batching, frame choice
 * and output. New dive policies, moves and stages belong in E555_dive.c's
 * layers; new ways of choosing what to dive (holes masks, reopened rows,
 * alternative seeders) belong here, as preparation of the board handed to
 * dv_add().
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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

/* -- Options ---------------------------------------------------------------- */
static const char *g_rot_path     = NULL;    /* --rotations FILE */
static uint32_t    g_dives        = DR_M_DEFAULT;
static int         g_polish       = -1;      /* --end_polish R; -1 = off */
static int         g_emit         = DR_S_DEFAULT;
static int         g_seeds        = 0;       /* --corner_seeds N; 0 = off */
static uint64_t    g_rng          = DR_SEED_DEFAULT;
static double      g_wall         = 0.0;     /* --wall_time S; 0 = none */
static uint64_t    g_max_written  = 0;       /* --max_emitted N; 0 = none */

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* -- Input ------------------------------------------------------------------ */
typedef struct {
    uint16_t pid[NUM_PIECES];      /* per cell; DV_EMPTY = open */
    uint8_t  rot[NUM_PIECES];
    uint64_t line;
} DrBoard;

static struct {
    char     id[256];
    DrBoard *b;
    size_t   n, cap;
} g_batch;

static struct {
    uint64_t rows, batches, complete, malformed, misfit, unframed, dived;
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
static FILE *g_out;

/* Dive boards idx[0..n) of the batch under one frame, and write the result. */
static void dr_run(const size_t *idx, size_t n, bool by_side, bool seed) {
    if (!n) return;
    dv_frame(by_side);
    dv_seeding(seed);
    for (size_t i = 0; i < n; i++) {
        const DrBoard *b = &g_batch.b[idx[i]];
        if (seed) dr_corner_config(b);
        dv_add(b->pid, b->rot);
    }
    if (g_verbose) {
        printf("[batch] %s: %zu board(s), %s edges%s\n", g_batch.id, n,
               by_side ? "dealt" : "free", seed ? ", corner seeding" : "");
        fflush(stdout);
    }
    dv_run(g_batch.id);
    dv_flush(g_out);
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
    if (g_print_cmd) print_cmd(argv[0], seed_path, in_path, out_path);
    printf("[cfg] dives=%u polish=%d emit_score=%d frame=%s corner_seeds=%d%s threads=%d rng_seed=%" PRIu64 "\n",
           g_dives, g_polish, g_emit, g_rot_path ? g_rot_path : "(free edges)", g_seeds,
           (g_clue_mask & CLUE_CORNERS) ? " (clue 2x3)" : "", g_nthreads, g_rng);
    fflush(stdout);

    const double t0 = omp_get_wtime();
    load_seed_and_catalog(seed_path);
    FILE *in = fopen(in_path, "r");
    if (!in) fatal("cannot open %s: %s", in_path, strerror(errno));
    g_out = fopen(out_path, "w");
    if (!g_out) fatal("cannot open %s: %s", out_path, strerror(errno));

    signal(SIGINT, handle_stop); signal(SIGTERM, handle_stop);
    const double deadline = g_wall > 0.0 ? t0 + g_wall : 0.0;
    DvParams dp = {
        .dives = g_dives, .emit_score = g_emit, .polish = g_polish,
        .corner_seeds = g_seeds, .seed_corners = g_seeds > 0,
        .master_seed = g_rng, .max_written = g_max_written,
        .deadline = deadline, .threads = g_nthreads, .stop = &g_stop,
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
        bool open = false;
        for (int x = 0; x < NUM_PIECES && !open; x++) open = b.pid[x] == DV_EMPTY;
        if (!open) { g_st.complete++; continue; }
        dr_take(n > 2 * NUM_PIECES ? f[0] : "board", &b);
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
    if (g_st.complete)  printf(", %" PRIu64 " already complete (nothing to dive)", g_st.complete);
    if (g_st.malformed) printf(", %" PRIu64 " skipped as malformed", g_st.malformed);
    printf("\n");
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
