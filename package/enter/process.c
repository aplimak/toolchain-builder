/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "process.h"

#include "diagnostics.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void note_child_state_change(int signal_number)
{
    (void)signal_number;
}

static int install_child_signal_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = note_child_state_change;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot install child signal handler");
        return -1;
    }
    return 0;
}

static int wait_for_process(pid_t target_pid, int *target_status)
{
    sigset_t child_signal;
    sigset_t original_mask;
    sigset_t wait_mask;
    int target_finished = 0;
    int wait_error = 0;

    sigemptyset(&child_signal);
    sigaddset(&child_signal, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &child_signal, &original_mask) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot block SIGCHLD");
        return -1;
    }
    wait_mask = original_mask;
    sigdelset(&wait_mask, SIGCHLD);

    while (!target_finished && !wait_error)
    {
        for (;;)
        {
            int status;
            pid_t child_pid = waitpid(-1, &status, WNOHANG);

            if (child_pid > 0)
            {
                if (child_pid == target_pid)
                {
                    *target_status = status;
                    target_finished = 1;
                }
                continue;
            }
            if (child_pid == 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno != ECHILD || !target_finished)
                wait_error = errno;
            break;
        }

        if (target_finished || wait_error)
            break;
        if (sigsuspend(&wait_mask) == -1 && errno != EINTR)
            wait_error = errno;
    }

    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) != 0 && !wait_error)
        wait_error = errno;
    if (wait_error)
    {
        errno = wait_error;
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot wait for child process");
        return -1;
    }
    return 0;
}

static int child_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static int run_child(const char *program, char *const arguments[], int search_path)
{
    struct sigaction default_action;
    sigset_t child_signal;
    sigset_t original_mask;
    pid_t child_pid;
    int status = 0;

    if (install_child_signal_handler() != 0)
        return -1;
    sigemptyset(&child_signal);
    sigaddset(&child_signal, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &child_signal, &original_mask) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot block SIGCHLD before fork");
        return -1;
    }

    child_pid = fork();
    if (child_pid < 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot fork program process");
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        return -1;
    }
    if (child_pid == 0)
    {
        memset(&default_action, 0, sizeof(default_action));
        default_action.sa_handler = SIG_DFL;
        sigemptyset(&default_action.sa_mask);
        (void)sigaction(SIGCHLD, &default_action, NULL);
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        if (search_path)
            execvp(program, arguments);
        else
            execv(program, arguments);
        diagnostic_errno(DIAGNOSTIC_ERROR, program);
        _exit(127);
    }

    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot restore signal mask");
        return -1;
    }
    if (wait_for_process(child_pid, &status) != 0)
        return -1;
    return child_exit_code(status);
}

int process_run_optional_script(const char *path, int *was_started)
{
    char *arguments[2];
    int result;

    *was_started = 0;
    if (access(path, X_OK) != 0)
    {
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES)
            diagnostic_errno(DIAGNOSTIC_WARNING, path);
        return 0;
    }

    *was_started = 1;
    arguments[0] = (char *)path;
    arguments[1] = NULL;
    result = run_child(path, arguments, 0);
    if (result < 0)
        return -1;
    if (result != 0)
        diagnostic(DIAGNOSTIC_WARNING, "%s exited with status %d", path, result);
    return 0;
}

int process_run_program(const char *program, char *const arguments[])
{
    int result = run_child(program, arguments, strchr(program, '/') == NULL);

    return result < 0 ? 1 : result;
}

void process_set_name(const char *name)
{
    (void)prctl(PR_SET_NAME, name, 0, 0, 0);
}