/*
 * E555_soft.c -- a relaxed board that settles into a real one.
 *
 *   E555_soft seed.txt boards.csv output.csv [options]
 *
 * The free cells of a board hold mixes over (piece, spin) instead of pieces.
 * The expected number of broken junctions is minimised over the mixes while an
 * entropy term, lowered in stages, lets every mix settle on a single choice (mean
 * field annealing); the result is rounded to a real board and hill-climbed. Many
 * starts per board give distinct complete boards: new places to start for
 * E555_diver --reopen and E555_ender.py. An experiment (README.md beside this
 * file has the measurements); needs NLopt; build it here with make.
 *
 * INPUT. Any board CSV: the last 512 fields of a row are pos[256] and rot[256]
 * (999 = unplaced), a leading field is the config id, '#' and '%' lines are
 * comments. --start_row R / --num_rows N read data rows R..R+N-1 only.
 *
 * FREE REGION. By default the open cells of each board, for its unplaced
 * pieces; a complete board has nothing free and is skipped. --core N keeps only
 * the N placed pieces nearest the board centre -- square rings out from the
 * central 2x2, a ring taken from the middle of its sides first -- and frees all
 * the others; --core 0 builds a whole new board.
 *
 * MODEL. A variable y[c,p,s] >= 0 for each free cell c, free piece p of the same
 * kind and frame-legal spin s: a corner or edge piece has one legal spin in a
 * cell of its kind, an inner piece four, and an edge piece may take any free
 * border cell. Every cell is filled once and every piece used once,
 *     sum_{p,s} y[c,p,s] = 1,   sum_{c,s} y[c,p,s] = 1,
 * a bipartite incidence matrix: every vertex of this region is a real board.
 * pi[c,d,k] = sum_{p,s} y[c,p,s] [side d of p at spin s shows k] is the chance
 * that c shows colour k on side d (a placed neighbour's is 0 or 1), and
 *     B(y) = sum over junctions touching a free cell of (1 - sum_k pi pi')
 * is the expected number of breaks, exact on a real board.
 *
 * METHOD. Each cell's mix is a softmax, y[c,.] = softmax(theta[c,.]), so the cell
 * sums hold exactly and theta is unconstrained. At temperature T,
 *     F_T = B + T sum y log y + sum_p lambda_p h_p + rho/2 sum_p h_p^2
 * with h_p the piece sums minus 1, is minimised over theta by NLopt's L-BFGS
 * (NLOPT_LD_LBFGS) in rounds that update lambda and rho (an augmented
 * Lagrangian; the residuals' sparse gradients live in this objective, where
 * NLopt's AUGLAG would evaluate them as dense vectors). T falls geometrically
 * over SOFT_STAGES stages from SOFT_T0, where the mixes are nearly uniform, to
 * SOFT_T1, where the cells have committed (they do between T = 0.3 and 0.2 on
 * seed_Edge5), each stage starting from the last one's result. Start 0 is the
 * uniform mix, start j > 0 a Gumbel-Sinkhorn mix (random weights scaled until
 * every cell and every piece sums to 1).
 *
 * Measured against the variant first planned -- the mixes as variables in
 * [0,1] with both sums in the Lagrangian, and a concave penalty mu sum y (1-y)
 * raised from 0 -- this one settles (the cell sums made the bounded problem
 * so stiff that the mixes never did), and it starts from a nearly convex
 * problem: B alone is not convex, so a ladder starting at mu = 0 froze in the
 * first local minimum it met (README.md).
 *
 * FINISH. Per piece kind, a linear assignment (Hungarian) takes the board of
 * most mass, each (cell, piece) at its heaviest spin; the dive engine's polish
 * climb (dv_climb: turn one piece or swap two while an edge is gained) then runs
 * over the free cells. Per input board the --keep best boards are written, best
 * first, skipping any that shares more than SOFT_SHARED of its free cells with
 * a better one, as canonical "id_s<start>, score, pos[256], rot[256]" rows.
 *
 * Every start runs serially on a random stream keyed by the input board, the
 * start and --rng_seed, so the output does not depend on --threads.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <nlopt.h>
#include <omp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/B_beam/E555_database.h"
#include "../../src/C_tail/E555_dive.h"

/* ============================================================================
 * Tuning (measured; see README.md)
 * ========================================================================== */
#define SOFT_T0        0.45    /* temperature of the first stage: nearly uniform mixes... */
#define SOFT_T1        0.12    /* ...and of the last: the cells have committed */
#define SOFT_STAGES    30      /* stages, geometric in T */
#define SOFT_ROUNDS    3       /* augmented-Lagrangian rounds per stage, at most */
#define SOFT_EVALS     100     /* L-BFGS evaluations per round, at most */
#define SOFT_LBFGS_M   4       /* L-BFGS vector storage */
#define SOFT_FTOL      1e-7    /* L-BFGS relative tolerance on the objective */
#define SOFT_RHO0      10.0    /* weight on the squared residuals at the first round... */
#define SOFT_RHO_MAX   100.0   /* ...times 10 after a round that does not cut them by 4, up to this */
#define SOFT_FEAS      1e-4    /* a stage ends once every residual is below this */
#define SOFT_SETTLED   1e-3    /* the stages end once mean(1 - max y of a cell) is below this */
#define SOFT_NOISE     1.0     /* start weights exp(SOFT_NOISE * Gumbel) */
#define SOFT_SINKHORN  50      /* scaling sweeps of a start */
#define SOFT_SHARED    0.8     /* not written: more than this share of the free cells as a better board */

#define SF_STARTS_DEFAULT 16   /* --starts */
#define SF_KEEP_DEFAULT   4    /* --keep */
#define SF_SEED_DEFAULT   1    /* --rng_seed */
#define SF_MAX_FIELDS     1024
#define SF_CHUNK_MAX      64   /* boards relaxed at once, at most */
#define SF_COLS           24   /* colours, padded */
#define SF_FRAME          (-1) /* across[k][d]: a side facing the frame... */
#define SF_FIXED          (-2) /* ...or SF_FIXED - colour: a placed neighbour showing that colour */

/* -- Options ---------------------------------------------------------------- */
static int         g_core       = -1;     /* --core N; -1 = every placed piece stays */
static int         g_starts     = SF_STARTS_DEFAULT;
static int         g_keep       = SF_KEEP_DEFAULT;
static uint64_t    g_rng        = SF_SEED_DEFAULT;
static double      g_wall       = 0.0;    /* --wall_time S; 0 = none */
static uint64_t    g_rec_start  = 0;      /* --start_row R */
static uint64_t    g_rec_count  = 0;      /* --num_rows N; 0 = the rest */

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* ============================================================================
 * Pieces, cells, spins
 * ========================================================================== */
static uint8_t g_legal[NUM_PIECES][9];          /* distinct frame-legal spins per cell class, one bit each */
static uint8_t g_pkind[NUM_PIECES];             /* grey sides: 0 inner, 1 edge, 2 corner */
static double  g_onehot[NUM_COLORS_TOTAL][SF_COLS];
static const double g_none[SF_COLS];            /* what a side facing the frame sees */

/* Side d (0 top, 1 right, 2 bottom, 3 left) of piece p at CCW spin s. */
static inline int sf_side(int p, int s, int d) {
    const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
    return e[(d + s) & 3];
}
/* The frame class of a cell, as E555_dive.c numbers them: rowcls * 3 + colcls. */
static inline int sf_class(int x) {
    const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    return ((r == 0) ? 1 : (r == PUZZLE_SIDE - 1) ? 2 : 0) * 3
         + ((c == 0) ? 1 : (c == PUZZLE_SIDE - 1) ? 2 : 0);
}
/* Outward sides of a cell: the kind of piece it takes. */
static inline int sf_ckind(int x) {
    const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    return (r == 0 || r == PUZZLE_SIDE - 1) + (c == 0 || c == PUZZLE_SIDE - 1);
}
/* Neighbour of x toward d, or -1. */
static inline int sf_nb(int x, int d) {
    const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    switch (d) {
        case 0:  return r < PUZZLE_SIDE - 1 ? x + PUZZLE_SIDE : -1;
        case 1:  return c < PUZZLE_SIDE - 1 ? x + 1 : -1;
        case 2:  return r > 0 ? x - PUZZLE_SIDE : -1;
        default: return c > 0 ? x - 1 : -1;
    }
}

static void sf_tables(void) {
    for (int p = 0; p < NUM_PIECES; p++) {
        g_pkind[p] = (uint8_t)((g_seed_top[p] == 0) + (g_seed_right[p] == 0) +
                               (g_seed_bottom[p] == 0) + (g_seed_left[p] == 0));
        for (int k = 0; k < 9; k++) {
            const int rk = k / 3, ck = k % 3;
            const bool out[4] = { rk == 2, ck == 2, rk == 1, ck == 1 };
            uint8_t m = 0;
            for (int s = 0; s < 4; s++) {
                bool ok = true;
                for (int d = 0; d < 4; d++) ok = ok && ((sf_side(p, s, d) == 0) == out[d]);
                for (int t = 0; t < s && ok; t++)         /* a symmetric piece: one of equal spins */
                    if (m >> t & 1u) {
                        bool same = true;
                        for (int d = 0; d < 4; d++) same = same && sf_side(p, s, d) == sf_side(p, t, d);
                        if (same) ok = false;
                    }
                if (ok) m |= (uint8_t)(1u << s);
            }
            g_legal[p][k] = m;
        }
    }
    for (int c = 0; c < NUM_COLORS_TOTAL; c++) g_onehot[c][c] = 1.0;
}

/* Does piece p at spin s show grey exactly toward the frame at cell x? */
static bool sf_frame_ok(int p, int s, int x) {
    for (int d = 0; d < 4; d++) if ((sf_side(p, s, d) == 0) != (sf_nb(x, d) < 0)) return false;
    return true;
}

/* Matched edges of a complete board, out of 480. */
static int sf_score(const uint16_t *pid, const uint8_t *rot) {
    int broken = 0;
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        if (c < PUZZLE_SIDE - 1 && sf_side(pid[x], rot[x], 1) != sf_side(pid[x + 1], rot[x + 1], 3))
            broken++;
        if (r < PUZZLE_SIDE - 1 &&
            sf_side(pid[x], rot[x], 0) != sf_side(pid[x + PUZZLE_SIDE], rot[x + PUZZLE_SIDE], 2))
            broken++;
    }
    return DV_EDGES - broken;
}

/* --core: square rings out from the central 2x2, then the distance to the
   centre (a ring is taken from the middle of its sides), then the cell. */
static int sf_core_key(int x) {
    const int dr = abs(2 * (x / PUZZLE_SIDE) - (PUZZLE_SIDE - 1));
    const int dc = abs(2 * (x % PUZZLE_SIDE) - (PUZZLE_SIDE - 1));
    const int ring = ((dr > dc ? dr : dc) - 1) / 2;
    return (ring * 1024 + dr * dr + dc * dc) * NUM_PIECES + x;
}
static int sf_cmp_core(const void *a, const void *b) {
    const int x = sf_core_key(*(const int *)a), y = sf_core_key(*(const int *)b);
    return (x > y) - (x < y);
}

/* ============================================================================
 * One board's problem: the free region and its variables
 * ========================================================================== */
typedef struct {
    char      id[256];
    uint64_t  line;
    uint16_t  pid[NUM_PIECES];      /* the board as read, per cell; DV_EMPTY = open */
    uint8_t   rot[NUM_PIECES];
    uint64_t  fp;                   /* keys the starts' random streams */
    int       m;                    /* free cells = free pieces */
    int       kept, freed;          /* placed pieces kept; placed pieces --core freed */
    uint8_t   cell[NUM_PIECES];     /* free cell k -> board cell */
    int16_t   slot[NUM_PIECES];     /* board cell -> free cell k, -1 = kept */
    uint16_t  piece[NUM_PIECES];    /* free piece j -> piece id */
    int16_t   across[NUM_PIECES][4];/* free cell k, side d: a free cell, SF_FRAME, or SF_FIXED - colour */
    int       nk[3];                /* free cells (= free pieces) per kind */
    int16_t   kcell[3][NUM_PIECES]; /* the free cells of a kind... */
    int16_t   kpiece[3][NUM_PIECES];/* ...and its free pieces */
    int16_t   ploc[NUM_PIECES];     /* free piece j -> its index in kpiece */
    int       n;                    /* variables: arc a = (free cell, free piece, spin), by cell */
    int      *cbeg;                 /* [m+1]: the arcs of free cell k */
    uint16_t *aj;                   /* [n]: free piece */
    uint8_t  *as;                   /* [n]: spin */
    uint8_t (*ac)[4];               /* [n]: colour on side d */
    int      *pbeg, *parc;          /* [m+1], [n]: the arcs of free piece j */
} SfProb;

static void sf_free(SfProb *P) {
    free(P->cbeg); free(P->aj); free(P->as); free(P->ac); free(P->pbeg); free(P->parc);
    P->cbeg = P->pbeg = P->parc = NULL; P->aj = NULL; P->as = NULL; P->ac = NULL;
}

/* Free region, variables and fingerprint of a parsed board. 0 when it has
   nothing free, -1 (with the reason) when it cannot be relaxed. */
static int sf_build(SfProb *P, const char **why) {
    uint64_t h = 14695981039346656037ULL;
    for (int x = 0; x < NUM_PIECES; x++) {
        h ^= P->pid[x]; h *= 1099511628211ULL;
        h ^= P->rot[x]; h *= 1099511628211ULL;
    }
    P->fp = splitmix64(h ^ splitmix64(g_rng));

    bool keep[NUM_PIECES] = { false }, used[NUM_PIECES] = { false };
    int placed[NUM_PIECES], np = 0;
    for (int x = 0; x < NUM_PIECES; x++) if (P->pid[x] != DV_EMPTY) placed[np++] = x;
    int nkeep = np;
    if (g_core >= 0 && g_core < np) {
        qsort(placed, (size_t)np, sizeof *placed, sf_cmp_core);
        nkeep = g_core;
    }
    for (int i = 0; i < nkeep; i++) {
        const int x = placed[i], p = P->pid[x];
        if (!sf_frame_ok(p, P->rot[x], x)) { *why = "a placed piece breaks the frame rule"; return -1; }
        keep[x] = used[p] = true;
    }
    P->kept = nkeep;
    P->freed = np - nkeep;
    P->m = 0;
    for (int x = 0; x < NUM_PIECES; x++) {
        P->slot[x] = -1;
        if (!keep[x]) { P->slot[x] = (int16_t)P->m; P->cell[P->m++] = (uint8_t)x; }
    }
    if (!P->m) return 0;
    int nfree = 0;
    for (int t = 0; t < 3; t++) P->nk[t] = 0;
    for (int k = 0; k < P->m; k++) { const int t = sf_ckind(P->cell[k]); P->kcell[t][P->nk[t]++] = (int16_t)k; }
    int pk[3] = { 0, 0, 0 };
    for (int p = 0; p < NUM_PIECES; p++) {
        if (used[p]) continue;
        const int t = g_pkind[p];
        if (t > 2 || pk[t] >= P->nk[t]) { *why = "the free pieces do not fit the free cells' kinds"; return -1; }
        P->piece[nfree] = (uint16_t)p;
        P->ploc[nfree] = (int16_t)pk[t];
        P->kpiece[t][pk[t]++] = (int16_t)nfree;
        nfree++;
    }
    if (nfree != P->m) { *why = "the free pieces do not fit the free cells' kinds"; return -1; }

    for (int k = 0; k < P->m; k++)
        for (int d = 0; d < 4; d++) {
            const int y = sf_nb(P->cell[k], d);
            if (y < 0) P->across[k][d] = SF_FRAME;
            else if (P->slot[y] >= 0) P->across[k][d] = P->slot[y];
            else P->across[k][d] = (int16_t)(SF_FIXED - sf_side(P->pid[y], P->rot[y], (d + 2) & 3));
        }

    P->n = 0;
    for (int k = 0; k < P->m; k++) {
        const int t = sf_ckind(P->cell[k]), cls = sf_class(P->cell[k]);
        for (int i = 0; i < P->nk[t]; i++)
            P->n += __builtin_popcount(g_legal[P->piece[P->kpiece[t][i]]][cls]);
    }
    P->cbeg = xmalloc((size_t)(P->m + 1) * sizeof *P->cbeg);
    P->pbeg = xmalloc((size_t)(P->m + 1) * sizeof *P->pbeg);
    P->aj   = xmalloc((size_t)P->n * sizeof *P->aj);
    P->as   = xmalloc((size_t)P->n * sizeof *P->as);
    P->ac   = xmalloc((size_t)P->n * sizeof *P->ac);
    P->parc = xmalloc((size_t)P->n * sizeof *P->parc);
    int a = 0;
    for (int k = 0; k < P->m; k++) {
        const int t = sf_ckind(P->cell[k]), cls = sf_class(P->cell[k]);
        P->cbeg[k] = a;
        for (int i = 0; i < P->nk[t]; i++) {
            const int j = P->kpiece[t][i], p = P->piece[j];
            for (int s = 0; s < 4; s++) {
                if (!(g_legal[p][cls] >> s & 1u)) continue;
                P->aj[a] = (uint16_t)j;
                P->as[a] = (uint8_t)s;
                for (int d = 0; d < 4; d++) P->ac[a][d] = (uint8_t)sf_side(p, s, d);
                a++;
            }
        }
        if (a == P->cbeg[k]) { *why = "a free cell has no legal piece"; sf_free(P); return -1; }
    }
    P->cbeg[P->m] = a;
    for (int j = 0; j <= P->m; j++) P->pbeg[j] = 0;
    for (a = 0; a < P->n; a++) P->pbeg[P->aj[a] + 1]++;
    for (int j = 0; j < P->m; j++) P->pbeg[j + 1] += P->pbeg[j];
    int *fill = xmalloc((size_t)P->m * sizeof *fill);
    memcpy(fill, P->pbeg, (size_t)P->m * sizeof *fill);
    for (a = 0; a < P->n; a++) P->parc[fill[P->aj[a]]++] = a;
    free(fill);
    return 1;
}

/* ============================================================================
 * The relaxation: objective, augmented Lagrangian, starts
 * ========================================================================== */
typedef struct {
    const SfProb  *P;
    double         T, rho;
    double        *y;               /* [n]: the mixes, softmax of theta per cell */
    double        *pi;              /* [m][4][SF_COLS]: colour marginals */
    const double **f;               /* [m][4]: what side d of k sees (a pi row, a one-hot, zeros) */
    double        *w;               /* [m][4]: junction weight, 1 placed, 1/2 free (seen twice), 0 frame */
    double        *hp, *lp;         /* piece sums - 1, and their multipliers */
    double        *lse;             /* [m]: log of each cell's softmax sum, so log y = theta - lse */
    long           evals;
} SfWork;

/* y = softmax of theta within each free cell; lse[k] = log sum exp theta. */
static void sf_softmax(const SfProb *P, const double *th, double *y, double *lse) {
    for (int k = 0; k < P->m; k++) {
        double mx = -INFINITY, s = 0.0;
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) if (th[a] > mx) mx = th[a];
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) { y[a] = exp(th[a] - mx); s += y[a]; }
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) y[a] /= s;
        lse[k] = mx + log(s);
    }
}

/* The colour marginals and the piece residuals at y. */
static void sf_marginals(SfWork *W, const double *y) {
    const SfProb *P = W->P;
    memset(W->pi, 0, (size_t)P->m * 4 * SF_COLS * sizeof *W->pi);
    for (int j = 0; j < P->m; j++) W->hp[j] = -1.0;
    for (int k = 0; k < P->m; k++) {
        double *q = W->pi + (size_t)k * 4 * SF_COLS;
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) {
            const double v = y[a];
            const uint8_t *c = P->ac[a];
            q[c[0]] += v; q[SF_COLS + c[1]] += v; q[2 * SF_COLS + c[2]] += v; q[3 * SF_COLS + c[3]] += v;
            W->hp[P->aj[a]] += v;
        }
    }
}

/* B at the current marginals: expected breaks over the junctions touching the
   free region. */
static double sf_expected(const SfWork *W) {
    double B = 0.0;
    for (int k = 0; k < W->P->m; k++)
        for (int d = 0; d < 4; d++) {
            const double wt = W->w[k * 4 + d];
            if (wt == 0.0) continue;
            const double *q = W->pi + ((size_t)k * 4 + (size_t)d) * SF_COLS, *f = W->f[k * 4 + d];
            double dot = 0.0;
            for (int c = 0; c < NUM_COLORS_TOTAL; c++) dot += q[c] * f[c];
            B += wt * (1.0 - dot);
        }
    return B;
}

/* NLopt's objective over theta: F_T, and its gradient through the softmax,
   g = y (G - sum_cell y G) with G = dF_T/dy. */
static double sf_lagrangian(unsigned n, const double *th, double *g, void *data) {
    (void)n;
    SfWork *W = data;
    const SfProb *P = W->P;
    double *y = W->y;
    W->evals++;
    sf_softmax(P, th, y, W->lse);
    sf_marginals(W, y);
    double L = sf_expected(W);
    for (int k = 0; k < P->m; k++)                      /* the entropy term, T sum y log y */
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) L += W->T * y[a] * (th[a] - W->lse[k]);
    for (int j = 0; j < P->m; j++) L += W->lp[j] * W->hp[j] + 0.5 * W->rho * W->hp[j] * W->hp[j];
    if (!g) return L;
    double *up = W->hp;                 /* lambda_p + rho h_p, in place: hp is rebuilt every call */
    for (int j = 0; j < P->m; j++) up[j] = W->lp[j] + W->rho * W->hp[j];
    for (int k = 0; k < P->m; k++) {
        const double *f0 = W->f[k * 4], *f1 = W->f[k * 4 + 1], *f2 = W->f[k * 4 + 2], *f3 = W->f[k * 4 + 3];
        double mean = 0.0;
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) {
            const uint8_t *c = P->ac[a];
            g[a] = up[P->aj[a]] + W->T * (th[a] - W->lse[k]) - (f0[c[0]] + f1[c[1]] + f2[c[2]] + f3[c[3]]);
            mean += y[a] * g[a];
        }
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) g[a] = y[a] * (g[a] - mean);
    }
    return L;
}

/* Mean over the free cells of 1 - the cell's largest variable: 0 on a board. */
static double sf_unsettled(const SfProb *P, const double *y) {
    double u = 0.0;
    for (int k = 0; k < P->m; k++) {
        double mx = 0.0;
        for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) if (y[a] > mx) mx = y[a];
        u += 1.0 - mx;
    }
    return u / P->m;
}

/* Start j: weights 1 (j = 0, unless nothing is placed: that mix is symmetric)
   or exp(SOFT_NOISE * Gumbel), scaled until every piece and cell sums to 1. */
static void sf_start(const SfProb *P, int j, double *y) {
    RNG r = rng_for(P->fp, 0x50F7u, (uint32_t)j, 0u);
    const bool noise = j > 0 || P->kept == 0;
    for (int a = 0; a < P->n; a++) y[a] = noise ? exp(SOFT_NOISE * gumbel_noise(&r)) : 1.0;
    for (int it = 0; it < SOFT_SINKHORN; it++) {
        for (int q = 0; q < P->m; q++) {
            double s = 0.0;
            for (int i = P->pbeg[q]; i < P->pbeg[q + 1]; i++) s += y[P->parc[i]];
            for (int i = P->pbeg[q]; i < P->pbeg[q + 1]; i++) y[P->parc[i]] /= s;
        }
        for (int k = 0; k < P->m; k++) {
            double s = 0.0;
            for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) s += y[a];
            for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) y[a] /= s;
        }
    }
}

/* What one start produced. */
typedef struct {
    int      score, rounded, stages, fails;
    double   breaks, unsettled, resid;
    long     evals;
    uint16_t pid[NUM_PIECES];       /* per free cell k */
    uint8_t  rot[NUM_PIECES];
} SfResult;

/* The annealing from theta: each stage an augmented Lagrangian solved by
   L-BFGS in rounds. Leaves the final mixes in W->y. */
static void sf_relax(SfWork *W, double *th, SfResult *R) {
    const SfProb *P = W->P;
    nlopt_opt o = nlopt_create(NLOPT_LD_LBFGS, (unsigned)P->n);
    if (!o) fatal("NLopt could not create an L-BFGS problem of %d variables", P->n);
    nlopt_set_min_objective(o, sf_lagrangian, W);
    nlopt_set_vector_storage(o, SOFT_LBFGS_M);
    nlopt_set_maxeval(o, SOFT_EVALS);
    nlopt_set_ftol_rel(o, SOFT_FTOL);
    W->rho = SOFT_RHO0;
    double worst = 0.0;
    for (int s = 0; s < SOFT_STAGES; s++) {
        W->T = SOFT_T0 * pow(SOFT_T1 / SOFT_T0, (double)s / (SOFT_STAGES - 1));
        double prev = INFINITY;
        for (int r = 0; r < SOFT_ROUNDS; r++) {
            double fx;
            const nlopt_result res = nlopt_optimize(o, th, &fx);
            if (res < 0 && res != NLOPT_ROUNDOFF_LIMITED) R->fails++;
            sf_softmax(P, th, W->y, W->lse);
            sf_marginals(W, W->y);
            worst = 0.0;
            for (int j = 0; j < P->m; j++) {
                worst = fmax(worst, fabs(W->hp[j]));
                W->lp[j] += W->rho * W->hp[j];
            }
            if (worst < SOFT_FEAS) break;
            if (worst > 0.25 * prev) W->rho = fmin(W->rho * 10.0, SOFT_RHO_MAX);
            prev = worst;
        }
        R->stages = s + 1;
        if (sf_unsettled(P, W->y) < SOFT_SETTLED) break;
    }
    nlopt_destroy(o);
    sf_softmax(P, th, W->y, W->lse);
    sf_marginals(W, W->y);
    R->breaks = sf_expected(W);
    R->resid = worst;
}

/* ============================================================================
 * Finish: rounding, the climb
 * ========================================================================== */

/* Minimum-cost perfect assignment of the q x q matrix a (row-major): row i takes
   column to[i]. Kuhn-Munkres with potentials, O(q^3); ties go to lower indices. */
static void sf_hungarian(int q, const double *a, int *to) {
    double u[NUM_PIECES + 1], v[NUM_PIECES + 1], minv[NUM_PIECES + 1];
    int p[NUM_PIECES + 1], way[NUM_PIECES + 1];
    bool used[NUM_PIECES + 1];
    for (int j = 0; j <= q; j++) { u[j] = v[j] = 0.0; p[j] = way[j] = 0; }
    for (int i = 1; i <= q; i++) {
        p[0] = i;
        int j0 = 0;
        for (int j = 0; j <= q; j++) { minv[j] = INFINITY; used[j] = false; }
        do {
            used[j0] = true;
            const int i0 = p[j0];
            double delta = INFINITY;
            int j1 = 0;
            for (int j = 1; j <= q; j++) {
                if (used[j]) continue;
                const double cur = a[(size_t)(i0 - 1) * (size_t)q + (size_t)(j - 1)] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j = 0; j <= q; j++) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
    }
    for (int j = 1; j <= q; j++) to[p[j] - 1] = j - 1;
}

/* The board of most mass: per kind, the assignment of free pieces to free
   cells maximising sum y at each pair's heaviest spin. Writes the piece and
   spin of every free cell. */
static void sf_round(const SfProb *P, const double *y, double *cost, uint8_t *spin,
                     uint16_t *pid, uint8_t *rot) {
    int to[NUM_PIECES];
    for (int t = 0; t < 3; t++) {
        const int q = P->nk[t];
        if (!q) continue;
        for (int i = 0; i < q * q; i++) { cost[i] = 1.0; spin[i] = 0xFF; }
        for (int r = 0; r < q; r++) {
            const int k = P->kcell[t][r];
            for (int a = P->cbeg[k]; a < P->cbeg[k + 1]; a++) {
                const int i = r * q + P->ploc[P->aj[a]];
                if (-y[a] < cost[i]) { cost[i] = -y[a]; spin[i] = P->as[a]; }
            }
        }
        sf_hungarian(q, cost, to);
        for (int r = 0; r < q; r++) {
            const int k = P->kcell[t][r], i = r * q + to[r];
            if (spin[i] == 0xFF) fatal("internal error: rounding chose a piece with no legal spin");
            pid[k] = P->piece[P->kpiece[t][to[r]]];
            rot[k] = spin[i];
        }
    }
}

/* The complete board: the kept cells as read, the free cells from fpid/frot. */
static void sf_board(const SfProb *P, const uint16_t *fpid, const uint8_t *frot,
                     uint16_t *pid, uint8_t *rot) {
    for (int x = 0; x < NUM_PIECES; x++) {
        const int k = P->slot[x];
        pid[x] = k < 0 ? P->pid[x] : fpid[k];
        rot[x] = k < 0 ? P->rot[x] : frot[k];
    }
}

/* Start j of board P, from the start mix to the climbed board. */
static void sf_one(const SfProb *P, int j, SfResult *R) {
    const int m = P->m;
    memset(R, 0, sizeof *R);
    SfWork W = { .P = P };
    double *th = xmalloc((size_t)P->n * sizeof *th);
    W.y  = xmalloc((size_t)P->n * sizeof *W.y);
    W.pi = xmalloc((size_t)m * 4 * SF_COLS * sizeof *W.pi);
    W.f  = xmalloc((size_t)m * 4 * sizeof *W.f);
    W.w  = xmalloc((size_t)m * 4 * sizeof *W.w);
    W.hp = xmalloc((size_t)m * sizeof *W.hp);
    W.lp = xmalloc((size_t)m * sizeof *W.lp);
    W.lse = xmalloc((size_t)m * sizeof *W.lse);
    for (int k = 0; k < m; k++) {
        W.lp[k] = 0.0;
        for (int d = 0; d < 4; d++) {
            const int t = P->across[k][d];
            if (t >= 0)            { W.f[k * 4 + d] = W.pi + ((size_t)t * 4 + (size_t)((d + 2) & 3)) * SF_COLS;
                                     W.w[k * 4 + d] = 0.5; }
            else if (t == SF_FRAME) { W.f[k * 4 + d] = g_none; W.w[k * 4 + d] = 0.0; }
            else                    { W.f[k * 4 + d] = g_onehot[SF_FIXED - t]; W.w[k * 4 + d] = 1.0; }
        }
    }
    sf_start(P, j, W.y);
    for (int a = 0; a < P->n; a++) th[a] = log(W.y[a]);
    sf_relax(&W, th, R);
    const double *y = W.y;
    R->unsettled = sf_unsettled(P, y);
    R->evals = W.evals;

    int qmax = 0;
    for (int t = 0; t < 3; t++) if (P->nk[t] > qmax) qmax = P->nk[t];
    double *cost = xmalloc((size_t)qmax * (size_t)qmax * sizeof *cost);
    uint8_t *spin = xmalloc((size_t)qmax * (size_t)qmax);
    uint16_t pid[NUM_PIECES];
    uint8_t rot[NUM_PIECES];
    sf_round(P, y, cost, spin, R->pid, R->rot);
    sf_board(P, R->pid, R->rot, pid, rot);
    R->rounded = sf_score(pid, rot);
    R->score = R->rounded + dv_climb(pid, rot, P->cell, m);
    for (int k = 0; k < m; k++) { R->pid[k] = pid[P->cell[k]]; R->rot[k] = rot[P->cell[k]]; }

    free(cost); free(spin); free(th);
    free(W.y); free(W.pi); free(W.f); free(W.w); free(W.hp); free(W.lp); free(W.lse);
}

/* ============================================================================
 * Input, selection, output
 * ========================================================================== */

/* Split a data line into trimmed comma-separated fields, in place (as the diver). */
static int sf_split(char *s, char **f, int max) {
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

/* One row into a board: false (with the reason) when it describes none. */
static bool sf_parse(char **f, int n, SfProb *b, const char **why) {
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

static void sf_write(FILE *fp, const SfProb *P, const SfResult *R, int j) {
    uint16_t pid[NUM_PIECES];
    uint8_t rot[NUM_PIECES];
    uint32_t pos[NUM_PIECES], rr[NUM_PIECES];
    sf_board(P, R->pid, R->rot, pid, rot);
    for (int x = 0; x < NUM_PIECES; x++) { pos[pid[x]] = (uint32_t)x; rr[pid[x]] = rot[x]; }
    fprintf(fp, "%s_s%d, %d", P->id, j, R->score);
    for (int p = 0; p < NUM_PIECES; p++) fprintf(fp, ", %u", pos[p]);
    for (int p = 0; p < NUM_PIECES; p++) fprintf(fp, ", %u", rr[p]);
    fprintf(fp, "\n");
}

static struct {
    uint64_t rows, relaxed, complete, malformed, unfit, starts, fails, written;
    uint64_t cells, vars, kept, freed, returned, freed_written;
    int      cells_min, cells_max, best_rounded, best, stages_max;
    double   breaks, unsettled, resid_max, rounded, climbed, evals, t_relax;
} g_st = { .cells_min = INT_MAX, .best_rounded = -1, .best = -1 };

/* Per board: its starts by score (then start), each written unless it shares
   more than SOFT_SHARED of the free cells with a better one already written. */
static void sf_finish(FILE *fp, const SfProb *P, const SfResult *R) {
    int *order = xmalloc((size_t)g_starts * sizeof *order), *kept = xmalloc((size_t)g_keep * sizeof *kept);
    int nk = 0;
    for (int j = 0; j < g_starts; j++) {
        int i = j;
        while (i > 0 && R[order[i - 1]].score < R[j].score) { order[i] = order[i - 1]; i--; }
        order[i] = j;
    }
    for (int i = 0; i < g_starts && nk < g_keep; i++) {
        const SfResult *a = &R[order[i]];
        bool far = true;
        for (int e = 0; e < nk && far; e++) {
            const SfResult *b = &R[kept[e]];
            int same = 0;
            for (int k = 0; k < P->m; k++) same += a->pid[k] == b->pid[k] && a->rot[k] == b->rot[k];
            far = same <= SOFT_SHARED * P->m;
        }
        if (far) kept[nk++] = order[i];
    }
    for (int e = 0; e < nk; e++) {
        const SfResult *a = &R[kept[e]];
        sf_write(fp, P, a, kept[e]);
        for (int k = 0; k < P->m && P->freed; k++) {
            const int x = P->cell[k];
            g_st.returned += P->pid[x] != DV_EMPTY && a->pid[k] == P->pid[x] && a->rot[k] == P->rot[x];
        }
        g_st.freed_written += (uint64_t)P->freed;
    }
    fflush(fp);
    free(order); free(kept);
    g_st.written += (uint64_t)nk;
    for (int j = 0; j < g_starts; j++) {
        const SfResult *a = &R[j];
        g_st.starts++;
        g_st.fails += (uint64_t)a->fails;
        g_st.breaks += a->breaks;
        g_st.unsettled += a->unsettled;
        g_st.resid_max = fmax(g_st.resid_max, a->resid);
        g_st.evals += (double)a->evals;
        g_st.rounded += a->rounded;
        g_st.climbed += a->score;
        if (a->rounded > g_st.best_rounded) g_st.best_rounded = a->rounded;
        if (a->score > g_st.best) g_st.best = a->score;
        if (a->stages > g_st.stages_max) g_st.stages_max = a->stages;
        if (g_verbose)
            printf("[soft] line %" PRIu64 " start %d: %d stage(s), %ld evaluations, expected breaks %.2f, "
                   "unsettled %.4f, residual %.1e, rounded %d, climbed %d%s\n", P->line, j, a->stages,
                   a->evals, a->breaks, a->unsettled, a->resid, a->rounded, a->score,
                   a->fails ? " (NLopt failed)" : "");
    }
}

/* Relax every start of the chunk's boards in parallel, then write them in order. */
static void sf_chunk(FILE *fp, SfProb *chunk, int nb) {
    if (!nb) return;
    SfResult *res = xmalloc((size_t)nb * (size_t)g_starts * sizeof *res);
    const double t = omp_get_wtime();
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < nb * g_starts; i++)
        sf_one(&chunk[i / g_starts], i % g_starts, &res[i]);
    g_st.t_relax += omp_get_wtime() - t;
    for (int b = 0; b < nb; b++) {
        sf_finish(fp, &chunk[b], &res[(size_t)b * (size_t)g_starts]);
        sf_free(&chunk[b]);
    }
    free(res);
}

/* -- Command line ----------------------------------------------------------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "\nUsage: %s seed.txt boards.csv output.csv [options]\n\n"
        "Relax the free cells of every board to mixes over (piece, spin), minimise the\n"
        "expected number of broken edges while a falling temperature settles the mixes\n"
        "into a real board (NLopt L-BFGS), round it and hill-climb it. Output: the best\n"
        "distinct boards of each input as canonical rows \"id_s<start>, score,\n"
        "pos[256], rot[256]\", best first, and output.csv.outputs.txt.\n\n", prog);
    fprintf(stderr,
        "Options:\n"
        "  --core N            keep the N placed pieces nearest the board centre (square\n"
        "                      rings outward) and free all the others; 0 = a whole new\n"
        "                      board (default: every placed piece stays)\n"
        "  --starts N          relaxations per board, each from its own random mix\n"
        "                      (default %d)\n"
        "  --keep K            distinct boards written per input board (default %d)\n"
        "  --start_row R       read input records R.. only (0-based data rows)\n"
        "  --num_rows N        ... and at most N of them (default 0 = the rest)\n"
        "  --threads N         worker threads (default: all)\n"
        "  --rng_seed S        keys every start (default %d)\n"
        "  --wall_time S       stop after S seconds; the boards in flight are still written\n"
        "  --print_cmd         echo the full command line\n"
        "  --verbose           one line per start\n\n",
        SF_STARTS_DEFAULT, SF_KEEP_DEFAULT, SF_SEED_DEFAULT);
}

static void print_cmd(const char *a0, const char *seed, const char *in, const char *out) {
    printf("[cmd] %s %s %s %s --starts %d --keep %d --threads %d --rng_seed %" PRIu64,
           a0, seed, in, out, g_starts, g_keep, g_nthreads, g_rng);
    if (g_core >= 0)     printf(" --core %d", g_core);
    if (g_wall > 0.0)    printf(" --wall_time %g", g_wall);
    if (g_rec_start)     printf(" --start_row %" PRIu64, g_rec_start);
    if (g_rec_count)     printf(" --num_rows %" PRIu64, g_rec_count);
    if (g_print_cmd)     printf(" --print_cmd");
    if (g_verbose)       printf(" --verbose");
    printf("\n");
}

static long sf_int_arg(const char *flag, const char *s, long lo, long hi) {
    char *e;
    errno = 0;
    const long v = strtol(s, &e, 10);
    if (errno || e == s || *e || v < lo || v > hi)
        fatal("%s expects an integer in %ld..%ld, got '%s'", flag, lo, hi, s);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }
    const char *seed_path = argv[1], *in_path = argv[2], *out_path = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc)
            g_core = (int)sf_int_arg("--core", argv[++i], 0, NUM_PIECES);
        else if (!strcmp(argv[i], "--starts") && i + 1 < argc)
            g_starts = (int)sf_int_arg("--starts", argv[++i], 1, 100000);
        else if (!strcmp(argv[i], "--keep") && i + 1 < argc)
            g_keep = (int)sf_int_arg("--keep", argv[++i], 1, 100000);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rng_seed") && i + 1 < argc) {
            unsigned long long s;
            if (!parse_u64_token(argv[++i], &s)) fatal("--rng_seed needs an integer");
            g_rng = (uint64_t)s;
        }
        else if (!strcmp(argv[i], "--wall_time") && i + 1 < argc) g_wall = atof(argv[++i]);
        else if (!strcmp(argv[i], "--start_row") && i + 1 < argc) {
            unsigned long long v;
            if (!parse_u64_token(argv[++i], &v))
                fatal("--start_row expects a non-negative integer (0-based record index), got '%s'", argv[i]);
            g_rec_start = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "--num_rows") && i + 1 < argc) {
            unsigned long long v;
            if (!parse_u64_token(argv[++i], &v))
                fatal("--num_rows expects a non-negative integer (0 = every remaining record), got '%s'", argv[i]);
            g_rec_count = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "--print_cmd")) g_print_cmd = true;
        else if (!strcmp(argv[i], "--verbose"))   g_verbose = true;
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); usage(argv[0]); return 1; }
    }
    if (g_nthreads <= 0) g_nthreads = omp_get_max_threads();
    omp_set_num_threads(g_nthreads);
    if (g_print_cmd) print_cmd(argv[0], seed_path, in_path, out_path);
    int va, vb, vc;
    nlopt_version(&va, &vb, &vc);
    printf("[cfg] core=");
    if (g_core < 0) printf("every placed piece");
    else if (g_core == 0) printf("0 (a whole new board)");
    else printf("%d nearest the centre", g_core);
    printf(" starts=%d keep=%d threads=%d rng_seed=%" PRIu64 "\n", g_starts, g_keep, g_nthreads, g_rng);
    printf("[cfg] NLopt %d.%d.%d L-BFGS: temperature %g..%g over %d stages, %d rounds of %d evaluations a "
           "stage at most\n", va, vb, vc, SOFT_T0, SOFT_T1, SOFT_STAGES, SOFT_ROUNDS, SOFT_EVALS);
    fflush(stdout);

    const double t0 = omp_get_wtime();
    load_seed_and_catalog(seed_path);
    sf_tables();
    DvParams dp = { .threads = g_nthreads };
    dv_init(&dp);
    dv_frame(false);
    FILE *in = fopen(in_path, "r");
    if (!in) fatal("cannot open %s: %s", in_path, strerror(errno));
    FILE *out = fopen(out_path, "w");
    if (!out) fatal("cannot open %s: %s", out_path, strerror(errno));
    signal(SIGINT, handle_stop); signal(SIGTERM, handle_stop);
    const double deadline = g_wall > 0.0 ? t0 + g_wall : 0.0;

    int cap = (4 * g_nthreads + g_starts - 1) / g_starts;
    if (cap < 1) cap = 1;
    if (cap > SF_CHUNK_MAX) cap = SF_CHUNK_MAX;
    SfProb *chunk = xmalloc((size_t)cap * sizeof *chunk);
    char *line = NULL, **f = xmalloc(SF_MAX_FIELDS * sizeof *f);
    size_t lcap = 0;
    uint64_t lno = 0, nrec = 0;
    int nb = 0;
    while (!g_stop && !(deadline > 0.0 && omp_get_wtime() >= deadline) && getline(&line, &lcap, in) > 0) {
        lno++;
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '%' || *s == '\n' || *s == '\r' || !*s) continue;
        const uint64_t rec = nrec++;
        if (rec < g_rec_start) continue;
        if (g_rec_count && rec >= g_rec_start + g_rec_count) break;
        const int n = sf_split(line, f, SF_MAX_FIELDS);
        SfProb *P = &chunk[nb];
        const char *why = "more fields than any board row";
        if (n > SF_MAX_FIELDS || !sf_parse(f, n, P, &why)) {
            printf("[soft] line %" PRIu64 ": not a board (%s); skipped\n", lno, why);
            g_st.malformed++;
            continue;
        }
        g_st.rows++;
        snprintf(P->id, sizeof P->id, "%s", n > 2 * NUM_PIECES ? f[0] : "board");
        P->line = lno;
        const int r = sf_build(P, &why);
        if (r < 0) {
            printf("[soft] line %" PRIu64 ": %s; skipped\n", lno, why);
            g_st.unfit++;
            continue;
        }
        if (r == 0) { g_st.complete++; continue; }
        g_st.relaxed++;
        g_st.cells += (uint64_t)P->m;
        g_st.vars += (uint64_t)P->n;
        g_st.kept += (uint64_t)P->kept;
        g_st.freed += (uint64_t)P->freed;
        if (P->m < g_st.cells_min) g_st.cells_min = P->m;
        if (P->m > g_st.cells_max) g_st.cells_max = P->m;
        if (++nb == cap) { sf_chunk(out, chunk, nb); nb = 0; }
    }
    sf_chunk(out, chunk, nb);
    fclose(in);
    free(line); free(f); free(chunk);
    if (fclose(out) != 0) fatal("writing %s failed", out_path);

    char man[PATH_MAX];
    snprintf(man, sizeof man, "%s.outputs.txt", out_path);
    FILE *mf = fopen(man, "w");
    if (mf) { fprintf(mf, "%s\n", out_path); fclose(mf); }

    const double wall = omp_get_wtime() - t0;
    const double nr = g_st.relaxed ? (double)g_st.relaxed : 1.0, ns = g_st.starts ? (double)g_st.starts : 1.0;
    printf("[sum] soft: %" PRIu64 " board row(s) read, %" PRIu64 " relaxed", g_st.rows, g_st.relaxed);
    if (g_st.complete)  printf(", %" PRIu64 " already complete (nothing free; see --core)", g_st.complete);
    if (g_st.unfit)     printf(", %" PRIu64 " skipped (their pieces do not fit the frame)", g_st.unfit);
    if (g_st.malformed) printf(", %" PRIu64 " skipped as malformed", g_st.malformed);
    printf("\n");
    if (g_st.relaxed) {
        printf("[sum] free region: %.1f cells a board (%d..%d), %.0f variables, %.1f placed pieces kept",
               (double)g_st.cells / nr, g_st.cells_min, g_st.cells_max, (double)g_st.vars / nr,
               (double)g_st.kept / nr);
        if (g_st.freed) printf(", %.1f freed by --core", (double)g_st.freed / nr);
        printf("\n");
        printf("[sum] relaxation: %d start(s) a board; at the end %.2f expected breaks, %.4f unsettled, "
               "worst residual %.1e; %.0f evaluations and %d stage(s) at most a start",
               g_starts, g_st.breaks / ns, g_st.unsettled / ns, g_st.resid_max, g_st.evals / ns,
               g_st.stages_max);
        if (g_st.fails) printf("; NLopt failed %" PRIu64 " time(s)", g_st.fails);
        printf("\n");
        printf("[sum] boards: rounded mean %.2f best %d; climbed mean %.2f best %d\n",
               g_st.rounded / ns, g_st.best_rounded, g_st.climbed / ns, g_st.best);
        printf("[sum] written: %" PRIu64 " board(s), %.2f a board", g_st.written,
               (double)g_st.written / nr);
        if (g_st.freed_written)
            printf("; %.1f%% of the pieces --core freed went back to their cell",
                   100.0 * (double)g_st.returned / (double)g_st.freed_written);
        printf("\n");
        printf("[sum] time: %.2f s a board, %.3f s a start (relaxed on %d thread(s))\n",
               g_st.t_relax / nr, g_st.t_relax * g_nthreads / ns, g_nthreads);
    }
    printf("[sum] %" PRIu64 " board(s) written to %s in %.1fs%s\n", g_st.written, out_path, wall,
           g_stop ? " (interrupted)" : (deadline > 0.0 && omp_get_wtime() >= deadline) ? " (wall time reached)" : "");
    printf("  outputs_txt = %s\n", man);
    return 0;
}
