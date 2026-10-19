#include "world_mode.h"

#include <algorithm>
#include <array>
#include <vector>

#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace world_mode {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr double kHostCheckMs = 2000;     // host: how often our mode is compared with what we last sent
constexpr double kWaitForHostMs = 10000;  // client: how long world_join waits for the host's mode once linked

struct Api {
    const MethodInfo* current_type = nullptr; // static ExperienceModeManager.GetCurrentExperienceModeType()
    Il2CppClass* mode_type = nullptr;         // enum ExperienceModeType
    const MethodInfo* instance = nullptr;     // static ExperienceModeManager.get_Instance()
    const MethodInfo* serialize = nullptr;    // ExperienceModeManager.Serialize()
    const MethodInfo* deserialize = nullptr;  // ExperienceModeManager.Deserialize(string)
    FieldInfo* current = nullptr;             // static GameModeConfig ExperienceModeManager.s_CurrentGameMode
    // The game's random seed: TLD's rolls (spawn chances of containers, corpses, gear and plants, what a container
    // holds, gear condition) start from GameManager.GetRandomSeed(x) = x ^ this (^ where we came in from, indoors).
    FieldInfo* transition = nullptr;          // static SceneTransitionData GameManager.m_SceneTransitionData
    ptrdiff_t seed = -1;                      // SceneTransitionData.m_GameRandomSeed
    ptrdiff_t entry = -1;                     // SceneTransitionData.m_PosBeforeInteriorLoad
    const MethodInfo* random_seed = nullptr;  // static GameManager.GetRandomSeed(int)
} api;

bool g_ready = false;

// Host.
std::array<uint32_t, net::kMaxPlayers> g_told{};  // the link each friend heard our mode on (0: not yet)
std::string g_sent;
int32_t g_sent_seed = 0;
double g_last_check = -1e18;

// Client.
std::string g_host;              // the host's mode text, newest
bool g_applied = false;          // the host's mode is in place in this game (or was ours anyway); until the main menu
bool g_overriding = false;       // ours differs from the host's and the host's is in place
bool g_applying = false;         // we're calling Deserialize ourselves
std::string g_own;               // our save's own mode text
Il2CppObject* g_own_config = nullptr;  // its GameModeConfig (an addressable asset TLD keeps loaded; held by a handle)
uintptr_t g_own_handle = 0;
double g_linked_at = -1;
int g_restores_logged = 0;
bool g_have_seed = false;  // the host's seed is known (while linked to it): TLD's rolls here use it
int32_t g_host_seed = 0;

using SerializeFn = Il2CppString* (*)(Il2CppObject* self, const MethodInfo* method);
using DeserializeFn = void (*)(Il2CppObject* self, Il2CppString* text, const MethodInfo* method);
using SaveInfoFn = void (*)(Il2CppObject* slot, const MethodInfo* method);
using RandomSeedFn = int32_t (*)(int32_t seed, const MethodInfo* method);
SerializeFn g_serialize = nullptr;
DeserializeFn g_deserialize = nullptr;
SaveInfoFn g_save_info = nullptr;
RandomSeedFn g_random_seed = nullptr;

bool is_client() {
    const auto role = netrt::role();
    return role && *role == net::Role::Client;
}

bool is_host() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

// "Stalker" from {"m_CurrentModeType":"Stalker",...}.
std::string mode_name(const std::string& text) {
    const std::string key = "\"m_CurrentModeType\":\"";
    const size_t at = text.find(key);
    if (at == std::string::npos) return "?";
    const size_t end = text.find('"', at + key.size());
    return end == std::string::npos ? "?" : text.substr(at + key.size(), end - at - key.size());
}

Il2CppObject* manager() { return api.instance ? il2::invoke(api.instance, nullptr) : nullptr; }

Il2CppObject* current_config() {
    Il2CppObject* config = nullptr;
    if (api.current) il2::il2cpp_field_static_get_value(api.current, &config);
    return config;
}

bool set_current_config(Il2CppObject* config) {
    if (!api.current) return false;
    il2::il2cpp_field_static_set_value(api.current, config);
    return current_config() == config;
}

// Our game's random seed field (SceneTransitionData.m_GameRandomSeed); null before TLD has one.
int32_t* seed_field() {
    Il2CppObject* data = nullptr;
    if (api.transition) il2::il2cpp_field_static_get_value(api.transition, &data);
    return data && api.seed >= 0 ? reinterpret_cast<int32_t*>(reinterpret_cast<char*>(data) + api.seed) : nullptr;
}

struct Vector3 {
    float x, y, z;
};

// Where we came in from, going indoors (SceneTransitionData.m_PosBeforeInteriorLoad: the door's spot outside); null
// before TLD has it.
Vector3* entry_field() {
    Il2CppObject* data = nullptr;
    if (api.transition) il2::il2cpp_field_static_get_value(api.transition, &data);
    return data && api.entry >= 0 ? reinterpret_cast<Vector3*>(reinterpret_cast<char*>(data) + api.entry) : nullptr;
}

// TLD rolling something here: with the host's seed, the way the host's game rolls it (the same spot, the same
// result), so a crate, a corpse or a can the host has is here too. Our own seed stays ours (and in our save).
// Indoors TLD also mixes in where the player came in from (GetRandomSeed: seed ^ x ^ hash of
// m_PosBeforeInteriorLoad), so two players coming in by different doors roll a building differently (Camp Office:
// other loose items, other things in 4 of its 25 containers). In co-op every game (the host's too) rolls as if from
// (0, 0, 0): a building's crates, corpses and cans are the same for everyone, whoever walked in first and by
// whichever door. The field is put back at once (TLD puts us there on the way out).
int32_t random_seed_detour(int32_t seed, const MethodInfo* method) {
    int32_t* own = g_have_seed ? seed_field() : nullptr;
    Vector3* entry = is_host() || is_client() ? entry_field() : nullptr;
    if (!own && !entry) return g_random_seed(seed, method);
    const int32_t keep = own ? *own : 0;
    const Vector3 keep_entry = entry ? *entry : Vector3{};
    if (own) *own = g_host_seed;
    if (entry) *entry = Vector3{0, 0, 0};
    const int32_t out = g_random_seed(seed, method);
    if (own) *own = keep;
    if (entry) *entry = keep_entry;
    return out;
}

// What TLD would save as our mode right now (with the host's in place: our own, see serialize_detour).
std::string serialize_now() {
    Il2CppObject* m = manager();
    Il2CppString* s = m ? reinterpret_cast<Il2CppString*>(il2::invoke(api.serialize, m)) : nullptr;
    return s ? il2::to_utf8(s) : std::string();
}

void remember_own_config() {
    Il2CppObject* config = current_config();
    if (config == g_own_config) return;
    if (g_own_handle) il2::il2cpp_gchandle_free(g_own_handle);
    g_own_config = config;
    g_own_handle = config ? il2::il2cpp_gchandle_new(config, false) : 0;
}

bool apply_host() {
    Il2CppObject* m = manager();
    if (!m) return false;
    g_applying = true;
    void* args[] = {il2::il2cpp_string_new(g_host.c_str())};
    il2::invoke(api.deserialize, m, args);
    g_applying = false;
    return true;
}

// The host's mode in place of ours (the first time in this game, or the host changed it).
void take_host_mode() {
    if (!g_overriding) {
        g_own = serialize_now();
        if (g_own.empty()) return;  // no ExperienceModeManager yet; next frame
        remember_own_config();
    }
    if (g_own == g_host) {
        if (g_overriding || !g_applied)
            logx::info("mode: the host plays at %s, as our save does", mode_name(g_host).c_str());
        if (g_overriding) {  // the host went back to ours
            g_overriding = false;
            g_applying = true;
            void* args[] = {il2::il2cpp_string_new(g_own.c_str())};
            il2::invoke(api.deserialize, manager(), args);
            g_applying = false;
        }
        g_applied = true;
        return;
    }
    if (!apply_host()) return;
    g_overriding = true;
    g_applied = true;
    logx::info("mode: playing at the host's %s (our save's is %s; it keeps that)", mode_name(g_host).c_str(),
               mode_name(g_own).c_str());
}

// `put_back`: in the game, our own mode goes back on now (at the main menu TLD sets its menu mode itself, and the next
// load restores that save's).
void stop_overriding(const char* why, bool put_back) {
    if (g_overriding) logx::info("mode: %s; our own mode again from here", why);
    if (g_overriding && put_back && !g_own.empty()) {
        g_applying = true;
        void* args[] = {il2::il2cpp_string_new(g_own.c_str())};
        if (Il2CppObject* m = manager()) il2::invoke(api.deserialize, m, args);
        g_applying = false;
    }
    g_overriding = false;
    g_applied = false;
    g_own.clear();
}

// TLD restoring a save's global data (a save load, and every scene load) sets the save's mode: that's our own, and
// the host's goes back on right after.
void deserialize_detour(Il2CppObject* self, Il2CppString* text, const MethodInfo* method) {
    if (!g_overriding || g_applying) return g_deserialize(self, text, method);
    g_deserialize(self, text, method);
    const std::string restored = text ? il2::to_utf8(text) : std::string();
    if (!restored.empty()) g_own = restored;
    remember_own_config();
    g_applying = true;
    g_deserialize(self, il2::il2cpp_string_new(g_host.c_str()), method);
    g_applying = false;
    if (g_restores_logged < 10) {
        ++g_restores_logged;
        logx::info("mode: TLD restored our save's %s; the host's %s again", mode_name(g_own).c_str(),
                   mode_name(g_host).c_str());
    }
}

// Saving: our own mode goes into our save.
Il2CppString* serialize_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_overriding && !g_applying && !g_own.empty()) return il2::il2cpp_string_new(g_own.c_str());
    return g_serialize(self, method);
}

void save_info_detour(Il2CppObject* slot, const MethodInfo* method) {
    Il2CppObject* host_config = g_overriding ? current_config() : nullptr;
    const bool swapped = host_config && g_own_config && g_own_config != host_config && set_current_config(g_own_config);
    g_save_info(slot, method);
    if (swapped && !set_current_config(host_config)) logx::warn("mode: couldn't put the host's mode back after saving");
}

void host_frame() {
    const double now = net::now_ms();
    const net::Status st = netrt::status();
    if (!st.connected) {
        g_told.fill(0);
        return;
    }
    // A friend who just linked hears our mode right away; everyone hears a change (checked every kHostCheckMs).
    std::vector<uint8_t> fresh;
    for (const net::PlayerInfo& p : st.players)
        if (p.linked && p.id < net::kMaxPlayers && g_told[p.id] != p.link) fresh.push_back(p.id);
    if (fresh.empty() && now - g_last_check < kHostCheckMs) return;
    g_last_check = now;
    const std::string text = serialize_now();
    const int32_t* seed = seed_field();
    if (text.empty() || !seed) return;
    net::Writer w(net::Msg::GameMode, net::kMaxMessage);
    net::write_game_mode(w, text, *seed);
    if (text != g_sent || *seed != g_sent_seed) {
        netrt::send_packet(w, true);
        logx::info("mode: the other player plays at our %s (our game's seed %d)", mode_name(text).c_str(), *seed);
        g_sent = text;
        g_sent_seed = *seed;
        for (const net::PlayerInfo& p : st.players)
            if (p.linked && p.id < net::kMaxPlayers) g_told[p.id] = p.link;
        return;
    }
    for (uint8_t id : fresh) {
        netrt::send_packet_to(id, w, true);
        g_told[id] = std::find_if(st.players.begin(), st.players.end(), [&](const net::PlayerInfo& p) { return p.id == id; })->link;
        logx::info("mode: player %u plays at our %s", id, mode_name(text).c_str());
    }
}

void client_frame(bool in_game) {
    static std::vector<net::Packet> in;
    in.clear();
    netrt::drain_packets(net::Msg::GameMode, in);
    for (const net::Packet& p : in) {
        net::Reader r = net::packet_body(p);
        std::string text;
        int32_t seed = 0;
        if (!net::read_game_mode(r, text, seed) || text.empty()) continue;
        if (!g_have_seed || seed != g_host_seed) {
            const int32_t* own = seed_field();
            logx::info("mode: TLD's rolls here start from the host's game seed %d (ours %s)", seed,
                       own ? std::to_string(*own).c_str() : "not set yet");
        }
        g_host_seed = seed;
        g_have_seed = api.random_seed != nullptr;
        if (text != g_host) {
            g_host = std::move(text);
            if (g_applied) g_applied = false;  // a change: take it again
        }
    }
    if (netrt::status().connected) {
        if (g_linked_at < 0) g_linked_at = net::now_ms();
    } else {
        g_linked_at = -1;
    }
    if (in_game && !g_host.empty() && !g_applied) take_host_mode();
}

}  // namespace

bool init() {
    Il2CppClass* em = il2::find_class(kGame, "", "ExperienceModeManager");
    api.current_type = il2::find_method(em, "GetCurrentExperienceModeType", 0);
    api.mode_type = il2::find_class(kGame, "", "ExperienceModeType");
    api.instance = il2::find_method(em, "get_Instance", 0);
    api.serialize = il2::find_method(em, "Serialize", 0);
    api.deserialize = il2::find_method(em, "Deserialize", 1);
    if (em) api.current = il2::il2cpp_class_get_field_from_name(em, "s_CurrentGameMode");
    const MethodInfo* save_info = il2::find_method(il2::find_class(kGame, "", "SaveGameSystem"), "SaveInfoData", 1);
    Il2CppClass* gm = il2::find_class(kGame, "", "GameManager");
    if (gm) api.transition = il2::il2cpp_class_get_field_from_name(gm, "m_SceneTransitionData");
    api.seed = il2::field_offset(il2::find_class(kGame, "", "SceneTransitionData"), "m_GameRandomSeed");
    api.entry = il2::field_offset(il2::find_class(kGame, "", "SceneTransitionData"), "m_PosBeforeInteriorLoad");
    api.random_seed = il2::find_method(gm, "GetRandomSeed", 1);
    g_ready = api.instance && api.serialize && api.deserialize && api.current && save_info &&
              hooks::install_method("ExperienceModeManager.Deserialize", api.deserialize,
                                    reinterpret_cast<void*>(&deserialize_detour), reinterpret_cast<void**>(&g_deserialize)) &&
              hooks::install_method("ExperienceModeManager.Serialize", api.serialize,
                                    reinterpret_cast<void*>(&serialize_detour), reinterpret_cast<void**>(&g_serialize)) &&
              hooks::install_method("SaveGameSystem.SaveInfoData", save_info, reinterpret_cast<void*>(&save_info_detour),
                                    reinterpret_cast<void**>(&g_save_info));
    if (!g_ready) {
        logx::warn("world mode: UNAVAILABLE (TLD API missing): a friend plays at their own save's experience mode");
        return false;
    }
    const bool seeded = api.transition && api.seed >= 0 && api.random_seed &&
                        hooks::install_method("GameManager.GetRandomSeed", api.random_seed,
                                              reinterpret_cast<void*>(&random_seed_detour),
                                              reinterpret_cast<void**>(&g_random_seed));
    if (!seeded) api.random_seed = nullptr;
    logx::info("world mode: ready (a friend plays at the host's experience mode; their save keeps its own)%s",
               seeded ? (api.entry >= 0 ? "; TLD's rolls use the host's game seed, indoors as if everyone came in from one spot"
                                        : "; TLD's rolls use the host's game seed (indoors they still depend on the door)")
                      : "; the game seed ISN'T shared (spawn chances may differ)");
    return true;
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    if (scene == "MainMenu") {
        stop_overriding("back at the main menu", false);
        g_told.fill(0);
        g_sent.clear();
        return;  // (the host's seed stays: a friend loading in rolls with it from the first frame)
    }
    if (is_host()) return host_frame();
    if (!is_client()) {
        stop_overriding("co-op is off", true);
        g_host.clear();
        g_have_seed = false;
        return;
    }
    client_frame(in_game);
}

void describe() {
    int32_t type = -1;
    if (api.current_type) il2::invoke_value(api.current_type, nullptr, type);
    const std::string in_place = api.mode_type && type >= 0 ? il2::enum_name(api.mode_type, type) : std::string("?");
    const std::string saved = g_ready ? serialize_now() : std::string();
    const int32_t* seed = seed_field();
    logx::info("dev: mode: in place %s (%d); a save writes %s; %s; game seed %s%s", in_place.c_str(), type,
               mode_name(saved).c_str(),
               g_overriding ? ("the host's " + mode_name(g_host) + " over our own " + mode_name(g_own)).c_str()
                            : "our own (nothing overridden)",
               seed ? std::to_string(*seed).c_str() : "?",
               g_have_seed ? (", rolls use the host's " + std::to_string(g_host_seed)).c_str() : "");
}

bool ready() {
    if (!g_ready || !is_client()) return true;
    if (g_applied) return true;
    return g_linked_at >= 0 && net::now_ms() - g_linked_at > kWaitForHostMs;
}

}  // namespace world_mode
