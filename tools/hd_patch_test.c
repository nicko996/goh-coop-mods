/* hd_patch_test <patch.hdp> <input> <output>: applies the patch with hd_patch.h (the DLL's code).
 * Exit 0 = patched (output written), 2 = left as is (reason on stdout), 1 = error.  Used by test_hd_patch.py. */
#include "../mods/historical-doctrine/hd_patch.h"

static char *slurp(const char *path, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *) malloc((size_t) n + 1);
  if (b && fread(b, 1, (size_t) n, f) != (size_t) n) { free(b); b = NULL; }
  fclose(f);
  if (b) { b[n] = 0; *len = (size_t) n; }
  return b;
}

int main(int argc, char **argv)
{
  if (argc != 4) { fprintf(stderr, "usage: hd_patch_test <patch> <input> <output>\n"); return 1; }
  size_t plen = 0, ilen = 0;
  char *pbuf = slurp(argv[1], &plen), *in = slurp(argv[2], &ilen);
  if (!pbuf || !in) { fprintf(stderr, "cannot read\n"); return 1; }
  HdPatch p;
  if (!hd_parse(pbuf, plen, &p)) { fprintf(stderr, "bad patch\n"); return 1; }
  size_t olen = 0;
  const char *why = NULL;
  char *out = hd_apply(in, ilen, &p, &olen, &why);
  if (!out) { printf("%s\n", why ? why : "?"); return 2; }
  FILE *f = fopen(argv[3], "wb");
  if (!f) return 1;
  fwrite(out, 1, olen, f);
  fclose(f);
  return 0;
}
