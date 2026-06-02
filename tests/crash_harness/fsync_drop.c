/*
 * fsync_drop.so — LD_PRELOAD shim that turns disk-flush syscalls into no-ops.
 *
 * Purpose: a poor man's dm-log-writes for environments without root.
 *   - The single_txn branch claims durability: an HTTP-200 insert means the
 *     shared MDBX txn fsync'd before returning. That guarantee depends on the
 *     OS actually flushing dirty pages to disk.
 *   - If we drop fsync/fdatasync/msync and SIGKILL the server, dirty pages in
 *     the page cache never reach disk. On restart, a fraction of ack'd ops
 *     SHOULD be missing.
 *   - That outcome validates the harness: it proves the kill mechanism, the
 *     restart logic, and the shadow-model invariant can all see lost-data
 *     failures when they exist. Without this control, "0 failures" might just
 *     mean the test is incapable of detecting a real bug.
 *
 * Build:  gcc -shared -fPIC -O2 -o fsync_drop.so fsync_drop.c -ldl
 * Use:    LD_PRELOAD=/abs/path/to/fsync_drop.so <binary>
 */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>

/* Intentionally do nothing. We return 0 (success) so the application never
 * notices anything wrong and never falls back to a stricter sync path.
 *
 * Counters are exposed via SIGUSR1 — kill -USR1 <pid> dumps the running totals
 * to stderr. Useful for confirming the shim is actually being called.
 */

static atomic_long fsync_calls;
static atomic_long fdatasync_calls;
static atomic_long msync_calls;
static atomic_long sync_calls;
static atomic_long syncfs_calls;

__attribute__((constructor))
static void fsync_drop_init(void) {
    fprintf(stderr, "[fsync_drop] LD_PRELOAD shim loaded, all sync calls are no-ops\n");
}

__attribute__((destructor))
static void fsync_drop_fini(void) {
    fprintf(stderr,
        "[fsync_drop] totals: fsync=%ld fdatasync=%ld msync=%ld sync=%ld syncfs=%ld\n",
        atomic_load(&fsync_calls),
        atomic_load(&fdatasync_calls),
        atomic_load(&msync_calls),
        atomic_load(&sync_calls),
        atomic_load(&syncfs_calls));
}

int fsync(int fd) {
    (void)fd;
    atomic_fetch_add(&fsync_calls, 1);
    return 0;
}

int fdatasync(int fd) {
    (void)fd;
    atomic_fetch_add(&fdatasync_calls, 1);
    return 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    atomic_fetch_add(&msync_calls, 1);
    return 0;
}

void sync(void) {
    atomic_fetch_add(&sync_calls, 1);
}

int syncfs(int fd) {
    (void)fd;
    atomic_fetch_add(&syncfs_calls, 1);
    return 0;
}

/* Some glibc / libc paths route through __fdatasync; cover that too. */
int __fdatasync(int fd) {
    (void)fd;
    atomic_fetch_add(&fdatasync_calls, 1);
    return 0;
}
