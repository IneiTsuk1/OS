#include "keyboard.h"
#include "../../kernel/irq.h"
#include "../../kernel/klog.h"
#include "../../kernel/scheduler.h"
#include <stdint.h>

// ---- I/O port ---------------------------------------------------------------

#define KB_DATA        0x60
#define KB_STATUS      0x64

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ---- Circular buffer --------------------------------------------------------
//
// Stores uint8_t values: ASCII (1–127) or KEY_* special codes (0x80–0xFF).
// 0 is reserved as "empty" sentinel and is never pushed.

#define KB_BUFFER_SIZE 128

static volatile uint8_t  kb_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

// Single-producer (IRQ handler only). Silently drops if full.
static void kb_push(uint8_t c)
{
    if (c == 0) return;
    uint32_t next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next == kb_tail) return;  // full
    kb_buffer[kb_head] = c;
    kb_head = next;
}

// ---- Modifier state ---------------------------------------------------------

static volatile uint8_t shift_held  = 0;   // non-zero when either shift is down
static volatile uint8_t caps_lock   = 0;   // toggled by caps lock press
static volatile uint8_t ctrl_held   = 0;   // left or right ctrl
static volatile uint8_t alt_held    = 0;   // left or right alt
static volatile uint8_t e0_prefix   = 0;   // set when 0xE0 was the last byte

// Expose modifier state for callers that want to check (e.g. shell ctrl+c)
uint8_t keyboard_shift(void) { return shift_held; }
uint8_t keyboard_ctrl(void)  { return ctrl_held;  }
uint8_t keyboard_alt(void)   { return alt_held;   }

// ---- Scancode set 1 tables --------------------------------------------------
//
// Index = scancode byte, value = ASCII (unshifted).
// 0 = no mapping or handled specially.

static const uint8_t base_map[128] =
{
//  0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',  '=',   8,   '\t', // 0x00
   'q',  'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',   0,  'a',  's',  // 0x10
   'd',  'f',  'g', 'h', 'j', 'k', 'l', ';','\'',  '`',   0, '\\','z',  'x',  'c',  'v',  // 0x20
   'b',  'n',  'm', ',', '.', '/',   0,  '*',   0,  ' ',   0,    0,   0,    0,   0,    0,  // 0x30
     0,    0,    0,   0,   0,   0,   0,  '7', '8', '9',  '-', '4', '5', '6',  '+', '1',  // 0x40
    '2',  '3',  '0', '.',  0,   0,   0,    0,   0,   0,                                    // 0x50
};

// Shifted versions of keys that differ from shift+base.
// Only entries that differ from toupper(base) need filling.
static const uint8_t shift_map[128] =
{
//  0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',  '+',   8,   '\t', // 0x00
   'Q',  'W',  'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',   0,  'A',  'S',  // 0x10
   'D',  'F',  'G', 'H', 'J', 'K', 'L', ':', '"',  '~',   0,  '|', 'Z',  'X',  'C',  'V',  // 0x20
   'B',  'N',  'M', '<', '>', '?',   0,  '*',   0,  ' ',   0,    0,   0,    0,   0,    0,  // 0x30
     0,    0,    0,   0,   0,   0,   0,  '7', '8', '9',  '-', '4', '5', '6',  '+', '1',  // 0x40
    '2',  '3',  '0', '.',  0,   0,   0,    0,   0,   0,                                    // 0x50
};

// Extended (0xE0-prefixed) scancodes → KEY_* or ASCII
// Only the subset we care about; others produce 0 (ignored).
static uint8_t extended_map(uint8_t sc)
{
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x35: return '/';      // numpad /
        case 0x1C: return '\n';     // numpad enter
        case 0x1D: return 0;        // right ctrl — handled as modifier below
        case 0x38: return 0;        // right alt  — handled as modifier below
        default:   return 0;
    }
}

// Function key scancodes (non-extended)
static uint8_t fkey_map(uint8_t sc)
{
    switch (sc) {
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:   return 0;
    }
}

// ---- IRQ handler ------------------------------------------------------------

void keyboard_irq_handler(regs_t* r)
{
    (void)r;
    uint8_t sc = inb(KB_DATA);

    // ---- 0xE0 prefix --------------------------------------------------------
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    uint8_t released = sc & 0x80;      // bit 7: 1 = key up, 0 = key down
    uint8_t code     = sc & 0x7F;      // strip release bit

    // ---- Extended scancodes (0xE0 prefix) -----------------------------------
    if (e0_prefix) {
        e0_prefix = 0;

        // Right ctrl / right alt — update modifier state
        if (code == 0x1D) { ctrl_held = !released; return; }
        if (code == 0x38) { alt_held  = !released; return; }

        if (released) return;   // key-up for other extended keys: ignore

        uint8_t k = extended_map(code);
        if (k) kb_push(k);
        return;
    }

    // ---- Modifier keys (non-extended) ---------------------------------------
    switch (code) {
        case 0x2A:  // left shift
        case 0x36:  // right shift
            shift_held = !released;
            return;

        case 0x1D:  // left ctrl
            ctrl_held = !released;
            return;

        case 0x38:  // left alt
            alt_held = !released;
            return;

        case 0x3A:  // caps lock — toggle on press only
            if (!released) caps_lock ^= 1;
            return;

        case 0x45:  // num lock — acknowledge, ignore for now
        case 0x46:  // scroll lock
            return;
    }

    // ---- Key-up for all other keys: discard ---------------------------------
    if (released) return;

    // ---- Function keys ------------------------------------------------------
    uint8_t fk = fkey_map(code);
    if (fk) {
        kb_push(fk);
        return;
    }

    // ---- Normal printable keys ----------------------------------------------
    if (code >= 128) return;    // shouldn't happen, but be safe

    // Determine which table to use.
    // Caps lock only affects letters (a-z / A-Z), not numbers/symbols.
    uint8_t use_shift = shift_held;

    uint8_t base = base_map[code];
    if (base >= 'a' && base <= 'z') {
        // Letter: caps lock XORs the shift state
        use_shift = shift_held ^ caps_lock;
    }

    uint8_t ch = use_shift ? shift_map[code] : base_map[code];
    if (ch == 0) return;

    // Ctrl+key: mask to control character (ctrl+a = 0x01, ctrl+c = 0x03, etc.)
    if (ctrl_held && ch >= 'a' && ch <= 'z') ch -= ('a' - 1);
    if (ctrl_held && ch >= 'A' && ch <= 'Z') ch -= ('A' - 1);

    kb_push(ch);
}

// ---- Public API -------------------------------------------------------------

void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq_handler);
    klog_info("Keyboard: initialized (shift, caps, ctrl, alt, arrows, Fn)");
}

char keyboard_getchar(void)
{
    if (kb_tail == kb_head) return 0;
    uint8_t c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return (char)c;
}
// ---------------------------------------------------------------------------
// keyboard_read — blocking canonical-mode read
//
// Blocks via hlt until input is available.  Returns on newline or when len
// bytes have been collected.  Special KEY_* codes (0x80+) are discarded —
// they are only meaningful to the shell's own line editor.
//
// Interrupts must be ENABLED when this is called (we rely on the keyboard
// IRQ to fill the buffer while we hlt).  The syscall handler re-enables
// interrupts before calling vfs_read -> term_read -> here, so this is safe.
// ---------------------------------------------------------------------------
uint32_t keyboard_read(char* buf, uint32_t len)
{
    if (!buf || len == 0)
        return 0;

    uint32_t n = 0;

    while (n < len) {
        while (kb_tail == kb_head)
            scheduler_yield();

        uint8_t c = kb_buffer[kb_tail];
        kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;

        if (c >= 0x80)
            continue;

        // Ctrl+D — EOF
        if (c == 0x04)
            break;

        buf[n++] = (char)c;

        if (c == '\n')
            break;
    }

    return n;

    return n;
}