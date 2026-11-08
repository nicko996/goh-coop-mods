/* Offline check of mods/coop-checkpoint/mission_patch.h: patches every file given on the command line
 * (in -> in.c_patched) so tools/test_mission_patch.py can compare it with the Python generator.
 * Build: zig cc -O2 -o test_mission_patch.exe tools/test_mission_patch.c */
#include "../mods/coop-checkpoint/mission_patch.h"

int main(int argc, char **argv)
{
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) { printf("cannot open %s\n", argv[i]); bad++; continue; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = (char *) malloc((size_t) n + 1);
    fread(t, 1, (size_t) n, f); fclose(f); t[n] = 0;
    size_t on = 0; const char *why = NULL;
    char *o = mp_patch(t, (size_t) n, &on, &why);
    char out[1024]; snprintf(out, sizeof out, "%s.c_patched", argv[i]);
    f = fopen(out, "wb");
    if (o) fwrite(o, 1, on, f); else fprintf(f, "NULL: %s", why);
    fclose(f);
    free(o); free(t);
  }
  return bad;
}
