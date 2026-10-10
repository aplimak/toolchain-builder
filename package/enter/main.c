/* SPDX-License-Identifier: MIT */
#include "container.h"
#include "diagnostics.h"
#include "filesystem.h"

#include <limits.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int main(int argument_count, char *arguments[])
{
    char root[PATH_MAX];
    char login_program[PATH_MAX];
    char *automatic_arguments[4];
    const char *program;
    char *const *program_arguments;
    int run_init_scripts = 0;

    if (filesystem_locate_root(root, sizeof(root), arguments[0]) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot determine launcher directory");
        return 1;
    }
    if (filesystem_validate_root(root) != 0)
        return 1;
    if (chdir(root) != 0)
    {
        diagnostic_errno(DIAGNOSTIC_ERROR, "cannot select rootfs directory");
        return 1;
    }

    if (argument_count >= 2)
    {
        program = arguments[1];
        program_arguments = &arguments[1];
    }
    else
    {
        if (filesystem_find_login(root, login_program, sizeof(login_program)) != 0)
        {
            diagnostic(DIAGNOSTIC_ERROR,
                       "no executable login program found in %s", root);
            return 1;
        }
        automatic_arguments[0] = login_program;
        automatic_arguments[1] = "-f";
        automatic_arguments[2] = "root";
        automatic_arguments[3] = NULL;
        program = login_program;
        program_arguments = automatic_arguments;
        run_init_scripts = 1;
    }

    return container_run(program, (char *const *)program_arguments,
                         run_init_scripts);
}