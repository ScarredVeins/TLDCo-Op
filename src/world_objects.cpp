#include "world_objects.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "avatar.h"
#include "coop_death.h"
#include "core.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace world_objects {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr double kOursMs = 5000;  // client: our own change this recent may not be in the host's list yet
constexpr double kGoneMs = 1000;  // objects without a TLD id that a player broke down are checked this often
constexpr double kLocksMs = 500;  // locks and safes are looked at this often
constexpr double kPlacedMs = 300;  // a placement a player changed goes this long after (TLD is done with it, its stack too)
constexpr int32_t kRemoved = 1, kOnPlayer = 2;  // TLD.Placement.PlacementState (0 there, 1 gone, 2 carried, 3 hidden)
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone

using Kind = net::ObjectKind;

struct Vector3 {
    float x, y, z;
};

struct Api {
    // Unity
    const MethodInfo* component_go = nullptr;
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* object_name = nullptr;
    const MethodInfo* active_self = nullptr;      // GameObject.get_activeSelf()
    const MethodInfo* set_active = nullptr;       // GameObject.SetActive(bool)
    const MethodInfo* parent = nullptr;           // Transform.get_parent()
    const MethodInfo* get_component = nullptr;    // Component.GetComponent(Type)
    Il2CppObject* radial_type = nullptr;          // typeof(RadialObjectSpawner)
    Il2CppObject* random_type = nullptr;          // typeof(RandomSpawnObject)
    const MethodInfo* guid_of = nullptr;          // static ObjectGuid.GetGuidFromGameObject(GameObject, bool)
    // BreakDown
    FieldInfo* breakdowns = nullptr;              // static BreakDown.m_BreakDownObjects
    const MethodInfo* break_down = nullptr;       // BreakDown.DoBreakDown(bool spawnYieldObjects)
    // Harvestable
    FieldInfo* harvestables = nullptr;            // static HarvestableManager.m_Harvestables
    const MethodInfo* harvest = nullptr;          // Harvestable.Harvest()
    ptrdiff_t harvested = -1;                     // Harvestable.m_Harvested
    ptrdiff_t refresh = -1;                       // Harvestable.m_RefreshTime (the game hour it grows back)
    ptrdiff_t post_harvest = -1;                  // Harvestable.m_ActivateObjectPostHarvest (GameObject)
    // RandomSpawnObject: TLD picks which of a set (a backpack here or there, a car or not) is in the world
    const MethodInfo* spawns_save = nullptr;      // static RandomSpawnObjectManager.Serialize()
    const MethodInfo* spawns_load = nullptr;      // static RandomSpawnObjectManager.Deserialize(string)
    // For the dev listing of TLD's random spawners
    const MethodInfo* find_all = nullptr;         // static Object.FindObjectsOfType(Type, bool includeInactive)
    const MethodInfo* go_transform = nullptr;     // GameObject.get_transform()
    ptrdiff_t set_members = -1;                   // RandomSpawnObject.m_ObjectList (GameObject[])
    const MethodInfo* not_saved = nullptr;        // static RandomSpawnObject.ShouldSkipSerialization(GameObject)
    ptrdiff_t set_inited = -1;                    // RandomSpawnObject.m_Inited
    ptrdiff_t set_had_save = -1;                  // RandomSpawnObject.m_HadSaveData
    Il2CppObject* prefab_type = nullptr;          // typeof(PrefabSpawn)
    ptrdiff_t prefab_guid = -1;                   // PrefabSpawn.m_Guid
    ptrdiff_t prefab_done = -1;                   // PrefabSpawn.m_SpawnComplete
    ptrdiff_t prefab_saved = -1;                  // PrefabSpawn.m_UseSerialization
    // TLD's placements (PlaceableManager): which movable containers, corpses and decorations are where in a scene,
    // and which of them are gone. A new game picks where some of them are (a backpack here or at another spot).
    const MethodInfo* placements_save = nullptr;  // static TLD.Placement.PlaceableManager.SerializeScenePlacements()
    const MethodInfo* placements_load = nullptr;  // static PlaceableManager.DeserializeScenePlacements(List<...>)
    const MethodInfo* to_json = nullptr;          // static TLD.Serialization.SerializationUtils.SerializeObject(object)
    const MethodInfo* from_json = nullptr;        // static Newtonsoft.Json.JsonConvert.DeserializeObject(string, Type, settings)
    FieldInfo* json_settings = nullptr;           // static SerializationUtils.m_SerializerSettings (TLD's converters)
    Il2CppObject* placements_type = nullptr;      // typeof(List<PlaceableInfoSaveData>)
    ptrdiff_t placement_state = -1;               // PlaceableInfoSaveData.m_State (PlacementState)
    Il2CppObject* placeable_type = nullptr;       // typeof(TLD.Placement.Placeable)
    Il2CppObject* container_type = nullptr;       // typeof(Container)
    ptrdiff_t container_rolled = -1;              // Container.m_RolledSpawnChance
    Il2CppObject* xp_disabler_type = nullptr;     // typeof(DisableObjectForXPMode)
    Il2CppObject* mode_disabler_type = nullptr;   // typeof(TLD.Gameplay.DisableObjectForGameMode)
    // Locks and safes (milestone 10): on containers, lockers and safes (OpenClose) and doors (LoadScene)
    FieldInfo* containers = nullptr;              // static ContainerManager.m_Containers
    FieldInfo* open_closers = nullptr;            // static OpenCloseManager.m_OpenClosers
    FieldInfo* doors = nullptr;                   // static LoadScene.m_LoadScenesList
    ptrdiff_t container_lock = -1;                // Container.m_Lock
    ptrdiff_t container_safe = -1;                // Container.m_Safe
    ptrdiff_t interaction_lock = -1;              // LockedInteraction.<Lock>k__BackingField (OpenClose, LoadScene)
    ptrdiff_t open_close_safe = -1;               // OpenClose.m_Safe
    ptrdiff_t lock_state = -1;                    // Lock.m_LockState (0 locked, 1 unlocked, 2 forced)
    const MethodInfo* unlock_companion = nullptr; // Lock.UnlockCompanionLock(): its other side (a door's) opens too
    const MethodInfo* force_lock = nullptr;       // Lock.FinishForceLock(bool complete)
    ptrdiff_t safe_cracked = -1;                  // SafeCracking.m_Cracked
    const MethodInfo* crack_safe = nullptr;       // SafeCracking.UnlockSafe()
    // Placements a player moves, picks up or puts down (milestone 10): decorations, movable containers
    ptrdiff_t placeable_guid = -1;                // Placeable.m_Guid
    const MethodInfo* to_player = nullptr;        // static PlaceableManager.MoveToPlayer(Placeable): picked up
    const MethodInfo* placed = nullptr;           // DecorationItem.FinalizePlacementRecursive(bool success, bool isParent)
    const MethodInfo* into_container = nullptr;   // Container.AddDecorationItem(DecorationItem)
    const MethodInfo* placement_of = nullptr;     // static PlaceableManager.GetPlacementState(Placeable)
    const MethodInfo* pdid_object = nullptr;      // static PdidTable.GetGameObject(string)
    // For the dev commands
    Il2CppObject* decoration_type = nullptr;      // typeof(DecorationItem)
    const MethodInfo* pick_up = nullptr;          // DecorationItem.OnPickup() (the action picker's pick up)
    const MethodInfo* set_position = nullptr;     // Transform.set_position(Vector3)
    Il2CppObject* transform_type = nullptr;       // typeof(Transform)
    Il2CppObject* component_type = nullptr;       // typeof(Component)
    const MethodInfo* components = nullptr;       // GameObject.GetComponents(Type)
    const MethodInfo* safehouse = nullptr;        // static GameManager.GetSafehouseManager()
    ptrdiff_t customizing = -1;                   // SafehouseManager.m_IsCustomizing (TLD's decorating mode)
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

size_t count_of(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}
std::string name_of(Il2CppObject* o) {
    return alive(o) ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, o))) : std::string();
}
std::string short_id(const std::string& id) { return id.size() > 8 ? id.substr(0, 8) : id; }

float distance(const Vector3& a, const Vector3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool contains_nocase(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return needle.empty() || it != hay.end();
}

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

Il2CppObject* go_of(Il2CppObject* component) { return il2::invoke(api.component_go, component); }

Vector3 position_of(Il2CppObject* component) {
    Vector3 p{};
    il2::invoke_value(api.get_position, il2::invoke(api.component_transform, component), p);
    return p;
}

bool active(Il2CppObject* component) {
    Il2CppObject* go = go_of(component);
    bool on = false;
    return go && il2::invoke_value(api.active_self, go, on) && on;
}

void set_active(Il2CppObject* go, bool on) {
    void* args[] = {&on};
    if (go) il2::invoke(api.set_active, go, args);
}

// A limb or the like that a RadialObjectSpawner put there: breaking it down hands it back to the spawner's pool (TLD
// doesn't save it either), so it isn't put back.
bool spawned(Il2CppObject* component) {
    Il2CppObject* parent = il2::invoke(api.parent, il2::invoke(api.component_transform, component));
    if (!alive(parent) || !api.get_component || !api.radial_type) return false;
    void* args[] = {api.radial_type};
    return alive(il2::invoke(api.get_component, parent, args));
}

// What TLD's RandomSpawnObject sets decide on: the GameObjects in their m_ObjectList.
std::unordered_set<uintptr_t> set_members() {
    std::unordered_set<uintptr_t> out;
    if (!api.find_all || !api.random_type || api.set_members < 0) return out;
    bool inactive_too = true;
    void* args[] = {api.random_type, &inactive_too};
    Il2CppObject* sets = il2::invoke(api.find_all, nullptr, args);
    for (size_t i = 0; i < il2::array_length(sets); ++i) {
        Il2CppObject* members = get<Il2CppObject*>(il2::array_object_at(sets, i), api.set_members);
        for (size_t k = 0; k < il2::array_length(members); ++k)
            out.insert(reinterpret_cast<uintptr_t>(il2::array_object_at(members, k)));
    }
    return out;
}

// A backpack, box or corpse in one of those sets (itself or something it's under): whether it's there is the set's
// pick (TLD's random spawns, which the host's list carries), not something a player did.
bool random_pick(Il2CppObject* component, const std::unordered_set<uintptr_t>& members) {
    if (members.empty()) return false;
    for (Il2CppObject* t = il2::invoke(api.component_transform, component); alive(t); t = il2::invoke(api.parent, t))
        if (members.count(reinterpret_cast<uintptr_t>(go_of(t)))) return true;
    return false;
}

// The scene's placements as TLD's save writes them (JSON).
std::string placements_text() {
    if (!api.placements_save || !api.to_json) return {};
    Il2CppObject* list = il2::invoke(api.placements_save, nullptr);
    if (!list) return {};
    void* args[] = {list};
    return text(reinterpret_cast<Il2CppString*>(il2::invoke(api.to_json, nullptr, args)));
}

// TLD's own id for it (what its save names it by); name and position (cm) without one.
std::string id_of(Il2CppObject* o) {
    void* args[] = {go_of(o), nullptr};
    bool warn = false;
    args[1] = &warn;
    std::string id = args[0] ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.guid_of, nullptr, args))) : std::string();
    if (!id.empty()) return id;
    const Vector3 p = position_of(o);
    char key[160];
    snprintf(key, sizeof(key), "%s|%ld|%ld|%ld", name_of(o).c_str(), std::lround(p.x * 100), std::lround(p.y * 100),
             std::lround(p.z * 100));
    char out[24];
    snprintf(out, sizeof(out), "@%016llx", static_cast<unsigned long long>(fnv(key)));
    return out;
}

// Limbs and branches a RadialObjectSpawner put there have no TLD id (and TLD doesn't save them: the spawner does).
// Their spawner hides them (back in a pool) when the player is far away and puts them back later, so for them "not
// active" isn't "broken down": only what a player broke down counts (g_gone).
bool has_tld_id(const std::string& id) { return !id.empty() && id[0] != '@'; }

struct Object {
    Kind kind = Kind::BreakDown;
    Il2CppObject* o = nullptr;
};

template <typename F>
void each_in(FieldInfo* field, F f) {
    if (!field) return;
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(field, &list);
    if (!list) return;
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* o = il2::array_object_at(items, static_cast<size_t>(i)); alive(o)) f(o);
}

void list_into(FieldInfo* field, Kind kind, std::vector<Object>& out) {
    each_in(field, [&](Il2CppObject* o) { out.push_back(Object{kind, o}); });
}

bool is_lock(Kind k) { return k == Kind::Lock || k == Kind::Safe; }

// Locks and safes: on containers (Container.m_Lock / m_Safe), lockers and safes (OpenClose) and doors (LoadScene).
void locks_into(std::vector<Object>& out) {
    if (api.lock_state < 0 || api.safe_cracked < 0) return;
    std::unordered_set<Il2CppObject*> seen;
    auto add = [&](Kind kind, Il2CppObject* owner, ptrdiff_t field) {
        Il2CppObject* o = field >= 0 ? get<Il2CppObject*>(owner, field) : nullptr;
        if (alive(o) && seen.insert(o).second) out.push_back(Object{kind, o});
    };
    each_in(api.containers, [&](Il2CppObject* c) {
        add(Kind::Lock, c, api.container_lock);
        add(Kind::Safe, c, api.container_safe);
    });
    each_in(api.open_closers, [&](Il2CppObject* oc) {
        add(Kind::Lock, oc, api.interaction_lock);
        add(Kind::Safe, oc, api.open_close_safe);
    });
    each_in(api.doors, [&](Il2CppObject* door) { add(Kind::Lock, door, api.interaction_lock); });
}

std::vector<Object> lock_objects() {
    std::vector<Object> out;
    locks_into(out);
    return out;
}

std::vector<Object> all_objects() {
    std::vector<Object> out;
    list_into(api.breakdowns, Kind::BreakDown, out);
    list_into(api.harvestables, Kind::Harvestable, out);
    locks_into(out);
    return out;
}

// Broken down (TLD saves a BreakDown that isn't active as broken down) / harvested; a lock's LockState (0 locked);
// a safe cracked.
uint8_t value_of(const Object& x) {
    switch (x.kind) {
        case Kind::BreakDown: return active(x.o) ? 0 : 1;
        case Kind::Harvestable: return get<bool>(x.o, api.harvested) ? 1 : 0;
        case Kind::Lock: return static_cast<uint8_t>(get<int32_t>(x.o, api.lock_state));
        case Kind::Safe: return get<bool>(x.o, api.safe_cracked) ? 1 : 0;
    }
    return 0;
}
bool done(const Object& x) { return value_of(x) != 0; }

const char* kind_name(Kind k) {
    return k == Kind::BreakDown ? "breakdown" : k == Kind::Harvestable ? "plant" : k == Kind::Lock ? "lock"
         : k == Kind::Safe ? "safe" : k == Kind::Body ? "body" : "placeable";
}
const char* done_name(Kind k) {
    return k == Kind::BreakDown ? "broken down" : k == Kind::Harvestable ? "harvested" : k == Kind::Lock ? "open" : "cracked";
}
const char* placement_name(int32_t v) {
    return v == 0 ? "there" : v == kRemoved ? "gone" : v == kOnPlayer ? "carried" : v == 3 ? "inactive" : "unregistered";
}
const char* state_name(Kind k, uint8_t v) {
    if (k == Kind::Lock) return v == 0 ? "locked" : v == 1 ? "unlocked" : "forced";
    if (k == Kind::Safe) return v ? "cracked" : "locked";
    if (k == Kind::Placement) return placement_name(v);
    return v ? done_name(k) : "whole";
}
const char* verb(const net::ObjectState& s) {
    switch (s.kind) {
        case Kind::BreakDown: return s.done ? "broke down" : "put back";
        case Kind::Harvestable: return s.done ? "harvested" : "put back";
        case Kind::Lock: return s.done == 0 ? "locked" : s.done == 1 ? "unlocked" : "forced";
        case Kind::Safe: return s.done ? "cracked" : "locked";
        case Kind::Placement: return s.done == 0 ? "placed" : s.done == kRemoved ? "removed" : s.done == kOnPlayer ? "picked up" : "put away";
        case Kind::Body: return "died, leaving";
    }
    return "changed";
}

std::string key_of(Kind k, const std::string& id) {
    return (k == Kind::BreakDown ? "b/" : k == Kind::Harvestable ? "h/" : k == Kind::Lock ? "l/" : k == Kind::Safe ? "s/"
            : k == Kind::Body ? "d/" : "p/") + id;
}

net::ObjectState state_of(const Object& x, const std::string& id) {
    net::ObjectState s;
    s.kind = x.kind;
    s.done = value_of(x);
    s.refresh = x.kind == Kind::Harvestable && s.done ? get<float>(x.o, api.refresh) : 0.0f;
    s.id = id;
    return s;
}

// --- state ---------------------------------------------------------------------------------------------------------

std::string g_scene;                                   // the scene the state below is about ("" = none)
std::unordered_map<std::string, Object> g_by_key;      // this scene's objects (key_of)
netrt::Newest g_newest;                                // key -> each player's newest change applied
std::unordered_map<std::string, double> g_ours;        // key -> when we changed it here
uint32_t g_seq = 0;
bool g_applying = false;                               // our own change for the other player: not one to send
bool g_connected = false;
netrt::Together g_together;                            // host: friends in our scene (our list went to each as they came)
// Host: the client's changes in scenes we aren't in (made when we get there).
std::unordered_map<std::string, std::unordered_map<std::string, net::ObjectState>> g_store;
// Objects without a TLD id that a player broke down or harvested (in either game), per scene: broken down again
// whenever their spawner puts them back.
std::unordered_map<std::string, std::unordered_map<std::string, net::ObjectState>> g_gone;
double g_last_gone = 0;
std::optional<net::ObjectListMsg> g_list;              // client: the host's list for a scene we aren't in yet
std::unordered_map<uintptr_t, uint8_t> g_lock_seen;    // this scene's locks and safes -> their state when we last looked
double g_last_locks = 0;
struct Marked {
    double at = 0;
    std::string name;
};
std::unordered_map<std::string, Marked> g_marked;      // placements a player changed here (guid), not sent yet
double g_last_mark = 0;
std::unordered_map<std::string, std::string> g_placed; // guid -> its record as we last sent or loaded it

bool tracking() { return g_ready && g_connected && !g_scene.empty() && !g_applying; }

Il2CppObject* find(Kind kind, const std::string& id) {
    const std::string key = key_of(kind, id);
    if (const auto it = g_by_key.find(key); it != g_by_key.end() && alive(it->second.o)) return it->second.o;
    g_by_key.clear();
    for (const Object& x : all_objects()) g_by_key[key_of(x.kind, id_of(x.o))] = x;
    const auto it = g_by_key.find(key);
    return it != g_by_key.end() ? it->second.o : nullptr;
}

void reset(const char* why) {
    if (!g_newest.empty()) logx::info("objects: forgetting shared state (%s)", why);
    g_scene.clear();
    g_marked.clear();
    g_placed.clear();
    g_by_key.clear();
    g_newest.clear();
    g_ours.clear();
    g_list.reset();
    g_together.clear();
}

// What we keep of the world for later (changes for scenes someone isn't in) belongs to this game: a dropped link
// keeps it, back at the main menu (another save may load next) it goes.
void forget_world() {
    if (g_store.empty() && g_gone.empty()) return;
    logx::info("objects: back at the main menu; forgetting what was kept for %zu scene(s) and broken in %zu", g_store.size(),
               g_gone.size());
    g_store.clear();
    g_gone.clear();
}

// --- sending -------------------------------------------------------------------------------------------------------

void send_put(const net::ObjectState& s) {
    net::ObjectPutMsg m;
    m.scene = g_scene;
    m.seq = ++g_seq;
    m.object = s;
    net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);  // a placement or a body may be more than one datagram
    net::write_object_put(w, m);
    netrt::send_packet(w, true);
}

// Host: what one friend changed goes on to the others, as a change of ours (our counter: each friend's is its own).
void pass_on(uint8_t from, net::ObjectPutMsg m) {
    m.seq = ++g_seq;
    net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
    net::write_object_put(w, m);
    netrt::send_packet_except(from, w, true);
}

void remember(const std::string& scene, const net::ObjectState& s) {
    if (!has_tld_id(s.id) && s.done && !is_lock(s.kind)) g_gone[scene][key_of(s.kind, s.id)] = s;
}

void changed_here(const Object& x, const std::string& id, const std::string& name) {
    const net::ObjectState s = state_of(x, id);
    remember(g_scene, s);
    g_ours[key_of(x.kind, id)] = now_ms();
    send_put(s);
    logx::info("objects: we %s %s%s%s (%s); told the other player", verb(s), is_lock(s.kind) ? kind_name(s.kind) : "",
               is_lock(s.kind) ? " of " : "", name.c_str(), short_id(id).c_str());
}

// Host: everything broken down or harvested in our scene, for friend `to` who just came in: what TLD has (for objects
// with a TLD id) and what a player broke down without one.
void send_list(uint8_t to) {
    net::ObjectListMsg m;
    m.scene = g_scene;
    const auto members = set_members();
    for (const Object& x : all_objects()) {
        if (is_lock(x.kind)) {  // every one: whether it's locked at all is rolled in each game (Lock.RollLockedState)
            m.objects.push_back(state_of(x, id_of(x.o)));
            continue;
        }
        if (!done(x) || (x.kind == Kind::BreakDown && random_pick(x.o, members))) continue;  // a pick: in m.spawns
        std::string id = id_of(x.o);
        if (has_tld_id(id)) m.objects.push_back(state_of(x, id));
    }
    if (const auto it = g_gone.find(g_scene); it != g_gone.end())
        for (const auto& [key, s] : it->second) m.objects.push_back(s);
    for (net::ObjectState& s : coop_death::bodies_here()) m.objects.push_back(std::move(s));  // (what they hold: opened)
    if (api.spawns_save) m.spawns = text(reinterpret_cast<Il2CppString*>(il2::invoke(api.spawns_save, nullptr)));
    if (m.spawns.size() > net::kSpawnTextMax) {
        logx::warn("objects: TLD's random spawns here are too big to send (%zu chars)", m.spawns.size());
        m.spawns.clear();
    }
    m.placements = placements_text();
    if (m.placements.size() > net::kPlacementTextMax) {
        logx::warn("objects: TLD's placements here are too big to send (%zu chars)", m.placements.size());
        m.placements.clear();
    }
    net::Writer w(net::Msg::ObjectList, net::kMaxMessage);
    net::write_object_list(w, m);
    netrt::send_packet_to(to, w, true);
    logx::info("objects: the other player is here (%s); sent what's broken down or harvested here and its locks (%zu), TLD's random "
               "spawns (%zu picked, %zu chars) and placements (%zu, %zu chars) (player %u)", g_scene.c_str(),
               m.objects.size(), count_of(m.spawns, "\"m_ObjectName\""), m.spawns.size(),
               count_of(m.placements, "\"m_Guid\""), m.placements.size(), to);
}

// --- placements a player moves -------------------------------------------------------------------------------------
// TLD's placeables (decorations, movable containers and corpses) are where its save says, by id (PlaceableManager). What
// a player moves, puts down, picks up or puts in a container (DecorationItem.FinalizePlacementRecursive,
// PlaceableManager.MoveToPlayer, Container.AddDecorationItem) goes to the other game as that placeable's record
// (PlaceableInfoSaveData, as TLD's save writes it) once TLD is done with it (its stack moves too). One broken down goes
// as a breakdown (TLD's DoBreakDown in the other game removes its placement too).
// The other game loads it as loading a save does (PlaceableManager.DeserializeScenePlacements, of that one record):
// moved, made (FindOrCreateAndDeserialize, from its addressable) or destroyed. One a player carries off is gone there
// (TLD would put it in that player's pack).

std::string placeable_guid(Il2CppObject* placeable) {
    return alive(placeable) && api.placeable_guid >= 0 ? text(get<Il2CppString*>(placeable, api.placeable_guid)) : std::string();
}

Il2CppObject* placeable_on(Il2CppObject* component) {
    if (!alive(component) || !api.placeable_type || !api.get_component) return nullptr;
    void* args[] = {api.placeable_type};
    Il2CppObject* p = il2::invoke(api.get_component, component, args);
    return alive(p) ? p : nullptr;
}

void mark(const std::string& guid, const std::string& name) {
    if (!tracking() || guid.empty()) return;
    g_last_mark = now_ms();
    g_marked.try_emplace(guid, Marked{g_last_mark, name});
}

// The records of a JSON array of objects (TLD's placements), each as its text.
std::vector<std::string_view> json_records(const std::string& json) {
    std::vector<std::string_view> out;
    int depth = 0;
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (quoted) {
            if (c == '\\') ++i;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}' && depth > 0 && --depth == 0) {
            out.emplace_back(json.data() + start, i + 1 - start);
        }
    }
    return out;
}

// A field of a JSON record: a string's text, or a number (true, false, null) as written. Its first (`last`: its last)
// field of that name.
std::string json_field(std::string_view record, const std::string& key, bool last = false) {
    const std::string tag = "\"" + key + "\":";
    size_t at = last ? record.rfind(tag) : record.find(tag);
    if (at == std::string_view::npos) return {};
    at += tag.size();
    while (at < record.size() && record[at] == ' ') ++at;
    if (at < record.size() && record[at] == '"') {
        const size_t end = record.find('"', at + 1);  // (ids and names have no escapes)
        return end == std::string_view::npos ? std::string() : std::string(record.substr(at + 1, end - at - 1));
    }
    size_t end = at;
    while (end < record.size() && record[end] != ',' && record[end] != '}') ++end;
    return std::string(record.substr(at, end - at));
}

// Its PlacementState: TLD's save writes the name ("OnPlayer"), and leaves out what's default (there: no m_State).
int32_t record_state(std::string_view record) {
    static const char* const kNames[] = {"Active", "Removed", "OnPlayer", "Inactive", "Unregistered"};
    const std::string v = json_field(record, "m_State", true);
    for (int32_t i = 0; i < 5; ++i)
        if (v == kNames[i]) return i;
    return std::atoi(v.c_str());
}

// " at (x, y, z)" for a placeable that's there (its record has where: "m_Position":[x,y,z]).
std::string where_of(std::string_view record) {
    const std::string tag = "\"m_Position\":[";
    const size_t at = record.find(tag);
    if (at == std::string_view::npos || record_state(record) != 0) return {};
    const std::string list(record.substr(at + tag.size(), 96));
    float v[3] = {};
    if (sscanf(list.c_str(), "%f , %f , %f", &v[0], &v[1], &v[2]) != 3) return {};
    char text[96];
    snprintf(text, sizeof(text), " at (%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
    return text;
}

// The placeable's name here (by its id), or as its record has it.
std::string placeable_name(const std::string& guid, std::string_view record) {
    if (api.pdid_object) {
        void* args[] = {il2::il2cpp_string_new(guid.c_str())};
        if (Il2CppObject* go = il2::invoke(api.pdid_object, nullptr, args); alive(go)) return name_of(go);
    }
    const std::string name = json_field(record, "m_Name");
    return name.empty() || name == "null" ? "a placeable we don't have" : name;
}

// What a player changed here goes once TLD is done with it: each marked placeable's record, unless it's what we
// last sent or loaded.
void send_placements() {
    const double now = now_ms();
    if (g_marked.empty() || now - g_last_mark < kPlacedMs) return;
    const std::string json = placements_text();
    const std::vector<std::string_view> records = json_records(json);
    for (const auto& [guid, m] : g_marked) {
        std::string_view record;
        for (std::string_view r : records)
            if (json_field(r, "m_Guid") == guid) {
                record = r;
                break;
            }
        if (record.empty() || record.size() > net::kPlacementEntryMax) {
            logx::warn("objects: %s (%s) %s TLD's placements here; not sent", m.name.c_str(), short_id(guid).c_str(),
                       record.empty() ? "isn't in" : "has too big a record in");
            continue;
        }
        std::string& placed = g_placed[guid];
        if (placed == record) continue;
        placed = record;
        net::ObjectState s;
        s.kind = Kind::Placement;
        s.done = static_cast<uint8_t>(record_state(record));
        s.id = guid;
        s.text = placed;
        g_ours[key_of(s.kind, guid)] = now;
        send_put(s);
        logx::info("objects: we %s %s (%s)%s; told the other player", verb(s), m.name.c_str(), short_id(guid).c_str(),
                   where_of(record).c_str());
    }
    g_marked.clear();
}

// The other player's record of a placeable, loaded as loading a save does. One they carry is gone here. False when TLD
// couldn't read it.
bool apply_placement(const net::ObjectState& s) {
    if (!api.placements_load || !api.from_json || !api.placements_type || api.placement_state < 0 || s.text.empty())
        return false;
    Il2CppObject* settings = nullptr;
    if (api.json_settings) il2::il2cpp_field_static_get_value(api.json_settings, &settings);
    const std::string json = "[" + s.text + "]";
    void* args[] = {il2::il2cpp_string_new(json.c_str()), api.placements_type, settings};
    Il2CppObject* list = il2::invoke(api.from_json, nullptr, args);
    Il2CppObject* e = list && get<int32_t>(list, kListSize) == 1 ? il2::array_object_at(get<Il2CppObject*>(list, kListItems), 0)
                                                                 : nullptr;
    if (!e) return false;
    if (get<int32_t>(e, api.placement_state) == kOnPlayer) put<int32_t>(e, api.placement_state, kRemoved);
    g_applying = true;
    void* load_args[] = {list};
    il2::invoke(api.placements_load, nullptr, load_args);
    g_applying = false;
    g_placed[s.id] = s.text;
    g_marked.erase(s.id);
    return true;
}

// --- applying the other player's state -----------------------------------------------------------------------------

// Make one of our objects what the other player has, the way TLD does it: broken down as breaking it down does (without
// the yield, which the other player got), back as loading a save that has it whole does (BreakDown.Deserialize:
// active), harvested or not as Harvestable.Deserialize does. False when it already was.
bool apply(const Object& x, const net::ObjectState& s) {
    if (is_lock(x.kind)) {  // as loading a save does (Lock.Deserialize: the state; SafeCracking: cracked)
        if (value_of(x) == s.done) return false;
        if (x.kind == Kind::Lock) {
            put<int32_t>(x.o, api.lock_state, s.done);
            if (s.done && api.unlock_companion) il2::invoke(api.unlock_companion, x.o);  // as forcing it does
        } else {
            put<bool>(x.o, api.safe_cracked, s.done != 0);
        }
        g_lock_seen[reinterpret_cast<uintptr_t>(x.o)] = s.done;
        return true;
    }
    const bool want = s.done != 0;
    if (done(x) == want) return false;
    g_applying = true;
    if (x.kind == Kind::BreakDown) {
        if (want) {
            bool yield = false;
            void* args[] = {&yield};
            il2::invoke(api.break_down, x.o, args);
        } else {
            set_active(go_of(x.o), true);
        }
    } else {
        put<bool>(x.o, api.harvested, want);
        put<float>(x.o, api.refresh, want ? s.refresh : 0.0f);
        set_active(go_of(x.o), !want);
        if (Il2CppObject* after = get<Il2CppObject*>(x.o, api.post_harvest); alive(after)) set_active(after, want);
    }
    g_applying = false;
    return true;
}

void apply_stored() {
    const auto it = g_store.find(g_scene);
    if (it == g_store.end()) return;
    int applied = 0, missing = 0;
    for (const auto& [key, s] : it->second) {
        if (s.kind == Kind::Body) {
            if (coop_death::apply_body(s)) ++applied;
            continue;
        }
        if (s.kind == Kind::Placement) {
            const std::string name = placeable_name(s.id, s.text);
            if (!apply_placement(s)) {
                ++missing;
                continue;
            }
            ++applied;
            logx::info("objects: while we were elsewhere the other player %s %s (%s)%s", verb(s), name.c_str(),
                       short_id(s.id).c_str(), where_of(s.text).c_str());
            continue;
        }
        Il2CppObject* o = find(s.kind, s.id);
        if (!o) {
            ++missing;
            continue;
        }
        const Object x{s.kind, o};
        if (apply(x, s)) {
            ++applied;
            logx::info("objects: while we were elsewhere the other player %s %s (%s)", verb(s), name_of(o).c_str(),
                       short_id(s.id).c_str());
        }
    }
    g_store.erase(it);
    logx::info("objects: %d change(s) the other player made in %s while we were elsewhere%s", applied, g_scene.c_str(),
               missing ? " (some aren't here)" : "");
}

// Client: the host's list for our scene. What the host has broken down or harvested goes here too; what we have broken
// down or harvested and the host doesn't comes back (unless we did it just now: the host may not have heard yet).
// TLD's random spawns (which backpack, car, crate... of each set is there) as the host's game picked them, the way
// loading a save does: every set is emptied, then the saved picks are put back (RandomSpawnObject.LoadObject). A set
// that hasn't picked yet keeps what's loaded, as after a load.
// Gear a set picked (a camera, the salt and flour of Camp Office's kitchen) isn't in that list: TLD saves it with the
// rest of the gear (RandomSpawnObject.ShouldSkipSerialization) and the load switches it off all the same, for the gear
// load to put back. So it stays as it was here: the items lists decide on it (world_items), and switched off it would
// count as taken, in both games.
void load_spawns(const net::ObjectListMsg& m) {
    if (m.spawns.empty() || !api.spawns_load) return;
    std::vector<std::pair<Il2CppObject*, bool>> gear;  // set members TLD doesn't save in the list, and whether active
    if (api.not_saved && api.find_all && api.random_type && api.set_members >= 0) {
        bool inactive_too = true;
        void* args[] = {api.random_type, &inactive_too};
        Il2CppObject* sets = il2::invoke(api.find_all, nullptr, args);
        for (size_t i = 0; i < il2::array_length(sets); ++i) {
            Il2CppObject* members = get<Il2CppObject*>(il2::array_object_at(sets, i), api.set_members);
            for (size_t k = 0; k < il2::array_length(members); ++k) {
                Il2CppObject* go = il2::array_object_at(members, k);
                bool skip = false, on = false;
                void* item[] = {go};
                if (alive(go) && il2::invoke_value(api.not_saved, nullptr, skip, item) && skip &&
                    il2::invoke_value(api.active_self, go, on))
                    gear.emplace_back(go, on);
            }
        }
    }
    g_applying = true;
    void* args[] = {il2::il2cpp_string_new(m.spawns.c_str())};
    il2::invoke(api.spawns_load, nullptr, args);
    int back = 0;
    for (const auto& [go, on] : gear) {
        bool now = false;
        if (alive(go) && il2::invoke_value(api.active_self, go, now) && now != on) {
            set_active(go, on);
            ++back;
        }
    }
    g_applying = false;
    logx::info("objects: TLD's random spawns in %s are the host's now (%zu picked; %zu gear picks left as they were, %d "
               "put back)", m.scene.c_str(), count_of(m.spawns, "\"m_ObjectName\""), gear.size(), back);
}

// A placed container or corpse the host's world has is there: TLD's first-frame spawn-chance roll here
// (Container.UpdateContainer) mustn't hide it again. (Our rolls match the host's anyway, with its seed, indoors too;
// see world_mode. A building the host's save rolled before co-op may not.) How many were marked rolled.
int keep_host_rolls() {
    if (!api.find_all || !api.container_type || api.container_rolled < 0 || !api.placeable_type) return 0;
    bool inactive_too = false;
    void* args[] = {api.container_type, &inactive_too};
    Il2CppObject* containers = il2::invoke(api.find_all, nullptr, args);
    int kept = 0;
    for (size_t i = 0; i < il2::array_length(containers); ++i) {
        Il2CppObject* c = il2::array_object_at(containers, i);
        if (!alive(c) || get<bool>(c, api.container_rolled)) continue;
        void* type[] = {api.placeable_type};
        if (!alive(il2::invoke(api.get_component, c, type))) continue;
        put<bool>(c, api.container_rolled, true);
        ++kept;
    }
    return kept;
}

// TLD's placements in our scene as the host's game has them, the way loading a save does: what the host's world has
// elsewhere or not at all is moved or destroyed here too. What the host carries (OnPlayer) isn't in the world here
// either (TLD would put it in our pack).
void load_placements(const net::ObjectListMsg& m) {
    if (m.placements.empty() || !api.placements_load || !api.from_json || !api.placements_type || api.placement_state < 0)
        return;
    Il2CppObject* settings = nullptr;
    if (api.json_settings) il2::il2cpp_field_static_get_value(api.json_settings, &settings);
    void* args[] = {il2::il2cpp_string_new(m.placements.c_str()), api.placements_type, settings};
    Il2CppObject* list = il2::invoke(api.from_json, nullptr, args);
    if (!list) {
        logx::warn("objects: couldn't read the host's placements in %s (%zu chars)", m.scene.c_str(), m.placements.size());
        return;
    }
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t size = get<int32_t>(list, kListSize);
    int carried = 0;
    for (int32_t i = 0; i < size && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* e = il2::array_object_at(items, static_cast<size_t>(i));
        if (e && get<int32_t>(e, api.placement_state) == kOnPlayer) {
            put<int32_t>(e, api.placement_state, kRemoved);
            ++carried;
        }
    }
    g_applying = true;
    void* load_args[] = {list};
    il2::invoke(api.placements_load, nullptr, load_args);
    g_applying = false;
    const int kept = keep_host_rolls();
    logx::info("objects: TLD's placements in %s are the host's now (%d; %d the host carries, left out; %d container(s) "
               "kept as the host rolled them)", m.scene.c_str(), size, carried, kept);
}

void apply_list(const net::ObjectListMsg& m) {
    load_spawns(m);      // first: a set's pick decides whether a backpack or crate in it is there at all
    load_placements(m);  // and where a movable one is, or whether it's gone
    std::unordered_map<std::string, const net::ObjectState*> host;
    size_t bodies = 0, made = 0;
    for (const net::ObjectState& s : m.objects) {
        if (s.kind == Kind::Body) {  // a player's body the host's world has here
            ++bodies;
            made += coop_death::apply_body(s);
            continue;
        }
        host[key_of(s.kind, s.id)] = &s;
        remember(m.scene, s);
    }
    const double now = now_ms();
    int applied = 0, restored = 0, kept = 0;
    size_t matched = 0;
    int picks = 0;
    const auto members = set_members();
    for (const Object& x : all_objects()) {
        if (x.kind == Kind::BreakDown && random_pick(x.o, members)) {  // the host's random spawns decided it (load_spawns)
            ++picks;
            continue;
        }
        const std::string id = id_of(x.o), key = key_of(x.kind, id);
        if (const auto it = host.find(key); it != host.end()) {
            ++matched;
            if (is_lock(x.kind) && done(x) && !it->second->done) {
                if (const auto ours = g_ours.find(key); ours != g_ours.end() && now - ours->second < kOursMs) {
                    ++kept;  // we just opened it: the host hasn't heard yet
                    continue;
                }
            }
            if (apply(x, *it->second)) {
                ++applied;
                logx::info("objects: the host's %s %s (%s) is %s", kind_name(x.kind), name_of(x.o).c_str(), short_id(id).c_str(),
                           state_name(x.kind, it->second->done));
            }
            continue;
        }
        if (!done(x) || !has_tld_id(id) || is_lock(x.kind)) continue;
        if (const auto ours = g_ours.find(key); ours != g_ours.end() && now - ours->second < kOursMs) {
            ++kept;
            continue;
        }
        net::ObjectState whole = state_of(x, id);
        whole.done = 0;
        if (apply(x, whole)) {
            ++restored;
            logx::info("objects: %s %s (%s) isn't %s in the host's world; put back", kind_name(x.kind), name_of(x.o).c_str(),
                       short_id(id).c_str(), done_name(x.kind));
        }
    }
    size_t locks = 0;
    for (const net::ObjectState& s : m.objects) locks += s.kind == net::ObjectKind::Lock || s.kind == net::ObjectKind::Safe;
    logx::info("objects: the host has %zu broken down or harvested in %s, and %zu locks and safes: %d made so here, %d put "
               "back, %d ours kept, %zu not here (%d random spawn picks left to the spawns); %zu body(ies), %zu made here",
               m.objects.size() - locks - bodies, g_scene.c_str(), locks, applied, restored, kept,
               m.objects.size() - bodies - matched, picks, bodies, made);
}

// What a player broke down without a TLD id, put back by its spawner: broken down again.
void keep_gone() {
    const auto it = g_gone.find(g_scene);
    if (it == g_gone.end() || it->second.empty()) return;
    for (const Object& x : all_objects()) {
        if (done(x) || is_lock(x.kind)) continue;
        const std::string id = id_of(x.o);
        if (has_tld_id(id)) continue;
        const auto gone = it->second.find(key_of(x.kind, id));
        if (gone == it->second.end()) continue;
        if (apply(x, gone->second))
            logx::info("objects: %s (%s) is back (its spawner put it there again); %s again", name_of(x.o).c_str(),
                       short_id(id).c_str(), done_name(x.kind));
    }
}

void receive_put(const net::Packet& p, const net::ObjectPutMsg& m) {
    const net::ObjectState& s = m.object;
    const std::string key = key_of(s.kind, s.id);
    if (!g_newest.take(p, key, m.seq)) return;  // a late older change
    if (hosting()) pass_on(p.from, m);
    remember(m.scene, s);
    if (m.scene != g_scene) {
        if (hosting()) {
            g_store[m.scene][key] = s;
            logx::info("objects: the other player %s %s %s in %s; kept for when we get there", verb(s), kind_name(s.kind),
                       short_id(s.id).c_str(), m.scene.c_str());
        }
        return;  // client: the host's list tells us when we get there
    }
    if (s.kind == Kind::Body) {
        const bool made = coop_death::apply_body(s);
        logx::info("objects: %s died here, leaving a body (%s)%s", coop_death::body_name(s).c_str(), short_id(s.id).c_str(),
                   made ? "" : "; it already was here");
        return;
    }
    if (s.kind == Kind::Placement) {
        const std::string name = placeable_name(s.id, s.text);  // (before: it may go)
        const bool applied = apply_placement(s);
        logx::info("objects: the other player %s %s (%s)%s%s", verb(s), name.c_str(), short_id(s.id).c_str(),
                   where_of(s.text).c_str(), applied ? "; same here now" : "; TLD couldn't read its record");
        return;
    }
    Il2CppObject* o = find(s.kind, s.id);
    if (!o) {
        logx::warn("objects: the other player's %s %s isn't in our %s", kind_name(s.kind), short_id(s.id).c_str(), g_scene.c_str());
        return;
    }
    const Object x{s.kind, o};
    if (is_lock(s.kind) && !s.done && done(x)) {
        logx::info("objects: the other player's %s %s (%s) is %s; ours is open: kept", kind_name(s.kind), name_of(o).c_str(),
                   short_id(s.id).c_str(), state_name(s.kind, s.done));
        return;
    }
    const bool changed = apply(x, s);
    logx::info("objects: the other player %s %s (%s)%s", verb(s), name_of(o).c_str(), short_id(s.id).c_str(),
               changed ? "; same here now" : "; it already was here");
}

// Locks and safes have no one place TLD opens them (forced, a keypad, its other side opening, a safe's dial): we look
// twice a second. The first look in a scene only takes them in.
void scan_locks() {
    for (const Object& x : lock_objects()) {
        const uint8_t v = value_of(x);
        const auto [it, fresh] = g_lock_seen.try_emplace(reinterpret_cast<uintptr_t>(x.o), v);
        if (fresh || it->second == v) continue;
        it->second = v;
        if (v) changed_here(x, id_of(x.o), name_of(x.o));  // (locked again only by TLD loading: not a change)
    }
}

void receive() {
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::ObjectPut, packets);
    netrt::drain_packets(net::Msg::ObjectList, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        if (p.type == net::Msg::ObjectPut) {
            net::ObjectPutMsg m;
            if (net::read_object_put(r, m)) receive_put(p, m);
        } else if (p.type == net::Msg::ObjectList && !hosting()) {
            net::ObjectListMsg m;
            if (!net::read_object_list(r, m)) continue;
            if (m.scene == g_scene)
                apply_list(m);
            else
                g_list = std::move(m);  // for when we're in
        }
    }
}

// --- hooks ---------------------------------------------------------------------------------------------------------

using BreakDownFn = void (*)(Il2CppObject* self, bool spawn_yield, const MethodInfo* method);
using HarvestFn = void (*)(Il2CppObject* self, const MethodInfo* method);
BreakDownFn g_break_down = nullptr;
HarvestFn g_harvest = nullptr;

void break_down_detour(Il2CppObject* self, bool spawn_yield, const MethodInfo* method) {
    const bool track = tracking() && alive(self) && active(self);  // (TLD also breaks down saved ones on load)
    const std::string id = track ? id_of(self) : std::string(), name = track ? name_of(self) : std::string();
    g_break_down(self, spawn_yield, method);
    if (track && !active(self)) changed_here(Object{Kind::BreakDown, self}, id, name);
}

using PlacedFn = void (*)(Il2CppObject* self, bool success, bool is_parent, const MethodInfo* method);
using ToPlayerFn = void (*)(Il2CppObject* placeable, const MethodInfo* method);
using IntoContainerFn = void (*)(Il2CppObject* self, Il2CppObject* decoration, const MethodInfo* method);
PlacedFn g_placed_fn = nullptr;
ToPlayerFn g_to_player = nullptr;
IntoContainerFn g_into_container = nullptr;

void placed_detour(Il2CppObject* self, bool success, bool is_parent, const MethodInfo* method) {
    g_placed_fn(self, success, is_parent, method);
    if (success && tracking()) {  // put down from the pack, or moved where it is
        Il2CppObject* p = placeable_on(self);
        mark(placeable_guid(p), name_of(p));
    }
}

void to_player_detour(Il2CppObject* placeable, const MethodInfo* method) {
    g_to_player(placeable, method);
    if (tracking()) mark(placeable_guid(placeable), name_of(placeable));
}

void into_container_detour(Il2CppObject* self, Il2CppObject* decoration, const MethodInfo* method) {
    Il2CppObject* p = tracking() ? placeable_on(decoration) : nullptr;  // (TLD destroys it)
    const std::string guid = placeable_guid(p), name = name_of(p);
    g_into_container(self, decoration, method);
    mark(guid, name);
}

void harvest_detour(Il2CppObject* self, const MethodInfo* method) {
    const bool track = tracking() && alive(self) && !get<bool>(self, api.harvested);
    const std::string id = track ? id_of(self) : std::string(), name = track ? name_of(self) : std::string();
    g_harvest(self, method);  // (may destroy it: what's read after this is managed fields only)
    if (track && get<bool>(self, api.harvested)) {
        net::ObjectState s;
        s.kind = Kind::Harvestable;
        s.done = 1;
        s.refresh = get<float>(self, api.refresh);
        s.id = id;
        remember(g_scene, s);
        g_ours[key_of(s.kind, id)] = now_ms();
        send_put(s);
        logx::info("objects: we %s %s (%s); told the other player", verb(s), name.c_str(), short_id(id).c_str());
    }
}

// --- test helpers --------------------------------------------------------------------------------------------------

Vector3 player_pos() {
    const game::PlayerState p = game::read_player();
    return Vector3{p.position.x, p.position.y, p.position.z};
}

Il2CppObject* nearest(Kind kind, const std::string& name, float max_metres, float* metres) {
    const Vector3 me = player_pos();
    Il2CppObject* best = nullptr;
    float best_d = max_metres;
    for (const Object& x : all_objects()) {
        if (x.kind != kind || done(x) || !active(x.o)) continue;
        const float d = distance(position_of(x.o), me);
        if (d <= best_d && contains_nocase(name_of(x.o), name)) best = x.o, best_d = d;
    }
    if (metres) *metres = best_d;
    return best;
}

}  // namespace

void describe(float metres, size_t most) {
    if (!g_ready) return;
    const Vector3 me = player_pos();
    std::vector<std::pair<float, Object>> nearby;
    int breakdowns = 0, plants = 0, broken = 0, harvested = 0, locks = 0, open = 0;
    for (const Object& x : all_objects()) {
        const bool is_done = done(x);
        if (x.kind == Kind::BreakDown) ++breakdowns, broken += is_done;
        else if (x.kind == Kind::Harvestable) ++plants, harvested += is_done;
        else ++locks, open += is_done;
        const float d = distance(position_of(x.o), me);
        if (d <= metres) nearby.emplace_back(d, x);
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto members = set_members();
    logx::info("dev: objects: %zu within %.0f m; %d breakdowns (%d broken down), %d plants (%d harvested), %d locks and "
               "safes (%d open) in %s", nearby.size(), metres, breakdowns, broken, plants, harvested, locks, open, g_scene.c_str());
    for (size_t i = 0; i < nearby.size() && i < most; ++i) {
        const Object& x = nearby[i].second;
        const Vector3 p = position_of(x.o);
        std::string where;  // its parents, nearest first (three at most)
        Il2CppObject* t = il2::invoke(api.parent, il2::invoke(api.component_transform, x.o));
        for (int up = 0; up < 3 && alive(t); ++up, t = il2::invoke(api.parent, t)) where += (up ? " < " : "") + name_of(t);
        const bool pick = x.kind == Kind::BreakDown && random_pick(x.o, members);
        std::string has;  // TLD components that decide whether it's there
        auto with = [&](Il2CppObject* type, const char* what) {
            void* args[] = {type};
            if (type && alive(il2::invoke(api.get_component, x.o, args))) has += what;
        };
        with(api.placeable_type, ", placeable");
        with(api.xp_disabler_type, ", XP-mode disabler");
        with(api.mode_disabler_type, ", game-mode disabler");
        logx::info("dev:   %s %s %s at (%.1f, %.1f, %.1f), %.1f m, %s%s%s%s, in %s", short_id(id_of(x.o)).c_str(),
                   kind_name(x.kind), name_of(x.o).c_str(), p.x, p.y, p.z, nearby[i].first,
                   pick ? (done(x) ? "a random spawn, not there" : "a random spawn, there")
                        : state_name(x.kind, value_of(x)),
                   active(x.o) ? "" : ", hidden",
                   x.kind == Kind::BreakDown && spawned(x.o) ? ", spawned" : "", has.c_str(), where.empty() ? "-" : where.c_str());
    }
}

// TLD's random spawners within `metres`: each RandomSpawnObject set with its members (and the breakdowns at or below
// each), then each PrefabSpawn with what it spawned.
void describe_spawners(float metres) {
    if (!g_ready || !api.find_all || !api.random_type || api.set_members < 0) {
        logx::warn("dev: spawners: unavailable");
        return;
    }
    const Vector3 me = player_pos();
    std::unordered_map<uintptr_t, std::vector<Il2CppObject*>> under;  // GameObject -> the breakdowns at or below it
    for (const Object& x : all_objects()) {
        if (x.kind != Kind::BreakDown) continue;
        for (Il2CppObject* t = il2::invoke(api.component_transform, x.o); alive(t); t = il2::invoke(api.parent, t))
            under[reinterpret_cast<uintptr_t>(go_of(t))].push_back(x.o);
    }
    auto prefab_of = [](Il2CppObject* go) -> Il2CppObject* {
        if (!api.prefab_type || !alive(go)) return nullptr;
        void* args[] = {api.prefab_type};
        Il2CppObject* p = il2::invoke(api.get_component, il2::invoke(api.go_transform, go), args);
        return alive(p) ? p : nullptr;
    };
    auto prefab_text = [](Il2CppObject* p) {
        char buf[160];
        snprintf(buf, sizeof(buf), "; spawner %s%s%s", short_id(text(get<Il2CppString*>(p, api.prefab_guid))).c_str(),
                 get<bool>(p, api.prefab_done) ? ", spawned" : ", not spawned yet",
                 get<bool>(p, api.prefab_saved) ? ", saved" : "");
        return std::string(buf);
    };
    auto list_under = [&](Il2CppObject* go) {
        const auto it = under.find(reinterpret_cast<uintptr_t>(go));
        if (it == under.end()) return;
        for (Il2CppObject* b : it->second)
            logx::info("dev:       %s %s%s", short_id(id_of(b)).c_str(), name_of(b).c_str(), active(b) ? "" : " (hidden)");
    };
    bool inactive_too = true;
    void* set_args[] = {api.random_type, &inactive_too};
    Il2CppObject* sets = il2::invoke(api.find_all, nullptr, set_args);
    int shown = 0;
    std::unordered_map<uintptr_t, bool> in_set;
    for (size_t i = 0; i < il2::array_length(sets); ++i) {
        Il2CppObject* s = il2::array_object_at(sets, i);
        Il2CppObject* members = get<Il2CppObject*>(s, api.set_members);
        for (size_t k = 0; k < il2::array_length(members); ++k)
            in_set[reinterpret_cast<uintptr_t>(il2::array_object_at(members, k))] = true;
        const Vector3 p = position_of(s);
        float d = distance(p, me);  // a set may sit anywhere (at the scene's origin, say): its nearest member counts
        for (size_t k = 0; k < il2::array_length(members); ++k) {
            Il2CppObject* go = il2::array_object_at(members, k);
            Vector3 at{};
            if (alive(go) && il2::invoke_value(api.get_position, il2::invoke(api.go_transform, go), at))
                d = std::min(d, distance(at, me));
        }
        if (d > metres) continue;
        ++shown;
        logx::info("dev:   set %s at (%.1f, %.1f, %.1f), %.1f m: %zu members%s%s", name_of(s).c_str(), p.x, p.y, p.z, d,
                   il2::array_length(members), get<bool>(s, api.set_inited) ? ", picked" : ", not picked yet",
                   get<bool>(s, api.set_had_save) ? ", from the save" : "");
        for (size_t k = 0; k < il2::array_length(members); ++k) {
            Il2CppObject* go = il2::array_object_at(members, k);
            Il2CppObject* prefab = prefab_of(go);
            bool on = false;
            if (alive(go)) il2::invoke_value(api.active_self, go, on);
            Vector3 at{};
            if (alive(go)) il2::invoke_value(api.get_position, il2::invoke(api.go_transform, go), at);
            logx::info("dev:     %s at (%.1f, %.1f, %.1f)%s%s", alive(go) ? name_of(go).c_str() : "(gone)", at.x, at.y, at.z,
                       on ? ", there" : ", not there", prefab ? prefab_text(prefab).c_str() : "");
            if (alive(go)) list_under(go);
        }
    }
    int loose = 0;
    size_t prefabs = 0;
    if (api.prefab_type) {
        void* prefab_args[] = {api.prefab_type, &inactive_too};
        Il2CppObject* spawners = il2::invoke(api.find_all, nullptr, prefab_args);
        prefabs = il2::array_length(spawners);
        for (size_t i = 0; i < il2::array_length(spawners); ++i) {
            Il2CppObject* p = il2::array_object_at(spawners, i);
            Il2CppObject* go = go_of(p);
            if (in_set.count(reinterpret_cast<uintptr_t>(go))) continue;
            const Vector3 at = position_of(p);
            const float d = distance(at, me);
            if (d > metres) continue;
            ++loose;
            logx::info("dev:   prefab spawn %s at (%.1f, %.1f, %.1f), %.1f m%s%s", name_of(p).c_str(), at.x, at.y, at.z, d,
                       prefab_text(p).c_str(), active(p) ? "" : ", hidden");
            list_under(go);
        }
    }
    logx::info("dev: spawners: %d random sets (of %zu) and %d other prefab spawners (of %zu) within %.0f m in %s", shown,
               il2::array_length(sets), loose, prefabs, metres, g_scene.c_str());
}

// TLD's placements in this scene (PlaceableManager: which movable containers and corpses are where, and which of
// them are gone), as its save writes them.
void describe_placements() {
    if (!api.placements_save || !api.to_json) {
        logx::warn("dev: placements: unavailable");
        return;
    }
    const std::string json = placements_text();
    logx::info("dev: placements in %s: %zu chars, %zu entries", g_scene.c_str(), json.size(), count_of(json, "\"m_Guid\""));
    for (size_t at = 0; at < json.size(); at += 1500) logx::info("dev:   %s", json.substr(at, 1500).c_str());
}

// Dev: TLD's decorations within 10 m and those we carry (`what` empty), or pick up / move / put down one whose name
// contains `name` (or whose id starts with it): "take" (the nearest within 6 m, as the action picker's pick up does),
// "move" (the nearest within 6 m, half a metre along x, as finishing TLD's placement does), "place" (one we carry, a
// metre ahead of us). TLD places only in its decorating mode (SafehouseManager.m_IsCustomizing): on for the call.
bool decorations(const std::string& what, const std::string& name) {
    if (!g_ready || !api.find_all || !api.decoration_type || !api.placement_of) {
        logx::warn("dev: decor: unavailable");
        return false;
    }
    const game::PlayerState me_state = game::read_player();
    const Vector3 me = player_pos();
    bool inactive_too = true;
    void* args[] = {api.decoration_type, &inactive_too};
    Il2CppObject* all = il2::invoke(api.find_all, nullptr, args);
    auto state_of_placeable = [](Il2CppObject* p) {
        int32_t v = -1;
        void* a[] = {p};
        if (p) il2::invoke_value(api.placement_of, nullptr, v, a);
        return v;
    };
    Il2CppObject* best = nullptr;
    float best_d = 6.0f;
    int listed = 0;
    for (size_t i = 0; i < il2::array_length(all); ++i) {
        Il2CppObject* d = il2::array_object_at(all, i);
        Il2CppObject* p = placeable_on(d);
        if (!alive(d) || !p) continue;
        const int32_t state = state_of_placeable(p);
        const Vector3 at = position_of(d);
        const float dist = distance(at, me);
        if (what.empty()) {
            if (dist > 10.0f && state != kOnPlayer) continue;
            ++listed;
            logx::info("dev:   %s %s at (%.2f, %.2f, %.2f), %.1f m, %s%s", short_id(placeable_guid(p)).c_str(), name_of(d).c_str(),
                       at.x, at.y, at.z, dist, placement_name(state), active(d) ? "" : ", hidden");
            continue;
        }
        if (!contains_nocase(name_of(d), name) && placeable_guid(p).rfind(name, 0) != 0) continue;  // a name or an id
        if (what == "place" ? state == kOnPlayer : state == 0 && active(d) && dist <= best_d)
            best = d, best_d = what == "place" ? 0 : dist;
    }
    if (what.empty()) {
        logx::info("dev: decor: %d within 10 m or carried, of %zu in %s", listed, il2::array_length(all), g_scene.c_str());
        return true;
    }
    if (!best) {
        logx::warn("dev: decor: no %s decoration%s%s", what == "place" ? "carried" : "placed", name.empty() ? "" : " like ",
                   name.c_str());
        return false;
    }
    const std::string label = name_of(best), guid = placeable_guid(placeable_on(best));
    if (what == "take") {
        il2::invoke(api.pick_up, best);
    } else if (what == "move" || what == "place") {
        Vector3 to = position_of(best);
        if (what == "move") {
            to.x += 0.5f;
        } else {
            const float yaw = me_state.yaw * 3.14159265f / 180.0f;
            to = Vector3{me.x + std::sin(yaw), me.y, me.z + std::cos(yaw)};
        }
        void* pos_args[] = {&to};
        il2::invoke(api.set_position, il2::invoke(api.component_transform, best), pos_args);
        Il2CppObject* safehouse = api.safehouse && api.customizing >= 0 ? il2::invoke(api.safehouse, nullptr) : nullptr;
        const bool was = safehouse && get<bool>(safehouse, api.customizing);
        if (safehouse) put<bool>(safehouse, api.customizing, true);
        bool success = true, is_parent = true;
        void* place_args[] = {&success, &is_parent};
        il2::invoke(api.placed, best, place_args);
        if (safehouse) put<bool>(safehouse, api.customizing, was);
    } else {
        logx::warn("dev: decor: take, move or place, not '%s'", what.c_str());
        return false;
    }
    const Vector3 at = position_of(best);
    logx::info("dev: decor: %s %s (%s): %s at (%.2f, %.2f, %.2f)", what.c_str(), label.c_str(), short_id(guid).c_str(),
               placement_name(state_of_placeable(placeable_on(best))), at.x, at.y, at.z);
    return true;
}

// Dev: GameObjects (active or not) whose name contains `name`, the first `most`: where, whether active, their parents
// and their components.
void describe_game_objects(const std::string& name, size_t most) {
    if (!g_ready || !api.find_all || !api.transform_type || !api.components) {
        logx::warn("dev: go: unavailable");
        return;
    }
    const Vector3 me = player_pos();
    bool inactive_too = true;
    void* args[] = {api.transform_type, &inactive_too};
    Il2CppObject* all = il2::invoke(api.find_all, nullptr, args);
    size_t found = 0;
    for (size_t i = 0; i < il2::array_length(all); ++i) {
        Il2CppObject* t = il2::array_object_at(all, i);
        if (!alive(t) || !contains_nocase(name_of(t), name) || found++ >= most) continue;
        Il2CppObject* go = go_of(t);
        bool on = false;
        if (go) il2::invoke_value(api.active_self, go, on);
        std::string where;  // its parents, nearest first (three at most)
        Il2CppObject* up = il2::invoke(api.parent, t);
        for (int n = 0; n < 3 && alive(up); ++n, up = il2::invoke(api.parent, up)) where += (n ? " < " : "") + name_of(up);
        std::string has;
        bool light = false;
        void* type_args[] = {api.component_type};
        Il2CppObject* parts = go ? il2::invoke(api.components, go, type_args) : nullptr;
        for (size_t k = 0; k < il2::array_length(parts); ++k)
            if (Il2CppObject* c = il2::array_object_at(parts, k)) {
                const std::string cls = il2::il2cpp_class_get_name(il2::il2cpp_object_get_class(c));
                has += (has.empty() ? "" : ", ") + cls;
                light = light || cls == "Light";
            }
        const Vector3 p = position_of(t);
        logx::info("dev:   %s at (%.1f, %.1f, %.1f), %.1f m, %s, in %s: %s%s%s", name_of(t).c_str(), p.x, p.y, p.z,
                   distance(p, me), on ? "active" : "inactive", where.empty() ? "-" : where.c_str(), has.c_str(),
                   light ? " | " : "", light ? avatar::describe_lights(go).c_str() : "");
    }
    logx::info("dev: go: %zu named like '%s' in %s (of %zu)", found, name.c_str(), g_scene.c_str(), il2::array_length(all));
}

bool break_down(const std::string& name) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* o = nearest(Kind::BreakDown, name, 6.0f, &d);
    if (!o) {
        logx::warn("dev: breakdown: nothing%s%s to break down within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    const std::string what = name_of(o), id = id_of(o);
    bool yield = true;  // what finishing TLD's break down panel does (Panel_BreakDown.BreakDownFinished)
    void* args[] = {&yield};
    il2::invoke(api.break_down, o, args);
    logx::info("dev: breakdown: %s (%s), %.1f m away: %s", what.c_str(), short_id(id).c_str(), d,
               active(o) ? "still there" : "broken down");
    return true;
}

bool force(const std::string& name) {
    if (!g_ready || !api.force_lock) return false;
    float d = 0;
    Il2CppObject* o = nearest(Kind::Lock, name, 6.0f, &d);
    if (!o) {
        logx::warn("dev: force: no locked lock%s%s within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    const std::string what = name_of(o), id = id_of(o);
    bool complete = true;  // what finishing TLD's force (a prybar's progress bar) does: LockedInteraction.FinishForceLock
    void* args[] = {&complete};
    il2::invoke(api.force_lock, o, args);
    logx::info("dev: force: %s (%s), %.1f m away: %s", what.c_str(), short_id(id).c_str(), d,
               state_name(Kind::Lock, value_of(Object{Kind::Lock, o})));
    return true;
}

bool crack(const std::string& name) {
    if (!g_ready || !api.crack_safe) return false;
    float d = 0;
    Il2CppObject* o = nearest(Kind::Safe, name, 6.0f, &d);
    if (!o) {
        logx::warn("dev: crack: no locked safe%s%s within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    const std::string what = name_of(o), id = id_of(o);
    il2::invoke(api.crack_safe, o);  // the dial's last tumbler (SafeCracking.UnlockCurrentTumbler)
    logx::info("dev: crack: %s (%s), %.1f m away: %s", what.c_str(), short_id(id).c_str(), d,
               state_name(Kind::Safe, value_of(Object{Kind::Safe, o})));
    return true;
}

bool harvest(const std::string& name) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* o = nearest(Kind::Harvestable, name, 6.0f, &d);
    if (!o) {
        logx::warn("dev: plant: no plant%s%s to harvest within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    const std::string what = name_of(o), id = id_of(o);
    il2::invoke(api.harvest, o);  // what HarvestableInteraction.PerformInteraction does
    logx::info("dev: plant: %s (%s), %.1f m away: %s", what.c_str(), short_id(id).c_str(), d,
               get<bool>(o, api.harvested) ? "harvested" : "NOT harvested");
    return true;
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* breakdown = il2::find_class(kGame, "", "BreakDown");
    Il2CppClass* harvestable = il2::find_class(kGame, "", "Harvestable");
    Il2CppClass* harvestables = il2::find_class(kGame, "", "HarvestableManager");
    Il2CppClass* radial = il2::find_class(kGame, "", "RadialObjectSpawner");
    Il2CppClass* random_set = il2::find_class(kGame, "", "RandomSpawnObject");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.object_name = il2::find_method(object, "get_name", 0);
    api.active_self = il2::find_method(game_object, "get_activeSelf", 0);
    api.set_active = il2::find_method(game_object, "SetActive", 1);
    api.parent = il2::find_method(transform, "get_parent", 0);
    api.get_component = il2::find_method_params(component, "GetComponent", {"System.Type"});
    api.radial_type = radial ? il2::type_object(radial) : nullptr;
    if (api.radial_type) il2::il2cpp_gchandle_new(api.radial_type, false);
    api.random_type = random_set ? il2::type_object(random_set) : nullptr;
    if (api.random_type) il2::il2cpp_gchandle_new(api.random_type, false);
    api.guid_of = il2::find_method_params(il2::find_class(kGame, "", "ObjectGuid"), "GetGuidFromGameObject",
                                          {"UnityEngine.GameObject", "System.Boolean"});
    if (breakdown) api.breakdowns = il2::il2cpp_class_get_field_from_name(breakdown, "m_BreakDownObjects");
    api.break_down = il2::find_method_params(breakdown, "DoBreakDown", {"System.Boolean"});
    if (harvestables) api.harvestables = il2::il2cpp_class_get_field_from_name(harvestables, "m_Harvestables");
    api.harvest = il2::find_method(harvestable, "Harvest", 0);
    api.harvested = il2::field_offset(harvestable, "m_Harvested");
    api.refresh = il2::field_offset(harvestable, "m_RefreshTime");
    api.post_harvest = il2::field_offset(harvestable, "m_ActivateObjectPostHarvest");
    Il2CppClass* spawns = il2::find_class(kGame, "", "RandomSpawnObjectManager");
    api.spawns_save = il2::find_method(spawns, "Serialize", 0);
    api.spawns_load = il2::find_method(spawns, "Deserialize", 1);
    api.find_all = il2::find_method_params(object, "FindObjectsOfType", {"System.Type", "System.Boolean"});
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.set_members = il2::field_offset(random_set, "m_ObjectList");
    api.not_saved = il2::find_method_params(random_set, "ShouldSkipSerialization", {"UnityEngine.GameObject"});
    api.set_inited = il2::field_offset(random_set, "m_Inited");
    api.set_had_save = il2::field_offset(random_set, "m_HadSaveData");
    Il2CppClass* prefab = il2::find_class(kGame, "", "PrefabSpawn");
    api.prefab_type = prefab ? il2::type_object(prefab) : nullptr;
    if (api.prefab_type) il2::il2cpp_gchandle_new(api.prefab_type, false);
    api.prefab_guid = il2::field_offset(prefab, "m_Guid");
    api.prefab_done = il2::field_offset(prefab, "m_SpawnComplete");
    api.prefab_saved = il2::field_offset(prefab, "m_UseSerialization");
    Il2CppClass* placements = il2::find_class(kGame, "TLD.Placement", "PlaceableManager");
    Il2CppClass* serialization = il2::find_class("TLD.Serialization.dll", "TLD.Serialization", "SerializationUtils");
    api.placements_save = il2::find_method(placements, "SerializeScenePlacements", 0);
    api.placements_load = il2::find_method(placements, "DeserializeScenePlacements", 1);
    api.to_json = il2::find_method(serialization, "SerializeObject", 1);
    api.from_json = il2::find_method_params(il2::find_class("Newtonsoft.Json.dll", "Newtonsoft.Json", "JsonConvert"),
                                            "DeserializeObject",
                                            {"System.String", "System.Type", "Newtonsoft.Json.JsonSerializerSettings"});
    if (serialization) api.json_settings = il2::il2cpp_class_get_field_from_name(serialization, "m_SerializerSettings");
    if (api.placements_save) {
        api.placements_type = il2::il2cpp_type_get_object(il2::il2cpp_method_get_return_type(api.placements_save));
        if (api.placements_type) il2::il2cpp_gchandle_new(api.placements_type, false);
    }
    api.placement_state = il2::field_offset(il2::find_class(kGame, "TLD.Placement", "PlaceableInfoSaveData"), "m_State");
    auto type_of = [](Il2CppClass* k) {
        Il2CppObject* t = k ? il2::type_object(k) : nullptr;
        if (t) il2::il2cpp_gchandle_new(t, false);
        return t;
    };
    api.placeable_type = type_of(il2::find_class(kGame, "TLD.Placement", "Placeable"));
    api.container_type = type_of(il2::find_class(kGame, "", "Container"));
    api.container_rolled = il2::field_offset(il2::find_class(kGame, "", "Container"), "m_RolledSpawnChance");
    api.xp_disabler_type = type_of(il2::find_class(kGame, "", "DisableObjectForXPMode"));
    Il2CppClass* container = il2::find_class(kGame, "", "Container");
    Il2CppClass* lock = il2::find_class(kGame, "", "Lock");
    Il2CppClass* safe = il2::find_class(kGame, "", "SafeCracking");
    Il2CppClass* open_close = il2::find_class(kGame, "", "OpenClose");
    Il2CppClass* door = il2::find_class(kGame, "", "LoadScene");
    if (Il2CppClass* containers = il2::find_class(kGame, "", "ContainerManager"))
        api.containers = il2::il2cpp_class_get_field_from_name(containers, "m_Containers");
    if (Il2CppClass* closers = il2::find_class(kGame, "", "OpenCloseManager"))
        api.open_closers = il2::il2cpp_class_get_field_from_name(closers, "m_OpenClosers");
    if (door) api.doors = il2::il2cpp_class_get_field_from_name(door, "m_LoadScenesList");
    api.container_lock = il2::field_offset(container, "m_Lock");
    api.container_safe = il2::field_offset(container, "m_Safe");
    api.interaction_lock = il2::field_offset(il2::find_class(kGame, "", "LockedInteraction"), "<Lock>k__BackingField");
    api.open_close_safe = il2::field_offset(open_close, "m_Safe");
    api.lock_state = il2::field_offset(lock, "m_LockState");
    api.unlock_companion = il2::find_method(lock, "UnlockCompanionLock", 0);
    api.force_lock = il2::find_method_params(lock, "FinishForceLock", {"System.Boolean"});
    api.safe_cracked = il2::field_offset(safe, "m_Cracked");
    api.crack_safe = il2::find_method(safe, "UnlockSafe", 0);
    Il2CppClass* placeable = il2::find_class(kGame, "TLD.Placement", "Placeable");
    Il2CppClass* decoration = il2::find_class(kGame, "", "DecorationItem");
    api.placeable_guid = il2::field_offset(placeable, "m_Guid");
    api.to_player = il2::find_method_params(placements, "MoveToPlayer", {"TLD.Placement.Placeable"});
    api.placed = il2::find_method_params(decoration, "FinalizePlacementRecursive", {"System.Boolean", "System.Boolean"});
    api.into_container = il2::find_method_params(container, "AddDecorationItem", {"DecorationItem"});
    api.placement_of = il2::find_method_params(placements, "GetPlacementState", {"TLD.Placement.Placeable"});
    api.pdid_object = il2::find_method_params(il2::find_class("TLD.PDID.dll", "TLD.PDID", "PdidTable"), "GetGameObject",
                                              {"System.String"});
    api.decoration_type = type_of(decoration);
    api.pick_up = il2::find_method(decoration, "OnPickup", 0);
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.transform_type = type_of(transform);
    api.component_type = type_of(component);
    api.components = il2::find_method_params(game_object, "GetComponents", {"System.Type"});
    api.safehouse = il2::find_method(il2::find_class(kGame, "", "GameManager"), "GetSafehouseManager", 0);
    api.customizing = il2::field_offset(il2::find_class(kGame, "", "SafehouseManager"), "m_IsCustomizing");
    api.mode_disabler_type = type_of(il2::find_class(kGame, "TLD.Gameplay", "DisableObjectForGameMode"));
    g_ready = api.component_go && api.component_transform && api.get_position && api.object_name && api.active_self &&
              api.set_active && api.parent && api.guid_of && api.breakdowns && api.break_down && api.harvestables &&
              api.harvest && api.harvested >= 0 && api.refresh >= 0 && api.post_harvest >= 0;
    if (!g_ready) {
        logx::warn("world objects: UNAVAILABLE (TLD API missing)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    g_ready = hook("BreakDown.DoBreakDown", api.break_down, &break_down_detour, &g_break_down) &&
              hook("Harvestable.Harvest", api.harvest, &harvest_detour, &g_harvest);
    const bool placements_moved =
        g_ready && api.placeable_guid >= 0 && api.placements_save && api.placements_load && api.to_json &&
        api.from_json && api.placements_type && api.placement_state >= 0 && api.to_player && api.placed &&
        api.into_container && hook("DecorationItem.FinalizePlacementRecursive", api.placed, &placed_detour, &g_placed_fn) &&
        hook("PlaceableManager.MoveToPlayer", api.to_player, &to_player_detour, &g_to_player) &&
        hook("Container.AddDecorationItem", api.into_container, &into_container_detour, &g_into_container);
    logx::info("world objects: %s%s%s%s%s%s", g_ready ? "ready (what's broken down or harvested is shared)" : "UNAVAILABLE (hooks failed)",
               g_ready && !api.radial_type ? "; spawned limbs can't be told apart" : "",
               g_ready && !(api.spawns_save && api.spawns_load) ? "; TLD's random spawns aren't shared" : "",
               g_ready && !(api.placements_save && api.placements_load && api.to_json && api.from_json && api.placements_type &&
                            api.placement_state >= 0)
                   ? "; TLD's placements aren't shared"
                   : "",
               g_ready && !(api.lock_state >= 0 && api.safe_cracked >= 0 && api.containers && api.open_closers && api.doors &&
                            api.container_lock >= 0 && api.interaction_lock >= 0)
                   ? "; locks and safes aren't shared"
                   : "",
               g_ready && !placements_moved ? "; decorations a player moves aren't shared" : "");
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
    const std::string& settled = in_game ? core::settled_scene() : std::string();
    if (settled != g_scene) {
        g_by_key.clear();
        g_ours.clear();
        g_together.clear();
        g_lock_seen.clear();
        g_marked.clear();
        g_placed.clear();
        g_scene = settled;
        if (!g_scene.empty()) {
            if (hosting())
                apply_stored();
            else if (g_list && g_list->scene == g_scene)
                apply_list(*g_list);
            g_list.reset();
        }
    }
    receive();
    if (g_scene.empty()) return;
    if (const double now = now_ms(); now - g_last_locks >= kLocksMs) {
        g_last_locks = now;
        scan_locks();
    }
    if (const double now = now_ms(); now - g_last_gone >= kGoneMs) {
        g_last_gone = now;
        keep_gone();
    }
    send_placements();
    if (!hosting()) return;
    for (uint8_t id : g_together.arrived(g_scene)) send_list(id);  // (see world_items)
}

bool share(const net::ObjectState& s) {
    if (!g_ready || !g_connected || g_scene.empty()) return false;
    remember(g_scene, s);
    g_ours[key_of(s.kind, s.id)] = now_ms();
    send_put(s);
    return true;
}

void save_kept(std::vector<world_keep::Record>& out) {
    auto add = [&](world_keep::Tag tag, const std::string& scene, const net::ObjectState& s) {
        net::ObjectPutMsg m;
        m.scene = scene;
        m.object = s;
        net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
        net::write_object_put(w, m);
        if (w.ok()) out.push_back(world_keep::record(tag, w));
    };
    for (const auto& [scene, states] : g_store)
        for (const auto& [key, s] : states) add(world_keep::Tag::ObjectChange, scene, s);
    for (const auto& [scene, states] : g_gone)
        for (const auto& [key, s] : states) add(world_keep::Tag::ObjectGone, scene, s);
}

bool load_kept(const world_keep::Record& r) {
    const net::Packet p = world_keep::packet(r);
    net::Reader body = net::packet_body(p);
    net::ObjectPutMsg m;
    if (p.type != net::Msg::ObjectPut || !net::read_object_put(body, m)) return false;
    auto& to = r.tag == world_keep::Tag::ObjectGone ? g_gone : g_store;
    to[m.scene][key_of(m.object.kind, m.object.id)] = m.object;
    return true;
}

}  // namespace world_objects
