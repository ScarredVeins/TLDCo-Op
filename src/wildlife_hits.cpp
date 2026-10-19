#include "wildlife_hits.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"
#include "wildlife_sync.h"
#include "wildlife_targets.h"

namespace wildlife_hits {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule.dll";
constexpr const char* kAi = "UnityEngine.AIModule.dll";
constexpr int32_t kAiModeDead = 2, kAiModeStruggle = 14, kAiModeStunned = 20;
constexpr int32_t kDamageSourcePlayer = 1;
constexpr int32_t kSkillRifle = 4, kSkillArchery = 5, kSkillRevolver = 8;  // SkillType
constexpr int32_t kFleeThrownStone = 1, kFleeNoiseMaker = 10;              // BaseAi.AiFleeReason
constexpr float kNoiseMakerFleeMetres = 30;  // a noise maker's scare reach (TLD's is its own radius; close enough)
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kRaycastHitCollider = 0x28;  // RaycastHit.m_Collider (instance ID)
constexpr ptrdiff_t kContactOtherCollider = 0x28;  // ContactPoint.m_OtherColliderInstanceID
constexpr ptrdiff_t kContactSize = 0x30;
constexpr ptrdiff_t kArrayData = 0x20;

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

struct Api {
    // Unity
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* component_get = nullptr;    // Component.GetComponent(Type)
    const MethodInfo* go_transform = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* get_forward = nullptr;
    const MethodInfo* find_child = nullptr;       // Transform.Find(string)
    const MethodInfo* to_world = nullptr;         // Transform.TransformPoint(Vector3)
    const MethodInfo* to_local = nullptr;         // Transform.InverseTransformPoint(Vector3)
    const MethodInfo* object_name = nullptr;
    const MethodInfo* find_objects = nullptr;     // static Object.FindObjectsOfType(Type)
    const MethodInfo* find_object = nullptr;      // static Object.FindObjectOfType(Type)
    const MethodInfo* collider_by_id = nullptr;   // static Physics.GetColliderByInstanceID(int)
    const MethodInfo* navmesh_sample = nullptr;   // static NavMesh.SamplePosition(Vector3, out NavMeshHit, float, int)
    Il2CppObject* localized_damage_type = nullptr;
    Il2CppObject* shooter_type = nullptr;
    Il2CppObject* camera_type = nullptr;
    // LocalizedDamage: what TLD's own hit code asks (each forwards to the root's BodyDamage with the shooter's skills)
    const MethodInfo* damage_scale = nullptr;     // GetDamageScale(WeaponSource)
    const MethodInfo* bleed_minutes = nullptr;    // GetBleedOutMinutes(WeaponSource)
    const MethodInfo* roll_kill = nullptr;        // RollChanceToKill(WeaponSource)
    ptrdiff_t body_part = -1;                     // m_BodyPart
    // Projectiles
    ptrdiff_t bullet_damage = -1, bullet_accuracy = -1, bullet_falloff = -1, bullet_min_falloff = -1, bullet_gun = -1;
    ptrdiff_t arrow_type = -1, arrow_damage = -1, arrow_bow = -1;
    ptrdiff_t round_damage = -1;
    ptrdiff_t gear_stone = -1, gear_flare = -1, gear_torch = -1, gear_noise = -1;  // GearItem sub-components
    ptrdiff_t flare_type = -1;                    // FlareItem.m_Type
    // Skills (the shooter's, as vp_Bullet / ArrowItem award them on a hit)
    const MethodInfo* skills = nullptr;           // static GameManager.GetSkillsManager()
    const MethodInfo* add_skill = nullptr;        // SkillsManager.IncrementPointsAndNotify(SkillType, int, mode)
    // BaseAi (host)
    const MethodInfo* setup_damage_anim = nullptr;  // SetupDamageForAnim(Vector3, Vector3, LocalizedDamage)
    const MethodInfo* apply_damage = nullptr;       // virtual ApplyDamage(float, float, DamageSource, string)
    const MethodInfo* react_to_hit = nullptr;       // virtual MaybeFleeOrAttackFromProjectileHit(WeaponSource)
    const MethodInfo* flee_from = nullptr;          // FleeFrom(Vector3)
    const MethodInfo* stun = nullptr;               // Stun(Vector3)
    const MethodInfo* can_stun = nullptr;           // CurrentAIModeAllowStunFromThrownStone()
    const MethodInfo* stun_chance = nullptr;        // GetChanceStunFromThrownStone()
    FieldInfo* ai_list = nullptr;                   // static BaseAiManager.m_BaseAis
    ptrdiff_t ai_mode = -1, ai_ignore_crits = -1, ai_flee_reason = -1;
    ptrdiff_t chance_hit_stone = -1, chance_stone = -1, chance_flare = -1, chance_torch = -1, chance_noise = -1;
    ptrdiff_t reach_stone = -1, reach_flare = -1, reach_torch = -1;
    // Tests
    const MethodInfo* fire = nullptr;               // vp_FPSShooter.Fire()
    ptrdiff_t camera_yaw = -1, camera_pitch = -1;   // vp_FPSCamera.m_Yaw / m_Pitch (degrees)
    const MethodInfo* player_manager = nullptr;
    const MethodInfo* teleport = nullptr;           // PlayerManager.TeleportPlayer(Vector3, Quaternion)
    const MethodInfo* look_rotation = nullptr;      // static Quaternion.LookRotation(Vector3)
    ptrdiff_t arrow_speed = -1, round_speed = -1;   // ArrowItem / FlareGunRoundItem.m_ReleaseVelocity
    ptrdiff_t round_gravity = -1;                   // FlareGunRoundItem.m_AdditionalGravity
    const MethodInfo* in_hands = nullptr;           // PlayerManager.get_m_ItemInHands
    const MethodInfo* stone_throw = nullptr;        // StoneItem.Throw()
    const MethodInfo* spawn_and_fire = nullptr;     // static FlareGunRoundItem.SpawnAndFire(GameObject, Vector3, Quaternion)
    ptrdiff_t shooter_prefab = -1, shooter_custom = -1;  // vp_FPSShooter.ProjectilePrefab / ProjectileCustomPrefab
    const MethodInfo* set_rotation = nullptr;       // Transform.set_rotation
} api;

bool g_ready = false;

bool is_client() {
    const auto role = netrt::role();
    return role && *role == net::Role::Client;
}
bool is_host() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

std::string name_of(Il2CppObject* o) {
    return o ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, o))) : std::string();
}

Il2CppObject* component(Il2CppObject* c, Il2CppObject* type) {
    if (!c) return nullptr;
    void* args[] = {type};
    return il2::invoke(api.component_get, c, args);
}

bool position(Il2CppObject* transform, Vector3& out) {
    return transform && il2::invoke_value(api.get_position, transform, out);
}

float distance(const Vector3& a, const Vector3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float lz_call(const MethodInfo* m, Il2CppObject* ld, int32_t weapon) {
    float v = 0;
    void* args[] = {&weapon};
    il2::invoke_value(m, ld, v, args);
    return v;
}

bool roll_kill(Il2CppObject* ld, int32_t weapon) {
    bool v = false;
    void* args[] = {&weapon};
    return il2::invoke_value(api.roll_kill, ld, v, args) && v;
}

void add_skill_point(int32_t skill) {
    Il2CppObject* skills = il2::invoke(api.skills, nullptr);
    int32_t points = 1, mode = 0;  // AssignOnlyInSandbox, as vp_Bullet / ArrowItem do
    void* args[] = {&skill, &points, &mode};
    if (skills) il2::invoke(api.add_skill, skills, args);
}

const char* weapon_name(net::Weapon w) {
    switch (w) {
        case net::Weapon::Arrow: return "arrow";
        case net::Weapon::HardenedArrow: return "hardened arrow";
        case net::Weapon::FlareGun: return "flare-gun round";
        case net::Weapon::Revolver: return "revolver";
        case net::Weapon::Rifle: return "rifle";
    }
    return "?";
}

// --- client: our projectiles on the host's animals ---------------------------------------------------------------

// The hit as TLD would have dealt it to a real animal, sent to the host (which owns the animal).
void send_hit(uint32_t animal, net::Weapon weapon, Il2CppObject* ld, Il2CppObject* collider_transform, float damage,
              float bleed, bool crit, bool stuck, const Vector3& world) {
    net::HitMsg h;
    h.animal = animal;
    h.weapon = weapon;
    h.body_part = static_cast<uint8_t>(il2::read_field<int32_t>(ld, api.body_part));
    h.flags = static_cast<uint8_t>((crit ? net::kHitCritical : 0) | (stuck ? net::kHitStuck : 0));
    h.collider = name_of(collider_transform);
    h.damage = damage;
    h.bleed_minutes = bleed;
    h.world[0] = world.x, h.world[1] = world.y, h.world[2] = world.z;
    Vector3 local{};
    Vector3 point = world;
    void* args[] = {&point};
    il2::invoke_value(api.to_local, collider_transform, local, args);
    h.local[0] = local.x, h.local[1] = local.y, h.local[2] = local.z;
    net::Writer w(net::Msg::Hit);
    net::write_hit(w, h);
    netrt::send_packet(w, true);
    char bleeding[48] = "";
    if (bleed > 0) snprintf(bleeding, sizeof(bleeding), ", bleeds out in %.0f min", bleed);
    logx::info("wildlife: our %s hit the host's #%u (%s): %.1f damage%s%s%s", weapon_name(weapon), animal,
               h.collider.c_str(), damage, crit ? " (kill chance came up)" : "", bleeding, stuck ? ", stuck in" : "");
}

// Rifle and revolver: vp_Bullet.Start has raycast and shown the impact (blood on our copy); straight after, it would
// look for the animal's BaseAi and find none. The same sums it would then do.
using SpawnImpactFn = void (*)(Il2CppObject* self, void* hit, const MethodInfo* method);
SpawnImpactFn g_spawn_impact = nullptr;

void spawn_impact_detour(Il2CppObject* self, void* hit, const MethodInfo* method) {
    g_spawn_impact(self, hit, method);
    if (!g_ready || !hit || !is_client()) return;
    int32_t collider_id = 0;
    memcpy(&collider_id, static_cast<char*>(hit) + kRaycastHitCollider, sizeof(collider_id));
    void* id_args[] = {&collider_id};
    Il2CppObject* collider = il2::invoke(api.collider_by_id, nullptr, id_args);
    Il2CppObject* tf = collider ? il2::invoke(api.component_transform, collider) : nullptr;
    uint32_t animal = 0;
    bool dead = false;
    const bool copy = tf && wildlife_sync::puppet_at(tf, animal, dead);
    static int impacts_logged = 0;
    if (impacts_logged++ < 30) {
        Vector3 point{};
        memcpy(&point, hit, sizeof(point));
        logx::info("wildlife: a bullet of ours hit '%s' at (%.1f, %.1f, %.1f)%s", tf ? name_of(tf).c_str() : "?", point.x,
                   point.y, point.z, copy ? (" on our copy of the host's #" + std::to_string(animal)).c_str() : "");
    }
    if (!copy || dead) return;
    Il2CppObject* ld = component(collider, api.localized_damage_type);
    if (!ld) return;
    const int32_t gun = il2::read_field<int32_t>(self, api.bullet_gun);  // GunType: Revolver 0, Rifle 1
    const net::Weapon weapon = gun == 0 ? net::Weapon::Revolver : net::Weapon::Rifle;
    const int32_t ws = static_cast<int32_t>(weapon);
    Vector3 at{}, me{};
    position(tf, at);
    const game::PlayerState p = game::read_player();
    me = {p.position.x, p.position.y, p.position.z};
    const float range = distance(at, me);
    const float bleed = lz_call(api.bleed_minutes, ld, ws);
    float damage = il2::read_field<float>(self, api.bullet_damage) * lz_call(api.damage_scale, ld, ws);
    const float effective = il2::read_field<float>(self, api.bullet_accuracy);
    bool crit = false;
    if (effective <= range) {  // beyond the effective range: falloff, and no kill chance
        const float falloff = damage - (range - effective) * il2::read_field<float>(self, api.bullet_falloff);
        const float minimum = il2::read_field<float>(self, api.bullet_min_falloff);
        damage = falloff > minimum ? falloff : minimum;
    } else {
        crit = roll_kill(ld, ws);
    }
    add_skill_point(weapon == net::Weapon::Revolver ? kSkillRevolver : kSkillRifle);
    Vector3 point{};
    memcpy(&point, hit, sizeof(point));  // RaycastHit.m_Point
    send_hit(animal, weapon, ld, tf, damage, bleed, crit, false, point);
}

// Arrows: ArrowItem.HandleCollisionWithObject has stuck the arrow in our copy (parented to the hit collider) and
// shown the impact; InflictDamage would now find no BaseAi. Done here instead, with ArrowItem's own sums.
using ArrowDamageFn = Il2CppObject* (*)(Il2CppObject* self, Il2CppObject* victim, float scalar, bool stick_in,
                                         Il2CppString* collider, Vector3* point, const MethodInfo* method);
ArrowDamageFn g_arrow_damage = nullptr;

bool copy_hit(Il2CppObject* victim_go, uint32_t& animal, Il2CppObject*& tf, Il2CppObject*& ld) {
    if (!g_ready || !victim_go || !is_client()) return false;
    tf = il2::invoke(api.go_transform, victim_go);
    bool dead = false;
    if (!tf || !wildlife_sync::puppet_at(tf, animal, dead)) return false;
    if (dead) return true;  // a copy, but nothing left to hurt
    ld = component(tf, api.localized_damage_type);
    return true;
}

Il2CppObject* arrow_damage_detour(Il2CppObject* self, Il2CppObject* victim, float scalar, bool stick_in,
                                  Il2CppString* collider, Vector3* point, const MethodInfo* method) {
    uint32_t animal = 0;
    Il2CppObject *tf = nullptr, *ld = nullptr;
    if (!copy_hit(victim, animal, tf, ld)) return g_arrow_damage(self, victim, scalar, stick_in, collider, point, method);
    if (!ld) return nullptr;
    const int32_t ws = il2::read_field<int32_t>(self, api.arrow_type);  // WeaponSource: Arrow 1, HardenedArrow 2
    const float scale = lz_call(api.damage_scale, ld, ws);
    const float damage = scalar * il2::read_field<float>(self, api.arrow_damage) * scale *
                         il2::read_field<float>(self, api.arrow_bow);
    const bool hardened_no_part = ws == 2 && scale == 0;
    const bool crit = roll_kill(ld, ws);
    const float bleed = stick_in && !hardened_no_part ? lz_call(api.bleed_minutes, ld, ws) : 0;
    add_skill_point(kSkillArchery);
    send_hit(animal, ws == 2 ? net::Weapon::HardenedArrow : net::Weapon::Arrow, ld, tf, damage, bleed, crit, stick_in,
             point ? *point : Vector3{});
    return nullptr;
}

// Flare-gun rounds: the same, with FlareGunRoundItem's sums (weapon source FlareGun, no skill).
using RoundDamageFn = Il2CppObject* (*)(Il2CppObject* self, Il2CppObject* victim, float scalar, bool stick_in,
                                         Il2CppString* collider, const MethodInfo* method);
RoundDamageFn g_round_damage = nullptr;

Il2CppObject* round_damage_detour(Il2CppObject* self, Il2CppObject* victim, float scalar, bool stick_in,
                                  Il2CppString* collider, const MethodInfo* method) {
    uint32_t animal = 0;
    Il2CppObject *tf = nullptr, *ld = nullptr;
    static int calls_logged = 0;
    if (calls_logged++ < 20)
        logx::info("wildlife: a flare round hit '%s'", victim ? name_of(victim).c_str() : "?");
    if (!copy_hit(victim, animal, tf, ld)) return g_round_damage(self, victim, scalar, stick_in, collider, method);
    if (!ld) return nullptr;
    const int32_t ws = static_cast<int32_t>(net::Weapon::FlareGun);
    const float damage = il2::read_field<float>(self, api.round_damage) * scalar * lz_call(api.damage_scale, ld, ws);
    const bool crit = roll_kill(ld, ws);
    const float bleed = stick_in ? lz_call(api.bleed_minutes, ld, ws) : 0;
    Vector3 at{};
    position(il2::invoke(api.component_transform, self), at);
    send_hit(animal, net::Weapon::FlareGun, ld, tf, damage, bleed, crit, stick_in, at);
    return nullptr;
}

// Thrown stones, flares, torches and noise makers: TLD's BaseAi.MaybeFleeFromThrownItem decides which animals take
// fright (and the stone then stuns small game); ours are hidden, so the host does it with its own.
using ThrownFn = void (*)(Il2CppObject* item, Il2CppObject* contacts, const MethodInfo* method);
ThrownFn g_thrown = nullptr;

void thrown_detour(Il2CppObject* item, Il2CppObject* contacts, const MethodInfo* method) {
    g_thrown(item, contacts, method);
    if (!g_ready || !item || !is_client()) return;
    net::ThrownMsg t;
    if (il2::read_field<Il2CppObject*>(item, api.gear_stone)) t.item = net::ThrownItem::Stone;
    else if (Il2CppObject* flare = il2::read_field<Il2CppObject*>(item, api.gear_flare)) {
        t.item = net::ThrownItem::Flare;
        t.flare_type = static_cast<uint8_t>(il2::read_field<int32_t>(flare, api.flare_type));
    } else if (il2::read_field<Il2CppObject*>(item, api.gear_torch)) t.item = net::ThrownItem::Torch;
    else if (il2::read_field<Il2CppObject*>(item, api.gear_noise)) t.item = net::ThrownItem::NoiseMaker;
    else return;
    Vector3 at{};
    position(il2::invoke(api.component_transform, item), at);
    t.pos[0] = at.x, t.pos[1] = at.y, t.pos[2] = at.z;
    for (size_t i = 0, n = il2::array_length(contacts); i < n && !t.animal; ++i) {
        const char* contact = reinterpret_cast<const char*>(contacts) + kArrayData + i * kContactSize;
        int32_t id = 0;
        memcpy(&id, contact + kContactOtherCollider, sizeof(id));
        void* id_args[] = {&id};
        Il2CppObject* collider = il2::invoke(api.collider_by_id, nullptr, id_args);
        uint32_t animal = 0;
        bool dead = false;
        if (collider && wildlife_sync::puppet_at(il2::invoke(api.component_transform, collider), animal, dead) && !dead) {
            t.animal = animal;
            memcpy(t.contact, contact, sizeof(t.contact));  // ContactPoint.m_Point
        }
    }
    net::Writer w(net::Msg::Thrown);
    net::write_thrown(w, t);
    netrt::send_packet(w, true);
    static const char* kItems[] = {"?", "stone", "flare", "torch", "noise maker"};
    logx::info("wildlife: our thrown %s landed at (%.1f, %.1f, %.1f)%s", kItems[static_cast<int>(t.item)], at.x, at.y,
               at.z, t.animal ? (" on the host's #" + std::to_string(t.animal)).c_str() : "");
}

// --- host: the other players' hits on our animals ----------------------------------------------------------------

// Friend `from` shot one of our animals.
void apply_hit(const net::HitMsg& h, uint8_t from) {
    Il2CppObject* ai = wildlife_sync::host_animal(h.animal);
    if (!ai || il2::read_field<int32_t>(ai, api.ai_mode) == kAiModeDead) {
        logx::info("wildlife: the other player's %s hit #%u, which is %s", weapon_name(h.weapon), h.animal,
                   ai ? "dead already" : "gone");
        return;
    }
    // The same collider on our animal: its LocalizedDamage tells TLD the body part (hit reaction, which side it falls).
    Il2CppObject* root = il2::invoke(api.component_transform, ai);
    void* find_args[] = {il2::il2cpp_string_new(("colliders/" + h.collider).c_str())};
    Il2CppObject* collider_tf = root ? il2::invoke(api.find_child, root, find_args) : nullptr;
    Il2CppObject* ld = component(collider_tf, api.localized_damage_type);
    Vector3 world{h.world[0], h.world[1], h.world[2]};
    if (collider_tf) {
        Vector3 local{h.local[0], h.local[1], h.local[2]};
        void* args[] = {&local};
        il2::invoke_value(api.to_world, collider_tf, world, args);
    }
    float damage = h.damage;
    const bool kill = (h.flags & net::kHitCritical) && !il2::read_field<bool>(ai, api.ai_ignore_crits);
    if (kill) damage = std::numeric_limits<float>::infinity();
    const float hp_before = wildlife_sync::host_animal_hp(ai);
    const bool done = wildlife_targets::as_friends_doing(ai, from, [&](const wildlife_targets::FriendStandIn& who) {
        Vector3 source{who.position[0], who.position[1], who.position[2]};
        void* anim_args[] = {&world, &source, ld};
        il2::invoke(api.setup_damage_anim, ai, anim_args);
        float bleed = h.bleed_minutes;
        int32_t src = kDamageSourcePlayer;
        void* damage_args[] = {&damage, &bleed, &src, il2::il2cpp_string_new(h.collider.c_str())};
        il2::invoke(il2::virtual_method(ai, api.apply_damage), ai, damage_args);
        int32_t ws = static_cast<int32_t>(h.weapon);
        void* react_args[] = {&ws};
        il2::invoke(il2::virtual_method(ai, api.react_to_hit), ai, react_args);
    });
    if (!done) {
        logx::warn("wildlife: the other player's hit on #%u can't be applied (their avatar isn't shown)", h.animal);
        return;
    }
    logx::info("wildlife: the other player's %s hit #%u (%s%s): hp %.1f -> %.1f, now %s", weapon_name(h.weapon), h.animal,
               h.collider.c_str(), kill ? ", a kill shot" : "", hp_before, wildlife_sync::host_animal_hp(ai),
               il2::read_field<int32_t>(ai, api.ai_mode) == kAiModeDead ? "dead" : "alive");
}

std::vector<Il2CppObject*> all_animals() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = nullptr;
    if (api.ai_list) il2::il2cpp_field_static_get_value(api.ai_list, &list);
    Il2CppObject* items = il2::read_field<Il2CppObject*>(list, kListItems);
    const int32_t count = il2::read_field<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* o = il2::array_object_at(items, static_cast<size_t>(i))) out.push_back(o);
    return out;
}

// TLD's thrown-item rules (BaseAi.MaybeFleeFromThrownItem / MaybeStunnedByStone), with each animal's own chances: a
// hit always scares (a stone has its own chance), a near miss scares within the item's reach.
void apply_thrown(const net::ThrownMsg& t, uint8_t from) {
    const Vector3 at{t.pos[0], t.pos[1], t.pos[2]};
    int scared = 0;
    bool stunned = false;
    for (Il2CppObject* ai : all_animals()) {
        const int32_t mode = il2::read_field<int32_t>(ai, api.ai_mode);
        if (mode == kAiModeDead || mode == kAiModeStruggle || mode == kAiModeStunned) continue;
        const bool hit = t.animal && wildlife_sync::host_animal_id(ai) == t.animal;
        Vector3 pos{};
        if (!position(il2::invoke(api.component_transform, ai), pos)) continue;
        const float metres = distance(pos, at);
        int32_t chance = 0;
        float reach = 0;
        switch (t.item) {
            case net::ThrownItem::Stone:
                chance = il2::read_field<int32_t>(ai, hit ? api.chance_hit_stone : api.chance_stone);
                reach = static_cast<float>(il2::read_field<int32_t>(ai, api.reach_stone));
                break;
            case net::ThrownItem::Flare:
                chance = hit ? 100 : il2::read_field<int32_t>(ai, api.chance_flare);
                reach = static_cast<float>(il2::read_field<int32_t>(ai, api.reach_flare));
                break;
            case net::ThrownItem::Torch:
                chance = hit ? 100 : il2::read_field<int32_t>(ai, api.chance_torch);
                reach = static_cast<float>(il2::read_field<int32_t>(ai, api.reach_torch));
                break;
            case net::ThrownItem::NoiseMaker:
                chance = hit ? 100 : il2::read_field<int32_t>(ai, api.chance_noise);
                reach = kNoiseMakerFleeMetres;
                break;
        }
        if (hit && t.item == net::ThrownItem::Stone) {
            bool can = false;
            float stun = 0;
            il2::invoke_value(api.can_stun, ai, can);
            il2::invoke_value(api.stun_chance, ai, stun);
            if (can && static_cast<float>(rand() % 100) < stun) {
                Vector3 contact{t.contact[0], t.contact[1], t.contact[2]};
                void* args[] = {&contact};
                il2::invoke(api.stun, ai, args);
                stunned = true;
                continue;
            }
        }
        if (!hit && metres > reach) continue;
        if (rand() % 100 >= chance) continue;
        wildlife_targets::as_friends_doing(ai, from, [&](const wildlife_targets::FriendStandIn&) {
            Vector3 landed = at;
            void* args[] = {&landed};
            il2::invoke(api.flee_from, ai, args);
        });
        int32_t reason = t.item == net::ThrownItem::Stone ? kFleeThrownStone : t.item == net::ThrownItem::NoiseMaker ? kFleeNoiseMaker : 0;
        if (reason && !il2::read_field<int32_t>(ai, api.ai_flee_reason))
            memcpy(reinterpret_cast<char*>(ai) + api.ai_flee_reason, &reason, sizeof(reason));
        ++scared;
    }
    static const char* kItems[] = {"?", "stone", "flare", "torch", "noise maker"};
    logx::info("wildlife: the other player's thrown %s landed %.0f m from us%s: %d animal(s) take fright%s",
               kItems[static_cast<int>(t.item)], distance(at, Vector3{game::read_player().position.x,
                                                                       game::read_player().position.y,
                                                                       game::read_player().position.z}),
               t.animal ? (" on #" + std::to_string(t.animal)).c_str() : "", scared, stunned ? ", one stunned" : "");
}

// --- client tests: aim at a copy and fire -----------------------------------------------------------------------------

// A test shot's bullet is pointed straight at the target collider as it starts (hip fire spreads, and the barrel sits
// off the camera): what's tested is everything after the gun, i.e. TLD's raycast and impact, then our hit.
uintptr_t g_bullet_target = 0;  // Transform

// Arrows and flare rounds fly an arc: pointed up enough to drop onto the target at their release speed (no drag).
void aim_projectile(Il2CppObject* self, float speed, float extra_gravity = 0) {
    Il2CppObject* target = g_bullet_target ? il2::il2cpp_gchandle_get_target(g_bullet_target) : nullptr;
    Il2CppObject* tf = il2::invoke(api.component_transform, self);
    Vector3 from{}, to{};
    if (!target || !position(tf, from) || !position(target, to)) return;
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const float flat = std::sqrt(dx * dx + dz * dz);
    const float gravity = 9.81f + extra_gravity;
    const float lift = speed > 1 ? 0.5f * std::asin(std::fmin(1.0f, gravity * flat / (speed * speed))) : 0;
    const float up = std::atan2(dy, flat) + lift;
    Vector3 dir{dx / flat * std::cos(up), std::sin(up), dz / flat * std::cos(up)};
    Quaternion rot{};
    void* look_args[] = {&dir};
    if (il2::invoke_value(api.look_rotation, nullptr, rot, look_args)) {
        void* rot_args[] = {&rot};
        il2::invoke(api.set_rotation, tf, rot_args);
    }
    logx::info("dev: shoot: the projectile leaves %.1f m from the target at %.0f m/s, aimed %.1f degrees above it",
               distance(from, to), speed, lift * 57.29578f);
    il2::il2cpp_gchandle_free(g_bullet_target);
    g_bullet_target = 0;
}

using ArrowFireFn = void (*)(Il2CppObject* self, float bow_multiplier, const MethodInfo* method);
using RoundFireFn = void (*)(Il2CppObject* self, const MethodInfo* method);
ArrowFireFn g_arrow_fire = nullptr;
RoundFireFn g_round_fire = nullptr;

void arrow_fire_detour(Il2CppObject* self, float bow_multiplier, const MethodInfo* method) {
    if (g_bullet_target) aim_projectile(self, il2::read_field<float>(self, api.arrow_speed));
    g_arrow_fire(self, bow_multiplier, method);
}

void round_fire_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_bullet_target) {  // flare rounds fall faster than g (m_AdditionalGravity)
        const Vector3 extra = il2::read_field<Vector3>(self, api.round_gravity);
        aim_projectile(self, il2::read_field<float>(self, api.round_speed), extra.y < 0 ? -extra.y : 0);
    }
    g_round_fire(self, method);
}

using BulletStartFn = void (*)(Il2CppObject* self, const MethodInfo* method);
BulletStartFn g_bullet_start = nullptr;

void bullet_start_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_bullet_target) {
        Il2CppObject* target = il2::il2cpp_gchandle_get_target(g_bullet_target);
        Il2CppObject* tf = il2::invoke(api.component_transform, self);
        Vector3 from{}, to{};
        if (target && position(tf, from) && position(target, to)) {
            Vector3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
            Quaternion rot{};
            void* look_args[] = {&dir};
            if (il2::invoke_value(api.look_rotation, nullptr, rot, look_args)) {
                void* rot_args[] = {&rot};
                il2::invoke(api.set_rotation, tf, rot_args);
            }
            logx::info("dev: shoot: the bullet starts %.1f m from the target, pointed at it", distance(from, to));
        }
        il2::il2cpp_gchandle_free(g_bullet_target);
        g_bullet_target = 0;
    }
    g_bullet_start(self, method);
}

// A test shot is played as the player's inputs: aim the camera, hold the aim button (TLD's "alt fire", which raises a
// rifle to the shoulder: TLD only fires a raised rifle), press fire once, let go.
enum class ShotStep { Aim, Raise, Fire, Draw, Loose, Hold, Release };
struct Shot {
    bool active = false;
    uintptr_t target = 0;  // the copy's hit collider (Transform)
    uint32_t animal = 0;
    std::string collider;
    int frames = 0;
    float pitch_sign = 1;
    bool flipped = false;
    ShotStep step = ShotStep::Aim;
    ULONGLONG step_since = 0;
    bool look_only = false;   // 'look': aim the camera at a point and stop there
    float point[3] = {};
} g_shot;

using InputFn = bool (*)(Il2CppObject* context, const MethodInfo* method);
InputFn g_alt_fire = nullptr, g_alt_fire_pressed = nullptr, g_alt_fire_released = nullptr;
InputFn g_fire_pressed = nullptr, g_fire_released = nullptr;
bool g_alt_pressed_sent = false;
// hold_aim: the aim button held (pressed once first), then released once.
bool g_aim_held = false, g_aim_pressed_sent = false, g_aim_release_due = false;

bool alt_fire_detour(Il2CppObject* c, const MethodInfo* m) {
    if (g_shot.active && g_shot.step >= ShotStep::Raise && g_shot.step <= ShotStep::Hold) return true;  // aim held
    if (g_aim_held) return true;
    return g_alt_fire(c, m);
}
bool alt_fire_pressed_detour(Il2CppObject* c, const MethodInfo* m) {
    if (g_shot.active && g_shot.step == ShotStep::Raise && !g_alt_pressed_sent) return g_alt_pressed_sent = true;
    if (g_aim_held && !g_aim_pressed_sent) return g_aim_pressed_sent = true;
    return g_alt_fire_pressed(c, m);
}
bool alt_fire_released_detour(Il2CppObject* c, const MethodInfo* m) {
    if (g_shot.active && g_shot.step == ShotStep::Release) return true;
    if (g_aim_release_due) {
        g_aim_release_due = false;
        return true;
    }
    return g_alt_fire_released(c, m);
}
bool fire_pressed_detour(Il2CppObject* c, const MethodInfo* m) {
    if (g_shot.active && g_shot.step == ShotStep::Fire) return true;
    return g_fire_pressed(c, m);
}
bool fire_released_detour(Il2CppObject* c, const MethodInfo* m) {
    if (g_shot.active && g_shot.step == ShotStep::Loose) return true;  // a bow looses its arrow on release
    return g_fire_released(c, m);
}

// The nearest live copy's hit collider called `collider` (any torso part if there's none by that name).
Il2CppObject* copy_collider(const std::string& wanted, uint32_t& animal) {
    Il2CppObject* found = nullptr;
    float best = 0;
    const game::PlayerState p = game::read_player();
    const Vector3 me{p.position.x, p.position.y, p.position.z};
    void* args[] = {api.localized_damage_type};
    Il2CppObject* all = il2::invoke(api.find_objects, nullptr, args);
    for (size_t i = 0, n = il2::array_length(all); i < n; ++i) {
        Il2CppObject* ld = il2::array_object_at(all, i);
        Il2CppObject* tf = ld ? il2::invoke(api.component_transform, ld) : nullptr;
        uint32_t id = 0;
        bool dead = false;
        if (!tf || !wildlife_sync::puppet_at(tf, id, dead) || dead) continue;
        const std::string name = name_of(tf);
        const bool match = wanted.empty() ? il2::read_field<int32_t>(ld, api.body_part) == 1 : name == wanted;
        if (!match) continue;
        Vector3 at{};
        position(tf, at);
        const float d = distance(at, me);
        if (!found || d < best) found = tf, best = d, animal = id;
    }
    return found;
}

Il2CppObject* fps_camera() {
    void* args[] = {api.camera_type};
    return il2::invoke(api.find_object, nullptr, args);
}

void aim(Il2CppObject* camera, const Vector3& from, const Vector3& to, float pitch_sign) {
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    float yaw = std::atan2(dx, dz) * 57.29578f;
    if (yaw < 0) yaw += 360;
    const float pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 57.29578f * pitch_sign;
    memcpy(reinterpret_cast<char*>(camera) + api.camera_yaw, &yaw, sizeof(yaw));
    memcpy(reinterpret_cast<char*>(camera) + api.camera_pitch, &pitch, sizeof(pitch));
}

void run_shot() {
    if (!g_shot.active) return;
    Il2CppObject* target = g_shot.target ? il2::il2cpp_gchandle_get_target(g_shot.target) : nullptr;
    Il2CppObject* camera = fps_camera();
    Il2CppObject* camera_tf = camera ? il2::invoke(api.component_transform, camera) : nullptr;
    Vector3 to{g_shot.point[0], g_shot.point[1], g_shot.point[2]}, from{}, forward{};
    if ((!g_shot.look_only && (!target || !position(target, to))) || !camera_tf || !position(camera_tf, from)) {
        logx::warn("dev: shoot: lost the target or the camera");
        g_shot.active = false;
        return;
    }
    aim(camera, from, to, g_shot.pitch_sign);
    if (++g_shot.frames < 6) return;
    il2::invoke_value(api.get_forward, camera_tf, forward);
    const float len = distance(to, from);
    const float dot = ((to.x - from.x) * forward.x + (to.y - from.y) * forward.y + (to.z - from.z) * forward.z) / len;
    const float off = std::acos(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 57.29578f;
    if (off > 2.0f && !g_shot.flipped) {  // vp_FPSCamera's pitch runs the other way: aim again
        g_shot.flipped = true;
        g_shot.pitch_sign = -g_shot.pitch_sign;
        g_shot.frames = 0;
        return;
    }
    if (g_shot.look_only) {
        logx::info("dev: look: the camera points at (%.1f, %.1f, %.1f), %.1f m away (%.1f degrees off)", to.x, to.y, to.z,
                   len, off);
        g_shot = {};
        return;
    }
    const ULONGLONG now = GetTickCount64();
    auto next = [&](ShotStep step) {
        g_shot.step = step;
        g_shot.step_since = now;
    };
    switch (g_shot.step) {
        case ShotStep::Aim: {
            // A stone in our hands: TLD's own throw (StoneItem.Throw, along the camera we just aimed).
            Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
            Il2CppObject* held = pm ? il2::invoke(api.in_hands, pm) : nullptr;
            if (Il2CppObject* stone = held ? il2::read_field<Il2CppObject*>(held, api.gear_stone) : nullptr) {
                logx::info("dev: shoot: aimed at the host's #%u %s, %.1f m away (%.1f degrees off); throwing a stone",
                           g_shot.animal, g_shot.collider.c_str(), len, off);
                il2::invoke(api.stone_throw, stone);
                break;
            }
            void* args[] = {api.shooter_type};
            Il2CppObject* shooter = il2::invoke(api.find_object, nullptr, args);
            // A flare gun: its own round, launched from just in front of the camera at the target (FlareGunRoundItem.
            // SpawnAndFire, what vp_FPSShooter.Fire does for it).
            if (shooter && il2::read_field<bool>(shooter, api.shooter_custom)) {
                Vector3 start{from.x + forward.x * 0.6f, from.y + forward.y * 0.6f, from.z + forward.z * 0.6f};
                Vector3 dir{to.x - start.x, to.y - start.y, to.z - start.z};
                Quaternion rot{};
                void* look_args[] = {&dir};
                il2::invoke_value(api.look_rotation, nullptr, rot, look_args);
                if (g_bullet_target) il2::il2cpp_gchandle_free(g_bullet_target);
                g_bullet_target = il2::il2cpp_gchandle_new(target, false);
                void* fire_args[] = {il2::read_field<Il2CppObject*>(shooter, api.shooter_prefab), &start, &rot};
                logx::info("dev: shoot: aimed at the host's #%u %s, %.1f m away; launching a flare round", g_shot.animal,
                           g_shot.collider.c_str(), len);
                il2::invoke(api.spawn_and_fire, nullptr, fire_args);
                if (g_bullet_target) {
                    logx::warn("dev: shoot: the flare round didn't fire");
                    il2::il2cpp_gchandle_free(g_bullet_target);
                    g_bullet_target = 0;
                }
                break;
            }
            if (!shooter && g_shot.frames < 300) return;  // the weapon is still being drawn: keep aiming
            logx::info("dev: shoot: aimed at the host's #%u %s, %.1f m away (%.1f degrees off); %s", g_shot.animal,
                       g_shot.collider.c_str(), len, off, shooter ? "raising the weapon" : "nothing in our hands to fire");
            if (!shooter) break;
            g_alt_pressed_sent = false;
            next(ShotStep::Raise);
            return;
        }
        case ShotStep::Raise:
            if (now - g_shot.step_since < 1500) return;
            if (g_bullet_target) il2::il2cpp_gchandle_free(g_bullet_target);
            g_bullet_target = il2::il2cpp_gchandle_new(target, false);
            logx::info("dev: shoot: firing");
            next(ShotStep::Fire);
            return;
        case ShotStep::Fire:
            next(ShotStep::Draw);  // fire was "pressed" for one frame...
            return;
        case ShotStep::Draw:
            if (now - g_shot.step_since < 1200) return;  // ...and held (a bow draws meanwhile; a gun has fired)
            next(ShotStep::Loose);
            return;
        case ShotStep::Loose:
            next(ShotStep::Hold);  // released for one frame
            return;
        case ShotStep::Hold:
            if (now - g_shot.step_since < 600) return;
            next(ShotStep::Release);
            return;
        case ShotStep::Release:
            break;
    }
    if (g_bullet_target && g_shot.step == ShotStep::Release) {  // no bullet came out of it
        logx::warn("dev: shoot: TLD fired no bullet (not raised? no rounds?)");
        il2::il2cpp_gchandle_free(g_bullet_target);
        g_bullet_target = 0;
    }
    il2::il2cpp_gchandle_free(g_shot.target);
    g_shot = {};
}

}  // namespace

bool approach(float metres) {
    if (!is_client()) {
        logx::warn("dev: approach: only the friend's game has copies of the host's animals");
        return false;
    }
    uint32_t animal = 0;
    Il2CppObject* tf = copy_collider("", animal);
    Vector3 at{};
    if (!tf || !position(tf, at)) {
        logx::warn("dev: approach: none of the host's animals is shown here");
        return false;
    }
    const game::PlayerState p = game::read_player();
    float dx = p.position.x - at.x, dz = p.position.z - at.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.1f) dx = 1, dz = 0;
    else dx /= len, dz /= len;
    Vector3 pos{at.x + dx * metres, at.y, at.z + dz * metres};
    NavMeshHit hit{};
    float radius = 10;
    int32_t areas = -1;
    void* nav_args[] = {&pos, &hit, &radius, &areas};
    bool on_mesh = false;
    if (il2::invoke_value(api.navmesh_sample, nullptr, on_mesh, nav_args) && on_mesh) pos = hit.position;
    pos.y += 0.3f;
    const float yaw = std::atan2(at.x - pos.x, at.z - pos.z);
    Quaternion rot{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    void* args[] = {&pos, &rot};
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    if (pm) il2::invoke(api.teleport, pm, args);
    logx::info("dev: approach: %.0f m from the host's #%u, now at (%.1f, %.1f, %.1f)", metres, animal, pos.x, pos.y, pos.z);
    return pm != nullptr;
}

bool look(float x, float y, float z) {
    if (g_shot.target) il2::il2cpp_gchandle_free(g_shot.target);
    g_shot = {};
    g_shot.active = true;
    g_shot.look_only = true;
    g_shot.point[0] = x, g_shot.point[1] = y, g_shot.point[2] = z;
    return true;
}

bool hold_aim(bool on) {
    if (!g_alt_fire || !g_alt_fire_pressed || !g_alt_fire_released) return false;
    // Each sent once, when TLD next asks: a release still due from before (nothing in hand asked) would end this aim.
    if (on && !g_aim_held) g_aim_pressed_sent = false, g_aim_release_due = false;
    if (!on && g_aim_held) g_aim_release_due = true, g_aim_pressed_sent = true;
    g_aim_held = on;
    return true;
}

bool shoot(const std::string& collider) {
    if (!is_client()) {
        logx::warn("dev: shoot: only the friend's game has copies of the host's animals");
        return false;
    }
    uint32_t animal = 0;
    Il2CppObject* tf = copy_collider(collider, animal);
    if (!tf) {
        logx::warn("dev: shoot: no copy of a host animal with a '%s' collider here", collider.empty() ? "torso" : collider.c_str());
        return false;
    }
    if (g_shot.target) il2::il2cpp_gchandle_free(g_shot.target);
    g_shot = {};
    g_shot.active = true;
    g_shot.target = il2::il2cpp_gchandle_new(tf, false);
    g_shot.animal = animal;
    g_shot.collider = name_of(tf);
    return true;
}

bool init() {
    Il2CppClass* component_class = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* base_ai = il2::find_class(kGame, "", "BaseAi");
    Il2CppClass* localized = il2::find_class(kGame, "", "LocalizedDamage");
    Il2CppClass* bullet = il2::find_class(kGame, "", "vp_Bullet");
    Il2CppClass* arrow = il2::find_class(kGame, "", "ArrowItem");
    Il2CppClass* round = il2::find_class(kGame, "", "FlareGunRoundItem");
    Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");

    api.component_transform = il2::find_method(component_class, "get_transform", 0);
    api.component_get = il2::find_method_params(component_class, "GetComponent", {"System.Type"});
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.get_forward = il2::find_method(transform, "get_forward", 0);
    api.find_child = il2::find_method_params(transform, "Find", {"System.String"});
    api.to_world = il2::find_method_params(transform, "TransformPoint", {"UnityEngine.Vector3"});
    api.to_local = il2::find_method_params(transform, "InverseTransformPoint", {"UnityEngine.Vector3"});
    api.object_name = il2::find_method(object, "get_name", 0);
    api.find_objects = il2::find_method_params(object, "FindObjectsOfType", {"System.Type"});
    api.find_object = il2::find_method_params(object, "FindObjectOfType", {"System.Type"});
    api.collider_by_id = il2::find_method(il2::find_class(kPhysics, "UnityEngine", "Physics"), "GetColliderByInstanceID", 1);
    api.navmesh_sample = il2::find_method(il2::find_class(kAi, "UnityEngine.AI", "NavMesh"), "SamplePosition", 4);
    for (auto [slot, klass] : {std::pair{&api.localized_damage_type, localized},
                               std::pair{&api.shooter_type, il2::find_class(kGame, "", "vp_FPSShooter")},
                               std::pair{&api.camera_type, il2::find_class(kGame, "", "vp_FPSCamera")}}) {
        *slot = il2::type_object(klass);
        if (*slot) il2::il2cpp_gchandle_new(*slot, false);
    }
    api.damage_scale = il2::find_method_params(localized, "GetDamageScale", {"WeaponSource"});
    api.bleed_minutes = il2::find_method_params(localized, "GetBleedOutMinutes", {"WeaponSource"});
    api.roll_kill = il2::find_method_params(localized, "RollChanceToKill", {"WeaponSource"});
    api.body_part = il2::field_offset(localized, "m_BodyPart");
    api.bullet_damage = il2::field_offset(bullet, "Damage");
    api.bullet_accuracy = il2::field_offset(bullet, "Accuracy");
    api.bullet_falloff = il2::field_offset(bullet, "DamageFalloffPerMeterBeyondEffectiveRange");
    api.bullet_min_falloff = il2::field_offset(bullet, "MinimumDamageFalloffBeyondEffectiveRange");
    api.bullet_gun = il2::field_offset(bullet, "m_GunType");
    api.arrow_type = il2::field_offset(arrow, "m_ArrowType");
    api.arrow_damage = il2::field_offset(arrow, "m_VictimDamage");
    api.arrow_bow = il2::field_offset(arrow, "m_BowDamageMultiplier");
    api.round_damage = il2::field_offset(round, "m_VictimDamage");
    api.gear_stone = il2::field_offset(gear, "m_StoneItem");
    api.gear_flare = il2::field_offset(gear, "m_FlareItem");
    api.gear_torch = il2::field_offset(gear, "m_TorchItem");
    api.gear_noise = il2::field_offset(gear, "m_NoiseMakerItem");
    api.flare_type = il2::field_offset(il2::find_class(kGame, "", "FlareItem"), "m_Type");
    api.skills = il2::find_method(game_manager, "GetSkillsManager", 0);
    api.add_skill = il2::find_method(il2::find_class(kGame, "", "SkillsManager"), "IncrementPointsAndNotify", 3);
    api.setup_damage_anim = il2::find_method(base_ai, "SetupDamageForAnim", 3);
    api.apply_damage = il2::find_method_params(base_ai, "ApplyDamage",
                                               {"System.Single", "System.Single", "DamageSource", "System.String"});
    api.react_to_hit = il2::find_method(base_ai, "MaybeFleeOrAttackFromProjectileHit", 1);
    api.flee_from = il2::find_method_params(base_ai, "FleeFrom", {"UnityEngine.Vector3"});
    api.stun = il2::find_method(base_ai, "Stun", 1);
    api.can_stun = il2::find_method(base_ai, "CurrentAIModeAllowStunFromThrownStone", 0);
    api.stun_chance = il2::find_method(base_ai, "GetChanceStunFromThrownStone", 0);
    if (Il2CppClass* manager = il2::find_class(kGame, "", "BaseAiManager"))
        api.ai_list = il2::il2cpp_class_get_field_from_name(manager, "m_BaseAis");
    api.ai_mode = il2::field_offset(base_ai, "m_CurrentMode");
    api.ai_ignore_crits = il2::field_offset(base_ai, "m_IgnoreCriticalHits");
    api.ai_flee_reason = il2::field_offset(base_ai, "m_FleeReason");
    api.chance_hit_stone = il2::field_offset(base_ai, "m_ChanceToFleeWhenHitByStone");
    api.chance_stone = il2::field_offset(base_ai, "m_ChanceToFleeFromThrownStone");
    api.chance_flare = il2::field_offset(base_ai, "m_ChanceToFleeFromThrownFlare");
    api.chance_torch = il2::field_offset(base_ai, "m_ChanceToFleeFromThrownTorch");
    api.chance_noise = il2::field_offset(base_ai, "m_ChanceToFleeFromThrownNoiseMaker");
    api.reach_stone = il2::field_offset(base_ai, "m_FleeFromStoneBounceDistance");
    api.reach_flare = il2::field_offset(base_ai, "m_FleeFromFlareBounceDistance");
    api.reach_torch = il2::field_offset(base_ai, "m_FleeFromTorchBounceDistance");
    Il2CppClass* camera = il2::find_class(kGame, "", "vp_FPSCamera");
    api.fire = il2::find_method(il2::find_class(kGame, "", "vp_FPSShooter"), "Fire", 0);
    api.camera_yaw = il2::field_offset(camera, "m_Yaw");
    api.camera_pitch = il2::field_offset(camera, "m_Pitch");
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.teleport = il2::find_method(il2::find_class(kGame, "", "PlayerManager"), "TeleportPlayer", 2);
    api.look_rotation = il2::find_method_params(il2::find_class(kCore, "UnityEngine", "Quaternion"), "LookRotation",
                                                {"UnityEngine.Vector3"});
    api.set_rotation = il2::find_method(transform, "set_rotation", 1);
    api.arrow_speed = il2::field_offset(arrow, "m_ReleaseVelocity");
    api.round_speed = il2::field_offset(round, "m_ReleaseVelocity");
    api.round_gravity = il2::field_offset(round, "m_AdditionalGravity");
    api.in_hands = il2::find_method(il2::find_class(kGame, "", "PlayerManager"), "get_m_ItemInHands", 0);
    api.stone_throw = il2::find_method(il2::find_class(kGame, "", "StoneItem"), "Throw", 0);
    api.spawn_and_fire = il2::find_method(round, "SpawnAndFire", 3);
    api.shooter_prefab = il2::field_offset(il2::find_class(kGame, "", "vp_FPSShooter"), "ProjectilePrefab");
    api.shooter_custom = il2::field_offset(il2::find_class(kGame, "", "vp_FPSShooter"), "ProjectileCustomPrefab");

    const MethodInfo* spawn_impact = il2::find_method(bullet, "SpawnImpactEffects", 1);
    const MethodInfo* arrow_damage = il2::find_method(arrow, "InflictDamage", 5);
    const MethodInfo* round_damage = il2::find_method(round, "InflictDamage", 4);
    const MethodInfo* thrown = il2::find_method(base_ai, "MaybeFleeFromThrownItem", 2);
    const MethodInfo* bullet_start = il2::find_method(bullet, "Start", 0);
    g_ready = api.component_transform && api.component_get && api.go_transform && api.get_position && api.find_child &&
              api.to_world && api.to_local && api.object_name && api.collider_by_id && api.localized_damage_type &&
              api.damage_scale && api.bleed_minutes && api.roll_kill && api.body_part >= 0 && api.bullet_damage >= 0 &&
              api.bullet_gun >= 0 && api.arrow_type >= 0 && api.round_damage >= 0 && api.setup_damage_anim &&
              api.apply_damage && api.react_to_hit && api.flee_from && api.ai_mode >= 0 && spawn_impact &&
              arrow_damage && round_damage && thrown;
    if (!g_ready) {
        logx::warn("wildlife hits: unavailable (TLD API missing); the other player's weapons won't reach the host's animals");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto* detour, auto** original) {
        return m && hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    Il2CppClass* input = il2::find_class(kGame, "", "InputManager");
    auto input_method = [input](const char* name) { return il2::find_method(input, name, 1); };
    const bool ok = hook("vp_Bullet.SpawnImpactEffects", spawn_impact, &spawn_impact_detour, &g_spawn_impact) &&
                    hook("ArrowItem.InflictDamage", arrow_damage, &arrow_damage_detour, &g_arrow_damage) &&
                    hook("FlareGunRoundItem.InflictDamage", round_damage, &round_damage_detour, &g_round_damage) &&
                    hook("BaseAi.MaybeFleeFromThrownItem", thrown, &thrown_detour, &g_thrown) &&
                    hook("vp_Bullet.Start", bullet_start, &bullet_start_detour, &g_bullet_start) &&
                    hook("ArrowItem.Fire", il2::find_method(arrow, "Fire", 1), &arrow_fire_detour, &g_arrow_fire) &&
                    hook("FlareGunRoundItem.Fire", il2::find_method(round, "Fire", 0), &round_fire_detour, &g_round_fire) &&
                    hook("InputManager.GetAltFire", input_method("GetAltFire"), &alt_fire_detour, &g_alt_fire) &&
                    hook("InputManager.GetAltFirePressed", input_method("GetAltFirePressed"), &alt_fire_pressed_detour,
                         &g_alt_fire_pressed) &&
                    hook("InputManager.GetAltFireReleased", input_method("GetAltFireReleased"), &alt_fire_released_detour,
                         &g_alt_fire_released) &&
                    hook("InputManager.GetFirePressed", input_method("GetFirePressed"), &fire_pressed_detour,
                         &g_fire_pressed) &&
                    hook("InputManager.GetFireReleased", input_method("GetFireReleased"), &fire_released_detour,
                         &g_fire_released);
    logx::info("wildlife hits: %s", ok ? "ready (the other player's weapons reach the host's animals)" : "PARTLY hooked");
    return ok;
}

void on_frame() {
    if (!g_ready) return;
    run_shot();
    if (!is_host()) return;
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::Hit, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        net::HitMsg h;
        if (net::read_hit(r, h)) apply_hit(h, p.from);
    }
    packets.clear();
    netrt::drain_packets(net::Msg::Thrown, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        net::ThrownMsg t;
        if (net::read_thrown(r, t)) apply_thrown(t, p.from);
    }
}

}  // namespace wildlife_hits
