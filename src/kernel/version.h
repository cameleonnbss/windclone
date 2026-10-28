/* WindClone — the version the running kernel says out loud.
 *
 * One header, one string: the status window, the taskbar and `about` all
 * read it, because three copies of a version number are three chances to be
 * wrong — and two of them were. The desktop drew "0.1.0" and the shell
 * answered "0.0.4" to `about` for whole releases after both numbers had
 * stopped being true, which is exactly the kind of claim a test suite is for:
 * tools/local-test.sh reads this file and requires the boot log to announce
 * the same version it finds here.
 *
 * The git tag is what names a released ISO; this is what the kernel itself
 * reports, and the two are bumped in the same commit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_VERSION_H
#define WINDCLONE_VERSION_H

#define WC_VERSION "0.1.26"

#endif
