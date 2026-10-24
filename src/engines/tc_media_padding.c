#include "tc_media_padding.h"

#define TC_PADDING_CHUNK (128U * 1024U)
#define TC_PADDING_MAX_ATOMS 100000U

static gboolean is_cancelled(GCancellable *cancellable) {
    return cancellable && g_cancellable_is_cancelled(cancellable);
}

static gboolean read_exact(int fd, void *buffer, gsize size, off_t offset,
                            GCancellable *cancellable) {
    gsize done = 0;
    while (done < size) {
        if (is_cancelled(cancellable)) { errno = ECANCELED; return FALSE; }
        ssize_t count = pread(fd, (char *)buffer + done, size - done, offset + (off_t)done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; return FALSE; }
        done += (gsize)count;
    }
    return TRUE;
}

static gboolean write_exact(int fd, const void *buffer, gsize size, GCancellable *cancellable) {
    gsize done = 0;
    while (done < size) {
        if (is_cancelled(cancellable)) { errno = ECANCELED; return FALSE; }
        ssize_t count = write(fd, (const char *)buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; return FALSE; }
        done += (gsize)count;
    }
    return TRUE;
}

static guint32 be32(const guint8 *bytes) {
    return ((guint32)bytes[0] << 24) | ((guint32)bytes[1] << 16) |
           ((guint32)bytes[2] << 8) | bytes[3];
}

static gboolean same_source(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec && before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec && before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static gboolean supported_brand(const guint8 *brand) {
    return !memcmp(brand, "isom", 4) ||
           (!memcmp(brand, "iso", 3) && brand[3] >= '2' && brand[3] <= '9') ||
           !memcmp(brand, "mp41", 4) || !memcmp(brand, "mp42", 4) ||
           !memcmp(brand, "avc1", 4) || !memcmp(brand, "M4V ", 4) || !memcmp(brand, "qt  ", 4);
}

static TcStatusCode failure(char **error_out, TcStatusCode status, const char *message) {
    if (error_out) *error_out = g_strdup(message);
    return status;
}

static TcStatusCode io_failure(char **error_out) {
    int saved = errno;
    if (error_out) *error_out = g_strdup_printf("Padding optimization I/O failed: %s", g_strerror(saved));
    if (saved == ECANCELED) return TC_STATUS_ERROR_CANCELLED;
    if (saved == ENOSPC || saved == EDQUOT) return TC_STATUS_ERROR_NO_SPACE;
    return TC_STATUS_ERROR;
}

static TcStatusCode scan_padding(int fd, guint64 size, GCancellable *cancellable,
                                  guint64 *retained, char **error_out) {
    guint64 position = 0;
    guint ftyp_count = 0, moov_count = 0, mdat_count = 0, atom_count = 0;
    *retained = 0;
    while (position < size) {
        if (++atom_count > TC_PADDING_MAX_ATOMS)
            return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Too many MP4/MOV atoms for padding optimization");
        guint8 header[16];
        if (size - position < 8)
            return failure(error_out, TC_STATUS_ERROR_CORRUPT, "Incomplete MP4/MOV atom header");
        if (!read_exact(fd, header, 8, (off_t)position, cancellable)) return io_failure(error_out);
        if (position == 0 && memcmp(header + 4, "ftyp", 4))
            return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "File is not a supported MP4/MOV layout");
        guint64 atom_size = be32(header);
        guint header_size = 8;
        if (atom_size == 1) {
            header_size = 16;
            if (size - position < 16)
                return failure(error_out, TC_STATUS_ERROR_CORRUPT, "Incomplete extended atom header");
            if (!read_exact(fd, header + 8, 8, (off_t)position + 8, cancellable)) return io_failure(error_out);
            atom_size = ((guint64)be32(header + 8) << 32) | be32(header + 12);
        } else if (atom_size == 0) {
            /* A size-zero mdat includes everything to EOF, even bytes that
             * look like padding headers. Those bytes must never be removed. */
            atom_size = size - position;
        }
        if (atom_size < header_size || atom_size > size - position)
            return failure(error_out, TC_STATUS_ERROR_CORRUPT, "Invalid MP4/MOV atom size");

        const guint8 *type = header + 4;
        gboolean padding = !memcmp(type, "free", 4) || !memcmp(type, "skip", 4);
        if (!memcmp(type, "ftyp", 4)) {
            if (position != 0 || ++ftyp_count != 1 || atom_size - header_size < 8 ||
                (atom_size - header_size) % 4 != 0)
                return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Unsupported MP4/MOV file-type layout");
            guint8 brand[4];
            if (!read_exact(fd, brand, sizeof(brand), (off_t)position + header_size, cancellable)) return io_failure(error_out);
            if (!supported_brand(brand))
                return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Unsupported MP4/MOV major brand");
        } else if (!memcmp(type, "moov", 4)) {
            if (++moov_count != 1)
                return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Multiple movie atoms are unsupported");
        } else if (!memcmp(type, "mdat", 4)) {
            if (atom_size == header_size)
                return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Empty media atom is unsupported");
            mdat_count++;
        } else if (!padding && (memcmp(type, "wide", 4) || atom_size != 8)) {
            /* In particular: UUID, fragmented/indexed layouts and opaque
             * top-level structures are not rewritten by this initial mode. */
            return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Unsupported top-level MP4/MOV atom");
        }
        position += atom_size;  /* Bounded by size before addition. */
        if (!padding) *retained = position;
    }
    if (ftyp_count != 1 || moov_count != 1 || mdat_count == 0)
        return failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "A self-contained MP4/MOV layout is required");
    if (*retained == size)
        return failure(error_out, TC_STATUS_NO_BENEFIT, "No removable trailing MP4/MOV padding");
    return TC_STATUS_OK;
}

TcStatusCode tc_media_optimize_padding(const char *source, const char *candidate,
                                       GCancellable *cancellable, char **error_out,
                                       TcProgressCallback progress, void *user_data) {
    if (!source || !candidate) return failure(error_out, TC_STATUS_ERROR, "Missing media path");
    if (is_cancelled(cancellable)) return failure(error_out, TC_STATUS_ERROR_CANCELLED, "Padding optimization cancelled");
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (input < 0) return io_failure(error_out);
    int output = -1;
    TcStatusCode status = TC_STATUS_ERROR;
    struct stat original, after, named, dest;
    if (fstat(input, &original) != 0) { status = io_failure(error_out); goto done; }
    if (!S_ISREG(original.st_mode) || original.st_size <= 0) {
        status = failure(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Padding optimization requires a regular nonempty file");
        goto done;
    }
    guint64 retained;
    status = scan_padding(input, (guint64)original.st_size, cancellable, &retained, error_out);
    if (status != TC_STATUS_OK) goto done;
    if (is_cancelled(cancellable)) {
        status = failure(error_out, TC_STATUS_ERROR_CANCELLED, "Padding optimization cancelled");
        goto done;
    }
    output = open(candidate, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (output < 0 || fstat(output, &dest) != 0) { status = io_failure(error_out); goto done; }
    if (!S_ISREG(dest.st_mode) || dest.st_nlink != 1 ||
        (original.st_dev == dest.st_dev && original.st_ino == dest.st_ino)) {
        status = failure(error_out, TC_STATUS_ERROR, "Candidate must be a separate regular file with one link");
        goto done;
    }
    if (ftruncate(output, 0) != 0) { status = io_failure(error_out); goto done; }

    guint8 *source_buffer = g_malloc(TC_PADDING_CHUNK);
    guint8 *candidate_buffer = g_malloc(TC_PADDING_CHUNK);
    for (guint64 offset = 0; offset < retained;) {
        gsize size = (gsize)MIN((guint64)TC_PADDING_CHUNK, retained - offset);
        if (!read_exact(input, source_buffer, size, (off_t)offset, cancellable) ||
            !write_exact(output, source_buffer, size, cancellable)) {
            status = io_failure(error_out);
            break;
        }
        offset += size;
        if (progress) progress(0.8 * (double)offset / retained, 0, user_data);
    }
    /* Compare every retained byte against the pinned source descriptor. This
     * preserves even metadata/track types the media probe cannot interpret. */
    for (guint64 offset = 0; status == TC_STATUS_OK && offset < retained;) {
        gsize size = (gsize)MIN((guint64)TC_PADDING_CHUNK, retained - offset);
        if (!read_exact(input, source_buffer, size, (off_t)offset, cancellable) ||
            !read_exact(output, candidate_buffer, size, (off_t)offset, cancellable)) {
            status = io_failure(error_out);
            break;
        }
        if (memcmp(source_buffer, candidate_buffer, size)) {
            status = failure(error_out, TC_STATUS_ERROR_CORRUPT, "Candidate bytes differ from the source");
            break;
        }
        offset += size;
        if (progress) progress(0.8 + 0.19 * (double)offset / retained, 0, user_data);
    }
    g_free(source_buffer);
    g_free(candidate_buffer);
    if (status != TC_STATUS_OK) goto done;
    if (is_cancelled(cancellable)) {
        status = failure(error_out, TC_STATUS_ERROR_CANCELLED, "Padding optimization cancelled");
    } else if (fstat(input, &after) != 0 || stat(source, &named) != 0 || fstat(output, &dest) != 0 ||
               !same_source(&original, &after) || !same_source(&original, &named) ||
               dest.st_size != (off_t)retained) {
        status = failure(error_out, TC_STATUS_ERROR, "Source or candidate changed during padding optimization");
    } else {
        int synced;
        do { synced = fsync(output); } while (synced < 0 && errno == EINTR && !is_cancelled(cancellable));
        if (synced != 0) status = io_failure(error_out);
    }

done:
    if (output >= 0 && close(output) != 0 && status == TC_STATUS_OK) status = io_failure(error_out);
    close(input);
    if (status == TC_STATUS_OK && is_cancelled(cancellable))
        status = failure(error_out, TC_STATUS_ERROR_CANCELLED, "Padding optimization cancelled");
    if (status == TC_STATUS_OK && progress) progress(1.0f, 0, user_data);
    return status;
}
