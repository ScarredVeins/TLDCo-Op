#include "wildlife_sync.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "config.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/interp.h"
#include "net_runtime.h"

namespace wildlife_sync {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr const char* kAnim = "UnityEngine.AnimationModule.dll";

constexpr double kCaptureMs = 100;       // host: 10 Hz
constexpr float kRangeMetres = 400;      // host: animals farther than this from both players aren't sent
constexpr double kSmoothingMs = 150;     // client: interpolation delay
constexpr double kStaleMs = 2000;        // client: a copy that stops getting updates is removed after this
constexpr double kHideScanMs = 1000;     // client: how often our own (new) animals are looked for and hidden
constexpr float kResyncTime = 0.25f;     // client: animation time drift (fraction of the clip) before snapping
constexpr float kCrossFadeSeconds = 0.15f;
constexpr int32_t kAiModeDead = 2;
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;  // List<T>
// AnimatorControllerParameterType
constexpr int32_t kParamFloat = 1, kParamInt = 3, kParamBool = 4;
constexpr int32_t kCullAlwaysAnimate = 0;
constexpr const char* kKinds[] = {"none", "wolf", "bear", "stag", "rabbit", "moose", "cougar"};
constexpr uint64_t kNoWwiseObject = ~0ull;  // AK_INVALID_GAME_OBJECT

struct Vector3 {
    float x, y, z;
};
struct StateInfo {  // UnityEngine.AnimatorStateInfo
    int32_t name, path, full_path;
    float normalized_time, length, speed, speed_multiplier;
    int32_t tag, loop;
};

struct Api {
    FieldInfo* ai_list = nullptr;  // static List<BaseAi> BaseAiManager.m_BaseAis
    ptrdiff_t ai_kind = -1, ai_mode = -1, ai_hp = -1, ai_animator = -1;
    const MethodInfo* spawn_manager = nullptr;  // static GameManager.GetSpawnRegionManager()
    ptrdiff_t spawn_regions = -1, region_prefab = -1, region_kind = -1;

    const MethodInfo* component_go = nullptr;
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* go_transform = nullptr;   // GameObject.get_transform
    const MethodInfo* go_ctor = nullptr;
    const MethodInfo* go_active = nullptr;      // GameObject.get_activeInHierarchy
    const MethodInfo* go_active_self = nullptr; // GameObject.get_activeSelf
    const MethodInfo* go_set_active = nullptr;
    const MethodInfo* go_components = nullptr;  // GameObject.GetComponentsInChildren(Type, bool)
    const MethodInfo* get_position = nullptr;
    const MethodInfo* set_position = nullptr;
    const MethodInfo* get_euler = nullptr;
    const MethodInfo* set_euler = nullptr;
    const MethodInfo* set_parent = nullptr;     // Transform.SetParent(Transform, bool)
    const MethodInfo* instantiate = nullptr;    // static Object.Instantiate(Object, Transform)
    const MethodInfo* destroy = nullptr;
    const MethodInfo* destroy_now = nullptr;
    const MethodInfo* alive = nullptr;          // static Object.op_Implicit
    const MethodInfo* set_name = nullptr;
    Il2CppClass* game_object_class = nullptr;
    Il2CppObject* component_type = nullptr;
    Il2CppObject* animator_type = nullptr;

    const MethodInfo* layer_count = nullptr;
    const MethodInfo* state_info = nullptr;     // GetCurrentAnimatorStateInfo(int)
    const MethodInfo* in_transition = nullptr;  // IsInTransition(int)
    const MethodInfo* param_count = nullptr;
    const MethodInfo* get_param = nullptr;      // GetParameter(int) -> AnimatorControllerParameter
    const MethodInfo* param_hash = nullptr;     // AnimatorControllerParameter.get_nameHash
    const MethodInfo* param_type = nullptr;     // AnimatorControllerParameter.get_type
    const MethodInfo* get_float = nullptr;
    const MethodInfo* set_float = nullptr;
    const MethodInfo* get_int = nullptr;
    const MethodInfo* set_int = nullptr;
    const MethodInfo* get_bool = nullptr;
    const MethodInfo* set_bool = nullptr;
    const MethodInfo* play = nullptr;           // Play(int hash, int layer, float normalizedTime)
    const MethodInfo* crossfade = nullptr;      // CrossFadeInFixedTime(int hash, float seconds, int layer)
    const MethodInfo* set_root_motion = nullptr;
    const MethodInfo* set_culling = nullptr;

    Il2CppObject* ak_object_type = nullptr;      // AkGameObj: a Wwise sound object on a GameObject
    const MethodInfo* wwise_id = nullptr;        // static AkSoundEngine.GetAkGameObjectID(GameObject)

    // The client's side of the host's animals' attacks (milestone 7c).
    Il2CppObject* base_ai_type = nullptr;
    const MethodInfo* go_component = nullptr;     // GameObject.GetComponent(Type)
    const MethodInfo* set_mode = nullptr;         // BaseAi.SetAiMode(AiMode)
    const MethodInfo* damage_table = nullptr;     // BaseAi.GetAttackDamageEvents()
    const MethodInfo* apply_wounds = nullptr;     // DamageEventTable.MaybeApplyAttack(BaseAi, ApplyAttackOpts)
    const MethodInfo* get_struggle = nullptr;     // static GameManager.GetPlayerStruggleComponent()
    const MethodInfo* struggle_begin = nullptr;   // PlayerStruggle.Begin(GameObject)
    const MethodInfo* get_condition = nullptr;    // static GameManager.GetConditionComponent()
    const MethodInfo* add_health = nullptr;       // Condition.AddHealth(float, DamageSource, bool)
    ptrdiff_t region_refs = -1;                   // SpawnRegion.m_SpawnsPrefabReferences
    ptrdiff_t region_center = -1;                 // SpawnRegion.m_Center
    const MethodInfo* instantiate_spawn = nullptr;  // SpawnRegion.InstantiateSpawn(prefab, ref, pos, rot, mode, wm)
    // What a fight does to the stand-in (BaseAi), reported to the host.
    ptrdiff_t ai_wounded = -1, ai_bleeding = -1, ai_bleed_elapsed = -1, ai_bleed_limit = -1;
    // Weapon hits on the copies (wildlife_hits).
    Il2CppObject* localized_damage_type = nullptr;
    const MethodInfo* get_parent = nullptr;  // Transform.get_parent
    const MethodInfo* get_name = nullptr;    // Object.get_name
    Il2CppObject* arrow_type = nullptr;
    const MethodInfo* arrow_detach = nullptr;  // ArrowItem.DetachIntoWorld()
} api;

bool g_ready = false;

// --- shared helpers --------------------------------------------------------------------------------------------------

Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
uintptr_t hold(Il2CppObject* o) { return o ? il2::il2cpp_gchandle_new(o, false) : 0; }
void drop(uintptr_t& h) {
    if (h) il2::il2cpp_gchandle_free(h);
    h = 0;
}

bool alive(Il2CppObject* o) {
    if (!o) return false;
    bool yes = false;
    void* args[] = {o};
    return il2::invoke_value(api.alive, nullptr, yes, args) && yes;
}

Il2CppObject* go_of(Il2CppObject* component) { return component ? il2::invoke(api.component_go, component) : nullptr; }
Il2CppObject* transform_of(Il2CppObject* component) {
    return component ? il2::invoke(api.component_transform, component) : nullptr;
}
// Not interchangeable with transform_of: calling Component.get_transform on a GameObject crashes the game.
Il2CppObject* transform_of_go(Il2CppObject* go) { return go ? il2::invoke(api.go_transform, go) : nullptr; }

void set_active(Il2CppObject* go, bool on) {
    if (!go) return;
    void* args[] = {&on};
    il2::invoke(api.go_set_active, go, args);
}

bool active(Il2CppObject* go) {
    bool on = false;
    return go && il2::invoke_value(api.go_active, go, on) && on;
}

void destroy(Il2CppObject* o, bool now) {
    if (!o) return;
    void* args[] = {o};
    il2::invoke(now ? api.destroy_now : api.destroy, nullptr, args);
}

std::vector<Il2CppObject*> list_items(Il2CppObject* list) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* items = il2::read_field<Il2CppObject*>(list, kListItems);
    const int32_t count = il2::read_field<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* o = il2::array_object_at(items, static_cast<size_t>(i))) out.push_back(o);
    return out;
}

std::vector<Il2CppObject*> all_animals() {
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(api.ai_list, &list);
    return list_items(list);
}

// Unity euler angles are 0..360; a tilt reads better (and interpolates) as -180..180.
float signed_degrees(float d) { return d > 180.0f ? d - 360.0f : d; }

const char* kind_name(int kind) { return kind >= 0 && kind < static_cast<int>(std::size(kKinds)) ? kKinds[kind] : "?"; }

// An animator's parameters (hash and type), in the controller's order. Triggers aren't mirrored: the states they
// lead to are.
struct Param {
    int32_t hash, type;
};
std::vector<Param> read_schema(Il2CppObject* animator) {
    std::vector<Param> schema;
    int32_t count = 0;
    il2::invoke_value(api.param_count, animator, count);
    for (int32_t i = 0; i < count; ++i) {
        void* args[] = {&i};
        Il2CppObject* p = il2::invoke(api.get_param, animator, args);
        Param param{};
        if (!p || !il2::invoke_value(api.param_hash, p, param.hash) || !il2::invoke_value(api.param_type, p, param.type))
            continue;
        if (param.type == kParamFloat || param.type == kParamInt || param.type == kParamBool) schema.push_back(param);
    }
    return schema;
}

size_t schema_bytes(const std::vector<Param>& schema) {
    size_t n = 0;
    for (const Param& p : schema) n += p.type == kParamBool ? 1 : 4;
    return n;
}

// An animal's Wwise sound objects (AkGameObj), in hierarchy order: the same order on the host's animal and on the
// client's copy of it, so an index names the same one on both sides.
// The animal's own object comes first: TLD plays most animal sounds on it, and Wwise only gives it an AkGameObj when
// the first one plays.
std::vector<uint64_t> wwise_emitters(Il2CppObject* go) {
    std::vector<uint64_t> ids;
    if (!go || !api.ak_object_type || !api.wwise_id) return ids;
    uint64_t root = kNoWwiseObject;
    void* root_args[] = {go};
    il2::invoke_value(api.wwise_id, nullptr, root, root_args);
    ids.push_back(root);
    bool include_inactive = true;
    void* args[] = {api.ak_object_type, &include_inactive};
    Il2CppObject* objs = il2::invoke(api.go_components, go, args);
    for (size_t i = 0, n = il2::array_length(objs); i < n && ids.size() < 255; ++i) {
        uint64_t id = kNoWwiseObject;
        void* id_args[] = {go_of(il2::array_object_at(objs, i))};
        il2::invoke_value(api.wwise_id, nullptr, id, id_args);
        if (id != root) ids.push_back(id);
    }
    return ids;
}

// --- sounds (Wwise, native) --------------------------------------------------------------------------------------
// The hooks run on whatever thread plays the sound; everything they share is under g_sound_mutex.

using PostByIdFn = uint32_t (*)(uint32_t event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                                uint32_t externals, void* external_sources, uint32_t playing);
using PostByNameFn = uint32_t (*)(const char* event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                                  uint32_t externals, void* external_sources, uint32_t playing);
using PostByWideFn = uint32_t (*)(const wchar_t* event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                                  uint32_t externals, void* external_sources, uint32_t playing);
using ActionByIdFn = int32_t (*)(uint32_t event, int32_t action, uint64_t object, int32_t fade_ms, int32_t curve,
                                 uint32_t playing);
using ActionByNameFn = int32_t (*)(const char* event, int32_t action, uint64_t object, int32_t fade_ms, int32_t curve,
                                   uint32_t playing);
using StopPlayingFn = void (*)(uint32_t playing, int32_t fade_ms, int32_t curve);
using IdFromNameFn = uint32_t (*)(const char* name);
using IdFromWideFn = uint32_t (*)(const wchar_t* name);

PostByIdFn g_post_id = nullptr;
PostByNameFn g_post_name = nullptr;
PostByWideFn g_post_wide = nullptr;
ActionByIdFn g_action_id = nullptr;
ActionByNameFn g_action_name = nullptr;
StopPlayingFn g_stop_playing = nullptr;
IdFromNameFn g_id_from_name = nullptr;
IdFromWideFn g_id_from_wide = nullptr;
bool g_sound_hooks_tried = false;

std::mutex g_sound_mutex;
bool g_sound_host = false;  // only the host forwards
std::unordered_map<uint64_t, std::pair<uint32_t, uint8_t>> g_emitter_owner;  // Wwise object -> (animal, emitter)
std::unordered_map<uint32_t, std::pair<uint32_t, uint8_t>> g_playing_owner;  // host playing ID -> (animal, emitter)
std::vector<net::AnimalSound> g_sounds_out;
thread_local int t_sound_depth = 0;  // the name variants call the ID one: only the outermost call counts
uint64_t g_posts_seen = 0, g_posts_matched = 0;  // for the 'sounds' dev command
struct SeenPost {
    uint32_t event;
    uint64_t object;
    bool matched;
};
SeenPost g_recent_posts[12] = {};
size_t g_recent_next = 0;

void note_post(uint32_t event, uint64_t object, uint32_t playing) {
    std::lock_guard lock(g_sound_mutex);
    const auto owner = g_emitter_owner.find(object);
    ++g_posts_seen;
    g_recent_posts[g_recent_next++ % std::size(g_recent_posts)] = {event, object, owner != g_emitter_owner.end()};
    if (!g_sound_host || !event || !playing) return;
    if (owner == g_emitter_owner.end()) return;
    ++g_posts_matched;
    g_playing_owner[playing] = owner->second;
    if (g_playing_owner.size() > 4096) g_playing_owner.clear();
    net::AnimalSound s;
    s.animal = owner->second.first;
    s.emitter = owner->second.second;
    s.op = net::SoundOp::Post;
    s.event = event;
    s.playing = playing;
    g_sounds_out.push_back(s);
}

void note_action(uint32_t event, int32_t action, uint64_t object, int32_t fade_ms, int32_t curve, uint32_t playing) {
    std::lock_guard lock(g_sound_mutex);
    if (!g_sound_host) return;
    std::pair<uint32_t, uint8_t> owner;
    if (const auto o = g_emitter_owner.find(object); o != g_emitter_owner.end()) owner = o->second;
    else if (const auto p = g_playing_owner.find(playing); playing && p != g_playing_owner.end()) owner = p->second;
    else return;
    net::AnimalSound s;
    s.animal = owner.first;
    s.emitter = owner.second;
    s.op = net::SoundOp::Action;
    s.event = event;
    s.playing = playing;
    s.action = action;
    s.fade_ms = fade_ms;
    s.curve = curve;
    g_sounds_out.push_back(s);
}

uint32_t post_id_detour(uint32_t event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                        uint32_t externals, void* external_sources, uint32_t playing) {
    ++t_sound_depth;
    const uint32_t id = g_post_id(event, object, flags, callback, cookie, externals, external_sources, playing);
    if (--t_sound_depth == 0) note_post(event, object, id);
    return id;
}

uint32_t post_name_detour(const char* event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                          uint32_t externals, void* external_sources, uint32_t playing) {
    ++t_sound_depth;
    const uint32_t id = g_post_name(event, object, flags, callback, cookie, externals, external_sources, playing);
    if (--t_sound_depth == 0 && event && g_id_from_name) note_post(g_id_from_name(event), object, id);
    return id;
}

uint32_t post_wide_detour(const wchar_t* event, uint64_t object, uint32_t flags, void* callback, void* cookie,
                          uint32_t externals, void* external_sources, uint32_t playing) {
    ++t_sound_depth;
    const uint32_t id = g_post_wide(event, object, flags, callback, cookie, externals, external_sources, playing);
    if (--t_sound_depth == 0 && event && g_id_from_wide) note_post(g_id_from_wide(event), object, id);
    return id;
}

int32_t action_id_detour(uint32_t event, int32_t action, uint64_t object, int32_t fade_ms, int32_t curve,
                         uint32_t playing) {
    ++t_sound_depth;
    const int32_t result = g_action_id(event, action, object, fade_ms, curve, playing);
    if (--t_sound_depth == 0) note_action(event, action, object, fade_ms, curve, playing);
    return result;
}

int32_t action_name_detour(const char* event, int32_t action, uint64_t object, int32_t fade_ms, int32_t curve,
                           uint32_t playing) {
    ++t_sound_depth;
    const int32_t result = g_action_name(event, action, object, fade_ms, curve, playing);
    if (--t_sound_depth == 0 && event && g_id_from_name)
        note_action(g_id_from_name(event), action, object, fade_ms, curve, playing);
    return result;
}

void stop_playing_detour(uint32_t playing, int32_t fade_ms, int32_t curve) {
    g_stop_playing(playing, fade_ms, curve);
    std::lock_guard lock(g_sound_mutex);
    if (!g_sound_host) return;
    const auto owner = g_playing_owner.find(playing);
    if (owner == g_playing_owner.end()) return;
    net::AnimalSound s;
    s.animal = owner->second.first;
    s.emitter = owner->second.second;
    s.op = net::SoundOp::StopPlaying;
    s.playing = playing;
    s.fade_ms = fade_ms;
    s.curve = curve;
    g_sounds_out.push_back(s);
    g_playing_owner.erase(owner);
}

// Wwise is loaded by Unity during startup; hook it the first frame it's there.
void install_sound_hooks() {
    if (g_sound_hooks_tried) return;
    HMODULE ak = GetModuleHandleW(L"AkSoundEngine.dll");
    if (!ak) return;
    g_sound_hooks_tried = true;
    auto proc = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(ak, name)); };
    g_id_from_name = reinterpret_cast<IdFromNameFn>(proc("?GetIDFromString@SoundEngine@AK@@YAIPEBD@Z"));
    g_id_from_wide = reinterpret_cast<IdFromWideFn>(proc("?GetIDFromString@SoundEngine@AK@@YAIPEB_W@Z"));
    struct Hook {
        const char* name;
        const char* symbol;
        void* detour;
        void** original;
    };
    const Hook kHooks[] = {
        {"AK::SoundEngine::PostEvent(id)",
         "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",
         reinterpret_cast<void*>(&post_id_detour), reinterpret_cast<void**>(&g_post_id)},
        {"AK::SoundEngine::PostEvent(name)",
         "?PostEvent@SoundEngine@AK@@YAIPEBD_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",
         reinterpret_cast<void*>(&post_name_detour), reinterpret_cast<void**>(&g_post_name)},
        {"AK::SoundEngine::PostEvent(wide name)",
         "?PostEvent@SoundEngine@AK@@YAIPEB_W_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",
         reinterpret_cast<void*>(&post_wide_detour), reinterpret_cast<void**>(&g_post_wide)},
        {"AK::SoundEngine::ExecuteActionOnEvent(id)",
         "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@IW4AkActionOnEventType@12@_KHW4AkCurveInterpolation@@I@Z",
         reinterpret_cast<void*>(&action_id_detour), reinterpret_cast<void**>(&g_action_id)},
        {"AK::SoundEngine::ExecuteActionOnEvent(name)",
         "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@PEBDW4AkActionOnEventType@12@_KHW4AkCurveInterpolation@@I@Z",
         reinterpret_cast<void*>(&action_name_detour), reinterpret_cast<void**>(&g_action_name)},
        {"AK::SoundEngine::StopPlayingID", "?StopPlayingID@SoundEngine@AK@@YAXIHW4AkCurveInterpolation@@@Z",
         reinterpret_cast<void*>(&stop_playing_detour), reinterpret_cast<void**>(&g_stop_playing)},
    };
    int installed = 0;
    for (const Hook& h : kHooks)
        if (void* target = proc(h.symbol)) installed += hooks::install(h.name, target, h.detour, h.original) ? 1 : 0;
    logx::info("wildlife: %d of %zu Wwise hooks in (animal sounds %s)", installed, std::size(kHooks),
               installed == static_cast<int>(std::size(kHooks)) ? "shared" : "PARTLY shared");
}

// --- host ------------------------------------------------------------------------------------------------------------

double g_last_capture = 0;
uint32_t g_frame = 0;
uint32_t g_next_id = 1;
std::unordered_map<uintptr_t, uint32_t> g_ids;             // BaseAi object -> our id
std::unordered_map<uintptr_t, std::vector<Param>> g_schemas;  // Animator object -> its parameters
std::unordered_map<uintptr_t, std::vector<uint64_t>> g_ai_emitters;  // BaseAi object -> its Wwise sound objects
double g_emitters_refreshed = 0;
size_t g_last_shared = static_cast<size_t>(-1);

std::string read_params(Il2CppObject* animator) {
    auto it = g_schemas.find(reinterpret_cast<uintptr_t>(animator));
    if (it == g_schemas.end()) it = g_schemas.emplace(reinterpret_cast<uintptr_t>(animator), read_schema(animator)).first;
    std::string out;
    for (const Param& p : it->second) {
        int32_t hash = p.hash;
        void* args[] = {&hash};
        if (p.type == kParamFloat) {
            float v = 0;
            il2::invoke_value(api.get_float, animator, v, args);
            out.append(reinterpret_cast<const char*>(&v), 4);
        } else if (p.type == kParamInt) {
            int32_t v = 0;
            il2::invoke_value(api.get_int, animator, v, args);
            out.append(reinterpret_cast<const char*>(&v), 4);
        } else {
            bool v = false;
            il2::invoke_value(api.get_bool, animator, v, args);
            out.push_back(v ? 1 : 0);
        }
    }
    return out;
}

void capture(const std::string& scene, double now) {
    const game::PlayerState me = game::read_player();
    std::vector<net::StateMsg> friends_here = netrt::remote_states();
    friends_here.erase(std::remove_if(friends_here.begin(), friends_here.end(),
                                      [&](const net::StateMsg& s) { return !s.valid || s.scene != scene; }),
                       friends_here.end());
    auto in_range = [&](const Vector3& p) {
        auto within = [&](float x, float y, float z) {
            const float dx = p.x - x, dy = p.y - y, dz = p.z - z;
            return dx * dx + dy * dy + dz * dz <= kRangeMetres * kRangeMetres;
        };
        if (me.valid && within(me.position.x, me.position.y, me.position.z)) return true;
        for (const net::StateMsg& s : friends_here)
            if (within(s.x, s.y, s.z)) return true;
        return false;
    };

    std::vector<net::AnimalRec> animals;
    std::unordered_map<uintptr_t, uint32_t> seen;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint8_t>> owners;
    if (now - g_emitters_refreshed > 2000) {
        g_emitters_refreshed = now;
        g_ai_emitters.clear();
    }
    int counts[std::size(kKinds)] = {};
    for (Il2CppObject* ai : all_animals()) {
        if (!active(go_of(ai))) continue;
        Il2CppObject* tf = transform_of(ai);
        Vector3 pos{}, euler{};
        if (!tf || !il2::invoke_value(api.get_position, tf, pos) || !in_range(pos)) continue;
        il2::invoke_value(api.get_euler, tf, euler);
        const auto key = reinterpret_cast<uintptr_t>(ai);
        auto id = g_ids.find(key);
        if (id == g_ids.end()) id = g_ids.emplace(key, g_next_id++).first;
        seen.emplace(key, id->second);

        net::AnimalRec a;
        a.id = id->second;
        a.kind = static_cast<uint8_t>(il2::read_field<int32_t>(ai, api.ai_kind));
        a.mode = static_cast<uint8_t>(il2::read_field<int32_t>(ai, api.ai_mode));
        a.flags = a.mode == kAiModeDead ? net::kAnimalDead : 0;
        a.hp = il2::read_field<float>(ai, api.ai_hp);
        a.x = pos.x;
        a.y = pos.y;
        a.z = pos.z;
        a.yaw = euler.y;
        a.pitch = signed_degrees(euler.x);
        a.roll = signed_degrees(euler.z);
        if (Il2CppObject* animator = il2::read_field<Il2CppObject*>(ai, api.ai_animator)) {
            int32_t layers = 0;
            il2::invoke_value(api.layer_count, animator, layers);
            for (int32_t l = 0; l < layers && l < static_cast<int32_t>(net::kMaxAnimalLayers); ++l) {
                StateInfo si{};
                void* args[] = {&l};
                il2::invoke_value(api.state_info, animator, si, args);
                a.layers.push_back({si.full_path, si.normalized_time});
            }
            a.params = read_params(animator);
            if (a.params.size() > net::kMaxAnimalParams) a.params.clear();
        }
        if (a.kind < std::size(kKinds)) ++counts[a.kind];
        auto emitters = g_ai_emitters.find(key);
        if (emitters == g_ai_emitters.end()) emitters = g_ai_emitters.emplace(key, wwise_emitters(go_of(ai))).first;
        for (size_t e = 0; e < emitters->second.size(); ++e)
            owners[emitters->second[e]] = {a.id, static_cast<uint8_t>(e)};
        animals.push_back(std::move(a));
    }
    {
        std::lock_guard lock(g_sound_mutex);
        g_emitter_owner.swap(owners);
        g_sound_host = true;
    }
    for (auto it = g_ai_emitters.begin(); it != g_ai_emitters.end();)
        it = seen.count(it->first) ? std::next(it) : g_ai_emitters.erase(it);
    g_ids.swap(seen);  // forget animals that are gone
    if (g_schemas.size() > 256) g_schemas.clear();

    // One frame, split over as many parts as it takes.
    std::vector<net::AnimalsMsg> parts(1);
    size_t used = net::kAnimalsHeader;
    for (net::AnimalRec& a : animals) {
        const size_t size = net::animal_size(a);
        if (used + size > net::kMaxPacket || parts.back().animals.size() == 255) {
            parts.emplace_back();
            used = net::kAnimalsHeader;
        }
        used += size;
        parts.back().animals.push_back(std::move(a));
    }
    ++g_frame;
    for (size_t i = 0; i < parts.size(); ++i) {
        parts[i].frame = g_frame;
        parts[i].t_ms = now;
        parts[i].part = static_cast<uint8_t>(i);
        parts[i].parts = static_cast<uint8_t>(parts.size());
        parts[i].scene = scene;
    }
    if (animals.size() != g_last_shared) {
        g_last_shared = animals.size();
        std::string summary;
        for (size_t k = 1; k < std::size(kKinds); ++k)
            if (counts[k]) summary += std::string(summary.empty() ? "" : ", ") + std::to_string(counts[k]) + " " + kKinds[k];
        logx::info("wildlife: sharing %zu animal(s) in '%s'%s%s", g_last_shared, scene.c_str(),
                   summary.empty() ? "" : ": ", summary.c_str());
    }
    netrt::send_animals(std::move(parts));
}

// --- client ----------------------------------------------------------------------------------------------------------

struct Puppet {
    uint8_t kind = 0;
    uintptr_t go = 0, transform = 0, animator = 0;
    std::vector<Param> schema;
    size_t params_size = 0;
    net::Interpolator track{kSmoothingMs};
    net::AnimalRec latest;
    double updated_ms = 0;
    int32_t requested_state[net::kMaxAnimalLayers] = {};
    double requested_at[net::kMaxAnimalLayers] = {};
    bool params_mismatch_logged = false;
    std::vector<uint64_t> emitters;                     // its Wwise sound objects, same order as the host's animal
    std::unordered_map<uint32_t, uint32_t> playing;     // host playing ID -> ours
    bool carcass = false;                               // a real carcass (wildlife_carcass) has taken its place
};

std::map<uint32_t, Puppet> g_puppets;
uintptr_t g_puppet_root = 0;    // scene object holding the copies
uintptr_t g_loader = 0;         // inactive: copies are built here before they wake up
std::vector<uintptr_t> g_hidden;  // our own animals we hid
std::vector<Il2CppObject*> g_carcasses;  // our carcasses of the host's dead animals (wildlife_carcass): not hidden
std::unordered_set<uint32_t> g_carcass_ids;  // their animals: a copy made again (the link was back) stays hidden
double g_last_hide_scan = 0;
bool g_mirroring = false;
std::string g_mirror_scene;
std::vector<net::AnimalsMsg> g_incoming;
int g_failed_kinds = 0;  // bitmask of kinds we couldn't copy (logged once)
int g_spawns_blocked = 0;  // TLD spawns refused while mirroring (the first is logged)

Il2CppObject* make_object(const char* name, bool on) {
    Il2CppObject* go = il2::il2cpp_object_new(api.game_object_class);
    void* args[] = {il2::il2cpp_string_new(name)};
    il2::invoke(api.go_ctor, go, args);
    set_active(go, on);
    return go;
}

Il2CppObject* puppet_root() {
    if (!alive(target(g_puppet_root))) {
        drop(g_puppet_root);
        g_puppet_root = hold(make_object("TLDCoop_HostAnimals", true));
    }
    if (!alive(target(g_loader))) {
        drop(g_loader);
        g_loader = hold(make_object("TLDCoop_HostAnimalsLoader", false));
    }
    return target(g_puppet_root);
}

// This region's prefab for a kind: its spawn area's, or one of our own (hidden) animals of that kind.
Il2CppObject* template_for(int kind) {
    Il2CppObject* manager = il2::invoke(api.spawn_manager, nullptr);
    for (Il2CppObject* region : list_items(il2::read_field<Il2CppObject*>(manager, api.spawn_regions)))
        if (il2::read_field<int32_t>(region, api.region_kind) == kind)
            if (Il2CppObject* prefab = il2::read_field<Il2CppObject*>(region, api.region_prefab)) return prefab;
    for (uintptr_t h : g_hidden)
        if (Il2CppObject* go = target(h))
            for (Il2CppObject* ai : all_animals())
                if (go_of(ai) == go && il2::read_field<int32_t>(ai, api.ai_kind) == kind) return go;
    for (Il2CppObject* ai : all_animals())
        if (il2::read_field<int32_t>(ai, api.ai_kind) == kind && il2::read_field<int32_t>(ai, api.ai_mode) != kAiModeDead)
            return go_of(ai);
    return nullptr;
}

bool keep_on_copy(const std::string& cls) {
    static const char* kKeep[] = {"UnityEngine.Transform", "UnityEngine.Animator", "UnityEngine.SkinnedMeshRenderer",
                                  "UnityEngine.MeshRenderer", "UnityEngine.MeshFilter", "UnityEngine.LODGroup",
                                  "AkGameObj",     // where the animal's sounds come from
                                  "BodyDamage"};   // the root's damage tables, for the hit colliders below
    for (const char* k : kKeep)
        if (cls == k) return true;
    return false;
}

// The animal's hit colliders (colliders/capsule_*: a capsule on layer NPCBones with LocalizedDamage, a kinematic
// Rigidbody and MatchTransform, which keeps it on its bone) stay on the copy, so the other player's bullets, arrows and
// flare rounds hit it with TLD's own effects. With no BaseAi on the copy TLD's hit code stops before any damage
// (AiUtils.GetBaseAiFromObject finds none); wildlife_hits sends the hit to the host instead.
bool keep_on_hit_collider(const std::string& cls) {
    static const char* kKeep[] = {"LocalizedDamage", "MatchTransform", "UnityEngine.CapsuleCollider",
                                  "UnityEngine.SphereCollider", "UnityEngine.BoxCollider", "UnityEngine.Rigidbody"};
    for (const char* k : kKeep)
        if (cls == k) return true;
    return false;
}

bool has_localized_damage(Il2CppObject* go) {
    if (!go || !api.localized_damage_type) return false;
    void* args[] = {api.localized_damage_type};
    return il2::invoke(api.go_component, go, args) != nullptr;
}

// Everything but the model, its Animator and its hit colliders goes: AI, navigation, other colliders, the game's own
// scripts.
int strip(Il2CppObject* copy) {
    int removed = 0;
    for (int pass = 0; pass < 3; ++pass) {
        bool include_inactive = true;
        void* args[] = {api.component_type, &include_inactive};
        Il2CppObject* comps = il2::invoke(api.go_components, copy, args);
        for (size_t i = 0, n = il2::array_length(comps); i < n; ++i) {
            Il2CppObject* c = il2::array_object_at(comps, i);
            const std::string cls = il2::class_name_of(c);
            if (keep_on_copy(cls)) continue;
            if (keep_on_hit_collider(cls) && has_localized_damage(go_of(c))) continue;
            if (pass == 0 && cls.rfind("UnityEngine.", 0) == 0) continue;  // game scripts first (they need the rest)
            destroy(c, true);
            ++removed;
        }
    }
    return removed;
}

bool make_puppet(uint32_t id, const net::AnimalRec& rec, Puppet& p) {
    Il2CppObject* root = puppet_root();
    Il2CppObject* source = template_for(rec.kind);
    if (!root || !source) {
        if (!(g_failed_kinds & (1 << rec.kind)))
            logx::warn("wildlife: no %s to copy in this scene; the host's %s stays invisible here", kind_name(rec.kind),
                       kind_name(rec.kind));
        g_failed_kinds |= 1 << rec.kind;
        return false;
    }
    void* args[] = {source, transform_of_go(target(g_loader))};
    Il2CppObject* copy = il2::invoke(api.instantiate, nullptr, args);
    if (!copy) return false;
    const int removed = strip(copy);
    char name[64];
    snprintf(name, sizeof(name), "TLDCoop_HostAnimal_%u", id);
    void* name_args[] = {il2::il2cpp_string_new(name)};
    il2::invoke(api.set_name, copy, name_args);
    bool keep_world = false;
    void* parent_args[] = {transform_of_go(root), &keep_world};
    il2::invoke(api.set_parent, transform_of_go(copy), parent_args);
    set_active(copy, true);

    p = Puppet{};
    p.kind = rec.kind;
    p.go = hold(copy);
    p.transform = hold(transform_of_go(copy));
    bool include_inactive = true;
    void* comp_args[] = {api.animator_type, &include_inactive};
    Il2CppObject* animators = il2::invoke(api.go_components, copy, comp_args);
    if (il2::array_length(animators) > 0) {
        Il2CppObject* animator = il2::array_object_at(animators, 0);
        p.animator = hold(animator);
        bool root_motion = false;
        void* rm_args[] = {&root_motion};
        il2::invoke(api.set_root_motion, animator, rm_args);
        int32_t culling = kCullAlwaysAnimate;
        void* cull_args[] = {&culling};
        il2::invoke(api.set_culling, animator, cull_args);
        p.schema = read_schema(animator);
        p.params_size = schema_bytes(p.schema);
    }
    p.emitters = wwise_emitters(copy);
    void* ld_args[] = {api.localized_damage_type, &include_inactive};
    const size_t hit_colliders = api.localized_damage_type ? il2::array_length(il2::invoke(api.go_components, copy, ld_args)) : 0;
    logx::info("wildlife: showing the host's %s #%u (%s, %d game components removed, %zu animator parameters, %zu "
               "sound objects, %zu hit colliders)", kind_name(rec.kind), id,
               il2::enum_name(il2::find_class(kGame, "", "AiMode"), rec.mode).c_str(), removed, p.schema.size(),
               p.emitters.size(), hit_colliders);
    return true;
}

// Our own arrows stuck in the copy (parented to its hit colliders) drop into the world instead of going with it.
void free_stuck_arrows(Il2CppObject* copy) {
    if (!copy || !api.arrow_type || !api.arrow_detach) return;
    bool include_inactive = true;
    void* args[] = {api.arrow_type, &include_inactive};
    Il2CppObject* arrows = il2::invoke(api.go_components, copy, args);
    for (size_t i = 0, n = il2::array_length(arrows); i < n; ++i)
        il2::invoke(api.arrow_detach, il2::array_object_at(arrows, i));
}

void remove_puppet(Puppet& p) {
    free_stuck_arrows(target(p.go));
    destroy(target(p.go), false);
    drop(p.go);
    drop(p.transform);
    drop(p.animator);
}

void apply_params(Puppet& p, uint32_t id) {
    Il2CppObject* animator = target(p.animator);
    const std::string& v = p.latest.params;
    if (!animator || v.empty()) return;
    if (v.size() != p.params_size) {
        if (!p.params_mismatch_logged)
            logx::warn("wildlife: #%u's animator differs from the host's (%zu vs %zu bytes of parameters); "
                       "showing states only", id, p.params_size, v.size());
        p.params_mismatch_logged = true;
        return;
    }
    size_t at = 0;
    for (const Param& param : p.schema) {
        int32_t hash = param.hash;
        if (param.type == kParamBool) {
            bool b = v[at++] != 0;
            void* args[] = {&hash, &b};
            il2::invoke(api.set_bool, animator, args);
        } else if (param.type == kParamInt) {
            int32_t i = 0;
            memcpy(&i, v.data() + at, 4);
            at += 4;
            void* args[] = {&hash, &i};
            il2::invoke(api.set_int, animator, args);
        } else {
            float f = 0;
            memcpy(&f, v.data() + at, 4);
            at += 4;
            void* args[] = {&hash, &f};
            il2::invoke(api.set_float, animator, args);
        }
    }
}

// Same states as the host's animator: cross-fade into a new one, snap when the time drifts or a fade never lands.
void apply_states(Puppet& p, double now) {
    Il2CppObject* animator = target(p.animator);
    if (!animator) return;
    for (int32_t l = 0; l < static_cast<int32_t>(p.latest.layers.size()); ++l) {
        const net::AnimalLayer& host = p.latest.layers[static_cast<size_t>(l)];
        if (!host.state) continue;
        StateInfo mine{};
        void* args[] = {&l};
        if (!il2::invoke_value(api.state_info, animator, mine, args)) continue;
        if (mine.full_path != host.state) {
            if (p.requested_state[l] != host.state) {
                int32_t hash = host.state;
                float seconds = kCrossFadeSeconds;
                int32_t layer = l;
                void* cf_args[] = {&hash, &seconds, &layer};
                il2::invoke(api.crossfade, animator, cf_args);
                p.requested_state[l] = host.state;
                p.requested_at[l] = now;
            } else if (now - p.requested_at[l] > 500) {
                int32_t hash = host.state, layer = l;
                float time = host.time;
                void* play_args[] = {&hash, &layer, &time};
                il2::invoke(api.play, animator, play_args);
                p.requested_at[l] = now;
            }
            continue;
        }
        p.requested_state[l] = host.state;
        float drift = std::fabs(mine.normalized_time - host.time);
        if (mine.loop) drift = std::fabs(drift - std::round(drift));
        if (drift > kResyncTime) {
            int32_t hash = host.state, layer = l;
            float time = host.time;
            void* play_args[] = {&hash, &layer, &time};
            il2::invoke(api.play, animator, play_args);
        }
    }
}

// The host's animals' sounds, on our copies of them.
std::vector<net::AnimalSound> g_sounds_in;
int g_sounds_played = 0;

void play_host_sounds() {
    g_sounds_in.clear();
    netrt::drain_animal_sounds(g_sounds_in);
    if (!g_post_id || !g_stop_playing || !g_action_id) return;
    for (const net::AnimalSound& s : g_sounds_in) {
        const auto it = g_puppets.find(s.animal);
        if (it == g_puppets.end()) continue;
        Puppet& p = it->second;
        if (p.emitters.empty()) continue;
        const uint64_t object = p.emitters[s.emitter < p.emitters.size() ? s.emitter : 0];
        if (s.op == net::SoundOp::Post) {
            const uint32_t ours = g_post_id(s.event, object, 0, nullptr, nullptr, 0, nullptr, 0);
            if (ours) p.playing[s.playing] = ours;
            if (g_sounds_played++ == 0) logx::info("wildlife: first host animal sound played here (event %u)", s.event);
        } else if (s.op == net::SoundOp::StopPlaying) {
            if (const auto mine = p.playing.find(s.playing); mine != p.playing.end()) {
                g_stop_playing(mine->second, s.fade_ms, s.curve);
                p.playing.erase(mine);
            }
        } else {
            uint32_t ours = 0;
            if (s.playing)
                if (const auto mine = p.playing.find(s.playing); mine != p.playing.end()) ours = mine->second;
            if (s.playing && !ours) continue;  // a sound we never played
            g_action_id(s.event, s.action, object, s.fade_ms, s.curve, ours);
        }
    }
}

// --- the host's animals attacking us (milestone 7c) -------------------------------------------------------------------
// A fight runs here with one of our own (hidden) animals of the same kind standing in for the host's (or one made for
// it): woken right in front of us, on the host's animal's side, TLD's struggle with it, and then hidden again. The
// host hears how it ended.

constexpr int32_t kAiModeStruggle = 14;
constexpr int32_t kAiModeWander = 15;
constexpr double kLocalFightTimeoutMs = 90000;
constexpr float kFightStartMetres = 1.5f;  // how far in front of us our stand-in animal starts the fight

struct LocalFight {
    bool active = false;
    uint32_t animal = 0;  // the host's id
    uintptr_t ai = 0;     // our stand-in animal (BaseAi)
    double started_ms = 0;
    bool made_for_it = false;  // we spawned it for this fight (none of ours was hidden): removed afterwards
    float hp_begin = 0;        // the stand-in's health when the fight began (the host's animal's)
    bool saw_struggle = false; // the stand-in has been in TLD's Struggle mode
    bool reported = false;     // the host has been told how it ended
} g_fight;

struct Quaternion {
    float x, y, z, w;
};

// No animal of that kind of our own (TLD's spawning is off while mirroring): make one for the fight with this
// region's nearest spawn area for the kind, the way TLD spawns them, already in its fight mode.
Il2CppObject* spawn_stand_in(int kind, Vector3 pos, float yaw_degrees) {
    if (!api.instantiate_spawn) return nullptr;
    Il2CppObject* manager = il2::invoke(api.spawn_manager, nullptr);
    Il2CppObject* best = nullptr;
    float best_d = 0;
    for (Il2CppObject* region : list_items(il2::read_field<Il2CppObject*>(manager, api.spawn_regions))) {
        if (il2::read_field<int32_t>(region, api.region_kind) != kind) continue;
        if (!il2::read_field<Il2CppObject*>(region, api.region_prefab)) continue;
        const Vector3 c = il2::read_field<Vector3>(region, api.region_center);
        const float d = (c.x - pos.x) * (c.x - pos.x) + (c.z - pos.z) * (c.z - pos.z);
        if (!best || d < best_d) best = region, best_d = d;
    }
    if (!best) return nullptr;
    Il2CppObject* prefab = il2::read_field<Il2CppObject*>(best, api.region_prefab);
    const auto refs = list_items(il2::read_field<Il2CppObject*>(best, api.region_refs));
    Il2CppObject* prefab_ref = refs.empty() ? nullptr : refs[0];
    const float half = yaw_degrees * 0.5f * 3.14159265f / 180.0f;
    Quaternion rot{0, std::sin(half), 0, std::cos(half)};
    int32_t mode = 14;  // AiMode.Struggle
    int32_t wildlife = 0;
    void* args[] = {prefab, prefab_ref, &pos, &rot, &mode, &wildlife};
    Il2CppObject* ai = il2::invoke(api.instantiate_spawn, best, args);
    if (ai) set_active(go_of(ai), true);
    return ai;
}

Il2CppObject* base_ai_of(Il2CppObject* go) {
    if (!go) return nullptr;
    void* args[] = {api.base_ai_type};
    return il2::invoke(api.go_component, go, args);
}

// One of our hidden animals of that kind (a living one: an earlier fight may have killed one).
Il2CppObject* our_animal_of_kind(int kind) {
    for (uintptr_t h : g_hidden) {
        Il2CppObject* ai = base_ai_of(target(h));
        if (ai && alive(ai) && il2::read_field<int32_t>(ai, api.ai_kind) == kind &&
            il2::read_field<int32_t>(ai, api.ai_mode) != kAiModeDead)
            return ai;
    }
    return nullptr;
}

void reply(uint32_t animal, net::FightOutcome outcome, float hp) {
    net::AttackResultMsg r;
    r.animal = animal;
    r.outcome = outcome;
    r.hp = hp;
    netrt::send_attack_result(r);
}

template <typename T>
void write_field(Il2CppObject* obj, ptrdiff_t offset, T value) {
    if (obj && offset >= 0) memcpy(reinterpret_cast<char*>(obj) + offset, &value, sizeof(T));
}

// The stand-in becomes the host's animal for the fight: its health, and none of the wounds an earlier fight left on
// it. TLD decides in PlayerStruggle.PrepareStruggle from this health whether the animal fights to the death.
void prepare_stand_in(Il2CppObject* ai, float host_hp) {
    if (host_hp > 0) write_field<float>(ai, api.ai_hp, host_hp);
    write_field<bool>(ai, api.ai_wounded, false);
    write_field<bool>(ai, api.ai_bleeding, false);
    write_field<float>(ai, api.ai_bleed_elapsed, 0);
    write_field<float>(ai, api.ai_bleed_limit, 0);
}

// How TLD ended the fight, from the stand-in: the mode it left Struggle for (Flee: MakePartnerFlee; Wander: the fight
// timed out or the player died; Dead: a weapon took its health), the health the fight took (each weapon tap goes
// through BaseAi.ApplyDamage), and any bleeding out it started.
void report_fight(Il2CppObject* ai, const char* why) {
    net::AttackResultMsg r;
    r.animal = g_fight.animal;
    if (ai && alive(ai)) {
        const int32_t mode = il2::read_field<int32_t>(ai, api.ai_mode);
        r.hp = il2::read_field<float>(ai, api.ai_hp);
        r.outcome = mode == kAiModeDead || r.hp <= 1e-4f ? net::FightOutcome::Died
                    : mode == kAiModeWander                ? net::FightOutcome::WalkedOff
                                                           : net::FightOutcome::Fled;
        r.damage = g_fight.hp_begin > r.hp ? g_fight.hp_begin - r.hp : 0;
        if (il2::read_field<bool>(ai, api.ai_bleeding)) {
            r.flags |= net::kFightBleeding;
            r.bleed_minutes = il2::read_field<float>(ai, api.ai_bleed_limit) - il2::read_field<float>(ai, api.ai_bleed_elapsed);
            if (r.bleed_minutes < 0.01f) r.bleed_minutes = 0.01f;
        }
    }
    netrt::send_attack_result(r);
    g_fight.reported = true;
    logx::info("wildlife: our fight with the host's #%u is decided (%s): it %s, hp %.2f -> %.2f (%.2f damage)%s", g_fight.animal,
               why, r.outcome == net::FightOutcome::Died ? "died" : r.outcome == net::FightOutcome::Fled ? "fled"
                    : r.outcome == net::FightOutcome::WalkedOff ? "walked off" : "-",
               g_fight.hp_begin, r.hp, r.damage, (r.flags & net::kFightBleeding) ? ", bleeding out" : "");
    if (r.flags & net::kFightBleeding)
        logx::info("wildlife:   it bleeds out in %.0f game minutes", r.bleed_minutes);
}

bool start_fight(const net::AttackMsg& a, double now) {
    if (g_fight.active) return false;
    const auto puppet = g_puppets.find(a.animal);
    if (puppet == g_puppets.end()) {
        logx::info("wildlife: the host's #%u attacks us but isn't shown here; no fight", a.animal);
        return false;
    }
    // The fight starts at our feet, on the side the host's animal comes from: on the host's screen it has reached our
    // avatar, while here (a moment behind, or where our views of the ground differ) its copy can still be some way off.
    Vector3 seen{};
    il2::invoke_value(api.get_position, target(puppet->second.transform), seen);
    const game::PlayerState me = game::read_player();
    float dx = seen.x - me.position.x, dz = seen.z - me.position.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.1f) {
        dx = std::sin(me.yaw / 57.29578f);
        dz = std::cos(me.yaw / 57.29578f);
    } else {
        dx /= len;
        dz /= len;
    }
    Vector3 where{me.position.x + dx * kFightStartMetres, me.position.y, me.position.z + dz * kFightStartMetres};
    const float yaw = std::atan2(-dx, -dz) * 57.29578f;  // facing us
    Il2CppObject* ai = our_animal_of_kind(a.kind);
    bool made_for_it = false;
    if (!ai) {
        ai = spawn_stand_in(a.kind, where, yaw);
        made_for_it = ai != nullptr;
    }
    if (!ai) {
        logx::info("wildlife: the host's %s #%u attacks us, but we have no %s of our own and none could be made",
                   kind_name(a.kind), a.animal, kind_name(a.kind));
        return false;
    }
    Il2CppObject* go = go_of(ai);
    set_active(go, true);
    prepare_stand_in(ai, puppet->second.latest.hp);
    Il2CppObject* tf = transform_of(ai);
    void* pos_args[] = {&where};
    il2::invoke(api.set_position, tf, pos_args);
    Vector3 facing{0, yaw, 0};
    void* rot_args[] = {&facing};
    il2::invoke(api.set_euler, tf, rot_args);
    set_active(target(puppet->second.go), false);  // ours fights in its place

    int32_t mode = kAiModeStruggle;
    void* mode_args[] = {&mode};
    il2::invoke(api.set_mode, ai, mode_args);
    Il2CppObject* struggle = il2::invoke(api.get_struggle, nullptr);
    void* begin_args[] = {go};
    il2::invoke(api.struggle_begin, struggle, begin_args);
    g_fight = {true, a.animal, hold(ai), now, made_for_it, il2::read_field<float>(ai, api.ai_hp), false, false};
    logx::info("wildlife: the host's %s #%u jumped us: fighting it here (with %s)", kind_name(a.kind), a.animal,
               made_for_it ? "one made for the fight" : "one of ours");
    return true;
}

// The fight is over here too (TLD's struggle has ended, camera and all): the stand-in goes back, the host's animal
// shows again. The host was told how it ended as soon as TLD decided (report_fight), or is told now.
void finish_fight(const char* why, bool report) {
    if (!g_fight.active) return;
    Il2CppObject* ai = target(g_fight.ai);
    if (report && !g_fight.reported) report_fight(ai, why);
    if (ai && alive(ai)) {
        if (g_fight.made_for_it)
            destroy(go_of(ai), false);  // only here for the fight
        else
            set_active(go_of(ai), false);  // back among our hidden animals
    }
    if (const auto puppet = g_puppets.find(g_fight.animal); puppet != g_puppets.end() && !puppet->second.carcass)
        set_active(target(puppet->second.go), true);
    logx::info("wildlife: our fight with the host's #%u is over (%s)", g_fight.animal, why);
    drop(g_fight.ai);
    g_fight = {};
}

// TLD decides the fight the moment the stand-in leaves Struggle mode (its health and bleeding are final from then):
// the host hears it right away, while our camera and the get-up animation still play out.
void watch_fight() {
    if (!g_fight.active || g_fight.reported) return;
    Il2CppObject* ai = target(g_fight.ai);
    if (!ai || !alive(ai)) return;
    const int32_t mode = il2::read_field<int32_t>(ai, api.ai_mode);
    if (mode == kAiModeStruggle && !g_fight.saw_struggle) {
        // A stand-in spawned for the fight resets its health in its first Start (a frame after start_fight wrote the
        // host's): give it the host's animal's again before any tap lands (taps wait for the weapon picker).
        g_fight.saw_struggle = true;
        if (const auto puppet = g_puppets.find(g_fight.animal); puppet != g_puppets.end() && puppet->second.latest.hp > 0)
            write_field<float>(ai, api.ai_hp, puppet->second.latest.hp);
        g_fight.hp_begin = il2::read_field<float>(ai, api.ai_hp);
    } else if (mode != kAiModeStruggle && g_fight.saw_struggle) {
        report_fight(ai, "it left the fight");
    }
}

void handle_attacks(double now, bool can_fight) {
    std::vector<net::AttackMsg> attacks;
    netrt::drain_attacks(attacks);
    for (const net::AttackMsg& a : attacks) {
        if (a.type == net::AttackType::Struggle) {
            if (!can_fight || !start_fight(a, now)) reply(a.animal, net::FightOutcome::NoFight, 0);
        } else if (a.type == net::AttackType::Damage && can_fight) {
            Il2CppObject* condition = il2::invoke(api.get_condition, nullptr);
            float hp = -a.damage;
            int32_t source = a.source;
            bool show = true;
            void* args[] = {&hp, &source, &show};
            if (condition) il2::invoke(api.add_health, condition, args);
            logx::info("wildlife: the host's %s #%u hurt us: %.1f damage", kind_name(a.kind), a.animal, a.damage);
        } else if (a.type == net::AttackType::Wounds && can_fight) {
            Il2CppObject* ai = our_animal_of_kind(a.kind);
            Il2CppObject* table = ai ? il2::invoke(api.damage_table, ai) : nullptr;
            if (table) {
                int32_t options = a.options;
                void* args[] = {ai, &options};
                il2::invoke(api.apply_wounds, table, args);
            }
            logx::info("wildlife: the host's %s #%u wounded us%s", kind_name(a.kind), a.animal,
                       table ? "" : " (no animal of that kind here to take the wounds from)");
        }
    }
    watch_fight();
    if (g_fight.active && now - g_fight.started_ms > 1500 && !game::in_struggle()) finish_fight("struggle ended", true);
    if (g_fight.active && now - g_fight.started_ms > kLocalFightTimeoutMs) finish_fight("took too long", true);
}

void hide_our_animals(double now) {
    if (now - g_last_hide_scan < kHideScanMs) return;
    g_last_hide_scan = now;
    int hidden = 0;
    Il2CppObject* fighter = g_fight.active ? target(g_fight.ai) : nullptr;
    for (Il2CppObject* ai : all_animals()) {
        Il2CppObject* go = go_of(ai);
        if (!active(go) || ai == fighter) continue;
        if (std::find(g_carcasses.begin(), g_carcasses.end(), go) != g_carcasses.end()) continue;
        set_active(go, false);
        g_hidden.push_back(hold(go));
        ++hidden;
    }
    if (hidden) logx::info("wildlife: hid %d of our own animal(s); the host's are shown instead", hidden);
}

void stop_mirroring(const char* why) {
    if (!g_mirroring) return;
    finish_fight(why, true);
    g_mirroring = false;
    for (auto& [id, p] : g_puppets) remove_puppet(p);
    g_puppets.clear();
    int restored = 0;
    for (uintptr_t& h : g_hidden) {
        if (Il2CppObject* go = target(h); alive(go)) {
            set_active(go, true);
            ++restored;
        }
        drop(h);
    }
    g_hidden.clear();
    g_incoming.clear();
    g_spawns_blocked = 0;
    logx::info("wildlife: stopped mirroring the host's animals (%s); %d of ours back", why, restored);
}

void mirror(const std::string& scene, double now) {
    if (!g_mirroring || g_mirror_scene != scene) {
        stop_mirroring("scene changed");
        g_mirroring = true;
        g_mirror_scene = scene;
        g_failed_kinds = 0;
        logx::info("wildlife: mirroring the host's animals in '%s'", scene.c_str());
    }
    hide_our_animals(now);

    g_incoming.clear();
    netrt::drain_animals(g_incoming);
    for (const net::AnimalsMsg& part : g_incoming) {
        if (part.scene != scene) continue;
        for (const net::AnimalRec& rec : part.animals) {
            auto it = g_puppets.find(rec.id);
            if (it != g_puppets.end() && (it->second.kind != rec.kind || !alive(target(it->second.go)))) {
                remove_puppet(it->second);
                g_puppets.erase(it);
                it = g_puppets.end();
            }
            if (it == g_puppets.end()) {
                Puppet p;
                if (!make_puppet(rec.id, rec, p)) continue;
                if (g_carcass_ids.count(rec.id)) {  // our carcass kept while the link was down shows it
                    p.carcass = true;
                    free_stuck_arrows(target(p.go));
                    set_active(target(p.go), false);
                }
                it = g_puppets.emplace(rec.id, std::move(p)).first;
            }
            Puppet& p = it->second;
            if (p.carcass) {  // a real carcass shows it now; the copy just stays alive so the id isn't re-made
                p.updated_ms = now;
                continue;
            }
            net::StateMsg s;
            s.t_ms = part.t_ms;
            s.valid = true;
            s.scene = scene;
            s.x = rec.x;
            s.y = rec.y;
            s.z = rec.z;
            s.yaw = rec.yaw;
            s.pitch = rec.pitch;
            s.roll = rec.roll;
            p.track.add(s, part.received_ms);
            p.latest = rec;
            p.updated_ms = now;
        }
    }

    play_host_sounds();
    handle_attacks(now, true);

    for (auto it = g_puppets.begin(); it != g_puppets.end();) {
        Puppet& p = it->second;
        if (now - p.updated_ms > kStaleMs || !alive(target(p.go))) {
            logx::info("wildlife: the host's %s #%u is gone", kind_name(p.kind), it->first);
            remove_puppet(p);
            it = g_puppets.erase(it);
            continue;
        }
        net::StateMsg s;
        if (!p.carcass && p.track.sample(now, s)) {
            Vector3 pos{s.x, s.y, s.z}, euler{s.pitch, s.yaw, s.roll};
            void* pos_args[] = {&pos};
            void* rot_args[] = {&euler};
            il2::invoke(api.set_position, target(p.transform), pos_args);
            il2::invoke(api.set_euler, target(p.transform), rot_args);
        }
        apply_params(p, it->first);
        apply_states(p, now);
        ++it;
    }
}

// While we mirror the host's animals, TLD's own spawning stays off: every spawn it makes at runtime (topping up a
// region, respawning) goes through SpawnRegion.Spawn, and anything it made would only be hidden again. Animals
// restored from the save at load are hidden once by hide_our_animals.
// In co-op TLD's weapon picker (the choice of fists or a weapon when an animal jumps you) stays up for a fraction
// of the game's time ([struggle] picker_scale in coop.ini): less time to choose, a harder fight.
using PickerEnableFn = void (*)(Il2CppObject* self, bool enable, Il2CppObject* items, float seconds,
                                const MethodInfo* method);
PickerEnableFn g_picker_enable = nullptr;
float g_picker_scale = 0.5f;

void picker_enable_detour(Il2CppObject* self, bool enable, Il2CppObject* items, float seconds, const MethodInfo* method) {
    if (enable && netrt::role() && g_picker_scale > 0.05f && g_picker_scale != 1.0f) {
        logx::info("wildlife: weapon picker up for %.2f s (TLD's %.2f s x %.2f)", seconds * g_picker_scale, seconds,
                   g_picker_scale);
        seconds *= g_picker_scale;
    }
    g_picker_enable(self, enable, items, seconds, method);
}

using SpawnFn = bool (*)(Il2CppObject* self, int32_t wildlife_mode, const MethodInfo* method);
SpawnFn g_spawn = nullptr;

bool spawn_detour(Il2CppObject* self, int32_t wildlife_mode, const MethodInfo* method) {
    if (g_mirroring) {
        if (g_spawns_blocked++ == 0) logx::info("wildlife: TLD's own spawning is off while the host's animals are shown");
        return false;
    }
    return g_spawn(self, wildlife_mode, method);
}

double now_ms() {
    static const double ticks_per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / ticks_per_ms;
}

}  // namespace

uint32_t host_animal_id(Il2CppObject* ai) {
    const auto it = g_ids.find(reinterpret_cast<uintptr_t>(ai));
    return it == g_ids.end() ? 0 : it->second;
}

float host_animal_hp(Il2CppObject* ai) { return il2::read_field<float>(ai, api.ai_hp); }

bool mirroring() { return g_mirroring; }

void carcass_made(uint32_t id, Il2CppObject* carcass_go) {
    if (carcass_go) g_carcasses.push_back(carcass_go);
    g_carcass_ids.insert(id);
    if (auto it = g_puppets.find(id); it != g_puppets.end()) {
        it->second.carcass = true;
        free_stuck_arrows(target(it->second.go));
        set_active(target(it->second.go), false);
    }
}

void carcass_gone(uint32_t id, Il2CppObject* carcass_go) {
    g_carcasses.erase(std::remove(g_carcasses.begin(), g_carcasses.end(), carcass_go), g_carcasses.end());
    g_carcass_ids.erase(id);
}

Il2CppObject* host_animal(uint32_t id) {
    for (const auto& [ai, animal] : g_ids)
        if (animal == id) return reinterpret_cast<Il2CppObject*>(ai);
    return nullptr;
}

bool puppet_at(Il2CppObject* transform, uint32_t& id, bool& dead) {
    static const std::string kPrefix = "TLDCoop_HostAnimal_";
    for (int depth = 0; transform && depth < 32; ++depth) {
        const std::string name = il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.get_name, transform)));
        if (name.rfind(kPrefix, 0) == 0) {
            id = static_cast<uint32_t>(strtoul(name.c_str() + kPrefix.size(), nullptr, 10));
            const auto it = g_puppets.find(id);
            if (it == g_puppets.end()) return false;
            dead = (it->second.latest.flags & net::kAnimalDead) != 0;
            return true;
        }
        transform = il2::invoke(api.get_parent, transform);
    }
    return false;
}

std::string describe_sounds() {
    std::lock_guard lock(g_sound_mutex);
    char line[256];
    snprintf(line, sizeof(line), "Wwise posts seen %llu, on our animals %llu; %zu animal sound objects known; hooks %s",
             static_cast<unsigned long long>(g_posts_seen), static_cast<unsigned long long>(g_posts_matched),
             g_emitter_owner.size(), g_post_id ? "in" : "NOT in");
    std::string out = line;
    for (const auto& [object, owner] : g_emitter_owner) {
        snprintf(line, sizeof(line), "\n  animal #%u emitter %u = Wwise object %llu", owner.first, owner.second,
                 static_cast<unsigned long long>(object));
        out += line;
    }
    for (size_t i = 0; i < std::size(g_recent_posts); ++i) {
        const SeenPost& p = g_recent_posts[(g_recent_next + i) % std::size(g_recent_posts)];
        if (!p.event && !p.object) continue;
        snprintf(line, sizeof(line), "\n  recent post: event %u on Wwise object %llu%s", p.event,
                 static_cast<unsigned long long>(p.object), p.matched ? " (an animal)" : "");
        out += line;
    }
    return out;
}

bool init() {
    Il2CppClass* ai_manager = il2::find_class(kGame, "", "BaseAiManager");
    Il2CppClass* base_ai = il2::find_class(kGame, "", "BaseAi");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* spawn_manager = il2::find_class(kGame, "", "SpawnRegionManager");
    Il2CppClass* spawn_region = il2::find_class(kGame, "", "SpawnRegion");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* animator = il2::find_class(kAnim, "UnityEngine", "Animator");
    Il2CppClass* param = il2::find_class(kAnim, "UnityEngine", "AnimatorControllerParameter");

    if (ai_manager) api.ai_list = il2::il2cpp_class_get_field_from_name(ai_manager, "m_BaseAis");
    api.ai_kind = il2::field_offset(base_ai, "m_AiSubType");
    api.ai_mode = il2::field_offset(base_ai, "m_CurrentMode");
    api.ai_hp = il2::field_offset(base_ai, "m_CurrentHP");
    api.ai_animator = il2::field_offset(base_ai, "m_Animator");
    api.spawn_manager = il2::find_method(game_manager, "GetSpawnRegionManager", 0);
    api.spawn_regions = il2::field_offset(spawn_manager, "m_SpawnRegions");
    api.region_prefab = il2::field_offset(spawn_region, "m_SpawnablePrefab");
    api.region_kind = il2::field_offset(spawn_region, "m_AiSubTypeSpawned");

    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.go_ctor = il2::find_method_params(game_object, ".ctor", {"System.String"});
    api.go_active = il2::find_method(game_object, "get_activeInHierarchy", 0);
    api.go_active_self = il2::find_method(game_object, "get_activeSelf", 0);
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.go_components = il2::find_method_params(game_object, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.get_euler = il2::find_method(transform, "get_eulerAngles", 0);
    api.set_euler = il2::find_method(transform, "set_eulerAngles", 1);
    api.set_parent = il2::find_method(transform, "SetParent", 2);
    api.instantiate = il2::find_method_params(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
    api.destroy = il2::find_method(object, "Destroy", 1);
    api.destroy_now = il2::find_method(object, "DestroyImmediate", 1);
    api.alive = il2::find_method(object, "op_Implicit", 1);
    api.set_name = il2::find_method(object, "set_name", 1);
    api.game_object_class = game_object;
    api.component_type = il2::type_object(component);
    api.animator_type = il2::type_object(animator);
    for (Il2CppObject* t : {api.component_type, api.animator_type})
        if (t) il2::il2cpp_gchandle_new(t, false);

    api.layer_count = il2::find_method(animator, "get_layerCount", 0);
    api.state_info = il2::find_method(animator, "GetCurrentAnimatorStateInfo", 1);
    api.in_transition = il2::find_method(animator, "IsInTransition", 1);
    api.param_count = il2::find_method(animator, "get_parameterCount", 0);
    api.get_param = il2::find_method(animator, "GetParameter", 1);
    api.param_hash = il2::find_method(param, "get_nameHash", 0);
    api.param_type = il2::find_method(param, "get_type", 0);
    api.get_float = il2::find_method_params(animator, "GetFloat", {"System.Int32"});
    api.set_float = il2::find_method_params(animator, "SetFloat", {"System.Int32", "System.Single"});
    api.get_int = il2::find_method_params(animator, "GetInteger", {"System.Int32"});
    api.set_int = il2::find_method_params(animator, "SetInteger", {"System.Int32", "System.Int32"});
    api.get_bool = il2::find_method_params(animator, "GetBool", {"System.Int32"});
    api.set_bool = il2::find_method_params(animator, "SetBool", {"System.Int32", "System.Boolean"});
    api.play = il2::find_method_params(animator, "Play", {"System.Int32", "System.Int32", "System.Single"});
    api.crossfade = il2::find_method_params(animator, "CrossFadeInFixedTime", {"System.Int32", "System.Single", "System.Int32"});
    api.set_root_motion = il2::find_method(animator, "set_applyRootMotion", 1);
    // Wwise's Unity classes live in their own assemblies.
    if (Il2CppClass* ak_object = il2::find_class("AK.Wwise.Unity.MonoBehaviour.dll", "", "AkGameObj")) {
        api.ak_object_type = il2::type_object(ak_object);
        if (api.ak_object_type) il2::il2cpp_gchandle_new(api.ak_object_type, false);
    }
    api.wwise_id = il2::find_method(il2::find_class("AK.Wwise.Unity.API.dll", "", "AkSoundEngine"), "GetAkGameObjectID", 1);
    api.base_ai_type = il2::type_object(base_ai);
    if (api.base_ai_type) il2::il2cpp_gchandle_new(api.base_ai_type, false);
    api.go_component = il2::find_method_params(game_object, "GetComponent", {"System.Type"});
    api.set_mode = il2::find_method(base_ai, "SetAiMode", 1);
    api.damage_table = il2::find_method(base_ai, "GetAttackDamageEvents", 0);
    api.apply_wounds = il2::find_method(il2::find_class(kGame, "", "DamageEventTable"), "MaybeApplyAttack", 2);
    api.get_struggle = il2::find_method(game_manager, "GetPlayerStruggleComponent", 0);
    api.struggle_begin = il2::find_method_params(il2::find_class(kGame, "", "PlayerStruggle"), "Begin", {"UnityEngine.GameObject"});
    api.get_condition = il2::find_method(game_manager, "GetConditionComponent", 0);
    api.region_refs = il2::field_offset(spawn_region, "m_SpawnsPrefabReferences");
    api.region_center = il2::field_offset(spawn_region, "m_Center");
    api.instantiate_spawn = il2::find_method(spawn_region, "InstantiateSpawn", 6);
    api.add_health = il2::find_method_params(il2::find_class(kGame, "", "Condition"), "AddHealth",
                                             {"System.Single", "DamageSource", "System.Boolean"});
    api.set_culling = il2::find_method(animator, "set_cullingMode", 1);
    api.localized_damage_type = il2::type_object(il2::find_class(kGame, "", "LocalizedDamage"));
    if (api.localized_damage_type) il2::il2cpp_gchandle_new(api.localized_damage_type, false);
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    api.get_name = il2::find_method(object, "get_name", 0);
    if (Il2CppClass* arrow = il2::find_class(kGame, "", "ArrowItem")) {
        api.arrow_type = il2::type_object(arrow);
        if (api.arrow_type) il2::il2cpp_gchandle_new(api.arrow_type, false);
        api.arrow_detach = il2::find_method(arrow, "DetachIntoWorld", 0);
    }
    api.ai_wounded = il2::field_offset(base_ai, "m_Wounded");
    api.ai_bleeding = il2::field_offset(base_ai, "m_BleedingOut");
    api.ai_bleed_elapsed = il2::field_offset(base_ai, "m_ElapsedBleedingOutMinutes");
    api.ai_bleed_limit = il2::field_offset(base_ai, "m_DeathAfterBleeingOutMinutes");

    g_ready = api.go_transform && api.ai_list && api.ai_kind >= 0 && api.ai_mode >= 0 && api.ai_animator >= 0 && api.spawn_manager &&
              api.spawn_regions >= 0 && api.region_prefab >= 0 && api.region_kind >= 0 && api.go_active &&
              api.go_components && api.set_position && api.set_euler && api.instantiate && api.layer_count &&
              api.state_info && api.param_count && api.get_param && api.param_hash && api.param_type && api.get_float &&
              api.set_float && api.get_int && api.set_int && api.get_bool && api.set_bool && api.play && api.crossfade;
    if (g_ready) {
        if (const MethodInfo* spawn = il2::find_method(spawn_region, "Spawn", 1))
            hooks::install_method("SpawnRegion.Spawn", spawn, reinterpret_cast<void*>(&spawn_detour),
                                  reinterpret_cast<void**>(&g_spawn));
        g_picker_scale = config::load().picker_scale;
        if (const MethodInfo* picker = il2::find_method_params(il2::find_class(kGame, "", "Panel_WeaponPicker"), "Enable",
                                                               {"System.Boolean",
                                                                "System.Collections.Generic.List<GearItem>",
                                                                "System.Single"}))
            hooks::install_method("Panel_WeaponPicker.Enable", picker, reinterpret_cast<void*>(&picker_enable_detour),
                                  reinterpret_cast<void**>(&g_picker_enable));
        else
            logx::warn("wildlife: TLD's weapon picker not found; its time stays the game's own");
    }
    logx::info("wildlife sync: %s", g_ready ? "ready" : "UNAVAILABLE (TLD API missing; see warnings above)");
    return g_ready;
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    install_sound_hooks();
    const auto role = netrt::role();
    const double now = now_ms();
    {
        // Host: hand over what our animals said since the last frame.
        std::vector<net::AnimalSound> sounds;
        {
            std::lock_guard lock(g_sound_mutex);
            g_sound_host = role && *role == net::Role::Host;
            sounds.swap(g_sounds_out);
        }
        if (!sounds.empty() && g_sound_host) netrt::send_animal_sounds(sounds);
    }
    if (!role) {
        stop_mirroring("co-op off");
        return;
    }
    if (*role == net::Role::Host) {
        if (!in_game || now - g_last_capture < kCaptureMs) return;
        g_last_capture = now;
        capture(scene, now);
        return;
    }
    const auto host = netrt::remote_state();
    if (!in_game || !host || !host->valid || host->scene != scene) {
        stop_mirroring(!in_game ? "we're not in the game" : !host ? "not connected" : !host->valid ? "host loading"
                                                                                                     : "different scenes");
        handle_attacks(now, false);  // can't fight what we don't show: the host's animal gives up
        g_incoming.clear();
        netrt::drain_animals(g_incoming);  // drop what we can't use
        g_incoming.clear();
        return;
    }
    mirror(scene, now);
}

}  // namespace wildlife_sync
