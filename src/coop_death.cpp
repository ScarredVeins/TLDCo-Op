#include "coop_death.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#include "avatar.h"
#include "config.h"
#include "coop_menu.h"
#include "coop_notices.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"
#include "world_containers.h"
#include "world_join.h"
#include "world_objects.h"

namespace coop_death {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
// TLD's corpse prefab (CustomSpawnedItem_Corpse, an addressable): what a player leaves when they die.
constexpr const char* kCorpseAsset = "f83bbf944b72cbe40b7dfcdd613710c6";
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr size_t kItemEntry = 16;       // TLD.Gear.GearItemObject in an array: GearItem m_GearItem, string m_GearItemName
constexpr int32_t kNormal = 0;          // PlayerControlMode.Normal
constexpr int32_t kReviveStage = 0;     // Panel_LifeAfterDeath.LifeAfterDeathStage.Revive
constexpr double kSaveAfterMs = 3000;   // the respawned character is saved this long after (TLD settles the start gear)
constexpr const char* kStagePath = "Stage 1 - Revive";
constexpr const char* kLeftLabels[] = {"Stage 1 - Revive/ReviveInteractions/Buttons/LeftButton/Mouse/LeftButton/Mouse_Button_Label",
                                       "Stage 1 - Revive/ReviveInteractions/Buttons/LeftButton/Controller/SpriteLabel"};
constexpr const char* kRightLabels[] = {"Stage 1 - Revive/ReviveInteractions/Buttons/RightButton/Mouse/RightButton/Mouse_Button_Label",
                                        "Stage 1 - Revive/ReviveInteractions/Buttons/RightButton/Controller/SpriteLabel"};
// The next survivor above the cause of death (the screen is 1280 x 720 around its middle), their name below it.
constexpr float kPreviewY = 180, kNameY = -70;
constexpr int kPreviewWidth = 200, kPreviewHeight = 320, kPreviewDepth = 5;

struct Vector3 {
    float x, y, z;
};
struct Quaternion {
    float x, y, z, w;
};

struct Api {
    // Unity
    Il2CppClass* game_object_class = nullptr;
    const MethodInfo* go_ctor = nullptr;            // GameObject(string)
    const MethodInfo* component_go = nullptr;       // Component.get_gameObject()
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* go_transform = nullptr;       // GameObject.get_transform()
    const MethodInfo* go_set_active = nullptr;
    const MethodInfo* go_active = nullptr;          // GameObject.get_activeInHierarchy()
    const MethodInfo* go_get_layer = nullptr;
    const MethodInfo* go_set_layer = nullptr;
    const MethodInfo* add_component = nullptr;      // GameObject.AddComponent(Type)
    const MethodInfo* go_components = nullptr;      // GameObject.GetComponentsInChildren(Type, bool)
    const MethodInfo* in_children = nullptr;        // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* instantiate = nullptr;        // Object.Instantiate(Object, Transform)
    const MethodInfo* object_name = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* set_position = nullptr;
    const MethodInfo* set_rotation = nullptr;
    const MethodInfo* set_local_position = nullptr;
    const MethodInfo* set_local_scale = nullptr;
    const MethodInfo* set_parent = nullptr;         // Transform.SetParent(Transform, bool)
    const MethodInfo* get_parent = nullptr;
    const MethodInfo* find_child = nullptr;         // Transform.Find(string)
    // NGUI
    Il2CppObject* label_type = nullptr;             // typeof(UILabel)
    const MethodInfo* label_set_text = nullptr;
    const MethodInfo* widget_set_alpha = nullptr;
    const MethodInfo* widget_set_color = nullptr;   // UIWidget.set_color(Color)
    const MethodInfo* widget_set_width = nullptr;
    const MethodInfo* widget_set_height = nullptr;
    const MethodInfo* widget_set_depth = nullptr;
    Il2CppObject* texture_type = nullptr;           // typeof(UITexture)
    const MethodInfo* texture_set_main = nullptr;
    // The player
    const MethodInfo* player_manager = nullptr;     // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* inventory = nullptr;          // static GameManager.GetInventoryComponent()
    const MethodInfo* player_transform = nullptr;   // static GameManager.GetPlayerTransform()
    const MethodInfo* force_save = nullptr;         // static GameManager.ForceSaveGame()
    const MethodInfo* unequip = nullptr;            // PlayerManager.UnequipImmediate(bool)
    const MethodInfo* set_mode = nullptr;           // PlayerManager.SetControlMode(PlayerControlMode)
    const MethodInfo* take_off = nullptr;           // PlayerManager.TakeOffClothingItem(GearItem)
    ptrdiff_t start_gear_applied = -1;              // PlayerManager.m_StartGearApplied
    ptrdiff_t control_mode = -1;                    // PlayerManager.m_ControlMode
    ptrdiff_t lad_tunables = -1;                    // LifeAfterDeathManager.m_Tunables (a struct, lives first)
    ptrdiff_t inv_items = -1;                       // Inventory.m_Items (List<GearItemObject>)
    const MethodInfo* inv_remove = nullptr;         // Inventory.RemoveGear(GameObject, bool toContainer)
    const MethodInfo* potable = nullptr;            // Inventory.GetPotableWaterSupply()
    const MethodInfo* not_potable = nullptr;        // Inventory.GetNonPotableWaterSupply()
    ptrdiff_t gear_clothing = -1;                   // GearItem.m_ClothingItem
    ptrdiff_t gear_water = -1;                      // GearItem.m_WaterSupply
    ptrdiff_t water_volume = -1;                    // WaterSupply.m_VolumeInLiters (ItemLiquidVolume: int64 units)
    const MethodInfo* wearing = nullptr;            // ClothingItem.IsWearing()
    const MethodInfo* make_gear = nullptr;          // static GearItem.InstantiateGearItem(string)
    std::string potable_name = "GEAR_WaterSupplyPotable", not_potable_name = "GEAR_WaterSupplyNotPotable";
    Il2CppObject* audio_type = nullptr;             // typeof(AudioCallback)
    const MethodInfo* audio_toggle = nullptr;       // AudioCallback.ToggleDisableAudioEvents(bool)
    const MethodInfo* fade_in = nullptr;            // static CameraFade.FadeIn(float, float, Action)
    // The fresh character
    Il2CppClass* game_manager = nullptr;
    ptrdiff_t hp = -1, max_hp = -1;                 // Condition
    ptrdiff_t calories = -1, start_calories = -1;   // Hunger
    ptrdiff_t thirst = -1, fatigue = -1, freezing = -1;
    ptrdiff_t skills = -1, skill_points = -1;       // SkillsManager.m_Skills, Skill.m_CurrentPoints
    // The body
    const MethodInfo* spawn = nullptr;              // static CustomSpawnedItemManager.SpawnBigItem(AssetReference)
    const MethodInfo* load_items = nullptr;         // static CustomSpawnedItemManager.DeserializeAll(string)
    FieldInfo* spawned = nullptr;                   // static CustomSpawnedItemManager.s_Items
    Il2CppClass* asset_ref_class = nullptr;         // TLD.Gameplay.AssetReferenceCustomSpawnedItem
    const MethodInfo* asset_ref_ctor = nullptr;     // (string guid)
    const MethodInfo* item_save = nullptr;          // CustomSpawnedItem.Serialize()
    const MethodInfo* item_guid = nullptr;          // CustomSpawnedItem.get_Guid()
    const MethodInfo* item_address = nullptr;       // CustomSpawnedItem.get_Address()
    ptrdiff_t asset_guid = -1;                      // AssetReference.m_AssetGUID
    const MethodInfo* to_json = nullptr;            // static SerializationUtils.SerializeObject(object)
    Il2CppObject* container_type = nullptr;         // typeof(Container)
    const MethodInfo* container_start = nullptr;    // Container.Start(): its own rolls, once
    const MethodInfo* container_clear = nullptr;    // Container.DestroyAllGear()
    const MethodInfo* container_add = nullptr;      // Container.AddGear(GearItem)
    const MethodInfo* container_save = nullptr;     // Container.Serialize()
    const MethodInfo* container_load = nullptr;     // Container.Deserialize(string, List<GearItem>)
    ptrdiff_t inspected = -1, rolled = -1, not_populated = -1, no_save = -1;
    // TLD's screen after a death
    const MethodInfo* panel_enable = nullptr;       // Panel_LifeAfterDeath.Enable(bool)
    const MethodInfo* panel_left = nullptr;         // HandleOnLeftButtonPressed()
    const MethodInfo* panel_right = nullptr;        // HandleOnRightButtonPressed()
    const MethodInfo* panel_back = nullptr;         // HandleOnBackButtonPressed()
    const MethodInfo* panel_to_log = nullptr;       // ContinueToLogPanel(bool): the end-of-game log
    ptrdiff_t panel_stage = -1, panel_button = -1, panel_cause = -1, panel_campfires = -1, panel_buttons = -1;
    // Hooked
    const MethodInfo* handle_death = nullptr;       // static GameManager.HandlePlayerDeath(string)
    const MethodInfo* delete_saves = nullptr;       // static SaveGameSystem.DeleteSaveFilesForGameId(uint)
} api;

bool g_ready = false;

// --- helpers -------------------------------------------------------------------------------------------------------

template <typename T>
T get(Il2CppObject* o, ptrdiff_t off) {
    return il2::read_field<T>(o, off);
}

template <typename T>
void put(Il2CppObject* o, ptrdiff_t off, T value) {
    if (o && off >= 0) memcpy(reinterpret_cast<char*>(o) + off, &value, sizeof(T));
}

bool alive(Il2CppObject* o) { return o && get<void*>(o, kCachedPtr) != nullptr; }
std::string text(Il2CppString* s) { return s ? il2::to_utf8(s) : std::string(); }
std::string name_of(Il2CppObject* o) {
    return alive(o) ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, o))) : std::string("?");
}
std::string short_id(const std::string& id) { return id.size() > 8 ? id.substr(0, 8) : id; }
Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
uintptr_t hold(Il2CppObject* o) { return o ? il2::il2cpp_gchandle_new(o, false) : 0; }
void drop(uintptr_t& h) {
    if (h) il2::il2cpp_gchandle_free(h);
    h = 0;
}
Il2CppObject* go_of(Il2CppObject* component) { return alive(component) ? il2::invoke(api.component_go, component) : nullptr; }
Il2CppObject* transform_of(Il2CppObject* component) {
    return alive(component) ? il2::invoke(api.component_transform, component) : nullptr;
}

void set_active(Il2CppObject* go, bool on) {
    if (!alive(go)) return;
    void* args[] = {&on};
    il2::invoke(api.go_set_active, go, args);
}

bool active(Il2CppObject* go) {
    bool on = false;
    return alive(go) && il2::invoke_value(api.go_active, go, on) && on;
}

void set_vec(const MethodInfo* setter, Il2CppObject* transform, Vector3 v) {
    if (!transform) return;
    void* args[] = {&v};
    il2::invoke(setter, transform, args);
}

void set_int(const MethodInfo* setter, Il2CppObject* obj, int v) {
    if (!obj) return;
    void* args[] = {&v};
    il2::invoke(setter, obj, args);
}

void set_text(Il2CppObject* label, const std::string& value) {
    if (!alive(label)) return;
    void* args[] = {il2::il2cpp_string_new(value.c_str())};
    il2::invoke(api.label_set_text, label, args);
}

Il2CppObject* child(Il2CppObject* transform, const char* path) {
    if (!transform) return nullptr;
    void* args[] = {il2::il2cpp_string_new(path)};
    return il2::invoke(api.find_child, transform, args);
}

Il2CppObject* in_children(Il2CppObject* component, Il2CppObject* type) {
    if (!alive(component) || !type) return nullptr;
    bool inactive_too = true;
    void* args[] = {type, &inactive_too};
    Il2CppObject* found = il2::invoke(api.in_children, component, args);
    return alive(found) ? found : nullptr;
}

Il2CppObject* game_static(const char* field) {
    FieldInfo* f = api.game_manager ? il2::il2cpp_class_get_field_from_name(api.game_manager, field) : nullptr;
    Il2CppObject* o = nullptr;
    if (f) il2::il2cpp_field_static_get_value(f, &o);
    return o;
}

bool coop() { return netrt::role().has_value(); }

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// --- state ---------------------------------------------------------------------------------------------------------

bool g_dead = false;
std::string g_cause;
uintptr_t g_panel = 0;                          // TLD's Panel_LifeAfterDeath, once seen
uintptr_t g_preview = 0;                        // our UITexture on it: the next survivor
uintptr_t g_name_label = 0;                     // our label on it: their name
std::string g_preview_body;                     // the body the preview shows
std::vector<std::string> g_bodies;              // the survivors to choose from (characters, then the capsule)
size_t g_choice = 0;
std::unordered_map<std::string, std::string> g_names;  // body (CustomSpawnedItem guid) -> whose
double g_save_at = 0;                           // the respawned character is saved then (0: nothing to save)
std::map<uint8_t, bool> g_others_dead;          // other players -> dead when we last looked
bool g_keys_down[4] = {};

double now_ms() { return net::now_ms(); }

bool focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// A key that went down since the last frame (and the game window has the focus).
bool pressed(int index, int vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fresh = down && !g_keys_down[index];
    g_keys_down[index] = down;
    return fresh && focused();
}

// --- the body ------------------------------------------------------------------------------------------------------

std::vector<Il2CppObject*> spawned_items() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = nullptr;
    if (api.spawned) il2::il2cpp_field_static_get_value(api.spawned, &list);
    if (!list) return out;
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* item = il2::array_object_at(items, static_cast<size_t>(i)); alive(item)) out.push_back(item);
    return out;
}

std::string guid_of(Il2CppObject* item) { return text(reinterpret_cast<Il2CppString*>(il2::invoke(api.item_guid, item))); }

bool is_body(Il2CppObject* item) {
    Il2CppObject* address = il2::invoke(api.item_address, item);
    return address && text(get<Il2CppString*>(address, api.asset_guid)) == kCorpseAsset;
}

std::vector<Il2CppObject*> bodies() {
    std::vector<Il2CppObject*> out;
    for (Il2CppObject* item : spawned_items())
        if (is_body(item)) out.push_back(item);
    return out;
}

Il2CppObject* body_by_guid(const std::string& guid) {
    for (Il2CppObject* item : bodies())
        if (guid_of(item) == guid) return item;
    return nullptr;
}

Vector3 position_of(Il2CppObject* component) {
    Vector3 p{};
    il2::invoke_value(api.get_position, transform_of(component), p);
    return p;
}

// TLD's record of the body (CustomSpawnedItemSaveData, as its scene save writes it): what it is, where, its ids.
std::string record_of(Il2CppObject* item) {
    Il2CppObject* data = il2::invoke(api.item_save, item);
    void* args[] = {data};
    return data ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.to_json, nullptr, args))) : std::string();
}

std::string contents_of(Il2CppObject* container) {
    return container ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.container_save, container))) : std::string();
}

// A body's container as a body's should be: TLD's own first-frame setup done (Container.Start rolls loot for an
// unsearched one and would put it back later), then nothing in it, searched, there for good (no spawn-chance roll)
// and saved with the scene.
void ready_container(Il2CppObject* c) {
    il2::invoke(api.container_start, c);
    il2::invoke(api.container_clear, c);
    put<bool>(c, api.inspected, true);
    put<bool>(c, api.rolled, true);
    put<bool>(c, api.not_populated, false);
    put<bool>(c, api.no_save, false);
}

// "who\nrecord\ncontents" (contents may be left out).
struct BodyText {
    std::string name, record, contents;
};

BodyText parse(const std::string& s) {
    BodyText b;
    const size_t first = s.find('\n');
    if (first == std::string::npos) return b;
    const size_t second = s.find('\n', first + 1);
    b.name = s.substr(0, first);
    b.record = s.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    if (second != std::string::npos) b.contents = s.substr(second + 1);
    return b;
}

std::string clean_name(std::string name) {
    for (char& c : name)
        if (c == '\n' || c == '\r') c = ' ';
    return name.empty() ? std::string("A survivor") : name;
}

std::vector<Il2CppObject*> pack_items(Il2CppObject* inv) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = inv ? get<Il2CppObject*>(inv, api.inv_items) : nullptr;
    if (!list) return out;
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* gi = *reinterpret_cast<Il2CppObject* const*>(reinterpret_cast<const char*>(items) + 0x20 +
                                                                   static_cast<size_t>(i) * kItemEntry);
        if (alive(gi)) out.push_back(gi);
    }
    return out;
}

// The player's water (TLD keeps it as two items in the pack, never moved): poured into new water items in the body,
// as Cheat Death's recovery camp does (LifeAfterDeathManager.AddWaterToRecovery). Litres moved, as units.
int64_t pour_water(Il2CppObject* c, Il2CppObject* supply, const std::string& name) {
    if (!alive(supply)) return 0;
    Il2CppObject* water = get<Il2CppObject*>(supply, api.gear_water);
    const int64_t units = water ? get<int64_t>(water, api.water_volume) : 0;
    if (units <= 0) return 0;
    void* args[] = {il2::il2cpp_string_new(name.c_str())};
    Il2CppObject* gi = il2::invoke(api.make_gear, nullptr, args);
    Il2CppObject* poured = alive(gi) ? get<Il2CppObject*>(gi, api.gear_water) : nullptr;
    if (!poured) return 0;
    put<int64_t>(poured, api.water_volume, units);
    void* add[] = {gi};
    il2::invoke(api.container_add, c, add);
    put<int64_t>(water, api.water_volume, 0);
    return units;
}

// Where we died: TLD's corpse, holding all we carried and wore. Its record goes to the others.
void leave_body() {
    const game::PlayerState me = game::read_player();
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    Il2CppObject* inv = il2::invoke(api.inventory, nullptr);
    if (!me.valid || !pm || !inv || !api.asset_ref_class) {
        logx::warn("death: no body left (not in a game)");
        return;
    }
    Il2CppObject* ref = il2::il2cpp_object_new(api.asset_ref_class);
    void* ref_args[] = {il2::il2cpp_string_new(kCorpseAsset)};
    il2::invoke(api.asset_ref_ctor, ref, ref_args);
    void* spawn_args[] = {ref};
    Il2CppObject* item = il2::invoke(api.spawn, nullptr, spawn_args);
    if (!alive(item)) {
        logx::warn("death: TLD couldn't make a body; what we carried stays with this character");
        return;
    }
    Vector3 at{me.position.x, me.position.y, me.position.z};
    const float yaw = me.body_yaw * 3.14159265f / 180.0f;
    Quaternion rot{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    Il2CppObject* tf = transform_of(item);
    set_vec(api.set_position, tf, at);
    void* rot_args[] = {&rot};
    il2::invoke(api.set_rotation, tf, rot_args);

    Il2CppObject* c = in_children(item, api.container_type);
    int moved = 0;
    int64_t water = 0;
    if (c) {
        ready_container(c);
        bool save_last = false;
        void* unequip_args[] = {&save_last};
        il2::invoke(api.unequip, pm, unequip_args);  // nothing in our hands (TLD's death does this next anyway)
        Il2CppObject* potable = il2::invoke(api.potable, inv);
        Il2CppObject* not_potable = il2::invoke(api.not_potable, inv);
        for (Il2CppObject* gi : pack_items(inv)) {
            if (gi == potable || gi == not_potable) continue;
            Il2CppObject* clothing = get<Il2CppObject*>(gi, api.gear_clothing);
            bool worn = false;
            if (alive(clothing) && il2::invoke_value(api.wearing, clothing, worn) && worn) {
                void* args[] = {gi};
                il2::invoke(api.take_off, pm, args);
            }
            void* add[] = {gi};
            il2::invoke(api.container_add, c, add);
            bool to_container = true;
            void* remove[] = {go_of(gi), &to_container};
            il2::invoke(api.inv_remove, inv, remove);
            ++moved;
        }
        water += pour_water(c, potable, api.potable_name);
        water += pour_water(c, not_potable, api.not_potable_name);
    } else {
        logx::warn("death: the body has no container; what we carried stays with this character");
    }

    const std::string guid = guid_of(item), name = clean_name(config::load().name);
    g_names[guid] = name;
    net::ObjectState s;
    s.kind = net::ObjectKind::Body;
    s.done = 1;
    s.id = guid;
    s.text = name + "\n" + record_of(item) + "\n" + contents_of(c);
    const bool with_contents = s.text.size() <= net::kBodyTextMax;
    if (!with_contents) s.text = name + "\n" + record_of(item) + "\n";  // what it holds goes as a container's change
    const bool told = world_objects::share(s);
    if (told && with_contents && c) world_containers::told(c);
    logx::info("death: our body lies at (%.1f, %.1f, %.1f) (%s), holding %d item(s) and %.2f L of water (%zu chars)%s",
               at.x, at.y, at.z, short_id(guid).c_str(), moved, static_cast<double>(water) / 1e6, s.text.size(),
               told ? "; told the others" : "");
}

// --- the screen after a death --------------------------------------------------------------------------------------

void load_bodies() {
    g_bodies.clear();
    const std::string ids = avatar::body_ids();
    for (size_t pos = 0; pos <= ids.size();) {
        const size_t comma = ids.find(", ", pos);
        std::string id = ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!id.empty() && id != "capsule") g_bodies.push_back(id);
        if (comma == std::string::npos) break;
        pos = comma + 2;
    }
    g_bodies.push_back("capsule");
    const std::string current = config::load().body;
    g_choice = 0;
    for (size_t i = 0; i < g_bodies.size(); ++i)
        if (g_bodies[i] == current) g_choice = i;
}

std::string chosen() { return g_bodies.empty() ? std::string("capsule") : g_bodies[g_choice % g_bodies.size()]; }

Il2CppObject* panel() {
    Il2CppObject* p = target(g_panel);
    return alive(p) ? p : nullptr;
}

// Our widgets on TLD's revive stage, made once per panel.
void make_widgets(Il2CppObject* p) {
    Il2CppObject* stage = child(transform_of(p), kStagePath);
    if (!stage) return;
    if (!alive(target(g_preview)) && api.texture_type && api.add_component && api.go_ctor) {
        drop(g_preview);
        Il2CppObject* go = il2::il2cpp_object_new(api.game_object_class);
        void* name_args[] = {il2::il2cpp_string_new("CoopNextSurvivor")};
        il2::invoke(api.go_ctor, go, name_args);
        int layer = 5;  // NGUI's
        il2::invoke_value(api.go_get_layer, go_of(stage), layer);
        set_int(api.go_set_layer, go, layer);
        bool keep_world = false;
        void* parent_args[] = {stage, &keep_world};
        il2::invoke(api.set_parent, il2::invoke(api.go_transform, go), parent_args);
        set_vec(api.set_local_position, il2::invoke(api.go_transform, go), {0, kPreviewY, 0});
        set_vec(api.set_local_scale, il2::invoke(api.go_transform, go), {1, 1, 1});
        void* type_args[] = {api.texture_type};
        if (Il2CppObject* widget = il2::invoke(api.add_component, go, type_args)) {
            set_int(api.widget_set_width, widget, kPreviewWidth);
            set_int(api.widget_set_height, widget, kPreviewHeight);
            set_int(api.widget_set_depth, widget, kPreviewDepth);
            g_preview = hold(widget);
        }
        g_preview_body.clear();
    }
    if (!alive(target(g_name_label))) {
        drop(g_name_label);
        Il2CppObject* cause = get<Il2CppObject*>(p, api.panel_cause);
        if (alive(cause) && api.instantiate) {
            void* args[] = {go_of(cause), stage};
            Il2CppObject* clone = il2::invoke(api.instantiate, nullptr, args);
            Il2CppObject* clone_tf = clone ? il2::invoke(api.go_transform, clone) : nullptr;
            set_vec(api.set_local_position, clone_tf, {0, kNameY, 0});
            Il2CppObject* name = in_children(clone_tf, api.label_type);
            if (name && api.widget_set_color) {
                float colour[4] = {0.82f, 0.82f, 0.82f, 1.0f};  // TLD's light grey (the cause stays red)
                void* colour_args[] = {colour};
                il2::invoke(api.widget_set_color, name, colour_args);
            }
            g_name_label = hold(name);
        }
    }
}

void show_choice() {
    const std::string body = chosen();
    set_text(target(g_name_label), "<   " + upper(coop_menu::body_name(body)) + "   >");
    if (body == g_preview_body) return;
    g_preview_body = body;
    Il2CppObject* widget = target(g_preview);
    if (!alive(widget)) return;
    Il2CppObject* texture = avatar::preview(body);
    void* args[] = {texture};
    il2::invoke(api.texture_set_main, widget, args);
    set_active(go_of(widget), texture != nullptr);
    logx::info("death: the next survivor shown: %s", body.c_str());
}

// TLD's revive stage made co-op's, every frame it's up (TLD's localisation puts its own words back when it shows).
void restyle(Il2CppObject* p) {
    make_widgets(p);
    // The buttons: TLD switches them off in a mode without Cheat Death (or with no lives left).
    if (Il2CppObject* buttons = get<Il2CppObject*>(p, api.panel_buttons); alive(buttons)) set_active(buttons, true);
    if (Il2CppObject* grid = get<Il2CppObject*>(p, api.panel_campfires); alive(grid)) set_active(go_of(grid), false);
    if (Il2CppObject* button = get<Il2CppObject*>(p, api.panel_button); alive(button) && api.widget_set_alpha) {
        float full = 1.0f;  // TLD dims CHEAT DEATH when no lives are left
        void* args[] = {&full};
        il2::invoke(api.widget_set_alpha, button, args);
    }
    Il2CppObject* tf = transform_of(p);
    for (const char* path : kLeftLabels) set_text(in_children(child(tf, path), api.label_type), "RESPAWN");
    for (const char* path : kRightLabels) set_text(in_children(child(tf, path), api.label_type), "NEXT SURVIVOR");
    if (Il2CppObject* go = go_of(target(g_preview)); go && !g_preview_body.empty()) set_active(go, true);
    set_active(go_of(target(g_name_label)), true);
    show_choice();
}

// Our widgets away (TLD's screen in a game without co-op, or after the respawn).
void hide_widgets() {
    set_active(go_of(target(g_preview)), false);
    set_active(go_of(target(g_name_label)), false);
    if (Il2CppObject* p = panel())
        if (Il2CppObject* grid = get<Il2CppObject*>(p, api.panel_campfires); alive(grid)) set_active(go_of(grid), true);
    g_preview_body.clear();
    avatar::end_preview();
}

bool on_revive_stage(Il2CppObject* p) { return p && get<int32_t>(p, api.panel_stage) == kReviveStage; }

// --- the respawn ---------------------------------------------------------------------------------------------------

// The afflictions TLD's own respawn cures (LifeAfterDeathManager.HandlePlayerState), and Cheat Death's own, without
// their effects.
void cure_all() {
    struct Cure {
        const char* field;
        const char* method;
        int args;  // 0; 1: bool nofx (true); 2: AfflictionOptions (None)
    };
    static const Cure kCures[] = {
        {"m_BloodLoss", "Cure", 0},          {"m_Burns", "BurnsEnd", 1},           {"m_BurnsElectric", "BurnsElectricEnd", 1},
        {"m_BrokenRib", "Cure", 0},          {"m_CabinFever", "CabinFeverEnd", 0}, {"m_CheatDeathAffliction", "Cure", 2},
        {"m_DiminishedState", "Cure", 2},    {"m_Dysentery", "DysenteryEnd", 1},   {"m_FoodPoisoning", "FoodPoisoningEnd", 1},
        {"m_Frostbite", "FrostbiteEnd", 0},  {"m_Hypothermia", "HypothermiaEnd", 1}, {"m_Infection", "Cure", 0},
        {"m_InfectionRisk", "Cure", 0},      {"m_IntestinalParasites", "IntestinalParasitesEnd", 1},
        {"m_SprainedAnkle", "Cure", 0},      {"m_SprainedWrist", "Cure", 0},       {"m_SprainPain", "Cure", 0},
        {"m_WellFed", "WellFedEnd", 1},      {"m_SevereLacerations", "Cure", 2},   {"m_InsomniaManager", "RemoveInsomniaAll", 0},
        {"m_ScurvyManager", "Cure", 0},
    };
    int cured = 0;
    for (const Cure& c : kCures) {
        Il2CppObject* o = game_static(c.field);
        const MethodInfo* m = o ? il2::find_method(il2::il2cpp_object_get_class(o), c.method, c.args ? 1 : 0) : nullptr;
        if (!m) {
            logx::warn("death: can't cure %s (%s)", c.field + 2, o ? "no such method" : "not there");
            continue;
        }
        bool nofx = true;
        int32_t none = 0;
        void* args[] = {c.args == 1 ? static_cast<void*>(&nofx) : static_cast<void*>(&none)};
        il2::invoke(m, o, c.args ? args : nullptr);
        ++cured;
    }
    logx::info("death: %d affliction(s) cured", cured);
}

// A new survivor's body: full condition, fed, rested, warm and unskilled.
void fresh_body() {
    if (Il2CppObject* condition = game_static("m_Condition")) put<float>(condition, api.hp, get<float>(condition, api.max_hp));
    if (Il2CppObject* hunger = game_static("m_Hunger")) put<float>(hunger, api.calories, get<float>(hunger, api.start_calories));
    if (Il2CppObject* thirst = game_static("m_Thirst")) put<float>(thirst, api.thirst, 0.0f);
    if (Il2CppObject* fatigue = game_static("m_Fatigue")) put<float>(fatigue, api.fatigue, 0.0f);
    if (Il2CppObject* freezing = game_static("m_Freezing")) put<float>(freezing, api.freezing, 0.0f);
    int skills = 0;
    if (Il2CppObject* manager = game_static("m_SkillsManager")) {
        Il2CppObject* list = get<Il2CppObject*>(manager, api.skills);
        Il2CppObject* items = list ? get<Il2CppObject*>(list, kListItems) : nullptr;
        const int32_t count = list ? get<int32_t>(list, kListSize) : 0;
        for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
            if (Il2CppObject* skill = il2::array_object_at(items, static_cast<size_t>(i))) {
                put<int32_t>(skill, api.skill_points, 0);
                ++skills;
            }
    }
    logx::info("death: a fresh survivor: full condition, fed, rested and warm; %d skill(s) back to none", skills);
}

// What TLD's death switched off on the player: its sounds (HandlePlayerDeath mutes every AudioCallback under it).
void sounds_back() {
    Il2CppObject* tf = il2::invoke(api.player_transform, nullptr);
    Il2CppObject* parent = tf ? il2::invoke(api.get_parent, tf) : nullptr;
    if (!parent || !api.audio_type || !api.audio_toggle) return;
    bool inactive_too = true;
    void* args[] = {api.audio_type, &inactive_too};
    Il2CppObject* found = il2::invoke(api.go_components, go_of(parent), args);
    for (size_t i = 0; i < il2::array_length(found); ++i) {
        bool off = false;
        void* toggle[] = {&off};
        il2::invoke(api.audio_toggle, il2::array_object_at(found, i), toggle);
    }
}

// Beside another living player in our scene; a friend with nobody here goes to the host; otherwise where we died.
std::string go_somewhere() {
    const std::string here = game::active_scene();
    for (const net::StateMsg& s : netrt::remote_states()) {
        if (!s.valid || s.scene != here || (s.flags & net::kStateDead)) continue;
        if (world_join::stand_beside(s)) return "beside player " + std::to_string(s.player);
    }
    if (!hosting()) {
        const auto host = netrt::remote_state(0);
        if (host && host->valid && !(host->flags & net::kStateDead) && host->scene != here) {
            world_join::go_to_host();
            return "to the host, in " + host->scene;
        }
    }
    return "where we died";
}

bool do_respawn(const std::string& wanted) {
    if (!g_dead) {
        logx::warn("death: respawn: we aren't dead");
        return false;
    }
    if (g_bodies.empty()) load_bodies();
    const std::string body = wanted.empty() ? chosen() : wanted;
    if (!avatar::is_known_body(body)) {
        logx::warn("death: respawn: no body '%s' (choose from: %s)", body.c_str(), avatar::body_ids().c_str());
        return false;
    }
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    if (!pm) return false;
    if (Il2CppObject* p = panel()) {
        bool off = false;
        void* args[] = {&off};
        il2::invoke(api.panel_enable, p, args);
    }
    hide_widgets();
    cure_all();
    fresh_body();
    put<bool>(pm, api.start_gear_applied, false);  // PlayerManager.Update gives the experience mode's start gear
    sounds_back();
    int32_t normal = kNormal;
    void* mode_args[] = {&normal};
    il2::invoke(api.set_mode, pm, mode_args);
    g_dead = false;  // (before moving: our state says alive again)
    const std::string where = go_somewhere();
    if (api.fade_in) {
        float seconds = 1.5f, delay = 0.0f;
        void* args[] = {&seconds, &delay, nullptr};
        il2::invoke(api.fade_in, nullptr, args);
    }
    netrt::set_body(body);
    config::Settings s = config::load();
    s.body = body;
    config::save(s);
    g_save_at = now_ms() + kSaveAfterMs;
    coop_notices::tell("A new survivor: " + coop_menu::body_name(body));
    logx::info("death: respawned as %s, %s; the start gear comes next, the game saves in %.0f s", body.c_str(),
               where.c_str(), kSaveAfterMs / 1000);
    return true;
}

// --- hooks ---------------------------------------------------------------------------------------------------------

using DeathFn = void (*)(Il2CppString* cause, const MethodInfo* method);
using DeleteFn = void (*)(uint32_t game_id, const MethodInfo* method);
using PanelFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using PanelBoolFn = void (*)(Il2CppObject* self, bool value, const MethodInfo* method);
DeathFn g_death = nullptr;
DeleteFn g_delete = nullptr;
PanelFn g_left = nullptr, g_right = nullptr, g_back = nullptr;
PanelBoolFn g_to_log = nullptr, g_enable = nullptr;

void death_detour(Il2CppString* cause, const MethodInfo* method) {
    if (g_ready && coop() && !g_dead) {
        g_cause = text(cause);
        logx::info("death: we died%s%s; leaving a body, then TLD's screen after a death (co-op's)",
                   g_cause.empty() ? "" : ": ", g_cause.c_str());
        leave_body();
        g_dead = true;
        load_bodies();
    }
    g_death(cause, method);
}

void delete_detour(uint32_t game_id, const MethodInfo* method) {
    if (g_ready && coop()) {
        logx::info("death: TLD would delete this save (game %u); kept (co-op: the world goes on)", game_id);
        return;
    }
    g_delete(game_id, method);
}

// Co-op's revive stage: RESPAWN, NEXT SURVIVOR; nothing goes back (TLD's back button) or on to TLD's later stages.
bool ours(Il2CppObject* self) {
    if (!g_ready || !g_dead || !coop()) return false;
    if (target(g_panel) != self) {
        drop(g_panel);
        g_panel = hold(self);
    }
    return on_revive_stage(self);
}

void left_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!ours(self)) return g_left(self, method);
    do_respawn("");
}

void right_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!ours(self)) return g_right(self, method);
    next_survivor(1);
}

void back_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!ours(self)) g_back(self, method);
}

// TLD goes on to its end-of-game log when the mode has no lives left (or after END GAME): never while co-op's.
void to_log_detour(Il2CppObject* self, bool fast, const MethodInfo* method) {
    if (g_ready && g_dead && coop()) {
        logx::info("death: TLD would end the game (its log); co-op's screen stays");
        return;
    }
    g_to_log(self, fast, method);
}

void enable_detour(Il2CppObject* self, bool enable, const MethodInfo* method) {
    if (g_ready && enable && g_dead && coop()) {
        drop(g_panel);
        g_panel = hold(self);
    }
    g_enable(self, enable, method);
}

}  // namespace

bool dead() { return g_dead; }

std::vector<net::ObjectState> bodies_here() {
    std::vector<net::ObjectState> out;
    if (!g_ready) return out;
    for (Il2CppObject* item : bodies()) {
        net::ObjectState s;
        s.kind = net::ObjectKind::Body;
        s.done = 1;
        s.id = guid_of(item);
        const auto name = g_names.find(s.id);
        s.text = (name != g_names.end() ? name->second : std::string("A survivor")) + "\n" + record_of(item) + "\n";
        out.push_back(std::move(s));
    }
    return out;
}

std::string body_name(const net::ObjectState& s) { return parse(s.text).name; }

bool apply_body(const net::ObjectState& s) {
    if (!g_ready) return false;
    const BodyText b = parse(s.text);
    if (!b.name.empty()) g_names[s.id] = b.name;
    if (body_by_guid(s.id)) return false;
    if (b.record.empty()) {
        logx::warn("death: %s's body (%s) came without its record", b.name.c_str(), short_id(s.id).c_str());
        return false;
    }
    const std::string list = "[" + b.record + "]";
    void* args[] = {il2::il2cpp_string_new(list.c_str())};
    il2::invoke(api.load_items, nullptr, args);
    Il2CppObject* item = body_by_guid(s.id);
    if (!item) {
        logx::warn("death: TLD couldn't make %s's body (%s)", b.name.c_str(), short_id(s.id).c_str());
        return false;
    }
    if (Il2CppObject* c = in_children(item, api.container_type)) {
        ready_container(c);
        if (!b.contents.empty()) {
            world_containers::fill(c, b.contents);
            put<bool>(c, api.inspected, true);  // (its record may say otherwise)
            put<bool>(c, api.rolled, true);
            put<bool>(c, api.not_populated, false);
        }
    }
    const Vector3 at = position_of(item);
    logx::info("death: %s's body is here now, at (%.1f, %.1f, %.1f) (%s)%s", b.name.c_str(), at.x, at.y, at.z,
               short_id(s.id).c_str(), b.contents.empty() ? "; what it holds comes when it's opened" : "");
    return true;
}

bool respawn(const std::string& body) { return do_respawn(body); }

bool next_survivor(int step) {
    if (!g_dead) return false;
    if (g_bodies.empty()) load_bodies();
    const int n = static_cast<int>(g_bodies.size());
    g_choice = static_cast<size_t>(((static_cast<int>(g_choice) + step) % n + n) % n);
    show_choice();
    logx::info("death: next survivor: %s", chosen().c_str());
    return true;
}

bool set_lives(int lives) {
    Il2CppObject* lad = game_static("m_LifeAfterDeathManager");
    if (!lad || api.lad_tunables < 0) return false;
    put<int32_t>(lad, api.lad_tunables, lives);  // LifeAfterDeathTunables.m_AvailableLives, its first field
    logx::info("dev: lives: the experience mode gives %d now (this game only)", lives);
    return true;
}

void describe() {
    int32_t mode = -1;
    if (Il2CppObject* pm = il2::invoke(api.player_manager, nullptr)) mode = get<int32_t>(pm, api.control_mode);
    Il2CppObject* condition = game_static("m_Condition");
    logx::info("dev: bodies: we are %s%s%s (control mode %d, condition %.0f of %.0f); %zu body(ies) here",
               g_dead ? "dead" : "alive", g_dead ? ", choosing: " : "", g_dead ? chosen().c_str() : "", mode,
               condition ? get<float>(condition, api.hp) : -1.0f, condition ? get<float>(condition, api.max_hp) : -1.0f,
               bodies().size());
    for (Il2CppObject* item : bodies()) {
        const std::string guid = guid_of(item);
        const auto name = g_names.find(guid);
        const Vector3 at = position_of(item);
        Il2CppObject* c = in_children(item, api.container_type);
        logx::info("dev:   %s %s at (%.1f, %.1f, %.1f), container %s: %zu chars", short_id(guid).c_str(),
                   name != g_names.end() ? name->second.c_str() : "(whose unknown)", at.x, at.y, at.z,
                   c ? name_of(c).c_str() : "NONE", contents_of(c).size());
    }
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    if (!in_game) {
        if (g_dead && scene == "MainMenu") {
            g_dead = false;  // left the game while dead
            hide_widgets();
        }
        g_others_dead.clear();
        return;
    }
    if (g_save_at && now_ms() >= g_save_at) {
        g_save_at = 0;
        if (api.force_save) il2::invoke(api.force_save, nullptr);
        logx::info("death: the new survivor saved");
    }
    // Who else died: said once each time.
    const net::Status st = coop() ? netrt::status() : net::Status{};
    for (const net::StateMsg& s : coop() ? netrt::remote_states() : std::vector<net::StateMsg>{}) {
        const bool is_dead = (s.flags & net::kStateDead) != 0;
        bool& was = g_others_dead[s.player];
        if (is_dead && !was) {
            std::string who = "A player";
            for (const net::PlayerInfo& p : st.players)
                if (p.id == s.player && !p.name.empty()) who = p.name;
            coop_notices::tell(who + " died. Their body lies where they fell.");
            logx::info("death: %s (player %u) died", who.c_str(), s.player);
        } else if (!is_dead && was) {
            logx::info("death: player %u is back, a new survivor", s.player);
        }
        was = is_dead;
    }
    if (!g_dead) return;
    Il2CppObject* p = panel();
    if (!coop() || !p || !active(go_of(p)) || !on_revive_stage(p)) return;
    restyle(p);
    if (pressed(0, VK_RIGHT) || pressed(1, 'D')) next_survivor(1);
    if (pressed(2, VK_LEFT) || pressed(3, 'A')) next_survivor(-1);
}

bool init() {
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    api.game_object_class = game_object;
    api.go_ctor = il2::find_method_params(game_object, ".ctor", {"System.String"});
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.go_active = il2::find_method(game_object, "get_activeInHierarchy", 0);
    api.go_get_layer = il2::find_method(game_object, "get_layer", 0);
    api.go_set_layer = il2::find_method(game_object, "set_layer", 1);
    api.add_component = il2::find_method_params(game_object, "AddComponent", {"System.Type"});
    api.go_components = il2::find_method_params(game_object, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    api.in_children = il2::find_method_params(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    api.instantiate = il2::find_method_params(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
    api.object_name = il2::find_method(object, "get_name", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.set_rotation = il2::find_method(transform, "set_rotation", 1);
    api.set_local_position = il2::find_method(transform, "set_localPosition", 1);
    api.set_local_scale = il2::find_method(transform, "set_localScale", 1);
    api.set_parent = il2::find_method(transform, "SetParent", 2);
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    api.find_child = il2::find_method_params(transform, "Find", {"System.String"});

    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    Il2CppClass* widget = il2::find_class(kGame, "", "UIWidget");
    api.label_set_text = il2::find_method(label, "set_text", 1);
    if (label) {
        api.label_type = il2::type_object(label);
        if (api.label_type) il2::il2cpp_gchandle_new(api.label_type, false);
    }
    api.widget_set_alpha = il2::find_method(widget, "set_alpha", 1);
    api.widget_set_color = il2::find_method(widget, "set_color", 1);
    api.widget_set_width = il2::find_method(widget, "set_width", 1);
    api.widget_set_height = il2::find_method(widget, "set_height", 1);
    api.widget_set_depth = il2::find_method(widget, "set_depth", 1);
    if (Il2CppClass* ui_texture = il2::find_class(kGame, "", "UITexture")) {
        api.texture_type = il2::type_object(ui_texture);
        if (api.texture_type) il2::il2cpp_gchandle_new(api.texture_type, false);
        api.texture_set_main = il2::find_method(ui_texture, "set_mainTexture", 1);
    }

    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* inventory = il2::find_class(kGame, "", "Inventory");
    Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
    api.game_manager = game_manager;
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    api.player_transform = il2::find_method(game_manager, "GetPlayerTransform", 0);
    api.force_save = il2::find_method(game_manager, "ForceSaveGame", 0);
    api.handle_death = il2::find_method(game_manager, "HandlePlayerDeath", 1);
    api.unequip = il2::find_method(player_manager, "UnequipImmediate", 1);
    api.set_mode = il2::find_method(player_manager, "SetControlMode", 1);
    api.take_off = il2::find_method(player_manager, "TakeOffClothingItem", 1);
    api.start_gear_applied = il2::field_offset(player_manager, "m_StartGearApplied");
    api.control_mode = il2::field_offset(player_manager, "m_ControlMode");
    api.inv_items = il2::field_offset(inventory, "m_Items");
    api.inv_remove = il2::find_method_params(inventory, "RemoveGear", {"UnityEngine.GameObject", "System.Boolean"});
    api.potable = il2::find_method(inventory, "GetPotableWaterSupply", 0);
    api.not_potable = il2::find_method(inventory, "GetNonPotableWaterSupply", 0);
    api.gear_clothing = il2::field_offset(gear, "m_ClothingItem");
    api.gear_water = il2::field_offset(gear, "m_WaterSupply");
    api.water_volume = il2::field_offset(il2::find_class(kGame, "", "WaterSupply"), "m_VolumeInLiters");
    api.wearing = il2::find_method(il2::find_class(kGame, "", "ClothingItem"), "IsWearing", 0);
    api.make_gear = il2::find_method_params(gear, "InstantiateGearItem", {"System.String"});
    if (Il2CppClass* lad = il2::find_class(kGame, "TLD.Gameplay", "LifeAfterDeathManager")) {
        api.lad_tunables = il2::field_offset(lad, "m_Tunables");
        // Cheat Death's names for the water items (constants, read from the game's metadata).
        for (auto [field, out] : {std::pair{"POTABLE_WATER_SUPPLY_NAME", &api.potable_name},
                                  std::pair{"NONPOTABLE_WATER_SUPPLY_NAME", &api.not_potable_name}}) {
            Il2CppString* value = nullptr;
            if (FieldInfo* f = il2::il2cpp_class_get_field_from_name(lad, field)) il2::il2cpp_field_static_get_value(f, &value);
            if (value) *out = text(value);
        }
    }
    if (Il2CppClass* audio = il2::find_class(kGame, "", "AudioCallback")) {
        api.audio_type = il2::type_object(audio);
        if (api.audio_type) il2::il2cpp_gchandle_new(api.audio_type, false);
        api.audio_toggle = il2::find_method(audio, "ToggleDisableAudioEvents", 1);
    }
    api.fade_in = il2::find_method(il2::find_class(kGame, "", "CameraFade"), "FadeIn", 3);
    api.hp = il2::field_offset(il2::find_class(kGame, "", "Condition"), "m_CurrentHP");
    api.max_hp = il2::field_offset(il2::find_class(kGame, "", "Condition"), "m_MaxHP");
    api.calories = il2::field_offset(il2::find_class(kGame, "", "Hunger"), "m_CurrentReserveCalories");
    api.start_calories = il2::field_offset(il2::find_class(kGame, "", "Hunger"), "m_StartingReserveCalories");
    api.thirst = il2::field_offset(il2::find_class(kGame, "", "Thirst"), "m_CurrentThirst");
    api.fatigue = il2::field_offset(il2::find_class(kGame, "", "Fatigue"), "m_CurrentFatigue");
    api.freezing = il2::field_offset(il2::find_class(kGame, "", "Freezing"), "m_CurrentFreezing");
    api.skills = il2::field_offset(il2::find_class(kGame, "", "SkillsManager"), "m_Skills");
    api.skill_points = il2::field_offset(il2::find_class(kGame, "", "Skill"), "m_CurrentPoints");

    Il2CppClass* manager = il2::find_class(kGame, "TLD.Gameplay", "CustomSpawnedItemManager");
    Il2CppClass* item = il2::find_class(kGame, "TLD.Gameplay", "CustomSpawnedItem");
    api.spawn = il2::find_method(manager, "SpawnBigItem", 1);
    api.load_items = il2::find_method(manager, "DeserializeAll", 1);
    if (manager) api.spawned = il2::il2cpp_class_get_field_from_name(manager, "s_Items");
    api.asset_ref_class = il2::find_class(kGame, "TLD.Gameplay", "AssetReferenceCustomSpawnedItem");
    api.asset_ref_ctor = il2::find_method_params(api.asset_ref_class, ".ctor", {"System.String"});
    api.item_save = il2::find_method(item, "Serialize", 0);
    api.item_guid = il2::find_method(item, "get_Guid", 0);
    api.item_address = il2::find_method(item, "get_Address", 0);
    api.asset_guid = il2::field_offset(il2::find_class("Unity.Addressables.dll", "UnityEngine.AddressableAssets", "AssetReference"),
                                       "m_AssetGUID");
    api.to_json = il2::find_method(il2::find_class("TLD.Serialization.dll", "TLD.Serialization", "SerializationUtils"),
                                   "SerializeObject", 1);
    Il2CppClass* container = il2::find_class(kGame, "", "Container");
    if (container) {
        api.container_type = il2::type_object(container);
        if (api.container_type) il2::il2cpp_gchandle_new(api.container_type, false);
    }
    api.container_start = il2::find_method(container, "Start", 0);
    api.container_clear = il2::find_method(container, "DestroyAllGear", 0);
    api.container_add = il2::find_method(container, "AddGear", 1);
    api.container_save = il2::find_method(container, "Serialize", 0);
    api.container_load = il2::find_method(container, "Deserialize", 2);
    api.inspected = il2::field_offset(container, "m_Inspected");
    api.rolled = il2::field_offset(container, "m_RolledSpawnChance");
    api.not_populated = il2::field_offset(container, "m_NotPopulated");
    api.no_save = il2::field_offset(container, "m_DisableSerialization");

    Il2CppClass* panel_class = il2::find_class(kGame, "", "Panel_LifeAfterDeath");
    api.panel_enable = il2::find_method(panel_class, "Enable", 1);
    api.panel_left = il2::find_method(panel_class, "HandleOnLeftButtonPressed", 0);
    api.panel_right = il2::find_method(panel_class, "HandleOnRightButtonPressed", 0);
    api.panel_back = il2::find_method(panel_class, "HandleOnBackButtonPressed", 0);
    api.panel_to_log = il2::find_method(panel_class, "ContinueToLogPanel", 1);
    api.panel_stage = il2::field_offset(panel_class, "m_CurrentStage");
    api.panel_button = il2::field_offset(panel_class, "m_CheatDeathButtonWidget");
    api.panel_cause = il2::field_offset(panel_class, "m_CauseOfDeathLabel");
    api.panel_campfires = il2::field_offset(panel_class, "m_CampfireGrid");
    api.panel_buttons = il2::field_offset(panel_class, "m_ReviveInteractions");
    api.delete_saves = il2::find_method(il2::find_class(kGame, "", "SaveGameSystem"), "DeleteSaveFilesForGameId", 1);

    const bool body = api.spawn && api.load_items && api.spawned && api.asset_ref_ctor && api.item_save && api.item_guid &&
                      api.item_address && api.asset_guid >= 0 && api.to_json && api.container_type && api.container_start &&
                      api.container_clear && api.container_add && api.container_save && api.container_load &&
                      api.inspected >= 0 && api.rolled >= 0 && api.not_populated >= 0 && api.no_save >= 0 &&
                      api.inv_items >= 0 && api.inv_remove && api.potable && api.not_potable && api.gear_clothing >= 0 &&
                      api.gear_water >= 0 && api.water_volume >= 0 && api.wearing && api.make_gear && api.take_off &&
                      api.unequip && api.in_children;
    const bool revive = api.player_manager && api.inventory && api.player_transform && api.set_mode &&
                         api.start_gear_applied >= 0 && api.hp >= 0 && api.max_hp >= 0 && api.calories >= 0 &&
                         api.start_calories >= 0 && api.thirst >= 0 && api.fatigue >= 0 && api.freezing >= 0 &&
                         api.skills >= 0 && api.skill_points >= 0 && api.panel_enable && api.panel_stage >= 0 &&
                         api.panel_button >= 0 && api.panel_cause >= 0 && api.panel_campfires >= 0 &&
                         api.panel_buttons >= 0 && api.find_child &&
                         api.label_set_text && api.label_type && api.go_set_active && api.go_active;
    g_ready = body && revive;
    if (!g_ready) {
        logx::warn("death: UNAVAILABLE (TLD API missing: %s%s)", body ? "" : "the body", revive ? "" : " the respawn");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    g_ready = hook("GameManager.HandlePlayerDeath", api.handle_death, &death_detour, &g_death) &&
              hook("SaveGameSystem.DeleteSaveFilesForGameId", api.delete_saves, &delete_detour, &g_delete) &&
              hook("Panel_LifeAfterDeath.Enable", api.panel_enable, &enable_detour, &g_enable) &&
              hook("Panel_LifeAfterDeath.HandleOnLeftButtonPressed", api.panel_left, &left_detour, &g_left) &&
              hook("Panel_LifeAfterDeath.HandleOnRightButtonPressed", api.panel_right, &right_detour, &g_right) &&
              hook("Panel_LifeAfterDeath.HandleOnBackButtonPressed", api.panel_back, &back_detour, &g_back) &&
              hook("Panel_LifeAfterDeath.ContinueToLogPanel", api.panel_to_log, &to_log_detour, &g_to_log);
    logx::info("death: %s", g_ready ? "ready (a death in co-op leaves a body; the player comes back as a new survivor)"
                                    : "UNAVAILABLE (hooks failed)");
    return g_ready;
}

}  // namespace coop_death
