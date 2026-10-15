AC_PREREQ([2.69])
AC_INIT([enter], [1.0])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_MAINTAINER_MODE([disable])
AC_CONFIG_HEADERS([config.h])

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC

dnl Buildroot passes these common Autotools options even for a single binary.
AC_ARG_ENABLE([gtk-doc], [], [], [])
AC_ARG_ENABLE([gtk-doc-html], [], [], [])
AC_ARG_ENABLE([doc], [], [], [])
AC_ARG_ENABLE([docs], [], [], [])
AC_ARG_ENABLE([documentation], [], [], [])
AC_ARG_ENABLE([ipv6], [], [], [])
AC_ARG_ENABLE([nls], [], [], [])
AC_ARG_ENABLE([shared], [], [], [])
AC_ARG_WITH([xmlto], [], [], [])
AC_ARG_WITH([fop], [], [], [])

AC_CHECK_HEADERS([
    sched.h
    sys/prctl.h
    sys/syscall.h
    sys/sysmacros.h
    fcntl.h
    sys/ioctl.h
    sys/mount.h
    termios.h
    unistd.h
], [], [AC_MSG_ERROR([a required Linux system header is missing])])

AC_CHECK_FUNCS([
    clearenv
    setresgid
    setresuid
    unshare
    alarm
    dup2
    fchdir
    memmove
    memset
    mkdir
    realpath
    rmdir
    setenv
    sethostname
    strchr
    strcspn
    strdup
    strerror
    strrchr
    strstr
    ftruncate
    fopen
    fgets
    fclose
], [], [AC_MSG_ERROR([a required Linux system function is missing])])

AC_TYPE_UID_T
AC_TYPE_OFF_T
AC_TYPE_MODE_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_rdev])

AC_FUNC_MALLOC
AC_FUNC_FORK
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_HEADER_MAJOR

# ----- helper: auto/yes/no feature --------------------------------------
AC_DEFUN([ENTER_FEATURE], [
  m4_pushdef([enter_var],   [enter_]m4_translit([$1], [-], [_]))
  m4_pushdef([enter_macro], [FEATURE_]m4_translit([$1], [a-z-], [A-Z_]))

  AC_ARG_ENABLE([$1],
    [AS_HELP_STRING([--enable-$1], [$2 @<:@default=$3@:>@])],
    [enter_var=$enableval],
    [enter_var=$3])

  m4_if([$4], [], [], [
    if test "x$enter_var" = "xauto"; then
      $4
    fi
  ])

  AS_IF([test "x$enter_var" = "xyes"],
        [AC_DEFINE([enter_macro], [1], [$2])],
        [AC_DEFINE([enter_macro], [0], [])])
  AM_CONDITIONAL([enter_macro], [test "x$enter_var" = "xyes"])

  m4_popdef([enter_var])
  m4_popdef([enter_macro])
])

AC_ARG_ENABLE(
    [runtime-warnings],
    [AS_HELP_STRING([--disable-runtime-warnings], [suppress recoverable runtime warning messages])],
    [],
    [enable_runtime_warnings=yes]
)

AS_IF([test "x$enable_runtime_warnings" = "xyes"],
    [AC_DEFINE([ENABLE_RUNTIME_WARNINGS], [1], [Emit recoverable runtime warning messages])],
    [AC_DEFINE([ENABLE_RUNTIME_WARNINGS], [0], [Emit recoverable runtime warning messages])]
)

ENTER_FEATURE([custom-program],
  [Allow running custom program instead of default login], [yes])
ENTER_FEATURE([init-scripts],
  [Automatically run init scripts if ran with no argument], [yes])

ENTER_FEATURE([overlay],
  [Support OverlayFS writable root], [yes])

AC_ARG_ENABLE([forced-overlay],
  [AS_HELP_STRING([--enable-forced-overlay],
                  [Always use overlay even if rootfs is writable @<:@default=no@:>@])],
  [enter_forced_overlay=$enableval],
  [enter_forced_overlay=no])

AS_IF([test "x$enter_forced_overlay" = "xyes" -a \
            "x$enter_overlay"        != "xyes"],
  [AC_MSG_ERROR([--enable-forced-overlay requires --enable-overlay])])

AS_IF([test "x$enter_forced_overlay" = "xyes"],
  [AC_DEFINE([FEATURE_FORCED_OVERLAY], [1],
             [Always use overlay even if rootfs is writable])],
  [AC_DEFINE([FEATURE_FORCED_OVERLAY], [0], [])])
AM_CONDITIONAL([FORCED_OVERLAY], [test "x$enter_forced_overlay" = "xyes"])

AC_ARG_ENABLE([required-overlay],
  [AS_HELP_STRING([--enable-required-overlay],
                  [Error out instead of falling back when overlay mount fails @<:@default=no@:>@])],
  [enter_required_overlay=$enableval],
  [enter_required_overlay=no])

AS_IF([test "x$enter_required_overlay" = "xyes" -a \
            "x$enter_overlay"        != "xyes"],
  [AC_MSG_ERROR([--enable-required-overlay requires --enable-overlay])])

AS_IF([test "x$enter_required_overlay" = "xyes"],
  [AC_DEFINE([FEATURE_REQUIRED_OVERLAY], [1],
             [Error out instead of falling back when overlay mount fails])],
  [AC_DEFINE([FEATURE_REQUIRED_OVERLAY], [0], [])])
AM_CONDITIONAL([REQUIRED_OVERLAY], [test "x$enter_required_overlay" = "xyes"])

AC_ARG_ENABLE([image-overlay],
  [AS_HELP_STRING([--enable-image-overlay],
    [ext4 image-backed overlay fallback @<:@default=auto@:>@])],
  [enter_image_overlay=$enableval],
  [enter_image_overlay=auto])

if test "x$enter_overlay" != "xyes"; then
  AS_IF([test "x$enter_image_overlay" = "xyes"],
        [AC_MSG_WARN([--enable-image-overlay ignored: overlay is disabled])])
  enter_image_overlay=no
fi

if test "x$enter_overlay" = "xyes" -a "x$enter_image_overlay" != "xno"; then
  enter_image_overlay_usable=yes
  AC_CHECK_HEADERS([linux/loop.h sys/xattr.h], [],
    [enter_image_overlay_usable=no])
  AC_CHECK_FUNCS([fsetxattr], [], [enter_image_overlay_usable=no])

  if test "x$enter_image_overlay_usable" = "xyes"; then
    enter_image_overlay=yes
  else
    AS_IF([test "x$enter_image_overlay" = "xyes"],
      [AC_MSG_ERROR([--enable-image-overlay was requested but linux/loop.h,
                     sys/xattr.h or fsetxattr are missing])],
      [enter_image_overlay=no])
  fi
fi

AS_IF([test "x$enter_image_overlay" = "xyes"],
  [AC_DEFINE([FEATURE_IMAGE_OVERLAY], [1], [ext4 image-backed overlay fallback])],
  [AC_DEFINE([FEATURE_IMAGE_OVERLAY], [0], [])])
AM_CONDITIONAL([IMAGE_OVERLAY],
  [test "x$enter_image_overlay" = "xyes"])

ENTER_FEATURE([pty-relay],
  [Private PTY relay with window-size handling], [yes])
ENTER_FEATURE([user-namespace],
  [Attempt CLONE_NEWUSER when invoked with UID!=0], [yes])
ENTER_FEATURE([pid-namespace],
  [Attempt CLONE_NEWPID], [yes])
ENTER_FEATURE([uts-namespace],
  [Attempt CLONE_NEWUTS], [yes])
ENTER_FEATURE([ipc-namespace],
  [Attempt CLONE_NEWIPC], [yes])
ENTER_FEATURE([enter-open-tree],
  [Use open_tree/move_mount to enter rootfs], [yes])
ENTER_FEATURE([enter-pivot-root],
  [Use pivot_root to enter rootfs], [yes])
ENTER_FEATURE([enter-move-mount],
  [Use MS_MOVE+chroot to enter rootfs], [yes])
ENTER_FEATURE([enter-chroot],
  [Use plain chroot as last resort], [no])
ENTER_FEATURE([default-resolver],
  [Write a default /run/resolv.conf], [yes])

AC_ARG_WITH([selinux],
  [AS_HELP_STRING([--with-selinux],
                  [Use libselinux for ext4 image labeling @<:@default=no@:>@])],
  [enter_selinux=$withval],
  [enter_selinux=no])

AS_CASE([$enter_selinux],
  [yes], [],
  [no],  [],
  [*],   [AC_MSG_ERROR([--with-selinux only accepts yes or no])])

AS_IF([test "x$enter_selinux" = "xyes"], [
  PKG_CHECK_MODULES([SELINUX], [libselinux])
  AC_DEFINE([HAVE_LIBSELINUX], [1], [Use libselinux])
], [
  AC_DEFINE([HAVE_LIBSELINUX], [0], [])
])
AM_CONDITIONAL([HAVE_LIBSELINUX], [test "x$enter_selinux" = "xyes"])

AC_ARG_ENABLE([binary-trimming],
  [AS_HELP_STRING([--disable-binary-trimming],
                  [Skip --gc-sections / -fno-unwind-tables etc. @<:@default=enabled@:>@])],
  [], [enable_binary_trimming=yes])

AS_IF([test "x$enable_binary_trimming" = "xyes"], [
  CFLAGS="$CFLAGS -ffunction-sections -fdata-sections \
-fno-unwind-tables -fno-asynchronous-unwind-tables \
-fno-ident"
  LDFLAGS="$LDFLAGS -Wl,--gc-sections -Wl,--build-id=none"
])

AC_ARG_WITH([mke2fs],
    [AS_HELP_STRING([--with-mke2fs=PATH],
        [name or path to mke2fs @<:@default=mke2fs@:>@])],
    [host_mke2fs="$withval"],
    [host_mke2fs="mke2fs"]
)

AC_DEFINE([MKE2FS_PATH], ["$host_mke2fs"],
    [Path to mke2fs])

AC_ARG_ENABLE(
    [tests],
    [AS_HELP_STRING([--enable-tests], [build the optional native test suite])],
    [],
    [enable_tests=no]
)
AM_CONDITIONAL([BUILD_TESTS], [test "x$enable_tests" = "xyes"])

AC_ARG_ENABLE(
    [static],
    [AS_HELP_STRING([--enable-static], [link enter statically])],
    [],
    [enable_static=no]
)

ENTER_STATIC_LDFLAGS=
AS_IF([test "x$enable_static" = "xyes"], [
    enter_saved_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS -static"
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([], [])],
        [ENTER_STATIC_LDFLAGS=-static],
        [AC_MSG_ERROR([static linking was requested but is unavailable])]
    )
    LDFLAGS=$enter_saved_LDFLAGS
])
AC_SUBST([ENTER_STATIC_LDFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
