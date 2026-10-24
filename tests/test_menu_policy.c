#include "tc_menu_policy.h"
#define ALL 16383U
static void test_safe_modes(void){
    TcMenuFile file={"mp3","audio/mpeg",TRUE};TcMenuSelection s={&file,1,ALL};
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"safe-flac"));
    file=(TcMenuFile){"wav","audio/x-wav",TRUE};g_assert_true(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"safe-flac"));
    file=(TcMenuFile){"webp","image/webp",TRUE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-image"));
    file=(TcMenuFile){"png","image/png",TRUE};g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-image"));
    file=(TcMenuFile){"mkv","video/x-matroska",TRUE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-remux"));
    g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-web"));
    file=(TcMenuFile){"mov","video/quicktime",TRUE};g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-remux"));
}
static void test_batch_intersection(void){
    TcMenuFile files[]={{"jpg","image/jpeg",TRUE},{"png","image/png",TRUE}};TcMenuSelection s={files,2,ALL};
    g_assert_true(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"png"));
    g_assert_true(tc_menu_action_supported(&s,TC_ACTION_MERGE,"pdf"));
    files[0]=files[1];g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"png"));
    files[1]=(TcMenuFile){"txt","text/plain",TRUE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"jpg"));
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_MERGE,"pdf"));
    files[1]=(TcMenuFile){"webp","image/webp",TRUE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-image"));
    files[1]=(TcMenuFile){"png","image/png",FALSE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"jpg"));
}
static void test_missing_capabilities(void){
    TcMenuFile file={"png","image/png",TRUE};TcMenuSelection s={&file,1,0};
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"jpg"));g_assert_false(tc_menu_action_supported(&s,TC_ACTION_RESIZE,"resize"));g_assert_false(tc_menu_preview_supported(&s));
    s.capabilities=TC_MENU_VIPS;g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"jpg"));
    s.capabilities=TC_MENU_IMAGES|TC_MENU_DAEMON;g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-image"));
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_REMOVE_BG,"rmbg"));
    file=(TcMenuFile){"pdf","application/pdf",TRUE};s.capabilities=TC_MENU_PDF|TC_MENU_DAEMON;
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-pdf-safe"));g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"png"));
    s.capabilities|=TC_MENU_PDF_RASTER;g_assert_true(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"png"));
}
static void test_documents_and_preview(void){
    TcMenuFile f={"docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document",TRUE};TcMenuSelection s={&f,1,ALL};
    g_assert_false(tc_menu_action_supported(&s,TC_ACTION_CONVERT,"pdf"));g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-office"));g_assert_false(tc_menu_preview_supported(&s));
    f=(TcMenuFile){"doc","application/msword",TRUE};g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-office"));
    f=(TcMenuFile){"odt",NULL,TRUE};s.capabilities=TC_MENU_OFFICE|TC_MENU_DAEMON;g_assert_false(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-office"));
    s.capabilities|=TC_MENU_PYTHON;g_assert_true(tc_menu_action_supported(&s,TC_ACTION_COMPRESS,"comp-office"));
    f=(TcMenuFile){"txt","text/plain",TRUE};s.capabilities=TC_MENU_PREVIEW;g_assert_true(tc_menu_preview_supported(&s));
    f.local=FALSE;g_assert_false(tc_menu_preview_supported(&s));
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/menu/safe-format-scope",test_safe_modes);g_test_add_func("/menu/batch-intersection",test_batch_intersection);g_test_add_func("/menu/missing-capabilities",test_missing_capabilities);g_test_add_func("/menu/documents-and-preview",test_documents_and_preview);return g_test_run();}
