/* linux_firstboot.h - the first-boot setup every Linux guest runs. */
#ifndef LINUX_FIRSTBOOT_H
#define LINUX_FIRSTBOOT_H

#include <wchar.h>

const char *linux_firstboot_script(void);
const char *linux_firstboot_unit(void);

/* Writes both into out_dir. Returns 0 on success. */
int do_write_firstboot(const wchar_t *out_dir);

#endif
