/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "container.h"
#include "diagnostics.h"
#include "filesystem.h"

#include <limits.h>
#include <unistd.h>

#ifndef PACKAGE_STRING
#define PACKAGE_STRING "enter"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int main(int argument_count, char *arguments[]) {
  if (argument_count >= 2 && strcmp(arguments[1], "--version") == 0) {
    printf("%s\n", PACKAGE_STRING);
    return 0;
  }

  char root[PATH_MAX];
  char found_program_path[PATH_MAX];
  char *automatic_arguments[4];
  const char *program;
  char *const *program_arguments;
  int run_init_scripts = 0;

  if (filesystem_locate_root(root, sizeof(root), arguments[0]) != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot determine launcher directory");
    return 1;
  }
  if (filesystem_validate_root(root) != 0)
    return 1;
  if (chdir(root) != 0) {
    diagnostic_errno(DIAGNOSTIC_ERROR, "cannot select rootfs directory");
    return 1;
  }

  if (argument_count >= 2) {
    if (filesystem_find_executable(root, arguments[1], found_program_path,
                                   sizeof(found_program_path)) != 0) {
      diagnostic_errno(DIAGNOSTIC_ERROR, "can't find program");
      return 1;
    }
    program = found_program_path;
    program_arguments = &arguments[1];
  } else {
    if (filesystem_find_login(root, found_program_path,
                              sizeof(found_program_path)) != 0) {
      diagnostic_errno(DIAGNOSTIC_ERROR, "can't find login program");
      return 1;
    }
    automatic_arguments[0] = found_program_path;
    automatic_arguments[1] = "-f";
    automatic_arguments[2] = "root";
    automatic_arguments[3] = NULL;
    program = found_program_path;
    program_arguments = automatic_arguments;
    run_init_scripts = 1;
  }

  return container_run(program, (char *const *)program_arguments,
                       run_init_scripts);
}
