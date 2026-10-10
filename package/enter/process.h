/* SPDX-License-Identifier: MIT */
#ifndef ENTER_PROCESS_H
#define ENTER_PROCESS_H

int process_run_optional_script(const char *path, int *was_started);
int process_run_program(const char *program, char *const arguments[],
                        int use_private_pty);
void process_set_name(const char *name);

#endif