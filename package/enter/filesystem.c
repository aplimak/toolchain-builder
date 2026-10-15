/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef FEATURE_OVERLAY
#define FEATURE_OVERLAY 1
#endif
#ifndef FEATURE_FORCED_OVERLAY
#define FEATURE_FORCED_OVERLAY 1
#endif
#ifndef FEATURE_REQUIRED_OVERLAY
#define FEATURE_REQUIRED_OVERLAY 1
#endif
#ifndef FEATURE_IMAGE_OVERLAY
#define FEATURE_IMAGE_OVERLAY 1
#endif
#ifndef FEATURE_DEFAULT_RESOLVER
#define FEATURE_DEFAULT_RESOLVER 1
#endif
#ifndef FEATURE_ENTER_OPEN_TREE
#define FEATURE_ENTER_OPEN_TREE 1
#endif
#ifndef FEATURE_ENTER_PIVOT_ROOT
#define FEATURE_ENTER_PIVOT_ROOT 1
#endif
#ifndef FEATURE_ENTER_MOVE_MOUNT
#define FEATURE_ENTER_MOVE_MOUNT 1
#endif
#ifndef FEATURE_ENTER_CHROOT
#define FEATURE_ENTER_CHROOT 1
#endif
#ifndef MKE2FS_PATH
#define MKE2FS_PATH "mke2fs"
#endif

#include "filesystem.h"

#include "diagnostics.h"
#include "process.h"

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
#if FEATURE_IMAGE_OVERLAY
#include <linux/loop.h>
#include <sys/xattr.h>
#include <time.h>
#if HAVE_LIBSELINUX
#include <selinux/selinux.h>
#endif
#endif

#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *const search_paths[] = {"",     "bin",      "usr/bin",
                                           "sbin", "usr/sbin", NULL};

static int format_root_path(char *path, size_t path_size, const char *root,
                            const char *relative_path) {
  int length = snprintf(path, path_size, "%s/%s", root, relative_path);

  if (length < 0 || (size_t)length >= path_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int ensure_directory(const char *path, mode_t mode) {
  struct stat status;

  if (mkdir(path, mode) != 0 && errno != EEXIST)
    return -1;
  if (stat(path, &status) != 0)
    return -1;
  if (!S_ISDIR(status.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  return 0;
}

static int mount_filesystem(const char *source, const char *target,
                            const char *type, unsigned long flags,
                            const char *options) {
  return mount(source, target, type, flags, options);
}

#if FEATURE_IMAGE_OVERLAY
static int filesystem_supports_xattrs(const char *dir) {
  char path[256];
  snprintf(path, sizeof path, "%s/.xattr_test_%d", dir, getpid());

  int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return -1;

  const char *name = "trusted.overlay.test";
  const char *value = "1";
  int rc = fsetxattr(fd, name, value, strlen(value), 0);
  int saved_errno = errno;

  close(fd);
  unlink(path);

  errno = saved_errno;
  if (rc == 0)
    return 1; /* supported */
  if (errno == ENOTSUP || errno == EOPNOTSUPP)
    return 0; /* not supported */
  return -1;  /* other error (e.g. EPERM) */
}

static int create_ext4_image(char *mke2fs_path, const char *path,
                             off_t size_bytes, int *out_fd) {
  int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, size_bytes) < 0) {
    close(fd);
    unlink(path);
    return -1;
  }
  *out_fd = fd;

  char *arguments[] = {
      basename(mke2fs_path), "-t", "ext4", "-F", "-q", "-I", "256",
      (char *)path,          NULL};
  return process_run_program(mke2fs_path, arguments, 0);
}

static int open_loop_device(int num, char *out_path, size_t out_size) {
  static const char *const prefixes[] = {"/dev/loop", "/dev/block/loop",
                                         "/dev/loop/", NULL};
  char path[64];
  int i;
  long long deadline_ms = 5000; /* 5 s, toybox's empirical value */
  long long start_ms;

  /* Get a monotonic start time. */
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    start_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }

  for (;;) {
    for (i = 0; prefixes[i]; i++) {
      snprintf(path, sizeof(path), "%s%d", prefixes[i], num);
      int fd = open(path, O_RDWR | O_CLOEXEC);
      if (fd >= 0) {
        snprintf(out_path, out_size, "%s", path);
        return fd;
      }
      /* ENOENT: keep trying other variants / wait for ueventd.
         Any other errno: the node exists but we can't use it. */
      if (errno != ENOENT)
        return -1;
    }

    /* All variants missing so far.  Wait and retry if we still have time. */
    {
      struct timespec now;
      long long now_ms;
      clock_gettime(CLOCK_MONOTONIC, &now);
      now_ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
      if (now_ms - start_ms >= deadline_ms)
        break;

      struct timespec sleep = {0, 20 * 1000 * 1000}; /* 20 ms */
      nanosleep(&sleep, NULL);
    }
  }

  errno = ENOENT;
  return -1;
}
#endif

#if FEATURE_OVERLAY
/* Returns 1 if overlayfs is supported, 0 if not. */
static int overlayfs_is_supported(void) {
  FILE *fp = fopen("/proc/filesystems", "r");
  char line[256];

  if (!fp) {
#if FEATURE_REQUIRED_OVERLAY
    diagnostic_errno(DIAGNOSTIC_WARNING, "can't open /proc/filesystems");
#endif
    return -1;
  }

  while (fgets(line, sizeof line, fp)) {
    /* Lines look like "nodev\toverlay" or "nodev\toverlayfs". */
    if (strstr(line, "overlay")) {
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);
#if FEATURE_REQUIRED_OVERLAY
  diagnostic(DIAGNOSTIC_WARNING, "overlay is not supported");
#endif
  return 0;
}

static int mount_overlay_root(void) {
  char options[PATH_MAX];
  int length;

  if (mount_filesystem("tmpfs", "./tmp", "tmpfs", MS_NOSUID | MS_NODEV,
                       "mode=1777") != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "cannot prepare writable root overlay");
    return -1;
  }

  int inner_mount = 0;
#if FEATURE_IMAGE_OVERLAY
  int rc = filesystem_supports_xattrs("./tmp");
  if (rc <= 0) {
    if (rc < 0)
      diagnostic_errno(DIAGNOSTIC_WARNING, "xattr probe failed");
    else
      diagnostic(DIAGNOSTIC_WARNING, "tmpfs does not support xattrs");

    char mke2fs_path[PATH_MAX];
    if (filesystem_find_executable("/", MKE2FS_PATH, mke2fs_path,
                                   sizeof(mke2fs_path)) != 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING,
                       "can't find mke2fs program on the host");
      return -1;
    }

    static const char *const IMG_CTX = "u:object_r:media_rw_data_file:s0";
    static const char *const IMG_PATH = "./tmp/overlay.img";
    static const off_t IMG_SIZE = 256L * 1024 * 1024;
#if HAVE_LIBSELINUX
    if (setfscreatecon(IMG_CTX) != 0)
      diagnostic_errno(DIAGNOSTIC_WARNING, "setfscreatecon");
#endif
    int fd = -1;
    rc = create_ext4_image(mke2fs_path, IMG_PATH, IMG_SIZE, &fd);
    if (rc != 0) {
      if (rc < 0)
        diagnostic_errno(DIAGNOSTIC_WARNING, "create ext4 image error");
      else
        diagnostic(DIAGNOSTIC_WARNING,
                   "cannot create ext4 image (mke2fs status %d)", rc);
      if (fd != -1)
        close(fd);
#if HAVE_LIBSELINUX
      setfscreatecon(NULL);
#endif
      (void)umount2("./tmp", MNT_DETACH);
      return -1;
    }

#if HAVE_LIBSELINUX
    setfscreatecon(NULL);
#else
    if (fsetxattr(fd, "security.selinux", IMG_CTX, strlen(IMG_CTX) + 1, 0) != 0)
      diagnostic_errno(DIAGNOSTIC_WARNING,
                       "cannot label ext4 image; loop mount may be denied");
#endif

    int lc = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (lc < 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "can't open /dev/loop-control");
      (void)umount2("./tmp", MNT_DETACH);
      close(fd);
      return -1;
    }
    int num = ioctl(lc, LOOP_CTL_GET_FREE);
    close(lc);
    if (num < 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "can't allocate loop device");
      (void)umount2("./tmp", MNT_DETACH);
      close(fd);
      return -1;
    }
    char loopdev[64];
    int lfd = open_loop_device(num, loopdev, sizeof loopdev);
    if (lfd < 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "can't open loop device");
      (void)umount2("./tmp", MNT_DETACH);
      close(fd);
      return -1;
    }
    if (ioctl(lfd, LOOP_SET_FD, fd) != 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "can't setup loop device");
      close(lfd);
      close(fd);
      (void)umount2("./tmp", MNT_DETACH);
      return -1;
    }
    close(fd);

    struct loop_info64 info = {0};
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (ioctl(lfd, LOOP_SET_STATUS64, &info) != 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "can't set loop device auto clear");
      ioctl(lfd, LOOP_CLR_FD, 0);
      close(lfd);
      (void)umount2("./tmp", MNT_DETACH);
      return -1;
    }

    if (mount_filesystem(loopdev, "./tmp", "ext4", MS_NOSUID | MS_NODEV, NULL) <
        0) {
      diagnostic_errno(DIAGNOSTIC_WARNING,
                       "cannot mount ext4 writable overlay");
      ioctl(lfd, LOOP_CLR_FD, 0);
      close(lfd);
      (void)umount2("./tmp", MNT_DETACH);
      return -1;
    }
    close(lfd);
    inner_mount = 1;
  }
#endif

  if (ensure_directory("./tmp/upper", 0755) != 0 ||
      ensure_directory("./tmp/work", 0755) != 0 ||
      ensure_directory("./tmp/overlay", 0755) != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "cannot prepare OverlayFS directories");
    if (inner_mount)
      (void)umount2("./tmp", MNT_DETACH);
    (void)umount2("./tmp", MNT_DETACH);
    return -1;
  }

  length = snprintf(options, sizeof(options),
                    "lowerdir=.,upperdir=./tmp/upper,workdir=./tmp/work");
  if (length < 0 || (size_t)length >= sizeof(options)) {
    errno = ENAMETOOLONG;
    if (inner_mount)
      (void)umount2("./tmp", MNT_DETACH);
    (void)umount2("./tmp", MNT_DETACH);
    return -1;
  }

  if (mount_filesystem("overlay", "./tmp/overlay", "overlay",
                       MS_NOSUID | MS_NODEV, options) != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING, "OverlayFS unavailable");
    if (inner_mount)
      (void)umount2("./tmp", MNT_DETACH);
    (void)umount2("./tmp", MNT_DETACH);
    return -1;
  }

  if (chdir("./tmp/overlay") != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING, "cannot use OverlayFS root");
    (void)umount2("./tmp/overlay", MNT_DETACH);
    if (inner_mount)
      (void)umount2("./tmp", MNT_DETACH);
    (void)umount2("./tmp", MNT_DETACH);
    return -1;
  }
  return 0;
}
#endif

static int create_device(const char *directory, const char *name,
                         unsigned int major_number, unsigned int minor_number,
                         mode_t permissions) {
  char path[PATH_MAX];
  struct stat path_status;
  struct stat status;
  int target_exists;

  if (format_root_path(path, sizeof(path), directory, name) != 0)
    return -1;
  if (lstat(path, &path_status) == 0) {
    target_exists = stat(path, &status) == 0;
    if (!target_exists && errno != ENOENT) {
      diagnostic(DIAGNOSTIC_WARNING, "cannot inspect device target %s: %s",
                 path, strerror(errno));
      return -1;
    }
    if (target_exists && S_ISCHR(status.st_mode) &&
        major(status.st_rdev) == major_number &&
        minor(status.st_rdev) == minor_number) {
      if ((status.st_mode & 07777) == permissions ||
          chmod(path, permissions) == 0)
        return 0;
      diagnostic(DIAGNOSTIC_WARNING,
                 "cannot set device node permissions on %s: %s", path,
                 strerror(errno));
      return -1;
    }
    if (unlink(path) != 0) {
      diagnostic(DIAGNOSTIC_WARNING, "cannot remove invalid device path %s: %s",
                 path, strerror(errno));
      return -1;
    }
  } else if (errno != ENOENT) {
    diagnostic(DIAGNOSTIC_WARNING, "cannot inspect device path %s: %s", path,
               strerror(errno));
    return -1;
  }

  if (mknod(path, S_IFCHR | permissions, makedev(major_number, minor_number)) !=
      0) {
    diagnostic(DIAGNOSTIC_WARNING, "cannot create device node %s: %s", path,
               strerror(errno));
    return -1;
  }
  return 0;
}

static void create_device_links(const char *device_directory) {
  static const struct {
    const char *name;
    const char *target;
  } links[] = {{"fd", "/proc/self/fd"},
               {"stdin", "/proc/self/fd/0"},
               {"stdout", "/proc/self/fd/1"},
               {"stderr", "/proc/self/fd/2"}};
  size_t index;

  for (index = 0; index < sizeof(links) / sizeof(links[0]); index++) {
    char path[PATH_MAX];

    if (format_root_path(path, sizeof(path), device_directory,
                         links[index].name) != 0) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "cannot construct device link path");
      continue;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
      diagnostic_errno(DIAGNOSTIC_WARNING, "cannot replace device link");
      continue;
    }
    if (symlink(links[index].target, path) != 0)
      diagnostic_errno(DIAGNOSTIC_WARNING, "cannot create device link");
  }
}

static void populate_device_directory(const char *device_directory) {
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

static int device_null_is_usable(const char *device_directory) {
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

static int setup_device_filesystem(void) {
  const char *device_directory = "/dev";

  if (ensure_directory(device_directory, 0755) != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot prepare /dev");
    return -1;
  }
  if (device_null_is_usable(device_directory)) {
    populate_device_directory(device_directory);
  } else if (mount_filesystem("tmpfs", device_directory, "tmpfs", MS_NOSUID,
                              "mode=0755") == 0) {
    populate_device_directory(device_directory);
  } else {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot provide /dev");
    return -1;
  }

  if (!device_null_is_usable(device_directory)) {
    diagnostic(DIAGNOSTIC_ERROR, "no usable character device at /dev/null");
    return -1;
  }
  return 0;
}

static int mount_private_devpts(void) {
  static const char full_options[] = "mode=0620,ptmxmode=0666,newinstance";
  static const char compatible_options[] = "mode=0620,newinstance";
  int first_error;

  if (mount_filesystem("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
                       full_options) == 0)
    return 0;

  first_error = errno;

  if (mount_filesystem("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
                       compatible_options) == 0) {
    diagnostic(
        DIAGNOSTIC_WARNING,
        "devpts rejected ptmxmode option (%s); validating node permissions",
        strerror(first_error));
    return 0;
  }

  diagnostic_errno(DIAGNOSTIC_WARNING, "cannot mount private devpts");
  return -1;
}

static int private_devpts_multiplexer_is_usable(void) {
  struct stat status;
  int descriptor;

  if (stat("/dev/pts/ptmx", &status) != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "private devpts has no usable /dev/pts/ptmx");
    return 0;
  }
  if (!S_ISCHR(status.st_mode) || major(status.st_rdev) != 5 ||
      minor(status.st_rdev) != 2) {
    diagnostic(DIAGNOSTIC_WARNING,
               "/dev/pts/ptmx is not the expected character device; keeping "
               "/dev/ptmx fallback");
    return 0;
  }

  if ((status.st_mode & 07777) != 0666 && chmod("/dev/pts/ptmx", 0666) != 0) {
    diagnostic_errno(
        DIAGNOSTIC_WARNING,
        "cannot set /dev/pts/ptmx permissions; keeping /dev/ptmx fallback");
    return 0;
  }
  if (stat("/dev/pts/ptmx", &status) != 0 || !S_ISCHR(status.st_mode) ||
      major(status.st_rdev) != 5 || minor(status.st_rdev) != 2 ||
      (status.st_mode & 0666) != 0666) {
    diagnostic(DIAGNOSTIC_WARNING,
               "/dev/pts/ptmx remains unusable; keeping /dev/ptmx fallback");
    return 0;
  }

  descriptor = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (descriptor < 0) {
    diagnostic_errno(
        DIAGNOSTIC_WARNING,
        "cannot open private /dev/pts/ptmx; keeping /dev/ptmx fallback");
    return 0;
  }
  if (close(descriptor) != 0) {
    diagnostic_errno(
        DIAGNOSTIC_WARNING,
        "cannot close private /dev/pts/ptmx; keeping /dev/ptmx fallback");
    return 0;
  }
  return 1;
}

static int link_private_devpts_multiplexer(void) {
  if (!private_devpts_multiplexer_is_usable())
    return 0;

  if (unlink("/dev/ptmx") != 0 && errno != ENOENT) {
    diagnostic_errno(DIAGNOSTIC_WARNING, "cannot replace /dev/ptmx");
    return 0;
  }
  if (symlink("pts/ptmx", "/dev/ptmx") != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "cannot link /dev/ptmx to private devpts");
    (void)create_device("/dev", "ptmx", 5, 2, 0666);
    return 0;
  }
  return 1;
}

static int mount_optional_filesystem(const char *source, const char *target,
                                     const char *type, unsigned long flags,
                                     const char *options) {
  if (mount_filesystem(source, target, type, flags, options) == 0)
    return 0;
  diagnostic_errno(DIAGNOSTIC_WARNING, target);
  return -1;
}

#if FEATURE_DEFAULT_RESOLVER
static int write_default_resolver(void) {
  static const char resolver_contents[] =
      "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
  size_t written = 0;
  int descriptor = open("/run/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);

  if (descriptor < 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING, "cannot create /run/resolv.conf");
    return -1;
  }
  while (written < sizeof(resolver_contents) - 1) {
    ssize_t count = write(descriptor, resolver_contents + written,
                          sizeof(resolver_contents) - 1 - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (count == 0)
        errno = EIO;
      diagnostic_errno(DIAGNOSTIC_WARNING, "cannot write /run/resolv.conf");
      (void)close(descriptor);
      return -1;
    }
    written += (size_t)count;
  }
  if (close(descriptor) != 0) {
    diagnostic_errno(DIAGNOSTIC_WARNING, "cannot close /run/resolv.conf");
    return -1;
  }
  return 0;
}
#endif

int filesystem_locate_root(char *root, size_t root_size, const char *argv0) {
  char executable_path[PATH_MAX];
  char resolved_root[PATH_MAX];
  ssize_t path_length;
  char *last_separator;

  path_length =
      readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
  if (path_length >= 0) {
    if ((size_t)path_length >= sizeof(executable_path) - 1) {
      errno = ENAMETOOLONG;
      return -1;
    }
    executable_path[path_length] = '\0';
  } else {
    if (!argv0 || !strchr(argv0, '/')) {
      errno = ENOENT;
      return -1;
    }
    if (snprintf(executable_path, sizeof(executable_path), "%s", argv0) >=
        (int)sizeof(executable_path)) {
      errno = ENAMETOOLONG;
      return -1;
    }
  }

  last_separator = strrchr(executable_path, '/');
  if (!last_separator) {
    errno = EINVAL;
    return -1;
  }
  if (last_separator == executable_path)
    executable_path[1] = '\0';
  else
    *last_separator = '\0';

  if (!realpath(executable_path, resolved_root))
    return -1;
  if (snprintf(root, root_size, "%s", resolved_root) >= (int)root_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int filesystem_validate_root(const char *root) {
  static const char *const required_directories[] = {"usr", "tmp"};
  size_t index;

  for (index = 0;
       index < sizeof(required_directories) / sizeof(required_directories[0]);
       index++) {
    char path[PATH_MAX];
    struct stat status;

    if (format_root_path(path, sizeof(path), root,
                         required_directories[index]) != 0 ||
        stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
      diagnostic(DIAGNOSTIC_ERROR, "%s is not a rootfs (missing /%s)", root,
                 required_directories[index]);
      return -1;
    }
  }
  return 0;
}

int filesystem_find_executable(const char *root, const char *executable,
                               char *out_path, size_t out_path_size) {

  if (strchr(executable, '/') != NULL) {
    char path[PATH_MAX];
    struct stat status;

    if (format_root_path(path, sizeof(path), root, executable) != 0) {
      errno = ENAMETOOLONG;
      return -1;
    }
    if (stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
        access(path, X_OK) == 0) {
      if (snprintf(out_path, out_path_size, "/%s",
                   executable[0] == '/' ? executable + 1 : executable) >=
          (int)out_path_size) {
        errno = ENAMETOOLONG;
        return -1;
      }
      return 0;
    }
    errno = ENOENT;
    return -1;
  }

  size_t index;

  for (index = 0; search_paths[index]; index++) {
    char candidate[PATH_MAX];
    struct stat status;

    if (format_root_path(candidate, sizeof(candidate), root,
                         search_paths[index]) != 0)
      continue;

    size_t len = strlen(candidate);
    size_t exe_len = strlen(executable);

    if (len + 1 + exe_len + 1 > sizeof(candidate)) {
      continue;
    }

    candidate[len] = '/';
    memcpy(candidate + len + 1, executable, exe_len + 1);
    if (stat(candidate, &status) == 0 && S_ISREG(status.st_mode) &&
        access(candidate, X_OK) == 0) {
      if (snprintf(out_path, out_path_size, "/%s/%s", search_paths[index],
                   executable) >= (int)out_path_size) {
        errno = ENAMETOOLONG;
        return -1;
      }
      return 0;
    }
  }
  errno = ENOENT;
  return -1;
}

int filesystem_find_login(const char *root, char *program,
                          size_t program_size) {
  return filesystem_find_executable(root, "login", program, program_size);
}

int filesystem_is_readonly(void) {
  char test_path[] = ".enter-write-test-XXXXXX";
  int descriptor = mkstemp(test_path);

  if (descriptor >= 0) {
    (void)close(descriptor);
    (void)unlink(test_path);
    return 0;
  }
  return 1;
}

int filesystem_mount_root(int root_is_readonly) {
#if FEATURE_OVERLAY
  if (FEATURE_FORCED_OVERLAY || root_is_readonly) {
    if (overlayfs_is_supported() && mount_overlay_root() == 0)
      return 0;
#if FEATURE_REQUIRED_OVERLAY
    diagnostic(DIAGNOSTIC_ERROR, "overlay is required");
    return -1;
#endif
  }
#endif
  if (root_is_readonly)
    diagnostic(DIAGNOSTIC_WARNING,
               "rootfs is read-only and overlay is not available");

  if (mount_filesystem(".", ".", NULL, MS_BIND, NULL) != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot bind rootfs mountpoint");
    return -1;
  }
  if (chdir(".") != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot select rootfs mountpoint");
    return -1;
  }
  return 0;
}

#if FEATURE_ENTER_OPEN_TREE && defined(SYS_open_tree) && defined(SYS_move_mount)
static int enter_with_open_tree(void) {
  int root_descriptor;
  char resolved[PATH_MAX];

  if (!realpath(".", resolved))
    return -1;

  root_descriptor = syscall(SYS_open_tree, AT_FDCWD, resolved,
                            OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
  if (root_descriptor < 0)
    return -1;

  if (syscall(SYS_move_mount, root_descriptor, "", AT_FDCWD, "/",
              MOVE_MOUNT_F_EMPTY_PATH) != 0) {
    int saved_errno = errno;
    close(root_descriptor);
    errno = saved_errno;
    return -1;
  }

  if (fchdir(root_descriptor) != 0) {
    int saved_errno = errno;
    close(root_descriptor);
    errno = saved_errno;
    return -1;
  }

  if (chroot(".") != 0) {
    int saved_errno = errno;
    close(root_descriptor);
    errno = saved_errno;
    return -1;
  }

  if (close(root_descriptor) != 0)
    return -1;

  if (chdir("/") != 0)
    return -1;

  return 0;
}
#endif

#if FEATURE_ENTER_PIVOT_ROOT && defined(SYS_pivot_root)
static int enter_with_pivot_root(void) {
  char old_root[] = ".enter-oldroot-XXXXXX";

  char resolved[PATH_MAX];
  if (!realpath(".", resolved)) {
    return -1;
  }

  if (!mkdtemp(old_root))
    return -1;
  if (syscall(SYS_pivot_root, resolved, old_root) != 0) {
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
    diagnostic_errno(DIAGNOSTIC_WARNING,
                     "cannot remove previous root directory");
  return 0;
}
#endif

#if FEATURE_ENTER_MOVE_MOUNT
static int enter_with_move_mount(void) {
  char resolved[PATH_MAX];
  if (!realpath(".", resolved))
    return -1;

  if (mount(resolved, "/", NULL, MS_MOVE, NULL) != 0)
    return -1;

  if (chdir("/") != 0)
    return -1;

  if (chroot(".") != 0)
    return -1;

  return 0;
}
#endif

int filesystem_enter_root(void) {
#if FEATURE_ENTER_OPEN_TREE && defined(SYS_open_tree) && defined(SYS_move_mount)
  if (enter_with_open_tree() == 0)
    return 0;
  if (errno != ENOSYS)
    diagnostic_errno(DIAGNOSTIC_WARNING, "open_tree failed");
#endif

#if FEATURE_ENTER_PIVOT_ROOT && defined(SYS_pivot_root)
  if (enter_with_pivot_root() == 0)
    return 0;
  diagnostic_errno(DIAGNOSTIC_WARNING, "pivot_root failed");
#endif

#if FEATURE_ENTER_MOVE_MOUNT
  if (enter_with_move_mount() == 0)
    return 0;
  diagnostic_errno(DIAGNOSTIC_WARNING, "move-mount failed");
#endif

#if FEATURE_ENTER_CHROOT
  if (chroot(".") == 0 && chdir("/") == 0)
    return 0;
#endif

#if !FEATURE_ENTER_OPEN_TREE && !FEATURE_ENTER_PIVOT_ROOT &&                   \
    !FEATURE_ENTER_MOVE_MOUNT && !FEATURE_ENTER_CHROOT
#error "At least one root-entry strategy must be enabled"
#endif

  diagnostic_errno(DIAGNOSTIC_ERROR, "cannot enter rootfs");
  return -1;
}

int filesystem_setup_pseudo_filesystems(int *private_devpts_available) {
  *private_devpts_available = 0;

  if (setup_device_filesystem() != 0)
    return -1;

  if (ensure_directory("/proc", 0555) != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot prepare /proc");
    return -1;
  }
  if (mount_filesystem("proc", "/proc", "proc",
                       MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
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
  else {
    (void)mount_optional_filesystem("tmpfs", "/run", "tmpfs",
                                    MS_NOSUID | MS_NODEV, "mode=0755");
#if FEATURE_DEFAULT_RESOLVER
    (void)write_default_resolver();
#endif
    if (ensure_directory("/run/lock", 0755) != 0)
      diagnostic_errno(DIAGNOSTIC_WARNING, "cannot prepare /run/lock");
  }
  return 0;
}
