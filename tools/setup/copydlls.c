/* copydlls - run by the build (Windows): copies the DLLs cm9798.exe and cm9798setup.exe need
 * into the build folder, so it can be zipped up and run on any PC (see dlls.c).
 *   copydlls <program.exe> [<program.exe> ...]   (into each program's own folder)
 */
#include "dlls.h"
#include <stdio.h>
#include <string.h>
static void say(const char *t) { fputs(t, stdout); }
int main(int argc, char **argv) {
    int n = 0;
    for (int i = 1; i < argc; i++) {
        char dir[1300]; snprintf(dir, sizeof dir, "%s", argv[i]);
        char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\'), *s = a > b ? a : b;
        if (s) *s = 0; else snprintf(dir, sizeof dir, ".");
        n += copy_program_dlls(argv[i], dir, say);
    }
    if (n) printf("copydlls: %d DLL%s copied next to the program\n", n, n == 1 ? "" : "s");
    return 0;
}
