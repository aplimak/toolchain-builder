/* SPDX-License-Identifier: MIT */
#include "diagnostics.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic(enum diagnostic_level level, const char *format, ...)
{
    va_list arguments;

    fprintf(stderr, "%s: ",
            level == DIAGNOSTIC_ERROR ? "error" : "warning");
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

void diagnostic_errno(enum diagnostic_level level, const char *operation)
{
    int saved_errno = errno;

    diagnostic(level, "%s: %s", operation, strerror(saved_errno));
}