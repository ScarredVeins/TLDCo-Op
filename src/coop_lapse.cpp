#include "coop_lapse.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/lapse.h"
#include "net_runtime.h"

namespace coop_lapse {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr int32_t kActiveFishing = 1;  // FishingState.ActiveFishing
constexpr double kMaxFrameS = 0.25;    // a longer gap (a load, the game held) doesn't count as body hours

struct Api {
    const MethodInfo* get_tod = nullptr;        // static GameManager.GetTimeOfDayComponent()
    const MethodInfo* get_hunger = nullptr;     // static GameManager.Get*Component()
    const MethodInfo* get_thirst = nullptr;
    const MethodInfo* get_fatigue = nullptr;
    const MethodInfo* get_freezing = nullptr;
    const MethodInfo* get_condition = nullptr;
    const MethodInfo* set_scale = nullptr;      // TimeOfDay.SetDayLengthScale(float): every speed-up goes through it
    const MethodInfo* reapply = nullptr;        // TimeOfDay.DebugOnly_UpdateTimeOfDayScale(): re-applies it every frame
    const MethodInfo* hours_played = nullptr;   // TimeOfDay.GetHoursPlayedNotPaused()
    const MethodInfo* clock = nullptr;          // TimeOfDay.GetNormalizedTime()
    ptrdiff_t tod_storm = -1;                   // TimeOfDay.m_WeatherSystem (UniStormWeatherSystem)
    ptrdiff_t tod_debug = -1;                   // TimeOfDay.m_DayLengthScaleDebug (1 but for TLD's debug speed-up)
    ptrdiff_t storm_scale = -1;                 // UniStormWeatherSystem.m_DayLengthScale: what every update reads
    ptrdiff_t storm_day = -1;                   // UniStormWeatherSystem.m_DayLength: real seconds in a game day
    // Sleep and passing time (item 12's): their speed-ups pass through as TLD does them. Rest.BeginSleeping is
    // coop_sleep's, which says so through world_lapse.
    const MethodInfo* sleep_accelerate = nullptr;  // Rest.AccelerateTimeOfDay(int, float, bool)
    const MethodInfo* pass_begin = nullptr;     // PassTime.Begin(float, Bed)
    const MethodInfo* pass_accelerate = nullptr;  // PassTime.AccelerateTimeOfDay(float)
    // The body: what runs at the action's scale.
    const MethodInfo* hunger_update = nullptr;
    const MethodInfo* thirst_update = nullptr;
    const MethodInfo* fatigue_update = nullptr;
    const MethodInfo* freezing_update = nullptr;
    const MethodInfo* condition_update = nullptr;
    const MethodInfo* buffs_update = nullptr;   // PlayerManager.UpdateBuffDurations(Panel_HUD, float)
    const MethodInfo* fishing_update = nullptr; // IceFishingHole.Update(): the catch timer and the line's wear
    ptrdiff_t fishing_state = -1;               // IceFishingHole.m_State
    ptrdiff_t calories = -1, burn = -1;         // Hunger.m_CurrentReserveCalories, m_CurrentCalorieBurnPerHour
    ptrdiff_t thirst = -1, thirst_max = -1;     // Thirst.m_CurrentThirst, m_MaxThirst
    ptrdiff_t fatigue = -1, fatigue_rate = -1;  // Fatigue.m_CurrentFatigue, m_CurrentFatigueBurnPerHour
    ptrdiff_t freezing = -1, freezing_rate = -1, warming_rate = -1;  // Freezing.m_CurrentFreezing, ...PerHour
    ptrdiff_t hp = -1, max_hp = -1;             // Condition.m_CurrentHP, m_MaxHP
    // TLD's generic progress bar keeps the scale it sped up to and goes back to it on a resume (ice fishing
    // re-applies it every frame); it reads the world's back, 1, so it is given the body's.
    Il2CppClass* bar_class = nullptr;
    const MethodInfo* bar_launch = nullptr;     // Launch(string, float seconds, float minutes, float, bool, OnExitDelegate)
    ptrdiff_t bar_accelerated = -1;             // m_TimeAccelerated
    ptrdiff_t bar_scale = -1;                   // m_DayLengthScaleAccelerated
    const MethodInfo* load_panel = nullptr;     // static InterfaceManager.LoadPanel(Type)
    // What we eat or drink (milestone 13): the bar's exit handler says it's eating or drinking, PlayerManager what.
    const MethodInfo* bar_ended = nullptr;      // ProgressBarEnded(bool success, bool cancel): every end of a bar
    const MethodInfo* get_pm = nullptr;         // static GameManager.GetPlayerManagerComponent()
    ptrdiff_t food_eaten = -1;                  // PlayerManager.m_FoodItemEaten (FoodItem; set just after Launch)
    ptrdiff_t water_source = -1;                // PlayerManager.m_WaterSourceToDrinkFrom (WaterSupply)
    const MethodInfo* component_go = nullptr;   // Component.get_gameObject()
    const MethodInfo* object_name = nullptr;    // Object.get_name()
} api;

constexpr ptrdiff_t kDelegateMethod = 0x28;  // Il2CppDelegate.method: the MethodInfo it calls
enum class Eat : uint8_t { No, Food, Water };
Eat g_eat = Eat::No;
double g_eat_until = 0;  // a bar that never says it ended still lets go after its time

template <typename T>
void put(Il2CppObject* obj, ptrdiff_t offset, T value) {
    if (obj && offset >= 0) memcpy(reinterpret_cast<char*>(obj) + offset, &value, sizeof(T));
}

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

bool coop() { return netrt::role().has_value(); }

struct Needs {
    bool valid = false;
    float calories = 0, burn = 0, thirst = 0, thirst_max = 0, fatigue = 0, fatigue_rate = 0, freezing = 0,
          freezing_rate = 0, warming_rate = 0, hp = 0, max_hp = 0;
};

Needs read_needs() {
    Needs n;
    Il2CppObject* hunger = il2::invoke(api.get_hunger, nullptr);
    Il2CppObject* thirst = il2::invoke(api.get_thirst, nullptr);
    Il2CppObject* fatigue = il2::invoke(api.get_fatigue, nullptr);
    Il2CppObject* freezing = il2::invoke(api.get_freezing, nullptr);
    Il2CppObject* condition = il2::invoke(api.get_condition, nullptr);
    if (!hunger || !thirst || !fatigue || !freezing || !condition) return n;
    n.valid = true;
    n.calories = il2::read_field<float>(hunger, api.calories);
    n.burn = il2::read_field<float>(hunger, api.burn);
    n.thirst = il2::read_field<float>(thirst, api.thirst);
    n.thirst_max = il2::read_field<float>(thirst, api.thirst_max);
    n.fatigue = il2::read_field<float>(fatigue, api.fatigue);
    n.fatigue_rate = il2::read_field<float>(fatigue, api.fatigue_rate);
    n.freezing = il2::read_field<float>(freezing, api.freezing);
    n.freezing_rate = il2::read_field<float>(freezing, api.freezing_rate);
    n.warming_rate = il2::read_field<float>(freezing, api.warming_rate);
    n.hp = il2::read_field<float>(condition, api.hp);
    n.max_hp = il2::read_field<float>(condition, api.max_hp);
    return n;
}

float hours_played(Il2CppObject* tod) {
    float h = 0;
    if (tod) il2::invoke_value(api.hours_played, tod, h);
    return h;
}

std::string clock_text(Il2CppObject* tod) {
    float t = 0;
    if (tod) il2::invoke_value(api.clock, tod, t);
    t -= std::floor(t);
    const int minutes = static_cast<int>(t * 1440.0f + 0.5f) % 1440;
    char s[8];
    snprintf(s, sizeof(s), "%02d:%02d", minutes / 60, minutes % 60);
    return s;
}

// --- state ---------------------------------------------------------------------------------------------------------

bool g_ready = false;
lapse::Body g_body;  // an action's speed-up our body lives (co-op)
int g_reapplying = 0, g_sleeping = 0, g_passing = 0;  // inside those TLD calls
std::string g_scene;
double g_last_frame = 0;

struct Started {
    Needs needs;
    double real_ms = 0;
    float hours_played = 0;
    double body_hours = 0;
} g_started;

void began(Il2CppObject* tod) {
    g_started = {read_needs(), now_ms(), hours_played(tod), 0};
    logx::info("lapse: an action speeds time up %.0fx; in co-op only our body lives it (the clock stays at %s)",
               1.0 / g_body.arg, clock_text(tod).c_str());
}

void ended(Il2CppObject* tod, const char* why) {
    const Needs n = read_needs();
    const Needs& s = g_started.needs;
    const double real_s = (now_ms() - g_started.real_ms) / 1000.0;
    const float clock_min = tod ? (hours_played(tod) - g_started.hours_played) * 60.0f : 0.0f;
    if (n.valid && s.valid)
        logx::info("lapse: the action is over%s: our body lived %.2f game hours in %.1f s, the clock %.1f min "
                   "(calories %+.0f, thirst %+.2f, fatigue %+.2f, freezing %+.2f, condition %+.2f)",
                   why, g_started.body_hours, real_s, clock_min, n.calories - s.calories, n.thirst - s.thirst,
                   n.fatigue - s.fatigue, n.freezing - s.freezing, n.hp - s.hp);
    else
        logx::info("lapse: the action is over%s: our body lived %.2f game hours in %.1f s", why, g_started.body_hours,
                   real_s);
}

// For one update of the body: the world's scale (UniStorm's, which every update reads) at the action's, then back.
class BodyScale {
public:
    BodyScale() {
        if (!g_body.active) return;
        Il2CppObject* tod = il2::invoke(api.get_tod, nullptr);
        storm_ = il2::read_field<Il2CppObject*>(tod, api.tod_storm);
        if (!storm_) return;
        saved_ = il2::read_field<float>(storm_, api.storm_scale);
        put(storm_, api.storm_scale, g_body.arg * il2::read_field<float>(tod, api.tod_debug));
    }
    ~BodyScale() {
        if (storm_) put(storm_, api.storm_scale, saved_);  // (if the update ended the action, TLD wrote this too)
    }
    BodyScale(const BodyScale&) = delete;
    BodyScale& operator=(const BodyScale&) = delete;

private:
    Il2CppObject* storm_ = nullptr;
    float saved_ = 0;
};

// --- hooks -----------------------------------------------------------------------------------------------------------

using ScaleFn = void (*)(Il2CppObject* self, float scale, const MethodInfo* method);
using UpdateFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using BuffsFn = void (*)(Il2CppObject* self, Il2CppObject* hud, float seconds, const MethodInfo* method);
using SleepAccelerateFn = void (*)(Il2CppObject* self, int32_t minutes, float fade, bool use_fade, const MethodInfo* method);
using PassFn = void (*)(Il2CppObject* self, float hours, Il2CppObject* bed, const MethodInfo* method);
using PassAccelerateFn = void (*)(Il2CppObject* self, float hours, const MethodInfo* method);
using LaunchFn = void (*)(Il2CppObject* self, Il2CppString* name, float seconds, float minutes, float failure,
                          bool skip_restore, Il2CppObject* on_exit, const MethodInfo* method);
ScaleFn g_set_scale = nullptr;
UpdateFn g_reapply = nullptr, g_hunger = nullptr, g_thirst = nullptr, g_fatigue = nullptr, g_freezing = nullptr,
         g_condition = nullptr, g_fishing = nullptr;
BuffsFn g_buffs = nullptr;
SleepAccelerateFn g_sleep_accelerate = nullptr;
PassFn g_pass_begin = nullptr;
PassAccelerateFn g_pass_accelerate = nullptr;
LaunchFn g_launch = nullptr;
using BarEndedFn = void (*)(Il2CppObject* self, bool success, bool cancel, const MethodInfo* method);
BarEndedFn g_bar_ended = nullptr;

void set_scale_detour(Il2CppObject* tod, float scale, const MethodInfo* method) {
    const lapse::Kind kind = g_reapplying ? lapse::Kind::Reapply
                             : g_sleeping ? lapse::Kind::Sleep
                             : g_passing  ? lapse::Kind::PassTime
                                          : lapse::Kind::Action;
    const bool was = g_body.active;
    const lapse::Applied a = lapse::on_set(g_body, scale, kind, coop());
    g_body = a.body;
    g_set_scale(tod, a.world, method);
    if (g_body.active && !was) began(tod);
    else if (was && !g_body.active) ended(tod, kind == lapse::Kind::Action ? "" : " (sleep or passing time began)");
}

void reapply_detour(Il2CppObject* self, const MethodInfo* method) {
    ++g_reapplying;
    g_reapply(self, method);
    --g_reapplying;
}

void sleep_accelerate_detour(Il2CppObject* self, int32_t minutes, float fade, bool use_fade, const MethodInfo* method) {
    ++g_sleeping;
    g_sleep_accelerate(self, minutes, fade, use_fade, method);
    --g_sleeping;
}

void pass_begin_detour(Il2CppObject* self, float hours, Il2CppObject* bed, const MethodInfo* method) {
    ++g_passing;
    g_pass_begin(self, hours, bed, method);
    --g_passing;
}

void pass_accelerate_detour(Il2CppObject* self, float hours, const MethodInfo* method) {
    ++g_passing;
    g_pass_accelerate(self, hours, method);
    --g_passing;
}

void hunger_detour(Il2CppObject* self, const MethodInfo* method) {
    BodyScale body;
    g_hunger(self, method);
}

void thirst_detour(Il2CppObject* self, const MethodInfo* method) {
    BodyScale body;
    g_thirst(self, method);
}

void fatigue_detour(Il2CppObject* self, const MethodInfo* method) {
    BodyScale body;
    g_fatigue(self, method);
}

void freezing_detour(Il2CppObject* self, const MethodInfo* method) {
    BodyScale body;
    g_freezing(self, method);
}

float g_heal_boost = 1;  // coop_sleep's REST

void condition_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!g_body.active && g_heal_boost == 1) return g_condition(self, method);
    const float arg = g_body.arg;
    const float before = il2::read_field<float>(self, api.hp);
    if (g_body.active) {
        BodyScale body;
        g_condition(self, method);
    } else {
        g_condition(self, method);
    }
    float after = il2::read_field<float>(self, api.hp);
    if (after <= before) return;
    if (g_body.active) after = lapse::condition_after(before, after, arg);
    after = std::min(before + (after - before) * g_heal_boost, std::max(before, il2::read_field<float>(self, api.max_hp)));
    put(self, api.hp, after);
}

void buffs_detour(Il2CppObject* self, Il2CppObject* hud, float seconds, const MethodInfo* method) {
    BodyScale body;
    g_buffs(self, hud, seconds, method);
}

void fishing_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!g_body.active || il2::read_field<int32_t>(self, api.fishing_state) != kActiveFishing)
        return g_fishing(self, method);
    BodyScale body;
    g_fishing(self, method);
}

void launch_detour(Il2CppObject* self, Il2CppString* name, float seconds, float minutes, float failure,
                   bool skip_restore, Il2CppObject* on_exit, const MethodInfo* method) {
    g_launch(self, name, seconds, minutes, failure, skip_restore, on_exit, method);
    if (g_body.active && il2::read_field<bool>(self, api.bar_accelerated)) put(self, api.bar_scale, g_body.arg);
    const MethodInfo* on = on_exit ? il2::read_field<const MethodInfo*>(on_exit, kDelegateMethod) : nullptr;
    const char* handler = on ? il2::il2cpp_method_get_name(on) : nullptr;
    g_eat = !handler                                     ? Eat::No
            : !strcmp(handler, "OnEatingComplete")       ? Eat::Food
            : !strcmp(handler, "OnDrinkWaterComplete")   ? Eat::Water
                                                         : Eat::No;
    g_eat_until = now_ms() + seconds * 1000.0 + 3000.0;
}

void bar_ended_detour(Il2CppObject* self, bool success, bool cancel, const MethodInfo* method) {
    g_eat = Eat::No;
    g_bar_ended(self, success, cancel, method);
}

}  // namespace

bool body_lapse() { return g_body.active; }

void heal_boost(float factor) { g_heal_boost = factor > 0 ? factor : 1.0f; }

std::string eating() {
    if (g_eat == Eat::No || now_ms() > g_eat_until) return {};
    Il2CppObject* pm = api.get_pm ? il2::invoke(api.get_pm, nullptr) : nullptr;
    const ptrdiff_t field = g_eat == Eat::Food ? api.food_eaten : api.water_source;
    Il2CppObject* what = pm && field >= 0 ? il2::read_field<Il2CppObject*>(pm, field) : nullptr;
    Il2CppObject* go = what && api.component_go ? il2::invoke(api.component_go, what) : nullptr;
    std::string name = go && api.object_name ? il2::to_utf8(reinterpret_cast<Il2CppString*>(il2::invoke(api.object_name, go))) : "";
    if (const size_t clone = name.find("(Clone)"); clone != std::string::npos) name.erase(clone);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.rfind("GEAR_", 0) != 0) name = g_eat == Eat::Water ? "GEAR_WaterSupplyPotable" : "";
    return name;
}

void world_lapse(bool begin) { g_sleeping += begin ? 1 : -1; }

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    const double now = now_ms();
    const double dt = std::min((now - g_last_frame) / 1000.0, kMaxFrameS);
    g_last_frame = now;
    if (g_body.active && (scene != g_scene || !in_game)) {
        // TLD restores the scale at the end of every action; a scene change in the middle is the safety net.
        ended(nullptr, " (the scene changed)");
        g_body = {};
    }
    g_scene = scene;
    if (!g_body.active) return;
    Il2CppObject* tod = il2::invoke(api.get_tod, nullptr);
    Il2CppObject* storm = il2::read_field<Il2CppObject*>(tod, api.tod_storm);
    const float day = il2::read_field<float>(storm, api.storm_day);
    if (day > 0) g_started.body_hours += lapse::frame_hours(dt, g_body.arg, il2::read_field<float>(tod, api.tod_debug), day);
}

void describe() {
    const Needs n = read_needs();
    Il2CppObject* tod = il2::invoke(api.get_tod, nullptr);
    if (!n.valid || !tod) {
        logx::warn("dev: needs: not in a game");
        return;
    }
    Il2CppObject* storm = il2::read_field<Il2CppObject*>(tod, api.tod_storm);
    logx::info("dev: needs: calories %.1f (burning %.1f/h), thirst %.3f of %.0f, fatigue %.3f (+%.3f/h), freezing %.3f "
               "(+%.3f/h, warming %.3f/h), condition %.3f of %.0f; clock %s, %.4f h played, scale %.4f, body lapse %s",
               n.calories, n.burn, n.thirst, n.thirst_max, n.fatigue, n.fatigue_rate, n.freezing, n.freezing_rate,
               n.warming_rate, n.hp, n.max_hp, clock_text(tod).c_str(), hours_played(tod),
               il2::read_field<float>(storm, api.storm_scale), g_body.active ? "on" : "off");
}

bool launch(float seconds, float minutes) {
    if (!g_ready || !il2::invoke(api.get_tod, nullptr)) return false;
    void* load_args[] = {il2::type_object(api.bar_class)};
    Il2CppObject* bar = il2::invoke(api.load_panel, nullptr, load_args);
    if (!bar) return false;
    float failure = 0;
    bool skip_restore = true;  // nothing in the hands to put back
    void* args[] = {il2::il2cpp_string_new("GAMEPLAY_Crafting"), &seconds, &minutes, &failure, &skip_restore, nullptr};
    il2::invoke(api.bar_launch, bar, args);
    logx::info("dev: lapse: TLD's progress bar for %.0f s and %.0f game minutes", seconds, minutes);
    return true;
}

bool init() {
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* tod = il2::find_class(kGame, "", "TimeOfDay");
    Il2CppClass* storm = il2::find_class(kGame, "", "UniStormWeatherSystem");
    Il2CppClass* hunger = il2::find_class(kGame, "", "Hunger");
    Il2CppClass* thirst = il2::find_class(kGame, "", "Thirst");
    Il2CppClass* fatigue = il2::find_class(kGame, "", "Fatigue");
    Il2CppClass* freezing = il2::find_class(kGame, "", "Freezing");
    Il2CppClass* condition = il2::find_class(kGame, "", "Condition");
    Il2CppClass* rest = il2::find_class(kGame, "", "Rest");
    Il2CppClass* pass = il2::find_class(kGame, "", "PassTime");
    Il2CppClass* fishing = il2::find_class(kGame, "", "IceFishingHole");
    api.bar_class = il2::find_class(kGame, "", "Panel_GenericProgressBar");

    api.get_tod = il2::find_method(game_manager, "GetTimeOfDayComponent", 0);
    api.get_hunger = il2::find_method(game_manager, "GetHungerComponent", 0);
    api.get_thirst = il2::find_method(game_manager, "GetThirstComponent", 0);
    api.get_fatigue = il2::find_method(game_manager, "GetFatigueComponent", 0);
    api.get_freezing = il2::find_method(game_manager, "GetFreezingComponent", 0);
    api.get_condition = il2::find_method(game_manager, "GetConditionComponent", 0);
    api.set_scale = il2::find_method(tod, "SetDayLengthScale", 1);
    api.reapply = il2::find_method(tod, "DebugOnly_UpdateTimeOfDayScale", 0);
    api.hours_played = il2::find_method(tod, "GetHoursPlayedNotPaused", 0);
    api.clock = il2::find_method(tod, "GetNormalizedTime", 0);
    api.tod_storm = il2::field_offset(tod, "m_WeatherSystem");
    api.tod_debug = il2::field_offset(tod, "m_DayLengthScaleDebug");
    api.storm_scale = il2::field_offset(storm, "m_DayLengthScale");
    api.storm_day = il2::field_offset(storm, "m_DayLength");
    api.sleep_accelerate = il2::find_method(rest, "AccelerateTimeOfDay", 3);
    api.pass_begin = il2::find_method(pass, "Begin", 2);
    api.pass_accelerate = il2::find_method(pass, "AccelerateTimeOfDay", 1);
    api.hunger_update = il2::find_method(hunger, "Update", 0);
    api.thirst_update = il2::find_method(thirst, "Update", 0);
    api.fatigue_update = il2::find_method(fatigue, "Update", 0);
    api.freezing_update = il2::find_method(freezing, "Update", 0);
    api.condition_update = il2::find_method(condition, "Update", 0);
    api.buffs_update = il2::find_method(il2::find_class(kGame, "", "PlayerManager"), "UpdateBuffDurations", 2);
    api.fishing_update = il2::find_method(fishing, "Update", 0);
    api.fishing_state = il2::field_offset(fishing, "m_State");
    api.calories = il2::field_offset(hunger, "m_CurrentReserveCalories");
    api.burn = il2::field_offset(hunger, "m_CurrentCalorieBurnPerHour");
    api.thirst = il2::field_offset(thirst, "m_CurrentThirst");
    api.thirst_max = il2::field_offset(thirst, "m_MaxThirst");
    api.fatigue = il2::field_offset(fatigue, "m_CurrentFatigue");
    api.fatigue_rate = il2::field_offset(fatigue, "m_CurrentFatigueBurnPerHour");
    api.freezing = il2::field_offset(freezing, "m_CurrentFreezing");
    api.freezing_rate = il2::field_offset(freezing, "m_CurrentFreezingPerHour");
    api.warming_rate = il2::field_offset(freezing, "m_CurrentWarmingPerHour");
    api.hp = il2::field_offset(condition, "m_CurrentHP");
    api.max_hp = il2::field_offset(condition, "m_MaxHP");
    api.bar_launch = il2::find_method_params(api.bar_class, "Launch",
                                             {"System.String", "System.Single", "System.Single", "System.Single",
                                              "System.Boolean", "OnExitDelegate"});
    api.bar_accelerated = il2::field_offset(api.bar_class, "m_TimeAccelerated");
    api.bar_scale = il2::field_offset(api.bar_class, "m_DayLengthScaleAccelerated");
    api.load_panel = il2::find_method_params(il2::find_class(kGame, "", "InterfaceManager"), "LoadPanel", {"System.Type"});
    api.bar_ended = il2::find_method_params(api.bar_class, "ProgressBarEnded", {"System.Boolean", "System.Boolean"});
    api.get_pm = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    Il2CppClass* pm = il2::find_class(kGame, "", "PlayerManager");
    api.food_eaten = il2::field_offset(pm, "m_FoodItemEaten");
    api.water_source = il2::field_offset(pm, "m_WaterSourceToDrinkFrom");
    api.component_go = il2::find_method(il2::find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Component"), "get_gameObject", 0);
    api.object_name = il2::find_method(il2::find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Object"), "get_name", 0);

    g_ready = api.get_tod && api.get_hunger && api.get_thirst && api.get_fatigue && api.get_freezing &&
              api.get_condition && api.hours_played && api.clock && api.tod_storm >= 0 && api.tod_debug >= 0 &&
              api.storm_scale >= 0 && api.storm_day >= 0 && api.fishing_state >= 0 && api.calories >= 0 &&
              api.burn >= 0 && api.thirst >= 0 && api.thirst_max >= 0 && api.fatigue >= 0 && api.fatigue_rate >= 0 &&
              api.freezing >= 0 && api.freezing_rate >= 0 && api.warming_rate >= 0 && api.hp >= 0 &&
              api.max_hp >= 0 && api.bar_accelerated >= 0 && api.bar_scale >= 0 && api.load_panel;
    if (!g_ready) {
        logx::warn("lapse: UNAVAILABLE (TLD API missing; actions move the clock in co-op as in TLD)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    // The ones that pass sleep and passing time through first: without them those would become a body's lapse.
    g_ready = hook("Rest.AccelerateTimeOfDay", api.sleep_accelerate, &sleep_accelerate_detour, &g_sleep_accelerate) &&
              hook("PassTime.Begin", api.pass_begin, &pass_begin_detour, &g_pass_begin) &&
              hook("PassTime.AccelerateTimeOfDay", api.pass_accelerate, &pass_accelerate_detour, &g_pass_accelerate) &&
              hook("TimeOfDay.DebugOnly_UpdateTimeOfDayScale", api.reapply, &reapply_detour, &g_reapply) &&
              hook("Hunger.Update", api.hunger_update, &hunger_detour, &g_hunger) &&
              hook("Thirst.Update", api.thirst_update, &thirst_detour, &g_thirst) &&
              hook("Fatigue.Update", api.fatigue_update, &fatigue_detour, &g_fatigue) &&
              hook("Freezing.Update", api.freezing_update, &freezing_detour, &g_freezing) &&
              hook("Condition.Update", api.condition_update, &condition_detour, &g_condition) &&
              hook("PlayerManager.UpdateBuffDurations", api.buffs_update, &buffs_detour, &g_buffs) &&
              hook("IceFishingHole.Update", api.fishing_update, &fishing_detour, &g_fishing) &&
              hook("Panel_GenericProgressBar.Launch", api.bar_launch, &launch_detour, &g_launch) &&
              hook("TimeOfDay.SetDayLengthScale", api.set_scale, &set_scale_detour, &g_set_scale);
    logx::info("lapse: %s", g_ready ? "ready (in co-op an action's hours are our body's; the clock stays)"
                                    : "UNAVAILABLE (hooks failed)");
    // Not needed for the clock: without it the others see us eat until the bar's time is up.
    if (!api.bar_ended || !hook("Panel_GenericProgressBar.ProgressBarEnded", api.bar_ended, &bar_ended_detour, &g_bar_ended))
        logx::warn("lapse: a cancelled meal isn't seen as over by the others until its time is up");
    if (api.food_eaten < 0 || !api.component_go) logx::warn("lapse: the others won't see what we eat");
    return g_ready;
}

}  // namespace coop_lapse
