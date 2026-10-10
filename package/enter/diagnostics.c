/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "diagnostics.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic(enum diagnostic_level level, const char *format, ...)
{
    va_list arguments;

#if !ENABLE_RUNTIME_WARNINGS
    if (level == DIAGNOSTIC_WARNING)
        return;
#endif

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