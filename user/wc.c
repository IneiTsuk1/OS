#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stddef.h>

// wc [file...]
//
// Counts lines, words, and bytes.
// With no arguments: reads from stdin.
// With arguments: counts each file and prints a total if more than one.
// Output format: "%7d %7d %7d %s\n"  (lines, words, bytes, name)

#define BUF_SIZE 512

typedef struct {
    int lines;
    int words;
    int bytes;
} counts_t;

static void count_fd(int fd, counts_t* c)
{
    char buf[BUF_SIZE];
    int  n;
    int  in_word = 0;

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            c->bytes++;

            if (ch == '\n')
                c->lines++;

            // Word boundary: whitespace ends a word, non-whitespace starts one
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                in_word = 0;
            } else {
                if (!in_word) {
                    c->words++;
                    in_word = 1;
                }
            }
        }
    }
}

static void print_counts(const counts_t* c, const char* name)
{
    printf("%7d %7d %7d", c->lines, c->words, c->bytes);
    if (name)
        printf(" %s", name);
    printf("\n");
}

void _start(int argc, const char** argv)
{
    if (argc <= 1) {
        // No arguments — count stdin
        counts_t c = {0, 0, 0};
        count_fd(STDIN_FILENO, &c);
        print_counts(&c, NULL);
        sys_exit(0);
    }

    counts_t total = {0, 0, 0};
    int multiple = (argc > 2);

    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(STDERR_FILENO, "wc: %s: cannot open (err %d)\n", argv[i], fd);
            continue;
        }

        counts_t c = {0, 0, 0};
        count_fd(fd, &c);
        sys_close(fd);

        print_counts(&c, argv[i]);

        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }

    if (multiple)
        print_counts(&total, "total");

    sys_exit(0);
}