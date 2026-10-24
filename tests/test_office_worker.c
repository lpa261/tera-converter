#include "tc_worker_pool.h"
#include <glib/gstdio.h>

static void python(const char *script, const char *a, const char *b) {
    GError *error = NULL;
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                        "python3", "-c", script, a, b, NULL);
    g_assert_no_error(error);
    g_assert_nonnull(child);
    g_assert_true(g_subprocess_wait_check(child, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(child);
}

static char *checksum(const char *path) {
    char *data = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(path, &data, &length, NULL));
    char *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)data, length);
    g_free(data);
    return sum;
}

static void test_office_worker(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-office-worker-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree char *source = g_build_filename(dir, "report's draft.docx", NULL);
    g_autofree char *output = g_build_filename(dir, "report's draft_compressed.docx", NULL);
    python(
        "import sys,zipfile\n"
        "from xml.sax.saxutils import escape\n"
        "with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_STORED) as z:\n"
        " z.writestr('[Content_Types].xml','<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>')\n"
        " z.writestr('_rels/.rels','<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>')\n"
        " z.writestr('word/document.xml','<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>'+('<w:p><w:r><w:t>Keep this text unchanged.</w:t></w:r></w:p>'*4096)+'</w:body></w:document>')\n"
        " z.writestr('word/media/payload.bin',bytes(range(256))*16)\n",
        source, "");
    g_autofree char *before = checksum(source);
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    TcJobItem *job = g_new0(TcJobItem, 1);
    job->src_path = g_strdup(source);
    job->target_format = g_strdup("comp-office");
    job->action = TC_ACTION_COMPRESS;
    job->overwrite_policy = TC_OVERWRITE_AUTO_NUMBER;
    job->batch = batch;
    tc_worker_pool_init();
    tc_worker_pool_submit_job(job);
    tc_worker_pool_shutdown();
    if (batch->error_reports->len)
        g_test_message("worker result: %s", (char *)g_ptr_array_index(batch->error_reports, 0));
    g_assert_cmpuint(batch->completed_jobs, ==, 1);
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    g_assert_true(g_file_test(output, G_FILE_TEST_IS_REGULAR));
    g_autofree char *after = checksum(source);
    g_assert_cmpstr(before, ==, after);
    python(
        "import sys,zipfile,os,hashlib\n"
        "with zipfile.ZipFile(sys.argv[1]) as a,zipfile.ZipFile(sys.argv[2]) as b:\n"
        " assert sorted(a.namelist())==sorted(b.namelist())\n"
        " assert a.testzip() is None and b.testzip() is None\n"
        " for n in a.namelist(): assert hashlib.sha256(a.read(n)).digest()==hashlib.sha256(b.read(n)).digest(),n\n"
        "assert os.path.getsize(sys.argv[2])<os.path.getsize(sys.argv[1])\n",
        source, output);
    tc_batch_context_free(batch);
    g_assert_cmpint(g_unlink(source), ==, 0);
    g_assert_cmpint(g_unlink(output), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_rejected_package(gconstpointer data) {
    const char *kind = data;
    g_autofree char *dir = g_dir_make_tmp("tc-office-reject-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source.docx", NULL);
    g_autofree char *output = g_build_filename(dir, "output.docx", NULL);
    python(
        "import zipfile,sys,stat\n"
        "kind=sys.argv[2]\n"
        "with zipfile.ZipFile(sys.argv[1],'w') as z:\n"
        " z.writestr('[Content_Types].xml','<Types/>')\n"
        " z.writestr('word/document.xml','<document/>')\n"
        " if kind=='traversal': z.writestr('../escape','payload')\n"
        " if kind=='duplicate': z.writestr('word/document.xml','duplicate')\n"
        " if kind=='signed': z.writestr('_xmlsignatures/sig1.xml','signature')\n"
        " if kind=='symlink':\n"
        "  e=zipfile.ZipInfo('word/link');e.create_system=3;e.external_attr=(stat.S_IFLNK|0o777)<<16;z.writestr(e,'/etc/passwd')\n",
        source, kind);
    g_assert_true(g_file_set_contents(output, "preserve-existing-output", -1, NULL));
    g_autofree char *source_before = checksum(source);
    g_autofree char *output_before = checksum(output);
    TcEngine *engine = tc_engines_find_for_task(TC_CATEGORY_DOCUMENT, "comp-office");
    g_assert_nonnull(engine);
    char *error = NULL;
    TcStatusCode result = engine->convert(source, output, "comp-office", 0, 0, NULL, &error, NULL, NULL);
    g_assert_cmpint(result, !=, TC_STATUS_OK);
    g_assert_nonnull(error);
    g_free(error);
    g_autofree char *source_after = checksum(source);
    g_autofree char *output_after = checksum(output);
    g_assert_cmpstr(source_before, ==, source_after);
    g_assert_cmpstr(output_before, ==, output_after);
    g_unlink(source);
    g_unlink(output);
    g_rmdir(dir);
}

static void test_odf_preservation(void){
    g_autofree char *dir=g_dir_make_tmp("tc-odf-XXXXXX",NULL),*src=g_build_filename(dir,"source.odt",NULL),*dst=g_build_filename(dir,"output.odt",NULL);
    python("import zipfile,sys\nwith zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_STORED) as z:\n z.writestr('mimetype','application/vnd.oasis.opendocument.text')\n z.writestr('META-INF/manifest.xml','<manifest xmlns=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\"/>')\n z.writestr('content.xml','<document>'+('unchanged text '*4096)+'</document>')\n",src,"");
    TcEngine *e=tc_engines_find_for_task(TC_CATEGORY_DOCUMENT,"comp-office");char *error=NULL;
    TcStatusCode status=e->convert(src,dst,"comp-office",0,0,NULL,&error,NULL,NULL);
    if(error)g_test_message("%s",error);
    g_free(error);g_assert_cmpint(status,==,TC_STATUS_OK);
    python("import zipfile,sys,struct\nwith zipfile.ZipFile(sys.argv[1]) as a,zipfile.ZipFile(sys.argv[2]) as b:\n assert b.namelist()[0]=='mimetype' and b.getinfo('mimetype').compress_type==0\n assert {n:a.read(n) for n in a.namelist()}=={n:b.read(n) for n in b.namelist()}\nwith open(sys.argv[2],'rb') as f:\n h=f.read(30);assert struct.unpack_from('<H',h,28)[0]==0\n",src,dst);
    g_unlink(src);g_unlink(dst);g_rmdir(dir);
}

static void test_odf_active_features(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-odf-negative-XXXXXX",NULL);
    g_autofree char *src=g_build_filename(dir,"source.odt",NULL),*dst=g_build_filename(dir,"existing.odt",NULL);
    TcEngine *engine=tc_engines_find_for_task(TC_CATEGORY_DOCUMENT,"comp-office");
    for(unsigned i=0;i<2;i++){
        python("import zipfile,sys\nwith zipfile.ZipFile(sys.argv[1],'w') as z:\n z.writestr('mimetype','application/vnd.oasis.opendocument.text')\n z.writestr('content.xml','<document/>')\n z.writestr('META-INF/manifest.xml','<manifest><encryption-data/></manifest>' if sys.argv[2]=='encrypted' else '<manifest/>')\n if sys.argv[2]=='signed':z.writestr('META-INF/documentsignatures.xml','<signature/>')\n",src,i?"signed":"encrypted");
        g_assert_true(g_file_set_contents(dst,"existing-output",-1,NULL));
        char *error=NULL;TcStatusCode status=engine->convert(src,dst,"comp-office",0,0,NULL,&error,NULL,NULL);
        g_assert_cmpint(status,!=,TC_STATUS_OK);g_free(error);
        char *content=NULL;g_assert_true(g_file_get_contents(dst,&content,NULL,NULL));
        g_assert_cmpstr(content,==,"existing-output");g_free(content);
    }
    g_unlink(src);g_unlink(dst);g_rmdir(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    tc_engines_init_registry();
    g_test_add_func("/office/odf-reject-encrypted-and-signed",test_odf_active_features);
    g_test_add_func("/office/worker-preserves-package", test_office_worker);
    g_test_add_data_func("/office/reject-traversal", "traversal", test_rejected_package);
    g_test_add_data_func("/office/reject-duplicate", "duplicate", test_rejected_package);
    g_test_add_data_func("/office/reject-signed", "signed", test_rejected_package);
    g_test_add_data_func("/office/reject-symlink", "symlink", test_rejected_package);
    g_test_add_func("/office/odf-mimetype-and-entry-preservation",test_odf_preservation);
    int result = g_test_run();
    tc_engines_cleanup_all();
    return result;
}
