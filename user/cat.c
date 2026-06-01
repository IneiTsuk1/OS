#include "syscall.h"
#include "stdio.h"
#include "string.h"

// cat [file...]
//
// With no arguments: reads from stdin and echoes to stdout (until EOF / Ctrl+D).
// With arguments: opens each file and writes its contents to stdout.
// Matches the behaviour of POSIX cat for the common cases.

#define BUF_SIZE 512

static void cat_fd(int fd)
{
    char buf[BUF_SIZE];
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_fwrite(STDOUT_FILENO, buf, (unsigned int)n);
}

static void cat_file(const char* path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(STDERR_FILENO, "cat: %s: cannot open (err %d)\n", path, fd);
        return;
    }
    cat_fd(fd);
    sys_close(fd);
}

void _start(int argc, const char** argv)
{
    if (argc <= 1) {
        // No arguments — read from stdin
        cat_fd(STDIN_FILENO);
    } else {
        for (int i = 1; i < argc; i++)
            cat_file(argv[i]);
    }
    sys_exit(0);
}