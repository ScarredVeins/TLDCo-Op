#include "wildlife_carcass.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/harvests.h"
#include "net_runtime.h"
#include "wildlife_sync.h"

namespace wildlife_carcass {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr int32_t kAiModeDead = 2;
constexpr double kSendMs = 500;          // host: carcass states twice a second
constexpr float kDeathSeconds = 5.0f;    // BaseAi.DEATH_ANIMATION_DURATION: the copy shows the death until then
constexpr double kGoneMs = 3000;         // client: a carcass the host stops reporting is gone
constexpr double kQuietMs = 1000;        // client: the host's states stopped coming this long ago: the link stalls
constexpr double kSmallMs = 500;         // dead rabbits and ptarmigan are looked at twice a second
constexpr float kSmallDeadSeconds = 3.0f;  // after the death animation, they become items
constexpr double kUnits = 1e9;           // ItemWeight units per kg
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr size_t kPerPacket = (net::kMaxPacket - 8) / net::kCarcassSize;

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
    const MethodInfo* get_position = nullptr;
    const MethodInfo* get_rotation = nullptr;
    const MethodInfo* set_position = nullptr;
    const MethodInfo* set_rotation = nullptr;
    const MethodInfo* object_name = nullptr;
    const MethodInfo* alive = nullptr;          // static Object.op_Implicit
    const MethodInfo* destroy = nullptr;        // static Object.Destroy
    const MethodInfo* enabled = nullptr;        // Behaviour.get_enabled
    const MethodInfo* find_object = nullptr;    // static Object.FindObjectOfType(Type)
    const MethodInfo* go_component = nullptr;   // GameObject.GetComponent(Type)
    const MethodInfo* go_active = nullptr;      // GameObject.get_activeInHierarchy
    Il2CppObject* panel_type = nullptr;
    // Rabbits and ptarmigan (WildlifeItem)
    Il2CppObject* wildlife_item_type = nullptr;
    const MethodInfo* spawn_carcass = nullptr;  // WildlifeItem.SpawnCarcass(): the carcass item where the animal lies
    const MethodInfo* despawn = nullptr;        // BaseAi.Despawn()
    // BaseAi
    FieldInfo* ai_list = nullptr;               // static BaseAiManager.m_BaseAis
    ptrdiff_t ai_mode = -1, ai_body = -1, ai_side = -1, ai_dead_time = -1;
    // BodyHarvest
    ptrdiff_t meat = -1, hide = -1, gut = -1, condition = -1, frozen_pct = -1, frozen = -1, ravaged = -1;
    ptrdiff_t harvested = -1, bonus = -1, rolled_spawn = -1, allow_decay = -1, allow_freezing = -1, site_prefab = -1;
    const MethodInfo* round_meat = nullptr;     // MaybeRoundMeatAvailableToZero()
    const MethodInfo* carcass_mesh = nullptr;   // SetCarcassMeshEnabled(bool)
    const MethodInfo* frozen_shader = nullptr;  // UpdateFrozenShader()
    const MethodInfo* interact = nullptr;       // InteractWithBodyHarvest(bool): opens TLD's harvest panel
    const MethodInfo* bh_destroy = nullptr;     // BodyHarvestDestroy()
    // BodyHarvestManager / save data
    Il2CppClass* save_data_class = nullptr;
    const MethodInfo* save_data_ctor = nullptr;
    ptrdiff_t sd_position = -1, sd_rotation = -1, sd_prefab = -1;
    const MethodInfo* instantiate = nullptr;    // static InstantiateBodyHarvestGameObject(BodyHarvestSaveData)
    const MethodInfo* restore_corpse = nullptr; // static MaybeRestoreCorpseState(BodyHarvest)
    FieldInfo* bh_list = nullptr;               // static BodyHarvestManager.m_BodyHarvestList
    const MethodInfo* list_remove = nullptr;    // List<BodyHarvest>.Remove (found on first use)
    // Panel_BodyHarvest
    ptrdiff_t panel_bh = -1, item_meat = -1, item_hide = -1, item_gut = -1;
    const MethodInfo* set_amount = nullptr;     // Panel_BodyHarvest_MenuItem.set_HarvestAmount(ItemWeight)
    const MethodInfo* set_units = nullptr;      // Panel_BodyHarvest_MenuItem.set_HarvestUnits(int)
    const MethodInfo* harvest_done = nullptr;   // Panel_BodyHarvest.HarvestSuccessful()
    const MethodInfo* quarter_done = nullptr;   // Panel_BodyHarvest.QuarterSuccessful()
    const MethodInfo* exit_panel = nullptr;     // Panel_BodyHarvest.ExitBodyHarvestPanel()
    const MethodInfo* panel_enable = nullptr;   // Panel_BodyHarvest.Enable(bool, BodyHarvest, bool, ComingFromScreenCategory)
    const MethodInfo* load_panel = nullptr;     // static InterfaceManager.LoadPanel(Type)
} api;

bool g_ready = false;

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}
bool joined() {
    const auto role = netrt::role();
    return role && *role == net::Role::Client;
}

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
void put(Il2CppObject* o, ptrdiff_t off, T v) {
    if (o && off >= 0) memcpy(reinterpret_cast<char*>(o) + off, &v, sizeof(T));
}

Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
bool alive(Il2CppObject* o) {
    if (!o) return false;
    bool yes = false;
    void* args[] = {o};
    return il2::invoke_value(api.alive, nullptr, yes, args) && yes;
}
bool behaviour_enabled(Il2CppObject* b) {
    bool on = false;
    return b && il2::invoke_value(api.enabled, b, on) && on;
}
std::string name_of(Il2CppObject* o) {
    return o ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, o))) : std::string();
}

float distance(const Vector3& a, const Vector3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::vector<Il2CppObject*> all_animals() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = nullptr;
    if (api.ai_list) il2::il2cpp_field_static_get_value(api.ai_list, &list);
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* o = il2::array_object_at(items, static_cast<size_t>(i))) out.push_back(o);
    return out;
}

// A carcass as both games describe it.
struct Amounts {
    int64_t meat = 0;
    int32_t hide = 0, gut = 0;
    float condition = 0, percent_frozen = 0;
    uint8_t flags = 0;
};

Amounts read_amounts(Il2CppObject* bh) {
    Amounts a;
    a.meat = get<int64_t>(bh, api.meat);
    a.hide = get<int32_t>(bh, api.hide);
    a.gut = get<int32_t>(bh, api.gut);
    a.condition = get<float>(bh, api.condition);
    a.percent_frozen = get<float>(bh, api.frozen_pct);
    a.flags = static_cast<uint8_t>((get<bool>(bh, api.frozen) ? net::kCarcassFrozen : 0) |
                                   (get<bool>(bh, api.ravaged) ? net::kCarcassRavaged : 0) |
                                   (get<bool>(bh, api.harvested) ? net::kCarcassHarvested : 0) |
                                   (get<bool>(bh, api.bonus) ? net::kCarcassBonusTaken : 0));
    return a;
}

// What TLD calls after changing a carcass so its looks follow (round tiny meat to 0, the butchered mesh, frost).
void refresh(Il2CppObject* bh) {
    il2::invoke(api.round_meat, bh);
    bool butchered = get<bool>(bh, api.harvested) || get<bool>(bh, api.ravaged);
    void* args[] = {&butchered};
    il2::invoke(api.carcass_mesh, bh, args);
    il2::invoke(api.frozen_shader, bh);
}

void write_amounts(Il2CppObject* bh, const Amounts& a) {
    put<int64_t>(bh, api.meat, a.meat);
    put<int32_t>(bh, api.hide, a.hide);
    put<int32_t>(bh, api.gut, a.gut);
    put<float>(bh, api.condition, a.condition);
    put<float>(bh, api.frozen_pct, a.percent_frozen);
    put<bool>(bh, api.frozen, (a.flags & net::kCarcassFrozen) != 0);
    put<bool>(bh, api.ravaged, (a.flags & net::kCarcassRavaged) != 0);
    put<bool>(bh, api.harvested, (a.flags & net::kCarcassHarvested) != 0);
    put<bool>(bh, api.bonus, (a.flags & net::kCarcassBonusTaken) != 0);
    refresh(bh);
}

std::string describe_amounts(const Amounts& a) {
    char text[160];
    snprintf(text, sizeof(text), "%.2f kg meat, %d hide, %d gut, condition %.0f%%%s%s%s", a.meat / kUnits, a.hide, a.gut,
             a.condition, (a.flags & net::kCarcassFrozen) ? ", frozen" : "", (a.flags & net::kCarcassRavaged) ? ", ravaged" : "",
             (a.flags & net::kCarcassHarvested) ? ", harvested" : "");
    return text;
}

// --- small game: rabbits and ptarmigan become items -----------------------------------------------------------------
// A dead rabbit or ptarmigan is an animal (BaseAi with a WildlifeItem) until a player picks it up; then it's a carcass
// item (WildlifeItemInteraction: WildlifeItem.SpawnCarcass, the pickup, BaseAi.Despawn). The game that runs the scene
// (the host's, or a friend's the host isn't in) does that much once it lies still, without the pickup: the carcass lies
// where the animal did, an item both games share (world_items), and arrows in it drop into the world (SpawnCarcass).
// So both players see one rabbit, and either may pick it up. Its BodyHarvest goes with it (BaseAi.SpawnCarcassPickup).

std::unordered_map<Il2CppObject*, Vector3> g_small_seen;  // dead ones at the last look, and where
double g_last_small = 0;

Il2CppObject* wildlife_item_of(Il2CppObject* ai) {
    Il2CppObject* go = api.wildlife_item_type ? il2::invoke(api.component_go, ai) : nullptr;
    void* args[] = {api.wildlife_item_type};
    Il2CppObject* wi = go ? il2::invoke(api.go_component, go, args) : nullptr;
    return alive(wi) ? wi : nullptr;
}

// False for an animal TLD has put away: BaseAi.Despawn only deactivates it, for its pool (it stays in BaseAiManager's
// list, still dead, until it's spawned again).
bool in_world(Il2CppObject* ai) {
    Il2CppObject* go = il2::invoke(api.component_go, ai);
    bool active = false;
    return go && il2::invoke_value(api.go_active, go, active) && active;
}

bool runs_scene() {
    if (hosting()) return true;
    const auto host = netrt::remote_state();
    return joined() && !wildlife_sync::mirroring() && !(host && host->valid && host->scene == game::active_scene());
}

void small_game_to_items(double now) {
    if (now - g_last_small < kSmallMs || !api.spawn_carcass || !api.despawn || !api.go_active) return;
    g_last_small = now;
    std::unordered_map<Il2CppObject*, Vector3> seen;
    for (Il2CppObject* ai : all_animals()) {
        if (get<int32_t>(ai, api.ai_mode) != kAiModeDead || get<float>(ai, api.ai_dead_time) < kSmallDeadSeconds) continue;
        Il2CppObject* wi = wildlife_item_of(ai);
        Il2CppObject* bh = get<Il2CppObject*>(ai, api.ai_body);
        if (!wi || !bh || !behaviour_enabled(bh) || !in_world(ai)) continue;  // (TLD's condition for picking one up)
        Vector3 pos{};
        il2::invoke_value(api.get_position, il2::invoke(api.component_transform, ai), pos);
        seen[ai] = pos;
        const auto before = g_small_seen.find(ai);
        if (before == g_small_seen.end() || distance(before->second, pos) > 0.01f) continue;  // still falling (a bird)
        const std::string animal = name_of(il2::invoke(api.component_go, ai));
        Il2CppObject* gi = il2::invoke(api.spawn_carcass, wi);
        if (!gi) {
            logx::warn("wildlife: TLD made no carcass item of the dead %s; it stays an animal", animal.c_str());
            continue;
        }
        il2::invoke(api.despawn, ai);
        seen.erase(ai);
        logx::info("wildlife: the dead %s at (%.1f, %.1f, %.1f) is a %s item now (as picking it up makes it), for every "
                   "player", animal.c_str(), pos.x, pos.y, pos.z, name_of(il2::invoke(api.component_go, gi)).c_str());
    }
    g_small_seen.swap(seen);
}

// --- host --------------------------------------------------------------------------------------------------------

double g_last_send = 0;
// Animal id -> the newest Harvest applied, per friend (each counts its own).
std::array<std::unordered_map<uint32_t, uint32_t>, net::kMaxPlayers> g_acks;
std::array<uint32_t, net::kMaxPlayers> g_ack_link{};  // which connection of theirs g_acks counts (a new one starts over)
net::HarvestLedger g_counted;  // the friends' harvests counted (one may come again after the link drops and is back)

std::unordered_map<uint32_t, uint32_t>& acks_of(uint8_t id, uint32_t link) {
    if (g_ack_link[id] != link) {
        g_acks[id].clear();
        g_ack_link[id] = link;
    }
    return g_acks[id];
}

void send_carcasses(double now) {
    if (now - g_last_send < kSendMs) return;
    g_last_send = now;
    std::vector<net::CarcassRec> list;
    for (Il2CppObject* ai : all_animals()) {
        if (get<int32_t>(ai, api.ai_mode) != kAiModeDead) continue;
        Il2CppObject* bh = get<Il2CppObject*>(ai, api.ai_body);
        const uint32_t id = wildlife_sync::host_animal_id(ai);
        if (!id || !bh || !behaviour_enabled(bh)) continue;
        if (wildlife_item_of(ai)) continue;  // a rabbit or ptarmigan: an item soon (small_game_to_items)
        Il2CppObject* tf = il2::invoke(api.component_transform, ai);
        Vector3 pos{};
        Quaternion rot{};
        il2::invoke_value(api.get_position, tf, pos);
        il2::invoke_value(api.get_rotation, tf, rot);
        const Amounts a = read_amounts(bh);
        net::CarcassRec c;
        c.animal = id;
        c.prefab = name_of(il2::invoke(api.component_go, ai));
        c.pos[0] = pos.x, c.pos[1] = pos.y, c.pos[2] = pos.z;
        c.rot[0] = rot.x, c.rot[1] = rot.y, c.rot[2] = rot.z, c.rot[3] = rot.w;
        c.damage_side = static_cast<int8_t>(get<int32_t>(ai, api.ai_side));
        c.meat_units = a.meat;
        c.hide = a.hide;
        c.gut = a.gut;
        c.condition = a.condition;
        c.percent_frozen = a.percent_frozen;
        c.flags = a.flags;
        c.seconds_dead = get<float>(ai, api.ai_dead_time);
        list.push_back(std::move(c));
    }
    // Each friend's list says which of its own harvests are counted. A friend back from a dropped link gets it once
    // what it did meanwhile is in (caught up): before, our list would bring back a carcass it quartered offline.
    for (const net::PlayerInfo& p : netrt::status().players) {
        if (!p.linked || !p.caught_up || p.id == 0 || p.id >= net::kMaxPlayers) continue;
        auto& acks = acks_of(p.id, p.link);
        for (net::CarcassRec& c : list) {
            const auto ack = acks.find(c.animal);
            c.harvest_ack = ack == acks.end() ? 0 : ack->second;
        }
        for (size_t at = 0; at < list.size(); at += kPerPacket) {
            const std::vector<net::CarcassRec> part(list.begin() + static_cast<ptrdiff_t>(at),
                                                    list.begin() + static_cast<ptrdiff_t>(std::min(list.size(), at + kPerPacket)));
            net::Writer w(net::Msg::Carcasses);
            net::write_carcasses(w, part);
            netrt::send_packet_to(p.id, w, false);
        }
    }
}

Il2CppObject* host_carcass(uint32_t id) {
    Il2CppObject* ai = wildlife_sync::host_animal(id);
    if (!ai || get<int32_t>(ai, api.ai_mode) != kAiModeDead) return nullptr;
    return get<Il2CppObject*>(ai, api.ai_body);
}

// Friend `from` took meat / hide / guts at their copy (and TLD gave them the items there): the same comes off ours.
void apply_harvest(const net::HarvestMsg& h, uint8_t from, uint32_t link) {
    Il2CppObject* bh = host_carcass(h.animal);
    if (!bh) {
        logx::info("wildlife: the other player harvested #%u, which isn't a carcass here any more", h.animal);
        return;
    }
    uint32_t& ack = acks_of(from < net::kMaxPlayers ? from : 0, link)[h.animal];
    if (static_cast<int32_t>(h.seq - ack) > 0 || !ack) ack = h.seq;
    if (!g_counted.first_time(h.seq)) {
        logx::info("wildlife: the other player's harvest #%u of #%u came again (sent before the link dropped); "
                   "counted already", h.seq, h.animal);
        return;
    }
    Amounts a = read_amounts(bh);
    const Amounts before = a;
    a.meat = a.meat > h.meat_units ? a.meat - h.meat_units : 0;
    a.hide = a.hide > h.hide ? a.hide - h.hide : 0;
    a.gut = a.gut > h.gut ? a.gut - h.gut : 0;
    if (h.flags & net::kHarvestSetHarvested) a.flags |= net::kCarcassHarvested;
    if (h.flags & net::kHarvestBonusTaken) a.flags |= net::kCarcassBonusTaken;
    write_amounts(bh, a);
    logx::info("wildlife: the other player took %.2f kg meat, %d hide, %d gut from #%u: %s -> %s", h.meat_units / kUnits,
               h.hide, h.gut, h.animal, describe_amounts(before).c_str(), describe_amounts(read_amounts(bh)).c_str());
}

void apply_quartered(uint32_t id) {
    Il2CppObject* bh = host_carcass(id);
    logx::info("wildlife: the other player quartered #%u; %s", id, bh ? "its carcass goes here too" : "it was gone here already");
    if (bh) il2::invoke(api.bh_destroy, bh);
    for (auto& acks : g_acks) acks.erase(id);
}

// --- client ------------------------------------------------------------------------------------------------------

struct Local {
    uintptr_t bh = 0, go = 0, ai = 0;
    net::CarcassRec host;      // the host's newest state
    double seen_ms = 0;
    bool placed = false;       // put on the host's pose once dead (the first Update kills it: ForceToCorpse)
    Amounts applied;           // what we last wrote
    bool have_applied = false;
    uint32_t pending = 0;      // our newest Harvest seq the host hasn't counted yet
};
std::unordered_map<uint32_t, Local> g_locals;
uint32_t g_harvest_seq = 0;  // our newest harvest's number (net::first_harvest_seq at init)
uint64_t g_states_seen = 0;  // the host's states received, when we last looked
double g_heard_ms = -1e18;   // when they last went up
bool g_holding = false;      // hold_locals: the link stalls
std::unordered_set<uint32_t> g_quartered;  // the host's animals we quartered: never a carcass here again

void remove_from_save(Il2CppObject* bh) {
    Il2CppObject* list = nullptr;
    if (api.bh_list) il2::il2cpp_field_static_get_value(api.bh_list, &list);
    if (!list) return;
    if (!api.list_remove) api.list_remove = il2::find_method(il2::il2cpp_object_get_class(list), "Remove", 1);
    void* args[] = {bh};
    il2::invoke(api.list_remove, list, args);
}

// TLD's own way back from a save: the prefab by name at the host's pose, killed on its first frame with no death
// animation (MaybeRestoreCorpseState: ForceToCorpse), then our numbers.
bool make_local(const net::CarcassRec& c, double now) {
    Il2CppObject* data = il2::il2cpp_object_new(api.save_data_class);
    if (!data) return false;
    il2::invoke(api.save_data_ctor, data);
    const Vector3 pos{c.pos[0], c.pos[1], c.pos[2]};
    const Quaternion rot{c.rot[0], c.rot[1], c.rot[2], c.rot[3]};
    put<Vector3>(data, api.sd_position, pos);
    put<Quaternion>(data, api.sd_rotation, rot);
    Il2CppString* prefab = il2::il2cpp_string_new(c.prefab.c_str());
    put<Il2CppString*>(data, api.sd_prefab, prefab);
    void* args[] = {data};
    Il2CppObject* bh = il2::invoke(api.instantiate, nullptr, args);
    if (!bh) {
        logx::warn("wildlife: the host's #%u died, but TLD couldn't make a '%s' carcass here", c.animal, c.prefab.c_str());
        return false;
    }
    remove_from_save(bh);                    // the host's carcass, not ours: never in our save
    put<bool>(bh, api.rolled_spawn, true);   // it exists, no spawn-chance roll
    put<Il2CppObject*>(bh, api.site_prefab, nullptr);  // no carrion-bird site of ours (the host's shows its own)
    Il2CppObject* go = il2::invoke(api.component_go, bh);
    Il2CppObject* ai = nullptr;
    for (Il2CppObject* candidate : all_animals())
        if (il2::invoke(api.component_go, candidate) == go) ai = candidate;
    if (ai) put<int32_t>(ai, api.ai_side, c.damage_side);  // falls to the same side
    void* restore_args[] = {bh};
    il2::invoke(api.restore_corpse, nullptr, restore_args);
    Local& l = g_locals[c.animal];
    l = Local{};
    l.bh = il2::il2cpp_gchandle_new(bh, false);
    l.go = il2::il2cpp_gchandle_new(go, false);
    l.ai = ai ? il2::il2cpp_gchandle_new(ai, false) : 0;
    l.host = c;
    l.seen_ms = now;
    wildlife_sync::carcass_made(c.animal, go);
    logx::info("wildlife: the host's #%u is a carcass here now ('%s', %s)", c.animal, c.prefab.c_str(),
               describe_amounts(Amounts{c.meat_units, c.hide, c.gut, c.condition, c.percent_frozen, c.flags}).c_str());
    return true;
}

void drop_local(uint32_t id, Local& l, const char* why, bool destroy_it) {
    Il2CppObject* go = target(l.go);
    if (destroy_it && go && alive(go)) {
        void* args[] = {go};
        il2::invoke(api.destroy, nullptr, args);
    }
    wildlife_sync::carcass_gone(id, go);
    for (uintptr_t* h : {&l.bh, &l.go, &l.ai})
        if (*h) il2::il2cpp_gchandle_free(*h);
    logx::info("wildlife: our carcass of the host's #%u is gone (%s)", id, why);
}

void clear_locals(const char* why) {
    for (auto& [id, l] : g_locals) drop_local(id, l, why, true);
    g_locals.clear();
}

void receive_carcasses(double now) {
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::Carcasses, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        std::vector<net::CarcassRec> list;
        if (!net::read_carcasses(r, list)) continue;
        for (net::CarcassRec& c : list) {
            auto it = g_locals.find(c.animal);
            if (it == g_locals.end()) {
                if (c.seconds_dead >= kDeathSeconds && !g_quartered.count(c.animal)) make_local(c, now);
                continue;
            }
            it->second.host = std::move(c);
            it->second.seen_ms = now;
        }
    }
}

// The link to the host: its states (many a second) still come; they stopped (the link stalls before it times out, or
// it dropped and we play on: netrt::offline); or neither. A carcass the host stops listing is gone only while we hear
// the host: a link that stalls or drops keeps ours.
enum class HostLink { Hearing, Stalled, None };
HostLink host_link(double now) {
    const net::Status st = netrt::status();
    if (st.states_received != g_states_seen) {
        g_states_seen = st.states_received;
        g_heard_ms = now;
    }
    if (st.connected && now - g_heard_ms < kQuietMs) return HostLink::Hearing;
    return st.connected || netrt::offline() ? HostLink::Stalled : HostLink::None;
}

// The link is down and we play on (netrt::offline): our carcasses stay as they are, to harvest or quarter. What we
// take goes to the host once it's back (the session keeps it); then they follow the host's again.
void hold_locals(double now) {
    for (auto it = g_locals.begin(); it != g_locals.end();) {
        Il2CppObject* bh = target(it->second.bh);
        if (!bh || !alive(bh)) {  // quartered here, or gone with the scene
            drop_local(it->first, it->second, "no longer here", false);
            it = g_locals.erase(it);
            continue;
        }
        it->second.seen_ms = now;
        ++it;
    }
}

void update_locals(double now) {
    for (auto it = g_locals.begin(); it != g_locals.end();) {
        Local& l = it->second;
        Il2CppObject* bh = target(l.bh);
        if (!bh || !alive(bh)) {  // quartered here (TLD destroyed it), or gone with the scene
            drop_local(it->first, l, "no longer here", false);
            it = g_locals.erase(it);
            continue;
        }
        if (now - l.seen_ms > kGoneMs) {
            drop_local(it->first, l, "the host's is gone", true);
            it = g_locals.erase(it);
            continue;
        }
        Il2CppObject* ai = target(l.ai);
        if (!l.placed && (!ai || get<int32_t>(ai, api.ai_mode) == kAiModeDead)) {
            // Dead now (its first Update): on the host's pose, the host's numbers, and decay left to the host.
            Il2CppObject* tf = il2::invoke(api.component_transform, bh);
            Vector3 pos{l.host.pos[0], l.host.pos[1], l.host.pos[2]};
            Quaternion rot{l.host.rot[0], l.host.rot[1], l.host.rot[2], l.host.rot[3]};
            void* pos_args[] = {&pos};
            void* rot_args[] = {&rot};
            il2::invoke(api.set_position, tf, pos_args);
            il2::invoke(api.set_rotation, tf, rot_args);
            put<bool>(bh, api.allow_decay, false);
            put<bool>(bh, api.allow_freezing, false);
            l.placed = true;
        }
        if (l.placed && l.pending && net::harvest_counted(l.host.harvest_ack, l.pending)) l.pending = 0;
        if (l.placed && !l.pending) {
            const Amounts host{l.host.meat_units, l.host.hide, l.host.gut, l.host.condition, l.host.percent_frozen,
                               l.host.flags};
            const bool changed = !l.have_applied || host.meat != l.applied.meat || host.hide != l.applied.hide ||
                                 host.gut != l.applied.gut || host.flags != l.applied.flags ||
                                 std::fabs(host.condition - l.applied.condition) > 0.5f ||
                                 std::fabs(host.percent_frozen - l.applied.percent_frozen) > 0.5f;
            if (changed) {
                write_amounts(bh, host);
                if (l.have_applied && (host.meat != l.applied.meat || host.hide != l.applied.hide || host.gut != l.applied.gut))
                    logx::info("wildlife: the host's #%u carcass changed: %s", it->first, describe_amounts(host).c_str());
                l.applied = host;
                l.have_applied = true;
            }
        }
        ++it;
    }
}

uint32_t local_id_of(Il2CppObject* bh) {
    for (const auto& [id, l] : g_locals)
        if (target(l.bh) == bh) return id;
    return 0;
}

// --- harvest hooks (the taker's game) -----------------------------------------------------------------------------

using PanelFn = void (*)(Il2CppObject* self, const MethodInfo* method);
PanelFn g_harvest_done = nullptr;
PanelFn g_quarter_done = nullptr;

void harvest_done_detour(Il2CppObject* self, const MethodInfo* method) {
    Il2CppObject* bh = g_ready ? get<Il2CppObject*>(self, api.panel_bh) : nullptr;
    const uint32_t id = bh && joined() ? local_id_of(bh) : 0;
    const Amounts before = id ? read_amounts(bh) : Amounts{};
    g_harvest_done(self, method);
    if (!id) return;
    const Amounts after = read_amounts(bh);
    net::HarvestMsg h;
    h.animal = id;
    h.seq = g_harvest_seq = net::next_harvest_seq(g_harvest_seq);
    h.meat_units = before.meat - after.meat;  // includes TLD's bare-handed loss: gone for both
    h.hide = before.hide - after.hide;
    h.gut = before.gut - after.gut;
    if ((after.flags & net::kCarcassHarvested) && !(before.flags & net::kCarcassHarvested)) h.flags |= net::kHarvestSetHarvested;
    if ((after.flags & net::kCarcassBonusTaken) && !(before.flags & net::kCarcassBonusTaken)) h.flags |= net::kHarvestBonusTaken;
    net::Writer w(net::Msg::Harvest);
    net::write_harvest(w, h);
    netrt::send_packet(w, true);
    Local& l = g_locals[id];
    l.pending = h.seq;
    l.applied = after;
    logx::info("wildlife: we harvested %.2f kg meat, %d hide, %d gut from the host's #%u (%s left); sent to the host",
               h.meat_units / kUnits, h.hide, h.gut, id, describe_amounts(after).c_str());
}

void quarter_done_detour(Il2CppObject* self, const MethodInfo* method) {
    Il2CppObject* bh = g_ready ? get<Il2CppObject*>(self, api.panel_bh) : nullptr;
    const uint32_t id = bh && joined() ? local_id_of(bh) : 0;
    g_quarter_done(self, method);
    if (!id) return;
    g_quartered.insert(id);
    net::Writer w(net::Msg::Quartered);
    w.put(id);
    netrt::send_packet(w, true);
    logx::info("wildlife: we quartered the host's #%u; sent to the host", id);
}

// --- test helpers -------------------------------------------------------------------------------------------------

// The nearest carcass to us: on the host a dead animal's BodyHarvest, on the client one of ours (copies of the host's).
Il2CppObject* nearest_carcass(uint32_t& id) {
    const game::PlayerState p = game::read_player();
    Il2CppObject* best = nullptr;
    float best_d = 0;
    auto consider = [&](Il2CppObject* bh, uint32_t animal) {
        Vector3 at{};
        if (!il2::invoke_value(api.get_position, il2::invoke(api.component_transform, bh), at)) return;
        const float dx = at.x - p.position.x, dy = at.y - p.position.y, dz = at.z - p.position.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!best || d < best_d) best = bh, best_d = d, id = animal;
    };
    if (joined()) {
        for (const auto& [animal, l] : g_locals)
            if (Il2CppObject* bh = target(l.bh); bh && l.placed) consider(bh, animal);
    } else {
        for (Il2CppObject* ai : all_animals()) {
            Il2CppObject* bh = get<Il2CppObject*>(ai, api.ai_body);
            if (get<int32_t>(ai, api.ai_mode) == kAiModeDead && behaviour_enabled(bh)) consider(bh, wildlife_sync::host_animal_id(ai));
        }
    }
    return best;
}

// A test harvest / quarter: TLD's panel opens on the carcass over the next frames (InteractWithBodyHarvest, as when
// the player presses interact on it); then the amounts the player would dial in, and the harvest finishing.
struct Test {
    bool active = false, quarter = false;
    uintptr_t bh = 0;
    uint32_t id = 0;
    float meat_kg = 0;
    int hide = 0, gut = 0;
    double started_ms = 0;
    uintptr_t panel = 0;
} g_test;

void start_test(Il2CppObject* bh, uint32_t id, bool quarter, float meat_kg, int hide, int gut) {
    if (g_test.bh) il2::il2cpp_gchandle_free(g_test.bh);
    g_test = {true, quarter, il2::il2cpp_gchandle_new(bh, false), id, meat_kg, hide, gut, now_ms()};
    // InteractWithBodyHarvest opens the panel only if TLD has it loaded already (PanelReference.TryGetPanel); load it
    // the way TLD loads panels (InterfaceManager.LoadPanel), then what InteractWithBodyHarvest does with it.
    void* load_args[] = {api.panel_type};
    Il2CppObject* panel = il2::invoke(api.load_panel, nullptr, load_args);
    put<bool>(bh, api.allow_decay, get<bool>(bh, api.allow_decay));  // (InteractWithBodyHarvest sets decay; we don't)
    if (panel) {
        bool enable = true, book_anim = false;
        int32_t coming_from = 0;
        void* args[] = {&enable, bh, &book_anim, &coming_from};
        il2::invoke(api.panel_enable, panel, args);
        g_test.panel = il2::il2cpp_gchandle_new(panel, false);
        logx::info("dev: harvest panel loaded; it %s this carcass", get<Il2CppObject*>(panel, api.panel_bh) == bh ? "has" : "does NOT have");
    } else {
        logx::info("dev: harvest panel didn't load; asking the carcass to open it");
        bool book_anim = false;
        void* args[] = {&book_anim};
        il2::invoke(api.interact, bh, args);
    }
}

void run_test(double now) {
    if (!g_test.active) return;
    Il2CppObject* bh = target(g_test.bh);
    void* find_args[] = {api.panel_type};
    Il2CppObject* panel = g_test.panel ? target(g_test.panel) : bh ? il2::invoke(api.find_object, nullptr, find_args) : nullptr;
    if (!panel || get<Il2CppObject*>(panel, api.panel_bh) != bh) {
        if (bh && now - g_test.started_ms < 3000) return;
        logx::warn("dev: %s: TLD's harvest panel didn't open on #%u", g_test.quarter ? "quarter" : "harvest", g_test.id);
    } else if (g_test.quarter) {
        il2::invoke(api.quarter_done, panel);
        logx::info("dev: quarter: #%u quartered", g_test.id);
    } else {
        int64_t units = static_cast<int64_t>(std::llround(g_test.meat_kg * 10000.0)) * 100000;
        int32_t h = g_test.hide, g = g_test.gut;
        void* meat_args[] = {&units};
        void* hide_args[] = {&h};
        void* gut_args[] = {&g};
        il2::invoke(api.set_amount, get<Il2CppObject*>(panel, api.item_meat), meat_args);
        il2::invoke(api.set_units, get<Il2CppObject*>(panel, api.item_hide), hide_args);
        il2::invoke(api.set_units, get<Il2CppObject*>(panel, api.item_gut), gut_args);
        const Amounts before = read_amounts(bh);
        il2::invoke(api.harvest_done, panel);
        il2::invoke(api.exit_panel, panel);
        logx::info("dev: harvest: #%u %s -> %s", g_test.id, describe_amounts(before).c_str(),
                   describe_amounts(read_amounts(bh)).c_str());
    }
    il2::il2cpp_gchandle_free(g_test.bh);
    if (g_test.panel) il2::il2cpp_gchandle_free(g_test.panel);
    g_test = {};
}

}  // namespace

bool harvest(float meat_kg, int hide, int gut) {
    uint32_t id = 0;
    Il2CppObject* bh = nearest_carcass(id);
    if (!bh) {
        logx::warn("dev: harvest: no carcass here");
        return false;
    }
    start_test(bh, id, false, meat_kg, hide, gut);
    return true;
}

bool quarter() {
    uint32_t id = 0;
    Il2CppObject* bh = nearest_carcass(id);
    if (!bh) {
        logx::warn("dev: quarter: no carcass here");
        return false;
    }
    start_test(bh, id, true, 0, 0, 0);
    return true;
}

void describe() {
    auto where = [](Il2CppObject* bh) {
        Vector3 at{};
        il2::invoke_value(api.get_position, il2::invoke(api.component_transform, bh), at);
        char text[64];
        snprintf(text, sizeof(text), "at (%.1f, %.1f, %.1f)", at.x, at.y, at.z);
        return std::string(text);
    };
    if (joined()) {
        logx::info("dev: carcasses: %zu copies of the host's", g_locals.size());
        for (const auto& [id, l] : g_locals)
            if (Il2CppObject* bh = target(l.bh))
                logx::info("dev:   #%u %s, %s%s", id, where(bh).c_str(), describe_amounts(read_amounts(bh)).c_str(),
                           l.pending ? " (harvest not counted yet)" : "");
        return;
    }
    logx::info("dev: carcasses:");
    for (Il2CppObject* ai : all_animals()) {
        Il2CppObject* bh = get<Il2CppObject*>(ai, api.ai_body);
        if (get<int32_t>(ai, api.ai_mode) == kAiModeDead && behaviour_enabled(bh))
            logx::info("dev:   #%u %s, %s, dead %.0f s", wildlife_sync::host_animal_id(ai), where(bh).c_str(),
                       describe_amounts(read_amounts(bh)).c_str(), get<float>(ai, api.ai_dead_time));
    }
}

bool init() {
    LARGE_INTEGER clock;
    QueryPerformanceCounter(&clock);
    g_harvest_seq = net::first_harvest_seq(static_cast<uint64_t>(clock.QuadPart) ^ (GetTickCount64() << 20) ^
                                           GetCurrentProcessId());
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* base_ai = il2::find_class(kGame, "", "BaseAi");
    Il2CppClass* body = il2::find_class(kGame, "", "BodyHarvest");
    Il2CppClass* manager = il2::find_class(kGame, "", "BodyHarvestManager");
    Il2CppClass* panel = il2::find_class(kGame, "", "Panel_BodyHarvest");
    Il2CppClass* item = il2::find_class(kGame, "", "Panel_BodyHarvest_MenuItem");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.get_rotation = il2::find_method(transform, "get_rotation", 0);
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.set_rotation = il2::find_method(transform, "set_rotation", 1);
    api.object_name = il2::find_method(object, "get_name", 0);
    api.alive = il2::find_method(object, "op_Implicit", 1);
    api.destroy = il2::find_method(object, "Destroy", 1);
    api.find_object = il2::find_method_params(object, "FindObjectOfType", {"System.Type"});
    api.go_component =
        il2::find_method_params(il2::find_class(kCore, "UnityEngine", "GameObject"), "GetComponent", {"System.Type"});
    api.go_active = il2::find_method(il2::find_class(kCore, "UnityEngine", "GameObject"), "get_activeInHierarchy", 0);
    if (Il2CppClass* wildlife_item = il2::find_class(kGame, "", "WildlifeItem")) {
        api.wildlife_item_type = il2::type_object(wildlife_item);
        if (api.wildlife_item_type) il2::il2cpp_gchandle_new(api.wildlife_item_type, false);
        api.spawn_carcass = il2::find_method(wildlife_item, "SpawnCarcass", 0);
    }
    api.despawn = il2::find_method(base_ai, "Despawn", 0);
    api.enabled = il2::find_method(il2::find_class(kCore, "UnityEngine", "Behaviour"), "get_enabled", 0);
    api.panel_type = il2::type_object(panel);
    if (api.panel_type) il2::il2cpp_gchandle_new(api.panel_type, false);
    if (Il2CppClass* ai_manager = il2::find_class(kGame, "", "BaseAiManager"))
        api.ai_list = il2::il2cpp_class_get_field_from_name(ai_manager, "m_BaseAis");
    api.ai_mode = il2::field_offset(base_ai, "m_CurrentMode");
    api.ai_body = il2::field_offset(base_ai, "m_BodyHarvest");
    api.ai_side = il2::field_offset(base_ai, "m_LastDamageSide");
    api.ai_dead_time = il2::field_offset(base_ai, "m_TimeInDeadMode");
    api.meat = il2::field_offset(body, "m_MeatAvailableKG");
    api.hide = il2::field_offset(body, "m_HideAvailableUnits");
    api.gut = il2::field_offset(body, "m_GutAvailableUnits");
    api.condition = il2::field_offset(body, "m_Condition");
    api.frozen_pct = il2::field_offset(body, "m_PercentFrozen");
    api.frozen = il2::field_offset(body, "m_Frozen");
    api.ravaged = il2::field_offset(body, "m_Ravaged");
    api.harvested = il2::field_offset(body, "m_HasHarvested");
    api.bonus = il2::field_offset(body, "m_HaveBonusItemsBeenHarvested");
    api.rolled_spawn = il2::field_offset(body, "m_RolledSpawnChance");
    api.allow_decay = il2::field_offset(body, "m_AllowDecay");
    api.allow_freezing = il2::field_offset(body, "m_AllowFreezing");
    api.site_prefab = il2::field_offset(body, "m_CarcassSitePrefab");
    api.round_meat = il2::find_method(body, "MaybeRoundMeatAvailableToZero", 0);
    api.carcass_mesh = il2::find_method(body, "SetCarcassMeshEnabled", 1);
    api.frozen_shader = il2::find_method(body, "UpdateFrozenShader", 0);
    api.interact = il2::find_method(body, "InteractWithBodyHarvest", 1);
    api.bh_destroy = il2::find_method(body, "BodyHarvestDestroy", 0);
    api.save_data_class = il2::find_class(kGame, "", "BodyHarvestSaveData");
    api.save_data_ctor = il2::find_method(api.save_data_class, ".ctor", 0);
    api.sd_position = il2::field_offset(api.save_data_class, "m_Position");
    api.sd_rotation = il2::field_offset(api.save_data_class, "m_Rotation");
    api.sd_prefab = il2::field_offset(api.save_data_class, "m_PrefabName");
    api.instantiate = il2::find_method(manager, "InstantiateBodyHarvestGameObject", 1);
    api.restore_corpse = il2::find_method(manager, "MaybeRestoreCorpseState", 1);
    if (manager) api.bh_list = il2::il2cpp_class_get_field_from_name(manager, "m_BodyHarvestList");
    api.panel_bh = il2::field_offset(panel, "m_BodyHarvest");
    api.item_meat = il2::field_offset(panel, "m_MenuItem_Meat");
    api.item_hide = il2::field_offset(panel, "m_MenuItem_Hide");
    api.item_gut = il2::field_offset(panel, "m_MenuItem_Gut");
    api.set_amount = il2::find_method(item, "set_HarvestAmount", 1);
    api.set_units = il2::find_method(item, "set_HarvestUnits", 1);
    api.harvest_done = il2::find_method(panel, "HarvestSuccessful", 0);
    api.quarter_done = il2::find_method(panel, "QuarterSuccessful", 0);
    api.exit_panel = il2::find_method(panel, "ExitBodyHarvestPanel", 0);
    api.panel_enable = il2::find_method(panel, "Enable", 4);
    api.load_panel = il2::find_method_params(il2::find_class(kGame, "", "InterfaceManager"), "LoadPanel", {"System.Type"});
    g_ready = api.ai_list && api.ai_mode >= 0 && api.ai_body >= 0 && api.meat >= 0 && api.hide >= 0 && api.gut >= 0 &&
              api.instantiate && api.restore_corpse && api.save_data_ctor && api.sd_prefab >= 0 && api.panel_bh >= 0 &&
              api.harvest_done && api.quarter_done && api.round_meat && api.carcass_mesh;
    if (!g_ready) {
        logx::warn("wildlife carcasses: unavailable (TLD API missing); kills won't be shared");
        return false;
    }
    const bool ok =
        hooks::install_method("Panel_BodyHarvest.HarvestSuccessful", api.harvest_done,
                              reinterpret_cast<void*>(&harvest_done_detour), reinterpret_cast<void**>(&g_harvest_done)) &&
        hooks::install_method("Panel_BodyHarvest.QuarterSuccessful", api.quarter_done,
                              reinterpret_cast<void*>(&quarter_done_detour), reinterpret_cast<void**>(&g_quarter_done));
    logx::info("wildlife carcasses: %s%s", ok ? "ready (kills and harvests are shared)" : "PARTLY hooked",
               api.go_component && api.go_active && api.wildlife_item_type && api.spawn_carcass && api.despawn
                   ? ""
                   : "; dead rabbits and ptarmigan stay animals (not shared)");
    return ok;
}

void on_frame(bool in_game) {
    if (!g_ready) return;
    const double now = now_ms();
    run_test(now);
    if (in_game && runs_scene()) small_game_to_items(now);
    if (hosting()) {
        if (in_game) send_carcasses(now);
        std::vector<net::Packet> packets;
        netrt::drain_packets(net::Msg::Harvest, packets);
        for (const net::Packet& p : packets) {
            net::Reader r = net::packet_body(p);
            net::HarvestMsg h;
            if (net::read_harvest(r, h)) apply_harvest(h, p.from, p.link);
        }
        packets.clear();
        netrt::drain_packets(net::Msg::Quartered, packets);
        for (const net::Packet& p : packets) {
            net::Reader r = net::packet_body(p);
            const uint32_t id = r.get<uint32_t>();
            if (r.ok()) apply_quartered(id);
        }
        return;
    }
    const HostLink link = joined() && in_game ? host_link(now) : HostLink::None;
    const bool hearing = link == HostLink::Hearing;
    if (link == HostLink::Stalled && !g_locals.empty()) {
        if (!g_holding) logx::info("wildlife: the link to the host stalls; keeping our %zu carcass(es) of its animals",
                                   g_locals.size());
        g_holding = true;
        hold_locals(now);
        return;
    }
    if (g_holding && hearing) logx::info("wildlife: the host is back; our carcasses follow its again");
    g_holding = false;
    if (!joined()) g_quartered.clear();
    if (!joined() || !in_game || !wildlife_sync::mirroring()) {
        if (!g_locals.empty()) clear_locals("not mirroring the host's animals");
        std::vector<net::Packet> stale;
        netrt::drain_packets(net::Msg::Carcasses, stale);
        return;
    }
    receive_carcasses(now);
    update_locals(now);
}

}  // namespace wildlife_carcass
