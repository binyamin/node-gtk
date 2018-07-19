
#include <string.h>
#include <girffi.h>

#include "boxed.h"
#include "debug.h"
#include "function.h"
#include "gobject.h"
#include "type.h"
#include "value.h"

using namespace v8;
using Nan::New;
using Nan::WeakCallbackType;

namespace GNodeJS {

static void FillArgument(GIArgInfo *arg_info, GIArgument *argument, Local<Value> value) {
    GITypeInfo type_info;
    bool may_be_null = g_arg_info_may_be_null (arg_info);
    g_arg_info_load_type (arg_info, &type_info);
    V8ToGIArgument(&type_info, argument, value, may_be_null);
}

static int GetV8ArrayLength (Local<Value> value) {
    if (value->IsArray())
        return Local<Array>::Cast (value->ToObject ())->Length();
    else if (value->IsString())
        return Local<String>::Cast (value->ToObject ())->Length();
    else if (value->IsNull() || value->IsUndefined())
        return 0;

    printf("%s\n", *Nan::Utf8String(value->ToString()));
    g_assert_not_reached();
}

static void* AllocateArgument (GIBaseInfo *arg_info) {
    GITypeInfo arg_type;
    g_arg_info_load_type(arg_info, &arg_type);

    g_assert(g_type_info_get_tag(&arg_type) == GI_TYPE_TAG_INTERFACE);

    GIBaseInfo* base_info = g_type_info_get_interface (&arg_type);
    size_t size = Boxed::GetSize (base_info);
    void* pointer = g_slice_alloc0 (size);

    g_base_info_unref(base_info);
    return pointer;
}

static void ThrowNotEnoughArguments (int expected, int actual) {
    char *msg = g_strdup_printf(
        "Not enough arguments; expected %i, have %i",
        expected, actual);
    Nan::ThrowTypeError(msg);
    g_free(msg);
}

static void ThrowInvalidType (GIArgInfo *info, GITypeInfo *type_info, Local<Value> value) {
    char *expected = GetTypeName (type_info);
    char *msg = g_strdup_printf(
        "Expected argument of type %s for parameter %s, got '%s'",
        expected,
        g_base_info_get_name(info),
        *Nan::Utf8String(Nan::ToDetailString(value).ToLocalChecked()));
    Nan::ThrowTypeError(msg);
    g_free(expected);
    g_free(msg);
}

static bool IsMethod (GIBaseInfo *info) {
    auto flags = g_function_info_get_flags (info);
    return ((flags & GI_FUNCTION_IS_METHOD) != 0 &&
            (flags & GI_FUNCTION_IS_CONSTRUCTOR) == 0);
}

static bool ShouldSkipReturn(GIBaseInfo *info, GITypeInfo *return_type) {
    return g_type_info_get_tag(return_type) == GI_TYPE_TAG_VOID
        || g_callable_info_skip_return(info) == TRUE;
}

#define IS_OUT(direction) (direction == GI_DIRECTION_OUT || \
                           direction == GI_DIRECTION_INOUT)
#define IS_IN(direction) (direction == GI_DIRECTION_IN || \
                          direction == GI_DIRECTION_INOUT)
#define IS_INOUT(direction) (direction == GI_DIRECTION_INOUT)

void FunctionInvoker(const Nan::FunctionCallbackInfo<Value> &info) {

    FunctionInfo *func = (FunctionInfo *) External::Cast (*info.Data ())->Value ();
    GIBaseInfo *gi_info = func->info; // do-not-free

    // bool debug_mode = strcmp(g_base_info_get_name(gi_info), "file_get_contents") == 0;
    bool debug_mode = false;

    if (debug_mode)
        print_callable_info(gi_info);

    func->Init();

    if (!func->TypeCheck(info))
        return;

    /*
     * First, add arguments for the instance if it's a method,
     * and for error, if it can throw
     */

    GIArgument total_arg_values[func->n_total_args];
    GIArgument *callable_arg_values;
    GError *error = nullptr;

    if (func->is_method) {
        GIBaseInfo *container = g_base_info_get_container (gi_info);
        V8ToGIArgument(container, &total_arg_values[0], info.This());
        callable_arg_values = &total_arg_values[1];
    } else {
        callable_arg_values = &total_arg_values[0];
    }

    if (func->can_throw)
        callable_arg_values[func->n_callable_args].v_pointer = &error;


    /*
     * Second, allocate OUT-arguments and fill IN-arguments
     */

    for (int in_arg = 0, i = 0; i < func->n_callable_args; i++) {
        Parameter& param = func->call_parameters[i];

        if (param.type == ParameterType::SKIP)
            continue;

        GIArgInfo arg_info;
        GITypeInfo type_info;
        g_callable_info_load_arg (gi_info, i, &arg_info);
        GIDirection direction = g_arg_info_get_direction (&arg_info);

        if (param.type == ParameterType::ARRAY) {
            GIArgInfo  array_length_arg;
            GITypeInfo array_length_type;
            g_arg_info_load_type (&arg_info, &type_info);

            int length_i = g_type_info_get_array_length (&type_info);
            g_callable_info_load_arg(gi_info, length_i, &array_length_arg);
            g_arg_info_load_type (&array_length_arg, &array_length_type);

            Parameter& len_param = func->call_parameters[length_i];

            if (len_param.direction == GI_DIRECTION_IN) {
                param.length = GetV8ArrayLength(info[in_arg]);

                callable_arg_values[length_i].v_long = param.length;
            }
            else if (len_param.direction == GI_DIRECTION_INOUT) {
                len_param.data.v_long = GetV8ArrayLength(info[in_arg]);

                callable_arg_values[length_i].v_pointer = &len_param.data;
            }
            else if (direction == GI_DIRECTION_OUT) {
                len_param.data = {};

                callable_arg_values[length_i].v_pointer = &len_param.data;
            }
        }

        if (direction == GI_DIRECTION_OUT) {
            if (g_arg_info_is_caller_allocates (&arg_info)) {
                callable_arg_values[i].v_pointer = AllocateArgument(&arg_info);
            } else /* callee will allocate */ {
                param.data = {};
                callable_arg_values[i].v_pointer = &param.data;
            }
        }
        else /* (direction == GI_DIRECTION_IN || direction == GI_DIRECTION_INOUT) */ {

            // FIXME(handle failure here)
            FillArgument(&arg_info, &callable_arg_values[i], info[in_arg]);

            if (direction == GI_DIRECTION_INOUT) {
                param.data = {};
                param.data.v_pointer = callable_arg_values[i].v_pointer;
                callable_arg_values[i].v_pointer = &param.data;
            }

            in_arg++;
        }
    }


    /*
     * Third, make the actual ffi_call
     */

    void *ffi_args[func->n_total_args];
    for (int i = 0; i < func->n_total_args; i++)
        ffi_args[i] = &total_arg_values[i];


    GIArgument return_value;

    ffi_call (&func->invoker.cif, FFI_FN (func->invoker.native_address),
              &return_value, ffi_args);


    /*
     * Fourth, convert the return value & OUT-arguments back to JS
     */

    GITypeInfo return_type;
    g_callable_info_load_return_type(gi_info, &return_type);
    GITransfer return_transfer = g_callable_info_get_caller_owns(gi_info);

    // Return the value or throw the error, if any occured
    if (!error) {
        RETURN (func->GetReturnValue (&return_type, &return_value, callable_arg_values));
    } else {
        Nan::ThrowError(error->message);
        g_error_free(error);
    }

    /*
     * Fifth, free the return value and arguments
     */

    FreeGIArgument(&return_type, &return_value, return_transfer);

    for (int i = 0; i < func->n_callable_args; i++) {
        GIArgInfo  arg_info = {};
        GITypeInfo arg_type;
        GIArgument arg_value = callable_arg_values[i];
        Parameter &param = func->call_parameters[i];

        g_callable_info_load_arg ((GICallableInfo *) gi_info, i, &arg_info);
        g_arg_info_load_type (&arg_info, &arg_type);

        GIDirection direction = g_arg_info_get_direction (&arg_info);
        GITransfer transfer   = g_arg_info_get_ownership_transfer (&arg_info);

        if (param.type == ParameterType::ARRAY) {
            if (direction == GI_DIRECTION_INOUT || direction == GI_DIRECTION_OUT)
                FreeGIArgumentArray (&arg_type, (GIArgument*)arg_value.v_pointer, transfer, direction, param.length);
            else
                FreeGIArgumentArray (&arg_type, &arg_value, transfer, direction, param.length);
        }
        else {
            if (direction == GI_DIRECTION_INOUT || (direction == GI_DIRECTION_OUT && !g_arg_info_is_caller_allocates (&arg_info)))
                FreeGIArgument (&arg_type, (GIArgument*)arg_value.v_pointer, transfer, direction);
            else
                FreeGIArgument (&arg_type, &arg_value, transfer, direction);
        }
    }
}


/**
 * The constructor just stores the GIBaseInfo ref. The rest of the
 * initialization is done in FunctionInfo::Init, lazily.
 */
FunctionInfo::FunctionInfo (GIBaseInfo* gi_info) {
    info = g_base_info_ref (gi_info);
    call_parameters = nullptr;
}

FunctionInfo::~FunctionInfo () {
    g_base_info_unref (info);
    g_function_invoker_destroy (&invoker);

    if (call_parameters != nullptr)
        delete[] call_parameters;
}

/**
 * Initializes the function calling data.
 */
void FunctionInfo::Init() {

    if (call_parameters != nullptr)
        return;

    g_function_info_prep_invoker (info, &invoker, NULL);

    is_method = IsMethod(info);
    can_throw = g_callable_info_can_throw_gerror (info);

    n_callable_args = g_callable_info_get_n_args (info);
    n_total_args = n_callable_args;
    n_out_args = 0;
    n_in_args = 0;

    if (is_method)
        n_total_args++;

    if (can_throw)
        n_total_args++;

    call_parameters = new Parameter[n_callable_args]();

    /*
     * Examine load parameter types and count arguments
     */

    for (int i = 0; i < n_callable_args; i++) {
        GIArgInfo arg_info;
        GITypeInfo type_info;
        g_callable_info_load_arg ((GICallableInfo *) info, i, &arg_info);
        g_arg_info_load_type (&arg_info, &type_info);

        bool may_be_null = g_arg_info_may_be_null (&arg_info);
        GIDirection direction = g_arg_info_get_direction (&arg_info);

        call_parameters[i].direction = direction;

        if (call_parameters[i].type == ParameterType::SKIP)
            continue;

        // If there is an array length, this is an array
        int length_i = g_type_info_get_array_length (&type_info);
        if (length_i >= 0) {
            call_parameters[i].type        = ParameterType::ARRAY;
            call_parameters[length_i].type = ParameterType::SKIP;

            // If array length came before, we need to remove it from args count

            if (IS_IN(call_parameters[length_i].direction) && length_i < i)
                n_in_args--;

            if (IS_OUT(call_parameters[length_i].direction) && length_i < i)
                n_out_args--;

        } else {
            call_parameters[i].type = ParameterType::NORMAL;
        }

        if (IS_IN(call_parameters[i].direction) && !may_be_null)
            n_in_args++;

        if (IS_OUT(call_parameters[i].direction))
            n_out_args++;

    }

    /*
     * Examine return type
     */

    GITypeInfo return_type;
    g_callable_info_load_return_type(info, &return_type);
    bool should_skip_return = ShouldSkipReturn(info, &return_type);

    if (!should_skip_return)
        n_out_args++;
}

/**
 * Type checks the JS arguments, throwing an error.
 * @returns true if types match
 */
bool FunctionInfo::TypeCheck (const Nan::FunctionCallbackInfo<Value> &arguments) {

    if (arguments.Length() < n_in_args) {
        ThrowNotEnoughArguments(n_in_args, arguments.Length());
        return false;
    }

    /*
     * Type check every IN-argument that is not skipped
     */

    for (int in_arg = 0, i = 0; i < n_callable_args; i++) {
        Parameter &param = call_parameters[i];

        if (param.type == ParameterType::SKIP)
            continue;

        GIArgInfo arg_info;
        g_callable_info_load_arg (info, i, &arg_info);
        GIDirection direction = g_arg_info_get_direction (&arg_info);

        if (direction == GI_DIRECTION_IN || direction == GI_DIRECTION_INOUT) {
            GITypeInfo type_info;
            g_arg_info_load_type (&arg_info, &type_info);
            bool may_be_null = g_arg_info_may_be_null (&arg_info);

            if (!CanConvertV8ToGIArgument(&type_info, arguments[in_arg], may_be_null)) {
                ThrowInvalidType(&arg_info, &type_info, arguments[in_arg]);
                return false;
            }
            in_arg++;
        }
    }

    return true;
}

/**
 * Creates the JS return value from the C arguments list
 * @returns the JS return value
 */
Local<Value> FunctionInfo::GetReturnValue (GITypeInfo* return_type, GIArgument* return_value, GIArgument* callable_arg_values) {

    Local<Value> jsReturnValue;
    int jsReturnIndex = 0;

    if (n_out_args > 1)
        jsReturnValue = Nan::New<Array>();

#define ADD_RETURN(value)   if (n_out_args > 1) \
                                Nan::Set(jsReturnValue->ToObject(), jsReturnIndex++, (value)); \
                            else \
                                jsReturnValue = (value);

    if (!ShouldSkipReturn(info, return_type)) {
        long length = -1;
        int length_i = g_type_info_get_array_length(return_type);
        if (length_i >= 0)
            length = callable_arg_values[length_i].v_long;
        ADD_RETURN (GIArgumentToV8 (return_type, return_value, length))
    }

    for (int i = 0; i < n_callable_args; i++) {
        GIArgInfo  arg_info = {};
        GITypeInfo arg_type;
        GIArgument arg_value = callable_arg_values[i];
        Parameter &param = call_parameters[i];

        g_callable_info_load_arg (info, i, &arg_info);
        g_arg_info_load_type (&arg_info, &arg_type);

        GIDirection direction = g_arg_info_get_direction (&arg_info);

        if (direction == GI_DIRECTION_OUT || direction == GI_DIRECTION_INOUT) {

            if (param.type == ParameterType::ARRAY) {

                int length_i = g_type_info_get_array_length(&arg_type);
                GIArgInfo length_arg;
                g_callable_info_load_arg (info, length_i, &length_arg);
                GIDirection length_direction = g_arg_info_get_direction(&length_arg);

                if (IS_OUT(length_direction))
                    param.length = *(long*)callable_arg_values[length_i].v_pointer;
                else
                    param.length = callable_arg_values[length_i].v_long;

                Local<Value> result = ArrayToV8(&arg_type, *(void**)arg_value.v_pointer, param.length);

                ADD_RETURN (result)

            } else if (param.type == ParameterType::NORMAL) {

                ADD_RETURN (GIArgumentToV8(&arg_type, (GIArgument*) arg_value.v_pointer))
            }
        }
    }

#undef ADD_RETURN

    return jsReturnValue;
}




void FunctionDestroyed(const v8::WeakCallbackInfo<FunctionInfo> &data) {
    FunctionInfo *func = data.GetParameter ();
    delete func;
}

Local<Function> MakeFunction(GIBaseInfo *info) {
    FunctionInfo *func = new FunctionInfo(info);

    auto external = New<External>(func);
    auto name = UTF8(g_function_info_get_symbol (info));

    auto tpl = New<FunctionTemplate>(FunctionInvoker, external);
    tpl->SetLength(g_callable_info_get_n_args (info));

    auto fn = tpl->GetFunction();
    fn->SetName(name);

    Persistent<FunctionTemplate> persistent(Isolate::GetCurrent(), tpl);
    persistent.SetWeak(func, FunctionDestroyed, WeakCallbackType::kParameter);

    return fn;
}

bool PrepareVFuncInvoker (GIFunctionInfo *info, GIFunctionInvoker *invoker, GType implementor, GError **error) {
    gpointer address;
    ffi_type **atypes;
    GITypeInfo *tinfo;
    gint n_args, n_invoke_args, in_pos, out_pos;
    bool success;

    GITypeInfo *rinfo = g_callable_info_get_return_type ((GICallableInfo *)info);
    ffi_type *rtype = g_type_info_get_ffi_type (rinfo);

    in_pos = 0;
    out_pos = 0;

    n_args = g_callable_info_get_n_args ((GICallableInfo *)info);

    n_invoke_args = n_args;

    /* is_method */
    n_invoke_args += 1;
    in_pos++;

    int n_in_args = 0;
    int n_out_args = 0;

    for (int i = 0; i < n_args; i++) {
        GIArgInfo arg_info;
        g_callable_info_load_arg(info, i, &arg_info);
        auto direction = g_arg_info_get_direction(&arg_info);

        if (IS_IN(direction))
            n_in_args++;
        if (IS_OUT(direction))
            n_out_args++;
    }

    atypes = (ffi_type**)g_alloca (sizeof (ffi_type*) * n_invoke_args);

    /* is_method */
    atypes[0] = &ffi_type_pointer;

    for (int i = 0; i < n_args; i++) {
        int offset = 1;
        GIArgInfo *ainfo = g_callable_info_get_arg ((GICallableInfo *)info, i);

        switch (g_arg_info_get_direction (ainfo)) {
            case GI_DIRECTION_IN:
                tinfo = g_arg_info_get_type (ainfo);
                atypes[i+offset] = g_type_info_get_ffi_type (tinfo);
                g_base_info_unref ((GIBaseInfo *)tinfo);

                in_pos++;

                break;
            case GI_DIRECTION_OUT:
                atypes[i+offset] = &ffi_type_pointer;

                out_pos++;
                break;
            case GI_DIRECTION_INOUT:
                atypes[i+offset] = &ffi_type_pointer;

                in_pos++;
                out_pos++;
                break;
            default:
                g_assert_not_reached ();
        }
        g_base_info_unref ((GIBaseInfo *)ainfo);
    }

    success = ffi_prep_cif (&invoker->cif, FFI_DEFAULT_ABI, n_invoke_args, rtype, atypes) == FFI_OK;

    address = g_vfunc_info_get_address (info, implementor, error);
    invoker->native_address = address;

out:
    g_base_info_unref ((GIBaseInfo *)rinfo);
    return success;
}

MaybeLocal<Function> MakeVirtualFunction(GIBaseInfo *info, GType implementor) {
    GError* error = NULL;

    FunctionInfo *func = g_new0 (FunctionInfo, 1);
    func->info = g_base_info_ref (info);
    PrepareVFuncInvoker(info, &func->invoker, implementor, &error);

    if (error != NULL) {
        char* message = g_strdup_printf("Couldn't create virtual function '%s': %s",
                g_base_info_get_name(info), error->message);
        Nan::ThrowError(message);
        g_free (message);
        g_base_info_unref (func->info);
        g_function_invoker_destroy (&func->invoker);
        g_free (func);
        g_error_free (error);
        return MaybeLocal<Function>();
    }

    auto external = New<External>(func);
    auto name = UTF8(g_base_info_get_name (info));

    auto tpl = New<FunctionTemplate>(FunctionInvoker, external);
    tpl->SetLength(g_callable_info_get_n_args (info));

    auto fn = tpl->GetFunction();
    fn->SetName(name);

    Persistent<FunctionTemplate> persistent(Isolate::GetCurrent(), tpl);
    persistent.SetWeak(func, FunctionDestroyed, WeakCallbackType::kParameter);

    return MaybeLocal<Function>(fn);
}


#if 0
class TrampolineInfo {
    ffi_cif cif;
    ffi_closure *closure;
    Persistent<Function> persistent;
    GICallableInfo *info;
    GIScopeType scope_type;

    TrampolineInfo(Local<Function> function, GICallableInfo *info, GIScopeType scope_type);

    void Dispose();
    void *GetClosure();

    static void Call(ffi_cif *cif, void *result, void **args, void *data);
};

TrampolineInfo::TrampolineInfo(Local<Function>  function,
                               GICallableInfo   *info,
                               GIScopeType       scope_type) {
    this->closure = g_callable_info_prepare_closure (info, &cif, TrampolineInfo::Call, this);
    this->persistent.Reset(Isolate::GetCurrent(), function);
    this->info = g_base_info_ref (info);
    this->scope_type = scope_type;
}

void TrampolineInfo::Dispose() {
    persistent.Reset();
    g_base_info_unref (info);
    g_callable_info_free_closure (info, closure);
}

void TrampolineInfo::Call(ffi_cif *cif,
                          void *result,
                          void **args,
                          void *data) {
    TrampolineInfo *trampoline = (TrampolineInfo *) data;

    int argc = g_callable_info_get_n_args (trampoline->info);
    Local<Value> argv[argc];

    for (int i = 0; i < argc; i++) {
        GIArgInfo arg_info;
        g_callable_info_load_arg (trampoline->info, i, &arg_info);
        GITypeInfo type_info;
        g_arg_info_load_type (&arg_info, &type_info);
        argv[i] = GIArgumentToV8 (&type_info, (GIArgument *) &args[i]);
    }

    Local<Function> func = Nan::New<Function> (trampoline->persistent);
    Local<Object> this_obj = func;

    Local<Value> return_value = func->Call (this_obj, argc, argv);

    GITypeInfo type_info;
    g_callable_info_load_return_type (trampoline->info, &type_info);

    V8ToGIArgument (&type_info, (GIArgument *) &result, return_value,
                    g_callable_info_may_return_null (trampoline->info));
}
#endif

};
