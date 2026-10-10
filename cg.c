/*
 * cg.c - parallel POSIX/GNU-style grep.
 *
 * Build:  cc -O3 -Wall -Wextra -pthread -o cg cg.c
 *         (or: make -C the directory that holds this file)
 *
 * The byte scanners (wm_find / find_lit_avx2 / Teddy) are frozen.
 * Behavior changes belong in option parsing, pattern setup, and output.
 * Do not retune the anchor bloom without a cycle count on HARD_REAL_10000.
 *
 * POSIX options: -E -F -c -e pat -f file -i -l -n -q -s -v -x
 * Extras:        -H -h -j N, and the GNU options listed by --help.
 *
 * Architecture
 *  - Small files (<=1 MiB) are read() whole by one worker, which also formats
 *    their output (no mmap/munmap, no cross-thread handoff of results).
 *    Larger files are mmap'd and split into ~4 MiB chunks; a chunk owns the lines
 *    that START in its byte range (no pre-scan). Workers search chunks, a
 *    printer thread emits results in argument order => output identical to
 *    serial grep.
 *  - Three matching engines, picked once at startup:
 *      FAST  : single fixed string (memchr/memcmp), whole-chunk scan
 *      DFA   : own ERE/BRE subset -> Thompson NFA -> lazy DFA (all patterns
 *              merged into ONE automaton; per-thread state cache, no locks)
 *      REGEX : libc regcomp/regexec fallback (backrefs, intervals, classes
 *              in UTF-8, ...). Each thread owns its regex_t: glibc's regexec
 *              locks inside regex_t, so sharing it serializes the threads.
 *  - Literal prefilter: if multiple branches have required literals (>=3 bytes),
 *    <=4 use a specialized AVX2 first/last-byte filter; 5..128 use a Teddy-style
 *    16-bucket, 3-byte AVX2 fingerprint filter; the automaton verifies candidates.
 *  - Pure-literal sets (Sherlock|Holmes|..., or -f of plain words): a confirmed
 *    hit is a match. N<=4 uses the AVX2 first/last filter, N<=32 Teddy, and
 *    larger sets a widened prefix hash. When every literal is at least 8 bytes,
 *    each literal keeps one rare 4-byte anchor. AVX2 classifies 32 windows
 *    against a reject bloom; only surviving windows walk the anchor chain.
 *    A set of one-byte literals is one membership scan. Other literals
 *    shorter than 3 bytes stay on a memchr side path so they no longer
 *    force the Aho-Corasick fallback.
 *    Aho-Corasick remains only when the hash filter cannot be built.
 *    A single ERE/BRE that is only literal alternatives is recognized before
 *    the NFA is built, so a 10k-word alternation never falls through to libc
 *    just because the Thompson automaton would exceed MAX_NFA.
 *  - Default worker count is physical cores on Linux; -j overrides it.
 *  - Line numbers are counted lazily (only with -n, only up to each hit).
 *
 * Exit: 0 = match, 1 = no match, >1 = error (-q: 0 if any match).
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <langinfo.h>
#include <locale.h>
#include <pthread.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#include <regex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_DEFAULT ((size_t)4 << 20)
static size_t chunk_size = CHUNK_DEFAULT;
#define CHUNK_SIZE chunk_size
#define MAX_NFA 50000
static int nfa_limit = MAX_NFA;   /* raised for huge pure-literal alternations */
#define MAXD 2048          /* DFA states cached per thread */
#define TABSZ 8192         /* hash slots (power of 2, > 2*MAXD) */
#define UNK 0xFFFF
#define TERM 0x8000
#define NLV  0xFFFE                 /* streaming: newline marker (TERM set, never a real id) */

typedef struct { size_t off, len, lineno; } Match;
struct FileJob;
typedef struct Chunk {
    struct FileJob *job;
    size_t idx;
    Match *m;
    size_t nm, cap, nlines, count;
    int done;                  /* chunk finished (guarded by mu) */
    int is_open;               /* queue item: "open this file" task */
    struct Chunk *next;
} Chunk;
typedef struct FileJob {
    const char *name, *data;
    size_t size;
    int mapped;
    size_t nchunks;
    Chunk *chunks;
    atomic_int cancel;         /* printer has enough: workers skip unstarted chunks */
    int done;                  /* ready: small/failed => fully processed, big => chunks queued */
    int binary;                /* NUL byte in the first 32 KiB */
    int msg;                   /* small output is the "binary file matches" message */
    int failed;                /* open/read error: print nothing for this file */
    int isdir;                 /* operand is a directory: -c prints 0, -L lists it */
    int small;                 /* processed whole by one worker, output preformatted */
    size_t total;              /* match count (small mode) */
    char *out;
    size_t outlen;
    Chunk *ot;                 /* the open task */
} FileJob;

/* options */
static const char *prog = "cg";
static int Eflag, Fflag, cflag, iflag, lflag, nflag, qflag, sflag, vflag, xflag;
static int wflag, oflag, bflag, Lflag, rflag, deref, dirskip, nullflag, line_buffered;
static int color_on, binmode;      /* binmode: 0 report, 1 text (-a), 2 without-match (-I) */
static long maxcount = -1, ctxA = -1, ctxB = -1;
static const char *label;
static int show_name = -1, store_matches;

/* patterns */
static char **fx;
static size_t *fxlen, npat;
enum { ENG_REGEX, ENG_FAST, ENG_DFA };
static int engine;
static int mb_locale;
static int mb_class_used;     /* a [:class:] was parsed in a UTF-8 locale */

/* prefilter literal */
static char *lit;
static size_t litlen, lit_r;
static int lit_verify;

/* Multi-literal candidate filter.  Each literal is sufficient (not necessary)
 * for its branch, so a hit is only a candidate; line_matches() remains the
 * final authority. */
typedef struct { char *p; size_t n, r; } MLit;
static MLit *mlits;
static size_t nmlit;
static int mlit_exact; /* every branch is this literal: AC hit == match */
/* Every literal is one byte. find_mlit is one class scan, not N memchrs. */
static int byte_class, byte_avx, byte_n;
static unsigned char byte_set[256], byte_list[16];

/* Aho-Corasick for the pure-literal case. nxt[] is completed with failure
 * transitions, so the scanner is one table lookup per byte and hit is closed
 * under output links. */
typedef struct { int nxt[256]; int fail, hit; } ACNode;
static ACNode *ac;
static int nac;

#define TEDDY_BUCKETS 8
#define TEDDY_MAX_LITS 128
#ifndef TEDDY_LIMIT
#define TEDDY_LIMIT 32   /* above this, Teddy buckets saturate: use the hash filter */
#endif

#if defined(__x86_64__)
typedef struct {
    __m256i lo[3];                 /* bucket bits packed one-per-bit in each byte */
    __m256i hi[3];
    int nb[TEDDY_BUCKETS];
    int bl[TEDDY_BUCKETS][TEDDY_MAX_LITS];
} TeddyTables;
static TeddyTables teddy;
static int exact_filter;           /* pure-literal set served by AVX2 filter, not AC */
static int teddy_ready;
static size_t teddy_maxn;
#endif

/* pipeline */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER,
                      slot_cv = PTHREAD_COND_INITIALIZER;
static Chunk *qhead, *qtail;
static int feed_done, busy;          /* busy: workers inside open_job/run_chunk; they may still enqueue */
static FileJob **jobs;
static size_t nfiles, printed, window;
static const char **files;
static atomic_int had_error;
static int any_match, out_error, want_name;
#define SMALL_MAX ((size_t)1 << 20)

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", prog, msg);
    exit(2);
}
static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

/* ================= patterns ================= */

static void add_pattern(const char *s, size_t n)
{
    fx = xrealloc(fx, (npat + 1) * sizeof *fx);
    fxlen = xrealloc(fxlen, (npat + 1) * sizeof *fxlen);
    fx[npat] = xrealloc(NULL, n + 1);
    memcpy(fx[npat], s, n);
    fx[npat][n] = 0;
    fxlen[npat++] = n;
}
static void add_patterns(const char *s, size_t n, int from_file)
{
    size_t start = 0;
    for (size_t i = 0; i <= n; i++)
        if (i == n || s[i] == '\n') {
            if (i == n && from_file && start == n) break;
            add_pattern(s + start, i - start);
            start = i + 1;
        }
}

/* ================= regex subset parser ================= */

enum { N_EMPTY, N_LIT, N_SET, N_CAT, N_ALT, N_STAR, N_PLUS, N_QUES };
typedef struct Node {
    int type, mb;           /* mb: SET also matches any UTF-8 multibyte char */
    unsigned char c;
    uint64_t set[4];
    struct Node **kid;
    int nkid;
} Node;
typedef struct { Node *n; int bol, eol; } Branch;
static Branch *br;
static int nbr;

typedef struct { const char *p; size_t n, i; int ere, bail; } Parser;

static Node *nnew(int t)
{
    Node *x = calloc(1, sizeof *x);
    if (!x) die("out of memory");
    x->type = t;
    return x;
}
static void kid_add(Node *p, Node *k)
{
    p->kid = xrealloc(p->kid, (size_t)(p->nkid + 1) * sizeof *p->kid);
    p->kid[p->nkid++] = k;
}
static void setbit(uint64_t *s, int c) { s[c >> 6] |= 1ULL << (c & 63); }
static int hasbit(const uint64_t *s, int c) { return (int)((s[c >> 6] >> (c & 63)) & 1); }

static Node *make_lit(Parser *P, unsigned char c)
{
    if (c >= 0x80 && (iflag || (mb_locale && !Fflag))) { P->bail = 1; return NULL; }
    if (iflag && isalpha(c)) {
        Node *s = nnew(N_SET);
        setbit(s->set, tolower(c));
        setbit(s->set, toupper(c));
        return s;
    }
    Node *l = nnew(N_LIT);
    l->c = c;
    return l;
}
static Node *make_any(void)
{
    Node *s = nnew(N_SET);
    for (int c = 0; c < (mb_locale ? 128 : 256); c++) setbit(s->set, c);
    s->mb = mb_locale;
    return s;
}

static int at_alt(Parser *P)
{
    if (P->i >= P->n) return 0;
    if (P->ere) return P->p[P->i] == '|';
    return P->p[P->i] == '\\' && P->i + 1 < P->n && P->p[P->i + 1] == '|';
}
static int at_rp(Parser *P, int depth)
{
    if (P->i >= P->n) return 0;
    if (P->ere) return depth > 0 && P->p[P->i] == ')';
    return P->p[P->i] == '\\' && P->i + 1 < P->n && P->p[P->i + 1] == ')';
}
/* Interval {n} {n,} {,m} {n,m} (BRE: \\{ \\}).  Expanded into plain concatenation /
 * star / optional nodes so intervals stay on the DFA instead of libc regex. */
static int q_lo, q_hi;                      /* parse-time scratch; -1 = unbounded */
#define MAX_INTERVAL 1000
static int take_interval(Parser *P)
{
    size_t j = P->i + (P->ere ? 1 : 2), d1 = 0, d2 = 0;
    long lo = 0, hi = -1;
    int comma = 0;
    while (j < P->n && isdigit((unsigned char)P->p[j])) { lo = lo * 10 + (P->p[j] - '0'); j++; d1++; if (lo > 100000) break; }
    if (j < P->n && P->p[j] == ',') {
        comma = 1; j++;
        long h = 0;
        while (j < P->n && isdigit((unsigned char)P->p[j])) { h = h * 10 + (P->p[j] - '0'); j++; d2++; if (h > 100000) break; }
        hi = d2 ? h : -1;
    } else hi = lo;
    if (!d1 && !d2) { P->bail = 1; return 0; }          /* "{}" or "{,}" */
    if (P->ere) { if (j >= P->n || P->p[j] != '}') { P->bail = 1; return 0; } j++; }
    else { if (j + 1 >= P->n || P->p[j] != '\\' || P->p[j + 1] != '}') { P->bail = 1; return 0; } j += 2; }
    (void)comma;
    if (lo > MAX_INTERVAL || hi > MAX_INTERVAL || (hi >= 0 && hi < lo)) { P->bail = 1; return 0; }
    q_lo = (int)lo; q_hi = (int)hi;
    P->i = j;
    return 'R';
}
static int take_quant(Parser *P)
{
    if (P->i >= P->n) return 0;
    char c = P->p[P->i];
    if (c == '*') { P->i++; return '*'; }
    if (P->ere) {
        if (c == '+' || c == '?') { P->i++; return c; }
        if (c == '{') return take_interval(P);
        return 0;
    }
    if (c == '\\' && P->i + 1 < P->n) {
        char d = P->p[P->i + 1];
        if (d == '+' || d == '?') { P->i += 2; return d; }
        if (d == '{') return take_interval(P);
    }
    return 0;
}
static size_t node_count(const Node *n)
{
    size_t t = 1;
    for (int i = 0; i < n->nkid; i++) t += node_count(n->kid[i]);
    return t;
}
static Node *node_copy(const Node *n)
{
    Node *x = nnew(n->type);
    x->mb = n->mb; x->c = n->c;
    memcpy(x->set, n->set, sizeof x->set);
    for (int i = 0; i < n->nkid; i++) kid_add(x, node_copy(n->kid[i]));
    return x;
}
/* a{lo,hi}: lo copies, then (hi-lo) nested optionals, or a star when unbounded */
static Node *expand_interval(Parser *P, Node *a, int lo, int hi)
{
    size_t per = node_count(a) + 2;
    size_t copies = (size_t)(hi < 0 ? lo + 1 : hi);
    if (per * (copies ? copies : 1) > 200000) { P->bail = 1; return a; }
    Node *cat = nnew(N_CAT);
    for (int i = 0; i < lo; i++) kid_add(cat, node_copy(a));
    if (hi < 0) {
        Node *st = nnew(N_STAR);
        kid_add(st, node_copy(a));
        kid_add(cat, st);
    } else if (hi > lo) {
        Node *inner = NULL;
        for (int i = 0; i < hi - lo; i++) {
            Node *c2 = nnew(N_CAT);
            kid_add(c2, node_copy(a));
            if (inner) kid_add(c2, inner);
            Node *q = nnew(N_QUES);
            kid_add(q, c2);
            inner = q;
        }
        kid_add(cat, inner);
    }
    return cat;                           /* empty CAT (a{0}) matches the empty string */
}

static Node *parse_bracket(Parser *P)
{
    uint64_t set[4] = {0, 0, 0, 0};
    int neg = 0, first = 1;
    P->i++;
    if (P->i < P->n && P->p[P->i] == '^') { neg = 1; P->i++; }
    for (;;) {
        if (P->i >= P->n) { P->bail = 1; return NULL; }
        unsigned char c = (unsigned char)P->p[P->i];
        if (c == ']' && !first) { P->i++; break; }
        first = 0;
        if (c >= 0x80) { P->bail = 1; return NULL; }
        if (c == '[' && P->i + 1 < P->n && strchr(":=.", P->p[P->i + 1])) {
            if (P->p[P->i + 1] != ':') { P->bail = 1; return NULL; }
            /* In a UTF-8 locale a class also covers non-ASCII characters. The DFA
             * only knows the ASCII members, so lines with bytes >= 0x80 are
             * decided by libc regex instead (see line_matches_base). */
            if (mb_locale) mb_class_used = 1;
            const char *s = P->p + P->i + 2, *e = NULL;
            for (size_t k = P->i + 2; k + 1 < P->n; k++)
                if (P->p[k] == ':' && P->p[k + 1] == ']') { e = P->p + k; break; }
            if (!e) { P->bail = 1; return NULL; }
            size_t len = (size_t)(e - s);
            static const struct { const char *nm; int (*f)(int); } cls[] = {
                {"alpha", isalpha}, {"digit", isdigit}, {"alnum", isalnum}, {"upper", isupper},
                {"lower", islower}, {"space", isspace}, {"blank", isblank}, {"punct", ispunct},
                {"print", isprint}, {"graph", isgraph}, {"cntrl", iscntrl}, {"xdigit", isxdigit}};
            int found = 0;
            for (size_t k = 0; k < sizeof cls / sizeof *cls; k++)
                if (strlen(cls[k].nm) == len && !strncmp(cls[k].nm, s, len)) {
                    for (int ch = 0; ch < 256; ch++) if (cls[k].f(ch)) setbit(set, ch);
                    found = 1;
                }
            if (!found) { P->bail = 1; return NULL; }
            P->i = (size_t)(e - P->p) + 2;
            continue;
        }
        if (P->i + 2 < P->n && P->p[P->i + 1] == '-' && P->p[P->i + 2] != ']') {
            unsigned char hi = (unsigned char)P->p[P->i + 2];
            if (hi >= 0x80 || hi < c) { P->bail = 1; return NULL; }
            for (int ch = c; ch <= hi; ch++) setbit(set, ch);
            P->i += 3;
        } else {
            setbit(set, c);
            P->i++;
        }
    }
    if (iflag)
        for (int ch = 'a'; ch <= 'z'; ch++)
            if (hasbit(set, ch) || hasbit(set, ch - 32)) { setbit(set, ch); setbit(set, ch - 32); }
    Node *s = nnew(N_SET);
    if (neg) {
        for (int ch = 0; ch < (mb_locale ? 128 : 256); ch++)
            if (!hasbit(set, ch)) setbit(s->set, ch);
        s->mb = mb_locale;
    } else memcpy(s->set, set, sizeof set);
    return s;
}

static Node *parse_seq(Parser *P, int depth, int top, int *bol, int *eol);

static Node *parse_group(Parser *P, int depth)
{
    Node *alt = nnew(N_ALT);
    for (;;) {
        int b, e;
        Node *s = parse_seq(P, depth, 0, &b, &e);
        if (P->bail) return NULL;
        kid_add(alt, s);
        if (at_alt(P)) { P->i += P->ere ? 1 : 2; continue; }
        if (at_rp(P, depth)) { P->i += P->ere ? 1 : 2; break; }
        P->bail = 1;
        return NULL;
    }
    return alt->nkid == 1 ? alt->kid[0] : alt;
}

/* GNU escapes \w \W \s \S as byte sets.  In a UTF-8 locale they also cover
 * non-ASCII characters, so lines with bytes >= 0x80 are decided by libc regex
 * (mb_class_used), exactly as for [:class:] brackets. */
static Node *make_escape_class(Parser *P, unsigned char d)
{
    int neg = (d == 'W' || d == 'S');
    uint64_t set[4] = {0, 0, 0, 0};
    for (int ch = 0; ch < 128; ch++) {
        int in = (d == 'w' || d == 'W') ? (isalnum(ch) || ch == '_') : isspace(ch);
        in = !!in;
        if (in != neg) setbit(set, ch);
    }
    Node *n = nnew(N_SET);
    memcpy(n->set, set, sizeof set);
    if (mb_locale) { mb_class_used = 1; n->mb = neg; }
    else if (neg) for (int ch = 128; ch < 256; ch++) setbit(n->set, ch);
    (void)P;
    return n;
}
static Node *parse_atom(Parser *P, int depth, int first)
{
    unsigned char c = (unsigned char)P->p[P->i];
    if (c == '.') { P->i++; return make_any(); }
    if (c == '[') return parse_bracket(P);
    if (P->ere) {
        switch (c) {
        case '(': P->i++; return parse_group(P, depth + 1);
        case ')': case '{': case '*': case '+': case '?': case '^': case '$': case '|':
            P->bail = 1; return NULL;
        case '\\': {
            if (P->i + 1 >= P->n) { P->bail = 1; return NULL; }
            unsigned char d = (unsigned char)P->p[P->i + 1];
            if (d < 0x80 && ispunct(d) && !strchr("<>`'", d)) { P->i += 2; return make_lit(P, d); }
            if (strchr("wWsS", d)) { P->i += 2; return make_escape_class(P, d); }
            P->bail = 1; return NULL;
        }
        default: P->i++; return make_lit(P, c);
        }
    }
    if (c == '\\') {
        if (P->i + 1 >= P->n) { P->bail = 1; return NULL; }
        unsigned char d = (unsigned char)P->p[P->i + 1];
        if (d == '(') { P->i += 2; return parse_group(P, depth + 1); }
        if (strchr(".[]\\*^$/", d)) { P->i += 2; return make_lit(P, d); }
        if (d && strchr("wWsS", d)) { P->i += 2; return make_escape_class(P, d); }
        P->bail = 1; return NULL;
    }
    if (c == '*' && !first) { P->bail = 1; return NULL; }
    P->i++;
    return make_lit(P, c);
}

static Node *parse_seq(Parser *P, int depth, int top, int *bol, int *eol)
{
    Node *cat = nnew(N_CAT);
    *bol = *eol = 0;
    if (P->i < P->n && P->p[P->i] == '^') {
        if (!top) { P->bail = 1; return NULL; }
        P->i++;
        *bol = 1;
    }
    int first = 1;
    while (P->i < P->n) {
        if (at_alt(P) || at_rp(P, depth)) break;
        unsigned char c = (unsigned char)P->p[P->i];
        if (c == '$') {
            int end_ok;
            if (P->ere) {
                size_t save = P->i;
                P->i++;
                end_ok = P->i >= P->n || at_alt(P);
                int rp = at_rp(P, depth);
                P->i = save;
                if (!(end_ok || rp) || !top) { P->bail = 1; return NULL; }
                if (end_ok) { P->i++; *eol = 1; break; }
                P->bail = 1; return NULL;
            } else {
                size_t save = P->i;
                P->i++;
                end_ok = P->i >= P->n || at_alt(P);
                int rp = at_rp(P, depth);
                P->i = save;
                if (rp) { P->bail = 1; return NULL; }
                if (end_ok) {
                    if (!top) { P->bail = 1; return NULL; }
                    P->i++; *eol = 1; break;
                }
            }
        }
        Node *a = parse_atom(P, depth, first);
        if (P->bail) return NULL;
        first = 0;
        for (;;) {
            int q = take_quant(P);
            if (P->bail) return NULL;
            if (!q) break;
            if (q == 'R') {
                a = expand_interval(P, a, q_lo, q_hi);
                if (P->bail) return NULL;
                continue;
            }
            Node *w = nnew(q == '*' ? N_STAR : q == '+' ? N_PLUS : N_QUES);
            kid_add(w, a);
            a = w;
        }
        kid_add(cat, a);
    }
    return cat;
}

static int parse_pattern(const char *s, size_t n)
{
    Parser P = {s, n, 0, Eflag, 0};
    for (;;) {
        int b, e;
        Node *x = parse_seq(&P, 0, 1, &b, &e);
        if (P.bail) return 0;
        br = xrealloc(br, (size_t)(nbr + 1) * sizeof *br);
        br[nbr++] = (Branch){x, b, e};
        if (at_alt(&P)) { P.i += P.ere ? 1 : 2; continue; }
        if (P.i == P.n) return 1;
        return 0;
    }
}

static int literal_branch(const char *s, size_t n)
{
    Parser P = {s, n, 0, 0, 0};
    Node *cat = nnew(N_CAT);
    for (size_t i = 0; i < n; i++) {
        Node *l = make_lit(&P, (unsigned char)s[i]);
        if (P.bail) return 0;
        kid_add(cat, l);
    }
    br = xrealloc(br, (size_t)(nbr + 1) * sizeof *br);
    br[nbr++] = (Branch){cat, 0, 0};
    return 1;
}

/* longest literal that every match must contain */
static void consider(const char *s, size_t n, char **best, size_t *bl)
{
    if (n > *bl) {
        free(*best);
        *best = xrealloc(NULL, n + 1);
        memcpy(*best, s, n);
        (*best)[n] = 0;
        *bl = n;
    }
}
static void req_lit(Node *n, char **best, size_t *bl)
{
    if (n->type == N_PLUS) { req_lit(n->kid[0], best, bl); return; }
    if (n->type != N_CAT) return;
    char *run = xrealloc(NULL, (size_t)n->nkid + 1);
    size_t rl = 0;
    for (int k = 0; k < n->nkid; k++) {
        Node *c = n->kid[k];
        if (c->type == N_LIT) { run[rl++] = (char)c->c; continue; }
        consider(run, rl, best, bl);
        rl = 0;
        req_lit(c, best, bl);
    }
    consider(run, rl, best, bl);
    free(run);
}

/* ================= NFA ================= */

typedef struct { int type, out1, out2; uint64_t set[4]; } NState; /* 0 CS,1 SPLIT,2 MATCH,3 EOLM */
static NState *nfa;
static int nnfa, capnfa;
static int *starts_all, *starts_float, n_all, n_float;

static int nstate(int type, int o1, int o2, const uint64_t *set)
{
    if (nnfa == capnfa) {
        capnfa = capnfa ? capnfa * 2 : 256;
        nfa = xrealloc(nfa, (size_t)capnfa * sizeof *nfa);
    }
    NState *s = &nfa[nnfa];
    memset(s, 0, sizeof *s);
    s->type = type; s->out1 = o1; s->out2 = o2;
    if (set) memcpy(s->set, set, sizeof s->set);
    return nnfa++;
}
static int range_cs(int lo, int hi, int next)
{
    uint64_t s[4] = {0, 0, 0, 0};
    for (int c = lo; c <= hi; c++) setbit(s, c);
    return nstate(0, next, -1, s);
}
static int build_mb(int next)
{
    int a2 = range_cs(0xC2, 0xDF, range_cs(0x80, 0xBF, next));
    int a3 = range_cs(0xE0, 0xEF, range_cs(0x80, 0xBF, range_cs(0x80, 0xBF, next)));
    int a4 = range_cs(0xF0, 0xF4, range_cs(0x80, 0xBF, range_cs(0x80, 0xBF, range_cs(0x80, 0xBF, next))));
    return nstate(1, nstate(1, a2, a3, NULL), a4, NULL);
}
static int build(Node *n, int next)
{
    if (nnfa > nfa_limit) return next;
    switch (n->type) {
    case N_EMPTY: return next;
    case N_LIT: { uint64_t s[4] = {0, 0, 0, 0}; setbit(s, n->c); return nstate(0, next, -1, s); }
    case N_SET: {
        int a = nstate(0, next, -1, n->set);
        return n->mb ? nstate(1, a, build_mb(next), NULL) : a;
    }
    case N_CAT:
        for (int k = n->nkid - 1; k >= 0; k--) next = build(n->kid[k], next);
        return next;
    case N_ALT: {
        int acc = build(n->kid[n->nkid - 1], next);
        for (int k = n->nkid - 2; k >= 0; k--) acc = nstate(1, build(n->kid[k], next), acc, NULL);
        return acc;
    }
    case N_STAR: { int s = nstate(1, -1, next, NULL); int b = build(n->kid[0], s); nfa[s].out1 = b; return s; }
    case N_PLUS: { int s = nstate(1, -1, next, NULL); int b = build(n->kid[0], s); nfa[s].out1 = b; return b; }
    case N_QUES: return nstate(1, build(n->kid[0], next), next, NULL);
    }
    return next;
}

/* ================= lazy DFA (per thread) ================= */

typedef struct {
    uint16_t next[256];
    int n, *ids;
    uint8_t accept, endacc, dead;
} DState;
typedef struct { int *ids, n, accept, endacc; } Set;
typedef struct {
    regex_t *re;
    char *buf; size_t bcap;
    char *fbuf; size_t fcap;   /* reusable small-file read buffer */
    DState *ds; int nds;
    uint16_t *tab, init;
    uint16_t *sflat;                 /* streaming table: next[id<<8|b], '\\n' -> NLV */
    uint32_t *mark, *eqmark, gen, eqgen, epoch;
    int *stk, *tmp, *tmp2;
    int *restart; int nrestart;
    int restart_accept, restart_endacc;
} Matcher;

static void closure(Matcher *M, int s, Set *o)
{
    if (M->mark[s] == M->gen) return;
    M->mark[s] = M->gen;
    int sp = 0;
    M->stk[sp++] = s;
    while (sp) {
        int x = M->stk[--sp];
        NState *q = &nfa[x];
        switch (q->type) {
        case 0: o->ids[o->n++] = x; break;
        case 1:
            if (M->mark[q->out2] != M->gen) { M->mark[q->out2] = M->gen; M->stk[sp++] = q->out2; }
            if (M->mark[q->out1] != M->gen) { M->mark[q->out1] = M->gen; M->stk[sp++] = q->out1; }
            break;
        case 2: o->accept = 1; break;
        case 3: o->endacc = 1; break;
        }
    }
}
static void make_init(Matcher *M);
static void reset_cache(Matcher *M);
static uint16_t tag(Matcher *M, uint16_t id)
{
    DState *d = &M->ds[id];
    return (d->accept || d->dead) ? (uint16_t)(id | TERM) : id;
}
static void reset_cache(Matcher *M)
{
    for (int i = 0; i < M->nds; i++) free(M->ds[i].ids);
    M->nds = 0;
    memset(M->tab, 0xFF, TABSZ * sizeof *M->tab);
    M->epoch++;
    make_init(M);
}
static inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static int same_set(Matcher *M, const DState *d, const Set *t)
{
    if (d->n != t->n) return 0;
    M->eqgen++;
    uint32_t g = M->eqgen;
    for (int i = 0; i < d->n; i++) M->eqmark[d->ids[i]] = g;
    for (int i = 0; i < t->n; i++)
        if (M->eqmark[t->ids[i]] != g) return 0;
    return 1;
}
static uint16_t intern(Matcher *M, Set *t)
{
    /* closure() guarantees uniqueness. Hash the subset independent of its
     * traversal order, then use a stamped membership test for exact equality.
     * This removes qsort() from the DFA transition-construction hot path. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < t->n; i++)
        h ^= mix32((uint32_t)t->ids[i] + 0x9e3779b9U);
    h ^= mix32((uint32_t)t->n * 0x85ebca6bU);
    h ^= mix32((uint32_t)(t->accept * 2 + t->endacc));
    uint32_t pos = h & (TABSZ - 1);
    while (M->tab[pos] != UNK) {
        DState *d = &M->ds[M->tab[pos]];
        if (d->n == t->n && d->accept == t->accept && d->endacc == t->endacc &&
            same_set(M, d, t))
            return tag(M, M->tab[pos]);
        pos = (pos + 1) & (TABSZ - 1);
    }
    if (M->nds >= MAXD) {
        reset_cache(M);
        pos = h & (TABSZ - 1);
        while (M->tab[pos] != UNK) pos = (pos + 1) & (TABSZ - 1);
    }
    uint16_t id = (uint16_t)M->nds++;
    DState *d = &M->ds[id];
    memset(d->next, 0xFF, sizeof d->next);
    if (M->sflat) { memset(M->sflat + ((size_t)id << 8), 0xFF, 256 * sizeof *M->sflat); M->sflat[((size_t)id << 8) | '\n'] = NLV; }
    d->n = t->n;
    d->ids = xrealloc(NULL, (size_t)t->n * sizeof(int) + 1);
    memcpy(d->ids, t->ids, (size_t)t->n * sizeof(int));
    d->accept = (uint8_t)t->accept;
    d->endacc = (uint8_t)t->endacc;
    d->dead = (t->n == 0 && !t->accept && !t->endacc && n_float == 0);
    M->tab[pos] = id;
    return tag(M, id);
}
static void make_init(Matcher *M)
{
    Set t = {M->tmp2, 0, 0, 0};
    M->gen++;
    for (int k = 0; k < n_all; k++) closure(M, starts_all[k], &t);
    M->init = (uint16_t)(intern(M, &t) & ~TERM);
}
static void build_restart(Matcher *M)
{
    M->nrestart = 0;
    M->restart_accept = M->restart_endacc = 0;
    if (!n_float) return;
    Set t = {M->tmp2, 0, 0, 0};
    M->gen++;
    for (int k = 0; k < n_float; k++) closure(M, starts_float[k], &t);
    M->restart = xrealloc(M->restart, (size_t)t.n * sizeof *M->restart + 1);
    memcpy(M->restart, t.ids, (size_t)t.n * sizeof *M->restart);
    M->nrestart = t.n;
    M->restart_accept = t.accept;
    M->restart_endacc = t.endacc;
}
static uint16_t compute(Matcher *M, uint16_t s, unsigned char b)
{
    Set t = {M->tmp, 0, 0, 0};
    M->gen++;
    DState *d = &M->ds[s];
    for (int k = 0; k < d->n; k++) {
        NState *q = &nfa[d->ids[k]];
        if (hasbit(q->set, b)) closure(M, q->out1, &t);
    }
    /* Unanchored search can restart at every byte. The epsilon-closure of all
     * floating branch starts is invariant, so compute it once per matcher. */
    if (M->nrestart || M->restart_accept || M->restart_endacc) {   /* flags matter even with no char states, e.g. a lone $ */
        for (int k = 0; k < M->nrestart; k++) {
            int id = M->restart[k];
            if (M->mark[id] != M->gen) {
                M->mark[id] = M->gen;
                t.ids[t.n++] = id;
            }
        }
        if (M->restart_accept) t.accept = 1;
        if (M->restart_endacc) t.endacc = 1;
    }
    uint32_t ep = M->epoch;
    uint16_t r = intern(M, &t);
    if (ep == M->epoch) { M->ds[s].next[b] = r; if (M->sflat && b != '\n') M->sflat[((size_t)s << 8) | b] = r; }
    return r;
}
static int dfa_match(Matcher *M, const unsigned char *p, size_t n)
{
    DState *ds = M->ds;
    uint16_t s = M->init;
    if (ds[s].accept) return 1;
    if (ds[s].dead) return 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t t = ds[s].next[p[i]];
        if (t & TERM) {
            if (t == UNK) {
                t = compute(M, s, p[i]);
                if (!(t & TERM)) { s = t; continue; }
            }
            return ds[t & ~TERM].accept;
        }
        s = t;
    }
    return ds[s].endacc;
}

/* ================= regex fallback ================= */

static char *escape_ere(const char *p, size_t n)
{
    char *o = xrealloc(NULL, n * 2 + 1), *q = o;
    for (size_t i = 0; i < n; i++) {
        if (strchr("\\.[]*^$+?(){}|", p[i])) *q++ = '\\';
        *q++ = p[i];
    }
    *q = 0;
    return o;
}
/* GNU grep treats a '{' that does not begin a valid interval ({n} {n,} {,m} {n,m}
 * after an operand) as a literal in ERE; libc's regcomp rejects it. */
static char *fix_ere_braces(const char *s)
{
    size_t n = strlen(s);
    char *o = xrealloc(NULL, n * 2 + 1), *q = o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { *q++ = s[i]; *q++ = s[++i]; continue; }
        if (s[i] == '[') {                                  /* bracket expression: copy verbatim */
            size_t j = i + 1;
            if (j < n && s[j] == '^') j++;
            if (j < n && s[j] == ']') j++;
            while (j < n && s[j] != ']') {
                if (s[j] == '[' && j + 1 < n && strchr(":.=", s[j + 1])) {
                    char t = s[j + 1];
                    size_t k = j + 2;
                    while (k + 1 < n && !(s[k] == t && s[k + 1] == ']')) k++;
                    j = (k + 1 < n) ? k + 2 : n;
                    continue;
                }
                j++;
            }
            if (j < n) j++;
            memcpy(q, s + i, j - i); q += j - i; i = j - 1;
            continue;
        }
        if (s[i] == '{') {
            int ok = 0;
            if (i > 0 && s[i - 1] != '(' && s[i - 1] != '|') {
                size_t j = i + 1, d1 = 0, d2 = 0;
                while (j < n && isdigit((unsigned char)s[j])) { j++; d1++; }
                if (j < n && s[j] == ',') { j++; while (j < n && isdigit((unsigned char)s[j])) { j++; d2++; } }
                ok = j < n && s[j] == '}' && (d1 || d2);
            }
            if (!ok) *q++ = '\\';
        }
        *q++ = s[i];
    }
    *q = 0;
    return o;
}
static regex_t *compile_set(void)
{
    regex_t *re = calloc(npat, sizeof *re);
    if (!re) die("out of memory");
    for (size_t i = 0; i < npat; i++) {
        int ere = Eflag || Fflag;
        char *src = Fflag ? escape_ere(fx[i], fxlen[i]) : strdup(fx[i]);
        if (!src) die("out of memory");
        if (ere && !Fflag) { char *f = fix_ere_braces(src); free(src); src = f; }
        /* -x: no wrapping (it would shift backreference numbers); the match
         * span is checked against the whole line in line_matches() instead. */
        int rc = regcomp(&re[i], src, (ere ? REG_EXTENDED : 0) | (iflag ? REG_ICASE : 0) |
                                          (xflag ? 0 : REG_NOSUB));
        if (rc) {
            char eb[256];
            regerror(rc, &re[i], eb, sizeof eb);
            fprintf(stderr, "%s: %s\n", prog, eb);
            exit(2);
        }
        free(src);
    }
    return re;
}
static void free_set(regex_t *re)
{
    if (!re) return;
    for (size_t i = 0; i < npat; i++) regfree(&re[i]);
    free(re);
}

/* ================= matching ================= */

static int commonness(unsigned char c);
static const char *find_lit_scalar(const char *h, size_t hn)
{
    /* An empty pattern matches every line, including a zero-length one.
     * Returning the start lets the line walker advance one line per hit.
     * Searching for the terminating NUL would read off the buffer. */
    if (litlen == 0) return h;
    if (hn < litlen) return NULL;
    size_t L = hn - litlen + 1;
    const char *cur = h + lit_r;
    while (L > 0) {
        const char *q = memchr(cur, lit[lit_r], L);
        if (!q) return NULL;
        const char *s = q - lit_r;
        if (!memcmp(s, lit, litlen)) return s;
        L -= (size_t)(q + 1 - cur);
        cur = q + 1;
    }
    return NULL;
}
#if defined(__x86_64__)
/* Two rare bytes of one literal, 32 at a time. A one-byte memchr of 'm' in
 * "holmes" confirms on a few percent of English; the second byte drops that
 * to a memcmp only when both land. */
__attribute__((target("avx2")))
static const char *find_lit_avx2(const char *h, size_t hn)
{
    if (hn < litlen || litlen < 2) return find_lit_scalar(h, hn);
    size_t a = lit_r, b = a == 0 ? 1 : 0;
    for (size_t i = 0; i < litlen; i++) {
        if (i == a) continue;
        if (commonness((unsigned char)lit[i]) < commonness((unsigned char)lit[b])) b = i;
    }
    size_t span = a > b ? a : b;
    const char *end = h + hn, *p = h;
    __m256i va = _mm256_set1_epi8(lit[a]);
    __m256i vb = _mm256_set1_epi8(lit[b]);
    while ((size_t)(end - p) >= 32 + span) {
        unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + a)), va),
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + b)), vb)));
        while (m) {
            int bit = __builtin_ctz(m);
            m &= m - 1;
            if (!memcmp(p + bit, lit, litlen)) return p + bit;
        }
        p += 32;
    }
    return find_lit_scalar(p, (size_t)(end - p));
}
#endif
/* ---- case-insensitive single literal (-i) ----------------------------------
 * `lit` is stored lowercased.  Two rare bytes are compared after |0x20, which
 * folds ASCII letters (and over-accepts a few punctuation pairs - harmless, the
 * hit is verified).  Hits are confirmed with a folded compare, then the line is
 * verified by the DFA like any other prefilter hit. */
static int lit_icase;
static inline int fold_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x |= 0x20;
        if (x != y) return 0;
    }
    return 1;
}
static const char *find_lit_ci_scalar(const char *h, size_t hn)
{
    if (hn < litlen) return NULL;
    for (size_t i = 0; i + litlen <= hn; i++) {
        unsigned char x = (unsigned char)h[i + lit_r];
        if (x >= 'A' && x <= 'Z') x |= 0x20;
        if (x == (unsigned char)lit[lit_r] && fold_eq(h + i, lit, litlen)) return h + i;
    }
    return NULL;
}
#if defined(__x86_64__)
__attribute__((target("avx2")))
static const char *find_lit_ci_avx2(const char *h, size_t hn)
{
    if (hn < litlen || litlen < 2) return find_lit_ci_scalar(h, hn);
    size_t a = lit_r, b = a == 0 ? 1 : 0;
    for (size_t i = 0; i < litlen; i++) {
        if (i == a) continue;
        if (commonness((unsigned char)lit[i]) < commonness((unsigned char)lit[b])) b = i;
    }
    size_t span = a > b ? a : b;
    const char *end = h + hn, *p = h;
    const __m256i f = _mm256_set1_epi8(0x20);
    __m256i va = _mm256_set1_epi8((char)((unsigned char)lit[a] | 0x20));
    __m256i vb = _mm256_set1_epi8((char)((unsigned char)lit[b] | 0x20));
    while ((size_t)(end - p) >= 32 + span) {
        unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(
            _mm256_cmpeq_epi8(_mm256_or_si256(_mm256_loadu_si256((const __m256i *)(p + a)), f), va),
            _mm256_cmpeq_epi8(_mm256_or_si256(_mm256_loadu_si256((const __m256i *)(p + b)), f), vb)));
        while (m) {
            int bit = __builtin_ctz(m);
            m &= m - 1;
            if (fold_eq(p + bit, lit, litlen)) return p + bit;
        }
        p += 32;
    }
    return find_lit_ci_scalar(p, (size_t)(end - p));
}
#endif
static const char *find_lit(const char *h, size_t hn)
{
    if (lit_icase) {
#if defined(__x86_64__)
        if (litlen >= 2 && __builtin_cpu_supports("avx2")) return find_lit_ci_avx2(h, hn);
#endif
        return find_lit_ci_scalar(h, hn);
    }
#if defined(__x86_64__)
    if (litlen >= 2 && __builtin_cpu_supports("avx2")) return find_lit_avx2(h, hn);
#endif
    return find_lit_scalar(h, hn);
}
/* Approximate byte frequency rank for text corpora (higher = more common).
 * The literal prefilters scan for the *rarest* byte of a needle. */
static unsigned char bfreq[256];
static void init_bfreq(void)
{
    static const char order[] =
        " etaoinsrhldcumfpgwybvkxjqz"      /* most common letters (lowercase) */
        "\n,.-'\"?!:;0123456789";
    memset(bfreq, 1, sizeof bfreq);
    int n = (int)sizeof order - 1;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)order[i];
        int v = 40 - i;                     /* 40 .. */
        if (v < 4) v = 4;
        bfreq[c] = (unsigned char)v;
        if (c >= 'a' && c <= 'z') bfreq[c - 32] = (unsigned char)(v / 4 + 2);
    }
    for (int c = 128; c < 256; c++) bfreq[c] = 2;
}
static int commonness(unsigned char c)
{
    if (!bfreq[' ']) init_bfreq();
    return bfreq[c];
}
static void set_literal(const char *s, size_t n, int verify)
{
    lit = xrealloc(NULL, n + 1);
    memcpy(lit, s, n);
    lit[n] = 0;
    litlen = n;
    lit_verify = verify;
    lit_r = 0;
    for (size_t i = 1; i < n; i++)
        if (commonness((unsigned char)s[i]) < commonness((unsigned char)s[lit_r])) lit_r = i;
}

/* ---- match spans: -o, --color, and the exact -w check --------------------
 * The scan engines only answer "does this line match".  For the rarer features
 * that need *where*, each thread lazily compiles a libc regex per pattern.
 * -w wraps the pattern so the match must be bounded by non-word characters;
 * -x anchors it.  Back-references are renumbered to account for the wrapper. */
typedef struct { regex_t re; int kind; } SRe;       /* kind: 0 plain, 1 -w, 2 -x */
static __thread SRe *tl_sre;
/* glibc's regexec can mis-evaluate patterns with back-references when nmatch is
 * smaller than the number of groups, so always pass room for every group. */
static __thread regmatch_t *tl_pm;
static __thread size_t tl_pmcap;
static regmatch_t *rx_scratch(const regex_t *re)
{
    size_t k = re->re_nsub + 1;
    if (k < 3) k = 3;
    if (k > tl_pmcap) { tl_pm = xrealloc(tl_pm, k * sizeof *tl_pm); tl_pmcap = k; }
    return tl_pm;
}

static char *shift_backrefs(const char *s, int k)
{
    size_t n = strlen(s);
    char *o = xrealloc(NULL, n + 1), *q = o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            if (s[i + 1] >= '1' && s[i + 1] <= '9') {
                int d = s[i + 1] - '0' + k;
                if (d > 9) {
                    fprintf(stderr, "%s: back-reference \\%c is too high to combine with -w/-x\n",
                            prog, s[i + 1]);
                    exit(2);
                }
                *q++ = '\\'; *q++ = (char)('0' + d); i++; continue;
            }
            *q++ = s[i]; *q++ = s[i + 1]; i++; continue;
        }
        *q++ = s[i];
    }
    *q = 0;
    return o;
}
static char *cat3(const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *o = xrealloc(NULL, la + lb + lc + 1);
    memcpy(o, a, la); memcpy(o + la, b, lb); memcpy(o + la + lb, c, lc + 1);
    return o;
}
static SRe *span_compile(void)                      /* exits 2 on a bad pattern */
{
    SRe *sr = calloc(npat ? npat : 1, sizeof *sr);
    if (!sr) die("out of memory");
    int ere = Eflag || Fflag;
    for (size_t i = 0; i < npat; i++) {
        char *base = Fflag ? escape_ere(fx[i], fxlen[i]) : strdup(fx[i]);
        if (!base) die("out of memory");
        if (ere && !Fflag) { char *f = fix_ere_braces(base); free(base); base = f; }
        char *src;
        if (xflag) {
            char *sh = shift_backrefs(base, 1);
            src = ere ? cat3("^(", sh, ")$") : cat3("^\\(", sh, "\\)$");
            free(sh); sr[i].kind = 2;
        } else if (wflag) {
            char *sh = shift_backrefs(base, 2);
            src = ere ? cat3("(^|[^[:alnum:]_])(", sh, ")([^[:alnum:]_]|$)")
                      : cat3("\\(^\\|[^[:alnum:]_]\\)\\(", sh, "\\)\\([^[:alnum:]_]\\|$\\)");
            free(sh); sr[i].kind = 1;
        } else {
            src = strdup(base);
            if (!src) die("out of memory");
        }
        free(base);
        int rc = regcomp(&sr[i].re, src, (ere ? REG_EXTENDED : 0) | (iflag ? REG_ICASE : 0));
        if (rc) {
            char eb[256];
            regerror(rc, &sr[i].re, eb, sizeof eb);
            fprintf(stderr, "%s: %s\n", prog, eb);
            exit(2);
        }
        free(src);
    }
    return sr;
}
/* leftmost-longest match at or after `from` across all patterns */
static int ls_on;
static int lset_next(const char *l, size_t n, size_t from, size_t *ms, size_t *me);
static int pattern_as_literal(const char *s, size_t n, char **out, size_t *on);
static int span_next(const char *l, size_t n, size_t from, size_t *ms, size_t *me)
{
    if (ls_on) { int r = lset_next(l, n, from, ms, me); if (r >= 0) return r; }
    if (!tl_sre) tl_sre = span_compile();
    size_t bs = (size_t)-1, be = 0;
    for (size_t i = 0; i < npat; i++) {
        regmatch_t *pm = rx_scratch(&tl_sre[i].re);
        pm[0].rm_so = (regoff_t)from; pm[0].rm_eo = (regoff_t)n;
        if (regexec(&tl_sre[i].re, l, tl_sre[i].re.re_nsub + 1, pm, REG_STARTEND)) continue;
        regoff_t so = pm[0].rm_so, eo = pm[0].rm_eo;
        if (tl_sre[i].kind == 1) { so = pm[2].rm_so; eo = pm[2].rm_eo; }
        if (so < 0) continue;
        if ((size_t)so < bs || ((size_t)so == bs && (size_t)eo > be)) { bs = (size_t)so; be = (size_t)eo; }
    }
    if (bs == (size_t)-1) return 0;
    *ms = bs; *me = be;
    return 1;
}
/* Longest run of top-level literal / case-pair nodes of the single branch,
 * returned lowercased.  Only used for -i. */
static char *req_lit_ci(Node *n, size_t *bl)
{
    *bl = 0;
    if (n->type != N_CAT) return NULL;
    char *best = NULL, *run = xrealloc(NULL, (size_t)n->nkid + 1);
    size_t rl = 0;
    for (int k = 0; k <= n->nkid; k++) {
        int ch = -1;
        if (k < n->nkid) {
            Node *x = n->kid[k];
            if (x->type == N_LIT) ch = x->c < 0x80 ? x->c : -1;
            else if (x->type == N_SET) {
                int cnt = 0, first = -1, second = -1;
                for (int b = 0; b < 256 && cnt < 3; b++)
                    if ((x->set[b >> 6] >> (b & 63)) & 1) { if (cnt == 0) first = b; else second = b; cnt++; }
                if (cnt == 2 && !x->mb && first >= 'A' && first <= 'Z' && second == first + 32) ch = second;
            }
        }
        if (ch >= 0) { run[rl++] = (char)ch; continue; }
        if (rl > *bl) {
            free(best);
            best = xrealloc(NULL, rl + 1);
            memcpy(best, run, rl);
            best[rl] = 0;
            *bl = rl;
        }
        rl = 0;
    }
    free(run);
    return best;
}
static int pure_lit(Node *n, size_t *len);
/* -w with a pure-literal word set: a literal made only of word characters matches
 * as a whole word iff it equals a maximal word token of the line, so the check is
 * a hash lookup per token instead of a libc regex over a giant alternation. */
static char **ws_lit; static size_t *ws_len, ws_cap; static int wset_on;
static inline int isw(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static inline size_t ws_hash(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 1099511628211ULL;
    return (size_t)(h ^ (h >> 29));
}
static void wset_add(const char *p, size_t n)
{
    size_t i = ws_hash(p, n) & (ws_cap - 1);
    while (ws_lit[i]) {
        if (ws_len[i] == n && !memcmp(ws_lit[i], p, n)) return;
        i = (i + 1) & (ws_cap - 1);
    }
    ws_lit[i] = xrealloc(NULL, n + 1); memcpy(ws_lit[i], p, n); ws_len[i] = n;
}
static void wset_build(void)
{
    size_t total = 0;
    for (int i = 0; i < nbr; i++) {
        size_t L;
        if (br[i].bol || br[i].eol || !pure_lit(br[i].n, &L)) return;
        Node *nd = br[i].n;
        if (nd->type == N_LIT) { if (!isw(nd->c)) return; }
        else for (int k = 0; k < nd->nkid; k++) if (!isw(nd->kid[k]->c)) return;
        total++;
    }
    ws_cap = 16; while (ws_cap < total * 2) ws_cap <<= 1;
    ws_lit = calloc(ws_cap, sizeof *ws_lit); ws_len = calloc(ws_cap, sizeof *ws_len);
    if (!ws_lit || !ws_len) die("out of memory");
    for (int i = 0; i < nbr; i++) {
        Node *nd = br[i].n; char buf[4096]; size_t m = 0;
        if (nd->type == N_LIT) buf[m++] = (char)nd->c;
        else for (int k = 0; k < nd->nkid && m < sizeof buf; k++) buf[m++] = (char)nd->kid[k]->c;
        wset_add(buf, m);
    }
    wset_on = 1;
}
/* 1 = some token is a pattern, 0 = none, -1 = line has non-ASCII bytes: ask the regex */
static int wset_check(const char *l, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)l[i];
        if (c >= 0x80) return -1;
        if (!isw(c)) { i++; continue; }
        size_t j = i + 1;
        while (j < n && isw((unsigned char)l[j])) j++;
        if (j < n && (unsigned char)l[j] >= 0x80) return -1;
        size_t h = ws_hash(l + i, j - i) & (ws_cap - 1);
        while (ws_lit[h]) {
            if (ws_len[h] == j - i && !memcmp(ws_lit[h], l + i, j - i)) return 1;
            h = (h + 1) & (ws_cap - 1);
        }
        i = j;
    }
    return 0;
}

/* ---- literal-set spans for -o / --color ------------------------------------
 * A plain word list needs no regex to locate matches: bucket the literals by
 * their first two bytes (longest first) and report the leftmost-longest hit.
 * With -w and word-only literals a match is exactly a word token that is in
 * the set.  Anything else (alternations, classes, -i, -x) keeps the regex path. */
static char **ls_p; static size_t *ls_n, n_ls;
static unsigned char ls1[256];
static uint32_t *ls_idx;                    /* 65537 offsets into ls_p/ls_n by 2-byte key */
static int ls_cmp_i;
static int ls_cmp(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    unsigned kx = ((unsigned char)ls_p[x][0] << 8) | (unsigned char)ls_p[x][1];
    unsigned ky = ((unsigned char)ls_p[y][0] << 8) | (unsigned char)ls_p[y][1];
    if (kx != ky) return kx < ky ? -1 : 1;
    if (ls_n[x] != ls_n[y]) return ls_n[x] > ls_n[y] ? -1 : 1;
    return 0;
}
static void lset_build(void)
{
    if (!(oflag || color_on) || iflag || xflag || !npat) return;
    char **tp = xrealloc(NULL, npat * sizeof *tp);
    size_t *tn = xrealloc(NULL, npat * sizeof *tn), m = 0;
    for (size_t i = 0; i < npat; i++) {
        char *p; size_t n;
        if (!pattern_as_literal(fx[i], fxlen[i], &p, &n) || n == 0) {
            for (size_t k = 0; k < m; k++) free(tp[k]);
            free(tp); free(tn);
            return;
        }
        tp[m] = p; tn[m] = n; m++;
    }
    if (wflag && !wset_on) {                         /* -w needs the token set */
        for (size_t k = 0; k < m; k++) free(tp[k]);
        free(tp); free(tn);
        return;
    }
    if (wflag) {                                     /* token path: no table needed */
        for (size_t k = 0; k < m; k++) free(tp[k]);
        free(tp); free(tn);
        ls_on = 2;
        return;
    }
    size_t cnt2 = 0;
    for (size_t k = 0; k < m; k++) {
        if (tn[k] == 1) ls1[(unsigned char)tp[k][0]] = 1;
        else cnt2++;
    }
    ls_p = xrealloc(NULL, (cnt2 + 1) * sizeof *ls_p);
    ls_n = xrealloc(NULL, (cnt2 + 1) * sizeof *ls_n);
    size_t j = 0;
    for (size_t k = 0; k < m; k++) {
        if (tn[k] == 1) { free(tp[k]); continue; }
        ls_p[j] = tp[k]; ls_n[j] = tn[k]; j++;
    }
    free(tp); free(tn);
    size_t *ord = xrealloc(NULL, (cnt2 + 1) * sizeof *ord);
    for (size_t k = 0; k < cnt2; k++) ord[k] = k;
    qsort(ord, cnt2, sizeof *ord, ls_cmp);
    char **sp = xrealloc(NULL, (cnt2 + 1) * sizeof *sp);
    size_t *sn = xrealloc(NULL, (cnt2 + 1) * sizeof *sn);
    for (size_t k = 0; k < cnt2; k++) { sp[k] = ls_p[ord[k]]; sn[k] = ls_n[ord[k]]; }
    free(ls_p); free(ls_n); free(ord);
    ls_p = sp; ls_n = sn; n_ls = cnt2;
    ls_idx = calloc(65537, sizeof *ls_idx);
    if (!ls_idx) die("out of memory");
    for (size_t k = 0; k < cnt2; k++)
        ls_idx[(((unsigned char)ls_p[k][0] << 8) | (unsigned char)ls_p[k][1]) + 1]++;
    for (int k = 0; k < 65536; k++) ls_idx[k + 1] += ls_idx[k];
    ls_on = 1;
    (void)ls_cmp_i;
}
/* returns 1 found, 0 none, -1 caller must use the regex path */
static int lset_next(const char *l, size_t n, size_t from, size_t *ms, size_t *me)
{
    if (ls_on == 2) {                                /* -w: tokens */
        size_t i = from;
        if (i > 0 && i < n && isw((unsigned char)l[i - 1]) && isw((unsigned char)l[i])) {
            while (i < n && isw((unsigned char)l[i])) i++;      /* partial token: skip */
        }
        while (i < n) {
            unsigned char c = (unsigned char)l[i];
            if (c >= 0x80) return -1;
            if (!isw(c)) { i++; continue; }
            size_t j = i + 1;
            while (j < n && isw((unsigned char)l[j])) j++;
            if (j < n && (unsigned char)l[j] >= 0x80) return -1;
            size_t h = ws_hash(l + i, j - i) & (ws_cap - 1);
            while (ws_lit[h]) {
                if (ws_len[h] == j - i && !memcmp(ws_lit[h], l + i, j - i)) { *ms = i; *me = j; return 1; }
                h = (h + 1) & (ws_cap - 1);
            }
            i = j;
        }
        return 0;
    }
    for (size_t i = from; i < n; i++) {
        unsigned char c = (unsigned char)l[i];
        size_t best = ls1[c] ? 1 : 0;
        if (i + 1 < n) {
            unsigned key = (c << 8) | (unsigned char)l[i + 1];
            for (uint32_t k = ls_idx[key]; k < ls_idx[key + 1]; k++) {
                size_t m = ls_n[k];
                if (m <= best) break;
                if (m <= n - i && !memcmp(l + i + 2, ls_p[k] + 2, m - 2)) { best = m; break; }
            }
        }
        if (best) { *ms = i; *me = i + best; return 1; }
    }
    return 0;
}
static int wcheck(const char *l, size_t n)          /* exact -w verification */
{
    if (wset_on) { int r = wset_check(l, n); if (r >= 0) return r; }
    if (!tl_sre) tl_sre = span_compile();
    for (size_t i = 0; i < npat; i++) {
        regmatch_t *pm = rx_scratch(&tl_sre[i].re);
        pm[0].rm_so = 0; pm[0].rm_eo = (regoff_t)n;
        if (!regexec(&tl_sre[i].re, l, tl_sre[i].re.re_nsub + 1, pm, REG_STARTEND)) return 1;
    }
    return 0;
}

static int wdfa_on;
static inline int has_high_byte(const char *l, size_t n);
static int line_matches_base(const char *l, size_t n, Matcher *M);
static int line_matches(const char *l, size_t n, Matcher *M)
{
    if (wset_on) return wcheck(l, n);               /* token lookup is exact by itself */
    if (wdfa_on) {                                  /* bounded DFA is exact for ASCII lines */
        if (mb_locale && has_high_byte(l, n)) return wcheck(l, n);
        return dfa_match(M, (const unsigned char *)l, n);
    }
    int r = line_matches_base(l, n, M);
    if (r && wflag) r = wcheck(l, n);
    return r;
}
static int regex_line(const char *l, size_t n, Matcher *M);
static inline int has_high_byte(const char *l, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, l + i, 8);
        if (w & 0x8080808080808080ULL) return 1;
    }
    for (; i < n; i++) if ((unsigned char)l[i] & 0x80) return 1;
    return 0;
}
static int line_matches_base(const char *l, size_t n, Matcher *M)
{
    if (engine == ENG_DFA) {
        if (mb_class_used && has_high_byte(l, n)) {
            if (!M->re) M->re = compile_set();
            return regex_line(l, n, M);
        }
        return dfa_match(M, (const unsigned char *)l, n);
    }
    if (engine == ENG_FAST) {
        for (size_t i = 0; i < npat; i++) {
            if (xflag) {
                if (n == fxlen[i] && !memcmp(l, fx[i], n)) return 1;
            } else if (fxlen[i] == 0) {
                return 1;
            } else if (n >= fxlen[i]) {
                /* tiny local search */
                const char *e = l + n - fxlen[i] + 1, *h = l;
                while (h < e) {
                    h = memchr(h, fx[i][0], (size_t)(e - h));
                    if (!h) break;
                    if (!memcmp(h, fx[i], fxlen[i])) return 1;
                    h++;
                }
            }
        }
        return 0;
    }
    return regex_line(l, n, M);
}
static int regex_line(const char *l, size_t n, Matcher *M)
{
    if (M->bcap < n + 1) {
        M->bcap = (n + 1) * 2;
        M->buf = xrealloc(M->buf, M->bcap);
    }
    memcpy(M->buf, l, n);
    M->buf[n] = 0;
    for (size_t i = 0; i < npat; i++) {
        if (xflag) {
            regmatch_t *pm = rx_scratch(&M->re[i]);
            /* leftmost-longest: if a whole-line match exists, it is the one reported */
            if (!regexec(&M->re[i], M->buf, M->re[i].re_nsub + 1, pm, 0) && pm[0].rm_so == 0 &&
                (size_t)pm[0].rm_eo == n)
                return 1;
        } else if (!regexec(&M->re[i], M->buf, 0, NULL, 0)) {
            return 1;
        }
    }
    return 0;
}

static size_t line_start_at_or_after(const char *d, size_t n, size_t p)
{
    if (p == 0) return 0;
    if (p >= n) return n;
    const char *q = memchr(d + p - 1, '\n', n - (p - 1));
    return q ? (size_t)(q - d) + 1 : n;
}
#if defined(__x86_64__)
__attribute__((target("avx2")))
static size_t count_nl_avx2(const char *p, size_t n)
{
    const __m256i nl = _mm256_set1_epi8('\n');
    size_t c = 0, i = 0;
    while (i + 128 <= n) {                           /* 4 vectors per iteration: byte counters, then sum */
        __m256i a = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + i)), nl);
        __m256i b = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + i + 32)), nl);
        __m256i d = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + i + 64)), nl);
        __m256i e = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + i + 96)), nl);
        __m256i sum = _mm256_sub_epi8(_mm256_sub_epi8(_mm256_setzero_si256(), a), _mm256_add_epi8(_mm256_add_epi8(b, d), e)); /* -a - (b+d+e) = count */
        __m256i z = _mm256_sad_epu8(sum, _mm256_setzero_si256());
        c += (size_t)_mm256_extract_epi64(z, 0) + (size_t)_mm256_extract_epi64(z, 1) +
             (size_t)_mm256_extract_epi64(z, 2) + (size_t)_mm256_extract_epi64(z, 3);
        i += 128;
    }
    for (; i + 32 <= n; i += 32)
        c += (size_t)__builtin_popcount((unsigned)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + i)), nl)));
    for (; i < n; i++) c += (p[i] == '\n');
    return c;
}
#endif
static size_t count_nl(const char *p, size_t n)
{
#if defined(__x86_64__)
    if (n >= 64 && __builtin_cpu_supports("avx2")) return count_nl_avx2(p, n);
#endif
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += (p[i] == '\n');
    return c;
}
static void push_match(Chunk *c, size_t off, size_t len, size_t ln)
{
    if (c->nm == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->m = xrealloc(c->m, c->cap * sizeof *c->m);
    }
    c->m[c->nm++] = (Match){off, len, ln};
}

/* prefilter path: skip straight to lines containing the literal */
static void scan_literal(Chunk *c, Matcher *M, size_t begin, size_t end)
{
    const char *d = c->job->data;
    size_t pos = begin, cnt = 0, cnt_pos = begin, count = 0;
    while (pos < end) {
        const char *h = find_lit(d + pos, end - pos);
        if (!h) break;
        size_t hp = (size_t)(h - d), ls = hp;
        /* the line start is only needed to verify, store or number the line */
        if (lit_verify || store_matches || nflag)
            while (ls > pos && d[ls - 1] != '\n') ls--;
        const char *nl = memchr(d + hp, '\n', end - hp);
        size_t le = nl ? (size_t)(nl - d) : end;
        if (!lit_verify || line_matches(d + ls, le - ls, M)) {
            count++;
            if (qflag) _exit(0);
            if (store_matches) {
                size_t ln = 0;
                if (nflag) { cnt += count_nl(d + cnt_pos, ls - cnt_pos); cnt_pos = ls; ln = cnt + 1; }
                push_match(c, ls, le - ls, ln);
            }
        }
        pos = le < end ? le + 1 : end;
    }
    if (nflag) {
        cnt += count_nl(d + cnt_pos, end - cnt_pos);
        if (end > begin && d[end - 1] != '\n') cnt++;
        c->nlines = cnt;
    }
    c->count = count;
}

static const char *find_one_mlit(const char *h, size_t hn, const MLit *m)
{
    if (hn < m->n) return NULL;
    size_t L = hn - m->n + 1;
    const char *cur = h + m->r;
    while (L > 0) {
        const char *q = memchr(cur, m->p[m->r], L);
        if (!q) return NULL;
        const char *s = q - m->r;
        if (!memcmp(s, m->p, m->n)) return s;
        L -= (size_t)(q + 1 - cur);
        cur = q + 1;
    }
    return NULL;
}
static const char *find_byte_class_scalar(const char *h, size_t hn)
{
    const unsigned char *p = (const unsigned char *)h;
    for (size_t i = 0; i < hn; i++)
        if (byte_set[p[i]]) return h + i;
    return NULL;
}
#if defined(__x86_64__)
__attribute__((target("avx2"), noinline))
static const char *find_byte_class_avx2(const char *h, size_t hn)
{
    size_t i = 0;
    int n = byte_n;
    while (i + 32 <= hn) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(h + i));
        __m256i m = _mm256_setzero_si256();
        for (int k = 0; k < n; k++)
            m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)byte_list[k])));
        unsigned mask = (unsigned)_mm256_movemask_epi8(m);
        if (mask) return h + i + (size_t)__builtin_ctz(mask);
        i += 32;
    }
    return i < hn ? find_byte_class_scalar(h + i, hn - i) : NULL;
}
#endif
static const char *find_byte_class(const char *h, size_t hn)
{
#if defined(__x86_64__)
    if (byte_avx) return find_byte_class_avx2(h, hn);
#endif
    return find_byte_class_scalar(h, hn);
}
static void byte_class_build(void)
{
    byte_class = 0;
    byte_avx = 0;
    byte_n = 0;
    if (!mlit_exact || nmlit < 2) return;
    for (size_t i = 0; i < nmlit; i++)
        if (mlits[i].n != 1) return;
    memset(byte_set, 0, sizeof byte_set);
    for (size_t i = 0; i < nmlit; i++) {
        unsigned char b = (unsigned char)mlits[i].p[0];
        if (byte_set[b]) continue;
        byte_set[b] = 1;
        if (byte_n < 16) byte_list[byte_n] = b;
        byte_n++;
    }
    byte_class = 1;
#if defined(__x86_64__)
    if (byte_n <= 16 && __builtin_cpu_supports("avx2")) byte_avx = 1;
#endif
}
#if defined(__x86_64__)
/*
 * Teddy-style multi-literal prefilter.
 *
 * Literals are split into sixteen buckets, with up to eight literals per
 * bucket.  For each literal we fingerprint three bytes (first, middle,
 * last).  Each byte lookup is implemented as two 4-bit vpshufb lookups;
 * the resulting 8-bit literal mask is ANDed across the three fingerprint
 * positions.  This produces candidates for all literals in one AVX2 pass.
 *
 * The filter is deliberately only a candidate generator: memcmp() and
 * line_matches() remain authoritative, so false positives are harmless.
 */
static int teddy_cmp(const void *x, const void *y)
{
    return memcmp(mlits[*(const int *)x].p, mlits[*(const int *)y].p, 3);
}
__attribute__((target("avx2")))
static void teddy_build(void)
{
    memset(&teddy, 0, sizeof teddy);
    teddy_ready = 0;
    if (nmlit == 0 || nmlit > TEDDY_MAX_LITS || nmlit > TEDDY_LIMIT) return;
    teddy_maxn = 0;
    int ord[TEDDY_MAX_LITS];
    for (size_t i = 0; i < nmlit; i++) {
        if (mlits[i].n < 3) return;            /* 3-byte fingerprint needs n>=3 */
        if (mlits[i].n > teddy_maxn) teddy_maxn = mlits[i].n;
        ord[i] = (int)i;
    }
    /* Literals sharing a prefix land in the same bucket, so each bucket's
     * nibble masks stay sparse and false positives stay rare. */
    qsort(ord, nmlit, sizeof ord[0], teddy_cmp);
    unsigned char lo[3][16] = {{0}}, hi[3][16] = {{0}};
    for (size_t r = 0; r < nmlit; r++) {
        int b = (int)(r * TEDDY_BUCKETS / nmlit);
        int i = ord[r];
        teddy.bl[b][teddy.nb[b]++] = i;
        for (int k = 0; k < 3; k++) {
            unsigned char c = (unsigned char)mlits[i].p[k];
            lo[k][c & 15] |= (unsigned char)(1u << b);
            hi[k][c >> 4] |= (unsigned char)(1u << b);
        }
    }
    for (int k = 0; k < 3; k++) {
        teddy.lo[k] = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)lo[k]));
        teddy.hi[k] = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)hi[k]));
    }
    teddy_ready = 1;
}

__attribute__((target("avx2")))
static inline __m256i teddy_lookup(__m256i x, __m256i lo, __m256i hi)
{
    const __m256i nib = _mm256_set1_epi8(0x0f);
    __m256i xl = _mm256_and_si256(x, nib);
    __m256i xh = _mm256_and_si256(_mm256_srli_epi16(x, 4), nib);
    return _mm256_and_si256(_mm256_shuffle_epi8(lo, xl),
                            _mm256_shuffle_epi8(hi, xh));
}

__attribute__((target("avx2")))
static const char *find_mlit_teddy(const char *h, size_t hn)
{
    if (!teddy_ready) return NULL;
    const char *end = h + hn, *p = h;
    const size_t maxn = teddy_maxn;
    const __m256i zero = _mm256_setzero_si256();
    const __m256i lo0 = teddy.lo[0], lo1 = teddy.lo[1], lo2 = teddy.lo[2];
    const __m256i hi0 = teddy.hi[0], hi1 = teddy.hi[1], hi2 = teddy.hi[2];

    while ((size_t)(end - p) >= 32 + maxn - 1) {
        __m256i a0 = teddy_lookup(_mm256_loadu_si256((const __m256i *)(p + 0)), lo0, hi0);
        __m256i a1 = teddy_lookup(_mm256_loadu_si256((const __m256i *)(p + 1)), lo1, hi1);
        __m256i a2 = teddy_lookup(_mm256_loadu_si256((const __m256i *)(p + 2)), lo2, hi2);
        __m256i r = _mm256_and_si256(a0, _mm256_and_si256(a1, a2));
        unsigned cand = ~(unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(r, zero));
        if (cand) {
            unsigned char rb[32];
            _mm256_storeu_si256((__m256i *)rb, r);
            do {
                unsigned j = (unsigned)__builtin_ctz(cand);
                cand &= cand - 1;
                unsigned bm = rb[j];
                while (bm) {
                    unsigned b = (unsigned)__builtin_ctz(bm);
                    bm &= bm - 1;
                    for (int t = 0; t < teddy.nb[b]; t++) {
                        const MLit *m = &mlits[teddy.bl[b][t]];
                        if (!memcmp(p + j, m->p, m->n)) return p + j;
                    }
                }
            } while (cand);
        }
        p += 32;
    }
    const char *best = NULL;
    for (size_t i = 0; i < nmlit; i++) {
        const char *q = find_one_mlit(p, (size_t)(end - p), &mlits[i]);
        if (q && (!best || q < best)) best = q;
    }
    return best;
}

/*
 * Hashed prefix filter for large literal sets (any N, every literal >= 3 bytes).
 * Literals of >= 4 bytes are keyed on their first FOUR bytes (rare in text);
 * literals of exactly 3 bytes get a separate trigram table, probed only when
 * such literals exist. Per input byte: one 4-byte load, a multiply and a bit
 * test in an 8-16 KiB table; N does not change the per-byte cost. Candidates
 * are rejected on the stored 32-bit prefix before any memcmp.
 */
#define MH_BITS_MAX 22
static uint8_t *mh4_bits, *mh3_bits, *mh_pair;
static int32_t *mh4_head, *mh3_head, *mh_next;
static uint32_t *mh_pre;                       /* first 4 bytes (n>=4) or 3 (n==3) */
static int mh_ready, mh_has3, mh_bits, mlit_any_short;
static inline uint32_t mh_hash(uint32_t v) { return (v * 0x9E3779B1u) >> (32 - mh_bits); }
static inline void mh_setbit(uint8_t *b, unsigned k) { b[k >> 3] |= (uint8_t)(1u << (k & 7)); }
static inline int mh_getbit(const uint8_t *b, unsigned k) { return (b[k >> 3] >> (k & 7)) & 1; }
static void mh_build(void)
{
    /* Index only literals of >= 3 bytes. Shorter ones stay on the memchr
     * side path in find_mlit(); they must not disable this filter. Table
     * grows with N (up to 2^22 slots) so a 10k-word set does not saturate
     * the bitset and verify on every other byte. */
    mh_ready = 0; mh_has3 = 0;
    size_t nlong = 0;
    for (size_t i = 0; i < nmlit; i++) if (mlits[i].n >= 3) nlong++;
    if (!nlong) return;
    mh_bits = 16;
    while (mh_bits < MH_BITS_MAX && ((size_t)1 << mh_bits) < nlong * 128) mh_bits++;
    size_t T = (size_t)1 << mh_bits;
    mh4_bits = calloc(T / 8, 1); mh3_bits = calloc(T / 8, 1);
    mh_pair = calloc(65536 / 8, 1);
    mh4_head = malloc(T * sizeof *mh4_head); mh3_head = malloc(T * sizeof *mh3_head);
    mh_next = malloc(nmlit * sizeof *mh_next); mh_pre = malloc(nmlit * sizeof *mh_pre);
    if (!mh4_bits || !mh3_bits || !mh_pair || !mh4_head || !mh3_head || !mh_next || !mh_pre)
        die("out of memory");
    for (size_t k = 0; k < T; k++) { mh4_head[k] = -1; mh3_head[k] = -1; }
    for (size_t i = 0; i < nmlit; i++) {
        if (mlits[i].n < 3) continue;
        const unsigned char *q = (const unsigned char *)mlits[i].p;
        mh_setbit(mh_pair, ((unsigned)q[0] << 8) | q[1]);
        uint32_t v3 = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16;
        if (mlits[i].n >= 4) {
            uint32_t v4 = v3 | (uint32_t)q[3] << 24, hh = mh_hash(v4);
            mh_pre[i] = v4;
            mh_setbit(mh4_bits, hh);
            mh_next[i] = mh4_head[hh]; mh4_head[hh] = (int32_t)i;
        } else {
            uint32_t hh = mh_hash(v3);
            mh_pre[i] = v3; mh_has3 = 1;
            mh_setbit(mh3_bits, hh);
            mh_next[i] = mh3_head[hh]; mh3_head[hh] = (int32_t)i;
        }
    }
    mh_ready = 1;
}
static inline __attribute__((always_inline))
const char *mh_scan(const char *h, size_t hn, int has3)
{
    const unsigned char *p = (const unsigned char *)h;
    size_t i = 0;
    if (hn >= 4) {
        size_t lim = hn - 3;                   /* 4-byte load at i needs i+4 <= hn */
        for (; i < lim; i++) {
            if (!mh_getbit(mh_pair, ((unsigned)p[i] << 8) | p[i + 1])) continue;
            uint32_t v;
            memcpy(&v, p + i, 4);
            uint32_t h4 = mh_hash(v);
            if (mh_getbit(mh4_bits, h4)) {
                for (int32_t k = mh4_head[h4]; k >= 0; k = mh_next[k])
                    if (mh_pre[k] == v && mlits[k].n <= hn - i &&
                        !memcmp(p + i + 4, mlits[k].p + 4, mlits[k].n - 4))
                        return h + i;
            }
            if (has3) {
                uint32_t v3 = v & 0xFFFFFFu, h3 = mh_hash(v3);
                if (mh_getbit(mh3_bits, h3))
                    for (int32_t k = mh3_head[h3]; k >= 0; k = mh_next[k])
                        if (mh_pre[k] == v3 && mlits[k].n == 3) return h + i;
            }
        }
    }
    const char *best = NULL;                   /* tail shorter than a 4-byte window */
    for (size_t k = 0; k < nmlit; k++) {
        if (mlits[k].n < 3) continue;
        const char *q = find_one_mlit(h + i, hn - i, &mlits[k]);
        if (q && (!best || q < best)) best = q;
    }
    return best;
}
static const char *find_mlit_hash(const char *h, size_t hn)
{
    return mh_has3 ? mh_scan(h, hn, 1) : mh_scan(h, hn, 0);
}

/* The few most common literals in a large set. On English text one of these
 * hits on almost every line, so the 10k-pattern hash is only run on the
 * bytes before that hit (often a handful per line). */
static int hot_idx[4], nhot;
__attribute__((target("avx2")))
static const char *find_hot(const char *h, size_t hn)
{
    if (!nhot || hn < 2) return NULL;
    const char *end = h + hn, *p = h;
    size_t maxn = 1, len[4];
    const char *pat[4];
    __m256i va[4], vz[4];
    for (int i = 0; i < nhot; i++) {
        MLit *m = &mlits[hot_idx[i]];
        pat[i] = m->p; len[i] = m->n;
        if (m->n > maxn) maxn = m->n;
        va[i] = _mm256_set1_epi8(m->p[0]);
        vz[i] = _mm256_set1_epi8(m->p[m->n - 1]);
    }
    while ((size_t)(end - p) >= 32 + maxn - 1) {
        unsigned msk[4], any = 0;
        for (int i = 0; i < nhot; i++) {
            __m256i x = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), va[i]);
            __m256i y = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + len[i] - 1)), vz[i]);
            msk[i] = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(x, y));
            any |= msk[i];
        }
        while (any) {
            int b = __builtin_ctz(any);
            any &= any - 1;
            for (int i = 0; i < nhot; i++)
                if (((msk[i] >> b) & 1) && !memcmp(p + b, pat[i], len[i]))
                    return p + b;
        }
        p += 32;
    }
    const char *best = NULL;
    for (int i = 0; i < nhot; i++) {
        const char *q = find_one_mlit(p, (size_t)(end - p), &mlits[hot_idx[i]]);
        if (q && (!best || q < best)) best = q;
    }
    return best;
}
static void select_hot(void)
{
    nhot = 0;
#if defined(__x86_64__)
    if (!__builtin_cpu_supports("avx2") || nmlit < 8) return;
    int bests[4] = {-1, -1, -1, -1}, sc[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < nmlit; i++) {
        if (mlits[i].n < 2 || mlits[i].n > 8) continue;
        int score = 0;
        for (size_t k = 0; k < mlits[i].n; k++) score += commonness((unsigned char)mlits[i].p[k]);
        score /= (int)mlits[i].n;
        if (score < 14) continue;              /* skip id-like tokens; they never win the line */
        for (int j = 0; j < 4; j++) if (score > sc[j]) {
            for (int t = 3; t > j; t--) { sc[t] = sc[t - 1]; bests[t] = bests[t - 1]; }
            sc[j] = score; bests[j] = (int)i;
            break;
        }
    }
    for (int j = 0; j < 4; j++) if (bests[j] >= 0) hot_idx[nhot++] = bests[j];
#else
    (void)0;
#endif
}

/* Small literal sets use the existing lower-overhead first/last-byte filter. */
__attribute__((target("avx2")))
static const char *find_mlit_avx2(const char *h, size_t hn)
{
    const char *end = h + hn, *p = h;
    size_t maxn = 0;
    __m256i va[4], vz[4];
    for (size_t i = 0; i < nmlit; i++) {
        if (mlits[i].n > maxn) maxn = mlits[i].n;
        va[i] = _mm256_set1_epi8(mlits[i].p[0]);
        vz[i] = _mm256_set1_epi8(mlits[i].p[mlits[i].n - 1]);
    }
    while ((size_t)(end - p) >= 32 + maxn - 1) {
        unsigned m[4], any = 0;
        for (size_t i = 0; i < nmlit; i++) {
            __m256i x = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), va[i]);
            __m256i y = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(p + mlits[i].n - 1)), vz[i]);
            m[i] = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(x, y));
            any |= m[i];
        }
        while (any) {
            int b = __builtin_ctz(any);
            any &= any - 1;
            for (size_t i = 0; i < nmlit; i++)
                if (((m[i] >> b) & 1) && !memcmp(p + b, mlits[i].p, mlits[i].n))
                    return p + b;
        }
        p += 32;
    }
    const char *best = NULL;
    for (size_t i = 0; i < nmlit; i++) {
        const char *q = find_one_mlit(p, (size_t)(end - p), &mlits[i]);
        if (q && (!best || q < best)) best = q;
    }
    return best;
}
#endif

/* Per-literal next-occurrence cache: a literal that is absent (or far away)
 * is searched once, not re-scanned to the chunk end on every hit. */
typedef struct { const char *p; int done; } MCache;
/* Literals shorter than 3 bytes are not in the hash table. Their next hit is
 * cached per chunk so a 1-byte word does not rescan the chunk on every line. */
static const char *earliest_short(const char *h, size_t hn, MCache *cache)
{
    const char *best = NULL;
    for (size_t i = 0; i < nmlit; i++) {
        if (mlits[i].n >= 3) continue;
        if (mlits[i].n == 0) return h;
        MCache *c = &cache[i];
        if (c->done && !c->p) continue;
        if (!c->done || c->p < h) {
            c->p = find_one_mlit(h, hn, &mlits[i]);
            c->done = 1;
        }
        if (c->p && (!best || c->p < best)) best = c->p;
    }
    return best;
}
/* Next hit of the hot literals inside one chunk scan. Rare hot words must
 * not be re-scanned from every match back to the chunk end. */
#if defined(__x86_64__)
static __thread const char *hot_base, *hot_limit, *hot_hit;
static __thread int hot_have;
static const char *cached_hot(const char *h, size_t hn)
{
    const char *end = h + hn;
    if (!hot_have || h < hot_base || h >= hot_limit) {
        hot_base = h;
        hot_limit = end;
        hot_hit = find_hot(h, hn);
        hot_have = 1;
    } else if (hot_hit && hot_hit < h) {
        hot_hit = find_hot(h, (size_t)(hot_limit - h));
        hot_base = h;
    }
    return (hot_hit && hot_hit < end) ? hot_hit : NULL;
}
#endif
/*
 * One rare 4-byte anchor per literal. An anchor can begin on any byte, so a
 * miss advances one (or four, when the byte four ahead cannot occur in any
 * anchor). A hit verifies the whole literal at the anchor's offset. The
 * earliest start is not always the earliest anchor, so after a hit the scan
 * continues until every anchor that could belong to an earlier start has
 * been seen.
 */
#define WM_VBITS 18
#define WM_BLOOM_BITS 20
static int32_t *wm_head, *wm_next;
static uint32_t *wm_blk, *wm_off;
static uint8_t *wm_freq, *wm_bloom;
static uint8_t wm_bg[8192], wm_in[256];
static int wm_ready, wm_m, wm_avx;
static size_t wm_max_off;
static inline uint32_t wm_vhash(uint32_t v) { return (v * 0x85EBCA6Bu) >> (32 - WM_VBITS); }
static inline uint32_t wm_bloom_bit(uint32_t v) { return (v * 0x9E3779B1u) >> (32 - WM_BLOOM_BITS); }
static inline int wm_bg_has(unsigned a, unsigned b)
{
    unsigned bit = (a << 8) | b;
    return (wm_bg[bit >> 3] >> (bit & 7)) & 1;
}
static unsigned wm_letter_score(const unsigned char *s)
{
    /* Lower is rarer. Digits and punctuation score 0 so they win ties. */
    static const uint8_t common[26] = {
        12, 14, 10, 12, 18, 10, 10, 12, 12, 2, 8, 12, 10, 12, 12, 10, 1, 12, 14, 16, 8, 2, 8, 1, 6, 1
    };
    unsigned sc = 0;
    for (int k = 0; k < 4; k++) {
        unsigned char c = s[k];
        if (c >= 'a' && c <= 'z') sc += common[c - 'a'];
        else if (c >= 'A' && c <= 'Z') sc += common[c - 'A'];
    }
    return sc;
}
static void wm_sample(void)
{
    if (!nfiles || !files || !files[0] || !strcmp(files[0], "-")) return;
    FILE *fp = fopen(files[0], "rb");
    if (!fp) return;
    char *buf = malloc(1 << 20);
    if (!buf) { fclose(fp); return; }
    size_t n = fread(buf, 1, 1 << 20, fp);
    fclose(fp);
    if (n >= 4) {
        wm_freq = calloc(1 << 20, 1);
        if (wm_freq) {
            for (size_t i = 0; i + 4 <= n; i++) {
                uint32_t v;
                memcpy(&v, buf + i, 4);
                unsigned h = (v * 0x9E3779B1u) >> 12;
                if (wm_freq[h] != 255) wm_freq[h]++;
            }
        }
    }
    free(buf);
}
static void wm_build(void)
{
    wm_ready = 0;
    if (nmlit <= TEDDY_LIMIT) return;
    size_t m = (size_t)-1;
    for (size_t i = 0; i < nmlit; i++) if (mlits[i].n < m) m = mlits[i].n;
    if (m < 8) return;
    wm_sample();
    size_t V = (size_t)1 << WM_VBITS;
    wm_head = malloc(V * sizeof *wm_head);
    wm_next = malloc(nmlit * sizeof *wm_next);
    wm_blk = malloc(nmlit * sizeof *wm_blk);
    wm_off = malloc(nmlit * sizeof *wm_off);
    if (!wm_head || !wm_next || !wm_blk || !wm_off) die("out of memory");
    memset(wm_bg, 0, sizeof wm_bg);
    memset(wm_in, 0, sizeof wm_in);
    wm_bloom = calloc(((size_t)1 << (WM_BLOOM_BITS - 3)) + 16, 1);
    if (!wm_bloom) die("out of memory");
    for (size_t k = 0; k < V; k++) wm_head[k] = -1;
    wm_m = (int)m;
    wm_max_off = 0;
    for (size_t i = 0; i < nmlit; i++) {
        const unsigned char *p = (const unsigned char *)mlits[i].p;
        size_t n = mlits[i].n, best_o = 0, best_s = (size_t)-1;
        for (size_t o = 0; o + 4 <= n; o++) {
            unsigned sc = wm_letter_score(p + o);
            if (wm_freq) {
                uint32_t v;
                memcpy(&v, p + o, 4);
                sc = (unsigned)wm_freq[(v * 0x9E3779B1u) >> 12] * 256u + sc;
            }
            if (sc < best_s) { best_s = sc; best_o = o; }
        }
        uint32_t b;
        memcpy(&b, p + best_o, 4);
        wm_blk[i] = b;
        wm_off[i] = (uint32_t)best_o;
        if (best_o > wm_max_off) wm_max_off = best_o;
        unsigned g0 = ((unsigned)p[best_o] << 8) | p[best_o + 1];
        unsigned g1 = ((unsigned)p[best_o + 2] << 8) | p[best_o + 3];
        wm_bg[g0 >> 3] |= (uint8_t)(1u << (g0 & 7));
        wm_bg[g1 >> 3] |= (uint8_t)(1u << (g1 & 7));
        wm_in[p[best_o]] = wm_in[p[best_o + 1]] = wm_in[p[best_o + 2]] = wm_in[p[best_o + 3]] = 1;
        {
            uint32_t bit = wm_bloom_bit(b);
            wm_bloom[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        }
        uint32_t vh = wm_vhash(b);
        wm_next[i] = wm_head[vh];
        wm_head[vh] = (int32_t)i;
    }
    free(wm_freq);
    wm_freq = NULL;
    wm_ready = 1;
#if defined(__x86_64__)
    wm_avx = __builtin_cpu_supports("avx2");
#endif
}
static void wm_consider(const unsigned char *p, const char *h, size_t hn, size_t i,
                        const char **best, size_t *best_at)
{
    uint32_t v;
    memcpy(&v, p + i, 4);
    for (int32_t k = wm_head[wm_vhash(v)]; k >= 0; k = wm_next[k]) {
        if (wm_blk[k] != v || i < wm_off[k]) continue;
        size_t start = i - wm_off[k];
        size_t n = mlits[k].n;
        if (start + n <= hn && !memcmp(p + start, mlits[k].p, n)) {
            if (!*best || start < *best_at) { *best = h + start; *best_at = start; }
        }
    }
}
static const char *wm_find_scalar(const char *h, size_t hn)
{
    if (hn < (size_t)wm_m) return NULL;
    const unsigned char *p = (const unsigned char *)h;
    size_t i = 0;
    const char *best = NULL;
    size_t best_at = 0;
    while (i + 4 <= hn) {
        if (best && i >= best_at + wm_max_off) return best;
        /* No anchor contains p[i+3], so none can start at i .. i+3. */
        if (!wm_in[p[i + 3]]) { i += 4; continue; }
        if (!wm_bg_has(p[i], p[i + 1]) || !wm_bg_has(p[i + 2], p[i + 3])) { i++; continue; }
        wm_consider(p, h, hn, i, &best, &best_at);
        i++;
    }
    return best;
}
#if defined(__x86_64__)
/* 32 overlapping 4-byte windows, one gather each against the anchor bloom.
 * A clear bit cannot be an anchor. A set bit still takes the exact chain. */
__attribute__((target("avx2"), noinline))
static uint32_t wm_mask32(const unsigned char *p)
{
    uint32_t mask = 0;
    const __m256i mul = _mm256_set1_epi32(0x9E3779B1);
    const __m256i seven = _mm256_set1_epi32(7);
    const __m256i ff = _mm256_set1_epi32(0xff);
    const __m256i one = _mm256_set1_epi32(1);
    const __m128i sh0 = _mm_setr_epi8(0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 3, 4, 5, 6);
    const __m128i sh1 = _mm_setr_epi8(4, 5, 6, 7, 5, 6, 7, 8, 6, 7, 8, 9, 7, 8, 9, 10);
    for (int g = 0; g < 4; g++) {
        __m128i x = _mm_loadu_si128((const __m128i *)(p + g * 8));
        __m256i w = _mm256_set_m128i(_mm_shuffle_epi8(x, sh1), _mm_shuffle_epi8(x, sh0));
        __m256i bit = _mm256_srli_epi32(_mm256_mullo_epi32(w, mul), 32 - WM_BLOOM_BITS);
        __m256i idx = _mm256_srli_epi32(bit, 3);
        __m256i got = _mm256_i32gather_epi32((const int *)wm_bloom, idx, 1);
        __m256i byte = _mm256_and_si256(got, ff);
        __m256i sh = _mm256_and_si256(bit, seven);
        __m256i bitv = _mm256_sllv_epi32(one, sh);
        __m256i hit = _mm256_cmpeq_epi32(_mm256_and_si256(byte, bitv), bitv);
        mask |= (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(hit)) << (g * 8);
    }
    return mask;
}
__attribute__((target("avx2"), noinline))
static const char *wm_find_avx2(const char *h, size_t hn)
{
    if (hn < (size_t)wm_m) return NULL;
    const unsigned char *p = (const unsigned char *)h;
    size_t i = 0;
    const char *best = NULL;
    size_t best_at = 0;
    while (i + 40 <= hn) {
        if (best && i >= best_at + wm_max_off) return best;
        uint32_t m = wm_mask32(p + i);
        if (best) {
            size_t keep = best_at + wm_max_off - i;
            if (keep < 32) m &= (uint32_t)((1ull << keep) - 1);
        }
        while (m) {
            int s = __builtin_ctz(m);
            m &= m - 1;
            if (best && i + (size_t)s >= best_at + wm_max_off) return best;
            wm_consider(p, h, hn, i + (size_t)s, &best, &best_at);
        }
        i += 32;
    }
    while (i + 4 <= hn) {
        if (best && i >= best_at + wm_max_off) return best;
        if (!wm_in[p[i + 3]]) { i += 4; continue; }
        if (!wm_bg_has(p[i], p[i + 1]) || !wm_bg_has(p[i + 2], p[i + 3])) { i++; continue; }
        wm_consider(p, h, hn, i, &best, &best_at);
        i++;
    }
    return best;
}
#endif
static const char *wm_find(const char *h, size_t hn)
{
#if defined(__x86_64__)
    if (wm_avx) return wm_find_avx2(h, hn);
#endif
    return wm_find_scalar(h, hn);
}

static const char *find_mlit(const char *h, size_t hn, MCache *cache)
{
    if (byte_class) return find_byte_class(h, hn);
    if (wm_ready) return wm_find(h, hn);
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        if (nmlit >= 5 && teddy_ready)
            return find_mlit_teddy(h, hn);
        if (nmlit <= 4 && !mh_ready)
            return find_mlit_avx2(h, hn);
    }
#endif
    if (mh_ready) {
        const char *sh = mlit_any_short ? earliest_short(h, hn, cache) : NULL;
        const char *hot = NULL;
#if defined(__x86_64__)
        if (nhot) hot = cached_hot(h, hn);
#endif
        size_t lim = hn;
        if (sh && (size_t)(sh - h) < lim) lim = (size_t)(sh - h);
        if (hot && (size_t)(hot - h) < lim) lim = (size_t)(hot - h);
        const char *lo = lim ? find_mlit_hash(h, lim) : NULL;
        if (hot && (!lo || hot < lo)) lo = hot;
        if (sh && (!lo || sh < lo)) lo = sh;
        return lo;
    }
    const char *best = NULL;
    for (size_t i = 0; i < nmlit; i++) {
        MCache *c = &cache[i];
        if (c->done && !c->p) continue;
        if (!c->done || c->p < h) {
            c->p = find_one_mlit(h, hn, &mlits[i]);
            c->done = 1;
        }
        if (c->p && (!best || c->p < best)) best = c->p;
    }
    return best;
}

static int pure_lit(Node *n, size_t *len)
{
    if (n->type == N_LIT) { *len = 1; return 1; }
    if (n->type != N_CAT || n->nkid == 0) return 0;
    for (int k = 0; k < n->nkid; k++)
        if (n->kid[k]->type != N_LIT) return 0;
    *len = (size_t)n->nkid;
    return 1;
}
static ACNode *ac_node(void)
{
    ac = xrealloc(ac, (size_t)(nac + 1) * sizeof *ac);
    ACNode *s = &ac[nac++];
    memset(s, 0, sizeof *s);
    for (int c = 0; c < 256; c++) s->nxt[c] = -1;
    s->fail = 0;
    return s;
}
static void ac_build(void)
{
    nac = 0;
    ac_node();
    for (size_t i = 0; i < nmlit; i++) {
        int s = 0;
        for (size_t k = 0; k < mlits[i].n; k++) {
            unsigned char b = (unsigned char)mlits[i].p[k];
            if (ac[s].nxt[b] < 0) {
                ac_node();
                ac[s].nxt[b] = nac - 1;
            }
            s = ac[s].nxt[b];
        }
        ac[s].hit = 1;
    }
    int *q = xrealloc(NULL, (size_t)nac * sizeof *q);
    int qh = 0, qt = 0;
    for (int c = 0; c < 256; c++) {
        int t = ac[0].nxt[c];
        if (t < 0) ac[0].nxt[c] = 0;
        else { ac[t].fail = 0; q[qt++] = t; }
    }
    while (qh < qt) {
        int s = q[qh++];
        for (int c = 0; c < 256; c++) {
            int t = ac[s].nxt[c];
            int f = ac[ac[s].fail].nxt[c];
            if (t < 0) ac[s].nxt[c] = f;
            else {
                ac[t].fail = f;
                if (ac[f].hit) ac[t].hit = 1;
                q[qt++] = t;
            }
        }
    }
    free(q);
}
static void scan_ac(Chunk *c, size_t begin, size_t end)
{
    const char *d = c->job->data;
    size_t pos = begin, cnt = 0, cnt_pos = begin, count = 0;
    int s = 0;
    while (pos < end) {
        unsigned char b = (unsigned char)d[pos];
        if (b == '\n') { s = 0; pos++; continue; }
        s = ac[s].nxt[b];
        if (!ac[s].hit) { pos++; continue; }
        size_t ls = pos;
        while (ls > begin && d[ls - 1] != '\n') ls--;
        const char *nl = memchr(d + pos, '\n', end - pos);
        size_t le = nl ? (size_t)(nl - d) : end;
        count++;
        if (qflag) _exit(0);
        if (store_matches) {
            size_t ln = 0;
            if (nflag) { cnt += count_nl(d + cnt_pos, ls - cnt_pos); cnt_pos = ls; ln = cnt + 1; }
            push_match(c, ls, le - ls, ln);
        }
        pos = le < end ? le + 1 : end;
        s = 0;
    }
    if (nflag) {
        cnt += count_nl(d + cnt_pos, end - cnt_pos);
        if (end > begin && d[end - 1] != '\n') cnt++;
        c->nlines = cnt;
    }
    c->count = count;
}

/* Per-hit line boundary helpers: 16-byte SSE2 compares replace a bytewise
 * backward loop and a libc memchr call (call overhead + loop-exit mispredict
 * dominate when lines are short and hits are dense). */
static inline size_t line_start_before(const char *d, size_t pos, size_t hp)
{
    size_t e = hp;                                 /* last '\n' in [pos,hp) */
#if defined(__x86_64__)
    const __m128i nl = _mm_set1_epi8('\n');
    while (e - pos >= 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(d + e - 16)), nl));
        if (m) return e - 16 + (size_t)(31 - __builtin_clz(m)) + 1;
        e -= 16;
    }
#endif
    while (e > pos && d[e - 1] != '\n') e--;
    return e;
}
static inline size_t line_end_from(const char *d, size_t hp, size_t end)
{
    size_t i = hp;                                 /* first '\n' in [hp,end) */
#if defined(__x86_64__)
    const __m128i nl = _mm_set1_epi8('\n');
    while (end - i >= 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(d + i)), nl));
        if (m) return i + (size_t)__builtin_ctz(m);
        i += 16;
    }
#endif
    while (i < end && d[i] != '\n') i++;
    return i;
}

static void scan_multi_literal(Chunk *c, Matcher *M, size_t begin, size_t end)
{
    if (mlit_exact && !exact_filter) { scan_ac(c, begin, end); return; }
    const char *d = c->job->data;
    size_t pos = begin, cnt = 0, cnt_pos = begin, count = 0;
    MCache *cache = calloc(nmlit, sizeof *cache);
    if (!cache) die("out of memory");
    while (pos < end) {
        const char *h = find_mlit(d + pos, end - pos, cache);
        if (!h) break;
        size_t hp = (size_t)(h - d);
        /* -c/-l/-q on an exact literal set only needs the line end. The
         * backward scan is for verify, -n, or stored output. */
        int need_ls = !mlit_exact || store_matches || nflag;
        size_t ls = need_ls ? line_start_before(d, pos, hp) : hp;
        size_t le = line_end_from(d, hp, end);
        if (mlit_exact || line_matches(d + ls, le - ls, M)) {
            count++;
            if (qflag) _exit(0);
            if (store_matches) {
                size_t ln = 0;
                if (nflag) { cnt += count_nl(d + cnt_pos, ls - cnt_pos); cnt_pos = ls; ln = cnt + 1; }
                push_match(c, ls, le - ls, ln);
            }
        }
        pos = le < end ? le + 1 : end;
    }
    if (nflag) {
        cnt += count_nl(d + cnt_pos, end - cnt_pos);
        if (end > begin && d[end - 1] != '\n') cnt++;
        c->nlines = cnt;
    }
    free(cache);
    c->count = count;
}


/* ---- first-byte prefilter ---------------------------------------------------
 * Every match must begin with a byte from F = union of the byte sets reachable
 * from the NFA start states.  For a rare F (digits, a capital, punctuation) most
 * lines contain no such byte, so instead of running the DFA over every byte we
 * skip straight to the next F byte and run the DFA only on that line.  Patterns
 * that can match the empty string (or only at end of line) are excluded. */
static uint8_t fb_tab[256];
static int fb_on;
static uint8_t fb_lo[32] __attribute__((aligned(32))), fb_hi[32] __attribute__((aligned(32)));
static int fb_avx2;

static double fb_prob(int b)
{
    static const double lf[26] = {8.2,1.5,2.8,4.3,12.7,2.2,2.0,6.1,7.0,0.15,0.77,4.0,2.4,
                                  6.7,7.5,1.9,0.1,6.0,6.3,9.1,2.8,1.0,2.4,0.15,2.0,0.07};
    if (b >= 'a' && b <= 'z') return lf[b - 'a'] * 0.0072;
    if (b >= 'A' && b <= 'Z') return lf[b - 'A'] * 0.0010;
    if (b == ' ') return 0.17;
    if (b >= '0' && b <= '9') return 0.002;
    if (b == ',' || b == '.' || b == '\'' || b == '-' || b == '?' || b == '!') return 0.01;
    if (b == '\n') return 0.0;
    return 0.0008;
}
static void fb_setup(void)
{
    uint64_t F[4] = {0, 0, 0, 0};
    int *stk = xrealloc(NULL, ((size_t)nnfa + 1) * sizeof(int));
    unsigned char *seen = calloc((size_t)nnfa + 1, 1);
    if (!seen) die("out of memory");
    int ok = 1;
    for (int k = 0; k < n_all && ok; k++) {
        int sp = 0;
        stk[sp++] = starts_all[k];
        while (sp && ok) {
            int x = stk[--sp];
            if (x < 0 || seen[x]) continue;
            seen[x] = 1;
            NState *q = &nfa[x];
            switch (q->type) {
            case 0: for (int w = 0; w < 4; w++) F[w] |= q->set[w]; break;
            case 1: stk[sp++] = q->out1; stk[sp++] = q->out2; break;
            default: ok = 0;                 /* empty match / end-anchored accept */
            }
        }
    }
    free(stk); free(seen);
    if (!ok) return;
    double dens = 0;
    int cnt = 0;
    for (int b = 0; b < 256; b++) {
        int in = (F[b >> 6] >> (b & 63)) & 1;
        fb_tab[b] = (uint8_t)in;
        if (in && b != '\n') { dens += fb_prob(b); cnt++; }
    }
    if (mb_locale && (F[2] | F[3])) return;  /* keep multibyte handling on the DFA */
    if (cnt == 0 || dens > 0.03) return;
    for (int b = 0; b < 256; b++)
        if (fb_tab[b]) { fb_lo[b & 15] = fb_lo[16 + (b & 15)] = 1; fb_hi[b >> 4] = fb_hi[16 + (b >> 4)] = 1; }
#if defined(__x86_64__)
    fb_avx2 = __builtin_cpu_supports("avx2");
#endif
    fb_on = 1;
}
#if defined(__x86_64__)
__attribute__((target("avx2")))
static const char *fb_find_avx2(const char *h, size_t n)
{
    const __m256i lo = _mm256_load_si256((const __m256i *)fb_lo);
    const __m256i hi = _mm256_load_si256((const __m256i *)fb_hi);
    const __m256i m15 = _mm256_set1_epi8(0x0F), zero = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(h + i));
        __m256i a = _mm256_shuffle_epi8(lo, _mm256_and_si256(v, m15));
        __m256i b = _mm256_shuffle_epi8(hi, _mm256_and_si256(_mm256_srli_epi16(v, 4), m15));
        unsigned m = ~(unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(a, b), zero));
        while (m) {
            unsigned k = (unsigned)__builtin_ctz(m);
            if (fb_tab[(unsigned char)h[i + k]]) return h + i + k;
            m &= m - 1;
        }
    }
    for (; i < n; i++) if (fb_tab[(unsigned char)h[i]]) return h + i;
    return NULL;
}
#endif
static const char *fb_find(const char *h, size_t n)
{
#if defined(__x86_64__)
    if (fb_avx2) return fb_find_avx2(h, n);
#endif
    for (size_t i = 0; i < n; i++) if (fb_tab[(unsigned char)h[i]]) return h + i;
    return NULL;
}
static void scan_firstbyte(Chunk *c, Matcher *M, size_t begin, size_t end)
{
    const char *d = c->job->data;
    size_t pos = begin, cnt = 0, cnt_pos = begin, count = 0;
    while (pos < end) {
        const char *h = fb_find(d + pos, end - pos);
        if (!h) break;
        size_t hp = (size_t)(h - d);
        size_t ls = line_start_before(d, pos, hp);
        size_t le = line_end_from(d, hp, end);
        if (line_matches(d + ls, le - ls, M)) {
            count++;
            if (qflag) _exit(0);
            if (store_matches) {
                size_t ln = 0;
                if (nflag) { cnt += count_nl(d + cnt_pos, ls - cnt_pos); cnt_pos = ls; ln = cnt + 1; }
                push_match(c, ls, le - ls, ln);
            }
        }
        pos = le < end ? le + 1 : end;
    }
    if (nflag) {
        cnt += count_nl(d + cnt_pos, end - cnt_pos);
        if (end > begin && d[end - 1] != '\n') cnt++;
        c->nlines = cnt;
    }
    c->count = count;
}

/* A required literal is worth prefiltering on when it is long, or short but made of
 * rare bytes (e.g. the '@' in \w+@\w+).  Hits are always verified by the DFA. */
static int lit_worth(const char *p, size_t n)
{
    if (n >= 3) return 1;
    double q = 1;
    for (size_t k = 0; k < n; k++) q *= fb_prob((unsigned char)p[k]);
    return n >= 1 && q < 0.004;
}

/* ---- interleaved streaming DFA --------------------------------------------
 * The DFA's cost is the load-to-load latency of one state transition per byte.
 * Lines are independent, so four sub-ranges of the chunk are advanced in lock
 * step and the latencies overlap.  Newlines, accepting states and dead states
 * leave the fast loop through TERM.  Matching lines are recorded per stream
 * (the sub-ranges are consecutive, so concatenation is already in file order);
 * -c needs only the count. */
static int stream_ok;
typedef struct { size_t p, end, ls; uint16_t s; } Strm;
typedef struct { size_t *v, n, cap; } MList;       /* pairs: line start, line end */
static inline void ml_add(MList *l, size_t ls, size_t le)
{
    if (l->n + 2 > l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = xrealloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = ls; l->v[l->n++] = le;
}
typedef struct { size_t count; int coll; } SCtx;
static inline void s_hit(SCtx *cx, MList *l, size_t ls, size_t le)
{
    cx->count++;
    if (qflag) _exit(0);
    if (cx->coll) ml_add(l, ls, le);
}
static size_t stream_scalar(const char *d, Matcher *M, Strm *x, int *thrash, SCtx *cx, MList *l)
{
    const uint16_t init = M->init;
    if (*thrash) {                                  /* per-line fallback from the last line start */
        size_t pos = x->ls;
        while (pos < x->end) {
            size_t le = line_end_from(d, pos, x->end);
            if (dfa_match(M, (const unsigned char *)d + pos, le - pos)) s_hit(cx, l, pos, le);
            pos = le < x->end ? le + 1 : x->end;
        }
        return 0;
    }
    uint16_t s = x->s;
    size_t p = x->p, ls = x->ls;
    int inline_ = p > ls;                           /* current line has consumed bytes */
    while (p < x->end) {
        unsigned char b = (unsigned char)d[p++];
        uint16_t t = M->sflat[((size_t)s << 8) | b];
        if (!(t & TERM)) { s = t; inline_ = 1; continue; }
        if (t == NLV) {
            if (M->ds[s].endacc) s_hit(cx, l, ls, p - 1);
            s = init; inline_ = 0; ls = p; continue;
        }
        if (t == UNK) {
            t = compute(M, s, b);
            if (!(t & TERM)) { s = t; inline_ = 1; continue; }
        }
        size_t le = line_end_from(d, p, x->end);
        if (M->ds[t & ~TERM].accept) s_hit(cx, l, ls, le);
        p = le < x->end ? le + 1 : x->end;
        s = init; inline_ = 0; ls = p;
    }
    /* last line without a trailing newline */
    if (inline_ && x->end > 0 && d[x->end - 1] != '\n' && M->ds[s].endacc) s_hit(cx, l, ls, x->end);
    return 0;
}
static void stream_range(Chunk *c, Matcher *M, size_t begin, size_t end)
{
    const char *d = c->job->data;
    const uint16_t init = M->init;
    SCtx cx = {0, store_matches};
    MList ml[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    Strm st[4];
    int K = 4;
    if (end - begin < (size_t)(1 << 16)) K = 1;
    size_t cut[5];
    cut[0] = begin; cut[K] = end;
    for (int k = 1; k < K; k++) {
        size_t guess = begin + (end - begin) * (size_t)k / (size_t)K;
        cut[k] = line_start_at_or_after(d, end, guess);
        if (cut[k] < cut[k - 1]) cut[k] = cut[k - 1];
    }
    for (int k = 0; k < K; k++) { st[k].p = st[k].ls = cut[k]; st[k].end = cut[k + 1]; st[k].s = init; }
    const uint16_t *sf = M->sflat;
    uint32_t resets = 0;
    int thrash = 0;
    while (K == 4 && !thrash) {
        size_t n = (size_t)-1;
        for (int k = 0; k < 4; k++) { size_t r = st[k].end - st[k].p; if (r < n) n = r; }
        if (n == 0) break;
        uint32_t s0 = st[0].s, s1 = st[1].s, s2 = st[2].s, s3 = st[3].s;
        const unsigned char *p0 = (const unsigned char *)d + st[0].p, *p1 = (const unsigned char *)d + st[1].p,
                            *p2 = (const unsigned char *)d + st[2].p, *p3 = (const unsigned char *)d + st[3].p;
        uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
        size_t i = 0;
        for (; i < n; i++) {
            t0 = sf[(s0 << 8) | p0[i]]; t1 = sf[(s1 << 8) | p1[i]];
            t2 = sf[(s2 << 8) | p2[i]]; t3 = sf[(s3 << 8) | p3[i]];
            if ((t0 | t1 | t2 | t3) & TERM) { i++; break; }
            s0 = t0; s1 = t1; s2 = t2; s3 = t3;
        }
        st[0].p += i; st[1].p += i; st[2].p += i; st[3].p += i;
        if (!((t0 | t1 | t2 | t3) & TERM)) { st[0].s = (uint16_t)s0; st[1].s = (uint16_t)s1; st[2].s = (uint16_t)s2; st[3].s = (uint16_t)s3; continue; }
        uint32_t ss[4] = {s0, s1, s2, s3}, tt[4] = {t0, t1, t2, t3};
        int voided[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            if (voided[k]) continue;
            Strm *x = &st[k];
            uint32_t t = tt[k];
            x->s = (uint16_t)ss[k];
            if (!(t & TERM)) { x->s = (uint16_t)t; continue; }
            if (t == NLV) {
                if (M->ds[x->s].endacc) s_hit(&cx, &ml[k], x->ls, x->p - 1);
                x->s = init; x->ls = x->p; continue;
            }
            if (t == UNK) {
                unsigned char b = d[x->p - 1];
                uint32_t ep = M->epoch;
                uint16_t r = compute(M, x->s, b);
                if (ep != M->epoch) {                       /* cache was reset: other ids are stale */
                    resets++;
                    for (int j = 0; j < 4; j++) if (j != k) { st[j].p = st[j].ls; st[j].s = init; voided[j] = 1; }
                    if (resets > 8) thrash = 1;
                    sf = M->sflat;
                }
                t = r;
                if (!(t & TERM)) { x->s = (uint16_t)t; continue; }
            }
            /* accepting or dead: finish the line */
            size_t le = line_end_from(d, x->p, x->end);
            if (M->ds[t & ~TERM].accept) s_hit(&cx, &ml[k], x->ls, le);
            x->p = le < x->end ? le + 1 : x->end;
            x->ls = x->p; x->s = init;
        }
    }
    for (int k = 0; k < K; k++) stream_scalar(d, M, &st[k], &thrash, &cx, &ml[k]);

    /* ---- results in file order ---- */
    size_t matched = cx.count, cnt = 0, cnt_pos = begin;
    if (!vflag) {
        c->count = matched;
        if (store_matches) {
            for (int k = 0; k < K; k++)
                for (size_t i = 0; i < ml[k].n; i += 2) {
                    size_t ls = ml[k].v[i], le = ml[k].v[i + 1], ln = 0;
                    if (nflag) { cnt += count_nl(d + cnt_pos, ls - cnt_pos); cnt_pos = ls; ln = cnt + 1; }
                    push_match(c, ls, le - ls, ln);
                }
        }
    } else {
        size_t nlines = count_nl(d + begin, end - begin) + ((end > begin && d[end - 1] != '\n') ? 1 : 0);
        c->count = nlines - matched;
        if (store_matches) {
            int k = 0;
            size_t i = 0, pos = begin, ln = 0;
            while (pos < end) {
                size_t le = line_end_from(d, pos, end);
                ln++;
                while (k < K && i >= ml[k].n) { k++; i = 0; }
                if (k < K && ml[k].v[i] == pos) { i += 2; }
                else push_match(c, pos, le - pos, ln);
                pos = le < end ? le + 1 : end;
            }
        }
    }
    if (vflag) c->nlines = count_nl(d + begin, end - begin) + ((end > begin && d[end - 1] != '\n') ? 1 : 0);
    else if (nflag) c->nlines = cnt + count_nl(d + cnt_pos, end - cnt_pos) + ((end > begin && d[end - 1] != '\n') ? 1 : 0);
    for (int k = 0; k < 4; k++) free(ml[k].v);
}

static void run_chunk(Chunk *c, Matcher *M)
{
    FileJob *j = c->job;
    const char *d = j->data;
    if (atomic_load_explicit(&j->cancel, memory_order_relaxed)) return;
    size_t lo = c->idx * CHUNK_SIZE, hi = lo + CHUNK_SIZE;
    size_t begin = line_start_at_or_after(d, j->size, lo);
    size_t end = hi >= j->size ? j->size : line_start_at_or_after(d, j->size, hi);

    if (nmlit > 1) { scan_multi_literal(c, M, begin, end); return; }
    if (lit) { scan_literal(c, M, begin, end); return; }
    if (fb_on) { scan_firstbyte(c, M, begin, end); return; }
    if (stream_ok && !M->ds[M->init].accept && !M->ds[M->init].dead) { stream_range(c, M, begin, end); return; }

    size_t pos = begin, ln = 0, count = 0;
    while (pos < end) {
        const char *nl = memchr(d + pos, '\n', end - pos);
        size_t len = nl ? (size_t)(nl - (d + pos)) : end - pos;
        size_t next = nl ? pos + len + 1 : end;
        ln++;
        int m = line_matches(d + pos, len, M);
        if (vflag) m = !m;
        if (m) {
            count++;
            if (qflag) _exit(0);
            if (store_matches) push_match(c, pos, len, ln);
        }
        pos = next;
    }
    c->nlines = ln;
    c->count = count;
}

static void matcher_init(Matcher *M)
{
    memset(M, 0, sizeof *M);
    if (engine == ENG_REGEX) M->re = compile_set();
    if (engine != ENG_DFA) return;
    M->ds = calloc(MAXD, sizeof *M->ds);
    M->sflat = xrealloc(NULL, (size_t)MAXD * 256 * sizeof *M->sflat);
    M->tab = xrealloc(NULL, TABSZ * sizeof *M->tab);
    M->mark = calloc((size_t)nnfa + 1, sizeof *M->mark);
    M->eqmark = calloc((size_t)nnfa + 1, sizeof *M->eqmark);
    M->stk = xrealloc(NULL, ((size_t)nnfa + 1) * sizeof(int));
    M->tmp = xrealloc(NULL, ((size_t)nnfa + 1) * sizeof(int));
    M->tmp2 = xrealloc(NULL, ((size_t)nnfa + 1) * sizeof(int));
    if (!M->ds || !M->mark || !M->eqmark) die("out of memory");
    memset(M->tab, 0xFF, TABSZ * sizeof *M->tab);
    make_init(M);
    build_restart(M);
}
static void matcher_free(Matcher *M)
{
    free_set(M->re);
    for (int i = 0; i < M->nds; i++) free(M->ds[i].ids);
    free(M->sflat); free(M->ds); free(M->tab); free(M->mark); free(M->eqmark); free(M->stk); free(M->tmp); free(M->tmp2);
    free(M->restart);
    free(M->buf); free(M->fbuf);
}

/* ================= engine selection ================= */


/* A pattern is a literal when every byte is matched as itself. -F always is.
 * ERE/BRE: reject unescaped metacharacters. Used so a -f of plain words never
 * builds an NFA (and never falls off MAX_NFA into libc regex). */
static int pattern_as_literal(const char *s, size_t n, char **out, size_t *on)
{
    char *o = xrealloc(NULL, n + 1);
    size_t j = 0;
    if (Fflag) {
        memcpy(o, s, n);
        o[n] = 0; *out = o; *on = n; return 1;
    }
    const char *meta = Eflag ? ".[]*^$+?(){}|" : ".[]\\*^$";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 >= n) { free(o); return 0; }
            unsigned char d = (unsigned char)s[++i];
            if (!Eflag && d != '\\' && !strchr(".[]*^$", d)) { free(o); return 0; }
            /* ERE: \b \B \< \> \w \W \s \S \1..\9 are operators, not literals */
            if (Eflag && (isalnum(d) || d == '`' || d == '\'' || d == '<' || d == '>' || d >= 0x80)) { free(o); return 0; }
            o[j++] = (char)d;
            continue;
        }
        if (strchr(meta, c)) { free(o); return 0; }
        o[j++] = (char)c;
    }
    o[j] = 0; *out = o; *on = j; return 1;
}
static int lit_ord_cmp(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    if (mlits[ia].n != mlits[ib].n) return mlits[ia].n < mlits[ib].n ? -1 : 1;
    return memcmp(mlits[ia].p, mlits[ib].p, mlits[ia].n);
}
/* For an unanchored line match, a shorter literal that occurs inside a longer
 * one makes the longer one redundant. Drops duplicates too.
 * Kept literals are hashed per length, so this is O(total pattern bytes)
 * rather than one memcmp of every pair. Survivors stay in input order. */
typedef struct { uint64_t h; uint32_t idx; } LitSlot;
typedef struct { size_t len, mask, used; LitSlot *tab; } LitGroup;

static uint64_t lit_fnv(const char *s, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}
static int lit_in_group(const LitGroup *g, const char *s, size_t n, uint64_t h)
{
    size_t i = (size_t)h & g->mask;
    while (g->tab[i].idx) {
        if (g->tab[i].h == h) {
            const MLit *m = &mlits[g->tab[i].idx - 1];
            if (m->n == n && !memcmp(m->p, s, n)) return 1;
        }
        i = (i + 1) & g->mask;
    }
    return 0;
}
static void lit_group_add(LitGroup *g, size_t idx)
{
    if (g->used * 2 >= g->mask) {
        size_t nmask = g->mask ? (g->mask * 2 + 1) : 15;
        LitSlot *nt = calloc(nmask + 1, sizeof *nt);
        if (!nt) die("out of memory");
        if (g->tab) {
            for (size_t i = 0; i <= g->mask; i++) {
                if (!g->tab[i].idx) continue;
                size_t j = (size_t)g->tab[i].h & nmask;
                while (nt[j].idx) j = (j + 1) & nmask;
                nt[j] = g->tab[i];
            }
            free(g->tab);
        }
        g->tab = nt;
        g->mask = nmask;
    }
    uint64_t h = lit_fnv(mlits[idx].p, mlits[idx].n);
    size_t i = (size_t)h & g->mask;
    while (g->tab[i].idx) i = (i + 1) & g->mask;
    g->tab[i].h = h;
    g->tab[i].idx = (uint32_t)idx + 1;
    g->used++;
}
static void prune_subsumed(void)
{
    if (nmlit < 2 || oflag || color_on) return;
    size_t *ord = xrealloc(NULL, nmlit * sizeof *ord);
    for (size_t i = 0; i < nmlit; i++) ord[i] = i;
    qsort(ord, nmlit, sizeof *ord, lit_ord_cmp);
    unsigned char *dead = calloc(nmlit, 1);
    if (!dead) die("out of memory");
    LitGroup *gs = NULL;
    size_t ng = 0;
    for (size_t r = 0; r < nmlit; r++) {
        size_t i = ord[r];
        size_t n = mlits[i].n;
        int drop = 0;
        for (size_t g = 0; g < ng && !drop; g++) {
            size_t s = gs[g].len;
            if (s > n) continue;
            if (s == n) {
                uint64_t h = lit_fnv(mlits[i].p, n);
                if (lit_in_group(&gs[g], mlits[i].p, n, h)) drop = 1;
                continue;
            }
            if (s == 0) { drop = 1; continue; }
            for (size_t off = 0; off + s <= n && !drop; off++) {
                uint64_t h = lit_fnv(mlits[i].p + off, s);
                if (lit_in_group(&gs[g], mlits[i].p + off, s, h)) drop = 1;
            }
        }
        if (drop) {
            free(mlits[i].p);
            dead[i] = 1;
            continue;
        }
        gs = xrealloc(gs, (ng + 1) * sizeof *gs);
        int found = 0;
        for (size_t g = 0; g < ng; g++) if (gs[g].len == n) { found = 1; lit_group_add(&gs[g], i); }
        if (!found) {
            memset(&gs[ng], 0, sizeof gs[ng]);
            gs[ng].len = n;
            lit_group_add(&gs[ng], i);
            ng++;
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < nmlit; i++) if (!dead[i]) mlits[w++] = mlits[i];
    nmlit = w;
    for (size_t g = 0; g < ng; g++) free(gs[g].tab);
    free(gs);
    free(ord);
    free(dead);
}
static int collect_literal_set(void)
{
    if (!npat) return 0;
    MLit *m = NULL;
    size_t nm = 0;
    for (size_t i = 0; i < npat; i++) {
        char *litp = NULL; size_t ln = 0;
        if (!pattern_as_literal(fx[i], fxlen[i], &litp, &ln)) {
            for (size_t k = 0; k < nm; k++) free(m[k].p);
            free(m);
            return 0;
        }
        m = xrealloc(m, (nm + 1) * sizeof *m);
        m[nm].p = litp; m[nm].n = ln; m[nm].r = 0;
        if (ln) {
            for (size_t k = 1; k < ln; k++)
                if (commonness((unsigned char)litp[k]) < commonness((unsigned char)litp[m[nm].r]))
                    m[nm].r = k;
        }
        nm++;
    }
    mlits = m; nmlit = nm; mlit_exact = 1;
    prune_subsumed();
    return 1;
}

static void free_node(Node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkid; i++) free_node(n->kid[i]);
    free(n->kid);
    free(n);
}
static void reset_branches(void)
{
    for (int i = 0; i < nbr; i++) free_node(br[i].n);
    free(br);
    br = NULL;
    nbr = 0;
}
/* One ERE/BRE per pattern, every top-level alternative a plain literal.
 * Copies those literals and discards the AST so try_build_dfa is not asked
 * to build an NFA that cannot fit in MAX_NFA. Anchored or empty branches
 * stay on the DFA: the hash filter is unanchored. */
static int collect_pure_alternation(void)
{
    for (size_t i = 0; i < npat; i++) {
        if (!parse_pattern(fx[i], fxlen[i])) {
            reset_branches();
            return 0;
        }
    }
    if (nbr < 2) { reset_branches(); return 0; }
    MLit *m = NULL;
    size_t nm = 0;
    for (int i = 0; i < nbr; i++) {
        size_t L = 0;
        if (br[i].bol || br[i].eol || !pure_lit(br[i].n, &L) || L == 0) {
            for (size_t k = 0; k < nm; k++) free(m[k].p);
            free(m);
            reset_branches();
            return 0;
        }
        char *best = NULL;
        size_t bl = 0;
        req_lit(br[i].n, &best, &bl);
        m = xrealloc(m, (nm + 1) * sizeof *m);
        m[nm].p = best;
        m[nm].n = bl;
        m[nm].r = 0;
        if (bl) {
            for (size_t k = 1; k < bl; k++)
                if (commonness((unsigned char)best[k]) < commonness((unsigned char)best[m[nm].r]))
                    m[nm].r = k;
        }
        nm++;
    }
    reset_branches();
    mlits = m;
    nmlit = nm;
    mlit_exact = 1;
    prune_subsumed();
    return nmlit > 0;
}

static int try_build_dfa(void)
{
    mb_locale = MB_CUR_MAX > 1;
    if (mb_locale && strcmp(nl_langinfo(CODESET), "UTF-8")) return 0;
    for (size_t i = 0; i < npat; i++) {
        if (Fflag) { if (!literal_branch(fx[i], fxlen[i])) return 0; }
        else if (!parse_pattern(fx[i], fxlen[i])) return 0;
    }
    if (nbr >= 2) {         /* huge pure-literal alternations: literal path, NFA barely used */
        int pure = 1;
        for (int i = 0; i < nbr && pure; i++) {
            size_t L;
            if (br[i].bol || br[i].eol || xflag || !pure_lit(br[i].n, &L)) pure = 0;
        }
        if (pure) nfa_limit = 2000000;
    }
    int match = nstate(2, -1, -1, NULL), eolm = nstate(3, -1, -1, NULL);
    starts_all = xrealloc(NULL, (size_t)nbr * sizeof(int));
    starts_float = xrealloc(NULL, (size_t)nbr * sizeof(int));
    for (int i = 0; i < nbr; i++) {
        if (xflag) br[i].bol = br[i].eol = 1;
        int st = build(br[i].n, br[i].eol ? eolm : match);
        starts_all[n_all++] = st;
        if (!br[i].bol) starts_float[n_float++] = st;
    }
    return nnfa <= nfa_limit;
}


/* ---- -w as a DFA --------------------------------------------------------------
 * -w P  ==  (^|\W) P (\W|$)  with P bounded on both sides, exactly what the libc
 * span regex checks.  Instead of running the DFA for "contains P" and then libc
 * regex on every candidate line, compile the bounded form into four floating /
 * anchored branches so the DFA decides alone.  Patterns that can match the empty
 * string keep the old path (GNU has special rules there). */
static int nullable_node(const Node *n)
{
    switch (n->type) {
    case N_EMPTY: return 1;
    case N_LIT: case N_SET: return 0;
    case N_CAT: for (int k = 0; k < n->nkid; k++) if (!nullable_node(n->kid[k])) return 0; return 1;
    case N_ALT: for (int k = 0; k < n->nkid; k++) if (nullable_node(n->kid[k])) return 1; return 0;
    case N_PLUS: return nullable_node(n->kid[0]);
    default: return 1;                                  /* STAR, QUES */
    }
}
static Node *nonword_node(void)
{
    Node *x = nnew(N_SET);
    for (int ch = 0; ch < 256; ch++) {
        if (ch >= 0x80 && mb_locale) continue;          /* lines with such bytes go to libc */
        if (!(isalnum(ch) && ch < 128) && ch != '_') setbit(x->set, ch);
    }
    return x;
}
static void wrap_w_dfa(void)
{
    if (!wflag || xflag || wset_on || engine != ENG_DFA || nbr == 0) return;
    for (int i = 0; i < nbr; i++) if (nullable_node(br[i].n)) return;
    Branch *nb = NULL;
    int nn = 0;
    for (int i = 0; i < nbr; i++) {
        for (int v = 0; v < 4; v++) {
            int left_nw = v & 1, right_nw = (v >> 1) & 1;       /* 1: consume a non-word byte */
            if (left_nw && br[i].bol) continue;
            if (right_nw && br[i].eol) continue;
            Node *cat = nnew(N_CAT);
            if (left_nw) kid_add(cat, nonword_node());
            kid_add(cat, br[i].n);
            if (right_nw) kid_add(cat, nonword_node());
            nb = xrealloc(nb, (size_t)(nn + 1) * sizeof *nb);
            nb[nn++] = (Branch){cat, left_nw ? 0 : 1, right_nw ? 0 : 1};
        }
    }
    if (!nn) return;
    br = nb; nbr = nn;
    nnfa = 0; n_all = n_float = 0;
    int match = nstate(2, -1, -1, NULL), eolm = nstate(3, -1, -1, NULL);
    starts_all = xrealloc(starts_all, (size_t)nbr * sizeof(int));
    starts_float = xrealloc(starts_float, (size_t)nbr * sizeof(int));
    for (int i = 0; i < nbr; i++) {
        int st = build(br[i].n, br[i].eol ? eolm : match);
        starts_all[n_all++] = st;
        if (!br[i].bol) starts_float[n_float++] = st;
    }
    wdfa_on = nnfa <= nfa_limit;
}

/* ================= output formatting ================= */

typedef struct { char *p; size_t n, cap; } Buf;
static void buf_put(Buf *b, const char *s, size_t k)
{
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 256; b->p = xrealloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, k);
    b->n += k;
}
static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static size_t u2s(char *t, size_t v)                /* t has 24 bytes; returns start */
{
    size_t i = 24;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    return i;
}

/* GNU-compatible SGR sequences; GREP_COLORS keys mt/ms/mc/fn/ln/bn/se/ne are honoured */
static const char *col_ms = "01;31", *col_mc = "01;31", *col_fn = "35", *col_ln = "32",
                  *col_bn = "32", *col_se = "36";
static int col_ne;
static void color_init(void)
{
    const char *e = getenv("GREP_COLORS");
    if (!e) return;
    char *s = strdup(e), *save = NULL;
    if (!s) return;
    for (char *t = strtok_r(s, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) { if (!strcmp(t, "ne")) col_ne = 1; continue; }
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(t, "mt")) col_ms = col_mc = v;
        else if (!strcmp(t, "ms")) col_ms = v;
        else if (!strcmp(t, "mc")) col_mc = v;
        else if (!strcmp(t, "fn")) col_fn = v;
        else if (!strcmp(t, "ln")) col_ln = v;
        else if (!strcmp(t, "bn")) col_bn = v;
        else if (!strcmp(t, "se")) col_se = v;
    }                                               /* s is kept: the pointers live in it */
}
static void sgr_on(Buf *o, const char *c)
{
    buf_put(o, "\33[", 2); buf_puts(o, c); buf_put(o, "m", 1);
    if (!col_ne) buf_put(o, "\33[K", 3);
}
static void sgr_off(Buf *o) { buf_put(o, "\33[m", 3); if (!col_ne) buf_put(o, "\33[K", 3); }
static void put_col(Buf *o, const char *c, const char *s, size_t n)
{
    if (color_on && c && *c) { sgr_on(o, c); buf_put(o, s, n); sgr_off(o); }
    else buf_put(o, s, n);
}
static void put_sep(Buf *o, char sep) { put_col(o, col_se, &sep, 1); }
static void put_groupsep(Buf *o) { put_col(o, col_se, "--", 2); buf_put(o, "\n", 1); }

/* Per-file emission state.  Matches are stored as (offset, length, line) and the
 * whole file is addressable, so context lines are found by walking the data. */
typedef struct {
    Buf *o;
    const char *name, *data;
    size_t size;
    size_t nsel;                  /* selected lines emitted (-m) */
    int have_last;
    size_t last_end, last_ln;     /* just past the last printed line; its number */
    size_t after_left;            /* trailing context still owed */
    int limit;                    /* -m reached */
    int out_any;
    int cross_sep;                /* put "--" before this file's first line */
} Emit;

static int ctx_on(void) { return (ctxA > 0 || ctxB > 0) && !oflag; }
static size_t next_char(const char *l, size_t len, size_t s)
{
    size_t np = s + 1;
    if (mb_locale) while (np < len && ((unsigned char)l[np] & 0xC0) == 0x80) np++;
    return np;
}
static void emit_prefix(Emit *E, size_t boff, size_t ln, char sep)
{
    Buf *o = E->o;
    char t[24];
    if (!E->out_any) {
        E->out_any = 1;
        if (E->cross_sep) put_groupsep(o);
    }
    if (want_name) {
        put_col(o, col_fn, E->name, strlen(E->name));
        if (nullflag) buf_put(o, "", 1); else put_sep(o, sep);
    }
    if (nflag) { size_t i = u2s(t, ln); put_col(o, col_ln, t + i, 24 - i); put_sep(o, sep); }
    if (bflag) { size_t i = u2s(t, boff); put_col(o, col_bn, t + i, 24 - i); put_sep(o, sep); }
}
/* After an empty match, where may the next search start?  With -w the wrapper
 * consumed one lead character, so the next word's lead may be the character at
 * the empty match itself. */
static size_t after_empty(const char *l, size_t len, size_t pos, size_t ms)
{
    if (wflag && !xflag) return ms > pos ? ms : next_char(l, len, pos);
    return next_char(l, len, ms);
}
static void emit_content(Emit *E, size_t off, size_t len, int hl)
{
    const char *l = E->data + off;
    if (!(color_on && hl)) { buf_put(E->o, l, len); return; }
    const char *mc = vflag ? col_mc : col_ms;
    size_t pos = 0, ms, me;
    while (pos <= len && span_next(l, len, pos, &ms, &me)) {
        if (me == ms) {
            if (ms >= len) break;
            size_t np = after_empty(l, len, pos, ms);
            buf_put(E->o, l + pos, np - pos);
            pos = np;
            continue;
        }
        buf_put(E->o, l + pos, ms - pos);
        put_col(E->o, mc, l + ms, me - ms);
        pos = me;
    }
    if (pos < len) buf_put(E->o, l + pos, len - pos);
}
static void emit_ctx_line(Emit *E, size_t off, size_t len, size_t ln)
{
    emit_prefix(E, off, ln, '-');
    emit_content(E, off, len, vflag);               /* with -v, context lines are the matching ones */
    buf_put(E->o, "\n", 1);
}
static void emit_only(Emit *E, size_t off, size_t len, size_t ln)
{
    const char *l = E->data + off;
    size_t pos = 0, ms, me;
    while (pos <= len && span_next(l, len, pos, &ms, &me)) {
        if (me == ms) {
            if (ms >= len) break;
            pos = after_empty(l, len, pos, ms);
            continue;
        }
        emit_prefix(E, off + ms, ln, ':');
        put_col(E->o, col_ms, l + ms, me - ms);
        buf_put(E->o, "\n", 1);
        pos = me;
    }
}
static void emit_selected(Emit *E, size_t off, size_t len, size_t ln)
{
    if (E->limit) return;
    const char *d = E->data;
    if (ctx_on()) {
        size_t A = ctxA > 0 ? (size_t)ctxA : 0, B = ctxB > 0 ? (size_t)ctxB : 0;
        size_t from = E->have_last ? E->last_end : 0;
        while (E->after_left > 0 && from < off) {   /* trailing context of the previous hit */
            size_t le = line_end_from(d, from, E->size);
            emit_ctx_line(E, from, le - from, ++E->last_ln);
            from = le < E->size ? le + 1 : E->size;
            E->last_end = from;
            E->after_left--;
        }
        size_t bs = off, nb = 0;                    /* leading context, not before `from` */
        while (nb < B && bs > from) { bs = line_start_before(d, from, bs - 1); nb++; }
        if (E->have_last && from < bs) { if (!E->out_any) E->out_any = 1; put_groupsep(E->o); }
        for (size_t p = bs, k = 0; p < off; k++) {
            size_t le = line_end_from(d, p, off);
            emit_ctx_line(E, p, le - p, ln - nb + k);
            p = le + 1;
        }
        emit_prefix(E, off, ln, ':');
        emit_content(E, off, len, !vflag);
        buf_put(E->o, "\n", 1);
        E->have_last = 1;
        E->last_end = off + len + 1 < E->size ? off + len + 1 : E->size;
        E->last_ln = ln;
        E->after_left = A;
    } else if (oflag) {
        if (!vflag) emit_only(E, off, len, ln);
    } else {
        emit_prefix(E, off, ln, ':');
        emit_content(E, off, len, !vflag);
        buf_put(E->o, "\n", 1);
    }
    E->nsel++;
    if (maxcount >= 0 && E->nsel >= (size_t)maxcount) E->limit = 1;
}
static void emit_finish(Emit *E)                    /* trailing context after the last hit */
{
    if (!ctx_on() || !E->have_last) return;
    size_t from = E->last_end;
    while (E->after_left > 0 && from < E->size) {
        size_t le = line_end_from(E->data, from, E->size);
        emit_ctx_line(E, from, le - from, ++E->last_ln);
        from = le < E->size ? le + 1 : E->size;
        E->after_left--;
    }
}
/* Plain output (no context, -o, colour or -m): format straight into the buffer.
 * This is the printer thread's hot loop when most of a file is printed. */
static inline char *put_num(char *w, size_t v)
{
    char t[24];
    size_t i = 24;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    memcpy(w, t + i, 24 - i);
    return w + (24 - i);
}
static void emit_chunk(Emit *E, const Chunk *c, size_t base)
{
    size_t m = 0;
    if (ctx_on() || oflag || color_on || maxcount >= 0) {
        for (; m < c->nm && !E->limit; m++)
            emit_selected(E, c->m[m].off, c->m[m].len, nflag ? base + c->m[m].lineno : 0);
        return;
    }
    if (c->nm && !E->out_any) {                       /* first line: group separator bookkeeping */
        emit_selected(E, c->m[0].off, c->m[0].len, nflag ? base + c->m[0].lineno : 0);
        m = 1;
    }
    Buf *o = E->o;
    const size_t nlen = want_name ? strlen(E->name) : 0;
    const char *data = E->data;
    for (size_t k = m; k < c->nm; k++) {
        size_t off = c->m[k].off, len = c->m[k].len;
        size_t need = nlen + 80 + len;
        if (o->n + need > o->cap) { o->cap = (o->n + need) * 2 + 256; o->p = xrealloc(o->p, o->cap); }
        char *w = o->p + o->n;
        if (want_name) { memcpy(w, E->name, nlen); w += nlen; *w++ = nullflag ? '\0' : ':'; }
        if (nflag) { w = put_num(w, base + c->m[k].lineno); *w++ = ':'; }
        if (bflag) { w = put_num(w, off); *w++ = ':'; }
        memcpy(w, data + off, len); w += len;
        *w++ = '\n';
        o->n = (size_t)(w - o->p);
    }
    E->nsel += c->nm - m;
}
static void report_binary(const char *name)
{
    fflush(stdout);
    fprintf(stderr, "%s: %s: binary file matches\n", prog, name);
}

/* ================= file processing ================= */

static int read_all(int fd, char **out, size_t *outn)
{
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            char *nb = realloc(b, cap);
            if (!nb) { free(b); return -1; }
            b = nb;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(b); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    *out = b;
    *outn = n;
    return 0;
}

static void job_done(FileJob *j)
{
    pthread_mutex_lock(&mu);
    j->done = 1;
    pthread_cond_broadcast(&done_cv);
    pthread_mutex_unlock(&mu);
}
#define ZAP_MAX ((size_t)64 << 20)  /* larger binary files are searched with NULs intact */
static void zap_nuls(char *d, size_t n)
{
    for (char *p = memchr(d, 0, n); p; p = memchr(p + 1, 0, n - (size_t)(p + 1 - d))) *p = '\n';
}
static int looks_binary(const char *d, size_t n)
{
    if (binmode == 1 || n == 0) return 0;
    return memchr(d, 0, n < 32768 ? n : 32768) != NULL;
}

/* whole file in one buffer: search it and preformat the output */
static void process_small(FileJob *j, Matcher *M, const char *data, size_t size)
{
    FileJob tmp;
    Chunk c;
    memset(&tmp, 0, sizeof tmp);
    memset(&c, 0, sizeof c);
    j->small = 1;
    j->binary = looks_binary(data, size);
    if (j->binary && binmode == 2) return;          /* -I: binary files never match */
    if (j->binary) zap_nuls((char *)data, size);
    tmp.data = data;
    tmp.size = size;
    c.job = &tmp;
    run_chunk(&c, M);
    j->total = c.count;
    if (store_matches && c.count) {
        Buf o = {0, 0, 0};
        if (j->binary) {
            j->msg = 1;
        } else {
            Emit E = {&o, j->name, data, size, 0, 0, 0, 0, 0, 0, 0, 0};
            emit_chunk(&E, &c, 0);
            emit_finish(&E);
        }
        j->out = o.p;
        j->outlen = o.n;
    }
    free(c.m);
}

static void open_fail(FileJob *j, const char *name, int fd)
{
    if (!sflag) fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno));
    had_error = 1;
    j->small = 1;
    j->failed = 1;
    if (fd > 0) close(fd);
    job_done(j);
}
static void open_job(FileJob *j, Matcher *M)
{
    const char *name = j->name;
    int is_stdin = !strcmp(name, "-");
    int fd = is_stdin ? 0 : open(name, O_RDONLY);
    int ok = fd >= 0;
    struct stat st;
    if (ok && fstat(fd, &st) != 0) ok = 0;
    if (ok && !is_stdin && S_ISDIR(st.st_mode)) { errno = EISDIR; j->isdir = 1; ok = 0; }
    if (!ok) { open_fail(j, name, fd); return; }
    if (is_stdin) j->name = label ? label : "(standard input)";
    int reg = S_ISREG(st.st_mode);

    if (reg && (size_t)st.st_size <= SMALL_MAX) {
        size_t cap = (size_t)st.st_size + 1, n = 0;
        if (M->fcap < cap) { M->fcap = cap * 2; M->fbuf = xrealloc(M->fbuf, M->fcap); }
        for (;;) {
            ssize_t r = read(fd, M->fbuf + n, cap - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
            if (n == cap) break;
        }
        if (fd > 0) close(fd);
        process_small(j, M, M->fbuf, n);
        job_done(j);
        return;
    }
    if (reg) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            j->data = p; j->size = (size_t)st.st_size; j->mapped = 1;
#if defined(MADV_SEQUENTIAL)
            madvise(p, j->size, MADV_SEQUENTIAL);
#endif
        }
    }
    if (!j->data) {
        char *b;
        size_t n;
        if (read_all(fd, &b, &n) != 0) { open_fail(j, name, fd); return; }
        if (n <= SMALL_MAX) {
            if (fd > 0) close(fd);
            process_small(j, M, b, n);
            free(b);
            job_done(j);
            return;
        }
        j->data = b;
        j->size = n;
    }
    j->binary = looks_binary(j->data, j->size);
    if (j->binary && binmode == 0 && j->size <= ZAP_MAX) {
        if (j->mapped) {                            /* private writable view: only pages holding NULs get copied */
            void *p = mmap(NULL, j->size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) { munmap((void *)j->data, j->size); j->data = p; zap_nuls(p, j->size); }
        } else {
            zap_nuls((char *)j->data, j->size);
        }
    }
    if (fd > 0) close(fd);
    if (j->binary && binmode == 2) { j->small = 1; job_done(j); return; }

    j->nchunks = (j->size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    j->chunks = calloc(j->nchunks, sizeof *j->chunks);
    if (!j->chunks) die("out of memory");
    for (size_t k = 0; k < j->nchunks; k++) { j->chunks[k].job = j; j->chunks[k].idx = k; }
    pthread_mutex_lock(&mu);
    for (size_t k = 0; k < j->nchunks; k++) {
        Chunk *c = &j->chunks[k];
        c->next = NULL;
        if (qtail) qtail->next = c; else qhead = c;
        qtail = c;
    }
    j->done = 1;                                    /* chunks exist; each has its own done flag */
    pthread_cond_broadcast(&q_cv);
    pthread_cond_broadcast(&done_cv);
    pthread_mutex_unlock(&mu);
}

static void free_job(FileJob *j)
{
    for (size_t k = 0; k < j->nchunks; k++) free(j->chunks[k].m);
    free(j->chunks);
    free(j->out);
    free(j->ot);
    if (j->data) { if (j->mapped) munmap((void *)j->data, j->size); else free((void *)j->data); }
    free(j);
}

static void *worker(void *arg)
{
    (void)arg;
    Matcher M;
    matcher_init(&M);
    pthread_mutex_lock(&mu);
    for (;;) {
        /* An open task enqueues the real chunks. Exit only once feeding has
         * stopped, the queue is empty, and nobody is still inside open_job. */
        while (!qhead && !(feed_done && busy == 0)) pthread_cond_wait(&q_cv, &mu);
        if (!qhead) break;
        Chunk *c = qhead;
        qhead = c->next;
        if (!qhead) qtail = NULL;
        busy++;
        pthread_mutex_unlock(&mu);

        if (c->is_open) open_job(c->job, &M);
        else {
            run_chunk(c, &M);
            pthread_mutex_lock(&mu);                /* last touch of c: the printer may free it */
            c->done = 1;
            pthread_cond_broadcast(&done_cv);
            pthread_mutex_unlock(&mu);
        }

        pthread_mutex_lock(&mu);
        busy--;
        if (!qhead && feed_done && busy == 0) pthread_cond_broadcast(&q_cv);
    }
    pthread_mutex_unlock(&mu);
    matcher_free(&M);
    return NULL;
}

/* ================= ordered output ================= */

static void wait_chunk(Chunk *c)
{
    pthread_mutex_lock(&mu);
    while (!c->done) pthread_cond_wait(&done_cv, &mu);
    pthread_mutex_unlock(&mu);
}
static void flush_pb(Buf *pb)
{
    if (pb->n) { fwrite(pb->p, 1, pb->n, stdout); pb->n = 0; }
}
static void put_name_line(Buf *pb, const char *name, const char *tail)
{
    put_col(pb, col_fn, name, strlen(name));
    if (nullflag) buf_put(pb, "", 1); else buf_puts(pb, tail);
}

static void *printer(void *arg)
{
    (void)arg;
    Buf pb = {0, 0, 0};
    int group_started = 0;                          /* a context group was printed in an earlier file */
    size_t cap_total = (size_t)-1;                  /* stop scanning a file after this many hits */
    if (maxcount >= 0) cap_total = (size_t)maxcount;
    if (lflag || Lflag || qflag) cap_total = 1;
    for (size_t i = 0; i < nfiles; i++) {
        pthread_mutex_lock(&mu);
        while (!jobs[i] || !jobs[i]->done) pthread_cond_wait(&done_cv, &mu);
        pthread_mutex_unlock(&mu);
        FileJob *j = jobs[i];
        if (j->failed) {
            /* A directory is not searched. -c still reports 0 and -L still
             * lists it, matching grep, and the diagnostic stays on stderr. */
            if (j->isdir && cflag) {
                char t[24];
                if (want_name) put_name_line(&pb, j->name, ":");
                size_t q = u2s(t, 0);
                buf_put(&pb, t + q, 24 - q);
                buf_put(&pb, "\n", 1);
                flush_pb(&pb);
            } else if (j->isdir && Lflag) {
                put_name_line(&pb, j->name, "\n");
                flush_pb(&pb);
            }
            goto next;
        }
        size_t total = 0;
        int streamed = 0;
        if (j->small) {
            total = j->total;
        } else if (cflag || lflag || Lflag) {
            for (size_t k = 0; k < j->nchunks; k++) {
                wait_chunk(&j->chunks[k]);
                total += j->chunks[k].count;
                if (total >= cap_total) atomic_store(&j->cancel, 1);
            }
        } else {
            streamed = 1;
            Emit E = {&pb, j->name, j->data, j->size, 0, 0, 0, 0, 0, 0, 0, group_started && ctx_on()};
            size_t base = 0;
            int stopped = 0;
            for (size_t k = 0; k < j->nchunks; k++) {
                Chunk *c = &j->chunks[k];
                wait_chunk(c);
                total += c->count;
                if (!stopped) {
                    if (j->binary) {
                        if (c->count) {
                            flush_pb(&pb);
                            report_binary(j->name);
                            stopped = 1;
                            atomic_store(&j->cancel, 1);
                        }
                    } else {
                        emit_chunk(&E, c, base);
                        base += c->nlines;
                        if (E.limit) { stopped = 1; atomic_store(&j->cancel, 1); }
                    }
                }
                free(c->m); c->m = NULL; c->nm = 0;
                if (pb.n > (1 << 16)) { flush_pb(&pb); if (line_buffered) fflush(stdout); }
            }
            if (!j->binary) emit_finish(&E);
            flush_pb(&pb);
            if (E.out_any && ctx_on()) group_started = 1;
        }
        if (maxcount >= 0 && total > (size_t)maxcount) total = (size_t)maxcount;
        if (total) any_match = 1;
        if (lflag) {
            if (total) { put_name_line(&pb, j->name, "\n"); }
        } else if (Lflag) {
            if (!total) { put_name_line(&pb, j->name, "\n"); }
        } else if (cflag) {
            char t[24];
            if (want_name) put_name_line(&pb, j->name, ":");
            size_t q = u2s(t, total);
            buf_put(&pb, t + q, 24 - q);
            buf_put(&pb, "\n", 1);
        } else if (!streamed && j->msg) {
            flush_pb(&pb);
            report_binary(j->name);
        } else if (!streamed && j->outlen) {
            if (ctx_on()) { if (group_started) put_groupsep(&pb); group_started = 1; }
            buf_put(&pb, j->out, j->outlen);
        }
        flush_pb(&pb);
        if (line_buffered) fflush(stdout);
        if (ferror(stdout)) out_error = 1;
    next:
        free_job(j);
        pthread_mutex_lock(&mu);
        jobs[i] = NULL;
        printed = i + 1;
        pthread_cond_signal(&slot_cv);
        pthread_mutex_unlock(&mu);
    }
    free(pb.p);
    return NULL;
}

/* ================= file list: -r, --include/--exclude ================= */

static char **flist, **inc, **exc, **excdir;
static size_t nflist, cflist, ninc, nexc, nexcdir;
static int recursed_dir;

static void strv_add(char ***v, size_t *n, const char *s)
{
    *v = xrealloc(*v, (*n + 1) * sizeof **v);
    (*v)[(*n)++] = strdup(s);
}
static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}
static int glob_any(char **g, size_t n, const char *path)
{
    for (size_t i = 0; i < n; i++)
        if (!fnmatch(g[i], base_of(path), 0) || !fnmatch(g[i], path, 0)) return 1;
    return 0;
}
static int file_wanted(const char *path)
{
    if (nexc && glob_any(exc, nexc, path)) return 0;
    if (ninc && !glob_any(inc, ninc, path)) return 0;
    return 1;
}
static void flist_add(const char *p)
{
    if (nflist == cflist) { cflist = cflist ? cflist * 2 : 64; flist = xrealloc(flist, cflist * sizeof *flist); }
    flist[nflist++] = strdup(p);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk_dir(const char *dir, int depth)    /* dir == "" means the current directory */
{
    if (depth > 200) {
        if (!sflag) fprintf(stderr, "%s: %s: directory nesting too deep\n", prog, dir);
        had_error = 1;
        return;
    }
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) {
        if (!sflag) fprintf(stderr, "%s: %s: %s\n", prog, dir[0] ? dir : ".", strerror(errno));
        had_error = 1;
        return;
    }
    char **names = NULL;
    size_t nn = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        strv_add(&names, &nn, de->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof *names, cmp_str);       /* deterministic order */
    size_t dl = strlen(dir);
    for (size_t i = 0; i < nn; i++) {
        size_t nl = strlen(names[i]);
        char *child = xrealloc(NULL, dl + nl + 2);
        if (dl == 0) memcpy(child, names[i], nl + 1);
        else {
            memcpy(child, dir, dl);
            size_t o = dl;
            if (dir[dl - 1] != '/') child[o++] = '/';
            memcpy(child + o, names[i], nl + 1);
        }
        struct stat st;
        int rc = deref ? stat(child, &st) : lstat(child, &st);
        if (rc) {
            if (!sflag) fprintf(stderr, "%s: %s: %s\n", prog, child, strerror(errno));
            had_error = 1;
        } else if (S_ISDIR(st.st_mode)) {
            if (!(nexcdir && glob_any(excdir, nexcdir, names[i]))) walk_dir(child, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            if (file_wanted(child)) flist_add(child);
        }
        free(child);
        free(names[i]);
    }
    free(names);
}
static void add_operand(const char *p)
{
    if (!strcmp(p, "-")) { flist_add(p); return; }
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (rflag) { recursed_dir = 1; walk_dir(p, 0); return; }
        if (dirskip) return;
    } else if (!file_wanted(p)) {
        return;
    }
    flist_add(p);
}

/* ================= main ================= */

#if defined(__linux__)
static long physical_core_count(void)
{
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return 0;

    typedef struct { int package, core; } CoreKey;
    CoreKey *seen = NULL;
    size_t nseen = 0, cap = 0;
    struct dirent *de;

    while ((de = readdir(d)) != NULL) {
        unsigned cpu;
        if (sscanf(de->d_name, "cpu%u", &cpu) != 1) continue;

        char path[256];
        int package = -1, core = -1;
        FILE *f;

        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", cpu);
        f = fopen(path, "r");
        if (!f || fscanf(f, "%d", &package) != 1) {
            if (f) fclose(f);
            continue;
        }
        fclose(f);

        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
        f = fopen(path, "r");
        if (!f || fscanf(f, "%d", &core) != 1) {
            if (f) fclose(f);
            continue;
        }
        fclose(f);

        int duplicate = 0;
        for (size_t i = 0; i < nseen; i++)
            if (seen[i].package == package && seen[i].core == core) {
                duplicate = 1;
                break;
            }
        if (duplicate) continue;

        if (nseen == cap) {
            cap = cap ? cap * 2 : 16;
            seen = xrealloc(seen, cap * sizeof *seen);
        }
        seen[nseen++] = (CoreKey){package, core};
    }
    closedir(d);
    free(seen);
    return (long)nseen;
}
#endif

static long default_thread_count(void)
{
#if defined(__linux__)
    long n = physical_core_count();
    if (n > 0) return n;
    n = sysconf(_SC_NPROCESSORS_ONLN);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? n : 1;
}


static void usage_error(void)
{
    fprintf(stderr, "Usage: %s [OPTION]... PATTERNS [FILE]...\nTry '%s --help' for more information.\n",
            prog, prog);
    exit(2);
}
static void print_help(void)
{
    printf(
"Usage: %s [OPTION]... PATTERNS [FILE]...\n"
"Search for PATTERNS in each FILE (standard input if none, or '-').\n"
"Example: %s -i 'hello world' menu.h main.c\n\n"
"Pattern selection and interpretation:\n"
"  -E, --extended-regexp     PATTERNS are extended regular expressions\n"
"  -F, --fixed-strings       PATTERNS are strings\n"
"  -G, --basic-regexp        PATTERNS are basic regular expressions (default)\n"
"  -e, --regexp=PATTERNS     use PATTERNS for matching (may be repeated)\n"
"  -f, --file=FILE           take PATTERNS from FILE, one per line\n"
"  -i, --ignore-case         ignore case distinctions   (-y is a synonym)\n"
"      --no-ignore-case      do not ignore case distinctions (default)\n"
"  -w, --word-regexp         match only whole words\n"
"  -x, --line-regexp         match only whole lines\n\n"
"Miscellaneous:\n"
"  -s, --no-messages         suppress error messages\n"
"  -v, --invert-match        select non-matching lines\n"
"  -V, --version             display version information and exit\n"
"      --help                display this help text and exit\n"
"  -j, --threads=N           worker threads (default: physical cores)\n\n"
"Output control:\n"
"  -m, --max-count=NUM       stop after NUM selected lines per file\n"
"  -b, --byte-offset         print the byte offset with output lines\n"
"  -n, --line-number         print line number with output lines\n"
"      --line-buffered       flush output on every line\n"
"  -H, --with-filename       print file name with output lines\n"
"  -h, --no-filename         suppress the file name prefix on output\n"
"      --label=LABEL         use LABEL as the standard input file name\n"
"  -o, --only-matching       show only nonempty parts of lines that match\n"
"  -q, --quiet, --silent     suppress all normal output\n"
"      --binary-files=TYPE   TYPE is 'binary', 'text', or 'without-match'\n"
"  -a, --text                same as --binary-files=text\n"
"  -I                        same as --binary-files=without-match\n"
"  -d, --directories=ACTION  how to handle directories: read, recurse, skip\n"
"  -D, --devices=ACTION      accepted for compatibility (read, skip)\n"
"  -r, --recursive           like --directories=recurse (does not follow symlinks)\n"
"  -R, --dereference-recursive  likewise, but follow all symlinks\n"
"      --include=GLOB        search only files whose name matches GLOB\n"
"      --exclude=GLOB        skip files whose name matches GLOB\n"
"      --exclude-dir=GLOB    skip directories whose name matches GLOB\n"
"  -L, --files-without-match print only names of FILEs with no selected lines\n"
"  -l, --files-with-matches  print only names of FILEs with selected lines\n"
"  -c, --count               print only a count of selected lines per FILE\n"
"      --color[=WHEN], --colour[=WHEN]  use markers to highlight matches;\n"
"                            WHEN is 'always', 'never', or 'auto'\n"
"  -Z, --null                print 0 byte after FILE name\n\n"
"Context control:\n"
"  -B, --before-context=NUM  print NUM lines of leading context\n"
"  -A, --after-context=NUM   print NUM lines of trailing context\n"
"  -C, --context=NUM         print NUM lines of output context\n"
"  -NUM                      same as --context=NUM\n\n"
"Exit status is 0 if a line is selected, 1 if none were, and 2 if an error\n"
"occurred (unless -q is given and a line is selected).\n"
"Not supported: -P (Perl regex), -z (NUL-separated records).\n", prog, prog);
}
static void print_version(void)
{
    printf("%s 1.3.0\nA multi-threaded POSIX/GNU-style grep.\n", prog);
}
static long parse_num(const char *s, const char *what)
{
    char *e = NULL;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < 0 || errno) {
        if (!strcmp(what, "max count")) fprintf(stderr, "%s: invalid max count\n", prog);
        else fprintf(stderr, "%s: %s: invalid %s\n", prog, s, what);
        exit(2);
    }
    return v;
}
static void unsupported(const char *what) __attribute__((noreturn));
static void unsupported(const char *what)
{
    fprintf(stderr, "%s: %s is not supported\n", prog, what);
    exit(2);
}

enum { O_HELP = 256, O_COLOR, O_LABEL, O_INCLUDE, O_EXCLUDE, O_EXCLUDE_DIR, O_BINARY,
       O_LINEBUF, O_NOICASE };
static const struct option longopts[] = {
    {"extended-regexp", no_argument, 0, 'E'}, {"fixed-strings", no_argument, 0, 'F'},
    {"basic-regexp", no_argument, 0, 'G'}, {"perl-regexp", no_argument, 0, 'P'},
    {"regexp", required_argument, 0, 'e'}, {"file", required_argument, 0, 'f'},
    {"ignore-case", no_argument, 0, 'i'}, {"no-ignore-case", no_argument, 0, O_NOICASE},
    {"word-regexp", no_argument, 0, 'w'}, {"line-regexp", no_argument, 0, 'x'},
    {"null-data", no_argument, 0, 'z'}, {"no-messages", no_argument, 0, 's'},
    {"invert-match", no_argument, 0, 'v'}, {"version", no_argument, 0, 'V'},
    {"help", no_argument, 0, O_HELP}, {"max-count", required_argument, 0, 'm'},
    {"byte-offset", no_argument, 0, 'b'}, {"line-number", no_argument, 0, 'n'},
    {"line-buffered", no_argument, 0, O_LINEBUF}, {"with-filename", no_argument, 0, 'H'},
    {"no-filename", no_argument, 0, 'h'}, {"label", required_argument, 0, O_LABEL},
    {"only-matching", no_argument, 0, 'o'}, {"quiet", no_argument, 0, 'q'},
    {"silent", no_argument, 0, 'q'}, {"binary-files", required_argument, 0, O_BINARY},
    {"text", no_argument, 0, 'a'}, {"directories", required_argument, 0, 'd'},
    {"devices", required_argument, 0, 'D'}, {"recursive", no_argument, 0, 'r'},
    {"dereference-recursive", no_argument, 0, 'R'}, {"include", required_argument, 0, O_INCLUDE},
    {"exclude", required_argument, 0, O_EXCLUDE}, {"exclude-dir", required_argument, 0, O_EXCLUDE_DIR},
    {"files-without-match", no_argument, 0, 'L'}, {"files-with-matches", no_argument, 0, 'l'},
    {"count", no_argument, 0, 'c'}, {"color", optional_argument, 0, O_COLOR},
    {"colour", optional_argument, 0, O_COLOR}, {"null", no_argument, 0, 'Z'},
    {"after-context", required_argument, 0, 'A'}, {"before-context", required_argument, 0, 'B'},
    {"context", required_argument, 0, 'C'}, {"threads", required_argument, 0, 'j'},
    {0, 0, 0, 0}};

static const char *progname(const char *a0)
{
    const char *s = strrchr(a0, '/');
    s = s ? s + 1 : a0;
    return *s ? s : "cg";
}

int main(int argc, char **argv)
{
    long nthreads = default_thread_count();
    int have_pat = 0, opt, color_mode = 0;          /* 0 never, 1 always, 2 auto */
    long dctx = -1, digits = 0;
    int last_digit_cur = -1, last_was_digit = 0;
    prog = progname(argv[0]);
    {   /* CG_CHUNK_KIB: work-unit size in KiB (64 .. 1048576), for tuning */
        const char *e = getenv("CG_CHUNK_KIB");
        if (e && *e) {
            char *ep; long k = strtol(e, &ep, 10);
            if (!*ep && k >= 64 && k <= (1L << 20)) chunk_size = (size_t)k << 10;
        }
    }
    setlocale(LC_ALL, "");
    mb_locale = MB_CUR_MAX > 1;
    for (;;) {
        int cur = optind;
        opt = getopt_long(argc, argv, "0123456789A:B:C:D:EFGHILPRUVZabcd:e:f:hij:lm:noqrsvwxyz",
                          longopts, NULL);
        if (opt == -1) break;
        if (opt >= '0' && opt <= '9') {
            digits = (last_was_digit && cur == last_digit_cur) ? digits * 10 + (opt - '0') : opt - '0';
            last_digit_cur = cur; last_was_digit = 1;
            dctx = digits;
            continue;
        }
        last_was_digit = 0;
        switch (opt) {
        case 'E': Eflag = 1; Fflag = 0; break;
        case 'F': Fflag = 1; Eflag = 0; break;
        case 'G': Eflag = Fflag = 0; break;
        case 'P': unsupported("-P (Perl regular expressions)");
        case 'z': unsupported("-z (NUL-separated records)");
        case 'c': cflag = 1; break;
        case 'e': add_patterns(optarg, strlen(optarg), 0); have_pat = 1; break;
        case 'f': {
            int fd = !strcmp(optarg, "-") ? 0 : open(optarg, O_RDONLY);
            char *b; size_t n;
            if (fd < 0 || read_all(fd, &b, &n) != 0) {
                fprintf(stderr, "%s: %s: %s\n", prog, optarg, strerror(errno));
                return 2;
            }
            if (fd > 0) close(fd);
            add_patterns(b, n, 1);
            free(b);
            have_pat = 1;
            break;
        }
        case 'i': case 'y': iflag = 1; break;
        case O_NOICASE: iflag = 0; break;
        case 'w': wflag = 1; break;
        case 'x': xflag = 1; break;
        case 'l': lflag = 1; Lflag = 0; break;
        case 'L': Lflag = 1; lflag = 0; break;
        case 'n': nflag = 1; break;
        case 'q': qflag = 1; break;
        case 's': sflag = 1; break;
        case 'v': vflag = 1; break;
        case 'H': show_name = 1; break;
        case 'h': show_name = 0; break;
        case 'o': oflag = 1; break;
        case 'b': bflag = 1; break;
        case 'Z': nullflag = 1; break;
        case 'a': binmode = 1; break;
        case 'I': binmode = 2; break;
        case 'U': break;
        case 'r': rflag = 1; break;
        case 'R': rflag = 1; deref = 1; break;
        case 'm': maxcount = parse_num(optarg, "max count"); break;
        case 'A': ctxA = parse_num(optarg, "context length argument"); break;
        case 'B': ctxB = parse_num(optarg, "context length argument"); break;
        case 'C': dctx = parse_num(optarg, "context length argument"); break;
        case 'd':
            if (!strcmp(optarg, "recurse")) rflag = 1;
            else if (!strcmp(optarg, "skip")) dirskip = 1;
            else if (strcmp(optarg, "read")) { fprintf(stderr, "%s: invalid argument '%s' for '--directories'\n", prog, optarg); return 2; }
            break;
        case 'D':
            if (strcmp(optarg, "read") && strcmp(optarg, "skip")) { fprintf(stderr, "%s: unknown devices method\n", prog); return 2; }
            break;
        case O_BINARY:
            if (!strcmp(optarg, "binary")) binmode = 0;
            else if (!strcmp(optarg, "text")) binmode = 1;
            else if (!strcmp(optarg, "without-match")) binmode = 2;
            else { fprintf(stderr, "%s: unknown binary-files type\n", prog); return 2; }
            break;
        case O_INCLUDE: strv_add(&inc, &ninc, optarg); break;
        case O_EXCLUDE: strv_add(&exc, &nexc, optarg); break;
        case O_EXCLUDE_DIR: strv_add(&excdir, &nexcdir, optarg); break;
        case O_LABEL: label = optarg; break;
        case O_LINEBUF: line_buffered = 1; break;
        case O_COLOR:
            if (!optarg || !strcmp(optarg, "auto") || !strcmp(optarg, "tty") || !strcmp(optarg, "if-tty")) color_mode = 2;
            else if (!strcmp(optarg, "always") || !strcmp(optarg, "yes") || !strcmp(optarg, "force")) color_mode = 1;
            else if (!strcmp(optarg, "never") || !strcmp(optarg, "no") || !strcmp(optarg, "none")) color_mode = 0;
            else { fprintf(stderr, "%s: invalid argument '%s' for '--color'\n", prog, optarg); return 2; }
            break;
        case 'j': {
            char *e = NULL;
            long v = strtol(optarg, &e, 10);
            if (!*optarg || *e || v < 1 || v > 256) {
                fprintf(stderr, "%s: -j needs a thread count from 1 to 256 (got '%s')\n", prog, optarg);
                return 2;
            }
            nthreads = v;
            break;
        }
        case 'V': print_version(); return 0;
        case O_HELP: print_help(); return 0;
        default: usage_error();
        }
    }
    if (!have_pat) {
        if (optind >= argc) usage_error();
        add_patterns(argv[optind], strlen(argv[optind]), 0);
        optind++;
    }
    if (ctxA < 0) ctxA = dctx;
    if (ctxB < 0) ctxB = dctx;
    if (xflag) wflag = 0;
    if (maxcount == 0) return 1;                    /* nothing can be selected */
    if (nthreads > 256) nthreads = 256;
    color_on = color_mode == 1 || (color_mode == 2 && isatty(1) && getenv("TERM") &&
                                   strcmp(getenv("TERM"), "dumb"));
    if (color_on) color_init();

    /* the file list */
    int noperands = argc - optind;
    if (rflag && optind >= argc) { recursed_dir = 1; walk_dir("", 0); }
    else if (optind >= argc) flist_add("-");
    else for (int i = optind; i < argc; i++) add_operand(argv[i]);
    files = (const char **)flist;
    nfiles = nflist;
    store_matches = !(cflag || lflag || Lflag || qflag);

    /* \bWORD\b and \<WORD\> over plain word characters mean exactly -w WORD:
     * run them on the same fast path (token lookup) instead of libc regex. */
    if (!wflag && !xflag && !Fflag && npat) {
        int all = 1;
        for (size_t i = 0; i < npat && all; i++) {
            const char *p = fx[i]; size_t n = fxlen[i];
            int ok = 0;
            if (n > 4 && p[0] == '\\' && ((p[1] == 'b' && p[n - 2] == '\\' && p[n - 1] == 'b') ||
                                          (p[1] == '<' && p[n - 2] == '\\' && p[n - 1] == '>'))) {
                ok = 1;
                for (size_t k = 2; k < n - 2; k++) {
                    unsigned char c = (unsigned char)p[k];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) ok = 0;
                }
            }
            if (!ok) all = 0;
        }
        if (all) {
            for (size_t i = 0; i < npat; i++) {
                size_t n = fxlen[i] - 4;
                memmove(fx[i], fx[i] + 2, n);
                fx[i][n] = 0;
                fxlen[i] = n;
            }
            wflag = 1;
        }
    }
    /* engine selection */
    int use_fast = Fflag && !iflag;
    int litset = 0;
    engine = ENG_REGEX;
    /* Plain words (fixed strings, or ERE/BRE with no metacharacters) never
     * need the NFA. Building it used to exceed MAX_NFA and silently fall
     * back to one libc regex per pattern. */
    if (!vflag && !iflag && !xflag && !wflag)
        litset = collect_literal_set();
    /* -o and --color need match spans. A zero-length literal matches every
     * line, and the byte scanner would report the whole line as the span. */
    if (litset && (oflag || color_on)) {
        int empty = 0;
        for (size_t i = 0; i < nmlit; i++) if (mlits[i].n == 0) empty = 1;
        if (empty) {
            for (size_t i = 0; i < nmlit; i++) free(mlits[i].p);
            free(mlits);
            mlits = NULL;
            nmlit = 0;
            mlit_exact = 0;
            litset = 0;
        }
    }
    if (!litset && !vflag && !iflag && !xflag && !wflag && !Fflag)
        litset = collect_pure_alternation();
    if (litset && nmlit > 1) engine = ENG_FAST;
    else if (litset && nmlit == 1) {
        set_literal(mlits[0].p, mlits[0].n, 0);
        engine = ENG_FAST;
    } else if (use_fast && npat == 1) engine = ENG_FAST;
    else if (try_build_dfa()) engine = ENG_DFA;
    else if (use_fast) engine = ENG_FAST;
    else free_set(compile_set()); /* validate; exits 2 on bad pattern */
    if ((oflag || color_on || wflag) && !(oflag || color_on)) (void)span_compile();   /* -w alone: validate up front */
    else if ((oflag || color_on) && !(wflag && wset_on)) { /* lset decided below; validate only if it is not used */ }
    if (!vflag && !litset) {
        if (engine == ENG_FAST && npat == 1 && !xflag && fxlen[0] > 0) {
            set_literal(fx[0], fxlen[0], 0);
        } else if (engine == ENG_DFA && iflag && !xflag && nbr == 1 && !br[0].bol) {
            size_t bl = 0;
            char *best = req_lit_ci(br[0].n, &bl);
            if (best && lit_worth(best, bl)) { set_literal(best, bl, 1); lit_icase = 1; }
            free(best);
        } else if (engine == ENG_DFA && !iflag && !xflag) {
            /* If every branch has a required literal, use their union as a
             * candidate filter.  This is the important generalization of the
             * old single-branch prefilter: Sherlock|Holmes no longer drives
             * the full DFA over every byte of the file. */
            mlits = NULL; nmlit = 0;
            mlit_exact = 0;
            int all_pure = nbr > 1;
            for (int i = 0; i < nbr && all_pure; i++) {
                size_t L = 0;
                if (!pure_lit(br[i].n, &L) || L == 0) all_pure = 0;
            }
            /* Pure alternation: the literal is the branch, not a hint. AC is
             * the matcher. Short branches are included; an empty one is not. */
            if (all_pure) {
                for (int i = 0; i < nbr; i++) {
                    char *best = NULL; size_t bl = 0;
                    req_lit(br[i].n, &best, &bl);
                    mlits = xrealloc(mlits, (nmlit + 1) * sizeof *mlits);
                    mlits[nmlit].p = best; mlits[nmlit].n = bl; mlits[nmlit].r = 0;
                    nmlit++;
                }
                mlit_exact = 1;
                /* AC is built below only if the hash filter cannot take the set */
            }
            int all_required = !mlit_exact && nbr > 1;
            for (int i = 0; i < nbr && all_required; i++) {
                char *best = NULL; size_t bl = 0;
                req_lit(br[i].n, &best, &bl);
                if (bl < 3) all_required = 0;
                else {
                    mlits = xrealloc(mlits, (nmlit + 1) * sizeof *mlits);
                    mlits[nmlit].p = best; mlits[nmlit].n = bl; mlits[nmlit].r = 0;
                    for (size_t k = 1; k < bl; k++)
                        if (commonness((unsigned char)best[k]) < commonness((unsigned char)best[mlits[nmlit].r]))
                            mlits[nmlit].r = k;
                    nmlit++;
                    continue;
                }
                free(best);
            }
            if (mlit_exact) {
                /* already built */
            } else if (!all_required || nmlit != (size_t)nbr) {
                for (size_t i = 0; i < nmlit; i++) free(mlits[i].p);
                free(mlits); mlits = NULL; nmlit = 0;
                char *best = NULL; size_t bl = 0;
                if (nbr == 1) {
                    req_lit(br[0].n, &best, &bl);
                    if (lit_worth(best, bl)) set_literal(best, bl, 1);
                }
                free(best);
            }
        }
    }

    if (wflag && engine == ENG_DFA && !iflag && !xflag) wset_build();
    wrap_w_dfa();
    lset_build();
    if (wset_on) {
        /* The token lookup is exact, so literal hits only need to be rare enough
         * to be worth skipping to.  For common words nearly every line is a
         * candidate: drop the prefilter and test each line directly. */
        double dens = 0;
        for (size_t h = 0; h < ws_cap; h++) {
            if (!ws_lit[h]) continue;
            double q = 1;
            for (size_t k = 0; k < ws_len[h]; k++) q *= fb_prob((unsigned char)ws_lit[h][k]);
            dens += q;
        }
        if (dens > 0.0005 || ws_cap > 64) {
            for (size_t i = 0; i < nmlit; i++) free(mlits[i].p);
            free(mlits); mlits = NULL; nmlit = 0;
            free(lit); lit = NULL; litlen = 0;
        }
    }
    if (wflag) { mlit_exact = 0; lit_verify = 1; }   /* literal hits are only candidates for -w */

#if defined(__x86_64__)
    if (nmlit >= 5 && nmlit <= TEDDY_MAX_LITS && __builtin_cpu_supports("avx2"))
        teddy_build();
#endif
    if (engine == ENG_DFA && !vflag && !lit && nmlit == 0) fb_setup();
    stream_ok = engine == ENG_DFA && !(vflag && qflag) && !wflag && !mb_class_used && !fb_on && !lit && nmlit == 0;
    wm_build();
    byte_class_build();
    if (!byte_class && nmlit >= 5 && !teddy_ready && !wm_ready) mh_build();
    if (mh_ready) select_hot();
    mlit_any_short = 0;
    for (size_t i = 0; i < nmlit; i++) if (mlits[i].n < 3) mlit_any_short = 1;
    exact_filter = 0;
    if (mlit_exact && nmlit >= 2) {
        size_t mn = (size_t)-1, nlong = 0;
        for (size_t i = 0; i < nmlit; i++) {
            if (mlits[i].n < mn) mn = mlits[i].n;
            if (mlits[i].n >= 3) nlong++;
        }
#if defined(__x86_64__)
        if (nlong && nmlit <= 4 && mn >= 3 && __builtin_cpu_supports("avx2")) exact_filter = 1;
#endif
        if (nlong && (teddy_ready || mh_ready || wm_ready)) exact_filter = 1;
        if (!nlong) exact_filter = 1;          /* only short literals: cached memchr */
        /* Dense sets (common words) hit on almost every line, which is where the
         * per-literal filters lose to one table lookup per byte: estimate the
         * chance that some literal starts at a given position and use the
         * automaton when it is high.  Sparse sets keep the SIMD filters. */
        if (exact_filter && nmlit >= 5 && nmlit <= 1500) {
            double dens = 0;
            for (size_t i = 0; i < nmlit; i++) {
                double q = 1;
                for (size_t k = 0; k < mlits[i].n; k++) q *= fb_prob((unsigned char)mlits[i].p[k]);
                dens += q;
            }
            if (dens > 0.001) exact_filter = 0;
        }
        if (!exact_filter) ac_build();         /* dense set, or hash could not be built */
    }

    static char obuf[1 << 16];
    setvbuf(stdout, obuf, _IOFBF, sizeof obuf);
    jobs = calloc(nfiles, sizeof *jobs);
    if (!jobs) die("out of memory");
    window = (size_t)nthreads * 16 < 64 ? 64 : (size_t)nthreads * 16;
    want_name = show_name < 0 ? (noperands > 1 || recursed_dir) : show_name;

    pthread_t *tid = xrealloc(NULL, (size_t)nthreads * sizeof *tid), ptid;
    for (long t = 0; t < nthreads; t++)
        if (pthread_create(&tid[t], NULL, worker, NULL)) die("pthread_create failed");
    if (pthread_create(&ptid, NULL, printer, NULL)) die("pthread_create failed");

    for (size_t i = 0; i < nfiles; i++) {
        FileJob *j = calloc(1, sizeof *j);
        Chunk *ot = calloc(1, sizeof *ot);
        if (!j || !ot) die("out of memory");
        j->name = files[i];
        j->ot = ot;
        ot->job = j;
        ot->is_open = 1;
        pthread_mutex_lock(&mu);
        while (i - printed >= window) pthread_cond_wait(&slot_cv, &mu);
        jobs[i] = j;
        if (qtail) qtail->next = ot; else qhead = ot;
        qtail = ot;
        pthread_cond_signal(&q_cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    feed_done = 1;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&mu);

    pthread_join(ptid, NULL);
    for (long t = 0; t < nthreads; t++) pthread_join(tid[t], NULL);

    if (fflush(stdout) != 0 || out_error) {
        fprintf(stderr, "%s: write error\n", prog);
        free(tid);
        return 2;
    }
    for (size_t i = 0; i < nmlit; i++) free(mlits[i].p);
    free(mlits);
    free(ac);
    free(tid);
    if (had_error && !(qflag && any_match)) return 2;
    return any_match ? 0 : 1;
}
