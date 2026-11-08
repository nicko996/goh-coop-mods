/* Coop Checkpoint: the mission-script patch, applied in memory to every map/single/.../0.mi the game reads.
 *
 * Same output as patch() in data/make_coop_checkpoint_mod.py (tools/test_mission_patch.c checks it byte for
 * byte on every mission): event declarations in {Helpers}, the Save / Reload entries at the top of
 * {reinforcements}, the [coop_ckpt] triggers at the top of {triggers}, and every {"autosave"} of the mission
 * wrapped with the "restored" marks.  Because the patch is applied to whatever file the game loaded (vanilla or
 * another mod's), the buttons work together with any mod that replaces the missions.
 *
 * Plain C (no Windows headers): mp_patch() returns a malloc'ed buffer or NULL when the text is left alone. */
#ifndef COOP_CKPT_MISSION_PATCH_H
#define COOP_CKPT_MISSION_PATCH_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_MOD_NAME "coop_checkpoint"
#define MP_INIT_DELAY 1
#define MP_BUTTON_COOLDOWN 10
#define MP_LAST_SLOT 17
#define MP_EV_CLICK "[coop_ckpt]/click_save"
#define MP_EV_RELOAD "[coop_ckpt]/click_reload"
#define MP_EV_RESTORED "[coop_ckpt]/restored"
#define MP_TAG_RESTORED "_coop_ckpt_restored"

typedef struct { char *p; size_t n, cap; int oom; } MpBuf;

static void mp_put(MpBuf *b, const char *s, size_t n)
{
  if (b->oom) return;
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (b->n + n + 1 > cap) cap *= 2;
    char *p = (char *) realloc(b->p, cap);
    if (p == NULL) { b->oom = 1; return; }
    b->p = p; b->cap = cap;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void mp_puts(MpBuf *b, const char *s) { mp_put(b, s, strlen(s)); }

/* one line: depth tabs + printf text + CRLF */
static void mp_line(MpBuf *b, int depth, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  for (int i = 0; i < depth; i++) mp_put(b, "\t", 1);
  mp_puts(b, tmp);
  mp_put(b, "\r\n", 2);
}

static void mp_event_action(MpBuf *b, int d, const char *name, const char *mode)
{
  mp_line(b, d, "{\"event\"");
  mp_line(b, d + 1, "{event \"%s\"}", name);
  mp_line(b, d + 1, "{mode %s}", mode);
  mp_line(b, d + 1, "{delay 0}");
  mp_line(b, d, "}");
}

static void mp_tag_players(MpBuf *b, int d, int first, const char *op)
{
  for (int slot = first; slot <= MP_LAST_SLOT; slot++) {
    mp_line(b, d, "{\"entity_state\"");
    mp_line(b, d + 1, "{selector");
    mp_line(b, d + 2, "{source standart}");
    mp_line(b, d + 2, "{ignore_captured_by_user 0}");
    mp_line(b, d + 2, "{player \"%d\"}", slot);
    mp_line(b, d + 2, "{state \"not dead\"}");
    mp_line(b, d + 1, "}");
    mp_line(b, d + 1, "{%s %s}", op, MP_TAG_RESTORED);
    mp_line(b, d, "}");
  }
}

/* {"autosave"} wrapped so that only the checkpoint carries the "restored" marks (event + unit tag) */
static void mp_autosave_actions(MpBuf *b, int d, int first)
{
  mp_event_action(b, d, MP_EV_RESTORED, "set");
  mp_tag_players(b, d, first, "tag_add");
  mp_line(b, d, "{\"autosave\"}");
  mp_tag_players(b, d, first, "tag_remove");
  mp_event_action(b, d, MP_EV_RESTORED, "reset");
}

static void mp_event_declarations(MpBuf *b)
{
  mp_line(b, 2, "{event \"%s\"}", MP_EV_CLICK);
  mp_line(b, 2, "{event \"%s\"}", MP_EV_RELOAD);
  mp_line(b, 2, "{event \"%s\"}", MP_EV_RESTORED);
}

static void mp_reinforcement_entries(MpBuf *b)
{
  static const char *keys[2] = { "save", "reload" };
  static const char *evs[2] = { MP_EV_CLICK, MP_EV_RELOAD };
  for (int i = 0; i < 2; i++) {
    mp_line(b, 3, "{\"coop_ckpt_%s\"", keys[i]);
    mp_line(b, 4, "{state hidden}");
    mp_line(b, 4, "{text \"mod/%s/%s\"}", MP_MOD_NAME, keys[i]);
    mp_line(b, 4, "{image coop_ckpt_%s}", keys[i]);
    mp_line(b, 4, "{hint \"mod/%s/%s_hint\"}", MP_MOD_NAME, keys[i]);
    mp_line(b, 4, "{count -1}");
    mp_line(b, 4, "{score 0}");
    mp_line(b, 4, "{delay %d}", MP_BUTTON_COOLDOWN);
    mp_line(b, 4, "{eventStart \"%s\"}", evs[i]);
    mp_line(b, 3, "}");
  }
}

static void mp_triggers(MpBuf *b, int first)
{
  const int d = 3;
  mp_line(b, d, "{\"[coop_ckpt]/init\"");
  mp_line(b, d + 1, "{condition");
  mp_line(b, d + 1, "}");
  mp_line(b, d + 1, "{actions");
  mp_line(b, d + 2, "{\"delay\"");
  mp_line(b, d + 3, "{time %d}", MP_INIT_DELAY);
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"reinforcement\"");
  mp_line(b, d + 3, "{operation show}");
  mp_line(b, d + 3, "{name coop_ckpt_save}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"reinforcement\"");
  mp_line(b, d + 3, "{operation show}");
  mp_line(b, d + 3, "{name coop_ckpt_reload}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d, "}");

  mp_line(b, d, "{\"[coop_ckpt]/click_save\"");
  mp_line(b, d + 1, "{condition");
  mp_line(b, d + 2, "{terms");
  mp_line(b, d + 3, "{\"1.event\"");
  mp_line(b, d + 4, "{id \"%s\"}", MP_EV_CLICK);
  mp_line(b, d + 3, "}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d + 1, "{actions");
  mp_event_action(b, d + 2, MP_EV_CLICK, "reset");
  mp_line(b, d + 2, "{\"trigger\"");
  mp_line(b, d + 3, "{name \"[coop_ckpt]/click_save\"}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"sound\"");
  mp_line(b, d + 3, "{name \"interface/message\"}");
  mp_line(b, d + 2, "}");
  mp_autosave_actions(b, d + 2, first);
  mp_line(b, d + 1, "}");
  mp_line(b, d, "}");

  /* reload request: an achievement command, which the game already forwards to every client; "name" must be
   * a real achievement, "nation" is a free string the DLL recognises */
  mp_line(b, d, "{\"[coop_ckpt]/click_reload\"");
  mp_line(b, d + 1, "{condition");
  mp_line(b, d + 2, "{terms");
  mp_line(b, d + 3, "{\"1.event\"");
  mp_line(b, d + 4, "{id \"%s\"}", MP_EV_RELOAD);
  mp_line(b, d + 3, "}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d + 1, "{actions");
  mp_line(b, d + 2, "{\"achievement\"");
  mp_line(b, d + 3, "{name rus_skirmish1_easy}");
  mp_line(b, d + 3, "{nation coop_ckpt_reload}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"sound\"");
  mp_line(b, d + 3, "{name \"interface/message\"}");
  mp_line(b, d + 2, "}");
  mp_event_action(b, d + 2, MP_EV_RELOAD, "reset");
  mp_line(b, d + 2, "{\"delay\"");
  mp_line(b, d + 3, "{time 0.5}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"trigger\"");
  mp_line(b, d + 3, "{name \"[coop_ckpt]/click_reload\"}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d, "}");

  /* after a reload: the units of the coop player slots are autoassigned again */
  mp_line(b, d, "{\"[coop_ckpt]/after_reload\"");
  mp_line(b, d + 1, "{condition");
  mp_line(b, d + 2, "{expression \"1 | 2\"}");
  mp_line(b, d + 2, "{terms");
  mp_line(b, d + 3, "{\"1.event\"");
  mp_line(b, d + 4, "{id \"%s\"}", MP_EV_RESTORED);
  mp_line(b, d + 3, "}");
  mp_line(b, d + 3, "{\"2.entities\"");
  mp_line(b, d + 4, "{selector");
  mp_line(b, d + 5, "{tag %s}", MP_TAG_RESTORED);
  mp_line(b, d + 4, "}");
  mp_line(b, d + 4, "{count");
  mp_line(b, d + 5, "{op \">\"}");
  mp_line(b, d + 5, "{value 0}");
  mp_line(b, d + 4, "}");
  mp_line(b, d + 3, "}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d + 1, "{actions");
  mp_event_action(b, d + 2, MP_EV_RESTORED, "reset");
  mp_line(b, d + 2, "{\"entity_state\"");
  mp_line(b, d + 3, "{selector");
  mp_line(b, d + 4, "{source standart}");
  mp_line(b, d + 4, "{tag %s}", MP_TAG_RESTORED);
  mp_line(b, d + 3, "}");
  mp_line(b, d + 3, "{tag_remove %s}", MP_TAG_RESTORED);
  mp_line(b, d + 2, "}");
  mp_line(b, d + 2, "{\"delay\"");
  mp_line(b, d + 3, "{time 1}");
  mp_line(b, d + 2, "}");
  for (int slot = first; slot <= MP_LAST_SLOT; slot++) {
    mp_line(b, d + 2, "{\"autoassign\"");
    mp_line(b, d + 3, "{selector");
    mp_line(b, d + 4, "{ignore_captured_by_user 0}");
    mp_line(b, d + 4, "{player \"%d\"}", slot);
    mp_line(b, d + 4, "{state \"not dead\"}");
    mp_line(b, d + 3, "}");
    mp_line(b, d + 2, "}");
    mp_line(b, d + 2, "{\"delay\"");
    mp_line(b, d + 3, "{time 0.1}");
    mp_line(b, d + 2, "}");
  }
  mp_line(b, d + 2, "{\"trigger\"");
  mp_line(b, d + 3, "{name \"[coop_ckpt]/after_reload\"}");
  mp_line(b, d + 2, "}");
  mp_line(b, d + 1, "}");
  mp_line(b, d, "}");
}

static const char *mp_find(const char *s, const char *end, const char *needle)
{
  size_t n = strlen(needle);
  for (const char *p = s; p + n <= end; p++)
    if (*p == *needle && memcmp(p, needle, n) == 0) return p;
  return NULL;
}

static int mp_digits(const char *p, const char *end, int *value, const char **after)
{
  if (p >= end || *p < '0' || *p > '9') return 0;
  int v = 0;
  while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  *value = v;
  *after = p;
  return 1;
}

/* First scene player id of the humans, as the game computes it (after the highest owner of any actor:
 * "\n\t\t{Player N}"), else after the highest {alias} of the {Players} block, else 4. */
static int mp_first_coop_player(const char *t, const char *end)
{
  int best = -1, v;
  const char *after;
  for (const char *p = t; (p = mp_find(p, end, "\n\t\t{Player ")) != NULL; p++)
    if (mp_digits(p + 11, end, &v, &after) && after < end && *after == '}' && v > best) best = v;
  if (best >= 0) return best + 1;
  const char *pl = mp_find(t, end, "\n\t{Players\r\n");
  if (pl) {
    const char *body = pl + 12, *close = mp_find(body, end, "\r\n\t}");
    if (close) {
      for (const char *p = body; (p = mp_find(p, close, "{alias ")) != NULL; p++)
        if (mp_digits(p + 7, close, &v, &after) && after < close && *after == ' ' && v > best) best = v;
    }
  }
  return best >= 0 ? best + 1 : 4;
}

/* Insert text after the first occurrence of needle (count=1 semantics of the Python re.subn). */
static int mp_insert_after(MpBuf *out, const char *needle, size_t skip_extra(const char *, const char *),
                           void (*gen)(MpBuf *, int), int arg)
{
  const char *t = out->p, *end = out->p + out->n;
  const char *at = mp_find(t, end, needle);
  if (at == NULL) return 0;
  size_t pos = (size_t) (at - t) + strlen(needle);
  if (skip_extra) pos += skip_extra(t + pos, end);
  MpBuf ins = { 0 };
  gen(&ins, arg);
  if (ins.oom) { free(ins.p); out->oom = 1; return 0; }
  MpBuf res = { 0 };
  mp_put(&res, out->p, pos);
  mp_put(&res, ins.p, ins.n);
  mp_put(&res, out->p + pos, out->n - pos);
  free(ins.p);
  if (res.oom) { free(res.p); out->oom = 1; return 0; }
  free(out->p);
  *out = res;
  return 1;
}

/* optional "\t\t\t{scoreTotal ...}\r\n" then optional "\t\t\t{scoreCurrent ...}\r\n" (no CR/LF inside) */
static size_t mp_skip_one(const char *p, const char *end, const char *head)
{
  size_t n = strlen(head);
  if ((size_t) (end - p) < n || memcmp(p, head, n) != 0) return 0;
  const char *q = p + n;
  while (q < end && *q != '\r' && *q != '\n') q++;
  /* [^\r\n]*\}\r\n : the line must end with "}\r\n"; backtracking means the last '}' before CRLF */
  if (q + 1 < end && q[0] == '\r' && q[1] == '\n' && q > p + n && q[-1] == '}') return (size_t) (q + 2 - p);
  return 0;
}
static size_t mp_skip_scores(const char *p, const char *end)
{
  size_t a = mp_skip_one(p, end, "\t\t\t{scoreTotal ");
  return a + mp_skip_one(p + a, end, "\t\t\t{scoreCurrent ");
}
static void mp_gen_decl(MpBuf *b, int unused) { (void) unused; mp_event_declarations(b); }
static void mp_gen_entries(MpBuf *b, int unused) { (void) unused; mp_reinforcement_entries(b); }
static void mp_gen_triggers(MpBuf *b, int first) { mp_triggers(b, first); }

/* Returns the patched text (malloc, NUL-terminated, *out_len = length) or NULL: already patched, a block is
 * missing (*why says which), or out of memory. */
static char *mp_patch(const char *text, size_t len, size_t *out_len, const char **why)
{
  const char *end = text + len;
  *why = NULL;
  if (mp_find(text, end, "[coop_ckpt]/init")) { *why = "already patched"; return NULL; }
  int first = mp_first_coop_player(text, end);
  /* 1. (\t+)\{"autosave"\}\r\n -> the wrapped actions at the same indentation */
  MpBuf out = { 0 };
  const char *p = text;
  const char *tok = "{\"autosave\"}\r\n";
  for (;;) {
    const char *hit = mp_find(p, end, tok);
    while (hit && !(hit > p && hit[-1] == '\t')) hit = mp_find(hit + 1, end, tok);
    if (hit == NULL) break;
    const char *tabs = hit;
    while (tabs > p && tabs[-1] == '\t') tabs--;
    mp_put(&out, p, (size_t) (tabs - p));
    MpBuf w = { 0 };
    mp_autosave_actions(&w, 0, first);
    /* every generated line gets the original indentation in front */
    size_t ind = (size_t) (hit - tabs);
    for (const char *l = w.p; l && l < w.p + w.n;) {
      const char *nl = mp_find(l, w.p + w.n, "\r\n");
      mp_put(&out, tabs, ind);
      mp_put(&out, l, (size_t) (nl + 2 - l));
      l = nl + 2;
    }
    if (w.oom) out.oom = 1;
    free(w.p);
    p = hit + strlen(tok);
  }
  mp_put(&out, p, (size_t) (end - p));
  if (out.oom) { free(out.p); *why = "out of memory"; return NULL; }
  if (!mp_insert_after(&out, "\n\t{Helpers\r\n", NULL, mp_gen_decl, 0)) { *why = "{Helpers} block not found"; goto fail; }
  if (!mp_insert_after(&out, "\n\t\t{reinforcements\r\n", mp_skip_scores, mp_gen_entries, 0)) {
    *why = "{reinforcements} block not found"; goto fail; }
  if (!mp_insert_after(&out, "\n\t\t{triggers\r\n", NULL, mp_gen_triggers, first)) { *why = "{triggers} block not found"; goto fail; }
  *out_len = out.n;
  return out.p;
fail:
  if (out.oom && *why == NULL) *why = "out of memory";
  free(out.p);
  return NULL;
}

#endif
