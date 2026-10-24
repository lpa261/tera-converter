/* Exercise the actual menu builder with cached file metadata, without a desktop. */
#include "../src/client/tc_nautilus_ext.c"
#include <glib/gstdio.h>
typedef struct {GObject parent;char *uri,*mime;gboolean directory;} Fixture;
typedef struct {GObjectClass parent;} FixtureClass;
static void fixture_iface(NautilusFileInfoInterface *iface);
G_DEFINE_TYPE_WITH_CODE(Fixture,fixture,G_TYPE_OBJECT,G_IMPLEMENT_INTERFACE(NAUTILUS_TYPE_FILE_INFO,fixture_iface))
static char *fixture_uri(NautilusFileInfo *file){return g_strdup(((Fixture*)file)->uri);}
static char *fixture_mime(NautilusFileInfo *file){return g_strdup(((Fixture*)file)->mime);}
static gboolean fixture_dir(NautilusFileInfo *file){return ((Fixture*)file)->directory;}
static void fixture_iface(NautilusFileInfoInterface *iface){iface->get_uri=fixture_uri;iface->get_mime_type=fixture_mime;iface->is_directory=fixture_dir;}
static void fixture_finalize(GObject *object){Fixture *f=(Fixture*)object;g_free(f->uri);g_free(f->mime);G_OBJECT_CLASS(fixture_parent_class)->finalize(object);}
static void fixture_class_init(FixtureClass *klass){G_OBJECT_CLASS(klass)->finalize=fixture_finalize;}
static void fixture_init(Fixture *file){(void)file;}
static Fixture *new_file(const char *uri,const char *mime){Fixture *f=g_object_new(fixture_get_type(),NULL);f->uri=g_strdup(uri);f->mime=g_strdup(mime);return f;}
static void test_empty_and_batch(void){
    g_autofree char *old_path=g_strdup(g_getenv("PATH"));g_autofree char *tools=g_dir_make_tmp("tc-menu-tools-XXXXXX",NULL);
    g_autofree char *daemon=g_build_filename(tools,"teraconvert-daemon",NULL);
    g_assert_true(g_file_set_contents(daemon,"#!/bin/sh\nexit 0\n",-1,NULL));g_assert_cmpint(g_chmod(daemon,0700),==,0);
    g_setenv("PATH",tools,TRUE);
    Fixture *unknown=new_file("file:///tmp/unknown.bin","application/octet-stream");GList *files=g_list_append(NULL,unknown);
    GList *items=tc_nautilus_ext_get_file_items(NULL,files);g_assert_null(items);
    Fixture *image=new_file("file:///tmp/image.png","image/png");files=g_list_append(files,image);
    items=tc_nautilus_ext_get_file_items(NULL,files);g_assert_null(items);g_list_free(files);g_object_unref(unknown);
    files=g_list_append(NULL,image);items=tc_nautilus_ext_get_file_items(NULL,files);
    for(GList *it=items;it;it=it->next){
        g_assert_true(NAUTILUS_IS_MENU_ITEM(it->data));NautilusMenu *menu=NULL;g_object_get(it->data,"menu",&menu,NULL);
        if(menu){GList *children=nautilus_menu_get_items(menu);g_assert_nonnull(children);for(GList *c=children;c;c=c->next)g_assert_true(NAUTILUS_IS_MENU_ITEM(c->data));nautilus_menu_item_list_free(children);g_object_unref(menu);}
    }
    nautilus_menu_item_list_free(items);g_list_free(files);g_object_unref(image);
    Fixture *doc=new_file("file:///tmp/document.docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document");files=g_list_append(NULL,doc);
    items=tc_nautilus_ext_get_file_items(NULL,files);
    for(GList *it=items;it;it=it->next){g_autofree char *name=NULL;g_object_get(it->data,"name",&name,NULL);g_assert_cmpstr(name,!=,"TeraConvert::MenuRoot");}
    nautilus_menu_item_list_free(items);g_list_free(files);g_object_unref(doc);
    g_unlink(daemon);g_rmdir(tools);
    if(old_path)g_setenv("PATH",old_path,TRUE);else g_unsetenv("PATH");
    g_list_free_full(selected_rembg_uris,g_free);selected_rembg_uris=NULL;g_clear_pointer(&selected_resize_uri,g_free);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/menu/native-empty-submenus-and-selection",test_empty_and_batch);return g_test_run();}
