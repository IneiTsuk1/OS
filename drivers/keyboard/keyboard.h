#pragma once
#include <stdint.h>

// Special key codes returned by keyboard_getchar() for non-ASCII keys.
// These occupy the range 0x80–0xFF which standard ASCII never uses,
// so existing code that checks (c >= 32) or (c == '\n') is unaffected.
#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_HOME      0x84
#define KEY_END       0x85
#define KEY_PGUP      0x86
#define KEY_PGDN      0x87
#define KEY_INSERT    0x88
#define KEY_DELETE    0x89
#define KEY_F1        0x90
#define KEY_F2        0x91
#define KEY_F3        0x92
#define KEY_F4        0x93
#define KEY_F5        0x94
#define KEY_F6        0x95
#define KEY_F7        0x96
#define KEY_F8        0x97
#define KEY_F9        0x98
#define KEY_F10       0x99
#define KEY_F11       0x9A
#define KEY_F12       0x9B

void keyboard_init(void);

// Returns the next key from the buffer.
// Returns 0 if the buffer is empty.
// ASCII keys return their ASCII value (1–127).
// Special keys return one of the KEY_* constants above (0x80–0xFF).
// Cast the return value to uint8_t before comparing to KEY_* constants.
char keyboard_getchar(void);
// Block until up to `len` bytes of input are available, then return them.
// Behaviour mirrors a Unix line-discipline in canonical mode:
//   - Blocks (hlt loop) while the keyboard buffer is empty
//   - Returns as soon as a newline '\n' is received, including the newline
//   - Returns early if `len` bytes are collected before a newline
//   - Only printable ASCII and '\n' are returned; special KEY_* codes are
//     silently discarded (they are handled by the shell's own line editor)
// Returns the number of bytes written into buf (always >= 1).
// Caller must ensure buf is at least len bytes.
uint32_t keyboard_read(char* buf, uint32_t len);