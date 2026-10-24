#include "tc_media_probe.h"
#include "tc_engine.h"
#include "tc_process_runner.h"
#include "tc_runtime_limits.h"

#include <math.h>
#include <glib/gstdio.h>

extern TcEngine tc_engine_ffmpeg;
static char *fixture_dir, *rich_path, *rotated_path, *base_path, *native_mp4, *native_mov;
static gboolean capture_pipeline;
static char **captured_argv;

TcStatusCode __real_tc_process_run(const char *const *, const TcProcessOptions *, GCancellable *,
    TcProcessOutputFunc, gpointer, TcProcessResult *, char **);
TcStatusCode __wrap_tc_process_run(const char *const *argv, const TcProcessOptions *options,
    GCancellable *cancellable, TcProcessOutputFunc callback, gpointer data,
    TcProcessResult *result, char **error) {
    if (!capture_pipeline) return __real_tc_process_run(argv, options, cancellable, callback, data, result, error);
    capture_pipeline = FALSE;
    captured_argv = g_strdupv((char **)argv);
    if (result) *result = (TcProcessResult){ .exit_code = 42 };
    if (error) *error = g_strdup("Captured command for resource-policy test");
    return TC_STATUS_ERROR;
}

static char *path_for(const char *name) {
    return g_build_filename(fixture_dir, name, NULL);
}

static void run_fixture(const char *const *argv) {
    const TcProcessOptions options = { .timeout_ms = 20000, .terminate_grace_ms = 100 };
    g_autofree char *error = NULL;
    TcStatusCode status = tc_process_run(argv, &options, NULL, NULL, NULL, NULL, &error);
    if (status != TC_STATUS_OK) g_test_message("Fixture generation failed: %s", error);
    g_assert_cmpint(status, ==, TC_STATUS_OK);
}

static void create_fixtures(void) {
    fixture_dir = g_dir_make_tmp("tc-media-probe-XXXXXX", NULL);
    g_assert_nonnull(fixture_dir);
    rich_path = path_for("misleading-extension.dat");
    base_path = path_for("base.mp4");
    rotated_path = path_for("rotated.mp4");
    native_mp4 = path_for("native.mp4");
    native_mov = path_for("native.mov");
    g_autofree char *subtitles = path_for("subtitles.srt");
    g_autofree char *chapters = path_for("chapters.txt");
    g_autofree char *attachment = path_for("attachment.bin");
    g_assert_true(g_file_set_contents(subtitles, "1\n00:00:00,000 --> 00:00:00,700\nXin chao\n", -1, NULL));
    g_assert_true(g_file_set_contents(chapters, ";FFMETADATA1\ntitle=Probe fixture\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=500\ntitle=Intro\n", -1, NULL));
    const char binary[] = {0, 1, 2, 'A', 0, 'Z'};
    g_assert_true(g_file_set_contents(attachment, binary, sizeof(binary), NULL));
    const char *rich_argv[] = {
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y",
        "-f", "lavfi", "-i", "testsrc2=s=96x64:r=12:d=1",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=44100:duration=1",
        "-f", "srt", "-i", subtitles, "-f", "ffmetadata", "-i", chapters,
        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
        "-map_metadata", "4", "-map_chapters", "4",
        "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=vie",
        "-disposition:a:0", "default", "-disposition:a:1", "0", "-disposition:s:0", "forced",
        "-c:v", "libx264", "-preset", "ultrafast", "-threads:v", "1", "-pix_fmt", "yuv420p10le",
        "-vf", "format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=limited",
        "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
        "-c:a", "flac", "-c:s", "srt", "-attach", attachment,
        "-metadata:s:t", "mimetype=application/octet-stream", "-f", "matroska", rich_path, NULL
    };
    run_fixture(rich_argv);
    const char *base_argv[] = {
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        "testsrc2=s=96x64:r=12:d=1", "-c:v", "libx264", "-threads:v", "1", base_path, NULL
    };
    run_fixture(base_argv);
    const char *rotate_argv[] = {
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-display_rotation:v:0", "90", "-i", base_path,
        "-map", "0", "-c", "copy", rotated_path, NULL
    };
    run_fixture(rotate_argv);
    const char *native_argv[] = {
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-display_rotation:v:0", "90", "-i", rich_path,
        "-map", "0:v", "-map", "0:a", "-map", "0:s", "-map_metadata", "0", "-map_chapters", "0",
        "-c:v", "copy", "-c:a", "aac", "-c:s", "mov_text", native_mp4, NULL
    };
    run_fixture(native_argv);
    const char *mov_argv[] = {
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-i", native_mp4,
        "-map", "0:v", "-map", "0:a", "-map", "0:s", "-map_chapters", "0",
        "-c", "copy", "-f", "mov", native_mov, NULL
    };
    run_fixture(mov_argv);
}

static void assert_tag(AVDictionary *dict, const char *key, const char *expected) {
    AVDictionaryEntry *entry = av_dict_get(dict, key, NULL, 0);
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->value, ==, expected);
}

static void test_all_tracks(void) {
    g_autoptr(TcMediaInfo) info = NULL;
    g_autofree char *error = NULL;
    g_assert_cmpint(tc_media_probe_file(rich_path, NULL, NULL, &info, &error), ==, TC_STATUS_OK);
    g_assert_null(error);
    g_assert_nonnull(strstr(info->format_name, "matroska"));
    g_assert_cmpuint(info->streams->len, ==, 5);
    g_assert_cmpuint(info->chapters->len, ==, 1);
    assert_tag(info->metadata, "title", "Probe fixture");
    g_assert_cmpint(info->duration_us, >, 0);
    guint video_count = 0, audio_count = 0, subtitle_count = 0, attachment_count = 0;
    for (guint i = 0; i < info->streams->len; i++) {
        const TcMediaStream *stream = g_ptr_array_index(info->streams, i);
        const AVCodecParameters *codec = stream->codec;
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_count++;
            g_assert_true(stream->parameters_known);
            g_assert_cmpint(codec->width, ==, 96);
            g_assert_cmpint(codec->height, ==, 64);
            g_assert_cmpuint(stream->bit_depth, ==, 10);
            g_assert_cmpint(codec->color_primaries, ==, AVCOL_PRI_BT2020);
            g_assert_cmpint(codec->color_trc, ==, AVCOL_TRC_SMPTE2084);
            g_assert_cmpint(codec->color_space, ==, AVCOL_SPC_BT2020_NCL);
            g_assert_cmpint(stream->time_base.den, >, 0);
            /* avg/r frame rates are observations, not a claim that this is CFR. */
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            g_assert_true(stream->parameters_known);
            assert_tag(stream->metadata, "language", audio_count == 0 ? "eng" : "vie");
            g_assert_cmpint(codec->sample_rate, ==, audio_count == 0 ? 48000 : 44100);
            if (audio_count == 0) g_assert_true(stream->disposition & AV_DISPOSITION_DEFAULT);
            audio_count++;
        } else if (codec->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            subtitle_count++;
            g_assert_true(stream->disposition & AV_DISPOSITION_FORCED);
            g_assert_cmpint(codec->codec_id, ==, AV_CODEC_ID_SUBRIP);
        } else if (codec->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
            attachment_count++;
            const guint8 expected[] = {0, 1, 2, 'A', 0, 'Z'};
            g_assert_cmpint(codec->extradata_size, ==, sizeof(expected));
            g_assert_cmpmem(codec->extradata, codec->extradata_size, expected, sizeof(expected));
            assert_tag(stream->metadata, "mimetype", "application/octet-stream");
        }
    }
    g_assert_cmpuint(video_count, ==, 1);
    g_assert_cmpuint(audio_count, ==, 2);
    g_assert_cmpuint(subtitle_count, ==, 1);
    g_assert_cmpuint(attachment_count, ==, 1);
    const TcMediaChapter *chapter = g_ptr_array_index(info->chapters, 0);
    assert_tag(chapter->metadata, "title", "Intro");
    g_assert_cmpint(av_rescale_q(chapter->end, chapter->time_base, AV_TIME_BASE_Q), ==, 500000);
}

static void test_rotation_and_owned_snapshot(void) {
    g_autoptr(TcMediaInfo) info = NULL;
    g_assert_cmpint(tc_media_probe_file(rotated_path, NULL, NULL, &info, NULL), ==, TC_STATUS_OK);
    const TcMediaStream *stream = g_ptr_array_index(info->streams, 0);
    g_assert_true(stream->has_display_matrix);
    g_assert_cmpfloat_with_epsilon(fabs(stream->rotation_degrees), 90.0, 0.01);
    g_assert_cmpuint(stream->bit_depth, ==, 8);
    g_assert_cmpint(stream->codec->extradata_size, >, 0);
    g_assert_cmpuint(stream->side_data->len, >, 0);
    /* Read another input after the original libavformat context was closed. */
    g_autoptr(TcMediaInfo) another = NULL;
    g_assert_cmpint(tc_media_probe_file(base_path, NULL, NULL, &another, NULL), ==, TC_STATUS_OK);
    g_assert_cmpfloat_with_epsilon(fabs(stream->rotation_degrees), 90.0, 0.01);
    const TcMediaSideData *side = g_ptr_array_index(stream->side_data, 0);
    g_assert_cmpuint(g_bytes_get_size(side->bytes), >, 0);
}

static void test_limits_and_cancel(void) {
    TcMediaInfo *info = NULL;
    const TcMediaProbeLimits small = { .timeout_ms = 2000, .max_read_bytes = 64 };
    g_autofree char *error = NULL;
    g_assert_cmpint(tc_media_probe_file(rich_path, &small, NULL, &info, &error), ==, TC_STATUS_ERROR_UNSUPPORTED);
    g_assert_null(info);
    g_assert_nonnull(strstr(error, "read budget"));
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    g_assert_cmpint(tc_media_probe_file(rich_path, NULL, cancellable, &info, NULL), ==, TC_STATUS_ERROR_CANCELLED);
    g_assert_null(info);
    const TcMediaProbeLimits invalid = {0};
    g_assert_cmpint(tc_media_probe_file(rich_path, &invalid, NULL, &info, NULL), ==, TC_STATUS_ERROR);
}

static void test_reject_inputs(void) {
    g_autofree char *fifo = path_for("pipe");
    g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
    TcMediaInfo *info = NULL;
    gint64 start = g_get_monotonic_time();
    g_assert_cmpint(tc_media_probe_file(fifo, NULL, NULL, &info, NULL), ==, TC_STATUS_ERROR_UNSUPPORTED);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 1000000);
    g_assert_null(info);
    g_assert_cmpint(tc_media_probe_file(fixture_dir, NULL, NULL, &info, NULL), ==, TC_STATUS_ERROR_UNSUPPORTED);
    g_autofree char *playlist = path_for("references.txt");
    g_assert_true(g_file_set_contents(playlist, "ffconcat version 1.0\nfile 'https://invalid.example/media.mp4'\n", -1, NULL));
    g_assert_cmpint(tc_media_probe_file(playlist, NULL, NULL, &info, NULL), !=, TC_STATUS_OK);
    g_assert_null(info);
}

static void test_engine_probe_failure_preserves_output(void) {
    g_autofree char *corrupt = path_for("corrupt.mp4");
    g_autofree char *output = path_for("must-not-touch.mp4");
    g_assert_true(g_file_set_contents(corrupt, "invalid video", -1, NULL));
    g_assert_true(g_file_set_contents(output, "existing candidate", -1, NULL));
    g_autofree char *error = NULL;
    TcStatusCode status = tc_engine_ffmpeg.convert(corrupt, output, "comp-hq", 0, 0, NULL, &error, NULL, NULL);
    g_assert_cmpint(status, !=, TC_STATUS_OK);
    g_assert_nonnull(error);
    g_autofree char *content = NULL;
    g_assert_true(g_file_get_contents(output, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "existing candidate");
}

static guint open_fd_count(void) {
    g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
    g_assert_nonnull(dir);
    guint count = 0;
    while (g_dir_read_name(dir)) count++;
    return count;
}

static void test_fd_cleanup(void) {
    guint before = open_fd_count();
    for (int i = 0; i < 16; i++) {
        g_autoptr(TcMediaInfo) info = NULL;
        g_assert_cmpint(tc_media_probe_file(rotated_path, NULL, NULL, &info, NULL), ==, TC_STATUS_OK);
    }
    g_assert_cmpuint(open_fd_count(), ==, before);
}

static void test_padding_real_media(gconstpointer extension) {
    const char *native = !strcmp(extension, "mp4") ? native_mp4 : native_mov;
    g_autofree char *original = NULL;
    gsize native_size;
    g_assert_true(g_file_get_contents(native, &original, &native_size, NULL));
    g_autoptr(GByteArray) padded = g_byte_array_new();
    g_byte_array_append(padded, (guint8 *)original, native_size);
    const gsize padding_size = 256 * 1024;
    guint32 size_be = GUINT32_TO_BE(padding_size);
    g_byte_array_append(padded, (guint8 *)&size_be, 4);
    g_byte_array_append(padded, (guint8 *)"free", 4);
    g_byte_array_set_size(padded, native_size + padding_size);
    memset(padded->data + native_size + 8, 0xa5, padding_size - 8);
    g_autofree char *source = path_for(!strcmp(extension, "mp4") ? "padded.mp4" : "padded.mov");
    g_autofree char *candidate = path_for(!strcmp(extension, "mp4") ? "optimized.mp4" : "optimized.mov");
    g_assert_true(g_file_set_contents(source, (char *)padded->data, padded->len, NULL));
    g_autofree char *error = NULL;
    g_assert_cmpint(tc_engine_ffmpeg.convert(source, candidate, "comp-padding", 0, 0, NULL, &error, NULL, NULL), ==, TC_STATUS_OK);
    g_assert_null(error);
    g_autofree char *output = NULL, *source_after = NULL;
    gsize output_size, source_size;
    g_assert_true(g_file_get_contents(candidate, &output, &output_size, NULL));
    g_assert_true(g_file_get_contents(source, &source_after, &source_size, NULL));
    /* Exact original MP4/MOV bytes, preserving track tables and offsets too. */
    g_assert_cmpmem(output, output_size, original, native_size);
    g_assert_cmpmem(source_after, source_size, padded->data, padded->len);
    g_autoptr(TcMediaInfo) before = NULL, after = NULL;
    g_assert_cmpint(tc_media_probe_file(source, NULL, NULL, &before, NULL), ==, TC_STATUS_OK);
    g_assert_cmpint(tc_media_probe_file(candidate, NULL, NULL, &after, NULL), ==, TC_STATUS_OK);
    g_assert_cmpuint(after->streams->len, ==, before->streams->len);
    g_assert_cmpuint(after->streams->len, >=, 4);
    g_assert_cmpuint(after->chapters->len, ==, 1);
    assert_tag(after->metadata, "title", "Probe fixture");
    const TcMediaStream *video = g_ptr_array_index(after->streams, 0);
    g_assert_cmpuint(video->bit_depth, ==, 10);
    g_assert_true(video->has_display_matrix);
    g_assert_cmpfloat_with_epsilon(fabs(video->rotation_degrees), 90.0, 0.01);
    g_test_message("%s synthetic padded fixture: %zu -> %zu bytes; %u streams retained",
                   (const char *)extension, source_size, output_size, after->streams->len);
}

static void test_padding_unsupported_never_encodes(void) {
    g_autofree char *dest = path_for("unsupported-dest.mkv");
    g_assert_true(g_file_set_contents(dest, "keep existing output", -1, NULL));
    g_assert_cmpint(tc_engine_ffmpeg.convert(rich_path, dest, "comp-padding", 0, 0, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR_UNSUPPORTED);
    g_autofree char *output = NULL;
    g_assert_true(g_file_get_contents(dest, &output, NULL, NULL));
    g_assert_cmpstr(output, ==, "keep existing output");
}

static void test_thread_policy(gconstpointer format) {
    g_autofree char *candidate = path_for("captured-output");
    g_autofree char *error = NULL;
    capture_pipeline = TRUE;
    g_assert_cmpint(tc_engine_ffmpeg.convert(base_path, candidate, format, 0, 0, NULL, &error, NULL, NULL), ==, TC_STATUS_ERROR);
    g_assert_false(capture_pipeline);
    g_assert_nonnull(captured_argv);
    g_autofree char *limit = g_strdup_printf("%u", tc_engine_thread_limit());
    gboolean input = TRUE, decoder = FALSE, encoder = FALSE, filter = FALSE, complex = FALSE, x265 = FALSE;
    for (guint i = 0; captured_argv[i]; i++) {
        const char *argument = captured_argv[i];
        if (!strcmp(argument, "-i")) input = FALSE;
        if (!strcmp(argument, "-threads")) {
            g_assert_cmpstr(captured_argv[i + 1], ==, limit);
            if (input) decoder = TRUE; else encoder = TRUE;
        }
        if (!strcmp(argument, "-filter_threads") || !strcmp(argument, "-filter_complex_threads")) {
            g_assert_cmpstr(captured_argv[i + 1], ==, limit);
            if (!strcmp(argument, "-filter_threads")) filter = TRUE; else complex = TRUE;
        }
        if (!strcmp(argument, "-x265-params")) {
            g_assert_cmpstr(captured_argv[i + 1], ==, tc_engine_thread_limit() > 1 ?
                "pools=1:frame-threads=1" : "pools=none:frame-threads=1");
            x265 = TRUE;
        }
    }
    g_assert_true(decoder && encoder && filter && complex);
    g_assert_cmpint(x265, ==, !strcmp(format, "comp-saver"));
    g_assert_false(g_file_test(candidate, G_FILE_TEST_EXISTS));
    g_clear_pointer(&captured_argv, g_strfreev);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!g_file_test("/usr/bin/ffmpeg", G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_message("Required fixture binary /usr/bin/ffmpeg is unavailable");
        return 77;
    }
    create_fixtures();
    g_test_add_func("/probe/all-tracks-chapters-10bit-colour", test_all_tracks);
    g_test_add_func("/probe/rotation-owned-snapshot", test_rotation_and_owned_snapshot);
    g_test_add_func("/probe/read-budget-and-cancel", test_limits_and_cancel);
    g_test_add_func("/probe/local-file-only", test_reject_inputs);
    g_test_add_func("/probe/engine-failure-preserves-output", test_engine_probe_failure_preserves_output);
    g_test_add_func("/probe/fd-cleanup", test_fd_cleanup);
    g_test_add_data_func("/probe/padding-real-mp4", "mp4", test_padding_real_media);
    g_test_add_data_func("/probe/padding-real-mov", "mov", test_padding_real_media);
    g_test_add_func("/probe/padding-no-lossy-fallback", test_padding_unsupported_never_encodes);
    g_test_add_data_func("/probe/vp9-thread-policy", "webm", test_thread_policy);
    g_test_add_data_func("/probe/x265-thread-policy", "comp-saver", test_thread_policy);
    int status = g_test_run();
    g_autoptr(GDir) dir = g_dir_open(fixture_dir, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        g_autofree char *path = path_for(name);
        g_assert_cmpint(g_unlink(path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(fixture_dir), ==, 0);
    g_free(rich_path); g_free(rotated_path); g_free(base_path); g_free(fixture_dir);
    g_free(native_mp4); g_free(native_mov);
    return status;
}
