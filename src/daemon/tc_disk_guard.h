#ifndef TC_DISK_GUARD_H
#define TC_DISK_GUARD_H

#include "tc_common.h"

typedef struct {
    uint64_t device;
    uint64_t available_bytes;
} TcDiskSpace;

/* Query the filesystem of an open output directory; errors fail closed. */
bool tc_disk_guard_query_fd(int directory_fd, TcDiskSpace *space, char **error_msg);
bool tc_disk_guard_charge(uint64_t estimated_bytes, uint64_t *charged_bytes);

/* Check estimate + 20% + emergency buffer, with overflow-safe arithmetic. */
bool tc_disk_guard_check_space(const char *output_dir, uint64_t estimated_bytes_needed, char **error_msg);

/* Conservative startup hook: unowned legacy temporaries are never deleted. */
void tc_disk_guard_cleanup_stale_temps(void);

#endif /* TC_DISK_GUARD_H */
