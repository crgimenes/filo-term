/* appvm: the names a program's binary gives its bundle, as the profile
   filo check -vm reads. Linked with the program's own sources, it lists
   app_program's context, so the check and the binary cannot disagree.
   usage: appvm > build/NAME.vm */
#include <stdio.h>
#include <string.h>

#include "app.h"

static app A; /* the arenas are not for the stack */
static char text[64 * 1024];

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fputs("usage: appvm > NAME.vm\n"
              "\n"
              "Writes the builtins, then the globals, that this program's binary\n"
              "gives its bundle: the profile filo check -vm holds the bundle to.\n"
              "\n"
              "Example: build/appvm > build/edt.vm && filo check -vm build/edt.vm edt.fbb\n",
              stdout);
        return 0;
    }
    char why[256];
    if (!app_context(&A, &app_program, why, sizeof(why))) {
        fputs("appvm: ", stderr);
        fputs(why, stderr);
        fputs("\n", stderr);
        return 1;
    }
    if (app_profile(&A.ctx, text, sizeof(text)) == 0) {
        fputs("appvm: the profile does not fit\n", stderr);
        return 1;
    }
    fputs("# what ", stdout);
    fputs(app_program.name, stdout);
    fputs(" gives a bundle: builtins, then globals\n", stdout);
    fputs(text, stdout);
    return 0;
}
