#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

/* ---- helpers -------------------------------------------------------------- */

/* Print a horizontal rule */
static void rule(void)
{
    printf("----------------------------------------\n");
}

/* ---- file I/O test -------------------------------------------------------- */

static void test_file_io(void)
{
    rule();
    printf("Test 1: write a file\n");

    int fd = sys_open("/hello2.txt", O_WRONLY | O_CREAT);
    if (fd < 0) {
        printf("  FAIL: open for write returned %d\n", fd);
        return;
    }

    const char* msg = "Hello from hello2!\nThis file was written by a user program.\n";
    int n = sys_fwrite(fd, msg, (unsigned int)strlen(msg));
    printf("  wrote %d bytes to /hello2.txt (fd=%d)\n", n, fd);
    sys_close(fd);

    rule();
    printf("Test 2: read the file back\n");

    fd = sys_open("/hello2.txt", O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: open for read returned %d\n", fd);
        return;
    }

    char buf[128];
    int total = 0;
    int r;
    while ((r = sys_read(fd, buf + total, (unsigned int)(sizeof(buf) - 1 - total))) > 0)
        total += r;
    buf[total] = '\0';
    sys_close(fd);

    printf("  read %d bytes:\n", total);
    printf("  [%s]\n", buf);
}

/* ---- stat test ------------------------------------------------------------ */

static void test_stat(void)
{
    rule();
    printf("Test 3: stat\n");

    sys_stat_t st;
    int r = sys_stat("/hello2.txt", &st);
    if (r < 0) {
        printf("  FAIL: stat returned %d\n", r);
        return;
    }

    printf("  /hello2.txt: size=%u is_dir=%u\n", st.size, st.is_dir);

    r = sys_stat("/", &st);
    if (r == 0)
        printf("  /: size=%u is_dir=%u\n", st.size, st.is_dir);
}

/* ---- malloc test ---------------------------------------------------------- */

static void test_malloc(void)
{
    rule();
    printf("Test 4: malloc / free\n");

    /* Allocate a few buffers and write to them */
    char* a = (char*)malloc(64);
    char* b = (char*)malloc(128);
    char* c = (char*)malloc(32);

    if (!a || !b || !c) {
        printf("  FAIL: malloc returned NULL\n");
        return;
    }

    /* Use strcpy/sprintf from the CRT */
    strcpy(a, "buffer A");
    sprintf(b, "buffer B has %d bytes allocated", 128);
    strcpy(c, "buffer C");

    printf("  a = \"%s\"\n", a);
    printf("  b = \"%s\"\n", b);
    printf("  c = \"%s\"\n", c);

    free(b);

    /* Allocate again — should reuse freed block */
    char* b2 = (char*)malloc(64);
    if (!b2) {
        printf("  FAIL: second malloc returned NULL\n");
    } else {
        strcpy(b2, "reused block");
        printf("  b2 = \"%s\" (reused freed slot)\n", b2);
        free(b2);
    }

    free(a);
    free(c);
    printf("  all blocks freed OK\n");
}

/* ---- seek test ------------------------------------------------------------ */

static void test_seek(void)
{
    rule();
    printf("Test 5: seek\n");

    int fd = sys_open("/hello2.txt", O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: open returned %d\n", fd);
        return;
    }

    /* Seek to byte 6 ("from hello2!...") */
    int pos = sys_seek(fd, 6, SEEK_SET);
    printf("  seeked to pos %d\n", pos);

    char buf[32];
    int n = sys_read(fd, buf, 12);
    buf[n] = '\0';
    printf("  read %d bytes at offset 6: [%s]\n", n, buf);

    sys_close(fd);
}

/* ---- argc/argv test ------------------------------------------------------- */

static void test_args(int argc, const char** argv)
{
    rule();
    printf("Test 6: argc/argv\n");
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
}

/* ---- entry point ---------------------------------------------------------- */

void _start(int argc, const char** argv)
{
    printf("\nhello2 — CRT + file I/O test\n");
    rule();

    test_file_io();
    test_stat();
    test_malloc();
    test_seek();
    test_args(argc, argv);

    rule();
    printf("All tests done.\n\n");
    sys_exit(0);
}