#include "tc_media_padding.h"
#include <glib/gstdio.h>

static void append_atom(GByteArray *bytes, const char *type, const char *payload,
                         gsize payload_size, gboolean extended, gboolean to_eof) {
    guint32 short_size = GUINT32_TO_BE(to_eof ? 0 : extended ? 1 : (guint32)payload_size + 8);
    g_byte_array_append(bytes, (guint8 *)&short_size, 4);
    g_byte_array_append(bytes, (const guint8 *)type, 4);
    if (extended) {
        guint64 long_size = GUINT64_TO_BE(payload_size + 16);
        g_byte_array_append(bytes, (guint8 *)&long_size, 8);
    }
    if (payload_size) g_byte_array_append(bytes, (const guint8 *)payload, payload_size);
}

/* Atom-level fixtures exercise the byte-preservation contract. A separate
 * integration test uses real multi-track encoded video and the full worker. */
static GByteArray *make_prefix(void) {
    GByteArray *bytes = g_byte_array_new();
    const char brands[] = "isom\0\0\0\0isommp42";
    append_atom(bytes, "ftyp", brands, sizeof(brands) - 1, FALSE, FALSE);
    append_atom(bytes, "free", "middle padding must stay", 24, FALSE, FALSE);
    append_atom(bytes, "mdat", "encoded sample bytes", 20, FALSE, FALSE);
    append_atom(bytes, "moov", "opaque metadata bytes", 21, FALSE, FALSE);
    return bytes;
}

static void read_contents(const char *path, char **bytes, gsize *length) {
    g_assert_true(g_file_get_contents(path, bytes, length, NULL));
}

static void test_padding_suffix(gconstpointer mode) {
    g_autofree char *dir = g_dir_make_tmp("tc-padding-test-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source.mp4", NULL);
    g_autofree char *dest = g_build_filename(dir, "candidate.mp4", NULL);
    g_autoptr(GByteArray) bytes = make_prefix();
    gsize retained = bytes->len;
    append_atom(bytes, "free", "unused", 6, FALSE, FALSE);
    gboolean extended = !strcmp(mode, "extended");
    append_atom(bytes, "skip", "unused too", 10, extended, !strcmp(mode, "zero-size"));
    g_assert_true(g_file_set_contents(source, (char *)bytes->data, bytes->len, NULL));
    /* Worker destinations are already-created empty files. */
    g_assert_true(g_file_set_contents(dest, "", 0, NULL));
    g_autofree char *error = NULL;
    g_assert_cmpint(tc_media_optimize_padding(source, dest, NULL, &error, NULL, NULL), ==, TC_STATUS_OK);
    g_assert_null(error);
    g_autofree char *output = NULL, *original = NULL;
    gsize output_size, original_size;
    read_contents(dest, &output, &output_size);
    read_contents(source, &original, &original_size);
    g_assert_cmpmem(output, output_size, bytes->data, retained);
    g_assert_cmpmem(original, original_size, bytes->data, bytes->len);
    g_assert_cmpuint(output_size, <, original_size);
    g_unlink(source); g_unlink(dest); g_rmdir(dir);
}

static void test_rejected_layout(gconstpointer mode) {
    g_autofree char *dir = g_dir_make_tmp("tc-padding-reject-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source.mp4", NULL);
    g_autofree char *dest = g_build_filename(dir, "candidate.mp4", NULL);
    g_autoptr(GByteArray) bytes = make_prefix();
    TcStatusCode expected = TC_STATUS_ERROR_UNSUPPORTED;
    if (!strcmp(mode, "no-benefit")) expected = TC_STATUS_NO_BENEFIT;
    else if (!strcmp(mode, "zero-size-mdat")) {
        append_atom(bytes, "mdat", "sample", 6, FALSE, TRUE);
        append_atom(bytes, "free", "this is inside mdat", 19, FALSE, FALSE);
        expected = TC_STATUS_NO_BENEFIT;
    } else if (!strcmp(mode, "fragmented")) append_atom(bytes, "moof", NULL, 0, FALSE, FALSE);
    else if (!strcmp(mode, "opaque-uuid")) append_atom(bytes, "uuid", "opaque", 6, FALSE, FALSE);
    else if (!strcmp(mode, "unsupported-brand")) memcpy(bytes->data + 8, "heic", 4);
    else if (!strcmp(mode, "wrong-file-type")) memcpy(bytes->data + 4, "EBML", 4);
    else if (!strcmp(mode, "truncated")) {
        g_byte_array_append(bytes, (const guint8 *)"1234", 4);
        expected = TC_STATUS_ERROR_CORRUPT;
    } else if (!strcmp(mode, "overflow")) {
        const guint8 bad[] = {0,0,0,1,'f','r','e','e',255,255,255,255,255,255,255,255};
        g_byte_array_append(bytes, bad, sizeof(bad));
        expected = TC_STATUS_ERROR_CORRUPT;
    } else if (!strcmp(mode, "undersized")) {
        const guint8 bad[] = {0,0,0,4,'f','r','e','e'};
        g_byte_array_append(bytes, bad, sizeof(bad));
        expected = TC_STATUS_ERROR_CORRUPT;
    }
    if (expected == TC_STATUS_ERROR_UNSUPPORTED) append_atom(bytes, "free", "padding", 7, FALSE, FALSE);
    g_assert_true(g_file_set_contents(source, (char *)bytes->data, bytes->len, NULL));
    g_assert_true(g_file_set_contents(dest, "do not change", -1, NULL));
    g_autofree char *error = NULL;
    g_assert_cmpint(tc_media_optimize_padding(source, dest, NULL, &error, NULL, NULL), ==, expected);
    g_assert_nonnull(error);
    g_autofree char *output = NULL;
    gsize output_size;
    read_contents(dest, &output, &output_size);
    g_assert_cmpstr(output, ==, "do not change");
    g_autofree char *original = NULL;
    gsize original_size;
    read_contents(source, &original, &original_size);
    g_assert_cmpmem(original, original_size, bytes->data, bytes->len);
    g_unlink(source); g_unlink(dest); g_rmdir(dir);
}

static void test_source_alias(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-padding-alias-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source.mp4", NULL);
    g_autofree char *alias = g_build_filename(dir, "alias.mp4", NULL);
    g_autoptr(GByteArray) bytes = make_prefix();
    append_atom(bytes, "free", "padding", 7, FALSE, FALSE);
    g_assert_true(g_file_set_contents(source, (char *)bytes->data, bytes->len, NULL));
    g_assert_cmpint(tc_media_optimize_padding(source, source, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR);
    g_assert_cmpint(link(source, alias), ==, 0);
    g_assert_cmpint(tc_media_optimize_padding(source, alias, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR);
    g_unlink(alias);
    g_assert_cmpint(symlink(source, alias), ==, 0);
    g_assert_cmpint(tc_media_optimize_padding(source, alias, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR);
    g_autofree char *original = NULL;
    gsize size;
    read_contents(source, &original, &size);
    g_assert_cmpmem(original, size, bytes->data, bytes->len);
    g_unlink(alias); g_unlink(source); g_rmdir(dir);
}

typedef struct {
    const char *source, *dest, *mode;
    GCancellable *cancellable;
    gboolean triggered;
} Mutation;

static void mutate_after_copy(float fraction, uint32_t eta, void *opaque) {
    (void)eta;
    Mutation *mutation = opaque;
    if (mutation->triggered || fraction < 0.79) return;
    mutation->triggered = TRUE;
    if (!strcmp(mutation->mode, "cancel")) {
        g_cancellable_cancel(mutation->cancellable);
    } else {
        gboolean source = !strcmp(mutation->mode, "source-changed");
        int fd = open(source ? mutation->source : mutation->dest, O_WRONLY | O_CLOEXEC);
        g_assert_cmpint(fd, >=, 0);
        /* Mutate discarded padding in the source: byte comparison of the
         * retained prefix alone cannot detect this change. */
        off_t offset = source ? lseek(fd, -1, SEEK_END) : 0;
        g_assert_cmpint(pwrite(fd, "!", 1, offset), ==, 1);
        close(fd);
    }
}

static void test_reject_mutation(gconstpointer mode) {
    g_autofree char *dir = g_dir_make_tmp("tc-padding-mutation-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source.mp4", NULL);
    g_autofree char *dest = g_build_filename(dir, "candidate.mp4", NULL);
    g_autoptr(GByteArray) bytes = make_prefix();
    append_atom(bytes, "free", "padding", 7, FALSE, FALSE);
    g_assert_true(g_file_set_contents(source, (char *)bytes->data, bytes->len, NULL));
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    Mutation mutation = { source, dest, mode, cancellable, FALSE };
    g_autofree char *error = NULL;
    TcStatusCode result = tc_media_optimize_padding(source, dest, cancellable, &error, mutate_after_copy, &mutation);
    g_assert_true(mutation.triggered);
    g_assert_nonnull(error);
    if (!strcmp(mode, "cancel")) g_assert_cmpint(result, ==, TC_STATUS_ERROR_CANCELLED);
    else if (!strcmp(mode, "candidate-changed")) g_assert_cmpint(result, ==, TC_STATUS_ERROR_CORRUPT);
    else g_assert_cmpint(result, ==, TC_STATUS_ERROR);
    g_unlink(source); g_unlink(dest); g_rmdir(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/padding/suffix-normal", "normal", test_padding_suffix);
    g_test_add_data_func("/padding/suffix-extended", "extended", test_padding_suffix);
    g_test_add_data_func("/padding/suffix-to-eof", "zero-size", test_padding_suffix);
    const char *rejections[] = { "no-benefit", "zero-size-mdat", "fragmented", "opaque-uuid",
                                "unsupported-brand", "wrong-file-type", "truncated", "overflow", "undersized" };
    for (guint i = 0; i < G_N_ELEMENTS(rejections); i++) {
        g_autofree char *name = g_strdup_printf("/padding/reject-%s", rejections[i]);
        g_test_add_data_func(name, rejections[i], test_rejected_layout);
    }
    g_test_add_func("/padding/source-alias", test_source_alias);
    g_test_add_data_func("/padding/cancel-before-validation", "cancel", test_reject_mutation);
    g_test_add_data_func("/padding/candidate-corruption", "candidate-changed", test_reject_mutation);
    g_test_add_data_func("/padding/source-changed", "source-changed", test_reject_mutation);
    return g_test_run();
}
