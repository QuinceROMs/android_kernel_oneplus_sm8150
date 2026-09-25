#ifndef __KSU_H_SELINUX_HIDE
#define __KSU_H_SELINUX_HIDE

#include <linux/types.h>

int sepol_expected_argc(u32 cmd);

void ksu_selinux_hide_init();
void ksu_selinux_hide_exit();
void ksu_selinux_hide_handle_second_stage();
void ksu_selinux_hide_handle_post_fs_data();

// selinux/rules.c feeds every applied sepolicy command through this so the
// hide lists track what ksu actually loaded
void ksu_add_probe_to_list(u32 cmd, const char *args[]);

#endif
