#include "shell.h"
#include "vfs/vfs.h"
#include "klog.h"
#include "../drivers/vga/terminal.h"
#include "../drivers/serial/serial.h"
#include "../drivers/keyboard/keyboard.h"
#include <stdint.h>

#define SHELL_BUF_SIZE    256
#define SHELL_CWD_SIZE    256
#define SHELL_HIST_SIZE   16
#define SHELL_MAX_ARGS    16
#define SHELL_MAX_STAGES  4

// ---------------------------------------------------------------------------
// Output helpers
// When sh_redir_fd >= 0, all sh_puts/sh_putchar output goes to that fd
// instead of the terminal. exec_single sets this for built-in redirects.
// ---------------------------------------------------------------------------

static int sh_redir_fd = -1;

static void sh_puts(const char* s)
{
    if (sh_redir_fd >= 0) {
        int len = 0; while (s[len]) len++;
        vfs_write(sh_redir_fd, s, (uint32_t)len);
        return;
    }
    terminal_write(s);
    serial_write(s);
}

static void sh_putchar(char c)
{
    if (sh_redir_fd >= 0) {
        vfs_write(sh_redir_fd, &c, 1);
        return;
    }
    char buf[2] = {c, 0};
    terminal_write(buf);
    serial_write(buf);
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

static int sh_strlen(const char* s)
{
    int n = 0; while (s[n]) n++; return n;
}

static int sh_strcmp(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int sh_strncmp(const char* a, const char* b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
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

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

static void sh_print_uint(uint32_t n)
{
    if (n == 0) { sh_putchar('0'); return; }
    char tmp[12]; int i = 0;
    while (n) { tmp[i++] = '0' + n % 10; n /= 10; }
    while (i--) sh_putchar(tmp[i]);
}

static void sh_print_uint_w(uint32_t n, int width)
{
    char tmp[12]; int i = 0;
    if (n == 0) tmp[i++] = '0';
    else while (n) { tmp[i++] = '0' + n % 10; n /= 10; }
    for (int p = i; p < width; p++) sh_putchar(' ');
    while (i--) sh_putchar(tmp[i]);
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

static char cwd[SHELL_CWD_SIZE] = "/";

static void sh_normalise(char* path)
{
    static char out[SHELL_CWD_SIZE];
    int o = 0, len = sh_strlen(path);
    out[o++] = '/';
    for (int i = 0; i < len; ) {
        if (path[i] == '/') { i++; continue; }
        int j = i;
        while (j < len && path[j] != '/') j++;
        int clen = j - i;
        if (clen == 1 && path[i] == '.') {
            // skip
        } else if (clen == 2 && path[i] == '.' && path[i+1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o-1] != '/') o--; }
        } else {
            if (o > 1) out[o++] = '/';
            for (int k = 0; k < clen && o < SHELL_CWD_SIZE-1; k++)
                out[o++] = path[i+k];
        }
        i = j;
    }
    out[o] = '\0';
    sh_strcpy(path, out);
}

static void sh_resolve_path(const char* input, char* out)
{
    if (input[0] == '/') {
        sh_strcpy(out, input);
    } else if (input[0] == '\0') {
        sh_strcpy(out, cwd);
    } else {
        sh_strcpy(out, cwd);
        if (out[sh_strlen(out)-1] != '/') sh_strcat(out, "/");
        sh_strcat(out, input);
    }
    sh_normalise(out);
}

static int sh_find_exec(const char* name, char* out)
{
    static char c1[SHELL_CWD_SIZE], c2[SHELL_CWD_SIZE];
    int nc = 0;
    char* cands[2];
    cands[nc] = c1; sh_resolve_path(name, c1); nc++;

    int has_ext = 0;
    for (int i = sh_strlen(name)-1; i >= 0 && name[i] != '/'; i--)
        if (name[i] == '.') { has_ext = 1; break; }

    if (!has_ext) {
        cands[nc] = c2;
        sh_resolve_path(name, c2);
        int l = sh_strlen(c2);
        if (l + 4 < SHELL_CWD_SIZE) {
            c2[l]='.'; c2[l+1]='e'; c2[l+2]='l'; c2[l+3]='f'; c2[l+4]='\0';
        }
        nc++;
    }

    for (int i = 0; i < nc; i++) {
        int fd = vfs_open(cands[i], O_RDONLY);
        if (fd >= 0) { vfs_close(fd); sh_strcpy(out, cands[i]); return 1; }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Last exit status
// ---------------------------------------------------------------------------

static int last_exit_status = 0;

// TID of the currently running foreground child, or -1 if none.
// Set before SYS_WAITPID, cleared after it returns.
// Used by Ctrl+C handler to send SIGKILL to a running foreground job.
static int sh_foreground_tid = -1;

// ---------------------------------------------------------------------------
// Pre-processor: pad | > >> ; with spaces
// ---------------------------------------------------------------------------

static void sh_preprocess(const char* in, char* out)
{
    int o = 0;
    for (int i = 0; in[i] && o < SHELL_BUF_SIZE-4; i++) {
        char c = in[i];
        if (c == '|' || c == ';') {
            if (o > 0 && out[o-1] != ' ') out[o++] = ' ';
            out[o++] = c; out[o++] = ' ';
        } else if (c == '>') {
            if (o > 0 && out[o-1] != ' ') out[o++] = ' ';
            if (in[i+1] == '>') { out[o++]='>'; out[o++]='>'; out[o++]=' '; i++; }
            else { out[o++]='>'; out[o++]=' '; }
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static int sh_split(char* line, char* argv[], int max_args)
{
    int argc = 0;
    char* p = line;
    while (*p && argc < max_args-1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[argc] = 0;
    return argc;
}

// ---------------------------------------------------------------------------
// Command descriptor
// ---------------------------------------------------------------------------

typedef struct {
    char*  argv[SHELL_MAX_ARGS + 1];
    int    argc;
    char   redir_in[SHELL_CWD_SIZE];
    char   redir_out[SHELL_CWD_SIZE];
    char   redir_err[SHELL_CWD_SIZE];
    int    append_out;
    int    background;
} cmd_t;

typedef struct {
    cmd_t stages[SHELL_MAX_STAGES];
    int   nstages;
    int   is_pipe;
} pipeline_t;

// ---------------------------------------------------------------------------
// Command history
// ---------------------------------------------------------------------------

static char history[SHELL_HIST_SIZE][SHELL_BUF_SIZE];
static int  hist_count = 0, hist_head = 0;

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
    int idx = (hist_head - 1 - offset + SHELL_HIST_SIZE*2) % SHELL_HIST_SIZE;
    return history[idx];
}

// ---------------------------------------------------------------------------
// Tab completion
// ---------------------------------------------------------------------------

static void sh_tab_complete(char* buf, int* len, int* cur)
{
    int tok_start = *cur;
    while (tok_start > 0 && buf[tok_start-1] != ' ') tok_start--;

    char partial[SHELL_CWD_SIZE];
    int plen = *cur - tok_start;
    for (int i = 0; i < plen; i++) partial[i] = buf[tok_start+i];
    partial[plen] = '\0';

    char search_dir[SHELL_CWD_SIZE], prefix[SHELL_CWD_SIZE];
    char* slash = sh_strrchr(partial, '/');
    if (slash) {
        int dlen = (int)(slash - partial) + 1;
        for (int i = 0; i < dlen && i < SHELL_CWD_SIZE-1; i++)
            search_dir[i] = partial[i];
        search_dir[dlen] = '\0';
        sh_strcpy(prefix, slash+1);
    } else {
        search_dir[0] = '\0';
        sh_strcpy(prefix, partial);
    }

    char resolved_dir[SHELL_CWD_SIZE];
    if (search_dir[0]) sh_resolve_path(search_dir, resolved_dir);
    else sh_strcpy(resolved_dir, cwd);

    int prelen = sh_strlen(prefix);
    int fd = vfs_open(resolved_dir, O_RDONLY);
    if (fd < 0) return;

    static char match[SHELL_CWD_SIZE];
    int nmatch = 0;
    vfs_dirent_t ent;
    uint32_t idx = 0;
    while (vfs_readdir(fd, idx++, &ent) == VFS_EOK) {
        if (sh_strncmp(ent.name, prefix, prelen) == 0) {
            if (nmatch == 0) sh_strcpy(match, ent.name);
            nmatch++;
        }
    }
    vfs_close(fd);

    if (nmatch == 0) return;

    if (nmatch == 1) {
        char completion[SHELL_CWD_SIZE];
        sh_strcpy(completion, search_dir);
        sh_strcat(completion, match);
        int clen = sh_strlen(completion);
        for (int i = 0; i < plen; i++) { terminal_write("\b"); serial_write("\b"); }
        int tail = *len - *cur;
        int new_len = tok_start + clen + tail;
        if (new_len >= SHELL_BUF_SIZE) return;
        for (int i = tail; i >= 0; i--) buf[tok_start+clen+i] = buf[*cur+i];
        for (int i = 0; i < clen; i++) buf[tok_start+i] = completion[i];
        *len = tok_start + clen + tail;
        *cur = tok_start + clen;
        buf[*len] = '\0';
        for (int i = tok_start; i < *len; i++) sh_putchar(buf[i]);
        for (int i = *cur; i < *len; i++) { terminal_write("\b"); serial_write("\b"); }
        return;
    }

    sh_puts("\n");
    fd = vfs_open(resolved_dir, O_RDONLY);
    if (fd >= 0) {
        idx = 0;
        while (vfs_readdir(fd, idx++, &ent) == VFS_EOK) {
            if (sh_strncmp(ent.name, prefix, prelen) == 0) {
                sh_puts(ent.name);
                sh_puts(ent.is_dir ? "/  " : "  ");
            }
        }
        vfs_close(fd);
    }
    sh_puts("\n");
    terminal_set_color(0x0A); sh_puts("myos");
    terminal_set_color(0x0F); sh_puts(":");
    terminal_set_color(0x0B); sh_puts(cwd);
    terminal_set_color(0x0F); sh_puts("$ ");
    buf[*len] = '\0';
    sh_puts(buf);
    for (int i = *cur; i < *len; i++) { terminal_write("\b"); serial_write("\b"); }
}

// ---------------------------------------------------------------------------
// Line editor
// ---------------------------------------------------------------------------

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
    static char saved[SHELL_BUF_SIZE];
    saved[0] = buf[0] = '\0';

    while (1) {
        __asm__ volatile ("hlt");
        uint8_t c = (uint8_t)keyboard_getchar();
        if (!c) continue;

        if (c == '\n' || c == '\r') { buf[len]='\0'; sh_puts("\n"); return; }
        if (c == '\t') { sh_tab_complete(buf, &len, &cur); continue; }
        if (c == 3)  {
            if (sh_foreground_tid >= 0) {
                // Kill the running foreground job. Don't cancel readline —
                // we are not in readline during a foreground exec (that blocks
                // in SYS_WAITPID). If somehow we get here with a foreground
                // job (e.g. future async path), send SIGKILL and let the
                // waitpid in exec_single/exec_pipeline return.
                int kill_ret;
                __asm__ volatile(
                    "int $0x80"
                    :"=a"(kill_ret)
                    :"a"(14),"b"(sh_foreground_tid),"c"(9)
                    :"memory"
                );
                // Don't clear sh_foreground_tid here — exec_single clears it
                // after waitpid returns.
            } else {
                sh_puts("^C\n");
                buf[0]='\0';
                return;
            }
            continue;
        }
        if (c == 4 && len == 0) { sh_strcpy(buf,"exit"); sh_puts("exit\n"); return; }
        if (c == 21) {
            for (int i=0;i<cur;i++){terminal_write("\b");serial_write("\b");}
            for (int i=0;i<len;i++) sh_putchar(' ');
            for (int i=0;i<len;i++){terminal_write("\b");serial_write("\b");}
            len=cur=0; buf[0]='\0'; hist_pos=-1; continue;
        }
        if (c == 11) {
            for (int i=cur;i<len;i++) sh_putchar(' ');
            for (int i=cur;i<len;i++){terminal_write("\b");serial_write("\b");}
            len=cur; buf[len]='\0'; continue;
        }
        if (c == 1) { while(cur>0){terminal_write("\b");serial_write("\b");cur--;} continue; }
        if (c == 5) { while(cur<len){sh_putchar(buf[cur]);cur++;} continue; }
        if (c == 8 || c == 127) {
            if (cur==0) continue;
            for (int i=cur-1;i<len-1;i++) buf[i]=buf[i+1];
            len--; cur--; buf[len]='\0';
            terminal_write("\b"); serial_write("\b");
            sh_redraw_tail(buf,len,cur); continue;
        }
        if (c==KEY_LEFT)  {if(cur>0){terminal_write("\b");serial_write("\b");cur--;}continue;}
        if (c==KEY_RIGHT) {if(cur<len){sh_putchar(buf[cur]);cur++;}continue;}
        if (c==KEY_HOME)  {while(cur>0){terminal_write("\b");serial_write("\b");cur--;}continue;}
        if (c==KEY_END)   {while(cur<len){sh_putchar(buf[cur]);cur++;}continue;}
        if (c==KEY_DELETE){
            if(cur>=len) continue;
            for(int i=cur;i<len-1;i++) buf[i]=buf[i+1];
            len--; buf[len]='\0'; sh_redraw_tail(buf,len,cur); continue;
        }
        if (c==KEY_UP) {
            int next=hist_pos+1;
            if(!hist_get(next)) continue;
            if(hist_pos==-1) sh_strcpy(saved,buf);
            hist_pos=next;
            int oc=cur,ol=len;
            sh_strcpy(buf,hist_get(hist_pos)); len=sh_strlen(buf);
            sh_redraw_full(buf,len,oc,ol); cur=len; continue;
        }
        if (c==KEY_DOWN) {
            if(hist_pos<0) continue;
            int oc=cur,ol=len;
            if(hist_pos==0){hist_pos=-1;sh_strcpy(buf,saved);}
            else{hist_pos--;sh_strcpy(buf,hist_get(hist_pos));}
            len=sh_strlen(buf); sh_redraw_full(buf,len,oc,ol); cur=len; continue;
        }
        if (c<32||c>=128) continue;
        if (len>=max-1) continue;
        for(int i=len;i>cur;i--) buf[i]=buf[i-1];
        buf[cur]=(char)c; len++; cur++; buf[len]='\0';
        sh_putchar(c);
        if(cur<len) sh_redraw_tail(buf,len,cur);
    }
}

// ---------------------------------------------------------------------------
// Prompt
// ---------------------------------------------------------------------------

static void sh_print_prompt(void)
{
    terminal_set_color(0x0A); sh_puts("myos");
    terminal_set_color(0x0F); sh_puts(":");
    terminal_set_color(0x0B); sh_puts(cwd);
    terminal_set_color(0x0F); sh_puts("$ ");
}

// ---------------------------------------------------------------------------
// Built-in commands
// ---------------------------------------------------------------------------

static void cmd_help(void)
{
    sh_puts("Built-in commands:\n");
    sh_puts("  ls [-l] [path]          list directory\n");
    sh_puts("  cat <file> [...]        print file(s)\n");
    sh_puts("  touch <file> [...]      create file(s)\n");
    sh_puts("  echo [text]             print text\n");
    sh_puts("  stat <path>             show file info\n");
    sh_puts("  mkdir [-p] <path>       create directory\n");
    sh_puts("  rm [-r] <path>          remove file/directory\n");
    sh_puts("  rmdir <path>            remove empty directory\n");
    sh_puts("  cp <src> <dst>          copy file\n");
    sh_puts("  mv <src> <dst>          move/rename file\n");
    sh_puts("  cd [path]               change directory\n");
    sh_puts("  pwd                     print working directory\n");
    sh_puts("  clear                   clear screen\n");
    sh_puts("  exit [code]             exit shell\n");
    sh_puts("  help                    this message\n");
    sh_puts("Operators:\n");
    sh_puts("  cmd > file              redirect stdout to file\n");
    sh_puts("  cmd >> file             append stdout to file\n");
    sh_puts("  cmd1 | cmd2             pipe stdout to stdin\n");
    sh_puts("  cmd &                   run in background\n");
    sh_puts("  cmd1 ; cmd2             run sequentially\n");
    sh_puts("  $?                      last exit status\n");
    sh_puts("Line editing:\n");
    sh_puts("  Tab       complete filename\n");
    sh_puts("  Up/Down   history        Left/Right  move cursor\n");
    sh_puts("  Home/End  jump           Delete      delete at cursor\n");
    sh_puts("  Ctrl+A/E  home/end       Ctrl+U/K    kill left/right\n");
    sh_puts("  Ctrl+C    cancel         Ctrl+D      exit\n");
}

static void cmd_ls(int argc, char* argv[])
{
    int long_fmt = 0;
    const char* path_arg = "";
    for (int i=1;i<argc;i++) {
        if (sh_strcmp(argv[i],"-l")==0) long_fmt=1;
        else path_arg=argv[i];
    }
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);
    int fd = vfs_open(path, O_RDONLY);
    if (fd<0) { sh_puts("ls: cannot access '"); sh_puts(path); sh_puts("': no such file or directory\n"); return; }
    vfs_dirent_t ent;
    uint32_t idx=0;
    while (vfs_readdir(fd,idx++,&ent)==VFS_EOK) {
        if (long_fmt) {
            if (ent.is_dir) {
                if (sh_redir_fd<0) terminal_set_color(0x0B);
                sh_puts("d");
                if (sh_redir_fd<0) terminal_set_color(0x0F);
                sh_puts("rwxr-xr-x  "); sh_print_uint_w(0,8);
            } else {
                sh_puts("-rw-r--r--  "); sh_print_uint_w(ent.size,8);
            }
            sh_puts("  ");
        }
        if (ent.is_dir) {
            if (sh_redir_fd<0) terminal_set_color(0x0B);
            sh_puts(ent.name);
            if (sh_redir_fd<0) terminal_set_color(0x0F);
            sh_puts(long_fmt ? "\n" : "/  ");
        } else {
            sh_puts(ent.name);
            sh_puts(long_fmt ? "\n" : "  ");
        }
    }
    if (!long_fmt) sh_puts("\n");
    vfs_close(fd);
}

static void cmd_cat(int argc, char* argv[])
{
    if (argc<2) { sh_puts("cat: missing operand\n"); return; }
    for (int i=1;i<argc;i++) {
        static char path[SHELL_CWD_SIZE];
        sh_resolve_path(argv[i], path);
        int fd = vfs_open(path, O_RDONLY);
        if (fd<0) { sh_puts("cat: "); sh_puts(argv[i]); sh_puts(": no such file or directory\n"); continue; }
        static char buf[256]; int n;
        while ((n=vfs_read(fd,buf,sizeof(buf)-1))>0) { buf[n]='\0'; sh_puts(buf); }
        vfs_close(fd);
    }
}

static void cmd_touch(int argc, char* argv[])
{
    if (argc<2) { sh_puts("touch: missing operand\n"); return; }
    for (int i=1;i<argc;i++) {
        static char path[SHELL_CWD_SIZE];
        sh_resolve_path(argv[i], path);
        int fd = vfs_open(path, O_RDONLY);
        if (fd>=0) { vfs_close(fd); continue; }
        if (vfs_create(path)<0) { sh_puts("touch: cannot create '"); sh_puts(path); sh_puts("'\n"); }
    }
}

static void cmd_echo(int argc, char* argv[])
{
    for (int i=1;i<argc;i++) {
        sh_puts(argv[i]);
        if (i+1<argc) sh_puts(" ");
    }
    sh_puts("\n");
}

static void cmd_stat(int argc, char* argv[])
{
    if (argc<2) { sh_puts("stat: missing operand\n"); return; }
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1], path);
    int fd = vfs_open(path, O_RDONLY);
    if (fd<0) { sh_puts("stat: cannot access '"); sh_puts(path); sh_puts("': no such file or directory\n"); return; }
    vfs_dirent_t ent;
    int is_dir = (vfs_readdir(fd,0,&ent)!=VFS_ENOTDIR);
    vfs_file_t* f = vfs_get_fd(fd);
    uint32_t size = f ? f->size : 0;
    vfs_close(fd);
    sh_puts("  File: "); sh_puts(path); sh_puts("\n");
    sh_puts("  Type: "); sh_puts(is_dir?"directory":"regular file"); sh_puts("\n");
    if (!is_dir) { sh_puts("  Size: "); sh_print_uint(size); sh_puts(" bytes\n"); }
}

static void cmd_mkdir(int argc, char* argv[])
{
    if (argc<2) { sh_puts("mkdir: missing operand\n"); return; }
    int ignore_exist=0; const char* path_arg=0;
    for (int i=1;i<argc;i++) {
        if (sh_strcmp(argv[i],"-p")==0) ignore_exist=1;
        else path_arg=argv[i];
    }
    if (!path_arg) { sh_puts("mkdir: missing operand\n"); return; }
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);
    int r=vfs_mkdir(path);
    if (r==VFS_EEXIST&&ignore_exist) return;
    if (r<0) { sh_puts("mkdir: cannot create '"); sh_puts(path); sh_puts(r==VFS_EEXIST?"': file exists\n":"'\n"); }
}

static void cmd_rm(int argc, char* argv[])
{
    if (argc<2) { sh_puts("rm: missing operand\n"); return; }
    int recursive=0; const char* path_arg=0;
    for (int i=1;i<argc;i++) {
        if (sh_strcmp(argv[i],"-r")==0||sh_strcmp(argv[i],"-rf")==0) recursive=1;
        else path_arg=argv[i];
    }
    if (!path_arg) { sh_puts("rm: missing operand\n"); return; }
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(path_arg, path);
    int r=vfs_unlink(path);
    if (r==0) return;
    if (r==VFS_EISDIR) {
        if (!recursive) { sh_puts("rm: '"); sh_puts(path); sh_puts("': is a directory (use -r)\n"); return; }
        int fd=vfs_open(path,O_RDONLY);
        if (fd<0) { sh_puts("rm: cannot open '"); sh_puts(path); sh_puts("'\n"); return; }
        vfs_dirent_t ent; uint32_t idx=0;
        while (vfs_readdir(fd,idx++,&ent)==VFS_EOK) {
            if (sh_strcmp(ent.name,".")==0||sh_strcmp(ent.name,"..")==0) continue;
            static char child[SHELL_CWD_SIZE];
            sh_strcpy(child,path);
            if (child[sh_strlen(child)-1]!='/') sh_strcat(child,"/");
            sh_strcat(child,ent.name);
            if (ent.is_dir) vfs_rmdir(child); else vfs_unlink(child);
        }
        vfs_close(fd);
        r=vfs_rmdir(path);
        if (r<0) { sh_puts("rm: cannot remove '"); sh_puts(path); sh_puts("'\n"); }
    } else {
        sh_puts("rm: cannot remove '"); sh_puts(path); sh_puts("': no such file or directory\n");
    }
}

static void cmd_rmdir(int argc, char* argv[])
{
    if (argc<2) { sh_puts("rmdir: missing operand\n"); return; }
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1],path);
    if (vfs_rmdir(path)<0) { sh_puts("rmdir: failed to remove '"); sh_puts(path); sh_puts("'\n"); }
}

static void cmd_cp(int argc, char* argv[])
{
    if (argc<3) { sh_puts("cp: usage: cp <src> <dst>\n"); return; }
    static char src[SHELL_CWD_SIZE], dst[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1],src); sh_resolve_path(argv[2],dst);
    int src_fd=vfs_open(src,O_RDONLY);
    if (src_fd<0) { sh_puts("cp: cannot open '"); sh_puts(src); sh_puts("': no such file or directory\n"); return; }
    int dst_fd=vfs_open(dst,O_RDONLY);
    if (dst_fd>=0) {
        vfs_dirent_t ent;
        int is_dir=(vfs_readdir(dst_fd,0,&ent)!=VFS_ENOTDIR);
        vfs_close(dst_fd);
        if (is_dir) { char* fname=sh_strrchr(src,'/'); fname=fname?fname+1:src; sh_strcat(dst,"/"); sh_strcat(dst,fname); }
    }
    vfs_unlink(dst);
    if (vfs_create(dst)<0) { sh_puts("cp: cannot create '"); sh_puts(dst); sh_puts("'\n"); vfs_close(src_fd); return; }
    int out_fd=vfs_open(dst,O_WRONLY);
    if (out_fd<0) { sh_puts("cp: cannot open destination\n"); vfs_close(src_fd); return; }
    static char buf[512]; int n;
    while ((n=vfs_read(src_fd,buf,sizeof(buf)))>0) vfs_write(out_fd,buf,(uint32_t)n);
    vfs_close(src_fd); vfs_close(out_fd);
}

static void cmd_mv(int argc, char* argv[])
{
    if (argc<3) { sh_puts("mv: usage: mv <src> <dst>\n"); return; }
    static char src[SHELL_CWD_SIZE], dst[SHELL_CWD_SIZE];
    sh_resolve_path(argv[1],src); sh_resolve_path(argv[2],dst);

    // If dst is an existing directory, move src inside it.
    int dst_fd=vfs_open(dst,O_RDONLY);
    if (dst_fd>=0) {
        vfs_dirent_t ent;
        int is_dir=(vfs_readdir(dst_fd,0,&ent)!=VFS_ENOTDIR);
        vfs_close(dst_fd);
        if (is_dir) {
            char* fname=sh_strrchr(src,'/');
            fname=fname?fname+1:src;
            if (dst[sh_strlen(dst)-1]!='/') sh_strcat(dst,"/");
            sh_strcat(dst,fname);
        }
    }

    // Fast path: native rename (in-place dirent update, no data copy).
    int r=vfs_rename(src,dst);
    if (r==VFS_EOK) return;

    // Fallback: cp + rm (used if vfs_rename returns VFS_EGENERIC, e.g.
    // cross-device move, though that can't happen on a single-volume OS).
    if (r!=VFS_EGENERIC) {
        sh_puts("mv: cannot move '"); sh_puts(src); sh_puts("': ");
        if (r==VFS_ENOENT)  sh_puts("no such file or directory\n");
        else if (r==VFS_ENOTDIR) sh_puts("not a directory\n");
        else if (r==VFS_EISDIR)  sh_puts("is a directory\n");
        else sh_puts("error\n");
        return;
    }

    int src_fd=vfs_open(src,O_RDONLY);
    if (src_fd<0) { sh_puts("mv: cannot stat '"); sh_puts(src); sh_puts("': no such file or directory\n"); return; }
    vfs_unlink(dst);
    if (vfs_create(dst)<0) { sh_puts("mv: cannot create '"); sh_puts(dst); sh_puts("'\n"); vfs_close(src_fd); return; }
    int out_fd=vfs_open(dst,O_WRONLY);
    if (out_fd<0) { sh_puts("mv: cannot open destination\n"); vfs_close(src_fd); return; }
    static char buf[512]; int n;
    while ((n=vfs_read(src_fd,buf,sizeof(buf)))>0) vfs_write(out_fd,buf,(uint32_t)n);
    vfs_close(src_fd); vfs_close(out_fd); vfs_unlink(src);
}

static void cmd_cd(int argc, char* argv[])
{
    const char* target=(argc<2)?"/":argv[1];
    static char path[SHELL_CWD_SIZE];
    sh_resolve_path(target,path);
    int fd=vfs_open(path,O_RDONLY);
    if (fd<0) { sh_puts("cd: "); sh_puts(path); sh_puts(": no such file or directory\n"); return; }
    vfs_dirent_t ent;
    int r=vfs_readdir(fd,0,&ent);
    vfs_close(fd);
    if (r==VFS_ENOTDIR) { sh_puts("cd: "); sh_puts(path); sh_puts(": not a directory\n"); return; }
    sh_strcpy(cwd,path);
}

// ---------------------------------------------------------------------------
// Pipeline parser
// ---------------------------------------------------------------------------

static int parse_pipeline(int argc, char* argv[], pipeline_t* pl)
{
    pl->nstages=0; pl->is_pipe=0;
    int stage=0;
    cmd_t* c=&pl->stages[0];
    c->argc=0; c->redir_in[0]=c->redir_out[0]=c->redir_err[0]='\0';
    c->append_out=0; c->background=0;

    for (int i=0;i<argc;i++) {
        char* tok=argv[i];

        if (sh_strcmp(tok,"|")==0) {
            c->argv[c->argc]=0; pl->nstages++; pl->is_pipe=1;
            if (pl->nstages>=SHELL_MAX_STAGES) break;
            stage++; c=&pl->stages[stage];
            c->argc=0; c->redir_in[0]=c->redir_out[0]=c->redir_err[0]='\0';
            c->append_out=0; c->background=0; continue;
        }
        if (sh_strcmp(tok,";")==0) {
            c->argv[c->argc]=0; pl->nstages++;
            if (pl->nstages>=SHELL_MAX_STAGES) break;
            stage++; c=&pl->stages[stage];
            c->argc=0; c->redir_in[0]=c->redir_out[0]=c->redir_err[0]='\0';
            c->append_out=0; c->background=0; continue;
        }
        if (sh_strcmp(tok,">")==0||sh_strcmp(tok,">>")==0) {
            c->append_out=(tok[1]=='>');
            if (i+1<argc) sh_resolve_path(argv[++i],c->redir_out);
            continue;
        }
        if (sh_strcmp(tok,"2>")==0) {
            if (i+1<argc) sh_resolve_path(argv[++i],c->redir_err); continue;
        }
        if (sh_strcmp(tok,"<")==0) {
            if (i+1<argc) sh_resolve_path(argv[++i],c->redir_in); continue;
        }
        if (sh_strcmp(tok,"&")==0&&i==argc-1) { c->background=1; continue; }

        // $? substitution
        if (tok[0]=='$'&&tok[1]=='?'&&tok[2]=='\0') {
            static char sbuf[8];
            int s=last_exit_status,neg=(s<0); if(neg)s=-s;
            int j=0; char tmp[8]; int ti=0;
            if(s==0)tmp[ti++]='0'; else while(s){tmp[ti++]='0'+s%10;s/=10;}
            if(neg)sbuf[j++]='-';
            while(ti--)sbuf[j++]=tmp[ti];
            sbuf[j]='\0';
            if(c->argc<SHELL_MAX_ARGS) c->argv[c->argc++]=sbuf;
            continue;
        }

        if (c->argc<SHELL_MAX_ARGS) c->argv[c->argc++]=tok;
    }
    c->argv[c->argc]=0;
    pl->nstages++;
    return pl->nstages;
}

// ---------------------------------------------------------------------------
// exec_single — execute one command with redirect support
// ---------------------------------------------------------------------------

static int exec_single(cmd_t* c)
{
    if (c->argc==0) return 0;
    const char* cmd=c->argv[0];

    // Open redirect fd for built-ins
    int redir_fd=-1;
    if (c->redir_out[0]) {
        if (!c->append_out) { vfs_unlink(c->redir_out); vfs_create(c->redir_out); }
        uint32_t flags=c->append_out?(O_WRONLY|O_APPEND):O_WRONLY;
        redir_fd=vfs_open(c->redir_out,flags);
        if (redir_fd<0) {
            terminal_write(cmd); terminal_write(": cannot open '");
            terminal_write(c->redir_out); terminal_write("' for writing\n");
            return 1;
        }
    }

    // Activate output redirect for built-ins
    sh_redir_fd=redir_fd;

    int is_builtin=1;
    if      (sh_strcmp(cmd,"ls")   ==0) cmd_ls(c->argc,c->argv);
    else if (sh_strcmp(cmd,"cat")  ==0) cmd_cat(c->argc,c->argv);
    else if (sh_strcmp(cmd,"touch")==0) cmd_touch(c->argc,c->argv);
    else if (sh_strcmp(cmd,"echo") ==0) cmd_echo(c->argc,c->argv);
    else if (sh_strcmp(cmd,"stat") ==0) cmd_stat(c->argc,c->argv);
    else if (sh_strcmp(cmd,"mkdir")==0) cmd_mkdir(c->argc,c->argv);
    else if (sh_strcmp(cmd,"rm")   ==0) cmd_rm(c->argc,c->argv);
    else if (sh_strcmp(cmd,"rmdir")==0) cmd_rmdir(c->argc,c->argv);
    else if (sh_strcmp(cmd,"cp")   ==0) cmd_cp(c->argc,c->argv);
    else if (sh_strcmp(cmd,"mv")   ==0) cmd_mv(c->argc,c->argv);
    else if (sh_strcmp(cmd,"cd")   ==0) cmd_cd(c->argc,c->argv);
    else if (sh_strcmp(cmd,"pwd")  ==0) { sh_puts(cwd); sh_puts("\n"); }
    else if (sh_strcmp(cmd,"clear")==0) { sh_redir_fd=-1; terminal_clear(); }
    else if (sh_strcmp(cmd,"help") ==0) cmd_help();
    else if (sh_strcmp(cmd,"exit") ==0) {
        if (c->argc>1) {
            const char* s=c->argv[1]; int v=0;
            while(*s>='0'&&*s<='9') v=v*10+(*s++-'0');
            last_exit_status=v;
        }
        sh_redir_fd=-1;
        if (redir_fd>=0) vfs_close(redir_fd);
        sh_puts("exit\n");
        while(1) __asm__ volatile("hlt");
    }
    else is_builtin=0;

    // Deactivate redirect
    sh_redir_fd=-1;

    if (is_builtin) {
        if (redir_fd>=0) vfs_close(redir_fd);
        return 0;
    }

    // External program — close the fd we opened (SYS_EXEC reopens via path)
    if (redir_fd>=0) { vfs_close(redir_fd); redir_fd=-1; }

    static char exec_path[SHELL_CWD_SIZE];
    if (!sh_find_exec(cmd, exec_path)) {
        sh_puts(cmd); sh_puts(": command not found\n");
        return 127;
    }

    static const char* exec_argv[SHELL_MAX_ARGS+1];
    exec_argv[0]=exec_path;
    for (int i=1;i<c->argc&&i<SHELL_MAX_ARGS;i++) exec_argv[i]=c->argv[i];
    exec_argv[c->argc]=0;

    const char* redir_path=c->redir_out[0]?c->redir_out:(const char*)0;

    int tid;
    __asm__ volatile(
        "int $0x80"
        :"=a"(tid)
        :"a"(9),"b"(exec_path),"c"(exec_argv),"d"(redir_path)
        :"memory"
    );

    if (tid<0) { sh_puts(cmd); sh_puts(": exec failed\n"); return 126; }
    if (c->background) return 0;

    sh_foreground_tid = tid;
    int wait_ret;
    __asm__ volatile("int $0x80":"=a"(wait_ret):"a"(10),"b"(tid):"memory");
    sh_foreground_tid = -1;
    last_exit_status = wait_ret;
    return last_exit_status;
}

// ---------------------------------------------------------------------------
// exec_pipeline — execute a multi-stage pipe
// ---------------------------------------------------------------------------

static void exec_pipeline(pipeline_t* pl)
{
    if (pl->nstages==1) { last_exit_status=exec_single(&pl->stages[0]); return; }

    static int pipe_fds[SHELL_MAX_STAGES-1][2];
    int tids[SHELL_MAX_STAGES];

    for (int i=0;i<pl->nstages-1;i++) {
        int pr;
        __asm__ volatile("int $0x80":"=a"(pr):"a"(11),"c"(pipe_fds[i]):"memory");
        if (pr<0) {
            sh_puts("shell: pipe creation failed\n");
            for (int j=0;j<i;j++) {
                __asm__ volatile("int $0x80"::"a"(6),"b"(pipe_fds[j][0]):"memory");
                __asm__ volatile("int $0x80"::"a"(6),"b"(pipe_fds[j][1]):"memory");
            }
            return;
        }
    }

    for (int i=0;i<pl->nstages;i++) {
        cmd_t* c=&pl->stages[i];
        if (c->argc==0) { tids[i]=-1; continue; }

        int stdin_fd  =(i>0)               ?pipe_fds[i-1][0]:-1;
        int stdout_fd =(i<pl->nstages-1)   ?pipe_fds[i][1]  :-1;

        static char exec_path[SHELL_CWD_SIZE];
        if (!sh_find_exec(c->argv[0],exec_path)) {
            sh_puts(c->argv[0]); sh_puts(": command not found\n");
            tids[i]=-1; continue;
        }

        static const char* exec_argv[SHELL_MAX_ARGS+1];
        exec_argv[0]=exec_path;
        for (int j=1;j<c->argc&&j<SHELL_MAX_ARGS;j++) exec_argv[j]=c->argv[j];
        exec_argv[c->argc]=0;

        int tid;
        __asm__ volatile(
            "int $0x80":"=a"(tid)
            :"a"(13),"b"(exec_path),"c"(exec_argv),"d"(stdout_fd),"S"(stdin_fd)
            :"memory"
        );
        tids[i]=tid;
    }

    for (int i=0;i<pl->nstages-1;i++) {
        __asm__ volatile("int $0x80"::"a"(6),"b"(pipe_fds[i][0]):"memory");
        __asm__ volatile("int $0x80"::"a"(6),"b"(pipe_fds[i][1]):"memory");
    }

    // Set foreground tid to the last stage (rightmost process in the pipeline)
    // so Ctrl+C kills it and causes the pipe to collapse.
    for (int i=pl->nstages-1;i>=0;i--)
        if (tids[i]>0) { sh_foreground_tid=tids[i]; break; }

    for (int i=pl->nstages-1;i>=0;i--)
        if (tids[i]>0)
            __asm__ volatile("int $0x80"::"a"(10),"b"(tids[i]):"memory");

    sh_foreground_tid=-1;
}

// ---------------------------------------------------------------------------
// Main shell loop
// ---------------------------------------------------------------------------

void shell_task(void)
{
    sh_puts("\nMyOS Shell - type 'help' for commands\n\n");

    static char raw[SHELL_BUF_SIZE];
    static char preprocessed[SHELL_BUF_SIZE];
    static char line[SHELL_BUF_SIZE];
    static char* argv[SHELL_MAX_ARGS];
    static pipeline_t pl;

    while (1) {
        sh_print_prompt();
        sh_readline(raw, SHELL_BUF_SIZE);
        if (!raw[0]) continue;

        hist_push(raw);
        sh_preprocess(raw, preprocessed);
        sh_strcpy(line, preprocessed);
        int argc=sh_split(line, argv, SHELL_MAX_ARGS);
        if (argc==0) continue;

        parse_pipeline(argc, argv, &pl);

        if (pl.is_pipe) {
            exec_pipeline(&pl);
        } else {
            for (int i=0;i<pl.nstages;i++)
                last_exit_status=exec_single(&pl.stages[i]);
        }
    }
}