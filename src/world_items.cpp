#include "world_items.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coop_notices.h"
#include "core.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/claims.h"
#include "net/uniques.h"
#include "net_runtime.h"

namespace world_items {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr double kScanMs = 500;        // loose items are checked twice a second
constexpr double kSettleMs = 1000;     // an item has to stop moving this long before the other player hears where it is
constexpr float kStillMetres = 0.02f;  // moving less than this between scans: still
constexpr float kMovedMetres = 0.3f;   // closer than this to where the other player has it: the same place
constexpr double kListGraceMs = 5000;  // client: our own changes this recent aren't undone by the host's list
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr size_t kInventoryEntry = 16;  // TLD.Gear.GearItemObject in an array: GearItem m_GearItem, string m_GearItemName

struct Vector3 {
    float x, y, z;
};
struct Quaternion {
    float x, y, z, w;
};

struct Api {
    // Unity
    const MethodInfo* component_go = nullptr;
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* go_active = nullptr;     // GameObject.get_activeInHierarchy
    const MethodInfo* go_component = nullptr;  // GameObject.GetComponent(Type)
    const MethodInfo* get_position = nullptr;
    const MethodInfo* get_rotation = nullptr;
    const MethodInfo* get_parent = nullptr;
    const MethodInfo* set_pose = nullptr;      // Transform.SetPositionAndRotation(Vector3, Quaternion)
    const MethodInfo* object_name = nullptr;
    // GearItem
    Il2CppObject* gear_type = nullptr;
    ptrdiff_t in_inventory = -1, no_save = -1, object_guid = -1, radial_parent = -1;
    ptrdiff_t narrative = -1;                      // m_NarrativeCollectibleItem: a note, key, postcard... (one of a kind)
    const MethodInfo* create_save_data = nullptr;  // GearItem.CreateSaveData(): TLD's save record for it
    ptrdiff_t sd_prefab = -1, sd_gear = -1;        // GearSaveData.m_PrefabName / m_SearializedGear
    const MethodInfo* instantiate = nullptr;       // static GearItem.InstantiateAndDeserializeGearItem(string, string, Transform, bool, bool)
    const MethodInfo* sanitize = nullptr;          // static Utils.SanitizePrefabName(string)
    const MethodInfo* drop = nullptr;              // GearItem.Drop(int, bool, bool, bool)
    const MethodInfo* display_name = nullptr;      // static GearItem.GetGearDisplayName(string prefab)
    ptrdiff_t stackable = -1, units = -1;          // GearItem.m_StackableItem, StackableItem.m_Units
    // ObjectGuid / PdidTable
    ptrdiff_t guid = -1;                           // ObjectGuid.m_Guid (legacy)
    ptrdiff_t cached_pdid = -1;                    // PdidObjectBase.m_RuntimeCachedPdid (what get_PDID returns)
    const MethodInfo* guid_of_go = nullptr;        // static ObjectGuid.GetObjectGuidFromGO(GameObject)
    const MethodInfo* attach_guid = nullptr;       // static ObjectGuid.MaybeAttachObjectGuidAndRegister(GameObject, string)
    const MethodInfo* pdid_object = nullptr;       // static PdidTable.GetGameObject(string)
    const MethodInfo* new_pdid = nullptr;          // static PdidTable.GenerateNewID()
    // GearManager
    FieldInfo* all_gear = nullptr;                 // static GearManager.m_Gear
    const MethodInfo* destroy_gear = nullptr;      // static GearManager.DestroyGearObject(GameObject)
    const MethodInfo* serialize_all = nullptr;     // static GearManager.Serialize()
    Il2CppObject* held_by = nullptr;               // gear whose parent has this component isn't loose (see resolve_held_by)
    bool held_by_resolved = false;
    // Player
    const MethodInfo* player_manager = nullptr;    // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* inventory = nullptr;         // static GameManager.GetInventoryComponent()
    const MethodInfo* inspected = nullptr;         // PlayerManager.GearItemBeingInspected() (the last one: check
    const MethodInfo* inspecting = nullptr;        // PlayerManager.IsInspectModeActive() first)
    const MethodInfo* put_back = nullptr;          // PlayerManager.ExitInspectGearMode(bool): what "Leave it" calls
    ptrdiff_t pickup_action = -1, put_back_action = -1;  // PlayerManager.m_PickupAction / m_PutBackAction (Action)
    ptrdiff_t placing = -1;                        // PlayerManager.m_ObjectToPlaceGearItem
    const MethodInfo* pickup = nullptr;            // PlayerManager.ProcessPickupItemInteraction(GearItem, bool, bool, bool)
    const MethodInfo* inspect = nullptr;           // PlayerManager.EnterInspectGearMode(GearItem)
    ptrdiff_t inv_items = -1;                      // Inventory.m_Items (List<GearItemObject>)
    const MethodInfo* inv_destroy = nullptr;       // Inventory.DestroyGear(GearItem)
    const MethodInfo* remove_units = nullptr;      // Inventory.RemoveUnits(GearItem, int)
    // SnareItem
    ptrdiff_t snare = -1;                          // GearItem.m_SnareItem
    ptrdiff_t snare_state = -1;                    // SnareItem.m_State (SnareState: 0 not set, 1 set, 2 broken, 3 caught)
    ptrdiff_t snare_rabbit = -1;                   // SnareItem.m_RabbitPrefab
    const MethodInfo* snare_load = nullptr;        // SnareItem.Deserialize(string)
    const MethodInfo* snare_placed = nullptr;      // SnareItem.OnSnarePlaced(): set, rolling from now
    const MethodInfo* snare_set_state = nullptr;   // SnareItem.SetState(SnareState)
    const MethodInfo* snare_roll = nullptr;        // SnareItem.DoRoll()
    const MethodInfo* snare_spawn_rabbit = nullptr;  // SnareItem.SpawnDeadRabbitOnSnare(GameObject)
    // CookingPotItem (a pot, or TLD's dummy pot under food cooked without one)
    ptrdiff_t pot = -1;                            // GearItem.m_CookingPotItem
    ptrdiff_t pot_fire = -1;                       // CookingPotItem.m_FireBeingUsed
    ptrdiff_t pot_food = -1;                       // CookingPotItem.m_GearItemBeingCooked
    ptrdiff_t pot_hours = -1;                      // CookingPotItem.m_CookingElapsedHours
    ptrdiff_t pot_state = -1;                      // CookingPotItem.m_CookingState (0 cooking, 1 ready, 2 ruined)
    ptrdiff_t pot_cooked = -1, pot_ruined = -1;    // CookingPotItem.m_PercentCooked, m_PercentRuined
    const MethodInfo* pot_take = nullptr;          // CookingPotItem.PickUpCookedItem()
    const MethodInfo* pot_cook = nullptr;          // CookingPotItem.StartCooking(GearItem)
    const MethodInfo* pot_boil = nullptr;          // CookingPotItem.StartBoilingWater(ItemLiquidVolume, bool doWaterRemoval)
    const MethodInfo* pot_melt = nullptr;          // CookingPotItem.StartMeltingSnow(ItemLiquidVolume)
    const MethodInfo* pot_dummy = nullptr;         // CookingPotItem.IsDummyPot()
    ptrdiff_t pot_water = -1, pot_snow = -1;       // CookingPotItem.m_LitersWaterBeingBoiled, m_LitersSnowBeingMelted
    const MethodInfo* pot_load = nullptr;          // CookingPotItem.Deserialize(string)
    const MethodInfo* pot_drop_food = nullptr;     // CookingPotItem.DestroyGearItemBeingCooked()
} api;

bool g_ready = false;

double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

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
    return o ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, o))) : std::string();
}
std::string short_id(const std::string& pdid) { return pdid.size() > 8 ? pdid.substr(0, 8) : pdid; }

float distance(const Vector3& a, const Vector3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

bool contains_nocase(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return needle.empty() || it != hay.end();
}

// TLD's persistent id for a GearItem: what ObjectGuid.GetGuidFromGameObject returns (the virtual get_PDID, i.e.
// PdidObjectBase.m_RuntimeCachedPdid), with the legacy ObjectGuid.m_Guid as a fallback.
std::string pdid_of(Il2CppObject* gi) {
    Il2CppObject* og = get<Il2CppObject*>(gi, api.object_guid);
    if (!alive(og)) {
        void* args[] = {il2::invoke(api.component_go, gi)};
        og = args[0] ? il2::invoke(api.guid_of_go, nullptr, args) : nullptr;
    }
    if (!alive(og)) return {};
    std::string id = text(get<Il2CppString*>(og, api.cached_pdid));
    return id.empty() ? text(get<Il2CppString*>(og, api.guid)) : id;
}

Il2CppObject* transform_of(Il2CppObject* component) { return il2::invoke(api.component_transform, component); }

void pose_of(Il2CppObject* component, Vector3& pos, Quaternion& rot) {
    Il2CppObject* tf = transform_of(component);
    il2::invoke_value(api.get_position, tf, pos);
    il2::invoke_value(api.get_rotation, tf, rot);
}

void set_pose(Il2CppObject* component, Vector3 pos, Quaternion rot) {
    void* args[] = {&pos, &rot};
    il2::invoke(api.set_pose, transform_of(component), args);
}

Il2CppObject* gear_on(Il2CppObject* go) {
    if (!alive(go)) return nullptr;
    void* args[] = {api.gear_type};
    Il2CppObject* gi = il2::invoke(api.go_component, go, args);
    return alive(gi) ? gi : nullptr;
}

// What's registered under this persistent id here (any object), and its GearItem if it has one.
Il2CppObject* object_by_pdid(const std::string& pdid) {
    if (pdid.empty()) return nullptr;
    void* args[] = {il2::il2cpp_string_new(pdid.c_str())};
    Il2CppObject* go = il2::invoke(api.pdid_object, nullptr, args);
    return alive(go) ? go : nullptr;
}

// GearManager.Serialize skips gear whose parent has one particular component (gear held by something else). The
// component is a generic GetComponent<T>() whose MethodInfo the method loads right before the call
// (mov rdx, [rip+x]; mov rcx, rax; call); T is its return type. IL2CPP fills that slot the first time the method runs,
// so it is run once here (it only builds a string).
void resolve_held_by() {
    api.held_by_resolved = true;
    const double t0 = now_ms();
    Il2CppString* all = reinterpret_cast<Il2CppString*>(il2::invoke(api.serialize_all, nullptr));
    const double ms = now_ms() - t0;
    const auto* code = static_cast<const uint8_t*>(il2::method_pointer(api.serialize_all));
    for (size_t i = 0; code && i + 11 < 0x400; ++i) {
        if (code[i] != 0x48 || code[i + 1] != 0x8B || code[i + 2] != 0x15 || code[i + 7] != 0x48 || code[i + 8] != 0x8B ||
            code[i + 9] != 0xC8 || code[i + 10] != 0xE8)
            continue;
        int32_t disp = 0;
        memcpy(&disp, code + i + 3, 4);
        const auto* slot = reinterpret_cast<const uintptr_t*>(code + i + 7 + disp);
        const uintptr_t value = *slot;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!value || (value & 1) || !VirtualQuery(reinterpret_cast<void*>(value), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT)
            break;
        const auto* method = reinterpret_cast<const MethodInfo*>(value);
        const char* name = il2::il2cpp_method_get_name(method);
        const Il2CppType* type = il2::il2cpp_method_get_return_type(method);
        if (!name || strcmp(name, "GetComponent") != 0 || !type) break;
        api.held_by = il2::il2cpp_type_get_object(type);
        if (api.held_by) il2::il2cpp_gchandle_new(api.held_by, false);
        char* type_name = il2::il2cpp_type_get_name(type);
        logx::info("items: TLD's save leaves out gear whose parent has a %s (GearManager.Serialize: %d chars of loose "
                   "gear here, %.1f ms)", type_name ? type_name : "?", all ? il2::il2cpp_string_length(all) : -1, ms);
        if (type_name) il2::il2cpp_free(type_name);
        return;
    }
    logx::warn("items: couldn't read GearManager.Serialize's parent rule; any parented gear counts as loose");
}

// TLD's own rule for gear saved as lying in the world (GearManager.Serialize): alive, saved, active, not in the pack,
// and not held by its parent. Plus: not one of the sticks or stones TLD scatters around us by itself.
bool is_loose(Il2CppObject* gi) {
    if (!alive(gi) || get<bool>(gi, api.no_save) || get<bool>(gi, api.in_inventory)) return false;
    if (get<Il2CppObject*>(gi, api.radial_parent)) return false;
    Il2CppObject* go = il2::invoke(api.component_go, gi);
    bool active = false;
    if (!go || !il2::invoke_value(api.go_active, go, active) || !active) return false;
    Il2CppObject* parent = il2::invoke(api.get_parent, transform_of(gi));
    if (alive(parent) && api.held_by) {
        void* args[] = {api.held_by};
        if (alive(il2::invoke(api.go_component, il2::invoke(api.component_go, parent), args))) return false;
    }
    return true;
}

// Some gear has no persistent id at all (TLD saves it without one, e.g. items dropped in earlier play). Both games
// loaded it from the same save at the same spot, so its id is made from what it is and where it lies (to the cm),
// GUID-shaped, and registered the way TLD registers ids (ObjectGuid.MaybeAttachObjectGuidAndRegister). TLD's save
// keeps it from then on (GearItem.Serialize picks the new ObjectGuid up).
uint64_t fnv(const std::string& s, uint64_t h) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string made_id(const std::string& what, const Vector3& at, int duplicate) {
    char key[160];
    snprintf(key, sizeof(key), "%s|%ld|%ld|%ld|%d", what.c_str(), std::lround(at.x * 100), std::lround(at.y * 100),
             std::lround(at.z * 100), duplicate);
    const uint64_t a = fnv(key, 1469598103934665603ull), b = fnv(key, 0x9E3779B97F4A7C15ull);
    char id[40];
    snprintf(id, sizeof(id), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32), static_cast<unsigned>(a >> 16) & 0xFFFF,
             static_cast<unsigned>(a) & 0xFFFF, static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return id;
}

std::string give_id(Il2CppObject* gi, const Vector3& at) {
    const std::string what = name_of(gi);
    std::string id;
    for (int duplicate = 0; duplicate < 16; ++duplicate) {
        id = made_id(what, at, duplicate);
        if (!object_by_pdid(id)) break;
    }
    void* args[] = {il2::invoke(api.component_go, gi), il2::il2cpp_string_new(id.c_str())};
    il2::invoke(api.attach_guid, nullptr, args);
    return pdid_of(gi);
}

// Units in a stack (matches, cartridges...), 0 for an item that isn't one. Taken into a pack, a stack goes onto the one
// there (its own object is gone).
int units_of(Il2CppObject* gi) {
    Il2CppObject* s = alive(gi) ? get<Il2CppObject*>(gi, api.stackable) : nullptr;
    return alive(s) ? get<int32_t>(s, api.units) : 0;
}

// One of a kind (net/uniques.h): each player takes their own where it lies, and what they drop stays theirs. Never
// shared: the other game keeps its own copy.
bool is_unique(Il2CppObject* gi) {
    return (api.narrative >= 0 && alive(get<Il2CppObject*>(gi, api.narrative))) || net::is_unique_gear(name_of(gi));
}

struct Loose {
    Il2CppObject* gi = nullptr;
    std::string pdid;
    Vector3 pos{};
    Quaternion rot{};
    int units = 0;
};

int g_without_id = 0, g_ids_given = 0;

std::vector<Loose> scan_loose(bool give_ids) {
    std::vector<Loose> out;
    g_without_id = 0;
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(api.all_gear, &list);
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* gi = il2::array_object_at(items, static_cast<size_t>(i));
        if (!is_loose(gi)) continue;
        Loose l;
        l.gi = gi;
        l.pdid = pdid_of(gi);
        pose_of(gi, l.pos, l.rot);
        l.units = units_of(gi);
        if (l.pdid.empty() && give_ids) {
            l.pdid = give_id(gi, l.pos);
            if (!l.pdid.empty()) ++g_ids_given;
        }
        if (l.pdid.empty()) {
            ++g_without_id;
            continue;
        }
        out.push_back(std::move(l));
    }
    return out;
}

// The item TLD's inspect screen shows (Take it / Leave it), if it's up. GearItemBeingInspected keeps returning the
// last one after the screen closes.
Il2CppObject* inspected_now(Il2CppObject* pm) {
    bool active = false;
    if (!pm || !il2::invoke_value(api.inspecting, pm, active) || !active) return nullptr;
    Il2CppObject* gi = il2::invoke(api.inspected, pm);
    return alive(gi) ? gi : nullptr;
}

// The items the player has in their hands right now without them being in the pack: one being looked at after
// picking it up, one being placed. They don't count as gone (or moved) while that lasts.
void busy_items(std::string& inspected, std::string& placing) {
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    if (!pm) return;
    if (Il2CppObject* gi = inspected_now(pm)) inspected = pdid_of(gi);
    if (Il2CppObject* gi = get<Il2CppObject*>(pm, api.placing); alive(gi)) placing = pdid_of(gi);
}

// --- state ---------------------------------------------------------------------------------------------------------

// A loose item in our scene, as of the last scan.
struct Known {
    std::string prefab;      // for the log
    Vector3 pos{};           // where it is
    Quaternion rot{};
    int units = 0;           // a stack's units (units_of)
    Vector3 told{};          // where the other player has it
    double moved_ms = 0;     // when it last moved
    double changed_ms = -1e18;  // when we last told the other player about it
    bool shared = false;     // the other player has it: it was here when we came in, they told us, or we told them
    bool unique = false;     // one of a kind (is_unique): never shared
    int snare = -1;          // a snare's state (snare_state), -1 for other items
};
std::unordered_map<std::string, Known> g_known;  // our pdid -> item
std::string g_scene;                             // the scene g_known is about ("" = none)
bool g_baseline = false;                         // g_known holds at least one full scan of g_scene
double g_last_scan = 0;
uint32_t g_seq = 0;                              // our change counter
netrt::Newest g_newest;                          // wire pdid -> each player's newest change applied
// Ids differ when ours was taken (by an item in our pack, both games having loaded the same save).
std::unordered_map<std::string, std::string> g_alias_in;   // wire pdid -> ours
std::unordered_map<std::string, std::string> g_alias_out;  // ours -> wire pdid
// The other player's changes for scenes we aren't in (yet), newest per item.
struct Change {
    bool put = false;
    net::ItemPutMsg put_msg;
    net::ItemGoneMsg gone_msg;
};
std::unordered_map<std::string, std::unordered_map<std::string, Change>> g_pending;
netrt::Together g_together;          // host: friends in our scene (our list went to each as they came)
bool g_have_list = false;            // client: a host list for our scene waits for our first scan
net::ItemHashesMsg g_list;
bool g_connected = false;
// Two players taking the same item at about the same time: the host decides who gets it (net::Claims: at the same
// moment, a coin toss).
net::Claims g_claims;                // host: who took what, and when we heard of it
struct Took {
    std::string ours;                // our id for it
    std::string prefab;
    int units = 0;                   // what it added to a stack in our pack
};
std::unordered_map<std::string, Took> g_took;   // wire pdid -> what we took (until the host refuses it, or someone's
                                                // take at the same moment wins the toss)
// The item in our hands (TLD's Take it / Leave it) and since when (looked at every frame): picked up at the same
// moment as another player's take, it goes to the coin toss too.
Il2CppObject* g_hold_gi = nullptr;
std::string g_hold;
double g_hold_since = 0;
std::vector<net::ItemPutMsg> g_put_back;        // client: refused takes that lie where they were again (next frame,
                                                // once our copy is gone)

std::vector<Il2CppObject*> pack_items();

const std::string& wire_id(const std::string& ours) {
    const auto it = g_alias_out.find(ours);
    return it == g_alias_out.end() ? ours : it->second;
}
const std::string& our_id(const std::string& wire) {
    const auto it = g_alias_in.find(wire);
    return it == g_alias_in.end() ? wire : it->second;
}
void forget_alias(const std::string& ours) {
    const auto it = g_alias_out.find(ours);
    if (it == g_alias_out.end()) return;
    g_alias_in.erase(it->second);
    g_alias_out.erase(it);
}

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

void reset(const char* why) {
    if (!g_known.empty()) logx::info("items: forgetting shared item state (%s)", why);
    g_known.clear();
    g_scene.clear();
    g_baseline = false;
    g_newest.clear();
    g_alias_in.clear();
    g_alias_out.clear();
    g_together.clear();
    g_have_list = false;
    g_put_back.clear();
    g_hold_gi = nullptr;
    g_hold.clear();
}

// What we keep of the world for later (changes for scenes someone isn't in, who took what) belongs to this game: a
// dropped link keeps it, back at the main menu (another save may load next) it goes.
void forget_world() {
    g_claims.clear();
    g_took.clear();
    if (g_pending.empty()) return;
    logx::info("items: back at the main menu; forgetting changes kept for %zu scene(s)", g_pending.size());
    g_pending.clear();
}

// --- snares --------------------------------------------------------------------------------------------------------
// A set snare rolls for a catch every few game hours (SnareItem.DoRoll; on its scene's load too, for the hours
// missed): it catches a rabbit (a dead one on it: an item soon, wildlife_carcass) or breaks. The game that runs the
// scene rolls, the host's or a friend's the host isn't in; two games rolling for one snare would disagree. The other
// game gets the outcome as the snare's new record (sent when its state changes) and loads its state from it.

// A snare's state (0 not set, 1 set, 2 broken, 3 with a rabbit); -1 for an item that isn't one.
int snare_state(Il2CppObject* gi) {
    Il2CppObject* s = api.snare >= 0 && api.snare_state >= 0 && alive(gi) ? get<Il2CppObject*>(gi, api.snare) : nullptr;
    return alive(s) ? get<int32_t>(s, api.snare_state) : -1;
}

const char* snare_name(int state) {
    switch (state) {
        case 0: return "not set";
        case 1: return "set";
        case 2: return "broken";
        case 3: return "with a rabbit";
    }
    return "?";
}

// A JSON string value ("key":"...") in TLD's save text, unescaped; empty if there's none.
std::string json_string(const std::string& text, const std::string& key) {
    const std::string at = "\"" + key + "\":\"";
    size_t i = text.find(at);
    if (i == std::string::npos) return {};
    std::string out;
    for (i += at.size(); i < text.size() && text[i] != '"'; ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        const char c = text[++i];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;  // \" \\ \/ as themselves
    }
    return out;
}

// The other player's record of a snare we have: its state and when it last rolled, as loading a save does.
void load_snare(Il2CppObject* gi, const std::string& gear, const std::string& wire, Known& k) {
    Il2CppObject* s = api.snare >= 0 ? get<Il2CppObject*>(gi, api.snare) : nullptr;
    const std::string text = json_string(gear, "m_SnareItemSerialized");
    if (!alive(s) || !api.snare_load || text.empty()) return;
    const int before = snare_state(gi);
    void* args[] = {il2::il2cpp_string_new(text.c_str())};
    il2::invoke(api.snare_load, s, args);
    k.snare = snare_state(gi);
    if (k.snare != before)
        logx::info("items: the other player's snare (%s) is %s now", short_id(wire).c_str(), snare_name(k.snare));
}

// Whether the host's game runs the scene we're in (it's there too): our rolls for its snares are its.
bool host_runs_our_scene() {
    const auto role = netrt::role();
    if (!role || *role != net::Role::Client) return false;
    const auto host = netrt::remote_state();
    return host && host->scene == game::active_scene();
}

using RollFn = void (*)(Il2CppObject* self, const MethodInfo* method);
RollFn g_roll = nullptr;

void roll_detour(Il2CppObject* self, const MethodInfo* method) {
    if (host_runs_our_scene()) {
        logx::info("items: a snare's roll for a catch is the host's here (it runs this scene too); skipped");
        return;
    }
    g_roll(self, method);
}

// --- cooking pots --------------------------------------------------------------------------------------------------
// A pot on a fire (CookingPotItem; TLD's dummy pot too, under food cooked without one) cooks food, boils water or
// melts snow. The food is its own item, lying in the pot (hidden; TLD saves it as loose gear) and found by id when
// the pot's record loads (CookingPotItem.FindGearItemBeingCooked). Both games cook on their own: the same fire, the
// same clock. What a player puts in or takes out (TLD's StartCooking, StartBoilingWater, StartMeltingSnow,
// PickUpCookedItem: hooked) goes to the other game as the pot's record again, the food's own record first.

std::unordered_set<Il2CppObject*> g_pots_changed;  // CookingPotItems a player put something in or took it out of

Il2CppObject* pot_of(Il2CppObject* gi) {
    Il2CppObject* pot = api.pot >= 0 && alive(gi) ? get<Il2CppObject*>(gi, api.pot) : nullptr;
    return alive(pot) ? pot : nullptr;
}

Il2CppObject* food_in(Il2CppObject* pot) {
    Il2CppObject* food = pot ? get<Il2CppObject*>(pot, api.pot_food) : nullptr;
    return alive(food) ? food : nullptr;
}

std::string describe_pot(Il2CppObject* pot) {
    Il2CppObject* food = food_in(pot);
    const int64_t water = get<int64_t>(pot, api.pot_water), snow = get<int64_t>(pot, api.pot_snow);
    char text[160];
    if (food)
        snprintf(text, sizeof(text), "is a pot cooking %s now", name_of(food).c_str());
    else if (water > 0 || snow > 0)
        snprintf(text, sizeof(text), "is a pot %s %.2f l now", snow > 0 ? "melting snow," : "boiling", (snow > 0 ? snow : water) / 1e9);
    else
        snprintf(text, sizeof(text), "is an empty pot now");
    return text;
}

// The other player's record of a pot we have, as loading a save does. Food they took out goes from ours too.
void load_pot(Il2CppObject* gi, const std::string& gear, const std::string& wire) {
    Il2CppObject* pot = pot_of(gi);
    std::string text = json_string(gear, "m_CookingPotItemSerialized");
    if (!pot || !api.pot_load || text.empty()) return;
    const std::string food_wire = json_string(text, "m_GearItemBeingCookedGUID");
    const std::string food_ours = food_wire.empty() ? std::string() : our_id(food_wire);
    if (food_ours != food_wire) replace_all(text, food_wire, food_ours);
    const std::string before = describe_pot(pot);
    if (Il2CppObject* food = food_in(pot); food && pdid_of(food) != food_ours) {
        const std::string id = pdid_of(food);
        g_known.erase(id);  // taken out there (the other game tells us): not gone here by our doing
        il2::invoke(api.pot_drop_food, pot);
    }
    void* args[] = {il2::il2cpp_string_new(text.c_str())};
    il2::invoke(api.pot_load, pot, args);
    g_pots_changed.erase(pot);
    std::string after = describe_pot(pot);  // the food goes in on the pot's next frame (FindGearItemBeingCooked)
    if (!food_ours.empty()) {
        Il2CppObject* food = gear_on(object_by_pdid(food_ours));
        after = "is a pot cooking " + (food ? name_of(food) : "food we don't have (" + short_id(food_ours) + ")") + " now";
    }
    if (after != before)
        logx::info("items: the other player's %s (%s) %s", name_of(gi).c_str(), short_id(wire).c_str(), after.c_str());
}

using PotFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using PotFoodFn = void (*)(Il2CppObject* self, Il2CppObject* gi, const MethodInfo* method);
using PotBoilFn = void (*)(Il2CppObject* self, int64_t litres, bool take_water, const MethodInfo* method);
using PotMeltFn = void (*)(Il2CppObject* self, int64_t litres, const MethodInfo* method);
PotFn g_pot_take = nullptr;
PotFoodFn g_pot_cook = nullptr;
PotBoilFn g_pot_boil = nullptr;
PotMeltFn g_pot_melt = nullptr;

void pot_take_detour(Il2CppObject* self, const MethodInfo* method) {
    g_pot_take(self, method);
    g_pots_changed.insert(self);
}
void pot_cook_detour(Il2CppObject* self, Il2CppObject* gi, const MethodInfo* method) {
    g_pot_cook(self, gi, method);
    g_pots_changed.insert(self);
}
void pot_boil_detour(Il2CppObject* self, int64_t litres, bool take_water, const MethodInfo* method) {
    g_pot_boil(self, litres, take_water, method);
    g_pots_changed.insert(self);
}
void pot_melt_detour(Il2CppObject* self, int64_t litres, const MethodInfo* method) {
    g_pot_melt(self, litres, method);
    g_pots_changed.insert(self);
}

// --- sending -------------------------------------------------------------------------------------------------------

// The item as ItemPut: TLD's save record for it, lying at `pos` / `rot`. False if TLD made none or it's too big.
bool make_put(Il2CppObject* gi, const std::string& pdid, const std::string& prefab, uint8_t flags, const Vector3& pos,
              const Quaternion& rot, net::ItemPutMsg& m) {
    Il2CppObject* data = il2::invoke(api.create_save_data, gi);
    if (!data) {
        logx::warn("items: TLD made no save record for %s (%s); the other player won't see it", prefab.c_str(),
                   short_id(pdid).c_str());
        return false;
    }
    m.scene = g_scene;
    m.seq = ++g_seq;
    m.flags = flags;
    m.pdid = wire_id(pdid);
    m.prefab = text(get<Il2CppString*>(data, api.sd_prefab));
    m.gear = text(get<Il2CppString*>(data, api.sd_gear));
    if (m.pdid != pdid) replace_all(m.gear, pdid, m.pdid);  // our copy goes by another id here
    m.pos[0] = pos.x, m.pos[1] = pos.y, m.pos[2] = pos.z;
    m.rot[0] = rot.x, m.rot[1] = rot.y, m.rot[2] = rot.z, m.rot[3] = rot.w;
    if (m.gear.size() > net::kGearTextMax) {
        logx::warn("items: %s's save record is too big to send (%zu chars)", m.prefab.c_str(), m.gear.size());
        return false;
    }
    return true;
}

// To every friend (the host) or, `to`, one friend (an answer). `change`: what changed, for the log (else it's new or
// it moved).
void send_put(Il2CppObject* gi, const std::string& pdid, Known& k, double now, uint8_t flags, uint8_t to = 0,
              const char* change = nullptr) {
    Quaternion rot{};
    Vector3 pos{};
    pose_of(gi, pos, rot);
    net::ItemPutMsg m;
    if (!make_put(gi, pdid, k.prefab, flags, pos, rot, m)) return;
    net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
    net::write_item_put(w, m);
    if (to)
        netrt::send_packet_to(to, w, true);
    else
        netrt::send_packet(w, true);
    if (hosting()) g_claims.lies_again(g_scene, m.pdid);  // whoever took it before, it's lying here now
    const bool was_shared = k.shared;
    k.told = pos;
    k.shared = true;
    k.changed_ms = now;
    if (change)
        logx::info("items: %s (%s) %s; told the other player (%zu chars)", m.prefab.c_str(), short_id(m.pdid).c_str(),
                   change, m.gear.size());
    else if (!(flags & net::kItemAnswer))
        logx::info("items: %s %s (%s) at (%.1f, %.1f, %.1f); told the other player (%zu chars)", m.prefab.c_str(),
                   was_shared ? "moved to" : "is new here,", short_id(m.pdid).c_str(), pos.x, pos.y, pos.z, m.gear.size());
}

// `taken_now`: we just took it into our pack (TLD's Take it), whatever the pack says yet.
void send_gone(const std::string& pdid, const Known& k, bool taken_now = false) {
    net::ItemGoneMsg m;
    m.scene = g_scene;
    m.seq = ++g_seq;
    m.pdid = wire_id(pdid);
    Il2CppObject* gi = gear_on(object_by_pdid(pdid));
    // In our pack, or (a stack) gone onto one there.
    const bool taken = taken_now || (gi ? get<bool>(gi, api.in_inventory) : k.units > 0);
    m.reason = taken ? net::ItemGoneReason::Taken : net::ItemGoneReason::Destroyed;
    net::Writer w(net::Msg::ItemGone);
    net::write_item_gone(w, m);
    netrt::send_packet(w, true);
    if (taken) g_took[m.pdid] = Took{pdid, k.prefab, k.units};  // it may go to someone's take at the same moment
    if (taken && hosting()) g_claims.taken_by_host(g_scene, m.pdid, now_ms());
    logx::info("items: %s (%s) %s; told the other player", k.prefab.c_str(), short_id(m.pdid).c_str(),
               m.reason == net::ItemGoneReason::Taken ? "went into our pack" : "is gone from the world");
    forget_alias(pdid);
}

void send_list(uint8_t to) {
    net::ItemHashesMsg m;
    m.scene = g_scene;
    for (const auto& [pdid, k] : g_known)
        if (!k.unique) m.hashes.push_back(net::pdid_hash(wire_id(pdid)));
    net::Writer w(net::Msg::ItemList, net::kMaxMessage);
    net::write_item_hashes(w, m);
    netrt::send_packet_to(to, w, true);
    logx::info("items: the other player is in %s with us; sent our %zu loose items (player %u)", g_scene.c_str(),
               m.hashes.size(), to);
}

// Host: what one friend changed goes on to the others, as a change of ours (our counter: each friend's is its own).
void pass_on(uint8_t from, Change c) {
    net::Writer w(c.put ? net::Msg::ItemPut : net::Msg::ItemGone, net::kMaxMessage);
    if (c.put) {
        c.put_msg.seq = ++g_seq;
        net::write_item_put(w, c.put_msg);
    } else {
        c.gone_msg.seq = ++g_seq;
        net::write_item_gone(w, c.gone_msg);
    }
    netrt::send_packet_except(from, w, true);
}

// --- applying the other player's changes ---------------------------------------------------------------------------

bool held_now(const std::string& pdid) {
    std::string inspected, placing;
    busy_items(inspected, placing);
    return pdid == inspected || pdid == placing;
}

void destroy_loose(Il2CppObject* gi) {
    void* args[] = {il2::invoke(api.component_go, gi)};
    il2::invoke(api.destroy_gear, nullptr, args);
}

// TLD's inspect screen buttons (PlayerManager.UpdateInspectGear): Take it / Leave it run the action the screen was
// opened with (m_PickupAction / m_PutBackAction: e.g. back into the container it came from), then
// ExitInspectGearMode(false) (which shows a search's next find).
void press_inspect(Il2CppObject* pm, bool take) {
    const ptrdiff_t field = take ? api.pickup_action : api.put_back_action;
    Il2CppObject* action = field >= 0 ? get<Il2CppObject*>(pm, field) : nullptr;
    if (action) il2::invoke(il2::find_method(il2::il2cpp_object_get_class(action), "Invoke", 0), action);
    bool hard_exit = false;
    void* args[] = {&hard_exit};
    il2::invoke(api.put_back, pm, args);
}

std::string display_name(const std::string& prefab) {
    void* args[] = {il2::il2cpp_string_new(prefab.c_str())};
    const std::string name = api.display_name ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.display_name, nullptr, args)))
                                              : std::string();
    return name.empty() ? prefab : name;
}

// Host: what our game says about an item a friend just took.
net::TakeFacts facts_for(const std::string& scene, const std::string& wire) {
    net::TakeFacts f;
    f.host_scene = scene == g_scene && g_baseline;
    if (!f.host_scene) return f;
    const std::string& ours = our_id(wire);
    Il2CppObject* gi = gear_on(object_by_pdid(ours));
    f.held = held_now(ours);
    f.held_since_ms = f.held && g_hold == ours ? g_hold_since : -1e18;
    f.loose = gi && is_loose(gi) && !f.held;
    f.known = g_known.count(ours) > 0;
    return f;
}

// Host: friend `to` loses the item: out of their pack (LostToss: someone took it at the same moment and won the toss).
void send_not_yours(uint8_t to, const std::string& scene, const std::string& wire, bool tossed) {
    net::ItemGoneMsg m;
    m.scene = scene;
    m.seq = ++g_seq;
    m.reason = tossed ? net::ItemGoneReason::LostToss : net::ItemGoneReason::NotYours;
    m.pdid = wire;
    net::Writer w(net::Msg::ItemGone);
    net::write_item_gone(w, m);
    netrt::send_packet_to(to, w, true);
}

// Host: friend `to` took an item that isn't theirs: someone's take came first or won the coin toss (out of their
// pack), or we have it in our hands (out of their pack, and it lies where it was for them: we may still leave it).
void refuse(uint8_t to, const net::ItemGoneMsg& take, const net::TakeDecision& d) {
    const std::string& ours = our_id(take.pdid);
    const auto k = g_known.find(ours);
    const auto took = g_took.find(take.pdid);
    const std::string prefab = k != g_known.end() ? k->second.prefab : took != g_took.end() ? took->second.prefab
                                                                                            : short_id(take.pdid);
    if (d.answer == net::TakeAnswer::PutBack && k != g_known.end()) {
        net::ItemPutMsg m;
        if (Il2CppObject* gi = gear_on(object_by_pdid(ours));
            gi && make_put(gi, ours, prefab, net::kItemNotYours, k->second.pos, k->second.rot, m)) {
            m.scene = take.scene;
            net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
            net::write_item_put(w, m);
            netrt::send_packet_to(to, w, true);
            logx::info("items: the other player took %s (%s), but it's in our hands%s; refused: out of their pack, "
                       "back where it lay (player %u)", prefab.c_str(), short_id(take.pdid).c_str(),
                       d.tossed ? " (picked up at the same moment: we won the coin toss)" : "", to);
            return;
        }
    }
    send_not_yours(to, take.scene, take.pdid, d.tossed);
    const int taker = g_claims.taker(take.scene, take.pdid);
    const bool ours_won = taker == net::Claims::kHost;
    if (d.tossed)
        logx::info("items: the other player took %s (%s) at the same moment as %s; the coin said %s: out of their pack "
                   "(player %u)", prefab.c_str(), short_id(take.pdid).c_str(), ours_won ? "we did" : "another player",
                   ours_won ? "ours" : "theirs", to);
    else
        logx::info("items: the other player took %s (%s) after %s did; refused: out of their pack (player %u)",
                   prefab.c_str(), short_id(take.pdid).c_str(), ours_won ? "we" : "another player", to);
}

bool out_of_pack(const Took& took);

// Host: friend `winner`'s take at the same moment as an earlier one won the coin toss: the earlier one is undone, out
// of that player's pack (ours, or a friend's told so) or, ours still in our hands, left.
void undo_take(int loser, uint8_t winner, const net::ItemGoneMsg& take) {
    if (loser != net::Claims::kHost) {
        send_not_yours(static_cast<uint8_t>(loser), take.scene, take.pdid, true);
        logx::info("items: player %u took %s at the same moment as player %d and won the coin toss: out of player %d's pack",
                   winner, short_id(take.pdid).c_str(), loser, loser);
        return;
    }
    const std::string ours = our_id(take.pdid);
    Took took{ours, std::string(), 0};
    bool held = false;
    if (const auto k = g_known.find(ours); k != g_known.end()) {  // just now (our look hasn't seen it go yet), or in hand
        took = Took{ours, k->second.prefab, k->second.units};
        held = held_now(ours);
        if (!held) g_known.erase(k);
    } else if (const auto t = g_took.find(take.pdid); t != g_took.end()) {
        took = t->second;
        g_took.erase(t);
    }
    const std::string what = took.prefab.empty() ? short_id(take.pdid) : took.prefab;
    coop_notices::tell("Coin toss: someone else gets the " + display_name(what));
    if (held) {  // left where it lay; the friend's take, applied next, removes it here
        press_inspect(il2::invoke(api.player_manager, nullptr), false);
        logx::info("items: player %u took %s (%s) as we picked it up and won the coin toss: left it", winner, what.c_str(),
                   short_id(take.pdid).c_str());
        return;
    }
    forget_alias(ours);
    const bool out = out_of_pack(took);
    logx::info("items: player %u took %s (%s) at the same moment as we did and won the coin toss: %s", winner, what.c_str(),
               short_id(take.pdid).c_str(), out ? "out of our pack" : "it isn't in our pack");
}

// Our take of an item is undone: out of our pack (the item, or the units it added to a stack there).
bool out_of_pack(const Took& took) {
    Il2CppObject* inv = il2::invoke(api.inventory, nullptr);
    if (!inv) return false;
    if (Il2CppObject* gi = gear_on(object_by_pdid(took.ours)); gi && get<bool>(gi, api.in_inventory)) {
        void* args[] = {gi};
        il2::invoke(api.inv_destroy, inv, args);
        return true;
    }
    if (took.units <= 0) return false;
    for (Il2CppObject* s : pack_items()) {
        if (name_of(s) != took.prefab || units_of(s) < took.units) continue;
        int32_t n = took.units;
        void* args[] = {s, &n};
        il2::invoke(api.remove_units, inv, args);
        return true;
    }
    return false;
}

// Client: the host refused our take (ItemGone NotYours: someone else's came first; LostToss: someone took it at the
// same moment and won the coin toss; ItemPut kItemNotYours: the host has it in its hands, so it lies where it was
// again).
void refused(const Change& c) {
    const std::string& wire = c.put ? c.put_msg.pdid : c.gone_msg.pdid;
    const auto it = g_took.find(wire);
    const Took took = it != g_took.end() ? it->second : Took{our_id(wire), c.put ? c.put_msg.prefab : std::string(), 0};
    if (it != g_took.end()) g_took.erase(it);
    const bool out = out_of_pack(took);
    const std::string what = took.prefab.empty() ? short_id(wire) : took.prefab;
    if (c.put) {
        if (c.put_msg.scene == g_scene && g_baseline) g_put_back.push_back(c.put_msg);  // (else the host's list, there)
        coop_notices::tell("Someone else had the " + display_name(what) + " in hand first");
    } else if (c.gone_msg.reason == net::ItemGoneReason::LostToss) {
        coop_notices::tell("Coin toss: someone else gets the " + display_name(what));
    } else {
        coop_notices::tell("Someone else took the " + display_name(what) + " first");
    }
    logx::info("items: our take of %s (%s) is refused: %s; %s%s", what.c_str(), short_id(wire).c_str(),
               c.put ? "the host has it in its hands"
               : c.gone_msg.reason == net::ItemGoneReason::LostToss ? "taken at the same moment, and the coin said theirs"
                                                                     : "someone else's came first",
               out ? "out of our pack" : "it isn't in our pack", c.put ? ", back where it lay" : "");
}

void apply_put(const net::ItemPutMsg& m, double now) {
    if (net::is_unique_gear(m.prefab)) {  // (never sent; a game from before keeps theirs to themselves too)
        logx::info("items: the other player's %s (%s) is one of a kind: ours stays ours", m.prefab.c_str(),
                   short_id(m.pdid).c_str());
        return;
    }
    const Vector3 pos{m.pos[0], m.pos[1], m.pos[2]};
    const Quaternion rot{m.rot[0], m.rot[1], m.rot[2], m.rot[3]};
    const std::string ours = our_id(m.pdid);
    Il2CppObject* go = object_by_pdid(ours);
    Il2CppObject* gi = gear_on(go);
    if (gi && is_loose(gi)) {  // we have it lying around: it moved, or (a snare) its state changed
        if (held_now(ours)) return;
        Known& k = g_known[ours];
        const bool moved = distance(k.pos, pos) > kStillMetres;
        set_pose(gi, pos, rot);
        k.prefab = m.prefab;
        k.pos = k.told = pos;
        k.rot = rot;
        k.moved_ms = now;
        k.shared = true;
        if (moved || (snare_state(gi) < 0 && !pot_of(gi)))  // (a snare's or a pot's change says itself)
            logx::info("items: the other player's %s (%s) moved to (%.1f, %.1f, %.1f)", m.prefab.c_str(),
                       short_id(m.pdid).c_str(), pos.x, pos.y, pos.z);
        load_snare(gi, m.gear, m.pdid, k);
        load_pot(gi, m.gear, m.pdid);
        return;
    }
    // Make it from TLD's save record, under the same id unless something here already goes by it.
    std::string id = m.pdid, gear = m.gear;
    forget_alias(ours);
    if (object_by_pdid(m.pdid)) {
        id = text(reinterpret_cast<Il2CppString*>(il2::invoke(api.new_pdid, nullptr)));
        replace_all(gear, m.pdid, id);
    }
    void* name_args[] = {il2::il2cpp_string_new(m.prefab.c_str())};
    Il2CppObject* prefab = il2::invoke(api.sanitize, nullptr, name_args);
    bool positioning_fix = false, in_container = false;
    void* args[] = {prefab ? prefab : name_args[0], il2::il2cpp_string_new(gear.c_str()), nullptr, &positioning_fix,
                    &in_container};
    Il2CppObject* made = il2::invoke(api.instantiate, nullptr, args);
    if (!alive(made)) {
        logx::warn("items: the other player's %s (%s) couldn't be made here", m.prefab.c_str(), short_id(m.pdid).c_str());
        return;
    }
    set_pose(made, pos, rot);
    const std::string got = pdid_of(made);
    if (got != m.pdid && !got.empty()) {
        g_alias_in[m.pdid] = got;
        g_alias_out[got] = m.pdid;
    }
    if (got != id) logx::warn("items: our copy of %s came out as %s (wanted %s)", m.prefab.c_str(), got.c_str(), id.c_str());
    Known& k = g_known[got.empty() ? id : got];
    k.prefab = m.prefab;
    k.pos = k.told = pos;
    k.rot = rot;
    k.units = units_of(made);
    k.snare = snare_state(made);
    k.moved_ms = now;
    k.shared = true;
    logx::info("items: the other player's %s (%s%s%s) %s at (%.1f, %.1f, %.1f)", m.prefab.c_str(), short_id(m.pdid).c_str(),
               got != m.pdid ? ", ours: " : "", got != m.pdid ? short_id(got).c_str() : "",
               (m.flags & net::kItemAnswer) ? "was missing here; now it's"
               : (m.flags & net::kItemNotYours) ? "lies where it was again"
                                                : "is here now",
               pos.x, pos.y, pos.z);
}

void remove_ours(const std::string& ours, const char* why) {
    Il2CppObject* gi = gear_on(object_by_pdid(ours));
    const auto known = g_known.find(ours);
    const std::string prefab = known != g_known.end() ? known->second.prefab : gi ? name_of(gi) : "item";
    if (known != g_known.end()) g_known.erase(known);
    forget_alias(ours);
    // In our hands (TLD's Take it / Leave it): the other take came first (the host decided). We leave it, then it goes.
    const bool held = held_now(ours);
    if (held) {
        Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
        if (inspected_now(pm) == gi) press_inspect(pm, false);
        if (held_now(ours)) {
            logx::warn("items: %s (%s) %s while we hold it; now both of you have one", prefab.c_str(), short_id(ours).c_str(), why);
            return;
        }
    }
    if (!gi || !is_loose(gi)) {
        logx::info("items: %s (%s) %s, but it isn't lying here (already gone or in a pack)", prefab.c_str(),
                   short_id(ours).c_str(), why);
        return;
    }
    destroy_loose(gi);
    if (held) coop_notices::tell("Someone else took the " + display_name(prefab) + " first");
    logx::info("items: %s (%s) %s; %sgone here too", prefab.c_str(), short_id(ours).c_str(), why,
               held ? "it was in our hands: left it, " : "");
}

// Client: someone's take reaches us before our scan has told the host that we took the same item: we both took it
// before hearing of the other's take. Ours goes to the host now and stays in our pack: the host tosses a coin (or, if
// ours comes too late for the same moment, refuses it).
bool report_our_take(const std::string& ours, bool taken_now = false) {
    const auto k = g_known.find(ours);
    if (k == g_known.end()) return false;
    Il2CppObject* gi = gear_on(object_by_pdid(ours));
    if (!taken_now && (gi ? !get<bool>(gi, api.in_inventory) : k->second.units <= 0)) return false;  // (send_gone's rule)
    const std::string prefab = k->second.prefab;
    send_gone(ours, k->second, true);
    g_known.erase(k);
    logx::info("items: %s (%s) was taken by the other player as we %s it; ours went to the host too (a coin decides)",
               prefab.c_str(), short_id(ours).c_str(), taken_now ? "picked" : "took");
    return true;
}

// Client: the same, with it in our hands (Take it / Leave it) since the same moment: we take it, and the coin decides.
// In our hands from before, theirs came after ours but we hadn't said: we leave it (remove_ours).
bool take_held_at_same_moment(const std::string& ours, double now) {
    if (g_hold != ours || now - g_hold_since > net::Claims::kSameMomentMs || !held_now(ours)) return false;
    press_inspect(il2::invoke(api.player_manager, nullptr), true);
    return report_our_take(ours, true);
}

void apply_gone(const net::ItemGoneMsg& m, double now) {
    const std::string ours = our_id(m.pdid);
    if (const auto k = g_known.find(ours); k != g_known.end() && k->second.unique) {
        logx::info("items: the other player took their %s (%s); one of a kind: ours stays", k->second.prefab.c_str(),
                   short_id(m.pdid).c_str());
        return;
    }
    if (!hosting() && m.reason == net::ItemGoneReason::Taken &&
        (take_held_at_same_moment(ours, now) || report_our_take(ours)))
        return;
    remove_ours(ours, m.reason == net::ItemGoneReason::Taken ? "was taken by the other player"
                                                             : "is gone from the other player's world");
}

void apply_pending() {
    const auto it = g_pending.find(g_scene);
    if (it == g_pending.end()) return;
    std::vector<Change> changes;
    for (auto& [pdid, c] : it->second) changes.push_back(std::move(c));
    g_pending.erase(it);
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) {
        return static_cast<int32_t>((a.put ? a.put_msg.seq : a.gone_msg.seq) - (b.put ? b.put_msg.seq : b.gone_msg.seq)) < 0;
    });
    logx::info("items: %zu change(s) the other player made in %s while we were elsewhere", changes.size(), g_scene.c_str());
    const double now = now_ms();
    for (const Change& c : changes) {
        if (c.put)
            apply_put(c.put_msg, now);
        else
            apply_gone(c.gone_msg, now);
    }
}

// Client: the host's list of what lies in this scene. What we have and the host doesn't goes (unless we changed it
// just now: the host may not have heard yet); what the host has and we don't, we ask for.
void apply_list(const net::ItemHashesMsg& m, double now) {
    const std::unordered_set<uint64_t> host(m.hashes.begin(), m.hashes.end());
    std::unordered_set<uint64_t> ours;
    std::vector<std::string> extra;
    for (const auto& [pdid, k] : g_known) {
        if (k.unique) continue;  // ours, whatever the host has
        const uint64_t h = net::pdid_hash(wire_id(pdid));
        ours.insert(h);
        if (!host.count(h) && k.shared && now - k.changed_ms > kListGraceMs) extra.push_back(pdid);
    }
    for (const std::string& pdid : extra) remove_ours(pdid, "isn't in the host's world");
    net::ItemHashesMsg want;
    want.scene = g_scene;
    for (uint64_t h : m.hashes)
        if (!ours.count(h)) want.hashes.push_back(h);
    logx::info("items: the host has %zu loose items in %s, we had %zu: removed %zu, asking for %zu", m.hashes.size(),
               g_scene.c_str(), ours.size(), extra.size(), want.hashes.size());
    if (want.hashes.empty()) return;
    net::Writer w(net::Msg::ItemWant, net::kMaxMessage);
    net::write_item_hashes(w, want);
    netrt::send_packet(w, true);
}

// Host: friend `from` asks for these.
void answer_want(const net::ItemHashesMsg& m, double now, uint8_t from) {
    if (m.scene != g_scene) {
        logx::info("items: the other player asked for items in %s; we're in %s now", m.scene.c_str(), g_scene.c_str());
        return;
    }
    std::unordered_map<uint64_t, std::string> by_hash;
    for (const auto& [pdid, k] : g_known)
        if (!k.unique) by_hash[net::pdid_hash(wire_id(pdid))] = pdid;
    int sent = 0;
    for (uint64_t h : m.hashes) {
        const auto it = by_hash.find(h);
        if (it == by_hash.end()) continue;
        Il2CppObject* gi = gear_on(object_by_pdid(it->second));
        if (!gi) continue;
        send_put(gi, it->second, g_known[it->second], now, net::kItemAnswer, from);
        ++sent;
    }
    logx::info("items: the other player was missing %zu of our items; sent %d", m.hashes.size(), sent);
}

void receive(double now) {
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::ItemPut, packets);
    netrt::drain_packets(net::Msg::ItemGone, packets);
    netrt::drain_packets(net::Msg::ItemList, packets);
    netrt::drain_packets(net::Msg::ItemWant, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        if (p.type == net::Msg::ItemPut || p.type == net::Msg::ItemGone) {
            Change c;
            c.put = p.type == net::Msg::ItemPut;
            if (c.put ? !net::read_item_put(r, c.put_msg) : !net::read_item_gone(r, c.gone_msg)) continue;
            const std::string& scene = c.put ? c.put_msg.scene : c.gone_msg.scene;
            const std::string& pdid = c.put ? c.put_msg.pdid : c.gone_msg.pdid;
            const uint32_t seq = c.put ? c.put_msg.seq : c.gone_msg.seq;
            if (!g_newest.take(p, pdid, seq)) continue;  // a late older change
            if (c.put ? (c.put_msg.flags & net::kItemNotYours) != 0
                      : c.gone_msg.reason == net::ItemGoneReason::NotYours || c.gone_msg.reason == net::ItemGoneReason::LostToss) {
                if (!hosting()) refused(c);
                continue;
            }
            if (hosting()) {
                if (c.put) {
                    g_claims.lies_again(scene, pdid);
                } else if (c.gone_msg.reason == net::ItemGoneReason::Taken) {
                    const net::TakeDecision d = g_claims.take(scene, pdid, p.from, facts_for(scene, pdid), now);
                    if (d.answer != net::TakeAnswer::Yours) {  // not theirs: the others never hear of it
                        refuse(p.from, c.gone_msg, d);
                        continue;
                    }
                    if (d.loser >= 0) undo_take(d.loser, p.from, c.gone_msg);
                }
                pass_on(p.from, c);
            }
            if (scene == g_scene && g_baseline) {
                if (c.put)
                    apply_put(c.put_msg, now);
                else
                    apply_gone(c.gone_msg, now);
            } else {
                logx::info("items: the other player's change to %s in %s waits until we get there", short_id(pdid).c_str(),
                           scene.c_str());
                g_pending[scene][pdid] = std::move(c);
            }
        } else if (p.type == net::Msg::ItemList) {
            net::ItemHashesMsg m;
            if (!net::read_item_hashes(r, m) || hosting()) continue;
            if (m.scene == g_scene && g_baseline) {
                apply_list(m, now);
            } else if (m.scene == g_scene) {
                g_list = std::move(m);
                g_have_list = true;
            }
        } else if (p.type == net::Msg::ItemWant) {
            net::ItemHashesMsg m;
            if (net::read_item_hashes(r, m) && hosting()) answer_want(m, now, p.from);
        }
    }
}

// --- scanning ------------------------------------------------------------------------------------------------------

void scan(double now) {
    if (!api.held_by_resolved) resolve_held_by();
    const bool first = !g_baseline;
    if (first) apply_pending();  // what the other player changed here while we were elsewhere
    const double t0 = now_ms();
    std::vector<Loose> loose = scan_loose(true);
    std::string inspected, placing;
    busy_items(inspected, placing);
    std::unordered_set<std::string> present;
    for (const Loose& l : loose) {
        present.insert(l.pdid);
        if (l.pdid == inspected || l.pdid == placing) continue;
        auto it = g_known.find(l.pdid);
        if (it == g_known.end()) {
            Known k;
            k.prefab = name_of(l.gi);
            k.pos = l.pos;
            k.rot = l.rot;
            k.units = l.units;
            k.moved_ms = now;
            k.snare = snare_state(l.gi);
            k.unique = is_unique(l.gi);
            if (k.unique) {
                logx::info("items: %s (%s) at (%.1f, %.1f, %.1f) is one of a kind: %s", k.prefab.c_str(),
                           short_id(l.pdid).c_str(), l.pos.x, l.pos.y, l.pos.z,
                           first ? "each player has their own" : "it lies here for us only");
            } else if (first) {  // it was here when we came in: the other player's game has it too
                k.told = l.pos;
                k.shared = true;
            }
            g_known.emplace(l.pdid, std::move(k));
            continue;
        }
        Known& k = it->second;
        if (distance(l.pos, k.pos) > kStillMetres) {
            k.pos = l.pos;
            k.rot = l.rot;
            k.moved_ms = now;
            continue;
        }
        if (k.unique) continue;
        if (const int snare = snare_state(l.gi); snare != k.snare) {  // set, caught or broken here
            k.snare = snare;
            if (k.shared) {
                const std::string change = std::string("is a snare ") + snare_name(snare) + " now";
                send_put(l.gi, l.pdid, k, now, 0, 0, change.c_str());
                continue;
            }
        }
        if (Il2CppObject* pot = pot_of(l.gi); pot && g_pots_changed.erase(pot) && k.shared) {  // put in or taken out
            if (Il2CppObject* food = food_in(pot)) {  // the food first: the pot finds it by id
                const std::string id = pdid_of(food);
                const auto f = g_known.find(id);
                if (f == g_known.end() || !f->second.shared) {
                    Known& fk = g_known[id];
                    fk.prefab = name_of(food);
                    pose_of(food, fk.pos, fk.rot);
                    fk.units = units_of(food);
                    fk.moved_ms = now;
                    send_put(food, id, fk, now, 0, 0, "went into a pot");
                }
            }
            send_put(l.gi, l.pdid, k, now, 0, 0, describe_pot(pot).c_str());
            continue;
        }
        if (now - k.moved_ms >= kSettleMs && (!k.shared || distance(k.pos, k.told) > kMovedMetres))
            send_put(l.gi, l.pdid, k, now, 0);
    }
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (present.count(it->first) || it->first == inspected || it->first == placing) {
            ++it;
            continue;
        }
        if (it->second.unique)
            logx::info("items: %s (%s), one of a kind, is gone from our world; the other player keeps theirs",
                       it->second.prefab.c_str(), short_id(it->first).c_str());
        else if (it->second.shared)
            send_gone(it->first, it->second);
        it = g_known.erase(it);
    }
    if (first) {
        g_baseline = true;
        logx::info("items: %zu loose items in %s (%d given an id now, %d without one), scan %.1f ms", g_known.size(),
                   g_scene.c_str(), g_ids_given, g_without_id, now_ms() - t0);
        g_ids_given = 0;
        if (g_have_list) {
            g_have_list = false;
            apply_list(g_list, now);
        }
    }
}

// --- test helpers --------------------------------------------------------------------------------------------------

Vector3 player_pos() {
    const game::PlayerState p = game::read_player();
    return Vector3{p.position.x, p.position.y, p.position.z};
}

std::vector<Il2CppObject*> pack_items() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* inv = il2::invoke(api.inventory, nullptr);
    Il2CppObject* list = get<Il2CppObject*>(inv, api.inv_items);
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* gi = *reinterpret_cast<Il2CppObject* const*>(reinterpret_cast<const char*>(items) + 0x20 +
                                                                   static_cast<size_t>(i) * kInventoryEntry);
        if (alive(gi)) out.push_back(gi);
    }
    return out;
}

}  // namespace

void describe(float metres) {
    if (!g_ready) return;
    const Vector3 me = player_pos();
    std::vector<std::pair<float, Loose>> nearby;
    for (Loose& l : scan_loose(false)) {
        const float d = distance(l.pos, me);
        if (d <= metres) nearby.emplace_back(d, std::move(l));
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    logx::info("dev: items: %zu loose within %.0f m (%zu known in %s)", nearby.size(), metres, g_known.size(),
               g_scene.empty() ? "no scene" : g_scene.c_str());
    // Loose gear with no persistent id (not shared): what it is, so we can tell why.
    {
        Il2CppObject* list = nullptr;
        il2::il2cpp_field_static_get_value(api.all_gear, &list);
        Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
        const int32_t count = get<int32_t>(list, kListSize);
        int shown = 0;
        for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
            Il2CppObject* gi = il2::array_object_at(items, static_cast<size_t>(i));
            if (!is_loose(gi) || !pdid_of(gi).empty() || shown >= 12) continue;
            ++shown;
            Vector3 pos{};
            Quaternion rot{};
            pose_of(gi, pos, rot);
            Il2CppObject* og = get<Il2CppObject*>(gi, api.object_guid);
            Il2CppObject* parent = il2::invoke(api.get_parent, transform_of(gi));
            logx::info("dev:   no id: %s at (%.1f, %.1f, %.1f), %.0f m, ObjectGuid %s%s%s", name_of(gi).c_str(), pos.x, pos.y,
                       pos.z, distance(pos, me), alive(og) ? "present" : "none", alive(parent) ? ", under " : "",
                       alive(parent) ? name_of(parent).c_str() : "");
        }
    }
    for (const auto& [d, l] : nearby) {
        const auto k = g_known.find(l.pdid);
        Il2CppObject* parent = il2::invoke(api.get_parent, transform_of(l.gi));
        logx::info("dev:   %s %s at (%.1f, %.1f, %.1f), %.1f m%s%s%s", short_id(l.pdid).c_str(), name_of(l.gi).c_str(),
                   l.pos.x, l.pos.y, l.pos.z, d, k == g_known.end() ? ", not scanned yet" : k->second.shared ? ", shared" : ", ours only",
                   alive(parent) ? ", under " : "", alive(parent) ? name_of(parent).c_str() : "");
    }
}

void describe_pack() {
    if (!g_ready) return;
    const std::vector<Il2CppObject*> items = pack_items();
    logx::info("dev: pack: %zu items", items.size());
    for (Il2CppObject* gi : items) logx::info("dev:   %s %s", short_id(pdid_of(gi)).c_str(), name_of(gi).c_str());
}

bool pickup(const std::string& name) {
    if (!g_ready) return false;
    const Vector3 me = player_pos();
    Il2CppObject* best = nullptr;
    float best_d = 1e9f;
    for (const Loose& l : scan_loose(false)) {
        const float d = distance(l.pos, me);
        if (d < best_d && contains_nocase(name_of(l.gi), name)) best = l.gi, best_d = d;
    }
    if (!best || best_d > 6.0f) {
        logx::warn("dev: pickup: no loose %s within 6 m", name.empty() ? "item" : name.c_str());
        return false;
    }
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    const std::string what = name_of(best), id = pdid_of(best);
    bool force_equip = false, equip_now = false, skip_audio = false, taken = false;
    void* args[] = {best, &force_equip, &equip_now, &skip_audio};
    il2::invoke_value(api.pickup, pm, taken, args);
    Il2CppObject* after = gear_on(object_by_pdid(id));
    logx::info("dev: pickup: %s (%s) %.1f m away: TLD %s; %s", what.c_str(), short_id(id).c_str(), best_d,
               taken ? "took it" : "said no",
               after && get<bool>(after, api.in_inventory) ? "it's in our pack" : alive(after) ? "it's still lying there" : "it's gone (into a stack)");
    return taken;
}

bool press_inspect_button(bool take) {
    const char* verb = take ? "takeit" : "putback";
    if (!g_ready || !api.put_back) return false;
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    Il2CppObject* gi = inspected_now(pm);
    if (!gi) {
        logx::info("dev: %s: not looking at an item", verb);
        return false;
    }
    const std::string what = name_of(gi), id = pdid_of(gi);
    press_inspect(pm, take);
    Il2CppObject* after = gear_on(object_by_pdid(id));
    Il2CppObject* next = inspected_now(pm);
    logx::info("dev: %s: %s (%s), as TLD's %s button does; %s%s%s", verb, what.c_str(), short_id(id).c_str(),
               take ? "Take it" : "Leave it",
               after && get<bool>(after, api.in_inventory) ? "it's in our pack" : alive(after) ? "it's out of our hands" : "it's gone",
               alive(next) ? "; now looking at " : "", alive(next) ? name_of(next).c_str() : "");
    return true;
}

bool put_back() { return press_inspect_button(false); }
bool take_it() { return press_inspect_button(true); }

bool snare(const std::string& what) {
    const Vector3 me = player_pos();
    Loose best;
    float best_m = 6;
    for (const Loose& l : scan_loose(false))
        if (snare_state(l.gi) >= 0 && distance(l.pos, me) < best_m) {
            best = l;
            best_m = distance(l.pos, me);
        }
    if (!best.gi) {
        logx::warn("dev: snare: no snare lying within 6 m");
        return false;
    }
    Il2CppObject* s = get<Il2CppObject*>(best.gi, api.snare);
    if (what == "set") {
        il2::invoke(api.snare_placed, s);  // as placing it does: set, rolling from now
    } else if (what == "break" || what == "catch") {
        int32_t state = what == "break" ? 2 : 3;
        void* args[] = {&state};
        il2::invoke(api.snare_set_state, s, args);
        if (what == "catch") {  // what a roll that catches does (DoRoll)
            void* rabbit[] = {api.snare_rabbit >= 0 ? get<Il2CppObject*>(s, api.snare_rabbit) : nullptr};
            il2::invoke(api.snare_spawn_rabbit, s, rabbit);
        }
    } else if (what == "roll") {
        il2::invoke(api.snare_roll, s);  // TLD's own roll (skipped in a friend's game while the host runs the scene)
    } else if (!what.empty()) {
        logx::warn("dev: snare: say set, catch, break or roll (or nothing: its state)");
        return false;
    }
    logx::info("dev: snare: %s (%s), %.1f m away, is %s", name_of(best.gi).c_str(), short_id(best.pdid).c_str(), best_m,
               snare_name(snare_state(best.gi)));
    return true;
}

bool record(const std::string& name) {
    const Vector3 me = player_pos();
    Loose best;
    float best_m = 6;
    for (const Loose& l : scan_loose(false))
        if ((name.empty() || contains_nocase(name_of(l.gi), name)) && distance(l.pos, me) < best_m) {
            best = l;
            best_m = distance(l.pos, me);
        }
    net::ItemPutMsg m;
    if (!best.gi || !make_put(best.gi, best.pdid, name_of(best.gi), 0, best.pos, best.rot, m)) {
        logx::warn("dev: record: no loose item%s%s within 6 m with a save record", name.empty() ? "" : " like ",
                   name.c_str());
        return false;
    }
    logx::info("dev: record: %s (%s), %zu chars: %.1500s", m.prefab.c_str(), short_id(best.pdid).c_str(), m.gear.size(),
               m.gear.c_str());
    return true;
}

bool gear_record(Il2CppObject* gi, std::string& prefab, std::string& record) {
    if (!g_ready || !alive(gi)) return false;
    Il2CppObject* data = il2::invoke(api.create_save_data, gi);
    if (!data) return false;
    prefab = text(get<Il2CppString*>(data, api.sd_prefab));
    record = text(get<Il2CppString*>(data, api.sd_gear));
    const std::string pdid = pdid_of(gi);
    const std::string fresh = text(reinterpret_cast<Il2CppString*>(il2::invoke(api.new_pdid, nullptr)));
    if (!pdid.empty() && !fresh.empty()) replace_all(record, pdid, fresh);  // the given one is another item
    return !prefab.empty() && record.size() <= net::kGearTextMax;
}

Il2CppObject* gear_from_record(const std::string& prefab, const std::string& record) {
    if (!g_ready) return nullptr;
    // (Its id is new: gear_record gave it one, so nothing here goes by it.)
    void* name_args[] = {il2::il2cpp_string_new(prefab.c_str())};
    Il2CppObject* sane = il2::invoke(api.sanitize, nullptr, name_args);
    bool positioning_fix = false, in_container = false;
    void* args[] = {sane ? sane : name_args[0], il2::il2cpp_string_new(record.c_str()), nullptr, &positioning_fix,
                    &in_container};
    Il2CppObject* made = il2::invoke(api.instantiate, nullptr, args);
    return alive(made) ? made : nullptr;
}

std::vector<Il2CppObject*> pack() { return g_ready ? pack_items() : std::vector<Il2CppObject*>{}; }

bool in_pack(Il2CppObject* gi) { return alive(gi) && api.in_inventory >= 0 && get<bool>(gi, api.in_inventory); }

int units(Il2CppObject* gi) { return units_of(gi); }

bool one_of_a_kind(Il2CppObject* gi) { return alive(gi) && is_unique(gi); }

std::string gear_name(const std::string& prefab) { return display_name(prefab); }

std::string prefab_of(Il2CppObject* gi) {
    if (!alive(gi)) return {};
    void* args[] = {il2::il2cpp_string_new(name_of(gi).c_str())};
    Il2CppString* sane = reinterpret_cast<Il2CppString*>(il2::invoke(api.sanitize, nullptr, args));
    return sane ? text(sane) : name_of(gi);
}

bool cooking(const std::string& what, const std::string& arg) {
    if (api.pot < 0 || api.pot_hours < 0 || api.pot_state < 0) return false;
    const Vector3 me = player_pos();
    const bool act = what == "take" || what == "put" || what == "boil" || what == "melt";
    const float hours = what.empty() || act ? 0.0f : static_cast<float>(atof(what.c_str()));
    int pots = 0;
    for (const Loose& l : scan_loose(false)) {
        Il2CppObject* pot = get<Il2CppObject*>(l.gi, api.pot);
        if (!alive(pot) || distance(l.pos, me) > 6) continue;
        ++pots;
        if (hours > 0) put<float>(pot, api.pot_hours, get<float>(pot, api.pot_hours) + hours);  // as if they'd passed
        Il2CppObject* food = get<Il2CppObject*>(pot, api.pot_food);
        Il2CppObject* fire = get<Il2CppObject*>(pot, api.pot_fire);
        const int state = get<int32_t>(pot, api.pot_state);
        logx::info("dev: cooking: %s (%s), %.1f m away, on %s: %s %s, %.2f h, %.0f%% cooked, %.0f%% ruined, water %.2f l, "
                   "snow %.2f l", name_of(l.gi).c_str(), short_id(l.pdid).c_str(), distance(l.pos, me),
                   alive(fire) ? "a fire" : "no fire", alive(food) ? name_of(food).c_str() : "nothing",
                   state == 0 ? "cooking" : state == 1 ? "ready" : "ruined", get<float>(pot, api.pot_hours),
                   get<float>(pot, api.pot_cooked) * 100, get<float>(pot, api.pot_ruined) * 100,
                   get<int64_t>(pot, api.pot_water) / 1e9, get<int64_t>(pot, api.pot_snow) / 1e9);
        if (!act || !alive(fire)) continue;
        bool dummy = false;
        if (what != "take" && api.pot_dummy && il2::invoke_value(api.pot_dummy, pot, dummy) && dummy) continue;
        if (what == "take") {
            if (!api.pot_take) return false;
            il2::invoke(api.pot_take, pot);  // what taking it from the fire does (CookingPotItem.PerformInteraction)
            logx::info("dev: cooking: took what was in the %s", name_of(l.gi).c_str());
        } else if (what == "put") {
            Il2CppObject* gi = nullptr;
            for (Il2CppObject* candidate : pack_items())
                if (!gi && contains_nocase(name_of(candidate), arg)) gi = candidate;
            if (!gi || !api.pot_cook) {
                logx::warn("dev: cooking: nothing like %s in our pack", arg.c_str());
                return false;
            }
            const std::string name = name_of(gi);
            void* args[] = {gi};
            il2::invoke(api.pot_cook, pot, args);  // what the cooking panel does with a pot (OnCookNormalFood)
            logx::info("dev: cooking: put the %s in the %s", name.c_str(), name_of(l.gi).c_str());
        } else {
            int64_t units = static_cast<int64_t>(atof(arg.c_str()) * 1e9);  // ItemLiquidVolume.m_Units: 1e-9 l
            bool take_water = true;
            void* boil_args[] = {&units, &take_water};
            void* melt_args[] = {&units};
            il2::invoke(what == "boil" ? api.pot_boil : api.pot_melt, pot, what == "boil" ? boil_args : melt_args);
            logx::info("dev: cooking: %s %.2f l in the %s", what == "boil" ? "boiling" : "melting", units / 1e9,
                       name_of(l.gi).c_str());
        }
        return true;
    }
    if (!pots) logx::warn("dev: cooking: no pot or food on a fire within 6 m");
    else if (act) logx::warn("dev: cooking: no %s on a fire within 6 m", what == "take" ? "pot or food" : "pot");
    return pots > 0 && !act;
}

bool coin(const std::string& how) {
    const int which = how == "fair" ? -1 : how == "first" ? 0 : how == "last" ? 1 : -2;
    if (which == -2) {
        logx::warn("dev: coin: say fair, first (the earlier take or hold keeps it) or last (the newcomer gets it)");
        return false;
    }
    g_claims.force(which);
    logx::info("dev: coin: %s", which < 0 ? "fair" : which ? "the newcomer gets it" : "the earlier take keeps it");
    return true;
}

bool inspect(const std::string& name) {
    if (!g_ready || !api.inspect) return false;
    const Vector3 me = player_pos();
    Il2CppObject* best = nullptr;
    float best_d = 1e9f;
    for (const Loose& l : scan_loose(false)) {
        const float d = distance(l.pos, me);
        if (d < best_d && contains_nocase(name_of(l.gi), name)) best = l.gi, best_d = d;
    }
    if (!best || best_d > 6.0f) {
        logx::warn("dev: inspect: no loose %s within 6 m", name.empty() ? "item" : name.c_str());
        return false;
    }
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    void* args[] = {best};
    il2::invoke(api.inspect, pm, args);
    logx::info("dev: inspect: %s (%s) %.1f m away: %s", name_of(best).c_str(), short_id(pdid_of(best)).c_str(), best_d,
               inspected_now(pm) == best ? "in our hands (Take it / Leave it)" : "TLD didn't show it");
    return inspected_now(pm) == best;
}

bool drop(const std::string& name, int units) {
    if (!g_ready) return false;
    Il2CppObject* gi = nullptr;
    for (Il2CppObject* candidate : pack_items())
        if (contains_nocase(name_of(candidate), name)) gi = candidate;
    if (!gi) {
        logx::warn("dev: drop: no %s in our pack", name.c_str());
        return false;
    }
    int32_t n = std::max(1, units);
    bool sound = true, at_feet = false, force = true;
    void* args[] = {&n, &sound, &at_feet, &force};
    Il2CppObject* dropped = il2::invoke(api.drop, gi, args);
    Vector3 pos{};
    Quaternion rot{};
    if (alive(dropped)) pose_of(dropped, pos, rot);
    logx::info("dev: drop: %s x%d -> %s (%s) at (%.1f, %.1f, %.1f)", name_of(gi).c_str(), n,
               alive(dropped) ? name_of(dropped).c_str() : "nothing", alive(dropped) ? short_id(pdid_of(dropped)).c_str() : "-",
               pos.x, pos.y, pos.z);
    return alive(dropped);
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
    Il2CppClass* save_data = il2::find_class(kGame, "", "GearSaveData");
    Il2CppClass* object_guid = il2::find_class(kGame, "", "ObjectGuid");
    Il2CppClass* pdid_table = il2::find_class("TLD.PDID.dll", "TLD.PDID", "PdidTable");
    Il2CppClass* gear_manager = il2::find_class(kGame, "", "GearManager");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* inventory = il2::find_class(kGame, "", "Inventory");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.go_active = il2::find_method(game_object, "get_activeInHierarchy", 0);
    api.go_component = il2::find_method_params(game_object, "GetComponent", {"System.Type"});
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.get_rotation = il2::find_method(transform, "get_rotation", 0);
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    api.set_pose = il2::find_method(transform, "SetPositionAndRotation", 2);
    api.object_name = il2::find_method(object, "get_name", 0);
    api.gear_type = il2::type_object(gear);
    if (api.gear_type) il2::il2cpp_gchandle_new(api.gear_type, false);
    api.in_inventory = il2::field_offset(gear, "m_InPlayerInventory");
    api.no_save = il2::field_offset(gear, "m_DisableSerialization");
    api.object_guid = il2::field_offset(gear, "m_ObjectGuid");
    api.radial_parent = il2::field_offset(gear, "m_RadialObjectSpawnerParent");
    api.narrative = il2::field_offset(gear, "m_NarrativeCollectibleItem");
    api.create_save_data = il2::find_method(gear, "CreateSaveData", 0);
    api.sd_prefab = il2::field_offset(save_data, "m_PrefabName");
    api.sd_gear = il2::field_offset(save_data, "m_SearializedGear");
    api.instantiate = il2::find_method_params(gear, "InstantiateAndDeserializeGearItem",
                                              {"System.String", "System.String", "UnityEngine.Transform", "System.Boolean",
                                               "System.Boolean"});
    api.sanitize = il2::find_method_params(il2::find_class(kGame, "", "Utils"), "SanitizePrefabName", {"System.String"});
    api.drop = il2::find_method(gear, "Drop", 4);
    api.display_name = il2::find_method_params(gear, "GetGearDisplayName", {"System.String"});
    api.stackable = il2::field_offset(gear, "m_StackableItem");
    api.units = il2::field_offset(il2::find_class(kGame, "", "StackableItem"), "m_Units");
    api.guid = il2::field_offset(object_guid, "m_Guid");
    api.cached_pdid = il2::field_offset(il2::find_class("TLD.PDID.dll", "TLD.PDID", "PdidObjectBase"), "m_RuntimeCachedPdid");
    api.guid_of_go = il2::find_method(object_guid, "GetObjectGuidFromGO", 1);
    api.attach_guid = il2::find_method(object_guid, "MaybeAttachObjectGuidAndRegister", 2);
    api.pdid_object = il2::find_method_params(pdid_table, "GetGameObject", {"System.String"});
    api.new_pdid = il2::find_method(pdid_table, "GenerateNewID", 0);
    if (gear_manager) api.all_gear = il2::il2cpp_class_get_field_from_name(gear_manager, "m_Gear");
    api.destroy_gear = il2::find_method_params(gear_manager, "DestroyGearObject", {"UnityEngine.GameObject"});
    api.serialize_all = il2::find_method(gear_manager, "Serialize", 0);
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    api.inspected = il2::find_method(player_manager, "GearItemBeingInspected", 0);
    api.inspecting = il2::find_method(player_manager, "IsInspectModeActive", 0);
    api.put_back = il2::find_method(player_manager, "ExitInspectGearMode", 1);
    api.pickup_action = il2::field_offset(player_manager, "m_PickupAction");
    api.put_back_action = il2::field_offset(player_manager, "m_PutBackAction");
    api.placing = il2::field_offset(player_manager, "m_ObjectToPlaceGearItem");
    api.pickup = il2::find_method(player_manager, "ProcessPickupItemInteraction", 4);
    api.inspect = il2::find_method_params(player_manager, "EnterInspectGearMode", {"GearItem"});
    api.inv_items = il2::field_offset(inventory, "m_Items");
    api.inv_destroy = il2::find_method_params(inventory, "DestroyGear", {"GearItem"});
    api.remove_units = il2::find_method(inventory, "RemoveUnits", 2);
    if (Il2CppClass* snare = il2::find_class(kGame, "", "SnareItem")) {
        api.snare = il2::field_offset(gear, "m_SnareItem");
        api.snare_state = il2::field_offset(snare, "m_State");
        api.snare_rabbit = il2::field_offset(snare, "m_RabbitPrefab");
        api.snare_load = il2::find_method_params(snare, "Deserialize", {"System.String"});
        api.snare_placed = il2::find_method(snare, "OnSnarePlaced", 0);
        api.snare_set_state = il2::find_method(snare, "SetState", 1);
        api.snare_roll = il2::find_method(snare, "DoRoll", 0);
        api.snare_spawn_rabbit = il2::find_method(snare, "SpawnDeadRabbitOnSnare", 1);
    }
    if (Il2CppClass* pot = il2::find_class(kGame, "", "CookingPotItem")) {
        api.pot = il2::field_offset(gear, "m_CookingPotItem");
        api.pot_fire = il2::field_offset(pot, "m_FireBeingUsed");
        api.pot_food = il2::field_offset(pot, "m_GearItemBeingCooked");
        api.pot_hours = il2::field_offset(pot, "m_CookingElapsedHours");
        api.pot_state = il2::field_offset(pot, "m_CookingState");
        api.pot_cooked = il2::field_offset(pot, "m_PercentCooked");
        api.pot_ruined = il2::field_offset(pot, "m_PercentRuined");
        api.pot_take = il2::find_method(pot, "PickUpCookedItem", 0);
        api.pot_cook = il2::find_method(pot, "StartCooking", 1);
        api.pot_boil = il2::find_method(pot, "StartBoilingWater", 2);
        api.pot_melt = il2::find_method(pot, "StartMeltingSnow", 1);
        api.pot_dummy = il2::find_method(pot, "IsDummyPot", 0);
        api.pot_water = il2::field_offset(pot, "m_LitersWaterBeingBoiled");
        api.pot_snow = il2::field_offset(pot, "m_LitersSnowBeingMelted");
        api.pot_load = il2::find_method_params(pot, "Deserialize", {"System.String"});
        api.pot_drop_food = il2::find_method(pot, "DestroyGearItemBeingCooked", 0);
    }
    g_ready = api.component_go && api.go_active && api.go_component && api.get_position && api.get_parent && api.set_pose &&
              api.gear_type && api.in_inventory >= 0 && api.no_save >= 0 && api.object_guid >= 0 && api.guid >= 0 && api.cached_pdid >= 0 && api.guid_of_go && api.attach_guid &&
              api.create_save_data && api.sd_prefab >= 0 && api.sd_gear >= 0 && api.instantiate && api.pdid_object &&
              api.new_pdid && api.all_gear && api.destroy_gear && api.serialize_all && api.player_manager && api.inspected &&
              api.inspecting && api.stackable >= 0 && api.units >= 0 && api.inventory && api.inv_destroy && api.remove_units;
    const bool snares = g_ready && api.snare >= 0 && api.snare_state >= 0 && api.snare_load && api.snare_roll &&
                        hooks::install_method("SnareItem.DoRoll", api.snare_roll, reinterpret_cast<void*>(&roll_detour),
                                              reinterpret_cast<void**>(&g_roll));
    const bool pots =
        g_ready && api.pot >= 0 && api.pot_food >= 0 && api.pot_water >= 0 && api.pot_snow >= 0 && api.pot_load &&
        api.pot_drop_food && api.pot_take && api.pot_cook && api.pot_boil && api.pot_melt &&
        hooks::install_method("CookingPotItem.PickUpCookedItem", api.pot_take, reinterpret_cast<void*>(&pot_take_detour),
                              reinterpret_cast<void**>(&g_pot_take)) &&
        hooks::install_method("CookingPotItem.StartCooking", api.pot_cook, reinterpret_cast<void*>(&pot_cook_detour),
                              reinterpret_cast<void**>(&g_pot_cook)) &&
        hooks::install_method("CookingPotItem.StartBoilingWater", api.pot_boil, reinterpret_cast<void*>(&pot_boil_detour),
                              reinterpret_cast<void**>(&g_pot_boil)) &&
        hooks::install_method("CookingPotItem.StartMeltingSnow", api.pot_melt, reinterpret_cast<void*>(&pot_melt_detour),
                              reinterpret_cast<void**>(&g_pot_melt));
    logx::info("world items: %s%s%s", g_ready ? "ready (loose items are shared)" : "UNAVAILABLE (TLD API missing)",
               g_ready && !snares ? "; snares roll in each game (not shared)" : "",
               g_ready && !pots ? "; what goes in a pot isn't shared" : "");
    return g_ready;
}

void on_frame(bool in_game) {
    if (!g_ready) return;
    if (!in_game && game::active_scene() == "MainMenu") forget_world();
    const bool connected = netrt::status().connected;
    if (!connected && !netrt::offline()) {
        if (g_connected) reset("not connected");
        g_connected = false;
        return;
    }
    // Offline (a friend's link dropped mid-game, or we host with nobody linked): we go on. The friend's changes go to
    // the host once it's back; the host applies what it kept for a scene it walks into (kept with its save, too).
    if (!connected) g_newest.clear();  // the host's change counters may start over meanwhile (its game restarted)
    g_connected = true;
    const double now = now_ms();
    const std::string& settled = in_game ? core::settled_scene() : std::string();
    if (settled != g_scene) {
        if (!g_scene.empty()) logx::info("items: left %s", g_scene.c_str());
        g_known.clear();
        g_alias_in.clear();
        g_alias_out.clear();
        g_scene = settled;
        g_baseline = false;
        g_together.clear();
        g_have_list = false;
        g_put_back.clear();  // the host's list there brings them
        g_last_scan = now - kScanMs;  // first scan on the next frame
    }
    if (Il2CppObject* gi = inspected_now(il2::invoke(api.player_manager, nullptr)); gi != g_hold_gi) {
        g_hold_gi = gi;
        g_hold = gi ? pdid_of(gi) : std::string();
        g_hold_since = now;
    }
    if (!g_put_back.empty()) {  // refused takes: our copy went at the end of the last frame
        std::vector<net::ItemPutMsg> back;
        back.swap(g_put_back);
        for (const net::ItemPutMsg& m : back) apply_put(m, now);
    }
    receive(now);
    if (g_scene.empty()) return;
    if (now - g_last_scan >= kScanMs) {
        g_last_scan = now;
        scan(now);
    }
    // Host: once a friend's changes from while the link was down are in, our list has them.
    if (hosting())
        for (uint8_t id : g_together.arrived(g_baseline ? g_scene : std::string())) send_list(id);
}

void save_kept(std::vector<world_keep::Record>& out) {
    for (const auto& [scene, changes] : g_pending) {
        for (const auto& [pdid, c] : changes) {
            net::Writer w(c.put ? net::Msg::ItemPut : net::Msg::ItemGone, net::kMaxMessage);
            if (c.put)
                net::write_item_put(w, c.put_msg);
            else
                net::write_item_gone(w, c.gone_msg);
            if (w.ok()) out.push_back(world_keep::record(world_keep::Tag::ItemChange, w));
        }
    }
}

bool load_kept(const world_keep::Record& r) {
    const net::Packet p = world_keep::packet(r);
    net::Reader body = net::packet_body(p);
    Change c;
    c.put = p.type == net::Msg::ItemPut;
    if (c.put ? !net::read_item_put(body, c.put_msg) : p.type != net::Msg::ItemGone || !net::read_item_gone(body, c.gone_msg))
        return false;
    const std::string scene = c.put ? c.put_msg.scene : c.gone_msg.scene;
    const std::string pdid = c.put ? c.put_msg.pdid : c.gone_msg.pdid;
    g_pending[scene][pdid] = std::move(c);
    return true;
}

}  // namespace world_items
