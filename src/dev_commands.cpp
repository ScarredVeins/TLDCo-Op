#include "dev_commands.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "game_state.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/held.h"
#include "net_runtime.h"
#include "avatar.h"
#include "coop_death.h"
#include "coop_give.h"
#include "coop_hud.h"
#include "coop_lapse.h"
#include "coop_sleep.h"
#include "coop_menu.h"
#include "coop_notices.h"
#include "paths.h"
#include "test_harness.h"
#include "wildlife_carcass.h"
#include "wildlife_hits.h"
#include "wildlife_sync.h"
#include "wildlife_targets.h"
#include "world_containers.h"
#include "world_items.h"
#include "world_fires.h"
#include "world_join.h"
#include "world_mode.h"
#include "world_objects.h"

namespace dev_commands {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr ULONGLONG kPollMs = 500;
constexpr float kStandOffMetres = 3.0f;
constexpr ptrdiff_t kListItems = 0x10;  // List<T>: T[] _items
constexpr ptrdiff_t kListSize = 0x18;   //          int _size

struct Vector3 {
    float x, y, z;
};
struct Quaternion {
    float x, y, z, w;
};

struct NavMeshHit {
    Vector3 position;
    Vector3 normal;
    float distance;
    int32_t mask;
    int32_t hit;
};

constexpr const char* kAnimalKinds[] = {"none", "wolf", "bear", "stag", "rabbit", "moose", "cougar"};  // AiSubType
constexpr int32_t kAiModeWander = 15;
constexpr int32_t kAiModeDead = 2;
constexpr float kSpawnAheadMetres = 25.0f;
constexpr float kGotoAnimalMetres = 25.0f;

struct Api {
    FieldInfo* ai_list = nullptr;                 // static List<BaseAi> BaseAiManager.m_BaseAis
    ptrdiff_t ai_kind = -1, ai_mode = -1, ai_hp = -1;  // BaseAi.m_AiSubType, m_CurrentMode, m_CurrentHP
    Il2CppClass* ai_mode_enum = nullptr;
    const MethodInfo* experience_mode = nullptr;  // static ExperienceModeManager.GetCurrentExperienceModeType()
    Il2CppClass* experience_enum = nullptr;
    const MethodInfo* spawn_manager = nullptr;    // static GameManager.GetSpawnRegionManager()
    ptrdiff_t spawn_regions = -1;                 // SpawnRegionManager.m_SpawnRegions (List<SpawnRegion>)
    ptrdiff_t region_prefab = -1, region_refs = -1, region_kind = -1;  // SpawnRegion.m_SpawnablePrefab, ...
    ptrdiff_t region_center = -1;                 // SpawnRegion.m_Center
    const MethodInfo* go_active = nullptr;        // GameObject.get_activeSelf
    const MethodInfo* go_set_active = nullptr;
    const MethodInfo* instantiate_spawn = nullptr;  // SpawnRegion.InstantiateSpawn(prefab, ref, pos, rot, mode, wm)
    const MethodInfo* navmesh_sample = nullptr;   // static NavMesh.SamplePosition(Vector3, out hit, float, int)
    const MethodInfo* clone_at = nullptr;         // static Object.Instantiate(Object, Vector3, Quaternion)
    const MethodInfo* component_go = nullptr;     // Component.get_gameObject()
    FieldInfo* door_list = nullptr;               // static List<LoadScene> LoadScene.m_LoadScenesList
    ptrdiff_t door_target = -1;                   // LoadScene.m_SceneToLoad
    const MethodInfo* door_scene = nullptr;       // LoadScene.GetSceneToLoad(): where it really goes (a building used
                                                  // in several regions goes back to the one we came from)
    ptrdiff_t door_instanced = -1;                // LoadScene.m_SceneCanBeInstanced
    FieldInfo* transition = nullptr;              // static GameManager.m_SceneTransitionData
    ptrdiff_t force_next = -1, last_outdoor = -1; // SceneTransitionData.m_ForceNextSceneLoadTriggerScene, m_LastOutdoorScene
    const MethodInfo* door_activate = nullptr;    // LoadScene.Activate(bool transitionImmediately): go through it
    const MethodInfo* is_outdoors = nullptr;      // static GameManager.IsOutDoorsScene(string)
    const MethodInfo* player_manager = nullptr;   // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* teleport = nullptr;         // PlayerManager.TeleportPlayer(Vector3, Quaternion)
    const MethodInfo* component_transform = nullptr;  // Component.get_transform()
    const MethodInfo* transform_position = nullptr;   // Transform.get_position()
} api;

bool g_ready = false;
ULONGLONG g_last_poll = 0;
std::string g_at_command;  // `at MS COMMAND`: runs when the PC's clock reaches MS (two copies at the same moment)
long long g_at_ms = 0;

long long wall_ms() {  // ms since 1970 (UTC), as Python's time.time() * 1000
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER t{};
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(t.QuadPart / 10000ULL) - 11644473600000LL;
}

// Fight helpers: TLD's console (uConsole.RunCommand, e.g. "gear_add GEAR_Hatchet"), and taps in a struggle as if the
// player were hammering the key: what PlayerStruggle.UpdateTapInput calls on a key press, WolfTap in a wolf fight
// (the tap bar plus the weapon's damage), DoTap otherwise (the tap bar only).
struct FightApi {
    const MethodInfo* run_console = nullptr;  // static uConsole.RunCommand(string)
    const MethodInfo* get_struggle = nullptr; // static GameManager.GetPlayerStruggleComponent()
    const MethodInfo* do_tap = nullptr;       // PlayerStruggle.DoTap()
    const MethodInfo* wolf_tap = nullptr;     // PlayerStruggle.WolfTap()
    ptrdiff_t partner = -1;                   // PlayerStruggle.m_PartnerBaseAi
    const MethodInfo* picker_pending = nullptr;  // PlayerStruggle.IsWeaponPickerSelectionPending()
    const MethodInfo* weapon_picked = nullptr;   // PlayerStruggle.OnWeaponPicked(GearItem): the picker's choice
    const MethodInfo* get_inventory = nullptr;   // static GameManager.GetInventoryComponent()
    const MethodInfo* best_weapon = nullptr;     // Inventory.GetBestStuggleBonusItem()
    const MethodInfo* object_name = nullptr;     // Object.get_name
    ptrdiff_t weapon_used = -1;                  // PlayerStruggle.m_WeaponUsed
    ptrdiff_t weapon_choice = -1;                // PlayerStruggle.m_WeaponPicked (the picker's choice so far)
    // equip: what TLD's own "rifle"/"bow" console commands do, step by step
    const MethodInfo* add_item = nullptr;        // PlayerManager.AddItemCONSOLE(string, int, float)
    const MethodInfo* use_item = nullptr;        // PlayerManager.UseInventoryItem(GearItem, bool): equips a weapon
    const MethodInfo* in_hands = nullptr;        // PlayerManager.get_m_ItemInHands
    const MethodInfo* equip_item = nullptr;      // PlayerManager.EquipItem(GearItem, bool fromDeserialize)
    const MethodInfo* ready_to_equip = nullptr;  // PlayerManager.IsReadyToEquip()
    const MethodInfo* fps_camera = nullptr;      // static GameManager.GetVpFPSCamera()
    const MethodInfo* weapon_switching = nullptr;  // vp_FPSCamera.WeaponSwitchInProgress()
    const MethodInfo* find_objects = nullptr;    // static Object.FindObjectsOfType(Type)
    const MethodInfo* find_objects_inactive = nullptr;  // static Object.FindObjectsOfType(Type, bool)
    Il2CppObject* shooter_type = nullptr;
    const MethodInfo* add_rounds = nullptr;      // GunItem.AddRoundsToClip(int, int)
    ptrdiff_t gun_item = -1;                     // GearItem.m_GunItem
    const MethodInfo* apply_damage = nullptr;    // virtual BaseAi.ApplyDamage(float, float, DamageSource, string)
    const MethodInfo* set_enabled = nullptr;     // Behaviour.set_enabled
    const MethodInfo* animator_speed = nullptr;  // Animator.set_speed
    ptrdiff_t ai_animator = -1;                  // BaseAi.m_Animator
} fight;
// hands: the item in our hands lit, put out or aimed, as TLD's own actions do (without their animations).
struct HandsApi {
    ptrdiff_t torch = -1, flare = -1, lamp = -1, flashlight = -1, gun = -1, bow = -1, stone = -1, spear = -1;  // GearItem
    const MethodInfo* torch_ignite = nullptr;     // TorchItem.Ignite()
    const MethodInfo* torch_out = nullptr;        // TorchItem.ForceExtinguish()
    const MethodInfo* flare_ignite = nullptr;     // FlareItem.Ignite()
    const MethodInfo* lamp_turn_on = nullptr;     // KeroseneLampItem.TurnOn(bool supressStartAudio)
    const MethodInfo* lamp_turn_off = nullptr;    // KeroseneLampItem.TurnOff(bool playAudio)
    const MethodInfo* lamp_room = nullptr;        // KeroseneLampItem.GetMaxFuelToAdd()
    const MethodInfo* lamp_fill = nullptr;        // KeroseneLampItem.AddFuel(ItemLiquidVolume)
    const MethodInfo* flashlight_on = nullptr;    // FlashlightItem.TurnOn()
    const MethodInfo* flashlight_off = nullptr;   // FlashlightItem.TurnOffImmediate()
    const MethodInfo* flashlight_lights = nullptr;  // FlashlightItem.EnableLights(State)
    ptrdiff_t flashlight_state = -1;              // FlashlightItem.m_State
    ptrdiff_t flashlight_charge = -1;             // FlashlightItem.m_CurrentBatteryCharge (0-1)
    const MethodInfo* put_away = nullptr;         // PlayerManager.UnequipImmediate(bool saveLastItem)
    // ZoomStart / ZoomEnd: what the aim button does (GunItem, BowItem, StoneItem, BearSpearItem)
    const MethodInfo* gun_aim = nullptr, *gun_lower = nullptr, *bow_aim = nullptr, *bow_lower = nullptr;
    const MethodInfo* stone_aim = nullptr, *stone_lower = nullptr, *spear_aim = nullptr, *spear_lower = nullptr;
} held_api;

uintptr_t g_frozen = 0;          // 'freeze': the paused animal...
ULONGLONG g_frozen_until = 0;    // ...until then
float g_taps_per_second = 0;
double g_tap_credit = 0;
ULONGLONG g_last_tap_frame = 0;
int g_taps_done = 0;
bool g_weapon_chosen = false;  // the struggle's weapon picker was answered this fight

struct Door {
    Il2CppObject* door = nullptr;  // LoadScene
    Vector3 pos;
    std::string target;  // where it goes
    std::string own;     // its m_SceneToLoad, when that isn't where it goes
    bool instanced = false;
    bool interior = false;
    float distance = 0;
};

std::vector<Door> list_doors(const game::Vec3& me) {
    std::vector<Door> doors;
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(api.door_list, &list);
    Il2CppObject* items = il2::read_field<Il2CppObject*>(list, kListItems);
    const int32_t count = il2::read_field<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* door = il2::array_object_at(items, static_cast<size_t>(i));
        if (!door) continue;
        Door d;
        d.door = door;
        Il2CppObject* tf = il2::invoke(api.component_transform, door);
        if (!tf || !il2::invoke_value(api.transform_position, tf, d.pos)) continue;
        d.target = il2::to_utf8(il2::read_field<Il2CppString*>(door, api.door_target));
        if (api.door_scene) {
            const std::string to = il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.door_scene, door)));
            if (!to.empty() && to != d.target) d.own = d.target, d.target = to;
        }
        d.instanced = il2::read_field<bool>(door, api.door_instanced);
        bool outdoors = true;
        void* args[] = {il2::il2cpp_string_new(d.target.c_str())};
        il2::invoke_value(api.is_outdoors, nullptr, outdoors, args);
        d.interior = !d.target.empty() && !outdoors;
        const float dx = d.pos.x - me.x, dy = d.pos.y - me.y, dz = d.pos.z - me.z;
        d.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        doors.push_back(d);
    }
    return doors;
}

void cmd_doors(const game::Vec3& me) {
    const auto doors = list_doors(me);
    Il2CppObject* transition = nullptr;
    if (api.transition) il2::il2cpp_field_static_get_value(api.transition, &transition);
    auto field_text = [&](ptrdiff_t off) {
        return transition && off >= 0 ? il2::to_utf8(il2::read_field<Il2CppString*>(transition, off)) : std::string();
    };
    logx::info("dev: %zu doors here (TLD's next scene override '%s', last outdoor scene '%s')", doors.size(),
               field_text(api.force_next).c_str(), field_text(api.last_outdoor).c_str());
    for (size_t i = 0; i < doors.size(); ++i) {
        const Door& d = doors[i];
        logx::info("dev:   [%zu] -> '%s' (%s) at (%.1f, %.1f, %.1f), %.0f m away%s%s%s%s", i, d.target.c_str(),
                   d.interior ? "building" : "outdoors", d.pos.x, d.pos.y, d.pos.z, d.distance,
                   d.instanced ? "; a building used in several regions" : "", d.own.empty() ? "" : "; its own scene is '",
                   d.own.c_str(), d.own.empty() ? "" : "'");
    }
}

// Door `index`, or (-1) the nearest one into a building.
int pick_door(const std::vector<Door>& doors, int index, const char* verb) {
    int pick = index;
    if (pick < 0) {
        for (size_t i = 0; i < doors.size(); ++i)
            if (doors[i].interior && (pick < 0 || doors[i].distance < doors[pick].distance)) pick = static_cast<int>(i);
    }
    if (pick < 0 || static_cast<size_t>(pick) >= doors.size()) {
        logx::warn("dev: %s: no such door (%zu here; try 'doors')", verb, doors.size());
        return -1;
    }
    return pick;
}

// Go through door `index` (default: the nearest into a building) the way using it does (LoadScene.Activate).
void cmd_enter(const game::Vec3& me, int index) {
    const auto doors = list_doors(me);
    const int pick = pick_door(doors, index, "enter");
    if (pick < 0) return;
    const Door& d = doors[static_cast<size_t>(pick)];
    // A building used in several regions goes back to the region we came from through TLD's next scene override,
    // which going in sets. With saving off (the test rig's nosave) it's gone by the time we're inside, and the door
    // would take us to its own region: put back TLD's last outdoor scene.
    std::string back;
    Il2CppObject* transition = nullptr;
    if (api.transition) il2::il2cpp_field_static_get_value(api.transition, &transition);
    if (transition && !d.interior && api.force_next >= 0 && api.last_outdoor >= 0) {
        const std::string last = il2::to_utf8(il2::read_field<Il2CppString*>(transition, api.last_outdoor));
        if (!last.empty() && last != d.target) {
            Il2CppString* s = il2::il2cpp_string_new(last.c_str());
            il2::il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(s), false);  // kept alive (no write barrier here)
            memcpy(reinterpret_cast<char*>(transition) + api.force_next, &s, sizeof(s));
            back = last;
        }
    }
    bool immediately = false;
    void* args[] = {&immediately};
    il2::invoke(api.door_activate, d.door, args);
    logx::info("dev: enter: door [%d] -> '%s' (%s), %.0f m away: activated%s%s%s", pick, back.empty() ? d.target.c_str() : back.c_str(),
               d.interior ? "building" : "outdoors", d.distance, back.empty() ? "" : " (back to the region we came from; its own is '",
               back.empty() ? "" : d.target.c_str(), back.empty() ? "" : "')");
}

void cmd_goto_door(const game::Vec3& me, int index) {
    const auto doors = list_doors(me);
    const int pick = pick_door(doors, index, "goto_door");
    if (pick < 0) return;
    const Door& d = doors[static_cast<size_t>(pick)];
    // Stand off on our side of the door (doors face the way you come from, usually), facing it.
    float dx = me.x - d.pos.x, dz = me.z - d.pos.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.1f) {
        dx = 0;
        dz = 1;
    } else {
        dx /= len;
        dz /= len;
    }
    Vector3 pos{d.pos.x + dx * kStandOffMetres, d.pos.y + 0.5f, d.pos.z + dz * kStandOffMetres};
    const float yaw = std::atan2(-dx, -dz);  // Unity: 0 faces +Z, 90 degrees faces +X
    Quaternion rot{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
    if (!player) {
        logx::warn("dev: goto_door: no player");
        return;
    }
    void* args[] = {&pos, &rot};
    il2::invoke(api.teleport, player, args);
    logx::info("dev: moved to door [%d] -> '%s' (%s): now at (%.1f, %.1f, %.1f), %.0f m from where we were", pick,
               d.target.c_str(), d.interior ? "building" : "outdoors", pos.x, pos.y, pos.z, d.distance);
}

// --- wildlife ------------------------------------------------------------------------------------------------------

struct Animal {
    Il2CppObject* ai = nullptr;
    int32_t kind = 0, mode = 0;
    float hp = 0;
    Vector3 pos{};
    float distance = 0;
};

std::vector<Il2CppObject*> list_items(Il2CppObject* list) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* items = il2::read_field<Il2CppObject*>(list, kListItems);
    const int32_t count = il2::read_field<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* o = il2::array_object_at(items, static_cast<size_t>(i))) out.push_back(o);
    return out;
}

const char* kind_name(int32_t kind) {
    return kind >= 0 && static_cast<size_t>(kind) < std::size(kAnimalKinds) ? kAnimalKinds[kind] : "?";
}

int32_t kind_from(std::string name) {
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (size_t i = 1; i < std::size(kAnimalKinds); ++i)
        if (name == kAnimalKinds[i]) return static_cast<int32_t>(i);
    return -1;
}

std::vector<Animal> list_animals(const game::Vec3& me) {
    std::vector<Animal> out;
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(api.ai_list, &list);
    for (Il2CppObject* ai : list_items(list)) {
        Animal a;
        a.ai = ai;
        a.kind = il2::read_field<int32_t>(ai, api.ai_kind);
        a.mode = il2::read_field<int32_t>(ai, api.ai_mode);
        a.hp = il2::read_field<float>(ai, api.ai_hp);
        Il2CppObject* tf = il2::invoke(api.component_transform, ai);
        if (!tf || !il2::invoke_value(api.transform_position, tf, a.pos)) continue;
        const float dx = a.pos.x - me.x, dy = a.pos.y - me.y, dz = a.pos.z - me.z;
        a.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        out.push_back(a);
    }
    return out;
}

std::string experience_name() {
    int32_t mode = -1;
    il2::invoke_value(api.experience_mode, nullptr, mode);
    return il2::enum_name(api.experience_enum, mode);
}

void cmd_info(const game::PlayerState& me) {
    const auto animals = list_animals(me.position);
    int counts[std::size(kAnimalKinds)] = {};
    for (const Animal& a : animals)
        if (a.kind >= 0 && static_cast<size_t>(a.kind) < std::size(kAnimalKinds) && a.mode != kAiModeDead) ++counts[a.kind];
    std::string summary;
    for (size_t i = 1; i < std::size(kAnimalKinds); ++i)
        summary += std::string(summary.empty() ? "" : ", ") + std::to_string(counts[i]) + " " + kAnimalKinds[i];
    logx::info("dev: difficulty %s, scene '%s', us at (%.1f, %.1f, %.1f); living animals: %s",
               experience_name().c_str(), game::active_scene().c_str(), me.position.x, me.position.y,
               me.position.z, summary.c_str());
}

void cmd_animals(const game::Vec3& me) {
    const auto animals = list_animals(me);
    logx::info("dev: %zu animals here", animals.size());
    for (size_t i = 0; i < animals.size(); ++i) {
        const Animal& a = animals[i];
        logx::info("dev:   [%zu] %s, %s, hp %.0f, at (%.1f, %.1f, %.1f), %.0f m away", i, kind_name(a.kind),
                   il2::enum_name(api.ai_mode_enum, a.mode).c_str(), a.hp, a.pos.x, a.pos.y, a.pos.z, a.distance);
    }
}

// A point near `want` on the walkable mesh animals use, or `want` itself if there's none within 15 m.
Vector3 on_navmesh(Vector3 want) {
    NavMeshHit hit{};
    float radius = 15.0f;
    int32_t all_areas = -1;
    void* args[] = {&want, &hit, &radius, &all_areas};
    bool found = false;
    if (api.navmesh_sample && il2::invoke_value(api.navmesh_sample, nullptr, found, args) && found) return hit.position;
    return want;
}

Quaternion facing(const Vector3& from, const Vector3& to) {
    const float yaw = std::atan2(to.x - from.x, to.z - from.z);  // Unity: 0 faces +Z
    return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
}

void cmd_spawn(const game::PlayerState& me, int32_t kind, int count) {
    Il2CppObject* manager = il2::invoke(api.spawn_manager, nullptr);
    // The spawn area for this kind closest to us: far ones are dormant and switch their animals off again.
    Il2CppObject* region = nullptr;
    int regions_of_kind = 0;
    float region_distance = 0;
    Vector3 region_center{};
    for (Il2CppObject* r : list_items(il2::read_field<Il2CppObject*>(manager, api.spawn_regions))) {
        if (il2::read_field<int32_t>(r, api.region_kind) != kind) continue;
        ++regions_of_kind;
        const Vector3 c = il2::read_field<Vector3>(r, api.region_center);
        const float dx = c.x - me.position.x, dz = c.z - me.position.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (!region || d < region_distance) {
            region = r;
            region_distance = d;
            region_center = c;
        }
    }
    // Without a spawn area for this kind, copy a living animal of the kind instead.
    Il2CppObject* template_ai = nullptr;
    for (const Animal& a : list_animals(me.position))
        if (a.kind == kind && a.mode != kAiModeDead) template_ai = a.ai;
    if (!region && !template_ai) {
        logx::warn("dev: spawn: no %s spawn area or %s in this scene", kind_name(kind), kind_name(kind));
        return;
    }
    Il2CppObject* prefab = region ? il2::read_field<Il2CppObject*>(region, api.region_prefab) : nullptr;
    const auto refs = region ? list_items(il2::read_field<Il2CppObject*>(region, api.region_refs)) : std::vector<Il2CppObject*>{};
    Il2CppObject* prefab_ref = refs.empty() ? nullptr : refs[0];
    const float yaw = me.yaw * 3.14159265f / 180.0f;
    const Vector3 us{me.position.x, me.position.y, me.position.z};
    for (int i = 0; i < count; ++i) {
        const float ahead = kSpawnAheadMetres + 4.0f * static_cast<float>(i / 2);
        const float side = (i % 2 == 0 ? 1.0f : -1.0f) * 3.0f * static_cast<float>((i + 1) / 2);
        Vector3 want{us.x + std::sin(yaw) * ahead + std::cos(yaw) * side, us.y + 1.0f,
                     us.z + std::cos(yaw) * ahead - std::sin(yaw) * side};
        Vector3 pos = on_navmesh(want);
        Quaternion rot = facing(pos, us);
        Il2CppObject* spawned = nullptr;
        const char* how = "spawn area";
        if (region && (prefab || prefab_ref)) {
            int32_t mode = kAiModeWander, wildlife = 0;
            void* args[] = {prefab, prefab_ref, &pos, &rot, &mode, &wildlife};
            spawned = il2::invoke(api.instantiate_spawn, region, args);
        }
        // TLD registers a new animal a moment later, so a wolf back from the spawn area counts as made; it only has
        // to be switched on.
        if (spawned) {
            Il2CppObject* go = il2::invoke(api.component_go, spawned);
            bool on = true;
            if (go && il2::invoke_value(api.go_active, go, on) && !on) {
                bool yes = true;
                void* active_args[] = {&yes};
                il2::invoke(api.go_set_active, go, active_args);
            }
        }
        if (!spawned && template_ai) {
            how = "copy of a living one";
            void* args[] = {il2::invoke(api.component_go, template_ai), &pos, &rot};
            spawned = il2::invoke(api.clone_at, nullptr, args);
        }
        logx::info("dev: spawn %s #%d: %s at (%.1f, %.1f, %.1f), %.0f m ahead (%s; nearest of %d %s spawn areas, "
                   "%.0f m away at (%.1f, %.1f, %.1f))", kind_name(kind), i + 1, spawned ? "OK" : "FAILED", pos.x, pos.y,
                   pos.z, ahead, how, regions_of_kind, kind_name(kind), region_distance, region_center.x, region_center.y,
                   region_center.z);
    }
}

void cmd_goto_animal(const game::Vec3& me, int32_t kind) {
    const Animal* best = nullptr;
    const auto animals = list_animals(me);
    for (const Animal& a : animals)
        if ((kind < 0 || a.kind == kind) && a.mode != kAiModeDead && (!best || a.distance < best->distance)) best = &a;
    if (!best) {
        logx::warn("dev: goto_animal: no living %s here", kind < 0 ? "animal" : kind_name(kind));
        return;
    }
    float dx = me.x - best->pos.x, dz = me.z - best->pos.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.1f) dx = 0, dz = 1;
    else dx /= len, dz /= len;
    Vector3 pos = on_navmesh({best->pos.x + dx * kGotoAnimalMetres, best->pos.y + 1.0f, best->pos.z + dz * kGotoAnimalMetres});
    pos.y += 0.3f;
    Quaternion rot = facing(pos, best->pos);
    Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
    if (!player) return;
    void* args[] = {&pos, &rot};
    il2::invoke(api.teleport, player, args);
    logx::info("dev: moved %.0f m from a %s (%s), now at (%.1f, %.1f, %.1f)", kGotoAnimalMetres, kind_name(best->kind),
               il2::enum_name(api.ai_mode_enum, best->mode).c_str(), pos.x, pos.y, pos.z);
}

void cmd_goto(const game::PlayerState& me, float x, float y, float z) {
    Vector3 pos{x, y + 0.3f, z};
    const float yaw = me.yaw * 3.14159265f / 180.0f;
    Quaternion rot{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
    if (!player) return;
    void* args[] = {&pos, &rot};
    il2::invoke(api.teleport, player, args);
    logx::info("dev: moved to (%.1f, %.1f, %.1f)", x, y, z);
}

// Stand beside the other player (to their right, `metres` away), facing them: to watch their body up close.
void cmd_watch(float metres) {
    const auto them = netrt::remote_state();
    if (!them || !them->valid || them->scene != game::active_scene()) {
        logx::warn("dev: watch: the other player isn't in this scene");
        return;
    }
    const float yaw = them->yaw * 3.14159265f / 180.0f;
    const float rx = std::cos(yaw), rz = -std::sin(yaw);  // their right (Unity: yaw 0 faces +Z, 90 faces +X)
    // On the ground there, not at their height: on a slope that would put us inside the hill, and we'd fall through
    // the world.
    Vector3 pos = on_navmesh({them->x + rx * metres, them->y, them->z + rz * metres});
    pos.y += 0.3f;
    Quaternion rot = facing(pos, {them->x, them->y, them->z});
    Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
    if (!player) return;
    void* args[] = {&pos, &rot};
    il2::invoke(api.teleport, player, args);
    logx::info("dev: watching the other player from %.1f m: now at (%.1f, %.1f, %.1f)", metres, pos.x, pos.y, pos.z);
}

// The living animal (of a kind, -1 any) nearest the other player's avatar (the first one shown here).
Il2CppObject* animal_near_friend(int32_t kind, float& metres) {
    Il2CppObject *go = nullptr, *transform = nullptr, *target = nullptr;
    Vector3 at{};
    const std::vector<uint8_t> shown = avatar::shown();
    if (shown.empty() || !avatar::stand_in(shown.front(), go, transform, target) ||
        !il2::invoke_value(api.transform_position, transform, at))
        return nullptr;
    const game::Vec3 friend_pos{at.x, at.y, at.z};
    Il2CppObject* best = nullptr;
    for (const Animal& a : list_animals(friend_pos)) {
        if (a.mode == kAiModeDead || (kind >= 0 && a.kind != kind)) continue;
        if (!best || a.distance < metres) best = a.ai, metres = a.distance;
    }
    return best;
}

void cmd_at_friend(const std::string& verb, int32_t kind) {
    float metres = 0;
    Il2CppObject* ai = animal_near_friend(kind, metres);
    if (!ai) {
        logx::warn("dev: %s: no living %s here, or the other player isn't shown", verb.c_str(),
                   kind < 0 ? "animal" : kind_name(kind));
        return;
    }
    const bool ok = verb == "fight_friend" ? wildlife_targets::test_fight(ai) : wildlife_targets::test_attack(ai);
    logx::info("dev: %s: the %s %.0f m from the other player %s", verb.c_str(),
               kind_name(il2::read_field<int32_t>(ai, api.ai_kind)), metres, ok ? "is on them" : "can't (host only)");
}

Il2CppObject* hands_part(Il2CppObject* gear, ptrdiff_t field) {
    return gear && field >= 0 ? il2::read_field<Il2CppObject*>(gear, field) : nullptr;
}

void do_hands(const std::string& action) {
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    Il2CppObject* gear = pm ? il2::invoke(fight.in_hands, pm) : nullptr;
    const char* done = nullptr;
    if (action == "away") wildlife_hits::hold_aim(false);  // the aim button `hands aim` holds goes too
    if (action == "away" && gear) {
        bool save_last = false;
        void* args[] = {&save_last};
        il2::invoke(held_api.put_away, pm, args);
        gear = il2::invoke(fight.in_hands, pm);
        done = "put away (PlayerManager.UnequipImmediate)";
    } else if (!gear && !action.empty()) {
        logx::warn("dev: hands %s: nothing in our hands (equip GEAR first)", action.c_str());
        return;
    }
    Il2CppObject *torch = hands_part(gear, held_api.torch), *flare = hands_part(gear, held_api.flare),
                 *lamp = hands_part(gear, held_api.lamp), *flashlight = hands_part(gear, held_api.flashlight);
    auto zoom = [&](bool start) -> const char* {
        struct Way {
            Il2CppObject* part;
            const MethodInfo *aim, *lower;
        };
        for (const Way& w : {Way{hands_part(gear, held_api.gun), held_api.gun_aim, held_api.gun_lower},
                             Way{hands_part(gear, held_api.bow), held_api.bow_aim, held_api.bow_lower},
                             Way{hands_part(gear, held_api.stone), held_api.stone_aim, held_api.stone_lower},
                             Way{hands_part(gear, held_api.spear), held_api.spear_aim, held_api.spear_lower}})
            if (w.part) {
                il2::invoke(start ? w.aim : w.lower, w.part);
                return start ? "raised to aim (ZoomStart)" : "lowered (ZoomEnd)";
            }
        return nullptr;
    };
    if (action == "light") {
        if (torch) il2::invoke(held_api.torch_ignite, torch), done = "torch lit (TorchItem.Ignite)";
        else if (flare) il2::invoke(held_api.flare_ignite, flare), done = "flare lit (FlareItem.Ignite)";
        else if (lamp) {  // filled up first: a new lantern is empty
            int64_t room = 0;  // TLD.IntBackedUnit.ItemLiquidVolume
            if (il2::invoke_value(held_api.lamp_room, lamp, room) && room > 0) {
                void* fill_args[] = {&room};
                il2::invoke(held_api.lamp_fill, lamp, fill_args);
            }
            bool quiet = false;
            void* on_args[] = {&quiet};
            il2::invoke(held_api.lamp_turn_on, lamp, on_args);
            done = "lantern filled and on (KeroseneLampItem.AddFuel, TurnOn)";
        } else if (flashlight) {  // charged first: it charges only in an aurora
            if (held_api.flashlight_charge >= 0) {
                const float full = 1.0f;
                memcpy(reinterpret_cast<char*>(flashlight) + held_api.flashlight_charge, &full, sizeof(full));
            }
            il2::invoke(held_api.flashlight_on, flashlight);
            done = "flashlight charged and on (m_CurrentBatteryCharge 1, TurnOn)";
            const int32_t state = held_api.flashlight_state >= 0
                                      ? il2::read_field<int32_t>(flashlight, held_api.flashlight_state) : 0;
            if (state == 0 && held_api.flashlight_state >= 0) {  // TurnOn left it off (charged or not): low beam set
                int32_t low = 1;                                   // FlashlightItem.State.Low
                memcpy(reinterpret_cast<char*>(flashlight) + held_api.flashlight_state, &low, sizeof(low));
                void* args[] = {&low};
                il2::invoke(held_api.flashlight_lights, flashlight, args);
                done = "flashlight on its low beam (TurnOn refused; m_State Low, EnableLights)";
            }
        }
    } else if (action == "out") {
        if (torch) il2::invoke(held_api.torch_out, torch), done = "torch put out (ForceExtinguish)";
        else if (lamp) {
            bool audio = true;
            void* off_args[] = {&audio};
            il2::invoke(held_api.lamp_turn_off, lamp, off_args);
            done = "lantern off (KeroseneLampItem.TurnOff)";
        } else if (flashlight) il2::invoke(held_api.flashlight_off, flashlight), done = "flashlight off";
    } else if (action == "high") {
        if (flashlight && held_api.flashlight_state >= 0) {
            int32_t high = 2;  // FlashlightItem.State.High
            memcpy(reinterpret_cast<char*>(flashlight) + held_api.flashlight_state, &high, sizeof(high));
            void* args[] = {&high};
            il2::invoke(held_api.flashlight_lights, flashlight, args);
            done = "flashlight on its high beam (m_State High, EnableLights)";
        }
    } else if (action == "aim" || action == "lower") {  // the aim button held (or let go) as a player does
        const bool aim = action == "aim";
        done = wildlife_hits::hold_aim(aim) ? (aim ? "aim button held (InputManager.GetAltFire)" : "aim button let go")
                                            : zoom(aim);
    }
    if (!action.empty() && !done) {
        logx::warn("dev: hands %s: not something the item in our hands does", action.c_str());
        return;
    }
    const game::HeldState held = game::read_held();
    Il2CppObject* go = gear ? il2::invoke(api.component_go, gear) : nullptr;
    logx::info("dev: hands%s%s: %s; %s", done ? ": " : "", done ? done : "",
               net::describe_held(held.gear, net::held_kind(held.kind), held.flags).c_str(),
               avatar::describe_lights(go).c_str());
}

void run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty()) return;
    logx::info("dev: command '%s'", line.c_str());
    if (cmd == "menu") {  // the co-op screens (main menu)
        std::string action, arg;
        in >> action;
        std::getline(in >> std::ws, arg);
        coop_menu::dev(action, arg);
        return;
    }
    if (cmd == "reload") {  // the test save again, without restarting the game (test copies)
        if (!test_harness::reload()) logx::warn("dev: reload: not a test copy (no [test] resume=), or TLD's quit is missing");
        return;
    }
    if (cmd == "answer") {  // F9 / F10 to a friend asking to join while we play
        std::string what;
        in >> what;
        if (!coop_notices::answer(what != "away")) logx::warn("dev: answer: nobody is asking to join (or we don't host)");
        return;
    }
    const game::PlayerState me = game::read_player();
    if (!me.valid) {
        logx::warn("dev: not in game; command ignored");
        return;
    }
    if (cmd == "doors") {
        cmd_doors(me.position);
    } else if (cmd == "goto_door") {
        int index = -1;
        in >> index;
        cmd_goto_door(me.position, index);
    } else if (cmd == "enter") {
        int index = -1;
        in >> index;
        cmd_enter(me.position, index);
    } else if (cmd == "goto") {
        float x = 0, y = 0, z = 0;
        if (!(in >> x >> y >> z)) {
            logx::warn("dev: goto needs X Y Z");
            return;
        }
        cmd_goto(me, x, y, z);
    } else if (cmd == "time") {
        // TimeOfDay.SetNormalizedTime(t, forceUpdate): HH:MM, 24-hour.
        int h = 0, m = 0;
        char colon = 0;
        if (!(in >> h >> colon >> m) || colon != ':') {
            logx::warn("dev: time needs HH:MM");
            return;
        }
        Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
        Il2CppObject* tod = il2::invoke(il2::find_method(game_manager, "GetTimeOfDayComponent", 0), nullptr);
        const MethodInfo* set = il2::find_method_params(il2::find_class(kGame, "", "TimeOfDay"), "SetNormalizedTime",
                                                        {"System.Single", "System.Boolean"});
        float t = static_cast<float>(((h * 60 + m) % 1440 + 1440) % 1440) / 1440.0f;
        bool force = true;
        void* args[] = {&t, &force};
        if (tod && set) il2::invoke(set, tod, args);
        logx::info("dev: time set to %02d:%02d", h, m);
    } else if (cmd == "console") {
        // TLD's own debug console commands (ConsoleManager.CONSOLE_<name>): god, invulnerable, ghost, fly...
        std::string name;
        in >> name;
        Il2CppClass* console = il2::find_class(kGame, "", "ConsoleManager");
        const MethodInfo* m = console ? il2::find_method(console, ("CONSOLE_" + name).c_str(), 0) : nullptr;
        if (!m) {
            logx::warn("dev: no TLD console command '%s'", name.c_str());
            return;
        }
        il2::invoke(m, nullptr);
        logx::info("dev: ran TLD's console command '%s'", name.c_str());
    } else if (cmd == "sounds") {
        std::istringstream lines(wildlife_sync::describe_sounds());
        for (std::string l; std::getline(lines, l);) logx::info("dev: %s", l.c_str());
    } else if (cmd == "info") {
        cmd_info(me);
    } else if (cmd == "animals") {
        cmd_animals(me.position);
    } else if (cmd == "spawn") {
        std::string kind_text;
        int count = 1;
        in >> kind_text >> count;
        const int32_t kind = kind_from(kind_text);
        if (kind < 0) {
            logx::warn("dev: spawn: unknown kind '%s' (wolf, bear, stag, rabbit, moose, cougar)", kind_text.c_str());
            return;
        }
        cmd_spawn(me, kind, std::clamp(count, 1, 8));
    } else if (cmd == "goto_animal") {
        std::string kind_text;
        in >> kind_text;
        cmd_goto_animal(me.position, kind_text.empty() ? -1 : kind_from(kind_text));
    } else if (cmd == "run") {
        // The rest of the line, as typed into TLD's own console (F1 in a dev build).
        std::string rest;
        std::getline(in, rest);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        if (!fight.run_console || rest.empty()) {
            logx::warn("dev: run needs a console line (and TLD's console)");
            return;
        }
        void* args[] = {il2::il2cpp_string_new(rest.c_str())};
        Il2CppObject* result = il2::invoke(fight.run_console, nullptr, args);
        logx::info("dev: ran TLD console line '%s'%s", rest.c_str(), result ? " (it answered)" : "");
    } else if (cmd == "taps") {
        float rate = 0;
        in >> rate;
        g_taps_per_second = std::clamp(rate, 0.0f, 30.0f);
        g_taps_done = 0;
        logx::info("dev: struggle taps: %.0f a second%s", g_taps_per_second, g_taps_per_second > 0 ? " while in a fight" : " (off)");
    } else if (cmd == "equip") {
        // equip GEAR_NAME [ROUNDS]: into the pack, then into our hands (a gun gets ROUNDS in its clip, default 5)
        std::string name;
        int rounds = 5;
        in >> name >> rounds;
        Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
        if (name.empty() || !pm || !fight.add_item) {
            logx::warn("dev: equip needs a GEAR_ name (e.g. GEAR_Rifle, GEAR_Revolver, GEAR_Bow, GEAR_FlareGun)");
            return;
        }
        if (Il2CppObject* held = il2::invoke(fight.in_hands, pm);
            held && il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(fight.object_name, held))) == name) {
            Il2CppObject* gun = il2::read_field<Il2CppObject*>(held, fight.gun_item);
            if (gun && rounds > 0) {  // already in our hands: just load it
                int32_t n = rounds, cond = 100;
                void* round_args[] = {&n, &cond};
                il2::invoke(fight.add_rounds, gun, round_args);
            }
            logx::info("dev: equip %s: already in our hands%s", name.c_str(), gun && rounds > 0 ? ", loaded" : "");
            return;
        }
        int32_t count = 1;
        float condition = 1.0f;
        void* add_args[] = {il2::il2cpp_string_new(name.c_str()), &count, &condition};
        Il2CppObject* gear = il2::invoke(fight.add_item, pm, add_args);
        bool suppress = true, used = false;
        void* use_args[] = {gear, &suppress};
        if (gear) il2::invoke_value(fight.use_item, pm, used, use_args);
        bool equipped = false;
        if (gear && !used && fight.equip_item) {  // UseInventoryItem said no: the lower-level equip
            bool from_save = false;
            void* equip_args[] = {gear, &from_save};
            il2::invoke(fight.equip_item, pm, equip_args);
            equipped = true;
        }
        Il2CppObject* gun = gear ? il2::read_field<Il2CppObject*>(gear, fight.gun_item) : nullptr;
        if (gun && rounds > 0) {
            int32_t n = rounds, cond = 100;
            void* round_args[] = {&n, &cond};
            il2::invoke(fight.add_rounds, gun, round_args);
        }
        Il2CppObject* hands = il2::invoke(fight.in_hands, pm);
        logx::info("dev: equip %s: %s, %s; in our hands: %s", name.c_str(), gear ? "added" : "NOT added (unknown name?)",
                   used ? "used" : equipped ? "UseInventoryItem refused, EquipItem called" : "not used",
                   hands ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(fight.object_name, hands))).c_str()
                         : "nothing");
    } else if (cmd == "give") {
        // give GEAR_NAME [N]: into the pack only (arrows, flare shells, stones...)
        std::string name;
        int n = 1;
        in >> name >> n;
        Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
        int32_t count = std::clamp(n, 1, 99);
        float condition = 1.0f;
        void* add_args[] = {il2::il2cpp_string_new(name.c_str()), &count, &condition};
        Il2CppObject* gear = pm && fight.add_item && !name.empty() ? il2::invoke(fight.add_item, pm, add_args) : nullptr;
        logx::info("dev: give %s x%d: %s", name.c_str(), count, gear ? "in the pack" : "NOT added (unknown name?)");
    } else if (cmd == "hands") {
        // hands [light|out|aim|lower|high]: what's in our hands (as State sends it), or that done to it
        std::string action;
        if (in >> action) {
            do_hands(action);
            return;
        }
        Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
        Il2CppObject* hands = pm ? il2::invoke(fight.in_hands, pm) : nullptr;
        bool ready = false, switching = false;
        if (pm) il2::invoke_value(fight.ready_to_equip, pm, ready);
        Il2CppObject* camera = il2::invoke(fight.fps_camera, nullptr);
        if (camera) il2::invoke_value(fight.weapon_switching, camera, switching);
        bool inactive_too = true;
        void* all_args[] = {fight.shooter_type, &inactive_too};
        void* active_args[] = {fight.shooter_type};
        const size_t all = il2::array_length(il2::invoke(fight.find_objects_inactive, nullptr, all_args));
        const size_t active = il2::array_length(il2::invoke(fight.find_objects, nullptr, active_args));
        logx::info("dev: hands: %s; ready to equip %d, weapon switch in progress %d; vp_FPSShooter %zu active, %zu in all",
                   hands ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(fight.object_name, hands))).c_str()
                         : "nothing",
                   ready ? 1 : 0, switching ? 1 : 0, active, all);
        do_hands("");
    } else if (cmd == "die") {
        // Condition.KillPlayer(cause, null): TLD's own death (god mode doesn't stop it), the way a wolf or the cold
        // ends it. A test copy doesn't delete its save for it (test_harness).
        int32_t cause = 0;  // DamageSource; 0: TLD names the cause itself
        in >> cause;
        Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
        Il2CppObject* condition = il2::invoke(il2::find_method(game_manager, "GetConditionComponent", 0), nullptr);
        const MethodInfo* kill = il2::find_method(il2::find_class(kGame, "", "Condition"), "KillPlayer", 2);
        if (!condition || !kill) {
            logx::warn("dev: die: not in a game");
            return;
        }
        logx::info("dev: die: the player dies (cause %d)", cause);
        void* args[] = {&cause, nullptr};
        il2::invoke(kill, condition, args);
    } else if (cmd == "call") {
        // A method without arguments on the first active object of one of TLD's classes, as a button would call it
        // (e.g. `call Panel_Log OnBack`).
        std::string class_name, method_name;
        in >> class_name >> method_name;
        Il2CppClass* klass = il2::find_class(kGame, "", class_name.c_str());
        const MethodInfo* method = klass ? il2::find_method(klass, method_name.c_str(), 0) : nullptr;
        bool inactive_too = false;
        void* find_args[] = {klass ? il2::type_object(klass) : nullptr, &inactive_too};
        Il2CppObject* found = method ? il2::invoke(fight.find_objects_inactive, nullptr, find_args) : nullptr;
        Il2CppObject* target = il2::array_length(found) ? il2::array_object_at(found, 0) : nullptr;
        if (!target) {
            logx::warn("dev: call: %s", !klass ? "no such class" : !method ? "no such method (without arguments)"
                                                                          : "none of those is active");
            return;
        }
        il2::invoke(method, target);
        logx::info("dev: call: %s.%s called", class_name.c_str(), method_name.c_str());
    } else if (cmd == "crouch") {
        // crouch on|off: as TLD's crouch key does in toggle mode (vp_FPSPlayer.EnableCrouch, m_CrouchToggleOn).
        std::string onoff;
        in >> onoff;
        Il2CppClass* klass = il2::find_class(kGame, "", "vp_FPSPlayer");
        const MethodInfo* enable = klass ? il2::find_method(klass, "EnableCrouch", 1) : nullptr;
        const ptrdiff_t toggle = klass ? il2::field_offset(klass, "m_CrouchToggleOn") : -1;
        bool inactive_too = false;
        void* find_args[] = {klass ? il2::type_object(klass) : nullptr, &inactive_too};
        Il2CppObject* found = enable ? il2::invoke(fight.find_objects_inactive, nullptr, find_args) : nullptr;
        Il2CppObject* player = il2::array_length(found) ? il2::array_object_at(found, 0) : nullptr;
        if (!player || (onoff != "on" && onoff != "off")) {
            logx::warn("dev: crouch on|off: %s", player ? "on or off?" : "no vp_FPSPlayer (not in a game?)");
            return;
        }
        bool on = onoff == "on", done = false;
        if (toggle >= 0) memcpy(reinterpret_cast<char*>(player) + toggle, &on, sizeof(on));
        void* args[] = {&on};
        il2::invoke_value(enable, player, done, args);
        logx::info("dev: crouch %s: %s", onoff.c_str(), done ? "done" : "TLD said no");
    } else if (cmd == "respawn") {
        std::string body;
        in >> body;
        logx::info("dev: respawn: %s", coop_death::respawn(body) ? "done" : "not done");
    } else if (cmd == "survivor") {
        int step = 1;
        in >> step;
        if (!coop_death::next_survivor(step)) logx::warn("dev: survivor: we aren't dead");
    } else if (cmd == "needs") {
        std::string charge;
        in >> charge;
        if (charge == "on" || charge == "off") test_harness::charge_needs(charge == "on");
        coop_lapse::describe();
    } else if (cmd == "lapse") {
        float seconds = 10, minutes = 60;
        in >> seconds >> minutes;
        if (!coop_lapse::launch(seconds, minutes)) logx::warn("dev: lapse: not in a game");
    } else if (cmd == "sleep") {
        std::string action, arg;
        in >> action >> arg;
        if (action.empty() || action == "status")
            coop_sleep::describe();
        else if (!coop_sleep::dev(action, arg))
            logx::warn("dev: sleep %s: not done (no bed near, no sleep screen up, no such choice, or not in a game)", action.c_str());
    } else if (cmd == "hud") {
        std::string action, arg;
        in >> action;
        std::getline(in >> std::ws, arg);
        if (action.empty() || action == "status")
            logx::info("dev: hud: %s", coop_hud::describe().c_str());
        else if (!coop_hud::dev(action, arg))
            logx::warn("dev: hud %s: not done (names|radar|chart on|off, chart now, map open|close|centre, tld "
                       "hide|show, radar whole|noclip|normal|rim|paper)", action.c_str());
    } else if (cmd == "bodies") {
        coop_death::describe();
    } else if (cmd == "held") {
        logx::info("dev: held: %s", avatar::describe_held().c_str());
    } else if (cmd == "hold") {
        std::string rest;
        std::getline(in, rest);
        logx::info("dev: %s", avatar::tune_hold(rest).c_str());
    } else if (cmd == "pose") {
        std::string rest;
        std::getline(in, rest);
        logx::info("dev: %s", avatar::tune_pose(rest).c_str());
    } else if (cmd == "handover") {
        std::string action, rest;
        in >> action;
        std::getline(in, rest);
        if (action.empty() || action == "status")
            coop_give::describe();
        else if (!coop_give::dev(action, rest))
            logx::warn("dev: handover %s: not done (look [ID] | face YAW [PITCH] | press | open ID | drop GEAR | units N "
                       "| close | offer ID GEAR [UNITS] | accept | back; or nobody there, nothing open, not in a game)", action.c_str());
    } else if (cmd == "lives") {
        int lives = 0;
        in >> lives;
        if (!coop_death::set_lives(lives)) logx::warn("dev: lives: not in a game");
    } else if (cmd == "afterlife") {
        // TLD's screen after a death (Panel_LifeAfterDeath): its stage, or one of its buttons pressed as a click does
        // (left: CHEAT DEATH / NEXT / the short penalty / the close respawn; right: END GAME / the permanent penalty /
        // the far respawn).
        std::string press;
        in >> press;
        if (press == "confirm") {  // its "Are you sure about your choices?" (Panel_Confirmation): CONFIRM
            Il2CppClass* confirm_class = il2::find_class(kGame, "", "Panel_Confirmation");
            bool inactive = false;
            void* args[] = {il2::type_object(confirm_class), &inactive};
            Il2CppObject* found = confirm_class ? il2::invoke(fight.find_objects_inactive, nullptr, args) : nullptr;
            Il2CppObject* confirm = il2::array_length(found) ? il2::array_object_at(found, 0) : nullptr;
            if (confirm) il2::invoke(il2::find_method(confirm_class, "OnConfirm", 0), confirm);
            logx::info("dev: afterlife: %s", confirm ? "CONFIRM pressed" : "no confirmation up");
            return;
        }
        Il2CppClass* panel_class = il2::find_class(kGame, "", "Panel_LifeAfterDeath");
        bool inactive_too = false;
        void* find_args[] = {il2::type_object(panel_class), &inactive_too};
        Il2CppObject* panels = panel_class ? il2::invoke(fight.find_objects_inactive, nullptr, find_args) : nullptr;
        Il2CppObject* panel = il2::array_length(panels) ? il2::array_object_at(panels, 0) : nullptr;
        if (!panel) {
            logx::warn("dev: afterlife: TLD's screen after a death isn't up");
            return;
        }
        const char* handler = press == "left"    ? "HandleOnLeftButtonPressed"
                              : press == "right" ? "HandleOnRightButtonPressed"
                              : press == "back"  ? "HandleOnBackButtonPressed"
                                                 : nullptr;
        if (handler) il2::invoke(il2::find_method(panel_class, handler, 0), panel);
        logx::info("dev: afterlife: %s%sstage %d (pending %d)", handler ? press.c_str() : "", handler ? " pressed; " : "",
                   il2::read_field<int32_t>(panel, il2::field_offset(panel_class, "m_CurrentStage")),
                   il2::read_field<int32_t>(panel, il2::field_offset(panel_class, "m_PendingStage")));
    } else if (cmd == "freeze") {
        // Host: the living animal nearest the other player stands still for S seconds (its AI paused), for tests.
        float seconds = 8, metres = 0;
        uint32_t id = 0;
        in >> seconds >> id;
        Il2CppObject* ai = id ? wildlife_sync::host_animal(id) : animal_near_friend(-1, metres);
        if (!ai || !fight.set_enabled) {
            logx::warn("dev: freeze: nothing near the other player");
            return;
        }
        bool off = false;
        void* args[] = {&off};
        il2::invoke(fight.set_enabled, ai, args);
        if (Il2CppObject* animator = il2::read_field<Il2CppObject*>(ai, fight.ai_animator)) {  // root motion too
            float still = 0;
            void* speed_args[] = {&still};
            il2::invoke(fight.animator_speed, animator, speed_args);
        }
        if (g_frozen) il2::il2cpp_gchandle_free(g_frozen);
        g_frozen = il2::il2cpp_gchandle_new(ai, false);
        g_frozen_until = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(seconds, 1.0f, 60.0f) * 1000);
        logx::info("dev: freeze: the %s%s stands still for %.0f s", kind_name(il2::read_field<int32_t>(ai, api.ai_kind)),
                   id ? (" #" + std::to_string(id)).c_str() : " nearest the other player", seconds);
    } else if (cmd == "kill") {
        // Host: the living animal (of KIND, default wolf) nearest the other player dies (a shot from us: ApplyDamage).
        std::string kind_text;
        in >> kind_text;
        float metres = 0;
        Il2CppObject* ai = animal_near_friend(kind_text.empty() ? 1 : kind_from(kind_text), metres);
        if (!ai || !fight.apply_damage) {
            logx::warn("dev: kill: nothing to kill near the other player");
            return;
        }
        float damage = 100000, bleed = 0;
        int32_t source = 1;  // DamageSource.Player
        void* args[] = {&damage, &bleed, &source, il2::il2cpp_string_new("capsule_chest")};
        il2::invoke(il2::virtual_method(ai, fight.apply_damage), ai, args);
        logx::info("dev: kill: the %s %.0f m from the other player is %s", kind_name(il2::read_field<int32_t>(ai, api.ai_kind)),
                   metres, il2::read_field<int32_t>(ai, api.ai_mode) == kAiModeDead ? "dead" : "STILL ALIVE");
    } else if (cmd == "harvest") {
        float kg = 1;
        int hide = 0, gut = 0;
        in >> kg >> hide >> gut;
        wildlife_carcass::harvest(kg, hide, gut);
    } else if (cmd == "quarter") {
        wildlife_carcass::quarter();
    } else if (cmd == "carcasses") {
        wildlife_carcass::describe();
    } else if (cmd == "items") {
        float metres = 10;
        in >> metres;
        world_items::describe(std::clamp(metres, 1.0f, 2000.0f));
    } else if (cmd == "pack") {
        world_items::describe_pack();
    } else if (cmd == "pickup") {
        std::string name;
        in >> name;
        world_items::pickup(name);
    } else if (cmd == "drop") {
        std::string name;
        int units = 1;
        in >> name >> units;
        world_items::drop(name, units);
    } else if (cmd == "putback") {
        world_items::put_back();
    } else if (cmd == "takeit") {
        world_items::take_it();
    } else if (cmd == "inspect") {
        std::string name;
        in >> name;
        world_items::inspect(name);
    } else if (cmd == "coin") {
        std::string how;
        in >> how;
        world_items::coin(how);
    } else if (cmd == "snare") {
        std::string what;
        in >> what;
        world_items::snare(what);
    } else if (cmd == "record") {
        std::string name;
        in >> name;
        world_items::record(name);
    } else if (cmd == "cooking") {
        std::string what, arg;
        in >> what >> arg;
        world_items::cooking(what, arg);
    } else if (cmd == "containers") {
        float metres = 10;
        in >> metres;
        world_containers::describe(std::clamp(metres, 1.0f, 2000.0f));
    } else if (cmd == "open" || cmd == "search") {
        std::string name;
        in >> name;
        world_containers::open(name, cmd == "search");
    } else if (cmd == "close") {
        world_containers::close();
    } else if (cmd == "take" || cmd == "put") {
        std::string name;
        in >> name;
        if (cmd == "take")
            world_containers::take(name);
        else
            world_containers::put(name);
    } else if (cmd == "objects") {
        float metres = 15;
        std::string most;
        in >> metres >> most;
        world_objects::describe(std::clamp(metres, 1.0f, 20000.0f),
                                most.empty() ? 40 : static_cast<size_t>(std::clamp(std::atoi(most.c_str()), 1, 5000)));
    } else if (cmd == "placements") {
        world_objects::describe_placements();
    } else if (cmd == "spawners") {
        float metres = 50;
        in >> metres;
        world_objects::describe_spawners(std::clamp(metres, 1.0f, 2000.0f));
    } else if (cmd == "breakdown" || cmd == "plant") {
        std::string name;
        in >> name;
        if (cmd == "breakdown")
            world_objects::break_down(name);
        else
            world_objects::harvest(name);
    } else if (cmd == "force" || cmd == "crack") {
        std::string name;
        in >> name;
        if (cmd == "force")
            world_objects::force(name);
        else
            world_objects::crack(name);
    } else if (cmd == "go") {
        std::string name;
        size_t most = 20;
        in >> name >> most;
        world_objects::describe_game_objects(name, std::clamp<size_t>(most, 1, 500));
    } else if (cmd == "decor") {
        std::string what, name;
        in >> what >> name;
        world_objects::decorations(what, name);
    } else if (cmd == "fires") {
        float metres = 15;
        in >> metres;
        world_fires::describe(std::clamp(metres, 1.0f, 2000.0f));
    } else if (cmd == "light") {
        std::string name;
        in >> name;
        world_fires::light(name);
    } else if (cmd == "feed") {
        std::string name;
        in >> name;
        world_fires::feed(name);
    } else if (cmd == "cook") {
        std::string name;
        in >> name;
        world_fires::cook(name);
    } else if (cmd == "campfire") {
        world_fires::campfire();
    } else if (cmd == "burnout") {
        float seconds = 30;
        in >> seconds;
        world_fires::burn_out(seconds);
    } else if (cmd == "dismantle") {
        std::string name;
        in >> name;
        world_fires::dismantle(name);
    } else if (cmd == "join") {
        world_join::go_to_host();
    } else if (cmd == "mode") {
        world_mode::describe();
    } else if (cmd == "cutlink") {
        float seconds = 8;
        in >> seconds;
        netrt::cut_link(seconds * 1000.0);
    } else if (cmd == "save") {
        static const MethodInfo* force_save =
            il2::find_method(il2::find_class(kGame, "", "GameManager"), "ForceSaveGame", 0);
        if (force_save) il2::invoke(force_save, nullptr);
        logx::info("dev: save: %s", force_save ? "asked TLD to save now (GameManager.ForceSaveGame)" : "UNAVAILABLE");
    } else if (cmd == "approach") {
        float metres = 15;
        in >> metres;
        wildlife_hits::approach(std::clamp(metres, 3.0f, 80.0f));
    } else if (cmd == "shoot") {
        std::string collider;
        in >> collider;
        wildlife_hits::shoot(collider);
    } else if (cmd == "fight_friend" || cmd == "attack_friend") {
        std::string kind_text;
        in >> kind_text;
        cmd_at_friend(cmd, kind_text.empty() ? 1 : kind_from(kind_text));
    } else if (cmd == "attack_host") {
        std::string kind_text;
        in >> kind_text;
        const int32_t kind = kind_text.empty() ? 1 : kind_from(kind_text);
        const game::PlayerState me = game::read_player();
        Il2CppObject* ai = nullptr;
        float metres = 0;
        for (const Animal& a : list_animals(me.position)) {
            if (a.mode == kAiModeDead || (kind >= 0 && a.kind != kind)) continue;
            if (!ai || a.distance < metres) ai = a.ai, metres = a.distance;
        }
        if (!ai) logx::warn("dev: attack_host: no living %s here", kind < 0 ? "animal" : kind_name(kind));
        else if (!wildlife_targets::test_attack_host(ai)) logx::warn("dev: attack_host: host only");
        else logx::info("dev: attack_host: the %s %.0f m from us is on us",
                        kind_name(il2::read_field<int32_t>(ai, api.ai_kind)), metres);
    } else if (cmd == "look") {
        float x = 0, y = 0, z = 0;
        if (in >> x >> y >> z) wildlife_hits::look(x, y, z);
        else logx::warn("dev: look needs X Y Z");
    } else if (cmd == "near") {
        // near X Y Z [M]: stand M m (default 4) from that point, on the ground, facing it
        Vector3 at{};
        float metres = 4;
        if (!(in >> at.x >> at.y >> at.z)) {
            logx::warn("dev: near needs X Y Z [M]");
            return;
        }
        in >> metres;
        const game::PlayerState p = game::read_player();
        float dx = p.position.x - at.x, dz = p.position.z - at.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.1f) dx = 1, dz = 0;
        else dx /= len, dz /= len;
        Vector3 pos = on_navmesh({at.x + dx * metres, at.y, at.z + dz * metres});
        pos.y += 0.3f;
        Quaternion rot = facing(pos, at);
        Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
        void* args[] = {&pos, &rot};
        if (player) il2::invoke(api.teleport, player, args);
        logx::info("dev: near: %.1f m from (%.1f, %.1f, %.1f), now at (%.1f, %.1f, %.1f)", metres, at.x, at.y, at.z, pos.x,
                   pos.y, pos.z);
    } else if (cmd == "watch") {
        float metres = 4;
        in >> metres;
        cmd_watch(std::clamp(metres, 1.5f, 30.0f));
    } else {
        logx::warn("dev: unknown command '%s' (doors, goto_door [N], info, animals, spawn KIND [N], goto_animal [KIND], "
                   "watch [M], run LINE, taps N, fight_friend [KIND], attack_friend [KIND], equip GEAR [N], approach [M], "
                   "shoot [COLLIDER], kill [KIND], harvest KG [HIDE] [GUT], quarter, carcasses)",
                   cmd.c_str());
    }
}

}  // namespace

bool init() {
    Il2CppClass* load_scene = il2::find_class(kGame, "", "LoadScene");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    if (load_scene) api.door_list = il2::il2cpp_class_get_field_from_name(load_scene, "m_LoadScenesList");
    api.door_target = il2::field_offset(load_scene, "m_SceneToLoad");
    api.door_scene = il2::find_method(load_scene, "GetSceneToLoad", 0);
    api.door_instanced = il2::field_offset(load_scene, "m_SceneCanBeInstanced");
    if (game_manager) api.transition = il2::il2cpp_class_get_field_from_name(game_manager, "m_SceneTransitionData");
    Il2CppClass* transition = il2::find_class(kGame, "", "SceneTransitionData");
    api.force_next = il2::field_offset(transition, "m_ForceNextSceneLoadTriggerScene");
    api.last_outdoor = il2::field_offset(transition, "m_LastOutdoorScene");
    api.door_activate = il2::find_method_params(load_scene, "Activate", {"System.Boolean"});
    api.is_outdoors = il2::find_method(game_manager, "IsOutDoorsScene", 1);
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.teleport = il2::find_method_params(player_manager, "TeleportPlayer", {"UnityEngine.Vector3", "UnityEngine.Quaternion"});
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.transform_position = il2::find_method(transform, "get_position", 0);

    Il2CppClass* ai_manager = il2::find_class(kGame, "", "BaseAiManager");
    Il2CppClass* base_ai = il2::find_class(kGame, "", "BaseAi");
    Il2CppClass* experience = il2::find_class(kGame, "", "ExperienceModeManager");
    Il2CppClass* spawn_manager = il2::find_class(kGame, "", "SpawnRegionManager");
    Il2CppClass* spawn_region = il2::find_class(kGame, "", "SpawnRegion");
    Il2CppClass* navmesh = il2::find_class("UnityEngine.AIModule.dll", "UnityEngine.AI", "NavMesh");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    if (ai_manager) api.ai_list = il2::il2cpp_class_get_field_from_name(ai_manager, "m_BaseAis");
    api.ai_kind = il2::field_offset(base_ai, "m_AiSubType");
    api.ai_mode = il2::field_offset(base_ai, "m_CurrentMode");
    api.ai_hp = il2::field_offset(base_ai, "m_CurrentHP");
    api.ai_mode_enum = il2::find_class(kGame, "", "AiMode");
    api.experience_mode = il2::find_method(experience, "GetCurrentExperienceModeType", 0);
    api.experience_enum = il2::find_class(kGame, "", "ExperienceModeType");
    api.spawn_manager = il2::find_method(game_manager, "GetSpawnRegionManager", 0);
    api.spawn_regions = il2::field_offset(spawn_manager, "m_SpawnRegions");
    api.region_prefab = il2::field_offset(spawn_region, "m_SpawnablePrefab");
    api.region_refs = il2::field_offset(spawn_region, "m_SpawnsPrefabReferences");
    api.region_kind = il2::field_offset(spawn_region, "m_AiSubTypeSpawned");
    api.region_center = il2::field_offset(spawn_region, "m_Center");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    api.go_active = il2::find_method(game_object, "get_activeSelf", 0);
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.instantiate_spawn = il2::find_method(spawn_region, "InstantiateSpawn", 6);
    api.navmesh_sample = il2::find_method_params(
        navmesh, "SamplePosition", {"UnityEngine.Vector3", "UnityEngine.AI.NavMeshHit&", "System.Single", "System.Int32"});
    api.clone_at = il2::find_method_params(object, "Instantiate",
                                           {"UnityEngine.Object", "UnityEngine.Vector3", "UnityEngine.Quaternion"});
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    const bool wildlife = api.ai_list && api.ai_kind >= 0 && api.ai_mode >= 0 && api.spawn_manager &&
                          api.spawn_regions >= 0 && api.region_kind >= 0 && api.instantiate_spawn && api.clone_at;
    logx::info("dev commands: wildlife helpers %s", wildlife ? "ready" : "INCOMPLETE (see warnings above)");

    g_ready = api.door_list && api.door_target >= 0 && api.is_outdoors && api.player_manager && api.teleport &&
              api.component_transform && api.transform_position;
    fight.run_console = il2::find_method_params(il2::find_class(kGame, "", "uConsole"), "RunCommand", {"System.String"});
    fight.get_struggle = il2::find_method(game_manager, "GetPlayerStruggleComponent", 0);
    Il2CppClass* player_struggle = il2::find_class(kGame, "", "PlayerStruggle");
    fight.do_tap = il2::find_method(player_struggle, "DoTap", 0);
    fight.wolf_tap = il2::find_method(player_struggle, "WolfTap", 0);
    fight.partner = il2::field_offset(player_struggle, "m_PartnerBaseAi");
    fight.picker_pending = il2::find_method(player_struggle, "IsWeaponPickerSelectionPending", 0);
    fight.weapon_picked = il2::find_method(player_struggle, "OnWeaponPicked", 1);
    fight.weapon_used = il2::field_offset(player_struggle, "m_WeaponUsed");
    fight.weapon_choice = il2::field_offset(player_struggle, "m_WeaponPicked");
    Il2CppClass* player_manager_class = il2::find_class(kGame, "", "PlayerManager");
    fight.add_item = il2::find_method(player_manager_class, "AddItemCONSOLE", 3);
    fight.use_item = il2::find_method_params(player_manager_class, "UseInventoryItem", {"GearItem", "System.Boolean"});
    fight.in_hands = il2::find_method(player_manager_class, "get_m_ItemInHands", 0);
    fight.equip_item = il2::find_method(player_manager_class, "EquipItem", 2);
    fight.ready_to_equip = il2::find_method(player_manager_class, "IsReadyToEquip", 0);
    fight.fps_camera = il2::find_method(game_manager, "GetVpFPSCamera", 0);
    fight.weapon_switching = il2::find_method(il2::find_class(kGame, "", "vp_FPSCamera"), "WeaponSwitchInProgress", 0);
    Il2CppClass* unity_object = il2::find_class(kCore, "UnityEngine", "Object");
    fight.find_objects = il2::find_method_params(unity_object, "FindObjectsOfType", {"System.Type"});
    fight.find_objects_inactive = il2::find_method_params(unity_object, "FindObjectsOfType", {"System.Type", "System.Boolean"});
    fight.shooter_type = il2::type_object(il2::find_class(kGame, "", "vp_FPSShooter"));
    fight.set_enabled = il2::find_method(il2::find_class(kCore, "UnityEngine", "Behaviour"), "set_enabled", 1);
    fight.animator_speed = il2::find_method(il2::find_class("UnityEngine.AnimationModule.dll", "UnityEngine", "Animator"),
                                            "set_speed", 1);
    fight.ai_animator = il2::field_offset(il2::find_class(kGame, "", "BaseAi"), "m_Animator");
    fight.apply_damage = il2::find_method_params(il2::find_class(kGame, "", "BaseAi"), "ApplyDamage",
                                                 {"System.Single", "System.Single", "DamageSource", "System.String"});
    if (fight.shooter_type) il2::il2cpp_gchandle_new(fight.shooter_type, false);
    fight.add_rounds = il2::find_method(il2::find_class(kGame, "", "GunItem"), "AddRoundsToClip", 2);
    fight.gun_item = il2::field_offset(il2::find_class(kGame, "", "GearItem"), "m_GunItem");
    fight.get_inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    fight.best_weapon = il2::find_method(il2::find_class(kGame, "", "Inventory"), "GetBestStuggleBonusItem", 0);
    fight.object_name = il2::find_method(il2::find_class(kCore, "UnityEngine", "Object"), "get_name", 0);
    {
        Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
        held_api.torch = il2::field_offset(gear, "m_TorchItem");
        held_api.flare = il2::field_offset(gear, "m_FlareItem");
        held_api.lamp = il2::field_offset(gear, "m_KeroseneLampItem");
        held_api.flashlight = il2::field_offset(gear, "m_FlashlightItem");
        held_api.gun = il2::field_offset(gear, "m_GunItem");
        held_api.bow = il2::field_offset(gear, "m_BowItem");
        held_api.stone = il2::field_offset(gear, "m_StoneItem");
        held_api.spear = il2::field_offset(gear, "m_BearSpearItem");
        Il2CppClass* torch = il2::find_class(kGame, "", "TorchItem");
        held_api.torch_ignite = il2::find_method(torch, "Ignite", 0);
        held_api.torch_out = il2::find_method(torch, "ForceExtinguish", 0);
        held_api.flare_ignite = il2::find_method(il2::find_class(kGame, "", "FlareItem"), "Ignite", 0);
        Il2CppClass* lamp = il2::find_class(kGame, "TLD.Gear", "KeroseneLampItem");
        held_api.lamp_turn_on = il2::find_method(lamp, "TurnOn", 1);
        held_api.lamp_turn_off = il2::find_method(lamp, "TurnOff", 1);
        held_api.lamp_room = il2::find_method(lamp, "GetMaxFuelToAdd", 0);
        held_api.lamp_fill = il2::find_method(lamp, "AddFuel", 1);
        Il2CppClass* flashlight = il2::find_class(kGame, "", "FlashlightItem");
        held_api.flashlight_on = il2::find_method(flashlight, "TurnOn", 0);
        held_api.flashlight_off = il2::find_method(flashlight, "TurnOffImmediate", 0);
        held_api.flashlight_lights = il2::find_method(flashlight, "EnableLights", 1);
        held_api.flashlight_state = il2::field_offset(flashlight, "m_State");
        held_api.flashlight_charge = il2::field_offset(flashlight, "m_CurrentBatteryCharge");
        held_api.put_away = il2::find_method(il2::find_class(kGame, "", "PlayerManager"), "UnequipImmediate", 1);
        for (auto [cls, aim, lower] : {std::tuple{"GunItem", &held_api.gun_aim, &held_api.gun_lower},
                                       std::tuple{"BowItem", &held_api.bow_aim, &held_api.bow_lower},
                                       std::tuple{"StoneItem", &held_api.stone_aim, &held_api.stone_lower},
                                       std::tuple{"BearSpearItem", &held_api.spear_aim, &held_api.spear_lower}}) {
            Il2CppClass* c = il2::find_class(kGame, "", cls);
            *aim = il2::find_method(c, "ZoomStart", 0);
            *lower = il2::find_method(c, "ZoomEnd", 0);
        }
    }
    logx::info("dev commands: %s (write one to TLDCoop\\command.txt)", g_ready ? "ready" : "UNAVAILABLE");
    return g_ready;
}

// Taps at the rate asked for while TLD runs a struggle (PlayerStruggle.DoTap is what the struggle key does).
void tap_in_fights() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_last_tap_frame;
    g_last_tap_frame = now;
    if (g_taps_per_second <= 0 || !fight.do_tap || !game::in_struggle()) {
        g_tap_credit = 0;
        g_weapon_chosen = false;
        return;
    }
    if (last) g_tap_credit += (now - last) * g_taps_per_second / 1000.0;
    Il2CppObject* struggle = il2::invoke(fight.get_struggle, nullptr);
    // With a weapon in the pack, TLD 2.55 opens its weapon picker as the fight starts (time slowed). It closes by itself
    // when its time runs out, and the struggle then fights with whatever was clicked (nothing: fists); opening it clears
    // an earlier choice. Choose the best struggle weapon while it's up, as clicking it would, and don't tap meanwhile.
    bool pending = false;
    if (struggle && fight.picker_pending && il2::invoke_value(fight.picker_pending, struggle, pending) && pending) {
        if (!il2::read_field<Il2CppObject*>(struggle, fight.weapon_choice) && fight.weapon_picked) {
            Il2CppObject* inventory = il2::invoke(fight.get_inventory, nullptr);
            Il2CppObject* weapon = inventory ? il2::invoke(fight.best_weapon, inventory) : nullptr;
            void* args[] = {weapon};
            if (weapon) il2::invoke(fight.weapon_picked, struggle, args);
            if (!g_weapon_chosen)
                logx::info("dev: struggle weapon picker: %s", weapon ? il2::to_utf8(reinterpret_cast<Il2CppString*>(
                                                                           il2::invoke(fight.object_name, weapon))).c_str()
                                                                     : "nothing to pick");
            g_weapon_chosen = true;
        }
        g_tap_credit = 0;
        return;
    }
    Il2CppObject* partner = il2::read_field<Il2CppObject*>(struggle, fight.partner);
    const bool wolf = partner && il2::read_field<int32_t>(partner, api.ai_kind) == 1;
    for (; g_tap_credit >= 1 && struggle; g_tap_credit -= 1) {
        il2::invoke(wolf && fight.wolf_tap ? fight.wolf_tap : fight.do_tap, struggle);
        if (++g_taps_done % 10 == 1) {
            Il2CppObject* weapon = il2::read_field<Il2CppObject*>(struggle, fight.weapon_used);
            logx::info("dev: struggle tap %d (weapon %s)", g_taps_done,
                       weapon ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(fight.object_name, weapon))).c_str()
                              : "none");
        }
    }
}

void on_frame(bool in_game) {
    if (!g_ready) return;
    if (in_game) tap_in_fights();
    if (in_game && g_frozen && GetTickCount64() >= g_frozen_until) {
        if (Il2CppObject* ai = il2::il2cpp_gchandle_get_target(g_frozen)) {
            bool on = true;
            void* args[] = {&on};
            il2::invoke(fight.set_enabled, ai, args);
            if (Il2CppObject* animator = il2::read_field<Il2CppObject*>(ai, fight.ai_animator)) {
                float normal = 1;
                void* speed_args[] = {&normal};
                il2::invoke(fight.animator_speed, animator, speed_args);
            }
        }
        il2::il2cpp_gchandle_free(g_frozen);
        g_frozen = 0;
        logx::info("dev: freeze: over");
    }
    if (!g_at_command.empty() && wall_ms() >= g_at_ms) {
        const std::string line = std::move(g_at_command);
        g_at_command.clear();
        logx::info("dev: at %lld (%lld ms late): '%s'", g_at_ms, wall_ms() - g_at_ms, line.c_str());
        if (in_game) run(line);
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_poll < kPollMs) return;
    g_last_poll = now;
    const auto file = paths::instance_dir() / L"command.txt";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return;
    std::string line;
    {
        std::ifstream in(file);
        std::getline(in, line);
    }
    std::filesystem::remove(file, ec);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("at ", 0) == 0) {
        std::istringstream at(line.substr(3));
        long long ms = 0;
        std::string rest;
        if (at >> ms) std::getline(at >> std::ws, rest);
        if (rest.empty()) {
            logx::warn("dev: at: use `at MS COMMAND` (MS: the PC's clock, ms since 1970)");
            return;
        }
        g_at_ms = ms;
        g_at_command = rest;
        logx::info("dev: '%s' waits until %lld (in %lld ms)", rest.c_str(), ms, ms - wall_ms());
        return;
    }
    if (!in_game && line.rfind("menu", 0) != 0) {
        logx::warn("dev: '%s' needs the game running (at the menus only `menu ...` works)", line.c_str());
        return;
    }
    run(line);
}

}  // namespace dev_commands
