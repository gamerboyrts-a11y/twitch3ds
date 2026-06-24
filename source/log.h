#pragma once
#include <stdio.h>
#include <stdarg.h>

static inline void log_write(const char *fmt, ...) {
    FILE *f = fopen("/config/twitch3ds.log", "a");
    if (!f) return;
    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va); fprintf(f, "\n");
    va_end(va); fclose(f);
}
#define LOG(...) log_write(__VA_ARGS__)
