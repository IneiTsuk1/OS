#include "syscall.h"

static void print_int(int n)
{
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (n == 0) { sys_write("0"); return; }
    int neg = (n < 0);
    if (neg) n = -n;
    while (n > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    if (neg) buf[--i] = '-';
    sys_write(buf + i);
}

void _start(int argc, const char** argv)
{
    sys_write("Hello from user space!\n");

    sys_write("argc: ");
    print_int(argc);
    sys_write("\n");

    for (int i = 0; i < argc; i++) {
        sys_write("argv[");
        print_int(i);
        sys_write("]: ");
        sys_write(argv[i]);
        sys_write("\n");
    }

    sys_exit(0);
}