/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"

#include "diagnostics.h"
#include "environment.h"
#include "filesystem.h"
#include "process.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned int checks_run;
static unsigned int checks_failed;
static unsigned int checks_skipped;

static void stop_timed_out_test(int signal_number)
{
    static const char message[] = "test suite timed out\n";

    (void)signal_number;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void check_result(int condition, const char *description)
{
    checks_run++;
    if (condition)
        printf("PASS: %s\n", description);
    else
    {
        fprintf(stderr, "FAIL: %s\n", description);
        checks_failed++;
    }
}

static int environment_equals(const char *name, const char *expected)
{
    const char *value = getenv(name);

    return value != NULL && strcmp(value, expected) == 0;
}

static int make_path(char *path, size_t path_size, const char *root,
                     const char *relative)
{
    int length = snprintf(path, path_size, "%s/%s", root, relative);

    return length >= 0 && (size_t)length < path_size ? 0 : -1;
}

static int create_executable(const char *path, const char *contents)
{
    size_t remaining = strlen(contents);
    const char *cursor = contents;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);

    if (descriptor < 0)
        return -1;
    while (remaining != 0)
    {
        ssize_t count = write(descriptor, cursor, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            int saved_errno = count == 0 ? EIO : errno;
            close(descriptor);
            errno = saved_errno;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    if (close(descriptor) != 0)
        return -1;
    return chmod(path, 0700);
}

static void test_filesystem_helpers(void)
{
    char root_template[] = "/tmp/enter-tests-root-XXXXXX";
    char *root = mkdtemp(root_template);
    char path[4096];
    char executable_path[4096];
    char expected_root[4096];
    char located_root[4096];
    char undersized_program[2];
    char *separator;
    char found_program[128];
    ssize_t executable_length;
    int valid_root = 0;

    if (!root)
    {
        check_result(0, "create temporary rootfs fixture");
        return;
    }
    if (make_path(path, sizeof(path), root, "usr") == 0 && mkdir(path, 0700) == 0 &&
        make_path(path, sizeof(path), root, "tmp") == 0 && mkdir(path, 0700) == 0)
    {
        valid_root = filesystem_validate_root(root) == 0;
    }
    check_result(valid_root, "accept rootfs containing /usr and /tmp");

    if (make_path(path, sizeof(path), root, "tmp") == 0 && rmdir(path) == 0)
    {
        check_result(filesystem_validate_root(root) != 0,
                     "reject rootfs missing /tmp");
        mkdir(path, 0700);
    }
    if (make_path(path, sizeof(path), root, "usr") == 0 && rmdir(path) == 0)
    {
        check_result(filesystem_validate_root(root) != 0,
                     "reject rootfs missing /usr");
        mkdir(path, 0700);
    }

    executable_length = readlink("/proc/self/exe", executable_path,
                                 sizeof(executable_path) - 1);
    if (executable_length > 0 &&
        (size_t)executable_length < sizeof(executable_path) - 1)
    {
        executable_path[executable_length] = '\0';
        separator = strrchr(executable_path, '/');
        if (separator)
        {
            *separator = '\0';
            if (realpath(executable_path, expected_root) &&
                filesystem_locate_root(located_root, sizeof(located_root),
                                       "unused/test_enter") == 0)
                check_result(strcmp(located_root, expected_root) == 0,
                             "locate root from executable path");
            else
                check_result(0, "locate root from executable path");
        }
        else
            check_result(0, "derive executable directory for root-location test");
    }
    else
        check_result(0, "read test executable path from procfs");

    if (make_path(path, sizeof(path), root, "bin") == 0 && mkdir(path, 0700) == 0 &&
        make_path(path, sizeof(path), root, "bin/login") == 0 &&
        create_executable(path, "#!/bin/sh\nexit 0\n") == 0)
    {
        int login_found = filesystem_find_login(root, found_program,
                                                sizeof(found_program)) == 0 &&
                          strcmp(found_program, "/bin/login") == 0;
        check_result(login_found, "find executable /bin/login in rootfs");
        check_result(filesystem_find_login(root, undersized_program,
                                           sizeof(undersized_program)) != 0,
                     "reject undersized login output buffer");

        chmod(path, 0600);
        check_result(filesystem_find_login(root, found_program,
                                           sizeof(found_program)) != 0,
                     "reject non-executable login candidate");
        unlink(path);
    }
    else
        check_result(0, "create login-candidate fixture");

    check_result(filesystem_find_login(root, found_program,
                                       sizeof(found_program)) != 0,
                 "report when no login candidate exists");

    if (make_path(path, sizeof(path), root, "bin") == 0)
        rmdir(path);
    if (make_path(path, sizeof(path), root, "usr") == 0)
        rmdir(path);
    if (make_path(path, sizeof(path), root, "tmp") == 0)
        rmdir(path);
    rmdir(root);
}

static void test_readonly_probe(void)
{
    char directory_template[] = "/tmp/enter-tests-write-XXXXXX";
    char *directory = mkdtemp(directory_template);
    int original_directory;
    int probe_succeeded;
    struct stat status;
    DIR *directory_stream;
    struct dirent *entry;
    int directory_is_empty = 1;

    if (!directory)
    {
        check_result(0, "create temporary writable directory");
        return;
    }
    original_directory = open(".", O_RDONLY | O_DIRECTORY);
    if (original_directory < 0 || chdir(directory) != 0)
    {
        check_result(0, "enter temporary writable directory");
        if (original_directory >= 0)
            close(original_directory);
        rmdir(directory);
        return;
    }

    probe_succeeded = filesystem_is_readonly() == 0;
    check_result(probe_succeeded, "detect writable root without leaving a probe file");
    directory_stream = opendir(".");
    if (directory_stream)
    {
        while ((entry = readdir(directory_stream)) != NULL)
        {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0)
                directory_is_empty = 0;
        }
        closedir(directory_stream);
    }
    else
        directory_is_empty = 0;
    check_result(directory_is_empty, "remove write-probe file after writable check");
    check_result(stat(".", &status) == 0, "temporary directory remains accessible");

    if (fchdir(original_directory) != 0)
        check_result(0, "restore original working directory");
    close(original_directory);
    rmdir(directory);
}

static int read_all(int descriptor, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity)
    {
        ssize_t count = read(descriptor, buffer + used, capacity - used - 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0)
            break;
        used += (size_t)count;
    }
    buffer[used] = '\0';
    return (int)used;
}

static void test_diagnostics(void)
{
    int output_pipe[2];
    int saved_stderr;
    char output[256];
    const char *expected;
    int output_length;

    if (pipe(output_pipe) != 0)
    {
        check_result(0, "create diagnostic capture pipe");
        return;
    }
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0)
    {
        check_result(0, "redirect diagnostic output");
        if (saved_stderr >= 0)
            close(saved_stderr);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return;
    }
    close(output_pipe[1]);
    diagnostic(DIAGNOSTIC_WARNING, "warning-probe");
    diagnostic(DIAGNOSTIC_ERROR, "error-probe");
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    output_length = read_all(output_pipe[0], output, sizeof(output));
    close(output_pipe[0]);
#if ENABLE_RUNTIME_WARNINGS
    expected = "warning: warning-probe\nerror: error-probe\n";
#else
    expected = "error: error-probe\n";
#endif
    check_result(output_length >= 0 && strcmp(output, expected) == 0,
                 "runtime warning toggle filters warnings but always shows errors");
}

static void test_environment_sanitizing(void)
{
    pid_t child_pid = fork();
    int status;

    if (child_pid < 0)
    {
        check_result(0, "fork environment test process");
        return;
    }
    if (child_pid == 0)
    {
        if (setenv("LANG", "test_LANG", 1) != 0 ||
            setenv("TERM", "test_TERM", 1) != 0 ||
            setenv("TZ", "test_TZ", 1) != 0 ||
            setenv("ENTER_TEST_SECRET", "must-be-removed", 1) != 0 ||
            environment_sanitize() != 0 ||
            !environment_equals("LANG", "test_LANG") ||
            !environment_equals("TERM", "test_TERM") ||
            !environment_equals("TZ", "test_TZ") ||
            getenv("ENTER_TEST_SECRET") != NULL ||
            !environment_equals("HOME", "/root") ||
            !environment_equals("USER", "root") ||
            !environment_equals("PATH", "/bin:/sbin:/usr/bin:/usr/sbin") ||
            !environment_equals("IFS", " \t\n"))
            _exit(1);
        _exit(0);
    }
    if (waitpid(child_pid, &status, 0) < 0)
    {
        check_result(0, "wait for environment test process");
        return;
    }
    check_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "sanitize environment while preserving LANG, TERM, and TZ");
}

static void test_environment_defaults(void)
{
    pid_t child_pid = fork();
    int status;

    if (child_pid < 0)
    {
        check_result(0, "fork default environment test process");
        return;
    }
    if (child_pid == 0)
    {
        unsetenv("LANG");
        unsetenv("TERM");
        unsetenv("TZ");
        if (environment_sanitize() != 0 ||
            !environment_equals("LANG", "C") ||
            !environment_equals("TERM", "linux") || getenv("TZ") != NULL)
            _exit(1);
        _exit(0);
    }
    if (waitpid(child_pid, &status, 0) < 0)
    {
        check_result(0, "wait for default environment test process");
        return;
    }
    check_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "use default LANG and TERM when caller values are absent");
}

static void test_process_supervision(void)
{
    char *success_arguments[] = {"/bin/sh", "-c", "exit 37", NULL};
    char *blocked_signal_arguments[] = {"/bin/sh", "-c", "exit 19", NULL};
    char *signal_arguments[] = {"/bin/sh", "-c", "kill -TERM $$", NULL};
    char *missing_arguments[] = {"missing-enter-test-command", NULL};
    char missing_path[] = "/tmp/enter-tests-command-does-not-exist";
    sigset_t blocked_signal;
    sigset_t original_mask;
    int mask_result;
    int was_started = 1;
    char script_template[] = "/tmp/enter-tests-script-XXXXXX";
    char failing_script_template[] = "/tmp/enter-tests-script-fail-XXXXXX";
    int script_descriptor;

    check_result(process_run_program("/bin/sh", success_arguments, 0) == 37,
                 "preserve program exit status");
    sigemptyset(&blocked_signal);
    sigaddset(&blocked_signal, SIGCHLD);
    mask_result = sigprocmask(SIG_BLOCK, &blocked_signal, &original_mask);
    check_result(mask_result == 0, "block SIGCHLD for inherited-mask test");
    if (mask_result == 0)
    {
        int blocked_mask_status = process_run_program("/bin/sh",
                                                      blocked_signal_arguments, 0);
        sigprocmask(SIG_SETMASK, &original_mask, NULL);
        check_result(blocked_mask_status == 19,
                     "supervise child when SIGCHLD was inherited blocked");
    }
    check_result(process_run_program("/bin/sh", signal_arguments, 0) == 128 + SIGTERM,
                 "translate signal termination to shell status");
    check_result(process_run_program("/definitely/missing/enter-test-program",
                                     missing_arguments, 0) == 127,
                 "report failed exec with status 127");

    check_result(process_run_optional_script(missing_path, &was_started) == 0 &&
                     was_started == 0,
                 "skip absent optional init script");

    script_descriptor = mkstemp(script_template);
    if (script_descriptor < 0)
    {
        check_result(0, "create optional-script fixture");
        return;
    }
    if (write(script_descriptor, "#!/bin/sh\nexit 0\n", 17) != 17 ||
        fchmod(script_descriptor, 0700) != 0 || close(script_descriptor) != 0)
    {
        close(script_descriptor);
        unlink(script_template);
        check_result(0, "prepare optional-script fixture");
        return;
    }
    check_result(process_run_optional_script(script_template, &was_started) == 0 &&
                     was_started == 1,
                 "run executable optional init script");
    unlink(script_template);

    script_descriptor = mkstemp(failing_script_template);
    if (script_descriptor < 0)
    {
        check_result(0, "create failing optional-script fixture");
        return;
    }
    if (write(script_descriptor, "#!/bin/sh\nexit 9\n", 17) != 17 ||
        fchmod(script_descriptor, 0700) != 0 || close(script_descriptor) != 0)
    {
        close(script_descriptor);
        unlink(failing_script_template);
        check_result(0, "prepare failing optional-script fixture");
        return;
    }
    was_started = 0;
    check_result(process_run_optional_script(failing_script_template,
                                             &was_started) == 0 &&
                     was_started == 1,
                 "treat a nonzero optional init script as a warning");
    unlink(failing_script_template);
}

static int private_pty_is_available(void)
{
    char slave_path[4096];
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    int slave;

    if (master < 0)
        return 0;
    if (grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, slave_path, sizeof(slave_path)) != 0)
    {
        close(master);
        return 0;
    }
    slave = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0)
    {
        close(master);
        return 0;
    }
    close(slave);
    close(master);
    return 1;
}

static void test_private_pty(void)
{
    int input_pipe[2];
    int output_pipe[2];
    int saved_stdin;
    int saved_stdout;
    int process_status;
    char output[4096];
    char *arguments[] = {
        "/bin/sh", "-c",
        "test -t 0 && test -t 1 && test -t 2 && "
        "IFS= read -r line && test \"$line\" = pty-input-ok && "
        "printf 'pty-output-ok\\n'",
        NULL};

    if (!private_pty_is_available())
    {
        checks_skipped++;
        printf("SKIP: private PTY unavailable\n");
        return;
    }
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0)
    {
        check_result(0, "create PTY relay test pipes");
        return;
    }
    if (write(input_pipe[1], "pty-input-ok\n", 13) != 13)
    {
        check_result(0, "write PTY relay input fixture");
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return;
    }
    close(input_pipe[1]);

    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0)
    {
        check_result(0, "redirect console streams for PTY test");
        if (saved_stdin >= 0)
            close(saved_stdin);
        if (saved_stdout >= 0)
            close(saved_stdout);
        close(input_pipe[0]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return;
    }
    close(input_pipe[0]);
    close(output_pipe[1]);

    process_status = process_run_program("/bin/sh", arguments, 1);
    fflush(stdout);
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);

    if (read_all(output_pipe[0], output, sizeof(output)) < 0)
        output[0] = '\0';
    close(output_pipe[0]);
    check_result(process_status == 0,
                 "give target a controlling PTY and relay input/EOF");
    check_result(strstr(output, "pty-input-ok") != NULL &&
                     strstr(output, "pty-output-ok") != NULL,
                 "relay PTY echo and program output to caller");
}

static void test_closed_pty_output(void)
{
    int output_pipe[2];
    int saved_stdout;
    int process_status;
    char *arguments[] = {
        "/bin/sh", "-c",
        "i=0; while [ \"$i\" -lt 5000 ]; do printf x; i=$((i + 1)); done",
        NULL};

    if (!private_pty_is_available())
    {
        checks_skipped++;
        printf("SKIP: closed-output PTY test unavailable\n");
        return;
    }
    if (pipe(output_pipe) != 0)
    {
        check_result(0, "create closed-output test pipe");
        return;
    }
    close(output_pipe[0]);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || dup2(output_pipe[1], STDOUT_FILENO) < 0)
    {
        check_result(0, "redirect stdout to closed-output pipe");
        if (saved_stdout >= 0)
            close(saved_stdout);
        close(output_pipe[1]);
        return;
    }
    close(output_pipe[1]);

    process_status = process_run_program("/bin/sh", arguments, 1);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    check_result(process_status == 0,
                 "finish PTY child when caller output has closed");
}

static void test_process_name(void)
{
    char original_name[64];
    char current_name[64];
    FILE *stream = fopen("/proc/self/comm", "r");

    if (!stream || !fgets(original_name, sizeof(original_name), stream))
    {
        if (stream)
            fclose(stream);
        check_result(0, "read original process name");
        return;
    }
    fclose(stream);
    original_name[strcspn(original_name, "\n")] = '\0';

    process_set_name("enter-unit-test");
    stream = fopen("/proc/self/comm", "r");
    if (!stream || !fgets(current_name, sizeof(current_name), stream))
    {
        if (stream)
            fclose(stream);
        check_result(0, "read changed process name");
        process_set_name(original_name);
        return;
    }
    fclose(stream);
    current_name[strcspn(current_name, "\n")] = '\0';
    check_result(strcmp(current_name, "enter-unit-test") == 0,
                 "set process name within Linux comm limit");
    process_set_name(original_name);
}

int main(void)
{
    signal(SIGALRM, stop_timed_out_test);
    alarm(30);

    test_filesystem_helpers();
    test_readonly_probe();
    test_diagnostics();
    test_environment_sanitizing();
    test_environment_defaults();
    test_process_supervision();
    test_private_pty();
    test_closed_pty_output();
    test_process_name();

    alarm(0);
    printf("%u checks, %u failed, %u skipped\n",
           checks_run, checks_failed, checks_skipped);
    return checks_failed == 0 ? 0 : 1;
}