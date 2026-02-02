/* stdio.h shim for PocketQuake */
#ifndef _STDIO_H_SHIM
#define _STDIO_H_SHIM
#include "../libc/libc.h"
#include "../terminal.h"

/* Quake uses printf/vprintf - map to our terminal */
#undef printf
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
#endif
