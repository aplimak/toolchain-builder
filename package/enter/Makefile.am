AM_CFLAGS = -std=gnu99 -Wall -Wextra -Wformat=2
EXTRA_DIST = README.md

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