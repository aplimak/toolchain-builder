/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "container.h"

#include "diagnostics.h"
#include "environment.h"
#include "filesystem.h"
#include "namespaces.h"
#include "process.h"

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void run_container_child(const char *program, char *const arguments[],
                                int run_init_scripts)
{
    int root_is_readonly = filesystem_is_readonly();
    int init_script_started = 0;
    int exit_status;

    process_set_name(getpid() == 1 ? "[enter: init]" : "[enter: container]");
    if (namespaces_setup_child() != 0)
        _exit(1);
    if (filesystem_mount_root(root_is_readonly) != 0)
        _exit(1);
    if (filesystem_enter_root() != 0)
        _exit(126);
    if (filesystem_setup_pseudo_filesystems() != 0)
        _exit(125);
    if (environment_sanitize() != 0)
        _exit(1);

    if (run_init_scripts &&
        process_run_optional_script("/etc/init.d/rcS", &init_script_started) != 0)
        _exit(1);

    exit_status = process_run_program(program, arguments);
    if (run_init_scripts && init_script_started)
    {
        int cleanup_script_started;
        (void)process_run_optional_script("/etc/init.d/rcK",
                                          &cleanup_script_started);
    }
    _exit(exit_status);
}

int container_run(const char *program, char *const arguments[],
                  int run_init_scripts)
{
    pid_t container_pid;
    int status;

    if (namespaces_prepare_user() != 0)
        return 1;
    (void)namespaces_try_pid();

    container_pid = fork();
    if (container_pid < 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot fork container process");
        return 1;
    }
    if (container_pid == 0)
        run_container_child(program, arguments, run_init_scripts);

    if (signal(SIGINT, SIG_IGN) == SIG_ERR)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot ignore host SIGINT");
    if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot ignore host SIGTERM");

    for (;;)
    {
        if (waitpid(container_pid, &status, 0) >= 0)
            break;
        if (errno == EINTR)
            continue;
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot wait for container process");
        return 1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}