AM_CFLAGS = -std=gnu99 -Wall -Wextra -Wformat=2
EXTRA_DIST = README.md tests/test_enter.c

bin_PROGRAMS = enter
enter_SOURCES = \
	main.c \
	container.c \
	container.h \
	diagnostics.c \
	diagnostics.h \
	environment.c \
	environment.h \
	filesystem.c \
	filesystem.h \
	namespaces.c \
	namespaces.h \
	process.c \
	process.h
enter_LDFLAGS = @ENTER_STATIC_LDFLAGS@

if BUILD_TESTS
check_PROGRAMS = tests/test_enter
TESTS = $(check_PROGRAMS)
tests_test_enter_SOURCES = \
	tests/test_enter.c \
	diagnostics.c \
	environment.c \
	filesystem.c \
	process.c
tests_test_enter_CPPFLAGS = -DHAVE_CONFIG_H -I$(top_builddir) -I$(top_srcdir)
tests_test_enter_CFLAGS = $(AM_CFLAGS)
endif