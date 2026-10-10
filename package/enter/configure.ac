AC_PREREQ([2.69])
AC_INIT([enter], [1.0])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign])
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
    sys/mount.h
    sys/prctl.h
    sys/syscall.h
    sys/sysmacros.h
], [], [AC_MSG_ERROR([a required Linux system header is missing])])

AC_CHECK_FUNCS([
    clearenv
    sethostname
    setresgid
    setresuid
    unshare
], [], [AC_MSG_ERROR([a required Linux system function is missing])])

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