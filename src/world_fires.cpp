#include "world_fires.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace world_fires {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr double kCheckMs = 500;  // fires are compared with what both games last had this often
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr size_t kItemEntry = 16;       // TLD.Gear.GearItemObject in an array: GearItem m_GearItem, string m_GearItemName

// FireState
constexpr int kOff = 0, kFullBurn = 6, kBlownOut = 7;
const char* const kStateNames[] = {"off", "starting (fire starter)", "starting (tinder smoulders)", "starting (small flame)",
                                   "starting (medium)", "starting (full)", "burning", "blown out"};
// CampfireState
constexpr int kPlaced = 0;
const char* const kCampfireStates[] = {"placed", "lit", "burnt out"};

struct Vector3 {
    float x, y, z;
};

struct Api {
    // Unity
    const MethodInfo* component_go = nullptr;
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* object_name = nullptr;
    const MethodInfo* guid_of = nullptr;          // static ObjectGuid.GetGuidFromGameObject(GameObject, bool)
    const MethodInfo* attach_guid = nullptr;      // static ObjectGuid.MaybeAttachObjectGuidAndRegister(GameObject, string)
    const MethodInfo* new_id = nullptr;           // static TLD.PDID.PdidTable.GenerateNewID()
    // FireManager
    FieldInfo* fires = nullptr;                   // static FireManager.m_Fires
    FieldInfo* dismantled = nullptr;              // static FireManager.m_DestroyedFireGuids (HashSet<string>)
    const MethodInfo* save_all = nullptr;         // static FireManager.Serialize()
    const MethodInfo* load = nullptr;             // static FireManager.Deserialize(string)
    const MethodInfo* remove_fire = nullptr;      // static FireManager.RemoveFire(Fire, bool permanent)
    const MethodInfo* destroy = nullptr;          // static UnityEngine.Object.Destroy(Object)
    const MethodInfo* fire_manager = nullptr;     // static GameManager.GetFireManagerComponent()
    const MethodInfo* start_fire = nullptr;       // FireManager.PlayerStartFire(Fire, starter, tinder, fuel, accelerant, Action<bool>)
    const MethodInfo* force_success = nullptr;    // FireManager.SetForceFireStartingSuccess(bool)
    const MethodInfo* make_campfire = nullptr;    // FireManager.InstantiateCampFire()
    const MethodInfo* set_position = nullptr;     // Transform.set_position(Vector3)
    const MethodInfo* set_active = nullptr;       // GameObject.SetActive(bool)
    // Fire
    const MethodInfo* state = nullptr;            // Fire.GetFireState()
    const MethodInfo* embers = nullptr;           // Fire.IsEmbers()
    const MethodInfo* remaining = nullptr;        // Fire.GetRemainingLifeTimeSeconds()
    const MethodInfo* add_fuel = nullptr;         // Fire.AddFuel(GearItem, bool inForge)
    const MethodInfo* shorten = nullptr;          // Fire.ReduceDurationByTODSeconds(float) (taking a torch)
    const MethodInfo* can_dismantle = nullptr;    // Fire.CanBreakdownFire(bool isCustomizing)
    ptrdiff_t max_on = -1;                        // Fire.m_MaxOnTODSeconds: how long it burns (fuel adds to it)
    ptrdiff_t elapsed_on = -1;                    // Fire.m_ElapsedOnTODSeconds: how long it has burnt
    ptrdiff_t charcoal = -1, charcoal_taken = -1; // Fire.m_NumGeneratedCharcoalPieces, m_HasCollectedCharcoal
    ptrdiff_t campfire = -1;                      // Fire.m_Campfire
    ptrdiff_t campfire_state = -1, placing = -1;  // Campfire.m_State, m_IsBeingPlaced
    // Player / pack
    const MethodInfo* player_manager = nullptr;   // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* inventory = nullptr;        // static GameManager.GetInventoryComponent()
    const MethodInfo* consume = nullptr;          // PlayerManager.ConsumeUnitFromInventory(GameObject)
    ptrdiff_t inv_items = -1;                     // Inventory.m_Items
    ptrdiff_t gear_starter = -1, gear_fuel = -1;  // GearItem.m_FireStarterItem, m_FuelSourceItem
    ptrdiff_t is_tinder = -1, is_accelerant = -1; // FuelSourceItem.m_IsTinder, FireStarterItem.m_IsAccelerant
    // Cooking
    const MethodInfo* get_parent = nullptr;       // Transform.get_parent()
    const MethodInfo* empty_slot = nullptr;       // static GearPlacePoint.FindFirstEmptyPlacePoint(GameObject parent)
    const MethodInfo* place_item = nullptr;       // GearPlacePoint.DropAndPlaceItem(GearItem): cooking meat (no pot)
    ptrdiff_t placed_gear = -1;                   // GearPlacePoint.m_PlacedGear
    // Clock
    const MethodInfo* unistorm = nullptr;         // static GameManager.GetUniStorm()
    ptrdiff_t elapsed = -1, elapsed_part = -1;    // UniStormWeatherSystem.m_ElapsedHours, m_ElapsedHoursAccumulator
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

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

Vector3 position_of(Il2CppObject* component) {
    Vector3 p{};
    il2::invoke_value(api.get_position, il2::invoke(api.component_transform, component), p);
    return p;
}

// TLD's own id for the fire (what FireManager saves it by). Fires without one aren't shared.
std::string id_of(Il2CppObject* fire) {
    void* args[] = {il2::invoke(api.component_go, fire), nullptr};
    bool warn = false;
    args[1] = &warn;
    return args[0] ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.guid_of, nullptr, args))) : std::string();
}

// A campfire a player just made has no id yet: give it a new one the way TLD registers ids (the other game registers
// the same one when it loads the fire).
std::string give_id(Il2CppObject* fire) {
    Il2CppObject* id = il2::invoke(api.new_id, nullptr);
    if (!id) return std::string();
    void* args[] = {il2::invoke(api.component_go, fire), id};
    il2::invoke(api.attach_guid, nullptr, args);
    return id_of(fire);
}

std::vector<Il2CppObject*> all_fires() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = nullptr;
    il2::il2cpp_field_static_get_value(api.fires, &list);
    if (!list) return out;
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* f = il2::array_object_at(items, static_cast<size_t>(i)); alive(f)) out.push_back(f);
    return out;
}

int state_of(Il2CppObject* fire) {
    int32_t s = 0;
    il2::invoke_value(api.state, fire, s);
    return s;
}

bool embers(Il2CppObject* fire) {
    bool e = false;
    il2::invoke_value(api.embers, fire, e);
    return e;
}

float minutes_left(Il2CppObject* fire) {
    float s = 0;
    il2::invoke_value(api.remaining, fire, s);
    return s / 60.0f;
}

Il2CppObject* campfire_of(Il2CppObject* fire) {
    Il2CppObject* c = get<Il2CppObject*>(fire, api.campfire);
    return alive(c) ? c : nullptr;
}

const char* state_name(Il2CppObject* fire) {
    const int s = state_of(fire);
    if (s == kOff && embers(fire)) return "embers";
    return s >= 0 && s <= kBlownOut ? kStateNames[s] : "?";
}

// Lit, out or blown out, and not a campfire still being placed: what the other game can load. (While a player lights
// it, TLD steps through its starting states with that player's fire starter.)
bool settled(Il2CppObject* fire) {
    const int s = state_of(fire);
    if (s != kOff && s != kFullBurn && s != kBlownOut) return false;
    Il2CppObject* c = campfire_of(fire);
    return !c || !get<bool>(c, api.placing);
}

// A campfire made here and not lit yet (the fire panel may still take it away again) isn't the other player's business.
bool unlit_new_campfire(Il2CppObject* fire) {
    Il2CppObject* c = campfire_of(fire);
    return c && get<int32_t>(c, api.campfire_state) == kPlaced && state_of(fire) == kOff;
}

// What changes when someone does something to the fire, not as it burns: its state, embers, charcoal, the campfire's
// state. (Fuel added or a torch taken is caught by hooks: TLD keeps that in burn time that also changes as it burns.)
std::string signature(Il2CppObject* fire) {
    char out[96];
    Il2CppObject* c = campfire_of(fire);
    snprintf(out, sizeof(out), "%d|%d|%ld|%d|%d|%d", state_of(fire), embers(fire) ? 1 : 0,
             std::lround(get<float>(fire, api.max_on) / 60.0f), get<int32_t>(fire, api.charcoal),
             get<bool>(fire, api.charcoal_taken) ? 1 : 0, c ? get<int32_t>(c, api.campfire_state) : -1);
    return out;
}

std::string describe_fire(Il2CppObject* fire) {
    char out[160];
    Il2CppObject* c = campfire_of(fire);
    const int cs = c ? get<int32_t>(c, api.campfire_state) : -1;
    snprintf(out, sizeof(out), "%s, %.0f min left%s%s", state_name(fire), minutes_left(fire), c ? ", campfire " : "",
             c ? (cs >= 0 && cs <= 2 ? kCampfireStates[cs] : "?") : "");
    return out;
}

// --- TLD's save text -----------------------------------------------------------------------------------------------

// Hours played, as Fire.Deserialize counts them (UniStorm's m_ElapsedHours + m_ElapsedHoursAccumulator). The two games'
// differ (the shared clock is the time of day, not hours played).
float hours_played() {
    Il2CppObject* u = il2::invoke(api.unistorm, nullptr);
    return u ? get<float>(u, api.elapsed) + get<float>(u, api.elapsed_part) : 0.0f;
}

// A fire's save record says when it was saved (m_HoursPlayed), and loading it burns the fire for the hours since. The
// other player's hours aren't ours: put in our own hours at the time it reached us, so it burns only for the time since.
std::string with_hours(const std::string& save_text, float hours) {
    static const std::string key = "m_HoursPlayed";
    char value[32];
    snprintf(value, sizeof(value), "%.6f", hours);
    std::string out;
    size_t from = 0;
    for (size_t p = save_text.find(key); p != std::string::npos; p = save_text.find(key, p)) {
        p += key.size();
        while (p < save_text.size() && (save_text[p] == '\\' || save_text[p] == '"' || save_text[p] == ':' || save_text[p] == ' ')) ++p;
        size_t end = p;
        while (end < save_text.size() && (std::isdigit(static_cast<unsigned char>(save_text[end])) || save_text[end] == '.' ||
                                          save_text[end] == '-' || save_text[end] == 'e' || save_text[end] == 'E' ||
                                          save_text[end] == '+'))
            ++end;
        if (end == p) continue;
        out.append(save_text, from, p - from);
        out += value;
        from = p = end;
    }
    out.append(save_text, from, std::string::npos);
    return out;
}

std::string save_all() { return text(reinterpret_cast<Il2CppString*>(il2::invoke(api.save_all, nullptr))); }

// The objects (as text) in the JSON array named `key`: "key":[{...},{...}].
std::vector<std::string> array_objects(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return out;
    p = json.find('[', p);
    if (p == std::string::npos) return out;
    ++p;
    while (p < json.size() && json[p] != ']') {
        if (json[p] != '{') {
            ++p;
            continue;
        }
        const size_t start = p;
        int depth = 0;
        bool in_string = false;
        for (; p < json.size(); ++p) {
            const char c = json[p];
            if (in_string) {
                if (c == '\\')
                    ++p;
                else if (c == '"')
                    in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if ((c == '}' || c == ']') && --depth == 0) {
                ++p;
                break;
            }
        }
        out.push_back(json.substr(start, p - start));
    }
    return out;
}

// The fire's entry in FireManager.Serialize()'s text.
std::string record_for(const std::string& all, const std::string& id) {
    for (std::string& r : array_objects(all, "m_SerializedFires"))
        if (r.find("\"" + id + "\"") != std::string::npos) return r;
    return std::string();
}

// The fires TLD counts as destroyed (dismantled) in FireManager.Serialize()'s text: its save keeps them gone.
std::vector<std::string> destroyed_ids(const std::string& all) {
    std::vector<std::string> out;
    size_t p = all.find("\"m_DestroyedFires\"");
    if (p == std::string::npos || (p = all.find('[', p)) == std::string::npos) return out;
    const size_t end = all.find(']', p);
    while ((p = all.find('"', p + 1)) < end) {
        const size_t close = all.find('"', p + 1);
        if (close == std::string::npos || close > end) break;
        out.push_back(all.substr(p + 1, close - p - 1));
        p = close;
    }
    return out;
}

bool destroyed_here(const std::string& all, const std::string& id) {
    const std::vector<std::string> ids = destroyed_ids(all);
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// --- state ---------------------------------------------------------------------------------------------------------

std::string g_scene;                                    // the scene the state below is about ("" = none)
std::unordered_map<std::string, std::string> g_sig;     // id -> signature both games have
std::unordered_map<std::string, Il2CppObject*> g_here;  // id -> the fire, at the last check
netrt::Newest g_newest;                                 // id -> each player's newest change applied
uint32_t g_seq = 0;
double g_last_check = 0;
bool g_baseline = false;                                // g_sig holds this scene's fires
bool g_applying = false;
bool g_connected = false;
bool g_at_menu = false;
std::vector<Il2CppObject*> g_touched;                  // fires fuel was added to (or a torch taken from) here
netrt::Together g_together;                             // host: friends in our scene (our list went to each as they came)
// Host: the client's changes in scenes we aren't in (loaded when we get there).
std::unordered_map<std::string, std::unordered_map<std::string, net::FirePutMsg>> g_store;
std::optional<net::FireListMsg> g_list;                 // client: the host's fires for a scene we aren't in yet

void reset(const char* why) {
    if (!g_sig.empty()) logx::info("fires: forgetting shared state (%s)", why);
    g_scene.clear();
    g_sig.clear();
    g_here.clear();
    g_newest.clear();
    g_list.reset();
    g_touched.clear();
    g_baseline = false;
    g_together.clear();
}

// What we keep of the world for later (changes for scenes someone isn't in) belongs to this game: a dropped link
// keeps it, back at the main menu (another save may load next) it goes.
void forget_world() {
    if (g_store.empty()) return;
    logx::info("fires: back at the main menu; forgetting fires kept for %zu scene(s)", g_store.size());
    g_store.clear();
}

// FireManager keeps the ids of dismantled fires in a static set that its saves write, and loading a game doesn't
// empty it (FireManager.Deserialize only destroys the saved ones' objects): a fire dismantled before going back to the
// main menu would count as dismantled in the next game loaded (and go to a friend as such). Back at the main menu it's
// emptied, as a restart of the game would.
void forget_dismantled() {
    Il2CppObject* set = nullptr;
    if (api.dismantled) il2::il2cpp_field_static_get_value(api.dismantled, &set);
    if (!set) return;
    Il2CppClass* klass = il2::il2cpp_object_get_class(set);
    int32_t count = 0;
    if (!il2::invoke_value(il2::find_method(klass, "get_Count", 0), set, count) || count == 0) return;
    il2::invoke(il2::find_method(klass, "Clear", 0), set);
    logx::info("fires: back at the main menu; TLD's %d dismantled fire id(s) from that game forgotten", count);
}

Il2CppObject* find(const std::string& id) {
    for (Il2CppObject* f : all_fires())
        if (id_of(f) == id) return f;
    return nullptr;
}

// What TLD's dismantle does (Panel_ActionPicker.DismantleFireCompleteCallback, without the yield): out of FireManager's
// list and into its destroyed ids (which its save writes, so a fire from the scene stays gone), then the object goes.
// TLD's load path (FireManager.Deserialize's destroyed fires) only destroys the object, and not one that's burning.
void remove_for_good(Il2CppObject* fire) {
    bool permanent = true;
    void* args[] = {fire, &permanent};
    il2::invoke(api.remove_fire, nullptr, args);
    void* destroy_args[] = {il2::invoke(api.component_go, fire)};
    il2::invoke(api.destroy, nullptr, destroy_args);
}

// What both games have now: every fire here as it is.
void baseline() {
    g_sig.clear();
    g_here.clear();
    for (Il2CppObject* f : all_fires()) {
        const std::string id = id_of(f);
        if (id.empty()) continue;
        g_here[id] = f;
        g_sig[id] = signature(f);
    }
    g_baseline = true;
}

// --- sending -------------------------------------------------------------------------------------------------------

void send_put(const std::string& id, const std::string& record, uint8_t flags) {
    net::FirePutMsg m;
    m.scene = g_scene;
    m.seq = ++g_seq;
    m.flags = flags;
    m.id = id;
    m.text = record;
    if (m.text.size() > net::kFireTextMax) {
        logx::warn("fires: %s is too big to send (%zu chars)", short_id(id).c_str(), m.text.size());
        return;
    }
    net::Writer w(net::Msg::FirePut, net::kMaxMessage);
    net::write_fire_put(w, m);
    netrt::send_packet(w, true);
}

// Host: what one friend changed goes on to the others, as a change of ours (our counter: each friend's is its own).
void pass_on(uint8_t from, net::FirePutMsg m) {
    m.seq = ++g_seq;
    net::Writer w(net::Msg::FirePut, net::kMaxMessage);
    net::write_fire_put(w, m);
    netrt::send_packet_except(from, w, true);
}

// `all` (FireManager.Serialize()'s text) without these fires' records.
std::string without_fires(std::string all, const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
        const std::string r = record_for(all, id);
        if (r.empty()) continue;
        size_t p = all.find(r), end = p + r.size();
        if (end < all.size() && all[end] == ',')
            ++end;
        else if (p > 0 && all[p - 1] == ',')
            --p;  // the last one in the array
        all.erase(p, end - p);
    }
    return all;
}

void send_list(uint8_t to) {
    net::FireListMsg m;
    m.scene = g_scene;
    // A fire being lit right now goes when it's lit (check() sends it then): its record now would load half-started there.
    std::vector<std::string> lighting;
    for (Il2CppObject* f : all_fires())
        if (!settled(f))
            if (std::string id = id_of(f); !id.empty()) lighting.push_back(std::move(id));
    m.text = without_fires(save_all(), lighting);
    if (m.text.size() > net::kFireListMax) {
        logx::warn("fires: our fires in %s are too big to send (%zu chars)", g_scene.c_str(), m.text.size());
        return;
    }
    net::Writer w(net::Msg::FireList, net::kMaxMessage);
    net::write_fire_list(w, m);
    netrt::send_packet_to(to, w, true);
    logx::info("fires: the other player is here (%s); sent our %zu fires (%zu chars)%s (player %u)", g_scene.c_str(),
               array_objects(m.text, "m_SerializedFires").size(), m.text.size(),
               lighting.empty() ? "" : "; one being lit goes once it's lit", to);
}

// Compare the fires here with what both games last had; send what changed.
void check() {
    std::unordered_map<std::string, Il2CppObject*> here;
    std::vector<std::pair<std::string, Il2CppObject*>> changed;
    for (Il2CppObject* f : all_fires()) {
        std::string id = id_of(f);
        if (id.empty() && settled(f) && !unlit_new_campfire(f)) id = give_id(f);
        if (id.empty()) continue;
        here[id] = f;
        if (!settled(f)) continue;
        const auto known = g_sig.find(id);
        const bool touched = std::find(g_touched.begin(), g_touched.end(), f) != g_touched.end();
        if (known == g_sig.end() ? !unlit_new_campfire(f) : touched || known->second != signature(f))
            changed.emplace_back(id, f);
    }
    g_touched.erase(std::remove_if(g_touched.begin(), g_touched.end(), [](Il2CppObject* f) { return !alive(f) || settled(f); }),
                    g_touched.end());
    // A fire that left FireManager's list is gone for the other player only if TLD counts it destroyed (dismantled):
    // its save keeps it gone then. (A fire whose object went for another reason would come back with the save.)
    std::string all;
    for (const auto& [id, f] : g_here) {
        if (here.count(id) || !g_sig.count(id)) continue;
        g_sig.erase(id);
        if (all.empty()) all = save_all();
        if (!destroyed_here(all, id)) {
            logx::info("fires: fire %s isn't in TLD's list any more, but TLD doesn't count it destroyed; not told",
                       short_id(id).c_str());
            continue;
        }
        send_put(id, std::string(), net::kFireGone);
        logx::info("fires: fire %s is gone here (dismantled); told the other player", short_id(id).c_str());
    }
    g_here = std::move(here);
    if (changed.empty()) return;
    if (all.empty()) all = save_all();
    for (const auto& [id, f] : changed) {
        const std::string record = record_for(all, id);
        if (record.empty()) {
            logx::warn("fires: %s (%s) isn't in TLD's fire save", name_of(f).c_str(), short_id(id).c_str());
            g_sig[id] = signature(f);
            continue;
        }
        const bool is_new = !g_sig.count(id);
        send_put(id, record, 0);
        g_sig[id] = signature(f);
        logx::info("fires: %s %s (%s) here: %s; told the other player (%zu chars)", is_new ? "new fire" : "fire",
                   name_of(f).c_str(), short_id(id).c_str(), describe_fire(f).c_str(), record.size());
    }
}

// --- applying the other player's fires -----------------------------------------------------------------------------

void load(const std::string& save_text) {
    g_applying = true;
    void* args[] = {il2::il2cpp_string_new(save_text.c_str())};
    il2::invoke(api.load, nullptr, args);
    g_applying = false;
}

void apply_put(const net::FirePutMsg& m, const char* who) {
    if (Il2CppObject* ours = find(m.id); ours && !settled(ours) && !campfire_of(ours)) {
        logx::info("fires: %s fire %s, but we're lighting it right now; ours stands", who, short_id(m.id).c_str());
        return;
    }
    if (m.flags & net::kFireGone) {
        Il2CppObject* ours = find(m.id);
        if (ours) remove_for_good(ours);
        g_sig.erase(m.id);
        g_here.erase(m.id);
        logx::info("fires: %s fire %s is gone; %s", who, short_id(m.id).c_str(),
                   ours ? "gone here too" : "it wasn't here");
        return;
    }
    load("{\"m_SerializedFires\":[" + m.text + "],\"m_DestroyedFires\":[]}");
    Il2CppObject* f = find(m.id);
    if (!f) {
        logx::warn("fires: %s fire %s didn't load here", who, short_id(m.id).c_str());
        return;
    }
    g_here[m.id] = f;
    g_sig[m.id] = signature(f);
    logx::info("fires: %s fire %s (%s): %s", who, name_of(f).c_str(), short_id(m.id).c_str(), describe_fire(f).c_str());
}

void apply_stored() {
    const auto it = g_store.find(g_scene);
    if (it == g_store.end()) return;
    std::vector<net::FirePutMsg> changes;
    for (auto& [id, m] : it->second) changes.push_back(std::move(m));
    g_store.erase(it);
    std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return static_cast<int32_t>(a.seq - b.seq) < 0; });
    for (const net::FirePutMsg& m : changes) apply_put(m, "while we were elsewhere the other player's");
    logx::info("fires: %zu fire(s) the other player changed in %s while we were elsewhere", changes.size(), g_scene.c_str());
}

void apply_list(const net::FireListMsg& m) {
    // The host's dismantled fires go the way a dismantle does (our save keeps them gone too) before TLD's load, which
    // would only destroy the objects (the next check would see them leave without TLD counting them destroyed).
    size_t removed = 0;
    for (const std::string& id : destroyed_ids(m.text))
        if (Il2CppObject* ours = find(id); ours && record_for(m.text, id).empty()) remove_for_good(ours), ++removed;
    load(m.text);
    baseline();
    logx::info("fires: loaded the host's %zu fires in %s%s", array_objects(m.text, "m_SerializedFires").size(), g_scene.c_str(),
               removed ? ("; " + std::to_string(removed) + " the host dismantled are gone here too").c_str() : "");
}

void receive() {
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::FirePut, packets);
    netrt::drain_packets(net::Msg::FireList, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        if (p.type == net::Msg::FirePut) {
            net::FirePutMsg m;
            if (!net::read_fire_put(r, m)) continue;
            if (!g_newest.take(p, m.id, m.seq)) continue;  // a late older one
            if (hosting()) pass_on(p.from, m);  // with their hours: each game puts in its own
            m.text = with_hours(m.text, hours_played());
            if (m.scene != g_scene || !g_baseline) {
                if (hosting()) {
                    logx::info("fires: the other player's fire %s in %s changed; kept for when we get there",
                               short_id(m.id).c_str(), m.scene.c_str());
                    g_store[m.scene][m.id] = std::move(m);
                }
                continue;  // client: the host's list comes when we get there
            }
            apply_put(m, "the other player's");
        } else if (p.type == net::Msg::FireList && !hosting()) {
            net::FireListMsg m;
            if (!net::read_fire_list(r, m)) continue;
            m.text = with_hours(m.text, hours_played());
            if (m.scene == g_scene && g_baseline)
                apply_list(m);
            else
                g_list = std::move(m);
        }
    }
}

// --- hooks ---------------------------------------------------------------------------------------------------------

using AddFuelFn = void (*)(Il2CppObject* self, Il2CppObject* fuel, bool in_forge, const MethodInfo* method);
using ShortenFn = void (*)(Il2CppObject* self, float seconds, const MethodInfo* method);
AddFuelFn g_add_fuel = nullptr;
ShortenFn g_shorten = nullptr;

void touch(Il2CppObject* fire) {
    if (g_ready && g_connected && !g_scene.empty() && !g_applying && alive(fire) &&
        std::find(g_touched.begin(), g_touched.end(), fire) == g_touched.end())
        g_touched.push_back(fire);
}

void add_fuel_detour(Il2CppObject* self, Il2CppObject* fuel, bool in_forge, const MethodInfo* method) {
    g_add_fuel(self, fuel, in_forge, method);
    touch(self);
}

void shorten_detour(Il2CppObject* self, float seconds, const MethodInfo* method) {
    g_shorten(self, seconds, method);
    touch(self);
}

// --- test helpers --------------------------------------------------------------------------------------------------

Vector3 player_pos() {
    const game::PlayerState p = game::read_player();
    return Vector3{p.position.x, p.position.y, p.position.z};
}

Il2CppObject* nearest(const std::string& name, bool burning, float max_metres, float* metres) {
    const Vector3 me = player_pos();
    Il2CppObject* best = nullptr;
    float best_d = max_metres;
    for (Il2CppObject* f : all_fires()) {
        if ((state_of(f) == kFullBurn) != burning) continue;
        const float d = distance(position_of(f), me);
        if (d <= best_d && contains_nocase(name_of(f), name)) best = f, best_d = d;
    }
    if (metres) *metres = best_d;
    return best;
}

std::vector<Il2CppObject*> pack_items() {
    std::vector<Il2CppObject*> out;
    Il2CppObject* inv = il2::invoke(api.inventory, nullptr);
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

}  // namespace

void describe(float metres) {
    if (!g_ready) return;
    const Vector3 me = player_pos();
    std::vector<std::pair<float, Il2CppObject*>> nearby;
    int total = 0, without_id = 0;
    for (Il2CppObject* f : all_fires()) {
        ++total;
        if (id_of(f).empty()) ++without_id;
        const float d = distance(position_of(f), me);
        if (d <= metres) nearby.emplace_back(d, f);
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::string all = save_all();
    logx::info("dev: fires: %zu within %.0f m (%d in %s, %d without a TLD id); TLD's fire save is %zu chars", nearby.size(),
               metres, total, g_scene.c_str(), without_id, all.size());
    for (size_t i = 0; i < nearby.size() && i < 25; ++i) {
        Il2CppObject* f = nearby[i].second;
        const Vector3 p = position_of(f);
        logx::info("dev:   %s %s at (%.1f, %.1f, %.1f), %.1f m, %s", short_id(id_of(f)).c_str(), name_of(f).c_str(), p.x,
                   p.y, p.z, nearby[i].first, describe_fire(f).c_str());
    }
    if (!nearby.empty()) {
        const std::string record = record_for(all, id_of(nearby[0].second));
        logx::info("dev:   its save record (%zu chars): %.900s", record.size(), record.c_str());
    }
    std::string destroyed;
    for (const std::string& id : destroyed_ids(all)) destroyed += " " + short_id(id);
    logx::info("dev:   dismantled here (TLD's save keeps them gone):%s", destroyed.empty() ? " none" : destroyed.c_str());
}

bool burn_out(float seconds) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* f = nearest("", true, 6.0f, &d);
    if (!f) {
        logx::warn("dev: burnout: no burning fire within 6 m");
        return false;
    }
    // What Fire.MaybeBlowOutFromWind does: the burnt time moves up to near the end (the longest burn time stays, so
    // the other game hears of it only when the fire goes out here).
    const float before = minutes_left(f), end = get<float>(f, api.max_on) - std::max(seconds, 0.0f);
    if (end > get<float>(f, api.elapsed_on)) put<float>(f, api.elapsed_on, end);
    logx::info("dev: burnout: %s (%s), %.1f m away: %.0f min left; now %.1f (as the wind does it, in this game only)",
               name_of(f).c_str(), short_id(id_of(f)).c_str(), d, before, minutes_left(f));
    return true;
}

bool dismantle(const std::string& name) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* f = nearest(name, false, 6.0f, &d);
    if (!f) {
        logx::warn("dev: dismantle: no fire%s%s that isn't burning within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    bool customizing = false, can = false;
    void* args[] = {&customizing};
    if (!il2::invoke_value(api.can_dismantle, f, can, args) || !can) {
        logx::warn("dev: dismantle: TLD doesn't let %s (%s) be dismantled (%s)", name_of(f).c_str(), short_id(id_of(f)).c_str(),
                   describe_fire(f).c_str());
        return false;
    }
    logx::info("dev: dismantle: %s (%s), %.1f m away, %s: removed as TLD's dismantle does", name_of(f).c_str(),
               short_id(id_of(f)).c_str(), d, describe_fire(f).c_str());
    remove_for_good(f);
    return true;
}

bool light(const std::string& name) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* f = nearest(name, false, 6.0f, &d);
    if (!f) {
        logx::warn("dev: light: no unlit fire%s%s within 6 m", name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    Il2CppObject *starter = nullptr, *tinder = nullptr, *fuel = nullptr;
    for (Il2CppObject* gi : pack_items()) {
        if (Il2CppObject* s = get<Il2CppObject*>(gi, api.gear_starter); alive(s) && !get<bool>(s, api.is_accelerant) && !starter)
            starter = s;
        if (Il2CppObject* s = get<Il2CppObject*>(gi, api.gear_fuel); alive(s)) {
            if (get<bool>(s, api.is_tinder)) {
                if (!tinder) tinder = s;
            } else if (!fuel) {
                fuel = s;
            }
        }
    }
    if (!starter || !tinder || !fuel) {
        logx::warn("dev: light: the pack has no %s", !starter ? "fire starter" : !tinder ? "tinder" : "fuel");
        return false;
    }
    Il2CppObject* fm = il2::invoke(api.fire_manager, nullptr);
    bool force = true;
    void* force_args[] = {&force};
    il2::invoke(api.force_success, fm, force_args);
    void* args[] = {f, starter, tinder, fuel, nullptr, nullptr};
    il2::invoke(api.start_fire, fm, args);
    logx::info("dev: light: %s (%s), %.1f m away, with %s, %s and %s: %s", name_of(f).c_str(), short_id(id_of(f)).c_str(), d,
               name_of(starter).c_str(), name_of(tinder).c_str(), name_of(fuel).c_str(), describe_fire(f).c_str());
    return true;
}

bool feed(const std::string& name) {
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* f = nearest("", true, 6.0f, &d);
    Il2CppObject* gi = nullptr;
    for (Il2CppObject* candidate : pack_items())
        if (alive(get<Il2CppObject*>(candidate, api.gear_fuel)) && contains_nocase(name_of(candidate), name)) gi = candidate;
    if (!f || !gi) {
        logx::warn("dev: feed: %s", !f ? "no burning fire within 6 m" : ("no fuel like " + name + " in our pack").c_str());
        return false;
    }
    const std::string before = describe_fire(f), what = name_of(gi);
    bool in_forge = false;
    void* args[] = {gi, &in_forge};
    il2::invoke(api.add_fuel, f, args);  // what the feed panel does (Panel_FeedFire.OnFeedFire)
    void* consume_args[] = {il2::invoke(api.component_go, gi)};
    il2::invoke(api.consume, il2::invoke(api.player_manager, nullptr), consume_args);
    logx::info("dev: feed: %s on %s (%s), %.1f m away: was %s; now %s", what.c_str(), name_of(f).c_str(),
               short_id(id_of(f)).c_str(), d, before.c_str(), describe_fire(f).c_str());
    return true;
}

bool cook(const std::string& name) {
    if (!g_ready || !api.empty_slot || !api.place_item || !api.get_parent || api.placed_gear < 0) return false;
    float d = 0;
    Il2CppObject* f = nearest("", true, 6.0f, &d);
    Il2CppObject* gi = nullptr;
    for (Il2CppObject* candidate : pack_items())
        if (contains_nocase(name_of(candidate), name)) gi = candidate;
    if (!f || !gi) {
        logx::warn("dev: cook: %s", !f ? "no burning fire within 6 m" : ("nothing like " + name + " in our pack").c_str());
        return false;
    }
    // The fire's cooking slots (GearPlacePoint) are under it or, for a stove, under an object above it.
    Il2CppObject* slot = nullptr;
    for (Il2CppObject* tf = il2::invoke(api.component_transform, f); tf && !slot; tf = il2::invoke(api.get_parent, tf)) {
        void* args[] = {il2::invoke(api.component_go, tf)};
        slot = il2::invoke(api.empty_slot, nullptr, args);
    }
    if (!alive(slot)) {
        logx::warn("dev: cook: %s (%s) has no empty cooking slot", name_of(f).c_str(), short_id(id_of(f)).c_str());
        return false;
    }
    const std::string what = name_of(gi);
    void* args[] = {gi};
    il2::invoke(api.place_item, slot, args);  // what the cooking panel does for food without a pot (OnCookNormalFood)
    Il2CppObject* placed = get<Il2CppObject*>(slot, api.placed_gear);
    logx::info("dev: cook: %s on %s (%s), %.1f m away: its slot %s holds %s", what.c_str(), name_of(f).c_str(),
               short_id(id_of(f)).c_str(), d, name_of(slot).c_str(), alive(placed) ? name_of(placed).c_str() : "nothing");
    return true;
}

bool campfire() {
    if (!g_ready) return false;
    Il2CppObject* f = il2::invoke(api.make_campfire, il2::invoke(api.fire_manager, nullptr));
    if (!alive(f)) {
        logx::warn("dev: campfire: FireManager.InstantiateCampFire made nothing");
        return false;
    }
    // Where placing it would put it: on the ground 1.5 m ahead of us (InstantiateCampFire leaves it inactive for that).
    const game::PlayerState p = game::read_player();
    const float yaw = p.body_yaw * 3.14159265f / 180.0f;
    Vector3 at{p.position.x + std::sin(yaw) * 1.5f, p.position.y, p.position.z + std::cos(yaw) * 1.5f};
    void* args[] = {&at};
    il2::invoke(api.set_position, il2::invoke(api.component_transform, f), args);
    bool on = true;
    void* on_args[] = {&on};
    il2::invoke(api.set_active, il2::invoke(api.component_go, f), on_args);
    logx::info("dev: campfire: made %s (%s) at (%.1f, %.1f, %.1f): %s", name_of(f).c_str(), short_id(id_of(f)).c_str(), at.x,
               at.y, at.z, describe_fire(f).c_str());
    return true;
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* manager = il2::find_class(kGame, "", "FireManager");
    Il2CppClass* fire = il2::find_class(kGame, "", "Fire");
    Il2CppClass* campfire = il2::find_class(kGame, "", "Campfire");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* inventory = il2::find_class(kGame, "", "Inventory");
    Il2CppClass* gear = il2::find_class(kGame, "", "GearItem");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.object_name = il2::find_method(object, "get_name", 0);
    api.guid_of = il2::find_method_params(il2::find_class(kGame, "", "ObjectGuid"), "GetGuidFromGameObject",
                                          {"UnityEngine.GameObject", "System.Boolean"});
    api.attach_guid = il2::find_method(il2::find_class(kGame, "", "ObjectGuid"), "MaybeAttachObjectGuidAndRegister", 2);
    api.new_id = il2::find_method(il2::find_class("TLD.PDID.dll", "TLD.PDID", "PdidTable"), "GenerateNewID", 0);
    if (manager) api.fires = il2::il2cpp_class_get_field_from_name(manager, "m_Fires");
    if (manager) api.dismantled = il2::il2cpp_class_get_field_from_name(manager, "m_DestroyedFireGuids");
    api.save_all = il2::find_method(manager, "Serialize", 0);
    api.load = il2::find_method(manager, "Deserialize", 1);
    api.remove_fire = il2::find_method_params(manager, "RemoveFire", {"Fire", "System.Boolean"});
    api.destroy = il2::find_method(object, "Destroy", 1);
    api.fire_manager = il2::find_method(game_manager, "GetFireManagerComponent", 0);
    api.start_fire = il2::find_method(manager, "PlayerStartFire", 6);
    api.force_success = il2::find_method(manager, "SetForceFireStartingSuccess", 1);
    api.state = il2::find_method(fire, "GetFireState", 0);
    api.embers = il2::find_method(fire, "IsEmbers", 0);
    api.remaining = il2::find_method(fire, "GetRemainingLifeTimeSeconds", 0);
    api.add_fuel = il2::find_method(fire, "AddFuel", 2);
    api.shorten = il2::find_method(fire, "ReduceDurationByTODSeconds", 1);
    api.can_dismantle = il2::find_method(fire, "CanBreakdownFire", 1);
    api.max_on = il2::field_offset(fire, "m_MaxOnTODSeconds");
    api.elapsed_on = il2::field_offset(fire, "m_ElapsedOnTODSeconds");
    api.charcoal = il2::field_offset(fire, "m_NumGeneratedCharcoalPieces");
    api.charcoal_taken = il2::field_offset(fire, "m_HasCollectedCharcoal");
    api.campfire = il2::field_offset(fire, "m_Campfire");
    api.campfire_state = il2::field_offset(campfire, "m_State");
    api.placing = il2::field_offset(campfire, "m_IsBeingPlaced");
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    api.consume = il2::find_method(player_manager, "ConsumeUnitFromInventory", 1);
    api.inv_items = il2::field_offset(inventory, "m_Items");
    api.gear_starter = il2::field_offset(gear, "m_FireStarterItem");
    api.gear_fuel = il2::field_offset(gear, "m_FuelSourceItem");
    api.is_tinder = il2::field_offset(il2::find_class(kGame, "", "FuelSourceItem"), "m_IsTinder");
    api.is_accelerant = il2::field_offset(il2::find_class(kGame, "", "FireStarterItem"), "m_IsAccelerant");
    api.make_campfire = il2::find_method(manager, "InstantiateCampFire", 0);
    api.set_position = il2::find_method(transform, "set_position", 1);
    api.set_active = il2::find_method(il2::find_class(kCore, "UnityEngine", "GameObject"), "SetActive", 1);
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    if (Il2CppClass* slot = il2::find_class(kGame, "", "GearPlacePoint")) {
        api.empty_slot = il2::find_method(slot, "FindFirstEmptyPlacePoint", 1);
        api.place_item = il2::find_method(slot, "DropAndPlaceItem", 1);
        api.placed_gear = il2::field_offset(slot, "m_PlacedGear");
    }
    api.unistorm = il2::find_method(game_manager, "GetUniStorm", 0);
    Il2CppClass* unistorm = il2::find_class(kGame, "", "UniStormWeatherSystem");
    api.elapsed = il2::field_offset(unistorm, "m_ElapsedHours");
    api.elapsed_part = il2::field_offset(unistorm, "m_ElapsedHoursAccumulator");
    g_ready = api.component_go && api.component_transform && api.get_position && api.object_name && api.guid_of &&
              api.fires && api.save_all && api.load && api.state && api.embers && api.remaining && api.max_on >= 0 &&
              api.charcoal >= 0 && api.charcoal_taken >= 0 && api.campfire >= 0 && api.campfire_state >= 0 && api.placing >= 0 &&
              api.unistorm && api.elapsed >= 0 && api.elapsed_part >= 0 && api.attach_guid && api.new_id && api.remove_fire &&
              api.destroy && api.elapsed_on >= 0;
    if (!g_ready) {
        logx::warn("world fires: UNAVAILABLE (TLD API missing)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    const bool fuel = hook("Fire.AddFuel", api.add_fuel, &add_fuel_detour, &g_add_fuel) &&
                      hook("Fire.ReduceDurationByTODSeconds", api.shorten, &shorten_detour, &g_shorten);
    logx::info("world fires: ready (fires are shared)%s", fuel ? "" : "; fuel added won't reach the other player");
    return g_ready;
}

void on_frame(bool in_game) {
    if (!g_ready) return;
    const bool at_menu = !in_game && game::active_scene() == "MainMenu";
    if (at_menu) forget_world();
    if (at_menu && !g_at_menu) forget_dismantled();
    g_at_menu = at_menu;
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
    const std::string& settled_scene = in_game ? core::settled_scene() : std::string();
    if (settled_scene != g_scene) {
        g_scene = settled_scene;
        g_sig.clear();
        g_here.clear();
        g_baseline = false;
        g_together.clear();
        if (!g_scene.empty()) {
            baseline();
            if (hosting())
                apply_stored();
            else if (g_list && g_list->scene == g_scene)
                apply_list(*g_list);
            g_list.reset();
        }
    }
    receive();
    if (g_scene.empty()) return;
    if (const double now = now_ms(); now - g_last_check >= kCheckMs) {
        g_last_check = now;
        check();
    }
    if (!hosting()) return;
    for (uint8_t id : g_together.arrived(g_scene)) send_list(id);  // (see world_items)
}

void save_kept(std::vector<world_keep::Record>& out) {
    for (const auto& [scene, fires] : g_store) {
        for (const auto& [id, m] : fires) {
            net::Writer w(net::Msg::FirePut, net::kMaxMessage);
            net::write_fire_put(w, m);
            if (w.ok()) out.push_back(world_keep::record(world_keep::Tag::FireChange, w));
        }
    }
}

bool load_kept(const world_keep::Record& r) {
    const net::Packet p = world_keep::packet(r);
    net::Reader body = net::packet_body(p);
    net::FirePutMsg m;
    if (p.type != net::Msg::FirePut || !net::read_fire_put(body, m)) return false;
    g_store[m.scene][m.id] = std::move(m);
    return true;
}

}  // namespace world_fires
