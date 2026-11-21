/*
 * Historical Doctrine - Gates of Hell mod DLL (goh_mods\historical_doctrine.dll)
 * ==============================================================================
 *
 * DLL edition of the "Historical Doctrine for Skirmishes" Workshop mod.  Instead of shipping whole copies of the
 * 20 skirmish missions (which replace any other mod's copies), it adds the Historical doctrine to whatever
 * version of each mission the game reads: vanilla, Spawn Menu, or another mod's.
 *
 * The patches are the files goh_mods\historical_doctrine\*.hdp (made by data/make_hd_data.py from the Workshop
 * mod): for each mission file, the lines to insert and the vanilla lines around each spot.  They are applied in
 * memory by hd_patch.h when the game's text reader (eSDLReader) has read the file and before it parses it, through
 * the shared hook of common/goh_sdl_hook.h (chained with the other mods that use it, e.g. Coop Checkpoint 1.0.3+).
 * If one spot can't be found the file is left as it is, never half patched.  A file that already has the doctrine
 * (the Workshop mod) is left alone too.
 *
 * Patched always, single player and coop, by the game and by the host's local dedicated server: every peer
 * patches the same files the same way.  EVERY player of a coop match needs this DLL and the historical_doctrine
 * data mod (texts), as with any mod that changes the missions.
 *
 * Log: %TEMP%\goh_historical_doctrine_<exe name>.log
 */
#define GOH_MOD_NAME "historical_doctrine"
#define GOH_MOD_VERSION "1.0.0"   /* this mod's own version (release tag <mod-id>-v<version>) */
#include "../../common/goh_common.h"
#include "../../common/goh_sdl_hook.h"
#include "hd_patch.h"

/* ------------------------------------------------------------------ patch files */
typedef struct {
  char path[MAX_PATH];     /* .hdp file */
  char target[300];        /* map/single/.../0.mi, lower case */
  HdPatch patch;
  int loaded, bad;
} HdEntry;

static HdEntry *g_entries;
static int g_nentries;
static CRITICAL_SECTION g_cs;

static char *read_file(const char *path, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = n >= 0 ? (char *) malloc((size_t) n + 1) : NULL;
  if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) { free(buf); buf = NULL; }
  fclose(f);
  if (buf) { buf[n] = 0; *len = (size_t) n; }
  return buf;
}

static int load_index(void)
{
  char dir[MAX_PATH];
  HMODULE self = NULL;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR) (void *) load_index, &self)) return 0;
  DWORD n = GetModuleFileNameA(self, dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return 0;
  char *slash = strrchr(dir, '\\');
  if (slash == NULL) return 0;
  snprintf(slash + 1, (size_t) (dir + MAX_PATH - slash - 1), "historical_doctrine");
  char pattern[MAX_PATH + 8];
  snprintf(pattern, sizeof pattern, "%s\\*.hdp", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) { logf_("ERROR: no patches in %s", dir); return 0; }
  int cap = 64;
  g_entries = (HdEntry *) calloc((size_t) cap, sizeof *g_entries);
  do {
    if (g_nentries == cap) {
      cap *= 2;
      g_entries = (HdEntry *) realloc(g_entries, (size_t) cap * sizeof *g_entries);
      memset(g_entries + g_nentries, 0, (size_t) (cap - g_nentries) * sizeof *g_entries);
    }
    HdEntry *e = &g_entries[g_nentries];
    snprintf(e->path, sizeof e->path, "%s\\%s", dir, fd.cFileName);
    /* the target is on the second line */
    FILE *f = fopen(e->path, "rb");
    char line[400];
    int ok = 0;
    if (f) {
      if (fgets(line, sizeof line, f) && strncmp(line, "HDP 1", 5) == 0 && fgets(line, sizeof line, f) &&
          strncmp(line, "target ", 7) == 0) {
        size_t k = strcspn(line + 7, "\r\n");
        if (k > 0 && k < sizeof e->target) { memcpy(e->target, line + 7, k); e->target[k] = 0; ok = 1; }
      }
      fclose(f);
    }
    if (ok) g_nentries++;
    else logf_("ERROR: not a patch file: %s", e->path);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  logf_("%d mission patches in %s", g_nentries, dir);
  return g_nentries > 0;
}

/* entry whose target is a path suffix of p (p lower case, forward slashes), or NULL */
static HdEntry *find_entry(const char *p)
{
  size_t n = strlen(p);
  for (int i = 0; i < g_nentries; i++) {
    size_t t = strlen(g_entries[i].target);
    if (t <= n && strcmp(p + n - t, g_entries[i].target) == 0 && (t == n || p[n - t - 1] == '/'))
      return &g_entries[i];
  }
  return NULL;
}

static HdPatch *get_patch(HdEntry *e)
{
  if (!e->loaded && !e->bad) {
    size_t len = 0;
    char *buf = read_file(e->path, &len);
    if (buf && hd_parse(buf, len, &e->patch)) e->loaded = 1;
    else { e->bad = 1; logf_("ERROR: cannot read the patch %s", e->path); }
  }
  return e->loaded ? &e->patch : NULL;
}

/* ------------------------------------------------------------------ SDL reader hook (common/goh_sdl_hook.h) */
static volatile LONG g_patched, g_skipped;

static void on_sdl_text(const char *raw, MsvcString *text)
{
  char low[512];
  size_t n = strlen(raw);
  if (n >= sizeof low) return;
  for (size_t i = 0; i <= n; i++) {
    char c = raw[i];
    low[i] = (char) (c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  HdEntry *e = find_entry(low);
  if (e == NULL) return;
  EnterCriticalSection(&g_cs);
  HdPatch *p = get_patch(e);
  const char *data;
  size_t len;
  goh_sdl_get_text(text, &data, &len);
  const char *why = "patch file unreadable";
  size_t out_len = 0;
  char *patched = p ? hd_apply(data, len, p, &out_len, &why) : NULL;
  if (patched) {
    if (goh_sdl_set_text(text, patched, out_len)) {
      LONG k = InterlockedIncrement(&g_patched);
      logf_("patched '%s': %llu -> %llu bytes [#%ld]", raw, (unsigned long long) len,
            (unsigned long long) out_len, (long) k);
    } else {
      logf_("ERROR: '%s': string assign failed (size %llu)", raw, (unsigned long long) text->size);
    }
    free(patched);
  } else {
    InterlockedIncrement(&g_skipped);
    logf_("'%s' left as is: %s", raw, why);
  }
  LeaveCriticalSection(&g_cs);
}

static void mod_start(int kind)
{
  (void) kind;
  InitializeCriticalSection(&g_cs);
  if (!load_index()) { logf_("ERROR: no mission patches: Historical doctrine disabled"); return; }
  if (!goh_sdl_hook(on_sdl_text)) logf_("ERROR: SDL reader not hooked: Historical doctrine disabled");
}

GOH_MOD_ENTRY()
