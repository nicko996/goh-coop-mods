/*
 * hd_patch.h - applies a Historical Doctrine mission patch (.hdp) to a mission text in memory.
 * Plain C, no Windows calls, so tools/test_hd_patch.py can build it into a small test program.
 * The reference implementation (same results) is apply() in data/make_hd_data.py; the file format is described
 * there too.
 *
 * A hunk = lines to delete (often none) + lines to insert, found by the vanilla lines around it:
 *   1. before + deleted + after (contiguous, at least one line each side), 2. deleted + after, 3. before + deleted.
 * Order 2 before 3: lines another mod inserted at the same spot end up before ours, whichever mod patched first
 * (tools/test_hd_coop_order.py), so every coop peer gets the same text.
 * The first window that occurs exactly once in the text gives the spot.  Every hunk must be found, in order,
 * otherwise the text is left unchanged.
 */
#ifndef HD_PATCH_H
#define HD_PATCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; size_t n; } HdLine;

typedef struct {
  int nb, na, nd, ni, kbc, kac, kbo, kao;
  HdLine *B, *A, *D, *I;
} HdHunk;

typedef struct {
  char target[300];       /* "map/single/.../0.mi", lower case */
  int nh;
  HdHunk *h;
  char *buf;              /* the file, owned (lines point into it) */
  HdLine *lines;
} HdPatch;

#define HD_MARKER "\"doctrine_historical\""

/* split buf (len bytes) at '\n'; a '\r' right before '\n' is dropped; n + 1 pieces for n newlines */
static HdLine *hd_split(const char *buf, size_t len, size_t *count)
{
  size_t n = 1;
  for (size_t i = 0; i < len; i++) if (buf[i] == '\n') n++;
  HdLine *l = (HdLine *) malloc(n * sizeof *l);
  if (l == NULL) return NULL;
  size_t k = 0, start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || buf[i] == '\n') {
      size_t e = i;
      if (i < len && e > start && buf[e - 1] == '\r') e--;
      l[k].p = buf + start;
      l[k].n = e - start;
      k++;
      start = i + 1;
    }
  }
  *count = k;
  return l;
}

static int hd_eq(const HdLine *a, const HdLine *b) { return a->n == b->n && memcmp(a->p, b->p, a->n) == 0; }

static int hd_atoi(const HdLine *l, size_t *pos)
{
  int v = 0;
  while (*pos < l->n && l->p[*pos] == ' ') (*pos)++;
  while (*pos < l->n && l->p[*pos] >= '0' && l->p[*pos] <= '9') v = v * 10 + (l->p[(*pos)++] - '0');
  return v;
}

static void hd_free(HdPatch *p)
{
  free(p->h); free(p->lines); free(p->buf);
  memset(p, 0, sizeof *p);
}

/* parse a .hdp file; takes ownership of buf (malloc'ed).  Returns 1 on success. */
static int hd_parse(char *buf, size_t len, HdPatch *p)
{
  memset(p, 0, sizeof *p);
  p->buf = buf;
  size_t n = 0;
  p->lines = hd_split(buf, len, &n);
  if (p->lines == NULL || n < 3) goto bad;
  HdLine *L = p->lines;
  if (L[0].n != 5 || memcmp(L[0].p, "HDP 1", 5) != 0) goto bad;
  if (L[1].n < 8 || L[1].n - 7 >= sizeof p->target || memcmp(L[1].p, "target ", 7) != 0) goto bad;
  memcpy(p->target, L[1].p + 7, L[1].n - 7);
  p->target[L[1].n - 7] = 0;
  if (L[2].n < 7 || memcmp(L[2].p, "hunks ", 6) != 0) goto bad;
  size_t q = 6;
  p->nh = hd_atoi(&L[2], &q);
  p->h = (HdHunk *) calloc(p->nh ? p->nh : 1, sizeof *p->h);
  if (p->h == NULL) goto bad;
  size_t pos = 3;
  for (int k = 0; k < p->nh; k++) {
    if (pos >= n || L[pos].n < 1 || L[pos].p[0] != '@') goto bad;
    HdHunk *h = &p->h[k];
    q = 1;
    h->nb = hd_atoi(&L[pos], &q); h->na = hd_atoi(&L[pos], &q); h->nd = hd_atoi(&L[pos], &q);
    h->ni = hd_atoi(&L[pos], &q); h->kbc = hd_atoi(&L[pos], &q); h->kac = hd_atoi(&L[pos], &q);
    h->kbo = hd_atoi(&L[pos], &q); h->kao = hd_atoi(&L[pos], &q);
    pos++;
    size_t need = (size_t) h->nb + h->na + h->nd + h->ni;
    if (pos + need > n || h->kbc > h->nb || h->kbo > h->nb || h->kac > h->na || h->kao > h->na) goto bad;
    h->B = &L[pos]; pos += h->nb;
    h->A = &L[pos]; pos += h->na;
    h->D = &L[pos]; pos += h->nd;
    h->I = &L[pos]; pos += h->ni;
  }
  return 1;
bad:
  hd_free(p);
  return 0;
}

/* window = seg0 + seg1 + seg2 (pointers to line arrays) */
typedef struct { const HdLine *s[3]; size_t n[3]; } HdWin;

static int hd_match_at(const HdLine *lines, size_t count, size_t at, const HdWin *w)
{
  size_t i = at;
  for (int s = 0; s < 3; s++)
    for (size_t k = 0; k < w->n[s]; k++, i++)
      if (i >= count || !hd_eq(&lines[i], &w->s[s][k])) return 0;
  return 1;
}

/* number of positions (0, 1 or 2 = "more than one") where w starts; first one in *at */
static int hd_find(const HdLine *lines, size_t count, const HdWin *w, size_t *at)
{
  size_t total = w->n[0] + w->n[1] + w->n[2];
  if (total == 0 || total > count) return 0;
  int hits = 0;
  for (size_t i = 0; i + total <= count; i++) {
    if (hd_match_at(lines, count, i, w)) {
      if (hits == 0) *at = i;
      if (++hits >= 2) break;
    }
  }
  return hits;
}

/* insertion point of hunk h in lines, or (size_t) -1 */
static size_t hd_locate(const HdLine *lines, size_t count, const HdHunk *h)
{
  HdWin tries[3];
  size_t offs[3];
  int nt = 0;
  if (h->kbc || h->kac) {
    /* at least one line on each side (when there is one): the spot must still be "between" the same lines */
    int kb = h->kbc > 0 ? h->kbc : (h->nb > 0), ka = h->kac > 0 ? h->kac : (h->na > 0);
    tries[nt] = (HdWin) { { h->B + (h->nb - kb), h->D, h->A }, { (size_t) kb, (size_t) h->nd, (size_t) ka } };
    offs[nt++] = (size_t) kb;
  }
  /* after-context before before-context: when another mod inserted lines at the same spot (typically right after
   * a block's opening line), ours go after them, whichever of the two mods patched first */
  if (h->kao) {
    tries[nt] = (HdWin) { { h->B, h->D, h->A }, { 0, (size_t) h->nd, (size_t) h->kao } };
    offs[nt++] = 0;
  }
  if (h->kbo) {
    tries[nt] = (HdWin) { { h->B + (h->nb - h->kbo), h->D, h->A }, { (size_t) h->kbo, (size_t) h->nd, 0 } };
    offs[nt++] = (size_t) h->kbo;
  }
  for (int t = 0; t < nt; t++) {
    size_t at = 0;
    if (hd_find(lines, count, &tries[t], &at) == 1) return at + offs[t];
  }
  return (size_t) -1;
}

static int hd_contains(const char *text, size_t len, const char *s)
{
  size_t n = strlen(s);
  for (size_t i = 0; i + n <= len; i++) if (text[i] == s[0] && memcmp(text + i, s, n) == 0) return 1;
  return 0;
}

/* patched copy of text (malloc'ed, *out_len bytes, not NUL terminated) or NULL with *why set */
static char *hd_apply(const char *text, size_t len, const HdPatch *p, size_t *out_len, const char **why)
{
  static char reason[64];
  if (hd_contains(text, len, HD_MARKER)) { *why = "already has the Historical doctrine"; return NULL; }
  int crlf = hd_contains(text, len, "\r\n");
  size_t count = 0;
  HdLine *lines = hd_split(text, len, &count);
  size_t *spot = (size_t *) malloc((p->nh ? p->nh : 1) * sizeof *spot);
  char *out = NULL;
  if (lines == NULL || spot == NULL) { *why = "out of memory"; goto done; }
  for (int k = 0; k < p->nh; k++) {
    spot[k] = hd_locate(lines, count, &p->h[k]);
    if (spot[k] == (size_t) -1) {
      snprintf(reason, sizeof reason, "hunk %d not found", k + 1);
      *why = reason;
      goto done;
    }
    if (k > 0 && spot[k] < spot[k - 1] + (size_t) p->h[k - 1].nd) {
      snprintf(reason, sizeof reason, "hunk %d out of order", k + 1);
      *why = reason;
      goto done;
    }
  }
  /* output size */
  size_t eol = crlf ? 2 : 1, total = 0, nlines = 0, cur = 0;
  for (int k = 0; k <= p->nh; k++) {
    size_t end = k < p->nh ? spot[k] : count;
    for (size_t i = cur; i < end; i++) { total += lines[i].n; nlines++; }
    if (k == p->nh) break;
    for (int i = 0; i < p->h[k].ni; i++) { total += p->h[k].I[i].n; nlines++; }
    cur = spot[k] + (size_t) p->h[k].nd;
  }
  total += (nlines ? nlines - 1 : 0) * eol;
  out = (char *) malloc(total ? total : 1);
  if (out == NULL) { *why = "out of memory"; goto done; }
  char *o = out;
  size_t written = 0;
#define HD_PUT(line) do { if (written++) { if (crlf) *o++ = '\r'; *o++ = '\n'; } \
                          memcpy(o, (line).p, (line).n); o += (line).n; } while (0)
  cur = 0;
  for (int k = 0; k <= p->nh; k++) {
    size_t end = k < p->nh ? spot[k] : count;
    for (size_t i = cur; i < end; i++) HD_PUT(lines[i]);
    if (k == p->nh) break;
    for (int i = 0; i < p->h[k].ni; i++) HD_PUT(p->h[k].I[i]);
    cur = spot[k] + (size_t) p->h[k].nd;
  }
#undef HD_PUT
  *out_len = (size_t) (o - out);
done:
  free(lines);
  free(spot);
  return out;
}

#endif
