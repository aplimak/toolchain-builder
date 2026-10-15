/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef FEATURE_CUSTOM_PROGRAM
#define FEATURE_CUSTOM_PROGRAM 1
#endif
#ifndef FEATURE_INIT_SCRIPTS
#define FEATURE_INIT_SCRIPTS 1
#endif

#include "container.h"
#include "diagnostics.h"
#include "filesystem.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef PACKAGE_STRING
#define PACKAGE_STRING "enter"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int main(int argument_count, char *arguments[]) {
  if (argument_count >= 2) {
    if (strcmp(arguments[1], "--help") == 0) {
      printf("usage: ");
      if (FEATURE_CUSTOM_PROGRAM) {
        printf("%s [program] [args]... \tEstablish environment and "
               "run provided program or login\n",
               arguments[0]);
      } else {
        printf("%s \t\t\tEstablish environment and "
               "run login program\n",
               arguments[0]);
      }
      printf("       %s --help\n", arguments[0]);
      printf("       %s --version\n", arguments[0]);
      return 0;
    }
    if (strcmp(arguments[1], "--version") == 0) {
      printf("%s\n", PACKAGE_STRING);
      return 0;
    }
    if (!FEATURE_CUSTOM_PROGRAM) {
      fprintf(stderr, "Unknown option: %s\n", arguments[1]);
      return 1;
    }
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
    run_init_scripts = FEATURE_INIT_SCRIPTS;
  }

  return container_run(program, (char *const *)program_arguments,
                       run_init_scripts);
}
