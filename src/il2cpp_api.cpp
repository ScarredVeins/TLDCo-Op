#include "il2cpp_api.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "log.h"

namespace il2 {

#define TLDCOOP_DEFINE(name, ret, args) ret(*name) args = nullptr;
TLDCOOP_IL2CPP_API(TLDCOOP_DEFINE)
#undef TLDCOOP_DEFINE

namespace {

HMODULE g_game_assembly = nullptr;

std::once_flag g_share_once;
std::unordered_map<const void*, size_t> g_share_counts;

void build_share_counts() {
    size_t assembly_count = 0;
    const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &assembly_count);
    for (size_t a = 0; a < assembly_count; ++a) {
        const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
        size_t class_count = il2cpp_image_get_class_count(image);
        for (size_t c = 0; c < class_count; ++c) {
            auto* klass = const_cast<Il2CppClass*>(il2cpp_image_get_class(image, c));
            void* iter = nullptr;
            while (const MethodInfo* m = il2cpp_class_get_methods(klass, &iter)) {
                if (const void* code = method_pointer(m)) ++g_share_counts[code];
            }
        }
    }
}

}  // namespace

bool resolve(HMODULE game_assembly) {
    g_game_assembly = game_assembly;
    bool ok = true;
#define TLDCOOP_RESOLVE(name, ret, args)                                                       \
    name = reinterpret_cast<ret(*) args>(GetProcAddress(game_assembly, #name));                \
    if (!name) {                                                                               \
        logx::error("GameAssembly.dll is missing export %s", #name);                           \
        ok = false;                                                                            \
    }
    TLDCOOP_IL2CPP_API(TLDCOOP_RESOLVE)
#undef TLDCOOP_RESOLVE
    return ok;
}

HMODULE game_assembly() { return g_game_assembly; }

const Il2CppImage* find_image(const char* assembly_file_name) {
    size_t count = 0;
    const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        if (const char* name = il2cpp_image_get_name(image); name && _stricmp(name, assembly_file_name) == 0)
            return image;
    }
    return nullptr;
}

Il2CppClass* find_class(const char* assembly_file_name, const char* name_space, const char* name) {
    const Il2CppImage* image = find_image(assembly_file_name);
    if (!image) {
        logx::warn("assembly %s not found", assembly_file_name);
        return nullptr;
    }
    Il2CppClass* klass = il2cpp_class_from_name(image, name_space, name);
    if (!klass) logx::warn("class %s%s%s not found in %s", name_space, *name_space ? "." : "", name, assembly_file_name);
    return klass;
}

const MethodInfo* find_method(Il2CppClass* klass, const char* name, int arg_count) {
    if (!klass) return nullptr;
    const MethodInfo* m = il2cpp_class_get_method_from_name(klass, name, arg_count);
    if (!m) {
        char cls[512];
        full_class_name(klass, cls, sizeof(cls));
        logx::warn("method %s.%s(%d args) not found", cls, name, arg_count);
    }
    return m;
}

const MethodInfo* find_method_params(Il2CppClass* klass, const char* name,
                                     std::initializer_list<const char*> param_types) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(klass, &iter)) {
        if (strcmp(il2cpp_method_get_name(m), name) != 0) continue;
        if (il2cpp_method_get_param_count(m) != param_types.size()) continue;
        bool match = true;
        uint32_t i = 0;
        for (const char* expected : param_types) {
            char* actual = il2cpp_type_get_name(il2cpp_method_get_param(m, i++));
            match = actual && strcmp(actual, expected) == 0;
            if (actual) il2cpp_free(actual);
            if (!match) break;
        }
        if (match) return m;
    }
    char cls[512];
    full_class_name(klass, cls, sizeof(cls));
    logx::warn("method %s.%s with the requested parameter types not found", cls, name);
    return nullptr;
}

Il2CppObject* type_object(Il2CppClass* klass) {
    return klass ? il2cpp_type_get_object(il2cpp_class_get_type(klass)) : nullptr;
}

uintptr_t rva(const void* code) {
    auto base = reinterpret_cast<uintptr_t>(g_game_assembly);
    auto p = reinterpret_cast<uintptr_t>(code);
    return (code && p > base) ? p - base : 0;
}

size_t code_share_count(const void* code) {
    std::call_once(g_share_once, build_share_counts);
    auto it = g_share_counts.find(code);
    return it == g_share_counts.end() ? 0 : it->second;
}

std::string to_utf8(Il2CppString* s) {
    if (!s) return {};
    int32_t len = il2cpp_string_length(s);
    if (len <= 0) return {};
    auto chars = reinterpret_cast<const wchar_t*>(il2cpp_string_chars(s));
    int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, chars, len, out.data(), n, nullptr, nullptr);
    return out;
}

namespace {

std::mutex g_exception_mutex;
std::unordered_map<const MethodInfo*, uint64_t> g_exception_counts;

void report_exception(const MethodInfo* method, Il2CppObject* exception) {
    uint64_t count;
    {
        std::lock_guard lock(g_exception_mutex);
        count = ++g_exception_counts[method];
    }
    // 1st, 10th, 100th, ... occurrence only: a getter that throws every frame must not flood the log.
    bool power_of_ten = true;
    for (uint64_t n = count; n > 1; n /= 10)
        if (n % 10) power_of_ten = false;
    if (!power_of_ten) return;

    char cls[512], exc[512];
    full_class_name(il2cpp_method_get_class(method), cls, sizeof(cls));
    full_class_name(il2cpp_object_get_class(exception), exc, sizeof(exc));
    logx::warn("%s.%s threw %s (x%llu)", cls, il2cpp_method_get_name(method), exc,
               static_cast<unsigned long long>(count));
}

}  // namespace

Il2CppObject* invoke(const MethodInfo* method, void* instance, void** args) {
    if (!method) return nullptr;
    Il2CppObject* exception = nullptr;
    Il2CppObject* result = il2cpp_runtime_invoke(method, instance, args, &exception);
    if (exception) {
        report_exception(method, exception);
        return nullptr;
    }
    return result;
}

ptrdiff_t field_offset(Il2CppClass* klass, const char* name) {
    FieldInfo* field = klass ? il2cpp_class_get_field_from_name(klass, name) : nullptr;
    if (!field) {
        char cls[512];
        full_class_name(klass, cls, sizeof(cls));
        logx::warn("field %s.%s not found", cls, name);
        return -1;
    }
    return static_cast<ptrdiff_t>(il2cpp_field_get_offset(field));
}

bool literal_value(FieldInfo* field, int64_t& out) {
    if (!(il2cpp_field_get_flags(field) & kFieldLiteral)) return false;
    int64_t raw = 0;  // the runtime copies only the field's own size (4 bytes for int-backed enums)
    il2cpp_field_static_get_value(field, &raw);
    out = raw;
    return true;
}

std::string class_name_of(Il2CppObject* obj) {
    if (!obj) return {};
    char name[512];
    full_class_name(il2cpp_object_get_class(obj), name, sizeof(name));
    return name;
}

bool enum_value(Il2CppClass* enum_class, const char* name, int64_t& out) {
    if (!enum_class) return false;
    FieldInfo* field = il2cpp_class_get_field_from_name(enum_class, name);
    return field && literal_value(field, out);
}

std::string enum_name(Il2CppClass* enum_class, int64_t value) {
    if (!enum_class) return {};
    void* iter = nullptr;
    while (FieldInfo* f = il2cpp_class_get_fields(enum_class, &iter)) {
        int64_t v;
        if (literal_value(f, v) && (v == value || static_cast<int32_t>(v) == static_cast<int32_t>(value)))
            return il2cpp_field_get_name(f);
    }
    return {};
}

void full_class_name(const Il2CppClass* klass, char* out, size_t out_size) {
    if (!klass) {
        snprintf(out, out_size, "<null>");
        return;
    }
    if (const Il2CppClass* outer = il2cpp_class_get_declaring_type(klass)) {
        char outer_name[512];
        full_class_name(outer, outer_name, sizeof(outer_name));
        snprintf(out, out_size, "%s/%s", outer_name, il2cpp_class_get_name(klass));
        return;
    }
    const char* ns = il2cpp_class_get_namespace(klass);
    snprintf(out, out_size, "%s%s%s", ns ? ns : "", (ns && *ns) ? "." : "", il2cpp_class_get_name(klass));
}

}  // namespace il2
