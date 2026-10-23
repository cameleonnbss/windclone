/* WindClone — initrd: the tar archive the bootloader gives us.
 *
 * Limine loads `module_path:` files into memory next to the kernel and
 * hands us their address + size. We treat the first module as our root
 * filesystem for Phase 4: a plain ustar tar, parsed *in place* (the data
 * already sits in RAM, so copying it would only cost memory).
 *
 * This is deliberately not a filesystem yet — no inodes, no writes, no
 * paths with "..", no VFS. initrd_find() is the one lookup the ELF
 * loader and the future shell need: "give me the bytes of /bin/hello".
 *
 * Call initrd_init() *after* kmalloc_init(): the file index lives on the
 * kernel heap.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_INITRD_H
#define WINDCLONE_INITRD_H

#include <stdint.h>
#include <stddef.h>

#define INITRD_NAME_MAX 100

struct initrd_file {
    char           name[INITRD_NAME_MAX];  /* no leading '/', NUL-terminated */
    const uint8_t *data;                   /* points into the module itself */
    uint64_t       size;
};

/* Parse the first boot module. 0 = archive mounted, -1 = no module (or
 * it is not a readable ustar archive). Never panics: a machine without
 * an initrd should still boot and say so. */
int initrd_init(void);

unsigned initrd_count(void);
const struct initrd_file *initrd_at(unsigned index);

/* -1 on an unparsable/absent archive. */
const struct initrd_file *initrd_find(const char *path);  /* "/etc/motd" or "etc/motd" */

const char *initrd_module_path(void);
uint64_t    initrd_module_size(void);

#endif
