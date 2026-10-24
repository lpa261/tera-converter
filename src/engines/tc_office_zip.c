#include "tc_engine.h"
#include "tc_process_runner.h"
#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

/* Bound work on hostile ZIPs without ever extracting archive paths to disk. */
#define TC_OFFICE_MAX_ENTRIES 10000U
#define TC_OFFICE_MAX_BYTES (1024ULL * 1024ULL * 1024ULL)

static TcStatusCode office_error(char **error_out, TcStatusCode code, const char *message) {
    if (error_out && !*error_out) *error_out = g_strdup(message ? message : "Invalid Office package");
    return code;
}

static bool office_path_valid(const char *name) {
    if (!name || !*name || strlen(name)>4096 || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    g_auto(GStrv) parts = g_strsplit(name, "/", -1);
    for (guint i = 0; parts[i]; i++) {
        if (!strcmp(parts[i], "..") || !strcmp(parts[i], ".")) return false;
        if (!*parts[i] && parts[i + 1]) return false;
    }
    return true;
}

static bool odf_mime(const char *value) {
    return !strcmp(value,"application/vnd.oasis.opendocument.text") ||
        !strcmp(value,"application/vnd.oasis.opendocument.spreadsheet") ||
        !strcmp(value,"application/vnd.oasis.opendocument.presentation");
}
static void odf_manifest_element(GMarkupParseContext *ctx,const gchar *name,const gchar **keys,const gchar **values,gpointer data,GError **error){
    (void)ctx;(void)data;
    if(strstr(name,"encryption")||strstr(name,"signature"))g_set_error_literal(error,G_MARKUP_ERROR,G_MARKUP_ERROR_INVALID_CONTENT,"Signed or encrypted ODF is unsupported");
    for(unsigned i=0;keys && keys[i] && !*error;i++)
        if(strstr(values[i],"digital-signature")||strstr(values[i],"encryption"))g_set_error_literal(error,G_MARKUP_ERROR,G_MARKUP_ERROR_INVALID_CONTENT,"Signed or encrypted ODF is unsupported");
}
static bool odf_manifest_valid(const char *text,gsize length){
    GMarkupParser parser={.start_element=odf_manifest_element};
    GMarkupParseContext *context=g_markup_parse_context_new(&parser,0,NULL,NULL);
    GError *error=NULL;
    bool ok=g_markup_parse_context_parse(context,text,length,&error)&&g_markup_parse_context_end_parse(context,&error);
    g_clear_error(&error);g_markup_parse_context_free(context);return ok;
}
static bool odf_local_header(const char *path){
    unsigned char h[38];int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    ssize_t n=read(fd,h,sizeof(h));close(fd);
    return n==sizeof(h)&&!memcmp(h,"PK\003\004",4)&&h[8]==0&&h[9]==0&&
        h[26]==8&&h[27]==0&&h[28]==0&&h[29]==0&&!memcmp(h+30,"mimetype",8);
}

/* Return a manifest of names -> type + SHA256. Check CRCs by consuming every entry.
 * If writer is provided, stream identical entry bytes into a new ZIP as we hash.
 * The caller compares this pass with its preflight manifest before commit. */
static TcStatusCode office_scan(const char *path, struct archive *writer,
                                GCancellable *cancel, GHashTable **manifest,
                                char **error_out) {
    struct archive *reader = archive_read_new();
    archive_read_support_format_zip(reader);
    GHashTable *entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    TcStatusCode result = TC_STATUS_ERROR_CORRUPT;
    struct archive_entry *entry = NULL;
    bool odf=false;
    guint count = 0;
    g_autoptr(GString) odf_manifest=g_string_new(NULL);
    uint64_t total = 0;
    size_t name_bytes=0;
    int next = ARCHIVE_FATAL;
    if (archive_read_open_filename(reader, path, 65536) != ARCHIVE_OK) goto archive_failure;
    while ((next = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        g_autoptr(GString) mime=NULL;
        if (g_cancellable_is_cancelled(cancel)) {
            result = office_error(error_out, TC_STATUS_ERROR_CANCELLED, "Office compression cancelled");
            goto cleanup;
        }
        const char *name = archive_entry_pathname(entry);
        mode_t type = archive_entry_filetype(entry);
        if (++count > TC_OFFICE_MAX_ENTRIES || !office_path_valid(name) ||
            g_hash_table_contains(entries, name) ||
            (type != AE_IFREG && type != AE_IFDIR) || archive_entry_symlink(entry) ||
            archive_entry_hardlink(entry)) {
            office_error(error_out, result, "Unsafe, duplicate or excessive Office ZIP entries");
            goto cleanup;
        }
        if (archive_entry_is_encrypted(entry) ||
            g_str_has_prefix(name, "_xmlsignatures/") ||
            (g_str_has_prefix(name,"META-INF/") && strstr(name,"signatures"))) {
            result = office_error(error_out, TC_STATUS_ERROR_UNSUPPORTED,
                                 "Signed or encrypted Office packages are not supported by this optimizer");
            goto cleanup;
        }
        name_bytes+=strlen(name);
        if(name_bytes>16U*1024U*1024U){result=office_error(error_out,TC_STATUS_ERROR_MEM_LIMIT,"Office entry names exceed metadata budget");goto cleanup;}
        la_int64_t declared = archive_entry_size(entry);
        if (declared < 0 || (uint64_t)declared > TC_OFFICE_MAX_BYTES - total) {
            result = office_error(error_out, TC_STATUS_ERROR_MEM_LIMIT, "Office package exceeds processing limit");
            goto cleanup;
        }
        if(count==1 && !strcmp(name,"mimetype"))odf=true;
        if(!strcmp(name,"mimetype") && (!odf || declared>128)){
            result=office_error(error_out,TC_STATUS_ERROR_UNSUPPORTED,"ODF mimetype must be first and bounded");goto cleanup;
        }
        if(writer){
            int compression=odf && !strcmp(name,"mimetype") ? archive_write_zip_set_compression_store(writer):archive_write_zip_set_compression_deflate(writer);
            if(compression!=ARCHIVE_OK)goto writer_failure;
            struct archive_entry *copy=archive_entry_clone(entry);
            if(odf&&!strcmp(name,"mimetype")){
                archive_entry_unset_mtime(copy);archive_entry_unset_atime(copy);archive_entry_unset_ctime(copy);
                archive_entry_set_uid(copy,0);archive_entry_set_gid(copy,0);archive_entry_set_uname(copy,NULL);archive_entry_set_gname(copy,NULL);
            }
            int wrote=archive_write_header(writer,copy);archive_entry_free(copy);
            if(wrote!=ARCHIVE_OK)goto writer_failure;
        }
        mime=g_string_new(NULL);
        GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
        unsigned char buffer[65536];
        la_ssize_t bytes;
        uint64_t entry_bytes = 0;
        while ((bytes = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
            if (g_cancellable_is_cancelled(cancel)) {
                g_checksum_free(hash);
                result = office_error(error_out, TC_STATUS_ERROR_CANCELLED, "Office compression cancelled");
                goto cleanup;
            }
            if ((uint64_t)bytes > TC_OFFICE_MAX_BYTES - total) {
                g_checksum_free(hash);
                result = office_error(error_out, TC_STATUS_ERROR_MEM_LIMIT, "Office package exceeds processing limit");
                goto cleanup;
            }
            total += bytes;
            entry_bytes += bytes;
            g_checksum_update(hash, buffer, bytes);
            if(odf&&!strcmp(name,"mimetype"))g_string_append_len(mime,(const char*)buffer,bytes);
            if(odf&&!strcmp(name,"META-INF/manifest.xml")){
                if(odf_manifest->len+(gsize)bytes>1024*1024){g_checksum_free(hash);result=office_error(error_out,TC_STATUS_ERROR_UNSUPPORTED,"ODF manifest exceeds parser budget");goto cleanup;}
                g_string_append_len(odf_manifest,(const char*)buffer,bytes);
            }
            if (writer) {
                size_t offset = 0;
                while (offset < (size_t)bytes) {
                    la_ssize_t written = archive_write_data(writer, buffer + offset, (size_t)bytes - offset);
                    if (written <= 0) {
                        g_checksum_free(hash);
                        goto writer_failure;
                    }
                    offset += written;
                }
            }
        }
        if (bytes < 0 || entry_bytes != (uint64_t)declared) {
            g_checksum_free(hash);
            goto archive_failure;
        }
        if(odf&&!strcmp(name,"mimetype")&&!odf_mime(mime->str)){
            g_checksum_free(hash);result=office_error(error_out,TC_STATUS_ERROR_UNSUPPORTED,"Unsupported ODF media type");goto cleanup;
        }
        char *digest = g_strdup_printf("%c:%s", type == AE_IFDIR ? 'd' : 'f', g_checksum_get_string(hash));
        g_checksum_free(hash);
        g_hash_table_insert(entries, g_strdup(name), digest);
        if (writer && archive_write_finish_entry(writer) != ARCHIVE_OK) goto writer_failure;
    }
    if (next != ARCHIVE_EOF || archive_read_has_encrypted_entries(reader) > 0) goto archive_failure;
    if(odf){
        if(!odf_manifest->len||!odf_manifest_valid(odf_manifest->str,odf_manifest->len)||!g_hash_table_contains(entries,"content.xml")){
            result=office_error(error_out,TC_STATUS_ERROR_UNSUPPORTED,"ODF manifest is invalid, signed or encrypted");goto cleanup;
        }
    }else if (!g_hash_table_contains(entries, "[Content_Types].xml") ||
        !(g_hash_table_contains(entries, "word/document.xml") ||
          g_hash_table_contains(entries, "xl/workbook.xml") ||
          g_hash_table_contains(entries, "ppt/presentation.xml"))) {
        result = office_error(error_out, TC_STATUS_ERROR_UNSUPPORTED, "Not a supported OOXML Office package");
        goto cleanup;
    }
    *manifest = entries;
    entries = NULL;
    result = TC_STATUS_OK;
    goto cleanup;
writer_failure:
    result = office_error(error_out, TC_STATUS_ERROR, archive_error_string(writer));
    goto cleanup;
archive_failure:
    office_error(error_out, result, archive_error_string(reader));
cleanup:
    archive_read_free(reader);
    if (entries) g_hash_table_unref(entries);
    return result;
}

static bool manifests_equal(GHashTable *a, GHashTable *b) {
    if (g_hash_table_size(a) != g_hash_table_size(b)) return false;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, a);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_strcmp0(value, g_hash_table_lookup(b, key))) return false;
    }
    return true;
}

static bool office_supports_format(const char *format) {
    return g_ascii_strcasecmp(format ? format : "", "comp-office") == 0;
}

static TcStatusCode odf_repack(const char *src,const char *dst,GCancellable *cancel,char **error){
    const char *script=
        "import os,sys,zipfile,shutil,copy,stat\n"
        "src,dst=sys.argv[1:]\n"
        "with open(src,'rb') as source:\n"
        " fd=os.open(dst,os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)\n"
        " a,b=os.fstat(source.fileno()),os.fstat(fd)\n"
        " if (a.st_dev,a.st_ino)==(b.st_dev,b.st_ino) or not stat.S_ISREG(b.st_mode): os.close(fd);sys.exit(2)\n"
        " os.ftruncate(fd,0)\n"
        " with os.fdopen(fd,'w+b') as target,zipfile.ZipFile(source) as zin,zipfile.ZipFile(target,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as zout:\n"
        "  entries=zin.infolist();total=0;seen=set()\n"
        "  if len(entries)>10000 or not entries or entries[0].filename!='mimetype':sys.exit(3)\n"
        "  for info in entries:\n"
        "   total+=info.file_size\n"
        "   if total>1073741824 or info.filename in seen:sys.exit(4)\n"
        "   seen.add(info.filename);item=copy.copy(info)\n"
        "   item.compress_type=zipfile.ZIP_STORED if item.filename=='mimetype' else zipfile.ZIP_DEFLATED\n"
        "   if item.filename=='mimetype':item.extra=b''\n"
        "   with zin.open(info) as reader,zout.open(item,'w') as writer:shutil.copyfileobj(reader,writer,65536)\n";
    const char *argv[]={"python3","-c",script,src,dst,NULL};
    const TcProcessOptions options={.timeout_ms=30*60*1000,.terminate_grace_ms=500,.max_address_bytes=2ULL*1024*1024*1024,.max_file_bytes=TC_OFFICE_MAX_BYTES};
    return tc_process_run(argv,&options,cancel,NULL,NULL,NULL,error);
}

static TcStatusCode office_convert(const char *src, const char *dst, const char *format,
                                   uint16_t width, uint16_t height, GCancellable *cancel,
                                   char **error_out, TcProgressCallback progress, void *user_data) {
    (void)format; (void)width; (void)height; (void)progress; (void)user_data;
    GHashTable *before = NULL, *during = NULL, *after = NULL;
    TcStatusCode result = office_scan(src, NULL, cancel, &before, error_out);
    if (result != TC_STATUS_OK) return result;
    struct archive *writer = archive_write_new();
    int fd = -1;
    struct stat source_stat, target_stat;
    if (stat(src, &source_stat) != 0 ||
        (fd = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)) < 0 ||
        fstat(fd, &target_stat) != 0 || !S_ISREG(target_stat.st_mode) ||
        (source_stat.st_dev == target_stat.st_dev && source_stat.st_ino == target_stat.st_ino) ||
        ftruncate(fd, 0) != 0) {
        result = office_error(error_out, TC_STATUS_ERROR, "Cannot safely open Office output file");
        goto done;
    }
    if(g_hash_table_contains(before,"mimetype")){
        result=odf_repack(src,dst,cancel,error_out);
        goto validate_output;
    }
    if (archive_write_set_format_zip(writer) != ARCHIVE_OK ||
        archive_write_set_options(writer, "zip:compression=deflate,zip:compression-level=9") != ARCHIVE_OK ||
        archive_write_open_fd(writer, fd) != ARCHIVE_OK) {
        result = office_error(error_out, TC_STATUS_ERROR, archive_error_string(writer));
        goto done;
    }
    result = office_scan(src, writer, cancel, &during, error_out);
    if (archive_write_close(writer) != ARCHIVE_OK && result == TC_STATUS_OK)
        result = office_error(error_out, TC_STATUS_ERROR, archive_error_string(writer));
    if (result == TC_STATUS_OK && !manifests_equal(before, during))
        result = office_error(error_out, TC_STATUS_ERROR_CORRUPT, "Office source changed during compression");
validate_output:
    if (result == TC_STATUS_OK) result = office_scan(dst, NULL, cancel, &after, error_out);
    if(result==TC_STATUS_OK && g_hash_table_contains(before,"mimetype") && !odf_local_header(dst))
        result=office_error(error_out,TC_STATUS_ERROR_UNSUPPORTED,"Writer cannot preserve the ODF stored-mimetype header");
    if (result == TC_STATUS_OK && !manifests_equal(before, after))
        result = office_error(error_out, TC_STATUS_ERROR_CORRUPT, "Office output failed entry integrity validation");
done:
    archive_write_free(writer);
    if (fd >= 0) close(fd);
    g_hash_table_unref(before);
    if (during) g_hash_table_unref(during);
    if (after) g_hash_table_unref(after);
    return result;
}

TcEngine tc_engine_office = {
    .name = "office_zip", .category = TC_CATEGORY_DOCUMENT,
    .supports_format = office_supports_format, .convert = office_convert
};
#endif
