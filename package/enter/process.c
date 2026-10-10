/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "process.h"

#include "diagnostics.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TERMINAL_BUFFER_SIZE 4096

struct terminal_buffer
{
    char bytes[TERMINAL_BUFFER_SIZE];
    size_t offset;
    size_t length;
};

static volatile sig_atomic_t terminal_window_changed;

static void note_child_state_change(int signal_number)
{
    (void)signal_number;
}

static void note_terminal_window_change(int signal_number)
{
    (void)signal_number;
    terminal_window_changed = 1;
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

static int allocate_private_pty(int *master_descriptor, int *slave_descriptor)
{
    char slave_path[PATH_MAX];
    int master;
    int slave_name_error;
    int descriptor_flags;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0)
        return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0)
    {
        int saved_errno = errno;
        close(master);
        errno = saved_errno;
        return -1;
    }

    slave_name_error = ptsname_r(master, slave_path, sizeof(slave_path));
    if (slave_name_error != 0)
    {
        close(master);
        errno = slave_name_error;
        return -1;
    }
    descriptor_flags = fcntl(master, F_GETFL);
    if (descriptor_flags < 0 ||
        fcntl(master, F_SETFL, descriptor_flags | O_NONBLOCK) != 0)
    {
        int saved_errno = errno;
        close(master);
        errno = saved_errno;
        return -1;
    }

    *slave_descriptor = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (*slave_descriptor < 0)
    {
        int saved_errno = errno;
        close(master);
        errno = saved_errno;
        return -1;
    }
    *master_descriptor = master;
    return 0;
}

static int attach_child_to_terminal(int slave_descriptor)
{
    int standard_descriptor;

    if (setsid() < 0)
        return -1;
    if (ioctl(slave_descriptor, TIOCSCTTY, 0) != 0)
        return -1;
    if (tcsetpgrp(slave_descriptor, getpgrp()) != 0)
        return -1;

    for (standard_descriptor = STDIN_FILENO;
         standard_descriptor <= STDERR_FILENO; standard_descriptor++)
    {
        if (dup2(slave_descriptor, standard_descriptor) < 0)
            return -1;
    }
    if (slave_descriptor > STDERR_FILENO)
        close(slave_descriptor);
    return 0;
}

static void update_terminal_window_size(int master_descriptor)
{
    struct winsize window_size;
    int source_descriptor = isatty(STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;

    if (ioctl(source_descriptor, TIOCGWINSZ, &window_size) == 0)
        (void)ioctl(master_descriptor, TIOCSWINSZ, &window_size);
}

static size_t terminal_buffer_available(const struct terminal_buffer *buffer)
{
    return TERMINAL_BUFFER_SIZE - (buffer->offset + buffer->length);
}

static void terminal_buffer_compact(struct terminal_buffer *buffer)
{
    if (buffer->offset != 0 && buffer->length != 0)
        memmove(buffer->bytes, buffer->bytes + buffer->offset, buffer->length);
    buffer->offset = 0;
}

static int terminal_buffer_write(int descriptor, struct terminal_buffer *buffer)
{
    while (buffer->length != 0)
    {
        ssize_t count = write(descriptor, buffer->bytes + buffer->offset,
                              buffer->length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (count <= 0)
        {
            if (count == 0)
                errno = EIO;
            return -1;
        }
        buffer->offset += (size_t)count;
        buffer->length -= (size_t)count;
    }
    buffer->offset = 0;
    return 0;
}

static int reap_child_processes(pid_t target_pid, int *target_status,
                                int *target_finished)
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
                *target_finished = 1;
            }
            continue;
        }
        if (child_pid == 0)
            return 0;
        if (child_pid < 0 && errno == EINTR)
            continue;
        if (child_pid < 0 && errno == ECHILD && *target_finished)
            return 0;
        return -1;
    }
}

static int relay_terminal(pid_t target_pid, int master_descriptor,
                          cc_t end_of_input_character, int *target_status)
{
    struct terminal_buffer input_buffer = {{0}, 0, 0};
    struct terminal_buffer output_buffer = {{0}, 0, 0};
    struct sigaction resize_action;
    struct sigaction previous_resize_action;
    struct sigaction ignore_pipe_action;
    struct sigaction previous_pipe_action;
    struct termios previous_terminal_settings;
    int terminal_settings_changed = 0;
    int resize_handler_installed = 0;
    int pipe_handler_installed = 0;
    int input_open = 1;
    int output_open = 1;
    int end_of_input_pending = 0;
    int end_of_input_sent = 0;
    int target_finished = 0;
    int wait_error = 0;
    int result = -1;

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,
                                          &previous_terminal_settings) == 0)
    {
        struct termios raw_terminal_settings = previous_terminal_settings;

        cfmakeraw(&raw_terminal_settings);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_terminal_settings) == 0)
            terminal_settings_changed = 1;
        else
            diagnostic_errno(DIAGNOSTIC_WARNING,
                             "cannot switch console to raw mode for PTY input");
    }

    memset(&resize_action, 0, sizeof(resize_action));
    resize_action.sa_handler = note_terminal_window_change;
    sigemptyset(&resize_action.sa_mask);
    if (sigaction(SIGWINCH, &resize_action, &previous_resize_action) == 0)
        resize_handler_installed = 1;
    else
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot monitor console window-size changes");

    memset(&ignore_pipe_action, 0, sizeof(ignore_pipe_action));
    ignore_pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe_action.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe_action, &previous_pipe_action) == 0)
        pipe_handler_installed = 1;
    else
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot protect PTY relay from SIGPIPE");

    update_terminal_window_size(master_descriptor);
    terminal_window_changed = 0;

    while (!target_finished || output_buffer.length != 0)
    {
        struct pollfd descriptors[3];
        int poll_result;

        if (terminal_window_changed)
        {
            terminal_window_changed = 0;
            update_terminal_window_size(master_descriptor);
        }
        if (reap_child_processes(target_pid, target_status,
                                 &target_finished) != 0)
        {
            wait_error = errno;
            break;
        }

        if (target_finished)
        {
            int read_anything = 0;
            char discard_buffer[TERMINAL_BUFFER_SIZE];

            while (!output_open ||
                   terminal_buffer_available(&output_buffer) != 0)
            {
                ssize_t count;

                if (output_open)
                {
                    terminal_buffer_compact(&output_buffer);
                    count = read(master_descriptor,
                                 output_buffer.bytes + output_buffer.length,
                                 terminal_buffer_available(&output_buffer));
                }
                else
                    count = read(master_descriptor, discard_buffer,
                                 sizeof(discard_buffer));
                if (count > 0)
                {
                    if (output_open)
                        output_buffer.length += (size_t)count;
                    read_anything = 1;
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EIO)
                    diagnostic_errno(DIAGNOSTIC_WARNING,
                                     "cannot drain output from private PTY");
                break;
            }
            if (output_buffer.length == 0 && !read_anything)
                break;
        }

        descriptors[0].fd = master_descriptor;
        descriptors[0].events = 0;
        if (!target_finished &&
            terminal_buffer_available(&output_buffer) != 0)
            descriptors[0].events |= POLLIN;
        if (!target_finished && input_buffer.length != 0)
            descriptors[0].events |= POLLOUT;
        descriptors[0].revents = 0;

        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = input_open && !target_finished &&
                                        terminal_buffer_available(&input_buffer) != 0
                                    ? POLLIN
                                    : 0;
        descriptors[1].revents = 0;

        descriptors[2].fd = output_open ? STDOUT_FILENO : -1;
        descriptors[2].events = output_open && output_buffer.length != 0
                                    ? POLLOUT
                                    : 0;
        descriptors[2].revents = 0;

        poll_result = poll(descriptors, 3, 100);
        if (poll_result < 0)
        {
            if (errno == EINTR)
                continue;
            wait_error = errno;
            break;
        }

        if ((descriptors[0].revents & POLLOUT) != 0 &&
            terminal_buffer_write(master_descriptor, &input_buffer) != 0)
        {
            diagnostic_errno(DIAGNOSTIC_WARNING,
                             "cannot forward console input to private PTY");
            input_buffer.length = 0;
            input_buffer.offset = 0;
        }

        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
            char *destination;
            ssize_t count;

            terminal_buffer_compact(&input_buffer);
            destination = input_buffer.bytes + input_buffer.length;
            count = read(STDIN_FILENO, destination,
                         terminal_buffer_available(&input_buffer));
            if (count > 0)
                input_buffer.length += (size_t)count;
            else if (count == 0)
            {
                input_open = 0;
                end_of_input_pending = 1;
            }
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                diagnostic_errno(DIAGNOSTIC_WARNING,
                                 "cannot read console input for private PTY");
                input_open = 0;
                end_of_input_pending = 1;
            }
        }

        if (end_of_input_pending && !end_of_input_sent &&
            input_buffer.length == 0)
        {
            terminal_buffer_compact(&input_buffer);
            input_buffer.bytes[0] = (char)end_of_input_character;
            input_buffer.length = 1;
            end_of_input_sent = 1;
        }

        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
            terminal_buffer_available(&output_buffer) != 0)
        {
            char discard_buffer[TERMINAL_BUFFER_SIZE];
            ssize_t count;

            if (output_open)
            {
                terminal_buffer_compact(&output_buffer);
                count = read(master_descriptor,
                             output_buffer.bytes + output_buffer.length,
                             terminal_buffer_available(&output_buffer));
            }
            else
                count = read(master_descriptor, discard_buffer,
                             sizeof(discard_buffer));
            if (count > 0)
            {
                if (output_open)
                    output_buffer.length += (size_t)count;
            }
            else if (count < 0 && errno != EINTR && errno != EAGAIN &&
                     errno != EWOULDBLOCK && errno != EIO)
                diagnostic_errno(DIAGNOSTIC_WARNING,
                                 "cannot read output from private PTY");
        }

        if ((descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            diagnostic(DIAGNOSTIC_WARNING,
                       "console output closed; discarding remaining PTY output");
            output_open = 0;
            output_buffer.length = 0;
            output_buffer.offset = 0;
        }
        else if ((descriptors[2].revents & POLLOUT) != 0 &&
                 terminal_buffer_write(STDOUT_FILENO, &output_buffer) != 0)
        {
            diagnostic_errno(DIAGNOSTIC_WARNING,
                             "cannot forward private PTY output to console");
            output_open = 0;
            output_buffer.length = 0;
            output_buffer.offset = 0;
        }
    }

    if (wait_error == 0)
        result = 0;
    else
    {
        errno = wait_error;
        diagnostic_errno(DIAGNOSTIC_ERROR, "PTY process supervision failed");
    }

    if (terminal_settings_changed &&
        tcsetattr(STDIN_FILENO, TCSANOW, &previous_terminal_settings) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot restore console terminal settings");
    if (resize_handler_installed &&
        sigaction(SIGWINCH, &previous_resize_action, NULL) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot restore SIGWINCH handler");
    if (pipe_handler_installed &&
        sigaction(SIGPIPE, &previous_pipe_action, NULL) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot restore SIGPIPE handler");
    return result;
}

static int run_child(const char *program, char *const arguments[], int search_path,
                     int use_private_pty)
{
    struct sigaction default_action;
    sigset_t child_signal;
    sigset_t original_mask;
    pid_t child_pid;
    int pty_master = -1;
    int pty_slave = -1;
    int status = 0;
    cc_t end_of_input_character = '\004';

    if (use_private_pty &&
        allocate_private_pty(&pty_master, &pty_slave) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot allocate private PTY; using inherited console");
        use_private_pty = 0;
    }
    if (use_private_pty)
    {
        struct termios terminal_settings;

        if (tcgetattr(pty_slave, &terminal_settings) == 0)
            end_of_input_character = terminal_settings.c_cc[VEOF];
    }

    if (install_child_signal_handler() != 0)
        return -1;
    sigemptyset(&child_signal);
    sigaddset(&child_signal, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &child_signal, &original_mask) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot block SIGCHLD before fork");
        if (pty_slave >= 0)
            close(pty_slave);
        if (pty_master >= 0)
            close(pty_master);
        return -1;
    }

    child_pid = fork();
    if (child_pid < 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot fork program process");
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        if (pty_slave >= 0)
            close(pty_slave);
        if (pty_master >= 0)
            close(pty_master);
        return -1;
    }
    if (child_pid == 0)
    {
        memset(&default_action, 0, sizeof(default_action));
        default_action.sa_handler = SIG_DFL;
        sigemptyset(&default_action.sa_mask);
        (void)sigaction(SIGCHLD, &default_action, NULL);
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        if (use_private_pty)
        {
            close(pty_master);
            if (attach_child_to_terminal(pty_slave) != 0)
            {
                diagnostic_errno(DIAGNOSTIC_ERROR,
                                 "cannot attach process to private PTY");
                _exit(126);
            }
        }
        if (search_path)
            execvp(program, arguments);
        else
            execv(program, arguments);
        diagnostic_errno(DIAGNOSTIC_ERROR, program);
        _exit(127);
    }

    if (pty_slave >= 0)
        close(pty_slave);
    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot restore signal mask");
        if (pty_master >= 0)
            close(pty_master);
        return -1;
    }
    if (use_private_pty)
    {
        if (relay_terminal(child_pid, pty_master, end_of_input_character,
                           &status) != 0)
        {
            close(pty_master);
            return 1;
        }
        close(pty_master);
        return child_exit_code(status);
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
    result = run_child(path, arguments, 0, 0);
    if (result < 0)
        return -1;
    if (result != 0)
        diagnostic(DIAGNOSTIC_WARNING, "%s exited with status %d", path, result);
    return 0;
}

int process_run_program(const char *program, char *const arguments[],
                        int use_private_pty)
{
    int result = run_child(program, arguments, strchr(program, '/') == NULL,
                           use_private_pty);

    return result < 0 ? 1 : result;
}

void process_set_name(const char *name)
{
    if (prctl(PR_SET_NAME, name, 0, 0, 0) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot set process name");
}