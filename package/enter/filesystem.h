/* SPDX-License-Identifier: MIT */
#ifndef ENTER_FILESYSTEM_H
#define ENTER_FILESYSTEM_H

#include <stddef.h>

int filesystem_locate_root(char *root, size_t root_size, const char *argv0);
int filesystem_validate_root(const char *root);
int filesystem_find_login(const char *root, char *program, size_t program_size);
int filesystem_is_readonly(void);
int filesystem_mount_root(int root_is_readonly);
int filesystem_enter_root(void);
int filesystem_setup_pseudo_filesystems(int *private_devpts_available);

#endif