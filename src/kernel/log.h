/* WindClone — boot messages ring buffer.
 * Drivers log through klog(): serial echo + a fixed ring of the most
 * recent LOG_MAX messages kept in kernel memory. Phase 5's debug shell
 * reads it back, so boot evidence survives even when nobody watched.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_LOG_H
#define WINDCLONE_LOG_H

void log_init(void);
void klog(const char *fmt, ...);      /* log message + echo to serial */
void log_message(const char *fmt, ...); /* record only, no echo */
void log_dump_tail(int n);            /* print the n most recent messages */
int  log_entries(void);

#endif
