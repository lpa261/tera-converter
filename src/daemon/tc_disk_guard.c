#include "tc_disk_guard.h"
#include "tc_logging.h"

bool tc_disk_guard_charge(uint64_t estimated_bytes, uint64_t *charged_bytes) {
    if (!charged_bytes) return false;
    uint64_t margin = estimated_bytes / 5 + (estimated_bytes % 5 != 0);
    if (estimated_bytes > UINT64_MAX - margin) return false;
    *charged_bytes = estimated_bytes + margin;
    return true;
}

bool tc_disk_guard_query_fd(int directory_fd, TcDiskSpace *space, char **error_msg) {
    struct stat st;
    struct statvfs vfs;
    if (!space || fstat(directory_fd, &st) != 0 || fstatvfs(directory_fd, &vfs) != 0) {
        if (error_msg) *error_msg = g_strdup_printf("Cannot query output filesystem: %s", g_strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode) || vfs.f_frsize == 0 ||
        vfs.f_bavail > vfs.f_bfree || vfs.f_bfree > vfs.f_blocks) {
        if (error_msg) *error_msg = g_strdup("Output is not a directory or filesystem space is unknown");
        return false;
    }
    uint64_t blocks = vfs.f_bavail, block_size = vfs.f_frsize;
    space->device = (uint64_t)st.st_dev;
    space->available_bytes = blocks > UINT64_MAX / block_size ? UINT64_MAX : blocks * block_size;
    return true;
}

bool tc_disk_guard_check_space(const char *output_dir, uint64_t estimated_bytes_needed, char **error_msg) {
    if (!output_dir) return false;
    int fd = open(output_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error_msg) *error_msg = g_strdup_printf("Cannot open output directory: %s", g_strerror(errno));
        return false;
    }
    TcDiskSpace space;
    bool queried = tc_disk_guard_query_fd(fd, &space, error_msg);
    close(fd);
    if (!queried) return false;
    uint64_t charged;
    if (!tc_disk_guard_charge(estimated_bytes_needed, &charged) ||
        charged > UINT64_MAX - TC_MIN_FREE_DISK_BYTES ||
        space.available_bytes < charged + TC_MIN_FREE_DISK_BYTES) {
        if (error_msg) *error_msg = g_strdup_printf("Insufficient disk space for output estimate and emergency buffer (%" G_GUINT64_FORMAT " bytes available)", space.available_bytes);
        return false;
    }
    return true;
}

void tc_disk_guard_cleanup_stale_temps(void) {
    /* Legacy prefix/age sweeping could delete another process's live files.
     * Each job cleans its own sibling temporary on success/error/cancel. A
     * crash-recovery sweep must wait for an ownership + liveness manifest;
     * until then retain unknown leftovers instead of deleting user data. */
    tc_log_debug("Skipping legacy /tmp sweep: no ownership/liveness proof");
}
