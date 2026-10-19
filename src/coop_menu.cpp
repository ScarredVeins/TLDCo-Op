#include "coop_menu.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "avatar.h"
#include "config.h"
#include "core.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace coop_menu {
namespace {

constexpr const char* kCore = "UnityEngine.CoreModule.dll";
constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule.dll";
struct Vector2 {
    float x, y;
};
struct Vector3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};

constexpr int kPanelDepth = 60;  // above TLD's menu screens (53)

// Where the parts of a TLD menu screen live, under its menu root (from TLDCoop/ui/Panel_*.txt). The two screens we
// clone share this layout and differ only in the root's name.
constexpr const char* kSandboxMenu = "RootMenu/Menu";   // Panel_Sandbox (the Survival menu)
constexpr const char* kSetupMenu = "MenuRoot/Menu";     // Panel_SelectExperience (difficulty), which setup stands in for
constexpr const char* kTitle = "Title";
constexpr const char* kTitleSmall = "Title/LabelHeaderTitle";
constexpr const char* kTitleBig = "Title/LabelTitle";
constexpr const char* kGrid = "Left_Align/Grid";
constexpr const char* kHighlight = "Left_Align/Selected";
constexpr const char* kDescription = "Left_Align/Description/DescriptionLabel";
constexpr const char* kArrowTemplate = "Left_Align/Scrollbar/Scrollbar/Button_ScrollUp";
constexpr const char* kConfirmRoot = "MouseButtonConfirm/MouseConfirm/Mouse_Button_Confirm";
constexpr const char* kConfirmButton = "MouseButtonConfirm/MouseConfirm/Mouse_Button_Confirm/ButtonActions";
constexpr const char* kBackGroup = "MouseButtonBack";
constexpr const char* kBackRoot = "MouseButtonBack/MouseBack/Button_Back";
constexpr const char* kBackButton = "MouseButtonBack/MouseBack/Button_Back/ButtonBottomRightMouse";
constexpr const char* kBackHome = "MouseButtonBackToHome";
constexpr const char* kExperienceExtras[] = {"XPSelect", "Console_Button_Legend_BottomRight(Clone)"};  // cards, legend
constexpr const char* kSandboxExtras[] = {"Sandbox_Menu_Content", "CloudSynch_Label"};  // logo, edition art...
constexpr const char* kDarkBackdrop = "CommonBG";  // the dark fill + vignette behind TLD's new-game screens
constexpr const char* kLinebreak = "Left_Align/Linebreak";
constexpr const char* kHighlightSprite = "SelectedSprite";
constexpr const char* kMenuArt = "Left_Align/mark";  // the faint texture behind the list on TLD's sub-screens
// TLD's CONFIRM button (as on its difficulty screen), rebuilt from the Survival menu's back button for loads.
constexpr float kConfirmX = -194, kConfirmY = 44;
constexpr Vector3 kConfirmHitBox = {160, 38, 0};

// NGUI enums.
constexpr int kPivotTop = 1, kPivotCenter = 4;
constexpr int kAlignCenter = 2;
constexpr int kOverflowResizeFreely = 2, kOverflowResizeHeight = 3;

// The SOLO / CO-OP screen.
enum Choice { kChooseSolo, kChooseCoop, kChoiceCount };
constexpr const char* kChoiceNames[] = {"SOLO", "CO-OP"};
// Its layout: one centred column, in the screen's own units around the middle (TLD's UI is 720 units high).
constexpr float kHeaderY = 190, kTitleY = 150, kTopLineY = 110, kBottomLineY = -82, kHelpY = -102;
constexpr float kChoiceY[kChoiceCount] = {52, -28};
constexpr int kTitleFont = 40, kChoiceFont = 54;
constexpr int kLineWidth = 460, kHelpWidth = 540;
constexpr Vector3 kChoiceHitBox = {420, 72, 0};
constexpr int kHighlightWidth = 440, kHighlightHeight = 110;

// The CO-OP setup screen.
// The last three are the HUD's switches (milestone 12), on the setup only: the lobby uses the rows up to the code.
enum Row { kMode, kCharacter, kVoice, kName, kCode, kNames, kRadar, kChart, kRowCount };
constexpr int kLobbyRows = kCode + 1;
constexpr int kSwitchRows[] = {kNames, kRadar, kChart};
enum Mode { kHost, kJoin };
constexpr const char* kModes[] = {"host", "join"};
constexpr const char* kModeNames[] = {"HOST", "JOIN"};
constexpr const char* kVoices[] = {"male", "female"};  // VoicePersona 0 / 1
constexpr const char* kVoiceNames[] = {"MALE", "FEMALE"};

// Join codes: 6 characters without look-alikes (no 0/O, 1/I).
constexpr const char* kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr size_t kCodeLength = 6;
constexpr float kArrowGap = 18.0f;  // between a row's text and its arrows
constexpr ULONGLONG kStatusRefreshMs = 500;
constexpr ULONGLONG kLobbyRefreshMs = 250;
constexpr ULONGLONG kNoticeMs = 4000;  // a one-off line in the lobby (LET IN, "Not yet"...) stays this long
constexpr float kActionGap = 135;  // the lobby's LET IN / KICK and TURN AWAY buttons, left of CONFIRM in TLD's bar
// The character preview (avatar::preview), where TLD's difficulty screen shows its animal: right of the list.
constexpr float kPreviewX = 90, kPreviewY = -20;
constexpr int kPreviewWidth = 400, kPreviewHeight = 640, kPreviewDepth = 5;
constexpr int kRowWidth = 240;        // a TLD menu row's label (wider text shrinks)
constexpr int kLobbyRowWidth = 420;   // the lobby's: "NAME (YOU) - CHARACTER - NOT READY" at full size
// TLD's menu highlight (TLDHOME_MenuHighlight, 256 wide, centred 65 right of a row's start): its brush shows over
// ~46% of the sprite, from ~24% in. Stretched so it covers the selected row's text, as it does TLD's short ones.
constexpr int kBandWidth = 256;
constexpr float kBandShown = 0.46f, kBandLead = 0.238f, kBandStart = -2, kBandMargin = 12;

struct Api {
    Il2CppClass* game_object_class = nullptr;
    const MethodInfo* go_ctor = nullptr;          // GameObject(string)
    const MethodInfo* component_go = nullptr;     // Component.get_gameObject()
    const MethodInfo* go_transform = nullptr;     // GameObject.get_transform()
    const MethodInfo* go_set_active = nullptr;    // GameObject.SetActive(bool)
    const MethodInfo* go_component = nullptr;     // GameObject.GetComponent(Type)
    const MethodInfo* go_components = nullptr;    // GameObject.GetComponentsInChildren(Type, bool)
    const MethodInfo* instantiate = nullptr;      // static Object.Instantiate(Object, Transform)
    const MethodInfo* destroy = nullptr;          // static Object.Destroy(Object)
    const MethodInfo* destroy_now = nullptr;      // static Object.DestroyImmediate(Object)
    const MethodInfo* alive = nullptr;            // static Object.op_Implicit(Object)
    const MethodInfo* set_name = nullptr;         // Object.set_name(string)
    const MethodInfo* find = nullptr;             // Transform.Find(string)
    const MethodInfo* get_parent = nullptr;       // Transform.get_parent()
    const MethodInfo* set_parent = nullptr;       // Transform.SetParent(Transform, bool)
    const MethodInfo* get_local_position = nullptr;
    const MethodInfo* set_local_position = nullptr;
    const MethodInfo* set_local_euler = nullptr;
    const MethodInfo* set_local_scale = nullptr;
    const MethodInfo* label_set_text = nullptr;   // UILabel.set_text(string)
    const MethodInfo* label_printed = nullptr;    // UILabel.get_printedSize()
    const MethodInfo* label_set_font_size = nullptr;
    const MethodInfo* label_set_alignment = nullptr;  // NGUIText.Alignment
    const MethodInfo* label_set_overflow = nullptr;   // UILabel.Overflow
    const MethodInfo* widget_set_pivot = nullptr;     // UIWidget.Pivot; NGUI keeps the widget where it was
    const MethodInfo* widget_get_color = nullptr;
    const MethodInfo* widget_set_color = nullptr;
    const MethodInfo* box_set_size = nullptr;     // BoxCollider.set_size(Vector3)
    const MethodInfo* box_set_center = nullptr;
    const MethodInfo* widget_pivot = nullptr;     // UIWidget.get_pivotOffset()
    const MethodInfo* widget_set_width = nullptr;
    const MethodInfo* widget_set_height = nullptr;
    const MethodInfo* widget_resize_collider = nullptr;
    const MethodInfo* sprite_set_name = nullptr;  // UISprite.set_spriteName(string)
    const MethodInfo* sprite_valid = nullptr;     // UISprite.get_isValid()
    const MethodInfo* panel_set_depth = nullptr;  // UIPanel.set_depth(int)
    const MethodInfo* panel_set_alpha = nullptr;  // UIPanel.set_alpha(float): a screen copied mid-fade stays faded
    const MethodInfo* go_active = nullptr;        // GameObject.get_activeInHierarchy()
    const MethodInfo* grid_reposition = nullptr;  // UIGrid.Reposition()
    const MethodInfo* hovered = nullptr;          // static UICamera.get_hoveredObject()
    // TLD's menu sounds (static GameAudioManager.PlayGUI...): our screens sound like TLD's own
    const MethodInfo* sound_scroll = nullptr;     // to another row
    const MethodInfo* sound_change = nullptr;     // a row's value (its arrows)
    const MethodInfo* sound_click = nullptr;
    const MethodInfo* sound_confirm = nullptr;
    const MethodInfo* sound_back = nullptr;
    const MethodInfo* sound_error = nullptr;
    Il2CppObject* component_type = nullptr;
    Il2CppObject* label_type = nullptr;
    Il2CppObject* sprite_type = nullptr;
    Il2CppObject* panel_type = nullptr;
    Il2CppObject* grid_type = nullptr;
    Il2CppObject* box_type = nullptr;
    Il2CppObject* texture_type = nullptr;            // typeof(UITexture): the character preview
    const MethodInfo* texture_set_main = nullptr;    // UITexture.set_mainTexture(Texture)
    const MethodInfo* widget_set_depth = nullptr;    // UIWidget.set_depth(int)
    const MethodInfo* add_component = nullptr;       // GameObject.AddComponent(Type)
    const MethodInfo* go_get_layer = nullptr;        // GameObject.get_layer()
    const MethodInfo* go_set_layer = nullptr;        // GameObject.set_layer(int)
    ptrdiff_t grid_hide_inactive = -1;  // UIGrid.hideInactive
} api;

using PanelEnableFn = void (*)(Il2CppObject* self, bool enable, const MethodInfo* method);
using ClickFn = void (*)(Il2CppObject* self, const MethodInfo* method);
PanelEnableFn g_sandbox_enable = nullptr;
PanelEnableFn g_experience_enable = nullptr;
PanelEnableFn g_survivor_enable = nullptr;
ClickFn g_sandbox_new = nullptr;
ClickFn g_sandbox_resume = nullptr;
ClickFn g_sandbox_load = nullptr;
ClickFn g_experience_back = nullptr;
ClickFn g_saves_back = nullptr;
ClickFn g_button_click = nullptr;
const MethodInfo* g_sandbox_new_info = nullptr;      // Panel_Sandbox.OnClickNew / OnClickResume / OnClickLoad,
const MethodInfo* g_sandbox_resume_info = nullptr;   //   to carry on with TLD's own once the player has chosen
const MethodInfo* g_sandbox_load_info = nullptr;
const MethodInfo* g_saves_back_info = nullptr;       // Panel_ChooseSandbox.OnClickBack (the save list)
const MethodInfo* g_experience_back_info = nullptr;  // Panel_SelectExperience.OnClickBack
const MethodInfo* g_select_survivor = nullptr;       // Panel_SelectSurvivor.OnSelectSurvivor(VoicePersona)
const MethodInfo* g_survivor_back = nullptr;         // Panel_SelectSurvivor.OnClickBack

// What the Survival menu was asked to do: NEW, RESUME or LOAD. SOLO / CO-OP is asked first for each.
enum class Purpose { NewGame, Resume, Load };
constexpr const char* kPurposeTitles[] = {"NEW GAME", "RESUME", "LOAD GAME"};
Purpose g_purpose = Purpose::NewGame;

// The co-op game being set up. After CO-OP (the Steam lobby is open) the setup screen comes first:
//   NEW    - in place of TLD's difficulty screen; then difficulty, map..., and Male / Female answered by VOICE.
//   RESUME - over the Survival menu; then TLD's RESUME.
//   LOAD   - over the Survival menu; then TLD's save list.
// Back at the Survival menu, all of it ends.
bool g_coop_flow = false;
bool g_setup_done = false;
bool g_started = false;           // the lobby let us go on (START, or READY with the host started): TLD's screens next
int32_t g_voice = 0;              // VoicePersona chosen on the setup screen
bool g_survivor_skipped = false;  // Male / Female was skipped; seeing it again means the player came back to it
enum class SurvivorStep { Skip, Back };
uintptr_t g_pending_survivor = 0;  // Male / Female to skip (or step back over) on the next frame
SurvivorStep g_pending_step = SurvivorStep::Skip;
uintptr_t g_sandbox = 0;      // the Survival menu, to bring our screens back over it
int g_reopen_chooser_in = 0;  // frames until SOLO / CO-OP comes back (after leaving the new-game setup)
int g_reopen_setup_in = 0;    // frames until the load setup comes back (after BACK on the save list)
bool g_back_to_setup = false; // BACK on the save list in a co-op load: the Survival menu reopens the setup
int g_last_choice = kChooseSolo;

// Some TLD buttons carry several UIButton components and one click fires each of them; act once.
uintptr_t g_last_click_go = 0;
ULONGLONG g_last_click = 0;
std::array<bool, 256> g_keys_down{};

struct Arrow {
    uintptr_t go = 0;
    int row = 0;
    int step = 0;
};

// The SOLO / CO-OP screen, shown in place of the Survival menu.
struct Chooser {
    bool open = false;
    uintptr_t sandbox_panel = 0;  // Panel_Sandbox, to continue TLD's new game after the choice
    uintptr_t sandbox_go = 0;     // its GameObject, hidden while ours shows
    uintptr_t loader = 0;
    uintptr_t root = 0;
    uintptr_t rows[kChoiceCount] = {};
    uintptr_t labels[kChoiceCount] = {};
    uintptr_t description = 0;
    uintptr_t back = 0;
    uintptr_t highlight = 0;
    int selected = kChooseSolo;
    Purpose purpose = Purpose::NewGame;
    Color selected_color{1, 1, 1, 1};  // TLD's own colours for the highlighted and the other menu items
    Color normal_color{1, 1, 1, 1};
} ch;

// One player line of the lobby.
struct LobbyLine {
    uint8_t id = 0;
    uint32_t link = 0;  // hosting: their connection (KICK), or their ticket while they ask (LET IN / TURN AWAY)
    std::string name, body;
    bool ready = false, you = false, host = false, asking = false;
};

// The CO-OP setup screen: in place of TLD's difficulty screen (NEW) or over the Survival menu (RESUME / LOAD).
// After CONFIRM the same screen is the LOBBY.
struct Menu {
    bool open = false;
    uintptr_t screen_panel = 0;  // Panel_SelectExperience or Panel_Sandbox, what TLD shows when we're done
    uintptr_t screen_go = 0;     // its GameObject, hidden while our screen shows
    bool with_voice = true;      // a save already has its voice: no VOICE row for RESUME / LOAD
    uintptr_t loader = 0;  // inactive parent: the clone is stripped there before it ever wakes up
    uintptr_t root = 0;    // our screen
    uintptr_t rows[kRowCount] = {};
    uintptr_t row_labels[kRowCount] = {};
    std::vector<Arrow> arrows;
    uintptr_t description = 0;
    uintptr_t confirm = 0;
    uintptr_t back = 0;
    uintptr_t highlight = 0;
    Color selected_color{1, 1, 1, 1};  // TLD's own colours for the selected and the other rows
    Color normal_color{1, 1, 1, 1};
    uintptr_t preview = 0;     // the UITexture showing the character
    std::string preview_body;  // whose body it shows
    int selected = 0;
    config::Settings settings;
    int mode = kHost;
    int voice = 0;  // VoicePersona: 0 male, 1 female
    bool hud[3] = {true, true, true};  // NAMES, RADAR, AUTO MAP (kSwitchRows' order)
    std::string join_code;  // being typed
    std::string notice;     // one-off message shown instead of the row help ("Copied!")
    std::vector<std::string> bodies;
    size_t body = 0;
    std::string shown_status;  // the lobby line last put on screen
    ULONGLONG last_status_check = 0;
    const char* menu_path = kSetupMenu;  // where the screen's parts are (kSetupMenu or kSandboxMenu)
    // The LOBBY (after CONFIRM): everyone in the game, READY, START; hosting, LET IN / TURN AWAY / KICK.
    bool lobby = false;
    bool ready = false;            // a friend's READY
    uintptr_t confirm_label = 0;   // CONFIRM's label: START / READY in the lobby
    uintptr_t actions[2] = {};     // TLD's button, twice: LET IN and TURN AWAY, or KICK alone, for the selected friend
    uintptr_t action_roots[2] = {};
    uintptr_t action_labels[2] = {};
    std::vector<LobbyLine> lines;  // what the player rows show, top to bottom
    std::string row_text[kRowCount];
    std::string shown_lobby;       // what the lobby last showed, to redraw only on a change
    std::string timed_notice;      // the notice whose clock runs (kNoticeMs)
    ULONGLONG notice_at = 0;
} g;

// --- small wrappers ------------------------------------------------------------------------------------------------

Il2CppObject* target(uintptr_t h) { return h ? il2::il2cpp_gchandle_get_target(h) : nullptr; }
uintptr_t hold(Il2CppObject* o) { return o ? il2::il2cpp_gchandle_new(o, false) : 0; }
void drop(uintptr_t& h) {
    if (h) il2::il2cpp_gchandle_free(h);
    h = 0;
}

Il2CppObject* transform_of(Il2CppObject* go) { return go ? il2::invoke(api.go_transform, go) : nullptr; }
Il2CppObject* go_of(Il2CppObject* component) { return component ? il2::invoke(api.component_go, component) : nullptr; }

Il2CppObject* find(Il2CppObject* root_go, const std::string& path) {
    Il2CppObject* tf = transform_of(root_go);
    if (!tf) return nullptr;
    void* args[] = {il2::il2cpp_string_new(path.c_str())};
    Il2CppObject* found = il2::invoke(api.find, tf, args);
    if (!found) logx::warn("menu: '%s' not found in TLD's screen", path.c_str());
    return found ? go_of(found) : nullptr;
}

// A part of a cloned menu screen, by its path under the menu root.
Il2CppObject* part(Il2CppObject* root_go, const char* menu, const char* path) {
    return find(root_go, std::string(menu) + "/" + path);
}

Il2CppObject* component(Il2CppObject* go, Il2CppObject* type) {
    if (!go) return nullptr;
    void* args[] = {type};
    return il2::invoke(api.go_component, go, args);
}

Il2CppObject* label_of(Il2CppObject* go) { return component(go, api.label_type); }

void set_active(Il2CppObject* go, bool on) {
    if (!go) return;
    void* args[] = {&on};
    il2::invoke(api.go_set_active, go, args);
}

void set_text(Il2CppObject* label, const std::string& text) {
    if (!label) return;
    void* args[] = {il2::il2cpp_string_new(text.c_str())};
    il2::invoke(api.label_set_text, label, args);
}

void set_vec(const MethodInfo* setter, Il2CppObject* transform, Vector3 v) {
    if (!transform) return;
    void* args[] = {&v};
    il2::invoke(setter, transform, args);
}

void set_int(const MethodInfo* setter, Il2CppObject* obj, int v) {
    if (!obj) return;
    void* args[] = {&v};
    il2::invoke(setter, obj, args);
}

void destroy(Il2CppObject* obj, bool now) {
    if (!obj) return;
    void* args[] = {obj};
    il2::invoke(now ? api.destroy_now : api.destroy, nullptr, args);
}

bool alive(uintptr_t h) {
    bool yes = false;
    void* args[] = {target(h)};
    return il2::invoke_value(api.alive, nullptr, yes, args) && yes;
}

template <typename T>
void write_field(Il2CppObject* obj, ptrdiff_t offset, T value) {
    if (obj && offset >= 0) memcpy(reinterpret_cast<char*>(obj) + offset, &value, sizeof(T));
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void play(const MethodInfo* sound) {
    if (sound) il2::invoke(sound, nullptr);
}

// --- clipboard -----------------------------------------------------------------------------------------------------

bool clipboard_set(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const std::wstring wide(text.begin(), text.end());  // codes are ASCII
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        memcpy(GlobalLock(mem), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::string clipboard_get() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE mem = GetClipboardData(CF_UNICODETEXT)) {
        if (auto text = static_cast<const wchar_t*>(GlobalLock(mem))) {
            for (; *text; ++text)
                if (*text < 128) out += static_cast<char>(*text);
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return out;
}

// --- keyboard ------------------------------------------------------------------------------------------------------

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Keys pressed since the last frame (edges), only while TLD has focus.
std::array<bool, 256> poll_keys() {
    std::array<bool, 256> edges{};
    const bool focused = game_focused();
    for (int vk = 1; vk < 256; ++vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        edges[vk] = down && !g_keys_down[vk] && focused;
        g_keys_down[vk] = down;
    }
    return edges;
}

// A screen opening mid-keypress (ENTER on TLD's previous screen) mustn't take that key as its own.
void sync_keys() {
    for (int vk = 1; vk < 256; ++vk) g_keys_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// --- content -------------------------------------------------------------------------------------------------------

bool code_char(char c) { return std::strchr(kCodeAlphabet, c) != nullptr; }

std::string clean_code(const std::string& raw) {
    std::string code;
    for (char c : upper(raw))
        if (code_char(c) && code.size() < kCodeLength) code += c;
    return code;
}

std::string new_code() {
    std::random_device rd;
    std::string code;
    for (size_t i = 0; i < kCodeLength; ++i) code += kCodeAlphabet[rd() % 32];
    return code;
}

std::string display_name(const std::string& id) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"capsule", "Capsule (no body)"},   {"mackenzie", "Mackenzie"},        {"mackenzie_indoor", "Mackenzie (indoors)"},
        {"astrid", "Astrid"},               {"astrid_outdoor", "Astrid (outdoors)"}, {"molly", "Molly"},
        {"jane", "Jane"},                   {"jane_outdoor", "Jane (outdoors)"}, {"jeremiah", "Jeremiah"},
        {"methuselah", "Methuselah"},       {"heller", "Heller"},              {"jace", "Jace"},
        {"mathis", "Mathis"},               {"terry", "Terry"},                {"atwood", "Atwood"},
        {"convict_c", "Convict (C)"},       {"convict_e", "Convict (E)"},      {"convict_f", "Convict (F)"},
        {"convict_g", "Convict (G)"},       {"millworker", "Mill worker"},     {"patron1", "Bar patron 1"},
        {"patron3", "Bar patron 2"},        {"patron4", "Bar patron 3"},       {"patron5", "Bar patron 4"},
    };
    for (const auto& [key, name] : kNames)
        if (id == key) return name;
    return id;
}

std::string steam_name() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return {};
    auto friends_fn = reinterpret_cast<void* (*)()>(GetProcAddress(steam, "SteamAPI_SteamFriends_v017"));
    auto persona_fn = reinterpret_cast<const char* (*)(void*)>(GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName"));
    if (!friends_fn || !persona_fn) return {};
    void* friends = friends_fn();
    const char* name = friends ? persona_fn(friends) : nullptr;
    return name ? name : "";
}

// coop.ini plus what the menus fill in: the Steam name, and a host code that then stays the same.
config::Settings settings_with_defaults() {
    config::Settings s = config::load();
    if (s.name.empty() || s.name == "Player") {
        const std::string steam = steam_name();
        if (!steam.empty()) s.name = steam;
    }
    s.code = clean_code(s.code);
    if (s.code.size() != kCodeLength) s.code = new_code();
    return s;
}

// --- shared screen building ----------------------------------------------------------------------------------------

bool keep_on_clone(const std::string& cls) {
    static const char* kKeep[] = {"UnityEngine.Transform", "UIPanel", "UISprite", "UILabel", "UITexture", "UIWidget",
                                  "UIButton", "UIButtonScale", "UIButtonOffset", "UIButtonColor", "UIAnchor",
                                  "UIStretch", "UIGrid", "UITable", "UnityEngine.BoxCollider", "UnityEngine.Rigidbody",
                                  "TweenColor", "TweenAlpha"};
    for (const char* k : kKeep)
        if (cls == k) return true;
    return false;
}

// Remove TLD's menu logic (Panel_*, BasicMenu, localisation, spawners...) while the clone is asleep under the
// inactive loader, leaving only what draws and what is clickable.
void strip(Il2CppObject* clone) {
    int removed = 0;
    for (int pass = 0; pass < 3; ++pass) {
        bool include_inactive = true;
        void* args[] = {api.component_type, &include_inactive};
        Il2CppObject* comps = il2::invoke(api.go_components, clone, args);
        for (size_t i = 0, n = il2::array_length(comps); i < n; ++i) {
            Il2CppObject* c = il2::array_object_at(comps, i);
            const std::string cls = il2::class_name_of(c);
            if (keep_on_clone(cls)) continue;
            if (pass == 0 && cls.rfind("UnityEngine.", 0) == 0) continue;  // game scripts first
            destroy(c, true);
            ++removed;
        }
    }
    logx::info("menu: removed %d game-logic components from the cloned screen", removed);
}

// A copy of one of TLD's screens without its logic, built under an inactive parent so none of the copied scripts
// wake up before they're stripped. `loader` receives that parent.
Il2CppObject* clone_screen(Il2CppObject* panel_go, const char* name, Il2CppObject*& loader) {
    loader = il2::il2cpp_object_new(api.game_object_class);
    void* ctor_args[] = {il2::il2cpp_string_new("TLDCoop_MenuLoader")};
    il2::invoke(api.go_ctor, loader, ctor_args);
    set_active(loader, false);
    void* inst_args[] = {panel_go, transform_of(loader)};
    Il2CppObject* clone = il2::invoke(api.instantiate, nullptr, inst_args);
    if (!clone) {
        destroy(loader, false);
        loader = nullptr;
        return nullptr;
    }
    strip(clone);
    void* name_args[] = {il2::il2cpp_string_new(name)};
    il2::invoke(api.set_name, clone, name_args);
    return clone;
}

// Moves a finished copy beside TLD's screen and shows it in that screen's place.
void show_clone(Il2CppObject* clone, Il2CppObject* panel_go, Il2CppObject* grid) {
    Il2CppObject* panel = component(clone, api.panel_type);
    set_int(api.panel_set_depth, panel, kPanelDepth);
    float alpha = 1;
    void* alpha_args[] = {&alpha};
    if (panel) il2::invoke(api.panel_set_alpha, panel, alpha_args);
    bool keep_world = false;
    void* parent_args[] = {il2::invoke(api.get_parent, transform_of(panel_go)), &keep_world};
    il2::invoke(api.set_parent, transform_of(clone), parent_args);
    Il2CppObject* grid_component = component(grid, api.grid_type);
    write_field(grid_component, api.grid_hide_inactive, true);  // hidden rows leave no gap
    il2::invoke(api.grid_reposition, grid_component);
    set_active(panel_go, false);
}

// The grid's rows MenuItem0..: the first `count` stay visible, the rest are hidden.
void take_rows(Il2CppObject* clone, const char* menu, int count, uintptr_t* rows, uintptr_t* labels) {
    for (int i = 0; i < 8; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "%s/MenuItem%d", kGrid, i);
        Il2CppObject* item = part(clone, menu, path);
        if (!item) break;
        set_active(item, i < count);
        if (i >= count) continue;
        rows[i] = hold(item);
        // TLD's spare items (its templates) keep their divider line (through the text) and second label on; TLD
        // turns them off on the items it uses.
        for (const char* extra : {"Linebreak", "SecondaryLabel"}) set_active(find(item, extra), false);
        if (labels) {
            snprintf(path, sizeof(path), "%s/MenuItem%d/Label", kGrid, i);
            labels[i] = hold(label_of(part(clone, menu, path)));
        }
    }
}

// Slides TLD's menu highlight onto a row (it sits beside the grid, like in TLD's menus).
void move_highlight(uintptr_t highlight, uintptr_t row) {
    Il2CppObject* row_tf = transform_of(target(row));
    Il2CppObject* grid_tf = row_tf ? il2::invoke(api.get_parent, row_tf) : nullptr;
    Il2CppObject* highlight_tf = transform_of(target(highlight));
    if (!row_tf || !grid_tf || !highlight_tf) return;
    Vector3 r{}, grid{};
    il2::invoke_value(api.get_local_position, row_tf, r);
    il2::invoke_value(api.get_local_position, grid_tf, grid);
    set_vec(api.set_local_position, highlight_tf, {grid.x, grid.y + r.y, 0});
}

// The highlight's band as wide as the selected row's text (never narrower than TLD's).
void fit_highlight(uintptr_t highlight, uintptr_t label) {
    Il2CppObject* band = component(find(target(highlight), kHighlightSprite), api.sprite_type);
    Vector2 printed{};
    if (!band || !il2::invoke_value(api.label_printed, target(label), printed)) return;
    const float width = std::max(static_cast<float>(kBandWidth), (printed.x + kBandMargin - kBandStart) / kBandShown);
    set_int(api.widget_set_width, band, static_cast<int>(width));
    Vector3 pos{};
    il2::invoke_value(api.get_local_position, transform_of(go_of(band)), pos);
    set_vec(api.set_local_position, transform_of(go_of(band)), {kBandStart + (0.5f - kBandLead) * width, pos.y, pos.z});
}

// --- the SOLO / CO-OP screen ---------------------------------------------------------------------------------------
// The Survival menu's clone, re-arranged into one centred column on the dark backdrop of TLD's new-game screens:
// TLD's small header over its big title, its divider lines, the two choices in large type with TLD's menu highlight
// behind the selected one, and the help text below. CLOSE stays bottom right, where TLD keeps it.

std::string choice_help(int choice) {
    if (choice == kChooseSolo) return "Play alone, the normal way. Co-op stays off and nothing goes online.";
    return "Play with up to three friends through Steam. The next screen gives you a code to send them, and you "
           "choose who gets in.\n\nJoining a friend? Pick CO-OP, then JOIN on the next screen.";
}

void set_color(Il2CppObject* widget, Color c) {
    if (!widget) return;
    void* args[] = {&c};
    il2::invoke(api.widget_set_color, widget, args);
}

float brightness(const Color& c) { return c.r + c.g + c.b; }

void refresh_chooser() {
    for (int i = 0; i < kChoiceCount; ++i) {
        Il2CppObject* label = target(ch.labels[i]);
        set_text(label, kChoiceNames[i]);
        set_color(label, i == ch.selected ? ch.selected_color : ch.normal_color);
    }
    set_text(target(ch.description), choice_help(ch.selected));
    set_vec(api.set_local_position, transform_of(target(ch.highlight)), {0, kChoiceY[ch.selected], 0});
}

void choose(int choice) {
    if (choice == ch.selected) return;
    play(api.sound_scroll);
    ch.selected = choice;
    refresh_chooser();
}

void release_chooser() {
    drop(ch.sandbox_panel);
    drop(ch.sandbox_go);
    drop(ch.loader);
    drop(ch.root);
    for (auto& h : ch.rows) drop(h);
    for (auto& h : ch.labels) drop(h);
    drop(ch.description);
    drop(ch.back);
    drop(ch.highlight);
    ch.open = false;
}

void reparent(Il2CppObject* go, Il2CppObject* parent_go) {
    if (!go || !parent_go) return;
    bool keep_world = false;
    void* args[] = {transform_of(parent_go), &keep_world};
    il2::invoke(api.set_parent, transform_of(go), args);
}

// Moves a widget into the centre column at height y. NGUI keeps a widget where it is when its pivot changes, so
// the pivot is set before the position.
void place(Il2CppObject* widget, Il2CppObject* column, int pivot, float y) {
    Il2CppObject* go = go_of(widget);
    if (!go) return;
    reparent(go, column);
    set_int(api.widget_set_pivot, widget, pivot);
    set_vec(api.set_local_position, transform_of(go), {0, y, 0});
}

void style_label(Il2CppObject* label, int font_size, int overflow, int width) {
    if (!label) return;
    set_int(api.label_set_overflow, label, overflow);
    set_int(api.label_set_alignment, label, kAlignCenter);
    if (font_size > 0) set_int(api.label_set_font_size, label, font_size);
    if (width > 0) set_int(api.widget_set_width, label, width);
}

// If the font ignores the size asked for (some bitmap fonts do), scale the label up to it instead.
void ensure_font_size(Il2CppObject* label, int font_size) {
    Vector2 printed{};
    if (!label || !il2::invoke_value(api.label_printed, label, printed) || printed.y <= 0) return;
    if (printed.y >= font_size * 0.7f) return;
    const float scale = font_size / printed.y;
    set_vec(api.set_local_scale, transform_of(go_of(label)), {scale, scale, 1});
    logx::info("menu: the font drew %.0f units high for size %d; scaled the label x%.2f", printed.y, font_size, scale);
}

void layout_chooser(Il2CppObject* clone) {
    const char* m = kSandboxMenu;
    set_active(part(clone, m, kDarkBackdrop), true);
    for (const char* extra : kSandboxExtras) set_active(find(clone, extra), false);

    // Header and title. Their corner group (and its shade) stays hidden.
    Il2CppObject* header = label_of(part(clone, m, kTitleSmall));
    Il2CppObject* title = label_of(part(clone, m, kTitleBig));
    set_text(header, kPurposeTitles[static_cast<int>(ch.purpose)]);
    set_text(title, "SOLO OR CO-OP?");
    style_label(header, 0, kOverflowResizeFreely, 0);
    style_label(title, kTitleFont, kOverflowResizeFreely, 0);
    place(header, clone, kPivotCenter, kHeaderY);
    place(title, clone, kPivotCenter, kTitleY);

    // Divider lines above and below the choices.
    Il2CppObject* line = component(part(clone, m, kLinebreak), api.sprite_type);
    set_int(api.widget_set_width, line, kLineWidth);
    place(line, clone, kPivotCenter, kTopLineY);
    if (Il2CppObject* line_go = go_of(line)) {
        void* args[] = {line_go, transform_of(clone)};
        set_vec(api.set_local_position, transform_of(il2::invoke(api.instantiate, nullptr, args)), {0, kBottomLineY, 0});
    }

    // The choices, in large type and clickable across the column.
    take_rows(clone, m, kChoiceCount, ch.rows, ch.labels);
    Color colors[kChoiceCount] = {};
    for (int i = 0; i < kChoiceCount; ++i) {
        Il2CppObject* row = target(ch.rows[i]);
        Il2CppObject* label = target(ch.labels[i]);
        if (!label || !il2::invoke_value(api.widget_get_color, label, colors[i]) || colors[i].a <= 0)
            colors[i] = {1, 1, 1, 1};
        reparent(row, clone);
        set_vec(api.set_local_position, transform_of(row), {0, kChoiceY[i], 0});
        style_label(label, kChoiceFont, kOverflowResizeFreely, 0);
        set_int(api.widget_set_pivot, label, kPivotCenter);
        set_vec(api.set_local_position, transform_of(go_of(label)), {0, 0, 0});
        if (Il2CppObject* box = component(row, api.box_type)) {
            Vector3 size = kChoiceHitBox, center{0, 0, 0};
            void* size_args[] = {&size};
            void* center_args[] = {&center};
            il2::invoke(api.box_set_size, box, size_args);
            il2::invoke(api.box_set_center, box, center_args);
        }
    }
    // TLD had NEW highlighted when it was clicked: the brighter colour is its "selected" one.
    const bool first_brighter = brightness(colors[0]) >= brightness(colors[1]);
    ch.selected_color = first_brighter ? colors[0] : colors[1];
    ch.normal_color = first_brighter ? colors[1] : colors[0];

    // TLD's menu highlight, stretched to the larger rows.
    Il2CppObject* highlight = part(clone, m, kHighlight);
    reparent(highlight, clone);
    Il2CppObject* band = highlight ? component(find(highlight, kHighlightSprite), api.sprite_type) : nullptr;
    set_int(api.widget_set_width, band, kHighlightWidth);
    set_int(api.widget_set_height, band, kHighlightHeight);
    set_int(api.widget_set_pivot, band, kPivotCenter);
    set_vec(api.set_local_position, transform_of(go_of(band)), {0, 0, 0});
    ch.highlight = hold(highlight);

    // Help text, centred below the choices; it grows downwards.
    Il2CppObject* help = label_of(part(clone, m, kDescription));
    style_label(help, 0, kOverflowResizeHeight, kHelpWidth);
    place(help, clone, kPivotTop, kHelpY);
    ch.description = hold(help);

    set_active(part(clone, m, kBackHome), false);  // CLOSE instead of BACK TO HOME, like TLD's sub-screens
    set_active(part(clone, m, kBackGroup), true);
    ch.back = hold(part(clone, m, kBackButton));
}

bool open_chooser(Il2CppObject* sandbox, Purpose purpose) {
    Il2CppObject* panel_go = go_of(sandbox);
    if (!panel_go) return false;
    ch.purpose = purpose;
    Il2CppObject* loader = nullptr;
    Il2CppObject* clone = clone_screen(panel_go, "Panel_TLDCoopChoice", loader);
    if (!clone) return false;
    ch.sandbox_panel = hold(sandbox);
    ch.sandbox_go = hold(panel_go);
    ch.loader = hold(loader);
    ch.root = hold(clone);
    layout_chooser(clone);

    show_clone(clone, panel_go, part(clone, kSandboxMenu, kGrid));
    ensure_font_size(label_of(find(clone, "LabelTitle")), kTitleFont);
    for (uintptr_t label : ch.labels) ensure_font_size(target(label), kChoiceFont);
    ch.open = true;
    ch.selected = g_last_choice;
    sync_keys();
    refresh_chooser();
    Vector2 printed{};
    il2::invoke_value(api.label_printed, target(ch.labels[0]), printed);
    logx::info("menu: SOLO / CO-OP screen opened (choices drawn %.0fx%.0f units)", printed.x, printed.y);
    return true;
}

// CO-OP: open the Steam lobby now, with the remembered host code.
void start_hosting() {
    config::Settings s = settings_with_defaults();
    config::save(s);  // keeps a new code and the Steam name; the saved role stays the setup screen's
    if (s.mode == "join") {  // they last joined a friend: CONFIRM connects them (or hosts, if they switch to HOST)
        logx::info("menu: CO-OP picked; last time we joined, so nothing opens before CONFIRM");
        return;
    }
    s.mode = "host";
    if (s.transport != "local") s.transport = "steam";  // local (LAN, testing): hosting by IP
    core::apply_settings(s);
    logx::info("menu: CO-OP picked; opening the %s lobby for code %s", s.transport.c_str(), s.code.c_str());
}

void end_coop_flow(const char* why) {
    if (!g_coop_flow) return;
    g_coop_flow = false;
    g_setup_done = false;
    g_started = false;
    g_survivor_skipped = false;
    core::stop_coop(why);
}

// TLD's own NEW / RESUME / LOAD, as if our screen had never been there (the trampolines skip our hooks).
void continue_with_tld(Il2CppObject* sandbox, Purpose purpose) {
    if (!sandbox) return;
    if (purpose == Purpose::NewGame) g_sandbox_new(sandbox, g_sandbox_new_info);
    if (purpose == Purpose::Resume) g_sandbox_resume(sandbox, g_sandbox_resume_info);
    if (purpose == Purpose::Load) g_sandbox_load(sandbox, g_sandbox_load_info);
}

bool open_menu(Il2CppObject* screen_panel);

// choice < 0: CLOSE, back to the Survival menu.
void close_chooser(int choice) {
    if (!ch.open) return;
    Il2CppObject* sandbox = target(ch.sandbox_panel);
    const Purpose purpose = ch.purpose;
    play(choice < 0 ? api.sound_back : api.sound_click);
    destroy(target(ch.root), false);
    destroy(target(ch.loader), false);
    set_active(target(ch.sandbox_go), true);
    release_chooser();
    if (choice < 0) {
        end_coop_flow("SOLO / CO-OP closed");
        logx::info("menu: SOLO / CO-OP closed; back to the Survival menu");
        return;
    }
    g_last_choice = choice;
    g_purpose = purpose;
    if (choice == kChooseSolo) {
        end_coop_flow("SOLO picked");
        logx::info("menu: SOLO picked; %s, offline", kPurposeTitles[static_cast<int>(purpose)]);
        continue_with_tld(sandbox, purpose);
        return;
    }
    if (!g_coop_flow) start_hosting();  // coming back from the setup screen, the lobby is still open
    g_coop_flow = true;
    g_setup_done = false;
    g_survivor_skipped = false;
    // NEW: TLD's own NEW opens its difficulty screen, which the setup screen replaces straight away.
    // RESUME / LOAD: there's no screen to replace before the game loads, so the setup goes over the Survival menu.
    if (purpose == Purpose::NewGame || !sandbox || !open_menu(sandbox)) continue_with_tld(sandbox, purpose);
}

bool chooser_click(Il2CppObject* go) {
    for (int i = 0; i < kChoiceCount; ++i)
        if (go == target(ch.rows[i])) {
            close_chooser(i);
            return true;
        }
    if (go != target(ch.back)) return false;
    close_chooser(-1);
    return true;
}

void chooser_frame() {
    if (!alive(ch.root)) {
        logx::warn("menu: the SOLO / CO-OP screen disappeared; closing");
        release_chooser();
        return;
    }
    if (Il2CppObject* hovered = il2::invoke(api.hovered, nullptr))
        for (int i = 0; i < kChoiceCount; ++i)
            if (hovered == target(ch.rows[i])) choose(i);
    const auto key = poll_keys();
    if (key[VK_UP] || key[VK_DOWN]) choose(1 - ch.selected);
    if (key[VK_RETURN]) close_chooser(ch.selected);
    else if (key[VK_ESCAPE]) close_chooser(-1);
}

// --- the CO-OP setup screen ----------------------------------------------------------------------------------------

// The lobby as it stands, for the code row.
std::string lobby_status() {
    if (g.mode == kJoin) return {};
    if (!netrt::active()) return "Couldn't open a Steam lobby. Make sure Steam is running and online.";
    const net::Status st = netrt::status();
    if (st.connected) {
        std::string names;
        for (size_t i = 0; i < st.players.size(); ++i)
            names += (i == 0 ? "" : i + 1 == st.players.size() ? " and " : ", ") + st.players[i].name;
        return names + (st.players.size() == 1 ? " is here." : " are here.");
    }
    return st.link.empty() ? "Steam lobby: opening..." : "Steam: " + st.link;
}

std::string row_help(int row) {
    switch (row) {
        case kMode:
            return "HOST: friends join your world, and you choose who gets in.\nJOIN: you join a friend's world "
                   "with their code.";
        case kCharacter:
            return "How your friends see you in the world. You still see your own arms in first person.";
        case kVoice:
            return "Your survivor's voice (and first-person hands), the Male / Female choice TLD normally asks for. "
                   "Your look is the CHARACTER above.";
        case kName:
            return "What your friends see you as: your Steam name.";
        case kNames:
            return "A small dot over each friend's head; their name and how far away they are when you look at them or "
                   "they're close. Out of sight, an arrow at the edge of the screen points the way.";
        case kRadar:
            return "A small round map of the area around you in the corner, as far as you have charted it, with your "
                   "friends on it.";
        case kChart:
            return "Your map charts itself where you walk, as charcoal mapping does. Only your own travels: your "
                   "friends chart theirs.";
        default:
            if (g.mode == kHost)
                return "Give this code to your friends (Discord, Steam chat...). It stays the same every time you "
                       "host. Click it, or press CTRL+C, to copy.\n\n" +
                       lobby_status();
            return "Type the code your friend gave you, or paste it with CTRL+V. BACKSPACE deletes.\n\nYou "
                   "connect through Steam: no IP address or port forwarding needed.";
    }
}

void place_arrows(int row) {
    Il2CppObject* label = target(g.row_labels[row]);
    if (!label) return;
    Vector2 size{}, pivot{};
    il2::invoke_value(api.label_printed, label, size);
    il2::invoke_value(api.widget_pivot, label, pivot);
    Vector3 pos{};
    il2::invoke_value(api.get_local_position, transform_of(go_of(label)), pos);
    const float left = pos.x - pivot.x * size.x;  // the text's left edge, whatever the label's anchor
    for (const Arrow& a : g.arrows) {
        if (a.row != row) continue;
        const float x = a.step < 0 ? left - kArrowGap : left + size.x + kArrowGap;
        set_vec(api.set_local_position, transform_of(target(a.go)), {x, pos.y, 0});
    }
}

void refresh_lobby(bool force);

// The UITexture the preview shows in, made on first use.
Il2CppObject* preview_widget() {
    if (Il2CppObject* widget = target(g.preview)) return widget;
    Il2CppObject* root = target(g.root);
    if (!root || !api.texture_type || !api.add_component || !api.texture_set_main) return nullptr;
    Il2CppObject* go = il2::il2cpp_object_new(api.game_object_class);
    void* name_args[] = {il2::il2cpp_string_new("CharacterPreview")};
    il2::invoke(api.go_ctor, go, name_args);
    int layer = 5;  // NGUI's
    il2::invoke_value(api.go_get_layer, root, layer);
    set_int(api.go_set_layer, go, layer);
    reparent(go, root);
    set_vec(api.set_local_position, transform_of(go), {kPreviewX, kPreviewY, 0});
    set_vec(api.set_local_scale, transform_of(go), {1, 1, 1});
    void* type_args[] = {api.texture_type};
    Il2CppObject* widget = il2::invoke(api.add_component, go, type_args);
    if (!widget) return nullptr;
    set_int(api.widget_set_width, widget, kPreviewWidth);
    set_int(api.widget_set_height, widget, kPreviewHeight);
    set_int(api.widget_set_depth, widget, kPreviewDepth);
    g.preview = hold(widget);
    return widget;
}

// The character beside the list, as TLD's difficulty screen shows its animal: the chosen body on the setup, the
// selected player's in the lobby. Hidden for a body the pack can't show.
void show_preview(const std::string& body) {
    if (body == g.preview_body && target(g.preview)) return;
    g.preview_body = body;
    Il2CppObject* texture = avatar::preview(body);
    Il2CppObject* widget = texture ? preview_widget() : target(g.preview);
    if (!widget) return;
    void* args[] = {texture};
    il2::invoke(api.texture_set_main, widget, args);
    set_active(go_of(widget), texture != nullptr);
    if (texture) logx::info("menu: the preview shows %s", body.c_str());
}

// The selected row bright, the others as TLD draws its unselected items. Its arrows too: grey ones vanish on the
// highlight's band.
void color_rows() {
    for (int i = 0; i < kRowCount; ++i)
        set_color(target(g.row_labels[i]), i == g.selected ? g.selected_color : g.normal_color);
    for (const Arrow& a : g.arrows)
        set_color(component(target(a.go), api.sprite_type), a.row == g.selected ? g.selected_color : g.normal_color);
}

void refresh() {
    if (g.lobby) {
        refresh_lobby(true);
        return;
    }
    set_text(target(g.row_labels[kMode]), std::string("MODE: ") + kModeNames[g.mode]);
    set_text(target(g.row_labels[kCharacter]), "CHARACTER: " + upper(display_name(g.bodies[g.body])));
    set_text(target(g.row_labels[kVoice]), std::string("VOICE: ") + kVoiceNames[g.voice]);
    set_text(target(g.row_labels[kName]), "NAME: " + g.settings.name);
    set_text(target(g.row_labels[kCode]),
             g.mode == kHost ? "YOUR CODE: " + g.settings.code
                             : "JOIN CODE: " + g.join_code + std::string(kCodeLength - g.join_code.size(), '_'));
    constexpr const char* kSwitchNames[] = {"NAMES: ", "RADAR: ", "AUTO MAP: "};
    for (int i = 0; i < 3; ++i)
        set_text(target(g.row_labels[kSwitchRows[i]]), std::string(kSwitchNames[i]) + (g.hud[i] ? "ON" : "OFF"));
    g.shown_status = lobby_status();
    set_text(target(g.description), g.notice.empty() ? row_help(g.selected) : g.notice);
    place_arrows(kMode);
    place_arrows(kCharacter);
    place_arrows(kVoice);
    for (int row : kSwitchRows) place_arrows(row);
    move_highlight(g.highlight, g.rows[g.selected]);
    fit_highlight(g.highlight, g.row_labels[g.selected]);
    color_rows();
    show_preview(g.bodies[g.body]);
}

void select(int row) {
    if (row == g.selected) return;
    play(api.sound_scroll);
    g.selected = row;
    g.notice.clear();
    refresh();
}

void change(int row, int step) {
    play(api.sound_change ? api.sound_change : api.sound_click);
    g.notice.clear();
    if (row == kMode) g.mode = 1 - g.mode;
    if (row == kCharacter) g.body = (g.body + g.bodies.size() + step) % g.bodies.size();
    if (row == kVoice) g.voice = (g.voice + 2 + step) % 2;
    for (int i = 0; i < 3; ++i)
        if (row == kSwitchRows[i]) g.hud[i] = !g.hud[i];
    refresh();
}

void copy_code() {
    if (g.mode != kHost) return;
    play(api.sound_click);
    g.notice = clipboard_set(g.settings.code) ? "Copied " + g.settings.code + " to the clipboard. Send it to your friend!"
                                              : std::string("Couldn't reach the clipboard; read the code out instead.");
    refresh();
}

void paste_code() {
    if (g.mode != kJoin) return;
    play(api.sound_click);
    const std::string code = clean_code(clipboard_get());
    g.notice = code.empty() ? "The clipboard doesn't hold a code." : std::string();
    if (!code.empty()) g.join_code = code;
    refresh();
}

// A left or right arrow button beside a row, made from TLD's own arrow button (the scroll arrow of this screen,
// re-skinned with the big arrow TLD's settings screens use when that sprite is available).
void add_arrow(Il2CppObject* template_go, int row, int step) {
    Il2CppObject* row_go = target(g.rows[row]);
    if (!template_go || !row_go) return;
    void* args[] = {template_go, transform_of(row_go)};
    Il2CppObject* arrow = il2::invoke(api.instantiate, nullptr, args);
    if (!arrow) return;
    Il2CppObject* sprite = component(arrow, api.sprite_type);
    void* name_args[] = {il2::il2cpp_string_new("arrow_nav3")};
    il2::invoke(api.sprite_set_name, sprite, name_args);
    bool big = false;
    il2::invoke_value(api.sprite_valid, sprite, big);
    if (big) {
        // arrow_nav3 points right.
        set_int(api.widget_set_width, sprite, 30);
        set_int(api.widget_set_height, sprite, 30);
        set_vec(api.set_local_euler, transform_of(arrow), {0, 0, step < 0 ? 180.0f : 0.0f});
    } else {
        // The scroll arrow points up.
        void* small_args[] = {il2::il2cpp_string_new("arrow")};
        il2::invoke(api.sprite_set_name, sprite, small_args);
        set_int(api.widget_set_width, sprite, 16);
        set_int(api.widget_set_height, sprite, 24);
        set_vec(api.set_local_euler, transform_of(arrow), {0, 0, step < 0 ? 90.0f : -90.0f});
    }
    il2::invoke(api.widget_resize_collider, sprite);
    set_active(arrow, true);
    g.arrows.push_back({hold(arrow), row, step});
}

void load_choices() {
    g.settings = settings_with_defaults();
    g.mode = g.settings.mode == "join" ? kJoin : kHost;
    g.join_code = clean_code(g.settings.join_code);
    g.voice = g.settings.voice == "female" ? 1 : 0;
    g.hud[0] = g.settings.hud_names;
    g.hud[1] = g.settings.hud_radar;
    g.hud[2] = g.settings.hud_chart;
    // Characters first, the capsule last.
    g.bodies.clear();
    std::string ids = avatar::body_ids();
    for (size_t pos = 0; pos <= ids.size();) {
        size_t comma = ids.find(", ", pos);
        std::string id = ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!id.empty() && id != "capsule") g.bodies.push_back(id);
        if (comma == std::string::npos) break;
        pos = comma + 2;
    }
    g.bodies.push_back("capsule");
    g.body = 0;
    for (size_t i = 0; i < g.bodies.size(); ++i)
        if (g.bodies[i] == g.settings.body) g.body = i;
    g.selected = 0;
    g.notice.clear();
}

void release() {
    drop(g.screen_panel);
    drop(g.screen_go);
    drop(g.loader);
    drop(g.root);
    for (auto& h : g.rows) drop(h);
    for (auto& h : g.row_labels) drop(h);
    for (auto& a : g.arrows) drop(a.go);
    g.arrows.clear();
    drop(g.description);
    drop(g.confirm);
    drop(g.confirm_label);
    for (int i = 0; i < 2; ++i) {
        drop(g.actions[i]);
        drop(g.action_roots[i]);
        drop(g.action_labels[i]);
    }
    drop(g.back);
    drop(g.highlight);
    drop(g.preview);
    g.preview_body.clear();
    avatar::end_preview();
    g.open = false;
    g.lobby = false;
    g.ready = false;
}

// One of TLD's bottom-bar buttons, made from the screen's back button and dressed like the CONFIRM on TLD's
// difficulty screen (its sprites and size): CONFIRM for loads (the Survival menu has none), and the lobby's LET IN /
// TURN AWAY / KICK. `root_out`: the copy's root, to show or hide it; `label_out`: its label.
Il2CppObject* make_button(Il2CppObject* clone, const char* m, const char* text, float x, float y,
                          Il2CppObject** root_out = nullptr, Il2CppObject** label_out = nullptr) {
    Il2CppObject* back_root = part(clone, m, kBackRoot);
    Il2CppObject* back_parent = back_root ? il2::invoke(api.get_parent, transform_of(back_root)) : nullptr;
    if (!back_parent) return nullptr;
    void* args[] = {back_root, back_parent};
    Il2CppObject* root = il2::invoke(api.instantiate, nullptr, args);
    if (!root) return nullptr;
    if (root_out) *root_out = root;
    set_vec(api.set_local_position, transform_of(root), {x, y, 0});
    Il2CppObject* button = find(root, "ButtonBottomRightMouse");
    Il2CppObject* label = label_of(find(button, "Mouse_Button_Label_BAck"));
    set_text(label, text);
    if (label_out) *label_out = label;
    const std::pair<const char*, const char*> kSkins[] = {{"Background", "menuButtonCenter_fill"},
                                                          {"BackGlow", "menuButtonCenter_outer"}};
    const int kSizes[][2] = {{160, 38}, {200, 83}};
    for (int i = 0; i < 2; ++i) {
        Il2CppObject* sprite = component(find(button, kSkins[i].first), api.sprite_type);
        void* name_args[] = {il2::il2cpp_string_new(kSkins[i].second)};
        if (sprite) il2::invoke(api.sprite_set_name, sprite, name_args);
        set_int(api.widget_set_width, sprite, kSizes[i][0]);
        set_int(api.widget_set_height, sprite, kSizes[i][1]);
    }
    if (Il2CppObject* box = component(button, api.box_type)) {
        Vector3 size = kConfirmHitBox;
        void* size_args[] = {&size};
        il2::invoke(api.box_set_size, box, size_args);
    }
    return button;
}

// panel: Panel_SelectExperience for NEW (the setup takes its place), Panel_Sandbox for RESUME / LOAD (the setup
// goes over the Survival menu, dressed like TLD's sub-screens).
bool open_menu(Il2CppObject* screen_panel) {
    Il2CppObject* panel_go = go_of(screen_panel);
    if (!panel_go) return false;
    Il2CppObject* loader = nullptr;
    Il2CppObject* clone = clone_screen(panel_go, "Panel_TLDCoop", loader);
    if (!clone) return false;

    load_choices();
    g.screen_panel = hold(screen_panel);
    g.screen_go = hold(panel_go);
    g.loader = hold(loader);
    g.root = hold(clone);

    const bool new_game = g_purpose == Purpose::NewGame;
    const char* m = new_game ? kSetupMenu : kSandboxMenu;
    g.menu_path = m;
    g.with_voice = new_game;
    g.lobby = false;
    g.ready = false;
    g.shown_lobby.clear();
    set_text(label_of(part(clone, m, kTitleSmall)), "SET UP");
    set_text(label_of(part(clone, m, kTitleBig)), "CO-OP");
    take_rows(clone, m, kRowCount, g.rows, g.row_labels);
    // TLD draws its selected item white and the others in its dimmer menu colour (the dimmest row has it).
    float least = 1e9f;
    for (uintptr_t label : g.row_labels) {
        Color c{};
        if (!il2::invoke_value(api.widget_get_color, target(label), c) || c.a <= 0) continue;
        if (brightness(c) < least) least = brightness(c), g.normal_color = c;
    }
    g.selected_color = {1, 1, 1, 1};
    logx::info("menu: row colours: selected white, others %.2f %.2f %.2f", g.normal_color.r, g.normal_color.g,
               g.normal_color.b);
    if (!g.with_voice) set_active(target(g.rows[kVoice]), false);
    Il2CppObject* arrow_template = part(clone, m, kArrowTemplate);
    for (int row : {kMode, kCharacter, kVoice, kNames, kRadar, kChart}) {
        add_arrow(arrow_template, row, -1);
        add_arrow(arrow_template, row, +1);
    }
    g.description = hold(label_of(part(clone, m, kDescription)));
    g.highlight = hold(part(clone, m, kHighlight));
    if (new_game) {
        set_active(part(clone, m, kConfirmRoot), true);
        set_active(part(clone, m, kBackRoot), true);
        g.confirm = hold(part(clone, m, kConfirmButton));
        for (const char* extra : kExperienceExtras) set_active(find(clone, extra), false);
    } else {
        // The Survival menu shows its logo and the 3D scene; TLD's sub-screens show this instead.
        set_active(part(clone, m, kTitle), true);
        set_active(part(clone, m, kDarkBackdrop), true);
        set_active(part(clone, m, kMenuArt), true);
        for (const char* extra : kSandboxExtras) set_active(find(clone, extra), false);
        set_active(part(clone, m, kBackHome), false);
        set_active(part(clone, m, kBackGroup), true);
        g.confirm = hold(make_button(clone, m, "CONFIRM", kConfirmX, kConfirmY));
    }
    g.back = hold(part(clone, m, kBackButton));
    if (Il2CppObject* confirm = target(g.confirm)) {
        Il2CppObject* label = new_game ? label_of(find(confirm, "Mouse_Button_Label")) : nullptr;
        g.confirm_label = hold(label ? label : label_of(find(confirm, "Mouse_Button_Label_BAck")));
    }
    for (int i = 0; i < 2; ++i) {
        Il2CppObject *root = nullptr, *label = nullptr;
        g.actions[i] = hold(make_button(clone, m, i == 0 ? "LET IN" : "TURN AWAY", kConfirmX - kActionGap * (i + 1),
                                        kConfirmY, &root, &label));
        g.action_roots[i] = hold(root);
        g.action_labels[i] = hold(label);
        set_active(root, false);  // the lobby shows them for a selected friend
    }

    show_clone(clone, panel_go, part(clone, m, kGrid));
    g.open = true;
    sync_keys();
    refresh();
    logx::info("menu: co-op setup opened for %s (mode %s, character %s, %zu arrows, confirm %s)",
               kPurposeTitles[static_cast<int>(g_purpose)], kModes[g.mode], g.bodies[g.body].c_str(),
               g.arrows.size(), g.confirm ? "yes" : "MISSING");
    return true;
}

void open_lobby();

// confirmed: CONFIRM, on to the lobby (the setup is saved and applied first). Else BACK, to SOLO / CO-OP.
void close_menu(bool confirmed) {
    if (!g.open) return;
    const bool by_code = g.settings.transport != "local";  // local (LAN, testing): the host's IP in coop.ini instead
    if (confirmed && g.mode == kJoin && by_code && g.join_code.size() != kCodeLength) {
        play(api.sound_error);
        g.selected = kCode;
        g.notice = "Enter the 6-character code from your friend first (or paste it with CTRL+V).";
        refresh();
        return;
    }
    play(confirmed ? api.sound_confirm : api.sound_back);
    if (confirmed) {
        g.settings.mode = kModes[g.mode];
        g.settings.body = g.bodies[g.body];
        if (g.with_voice) g.settings.voice = kVoices[g.voice];
        g.settings.hud_names = g.hud[0];
        g.settings.hud_radar = g.hud[1];
        g.settings.hud_chart = g.hud[2];
        if (by_code) g.settings.transport = "steam";  // codes run through Steam
        if (g.mode == kJoin) g.settings.join_code = g.join_code;
        config::save(g.settings);
        core::apply_settings(g.settings);  // hosting: keeps the lobby CO-OP opened; join: leaves it and joins
        g_voice = g.voice;  // answers TLD's Male / Female screen later (new games)
        logx::info("menu: confirmed (mode %s, character %s, voice %s, code %s); the lobby next",
                   g.settings.mode.c_str(), g.settings.body.c_str(), g.with_voice ? g.settings.voice.c_str() : "save's",
                   g.mode == kHost ? g.settings.code.c_str() : g.join_code.c_str());
        open_lobby();
        return;
    }
    Il2CppObject* screen = target(g.screen_panel);
    destroy(target(g.root), false);
    destroy(target(g.loader), false);
    set_active(target(g.screen_go), true);  // TLD's own screen, as TLD left it
    logx::info("menu: setup closed; back to SOLO / CO-OP");
    release();
    if (!screen) return;
    if (g_purpose == Purpose::NewGame) {
        // Leave the difficulty screen the way its own BACK does (to the Survival menu, which ends co-op), then
        // bring SOLO / CO-OP back over the Survival menu.
        g_experience_back(screen, g_experience_back_info);
        g_reopen_chooser_in = 2;
    } else {
        open_chooser(screen, g_purpose);  // the Survival menu is right there
    }
}

bool lobby_click(Il2CppObject* go);

bool setup_click(Il2CppObject* go) {
    if (g.lobby) return lobby_click(go);
    int row = -1;
    for (int i = 0; i < kRowCount; ++i)
        if (go == target(g.rows[i])) row = i;
    const Arrow* arrow = nullptr;
    for (const Arrow& a : g.arrows)
        if (go == target(a.go)) arrow = &a;
    const bool is_confirm = go == target(g.confirm), is_back = go == target(g.back);
    if (row < 0 && !arrow && !is_confirm && !is_back) return false;
    if (arrow) {
        const Arrow a = *arrow;
        select(a.row);
        change(a.row, a.step);
    } else if (row >= 0) {
        select(row);
        if (row == kMode || row == kCharacter || row == kVoice || row >= kNames) change(row, +1);
        if (row == kCode) copy_code();
    } else {
        close_menu(is_confirm);
    }
    return true;
}

void lobby_frame();

void setup_frame() {
    if (!alive(g.root)) {
        logx::warn("menu: the screen disappeared; closing");
        release();
        return;
    }
    if (g.lobby) {
        lobby_frame();
        return;
    }

    // Mouse hover selects a row, like TLD's own menus.
    if (Il2CppObject* hovered = il2::invoke(api.hovered, nullptr))
        for (int i = 0; i < kRowCount; ++i)
            if (hovered == target(g.rows[i])) select(i);

    // Keep the lobby line current while it's on screen.
    const ULONGLONG now = GetTickCount64();
    if (g.selected == kCode && g.notice.empty() && now - g.last_status_check >= kStatusRefreshMs) {
        g.last_status_check = now;
        if (lobby_status() != g.shown_status) refresh();
    }

    const auto key = poll_keys();
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    auto next_row = [](int row, int step) {
        do row = (row + kRowCount + step) % kRowCount;
        while (row == kVoice && !g.with_voice);
        return row;
    };
    if (key[VK_UP]) select(next_row(g.selected, -1));
    if (key[VK_DOWN]) select(next_row(g.selected, +1));
    if (key[VK_LEFT]) change(g.selected, -1);
    if (key[VK_RIGHT]) change(g.selected, +1);

    if (g.selected == kCode) {
        if (ctrl && key['C']) copy_code();
        if (ctrl && key['V']) paste_code();
        if (g.mode == kJoin && !ctrl) {
            bool typed = false;
            for (int vk = 'A'; vk <= 'Z'; ++vk)
                if (key[vk] && code_char(static_cast<char>(vk)) && g.join_code.size() < kCodeLength) {
                    g.join_code += static_cast<char>(vk);
                    typed = true;
                }
            for (int d = 0; d <= 9; ++d)
                if ((key['0' + d] || key[VK_NUMPAD0 + d]) && code_char(static_cast<char>('0' + d)) &&
                    g.join_code.size() < kCodeLength) {
                    g.join_code += static_cast<char>('0' + d);
                    typed = true;
                }
            if (key[VK_BACK] && !g.join_code.empty()) {
                g.join_code.pop_back();
                typed = true;
            }
            if (typed) {
                g.notice.clear();
                refresh();
            }
        }
    }

    if (key[VK_RETURN]) close_menu(true);
    else if (key[VK_ESCAPE]) close_menu(false);
}

// --- the LOBBY (milestone 4d) ----------------------------------------------------------------------------------------
// After CONFIRM the setup screen becomes the lobby, as in any game: everyone in it with their character and whether
// they're READY, and the code. Hosting, a friend who joins asks first: select them, then LET IN or TURN AWAY; one
// already in can be KICKED. Friends press READY; the host's START loads the world once every friend in is READY
// (alone, at once: friends can still join while the host plays). A READY friend's game goes on by itself once the
// host has started. BACK returns to the setup; the connection stays.

const net::PlayerInfo* lobby_host(const net::Status& st) {
    for (const net::PlayerInfo& p : st.players)
        if (p.id == 0) return &p;
    return nullptr;
}

std::string host_name(const net::Status& st) {
    if (const net::PlayerInfo* h = lobby_host(st)) return h->name;
    return st.peer_name.empty() ? std::string("your friend") : st.peer_name;
}

std::vector<LobbyLine> lobby_lines(const net::Status& st) {
    std::vector<LobbyLine> out;
    const bool hosting = g.mode == kHost;
    LobbyLine me;
    me.id = hosting ? uint8_t{0} : st.connected ? st.self_id : uint8_t{net::kMaxPlayers};
    me.name = g.settings.name;
    me.body = g.bodies[g.body];
    me.ready = hosting ? g_started : g.ready;
    me.you = true;
    me.host = hosting;
    out.push_back(me);
    if (!hosting && !st.connected && st.waiting) {  // who we wait for
        LobbyLine host;
        host.name = st.peer_name;
        host.host = true;
        out.push_back(host);
    }
    for (const net::PlayerInfo& p : st.players) {
        if (!st.connected) break;
        LobbyLine l;
        l.id = p.id;
        l.link = p.link;
        l.name = p.name;
        l.body = p.body;
        l.ready = p.ready;
        l.host = p.id == 0;
        out.push_back(l);
    }
    std::stable_sort(out.begin(), out.end(), [](const LobbyLine& a, const LobbyLine& b) { return a.id < b.id; });
    if (hosting)
        for (const net::PlayerInfo& p : st.asking) {
            LobbyLine l;
            l.id = net::kMaxPlayers;
            l.link = p.link;
            l.name = p.name;
            l.asking = true;
            out.push_back(l);
        }
    return out;
}

std::string line_text(const LobbyLine& l) {
    std::string s = upper(l.name) + (l.you ? " (YOU)" : "");
    if (l.asking) return s + " - WANTS TO JOIN";
    if (!l.body.empty()) s += " - " + upper(display_name(l.body));
    if (l.host) return s + (l.ready ? " - PLAYING" : " - HOST");
    return s + (l.ready ? " - READY" : " - NOT READY");
}

// Friends in the game who haven't pressed READY yet (hosting).
std::vector<std::string> not_ready(const net::Status& st) {
    std::vector<std::string> out;
    for (const net::PlayerInfo& p : st.players)
        if (!p.ready) out.push_back(p.name);
    return out;
}

std::string names(const std::vector<std::string>& list) {
    std::string s;
    for (size_t i = 0; i < list.size(); ++i) s += (i == 0 ? "" : i + 1 == list.size() ? " and " : ", ") + list[i];
    return s;
}

const LobbyLine* selected_line() {
    if (g.selected >= kCode || static_cast<size_t>(g.selected) >= g.lines.size()) return nullptr;
    return &g.lines[static_cast<size_t>(g.selected)];
}

// The selected line is a friend the host can act on.
const LobbyLine* chosen_friend() {
    const LobbyLine* l = selected_line();
    return g.mode == kHost && l && !l->you && !l->host ? l : nullptr;
}

std::string lobby_help(const net::Status& st) {
    if (g.mode == kHost) {
        std::vector<std::string> asking;
        for (const net::PlayerInfo& p : st.asking) asking.push_back(p.name);
        const std::string ask = asking.empty() ? std::string()
                                               : "\n\n" + names(asking) + (asking.size() == 1 ? " wants" : " want") +
                                                     " to join: select them to LET IN or TURN AWAY.";
        if (st.players.empty())
            return "Send your friends your code; you choose who gets in.\n\nPress START to begin. Friends can still "
                   "join while you play." + ask;
        const auto waiting = not_ready(st);
        if (!waiting.empty()) return "Waiting for " + names(waiting) + " to be READY.\n\nSTART loads the world once everyone is." + ask;
        return "Everyone is READY. Press START to load the world together." + ask;
    }
    const std::string host = host_name(st);
    if (st.turned_away) return host + " didn't let you in.\n\nPress BACK to join someone else.";
    if (st.rejected)
        return "Couldn't join: your friend's game is a different version (of The Long Dark, or of co-op).\n\nPress BACK.";
    if (st.waiting) return "Waiting for " + host + " to let you in...";
    if (!st.connected) return std::string("Joining your friend's game...") + (st.link.empty() ? "" : "\n\n" + st.link);
    const net::PlayerInfo* h = lobby_host(st);
    if (h && h->ready) return host + " is already playing. Press READY to join them.";
    if (!g.ready) return "Press READY when you are. The world loads when " + host + " starts the game.";
    return "READY. Waiting for " + host + " to start the game.";
}

std::string friend_help(const LobbyLine& l) {
    if (l.asking)
        return l.name + " wants to join your game.\n\nLET IN, or TURN AWAY? Turned away, they can't ask again until "
                        "you host again.";
    return l.name + (l.ready ? " is READY." : " isn't READY yet.") +
           "\n\nKICK sends them back to their menu; they can't come back until you host again.";
}

std::string code_line(const net::Status& st) {
    const bool local = g.settings.transport == "local";
    if (g.mode == kHost) return local ? std::string("LOCAL GAME") : "YOUR CODE: " + g.settings.code;
    if (st.connected) return "IN " + upper(host_name(st)) + "'S GAME";
    if (st.turned_away) return "NOT LET IN";
    if (st.rejected) return "COULDN'T JOIN";
    if (st.waiting) return "WAITING TO BE LET IN";
    return "JOINING " + (local ? std::string("BY IP") : g.join_code) + "...";
}

void refresh_lobby(bool force) {
    const net::Status st = netrt::status();
    g.lines = lobby_lines(st);
    const bool hosting = g.mode == kHost;
    for (int i = 0; i < kCode; ++i)
        g.row_text[i] = static_cast<size_t>(i) < g.lines.size() ? line_text(g.lines[static_cast<size_t>(i)])
                        : i == 1 && hosting && g.lines.size() == 1 ? "WAITING FOR FRIENDS..."
                                                                   : "";
    g.row_text[kCode] = code_line(st);
    if (g.row_text[g.selected].empty() || (g.selected > 0 && g.selected < kCode && static_cast<size_t>(g.selected) >= g.lines.size()))
        g.selected = 0;
    const LobbyLine* chosen = chosen_friend();
    const char* first = chosen && chosen->asking ? "LET IN" : "";
    const char* second = !chosen ? "" : chosen->asking ? "TURN AWAY" : "KICK";
    const std::string help = !g.notice.empty() ? g.notice : chosen ? friend_help(*chosen) : lobby_help(st);
    const char* button = hosting ? "START" : g.ready ? "CANCEL" : "READY";
    std::string shown;
    for (const auto& r : g.row_text) shown += r + "|";
    shown += help + "|" + button + "|" + first + "|" + second + "|" + std::to_string(g.selected);
    if (!force && shown == g.shown_lobby) return;
    g.shown_lobby = shown;
    for (int i = 0; i < kLobbyRows; ++i) set_text(target(g.row_labels[i]), g.row_text[i]);
    set_text(target(g.description), help);
    set_text(target(g.confirm_label), button);
    const char* action_text[2] = {first, second};
    int slot = 0;  // the shown ones sit next to START, one after the other
    for (int i = 0; i < 2; ++i) {
        const bool shown_action = *action_text[i] != 0;
        set_text(target(g.action_labels[i]), action_text[i]);
        set_active(target(g.action_roots[i]), shown_action);
        if (shown_action)
            set_vec(api.set_local_position, transform_of(target(g.action_roots[i])),
                    {kConfirmX - kActionGap * static_cast<float>(++slot), kConfirmY, 0});
    }
    move_highlight(g.highlight, g.rows[g.selected]);
    fit_highlight(g.highlight, g.row_labels[g.selected]);
    color_rows();
    const LobbyLine* line = selected_line();
    show_preview(line && !line->body.empty() ? line->body : g.bodies[g.body]);
}

void show_setup_parts(bool setup) {
    for (const Arrow& a : g.arrows) set_active(target(a.go), setup);
    for (uintptr_t label : g.row_labels) set_int(api.widget_set_width, target(label), setup ? kRowWidth : kLobbyRowWidth);
    for (uintptr_t root : g.action_roots) set_active(target(root), false);
    Il2CppObject* root = target(g.root);
    // The lobby uses every row up to the code (VOICE too), not the HUD's switches: the grid lays them out again.
    if (!g.with_voice) set_active(target(g.rows[kVoice]), !setup);
    for (int row : kSwitchRows) set_active(target(g.rows[row]), setup);
    il2::invoke(api.grid_reposition, component(part(root, g.menu_path, kGrid), api.grid_type));
    set_text(label_of(part(root, g.menu_path, kTitleSmall)), setup ? "SET UP" : "CO-OP");
    set_text(label_of(part(root, g.menu_path, kTitleBig)), setup ? "CO-OP" : "LOBBY");
    set_text(target(g.confirm_label), "CONFIRM");
}

void open_lobby() {
    g.lobby = true;
    g.ready = false;
    g.selected = 0;
    g.notice.clear();
    g.shown_lobby.clear();
    show_setup_parts(false);
    refresh_lobby(true);
    logx::info("menu: lobby open (%s; %s)", g.mode == kHost ? "hosting" : "joining", kPurposeTitles[static_cast<int>(g_purpose)]);
}

void leave_lobby() {
    play(api.sound_back);
    g.lobby = false;
    g.ready = false;
    g.selected = 0;
    g.notice.clear();
    show_setup_parts(true);
    refresh();
    logx::info("menu: lobby left; back to the setup");
}

void lobby_select(int row) {
    if (row == g.selected || g.row_text[row].empty()) return;
    play(api.sound_scroll);
    g.selected = row;
    g.notice.clear();
    refresh_lobby(true);
}

void lobby_notice(const std::string& text) {
    g.notice = text;
    refresh_lobby(true);
}

// Out of the lobby, on to TLD's own: NEW's difficulty screen (already there under ours), RESUME, or LOAD's list.
void go_on(const std::string& why) {
    Il2CppObject* screen = target(g.screen_panel);
    destroy(target(g.root), false);
    destroy(target(g.loader), false);
    set_active(target(g.screen_go), true);  // TLD's own screen, as TLD left it
    g_setup_done = true;
    g_started = true;
    g_survivor_skipped = false;
    logx::info("menu: %s; on to TLD's %s", why.c_str(), kPurposeTitles[static_cast<int>(g_purpose)]);
    release();
    if (screen && g_purpose != Purpose::NewGame) continue_with_tld(screen, g_purpose);
}

// START (hosting) / READY and CANCEL (a friend).
void lobby_confirm() {
    const net::Status st = netrt::status();
    if (g.mode == kJoin) {
        if (st.rejected) {
            play(api.sound_error);
            return;
        }
        g.ready = !g.ready;
        play(g.ready ? api.sound_confirm : api.sound_back);
        g.notice.clear();
        logx::info("menu: lobby: %s", g.ready ? "READY" : "not ready");
        refresh_lobby(true);
        return;
    }
    const auto waiting = not_ready(st);
    if (!waiting.empty()) {
        play(api.sound_error);
        lobby_notice("Not yet: " + names(waiting) + (waiting.size() == 1 ? " isn't" : " aren't") + " READY.");
        logx::info("menu: lobby: START waits for %s", names(waiting).c_str());
        return;
    }
    play(api.sound_confirm);
    go_on(st.players.empty() ? "START (nobody else in yet)" : "START (everyone READY)");
}

// LET IN (which 0) / TURN AWAY or KICK (which 1), for the selected friend.
void lobby_act(int which) {
    const LobbyLine* chosen = chosen_friend();
    if (!chosen || (which == 0 && !chosen->asking)) return;
    const LobbyLine l = *chosen;
    if (which == 0) {
        const bool in = netrt::admit(l.link);
        play(in ? api.sound_confirm : api.sound_error);
        lobby_notice(in ? "You let " + l.name + " in." : "Couldn't let " + l.name + " in: the game is full (4 players).");
        logx::info("menu: lobby: LET IN '%s'%s", l.name.c_str(), in ? "" : " (full)");
        return;
    }
    const bool out = netrt::turn_away(l.link);
    play(out ? api.sound_click : api.sound_error);
    lobby_notice(!out ? l.name + " already left." : l.asking ? "You turned " + l.name + " away." : "You kicked " + l.name + ".");
    logx::info("menu: lobby: %s '%s'", l.asking ? "TURN AWAY" : "KICK", l.name.c_str());
}

bool lobby_click(Il2CppObject* go) {
    if (go == target(g.confirm)) {
        lobby_confirm();
    } else if (go == target(g.back)) {
        leave_lobby();
    } else if (go == target(g.actions[0])) {
        lobby_act(0);
    } else if (go == target(g.actions[1])) {
        lobby_act(1);
    } else if (go == target(g.rows[kCode])) {
        lobby_select(kCode);
        copy_code();
    } else {
        int row = -1;
        for (int i = 0; i < kCode; ++i)
            if (go == target(g.rows[i])) row = i;
        if (row < 0) return false;
        lobby_select(row);
    }
    return true;
}

void lobby_frame() {
    if (Il2CppObject* hovered = il2::invoke(api.hovered, nullptr))
        for (int i = 0; i < kLobbyRows; ++i)
            if (hovered == target(g.rows[i])) lobby_select(i);
    const ULONGLONG now = GetTickCount64();
    if (g.notice != g.timed_notice) {
        g.timed_notice = g.notice;
        g.notice_at = now;
    } else if (!g.notice.empty() && now - g.notice_at >= kNoticeMs) {
        g.notice.clear();
        g.timed_notice.clear();
        refresh_lobby(true);
    }
    if (now - g.last_status_check >= kLobbyRefreshMs) {
        g.last_status_check = now;
        const net::Status st = netrt::status();
        const net::PlayerInfo* host = lobby_host(st);
        if (g.mode == kJoin && g.ready && st.connected && host && host->ready) {
            play(api.sound_confirm);
            go_on("READY, and " + host->name + " has started");
            return;
        }
        refresh_lobby(false);
    }
    const auto key = poll_keys();
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    auto next_row = [](int row, int step) {
        for (int n = 0; n < kLobbyRows; ++n) {
            row = (row + kLobbyRows + step) % kLobbyRows;
            if (!g.row_text[row].empty()) break;
        }
        return row;
    };
    if (key[VK_UP]) lobby_select(next_row(g.selected, -1));
    if (key[VK_DOWN]) lobby_select(next_row(g.selected, +1));
    if (ctrl && key['C']) copy_code();
    if (key['Y']) lobby_act(0);
    if (key['N'] || key[VK_DELETE]) lobby_act(1);
    if (key[VK_RETURN]) lobby_confirm();
    else if (key[VK_ESCAPE]) leave_lobby();
}

// --- hooks ---------------------------------------------------------------------------------------------------------

// NEW, RESUME and LOAD in the Survival menu ask SOLO or CO-OP first; TLD's own runs once the player has chosen.
// The Survival menu we bring our screens back over: the one TLD shows now (the main menu scene loads again after a
// game, and its panels with it).
void keep_sandbox(Il2CppObject* self) {
    if (target(g_sandbox) == self) return;
    drop(g_sandbox);
    g_sandbox = hold(self);
}

void ask_first(Il2CppObject* self, const MethodInfo* method, Purpose purpose, ClickFn original) {
    keep_sandbox(self);
    if (ch.open || g.open || !open_chooser(self, purpose)) original(self, method);
}
void sandbox_new_detour(Il2CppObject* self, const MethodInfo* method) {
    ask_first(self, method, Purpose::NewGame, g_sandbox_new);
}
void sandbox_resume_detour(Il2CppObject* self, const MethodInfo* method) {
    ask_first(self, method, Purpose::Resume, g_sandbox_resume);
}
void sandbox_load_detour(Il2CppObject* self, const MethodInfo* method) {
    ask_first(self, method, Purpose::Load, g_sandbox_load);
}

// Back at the Survival menu (from the new-game screens or the save list, or after quitting a game): co-op picked
// there ends, and the next NEW / RESUME / LOAD asks again. BACK on the save list in a co-op load returns to the
// setup screen instead, with the lobby kept.
void sandbox_enable_detour(Il2CppObject* self, bool enable, const MethodInfo* method) {
    g_sandbox_enable(self, enable, method);
    if (!enable) {
        if (ch.open) {
            // TLD is moving on without us (e.g. a menu reset): drop our screen quietly.
            destroy(target(ch.root), false);
            destroy(target(ch.loader), false);
            release_chooser();
        }
        return;
    }
    keep_sandbox(self);
    if (g_back_to_setup) {
        g_back_to_setup = false;
        g_setup_done = false;
        g_reopen_setup_in = 2;
        return;
    }
    end_coop_flow("back at the Survival menu");
}

// BACK on the save list: in a co-op load, the setup screen is the screen before it.
void saves_back_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_coop_flow && g_setup_done && g_purpose == Purpose::Load) g_back_to_setup = true;
    g_saves_back(self, method);
}

// Right after CO-OP, TLD opens its difficulty screen: the setup screen takes its place until confirmed. After SOLO,
// or once set up, TLD's own screen shows.
void experience_enable_detour(Il2CppObject* self, bool enable, const MethodInfo* method) {
    g_experience_enable(self, enable, method);
    if (enable && g_coop_flow && g_purpose == Purpose::NewGame && !g_setup_done && !g.open) {
        open_menu(self);
    } else if (!enable && g.open && g_purpose == Purpose::NewGame) {
        // TLD is moving on without us: drop our screen quietly.
        destroy(target(g.root), false);
        destroy(target(g.loader), false);
        release();
    }
}

// BACK on the difficulty screen in a co-op game returns to the setup screen, the screen before it.
void experience_back_detour(Il2CppObject* self, const MethodInfo* method) {
    if (g_coop_flow && g_purpose == Purpose::NewGame && g_setup_done && !g.open) {
        g_setup_done = false;
        if (open_menu(self)) return;
    }
    g_experience_back(self, method);
}

// Male / Female in a co-op game: VOICE on the setup screen already answered it, so it's never shown. Going
// forward it's skipped with that voice; arriving again means the player came back from the screen after it, so
// it's stepped back over to the map. Both happen on the next frame, outside TLD's screen switch.
void survivor_enable_detour(Il2CppObject* self, bool enable, const MethodInfo* method) {
    g_survivor_enable(self, enable, method);
    if (!enable || !g_coop_flow || !g_setup_done || g_purpose != Purpose::NewGame) return;
    set_active(go_of(self), false);
    drop(g_pending_survivor);
    g_pending_survivor = hold(self);
    g_pending_step = g_survivor_skipped ? SurvivorStep::Back : SurvivorStep::Skip;
}

void run_pending_survivor() {
    Il2CppObject* panel = target(g_pending_survivor);
    drop(g_pending_survivor);
    if (!panel) return;
    set_active(go_of(panel), true);
    if (g_pending_step == SurvivorStep::Skip) {
        int32_t persona = g_voice;
        void* args[] = {&persona};
        il2::invoke(g_select_survivor, panel, args);
        g_survivor_skipped = true;
        logx::info("menu: Male / Female answered by the setup's VOICE (%s)", kVoiceNames[g_voice]);
    } else {
        il2::invoke(g_survivor_back, panel);
        g_survivor_skipped = false;
        logx::info("menu: back past Male / Female to the map");
    }
}

// After the setup screen is left: SOLO / CO-OP again, over the Survival menu TLD has just gone back to.
void reopen_chooser() {
    Il2CppObject* sandbox = target(g_sandbox);
    bool shown = false;
    if (!sandbox || ch.open || g.open || !il2::invoke_value(api.go_active, go_of(sandbox), shown) || !shown) return;
    open_chooser(sandbox, g_purpose);
}

// After BACK on the save list in a co-op load: the setup screen again, over the Survival menu.
void reopen_setup() {
    Il2CppObject* sandbox = target(g_sandbox);
    bool shown = false;
    if (!sandbox || ch.open || g.open || !il2::invoke_value(api.go_active, go_of(sandbox), shown) || !shown) return;
    if (!open_menu(sandbox)) end_coop_flow("setup screen unavailable");
}

// Panel_MainMenu fades its home page (logo, news, its own list, the helicopter scene) back in whenever none of TLD's
// sub-menus is up, and keeps its own buttons live then. Our screens count as one, like the difficulty screen they
// stand in for: behind them stays the Survival menu's scene, dimmed, as TLD's own sub-screens show it.
using SubMenuFn = bool (*)(Il2CppObject* self, const MethodInfo* method);
SubMenuFn g_sub_menu_enabled = nullptr;

bool sub_menu_enabled_detour(Il2CppObject* self, const MethodInfo* method) {
    if (ch.open || g.open) return true;
    return g_sub_menu_enabled(self, method);
}

void button_click_detour(Il2CppObject* self, const MethodInfo* method) {
    const ULONGLONG now = GetTickCount64();
    const bool recent = now - g_last_click < 150;
    if (ch.open || g.open || recent) {
        Il2CppObject* go = go_of(self);
        const auto clicked = reinterpret_cast<uintptr_t>(go);
        if (recent && clicked == g_last_click_go) return;  // the same click, through another UIButton
        // Note the click before acting: acting can close the screen, which the repeats would then miss.
        const uintptr_t previous_go = g_last_click_go;
        const ULONGLONG previous_time = g_last_click;
        g_last_click_go = clicked;
        g_last_click = now;
        if ((ch.open && chooser_click(go)) || (g.open && setup_click(go))) return;
        g_last_click_go = previous_go;  // not ours
        g_last_click = previous_time;
    }
    g_button_click(self, method);
}

}  // namespace

bool init() {
    Il2CppClass* game_object = il2::find_class(kCore, "UnityEngine", "GameObject");
    Il2CppClass* object = il2::find_class(kCore, "UnityEngine", "Object");
    Il2CppClass* component_class = il2::find_class(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = il2::find_class(kCore, "UnityEngine", "Transform");
    Il2CppClass* label = il2::find_class(kGame, "", "UILabel");
    Il2CppClass* sprite = il2::find_class(kGame, "", "UISprite");
    Il2CppClass* widget = il2::find_class(kGame, "", "UIWidget");
    Il2CppClass* panel = il2::find_class(kGame, "", "UIPanel");
    Il2CppClass* grid = il2::find_class(kGame, "", "UIGrid");
    Il2CppClass* camera = il2::find_class(kGame, "", "UICamera");
    Il2CppClass* button = il2::find_class(kGame, "", "UIButton");
    Il2CppClass* survivor = il2::find_class(kGame, "", "Panel_SelectSurvivor");
    Il2CppClass* experience = il2::find_class(kGame, "", "Panel_SelectExperience");
    Il2CppClass* saves = il2::find_class(kGame, "", "Panel_ChooseSandbox");
    Il2CppClass* sandbox = il2::find_class(kGame, "", "Panel_Sandbox");
    Il2CppClass* box = il2::find_class(kPhysics, "UnityEngine", "BoxCollider");

    api.game_object_class = game_object;
    api.go_ctor = il2::find_method_params(game_object, ".ctor", {"System.String"});
    api.component_go = il2::find_method(component_class, "get_gameObject", 0);
    api.go_transform = il2::find_method(game_object, "get_transform", 0);
    api.go_set_active = il2::find_method(game_object, "SetActive", 1);
    api.go_component = il2::find_method_params(game_object, "GetComponent", {"System.Type"});
    api.go_components = il2::find_method_params(game_object, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    api.instantiate = il2::find_method_params(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
    api.destroy = il2::find_method(object, "Destroy", 1);
    api.destroy_now = il2::find_method(object, "DestroyImmediate", 1);
    api.alive = il2::find_method(object, "op_Implicit", 1);
    api.set_name = il2::find_method(object, "set_name", 1);
    api.find = il2::find_method_params(transform, "Find", {"System.String"});
    api.get_parent = il2::find_method(transform, "get_parent", 0);
    api.set_parent = il2::find_method(transform, "SetParent", 2);
    api.get_local_position = il2::find_method(transform, "get_localPosition", 0);
    api.set_local_position = il2::find_method(transform, "set_localPosition", 1);
    api.set_local_euler = il2::find_method(transform, "set_localEulerAngles", 1);
    api.set_local_scale = il2::find_method(transform, "set_localScale", 1);
    api.label_set_text = il2::find_method(label, "set_text", 1);
    api.label_printed = il2::find_method(label, "get_printedSize", 0);
    api.label_set_font_size = il2::find_method(label, "set_fontSize", 1);
    api.label_set_alignment = il2::find_method(label, "set_alignment", 1);
    api.label_set_overflow = il2::find_method(label, "set_overflowMethod", 1);
    api.widget_set_pivot = il2::find_method(widget, "set_pivot", 1);
    api.widget_get_color = il2::find_method(widget, "get_color", 0);
    api.widget_set_color = il2::find_method(widget, "set_color", 1);
    api.box_set_size = il2::find_method(box, "set_size", 1);
    api.box_set_center = il2::find_method(box, "set_center", 1);
    api.widget_pivot = il2::find_method(widget, "get_pivotOffset", 0);
    api.widget_set_width = il2::find_method(widget, "set_width", 1);
    api.widget_set_height = il2::find_method(widget, "set_height", 1);
    api.widget_resize_collider = il2::find_method(widget, "ResizeCollider", 0);
    api.sprite_set_name = il2::find_method(sprite, "set_spriteName", 1);
    api.sprite_valid = il2::find_method(sprite, "get_isValid", 0);
    api.panel_set_depth = il2::find_method(panel, "set_depth", 1);
    api.panel_set_alpha = il2::find_method(panel, "set_alpha", 1);
    api.go_active = il2::find_method(game_object, "get_activeInHierarchy", 0);
    api.grid_reposition = il2::find_method(grid, "Reposition", 0);
    api.hovered = il2::find_method(camera, "get_hoveredObject", 0);
    Il2CppClass* audio = il2::find_class(kGame, "", "GameAudioManager");
    api.sound_scroll = il2::find_method(audio, "PlayGUIScroll", 0);
    api.sound_change = il2::find_method(audio, "PlayGUIComboBoxScroll", 0);
    api.sound_click = il2::find_method(audio, "PlayGUIButtonClick", 0);
    api.sound_confirm = il2::find_method(audio, "PlayGuiConfirm", 0);
    api.sound_back = il2::find_method(audio, "PlayGUIButtonBack", 0);
    api.sound_error = il2::find_method(audio, "PlayGUIError", 0);
    api.component_type = il2::type_object(component_class);
    api.label_type = il2::type_object(label);
    api.sprite_type = il2::type_object(sprite);
    api.panel_type = il2::type_object(panel);
    api.grid_type = il2::type_object(grid);
    api.box_type = il2::type_object(box);
    if (Il2CppClass* ui_texture = il2::find_class(kGame, "", "UITexture")) {
        api.texture_type = il2::type_object(ui_texture);
        if (api.texture_type) il2::il2cpp_gchandle_new(api.texture_type, false);
        api.texture_set_main = il2::find_method(ui_texture, "set_mainTexture", 1);
    }
    api.widget_set_depth = il2::find_method(widget, "set_depth", 1);
    api.add_component = il2::find_method_params(game_object, "AddComponent", {"System.Type"});
    api.go_get_layer = il2::find_method(game_object, "get_layer", 0);
    api.go_set_layer = il2::find_method(game_object, "set_layer", 1);
    api.grid_hide_inactive = il2::field_offset(grid, "hideInactive");
    for (Il2CppObject* t : {api.component_type, api.label_type, api.sprite_type, api.panel_type, api.grid_type,
                            api.box_type})
        if (t) il2::il2cpp_gchandle_new(t, false);

    const MethodInfo* sandbox_enable = il2::find_method(sandbox, "Enable", 1);
    const MethodInfo* experience_enable = il2::find_method(experience, "Enable", 1);
    const MethodInfo* survivor_enable = il2::find_method(survivor, "Enable", 1);
    g_sandbox_new_info = il2::find_method(sandbox, "OnClickNew", 0);
    g_sandbox_resume_info = il2::find_method(sandbox, "OnClickResume", 0);
    g_sandbox_load_info = il2::find_method(sandbox, "OnClickLoad", 0);
    g_saves_back_info = il2::find_method(saves, "OnClickBack", 0);
    g_experience_back_info = il2::find_method(experience, "OnClickBack", 0);
    g_select_survivor = il2::find_method(survivor, "OnSelectSurvivor", 1);
    g_survivor_back = il2::find_method(survivor, "OnClickBack", 0);
    const MethodInfo* click = il2::find_method(button, "OnClick", 0);
    const bool ready = sandbox_enable && experience_enable && survivor_enable && g_sandbox_new_info &&
                       g_sandbox_resume_info && g_sandbox_load_info && g_saves_back_info &&
                       g_experience_back_info && g_select_survivor && g_survivor_back && click && api.instantiate &&
                       api.find && api.label_set_text && api.go_components && api.go_active;
    if (!ready) {
        logx::warn("menu: co-op screens unavailable (TLD UI API missing); co-op only via autostart in coop.ini");
        return false;
    }
    auto hook = [](const char* name, const MethodInfo* m, auto* detour, auto** original) {
        return hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    };
    const MethodInfo* sub_menu = il2::find_method(il2::find_class(kGame, "", "Panel_MainMenu"), "IsSubMenuEnabled", 0);
    if (!sub_menu || !hook("Panel_MainMenu.IsSubMenuEnabled", sub_menu, &sub_menu_enabled_detour, &g_sub_menu_enabled))
        logx::warn("menu: the main menu's home page will show behind the co-op screens (IsSubMenuEnabled not hooked)");
    return hook("Panel_Sandbox.OnClickNew", g_sandbox_new_info, &sandbox_new_detour, &g_sandbox_new) &&
           hook("Panel_Sandbox.OnClickResume", g_sandbox_resume_info, &sandbox_resume_detour, &g_sandbox_resume) &&
           hook("Panel_Sandbox.OnClickLoad", g_sandbox_load_info, &sandbox_load_detour, &g_sandbox_load) &&
           hook("Panel_ChooseSandbox.OnClickBack", g_saves_back_info, &saves_back_detour, &g_saves_back) &&
           hook("Panel_Sandbox.Enable", sandbox_enable, &sandbox_enable_detour, &g_sandbox_enable) &&
           hook("Panel_SelectExperience.Enable", experience_enable, &experience_enable_detour, &g_experience_enable) &&
           hook("Panel_SelectExperience.OnClickBack", g_experience_back_info, &experience_back_detour,
                &g_experience_back) &&
           hook("Panel_SelectSurvivor.Enable", survivor_enable, &survivor_enable_detour, &g_survivor_enable) &&
           hook("UIButton.OnClick", click, &button_click_detour, &g_button_click);
}

void on_frame() {
    if (g_pending_survivor) run_pending_survivor();
    if (g_reopen_chooser_in > 0 && --g_reopen_chooser_in == 0) reopen_chooser();
    if (g_reopen_setup_in > 0 && --g_reopen_setup_in == 0) reopen_setup();
    if (ch.open) chooser_frame();
    else if (g.open) setup_frame();
    else poll_keys();  // keep key states current so keys held before a screen opens don't fire
}

bool lobby_ready() { return g_coop_flow && (g_started || (g.open && g.lobby && g.mode == kJoin && g.ready)); }

std::string body_name(const std::string& id) { return display_name(id); }

bool dev(const std::string& action, const std::string& arg) {
    auto fail = [&](const char* why) {
        logx::warn("dev: menu %s: %s", action.c_str(), why);
        return false;
    };
    if (action == "new" || action == "resume" || action == "load") {
        Il2CppObject* sandbox = target(g_sandbox);
        if (!sandbox) return fail("the Survival menu hasn't been opened");
        if (action == "new") sandbox_new_detour(sandbox, g_sandbox_new_info);
        if (action == "resume") sandbox_resume_detour(sandbox, g_sandbox_resume_info);
        if (action == "load") sandbox_load_detour(sandbox, g_sandbox_load_info);
    } else if (action == "solo" || action == "coop") {
        if (!ch.open) return fail("SOLO / CO-OP isn't open");
        close_chooser(action == "solo" ? kChooseSolo : kChooseCoop);
    } else if (action == "mode" || action == "code") {
        if (!g.open || g.lobby) return fail("the setup isn't open");
        if (action == "mode") g.mode = arg == "join" ? kJoin : kHost;
        if (action == "code") g.join_code = clean_code(arg);
        refresh();
    } else if (action == "confirm" || action == "ready" || action == "start") {
        if (!g.open) return fail("the setup isn't open");
        if (!g.lobby) close_menu(true);
        else if (action != "ready" || !g.ready) lobby_confirm();
    } else if (action == "admit" || action == "kick") {
        if (!g.open || !g.lobby || g.mode != kHost) return fail("the host's lobby isn't open");
        refresh_lobby(true);
        int row = -1;
        for (size_t i = 0; i < g.lines.size() && i < static_cast<size_t>(kCode); ++i)
            if (g.lines[i].name == arg && !g.lines[i].you) row = static_cast<int>(i);
        if (row < 0) return fail("nobody by that name in the lobby");
        lobby_select(row);
        lobby_act(action == "admit" ? 0 : 1);
    } else if (action == "select") {  // a row, as the mouse or the arrow keys pick it (0 = the first)
        const int row = atoi(arg.c_str());
        if (!g.open || row < 0 || row >= kRowCount) return fail("the setup isn't open, or no such row");
        if (g.lobby) lobby_select(row);
        else select(row);
    } else if (action == "change") {  // the selected row's arrows: change +1 / change -1
        if (!g.open || g.lobby) return fail("the setup isn't open");
        change(g.selected, atoi(arg.c_str()) < 0 ? -1 : 1);
    } else if (action == "back") {
        if (ch.open) close_chooser(-1);
        else if (g.open && g.lobby) leave_lobby();
        else if (g.open) close_menu(false);
        else return fail("no co-op screen is open");
    } else if (action != "status") {
        return fail("unknown (new, resume, load, solo, coop, mode host|join, code X, select ROW, change +1|-1, confirm, "
                    "ready, start, admit NAME, kick NAME, back, status)");
    }
    if (g.open && g.lobby) refresh_lobby(true);
    std::string shown;
    if (g.open && g.lobby) {
        for (const auto& r : g.row_text)
            if (!r.empty()) shown += " | " + r;
        shown += " | button " + std::string(g.mode == kHost ? "START" : g.ready ? "CANCEL" : "READY");
    } else if (g.open) {
        constexpr const char* kSwitchNames[] = {"NAMES", "RADAR", "AUTO MAP"};
        for (int i = 0; i < 3; ++i) shown += std::string(" | ") + kSwitchNames[i] + (g.hud[i] ? " ON" : " OFF");
        shown += " | selected row " + std::to_string(g.selected);
    }
    logx::info("dev: menu: %s%s%s", ch.open ? "SOLO / CO-OP" : !g.open ? "no co-op screen" : g.lobby ? "LOBBY" : "SETUP",
               shown.c_str(), g_started ? " (started)" : "");
    return true;
}

bool resume_with_tld() {
    Il2CppObject* sandbox = target(g_sandbox);
    if (!sandbox || !g_sandbox_resume || !g_sandbox_resume_info) return false;
    continue_with_tld(sandbox, Purpose::Resume);
    return true;
}

}  // namespace coop_menu
