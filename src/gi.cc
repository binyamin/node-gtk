
#include <node.h>

#include <girepository.h>

using namespace v8;

static void DefineEnumeration(Handle<Object> module_obj, GIBaseInfo *info) {
    Handle<Object> enum_obj = Object::New ();

    int n = g_enum_info_get_n_values(info);
    for (int i = 0; i < n; i++) {
        GIValueInfo *value_info = g_enum_info_get_value (info, i);

        const char *value_name = g_base_info_get_name ((GIBaseInfo *) value_info);
        gint64 value_val = g_value_info_get_value (value_info);

        char *upcase_name = g_ascii_strup (value_name, -1);
        enum_obj->Set (String::NewSymbol (upcase_name), Number::New (value_val));
        g_free (upcase_name);
    }

    const char *enum_name = g_base_info_get_name (info);
    module_obj->Set (String::NewSymbol (enum_name), enum_obj);
}

static void DefineInfo(Handle<Object> module_obj, GIBaseInfo *info) {
    GIInfoType type = g_base_info_get_type (info);

    switch (type) {
    case GI_INFO_TYPE_ENUM:
    case GI_INFO_TYPE_FLAGS:
        DefineEnumeration (module_obj, info);
        break;
    default:
        break;
    }
}

static Handle<Value> ImportRepo(const Arguments& args) {
    GIRepository *repo = g_irepository_get_default ();
    HandleScope scope;

    if (args.Length() < 1) {
        ThrowException (Exception::TypeError (String::New ("Wrong number of arguments")));
        return scope.Close (Undefined ());
    }

    String::Utf8Value ns_str (args[0]->ToString());
    const char *ns = *ns_str;

    const char *version = NULL;
    if (args.Length() > 1) {
        String::Utf8Value version_str (args[1]->ToString());
        version = *version_str;
    }

    GError *error = NULL;
    g_irepository_require (repo, ns, version, (GIRepositoryLoadFlags) 0, &error);
    if (error) {
        ThrowException (Exception::TypeError (String::New (error->message)));
        return scope.Close (Undefined ());
    }

    Handle<Object> module_obj = Object::New ();

    int n = g_irepository_get_n_infos (repo, ns);
    for (int i = 0; i < n; i++) {
        GIBaseInfo *info = g_irepository_get_info (repo, ns, i);
        DefineInfo (module_obj, info);
        g_base_info_unref (info);
    }

    return scope.Close (module_obj);
}

void InitModule(Handle<Object> exports) {
    exports->Set(String::NewSymbol("importRepo"), FunctionTemplate::New(ImportRepo)->GetFunction());
}

NODE_MODULE(gi, InitModule)
