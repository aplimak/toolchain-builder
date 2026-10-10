/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "filesystem.h"

#include "diagnostics.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *const login_paths[] = {
    "/bin/login", "/usr/bin/login", "/sbin/login", "/usr/sbin/login", NULL};

static int format_root_path(char *path, size_t path_size, const char *root,
                            const char *relative_path)
{
    int length = snprintf(path, path_size, "%s/%s", root, relative_path);

    if (length < 0 || (size_t)length >= path_size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int ensure_directory(const char *path, mode_t mode)
{
    struct stat status;

    if (mkdir(path, mode) != 0 && errno != EEXIST)
        return -1;
    if (stat(path, &status) != 0)
        return -1;
    if (!S_ISDIR(status.st_mode))
    {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int mount_filesystem(const char *source, const char *target,
                            const char *type, unsigned long flags,
                            const char *options)
{
    return mount(source, target, type, flags, options);
}

static int mount_overlay_root(void)
{
    char options[PATH_MAX];
    int length;

    if (mount_filesystem("tmpfs", "./tmp", "tmpfs", MS_NOSUID | MS_NODEV,
                         "mode=1777") != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare writable root overlay");
        return -1;
    }

    if (ensure_directory("./tmp/upper", 0755) != 0 ||
        ensure_directory("./tmp/work", 0755) != 0 ||
        ensure_directory("./tmp/overlay", 0755) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare OverlayFS directories");
        (void)umount2("./tmp", MNT_DETACH);
        return -1;
    }

    length = snprintf(options, sizeof(options),
                      "lowerdir=.,upperdir=./tmp/upper,workdir=./tmp/work");
    if (length < 0 || (size_t)length >= sizeof(options))
    {
        errno = ENAMETOOLONG;
        (void)umount2("./tmp", MNT_DETACH);
        return -1;
    }

    if (mount_filesystem("overlay", "./tmp/overlay", "overlay",
                         MS_NOSUID | MS_NODEV, options) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "OverlayFS unavailable");
        (void)umount2("./tmp", MNT_DETACH);
        return -1;
    }

    if (chdir("./tmp/overlay") != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot use OverlayFS root");
        (void)umount2("./tmp/overlay", MNT_DETACH);
        (void)umount2("./tmp", MNT_DETACH);
        return -1;
    }
    return 0;
}

static int create_device(const char *directory, const char *name,
                         unsigned int major_number, unsigned int minor_number,
                         mode_t permissions)
{
    char path[PATH_MAX];
    struct stat path_status;
    struct stat status;
    int target_exists;

    if (format_root_path(path, sizeof(path), directory, name) != 0)
        return -1;
    if (lstat(path, &path_status) == 0)
    {
        target_exists = stat(path, &status) == 0;
        if (!target_exists && errno != ENOENT)
        {
            diagnostic(DIAGNOSTIC_WARNING, "cannot inspect device target %s: %s",
                       path, strerror(errno));
            return -1;
        }
        if (target_exists && S_ISCHR(status.st_mode) &&
            major(status.st_rdev) == major_number &&
            minor(status.st_rdev) == minor_number)
        {
            if ((status.st_mode & 07777) == permissions ||
                chmod(path, permissions) == 0)
                return 0;
            diagnostic(DIAGNOSTIC_WARNING,
                       "cannot set device node permissions on %s: %s",
                       path, strerror(errno));
            return -1;
        }
        if (unlink(path) != 0)
        {
            diagnostic(DIAGNOSTIC_WARNING,
                       "cannot remove invalid device path %s: %s",
                       path, strerror(errno));
            return -1;
        }
    }
    else if (errno != ENOENT)
    {
        diagnostic(DIAGNOSTIC_WARNING, "cannot inspect device path %s: %s",
                   path, strerror(errno));
        return -1;
    }

    if (mknod(path, S_IFCHR | permissions,
              makedev(major_number, minor_number)) != 0)
    {
        diagnostic(DIAGNOSTIC_WARNING, "cannot create device node %s: %s",
                   path, strerror(errno));
        return -1;
    }
    return 0;
}

static void create_device_links(const char *device_directory)
{
    static const struct
    {
        const char *name;
        const char *target;
    } links[] = {
        {"fd", "/proc/self/fd"},
        {"stdin", "/proc/self/fd/0"},
        {"stdout", "/proc/self/fd/1"},
        {"stderr", "/proc/self/fd/2"}};
    size_t index;

    for (index = 0; index < sizeof(links) / sizeof(links[0]); index++)
    {
        char path[PATH_MAX];

        if (format_root_path(path, sizeof(path), device_directory,
                             links[index].name) != 0)
        {
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot construct device link path");
            continue;
        }
        if (unlink(path) != 0 && errno != ENOENT)
        {
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot replace device link");
            continue;
        }
        if (symlink(links[index].target, path) != 0)
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot create device link");
    }
}

static void populate_device_directory(const char *device_directory)
{
    (void)create_device(device_directory, "null", 1, 3, 0666);
    (void)create_device(device_directory, "zero", 1, 5, 0666);
    (void)create_device(device_directory, "full", 1, 7, 0666);
    (void)create_device(device_directory, "random", 1, 8, 0666);
    (void)create_device(device_directory, "urandom", 1, 9, 0666);
    (void)create_device(device_directory, "tty", 5, 0, 0666);
    (void)create_device(device_directory, "console", 5, 1, 0600);
    (void)create_device(device_directory, "ptmx", 5, 2, 0666);
    create_device_links(device_directory);
}

static int device_null_is_usable(const char *device_directory)
{
    char path[PATH_MAX];
    struct stat status;
    int descriptor;

    if (format_root_path(path, sizeof(path), device_directory, "null") != 0 ||
        stat(path, &status) != 0 || !S_ISCHR(status.st_mode) ||
        major(status.st_rdev) != 1 || minor(status.st_rdev) != 3 ||
        (status.st_mode & 0666) != 0666)
        return 0;
    descriptor = open(path, O_WRONLY);
    if (descriptor < 0)
        return 0;
    (void)close(descriptor);
    return 1;
}

static int setup_device_filesystem(void)
{
    const char *device_directory = "/dev";

    if (ensure_directory(device_directory, 0755) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot prepare /dev");
        return -1;
    }
    if (device_null_is_usable(device_directory))
    {
        populate_device_directory(device_directory);
    }
    else if (mount_filesystem("tmpfs", device_directory, "tmpfs", MS_NOSUID,
                              "mode=0755") == 0)
    {
        populate_device_directory(device_directory);
    }
    else
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot provide /dev");
        return -1;
    }

    if (!device_null_is_usable(device_directory))
    {
        diagnostic(DIAGNOSTIC_ERROR, "no usable character device at /dev/null");
        return -1;
    }
    return 0;
}

static int mount_private_devpts(void)
{
    static const char full_options[] =
        "mode=0620,ptmxmode=0666,newinstance";
    static const char compatible_options[] = "mode=0620,newinstance";
    int first_error;

    if (mount_filesystem("devpts", "/dev/pts", "devpts",
                         MS_NOSUID | MS_NOEXEC, full_options) == 0)
        return 0;
    first_error = errno;

    if (mount_filesystem("devpts", "/dev/pts", "devpts",
                         MS_NOSUID | MS_NOEXEC, compatible_options) == 0)
    {
        diagnostic(DIAGNOSTIC_WARNING,
                   "devpts rejected ptmxmode option (%s); validating node permissions",
                   strerror(first_error));
        return 0;
    }
    diagnostic_errno(DIAGNOSTIC_WARNING, "cannot mount private devpts");
    return -1;
}

static int private_devpts_multiplexer_is_usable(void)
{
    struct stat status;
    int descriptor;

    if (stat("/dev/pts/ptmx", &status) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "private devpts has no usable /dev/pts/ptmx");
        return 0;
    }
    if (!S_ISCHR(status.st_mode) || major(status.st_rdev) != 5 ||
        minor(status.st_rdev) != 2)
    {
        diagnostic(DIAGNOSTIC_WARNING,
                   "/dev/pts/ptmx is not the expected character device; keeping /dev/ptmx fallback");
        return 0;
    }

    if ((status.st_mode & 07777) != 0666 && chmod("/dev/pts/ptmx", 0666) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot set /dev/pts/ptmx permissions; keeping /dev/ptmx fallback");
        return 0;
    }
    if (stat("/dev/pts/ptmx", &status) != 0 ||
        !S_ISCHR(status.st_mode) || major(status.st_rdev) != 5 ||
        minor(status.st_rdev) != 2 || (status.st_mode & 0666) != 0666)
    {
        diagnostic(DIAGNOSTIC_WARNING,
                   "/dev/pts/ptmx remains unusable; keeping /dev/ptmx fallback");
        return 0;
    }

    descriptor = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (descriptor < 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot open private /dev/pts/ptmx; keeping /dev/ptmx fallback");
        return 0;
    }
    if (close(descriptor) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot close private /dev/pts/ptmx; keeping /dev/ptmx fallback");
        return 0;
    }
    return 1;
}

static int link_private_devpts_multiplexer(void)
{
    if (!private_devpts_multiplexer_is_usable())
        return 0;

    if (unlink("/dev/ptmx") != 0 && errno != ENOENT)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot replace /dev/ptmx");
        return 0;
    }
    if (symlink("pts/ptmx", "/dev/ptmx") != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING,
                         "cannot link /dev/ptmx to private devpts");
        (void)create_device("/dev", "ptmx", 5, 2, 0666);
        return 0;
    }
    return 1;
}

static int mount_optional_filesystem(const char *source, const char *target,
                                     const char *type, unsigned long flags,
                                     const char *options)
{
    if (mount_filesystem(source, target, type, flags, options) == 0)
        return 0;
    diagnostic_errno(DIAGNOSTIC_WARNING, target);
    return -1;
}

static int write_default_resolver(void)
{
    static const char resolver_contents[] =
        "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
    size_t written = 0;
    int descriptor = open("/run/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (descriptor < 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot create /run/resolv.conf");
        return -1;
    }
    while (written < sizeof(resolver_contents) - 1)
    {
        ssize_t count = write(descriptor, resolver_contents + written,
                              sizeof(resolver_contents) - 1 - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            if (count == 0)
                errno = EIO;
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot write /run/resolv.conf");
            (void)close(descriptor);
            return -1;
        }
        written += (size_t)count;
    }
    if (close(descriptor) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot close /run/resolv.conf");
        return -1;
    }
    return 0;
}

int filesystem_locate_root(char *root, size_t root_size, const char *argv0)
{
    char executable_path[PATH_MAX];
    char resolved_root[PATH_MAX];
    ssize_t path_length;
    char *last_separator;

    path_length = readlink("/proc/self/exe", executable_path,
                           sizeof(executable_path) - 1);
    if (path_length >= 0)
    {
        if ((size_t)path_length >= sizeof(executable_path) - 1)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        executable_path[path_length] = '\0';
    }
    else
    {
        if (!argv0 || !strchr(argv0, '/'))
        {
            errno = ENOENT;
            return -1;
        }
        if (snprintf(executable_path, sizeof(executable_path), "%s", argv0) >=
            (int)sizeof(executable_path))
        {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    last_separator = strrchr(executable_path, '/');
    if (!last_separator)
    {
        errno = EINVAL;
        return -1;
    }
    if (last_separator == executable_path)
        executable_path[1] = '\0';
    else
        *last_separator = '\0';

    if (!realpath(executable_path, resolved_root))
        return -1;
    if (snprintf(root, root_size, "%s", resolved_root) >= (int)root_size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int filesystem_validate_root(const char *root)
{
    static const char *const required_directories[] = {"usr", "tmp"};
    size_t index;

    for (index = 0; index < sizeof(required_directories) /
                                sizeof(required_directories[0]);
         index++)
    {
        char path[PATH_MAX];
        struct stat status;

        if (format_root_path(path, sizeof(path), root,
                             required_directories[index]) != 0 ||
            stat(path, &status) != 0 || !S_ISDIR(status.st_mode))
        {
            diagnostic(DIAGNOSTIC_ERROR, "%s is not a rootfs (missing /%s)",
                       root, required_directories[index]);
            return -1;
        }
    }
    return 0;
}

int filesystem_find_login(const char *root, char *program, size_t program_size)
{
    size_t index;

    for (index = 0; login_paths[index]; index++)
    {
        char candidate[PATH_MAX];
        struct stat status;

        if (format_root_path(candidate, sizeof(candidate), root,
                             login_paths[index] + 1) != 0)
            continue;
        if (stat(candidate, &status) == 0 && S_ISREG(status.st_mode) &&
            access(candidate, X_OK) == 0)
        {
            if (snprintf(program, program_size, "%s", login_paths[index]) >=
                (int)program_size)
            {
                errno = ENAMETOOLONG;
                return -1;
            }
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

int filesystem_is_readonly(void)
{
    char test_path[] = ".enter-write-test-XXXXXX";
    int descriptor = mkstemp(test_path);

    if (descriptor >= 0)
    {
        (void)close(descriptor);
        (void)unlink(test_path);
        return 0;
    }
    return 1;
}

int filesystem_mount_root(int root_is_readonly)
{
    if (root_is_readonly && mount_overlay_root() == 0)
        return 0;
    if (root_is_readonly)
        diagnostic(DIAGNOSTIC_WARNING,
                   "read-only rootfs will be used without a writable overlay");

    if (mount_filesystem(".", ".", NULL, MS_BIND, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot bind rootfs mountpoint");
        return -1;
    }
    if (chdir(".") != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot select rootfs mountpoint");
        return -1;
    }
    return 0;
}

static int enter_with_pivot_root(void)
{
    char old_root[] = ".enter-oldroot-XXXXXX";

#ifdef SYS_pivot_root
    if (!mkdtemp(old_root))
        return -1;
    if (syscall(SYS_pivot_root, ".", old_root) != 0)
    {
        int saved_errno = errno;
        (void)rmdir(old_root);
        errno = saved_errno;
        return -1;
    }
    if (chdir("/") != 0)
        return -1;
    if (umount2(old_root, MNT_DETACH) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot detach previous root");
    if (rmdir(old_root) != 0 && errno != ENOENT)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot remove previous root directory");
    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int enter_with_move_mount(void)
{
    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0)
        return -1;
    if (chroot(".") != 0)
        return -1;
    return chdir("/");
}

int filesystem_enter_root(void)
{
    if (enter_with_pivot_root() == 0)
        return 0;
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "pivot_root failed; trying move-mount fallback");
    if (enter_with_move_mount() == 0)
        return 0;
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "move-mount fallback failed; trying chroot");
    if (chroot(".") == 0 && chdir("/") == 0)
        return 0;
    diagnostic_errno(DIAGNOSTIC_ERROR,
                     "cannot enter rootfs with pivot_root, move mount, or chroot");
    return -1;
}

int filesystem_setup_pseudo_filesystems(int *private_devpts_available)
{
    *private_devpts_available = 0;

    if (setup_device_filesystem() != 0)
        return -1;

    if (ensure_directory("/proc", 0555) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot prepare /proc");
        return -1;
    }
    if (mount_filesystem("proc", "/proc", "proc",
                         MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot mount required /proc");
        return -1;
    }

    if (ensure_directory("/sys", 0555) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /sys");
    else
        (void)mount_optional_filesystem("sysfs", "/sys", "sysfs",
                                        MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    if (ensure_directory("/dev/pts", 0755) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /dev/pts");
    else if (mount_private_devpts() == 0)
        *private_devpts_available = link_private_devpts_multiplexer();

    if (ensure_directory("/dev/shm", 01777) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /dev/shm");
    else
        (void)mount_optional_filesystem("tmpfs", "/dev/shm", "tmpfs",
                                        MS_NOSUID | MS_NODEV, "mode=1777");

    if (ensure_directory("/tmp", 01777) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /tmp");
    else
        (void)mount_optional_filesystem("tmpfs", "/tmp", "tmpfs",
                                        MS_NOSUID | MS_NODEV, "mode=1777");

    if (ensure_directory("/run", 0755) != 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /run");
    else
    {
        (void)mount_optional_filesystem("tmpfs", "/run", "tmpfs",
                                        MS_NOSUID | MS_NODEV, "mode=0755");
        (void)write_default_resolver();
        if (ensure_directory("/run/lock", 0755) != 0)
            diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /run/lock");
    }
    return 0;
}