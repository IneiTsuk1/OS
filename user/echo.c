#include "syscall.h"
#include "stdio.h"
#include "string.h"

// echo [-n] [string...]
//
// Prints its arguments separated by spaces, followed by a newline.
// -n: suppress the trailing newline.
// With no arguments: prints an empty line (or nothing with -n).

void _start(int argc, const char** argv)
{
    int no_newline = 0;
    int start = 1;

    // Check for -n flag
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        no_newline = 1;
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        if (i > start)
            sys_fwrite(STDOUT_FILENO, " ", 1);
        int len = (int)strlen(argv[i]);
        sys_fwrite(STDOUT_FILENO, argv[i], (unsigned int)len);
    }

    if (!no_newline)
        sys_fwrite(STDOUT_FILENO, "\n", 1);

    sys_exit(0);
}