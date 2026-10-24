#include "tc_engine.h"
#include "tc_process_runner.h"
static bool supports(const char *format){return format&&!strcmp(format,"comp-pdf-safe");}
static TcStatusCode convert(const char *src,const char *dst,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,char **error,TcProgressCallback progress,void *data){
    (void)format;(void)w;(void)h;(void)progress;(void)data;
    const char *directory=g_getenv("TC_HELPER_DIR");
    g_autofree char *helper=g_build_filename(directory?directory:TC_HELPER_DIR,"teraconvert-pdf-safe",NULL);
    if(!g_file_test(helper,G_FILE_TEST_IS_EXECUTABLE)){if(error)*error=g_strdup("QPDF validation helper is not installed");return TC_STATUS_ERROR_UNSUPPORTED;}
    const char *argv[]={"python3",helper,src,dst,NULL};
    TcProcessOptions options={.timeout_ms=30*60*1000,.terminate_grace_ms=500,
        .max_address_bytes=1024ULL*1024*1024,.max_file_bytes=1024ULL*1024*1024};
    TcProcessResult result={0};
    TcStatusCode status=tc_process_run(argv,&options,cancel,NULL,NULL,&result,error);
    if(status!=TC_STATUS_OK && result.exited && result.exit_code==42)status=TC_STATUS_ERROR_UNSUPPORTED;
    tc_process_result_clear(&result);return status;
}
TcEngine tc_engine_pdf_preserve={.name="pdf_preserve",.category=TC_CATEGORY_DOCUMENT,.supports_format=supports,.convert=convert};
