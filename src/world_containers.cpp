#include "world_containers.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/uniques.h"
#include "net_runtime.h"

namespace world_containers {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr double kFlushMs = 250;      // changed containers go out at most four times a second
constexpr double kWatchMs = 300;      // the container on screen is checked this often (taking part of a stack doesn't
                                      // go through AddGear / RemoveGear)
constexpr double kAnswerMs = 5000;    // client: how long our own changes to a container wait for the host's answer
constexpr double kOpenDelayMs = 1500; // dev open: the search before the panel shows
constexpr ptrdiff_t kListItems = 0x10, kListSize = 0x18;
constexpr ptrdiff_t kCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr size_t kItemEntry = 16;       // TLD.Gear.GearItemObject in an array: GearItem m_GearItem, string m_GearItemName

struct Vector3 {
    float x, y, z;
};

struct Api {
    // Unity
    const MethodInfo* component_go = nullptr;
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* get_position = nullptr;
    const MethodInfo* object_name = nullptr;
    // ContainerManager / Container
    FieldInfo* containers = nullptr;               // static ContainerManager.m_Containers
    FieldInfo* corpses = nullptr;                  // static ContainerManager.m_CorpseContainers
    ptrdiff_t items = -1;                          // Container.m_Items (List<GearItemObject>)
    ptrdiff_t to_make = -1;                        // Container.m_GearToInstantiate (List<GameObject>): rolled, not made yet
    ptrdiff_t inspected = -1, not_populated = -1, no_save = -1;
    const MethodInfo* serialize = nullptr;         // Container.Serialize()
    const MethodInfo* total_weight = nullptr;      // Container.GetTotalWeightKG() (ItemWeight: int64 units)
    const MethodInfo* deserialize = nullptr;       // Container.Deserialize(string, List<GearItem>)
    const MethodInfo* destroy_all = nullptr;       // Container.DestroyAllGear()
    const MethodInfo* make_contents = nullptr;     // Container.InstantiateContents()
    const MethodInfo* add_gear = nullptr;          // Container.AddGear(GearItem)
    const MethodInfo* remove_gear = nullptr;       // Container.RemoveGear(GearItem, bool)
    const MethodInfo* add_stack = nullptr;         // Container.AddToExistingStackable(GearItem, float, int)
    const MethodInfo* begin_open = nullptr;        // Container.BeginContainerOpen(): the search starts
    const MethodInfo* show = nullptr;              // Container.ShowContainerContents(Action): the panel opens
    const MethodInfo* guid_of = nullptr;           // static ObjectGuid.GetGuidFromGameObject(GameObject, bool)
    ptrdiff_t lock = -1, safe = -1;                // Container.m_Lock, m_Safe (in its save text: Lock / SafeCracking)
    ptrdiff_t lock_state = -1;                     // Lock.m_LockState (0 locked)
    ptrdiff_t safe_cracked = -1;                   // SafeCracking.m_Cracked
    // Panel_Container
    const MethodInfo* panel_enable = nullptr;      // Panel_Container.Enable(bool, bool, Action)
    const MethodInfo* panel_done = nullptr;        // Panel_Container.OnDone()
    const MethodInfo* panel_pack_list = nullptr;   // Panel_Container.UpdateFilteredInventoryList()
    const MethodInfo* panel_list = nullptr;        // Panel_Container.UpdateFilteredContainerList()
    const MethodInfo* panel_tables = nullptr;      // Panel_Container.RefreshTables()
    ptrdiff_t panel_container = -1;                // Panel_Container.m_Container
    const MethodInfo* load_panel = nullptr;        // static InterfaceManager.LoadPanel(Type)
    Il2CppObject* panel_type = nullptr;            // typeof(Panel_Container)
    // Player
    const MethodInfo* player_manager = nullptr;    // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* inventory = nullptr;         // static GameManager.GetInventoryComponent()
    const MethodInfo* to_pack = nullptr;           // PlayerManager.AddItemToPlayerInventory(GearItem, bool, bool)
    const MethodInfo* pack_remove = nullptr;       // Inventory.RemoveGear(GameObject, bool)
    ptrdiff_t inv_items = -1;                      // Inventory.m_Items (List<GearItemObject>)
    const MethodInfo* being_inspected = nullptr;   // PlayerManager.GearItemBeingInspected()
    const MethodInfo* inspecting = nullptr;        // PlayerManager.IsInspectModeActive()
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

Vector3 position_of(Il2CppObject* component) {
    Vector3 p{};
    il2::invoke_value(api.get_position, il2::invoke(api.component_transform, component), p);
    return p;
}

// TLD's own id for the container (what ContainerManager.Serialize saves it by); name and position (cm) without one.
std::string id_of(Il2CppObject* c) {
    void* args[] = {il2::invoke(api.component_go, c), nullptr};
    bool warn = false;
    args[1] = &warn;
    std::string id = args[0] ? text(reinterpret_cast<Il2CppString*>(il2::invoke(api.guid_of, nullptr, args))) : std::string();
    if (!id.empty()) return id;
    const Vector3 p = position_of(c);
    char key[160];
    snprintf(key, sizeof(key), "%s|%ld|%ld|%ld", name_of(c).c_str(), std::lround(p.x * 100), std::lround(p.y * 100),
             std::lround(p.z * 100));
    char out[24];
    snprintf(out, sizeof(out), "@%016llx", static_cast<unsigned long long>(fnv(key)));
    return out;
}

std::vector<Il2CppObject*> all_containers() {
    std::vector<Il2CppObject*> out;
    std::unordered_set<Il2CppObject*> seen;
    for (FieldInfo* field : {api.containers, api.corpses}) {
        if (!field) continue;
        Il2CppObject* list = nullptr;
        il2::il2cpp_field_static_get_value(field, &list);
        if (!list) continue;
        Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
        const int32_t count = get<int32_t>(list, kListSize);
        for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
            Il2CppObject* c = il2::array_object_at(items, static_cast<size_t>(i));
            if (alive(c) && !get<bool>(c, api.no_save) && seen.insert(c).second) out.push_back(c);
        }
    }
    return out;
}

// The GearItems in a container (Container.m_Items) and the prefabs it rolled but hasn't made yet.
std::vector<Il2CppObject*> gear_in(Il2CppObject* c) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* list = get<Il2CppObject*>(c, api.items);
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

std::vector<std::string> rolled_in(Il2CppObject* c) {
    std::vector<std::string> out;
    Il2CppObject* list = get<Il2CppObject*>(c, api.to_make);
    if (!list) return out;
    Il2CppObject* items = get<Il2CppObject*>(list, kListItems);
    const int32_t count = get<int32_t>(list, kListSize);
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i)
        if (Il2CppObject* prefab = il2::array_object_at(items, static_cast<size_t>(i)); alive(prefab))
            out.push_back(name_of(prefab));
    return out;
}

// "GEAR_Hacksaw, 2 GEAR_Stick, (rolled) GEAR_Coffee": what's in it, sorted, for the log.
std::string contents_of(Il2CppObject* c) {
    std::vector<std::string> names;
    for (Il2CppObject* gi : gear_in(c)) names.push_back(name_of(gi));
    for (const std::string& n : rolled_in(c)) names.push_back("(rolled) " + n);
    std::sort(names.begin(), names.end());
    std::string out;
    for (size_t i = 0; i < names.size();) {
        size_t j = i;
        while (j < names.size() && names[j] == names[i]) ++j;
        if (!out.empty()) out += ", ";
        if (j - i > 1) out += std::to_string(j - i) + " ";
        out += names[i];
        i = j;
    }
    return out.empty() ? "empty" : out;
}

std::string serialize(Il2CppObject* c) { return text(reinterpret_cast<Il2CppString*>(il2::invoke(api.serialize, c))); }

// What the other games get: our one-of-a-kind items (net/uniques.h) stay in our copy only.
std::string shared_text(Il2CppObject* c) { return net::strip_uniques(serialize(c)); }

// What's in it and how much it all weighs: changes when an item (or part of a stack, or some water) goes in or out, not
// when food slowly spoils (Serialize's text changes all the time while the game runs).
std::string signature(Il2CppObject* c) {
    int64_t units = 0;
    il2::invoke_value(api.total_weight, c, units);
    return contents_of(c) + " | " + std::to_string(units);
}

// --- state ---------------------------------------------------------------------------------------------------------

std::string g_scene;                                    // the scene the state below is about ("" = none)
std::unordered_map<std::string, Il2CppObject*> g_by_id; // this scene's containers
std::unordered_map<std::string, std::string> g_last;    // id -> its save text, as last sent, answered or applied
std::unordered_map<std::string, std::string> g_sig;     // id -> its signature then: what both games have
std::unordered_map<std::string, Il2CppObject*> g_dirty; // id -> changed here (to compare with g_last and send)
std::unordered_map<std::string, double> g_asked;        // client: id -> when we last asked the host (one question
                                                        // per opening: the search and the panel both ask)
std::unordered_set<std::string> g_waiting;              // client: ids whose answer hasn't come yet
netrt::Newest g_newest;                                 // id -> each player's newest change applied
uint32_t g_seq = 0;
double g_last_flush = 0, g_last_watch = 0;
bool g_applying = false;                                // our own Deserialize: not a change to send
bool g_connected = false;
Il2CppObject* g_panel = nullptr;                        // Panel_Container (from its Enable)
Il2CppObject* g_shown = nullptr;                        // the container the panel shows (ShowContainerContents, until
                                                        // the panel's Enable(false))
// Host: contents for scenes we aren't in: what we last had there, and the client's changes there (put in when we
// get there).
struct Stored {
    std::string text;
    bool from_peer = false;
};
std::unordered_map<std::string, std::unordered_map<std::string, Stored>> g_store;
// Dev open: the panel shows after the search.
Il2CppObject* g_opening = nullptr;
double g_open_at = 0;

bool tracking() { return g_ready && g_connected && !g_scene.empty() && !g_applying; }

// What we keep of the world for later (changes for scenes someone isn't in) belongs to this game: a dropped link
// keeps it, back at the main menu (another save may load next) it goes.
void forget_world() {
    if (g_store.empty()) return;
    logx::info("containers: back at the main menu; forgetting contents kept for %zu scene(s)", g_store.size());
    g_store.clear();
}

Il2CppObject* find(const std::string& id) {
    if (const auto it = g_by_id.find(id); it != g_by_id.end() && alive(it->second)) return it->second;
    g_by_id.clear();
    for (Il2CppObject* c : all_containers()) g_by_id[id_of(c)] = c;
    const auto it = g_by_id.find(id);
    return it != g_by_id.end() ? it->second : nullptr;
}

Il2CppObject* shown_container() {
    if (!alive(g_panel) || !alive(g_shown) || get<Il2CppObject*>(g_panel, api.panel_container) != g_shown) return nullptr;
    return g_shown;
}

void refresh_panel() {
    il2::invoke(api.panel_pack_list, g_panel);
    il2::invoke(api.panel_list, g_panel);
    il2::invoke(api.panel_tables, g_panel);
}

void mark(Il2CppObject* c) {
    if (!tracking() || !alive(c) || get<bool>(c, api.no_save)) return;
    g_dirty[id_of(c)] = c;
}

// --- sending -------------------------------------------------------------------------------------------------------

// To every friend (the host) or, `to`, one friend (an answer).
void send_put(const std::string& id, const std::string& contents, uint8_t flags, const std::string& scene, uint8_t to = 0) {
    net::ContainerPutMsg m;
    m.scene = scene;
    m.seq = ++g_seq;
    m.flags = flags;
    m.id = id;
    m.text = contents;
    if (m.text.size() > net::kContainerTextMax) {
        logx::warn("containers: %s is too big to send (%zu chars)", short_id(id).c_str(), m.text.size());
        return;
    }
    net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
    net::write_container_put(w, m);
    if (to)
        netrt::send_packet_to(to, w, true);
    else
        netrt::send_packet(w, true);
}

// Host: what one friend changed goes on to the others, as a change of ours (our counter: each friend's is its own).
void pass_on(uint8_t from, net::ContainerPutMsg m) {
    m.seq = ++g_seq;
    net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
    net::write_container_put(w, m);
    netrt::send_packet_except(from, w, true);
}

void flush(double now) {
    for (auto it = g_dirty.begin(); it != g_dirty.end();) {
        const std::string& id = it->first;
        Il2CppObject* c = it->second;
        if (g_waiting.count(id)) {
            if (now - g_asked[id] < kAnswerMs) {  // the host's contents come first
                ++it;
                continue;
            }
            logx::warn("containers: no answer from the host about %s (%s); ours stands", name_of(c).c_str(),
                       short_id(id).c_str());
            g_waiting.erase(id);
        }
        if (alive(c)) {
            std::string sig = signature(c);
            if (const auto known = g_sig.find(id); known == g_sig.end() || known->second != sig) {
                std::string contents = shared_text(c);
                if (!contents.empty()) {
                    send_put(id, contents, 0, g_scene);
                    logx::info("containers: %s (%s) changed here: %s; told the other player (%zu chars)", name_of(c).c_str(),
                               short_id(id).c_str(), contents_of(c).c_str(), contents.size());
                    g_last[id] = std::move(contents);
                    g_sig[id] = std::move(sig);
                }
            }
        }
        it = g_dirty.erase(it);
    }
}

void ask(Il2CppObject* c) {
    if (!tracking() || hosting() || !alive(c) || !netrt::status().connected) return;  // offline: ours stands
    const std::string id = id_of(c);
    const double now = now_ms();
    if (const auto it = g_asked.find(id); it != g_asked.end() && now - it->second < kAnswerMs) return;
    g_asked[id] = now;
    g_waiting.insert(id);
    net::ContainerWantMsg m;
    m.scene = g_scene;
    m.id = id;
    net::Writer w(net::Msg::ContainerWant);
    net::write_container_want(w, m);
    netrt::send_packet(w, true);
    logx::info("containers: opening %s (%s); asked the host what's in it", name_of(c).c_str(), short_id(id).c_str());
}

// --- applying the other player's contents --------------------------------------------------------------------------

// A container's save text carries its lock and safe. Those only ever open in play (world_objects shares that), and
// another player's text may be older than our opening it (their answer crossing our change): never shut one again.
struct Opened {
    Il2CppObject* lock = nullptr;
    Il2CppObject* safe = nullptr;
    int32_t lock_state = 0;
    bool cracked = false;
};

Opened opened(Il2CppObject* c) {
    Opened o;
    if (api.lock >= 0 && api.lock_state >= 0) o.lock = get<Il2CppObject*>(c, api.lock);
    if (api.safe >= 0 && api.safe_cracked >= 0) o.safe = get<Il2CppObject*>(c, api.safe);
    if (!alive(o.lock)) o.lock = nullptr;
    if (!alive(o.safe)) o.safe = nullptr;
    if (o.lock) o.lock_state = get<int32_t>(o.lock, api.lock_state);
    if (o.safe) o.cracked = get<bool>(o.safe, api.safe_cracked);
    return o;
}

void keep_opened(const Opened& before) {
    if (before.lock && before.lock_state != 0 && get<int32_t>(before.lock, api.lock_state) == 0)
        put<int32_t>(before.lock, api.lock_state, before.lock_state);
    if (before.safe && before.cracked && !get<bool>(before.safe, api.safe_cracked)) put<bool>(before.safe, api.safe_cracked, true);
}

void apply(Il2CppObject* c, const std::string& id, const std::string& theirs, const char* what) {
    const bool on_screen = shown_container() == c;
    const Opened before = opened(c);
    // Theirs, without their one-of-a-kind items and with ours (each game keeps its own in its copy).
    const std::string contents = net::merge_uniques(theirs, serialize(c));
    g_applying = true;
    il2::invoke(api.destroy_all, c);
    void* args[] = {il2::il2cpp_string_new(contents.c_str()), nullptr};
    il2::invoke(api.deserialize, c, args);
    keep_opened(before);
    g_last[id] = theirs;
    g_sig[id] = signature(c);  // what the other player has
    g_dirty.erase(id);
    if (on_screen) {  // what the panel does when it opens (the rolled items are made then), and a refresh
        il2::invoke(api.make_contents, c);
        refresh_panel();
        g_dirty[id] = c;  // items made from what it rolled go back, so both games have the same ones
    }
    g_applying = false;
    logx::info("containers: %s %s (%s): %s%s", what, name_of(c).c_str(), short_id(id).c_str(), contents_of(c).c_str(),
               on_screen ? " (on screen, refreshed)" : "");
}

void apply_stored() {
    const auto it = g_store.find(g_scene);
    if (it == g_store.end()) return;
    int applied = 0;
    for (const auto& [id, s] : it->second) {
        if (!s.from_peer) continue;
        if (Il2CppObject* c = find(id)) {
            apply(c, id, s.text, "the other player changed, while we were elsewhere,");
            ++applied;
        } else {
            logx::warn("containers: the other player changed %s in %s, but it isn't here", short_id(id).c_str(), g_scene.c_str());
        }
    }
    g_store.erase(it);  // our containers here are what counts now
    if (applied) logx::info("containers: %d container(s) the other player changed in %s while we were elsewhere", applied, g_scene.c_str());
}

// Host: friend `from` is opening this container.
void answer(const net::ContainerWantMsg& m, uint8_t from) {
    std::string contents;
    const char* source = "nothing known";
    if (m.scene == g_scene) {
        if (Il2CppObject* c = find(m.id)) {
            contents = shared_text(c);
            g_last[m.id] = contents;
            g_sig[m.id] = signature(c);
            g_dirty.erase(m.id);
            source = "ours";
        }
    } else if (const auto s = g_store.find(m.scene); s != g_store.end()) {
        if (const auto it = s->second.find(m.id); it != s->second.end()) {
            contents = net::strip_uniques(it->second.text);  // (kept from before, or a body's whole pack)
            source = "what we last knew";
        }
    }
    send_put(m.id, contents, net::kContainerAnswer | (contents.empty() ? net::kContainerUnknown : 0), m.scene, from);
    logx::info("containers: the other player is opening %s in %s; sent %s (%zu chars)", short_id(m.id).c_str(),
               m.scene.c_str(), source, contents.size());
}

void receive_put(const net::Packet& p, const net::ContainerPutMsg& m) {
    if (!g_newest.take(p, m.id, m.seq)) return;  // a late older change
    const bool answer = m.flags & net::kContainerAnswer;
    if (hosting() && !answer) pass_on(p.from, m);
    if (m.scene != g_scene) {
        if (hosting() && !answer) {
            g_store[m.scene][m.id] = Stored{m.text, true};
            logx::info("containers: the other player changed %s in %s; kept for when we get there", short_id(m.id).c_str(),
                       m.scene.c_str());
        }
        return;  // client: we ask when we open it
    }
    Il2CppObject* c = find(m.id);
    if (!c) {
        logx::warn("containers: the other player's container %s isn't in our %s", short_id(m.id).c_str(), g_scene.c_str());
        return;
    }
    if (answer) {
        g_waiting.erase(m.id);
        if (m.flags & net::kContainerUnknown) {  // the host has nothing for it: ours stands, and the host gets it
            g_sig.erase(m.id);
            g_dirty[m.id] = c;
            logx::info("containers: the host has nothing for %s (%s); ours stands", name_of(c).c_str(), short_id(m.id).c_str());
            return;
        }
        if (const std::string ours = serialize(c); ours == net::merge_uniques(m.text, ours)) {
            g_last[m.id] = m.text;
            g_sig[m.id] = signature(c);
            logx::info("containers: the host's %s (%s) is what we have: %s", name_of(c).c_str(), short_id(m.id).c_str(),
                       contents_of(c).c_str());
            return;
        }
        apply(c, m.id, m.text, "the host's");
        return;
    }
    apply(c, m.id, m.text, "the other player changed");
}

void receive() {
    std::vector<net::Packet> packets;
    netrt::drain_packets(net::Msg::ContainerPut, packets);
    netrt::drain_packets(net::Msg::ContainerWant, packets);
    for (const net::Packet& p : packets) {
        net::Reader r = net::packet_body(p);
        if (p.type == net::Msg::ContainerPut) {
            net::ContainerPutMsg m;
            if (net::read_container_put(r, m)) receive_put(p, m);
        } else if (p.type == net::Msg::ContainerWant) {
            net::ContainerWantMsg m;
            if (net::read_container_want(r, m) && hosting()) answer(m, p.from);
        }
    }
}

// --- hooks ---------------------------------------------------------------------------------------------------------

using AddGearFn = void (*)(Il2CppObject* self, Il2CppObject* gi, const MethodInfo* method);
using RemoveGearFn = void (*)(Il2CppObject* self, Il2CppObject* gi, bool to_inventory, const MethodInfo* method);
using AddStackFn = bool (*)(Il2CppObject* self, Il2CppObject* gi, float condition, int32_t units, const MethodInfo* method);
using VoidFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using ShowFn = void (*)(Il2CppObject* self, Il2CppObject* on_closed, const MethodInfo* method);
using PanelEnableFn = void (*)(Il2CppObject* self, bool enable, bool reset_filter, Il2CppObject* on_closed, const MethodInfo* method);
AddGearFn g_add_gear = nullptr;
RemoveGearFn g_remove_gear = nullptr;
AddStackFn g_add_stack = nullptr;
VoidFn g_begin_open = nullptr;
ShowFn g_show = nullptr;
PanelEnableFn g_panel_enable = nullptr;

void add_gear_detour(Il2CppObject* self, Il2CppObject* gi, const MethodInfo* method) {
    g_add_gear(self, gi, method);
    mark(self);
}

void remove_gear_detour(Il2CppObject* self, Il2CppObject* gi, bool to_inventory, const MethodInfo* method) {
    g_remove_gear(self, gi, to_inventory, method);
    mark(self);
}

bool add_stack_detour(Il2CppObject* self, Il2CppObject* gi, float condition, int32_t units, const MethodInfo* method) {
    const bool added = g_add_stack(self, gi, condition, units, method);
    if (added) mark(self);
    return added;
}

void begin_open_detour(Il2CppObject* self, const MethodInfo* method) {
    ask(self);
    g_begin_open(self, method);
}

void show_detour(Il2CppObject* self, Il2CppObject* on_closed, const MethodInfo* method) {
    ask(self);  // opened without a search (crafting, rock caches...): ask now
    g_show(self, on_closed, method);
    if (alive(g_panel) && get<Il2CppObject*>(g_panel, api.panel_container) == self) g_shown = self;
    mark(self);  // the panel makes what the container rolled
}

void panel_enable_detour(Il2CppObject* self, bool enable, bool reset_filter, Il2CppObject* on_closed, const MethodInfo* method) {
    g_panel = self;
    if (!enable) g_shown = nullptr;
    g_panel_enable(self, enable, reset_filter, on_closed, method);
}

// --- test helpers --------------------------------------------------------------------------------------------------

Vector3 player_pos() {
    const game::PlayerState p = game::read_player();
    return Vector3{p.position.x, p.position.y, p.position.z};
}

Il2CppObject* nearest(const std::string& name, float max_metres, float* metres = nullptr) {
    const Vector3 me = player_pos();
    Il2CppObject* best = nullptr;
    float best_d = max_metres;
    for (Il2CppObject* c : all_containers()) {
        const float d = distance(position_of(c), me);
        if (d <= best_d && contains_nocase(name_of(c), name)) best = c, best_d = d;
    }
    if (metres) *metres = best_d;
    return best;
}

// The container on screen, or the nearest one.
Il2CppObject* working_container() {
    if (Il2CppObject* c = shown_container()) return c;
    return nearest("", 6.0f);
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

void fill(Il2CppObject* c, const std::string& contents) {
    if (!g_ready || !alive(c)) return;
    apply(c, id_of(c), contents, "another game's body:");
}

void told(Il2CppObject* c) {
    if (!g_ready || !alive(c)) return;
    const std::string id = id_of(c);
    g_last[id] = shared_text(c);
    g_sig[id] = signature(c);
    g_dirty.erase(id);
}

void describe(float metres) {
    if (!g_ready) return;
    const Vector3 me = player_pos();
    std::vector<std::pair<float, Il2CppObject*>> nearby;
    int total = 0, without_id = 0;
    for (Il2CppObject* c : all_containers()) {
        ++total;
        const float d = distance(position_of(c), me);
        if (d <= metres) nearby.emplace_back(d, c);
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, c] : nearby)
        if (id_of(c)[0] == '@') ++without_id;
    logx::info("dev: containers: %zu within %.0f m (%d in %s)%s", nearby.size(), metres, total, g_scene.c_str(),
               without_id ? " (some without a TLD id)" : "");
    for (size_t i = 0; i < nearby.size() && i < 25; ++i) {
        Il2CppObject* c = nearby[i].second;
        const std::string id = id_of(c), contents = serialize(c);
        const Vector3 p = position_of(c);
        logx::info("dev:   %s %s at (%.1f, %.1f, %.1f), %.1f m, %s%s, %zu chars #%08llx: %s", short_id(id).c_str(),
                   name_of(c).c_str(), p.x, p.y, p.z, nearby[i].first, get<bool>(c, api.inspected) ? "searched" : "not searched",
                   get<bool>(c, api.not_populated) ? ", not populated" : "", contents.size(),
                   static_cast<unsigned long long>(fnv(contents) & 0xFFFFFFFFull), contents_of(c).c_str());
    }
}

bool open(const std::string& name, bool search) {
    const char* verb = search ? "search" : "open";
    if (!g_ready) return false;
    float d = 0;
    Il2CppObject* c = nearest(name, 6.0f, &d);
    if (!c) {
        logx::warn("dev: %s: no container%s%s within 6 m", verb, name.empty() ? "" : " like ", name.c_str());
        return false;
    }
    if (search) il2::invoke(api.begin_open, c);  // what holding the interact button starts (the search)
    g_opening = c;
    g_open_at = now_ms() + (search ? kOpenDelayMs : 0);
    logx::info("dev: %s: %s %s (%s), %.1f m away", verb, search ? "searching" : "opening", name_of(c).c_str(),
               short_id(id_of(c)).c_str(), d);
    return true;
}

bool close() {
    if (!g_ready) return false;
    Il2CppObject* c = shown_container();
    if (!c) {
        logx::info("dev: close: no container on screen");
        return false;
    }
    il2::invoke(api.panel_done, g_panel);
    logx::info("dev: close: closed %s (%s)", name_of(c).c_str(), short_id(id_of(c)).c_str());
    return true;
}

bool take(const std::string& name) {
    if (!g_ready) return false;
    Il2CppObject* c = working_container();
    Il2CppObject* gi = nullptr;
    if (c)
        for (Il2CppObject* candidate : gear_in(c))
            if (contains_nocase(name_of(candidate), name)) gi = candidate;
    if (!gi) {
        logx::warn("dev: take: no %s in %s", name.c_str(), c ? name_of(c).c_str() : "any container within 6 m");
        return false;
    }
    const std::string what = name_of(gi);
    Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
    bool track = true, notify = true, to_inventory = true;
    void* args[] = {gi, &track, &notify};
    il2::invoke(api.to_pack, pm, args);
    void* remove_args[] = {gi, &to_inventory};
    il2::invoke(api.remove_gear, c, remove_args);
    if (shown_container() == c) refresh_panel();
    logx::info("dev: take: %s from %s (%s); left: %s", what.c_str(), name_of(c).c_str(), short_id(id_of(c)).c_str(),
               contents_of(c).c_str());
    return true;
}

bool put(const std::string& name) {
    if (!g_ready) return false;
    Il2CppObject* c = working_container();
    Il2CppObject* gi = nullptr;
    for (Il2CppObject* candidate : pack_items())
        if (contains_nocase(name_of(candidate), name)) gi = candidate;
    if (!c || !gi) {
        logx::warn("dev: put: %s", !c ? "no container on screen or within 6 m" : ("no " + name + " in our pack").c_str());
        return false;
    }
    const std::string what = name_of(gi);
    void* add_args[] = {gi};
    il2::invoke(api.add_gear, c, add_args);
    bool to_container = true;
    void* remove_args[] = {il2::invoke(api.component_go, gi), &to_container};
    il2::invoke(api.pack_remove, il2::invoke(api.inventory, nullptr), remove_args);
    if (shown_container() == c) refresh_panel();
    logx::info("dev: put: %s into %s (%s); now: %s", what.c_str(), name_of(c).c_str(), short_id(id_of(c)).c_str(),
               contents_of(c).c_str());
    return true;
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* manager = il2::find_class(kGame, "", "ContainerManager");
    Il2CppClass* container = il2::find_class(kGame, "", "Container");
    Il2CppClass* panel = il2::find_class(kGame, "", "Panel_Container");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* player_manager = il2::find_class(kGame, "", "PlayerManager");
    Il2CppClass* inventory = il2::find_class(kGame, "", "Inventory");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.object_name = il2::find_method(object, "get_name", 0);
    if (manager) {
        api.containers = il2::il2cpp_class_get_field_from_name(manager, "m_Containers");
        api.corpses = il2::il2cpp_class_get_field_from_name(manager, "m_CorpseContainers");
    }
    api.items = il2::field_offset(container, "m_Items");
    api.to_make = il2::field_offset(container, "m_GearToInstantiate");
    api.inspected = il2::field_offset(container, "m_Inspected");
    api.not_populated = il2::field_offset(container, "m_NotPopulated");
    api.no_save = il2::field_offset(container, "m_DisableSerialization");
    api.serialize = il2::find_method(container, "Serialize", 0);
    api.deserialize = il2::find_method(container, "Deserialize", 2);
    api.destroy_all = il2::find_method(container, "DestroyAllGear", 0);
    api.make_contents = il2::find_method(container, "InstantiateContents", 0);
    api.add_gear = il2::find_method(container, "AddGear", 1);
    api.remove_gear = il2::find_method_params(container, "RemoveGear", {"GearItem", "System.Boolean"});
    api.add_stack = il2::find_method(container, "AddToExistingStackable", 3);
    api.begin_open = il2::find_method(container, "BeginContainerOpen", 0);
    api.show = il2::find_method(container, "ShowContainerContents", 1);
    api.guid_of = il2::find_method_params(il2::find_class(kGame, "", "ObjectGuid"), "GetGuidFromGameObject",
                                          {"UnityEngine.GameObject", "System.Boolean"});
    api.lock = il2::field_offset(container, "m_Lock");
    api.safe = il2::field_offset(container, "m_Safe");
    api.lock_state = il2::field_offset(il2::find_class(kGame, "", "Lock"), "m_LockState");
    api.safe_cracked = il2::field_offset(il2::find_class(kGame, "", "SafeCracking"), "m_Cracked");
    api.panel_enable = il2::find_method(panel, "Enable", 3);
    api.panel_done = il2::find_method(panel, "OnDone", 0);
    api.panel_pack_list = il2::find_method(panel, "UpdateFilteredInventoryList", 0);
    api.panel_list = il2::find_method(panel, "UpdateFilteredContainerList", 0);
    api.panel_tables = il2::find_method(panel, "RefreshTables", 0);
    api.panel_container = il2::field_offset(panel, "m_Container");
    api.load_panel = il2::find_method_params(il2::find_class(kGame, "", "InterfaceManager"), "LoadPanel", {"System.Type"});
    api.panel_type = il2::type_object(panel);
    if (api.panel_type) il2::il2cpp_gchandle_new(api.panel_type, false);
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.inventory = il2::find_method(game_manager, "GetInventoryComponent", 0);
    api.to_pack = il2::find_method(player_manager, "AddItemToPlayerInventory", 3);
    api.pack_remove = il2::find_method_params(inventory, "RemoveGear", {"UnityEngine.GameObject", "System.Boolean"});
    api.inv_items = il2::field_offset(inventory, "m_Items");
    api.being_inspected = il2::find_method(player_manager, "GearItemBeingInspected", 0);
    api.inspecting = il2::find_method(player_manager, "IsInspectModeActive", 0);
    api.total_weight = il2::find_method(container, "GetTotalWeightKG", 0);
    g_ready = api.component_go && api.component_transform && api.get_position && api.object_name && api.containers &&
              api.items >= 0 && api.to_make >= 0 && api.inspected >= 0 && api.not_populated >= 0 && api.no_save >= 0 &&
              api.serialize && api.total_weight && api.deserialize && api.destroy_all && api.make_contents && api.add_gear && api.remove_gear &&
              api.add_stack && api.begin_open && api.show && api.guid_of && api.panel_enable &&
              api.panel_done && api.panel_pack_list && api.panel_list && api.panel_tables && api.panel_container >= 0;
    if (!g_ready) {
        logx::warn("world containers: UNAVAILABLE (TLD API missing)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    // Changes are what counts; the panel and the opening only make them reach the other player sooner.
    const bool changes = hook("Container.AddGear", api.add_gear, &add_gear_detour, &g_add_gear) &&
                         hook("Container.RemoveGear", api.remove_gear, &remove_gear_detour, &g_remove_gear) &&
                         hook("Container.AddToExistingStackable", api.add_stack, &add_stack_detour, &g_add_stack);
    const bool refreshes = hook("Panel_Container.Enable", api.panel_enable, &panel_enable_detour, &g_panel_enable);
    const bool opening = hook("Container.BeginContainerOpen", api.begin_open, &begin_open_detour, &g_begin_open) &&
                         hook("Container.ShowContainerContents", api.show, &show_detour, &g_show);
    g_ready = changes;
    logx::info("world containers: %s%s%s", changes ? "ready (container contents are shared)" : "UNAVAILABLE (hooks failed)",
               refreshes ? "" : "; open panels won't refresh", opening ? "" : "; opening doesn't ask the host");
    return g_ready;
}

void on_frame(bool in_game) {
    if (!g_ready) return;
    if (!in_game && game::active_scene() == "MainMenu") forget_world();
    const double now = now_ms();
    if (g_opening && now >= g_open_at) {  // dev open: the search is over
        Il2CppObject* c = g_opening;
        g_opening = nullptr;
        if (alive(c)) {  // the panel opens only if TLD has it loaded (PanelReference.TryGetPanel): load it as TLD does
            void* load_args[] = {api.panel_type};
            if (api.load_panel && api.panel_type) il2::invoke(api.load_panel, nullptr, load_args);
            void* args[] = {nullptr};
            il2::invoke(api.show, c, args);
            Il2CppObject* pm = il2::invoke(api.player_manager, nullptr);
            bool inspecting = false;
            Il2CppObject* found = pm && il2::invoke_value(api.inspecting, pm, inspecting) && inspecting
                                      ? il2::invoke(api.being_inspected, pm) : nullptr;
            logx::info("dev: open: %s (%s) %s%s: %s", name_of(c).c_str(), short_id(id_of(c)).c_str(),
                       shown_container() == c ? "is on screen" : alive(found) ? "shows what the search found, one at a time, starting with " : "did NOT open",
                       shown_container() != c && alive(found) ? name_of(found).c_str() : "", contents_of(c).c_str());
        }
    }
    const bool connected = netrt::status().connected;
    if (!connected && !netrt::offline()) {
        if (g_connected) {
            logx::info("containers: not connected; nothing to share");
            g_last.clear();
            g_sig.clear();
            g_dirty.clear();
            g_asked.clear();
            g_waiting.clear();
            g_newest.clear();
        }
        g_connected = false;
        return;
    }
    // Offline (a friend's link dropped mid-game, or we host with nobody linked): we go on. The friend's changes go to
    // the host once it's back; the host applies what it kept for a scene it walks into (kept with its save, too).
    if (!connected) g_newest.clear();  // the host's change counters may start over meanwhile (its game restarted)
    g_connected = true;
    const std::string& settled = in_game ? core::settled_scene() : std::string();
    if (settled != g_scene) {
        if (hosting() && !g_scene.empty()) {  // what we had there answers the other player while we're away
            auto& stored = g_store[g_scene];
            for (auto& [id, contents] : g_last)
                if (!contents.empty()) stored[id] = Stored{std::move(contents), false};
        }
        g_last.clear();
        g_sig.clear();
        g_dirty.clear();
        g_asked.clear();
        g_waiting.clear();
        g_by_id.clear();
        g_shown = nullptr;
        g_scene = settled;
        if (hosting() && !g_scene.empty()) apply_stored();
    }
    receive();
    if (g_scene.empty()) return;
    if (now - g_last_watch >= kWatchMs) {
        g_last_watch = now;
        if (Il2CppObject* c = shown_container()) mark(c);
    }
    if (now - g_last_flush >= kFlushMs && !g_dirty.empty()) {
        g_last_flush = now;
        flush(now);
    }
}

void save_kept(std::vector<world_keep::Record>& out) {
    for (const auto& [scene, containers] : g_store) {
        for (const auto& [id, s] : containers) {
            net::ContainerPutMsg m;
            m.scene = scene;
            m.id = id;
            m.text = s.text;
            net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
            net::write_container_put(w, m);
            if (w.ok())
                out.push_back(world_keep::record(s.from_peer ? world_keep::Tag::ContainerTheirs : world_keep::Tag::ContainerOurs, w));
        }
    }
}

bool load_kept(const world_keep::Record& r) {
    const net::Packet p = world_keep::packet(r);
    net::Reader body = net::packet_body(p);
    net::ContainerPutMsg m;
    if (p.type != net::Msg::ContainerPut || !net::read_container_put(body, m)) return false;
    g_store[m.scene][m.id] = Stored{std::move(m.text), r.tag == world_keep::Tag::ContainerTheirs};
    return true;
}

}  // namespace world_containers
