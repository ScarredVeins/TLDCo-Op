#include "core.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "avatar.h"
#include "config.h"
#include "dumper.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/held.h"
#include "net_runtime.h"
#include "coop_death.h"
#include "coop_give.h"
#include "coop_lapse.h"
#include "coop_sleep.h"
#include "coop_menu.h"
#include "coop_notices.h"
#include "coop_hud.h"
#include "dev_commands.h"
#include "paths.h"
#include "steam_dispatch.h"
#include "test_harness.h"
#include "ui_inspect.h"
#include "wildlife_carcass.h"
#include "wildlife_hits.h"
#include "wildlife_sync.h"
#include "wildlife_targets.h"
#include "world_containers.h"
#include "world_fires.h"
#include "world_objects.h"
#include "world_items.h"
#include "world_join.h"
#include "world_keep.h"
#include "world_mode.h"
#include "world_sync.h"

namespace core {
namespace {

// The build this code was written and tested against. Other builds still load, with a warning in the log.
constexpr const char* kTestedGameVersion = "2.55 181032 S";

// Bump when the dump format changes so existing dumps are regenerated.
constexpr int kDumpFormat = 2;

// --- type dump, regenerated only when the game build changes --------------------------------------------------

std::string build_stamp() {
    std::ostringstream s;
    s << "format" << kDumpFormat << '|' << paths::game_version();
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW((paths::game_dir() / L"GameAssembly.dll").c_str(), GetFileExInfoStandard, &a))
        s << '|' << a.nFileSizeLow << '|' << a.ftLastWriteTime.dwLowDateTime << '|' << a.ftLastWriteTime.dwHighDateTime;
    return s.str();
}

void dump_types_if_build_changed() {
    const auto dir = paths::mod_dir();
    const auto stamp_file = dir / L"tld_types.stamp";
    const std::string stamp = build_stamp();

    std::string old;
    if (std::ifstream in(stamp_file); in) std::getline(in, old);
    if (old == stamp && std::filesystem::exists(dir / L"tld_types.txt")) {
        logx::info("type dump is current for this build, skipping");
        return;
    }

    DWORD start = GetTickCount();
    if (dumper::dump_types(dir / L"tld_types.txt") > 0) {
        std::ofstream(stamp_file, std::ios::trunc) << stamp << '\n';
        logx::info("type dump took %lu ms", GetTickCount() - start);
    }
}

// --- per-frame callback on the main thread ---------------------------------------------------------------------

using UpdateFn = void (*)(Il2CppObject* self, const MethodInfo* method);
UpdateFn g_game_manager_update = nullptr;
UpdateFn g_game_manager_late_update = nullptr;

void report_steam_once() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    logx::info("steam_api64.dll %s; SteamNetworkingMessages v002: %s, SteamMatchmaking v009: %s",
               steam ? "loaded" : "NOT loaded",
               steam && GetProcAddress(steam, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002") ? "yes" : "no",
               steam && GetProcAddress(steam, "SteamAPI_SteamMatchmaking_v009") ? "yes" : "no");
}

// --- milestone 2: telemetry ------------------------------------------------------------------------------------

constexpr ULONGLONG kSceneCheckMs = 250;
constexpr ULONGLONG kTelemetryMs = 2000;
constexpr int kSnapshotKey = VK_F8;

bool game_window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void log_telemetry(const char* tag, double fps) {
    const game::PlayerState p = game::read_player();
    const game::WorldState w = game::read_world();
    if (!p.valid) {
        logx::info("%s: no player yet", tag);
        return;
    }
    char fps_text[32] = "";
    if (fps > 0) snprintf(fps_text, sizeof(fps_text), " | %.0f fps", fps);
    logx::info("%s: pos=(%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f body=%.1f | day %d %02d:%02d (t=%.4f) | %s(%d) %.1fC "
               "wind %.1fmph %s%s",
               tag, p.position.x, p.position.y, p.position.z, p.yaw, p.pitch, p.body_yaw, w.day, w.hour, w.minute,
               w.normalized_time, w.weather_name.empty() ? "?" : w.weather_name.c_str(), w.weather_stage,
               w.air_temp_c, w.wind_mph, w.indoors ? "indoors" : "outdoors", fps_text);
}

// --- milestone 3: networking -----------------------------------------------------------------------------------

// Capture ~60 Hz so the 20 Hz sender always has a fresh sample; each carries its exact capture time.
constexpr double kNetPushMs = 16;
constexpr double kWorldReadMs = 1000;

bool g_net_enabled = false;
std::string g_settled_scene;  // the scene we report as in (see SceneTransition); "" while loading or in menus
const MethodInfo* g_set_run_in_background = nullptr;
const MethodInfo* g_get_run_in_background = nullptr;

void keep_running_in_background();

void coop_off(const char* why) {
    if (netrt::active()) netrt::stop();
    g_net_enabled = false;
    g_settled_scene.clear();
    logx::info("net: off (%s)", why);
}

// Start, restart or stop co-op to match the settings. Used at startup and by the co-op menu.
void apply_net_settings(const config::Settings& s, bool startup) {
    coop_hud::set_switches(s.hud_names, s.hud_radar, s.hud_chart);
    if (s.mode == "off") return coop_off("solo");
    if (s.mode != "host" && s.mode != "join") {
        logx::warn("net: unknown mode '%s' (use off, host or join)", s.mode.c_str());
        return coop_off("bad mode");
    }
    if (s.transport != "local" && s.transport != "steam") {
        logx::warn("net: unknown transport '%s' (use steam or local)", s.transport.c_str());
        return coop_off("bad transport");
    }
    net::Config c;
    c.role = s.mode == "host" ? net::Role::Host : net::Role::Client;
    c.transport = s.transport == "steam" ? "steam" : "udp";
    c.code = c.role == net::Role::Host ? s.code : s.join_code;
    if (c.transport == "steam" && c.code.empty()) {
        logx::warn("net: no %s set; open the co-op menu to get one", c.role == net::Role::Host ? "host code" : "join code");
        return coop_off("no code");
    }
    c.bind_ip = s.bind_ip;
    c.host_ip = s.host_ip;
    c.port = s.port;
    c.name = s.name;
    c.body = s.body;
    if (!avatar::is_known_body(c.body)) {
        logx::warn("net: unknown body '%s' in coop.ini (choose from: %s); using capsule", c.body.c_str(),
                   avatar::body_ids().c_str());
        c.body = "capsule";
    }
    c.game_version = paths::game_version();
    c.ask_to_join = s.ask_to_join && !startup;  // autostart (testing) lets friends straight in
    avatar::use_tld_shaders(s.body_shader == "tld");
    g_net_enabled = netrt::restart(c);
    // At startup the first frame does this; from the menu, the game is already running.
    if (g_net_enabled && !startup) keep_running_in_background();
}

void start_networking() {
    Il2CppClass* application = il2::find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Application");
    g_set_run_in_background = il2::find_method(application, "set_runInBackground", 1);
    g_get_run_in_background = il2::find_method(application, "get_runInBackground", 0);
    config::Settings s = config::load();
    if (!s.autostart) {
        coop_hud::set_switches(s.hud_names, s.hud_radar, s.hud_chart);
        logx::info("net: offline until CO-OP is picked (NEW, RESUME or LOAD in the Survival menu)");
        return;
    }
    apply_net_settings(s, true);
}

// Unity stops the whole player loop when the window loses focus. In co-op the other player would freeze.
void keep_running_in_background() {
    bool on = true;
    void* args[] = {&on};
    il2::invoke(g_set_run_in_background, nullptr, args);
    bool now_on = false;
    il2::invoke_value(g_get_run_in_background, nullptr, now_on);
    if (now_on)
        logx::info("net: game keeps running when unfocused");
    else
        logx::warn("net: could not enable run-in-background; the game will pause when unfocused");
}

// A scene change (region, building, cave, loading a save) isn't finished when m_ActiveScene changes. The new scene
// may still be loading, and once it is, TLD first puts the player at a default spot (the region's start, for a
// save load) and moves them to where they belong a moment later. So after a change we report "loading" until the
// new scene has run for a while and the player has stopped being moved.
struct SceneTransition {
    bool initialized = false;
    std::string scene;  // the scene changes are measured from
    bool settling = false;
    double started_ms = 0;
    float load_clock_at_change = 0;  // m_SecondsSinceLevelLoad when the name changed
    float lowest_load_clock = 0;
    bool have_sample = false;
    game::Vec3 last_sample;  // player position on the previous push while settling
    double last_jump_ms = 0;
    int jumps = 0;
    bool need_placement = false;  // came from another gameplay scene (a door): TLD must move the player in first
    bool have_last_seen = false;
    game::Vec3 last_seen;         // the player's position on the last push (in whatever scene that was)
    float last_clock = -1;        // the level clock on the previous frame (notice_reload)
};
SceneTransition g_transition;
constexpr float kSettledLoadSeconds = 2.0f;  // the new scene has run this long since it loaded...
constexpr double kStillMs = 1000;            // ...and TLD hasn't moved the player for this long
constexpr float kJumpMetres = 10.0f;         // a move this big between two pushes is TLD placing the player
constexpr double kSettleTimeoutMs = 30000;

float distance(const game::Vec3& a, const game::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// True while the scene we're in shouldn't be reported yet (see SceneTransition). Through a door the name changes (and
// the level clock restarts) while TLD is still loading the other side, with the player where they were; the scene is
// in once TLD has moved them there.
bool scene_settling(const std::string& scene, const game::PlayerState& p, double now) {
    SceneTransition& t = g_transition;
    const float load_clock = game::seconds_since_level_load();
    if (!t.initialized) {  // co-op started here: this scene is already in
        t.initialized = true;
        t.scene = scene;
    }
    if (scene != t.scene) {
        logx::info("scene change '%s' -> '%s': reporting 'loading' until it's in (level clock %.2f s)",
                   t.scene.c_str(), scene.c_str(), load_clock);
        t.need_placement = game::is_gameplay_scene(t.scene) && t.have_last_seen;
        t.scene = scene;
        t.settling = true;
        t.started_ms = now;
        t.load_clock_at_change = t.lowest_load_clock = load_clock;
        t.have_sample = t.need_placement;  // the move from where we were counts as TLD placing us
        t.last_sample = t.last_seen;
        t.last_jump_ms = now;
        t.jumps = 0;
    }
    if (p.valid) {  // (kept through a fade-out with no player)
        t.last_seen = p.position;
        t.have_last_seen = true;
    }
    if (!t.settling) return false;
    if (!p.valid) return true;  // no player yet
    if (t.have_sample && distance(p.position, t.last_sample) > kJumpMetres) {
        t.last_jump_ms = now;
        ++t.jumps;
        logx::info("scene '%s': player placed at (%.1f, %.1f, %.1f) (level clock %.2f s)", scene.c_str(),
                   p.position.x, p.position.y, p.position.z, load_clock);
    }
    t.last_sample = p.position;
    t.have_sample = true;
    t.lowest_load_clock = std::min(t.lowest_load_clock, load_clock);
    // Loaded: the name changed once the scene was in, or the level clock restarted after it changed.
    const bool loaded =
        t.load_clock_at_change < kSettledLoadSeconds || t.lowest_load_clock < t.load_clock_at_change - 0.05f;
    const bool placed = !t.need_placement || t.jumps > 0;
    const bool ready = loaded && placed && load_clock >= kSettledLoadSeconds && now - t.last_jump_ms >= kStillMs;
    const bool timeout = now - t.started_ms >= kSettleTimeoutMs;
    if (!ready && !timeout) return true;
    t.settling = false;
    logx::info("scene '%s' is in after %.0f ms%s: player at (%.1f, %.1f, %.1f), placed %d time(s), level clock %.2f s",
               scene.c_str(), now - t.started_ms, timeout ? " (TIMEOUT)" : "", p.position.x, p.position.y,
               p.position.z, t.jumps, load_clock);
    return false;
}

// TLD loading the scene we're in again, under the same name: its respawn close to where the player died (the same
// region), or a save of this region loaded from the pause menu. The level clock starts over. It counts as a scene
// change (see scene_settling): the world modules let go of what they knew of the scene and look at it afresh once
// it's in, rather than taking the reloaded scene's items for new ones and the old ones for taken. Every frame, before
// the modules, so none of them looks at the new scene as the old one.
void notice_reload(const std::string& scene) {
    SceneTransition& t = g_transition;
    const float clock = game::seconds_since_level_load();
    const float last = t.last_clock;
    t.last_clock = clock;
    if (!t.initialized || t.settling || scene != t.scene || !game::is_gameplay_scene(scene) || last < 0 || clock < 0 ||
        clock >= last - 0.5f)
        return;
    logx::info("scene '%s' loaded again (level clock %.2f s -> %.2f s): reporting 'loading' until it's in",
               scene.c_str(), last, clock);
    const double now = net::now_ms();
    t.settling = true;
    t.started_ms = now;
    t.load_clock_at_change = t.lowest_load_clock = clock;
    t.need_placement = false;
    t.have_sample = false;
    t.last_jump_ms = now;
    t.jumps = 0;
    g_settled_scene.clear();
}

void push_local_state(const std::string& scene) {
    static double last_push = 0, last_world = 0;
    static game::WorldState world;
    const double now = net::now_ms();  // QPC: GetTickCount64's 15.6 ms steps would skew the timestamps
    if (now - last_push < kNetPushMs) return;
    last_push = now;

    net::StateMsg m;
    m.t_ms = now;
    m.scene = scene;
    const game::PlayerState p = game::is_gameplay_scene(scene) ? game::read_player() : game::PlayerState{};
    if (scene_settling(scene, p, now)) {
        g_settled_scene.clear();
        netrt::set_local_state(m);  // valid = false: "loading"
        return;
    }
    g_settled_scene = game::is_gameplay_scene(scene) && p.valid ? scene : std::string();
    if (game::is_gameplay_scene(scene)) {
        if (now - last_world >= kWorldReadMs) {
            world = game::read_world();
            last_world = now;
        }
        m.valid = p.valid;
        m.x = p.position.x;
        m.y = p.position.y;
        m.z = p.position.z;
        m.yaw = p.yaw;
        m.pitch = p.pitch;
        m.normalized_time = world.normalized_time;
        m.day = world.day;
        if (game::in_struggle()) m.flags |= net::kStateStruggling;
        if (coop_death::dead()) m.flags |= net::kStateDead;
        if (p.crouched) m.flags |= net::kStateCrouched;
        if (coop_sleep::in_bed()) {
            m.flags |= net::kStateInBed;
            if (coop_sleep::waiting()) m.flags |= net::kStateWaiting;
            else if (coop_sleep::bed_pose(m.bed)) m.flags |= net::kStateBed;
            m.sleep_vote = coop_sleep::vote();
        }
        const game::HeldState held = game::read_held();
        m.held = held.gear;
        m.held_kind = held.kind;
        m.held_flags = held.flags;
        if (const std::string food = coop_lapse::eating(); !food.empty()) {  // in hand while it's eaten
            m.flags |= net::kStateEating;
            m.held = food;
            m.held_kind = static_cast<uint8_t>(net::HeldKind::Other);
            m.held_flags = 0;
        }
        static std::string last_held = "nothing";
        const std::string now_held = net::describe_held(held.gear, net::held_kind(held.kind), held.flags);
        if (now_held != last_held) {
            logx::info("hands: %s", now_held.c_str());
            last_held = now_held;
        }
    }
    netrt::set_local_state(m);
}

void log_net_status() {
    const net::Status st = netrt::status();
    if (!st.connected) return;  // connect/disconnect events are logged by the session itself
    const auto sent = static_cast<unsigned long long>(st.states_sent);
    const auto received = static_cast<unsigned long long>(st.states_received);
    for (const net::PlayerInfo& p : st.players) {
        const auto r = netrt::remote_state(p.id);
        char rtt[32] = "";
        if (p.linked) snprintf(rtt, sizeof(rtt), " rtt %.2f ms", p.rtt_ms);
        if (r && r->valid)
            logx::info("net: '%s' (player %u)%s | remote %s pos=(%.2f, %.2f, %.2f) yaw=%.1f | sent %llu recv %llu",
                       p.name.c_str(), p.id, rtt, r->scene.c_str(), r->x, r->y, r->z, r->yaw, sent, received);
        else
            logx::info("net: '%s' (player %u)%s | remote not in game | sent %llu recv %llu", p.name.c_str(), p.id, rtt,
                       sent, received);
    }
    logx::info("avatar: %s", avatar::describe().c_str());
}

void update_avatar(const std::string& scene, bool in_game) {
    static std::vector<net::TimedState> incoming;
    static double last_profile_check = 0;
    const double now = net::now_ms();
    if (now - last_profile_check >= 250) {
        last_profile_check = now;
        avatar::set_players(netrt::status().players);
    }
    netrt::drain_remote_states(incoming);
    avatar::push(incoming);
    avatar::update(scene, in_game);
}

// A friend whose link drops mid-game plays on: what they change in the world goes to the host once it's back. Not
// across the main menu (another save may load next).
void keep_world_changes(const std::string& scene, bool in_game) {
    const auto role = netrt::role();
    if (!role || *role != net::Role::Client || scene == "MainMenu")
        netrt::keep_offline(false);
    else if (in_game && netrt::status().connected)
        netrt::keep_offline(true);
}

void on_frame() {
    static bool first = true;
    static ULONGLONG last_scene_check = 0, last_telemetry = 0;
    static unsigned long long frames = 0;
    static std::string scene;
    static bool key_was_down = false;

    const ULONGLONG now = GetTickCount64();
    ++frames;
    if (first) {
        first = false;
        last_telemetry = now;
        logx::info("first GameManager.Update on main thread");
        report_steam_once();
        if (g_net_enabled) keep_running_in_background();
    }

    if (now - last_scene_check >= kSceneCheckMs) {
        last_scene_check = now;
        std::string current = game::active_scene();
        if (current != scene) {
            logx::info("scene: '%s' -> '%s'", scene.c_str(), current.c_str());
            scene = std::move(current);
        }
    }
    const bool in_game = game::is_gameplay_scene(scene);
    ui_inspect::on_frame();
    steam_dispatch::on_frame();
    if (g_net_enabled) notice_reload(scene);
    if (g_net_enabled) push_local_state(scene);
    if (g_net_enabled) world_sync::on_frame(scene, in_game);
    wildlife_sync::on_frame(scene, in_game);  // also tidies up when co-op stops
    wildlife_targets::on_frame();
    wildlife_hits::on_frame();
    wildlife_carcass::on_frame(in_game);
    world_keep::on_frame(scene, in_game);  // first: what the host kept with its save is in before a scene is looked at
    keep_world_changes(scene, in_game);
    world_mode::on_frame(scene, in_game);  // before world_join: a first join loads the host's scene at its mode
    world_items::on_frame(in_game);
    // Objects before containers: a body or a moved container another player left in a scene is made before what was
    // changed in it is put in (the host walking in).
    world_objects::on_frame(in_game);
    world_containers::on_frame(in_game);
    world_fires::on_frame(in_game);
    world_join::on_frame(scene, in_game);
    coop_death::on_frame(scene, in_game);
    coop_lapse::on_frame(scene, in_game);
    coop_sleep::on_frame(scene, in_game);
    coop_give::on_frame(in_game);   // before the avatars: what each body shows of a handover
    update_avatar(scene, in_game);  // also hides the avatar once co-op is switched off
    coop_menu::on_frame();
    if (g_net_enabled) netrt::set_ready(in_game || coop_menu::lobby_ready());  // the lobby's READY / START
    coop_notices::on_frame(in_game);
    coop_hud::on_frame(scene, in_game);
    dev_commands::on_frame(in_game);
    test_harness::on_frame(scene);

    const bool key_down = (GetAsyncKeyState(kSnapshotKey) & 0x8000) != 0;
    if (key_down && !key_was_down && game_window_focused()) {
        if (in_game)
            log_telemetry("F8 snapshot", 0);
        else
            logx::info("F8 snapshot: scene '%s' has no player", scene.c_str());
    }
    key_was_down = key_down;

    if (now - last_telemetry >= kTelemetryMs) {
        const double fps = frames * 1000.0 / static_cast<double>(now - last_telemetry);
        if (in_game) log_telemetry("telemetry", fps);
        if (g_net_enabled) log_net_status();
        frames = 0;
        last_telemetry = now;
    }
}

void game_manager_update_detour(Il2CppObject* self, const MethodInfo* method) {
    g_game_manager_update(self, method);
    on_frame();
}

// After the animation step: what the others hold goes into their avatars' hands, the arms posed over the clip.
void game_manager_late_update_detour(Il2CppObject* self, const MethodInfo* method) {
    g_game_manager_late_update(self, method);
    if (g_net_enabled) avatar::late_update();
    coop_hud::late_update();  // after the avatars moved: names over their heads, the radar
}

// GameManager.Awake runs on every scene load and sets Application.runInBackground = false (inlined icall at
// Awake+0x1a1 in 2.55), so the setting has to be re-applied after each one.
using AwakeFn = void (*)(Il2CppObject* self, const MethodInfo* method);
AwakeFn g_game_manager_awake = nullptr;

void game_manager_awake_detour(Il2CppObject* self, const MethodInfo* method) {
    g_game_manager_awake(self, method);
    if (g_net_enabled || test_harness::runs_unfocused()) keep_running_in_background();
}

// On focus loss TLD sets GameManager.m_IsPaused and pauses audio. In co-op the shared world must keep going.
using FocusFn = void (*)(Il2CppObject* self, bool focused, const MethodInfo* method);
FocusFn g_game_manager_on_focus = nullptr;

void game_manager_on_focus_detour(Il2CppObject* self, bool focused, const MethodInfo* method) {
    if (!focused && g_net_enabled) {
        logx::info("net: window lost focus; co-op keeps the game running");
        return;
    }
    g_game_manager_on_focus(self, focused, method);
}

// TLD's second focus-loss pause (registered in GameManager.Awake): sets m_IsPaused, which stops the clock and the
// world while the window is in the background. Same treatment.
using PauseWhenFocusLostFn = void (*)(bool focus_lost, const MethodInfo* method);
PauseWhenFocusLostFn g_pause_when_focus_lost = nullptr;

void pause_when_focus_lost_detour(bool focus_lost, const MethodInfo* method) {
    if (focus_lost && g_net_enabled) return;
    g_pause_when_focus_lost(focus_lost, method);
}

void install_hooks() {
    Il2CppClass* game_manager = il2::find_class("Assembly-CSharp.dll", "", "GameManager");
    if (const MethodInfo* update = il2::find_method(game_manager, "Update", 0))
        hooks::install_method("GameManager.Update", update, reinterpret_cast<void*>(&game_manager_update_detour),
                              reinterpret_cast<void**>(&g_game_manager_update));
    if (const MethodInfo* late_update = il2::find_method(game_manager, "LateUpdate", 0))
        hooks::install_method("GameManager.LateUpdate", late_update,
                              reinterpret_cast<void*>(&game_manager_late_update_detour),
                              reinterpret_cast<void**>(&g_game_manager_late_update));
    // These only change behaviour while co-op is on (the detours check), but are always installed so the
    // co-op menu can switch co-op on mid-session.
    if (const MethodInfo* awake = il2::find_method(game_manager, "Awake", 0))
        hooks::install_method("GameManager.Awake", awake, reinterpret_cast<void*>(&game_manager_awake_detour),
                              reinterpret_cast<void**>(&g_game_manager_awake));
    if (const MethodInfo* focus = il2::find_method(game_manager, "OnApplicationFocus", 1))
        hooks::install_method("GameManager.OnApplicationFocus", focus,
                              reinterpret_cast<void*>(&game_manager_on_focus_detour),
                              reinterpret_cast<void**>(&g_game_manager_on_focus));
    if (const MethodInfo* pause = il2::find_method(game_manager, "PauseWhenFocusLost", 1))
        hooks::install_method("GameManager.PauseWhenFocusLost", pause,
                              reinterpret_cast<void*>(&pause_when_focus_lost_detour),
                              reinterpret_cast<void**>(&g_pause_when_focus_lost));
}

}  // namespace

void on_runtime_ready() {
    const std::string version = paths::game_version();
    logx::info("IL2CPP runtime ready; game version '%s'", version.c_str());
    if (version != kTestedGameVersion)
        logx::warn("untested game version (tested: '%s'); expect breakage until re-verified", kTestedGameVersion);

    dump_types_if_build_changed();
    game::init();
    avatar::init();
    world_sync::init();
    wildlife_sync::init();
    wildlife_targets::init();
    wildlife_hits::init();
    wildlife_carcass::init();
    world_items::init();
    world_containers::init();
    world_objects::init();
    world_fires::init();
    world_join::init();
    coop_death::init();
    coop_lapse::init();
    coop_sleep::init();  // after coop_lapse: it tells coop_lapse when a sleep's speed-up is the world's
    coop_give::init();
    world_keep::init();
    world_mode::init();
    dev_commands::init();
    test_harness::init();
    steam_dispatch::init();  // before networking: a Steam session may start right away
    start_networking();
    install_hooks();
    ui_inspect::init();
    coop_menu::init();
    coop_notices::init();
    coop_hud::init();
}

void apply_settings(const config::Settings& s) { apply_net_settings(s, false); }

const std::string& settled_scene() { return g_settled_scene; }

void stop_coop(const char* why) {
    if (g_net_enabled || netrt::active()) coop_off(why);
}

}  // namespace core
