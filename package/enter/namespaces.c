/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "namespaces.h"

#include "diagnostics.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <unistd.h>

static int write_proc_value(const char *path, const char *value)
{
    size_t written = 0;
    size_t value_length = strlen(value);
    int descriptor = open(path, O_WRONLY);

    if (descriptor < 0)
        return -1;
    while (written < value_length)
    {
        ssize_t count = write(descriptor, value + written, value_length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            int saved_errno = count == 0 ? EIO : errno;
            (void)close(descriptor);
            errno = saved_errno;
            return -1;
        }
        written += (size_t)count;
    }
    if (close(descriptor) != 0)
        return -1;
    return 0;
}

int namespaces_prepare_user(void)
{
    uid_t host_uid;
    gid_t host_gid;
    char mapping[64];

    if (geteuid() == 0)
        return 0;

#ifdef CLONE_NEWUSER
    host_uid = getuid();
    host_gid = getgid();
    if (unshare(CLONE_NEWUSER) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot create required user namespace");
        diagnostic(DIAGNOSTIC_ERROR,
                   "run as root or enable unprivileged user namespaces on the host");
        return -1;
    }

    if (write_proc_value("/proc/self/setgroups", "deny") != 0 && errno != ENOENT)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot disable supplementary groups");

    (void)snprintf(mapping, sizeof(mapping), "0 %u 1", (unsigned int)host_uid);
    if (write_proc_value("/proc/self/uid_map", mapping) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot configure user namespace uid_map");
        return -1;
    }
    (void)snprintf(mapping, sizeof(mapping), "0 %u 1", (unsigned int)host_gid);
    if (write_proc_value("/proc/self/gid_map", mapping) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot configure user namespace gid_map");
        return -1;
    }
    if (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot assume uid/gid 0 in user namespace");
        return -1;
    }
    return 0;
#else
    diagnostic(DIAGNOSTIC_ERROR, "this build has no user namespace support");
    return -1;
#endif
}

int namespaces_try_pid(void)
{
#ifdef CLONE_NEWPID
    if (unshare(CLONE_NEWPID) == 0)
        return 1;
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "PID namespace unavailable; continuing without one");
#else
    diagnostic(DIAGNOSTIC_WARNING, "PID namespaces are unavailable in these headers");
#endif
    return 0;
}

int namespaces_setup_child(void)
{
#ifdef CLONE_NEWNS
    if (unshare(CLONE_NEWNS) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot create required mount namespace");
        return -1;
    }
#else
    diagnostic(DIAGNOSTIC_ERROR, "this build has no mount namespace support");
    return -1;
#endif

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot make mount tree private");
        return -1;
    }

#ifdef CLONE_NEWUTS
    if (unshare(CLONE_NEWUTS) == 0)
    {
        if (sethostname("buildroot", sizeof("buildroot") - 1) != 0)
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot set container hostname");
    }
    else
        diagnostic_errno(DIAGNOSTIC_WARNING, "UTS namespace unavailable");
#endif

#ifdef CLONE_NEWIPC
    if (unshare(CLONE_NEWIPC) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "IPC namespace unavailable");
#endif

    return 0;
}