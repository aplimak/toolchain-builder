/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "environment.h"

#include "diagnostics.h"

#include <stdlib.h>
#include <string.h>

static char *copy_environment_value(const char *name)
{
    const char *value = getenv(name);

    return value ? strdup(value) : NULL;
}

int environment_sanitize(void)
{
    char *language = copy_environment_value("LANG");
    char *terminal = copy_environment_value("TERM");
    char *timezone = copy_environment_value("TZ");
    int result = -1;

    if ((getenv("LANG") && !language) || (getenv("TERM") && !terminal) ||
        (getenv("TZ") && !timezone))
    {
        diagnostic(DIAGNOSTIC_ERROR, "cannot preserve environment settings");
        goto done;
    }

    if (clearenv() != 0 ||
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1) != 0 ||
        setenv("HOME", "/root", 1) != 0 ||
        setenv("USER", "root", 1) != 0 ||
        setenv("IFS", " \t\n", 1) != 0 ||
        setenv("LANG", language ? language : "C", 1) != 0 ||
        setenv("TERM", terminal ? terminal : "linux", 1) != 0 ||
        (timezone && setenv("TZ", timezone, 1) != 0))
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot construct sanitized environment");
        goto done;
    }

    result = 0;

done:
    free(language);
    free(terminal);
    free(timezone);
    return result;
}