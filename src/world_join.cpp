#include "world_join.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

#include "core.h"
#include "game_state.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"
#include "paths.h"
#include "world_mode.h"

namespace world_join {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kJoinedFile = "joined.txt";  // one line per save that joined a world: "<our save> <host world>"
constexpr double kLoadTimeoutMs = 180000;  // a region load plus TLD placing us; a slow disk can take a minute
constexpr int kMaxTries = 3;               // the host may move on while we load; follow that far, then stay
constexpr float kBesideOutdoors = 1.5f;    // metres to the host's right
constexpr float kBesideIndoors = 1.0f;

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
    const MethodInfo* load_scene = nullptr;      // static GameManager.LoadSceneWithLoadingScreen(string)
    const MethodInfo* stop_audio = nullptr;      // static GameAudioManager.StopAll()
    FieldInfo* transition = nullptr;             // static GameManager.m_SceneTransitionData
    ptrdiff_t force_next = -1;                   // SceneTransitionData.m_ForceNextSceneLoadTriggerScene
    ptrdiff_t last_outdoor = -1;                 // SceneTransitionData.m_LastOutdoorScene
    const MethodInfo* player_manager = nullptr;  // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* teleport = nullptr;        // PlayerManager.TeleportPlayer(Vector3, Quaternion)
    const MethodInfo* navmesh_sample = nullptr;  // static NavMesh.SamplePosition(Vector3, out hit, float, int)
} api;

bool g_ready = false;

enum class Step { Waiting, Loading, Done };
Step g_step = Step::Waiting;
bool g_force = false;      // dev: go to the host even if this save has joined this world before
std::string g_key;         // "<our save> <host world>" of this join
std::string g_target;      // the scene we're loading to be with the host
std::string g_region;      // its region when it's an interior ("" outdoors)
double g_load_started = 0;
int g_tries = 0;

std::set<std::string> g_joined;  // what joined.txt says
bool g_joined_loaded = false;

double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool is_client() {
    const auto role = netrt::role();
    return role && *role == net::Role::Client;
}

void start_over() {
    g_step = Step::Waiting;
    g_key.clear();
    g_target.clear();
    g_region.clear();
    g_tries = 0;
}

bool joined_before(const std::string& key) {
    if (!g_joined_loaded) {
        g_joined_loaded = true;
        std::ifstream in(paths::instance_dir() / kJoinedFile);
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) g_joined.insert(line);
    }
    return g_joined.count(key) != 0;
}

void remember_joined(const std::string& key) {
    if (key.empty() || !g_joined.insert(key).second) return;
    std::ofstream out(paths::instance_dir() / kJoinedFile, std::ios::app);
    out << key << "\n";
    logx::info("join: first time in this world from this save; next time we stay where our save has us");
}

void set_text(Il2CppObject* obj, ptrdiff_t off, const std::string& value) {
    Il2CppString* s = il2::il2cpp_string_new(value.c_str());
    il2::il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(s), false);  // kept alive (no write barrier here)
    memcpy(reinterpret_cast<char*>(obj) + off, &s, sizeof(s));
}

// A point near `want` on the walkable mesh, or `want` itself if there's none within 15 m.
Vector3 on_navmesh(Vector3 want) {
    NavMeshHit hit{};
    float radius = 15.0f;
    int32_t all_areas = -1;
    void* args[] = {&want, &hit, &radius, &all_areas};
    bool found = false;
    if (api.navmesh_sample && il2::invoke_value(api.navmesh_sample, nullptr, found, args) && found) return hit.position;
    return want;
}

void go(const std::string& scene, const std::string& region, const std::string& here) {
    Il2CppObject* transition = nullptr;
    if (api.transition) il2::il2cpp_field_static_get_value(api.transition, &transition);
    if (!region.empty() && transition && api.force_next >= 0 && api.last_outdoor >= 0) {
        // What going in through the building's door would have set: its exit then lets out in the host's region.
        set_text(transition, api.force_next, region);
        set_text(transition, api.last_outdoor, region);
    }
    if (api.stop_audio) il2::invoke(api.stop_audio, nullptr);
    void* args[] = {il2::il2cpp_string_new(scene.c_str())};
    il2::invoke(api.load_scene, nullptr, args);
    g_step = Step::Loading;
    g_target = scene;
    g_region = region;
    g_load_started = now_ms();
    ++g_tries;
    logx::info("join: the host is in %s%s%s%s; going there (we're in %s)", scene.c_str(), region.empty() ? "" : " (a building in ",
               region.c_str(), region.empty() ? "" : ")", here.c_str());
}

// Stand beside another player (to their right), facing them. How far from them we ended up; < 0: not in a game.
float place_beside(const net::StateMsg& other, bool indoors) {
    const float metres = indoors ? kBesideIndoors : kBesideOutdoors;
    const float yaw = other.yaw * 3.14159265f / 180.0f;
    const float rx = std::cos(yaw), rz = -std::sin(yaw);  // their right (Unity: yaw 0 faces +Z, 90 faces +X)
    Vector3 pos{other.x + rx * metres, other.y, other.z + rz * metres};
    if (!indoors) pos = on_navmesh(pos);  // on a slope their height would put us inside the hill
    pos.y += 0.3f;
    const float face = std::atan2(other.x - pos.x, other.z - pos.z);
    Quaternion rot{0, std::sin(face / 2), 0, std::cos(face / 2)};
    Il2CppObject* player = il2::invoke(api.player_manager, nullptr);
    if (!player) return -1;
    void* args[] = {&pos, &rot};
    il2::invoke(api.teleport, player, args);
    const float dx = pos.x - other.x, dy = pos.y - other.y, dz = pos.z - other.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void arrive(const net::StateMsg& host) {
    const float metres = place_beside(host, !g_region.empty());
    if (metres < 0) return;
    logx::info("join: in %s now; standing beside the host (%.1f m from them)", host.scene.c_str(), metres);
    remember_joined(g_key);
    g_step = Step::Done;
    g_force = false;
}

}  // namespace

bool stand_beside(const net::StateMsg& other) {
    if (!g_ready || !other.valid) return false;
    const float metres = place_beside(other, game::read_world().indoors);
    if (metres < 0) return false;
    logx::info("join: standing beside player %u in %s (%.1f m from them)", other.player, other.scene.c_str(), metres);
    return true;
}

bool init() {
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    api.load_scene = il2::find_method(game_manager, "LoadSceneWithLoadingScreen", 1);
    api.stop_audio = il2::find_method(il2::find_class(kGame, "", "GameAudioManager"), "StopAll", 0);
    if (game_manager) api.transition = il2::il2cpp_class_get_field_from_name(game_manager, "m_SceneTransitionData");
    Il2CppClass* transition = il2::find_class(kGame, "", "SceneTransitionData");
    api.force_next = il2::field_offset(transition, "m_ForceNextSceneLoadTriggerScene");
    api.last_outdoor = il2::field_offset(transition, "m_LastOutdoorScene");
    api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    api.teleport = il2::find_method_params(il2::find_class(kGame, "", "PlayerManager"), "TeleportPlayer",
                                           {"UnityEngine.Vector3", "UnityEngine.Quaternion"});
    api.navmesh_sample = il2::find_method_params(il2::find_class("UnityEngine.AIModule.dll", "UnityEngine.AI", "NavMesh"),
                                                 "SamplePosition",
                                                 {"UnityEngine.Vector3", "UnityEngine.AI.NavMeshHit&", "System.Single", "System.Int32"});
    g_ready = api.load_scene && api.player_manager && api.teleport;
    if (!g_ready) {
        logx::warn("world join: UNAVAILABLE (TLD API missing): a joining friend stays where their save is");
        return false;
    }
    logx::info("world join: ready (a friend joining a world for the first time goes to the host)%s",
               api.transition && api.force_next >= 0 && api.last_outdoor >= 0 ? "" : "; not into buildings (no scene override)");
    return true;
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    if (scene == "MainMenu") {
        if (g_step != Step::Waiting) logx::info("join: back at the main menu; the next load checks again");
        start_over();
        g_force = false;
        return;
    }
    if (!in_game || !is_client() || g_step == Step::Done) return;
    const std::string& here = core::settled_scene();
    if (here.empty()) return;  // loading, or TLD is still placing us
    const auto host = netrt::remote_state();
    const bool host_in = netrt::status().connected && host && host->valid && !host->scene.empty();

    if (g_step == Step::Loading) {
        if (here != g_target) {
            if (now_ms() - g_load_started > kLoadTimeoutMs) {
                logx::warn("join: %s didn't load (still in %s); staying here", g_target.c_str(), here.c_str());
                g_step = Step::Done;
            }
            return;
        }
        if (host_in && host->scene == here) {
            arrive(*host);
        } else if (g_tries < kMaxTries) {
            logx::info("join: in %s now, but the host has moved on (%s); following", here.c_str(),
                       host_in ? host->scene.c_str() : "not in the game");
            g_step = Step::Waiting;
        } else {
            logx::info("join: in %s now, but the host has moved on again; staying here", here.c_str());
            g_step = Step::Done;
        }
        return;
    }

    // Waiting: the first time we see the host in the game since this game was loaded.
    if (!host_in) return;
    const auto world = netrt::remote_world();
    if (!world || !world->valid) return;  // its region and which world it is come about once a second
    if (!world_mode::ready()) return;     // the host's experience mode first: the scene loads at it
    const std::string ours = game::save_id();
    if (g_key.empty()) g_key = ours.empty() || world->world_id.empty() ? std::string() : ours + " " + world->world_id;
    if (!g_force && g_tries == 0) {
        if (!g_key.empty() && joined_before(g_key)) {
            logx::info("join: this save (%s) has joined the host's world (%s) before; staying where it has us (%s)",
                       ours.c_str(), world->world_id.c_str(), here.c_str());
            g_step = Step::Done;
            return;
        }
        if (g_key.empty() && host->scene == here) {  // a host that doesn't say which world: same scene, stay
            logx::info("join: the host is here too (%s); staying where we are", here.c_str());
            g_step = Step::Done;
            return;
        }
    }
    g_region = !world->region.empty() && world->region != host->scene ? world->region : std::string();
    if (host->scene == here) {
        g_target = here;
        arrive(*host);
        return;
    }
    go(host->scene, g_region, here);
}

void go_to_host() {
    if (!g_ready || !is_client()) {
        logx::warn("dev: join: only a joined friend goes to the host");
        return;
    }
    logx::info("dev: join: going to the host as on a first join");
    start_over();
    g_force = true;
}

}  // namespace world_join
