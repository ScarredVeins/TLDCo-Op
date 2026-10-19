#pragma once
// Thin bridge to the IL2CPP runtime exported by GameAssembly.dll. Every game class, field and method is found by
// name at runtime, so nothing here hard-codes offsets that move between TLD builds.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppType;
struct FieldInfo;
struct MethodInfo;
struct Il2CppObject;
struct Il2CppString;
struct Il2CppThread;

// X(name, return type, parameter list)
#define TLDCOOP_IL2CPP_API(X)                                                                           \
    X(il2cpp_domain_get, Il2CppDomain*, ())                                                             \
    X(il2cpp_domain_get_assemblies, const Il2CppAssembly**, (const Il2CppDomain*, size_t*))            \
    X(il2cpp_assembly_get_image, const Il2CppImage*, (const Il2CppAssembly*))                          \
    X(il2cpp_image_get_name, const char*, (const Il2CppImage*))                                        \
    X(il2cpp_image_get_class_count, size_t, (const Il2CppImage*))                                      \
    X(il2cpp_image_get_class, const Il2CppClass*, (const Il2CppImage*, size_t))                        \
    X(il2cpp_class_from_name, Il2CppClass*, (const Il2CppImage*, const char*, const char*))            \
    X(il2cpp_class_get_name, const char*, (const Il2CppClass*))                                        \
    X(il2cpp_class_get_namespace, const char*, (const Il2CppClass*))                                   \
    X(il2cpp_class_get_parent, Il2CppClass*, (const Il2CppClass*))                                     \
    X(il2cpp_class_get_declaring_type, Il2CppClass*, (const Il2CppClass*))                             \
    X(il2cpp_class_is_valuetype, bool, (const Il2CppClass*))                                           \
    X(il2cpp_class_is_enum, bool, (const Il2CppClass*))                                                \
    X(il2cpp_class_get_fields, FieldInfo*, (Il2CppClass*, void**))                                     \
    X(il2cpp_class_get_methods, const MethodInfo*, (Il2CppClass*, void**))                             \
    X(il2cpp_class_get_method_from_name, const MethodInfo*, (Il2CppClass*, const char*, int))          \
    X(il2cpp_class_get_field_from_name, FieldInfo*, (Il2CppClass*, const char*))                       \
    X(il2cpp_field_get_name, const char*, (FieldInfo*))                                                \
    X(il2cpp_field_get_offset, size_t, (FieldInfo*))                                                   \
    X(il2cpp_field_get_type, const Il2CppType*, (FieldInfo*))                                          \
    X(il2cpp_field_get_flags, int, (FieldInfo*))                                                       \
    X(il2cpp_field_static_get_value, void, (FieldInfo*, void*))                                        \
    X(il2cpp_field_static_set_value, void, (FieldInfo*, void*))                                        \
    X(il2cpp_type_get_name, char*, (const Il2CppType*))                                                \
    X(il2cpp_free, void, (void*))                                                                      \
    X(il2cpp_method_get_name, const char*, (const MethodInfo*))                                        \
    X(il2cpp_method_get_param_count, uint32_t, (const MethodInfo*))                                    \
    X(il2cpp_method_get_param, const Il2CppType*, (const MethodInfo*, uint32_t))                       \
    X(il2cpp_method_get_param_name, const char*, (const MethodInfo*, uint32_t))                        \
    X(il2cpp_method_get_return_type, const Il2CppType*, (const MethodInfo*))                           \
    X(il2cpp_method_get_flags, uint32_t, (const MethodInfo*, uint32_t*))                               \
    X(il2cpp_runtime_invoke, Il2CppObject*, (const MethodInfo*, void*, void**, Il2CppObject**))        \
    X(il2cpp_thread_attach, Il2CppThread*, (Il2CppDomain*))                                            \
    X(il2cpp_string_new, Il2CppString*, (const char*))                                                 \
    X(il2cpp_resolve_icall, void*, (const char*))                                                      \
    X(il2cpp_object_get_class, Il2CppClass*, (Il2CppObject*))                                          \
    X(il2cpp_object_unbox, void*, (Il2CppObject*))                                                     \
    X(il2cpp_string_chars, uint16_t*, (Il2CppString*))                                                 \
    X(il2cpp_string_length, int32_t, (Il2CppString*))                                                  \
    X(il2cpp_method_get_class, Il2CppClass*, (const MethodInfo*))                                      \
    X(il2cpp_class_get_type, const Il2CppType*, (Il2CppClass*))                                        \
    X(il2cpp_type_get_object, Il2CppObject*, (const Il2CppType*))                                      \
    X(il2cpp_gchandle_new, uintptr_t, (Il2CppObject*, bool))                                           \
    X(il2cpp_gchandle_get_target, Il2CppObject*, (uintptr_t))                                          \
    X(il2cpp_gchandle_free, void, (uintptr_t))                                                         \
    X(il2cpp_object_new, Il2CppObject*, (const Il2CppClass*))                                          \
    X(il2cpp_object_get_virtual_method, const MethodInfo*, (Il2CppObject*, const MethodInfo*))

namespace il2 {

#define TLDCOOP_DECLARE(name, ret, args) extern ret(*name) args;
TLDCOOP_IL2CPP_API(TLDCOOP_DECLARE)
#undef TLDCOOP_DECLARE

// Field / method attribute bits from ECMA-335 (as used by IL2CPP).
constexpr int kFieldStatic = 0x10;
constexpr int kFieldLiteral = 0x40;
constexpr uint32_t kMethodStatic = 0x10;
constexpr uint32_t kMethodVirtual = 0x40;

// Resolves every export above. Returns false (and logs which one) if any is missing.
bool resolve(HMODULE game_assembly);

HMODULE game_assembly();

const Il2CppImage* find_image(const char* assembly_file_name);  // e.g. "Assembly-CSharp.dll"
Il2CppClass* find_class(const char* assembly_file_name, const char* name_space, const char* name);
const MethodInfo* find_method(Il2CppClass* klass, const char* name, int arg_count);  // -1 = any arity

// Overload-exact lookup by parameter type names, e.g. {"System.Type"} for GameObject.GetComponent(Type).
const MethodInfo* find_method_params(Il2CppClass* klass, const char* name,
                                     std::initializer_list<const char*> param_types);

// System.Type object for a class (for APIs like GetComponent(Type)). The runtime caches it; no handle needed.
Il2CppObject* type_object(Il2CppClass* klass);

// MethodInfo begins with the compiled code pointer in every IL2CPP version we target.
inline void* method_pointer(const MethodInfo* m) { return m ? *reinterpret_cast<void* const*>(m) : nullptr; }

uintptr_t rva(const void* code);

// How many methods in the whole program share this code pointer. >1 means the linker folded identical bodies
// (e.g. many empty Update() methods), so hooking it would also hook the others.
size_t code_share_count(const void* code);

// "Namespace.Outer/Inner" style name for logs and the dump.
void full_class_name(const Il2CppClass* klass, char* out, size_t out_size);

std::string to_utf8(Il2CppString* s);

// The override of a virtual method for this object's class (species-specific BaseAi behaviour and the like);
// il2cpp_runtime_invoke itself calls exactly the MethodInfo it's given.
inline const MethodInfo* virtual_method(Il2CppObject* obj, const MethodInfo* method) {
    const MethodInfo* m = obj && method ? il2cpp_object_get_virtual_method(obj, method) : nullptr;
    return m ? m : method;
}

// Calls a managed method through the runtime, which catches managed exceptions (null player in menus, destroyed
// objects). Returns the result object (boxed for value types) or nullptr; exceptions are logged, rate-limited.
Il2CppObject* invoke(const MethodInfo* method, void* instance, void** args = nullptr);

// invoke() for value-type results (float, int, bool, enum, Vector3...). False if it threw or returned nothing.
template <typename T>
bool invoke_value(const MethodInfo* method, void* instance, T& out, void** args = nullptr) {
    Il2CppObject* boxed = invoke(method, instance, args);
    if (!boxed) return false;
    out = *static_cast<const T*>(il2cpp_object_unbox(boxed));
    return true;
}

// Byte offset of an instance field, or -1 (logged) if missing.
ptrdiff_t field_offset(Il2CppClass* klass, const char* name);

template <typename T>
T read_field(Il2CppObject* obj, ptrdiff_t offset) {
    T value{};
    if (obj && offset >= 0) memcpy(&value, reinterpret_cast<const char*>(obj) + offset, sizeof(T));
    return value;
}

// Managed arrays (T[]): object header (16 bytes), bounds pointer, length, then the elements.
inline size_t array_length(Il2CppObject* array) {
    return array ? *reinterpret_cast<const size_t*>(reinterpret_cast<const char*>(array) + 0x18) : 0;
}
inline Il2CppObject* array_object_at(Il2CppObject* array, size_t i) {
    return reinterpret_cast<Il2CppObject* const*>(reinterpret_cast<const char*>(array) + 0x20)[i];
}

// "Namespace.Class" of an object's runtime class.
std::string class_name_of(Il2CppObject* obj);

// Name of an enum constant (e.g. WeatherStage 4 -> "Clear"), "" if no constant has that value.
std::string enum_name(Il2CppClass* enum_class, int64_t value);

// Value of a literal (const) field, e.g. an enum constant. False if the field isn't a literal.
bool literal_value(FieldInfo* field, int64_t& out);

// Value of the enum constant called `name` (e.g. PrimitiveType "Capsule" -> 1). False if there is none.
bool enum_value(Il2CppClass* enum_class, const char* name, int64_t& out);

}  // namespace il2
