#include "coop_give.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <sstream>
#include <vector>

#include "avatar.h"
#include "coop_notices.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/handover.h"
#include "net/protocol.h"
#include "net_runtime.h"
#include "world_items.h"

namespace coop_give {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr ptrdiff_t kCachedPtr = 0x10;     // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr float kBodyRadius = 0.4f;        // a friend's body, for the crosshair: this round
constexpr float kBodyHeight = 1.8f, kCrouchedHeight = 1.2f;  // and this tall
constexpr double kTakeFallbackMs = 4000;   // the receiver: in our pack this long after Done, whatever the bodies show
constexpr double kDoneWaitMs = 10000;      // accepted, and neither Done nor Failed this long after: it's off
constexpr int32_t kPanelOnExitInventory = 1;  // EnablePanelOnExit.Inventory: the units screen goes back to it
constexpr const char* kGiveLabel = "GIVE";    // the Drop buttons in give mode (Localization.Get gives a missing key back)
constexpr const char* kGiveAllLabel = "GIVE ALL";

struct Vector3 {
    float x, y, z;
};

struct Api {
    // Unity
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* get_forward = nullptr;
    Il2CppClass* game_object_class = nullptr;
    const MethodInfo* go_ctor = nullptr;            // GameObject(string)
    const MethodInfo* add_component = nullptr;      // GameObject.AddComponent(Type)
    const MethodInfo* go_in_children = nullptr;     // GameObject.GetComponentInChildren(Type, bool)
    const MethodInfo* keep_on_load = nullptr;       // static Object.DontDestroyOnLoad(Object)
    const MethodInfo* find_object = nullptr;        // static Object.FindObjectOfType(Type)
    // TLD
    const MethodInfo* main_camera = nullptr;        // static GameManager.GetMainCamera()
    const MethodInfo* player_manager = nullptr;     // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* player_transform = nullptr;   // static GameManager.GetPlayerTransform()
    const MethodInfo* inventory = nullptr;          // static GameManager.GetInventoryComponent()
    const MethodInfo* load_panel = nullptr;         // static InterfaceManager.LoadPanel(Type)
    // What the crosshair is on (PlayerManager), and ours (a SimpleInteraction on a GameObject of our own)
    Il2CppObject* interaction_type = nullptr;       // typeof(TLD.Interactions.SimpleInteraction)
    Il2CppClass* localized_class = nullptr;         // LocalizedString
    ptrdiff_t default_hover = -1;                   // BaseInteraction.m_DefaultHoverText (a LocalizedString)
    ptrdiff_t localization_id = -1;                 // LocalizedString.m_LocalizationID: the text when it becomes current
    const MethodInfo* set_hover = nullptr;          // BaseInteraction.set_HoverText(string): the text now
    const MethodInfo* perform = nullptr;            // SimpleInteraction.PerformInteraction(): interact pressed on it
    const MethodInfo* find_interactive = nullptr;   // PlayerManager.FindInteractiveObject(float): every frame
    const MethodInfo* set_current = nullptr;        // PlayerManager.SetCurrentInteraction(IInteraction)
    const MethodInfo* process_interaction = nullptr;  // PlayerManager.InteractiveObjectsProcessInteraction(): the key
    ptrdiff_t active = -1;                          // PlayerManager.<ActiveInteraction>k__BackingField
    ptrdiff_t last_hit = -1;                        // PlayerManager.m_LocationOfLastInteractHit: where TLD's ray hit
    // Giving and taking
    ptrdiff_t in_hands = -1;                        // PlayerManager.m_ItemInHandsInternal
    const MethodInfo* unequip = nullptr;            // PlayerManager.UnequipItemInHandsSkipAnimation()
    const MethodInfo* add_to_pack = nullptr;        // PlayerManager.AddItemToPlayerInventory(GearItem, bool, bool)
    const MethodInfo* destroy_gear = nullptr;       // Inventory.DestroyGear(GearItem)
    const MethodInfo* destroy_object = nullptr;     // static GearManager.DestroyGearObject(GearItem)
    const MethodInfo* remove_units = nullptr;       // Inventory.RemoveUnits(GearItem, int)
    ptrdiff_t food = -1, stackable = -1, units = -1;  // GearItem.m_FoodItem, m_StackableItem; StackableItem.m_Units
    ptrdiff_t cant_drop = -1, water = -1, clothing = -1;  // GearItem.m_CantDropItem, m_WaterSupply, m_ClothingItem
    const MethodInfo* wearing = nullptr;            // ClothingItem.IsWearing()
    // TLD's inventory screen in give mode, and its units screen
    Il2CppClass* inventory_panel = nullptr;         // Panel_Inventory
    const MethodInfo* inv_enable = nullptr;         // Enable(bool, bool resetFilter)
    const MethodInfo* inv_is_enabled = nullptr;
    const MethodInfo* inv_on_drop = nullptr;        // OnDrop(): its Drop button (the key too)
    const MethodInfo* inv_exit = nullptr;           // ExitInterface(bool restoreItemInHands)
    const MethodInfo* inv_selected = nullptr;       // GetCurrentlySelectedItem()
    const MethodInfo* inv_pause = nullptr;          // PauseAndHide(): under the units screen
    ptrdiff_t grid_gear = -1;                       // InventoryGridDataItem.m_GearItem
    const MethodInfo* drop_label = nullptr;         // static ItemDescriptionPage.GetDropButtonLocalizationId(GearItem)
    Il2CppClass* units_panel = nullptr;             // Panel_PickUnits
    const MethodInfo* units_enable = nullptr;       // Enable(bool)
    const MethodInfo* units_is_enabled = nullptr;
    const MethodInfo* units_for_drop = nullptr;     // SetGearForDrop(GearItem)
    const MethodInfo* units_drop = nullptr;         // DropGear(GearItem, int): its Drop and Drop all
    const MethodInfo* units_refresh = nullptr;      // Refresh()
    const MethodInfo* units_execute = nullptr;      // OnExecute(): its Drop button
    ptrdiff_t units_count = -1, units_on_exit = -1, units_button = -1;  // m_numUnits, m_EnablePanelOnExit, m_Execute_Button
    ptrdiff_t units_all_button = -1, units_title = -1, units_gear = -1;  // m_ExecuteAll_Button, m_Label_Description, m_GearItem
    Il2CppObject* label_type = nullptr;             // typeof(UILabel)
    const MethodInfo* label_set_text = nullptr;
    // The camera (dev `give look`)
    Il2CppObject* fps_camera_type = nullptr;        // typeof(vp_FPSCamera)
    ptrdiff_t camera_yaw = -1, camera_pitch = -1;   // vp_FPSCamera.m_Yaw / m_Pitch (degrees)
} api;

bool g_ready = false;
net::Handovers g_ledger;
uint32_t g_count = 0;          // our handovers so far: their ids (net::handover_id)
uintptr_t g_offered = 0;       // the item we hold out (GearItem) while our offer is open
uintptr_t g_interaction = 0;   // our SimpleInteraction
uintptr_t g_localized = 0;     // its LocalizedString, when we had to make it
uintptr_t g_hover_text = 0;    // its hover text (written into a field raw: kept alive here)
std::string g_hover;           // what it says
int g_looked_at = -1;          // the friend under the crosshair with something to do (-1: none)
int g_give_to = -1;            // the inventory is up to give to this friend (-1: it isn't)
bool g_close_inventory = false;  // an offer was made from it: close it (next frame, out of TLD's button handlers)
uintptr_t g_inventory = 0, g_units_screen = 0;  // the two screens, once we used them

// Given to us, not yet in our pack: it goes in once our body's hand has it.
struct Incoming {
    uint32_t id = 0;
    uint8_t giver = 0;
    std::string gear, prefab, record;
    int32_t units = 1;
    double done_ms = 0;
};
std::vector<Incoming> g_incoming;

using FindFn = void (*)(Il2CppObject* self, float range, const MethodInfo* method);
using BoolFn = bool (*)(Il2CppObject* self, const MethodInfo* method);
using VoidFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using DropGearFn = void (*)(Il2CppObject* self, Il2CppObject* gi, int32_t units, const MethodInfo* method);
using LabelFn = Il2CppString* (*)(Il2CppObject* gi, const MethodInfo* method);
FindFn g_find = nullptr;
BoolFn g_perform = nullptr;
VoidFn g_on_drop = nullptr;
DropGearFn g_units_drop = nullptr;
VoidFn g_units_refresh = nullptr;
LabelFn g_drop_label = nullptr;

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
Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
uintptr_t hold(Il2CppObject* o) { return o ? il2::il2cpp_gchandle_new(o, false) : 0; }
void drop(uintptr_t& h) {
    if (h) il2::il2cpp_gchandle_free(h);
    h = 0;
}

double now_ms() { return net::now_ms(); }

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

uint8_t me() { return netrt::status().self_id; }

std::string player_name(uint8_t id) {
    const net::Status st = netrt::status();
    if (id == st.self_id) return "You";
    for (const net::PlayerInfo& p : st.players)
        if (p.id == id) return p.name.empty() ? "Player " + std::to_string(id) : p.name;
    return "Player " + std::to_string(id);
}

std::string item_text(const std::string& prefab, int32_t units) {
    std::string name = world_items::gear_name(prefab);
    if (name.empty()) name = prefab;
    return units > 1 ? name + " (" + std::to_string(units) + ")" : name;
}

void tell(const std::string& text) {
    coop_notices::tell(text);
    logx::info("give: told: %s", text.c_str());
}

bool our_position(Vector3& out) {
    Il2CppObject* t = il2::invoke(api.player_transform, nullptr);
    return t && il2::invoke_value(api.get_position, t, out);
}

std::optional<avatar::Seen> friend_seen(uint8_t id) {
    for (const avatar::Seen& v : avatar::seen())
        if (v.player == id) return v;
    return std::nullopt;
}

bool present(const avatar::Seen& v) { return v.playing && v.here && !v.dead; }

float distance_to(const avatar::Seen& v) {
    Vector3 p{};
    if (!our_position(p)) return 1e9f;
    return std::hypot(v.at_x - p.x, v.at_z - p.z);
}

bool screen_up(uintptr_t h, const MethodInfo* is_enabled) {
    Il2CppObject* panel = target(h);
    bool on = false;
    return alive(panel) && il2::invoke_value(is_enabled, panel, on) && on;
}

Il2CppObject* load_panel(Il2CppClass* klass) {
    void* args[] = {il2::type_object(klass)};
    Il2CppObject* panel = il2::invoke(api.load_panel, nullptr, args);
    return alive(panel) ? panel : nullptr;
}

// --- messages ------------------------------------------------------------------------------------------------------

net::GiveMsg msg_of(const net::Handover& h, net::GiveOp op) {
    net::GiveMsg m;
    m.op = op;
    m.id = h.id;
    m.giver = h.giver;
    m.receiver = h.receiver;
    m.item = static_cast<uint8_t>(h.item);
    m.units = h.units;
    m.gear = h.gear;
    return m;
}

// Ours (from < 0), or a friend's the host passes on: a friend sends to the host; the host to every friend linked but
// the one it came from, the item's record only to the one receiving it.
void route(const net::GiveMsg& m, int from = -1) {
    if (!hosting()) {
        if (from >= 0) return;
        net::Writer w(net::Msg::Give, net::kMaxMessage);
        net::write_give(w, m);
        netrt::send_packet(w, true);
        return;
    }
    const net::Status st = netrt::status();
    for (const net::PlayerInfo& p : st.players) {
        if (!p.linked || p.id == st.self_id || static_cast<int>(p.id) == from) continue;
        net::GiveMsg c = m;
        if (c.op == net::GiveOp::Done && p.id != m.receiver) c.prefab.clear(), c.record.clear();
        net::Writer w(net::Msg::Give, net::kMaxMessage);
        net::write_give(w, c);
        netrt::send_packet_to(p.id, w, true);
    }
}

// --- what we do ----------------------------------------------------------------------------------------------------

// Why an item of ours can't be given (empty: it can).
std::string not_giveable(Il2CppObject* gi) {
    const std::string name = item_text(world_items::prefab_of(gi), 1);
    if (world_items::one_of_a_kind(gi)) return name + " is yours alone: it can't be given.";
    if (api.cant_drop >= 0 && get<bool>(gi, api.cant_drop)) return name + " can't be given.";
    if (api.water >= 0 && alive(get<Il2CppObject*>(gi, api.water))) return "Water can't be handed over.";
    Il2CppObject* clothes = api.clothing >= 0 ? get<Il2CppObject*>(gi, api.clothing) : nullptr;
    bool worn = false;
    if (alive(clothes) && il2::invoke_value(api.wearing, clothes, worn) && worn) return "Take " + name + " off first.";
    return {};
}

bool offer(uint8_t to, Il2CppObject* gi, int32_t units) {
    if (!alive(gi) || !world_items::in_pack(gi)) {
        logx::warn("give: that isn't in our pack");
        return false;
    }
    if (const std::string why = not_giveable(gi); !why.empty()) {
        tell(why);
        return false;
    }
    const uint8_t self = me();
    const std::string name = player_name(to);
    const std::optional<avatar::Seen> v = friend_seen(to);
    if (to == self || !v || !present(*v) || distance_to(*v) > net::kOfferRange) {
        tell(name + " is too far away.");
        return false;
    }
    const int have = world_items::units(gi);
    net::Handover h;
    h.id = net::handover_id(self, ++g_count);
    h.giver = self;
    h.receiver = to;
    h.gear = world_items::prefab_of(gi);
    h.item = net::hand_item_for(h.gear, api.food >= 0 && alive(get<Il2CppObject*>(gi, api.food)));
    h.units = have > 1 ? std::clamp(units, 1, have) : 1;
    h.offered_ms = now_ms();
    if (!g_ledger.offer(h)) {
        tell(g_ledger.open_of(self) ? "You're holding something out already." : name + " is busy.");
        return false;
    }
    drop(g_offered);
    g_offered = hold(gi);
    route(msg_of(h, net::GiveOp::Offer));
    g_give_to = -1;
    g_close_inventory = true;
    tell("You hold out " + item_text(h.gear, h.units) + " to " + name + ".");
    logx::info("give: #%08x: we offer %s x%d (%s) to %s (player %u)", h.id, h.gear.c_str(), h.units,
               net::hand_item_name(h.item), name.c_str(), to);
    return true;
}

void withdraw(uint32_t id, const std::string& told) {
    const net::Handover* h = g_ledger.find(id);
    if (!h || !g_ledger.end(id, now_ms(), false)) return;
    route(msg_of(*h, net::GiveOp::Withdraw));
    if (h->giver == me()) drop(g_offered);
    logx::info("give: #%08x: taken back by us", id);
    if (!told.empty()) tell(told);
}

void accept(uint32_t id) {
    const net::Handover* h = g_ledger.find(id);
    if (!h || h->receiver != me() || !g_ledger.accept(id, now_ms())) return;
    route(msg_of(*h, net::GiveOp::Accept));
    logx::info("give: #%08x: we take %s from %s", id, h->gear.c_str(), player_name(h->giver).c_str());
}

// The giver, once it's taken: still holding it, it comes out of our pack (the units given of a stack) and its record
// goes to them; or we no longer have it.
void give_now(const net::Handover h) {
    Il2CppObject* gi = target(g_offered);
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    Il2CppObject* inv = il2::invoke(api.inventory, nullptr);
    const std::string name = player_name(h.receiver), what = item_text(h.gear, h.units);
    const int have = alive(gi) ? world_items::units(gi) : 0;
    net::GiveMsg m = msg_of(h, net::GiveOp::Done);
    bool ok = alive(gi) && world_items::in_pack(gi) && pm && inv && (h.units <= 1 || have >= h.units);
    if (ok) {
        if (get<Il2CppObject*>(pm, api.in_hands) == gi) il2::invoke(api.unequip, pm);
        const bool part = have > h.units;  // some of a stack: the record says how many
        Il2CppObject* stack = part ? get<Il2CppObject*>(gi, api.stackable) : nullptr;
        if (part && alive(stack)) put<int32_t>(stack, api.units, h.units);
        ok = world_items::gear_record(gi, m.prefab, m.record);
        if (part && alive(stack)) put<int32_t>(stack, api.units, have);
        if (ok && part) {
            int32_t n = h.units;
            void* args[] = {gi, &n};
            il2::invoke(api.remove_units, inv, args);
        } else if (ok) {
            void* args[] = {gi};
            il2::invoke(api.destroy_gear, inv, args);
        }
    }
    drop(g_offered);
    if (!ok) {
        m.op = net::GiveOp::Failed;
        m.prefab.clear();
        m.record.clear();
        g_ledger.end(h.id, now_ms(), false);
        route(m);
        logx::warn("give: #%08x: %s took it, but we no longer have %s", h.id, name.c_str(), h.gear.c_str());
        tell(what + " is no longer in your pack.");
        return;
    }
    g_ledger.end(h.id, now_ms(), true);
    route(m);
    logx::info("give: #%08x: given to %s: %s x%d (%zu chars of record)", h.id, name.c_str(), m.prefab.c_str(), h.units,
               m.record.size());
    tell(name + " took " + what + ".");
}

void put_in_pack(const Incoming& in) {
    const std::string what = item_text(in.gear, in.units), name = player_name(in.giver);
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    Il2CppObject* gi = pm ? world_items::gear_from_record(in.prefab, in.record) : nullptr;
    if (!gi) {
        logx::error("give: #%08x: couldn't make %s from its record", in.id, in.prefab.c_str());
        tell(name + " gave you " + what + ", but it couldn't be made here.");
        return;
    }
    bool track = true, notify = true;
    void* args[] = {gi, &track, &notify};
    Il2CppObject* kept = il2::invoke(api.add_to_pack, pm, args);  // as a pickup does
    if (alive(kept) && kept != gi && alive(gi) && !world_items::in_pack(gi)) {
        // Its units went onto a stack we have; as after a pickup, the one made is spare (else it lies in the world).
        void* spare[] = {gi};
        il2::invoke(api.destroy_object, nullptr, spare);
        logx::info("give: #%08x: %s went onto the one we had (%d now)", in.id, in.prefab.c_str(), world_items::units(kept));
    }
    logx::info("give: #%08x: %s is in our pack (from %s)", in.id, in.prefab.c_str(), name.c_str());
    tell(name + " gave you " + what + ".");
}

void take_incoming(double now) {
    for (auto it = g_incoming.begin(); it != g_incoming.end();) {
        const net::Handover* h = g_ledger.find(it->id);
        bool due = !h || now - it->done_ms >= kTakeFallbackMs;
        if (!due) due = net::handover_view(h->offered_ms, h->accepted_ms, h->ended_ms, h->done, now).in_receiver;
        if (!due) {
            ++it;
            continue;
        }
        if (h) g_ledger.take(it->id);
        put_in_pack(*it);
        it = g_incoming.erase(it);
    }
}

// --- the crosshair -------------------------------------------------------------------------------------------------

enum class Act { None, Give, TakeBack, Take };
struct Choice {
    Act act = Act::None;
    std::string text;  // TLD's hover text for it
    uint32_t id = 0;   // the handover
};

Choice choice_for(uint8_t self, uint8_t friend_id) {
    const std::string name = player_name(friend_id);
    if (const net::Handover* h = g_ledger.open_of(self)) {
        if (h->accepted_ms > 0) return {};
        if (h->giver == self && h->receiver == friend_id)
            return {Act::TakeBack, "Take back " + item_text(h->gear, h->units), h->id};
        if (h->receiver == self && h->giver == friend_id)
            return {Act::Take, "Take " + item_text(h->gear, h->units) + " from " + name, h->id};
        return {};
    }
    if (g_ledger.open_of(friend_id)) return {};  // they're in one with someone else
    return {Act::Give, "Give to " + name, 0};
}

bool camera_ray(Vector3& eye, Vector3& forward) {
    Il2CppObject* camera = il2::invoke(api.main_camera, nullptr);
    Il2CppObject* tf = alive(camera) ? il2::invoke(api.component_transform, camera) : nullptr;
    return tf && il2::invoke_value(api.get_position, tf, eye) && il2::invoke_value(api.get_forward, tf, forward);
}

// The friend under the crosshair within reach (-1: none), and how far along the ray.
int friend_under_crosshair(const Vector3& eye, const Vector3& forward, float& along) {
    const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {forward.x, forward.y, forward.z};
    int best = -1;
    for (const avatar::Seen& v : avatar::seen()) {
        // Standing in them (a friend who just joined is where the host is): not looking at them.
        if (!present(v) || std::hypot(v.x - eye.x, v.z - eye.z) < kBodyRadius) continue;
        const float t = net::ray_hits_body(o, d, v.x, v.y, v.z, kBodyRadius, v.crouched ? kCrouchedHeight : kBodyHeight,
                                           net::kGiveRange);
        if (t >= 0 && (best < 0 || t < along)) best = v.player, along = t;
    }
    return best;
}

Il2CppObject* interaction() {
    if (Il2CppObject* it = target(g_interaction); alive(it)) return it;
    drop(g_interaction);
    Il2CppObject* go = il2::il2cpp_object_new(api.game_object_class);
    if (!go) return nullptr;
    void* name_args[] = {il2::il2cpp_string_new("TLDCoop_GiveInteraction")};
    il2::invoke(api.go_ctor, go, name_args);
    void* keep_args[] = {go};
    il2::invoke(api.keep_on_load, nullptr, keep_args);
    void* type_args[] = {api.interaction_type};
    Il2CppObject* it = il2::invoke(api.add_component, go, type_args);
    if (!alive(it)) {
        logx::warn("give: couldn't make our interaction");
        return nullptr;
    }
    if (!get<Il2CppObject*>(it, api.default_hover)) {  // (BaseInteraction's constructor makes one)
        Il2CppObject* text = il2::il2cpp_object_new(api.localized_class);
        drop(g_localized);
        g_localized = hold(text);
        put(it, api.default_hover, text);
    }
    g_interaction = hold(it);
    g_hover.clear();
    logx::info("give: our interaction is made");
    return it;
}

void set_hover(Il2CppObject* it, const std::string& text) {
    if (text == g_hover) return;
    g_hover = text;
    Il2CppString* s = il2::il2cpp_string_new(text.c_str());
    const uintptr_t old = g_hover_text;
    g_hover_text = hold(reinterpret_cast<Il2CppObject*>(s));
    put<Il2CppString*>(get<Il2CppObject*>(it, api.default_hover), api.localization_id, s);  // InitializeInteraction's
    void* args[] = {s};
    il2::invoke(api.set_hover, it, args);  // and now, if it's TLD's current one already
    if (old) il2::il2cpp_gchandle_free(old);
}

void set_current(Il2CppObject* pm, Il2CppObject* interaction) {
    void* args[] = {interaction};
    il2::invoke(api.set_current, pm, args);
}

// After TLD looked for what's under the crosshair: a friend in reach, nearer than what TLD found, is ours.
void find_detour(Il2CppObject* self, float range, const MethodInfo* method) {
    g_find(self, range, method);
    if (!g_ready || !netrt::active()) return;
    Il2CppObject* ours = target(g_interaction);
    Il2CppObject* active = get<Il2CppObject*>(self, api.active);
    Vector3 eye{}, forward{};
    float along = 0;
    const int who = camera_ray(eye, forward) ? friend_under_crosshair(eye, forward, along) : -1;
    const Choice c = who >= 0 ? choice_for(me(), static_cast<uint8_t>(who)) : Choice{};
    if (c.act != Act::None && active && active != ours) {
        const Vector3 hit = get<Vector3>(self, api.last_hit);
        const float d = std::sqrt((hit.x - eye.x) * (hit.x - eye.x) + (hit.y - eye.y) * (hit.y - eye.y) +
                                  (hit.z - eye.z) * (hit.z - eye.z));
        if (d < along) {  // TLD's is nearer
            g_looked_at = -1;
            return;
        }
    }
    if (c.act == Act::None) {
        g_looked_at = -1;
        if (ours && active == ours) set_current(self, nullptr);
        return;
    }
    Il2CppObject* it = interaction();
    if (!it) return;
    set_hover(it, c.text);
    g_looked_at = who;
    if (active != it) set_current(self, it);
}

bool open_give(uint8_t to) {
    Il2CppObject* panel = load_panel(api.inventory_panel);
    if (!panel) return false;
    drop(g_inventory);
    g_inventory = hold(panel);
    g_give_to = to;
    bool on = true, reset_filter = true;
    void* args[] = {&on, &reset_filter};
    il2::invoke(api.inv_enable, panel, args);
    logx::info("give: the inventory is up to give to %s (player %u)", player_name(to).c_str(), to);
    return true;
}

bool perform_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!g_ready || !self || self != target(g_interaction)) return g_perform(self, method);
    if (g_looked_at < 0) return false;
    const Choice c = choice_for(me(), static_cast<uint8_t>(g_looked_at));
    logx::info("give: interact on %s: %s", player_name(static_cast<uint8_t>(g_looked_at)).c_str(), c.text.c_str());
    switch (c.act) {
        case Act::Give: return open_give(static_cast<uint8_t>(g_looked_at));
        case Act::TakeBack: withdraw(c.id, "You took it back."); return true;
        case Act::Take: accept(c.id); return true;
        default: return false;
    }
}

// --- the inventory in give mode ------------------------------------------------------------------------------------

// GIVE on an item: a stack asks how many first (TLD's units screen, its GIVE goes to units_drop_detour).
void give_from_inventory(Il2CppObject* panel, Il2CppObject* gi) {
    if (const std::string why = not_giveable(gi); !why.empty()) {
        tell(why);
        return;
    }
    if (world_items::units(gi) > 1) {
        if (Il2CppObject* pick = load_panel(api.units_panel)) {
            drop(g_units_screen);
            g_units_screen = hold(pick);
            il2::invoke(api.inv_pause, panel);
            bool on = true;
            void* enable_args[] = {&on};
            il2::invoke(api.units_enable, pick, enable_args);
            void* gear_args[] = {gi};
            il2::invoke(api.units_for_drop, pick, gear_args);
            put<int32_t>(pick, api.units_on_exit, kPanelOnExitInventory);
            logx::info("give: how many of %s? (TLD's units screen)", world_items::prefab_of(gi).c_str());
            return;
        }
    }
    offer(static_cast<uint8_t>(g_give_to), gi, 1);
}

void on_drop_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!g_ready || g_give_to < 0) return g_on_drop(self, method);
    Il2CppObject* selected = il2::invoke(api.inv_selected, self);
    Il2CppObject* gi = selected ? get<Il2CppObject*>(selected, api.grid_gear) : nullptr;
    if (alive(gi)) give_from_inventory(self, gi);  // (a decoration isn't given)
}

void units_drop_detour(Il2CppObject* self, Il2CppObject* gi, int32_t units, const MethodInfo* method) {
    if (!g_ready || g_give_to < 0) return g_units_drop(self, gi, units, method);
    if (alive(gi) && units > 0) offer(static_cast<uint8_t>(g_give_to), gi, units);
}

void set_label(Il2CppObject* label, const std::string& text) {
    if (!alive(label)) return;
    void* args[] = {il2::il2cpp_string_new(text.c_str())};
    il2::invoke(api.label_set_text, label, args);
}

void set_button_label(Il2CppObject* button, const char* text) {
    if (!alive(button)) return;
    bool inactive_too = true;
    void* find_args[] = {api.label_type, &inactive_too};
    set_label(il2::invoke(api.go_in_children, button, find_args), text);
}

// The units screen says drop: in give mode its title and both buttons say give (Drop all is DropGear too).
void units_refresh_detour(Il2CppObject* self, const MethodInfo* method) {
    g_units_refresh(self, method);
    if (!g_ready || g_give_to < 0) return;
    set_button_label(get<Il2CppObject*>(self, api.units_button), kGiveLabel);
    if (api.units_all_button >= 0) set_button_label(get<Il2CppObject*>(self, api.units_all_button), kGiveAllLabel);
    Il2CppObject* gi = api.units_gear >= 0 ? get<Il2CppObject*>(self, api.units_gear) : nullptr;
    if (api.units_title >= 0 && alive(gi))
        set_label(get<Il2CppObject*>(self, api.units_title), "Select how many " + item_text(world_items::prefab_of(gi), 1) + " to give");
}

Il2CppString* drop_label_detour(Il2CppObject* gi, const MethodInfo* method) {
    if (g_ready && g_give_to >= 0) return il2::il2cpp_string_new(kGiveLabel);
    return g_drop_label(gi, method);
}

// --- the others' messages ------------------------------------------------------------------------------------------

void apply(const net::GiveMsg& m) {
    const uint8_t self = me();
    const double now = now_ms();
    const std::string giver = player_name(m.giver), what = item_text(m.gear, m.units);
    switch (m.op) {
        case net::GiveOp::Offer: {
            net::Handover h;
            h.id = m.id;
            h.giver = m.giver;
            h.receiver = m.receiver;
            h.item = net::hand_item(m.item);
            h.gear = m.gear;
            h.units = m.units;
            h.offered_ms = now;
            if (g_ledger.offer(h)) {
                logx::info("give: #%08x: %s holds out %s x%d to %s", m.id, giver.c_str(), m.gear.c_str(), m.units,
                           player_name(m.receiver).c_str());
                if (m.receiver == self) tell(giver + " holds out " + what + " for you: look at them and interact to take it.");
            } else if (m.receiver == self && !g_ledger.find(m.id)) {  // we're in another one: it's off
                net::GiveMsg back = m;
                back.op = net::GiveOp::Withdraw;
                route(back);
                logx::info("give: #%08x: %s holds out %s, but we're busy", m.id, giver.c_str(), m.gear.c_str());
            }
            break;
        }
        case net::GiveOp::Accept:
            if (!g_ledger.accept(m.id, now)) break;
            logx::info("give: #%08x: %s takes it", m.id, player_name(m.receiver).c_str());
            if (m.giver == self)
                if (const net::Handover* h = g_ledger.find(m.id)) give_now(*h);
            break;
        case net::GiveOp::Done:
            if (!g_ledger.end(m.id, now, true)) break;
            logx::info("give: #%08x: given", m.id);
            if (m.receiver != self) break;
            if (m.prefab.empty() || m.record.empty()) {
                logx::warn("give: #%08x: given to us without its record", m.id);
                break;
            }
            g_incoming.push_back({m.id, m.giver, m.gear, m.prefab, m.record, m.units, now});
            break;
        case net::GiveOp::Withdraw:
        case net::GiveOp::Failed:
            if (!g_ledger.end(m.id, now, false)) break;
            logx::info("give: #%08x: %s", m.id, m.op == net::GiveOp::Failed ? "the giver no longer has it" : "taken back");
            if (m.giver == self) {
                drop(g_offered);
                tell(player_name(m.receiver) + " didn't take " + what + ": you keep it.");
            } else if (m.receiver == self) {
                tell(m.op == net::GiveOp::Failed ? giver + " no longer has " + what + "." : giver + " took " + what + " back.");
            }
            break;
    }
}

void receive() {
    static std::vector<net::Packet> in;
    in.clear();
    netrt::drain_packets(net::Msg::Give, in);
    const bool host = hosting();
    for (const net::Packet& p : in) {
        net::Reader r = net::packet_body(p);
        net::GiveMsg m;
        if (!net::read_give(r, m) || net::handover_giver(m.id) != m.giver) continue;
        if (host) {  // a friend's: only about their own part in it
            const bool giver = p.from == m.giver, receiver = p.from == m.receiver;
            const bool theirs = m.op == net::GiveOp::Accept     ? receiver
                                : m.op == net::GiveOp::Withdraw ? giver || receiver
                                                                : giver;
            if (!theirs) {
                logx::warn("give: #%08x: player %u sent another's part in it; dropped", m.id, p.from);
                continue;
            }
            route(m, p.from);
        }
        apply(m);
    }
}

// --- every frame ---------------------------------------------------------------------------------------------------

// Ours: taken back when nobody takes it, or we're apart; an acceptance nothing came of is off. Anyone's with a player
// who left is over.
void watch(double now) {
    const uint8_t self = me();
    if (const net::Handover* open = g_ledger.open_of(self)) {
        const net::Handover h = *open;
        const uint8_t other = h.giver == self ? h.receiver : h.giver;
        const std::optional<avatar::Seen> v = friend_seen(other);
        const std::string name = player_name(other), what = item_text(h.gear, h.units);
        const bool apart = !v || !present(*v) || distance_to(*v) > net::kOfferRange;
        if (h.accepted_ms <= 0) {
            if (h.giver == self && now - h.offered_ms > net::kOfferTimeoutMs)
                withdraw(h.id, name + " didn't take " + what + ": you keep it.");
            else if (apart)
                withdraw(h.id, h.giver == self ? "Too far from " + name + ": you keep " + what + "."
                                               : "Too far from " + name + " to take " + what + ".");
        } else if (h.receiver == self && now - h.accepted_ms > kDoneWaitMs) {
            g_ledger.end(h.id, now, false);
            logx::warn("give: #%08x: accepted, but nothing came", h.id);
            tell(name + " couldn't give " + what + ".");
        }
    }
    const net::Status st = netrt::status();
    std::vector<uint8_t> here{st.self_id};
    for (const net::PlayerInfo& p : st.players)
        if (p.linked || !hosting()) here.push_back(p.id);
    std::vector<uint8_t> gone;
    for (const auto& [id, h] : g_ledger.all())
        if (h.ended_ms <= 0)
            for (uint8_t who : {h.giver, h.receiver})
                if (std::find(here.begin(), here.end(), who) == here.end()) gone.push_back(who);
    for (uint8_t who : gone)
        for (uint32_t id : g_ledger.end_all_of(who, now)) logx::info("give: #%08x: player %u left", id, who);
    g_ledger.prune(now);
}

// What each body here shows of a handover it's in.
void show_bodies(double now) {
    const uint8_t self = me();
    Vector3 mine{};
    const bool have_me = our_position(mine);
    const bool me_female = avatar::female_body(netrt::body());
    const auto female_of = [&](uint8_t id) { return id == self ? me_female : avatar::female(id); };
    const std::vector<avatar::Seen> all = avatar::seen();
    for (const avatar::Seen& v : all) {
        const net::Handover* h = v.here ? g_ledger.shown_for(v.player, now) : nullptr;
        avatar::HandoverShow s;
        bool show = false;
        if (h) {
            const net::HandoverView view = net::handover_view(h->offered_ms, h->accepted_ms, h->ended_ms, h->done, now);
            const bool giver = h->giver == v.player;
            const uint8_t partner = giver ? h->receiver : h->giver;
            s.giver = giver;
            s.t = giver ? view.giver_t : view.receiver_t;
            s.fading = view.fading;
            s.gear = h->gear;
            s.item = static_cast<uint8_t>(h->item);
            s.item_in_hand = view.item_shown && giver != view.in_receiver;
            s.giver_female = female_of(h->giver);
            s.partner_female = female_of(partner);
            if (partner == self) {
                show = have_me;
                s.partner_x = mine.x;
                s.partner_z = mine.z;
            } else {
                for (const avatar::Seen& p : all)
                    if (p.player == partner && p.here) {
                        show = true;
                        s.partner_x = p.at_x;
                        s.partner_z = p.at_z;
                        s.partner_moves = true;
                    }
            }
            show = show && (s.t >= 0 || s.item_in_hand);
        }
        avatar::show_handover(v.player, show ? &s : nullptr);
    }
}

void reset() {
    g_ledger.clear();
    drop(g_offered);
    g_give_to = g_looked_at = -1;
    g_close_inventory = false;
}

const net::Handover* ours() { return g_ledger.open_of(me()); }

Il2CppObject* pack_item(const std::string& like) {
    for (Il2CppObject* gi : world_items::pack())
        if (world_items::prefab_of(gi).find(like) != std::string::npos) return gi;
    return nullptr;
}

}  // namespace

void on_frame(bool in_game) {
    if (!g_ready) return;
    const double now = now_ms();
    if (in_game) take_incoming(now);
    if (!netrt::active()) {
        if (!g_ledger.all().empty()) reset();
        return;
    }
    receive();
    if (!in_game) {
        if (const net::Handover* h = ours(); h && h->accepted_ms <= 0) withdraw(h->id, "");
        g_give_to = g_looked_at = -1;
        return;
    }
    if (g_close_inventory) {
        g_close_inventory = false;
        if (screen_up(g_inventory, api.inv_is_enabled)) {
            bool restore = true;
            void* args[] = {&restore};
            il2::invoke(api.inv_exit, target(g_inventory), args);
        }
    }
    if (g_give_to >= 0 && !screen_up(g_inventory, api.inv_is_enabled) && !screen_up(g_units_screen, api.units_is_enabled)) {
        logx::info("give: the inventory is closed: no longer giving");
        g_give_to = -1;
    }
    watch(now);
    show_bodies(now);
}

void describe() {
    const double now = now_ms();
    std::string all;
    for (const auto& [id, h] : g_ledger.all()) {
        const net::HandoverView v = net::handover_view(h.offered_ms, h.accepted_ms, h.ended_ms, h.done, now);
        char line[256];
        snprintf(line, sizeof line, "%s#%08x %u->%u %s x%d (%s) %s%s, give %.2f receive %.2f%s", all.empty() ? "" : "; ",
                 h.id, h.giver, h.receiver, h.gear.c_str(), h.units, net::hand_item_name(h.item),
                 h.ended_ms > 0 ? (h.done ? "given" : "off") : h.accepted_ms > 0 ? "taken" : "offered",
                 h.taken ? " (in our pack)" : "", v.giver_t, v.receiver_t, v.in_receiver ? ", in the receiver's hand" : "");
        all += line;
    }
    logx::info("dev: handover: looking at %d, giving to %d, hover '%s', %zu coming to us; %s", g_looked_at, g_give_to,
               g_hover.c_str(), g_incoming.size(), all.empty() ? "no handovers" : all.c_str());
}

bool dev(const std::string& action, const std::string& args) {
    if (!g_ready) return false;
    std::istringstream in(args);
    if (action == "look") {  // face a friend: their chest under the crosshair
        int id = -1;
        in >> id;
        Vector3 eye{}, forward{};
        if (!camera_ray(eye, forward)) return false;
        std::optional<avatar::Seen> best;
        float best_d = 1e9f;
        for (const avatar::Seen& v : avatar::seen()) {
            if (!present(v) || (id >= 0 && v.player != id)) continue;
            const float d = std::hypot(v.x - eye.x, v.z - eye.z);
            if (d < best_d) best = v, best_d = d;
        }
        void* type_args[] = {api.fps_camera_type};
        Il2CppObject* camera = best ? il2::invoke(api.find_object, nullptr, type_args) : nullptr;
        if (!camera) return false;
        // Which way m_Pitch runs, from where the camera looks now (up, unless it looks clearly the other way).
        const float now_pitch = get<float>(camera, api.camera_pitch);
        const float sign = std::fabs(now_pitch) > 5 && std::fabs(forward.y) > 0.1f && (now_pitch > 0) != (forward.y > 0) ? -1.0f : 1.0f;
        const float chest = best->y + (best->crouched ? 0.8f : 1.2f);
        float yaw = std::atan2(best->x - eye.x, best->z - eye.z) * 57.29578f;
        if (yaw < 0) yaw += 360;
        const float pitch = std::atan2(chest - eye.y, best_d) * 57.29578f * sign;
        put(camera, api.camera_yaw, yaw);
        put(camera, api.camera_pitch, pitch);
        logx::info("dev: handover: looking at %s, %.2f m away (yaw %.1f, pitch %.1f)", best->name.c_str(), best_d, yaw,
                   pitch);
        return true;
    }
    if (action == "face") {  // turn our camera that way (tests that need a body facing a given way)
        float yaw = 0, pitch = 0;
        if (!(in >> yaw)) return false;
        in >> pitch;
        void* type_args[] = {api.fps_camera_type};
        Il2CppObject* camera = il2::invoke(api.find_object, nullptr, type_args);
        if (!camera) return false;
        put(camera, api.camera_yaw, yaw);
        put(camera, api.camera_pitch, pitch);
        logx::info("dev: handover: facing yaw %.1f, pitch %.1f", yaw, pitch);
        return true;
    }
    if (action == "close") {  // shut the give screens, as their Back does
        if (screen_up(g_units_screen, api.units_is_enabled)) {
            bool on = false;
            void* args[] = {&on};
            il2::invoke(api.units_enable, target(g_units_screen), args);
        }
        if (screen_up(g_inventory, api.inv_is_enabled)) {
            bool restore = true;
            void* args[] = {&restore};
            il2::invoke(api.inv_exit, target(g_inventory), args);
        }
        return true;
    }
    if (action == "press") {
        Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
        bool done = false;
        if (!pm || !il2::invoke_value(api.process_interaction, pm, done)) return false;
        logx::info("dev: handover: interact pressed (%s)", done ? "something was done" : "nothing to do");
        return true;
    }
    if (action == "open") {
        int id = -1;
        in >> id;
        return id >= 0 && open_give(static_cast<uint8_t>(id));
    }
    if (action == "drop") {
        std::string like;
        in >> like;
        Il2CppObject* gi = pack_item(like);
        Il2CppObject* panel = target(g_inventory);
        if (!gi || g_give_to < 0 || !screen_up(g_inventory, api.inv_is_enabled)) return false;
        give_from_inventory(panel, gi);
        return true;
    }
    if (action == "units") {
        int32_t n = 1;
        in >> n;
        Il2CppObject* pick = target(g_units_screen);
        if (g_give_to < 0 || !screen_up(g_units_screen, api.units_is_enabled)) return false;
        put<int32_t>(pick, api.units_count, n);
        il2::invoke(api.units_execute, pick);
        return true;
    }
    if (action == "offer") {
        int id = -1, units = 1;
        std::string like;
        in >> id >> like >> units;
        Il2CppObject* gi = id >= 0 ? pack_item(like) : nullptr;
        return gi && offer(static_cast<uint8_t>(id), gi, units);
    }
    if (action == "accept") {
        const net::Handover* h = ours();
        if (!h || h->receiver != me() || h->accepted_ms > 0) return false;
        accept(h->id);
        return true;
    }
    if (action == "back") {
        const net::Handover* h = ours();
        if (!h || h->accepted_ms > 0) return false;
        withdraw(h->id, "You took it back.");
        return true;
    }
    return false;
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.get_forward = il2::find_method(transform, "get_forward", 0);
    api.game_object_class = game_object;
    api.go_ctor = il2::find_method_params(game_object, ".ctor", {"System.String"});
    api.add_component = il2::find_method_params(game_object, "AddComponent", {"System.Type"});
    api.go_in_children = il2::find_method_params(game_object, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    api.keep_on_load = il2::find_method(object, "DontDestroyOnLoad", 1);
    api.find_object = il2::find_method_params(object, "FindObjectOfType", {"System.Type"});

    auto keep_type = [](Il2CppClass* klass) {
        Il2CppObject* type = klass ? il2::type_object(klass) : nullptr;
        if (type) il2::il2cpp_gchandle_new(type, false);
        return type;
    };
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* inventory = il2::find_class(kGame, "", "Inventory");
    Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
    api.main_camera = il2::find_method(game_manager, "GetMainCamera", 0);
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.player_transform = il2::find_method(game_manager, "GetPlayerTransform", 0);
    api.inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    api.load_panel = il2::find_method_params(il2::find_class(kGame, "", "InterfaceManager"), "LoadPanel", {"System.Type"});

    Il2CppClass* simple = il2::find_class(kGame, "TLD.Interactions", "SimpleInteraction");
    Il2CppClass* base = il2::find_class(kGame, "TLD.Interactions", "BaseInteraction");
    api.interaction_type = keep_type(simple);
    api.localized_class = il2::find_class(kGame, "", "LocalizedString");
    api.default_hover = il2::field_offset(base, "m_DefaultHoverText");
    api.localization_id = il2::field_offset(api.localized_class, "m_LocalizationID");
    api.set_hover = il2::find_method(base, "set_HoverText", 1);
    api.perform = il2::find_method(simple, "PerformInteraction", 0);
    api.find_interactive = il2::find_method_params(player_manager, "FindInteractiveObject", {"System.Single"});
    api.set_current = il2::find_method(player_manager, "SetCurrentInteraction", 1);
    api.process_interaction = il2::find_method(player_manager, "InteractiveObjectsProcessInteraction", 0);
    api.active = il2::field_offset(player_manager, "<ActiveInteraction>k__BackingField");
    api.last_hit = il2::field_offset(player_manager, "m_LocationOfLastInteractHit");

    api.in_hands = il2::field_offset(player_manager, "m_ItemInHandsInternal");
    api.unequip = il2::find_method(player_manager, "UnequipItemInHandsSkipAnimation", 0);
    api.add_to_pack = il2::find_method(player_manager, "AddItemToPlayerInventory", 3);
    api.destroy_gear = il2::find_method_params(inventory, "DestroyGear", {"GearItem"});
    api.destroy_object = il2::find_method_params(il2::find_class(kGame, "", "GearManager"), "DestroyGearObject", {"GearItem"});
    api.remove_units = il2::find_method(inventory, "RemoveUnits", 2);
    api.food = il2::field_offset(gear, "m_FoodItem");
    api.stackable = il2::field_offset(gear, "m_StackableItem");
    api.units = il2::field_offset(il2::find_class(kGame, "", "StackableItem"), "m_Units");
    api.cant_drop = il2::field_offset(gear, "m_CantDropItem");
    api.water = il2::field_offset(gear, "m_WaterSupply");
    api.clothing = il2::field_offset(gear, "m_ClothingItem");
    api.wearing = il2::find_method(il2::find_class(kGame, "", "ClothingItem"), "IsWearing", 0);

    Il2CppClass* panel = il2::find_class(kGame, "", "Panel_Inventory");
    api.inventory_panel = panel;
    api.inv_enable = il2::find_method_params(panel, "Enable", {"System.Boolean", "System.Boolean"});
    api.inv_is_enabled = il2::find_method(panel, "IsEnabled", 0);
    api.inv_on_drop = il2::find_method(panel, "OnDrop", 0);
    api.inv_exit = il2::find_method(panel, "ExitInterface", 1);
    api.inv_selected = il2::find_method(panel, "GetCurrentlySelectedItem", 0);
    api.inv_pause = il2::find_method(panel, "PauseAndHide", 0);
    api.grid_gear = il2::field_offset(il2::find_class(kGame, "", "InventoryGridDataItem"), "m_GearItem");
    api.drop_label = il2::find_method(il2::find_class(kGame, "", "ItemDescriptionPage"), "GetDropButtonLocalizationId", 1);
    Il2CppClass* pick = il2::find_class(kGame, "", "Panel_PickUnits");
    api.units_panel = pick;
    api.units_enable = il2::find_method(pick, "Enable", 1);
    api.units_is_enabled = il2::find_method(pick, "IsEnabled", 0);
    api.units_for_drop = il2::find_method(pick, "SetGearForDrop", 1);
    api.units_drop = il2::find_method(pick, "DropGear", 2);
    api.units_refresh = il2::find_method(pick, "Refresh", 0);
    api.units_execute = il2::find_method(pick, "OnExecute", 0);
    api.units_count = il2::field_offset(pick, "m_numUnits");
    api.units_on_exit = il2::field_offset(pick, "m_EnablePanelOnExit");
    api.units_button = il2::field_offset(pick, "m_Execute_Button");
    api.units_all_button = il2::field_offset(pick, "m_ExecuteAll_Button");
    api.units_title = il2::field_offset(pick, "m_Label_Description");
    api.units_gear = il2::field_offset(pick, "m_GearItem");
    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    api.label_type = keep_type(label);
    api.label_set_text = il2::find_method(label, "set_text", 1);
    Il2CppClass* camera = il2::find_class(kGame, "", "vp_FPSCamera");
    api.fps_camera_type = keep_type(camera);
    api.camera_yaw = il2::field_offset(camera, "m_Yaw");
    api.camera_pitch = il2::field_offset(camera, "m_Pitch");

    g_ready = api.component_transform && api.get_position && api.get_forward && api.go_ctor && api.add_component &&
              api.go_in_children && api.keep_on_load && api.find_object && api.main_camera && api.player_manager &&
              api.player_transform && api.inventory && api.load_panel && api.interaction_type && api.localized_class &&
              api.default_hover >= 0 && api.localization_id >= 0 && api.set_hover && api.perform &&
              api.find_interactive && api.set_current && api.process_interaction && api.active >= 0 &&
              api.last_hit >= 0 && api.in_hands >= 0 && api.unequip && api.add_to_pack && api.destroy_gear &&
              api.destroy_object && api.remove_units && api.food >= 0 && api.stackable >= 0 && api.units >= 0 && api.inventory_panel &&
              api.inv_enable && api.inv_is_enabled && api.inv_on_drop && api.inv_exit && api.inv_selected &&
              api.inv_pause && api.grid_gear >= 0 && api.drop_label && api.units_panel && api.units_enable &&
              api.units_is_enabled && api.units_for_drop && api.units_drop && api.units_refresh &&
              api.units_execute && api.units_count >= 0 && api.units_on_exit >= 0 && api.units_button >= 0 &&
              api.label_type && api.label_set_text && api.fps_camera_type && api.camera_yaw >= 0 &&
              api.camera_pitch >= 0;
    if (!g_ready) {
        logx::warn("give: UNAVAILABLE (TLD API missing; items can't be handed to friends)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    g_ready = hook("PlayerManager.FindInteractiveObject", api.find_interactive, &find_detour, &g_find) &&
              hook("SimpleInteraction.PerformInteraction", api.perform, &perform_detour, &g_perform) &&
              hook("Panel_Inventory.OnDrop", api.inv_on_drop, &on_drop_detour, &g_on_drop) &&
              hook("Panel_PickUnits.DropGear", api.units_drop, &units_drop_detour, &g_units_drop) &&
              hook("ItemDescriptionPage.GetDropButtonLocalizationId", api.drop_label, &drop_label_detour, &g_drop_label);
    // The units screen's button says GIVE too, where its Refresh can be hooked (else it says TLD's Drop).
    if (g_ready && !hook("Panel_PickUnits.Refresh", api.units_refresh, &units_refresh_detour, &g_units_refresh))
        logx::warn("give: the units screen's button keeps TLD's label");
    g_count = static_cast<uint32_t>(GetTickCount64() / 1000) & 0xFFFF00;  // ids not used before, a restart or not
    logx::info("give: %s", g_ready ? "ready (look at a friend close by and interact to give them something)"
                                   : "UNAVAILABLE (hooks failed)");
    return g_ready;
}

}  // namespace coop_give
