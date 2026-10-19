#include "dumper.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include "il2cpp_api.h"
#include "log.h"

namespace dumper {
namespace {

std::string type_name(const Il2CppType* type) {
    if (!type) return "?";
    char* name = il2::il2cpp_type_get_name(type);
    if (!name) return "?";
    std::string s = name;
    il2::il2cpp_free(name);
    return s;
}

void append(std::string& buf, const char* fmt, ...) {
    char line[2048];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n > 0) buf.append(line, static_cast<size_t>(n) < sizeof(line) ? n : sizeof(line) - 1);
}

void dump_class(std::string& buf, Il2CppClass* klass) {
    char name[512];
    il2::full_class_name(klass, name, sizeof(name));

    const char* kind = il2::il2cpp_class_is_enum(klass)        ? "enum"
                       : il2::il2cpp_class_is_valuetype(klass) ? "struct"
                                                               : "class";
    char parent[512] = "";
    if (Il2CppClass* p = il2::il2cpp_class_get_parent(klass)) il2::full_class_name(p, parent, sizeof(parent));
    append(buf, "%s %s%s%s\n", kind, name, *parent ? " : " : "", parent);

    const bool is_enum = il2::il2cpp_class_is_enum(klass);
    void* iter = nullptr;
    while (FieldInfo* f = il2::il2cpp_class_get_fields(klass, &iter)) {
        int flags = il2::il2cpp_field_get_flags(f);
        int64_t value = 0;
        if (is_enum && il2::literal_value(f, value)) {
            append(buf, "\tconst  %s = %lld\n", il2::il2cpp_field_get_name(f), static_cast<long long>(value));
            continue;
        }
        const char* tag = (flags & il2::kFieldLiteral) ? "const " : (flags & il2::kFieldStatic) ? "static" : "field ";
        append(buf, "\t%s 0x%04zx  %s %s\n", tag, il2::il2cpp_field_get_offset(f),
               type_name(il2::il2cpp_field_get_type(f)).c_str(), il2::il2cpp_field_get_name(f));
    }

    iter = nullptr;
    while (const MethodInfo* m = il2::il2cpp_class_get_methods(klass, &iter)) {
        uint32_t iflags = 0;
        uint32_t flags = il2::il2cpp_method_get_flags(m, &iflags);
        std::string params;
        uint32_t count = il2::il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < count; ++i) {
            if (i) params += ", ";
            params += type_name(il2::il2cpp_method_get_param(m, i));
            params += ' ';
            const char* pname = il2::il2cpp_method_get_param_name(m, i);
            params += pname ? pname : "?";
        }
        const void* code = il2::method_pointer(m);
        append(buf, "\tmethod rva=0x%08llx %s%s%s %s(%s)\n", static_cast<unsigned long long>(il2::rva(code)),
               (flags & il2::kMethodStatic) ? "static " : "", (flags & il2::kMethodVirtual) ? "virtual " : "",
               type_name(il2::il2cpp_method_get_return_type(m)).c_str(), il2::il2cpp_method_get_name(m),
               params.c_str());
    }
}

}  // namespace

size_t dump_types(const std::filesystem::path& out) {
    FILE* file = _wfopen(out.c_str(), L"wb");
    if (!file) {
        logx::error("cannot write %ls", out.c_str());
        return 0;
    }

    size_t total = 0;
    size_t assembly_count = 0;
    const Il2CppAssembly** assemblies = il2::il2cpp_domain_get_assemblies(il2::il2cpp_domain_get(), &assembly_count);
    std::string buf;
    buf.reserve(1 << 20);
    for (size_t a = 0; a < assembly_count; ++a) {
        const Il2CppImage* image = il2::il2cpp_assembly_get_image(assemblies[a]);
        size_t class_count = il2::il2cpp_image_get_class_count(image);
        append(buf, "// ===== %s (%zu classes)\n", il2::il2cpp_image_get_name(image), class_count);
        for (size_t c = 0; c < class_count; ++c) {
            dump_class(buf, const_cast<Il2CppClass*>(il2::il2cpp_image_get_class(image, c)));
            if (buf.size() > (1 << 20)) {
                fwrite(buf.data(), 1, buf.size(), file);
                buf.clear();
            }
        }
        total += class_count;
    }
    fwrite(buf.data(), 1, buf.size(), file);
    fclose(file);
    logx::info("type dump: %zu assemblies, %zu classes -> %ls", assembly_count, total, out.c_str());
    return total;
}

}  // namespace dumper
