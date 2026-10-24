#ifndef TC_MEDIA_PADDING_H
#define TC_MEDIA_PADDING_H

#include "tc_engine.h"

/* Remove only a suffix of top-level free/skip atoms from a conservative MP4/MOV
 * subset. All retained bytes and their offsets are verified unchanged. No
 * remux, decoding, or encoding is performed. The caller owns the candidate and
 * must discard it on any non-OK result; the source is always read-only. */
TcStatusCode tc_media_optimize_padding(const char *source, const char *candidate,
                                       GCancellable *cancellable, char **error_out,
                                       TcProgressCallback progress, void *user_data);

#endif
