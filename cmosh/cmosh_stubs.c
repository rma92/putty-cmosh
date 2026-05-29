#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void modalfatalbox(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("cmosh: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
