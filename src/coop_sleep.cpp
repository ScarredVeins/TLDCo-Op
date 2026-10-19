#include "coop_sleep.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "coop_lapse.h"
#include "coop_notices.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net/protocol.h"
#include "net/sleep_vote.h"
#include "net_runtime.h"

namespace coop_sleep {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule.dll";
constexpr ptrdiff_t kCachedPtr = 0x10;   // UnityEngine.Object.m_CachedPtr: null once the native object is gone
constexpr double kSettleMs = 2000;       // everyone in this long (their latest votes in) before the host starts
constexpr double kDoneGraceMs = 2000;    // the host's sleep is over: ours ends at the latest this much later
constexpr double kRemindMs = 60000;      // up while others are in: told again this often
constexpr float kCureBoost = 1.25f;      // in bed in real time, afflictions heal this much faster
constexpr float kRestBoost = 1.5f;       // REST (milestone 13): afflictions heal and condition comes back this much faster
constexpr float kSkipSeconds = 8;        // the shared skip's real seconds when TLD's sleep fade doesn't say
constexpr double kDoneSlackMs = 300;     // passing time this close to its real seconds is done, not stopped
// SleepChoice H4..Rest: the list, what TLD's hours selector runs over. REST is the bed's only (lying in it): the pass
// time screen's list ends at TILL DUSK.
constexpr int kFirstChoice = 1, kLastChoice = 6, kLastSleep = 5;
constexpr int kChoices = kLastChoice - kFirstChoice + 1;
constexpr int kOverflowResizeFreely = 2, kOverflowResizeHeight = 3;  // UILabel.Overflow
constexpr int kPivotLeft = 3, kPivotCenter = 4, kPivotRight = 5;      // UIWidget.Pivot
constexpr int32_t kCancelButton = 4;     // Rest.PassTimeOptions.DisplayCancelButton: TLD's HUD button over GET UP

// The list, in ControllerOffset/Shared's space (40 below the panel's centre): between the description and the stats.
constexpr float kRowTopY = 106, kRowPitch = 23, kRowTextX = 205;  // six rows between the description and TLD's stats
constexpr int kRowWidth = 450, kRowHeight = 21;
constexpr int kNameFont = 20, kVotersFont = 16;

// Panel_Rest's widgets (TLDCoop/ui/Panel_Rest.txt). RestOnly is the sleep screen's, PassTimeOnly the pass time one's.
constexpr const char* kBedTitle = "ControllerOffset/RestOnly/Label_Title";
constexpr const char* kBedDescription = "ControllerOffset/RestOnly/Label_Description";
constexpr const char* kBedCaption = "ControllerOffset/RestOnly/Label_RestDuration";
constexpr const char* kWaitTitle = "ControllerOffset/PassTimeOnly/Label_Title";
constexpr const char* kWaitDescription = "ControllerOffset/PassTimeOnly/Label_Description";
constexpr const char* kWaitCaption = "ControllerOffset/PassTimeOnly/Label_PassTimeDuration";
constexpr const char* kShared = "ControllerOffset/Shared";
constexpr const char* kSelector = "ControllerOffset/Shared/HoursSelection";
constexpr const char* kValue = "ControllerOffset/Shared/HoursSelection/Label_RestDurationVal";
constexpr const char* kValueBack = "ControllerOffset/Shared/HoursSelection/HoursBG";
constexpr const char* kIncrease = "ControllerOffset/Shared/HoursSelection/HoursSelectionButtons/Button_Increase";
constexpr const char* kCloseLabel = "Mouse/Button_Back/Button_Back/ButtonBottomRightMouse/Mouse_Button_Label_BAck";

struct Vector3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};
constexpr Color kSelected{1, 1, 1, 1}, kHovered{0.85f, 0.85f, 0.85f, 1}, kNormal{0.6f, 0.6f, 0.6f, 1};
constexpr float kBackSelected = 1.0f, kBackHovered = 0.45f;  // the row's backing (the selector's), by its own alpha

struct Api {
    // Unity
    const MethodInfo* component_go = nullptr;       // Component.get_gameObject()
    const MethodInfo* component_transform = nullptr;
    const MethodInfo* in_children = nullptr;        // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* go_component = nullptr;       // GameObject.GetComponent(Type)
    const MethodInfo* go_set_active = nullptr;
    const MethodInfo* set_enabled = nullptr;        // Behaviour.set_enabled(bool)
    const MethodInfo* find_child = nullptr;         // Transform.Find(string)
    const MethodInfo* get_position = nullptr;
    const MethodInfo* set_local_position = nullptr;
    const MethodInfo* find_objects = nullptr;       // static Object.FindObjectsOfType(Type)
    const MethodInfo* instantiate = nullptr;        // static Object.Instantiate(Object, Transform)
    const MethodInfo* set_name = nullptr;           // Object.set_name(string)
    Il2CppObject* box_type = nullptr;               // typeof(BoxCollider)
    const MethodInfo* box_set_size = nullptr;
    const MethodInfo* box_set_center = nullptr;
    // NGUI
    Il2CppObject* label_type = nullptr;             // typeof(UILabel)
    Il2CppObject* widget_type = nullptr;            // typeof(UIWidget)
    Il2CppObject* localize_type = nullptr;          // typeof(UILocalize): a label's text from TLD's string table
    Il2CppObject* button_type = nullptr;            // typeof(UIButton)
    Il2CppObject* button_scale_type = nullptr;      // typeof(UIButtonScale): a button grows when hovered
    Il2CppObject* button_offset_type = nullptr;     // typeof(UIButtonOffset): and moves when pressed
    const MethodInfo* button_on_click = nullptr;    // UIButton.OnClick(): what a click runs
    FieldInfo* button_current = nullptr;            // static UIButton.current: the button whose click is running
    const MethodInfo* hovered = nullptr;            // static UICamera.get_hoveredObject()
    const MethodInfo* label_set_text = nullptr;
    const MethodInfo* label_set_font_size = nullptr;
    const MethodInfo* label_get_overflow = nullptr;
    const MethodInfo* label_set_overflow = nullptr;
    const MethodInfo* widget_set_width = nullptr;
    const MethodInfo* widget_set_height = nullptr;
    const MethodInfo* widget_set_pivot = nullptr;
    const MethodInfo* widget_get_color = nullptr;
    const MethodInfo* widget_set_color = nullptr;
    // TLD
    const MethodInfo* get_rest = nullptr;           // static GameManager.GetRestComponent()
    const MethodInfo* get_pass_time = nullptr;      // static GameManager.GetPassTime()
    const MethodInfo* get_tod = nullptr;            // static GameManager.GetTimeOfDayComponent()
    const MethodInfo* get_fatigue = nullptr;
    const MethodInfo* get_freezing = nullptr;
    const MethodInfo* player_transform = nullptr;   // static GameManager.GetPlayerTransform()
    const MethodInfo* load_panel = nullptr;         // static InterfaceManager.LoadPanel(Type)
    Il2CppObject* bed_type = nullptr;               // typeof(Bed)
    // Panel_Rest: TLD's sleep and pass time screen
    Il2CppClass* panel_class = nullptr;
    const MethodInfo* panel_enable = nullptr;       // Enable(bool, bool passTimeOnly)
    const MethodInfo* panel_visuals = nullptr;      // UpdateVisuals(): every frame it's up
    const MethodInfo* panel_do_rest = nullptr;      // DoRest(int, bool): SLEEP, after TLD's checks
    const MethodInfo* panel_do_pass = nullptr;      // DoPassTime(float): PASS TIME, after TLD's checks
    const MethodInfo* panel_pass_check = nullptr;   // static CheckPassTimeErrorMessage(): true when time may pass
    const MethodInfo* panel_pick_up = nullptr;      // OnPickUp(): a bedroll back into the pack
    const MethodInfo* panel_start = nullptr;        // StartRest(Bed): what using a bed does
    const MethodInfo* panel_on_rest = nullptr;      // OnRest(): the SLEEP button
    const MethodInfo* panel_on_pass = nullptr;      // OnPassTime(): the PASS TIME button
    const MethodInfo* panel_on_cancel = nullptr;    // OnCancel(): the CLOSE button
    const MethodInfo* panel_increase = nullptr;     // OnIncreaseHours(): its right arrow (our rows' clicks)
    ptrdiff_t panel_hours = -1, panel_min = -1, panel_max = -1;  // m_SleepHours, m_MinSleepHours, m_CurrentMaxSleepHours
    ptrdiff_t panel_pass_allowed = -1;              // m_PassTimeIsAllowed (false: no PASS TIME tab)
    ptrdiff_t panel_bed = -1;                       // m_Bed
    ptrdiff_t panel_sleep_button = -1, panel_pick_up_button = -1, panel_pass_button = -1;
    // Rest: the sleep itself
    const MethodInfo* begin_sleeping = nullptr;     // BeginSleeping(Bed, int, int, float, PassTimeOptions, Action)
    const MethodInfo* end_sleeping = nullptr;       // EndSleeping(bool interrupted): every way a sleep ends
    const MethodInfo* unlimited = nullptr;          // AllowUnlimitedSleep()
    const MethodInfo* cures = nullptr;              // UpdateRestForCures(float todSeconds)
    ptrdiff_t sleeping = -1, slept_s = -1, duration_s = -1, duration_h = -1;
    ptrdiff_t interrupt_after = -1, predator = -1, interrupt_cold = -1, fade_seconds = -1;
    // PassTime: passing time
    const MethodInfo* pass_end = nullptr;           // End(): done, stopped (Escape) or afraid
    const MethodInfo* pass_afraid = nullptr;        // HasFearOrAnxiety()
    ptrdiff_t pass_rate = -1;                       // m_PassTimeRealtimeSecondsPerHour: Begin's duration and speed
    // The clock
    const MethodInfo* accelerate = nullptr;         // TimeOfDay.Accelerate(float realSeconds, float hours, bool fade)
    const MethodInfo* clock = nullptr;              // TimeOfDay.GetNormalizedTime()
    ptrdiff_t tod_storm = -1;                       // TimeOfDay.m_WeatherSystem
    const MethodInfo* dawn = nullptr;               // UniStormWeatherSystem.GetDawnTime(): daylight starts
    ptrdiff_t key_offset = -1, key_times = -1;      // m_MasterTimeKeyOffset, m_TODKeyframeTimes ([5]: dusk starts)
    ptrdiff_t fatigue = -1, freezing = -1, freezing_threshold = -1;
    // Where a sleeper lies (milestone 13: the others see our body there).
    ptrdiff_t rest_bed = -1;                        // Rest.m_Bed: from BeginSleeping to EndSleeping
    ptrdiff_t bed_place = -1;                       // Bed.m_BodyPlacementTransform
    const MethodInfo* get_rotation = nullptr;       // Transform.get_rotation()
} api;

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
Il2CppObject* go_of(Il2CppObject* component) { return alive(component) ? il2::invoke(api.component_go, component) : nullptr; }
Il2CppObject* transform_of(Il2CppObject* component) {
    return alive(component) ? il2::invoke(api.component_transform, component) : nullptr;
}

Il2CppObject* component(Il2CppObject* go, Il2CppObject* type) {
    if (!alive(go) || !type) return nullptr;
    void* args[] = {type};
    Il2CppObject* found = il2::invoke(api.go_component, go, args);
    return alive(found) ? found : nullptr;
}

void set_active(Il2CppObject* go, bool on) {
    if (!alive(go)) return;
    void* args[] = {&on};
    il2::invoke(api.go_set_active, go, args);
}

void set_int(const MethodInfo* setter, Il2CppObject* obj, int v) {
    if (!alive(obj)) return;
    void* args[] = {&v};
    il2::invoke(setter, obj, args);
}

int get_int(const MethodInfo* getter, Il2CppObject* obj) {
    int v = 0;
    if (alive(obj)) il2::invoke_value(getter, obj, v);
    return v;
}

void set_text(Il2CppObject* label, const std::string& value) {
    if (!alive(label)) return;
    void* args[] = {il2::il2cpp_string_new(value.c_str())};
    il2::invoke(api.label_set_text, label, args);
}

void set_color(Il2CppObject* widget, Color c) {
    if (!alive(widget)) return;
    void* args[] = {&c};
    il2::invoke(api.widget_set_color, widget, args);
}

void set_position(Il2CppObject* transform, Vector3 v) {
    if (!alive(transform)) return;
    void* args[] = {&v};
    il2::invoke(api.set_local_position, transform, args);
}

Il2CppObject* child(Il2CppObject* transform, const char* path) {
    if (!transform) return nullptr;
    void* args[] = {il2::il2cpp_string_new(path)};
    Il2CppObject* found = il2::invoke(api.find_child, transform, args);
    return alive(found) ? found : nullptr;
}

Il2CppObject* in_children(Il2CppObject* component, Il2CppObject* type) {
    if (!alive(component) || !type) return nullptr;
    bool inactive_too = true;
    void* args[] = {type, &inactive_too};
    Il2CppObject* found = il2::invoke(api.in_children, component, args);
    return alive(found) ? found : nullptr;
}

Il2CppObject* label_at(Il2CppObject* panel, const char* path) { return in_children(child(transform_of(panel), path), api.label_type); }

void set_enabled(Il2CppObject* behaviour, bool on) {
    if (!alive(behaviour)) return;
    void* args[] = {&on};
    il2::invoke(api.set_enabled, behaviour, args);
}

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

double now_ms() { return net::now_ms(); }

Il2CppObject* rest() { return il2::invoke(api.get_rest, nullptr); }
Il2CppObject* pass_time() { return il2::invoke(api.get_pass_time, nullptr); }
Il2CppObject* tod() { return il2::invoke(api.get_tod, nullptr); }

float fatigue() { return get<float>(il2::invoke(api.get_fatigue, nullptr), api.fatigue); }

float clock_hours() {
    float t = 0;
    if (Il2CppObject* t_o_d = tod()) il2::invoke_value(api.clock, t_o_d, t);
    return (t - std::floor(t)) * 24.0f;
}

std::string clock_text(float hours) {
    const int minutes = static_cast<int>(hours * 60.0f + 0.5f) % 1440;
    char s[8];
    snprintf(s, sizeof(s), "%02d:%02d", minutes / 60, minutes % 60);
    return s;
}

// The host's clock (ours follows it): now, and TLD's dawn and dusk.
net::SleepClock sleep_clock() {
    net::SleepClock k;
    k.now_h = clock_hours();
    Il2CppObject* storm = get<Il2CppObject*>(tod(), api.tod_storm);
    if (!storm) return k;
    float dawn = 0;
    if (il2::invoke_value(api.dawn, storm, dawn) && dawn > 0) k.dawn_h = std::fmod(dawn, 24.0f);
    Il2CppObject* keys = get<Il2CppObject*>(storm, api.key_times);
    if (il2::array_length(keys) > 5) {
        const float* key = reinterpret_cast<const float*>(reinterpret_cast<const char*>(keys) + 0x20);
        k.dusk_h = std::fmod(get<float>(storm, api.key_offset) + key[5] + 24.0f, 24.0f);
    }
    return k;
}

std::string hours_text(float h) {
    char s[16];
    if (std::fabs(h - std::round(h)) < 0.05f)
        snprintf(s, sizeof(s), "%.0f H", std::round(h));
    else
        snprintf(s, sizeof(s), "%.1f H", h);
    return s;
}

// Real seconds the shared skip takes in every game, sleeping or passing time: TLD's sleep fade.
float skip_seconds() {
    const float s = get<float>(rest(), api.fade_seconds);
    return s > 0 ? s : kSkipSeconds;
}

// --- state ---------------------------------------------------------------------------------------------------------

enum class Mode { Awake, RealTime, Shared };
enum class Kind { Bed, Wait };  // in bed, or waiting to pass time
const char* mode_name(Mode m, Kind k) {
    if (m == Mode::Awake) return "up";
    if (k == Kind::Wait) return m == Mode::RealTime ? "waiting in real time" : "passing time with everyone";
    return m == Mode::RealTime ? "in bed, resting in real time" : "sleeping with everyone";
}

bool g_ready = false;
Mode g_mode = Mode::Awake;
Kind g_kind = Kind::Bed;        // how we're in (while g_mode isn't Awake)
net::SleepChoice g_choice = net::SleepChoice::H8;  // the list's; our vote while in
bool g_screen = false;          // TLD's sleep or pass time screen is up, made co-op's
Kind g_screen_kind = Kind::Bed; // which of the two
uintptr_t g_panel = 0;          // that screen (Panel_Rest)
bool g_saved_pass_allowed = true;
int32_t g_saved_min = 1;
int g_in_do_rest = 0;           // inside TLD's DoRest from co-op's screen: its BeginSleeping is our bed
int g_hold_skip = 0;            // TimeOfDay.Accelerate does nothing: the sleep runs in real time
int g_ending = 0;               // inside Rest.EndSleeping
int g_ours = 0;                 // a sleep we end ourselves (GET UP): nobody is told we were woken
int g_closing = 0;              // we close the screen ourselves (the shared sleep starts)
bool g_close_after = false;     // our shared passing time is over: its screen goes next frame
double g_bed_ms = 0;            // when we went in
float g_bed_clock = 0, g_bed_fatigue = 0;
// The shared sleep.
uint32_t g_shared_id = 0;       // the one we're in (0: none)
uint32_t g_next_id = 0;         // host: the last one started
net::SleepChoice g_shared_choice = net::SleepChoice::None;
float g_shared_hours = 0;
float g_shared_s = 0;           // its real seconds
double g_settled_ms = 0;        // host: since when everyone has been in
double g_done_ms = 0;           // the host's sleep is over (End Done): ours ends by kDoneGraceMs after
std::string g_cut_short;        // why we end our sleep early (finish_now), for its log line
// Telling those who are up.
std::map<uint8_t, uint8_t> g_seen_in_bed;  // how each was in when we last looked (seen_as)
double g_reminded_ms = 0;

// The list's rows, made in the screen from its own parts (build_list).
struct Row {
    uintptr_t hit = 0;     // GameObject: the selector's arrow button, its arrow hidden, as wide as the row
    uintptr_t back = 0;    // UIWidget: the selector's backing, shown behind the picked and the hovered row
    uintptr_t name = 0;    // UILabel: the choice
    uintptr_t voters = 0;  // UILabel: who voted for it
};
Row g_rows[kChoices];
uintptr_t g_list_panel = 0;     // the Panel_Rest they're in
Color g_back_color{1, 1, 1, 1};

// TLD's own description layout, put back for its own screen.
struct Look {
    bool saved = false;
    int description_overflow = 0;
} g_look;

// Players we play with now (the host: friends linked; a friend: the link to the host is up).
bool together() {
    const auto role = netrt::role();
    if (!role) return false;
    const net::Status st = netrt::status();
    if (*role != net::Role::Host) return st.connected;
    return std::any_of(st.players.begin(), st.players.end(), [](const net::PlayerInfo& p) { return p.linked; });
}

std::string player_name(const net::Status& st, uint8_t id) {
    if (id == st.self_id) return "You";
    for (const net::PlayerInfo& p : st.players)
        if (p.id == id) return p.name.empty() ? "Player " + std::to_string(id) : p.name;
    return "Player " + std::to_string(id);
}

// Everyone in the game, us first, from their states.
std::vector<net::Sleeper> everyone(const net::Status& st) {
    std::vector<net::Sleeper> out;
    const bool in = g_mode != Mode::Awake;
    out.push_back({st.self_id, in, in ? g_choice : net::SleepChoice::None, in && g_kind == Kind::Wait});
    const bool host = hosting();
    const std::vector<net::StateMsg> states = netrt::remote_states();
    for (const net::PlayerInfo& p : st.players) {
        if (host && !p.linked) continue;
        net::Sleeper s{p.id, false, net::SleepChoice::None, false};
        for (const net::StateMsg& m : states) {
            if (m.player != p.id || !m.valid || !(m.flags & net::kStateInBed)) continue;
            s.in_bed = true;
            s.waiting = (m.flags & net::kStateWaiting) != 0;
            if (net::is_sleep_choice(m.sleep_vote)) s.vote = static_cast<net::SleepChoice>(m.sleep_vote);
        }
        out.push_back(s);
    }
    return out;
}

std::string names(const net::Status& st, const std::vector<uint8_t>& ids, const std::vector<uint8_t>& resting = {}) {
    std::string s;
    for (uint8_t id : ids)
        s += (s.empty() ? "" : ", ") + player_name(st, id) +
             (std::find(resting.begin(), resting.end(), id) != resting.end() ? " (resting)" : "");
    return s;
}

int last_choice(Kind kind) { return kind == Kind::Bed ? kLastChoice : kLastSleep; }

// 0: up; 1: in for a sleep or to pass time; 2: resting (REST).
uint8_t seen_as(const net::Sleeper& p) { return !p.in_bed ? 0 : p.vote == net::SleepChoice::Rest ? 2 : 1; }

bool resting_now() { return g_mode == Mode::RealTime && g_kind == Kind::Bed && g_choice == net::SleepChoice::Rest; }

std::string votes_text(const net::Status& st, const std::vector<net::Sleeper>& all) {
    std::string s;
    for (const net::Sleeper& p : all)
        if (p.in_bed && p.vote != net::SleepChoice::None)
            s += (s.empty() ? "" : "   ") + player_name(st, p.player) + ": " + net::sleep_choice_name(p.vote) +
                 (p.waiting ? ", waiting" : "");
    return s;
}

void send(const net::SleepMsg& m, int to = -1) {
    net::Writer w(net::Msg::Sleep);
    net::write_sleep(w, m);
    if (to >= 0)
        netrt::send_packet_to(static_cast<uint8_t>(to), w, true);
    else
        netrt::send_packet(w, true);
}

// After "woken": why, if TLD said.
const char* why_text(net::SleepWhy why) {
    switch (why) {
        case net::SleepWhy::Animal: return " by an animal";
        case net::SleepWhy::Cold: return " by the cold";
        case net::SleepWhy::NotInBed: return " (not in bed)";
        case net::SleepWhy::Stopped: return " (stopped passing time)";
        default: return "";
    }
}

// Why TLD woke us (read before EndSleeping clears it).
net::SleepWhy wake_reason(Il2CppObject* r) {
    const int32_t after = get<int32_t>(r, api.interrupt_after);
    if (get<bool>(r, api.predator) && after > 0 && get<float>(r, api.slept_s) >= static_cast<float>(after))
        return net::SleepWhy::Animal;
    Il2CppObject* freezing = il2::invoke(api.get_freezing, nullptr);
    if (get<bool>(r, api.interrupt_cold) && get<float>(freezing, api.freezing) > get<float>(freezing, api.freezing_threshold))
        return net::SleepWhy::Cold;
    return net::SleepWhy::Other;
}

// --- the screen ----------------------------------------------------------------------------------------------------

Il2CppObject* screen() {
    Il2CppObject* p = target(g_panel);
    return alive(p) ? p : nullptr;
}

bool list_alive(Il2CppObject* panel) {
    if (!panel || target(g_list_panel) != panel) return false;
    for (const Row& r : g_rows)
        if (!alive(target(r.hit)) || !alive(target(r.back)) || !alive(target(r.name)) || !alive(target(r.voters)))
            return false;
    return true;
}

void release_list() {
    for (Row& r : g_rows) {
        drop(r.hit);
        drop(r.back);
        drop(r.name);
        drop(r.voters);
    }
    drop(g_list_panel);
}

// A copy of a part of the screen (a Transform) under `parent`.
Il2CppObject* clone_under(Il2CppObject* original, Il2CppObject* parent, const std::string& name) {
    if (!alive(original) || !alive(parent)) return nullptr;
    void* args[] = {original, parent};
    Il2CppObject* copy = il2::invoke(api.instantiate, nullptr, args);
    if (Il2CppObject* go = go_of(copy)) {
        void* name_args[] = {il2::il2cpp_string_new(name.c_str())};
        il2::invoke(api.set_name, go, name_args);
    }
    return alive(copy) ? copy : nullptr;
}

Il2CppObject* row_label(Il2CppObject* source, Il2CppObject* parent, const std::string& name, int font, int pivot, float x,
                        float y) {
    Il2CppObject* copy = clone_under(source, parent, name);
    Il2CppObject* label = in_children(copy, api.label_type);
    set_int(api.label_set_overflow, label, kOverflowResizeFreely);
    set_int(api.label_set_font_size, label, font);
    set_int(api.widget_set_pivot, label, pivot);  // before the position: NGUI keeps a widget in place as its pivot moves
    set_position(copy, {x, y, 0});
    return label;
}

// The list's rows, made once per screen from the screen's own parts: a hit area (the selector's arrow button: a
// click runs OnIncreaseHours, which tells our rows apart by UIButton.current), a backing (the selector's) and two
// labels (the selector's value: the choice, and who voted for it).
bool build_list(Il2CppObject* panel) {
    if (list_alive(panel)) return true;
    release_list();
    Il2CppObject* tf = transform_of(panel);
    Il2CppObject* shared = child(tf, kShared);
    Il2CppObject* hit_source = child(tf, kIncrease);
    Il2CppObject* back_source = child(tf, kValueBack);
    Il2CppObject* label_source = child(tf, kValue);
    if (!shared || !hit_source || !back_source || !label_source) {
        logx::warn("sleep: the sleep screen's parts weren't found; no list");
        return false;
    }
    if (Il2CppObject* back = in_children(back_source, api.widget_type)) il2::invoke_value(api.widget_get_color, back, g_back_color);
    for (int i = 0; i < kChoices; ++i) {
        const float y = kRowTopY - kRowPitch * static_cast<float>(i);
        const std::string name = "TLDCoop_SleepRow" + std::to_string(i + kFirstChoice);
        Row& row = g_rows[i];

        Il2CppObject* back = clone_under(back_source, shared, name + "_Back");
        Il2CppObject* back_widget = in_children(back, api.widget_type);
        set_int(api.widget_set_pivot, back_widget, kPivotCenter);
        set_int(api.widget_set_width, back_widget, kRowWidth);
        set_int(api.widget_set_height, back_widget, kRowHeight);
        set_position(back, {0, y, 0});
        row.back = hold(back_widget);

        row.name = hold(row_label(label_source, shared, name + "_Choice", kNameFont, kPivotLeft, -kRowTextX, y));
        row.voters = hold(row_label(label_source, shared, name + "_Voters", kVotersFont, kPivotRight, kRowTextX, y));

        Il2CppObject* hit = clone_under(hit_source, shared, name);
        Il2CppObject* hit_go = go_of(hit);
        set_active(go_of(child(hit, "Background")), false);  // the arrow
        set_enabled(component(hit_go, api.button_scale_type), false);
        set_enabled(component(hit_go, api.button_offset_type), false);
        if (Il2CppObject* box = component(hit_go, api.box_type)) {
            Vector3 size{static_cast<float>(kRowWidth), static_cast<float>(kRowHeight), 1}, center{0, 0, 0};
            void* size_args[] = {&size};
            void* center_args[] = {&center};
            il2::invoke(api.box_set_size, box, size_args);
            il2::invoke(api.box_set_center, box, center_args);
        }
        set_position(hit, {0, y, 0});
        row.hit = hold(hit_go);
    }
    g_list_panel = hold(panel);
    if (!list_alive(panel)) {
        logx::warn("sleep: the list's rows couldn't be made");
        return false;
    }
    logx::info("sleep: the vote list is made (%d rows)", kChoices);
    return true;
}

void show_list(bool on) {
    for (const Row& r : g_rows)
        for (uintptr_t h : {r.back, r.name, r.voters}) set_active(go_of(target(h)), on);
    for (const Row& r : g_rows) set_active(target(r.hit), on);
}

// Co-op's screen (the list over TLD's hours selector, the description in as many lines as it needs), or TLD's own.
void restyle(Il2CppObject* panel, bool coop) {
    Il2CppObject* tf = transform_of(panel);
    for (const char* path : {kBedDescription, kWaitDescription}) {
        Il2CppObject* description = label_at(panel, path);
        if (!description) continue;
        if (!g_look.saved) {
            g_look.description_overflow = get_int(api.label_get_overflow, description);
            g_look.saved = true;
        }
        set_int(api.label_set_overflow, description, coop ? kOverflowResizeHeight : g_look.description_overflow);
    }
    for (const char* path : {kSelector, kBedCaption, kWaitCaption}) set_active(go_of(child(tf, path)), !coop);
    const bool list = coop && build_list(panel);
    if (list || list_alive(panel)) show_list(list);
    if (coop && !list) set_active(go_of(child(tf, kSelector)), true);  // no list: TLD's selector, as it was
}

// The close button's words in co-op (GET UP, STOP WAITING...), or TLD's own. Its UILocalize puts TLD's "Close" back
// every frame, so it is off meanwhile (on again, it puts "Close" back itself).
void close_says(Il2CppObject* panel, const char* text) {
    Il2CppObject* label = label_at(panel, kCloseLabel);
    set_enabled(in_children(label, api.localize_type), text == nullptr);
    if (text) set_text(label, text);
}

void paint_list(const net::Status& st, const std::vector<net::Sleeper>& all, const net::SleepClock& k) {
    Il2CppObject* hovered = il2::invoke(api.hovered, nullptr);
    for (int i = 0; i < kChoices; ++i) {
        const auto choice = static_cast<net::SleepChoice>(i + kFirstChoice);
        const Row& r = g_rows[i];
        const bool listed = i + kFirstChoice <= last_choice(g_screen_kind);
        for (uintptr_t h : {r.back, r.name, r.voters}) set_active(go_of(target(h)), listed);
        set_active(target(r.hit), listed);
        if (!listed) continue;
        std::string name = net::sleep_choice_name(choice);
        if (choice == net::SleepChoice::Dawn || choice == net::SleepChoice::Dusk)
            name += " (" + hours_text(net::sleep_hours(choice, k)) + ")";
        std::string voters;
        for (const net::Sleeper& p : all)
            if (p.in_bed && p.vote == choice)
                voters += (voters.empty() ? "" : ", ") + player_name(st, p.player) + (p.waiting ? " (waiting)" : "");
        set_text(target(r.name), name);
        set_text(target(r.voters), voters);
        const bool picked = choice == g_choice;
        const bool hover = hovered && hovered == target(r.hit);
        const Color text = picked ? kSelected : hover ? kHovered : kNormal;
        set_color(target(r.name), text);
        set_color(target(r.voters), text);
        Color back = g_back_color;
        back.a *= picked ? kBackSelected : hover ? kBackHovered : 0;
        set_color(target(r.back), back);
    }
}

// Co-op's words on the screen, every frame it's up (TLD's localisation puts its own back when it shows).
void show(Il2CppObject* panel) {
    const net::Status st = netrt::status();
    const std::vector<net::Sleeper> all = everyone(st);
    const net::SleepClock k = sleep_clock();
    const net::SleepOutcome o = net::decide_sleep(all, k);
    const bool wait_screen = g_screen_kind == Kind::Wait;
    const bool in = g_mode != Mode::Awake;
    std::string status;
    if (g_mode == Mode::Shared) {
        status = std::string(g_kind == Kind::Wait ? "Passing time" : "Sleeping") + " with everyone: " +
                 net::sleep_choice_name(g_shared_choice) + " (" + hours_text(g_shared_hours) + ").";
    } else if (!in) {
        const std::string verb = wait_screen ? "PASS TIME" : "SLEEP";
        status = o.kind == net::SleepOutcome::Kind::Nobody
                     ? "Time passes only when everyone is in bed or waiting. Pick how long, then " + verb + " to vote."
                     : "Time passes once everyone is in bed or waiting. Pick how long, then " + verb + " to vote.";
    } else if (resting_now()) {
        status = "Resting: you heal a little faster, in real time. Time doesn't pass for it; pick a sleep to vote for one.";
    } else if (o.kind == net::SleepOutcome::Kind::Shared) {
        status = std::string("Everyone is in: ") + net::sleep_choice_name(o.choice) + " (" + hours_text(o.hours) + ").";
    } else {
        status = std::string(g_kind == Kind::Wait ? "Waiting in real time."
                                                  : "Resting in real time; afflictions heal a little faster.") +
                 " Waiting for: " + names(st, o.awake, o.resting) + ".";
    }
    set_text(label_at(panel, wait_screen ? kWaitTitle : kBedTitle), wait_screen ? "PASS TIME TOGETHER" : "SLEEP TOGETHER");
    set_text(label_at(panel, wait_screen ? kWaitDescription : kBedDescription), status);
    if (list_alive(panel)) paint_list(st, all, k);
    if (in) {
        set_active(get<Il2CppObject*>(panel, api.panel_sleep_button), false);
        set_active(get<Il2CppObject*>(panel, api.panel_pick_up_button), false);
        set_active(get<Il2CppObject*>(panel, api.panel_pass_button), false);
    }
    close_says(panel, !in                       ? nullptr
                      : g_mode == Mode::Shared  ? "STOP"
                      : g_kind == Kind::Wait    ? "STOP WAITING"
                                                : "GET UP");
}

void leave_screen(Il2CppObject* panel) {
    close_says(panel, nullptr);
    put<bool>(panel, api.panel_pass_allowed, g_saved_pass_allowed);
    put<int32_t>(panel, api.panel_min, g_saved_min);
    g_screen = false;
}

void close_screen() {
    Il2CppObject* panel = screen();
    if (!panel || !g_screen) return;
    ++g_closing;
    bool enable = false, pass_only = false;
    void* args[] = {&enable, &pass_only};
    il2::invoke(api.panel_enable, panel, args);
    --g_closing;
}

// A choice picked on the list (a click), as TLD's arrows pick one.
void pick(Il2CppObject* panel, net::SleepChoice choice, const char* how) {
    put<int32_t>(panel, api.panel_hours, static_cast<int32_t>(choice));
    logx::info("sleep: %s %s%s", how, net::sleep_choice_name(choice), g_mode != Mode::Awake ? " (our vote)" : "");
    g_choice = choice;
}

// --- the sleep -----------------------------------------------------------------------------------------------------

void went_in(Kind kind, Il2CppObject* r) {
    const net::SleepClock k = sleep_clock();
    const float hours = net::sleep_hours(g_choice, k);
    if (kind == Kind::Bed) {
        // The real-time rest lasts the hours voted for (at the normal pace) unless everyone sleeps first.
        put<float>(r, api.duration_s, hours * 3600.0f);
        put<float>(r, api.duration_h, hours);
        // A friend's animals are the host's: TLD's roll for a wolf at the bed would pull one of those to us.
        if (!hosting()) {
            put<int32_t>(r, api.interrupt_after, 0);
            put<bool>(r, api.predator, false);
        }
    }
    g_kind = kind;
    g_mode = Mode::RealTime;
    g_bed_ms = now_ms();
    g_bed_clock = k.now_h;
    g_bed_fatigue = fatigue();
    if (kind == Kind::Bed && g_choice == net::SleepChoice::Rest)
        logx::info("sleep: resting in bed (at most %.0f h, from %s): in real time, healing %.2fx; no vote for a sleep",
                   hours, clock_text(k.now_h).c_str(), kRestBoost);
    else if (kind == Kind::Bed)
        logx::info("sleep: in bed (%s, %.2f h from %s): resting in real time until everyone is in",
                   net::sleep_choice_name(g_choice), hours, clock_text(k.now_h).c_str());
    else
        logx::info("sleep: waiting (%s, %.2f h from %s): in real time until everyone is in",
                   net::sleep_choice_name(g_choice), hours, clock_text(k.now_h).c_str());
}

void stop_waiting(const char* how) {
    if (g_mode != Mode::RealTime || g_kind != Kind::Wait) return;
    g_mode = Mode::Awake;
    logx::info("sleep: stopped waiting (%s): %.0f s in real time, the clock %s -> %s", how, (now_ms() - g_bed_ms) / 1000.0,
               clock_text(g_bed_clock).c_str(), clock_text(clock_hours()).c_str());
}

void begin_shared(uint32_t id, net::SleepChoice choice, float hours, float real_s) {
    g_mode = Mode::Shared;
    g_shared_s = real_s;
    g_shared_id = id;
    g_shared_choice = choice;
    g_shared_hours = hours;
    g_done_ms = 0;
    g_bed_clock = clock_hours();
    g_bed_fatigue = fatigue();
    g_bed_ms = now_ms();
}

// Everyone sleeps or passes time `hours` from now, over the same real seconds: our real-time rest becomes TLD's sleep
// with the skip, or our waiting TLD's passing time.
bool start_shared(uint32_t id, net::SleepChoice choice, float hours) {
    if (g_mode != Mode::RealTime || hours <= 0) return false;
    const float real_s = skip_seconds();
    if (g_kind == Kind::Bed) {
        Il2CppObject* r = rest();
        Il2CppObject* t_o_d = tod();
        if (!r || !t_o_d || !get<bool>(r, api.sleeping)) return false;
        const float slept = get<float>(r, api.slept_s);
        put<float>(r, api.duration_s, slept + hours * 3600.0f);
        put<float>(r, api.duration_h, (slept + hours * 3600.0f) / 3600.0f);
        begin_shared(id, choice, hours, real_s);
        close_screen();
        float seconds = real_s;
        bool fade = true;
        void* args[] = {&seconds, &hours, &fade};
        coop_lapse::world_lapse(true);
        il2::invoke(api.accelerate, t_o_d, args);
        coop_lapse::world_lapse(false);
    } else {
        Il2CppObject* panel = screen();
        Il2CppObject* pass = pass_time();
        if (!panel || !pass) return false;
        begin_shared(id, choice, hours, real_s);
        // TLD's passing time takes hours x its seconds per hour: for this one, the sleepers' real seconds.
        const float rate = get<float>(pass, api.pass_rate);
        put<float>(pass, api.pass_rate, real_s / hours);
        float h = hours;
        bool started = false;
        void* args[] = {&h};
        il2::invoke_value(api.panel_do_pass, panel, started, args);  // the hooked entry: ours lets it through (Shared)
        put<float>(pass, api.pass_rate, rate);
        if (!started) {  // TLD said no (afraid, in a cave...): its message says why
            g_mode = Mode::RealTime;
            g_shared_id = 0;
            stop_waiting("TLD wouldn't pass time");
            return false;
        }
    }
    logx::info("sleep: shared sleep #%u: everyone sleeps or waits %s, %.2f h from %s (we %s over %.1f s)", id,
               net::sleep_choice_name(choice), hours, clock_text(g_bed_clock).c_str(),
               g_kind == Kind::Bed ? "sleep" : "pass time", real_s);
    return true;
}

// Our part of shared sleep `id` ended (TLD's sleep or passing time): say so. Woken or stopped: everyone wakes.
void shared_over(uint32_t id, Kind kind, const std::string& how, bool woken, bool cut_short, net::SleepWhy why) {
    g_mode = Mode::Awake;
    g_shared_id = 0;
    g_done_ms = 0;
    g_settled_ms = 0;
    logx::info("sleep: shared sleep #%u is over (%s): the clock %s -> %s in %.0f s (fatigue %+.2f, %s)", id, how.c_str(),
               clock_text(g_bed_clock).c_str(), clock_text(clock_hours()).c_str(), (now_ms() - g_bed_ms) / 1000.0,
               fatigue() - g_bed_fatigue, kind == Kind::Bed ? "slept" : "passed time");
    if (!woken) {
        if (hosting() && !cut_short) {
            net::SleepMsg m;
            m.op = net::SleepOp::End;
            m.id = id;
            m.end = net::SleepEnd::Done;
            send(m);
        }
        return;
    }
    net::SleepMsg m;
    m.id = id;
    m.why = why;
    if (hosting()) {
        m.op = net::SleepOp::End;
        m.end = net::SleepEnd::Woken;
        m.who = netrt::status().self_id;
        send(m);
        logx::info("sleep: we were woken%s: everyone wakes", why_text(why));
    } else {
        m.op = net::SleepOp::Woke;
        send(m);
        logx::info("sleep: we were woken%s: telling the host", why_text(why));
    }
}

// Our shared sleep ends now (someone else was woken, or the host's is over): TLD's sleep ends next frame as if its
// hours were up (TLD logs it and saves, as after any sleep); passing time ends here.
void finish_now(const char* why) {
    if (g_mode != Mode::Shared) return;
    if (g_kind == Kind::Bed) {
        Il2CppObject* r = rest();
        if (!r || !get<bool>(r, api.sleeping)) return;
        put<float>(r, api.duration_s, get<float>(r, api.slept_s));
        g_cut_short = why;
        logx::info("sleep: our sleep ends now (%s)", why);
        return;
    }
    g_cut_short = why;
    logx::info("sleep: our passing time ends now (%s)", why);
    if (Il2CppObject* pass = pass_time()) il2::invoke(api.pass_end, pass);
}

void get_up() {
    Il2CppObject* r = rest();
    if (!r) return;
    ++g_ours;
    bool interrupted = true;
    void* args[] = {&interrupted};
    il2::invoke(api.end_sleeping, r, args);
    --g_ours;
}

// GET UP / STOP WAITING, or the screen closed (Escape): out, before everyone was in.
void leave(const char* how) {
    if (g_kind == Kind::Bed) {
        logx::info("sleep: getting up");
        get_up();  // EndSleeping closes the screen too
    } else {
        stop_waiting(how);
    }
}

// --- hooks ---------------------------------------------------------------------------------------------------------

using EnableFn = void (*)(Il2CppObject* self, bool enable, bool pass_only, const MethodInfo* method);
using VoidFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using DoRestFn = void (*)(Il2CppObject* self, int32_t hours, bool wake_at_full, const MethodInfo* method);
using DoPassFn = bool (*)(Il2CppObject* self, float hours, const MethodInfo* method);
using BeginFn = void (*)(Il2CppObject* self, Il2CppObject* bed, int32_t hours, int32_t max_hours, float fade,
                         int32_t options, Il2CppObject* on_end, const MethodInfo* method);
using EndFn = void (*)(Il2CppObject* self, bool interrupted, const MethodInfo* method);
using BoolFn = bool (*)(Il2CppObject* self, const MethodInfo* method);
using CuresFn = void (*)(Il2CppObject* self, float tod_seconds, const MethodInfo* method);
using AccelerateFn = void (*)(Il2CppObject* self, float real_seconds, float hours, bool fade, const MethodInfo* method);
EnableFn g_enable = nullptr;
VoidFn g_visuals = nullptr, g_pick_up = nullptr, g_increase = nullptr, g_pass_end = nullptr;
DoRestFn g_do_rest = nullptr;
DoPassFn g_do_pass = nullptr;
BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
BoolFn g_unlimited = nullptr;
CuresFn g_cures = nullptr;
AccelerateFn g_accelerate = nullptr;

void enable_detour(Il2CppObject* self, bool enable, bool pass_only, const MethodInfo* method) {
    if (enable) {
        const bool coop = together();
        if (coop) {
            if (!g_screen) {
                g_saved_pass_allowed = get<bool>(self, api.panel_pass_allowed);
                g_saved_min = get<int32_t>(self, api.panel_min);
            }
            // A bed's screen sleeps (the radial passes time): before Enable, TLD then hides the PASS TIME tab.
            if (!pass_only) put<bool>(self, api.panel_pass_allowed, false);
        } else if (g_screen) {
            leave_screen(self);
        }
        g_enable(self, enable, pass_only, method);
        restyle(self, coop);
        if (!coop) return;
        const Kind kind = pass_only ? Kind::Wait : Kind::Bed;
        if (!g_screen || kind != g_screen_kind)
            logx::info("sleep: co-op's %s screen is up (%s)", pass_only ? "pass time" : "sleep", mode_name(g_mode, g_kind));
        g_screen_kind = kind;
        g_screen = true;
        if (screen() != self) {
            drop(g_panel);
            g_panel = il2::il2cpp_gchandle_new(self, false);
        }
        if (static_cast<int>(g_choice) > last_choice(kind)) g_choice = net::SleepChoice::H8;  // no REST passing time
        put<int32_t>(self, api.panel_min, kFirstChoice);
        put<int32_t>(self, api.panel_max, last_choice(kind));
        put<int32_t>(self, api.panel_hours, static_cast<int32_t>(g_choice));
        return;
    }
    if (g_screen && g_mode == Mode::RealTime && !g_ending && !g_closing) {
        if (g_in_do_rest) return;  // TLD closes its screen once we're in bed: here it stays, our vote
        leave("got up");           // GET UP / STOP WAITING or Escape: out
    }
    if (g_screen) leave_screen(self);
    g_enable(self, enable, pass_only, method);
}

void visuals_detour(Il2CppObject* self, const MethodInfo* method) {
    if (!g_screen || self != screen()) return g_visuals(self, method);
    const int idx = std::clamp(get<int32_t>(self, api.panel_hours), kFirstChoice, last_choice(g_screen_kind));
    g_choice = static_cast<net::SleepChoice>(idx);
    // TLD's own lines (the calories it burns...) for the choice's hours; the list's place back after.
    const float hours = net::sleep_hours(g_choice, sleep_clock());
    put<int32_t>(self, api.panel_hours, std::max(1, static_cast<int32_t>(std::lround(hours))));
    put<int32_t>(self, api.panel_max, 24);
    g_visuals(self, method);
    put<int32_t>(self, api.panel_hours, idx);
    put<int32_t>(self, api.panel_max, last_choice(g_screen_kind));
    show(self);
}

// A click on one of our rows comes here (it's the arrow button's copy): that row's choice. TLD's own arrow and keys
// step through the list as before.
void increase_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_screen && self == screen()) {
        Il2CppObject* button = nullptr;
        il2::il2cpp_field_static_get_value(api.button_current, &button);
        Il2CppObject* go = alive(button) ? go_of(button) : nullptr;
        for (int i = 0; go && i < kChoices; ++i)
            if (go == target(g_rows[i].hit)) {
                pick(self, static_cast<net::SleepChoice>(i + kFirstChoice), "clicked");
                return;
            }
    }
    g_increase(self, method);
}

void do_rest_detour(Il2CppObject* self, int32_t hours, bool wake_at_full, const MethodInfo* method) {
    if (!g_screen || self != screen()) return g_do_rest(self, hours, wake_at_full, method);
    if (g_mode != Mode::Awake) return;  // in: the list is the vote
    const int idx = std::clamp(get<int32_t>(self, api.panel_hours), kFirstChoice, kLastChoice);
    g_choice = static_cast<net::SleepChoice>(idx);
    const int whole = std::max(1, static_cast<int>(std::ceil(net::sleep_hours(g_choice, sleep_clock()))));
    ++g_in_do_rest;
    g_do_rest(self, whole, false, method);  // TLD's checks, then BeginSleeping: ours
    --g_in_do_rest;
    put<int32_t>(self, api.panel_hours, idx);
}

bool do_pass_detour(Il2CppObject* self, float hours, const MethodInfo* method) {
    if (g_mode == Mode::Shared && g_kind == Kind::Wait) return g_do_pass(self, hours, method);  // start_shared's
    if (!together()) return g_do_pass(self, hours, method);
    if (!g_screen || self != screen() || g_mode != Mode::Awake) return false;  // in already: the list is the vote
    bool can = false;
    il2::invoke_value(api.panel_pass_check, nullptr, can);  // TLD's own message says why not
    if (!can) {
        logx::info("sleep: TLD won't pass time here now (its message says why)");
        return false;
    }
    went_in(Kind::Wait, nullptr);
    return true;
}

void pick_up_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_screen && g_mode != Mode::Awake) return;  // not the bedroll we lie in
    g_pick_up(self, method);
}

void begin_detour(Il2CppObject* self, Il2CppObject* bed, int32_t hours, int32_t max_hours, float fade, int32_t options,
                  Il2CppObject* on_end, const MethodInfo* method) {
    const bool ours = g_in_do_rest > 0;
    if (ours) options &= ~kCancelButton;  // it would sit on GET UP (it cancels click-holds, not sleep)
    coop_lapse::world_lapse(true);
    if (ours) ++g_hold_skip;
    g_begin(self, bed, hours, max_hours, fade, options, on_end, method);
    if (ours) --g_hold_skip;
    coop_lapse::world_lapse(false);
    if (ours && get<bool>(self, api.sleeping)) went_in(Kind::Bed, self);
}

void end_detour(Il2CppObject* self, bool interrupted, const MethodInfo* method) {
    const Mode mode = g_mode;
    const bool ours = mode != Mode::Awake && g_kind == Kind::Bed;
    const uint32_t id = g_shared_id;
    const net::SleepWhy why = ours && interrupted && !g_ours ? wake_reason(self) : net::SleepWhy::None;
    const float slept_h = get<float>(self, api.slept_s) / 3600.0f;
    ++g_ending;
    g_end(self, interrupted, method);
    --g_ending;
    if (!ours) return;
    const std::string how = g_ours ? "got up" : interrupted ? "woken" : !g_cut_short.empty() ? g_cut_short : "its hours are up";
    const bool cut_short = !g_cut_short.empty();
    g_cut_short.clear();
    if (mode == Mode::RealTime) {
        g_mode = Mode::Awake;
        logx::info("sleep: out of bed (%s): rested %.2f h in real time in %.0f s, the clock %s -> %s (fatigue %+.2f)",
                   how.c_str(), slept_h, (now_ms() - g_bed_ms) / 1000.0, clock_text(g_bed_clock).c_str(),
                   clock_text(clock_hours()).c_str(), fatigue() - g_bed_fatigue);
        return;
    }
    shared_over(id, Kind::Bed, how, interrupted && !g_ours, cut_short, why);
}

// TLD's passing time ends: its hours are up, the player stopped it (Escape, STOP), or fear did.
void pass_end_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_mode != Mode::Shared || g_kind != Kind::Wait) return g_pass_end(self, method);
    bool afraid = false;
    il2::invoke_value(api.pass_afraid, self, afraid);
    const uint32_t id = g_shared_id;
    // TLD ends it on its own timer after the skip's real seconds: earlier, the player or fear stopped it.
    const bool early = now_ms() - g_bed_ms < g_shared_s * 1000.0 - kDoneSlackMs;
    g_pass_end(self, method);
    const bool cut_short = !g_cut_short.empty();
    const bool woken = !cut_short && early;
    const std::string how = cut_short ? g_cut_short : !woken ? "its hours are up" : afraid ? "afraid" : "stopped";
    g_cut_short.clear();
    g_close_after = true;
    shared_over(id, Kind::Wait, how, woken, cut_short,
                !woken ? net::SleepWhy::None : afraid ? net::SleepWhy::Animal : net::SleepWhy::Stopped);
}

bool unlimited_detour(Il2CppObject* self, const MethodInfo* method) {
    // Co-op's bed: no "not tired" refusal (anyone may vote), and nobody wakes early once rested.
    if (g_screen || g_mode != Mode::Awake) return true;
    return g_unlimited(self, method);
}

void cures_detour(Il2CppObject* self, float tod_seconds, const MethodInfo* method) {
    const float boost = resting_now() ? kRestBoost : g_mode == Mode::RealTime && g_kind == Kind::Bed ? kCureBoost : 1.0f;
    g_cures(self, tod_seconds * boost, method);
}

void accelerate_detour(Il2CppObject* self, float real_seconds, float hours, bool fade, const MethodInfo* method) {
    if (g_hold_skip) return;  // co-op's bed: the sleep runs in real time
    g_accelerate(self, real_seconds, hours, fade, method);
}

// --- messages ------------------------------------------------------------------------------------------------------

std::string woke_text(const std::string& who, net::SleepWhy why) {
    if (why == net::SleepWhy::Stopped) return who + " stopped passing time";
    return who + (who == "You" ? " were woken" : " was woken") + why_text(why);
}

void receive() {
    static std::vector<net::Packet> in;
    in.clear();
    netrt::drain_packets(net::Msg::Sleep, in);
    const net::Status st = netrt::status();
    for (const net::Packet& p : in) {
        net::Reader r = net::packet_body(p);
        net::SleepMsg m;
        if (!net::read_sleep(r, m)) continue;
        if (m.op == net::SleepOp::Start && !hosting()) {
            logx::info("sleep: the host starts shared sleep #%u: %s, %.2f h", m.id,
                       net::sleep_choice_name(static_cast<net::SleepChoice>(m.choice)), m.hours);
            if (!start_shared(m.id, static_cast<net::SleepChoice>(m.choice), m.hours)) {
                net::SleepMsg back;
                back.op = net::SleepOp::Woke;
                back.id = m.id;
                back.why = net::SleepWhy::NotInBed;
                send(back);
                logx::info("sleep: we aren't in for shared sleep #%u: telling the host", m.id);
            }
        } else if (m.op == net::SleepOp::End && !hosting()) {
            if (m.id != g_shared_id || g_mode != Mode::Shared) continue;
            if (m.end == net::SleepEnd::Done) {
                g_done_ms = now_ms();
                continue;
            }
            const std::string text = woke_text(player_name(st, m.who), m.why);
            coop_notices::tell(text + ": everyone wakes.");
            finish_now(text.c_str());
        } else if (m.op == net::SleepOp::Woke && hosting()) {
            if (m.id != g_shared_id) continue;
            const std::string text = woke_text(player_name(st, p.from), m.why);
            logx::info("sleep: %s in shared sleep #%u: everyone wakes", text.c_str(), m.id);
            net::SleepMsg end;
            end.op = net::SleepOp::End;
            end.id = m.id;
            end.end = net::SleepEnd::Woken;
            end.who = p.from;
            end.why = m.why;
            send(end);
            coop_notices::tell(text + ": everyone wakes.");
            finish_now(text.c_str());
        }
    }
}

// Host: everyone in (and settled) starts the shared sleep.
void decide() {
    if (!hosting() || g_mode != Mode::RealTime) {
        g_settled_ms = 0;
        return;
    }
    const net::Status st = netrt::status();
    const std::vector<net::Sleeper> all = everyone(st);
    const net::SleepOutcome o = net::decide_sleep(all, sleep_clock());
    if (o.kind != net::SleepOutcome::Kind::Shared) {
        g_settled_ms = 0;
        return;
    }
    const double now = now_ms();
    if (!g_settled_ms) g_settled_ms = now;
    if (now - g_settled_ms < kSettleMs) return;
    g_settled_ms = 0;
    const uint32_t id = ++g_next_id;
    logx::info("sleep: everyone is in (%s): %s wins, %.2f h", votes_text(st, all).c_str(),
               net::sleep_choice_name(o.choice), o.hours);
    net::SleepMsg m;
    m.op = net::SleepOp::Start;
    m.id = id;
    m.choice = static_cast<uint8_t>(o.choice);
    m.hours = o.hours;
    if (all.size() > 1) send(m);
    if (start_shared(id, o.choice, o.hours)) return;
    // We couldn't (TLD won't pass time here): the others wake.
    net::SleepMsg end;
    end.op = net::SleepOp::End;
    end.id = id;
    end.end = net::SleepEnd::Woken;
    end.who = st.self_id;
    end.why = net::SleepWhy::NotInBed;
    if (all.size() > 1) send(end);
    logx::info("sleep: we couldn't start shared sleep #%u: everyone wakes", id);
}

// Up while others are in: TLD's HUD says who and asks us to join them.
void tell_the_awake() {
    if (!together()) {
        g_seen_in_bed.clear();
        return;
    }
    const net::Status st = netrt::status();
    const std::vector<net::Sleeper> all = everyone(st);
    const double now = now_ms();
    if (g_mode != Mode::Awake) {
        // In, we see them on our screen. Those in with us aren't news once we're up (a friend's state still says in
        // for a moment after a shared sleep), nor a reminder until kRemindMs later.
        for (const net::Sleeper& p : all) g_seen_in_bed[p.player] = seen_as(p);
        g_reminded_ms = now;
        return;
    }
    std::vector<uint8_t> beds;  // in for a sleep (not resting)
    std::string news;
    for (const net::Sleeper& p : all) {
        if (p.player == st.self_id) continue;
        uint8_t& seen = g_seen_in_bed[p.player];
        const bool rests = p.vote == net::SleepChoice::Rest;
        if (p.in_bed && !rests) beds.push_back(p.player);
        if (p.in_bed && seen != seen_as(p))  // went in, or from a sleep to REST in bed or back
            news += (news.empty() ? "" : " ") + player_name(st, p.player) +
                    (rests       ? std::string(" is resting.")
                     : p.waiting ? std::string(" is waiting to pass time (") + net::sleep_choice_name(p.vote) + ")."
                                 : std::string(" went to bed (") + net::sleep_choice_name(p.vote) + ").");
        seen = seen_as(p);
    }
    if (beds.empty()) {
        if (!news.empty()) {
            coop_notices::tell(news);
            logx::info("sleep: told: %s", news.c_str());
        }
        return;
    }
    const std::string count = std::to_string(beds.size()) + " of " + std::to_string(all.size()) + " in";
    if (!news.empty()) {
        coop_notices::tell(news + " Time passes once everyone sleeps or waits: " + count + ".");
        logx::info("sleep: told: %s (%s)", news.c_str(), count.c_str());
        g_reminded_ms = now;
    } else if (now - g_reminded_ms >= kRemindMs) {
        coop_notices::tell(names(st, beds) + " waiting for you to sleep or wait (" + count + ").");
        logx::info("sleep: reminded: %s in", names(st, beds).c_str());
        g_reminded_ms = now;
    }
}

Il2CppObject* nearest_bed() {
    void* args[] = {api.bed_type};
    Il2CppObject* found = il2::invoke(api.find_objects, nullptr, args);
    Il2CppObject* player = il2::invoke(api.player_transform, nullptr);
    Vector3 me{};
    if (player) il2::invoke_value(api.get_position, player, me);
    Il2CppObject* best = nullptr;
    float best_d = 1e30f;
    for (size_t i = 0; i < il2::array_length(found); ++i) {
        Il2CppObject* bed = il2::array_object_at(found, i);
        Vector3 at{};
        if (!alive(bed) || !il2::invoke_value(api.get_position, transform_of(bed), at)) continue;
        const float d = (at.x - me.x) * (at.x - me.x) + (at.y - me.y) * (at.y - me.y) + (at.z - me.z) * (at.z - me.z);
        if (d < best_d) {
            best_d = d;
            best = bed;
        }
    }
    if (best) logx::info("dev: sleep: the nearest bed is %.1f m away", std::sqrt(best_d));
    return best;
}

net::SleepChoice choice_named(const std::string& name) {
    static const std::pair<const char*, net::SleepChoice> kNames[] = {
        {"4", net::SleepChoice::H4}, {"8", net::SleepChoice::H8}, {"12", net::SleepChoice::H12},
        {"dawn", net::SleepChoice::Dawn}, {"dusk", net::SleepChoice::Dusk}, {"rest", net::SleepChoice::Rest}};
    for (const auto& [n, choice] : kNames)
        if (name == n) return choice;
    return net::SleepChoice::None;
}

}  // namespace

bool in_bed() { return g_mode != Mode::Awake; }
bool resting() { return resting_now(); }

bool bed_pose(float out[7]) {
    if (!g_ready || g_mode == Mode::Awake || g_kind != Kind::Bed) return false;
    Il2CppObject* r = rest();
    Il2CppObject* bed = r && api.rest_bed >= 0 ? get<Il2CppObject*>(r, api.rest_bed) : nullptr;
    if (!alive(bed)) return false;
    Il2CppObject* place = api.bed_place >= 0 ? get<Il2CppObject*>(bed, api.bed_place) : nullptr;
    if (!alive(place)) place = transform_of(bed);
    Vector3 p{};
    struct {
        float x, y, z, w;
    } q{0, 0, 0, 1};
    if (!place || !il2::invoke_value(api.get_position, place, p) || !api.get_rotation ||
        !il2::invoke_value(api.get_rotation, place, q))
        return false;
    const float values[7] = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
    for (int i = 0; i < 7; ++i) out[i] = values[i];
    return true;
}
bool waiting() { return g_mode != Mode::Awake && g_kind == Kind::Wait; }
uint8_t vote() { return g_mode != Mode::Awake ? static_cast<uint8_t>(g_choice) : 0; }

void on_frame(const std::string& scene, bool in_game) {
    (void)scene;
    if (!g_ready) return;
    receive();
    if (!in_game) {
        if (g_mode != Mode::Awake) logx::info("sleep: left the game while %s", mode_name(g_mode, g_kind));
        g_mode = Mode::Awake;
        g_screen = false;
        g_shared_id = 0;
        g_settled_ms = g_done_ms = 0;
        g_close_after = false;
        g_cut_short.clear();
        g_seen_in_bed.clear();
        return;
    }
    decide();
    coop_lapse::heal_boost(resting_now() ? kRestBoost : 1.0f);
    if (g_mode == Mode::Shared && g_done_ms > 0 && now_ms() - g_done_ms >= kDoneGraceMs) {
        g_done_ms = 0;
        finish_now("the host's sleep is over");
    }
    if (g_close_after) {
        g_close_after = false;
        if (g_mode == Mode::Awake) close_screen();
    }
    tell_the_awake();
}

void describe() {
    const net::Status st = netrt::status();
    const std::vector<net::Sleeper> all = everyone(st);
    const net::SleepClock k = sleep_clock();
    const net::SleepOutcome o = net::decide_sleep(all, k);
    Il2CppObject* r = rest();
    std::string people;
    for (const net::Sleeper& p : all)
        people += (people.empty() ? "" : "; ") + player_name(st, p.player) +
                  (!p.in_bed ? " up (" : p.waiting ? " waiting (" : " in bed (") + net::sleep_choice_name(p.vote) + ")";
    static const char* kKinds[] = {"nobody in", "waiting", "everyone in"};
    logx::info("dev: sleep: we're %s, choice %s, screen %s (%s), list %s, together %s; clock %s (dawn %s, dusk %s: %.2f h "
               "/ %.2f h); fatigue %.2f; TLD's sleep %s, %.2f of %.2f h; %s; outcome: %s (%s, %.2f h)",
               mode_name(g_mode, g_kind), net::sleep_choice_name(g_choice), g_screen ? "up" : "down",
               g_screen_kind == Kind::Wait ? "pass time" : "sleep", list_alive(screen()) ? "made" : "none",
               together() ? "yes" : "no", clock_text(k.now_h).c_str(), clock_text(k.dawn_h).c_str(),
               clock_text(k.dusk_h).c_str(), net::sleep_hours(net::SleepChoice::Dawn, k),
               net::sleep_hours(net::SleepChoice::Dusk, k), fatigue(), get<bool>(r, api.sleeping) ? "on" : "off",
               get<float>(r, api.slept_s) / 3600.0f, get<float>(r, api.duration_h), people.c_str(),
               kKinds[static_cast<int>(o.kind)], net::sleep_choice_name(o.choice), o.hours);
}

bool dev(const std::string& action, const std::string& arg) {
    if (!g_ready || !rest()) return false;
    if (action == "open" || action == "wait") {
        Il2CppObject* bed = action == "open" ? nearest_bed() : nullptr;
        void* load_args[] = {il2::type_object(api.panel_class)};
        Il2CppObject* panel = il2::invoke(api.load_panel, nullptr, load_args);
        if (!panel || (action == "open" && !bed)) return false;
        if (bed) {
            void* args[] = {bed};
            il2::invoke(api.panel_start, panel, args);
        } else {
            put<Il2CppObject*>(panel, api.panel_bed, nullptr);  // passing time where we stand, as the radial does
            bool on = true, pass_only = true;
            void* args[] = {&on, &pass_only};
            il2::invoke(api.panel_enable, panel, args);
        }
        return true;
    }
    if (action == "wake") {
        if (g_mode == Mode::Shared && g_kind == Kind::Wait) {
            il2::invoke(api.pass_end, pass_time());  // stopped, as Escape does
            return true;
        }
        Il2CppObject* r = rest();
        if (!get<bool>(r, api.sleeping)) return false;
        bool interrupted = true;
        void* args[] = {&interrupted};
        il2::invoke(api.end_sleeping, r, args);
        return true;
    }
    Il2CppObject* panel = screen();
    if (!panel || !g_screen) return false;
    if (action == "pick") {
        const net::SleepChoice choice = choice_named(arg);
        if (choice == net::SleepChoice::None) return false;
        put<int32_t>(panel, api.panel_hours, static_cast<int32_t>(choice));  // as the arrows do
        g_choice = choice;
        logx::info("dev: sleep: picked %s", net::sleep_choice_name(choice));
        return true;
    }
    if (action == "click") {
        const net::SleepChoice choice = choice_named(arg);
        if (choice == net::SleepChoice::None || !list_alive(panel)) return false;
        Il2CppObject* button = component(target(g_rows[static_cast<int>(choice) - kFirstChoice].hit), api.button_type);
        if (!button) return false;
        logx::info("dev: sleep: clicking %s", net::sleep_choice_name(choice));
        il2::invoke(api.button_on_click, button);  // NGUI's click: its onClick, with UIButton.current set
        return true;
    }
    if (action == "go") return il2::invoke(g_screen_kind == Kind::Wait ? api.panel_on_pass : api.panel_on_rest, panel), true;
    if (action == "up") return il2::invoke(api.panel_on_cancel, panel), true;
    return false;
}

bool init() {
    Il2CppClass* component = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* box = il2::find_class(kPhysics, "UnityEngine", "BoxCollider");
    api.component_go = il2::find_method(component, "get_gameObject", 0);
    api.component_transform = il2::find_method(component, "get_transform", 0);
    api.in_children = il2::find_method_params(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    api.go_component = il2::find_method_params(game_object, "GetComponent", {"System.Type"});
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.set_enabled = il2::find_method(il2::find_class(kCore, "UnityEngine", "Behaviour"), "set_enabled", 1);
    api.find_child = il2::find_method_params(transform, "Find", {"System.String"});
    api.get_position = il2::find_method(transform, "get_position", 0);
    api.set_local_position = il2::find_method(transform, "set_localPosition", 1);
    api.find_objects = il2::find_method_params(object, "FindObjectsOfType", {"System.Type"});
    api.instantiate = il2::find_method_params(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
    api.set_name = il2::find_method(object, "set_name", 1);
    api.box_set_size = il2::find_method(box, "set_size", 1);
    api.box_set_center = il2::find_method(box, "set_center", 1);

    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    Il2CppClass* widget = il2::find_class(kGame, "", "UIWidget");
    Il2CppClass* button = il2::find_class(kGame, "", "UIButton");
    auto keep_type = [](Il2CppClass* klass) {
        Il2CppObject* type = klass ? il2::type_object(klass) : nullptr;
        if (type) il2::il2cpp_gchandle_new(type, false);
        return type;
    };
    api.box_type = keep_type(box);
    api.label_type = keep_type(label);
    api.widget_type = keep_type(widget);
    api.localize_type = keep_type(il2::find_class(kGame, "", "UILocalize"));
    api.button_type = keep_type(button);
    api.button_scale_type = keep_type(il2::find_class(kGame, "", "UIButtonScale"));
    api.button_offset_type = keep_type(il2::find_class(kGame, "", "UIButtonOffset"));
    api.button_on_click = il2::find_method(button, "OnClick", 0);
    if (button) api.button_current = il2::il2cpp_class_get_field_from_name(button, "current");
    api.hovered = il2::find_method(il2::find_class(kGame, "", "UICamera"), "get_hoveredObject", 0);
    api.label_set_text = il2::find_method(label, "set_text", 1);
    api.label_set_font_size = il2::find_method(label, "set_fontSize", 1);
    api.label_get_overflow = il2::find_method(label, "get_overflowMethod", 0);
    api.label_set_overflow = il2::find_method(label, "set_overflowMethod", 1);
    api.widget_set_width = il2::find_method(widget, "set_width", 1);
    api.widget_set_height = il2::find_method(widget, "set_height", 1);
    api.widget_set_pivot = il2::find_method(widget, "set_pivot", 1);
    api.widget_get_color = il2::find_method(widget, "get_color", 0);
    api.widget_set_color = il2::find_method(widget, "set_color", 1);

    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    Il2CppClass* rest_class = il2::find_class(kGame, "", "Rest");
    Il2CppClass* pass = il2::find_class(kGame, "", "PassTime");
    Il2CppClass* tod_class = il2::find_class(kGame, "", "TimeOfDay");
    Il2CppClass* storm = il2::find_class(kGame, "", "UniStormWeatherSystem");
    Il2CppClass* freezing = il2::find_class(kGame, "", "Freezing");
    api.get_rest = il2::find_method(game_manager, "GetRestComponent", 0);
    api.get_pass_time = il2::find_method(game_manager, "GetPassTime", 0);
    api.get_tod = il2::find_method(game_manager, "GetTimeOfDayComponent", 0);
    api.get_fatigue = il2::find_method(game_manager, "GetFatigueComponent", 0);
    api.get_freezing = il2::find_method(game_manager, "GetFreezingComponent", 0);
    api.player_transform = il2::find_method(game_manager, "GetPlayerTransform", 0);
    api.load_panel = il2::find_method_params(il2::find_class(kGame, "", "InterfaceManager"), "LoadPanel", {"System.Type"});
    api.bed_type = keep_type(il2::find_class(kGame, "", "Bed"));

    Il2CppClass* panel = il2::find_class(kGame, "", "Panel_Rest");
    api.panel_class = panel;
    api.panel_enable = il2::find_method_params(panel, "Enable", {"System.Boolean", "System.Boolean"});
    api.panel_visuals = il2::find_method(panel, "UpdateVisuals", 0);
    api.panel_do_rest = il2::find_method(panel, "DoRest", 2);
    api.panel_do_pass = il2::find_method(panel, "DoPassTime", 1);
    api.panel_pass_check = il2::find_method(panel, "CheckPassTimeErrorMessage", 0);
    api.panel_pick_up = il2::find_method(panel, "OnPickUp", 0);
    api.panel_start = il2::find_method(panel, "StartRest", 1);
    api.panel_on_rest = il2::find_method(panel, "OnRest", 0);
    api.panel_on_pass = il2::find_method(panel, "OnPassTime", 0);
    api.panel_on_cancel = il2::find_method(panel, "OnCancel", 0);
    api.panel_increase = il2::find_method(panel, "OnIncreaseHours", 0);
    api.panel_hours = il2::field_offset(panel, "m_SleepHours");
    api.panel_min = il2::field_offset(panel, "m_MinSleepHours");
    api.panel_max = il2::field_offset(panel, "m_CurrentMaxSleepHours");
    api.panel_pass_allowed = il2::field_offset(panel, "m_PassTimeIsAllowed");
    api.panel_bed = il2::field_offset(panel, "m_Bed");
    api.panel_sleep_button = il2::field_offset(panel, "m_SleepButton");
    api.panel_pick_up_button = il2::field_offset(panel, "m_PickUpButton");
    api.panel_pass_button = il2::field_offset(panel, "m_PassTimeButtonObject");

    api.begin_sleeping = il2::find_method(rest_class, "BeginSleeping", 6);
    api.end_sleeping = il2::find_method(rest_class, "EndSleeping", 1);
    api.unlimited = il2::find_method(rest_class, "AllowUnlimitedSleep", 0);
    api.cures = il2::find_method(rest_class, "UpdateRestForCures", 1);
    api.sleeping = il2::field_offset(rest_class, "m_Sleeping");
    api.slept_s = il2::field_offset(rest_class, "m_NumSecondsSleeping");
    api.duration_s = il2::field_offset(rest_class, "m_SleepDurationSeconds");
    api.duration_h = il2::field_offset(rest_class, "m_SleepDurationHours");
    api.interrupt_after = il2::field_offset(rest_class, "m_InterruptionAfterSecondsSleeping");
    api.predator = il2::field_offset(rest_class, "m_PredatorInterruption");
    api.interrupt_cold = il2::field_offset(rest_class, "m_ShouldInterruptWhenFreezing");
    api.fade_seconds = il2::field_offset(rest_class, "m_SleepFadeOutSeconds");

    api.pass_end = il2::find_method(pass, "End", 0);
    api.pass_afraid = il2::find_method(pass, "HasFearOrAnxiety", 0);
    api.pass_rate = il2::field_offset(pass, "m_PassTimeRealtimeSecondsPerHour");

    api.accelerate = il2::find_method_params(tod_class, "Accelerate", {"System.Single", "System.Single", "System.Boolean"});
    api.clock = il2::find_method(tod_class, "GetNormalizedTime", 0);
    api.tod_storm = il2::field_offset(tod_class, "m_WeatherSystem");
    api.dawn = il2::find_method(storm, "GetDawnTime", 0);
    api.key_offset = il2::field_offset(storm, "m_MasterTimeKeyOffset");
    api.key_times = il2::field_offset(storm, "m_TODKeyframeTimes");
    api.fatigue = il2::field_offset(il2::find_class(kGame, "", "Fatigue"), "m_CurrentFatigue");
    api.freezing = il2::field_offset(freezing, "m_CurrentFreezing");
    api.freezing_threshold = il2::field_offset(freezing, "m_FreezingThreshold");
    api.rest_bed = il2::field_offset(rest_class, "m_Bed");
    api.bed_place = il2::field_offset(il2::find_class(kGame, "", "Bed"), "m_BodyPlacementTransform");
    api.get_rotation = il2::find_method(transform, "get_rotation", 0);

    g_ready = api.component_go && api.component_transform && api.in_children && api.go_component && api.go_set_active &&
              api.set_enabled && api.find_child && api.get_position && api.set_local_position && api.find_objects &&
              api.instantiate && api.set_name && api.box_type && api.box_set_size && api.box_set_center &&
              api.label_type && api.widget_type && api.localize_type && api.button_type && api.button_scale_type &&
              api.button_offset_type && api.button_on_click && api.button_current && api.hovered &&
              api.label_set_text && api.label_set_font_size && api.label_get_overflow && api.label_set_overflow &&
              api.widget_set_width && api.widget_set_height && api.widget_set_pivot && api.widget_get_color &&
              api.widget_set_color && api.get_rest && api.get_pass_time && api.get_tod && api.get_fatigue &&
              api.get_freezing && api.player_transform && api.load_panel && api.bed_type && api.panel_enable &&
              api.panel_visuals && api.panel_do_rest && api.panel_do_pass && api.panel_pass_check &&
              api.panel_pick_up && api.panel_start && api.panel_on_rest && api.panel_on_pass && api.panel_on_cancel &&
              api.panel_increase && api.panel_hours >= 0 && api.panel_min >= 0 && api.panel_max >= 0 &&
              api.panel_pass_allowed >= 0 && api.panel_bed >= 0 && api.panel_sleep_button >= 0 &&
              api.panel_pick_up_button >= 0 && api.panel_pass_button >= 0 && api.begin_sleeping &&
              api.end_sleeping && api.unlimited && api.cures && api.sleeping >= 0 && api.slept_s >= 0 &&
              api.duration_s >= 0 && api.duration_h >= 0 && api.interrupt_after >= 0 && api.predator >= 0 &&
              api.interrupt_cold >= 0 && api.fade_seconds >= 0 && api.pass_end && api.pass_afraid &&
              api.pass_rate >= 0 && api.accelerate && api.clock && api.tod_storm >= 0 && api.dawn &&
              api.key_offset >= 0 && api.key_times >= 0 && api.fatigue >= 0 && api.freezing >= 0 &&
              api.freezing_threshold >= 0;
    if (!g_ready) {
        logx::warn("sleep: UNAVAILABLE (TLD API missing; sleep and passing time are TLD's own in co-op)");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto detour, auto original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    g_ready = hook("TimeOfDay.Accelerate", api.accelerate, &accelerate_detour, &g_accelerate) &&
              hook("Rest.BeginSleeping", api.begin_sleeping, &begin_detour, &g_begin) &&
              hook("Rest.EndSleeping", api.end_sleeping, &end_detour, &g_end) &&
              hook("Rest.AllowUnlimitedSleep", api.unlimited, &unlimited_detour, &g_unlimited) &&
              hook("Rest.UpdateRestForCures", api.cures, &cures_detour, &g_cures) &&
              hook("PassTime.End", api.pass_end, &pass_end_detour, &g_pass_end) &&
              hook("Panel_Rest.Enable", api.panel_enable, &enable_detour, &g_enable) &&
              hook("Panel_Rest.UpdateVisuals", api.panel_visuals, &visuals_detour, &g_visuals) &&
              hook("Panel_Rest.OnIncreaseHours", api.panel_increase, &increase_detour, &g_increase) &&
              hook("Panel_Rest.DoRest", api.panel_do_rest, &do_rest_detour, &g_do_rest) &&
              hook("Panel_Rest.DoPassTime", api.panel_do_pass, &do_pass_detour, &g_do_pass) &&
              hook("Panel_Rest.OnPickUp", api.panel_pick_up, &pick_up_detour, &g_pick_up);
    logx::info("sleep: %s", g_ready ? "ready (in co-op, time passes for sleep or waiting only when everyone is in)"
                                    : "UNAVAILABLE (hooks failed)");
    return g_ready;
}

}  // namespace coop_sleep
