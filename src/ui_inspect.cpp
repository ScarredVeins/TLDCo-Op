#include "ui_inspect.h"

#include <windows.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "paths.h"

namespace ui_inspect {
namespace {

constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr ULONGLONG kSettleMs = 500;  // let the screen fill in its texts before dumping

struct Vector3 {
    float x, y, z;
};

struct Api {
    const MethodInfo* component_go = nullptr;    // Component.get_gameObject()
    const MethodInfo* go_transform = nullptr;    // GameObject.get_transform()
    const MethodInfo* go_components = nullptr;   // GameObject.GetComponents(Type)
    const MethodInfo* go_active = nullptr;       // GameObject.get_activeSelf()
    const MethodInfo* go_layer = nullptr;        // GameObject.get_layer()
    const MethodInfo* object_name = nullptr;     // Object.get_name()
    const MethodInfo* child_count = nullptr;     // Transform.get_childCount()
    const MethodInfo* get_child = nullptr;       // Transform.GetChild(int)
    const MethodInfo* get_parent = nullptr;      // Transform.get_parent()
    const MethodInfo* local_position = nullptr;  // Transform.get_localPosition()
    const MethodInfo* label_text = nullptr;      // UILabel.get_text()
    const MethodInfo* sprite_name = nullptr;     // UISprite.get_spriteName()
    const MethodInfo* widget_depth = nullptr;    // UIWidget.get_depth()
    const MethodInfo* widget_width = nullptr;
    const MethodInfo* widget_height = nullptr;
    const MethodInfo* panel_depth = nullptr;     // UIPanel.get_depth()
    const MethodInfo* delegate_method = nullptr; // EventDelegate.get_methodName()
    const MethodInfo* delegate_target = nullptr; // EventDelegate.get_target()
    ptrdiff_t button_on_click = -1;              // UIButton.onClick (List<EventDelegate>)
    Il2CppObject* component_type = nullptr;
} api;

using EnableFn = void (*)(Il2CppObject* self, bool enable, const MethodInfo* method);
EnableFn g_enable = nullptr;

struct Pending {
    uintptr_t panel;
    ULONGLONG due;
};
std::vector<Pending> g_pending;
std::set<std::string> g_dumped;  // one dump per screen type per session

std::string name_of(Il2CppObject* obj) {
    return obj ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, obj))) : "<null>";
}

int int_of(const MethodInfo* getter, Il2CppObject* obj) {
    int32_t v = 0;
    il2::invoke_value(getter, obj, v);
    return v;
}

std::string clip(std::string s, size_t n = 70) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s.size() > n ? s.substr(0, n) + "..." : s;
}

// Methods a UIButton's onClick list calls, e.g. "Panel_SelectSurvivor.OnSelectSurvivorMale".
std::string on_click_targets(Il2CppObject* button) {
    Il2CppObject* list = il2::read_field<Il2CppObject*>(button, api.button_on_click);
    if (!list) return "";
    Il2CppClass* list_class = il2::il2cpp_object_get_class(list);
    const MethodInfo* count_m = il2::il2cpp_class_get_method_from_name(list_class, "get_Count", 0);
    const MethodInfo* item_m = il2::il2cpp_class_get_method_from_name(list_class, "get_Item", 1);
    int32_t count = 0;
    il2::invoke_value(count_m, list, count);
    std::string out;
    for (int32_t i = 0; i < count; ++i) {
        void* args[] = {&i};
        Il2CppObject* del = il2::invoke(item_m, list, args);
        if (!del) continue;
        Il2CppObject* target = il2::invoke(api.delegate_target, del);
        const std::string method = il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.delegate_method, del)));
        out += (out.empty() ? "" : ", ") + il2::class_name_of(target) + "." + method;
    }
    return out;
}

std::string describe_component(Il2CppObject* c) {
    const std::string cls = il2::class_name_of(c);
    if (cls == "UnityEngine.Transform") return "";
    std::string s = cls;
    const bool widget = cls == "UILabel" || cls == "UISprite" || cls == "UITexture" || cls == "UIWidget" ||
                        cls == "UI2DSprite";
    if (cls == "UILabel") s += " text=\"" + clip(il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.label_text, c)))) + "\"";
    if (cls == "UISprite") s += " sprite=\"" + il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.sprite_name, c))) + "\"";
    if (widget) {
        char buf[96];
        snprintf(buf, sizeof(buf), " depth=%d %dx%d", int_of(api.widget_depth, c), int_of(api.widget_width, c),
                 int_of(api.widget_height, c));
        s += buf;
    }
    if (cls == "UIPanel") s += " depth=" + std::to_string(int_of(api.panel_depth, c));
    if (cls == "UIButton") {
        const std::string targets = on_click_targets(c);
        if (!targets.empty()) s += " onClick=[" + targets + "]";
    }
    return s;
}

void dump_node(FILE* f, Il2CppObject* transform, int depth) {
    Il2CppObject* go = il2::invoke(api.component_go, transform);
    bool active = false;
    il2::invoke_value(api.go_active, go, active);
    Vector3 p{};
    il2::invoke_value(api.local_position, transform, p);
    std::string comps;
    void* type_arg[] = {api.component_type};
    Il2CppObject* components = il2::invoke(api.go_components, go, type_arg);
    for (size_t i = 0, n = il2::array_length(components); i < n; ++i) {
        const std::string d = describe_component(il2::array_object_at(components, i));
        if (!d.empty()) comps += (comps.empty() ? "" : " | ") + d;
    }
    fprintf(f, "%*s%s%s pos=(%.0f,%.0f,%.0f) layer=%d%s%s\n", depth * 2, "", name_of(go).c_str(),
            active ? "" : " [inactive]", p.x, p.y, p.z, int_of(api.go_layer, go), comps.empty() ? "" : "  :: ",
            comps.c_str());
    int32_t children = 0;
    il2::invoke_value(api.child_count, transform, children);
    for (int32_t i = 0; i < children; ++i) {
        void* args[] = {&i};
        dump_node(f, il2::invoke(api.get_child, transform, args), depth + 1);
    }
}

void dump_panel(Il2CppObject* panel) {
    const std::string cls = il2::class_name_of(panel);
    const auto dir = paths::mod_dir() / L"ui";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / (std::wstring(cls.begin(), cls.end()) + L".txt");
    FILE* f = _wfopen(file.c_str(), L"w");
    if (!f) return;

    Il2CppObject* go = il2::invoke(api.component_go, panel);
    Il2CppObject* transform = il2::invoke(api.go_transform, go);
    // Where the screen sits: its ancestors up to the UI root.
    fprintf(f, "# %s\n# ancestors (nearest first):\n", cls.c_str());
    for (Il2CppObject* t = il2::invoke(api.get_parent, transform); t; t = il2::invoke(api.get_parent, t)) {
        Il2CppObject* ancestor = il2::invoke(api.component_go, t);
        std::string comps;
        void* type_arg[] = {api.component_type};
        Il2CppObject* components = il2::invoke(api.go_components, ancestor, type_arg);
        for (size_t i = 0, n = il2::array_length(components); i < n; ++i) {
            const std::string d = describe_component(il2::array_object_at(components, i));
            if (!d.empty()) comps += (comps.empty() ? "" : " | ") + d;
        }
        fprintf(f, "#   %s layer=%d :: %s\n", name_of(ancestor).c_str(), int_of(api.go_layer, ancestor), comps.c_str());
    }
    fprintf(f, "# hierarchy:\n");
    dump_node(f, transform, 0);
    fclose(f);
    logx::info("ui: dumped %s to %ls", cls.c_str(), file.c_str());
}

void enable_detour(Il2CppObject* self, bool enable, const MethodInfo* method) {
    g_enable(self, enable, method);
    if (!enable || !self) return;
    const std::string cls = il2::class_name_of(self);
    if (g_dumped.count(cls)) return;
    g_dumped.insert(cls);
    g_pending.push_back({il2::il2cpp_gchandle_new(self, false), GetTickCount64() + kSettleMs});
}

}  // namespace

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    Il2CppClass* sprite = il2::find_class(kGame, "", "UISprite");
    Il2CppClass* widget = il2::find_class(kGame, "", "UIWidget");
    Il2CppClass* panel = il2::find_class(kGame, "", "UIPanel");
    Il2CppClass* button = il2::find_class(kGame, "", "UIButton");
    Il2CppClass* event_delegate = il2::find_class(kGame, "", "EventDelegate");
    Il2CppClass* panel_base = il2::find_class(kGame, "", "Panel_Base");

    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.go_components = il2::find_method_params(game_object, "GetComponents", {"System.Type"});
    api.go_active = il2::find_method(game_object, "get_activeSelf", 0);
    api.go_layer = il2::find_method(game_object, "get_layer", 0);
    api.object_name = il2::find_method(object, "get_name", 0);
    api.child_count = il2::find_method(transform, "get_childCount", 0);
    api.get_child = il2::find_method(transform, "GetChild", 1);
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    api.local_position = il2::find_method(transform, "get_localPosition", 0);
    api.label_text = il2::find_method(label, "get_text", 0);
    api.sprite_name = il2::find_method(sprite, "get_spriteName", 0);
    api.widget_depth = il2::find_method(widget, "get_depth", 0);
    api.widget_width = il2::find_method(widget, "get_width", 0);
    api.widget_height = il2::find_method(widget, "get_height", 0);
    api.panel_depth = il2::find_method(panel, "get_depth", 0);
    api.delegate_method = il2::find_method(event_delegate, "get_methodName", 0);
    api.delegate_target = il2::find_method(event_delegate, "get_target", 0);
    api.button_on_click = il2::field_offset(button, "onClick");
    api.component_type = il2::type_object(component);
    if (api.component_type) il2::il2cpp_gchandle_new(api.component_type, false);

    const MethodInfo* enable = il2::find_method(panel_base, "Enable", 1);
    if (!enable || !api.go_components || !api.label_text) {
        logx::warn("ui: inspector unavailable");
        return false;
    }
    return hooks::install_method("Panel_Base.Enable", enable, reinterpret_cast<void*>(&enable_detour),
                                 reinterpret_cast<void**>(&g_enable));
}

void on_frame() {
    if (g_pending.empty()) return;
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now < it->due) {
            ++it;
            continue;
        }
        if (Il2CppObject* panel = il2::il2cpp_gchandle_get_target(it->panel)) dump_panel(panel);
        il2::il2cpp_gchandle_free(it->panel);
        it = g_pending.erase(it);
    }
}

}  // namespace ui_inspect
