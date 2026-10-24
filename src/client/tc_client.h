#ifndef TC_CLIENT_H
#define TC_CLIENT_H

#include "tc_common.h"
#include "tc_ipc.h"

/* Dispatch conversion request to teraconvert-daemon.
 * Returns true after a versioned daemon acknowledgement, false on failure.
 */
bool tc_client_dispatch_request(
    TcActionType action,
    const char *target_format,
    GList *file_uris,
    TcOverwritePolicy overwrite_policy
);

bool tc_client_dispatch_resize_request(
    uint16_t width,
    uint16_t height,
    GList *file_uris,
    TcOverwritePolicy overwrite_policy
);

/* Trigger transient UI helper process (e.g. Resize dialog or Setup AI) */
bool tc_client_launch_ui(const char *mode, GList *file_uris);

#endif /* TC_CLIENT_H */
