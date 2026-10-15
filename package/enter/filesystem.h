/* SPDX-License-Identifier: MIT */
#ifndef ENTER_FILESYSTEM_H
#define ENTER_FILESYSTEM_H

#include <stddef.h>

/**
 * Locates the root directory of the filesystem, and stores it in `root`.
 * @param root The buffer to store the root directory in.
 * @param root_size The size of the `root` buffer.
 * @param argv0 The path of the executable being run.
 * @return 0 on success, or a negative error code on failure.
 */
int filesystem_locate_root(char *root, size_t root_size, const char *argv0);
/**
 * Validates the root directory, and returns 0 on success, or a negative error
 * code on failure.
 * @param root The root directory to validate.
 * @return 0 on success, or a negative error code on failure.
 */
int filesystem_validate_root(const char *root);
/**
 * Finds the executable named `executable` in the `root` directory, and stores
 * the absolute path without the `root` prefix in `out_path`.
 * @param root The root directory to search in.
 * @param executable The name of the executable to find.
 * @param out_path The buffer to store the absolute path in.
 * @param out_psth_size The size of the `out_path` buffer.
 * @return 0 on success, or a negative error code on failure.
 */
int filesystem_find_executable(const char *root, const char *executable,
                               char *out_path, size_t out_psth_size);
/**
 * Finds the login program in the `root` directory, and stores the absolute path
 * without the `root` prefix in `program`.
 * @param root The root directory to search in.
 * @param program The buffer to store the absolute path in.
 * @param program_size The size of the `program` buffer.
 * @return 0 on success, or a negative error code on failure.
 */
int filesystem_find_login(const char *root, char *program, size_t program_size);
/**
 * Returns whether the current directory is read-only.
 * @return 1 if the current directory is read-only, 0 otherwise.
 */
int filesystem_is_readonly(void);
int filesystem_mount_root(int root_is_readonly);
int filesystem_enter_root(void);
int filesystem_setup_pseudo_filesystems(int *private_devpts_available);
int filesystem_read_hostname(char *out, size_t out_size);

#endif
