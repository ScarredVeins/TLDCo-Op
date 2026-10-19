#include "world_sync.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "game_state.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace world_sync {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";

constexpr double kPublishMs = 1000;           // host: read and hand over the world
constexpr double kApplyMs = 500;              // client: compare and correct
constexpr float kClockToleranceMinutes = 1;   // game minutes of drift before the clock is snapped
constexpr float kProgressTolerance = 0.03f;   // weather set progress drift before it is re-synced
constexpr double kWeatherRecheckMs = 30000;   // ...checked at most this often
constexpr float kWindAngleTolerance = 25;     // degrees
constexpr double kWindRecheckMs = 60000;      // our own wind wandered off: correct it at most this often
constexpr int32_t kUndefinedStage = 11;       // WeatherStage.Undefined

// List<T> in IL2CPP: object header, then T[] _items, int _size.
constexpr ptrdiff_t kListItems = 0x10;
constexpr ptrdiff_t kListSize = 0x18;

struct Api {
    const MethodInfo* get_tod = nullptr;         // static GameManager.GetTimeOfDayComponent()
    const MethodInfo* get_weather = nullptr;     // static GameManager.GetWeatherComponent()
    const MethodInfo* get_transition = nullptr;  // static GameManager.GetWeatherTransitionComponent()
    const MethodInfo* get_wind = nullptr;        // static GameManager.GetWindComponent()

    const MethodInfo* tod_time = nullptr;        // TimeOfDay.GetNormalizedTime()
    const MethodInfo* tod_set_time = nullptr;    // TimeOfDay.SetNormalizedTime(float, bool forceUpdate)
    const MethodInfo* tod_day = nullptr;         // TimeOfDay.GetDayNumber()
    const MethodInfo* tod_day_length = nullptr;  // TimeOfDay.GetDayLengthSeconds()
    const MethodInfo* tod_time_lapse = nullptr;  // TimeOfDay.IsTimeLapseActive()

    const MethodInfo* weather_stage = nullptr;   // Weather.GetWeatherStage()
    const MethodInfo* weather_set_by_name = nullptr;  // Weather.GetWeatherSetByName(string)
    ptrdiff_t weather_sets_for_scene = -1;       // Weather.m_WeatherSetsForScene (List<WeatherSetData>)

    ptrdiff_t transition_set = -1;               // WeatherTransition.m_CurrentWeatherSet
    ptrdiff_t transition_previous = -1;          // WeatherTransition.m_PreviousWeatherSetType
    const MethodInfo* transition_activate = nullptr;  // ActivateWeatherSet(WeatherSetData, float, WeatherStage)
    const MethodInfo* transition_activate_type = nullptr;  // ActivateWeatherSetAtFrac(WeatherStage, float)

    const MethodInfo* set_serialize = nullptr;   // WeatherSetData.Serialize()
    const MethodInfo* set_deserialize = nullptr; // WeatherSetData.Deserialize(string)
    const MethodInfo* set_prepare = nullptr;     // WeatherSetData.Prepare()
    const MethodInfo* set_progress = nullptr;    // WeatherSetData.GetProgressFrac()
    ptrdiff_t set_type = -1;                     // WeatherSetData.m_CharacterizingType
    const MethodInfo* object_name = nullptr;     // UnityEngine.Object.get_name()

    const MethodInfo* wind_strength = nullptr;   // Wind.GetStrength()
    const MethodInfo* wind_angle = nullptr;      // Wind.GetWindAngle_Base()
    const MethodInfo* wind_start = nullptr;      // Wind.StartPhase(WindStrength, float angle)
    const MethodInfo* wind_start_now = nullptr;  // Wind.StartPhaseImmediate(WindStrength, float angle)

    FieldInfo* scene_transition = nullptr;       // static GameManager.m_SceneTransitionData
    ptrdiff_t last_outdoor = -1;                 // SceneTransitionData.m_LastOutdoorScene

    Il2CppClass* stage_enum = nullptr;
    Il2CppClass* wind_enum = nullptr;
} api;

bool g_ready = false;

// Host.
double g_last_publish = 0;
std::string g_published_set;  // last set logged
void* g_listed_sets_for = nullptr;  // the Weather component whose set list was logged

// Client: what we last did to follow the host, so a set this region lacks isn't retried every tick.
struct Applied {
    bool any = false;  // synced at least once since connecting / entering the game
    std::string set;
    int32_t stage = -1;
    double weather_at = -1e18;
    int32_t wind_strength = -1;
    float wind_angle = 0;
    double wind_at = -1e18;
    bool clock_waiting = false;  // logged "waiting for our time lapse to end"
} g_applied;
double g_last_apply = 0;
std::string g_apply_scene;  // our scene when we last followed the host

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

std::string clock_text(float normalized) {
    float t = normalized - std::floor(normalized);
    int minutes = static_cast<int>(t * 1440.0f + 0.5f) % 1440;
    char s[8];
    snprintf(s, sizeof(s), "%02d:%02d", minutes / 60, minutes % 60);
    return s;
}

std::string stage_name(int32_t stage) { return il2::enum_name(api.stage_enum, stage); }
std::string wind_name(int32_t strength) { return il2::enum_name(api.wind_enum, strength); }

std::string name_of(Il2CppObject* obj) {
    return obj ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, obj))) : std::string();
}

Il2CppString* text(const std::string& s) { return il2::il2cpp_string_new(s.c_str()); }

float angle_between(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180 ? 360 - d : d;
}

// The weather sets this scene can use, logged once per scene: the names the host may send.
void list_sets_once(Il2CppObject* weather) {
    if (!weather || weather == g_listed_sets_for) return;
    g_listed_sets_for = weather;
    Il2CppObject* list = il2::read_field<Il2CppObject*>(weather, api.weather_sets_for_scene);
    Il2CppObject* items = il2::read_field<Il2CppObject*>(list, kListItems);
    const int32_t count = il2::read_field<int32_t>(list, kListSize);
    std::string names;
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < il2::array_length(items); ++i) {
        Il2CppObject* set = il2::array_object_at(items, static_cast<size_t>(i));
        if (!set) continue;
        names += (names.empty() ? "" : ", ") + name_of(set) + " (" +
                 stage_name(il2::read_field<int32_t>(set, api.set_type)) + ")";
    }
    logx::info("world: %d weather sets here: %s", count, names.c_str());
}

// --- host ------------------------------------------------------------------------------------------------------------

net::WorldMsg read_world(bool in_game) {
    net::WorldMsg w;
    if (!in_game) return w;
    Il2CppObject* tod = il2::invoke(api.get_tod, nullptr);
    Il2CppObject* weather = il2::invoke(api.get_weather, nullptr);
    Il2CppObject* transition = il2::invoke(api.get_transition, nullptr);
    Il2CppObject* wind = il2::invoke(api.get_wind, nullptr);
    if (!tod || !weather || !transition || !wind) return w;

    il2::invoke_value(api.tod_time, tod, w.normalized_time);
    il2::invoke_value(api.tod_day, tod, w.day);
    il2::invoke_value(api.tod_day_length, tod, w.day_length_s);
    il2::invoke_value(api.tod_time_lapse, tod, w.time_lapse);

    il2::invoke_value(api.weather_stage, weather, w.weather_stage);
    if (Il2CppObject* set = il2::read_field<Il2CppObject*>(transition, api.transition_set)) {
        w.weather_set = name_of(set);
        w.weather_set_state = il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.set_serialize, set)));
        il2::invoke_value(api.set_progress, set, w.weather_progress);
    }
    w.weather_previous = il2::read_field<int32_t>(transition, api.transition_previous);

    il2::invoke_value(api.wind_strength, wind, w.wind_strength);
    il2::invoke_value(api.wind_angle, wind, w.wind_angle);
    Il2CppObject* scene_transition = nullptr;
    if (api.scene_transition) il2::il2cpp_field_static_get_value(api.scene_transition, &scene_transition);
    if (scene_transition && api.last_outdoor >= 0)
        if (Il2CppString* s = il2::read_field<Il2CppString*>(scene_transition, api.last_outdoor)) w.region = il2::to_utf8(s);
    w.world_id = game::save_id();
    w.valid = true;

    list_sets_once(weather);
    if (w.weather_set != g_published_set) {
        g_published_set = w.weather_set;
        logx::info("world: sharing weather set '%s' (%s, %.0f%% through; state %zu chars%s)", w.weather_set.c_str(),
                   stage_name(w.weather_stage).c_str(), w.weather_progress * 100, w.weather_set_state.size(),
                   w.weather_set_state.size() > net::kWeatherStateMax ? ", TOO LONG to send" : "");
    }
    return w;
}

// --- client ----------------------------------------------------------------------------------------------------------

void follow_clock(const net::WorldMsg& w, double now) {
    Il2CppObject* tod = il2::invoke(api.get_tod, nullptr);
    if (!tod) return;
    bool our_lapse = false;
    il2::invoke_value(api.tod_time_lapse, tod, our_lapse);
    if (our_lapse) {
        // Our own sleep or pass-time: let it finish, then catch up with the host.
        if (!g_applied.clock_waiting) logx::info("world: our time lapse is running; the clock catches up after");
        g_applied.clock_waiting = true;
        return;
    }
    g_applied.clock_waiting = false;
    // The host's own lapse (a shared sleep): its clock races, so this one was behind before it arrived (a sleep at
    // 1.5 h a second: 50 minutes in half a second). Catch up from the first one after.
    if (w.time_lapse) return;
    float host = w.normalized_time;
    if (!w.time_lapse && w.day_length_s > 0)  // the host's clock moved on since it sent this
        host += static_cast<float>((now - w.received_ms) / 1000.0 / w.day_length_s);
    host -= std::floor(host);
    float ours = 0;
    il2::invoke_value(api.tod_time, tod, ours);
    float drift = host - ours;
    if (drift > 0.5f) drift -= 1;
    if (drift < -0.5f) drift += 1;
    if (std::fabs(drift) * 1440.0f < kClockToleranceMinutes) return;
    bool force = true;
    void* args[] = {&host, &force};
    il2::invoke(api.tod_set_time, tod, args);
    logx::info("world: clock %s -> %s to match the host (%+.0f min)", clock_text(ours).c_str(),
               clock_text(host).c_str(), drift * 1440.0f);
}

void follow_weather(const net::WorldMsg& w, double now) {
    Il2CppObject* weather = il2::invoke(api.get_weather, nullptr);
    Il2CppObject* transition = il2::invoke(api.get_transition, nullptr);
    if (!weather || !transition) return;
    list_sets_once(weather);

    if (w.weather_set.empty()) {
        // The host has no weather set running (scripted weather): match its stage, once per change.
        if (w.weather_stage == kUndefinedStage || w.weather_stage == g_applied.stage) return;
        int32_t stage = w.weather_stage;
        float frac = 0;
        void* args[] = {&stage, &frac};
        il2::invoke(api.transition_activate_type, transition, args);
        g_applied.stage = w.weather_stage;
        g_applied.set.clear();
        g_applied.weather_at = now;
        logx::info("world: weather -> a %s set to match the host (the host has no set running)",
                   stage_name(stage).c_str());
        return;
    }

    Il2CppObject* ours = il2::read_field<Il2CppObject*>(transition, api.transition_set);
    const std::string our_name = name_of(ours);
    if (our_name == w.weather_set) {
        // Same set: both run it in step; only correct real drift, rarely.
        if (now - g_applied.weather_at < kWeatherRecheckMs) return;
        g_applied.weather_at = now;
        float progress = 0;
        il2::invoke_value(api.set_progress, ours, progress);
        if (std::fabs(progress - w.weather_progress) < kProgressTolerance) return;
        logx::info("world: weather set '%s' drifted (%.0f%% vs the host's %.0f%%); re-syncing", our_name.c_str(),
                   progress * 100, w.weather_progress * 100);
    } else if (g_applied.set == w.weather_set && now - g_applied.weather_at < kWeatherRecheckMs) {
        return;  // already followed this set (this region may not have it); don't redo it every tick
    }

    g_applied.set = w.weather_set;
    g_applied.stage = w.weather_stage;
    g_applied.weather_at = now;
    void* name_args[] = {text(w.weather_set)};
    Il2CppObject* set = il2::invoke(api.weather_set_by_name, weather, name_args);
    if (!set) {
        int32_t stage = w.weather_stage;
        float frac = 0;
        void* args[] = {&stage, &frac};
        il2::invoke(api.transition_activate_type, transition, args);
        logx::info("world: weather set '%s' isn't available here; running a %s set instead", w.weather_set.c_str(),
                   stage_name(stage).c_str());
        return;
    }
    // TLD's own restore-from-save steps: the set's state, the previous type, then activate at the progress.
    if (!w.weather_set_state.empty()) {
        void* state_args[] = {text(w.weather_set_state)};
        il2::invoke(api.set_deserialize, set, state_args);
    } else {
        il2::invoke(api.set_prepare, set);
    }
    if (api.transition_previous >= 0)
        memcpy(reinterpret_cast<char*>(transition) + api.transition_previous, &w.weather_previous, sizeof(int32_t));
    float frac = w.weather_progress;
    int32_t previous = w.weather_previous;
    void* args[] = {set, &frac, &previous};
    il2::invoke(api.transition_activate, transition, args);
    logx::info("world: weather -> set '%s' at %.0f%% (%s) to match the host (was '%s')", w.weather_set.c_str(),
               frac * 100, stage_name(w.weather_stage).c_str(), our_name.c_str());
}

void follow_wind(const net::WorldMsg& w, double now, bool first) {
    Il2CppObject* wind = il2::invoke(api.get_wind, nullptr);
    if (!wind) return;
    if (!first && w.wind_strength == g_applied.wind_strength &&
        angle_between(w.wind_angle, g_applied.wind_angle) < kWindAngleTolerance) {
        // Still what we set; but if our own wind started a new phase of its own, put it back (not too often).
        int32_t ours = -1;
        il2::invoke_value(api.wind_strength, wind, ours);
        if (ours == w.wind_strength || now - g_applied.wind_at < kWindRecheckMs) return;
    }
    int32_t strength = w.wind_strength;
    float angle = w.wind_angle;
    void* args[] = {&strength, &angle};
    il2::invoke(first ? api.wind_start_now : api.wind_start, wind, args);
    g_applied.wind_strength = strength;
    g_applied.wind_angle = angle;
    g_applied.wind_at = now;
    logx::info("world: wind -> %s from %.0f deg to match the host%s", wind_name(strength).c_str(), angle,
               first ? " (immediately)" : "");
}

}  // namespace

bool init() {
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* tod = il2::find_class(kGame, "", "TimeOfDay");
    Il2CppClass* weather = il2::find_class(kGame, "", "Weather");
    Il2CppClass* transition = il2::find_class(kGame, "", "WeatherTransition");
    Il2CppClass* set = il2::find_class(kGame, "", "WeatherSetData");
    Il2CppClass* wind = il2::find_class(kGame, "", "Wind");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    api.stage_enum = il2::find_class(kGame, "", "WeatherStage");
    api.wind_enum = il2::find_class(kGame, "", "WindStrength");

    api.get_tod = il2::find_method(game_manager, "GetTimeOfDayComponent", 0);
    api.get_weather = il2::find_method(game_manager, "GetWeatherComponent", 0);
    api.get_transition = il2::find_method(game_manager, "GetWeatherTransitionComponent", 0);
    api.get_wind = il2::find_method(game_manager, "GetWindComponent", 0);
    if (game_manager) api.scene_transition = il2::il2cpp_class_get_field_from_name(game_manager, "m_SceneTransitionData");
    api.last_outdoor = il2::field_offset(il2::find_class(kGame, "", "SceneTransitionData"), "m_LastOutdoorScene");

    api.tod_time = il2::find_method(tod, "GetNormalizedTime", 0);
    api.tod_set_time = il2::find_method_params(tod, "SetNormalizedTime", {"System.Single", "System.Boolean"});
    api.tod_day = il2::find_method(tod, "GetDayNumber", 0);
    api.tod_day_length = il2::find_method(tod, "GetDayLengthSeconds", 0);
    api.tod_time_lapse = il2::find_method(tod, "IsTimeLapseActive", 0);

    api.weather_stage = il2::find_method(weather, "GetWeatherStage", 0);
    api.weather_set_by_name = il2::find_method(weather, "GetWeatherSetByName", 1);
    api.weather_sets_for_scene = il2::field_offset(weather, "m_WeatherSetsForScene");

    api.transition_set = il2::field_offset(transition, "m_CurrentWeatherSet");
    api.transition_previous = il2::field_offset(transition, "m_PreviousWeatherSetType");
    api.transition_activate =
        il2::find_method_params(transition, "ActivateWeatherSet", {"WeatherSetData", "System.Single", "WeatherStage"});
    api.transition_activate_type = il2::find_method(transition, "ActivateWeatherSetAtFrac", 2);

    api.set_serialize = il2::find_method(set, "Serialize", 0);
    api.set_deserialize = il2::find_method(set, "Deserialize", 1);
    api.set_prepare = il2::find_method(set, "Prepare", 0);
    api.set_progress = il2::find_method(set, "GetProgressFrac", 0);
    api.set_type = il2::field_offset(set, "m_CharacterizingType");
    api.object_name = il2::find_method(object, "get_name", 0);

    api.wind_strength = il2::find_method(wind, "GetStrength", 0);
    api.wind_angle = il2::find_method(wind, "GetWindAngle_Base", 0);
    api.wind_start = il2::find_method_params(wind, "StartPhase", {"WindStrength", "System.Single"});
    api.wind_start_now = il2::find_method_params(wind, "StartPhaseImmediate", {"WindStrength", "System.Single"});

    g_ready = api.get_tod && api.get_weather && api.get_transition && api.get_wind && api.tod_time &&
              api.tod_set_time && api.tod_time_lapse && api.weather_stage && api.weather_set_by_name &&
              api.transition_set >= 0 && api.transition_activate && api.transition_activate_type &&
              api.set_serialize && api.set_deserialize && api.set_prepare && api.set_progress && api.object_name &&
              api.wind_strength && api.wind_angle && api.wind_start && api.wind_start_now;
    logx::info("world sync: %s", g_ready ? "ready" : "UNAVAILABLE (TLD API missing; see warnings above)");
    return g_ready;
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    const auto role = netrt::role();
    if (!role) return;
    const double now = now_ms();

    if (*role == net::Role::Host) {
        if (now - g_last_publish < kPublishMs) return;
        g_last_publish = now;
        netrt::set_world(read_world(in_game));
        return;
    }

    if (now - g_last_apply < kApplyMs) return;
    g_last_apply = now;
    const auto world = netrt::remote_world();
    if (!world || !world->valid || !in_game) {
        if (g_applied.any) logx::info("world: not following the host for now (%s)",
                                      !world ? "not connected" : !in_game ? "we're not in the game" : "host not in the game");
        g_applied = Applied{};
        return;
    }
    if (scene != g_apply_scene) {
        if (g_applied.any) logx::info("world: we're now in '%s'; checking the host's world again", scene.c_str());
        g_apply_scene = scene;
        g_applied = Applied{};
    }
    const bool first = !g_applied.any;
    g_applied.any = true;
    follow_clock(*world, now);
    follow_weather(*world, now);
    follow_wind(*world, now, first);
}

}  // namespace world_sync
