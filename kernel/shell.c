#include "shell.h"
#include "vfs/vfs.h"
#include "klog.h"
#include "../drivers/vga/terminal.h"
#include "../drivers/serial/serial.h"
#include "../drivers/keyboard/keyboard.h"
#include <stdint.h>

#define SHELL_BUF_SIZE   256
#define SHELL_CWD_SIZE   256
#define SHELL_HIST_SIZE  16
#define SHELL_MAX_ARGS   16

// ---- output helpers --------------------------------------------------------

static void sh_puts(const char* s)
{
    terminal_write(s);
    serial_write(s);
}

static void sh_putchar(char c)
{
    char buf[2] = {c, 0};
    terminal_write(buf);
    serial_write(buf);
}

// ---- string helpers --------------------------------------------------------

static int sh_strlen(const char* s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int sh_strcmp(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static void sh_strcpy(char* dst, const char* src)
{
    while ((*dst++ = *src++));
}

static void sh_strcat(char* dst, const char* src)
{
    while (*dst) dst++;
    while ((*dst++ = *src++));
}

static char* sh_strrchr(const char* s, char c)
{
    const char* last = 0;
    while (*s) { if (*s == c) last = s; s++; }
    return (char*)last;
}

// ---- number formatting -----------------------------------------------------

static void sh_print_uint(uint32_t n)
{
    if (n == 0) { sh_putchar('0'); return; }
    char tmp[12]; int i = 0;
    while (n) { tmp[i++] = '0' + n % 10; n /= 10; }
    while (i--) sh_putchar(tmp[i]);
}

// Print n right-aligned in a field of `width` characters.
static void sh_print_uint_w(uint32_t n, int width)
{
    char tmp[12]; int i = 0;
    if (n == 0) tmp[i++] = '0';
    else while (n) { tmp[i++] = '0' + n % 10; n /= 10; }
    for (int p = i; p < width; p++) sh_putchar(' ');
    while (i--) sh_putchar(tmp[i]);
}

// ---- path helpers ----------------------------------------------------------

static char cwd[SHELL_CWD_SIZE] = "/";

// Normalise a path in-place: resolve redundant slashes and `..` components.
// Operates on an absolute path string.
static void sh_normalise(char* path)
{
    // Collapse double slashes and process . / ..
    char out[SHELL_CWD_SIZE];
    int  o = 0;
    int  len = sh_strlen(path);

    out[o++] = '/';

    for (int i = 0; i < len; ) {
        if (path[i] == '/') { i++; continue; }

        // Find end of component
        int j = i;
        while (j < len && path[j] != '/') j++;

        int clen = j - i;

        if (clen == 1 && path[i] == '.') {
            // "." — skip
        } else if (clen == 2 && path[i] == '.' && path[i+1] == '.') {
            // ".." — remove last component from out
            if (o > 1) {
                o--;
                while (o > 1 && out[o-1] != '/') o--;
            }
        } else {
            // Normal component — append
            if (o > 1) out[o++] = '/';
            for (int k = 0; k < clen && o < SHELL_CWD_SIZE - 1; k++)
                out[o++] = path[i + k];
        }

        i = j;
    }

    out[o] = '\0';
    sh_strcpy(path, out);
}

// Resolve `input` relative to cwd into `out` (SHELL_CWD_SIZE bytes).
static void sh_resolve_path(const char* input, char* out)
{
    if (input[0] == '/') {
        sh_strcpy(out, input);
    } else if (input[0] == '\0') {
        sh_strcpy(out, cwd);
    } else {
        sh_strcpy(out, cwd);
        if (out[sh_strlen(out) - 1] != '/')
            sh_strcat(out, "/");
        sh_strcat(out, input);
    }
    sh_normalise(out);
}

// ---- tokeniser -------------------------------------------------------------
//
// Splits `line` into at most SHELL_MAX_ARGS tokens in-place.
// Tokens are separated by spaces. Quoted strings ("foo bar") are treated as
// a single token (quotes stripped). Returns argc.

static int sh_tokenise(char* line, char* argv[], int max_args)
{
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args - 1) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (!*p) break;

        if (*p == '"') {
            // Quoted token
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }

    argv[argc] = 0;
    return argc;
}

// ---- command history -------------------------------------------------------

static char  history[SHELL_HIST_SIZE][SHELL_BUF_SIZE];
static int   hist_count = 0;
static int   hist_head  = 0;

static void hist_push(const char* cmd)
{
    if (!cmd[0]) return;
    if (hist_count > 0) {
        int last = (hist_head - 1 + SHELL_HIST_SIZE) % SHELL_HIST_SIZE;
        if (sh_strcmp(history[last], cmd) == 0) return;
    }
    sh_strcpy(history[hist_head], cmd);
    hist_head = (hist_head + 1) % SHELL_HIST_SIZE;
    if (hist_count < SHELL_HIST_SIZE) hist_count++;
}

static const char* hist_get(int offset)
{
    if (offset < 0 || offset >= hist_count) return 0;
    int idx = (hist_head - 1 - offset + SHELL_HIST_SIZE * 2) % SHELL_HIST_SIZE;
    return history[idx];
}

// ---- line editor -----------------------------------------------------------

static void sh_redraw_tail(const char* buf, int len, int cur)
{
    for (int i = cur; i < len; i++) sh_putchar(buf[i]);
    sh_putchar(' ');
    int back = len - cur + 1;
    for (int i = 0; i < back; i++) { terminal_write("\b"); serial_write("\b"); }
}

static void sh_redraw_full(const char* buf, int new_len, int old_cur, int old_len)
{
    for (int i = 0; i < old_cur; i++) { terminal_write("\b"); serial_write("\b"); }
    for (int i = 0; i < old_len; i++) sh_putchar(' ');
    for (int i = 0; i < old_len; i++) { terminal_write("\b"); serial_write("\b"); }
    for (int i = 0; i < new_len; i++) sh_putchar(buf[i]);
}

static void sh_readline(char* buf, int max)
{
    int len = 0, cur = 0, hist_pos = -1;
    char saved[SHELL_BUF_SIZE];
    saved[0] = buf[0] = '\0';

    while (1) {
        __asm__ volatile ("hlt");
        uint8_t c = (uint8_t)keyboard_getchar();
        if (!c) continue;

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            sh_puts("\n");
            return;
        }
        if (c == 3) {                                       // Ctrl+C
            sh_puts("^C\n");
            buf[0] = '\0';
            return;
        }
        if (c == 21) {                                      // Ctrl+U
            for (int i = 0; i < cur; i++) { terminal_write("\b"); serial_write("\b"); }
            for (int i = 0; i < len; i++) sh_putchar(' ');
            for (int i = 0; i < len; i++) { terminal_write("\b"); serial_write("\b"); }
            len = cur = 0; buf[0] = '\0'; hist_pos = -1;
            continue;
        }
        if (c == 1) {                                       // Ctrl+A
            while (cur > 0) { terminal_write("\b"); serial_write("\b"); cur--; }
            continue;
        }
        if (c == 5) {                                       // Ctrl+E
            while (cur < len) { sh_putchar(buf[cur]); cur++; }
            continue;
        }
        if (c == 8 || c == 127) {                          // Backspace
            if (cur == 0) continue;
            for (int i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
            len--; cur--;
            buf[len] = '\0';
            terminal_write("\b"); serial_write("\b");
            sh_redraw_tail(buf, len, cur);
            continue;
        }
        if (c == KEY_LEFT)   { if (cur > 0)  { terminal_write("\b"); serial_write("\b"); cur--; } continue; }
        if (c == KEY_RIGHT)  { if (cur < len){ sh_putchar(buf[cur]); cur++; } continue; }
        if (c == KEY_HOME)   { while (cur > 0)  { terminal_write("\b"); serial_write("\b"); cur--; } continue; }
        if (c == KEY_END)    { while (cur < len){ sh_putchar(buf[cur]); cur++; } continue; }
        if (c == KEY_DELETE) {
            if (cur >= len) continue;
            for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1];
            len--; buf[len] = '\0';
            sh_redraw_tail(buf, len, cur);
            continue;
        }
        if (c == KEY_UP) {
            int next = hist_pos + 1;
            if (!hist_get(next)) continue;
            if (hist_pos == -1) sh_strcpy(saved, buf);
            hist_pos = next;
            int oc = cur, ol = len;
            sh_strcpy(buf, hist_get(hist_pos));
            len = sh_strlen(buf);
            sh_redraw_full(buf, len, oc, ol);
            cur = len;
            continue;
        }
        if (c == KEY_DOWN) {
            if (hist_pos < 0) continue;
            int oc = cur, ol = len;
            if (hist_pos == 0) {
                hist_pos = -1;
                sh_strcpy(buf, saved);
            } else {
                hist_pos--;
                sh_strcpy(buf, hist_get(hist_pos));
            }
            len = sh_strlen(buf);
            sh_redraw_full(buf, len, oc, ol);
            cur = len;
            continue;
        }
        if (c < 32 || c >= 128) continue;
        if (len >= max - 1) continue;
        for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
        buf[cur] = (char)c;
        len++; cur++;
        buf[len] = '\0';
        sh_putchar(c);
        if (cur < len) sh_redraw_tail(buf, len, cur);
    }
}

// ---- prompt ----------------------------------------------------------------

static void sh_print_prompt(void)
{
    terminal_set_color(0x0A);   // green
    sh_puts("myos");
    terminal_set_color(0x0F);   // white
    sh_puts(":");
    terminal_set_color(0x0B);   // cyan
    sh_puts(cwd);
    terminal_set_color(0x0F);
    sh_puts("$ ");
}

// ---- commands --------------------------------------------------------------

static void cmd_help(void)
{
    sh_puts("Built-in commands:\n");
    sh_puts("  ls [-l] [path]          list directory contents\n");
    sh_puts("  cat <path>              print file contents\n");
    sh_puts("  touch <path>            create empty file\n");
    sh_puts("  mkdir [-p] <path>       create directory\n");
    sh_puts("  rm [-r] <path>          remove file or directory tree\n");
    sh_puts("  rmdir <path>            remove empty directory\n");
    sh_puts("  cp <src> <dst>          copy file\n");
    sh_puts("  mv <src> <dst>          move / rename file or directory\n");
    sh_puts("  echo [text] [> file]    print text or write to file\n");
    sh_puts("  stat <path>             show file information\n");
    sh_puts("  cd [path]               change directory\n");
    sh_puts("  pwd                     print working directory\n");
    sh_puts("  clear                   clear screen\n");
    sh_puts("  help                    show this message\n");
    sh_puts("  exec <path> [args...] [&] load and run ELF32; & = background\n");
    sh_puts("Shortcuts:\n");
    sh_puts("  Up/Down   history    Left/Right  move cursor\n");
    sh_puts("  Home/End  jump       Delete      delete at cursor\n");
    sh_puts("  Ctrl+C    cancel     Ctrl+U      clear line\n");
    sh_puts("  Ctrl+A/E  home/end\n");
}

// ls [-l] [path]
static void cmd_ls(int argc, char* argv[])
{
    int  long_fmt = 0;
    const char* path_arg = "";

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-l") == 0) long_fmt = 1;
        else path_arg = argv[i];
    }

    char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        sh_puts("ls: cannot access '"); sh_puts(path); sh_puts("': no such file or directory\n");
        return;
    }

    vfs_dirent_t ent;
    uint32_t idx = 0;
    while (vfs_readdir(fd, idx++, &ent) == VFS_EOK) {
        if (long_fmt) {
            // type + permissions placeholder + size + name
            if (ent.is_dir) {
                terminal_set_color(0x0B); sh_puts("d");
                terminal_set_color(0x0F); sh_puts("rwxr-xr-x  ");
                sh_print_uint_w(0, 8);
            } else {
                terminal_set_color(0x0F); sh_puts("-");
                sh_puts("rw-r--r--  ");
                sh_print_uint_w(ent.size, 8);
            }
            sh_puts("  ");
        }

        if (ent.is_dir) {
            terminal_set_color(0x0B);   // cyan for directories
            sh_puts(ent.name);
            terminal_set_color(0x0F);
            if (long_fmt) sh_puts("\n"); else sh_puts("  ");
        } else {
            sh_puts(ent.name);
            if (long_fmt) sh_puts("\n"); else sh_puts("  ");
        }
    }

    if (!long_fmt) sh_puts("\n");
    vfs_close(fd);
}

// cat <path> [path2 ...]  — concatenate and print files
static void cmd_cat(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("cat: missing operand\n"); return; }

    for (int i = 1; i < argc; i++) {
        char path[SHELL_CWD_SIZE];
        sh_resolve_path(argv[i], path);

        int fd = vfs_open(path, O_RDONLY);
        if (fd < 0) {
            sh_puts("cat: "); sh_puts(argv[i]); sh_puts(": no such file or directory\n");
            continue;
        }

        char buf[256];
        int n;
        while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            sh_puts(buf);
        }
        vfs_close(fd);
    }
    sh_puts("\n");
}

// touch <path> — create empty file if it doesn't exist
static void cmd_touch(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("touch: missing operand\n"); return; }
    for (int i = 1; i < argc; i++) {
        char path[SHELL_CWD_SIZE];
        sh_resolve_path(argv[i], path);

        // Try opening first — if it exists, nothing to do
        int fd = vfs_open(path, O_RDONLY);
        if (fd >= 0) { vfs_close(fd); continue; }

        // Doesn't exist — create it
        if (vfs_create(path) < 0) {
            sh_puts("touch: cannot create '"); sh_puts(path); sh_puts("'\n");
        }
    }
}

// echo [args...] [> file] — print text or redirect to file
static void cmd_echo(int argc, char* argv[])
{
    // Scan for redirection operator
    int redir = -1;
    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], ">") == 0) { redir = i; break; }
    }

    int fd = -1;
    if (redir >= 0 && redir + 1 < argc) {
        char path[SHELL_CWD_SIZE];
        sh_resolve_path(argv[redir + 1], path);
        vfs_unlink(path);
        if (vfs_create(path) < 0) {
            sh_puts("echo: cannot create '"); sh_puts(path); sh_puts("'\n");
            return;
        }
        fd = vfs_open(path, O_WRONLY);
        if (fd < 0) {
            sh_puts("echo: cannot open '"); sh_puts(path); sh_puts("'\n");
            return;
        }
    }

    int end = (redir >= 0) ? redir : argc;
    for (int i = 1; i < end; i++) {
        if (fd >= 0) {
            vfs_write(fd, argv[i], sh_strlen(argv[i]));
            if (i + 1 < end) vfs_write(fd, " ", 1);
        } else {
            sh_puts(argv[i]);
            if (i + 1 < end) sh_puts(" ");
        }
    }
    if (fd >= 0) { vfs_write(fd, "\n", 1); vfs_close(fd); }
    else sh_puts("\n");
}

// stat <path> — show file/directory info
static void cmd_stat(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("stat: missing operand\n"); return; }

    char path[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1], path);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        sh_puts("stat: cannot access '"); sh_puts(path); sh_puts("': no such file or directory\n");
        return;
    }

    // Get info via readdir on parent
    vfs_dirent_t ent;
    int is_dir = (vfs_readdir(fd, 0, &ent) != VFS_ENOTDIR);
    vfs_close(fd);

    sh_puts("  File: "); sh_puts(path); sh_puts("\n");
    sh_puts("  Type: "); sh_puts(is_dir ? "directory" : "regular file"); sh_puts("\n");

    if (!is_dir) {
        // Reopen to get size from the file handle itself
        fd = vfs_open(path, O_RDONLY);
        if (fd >= 0) {
            // Read through to get actual byte count
            char tmp[256]; int n; uint32_t total = 0;
            while ((n = vfs_read(fd, tmp, sizeof(tmp))) > 0) total += (uint32_t)n;
            vfs_close(fd);
            sh_puts("  Size: "); sh_print_uint(total); sh_puts(" bytes\n");
        }
    }
}

// mkdir [-p] <path>
static void cmd_mkdir(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("mkdir: missing operand\n"); return; }

    // -p flag: ignore error if already exists (we don't support recursive create yet)
    int ignore_exist = 0;
    const char* path_arg = 0;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-p") == 0) ignore_exist = 1;
        else path_arg = argv[i];
    }

    if (!path_arg) { sh_puts("mkdir: missing operand\n"); return; }

    char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);

    int r = vfs_mkdir(path);
    if (r == VFS_EEXIST && ignore_exist) return;
    if (r < 0) {
        sh_puts("mkdir: cannot create directory '"); sh_puts(path);
        sh_puts(r == VFS_EEXIST ? "': file exists\n" : "'\n");
    }
}

// rm [-r] <path> — remove file or directory tree
static void cmd_rm(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("rm: missing operand\n"); return; }

    int recursive = 0;
    const char* path_arg = 0;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-r") == 0 || sh_strcmp(argv[i], "-rf") == 0)
            recursive = 1;
        else path_arg = argv[i];
    }

    if (!path_arg) { sh_puts("rm: missing operand\n"); return; }

    char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);

    // Try as file first
    int r = vfs_unlink(path);
    if (r == 0) return;

    if (r == VFS_EISDIR) {
        if (!recursive) {
            sh_puts("rm: '"); sh_puts(path); sh_puts("' is a directory (use -r)\n");
            return;
        }
        // Recursive: empty the directory then rmdir
        // For now remove immediate children then the dir itself
        // (deep trees would need a stack — FAT32 dirs are typically shallow)
        int fd = vfs_open(path, O_RDONLY);
        if (fd < 0) { sh_puts("rm: cannot open '"); sh_puts(path); sh_puts("'\n"); return; }

        vfs_dirent_t ent;
        uint32_t idx = 0;
        while (vfs_readdir(fd, idx++, &ent) == VFS_EOK) {
            if (sh_strcmp(ent.name, ".") == 0 || sh_strcmp(ent.name, "..") == 0) continue;
            char child[SHELL_CWD_SIZE];
            sh_strcpy(child, path);
            if (child[sh_strlen(child) - 1] != '/') sh_strcat(child, "/");
            sh_strcat(child, ent.name);
            if (ent.is_dir) vfs_rmdir(child);
            else            vfs_unlink(child);
        }
        vfs_close(fd);
        r = vfs_rmdir(path);
        if (r < 0) { sh_puts("rm: cannot remove '"); sh_puts(path); sh_puts("'\n"); }
    } else {
        sh_puts("rm: cannot remove '"); sh_puts(path); sh_puts("': no such file or directory\n");
    }
}

// rmdir <path>
static void cmd_rmdir(int argc, char* argv[])
{
    if (argc < 2) { sh_puts("rmdir: missing operand\n"); return; }
    char path[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1], path);
    if (vfs_rmdir(path) < 0) {
        sh_puts("rmdir: failed to remove '"); sh_puts(path); sh_puts("'\n");
    }
}

// cp <src> <dst>
static void cmd_cp(int argc, char* argv[])
{
    if (argc < 3) { sh_puts("cp: usage: cp <src> <dst>\n"); return; }

    char src[SHELL_CWD_SIZE], dst[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1], src);
    sh_resolve_path(argv[2], dst);

    int src_fd = vfs_open(src, O_RDONLY);
    if (src_fd < 0) {
        sh_puts("cp: cannot open '"); sh_puts(src); sh_puts("': no such file or directory\n");
        return;
    }

    // If dst is an existing directory, copy src into it
    int dst_fd = vfs_open(dst, O_RDONLY);
    if (dst_fd >= 0) {
        vfs_dirent_t ent;
        int is_dir = (vfs_readdir(dst_fd, 0, &ent) != VFS_ENOTDIR);
        vfs_close(dst_fd);
        if (is_dir) {
            char* fname = sh_strrchr(src, '/');
            fname = fname ? fname + 1 : src;
            sh_strcat(dst, "/");
            sh_strcat(dst, fname);
        }
    }

    vfs_unlink(dst);
    if (vfs_create(dst) < 0) {
        sh_puts("cp: cannot create '"); sh_puts(dst); sh_puts("'\n");
        vfs_close(src_fd);
        return;
    }

    int out_fd = vfs_open(dst, O_WRONLY);
    if (out_fd < 0) {
        sh_puts("cp: cannot open destination\n");
        vfs_close(src_fd);
        return;
    }

    char buf[512]; int n;
    while ((n = vfs_read(src_fd, buf, sizeof(buf))) > 0)
        vfs_write(out_fd, buf, (uint32_t)n);

    vfs_close(src_fd);
    vfs_close(out_fd);
}

// mv <src> <dst> — rename (on FAT32 same-dir rename is not directly supported
// so we cp then rm as a fallback)
static void cmd_mv(int argc, char* argv[])
{
    if (argc < 3) { sh_puts("mv: usage: mv <src> <dst>\n"); return; }

    char src[SHELL_CWD_SIZE], dst[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1], src);
    sh_resolve_path(argv[2], dst);

    // If dst is an existing directory, move src into it
    int dst_fd = vfs_open(dst, O_RDONLY);
    if (dst_fd >= 0) {
        vfs_dirent_t ent;
        int is_dir = (vfs_readdir(dst_fd, 0, &ent) != VFS_ENOTDIR);
        vfs_close(dst_fd);
        if (is_dir) {
            char* fname = sh_strrchr(src, '/');
            fname = fname ? fname + 1 : src;
            sh_strcat(dst, "/");
            sh_strcat(dst, fname);
        }
    }

    // Try rename via cp+rm (FAT32 has no rename op at VFS level yet)
    int src_fd = vfs_open(src, O_RDONLY);
    if (src_fd < 0) {
        sh_puts("mv: cannot stat '"); sh_puts(src); sh_puts("': no such file or directory\n");
        return;
    }

    vfs_unlink(dst);
    if (vfs_create(dst) < 0) {
        sh_puts("mv: cannot create '"); sh_puts(dst); sh_puts("'\n");
        vfs_close(src_fd);
        return;
    }

    int out_fd = vfs_open(dst, O_WRONLY);
    if (out_fd < 0) {
        sh_puts("mv: cannot open destination\n");
        vfs_close(src_fd);
        return;
    }

    char buf[512]; int n;
    while ((n = vfs_read(src_fd, buf, sizeof(buf))) > 0)
        vfs_write(out_fd, buf, (uint32_t)n);

    vfs_close(src_fd);
    vfs_close(out_fd);
    vfs_unlink(src);
}

// cd [path]
static void cmd_cd(int argc, char* argv[])
{
    const char* target = (argc < 2) ? "/" : argv[1];

    char path[SHELL_CWD_SIZE];
    sh_resolve_path(target, path);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        sh_puts("cd: "); sh_puts(path); sh_puts(": no such file or directory\n");
        return;
    }

    vfs_dirent_t ent;
    int r = vfs_readdir(fd, 0, &ent);
    vfs_close(fd);

    if (r == VFS_ENOTDIR) {
        sh_puts("cd: "); sh_puts(path); sh_puts(": not a directory\n");
        return;
    }

    sh_strcpy(cwd, path);
}

// pwd
static void cmd_pwd(void)
{
    sh_puts(cwd); sh_puts("\n");
}

// ---- command dispatch ------------------------------------------------------

void shell_task(void)
{
    sh_puts("\nMyOS Shell — type 'help' for commands\n\n");

    char   raw[SHELL_BUF_SIZE];
    char*  argv[SHELL_MAX_ARGS];

    while (1) {
        sh_print_prompt();
        sh_readline(raw, SHELL_BUF_SIZE);
        if (!raw[0]) continue;

        hist_push(raw);

        // Tokenise into argv
        char line[SHELL_BUF_SIZE];
        sh_strcpy(line, raw);
        int argc = sh_tokenise(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        const char* cmd = argv[0];

        if      (sh_strcmp(cmd, "ls")    == 0) cmd_ls(argc, argv);
        else if (sh_strcmp(cmd, "cat")   == 0) cmd_cat(argc, argv);
        else if (sh_strcmp(cmd, "touch") == 0) cmd_touch(argc, argv);
        else if (sh_strcmp(cmd, "echo")  == 0) cmd_echo(argc, argv);
        else if (sh_strcmp(cmd, "stat")  == 0) cmd_stat(argc, argv);
        else if (sh_strcmp(cmd, "mkdir") == 0) cmd_mkdir(argc, argv);
        else if (sh_strcmp(cmd, "rm")    == 0) cmd_rm(argc, argv);
        else if (sh_strcmp(cmd, "rmdir") == 0) cmd_rmdir(argc, argv);
        else if (sh_strcmp(cmd, "cp")    == 0) cmd_cp(argc, argv);
        else if (sh_strcmp(cmd, "mv")    == 0) cmd_mv(argc, argv);
        else if (sh_strcmp(cmd, "cd")    == 0) cmd_cd(argc, argv);
        else if (sh_strcmp(cmd, "pwd")   == 0) cmd_pwd();
        else if (sh_strcmp(cmd, "clear") == 0) terminal_clear();
        else if (sh_strcmp(cmd, "help")  == 0) cmd_help();
        else if (sh_strcmp(cmd, "exec")  == 0) {
            if (argc < 2) { sh_puts("exec: usage: exec <path> [args...] [&]\n"); }
            else {
                // Check for background operator as last token
                int background = 0;
                int arg_end = argc;
                if (arg_end >= 2 && sh_strcmp(argv[arg_end - 1], "&") == 0) {
                    background = 1;
                    arg_end--;
                }

                char path[SHELL_CWD_SIZE];
                sh_resolve_path(argv[1], path);

                // Build argv: path as argv[0], then any extra args (excluding &)
                const char* exec_argv[SHELL_MAX_ARGS + 1];
                exec_argv[0] = path;
                int exec_argc = 1;
                for (int i = 2; i < arg_end && exec_argc < SHELL_MAX_ARGS; i++)
                    exec_argv[exec_argc++] = argv[i];
                exec_argv[exec_argc] = 0;

                // SYS_EXEC = 9; EBX = path, ECX = argv, EDX = 0 (no redirect)
                // EDX must be explicitly zeroed — leaving it unspecified lets the
                // compiler pass whatever garbage was in the register, which
                // SYS_EXEC interprets as a stdout redirect path and corrupts the
                // child's fd table, causing a double-free on task teardown.
                int tid;
                __asm__ volatile (
                    "int $0x80"
                    : "=a"(tid)
                    : "a"(9), "b"(path), "c"(exec_argv), "d"(0)
                    : "memory"
                );

                if (tid < 0) {
                    sh_puts("exec: failed\n");
                } else {
                    sh_puts("exec: launched tid ");
                    sh_print_uint((uint32_t)tid);
                    if (background) {
                        sh_puts(" (background)\n");
                    } else {
                        sh_puts("\n");
                        // SYS_WAITPID = 10; EBX = tid
                        __asm__ volatile (
                            "int $0x80"
                            :: "a"(10), "b"(tid)
                            : "memory"
                        );
                    }
                }
            }
        }
        else {
            sh_puts(cmd); sh_puts(": command not found\n");
        }
    }
}