/* SPDX-License-Identifier: MIT */
#ifndef ENTER_NAMESPACES_H
#define ENTER_NAMESPACES_H

int namespaces_prepare_user(void);
int namespaces_try_pid(void);
int namespaces_setup_child(void);

#endif