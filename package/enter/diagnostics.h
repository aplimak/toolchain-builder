/* SPDX-License-Identifier: MIT */
#ifndef ENTER_DIAGNOSTICS_H
#define ENTER_DIAGNOSTICS_H

enum diagnostic_level
{
    DIAGNOSTIC_WARNING,
    DIAGNOSTIC_ERROR
};

void diagnostic(enum diagnostic_level level, const char *format, ...);
void diagnostic_errno(enum diagnostic_level level, const char *operation);

#endif