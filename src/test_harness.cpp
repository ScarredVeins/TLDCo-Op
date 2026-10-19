#include "test_harness.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "config.h"
#include "coop_menu.h"
#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "paths.h"

namespace test_harness {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr int32_t kSaveSlotSandbox = 3;  // SaveSlotType.SANDBOX
constexpr ULONGLONG kMenuSettleMs = 3000;  // the main menu has been up (and no movie playing) this long before we load
constexpr ULONGLONG kSurvivalMenuMs = 2000;  // SURVIVAL has been open this long before we press RESUME

struct Api {
    const MethodInfo* on_sandbox = nullptr;    // Panel_MainMenu.OnSandbox(): the SURVIVAL button
    const MethodInfo* slot_info = nullptr;     // static SaveGameSlotHelper.GetSaveSlotInfo(SaveSlotType, int)
    const MethodInfo* slot_count = nullptr;    // static SaveGameSlotHelper.GetNumSaveSlots(SaveSlotType)
    ptrdiff_t slot_name = -1;                  // SaveSlotInfo.m_SaveSlotName
    const MethodInfo* load_slot = nullptr;     // Panel_MainMenu.OnLoadSaveSlot(SaveSlotType, int): how RESUME loads
    const MethodInfo* game_quit = nullptr;     // static GameManager.OnGameQuit(): the pause menu's quit, before...
    const MethodInfo* main_menu = nullptr;     // static GameManager.LoadMainMenu()
} api;

config::Settings g_settings;

// god=1: nobody dies while a test runs. TLD's own "god" console command toggles PlayerManager.m_God (and the save
// keeps it), so the flags are set directly and kept set: PlayerManager.m_God and Condition.m_Invulnerable.
struct GodApi {
    const MethodInfo* player_manager = nullptr;  // static GameManager.GetPlayerManagerComponent()
    const MethodInfo* condition = nullptr;       // static GameManager.GetConditionComponent()
    ptrdiff_t god = -1, invulnerable = -1;
} god_api;
ULONGLONG g_god_checked = 0;
bool g_charge_needs = false;  // dev `needs on`: m_God off, needs paid (still invulnerable)

void keep_god_mode() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_god_checked < 2000) return;
    g_god_checked = now;
    Il2CppObject* pm = il2::invoke(god_api.player_manager, nullptr);
    Il2CppObject* condition = il2::invoke(god_api.condition, nullptr);
    if (!pm || !condition || god_api.god < 0 || god_api.invulnerable < 0) return;
    const bool god = !g_charge_needs, on = true;
    const bool was = il2::read_field<bool>(pm, god_api.god) == god && il2::read_field<bool>(condition, god_api.invulnerable);
    memcpy(reinterpret_cast<char*>(pm) + god_api.god, &god, 1);
    memcpy(reinterpret_cast<char*>(condition) + god_api.invulnerable, &on, 1);
    if (!was) logx::info("test: god mode on (nobody dies while testing)%s", god ? "" : "; needs are paid");
}

enum class Step { MainMenu, SurvivalOpen, Done };
Step g_step = Step::MainMenu;
ULONGLONG g_step_since = 0;

// "Press any key" screens: the boot screens (disclaimers; Panel_Boot.Update asks InputManager.HasPressedKey) and the
// hold after a load (Panel_Loading.Update asks InputManager.AnyButtonsOrKeysPressed). With resume= set, the answer
// is yes while those panels ask, so the game gets into the save on its own.
using UpdateFn = void (*)(Il2CppObject* self, const MethodInfo* method);
using KeyFn = bool (*)(const MethodInfo* method);
UpdateFn g_boot_update = nullptr;
UpdateFn g_loading_update = nullptr;
KeyFn g_has_pressed_key = nullptr;
KeyFn g_any_key = nullptr;
bool g_in_wait_screen = false;
int g_boot_keys = 0, g_loading_keys = 0;
// Nothing while a movie runs: the main menu opens with the intro movie (intro_cards). Loading the save under it
// leaves it to finish in game, where MoviePlayer.DoStop throws every frame (RestorePanelsAfterSubtitle: the menu's
// panels are gone) and its last frame (the Hinterland logo on black) stays over the game for good.
FieldInfo* g_movie_state = nullptr;  // static MoviePlayer.State MoviePlayer.m_State: 0 = Stopped

int32_t movie_state();
bool movie_running() { return movie_state() != 0; }

bool press_now() { return g_in_wait_screen && !movie_running(); }

// Unity pauses an unfocused game, and co-op only lifts that once the game proper runs: a copy booting behind the
// other's window would sit at the boot screens until clicked.
const MethodInfo* g_set_run_in_background = nullptr;
bool g_background_set = false;

// The main menu's intro movie is skipped the way a player skips it: Escape, which MoviePlayer.CheckForInterrupt
// asks for while a movie plays (InputManager.GetEscapePressed; the intro isn't m_IsSkippable, so continue and
// interact don't, but Escape cancels it). It costs every boot ~11 s, and twice a windowed copy sat in it for minutes
// (cause not found: losing the focus, being covered or being minimized didn't do it).
using InterruptFn = bool (*)(Il2CppObject* self, const MethodInfo* method);
using EscapeFn = bool (*)(Il2CppObject* context, const MethodInfo* method);
InterruptFn g_movie_interrupt = nullptr;
EscapeFn g_escape_pressed = nullptr;
bool g_skip_movie = false;            // inside CheckForInterrupt, at the main menu, while the movie plays
constexpr int32_t kMoviePlaying = 2;  // MoviePlayer.State.Playing

int32_t movie_state() {
    int32_t state = 0;
    if (g_movie_state) il2::il2cpp_field_static_get_value(g_movie_state, &state);
    return state;
}

bool movie_interrupt_detour(Il2CppObject* self, const MethodInfo* method) {
    g_skip_movie = movie_state() == kMoviePlaying && game::active_scene() == "MainMenu";
    const bool stop = g_movie_interrupt(self, method);
    if (g_skip_movie && stop) {
        static bool told = false;
        if (!told) logx::info("test: skipped the intro movie (Escape, as a player does)");
        told = true;
    }
    g_skip_movie = false;
    return stop;
}

bool escape_pressed_detour(Il2CppObject* context, const MethodInfo* method) {
    return g_skip_movie || g_escape_pressed(context, method);
}

void keep_window_place();

void boot_update_detour(Il2CppObject* self, const MethodInfo* method) {
    keep_window_place();
    if (!g_background_set && g_set_run_in_background) {
        g_background_set = true;
        bool on = true;
        void* args[] = {&on};
        il2::invoke(g_set_run_in_background, nullptr, args);
        logx::info("test: boot keeps running when unfocused");
    }
    g_in_wait_screen = true;
    g_boot_update(self, method);
    g_in_wait_screen = false;
}

void loading_update_detour(Il2CppObject* self, const MethodInfo* method) {
    g_in_wait_screen = true;
    g_loading_update(self, method);
    g_in_wait_screen = false;
}

bool has_pressed_key_detour(const MethodInfo* method) {
    if (press_now()) {
        if (g_boot_keys++ == 0) logx::info("test: pressing past the boot screens");
        return true;
    }
    return g_has_pressed_key(method);
}

bool any_key_detour(const MethodInfo* method) {
    if (press_now()) {
        if (g_loading_keys++ == 0) logx::info("test: pressing past the loading screen");
        return true;
    }
    return g_any_key(method);
}

// own_saves: TLD keeps the save slots' folder in PersistentDataPath.m_PathSurvival: SaveGameDataPC.LoadAllSaves lists
// it, and SaveGameDataPC.GetFullPath (every slot read and write; the survival profile too, settings not) builds paths
// in it. PersistentDataPath.Init sets it at boot (and may again later): after each Init it's pointed at this copy's
// own folder, and GetFullPath's answers under TLD's folder are moved to ours as well, so nothing of this copy's is
// read from or written to TLD's folder (the player's own games are there). The own folder is first filled with
// copies of the slots: from <mod_dir>/seeds/<slot> when there is one, else from TLD's folder. Init can run before
// our hook is in (a warm start): then the first GetFullPath, or failing that the first frame, points it at ours.
FieldInfo* g_path_survival = nullptr;
using InitPathsFn = void (*)(const MethodInfo* method);
InitPathsFn g_init_paths = nullptr;
using FullPathFn = Il2CppString* (*)(Il2CppString* name, const MethodInfo* method);
FullPathFn g_full_path = nullptr;
std::string g_tld_survival, g_own_survival;  // TLD's folder and ours, '/' separated, no trailing '/'
bool g_own_saves_done = false;               // m_PathSurvival was pointed at ours and GetFullPath is hooked
int g_paths_moved = 0;

std::string path_key(std::string p) {
    for (char& c : p) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

void seed_own_saves(const std::filesystem::path& tld, const std::filesystem::path& own) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(own, ec);
    const std::string slots = g_settings.test_own_saves;
    for (size_t at = 0; at <= slots.size();) {
        const size_t comma = std::min(slots.find(',', at), slots.size());
        const std::string slot = slots.substr(at, comma - at);
        at = comma + 1;
        if (slot.empty()) continue;
        const fs::path seed = paths::mod_dir() / "seeds" / slot;
        const bool from_seed = fs::exists(seed, ec);
        fs::copy_file(from_seed ? seed : tld / slot, own / slot, ec);
        logx::info("test: own saves: copied %s from %s%s", slot.c_str(), from_seed ? "the rig's seeds" : "TLD's folder",
                   ec ? " (FAILED)" : "");
    }
    for (const auto& entry : fs::directory_iterator(tld, ec))
        if (entry.path().filename().wstring().rfind(L"profile_survival", 0) == 0)
            fs::copy_file(entry.path(), own / entry.path().filename(), ec);
}

void use_own_saves(bool after_init) {
    Il2CppString* now_text = nullptr;
    il2::il2cpp_field_static_get_value(g_path_survival, &now_text);
    const std::string now = now_text ? il2::to_utf8(now_text) : std::string();
    if (now.empty() || (!g_own_survival.empty() && path_key(now) == path_key(g_own_survival))) return;
    namespace fs = std::filesystem;
    const fs::path own = paths::instance_dir() / "Survival";
    const bool first = g_own_survival.empty();
    if (first) {
        g_tld_survival = now;
        while (!g_tld_survival.empty() && (g_tld_survival.back() == '/' || g_tld_survival.back() == '\\')) g_tld_survival.pop_back();
        const std::u8string own_text = own.generic_u8string();
        g_own_survival.assign(own_text.begin(), own_text.end());
        std::error_code ec;
        if (!fs::exists(own, ec)) seed_own_saves(fs::path(std::u8string(now.begin(), now.end())), own);
    }
    std::string path = g_own_survival;
    if (now.back() == '/' || now.back() == '\\') path += '/';
    Il2CppString* value = il2::il2cpp_string_new(path.c_str());
    il2::il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(value), false);  // kept alive for good
    il2::il2cpp_field_static_set_value(g_path_survival, value);
    Il2CppString* back = nullptr;
    il2::il2cpp_field_static_get_value(g_path_survival, &back);
    if (back != value) {  // the other way of passing a reference: a pointer to it
        il2::il2cpp_field_static_set_value(g_path_survival, &value);
        il2::il2cpp_field_static_get_value(g_path_survival, &back);
    }
    if (back != value) {
        logx::warn("test: own saves: couldn't point TLD at %s; this copy won't save", path.c_str());
        return;
    }
    g_own_saves_done = g_full_path != nullptr;
    if (first)
        logx::info("test: own saves: this copy's save slots are in %s (TLD's: %s)%s", path.c_str(), now.c_str(),
                   after_init ? "" : "; TLD had set its folder before our hook");
    else
        logx::info("test: own saves: TLD set its folder again (%s); pointed back at ours", now.c_str());
}

void init_paths_detour(const MethodInfo* method) {
    g_init_paths(method);
    use_own_saves(true);
}

Il2CppString* full_path_detour(Il2CppString* name, const MethodInfo* method) {
    if (g_own_survival.empty() && g_path_survival) use_own_saves(false);  // Init ran before our hook
    Il2CppString* path = g_full_path(name, method);
    if (!path || g_tld_survival.empty()) return path;
    const std::string text = il2::to_utf8(path);
    const std::string key = path_key(text), tld = path_key(g_tld_survival);
    if (key.size() <= tld.size() || key.compare(0, tld.size(), tld) != 0 || key[tld.size()] != '/') return path;
    const std::string moved = g_own_survival + "/" + text.substr(g_tld_survival.size() + 1);
    if (g_paths_moved++ < 10) logx::info("test: own saves: %s -> %s", text.c_str(), moved.c_str());
    return il2::il2cpp_string_new(moved.c_str());
}

using SaveGameFn = void (*)(Il2CppString* name, Il2CppString* scene_save_name, const MethodInfo* method);
SaveGameFn g_save_game = nullptr;
int g_saves_skipped = 0;

// nosave: never. own_saves: only once TLD saves into this copy's own folder (never into the player's).
void save_game_detour(Il2CppString* name, Il2CppString* scene_save_name, const MethodInfo* method) {
    if (!g_settings.test_nosave && g_own_saves_done) {
        g_save_game(name, scene_save_name, method);
        return;
    }
    if (g_saves_skipped++ < 20)
        logx::info("test: save of '%s' skipped (%s)", il2::to_utf8(name).c_str(),
                   g_settings.test_nosave ? "nosave" : "own saves aren't in use: it would go to TLD's folder");
}

using DeleteDataFn = int32_t (*)(Il2CppString* name, const MethodInfo* method);
DeleteDataFn g_delete_data = nullptr;
int g_deletes_skipped = 0;

// Deleting a save file (SaveGameDataPC.DeleteData: every file TLD deletes, the whole game when the player dies too):
// the same rule as saving. A copy that doesn't save into its own folder deletes nothing, and tells TLD it did.
int32_t delete_data_detour(Il2CppString* name, const MethodInfo* method) {
    if (!g_settings.test_nosave && g_own_saves_done) return g_delete_data(name, method);
    if (g_deletes_skipped++ < 20)
        logx::info("test: deleting '%s' skipped (%s)", il2::to_utf8(name).c_str(),
                   g_settings.test_nosave ? "nosave" : "own saves aren't in use: it would go to TLD's folder");
    return 4;  // SaveGameOperationReturnValue.SUCCESS
}

using DeleteSlotFn = void (*)(Il2CppObject* slot, const MethodInfo* method);
DeleteSlotFn g_delete_slot = nullptr;

// nosave: a save slot stays listed too (SaveGameSlots.DeleteSlot: what a death does, before deleting its files), so the
// test save can be loaded again after a player died in it (dev `reload`).
void delete_slot_detour(Il2CppObject* slot, const MethodInfo* method) {
    if (!g_settings.test_nosave) {
        g_delete_slot(slot, method);
        return;
    }
    if (g_deletes_skipped++ < 20) logx::info("test: deleting a save slot skipped (nosave)");
}

// window=WxH: TLD applies the player's display settings at boot through Screen.SetResolution (over -screen-width);
// a test copy asks Unity for a window of its own size instead. TLD's settings stay as the player has them.
using SetResolutionFn = void (*)(int32_t width, int32_t height, int32_t mode, uint64_t refresh, const MethodInfo* method);
SetResolutionFn g_set_resolution = nullptr;
int32_t g_window_w = 0, g_window_h = 0, g_window_x = -1, g_window_y = -1;
constexpr int32_t kWindowed = 3;  // FullScreenMode.Windowed
ULONGLONG g_window_checked = 0;

BOOL CALLBACK find_our_window(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || wcscmp(cls, L"UnityWndClass") != 0) return TRUE;
    *reinterpret_cast<HWND*>(out) = hwnd;
    return FALSE;
}

// Unity moves the window when the resolution changes: put it back where the rig wants it (about once a second).
void keep_window_place() {
    if (g_window_x < 0 || GetTickCount64() - g_window_checked < 1000) return;
    g_window_checked = GetTickCount64();
    HWND hwnd = nullptr;
    EnumWindows(&find_our_window, reinterpret_cast<LPARAM>(&hwnd));
    RECT rect{};
    if (!hwnd || IsIconic(hwnd) || !GetWindowRect(hwnd, &rect) || (rect.left == g_window_x && rect.top == g_window_y))
        return;
    SetWindowPos(hwnd, nullptr, g_window_x, g_window_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    static int told = 0;
    if (told++ < 3) logx::info("test: window moved to (%d, %d)", g_window_x, g_window_y);
}

// A movie still running 30 s into the main menu (once): its state and our window's, for the next stall's log.
ULONGLONG g_movie_seen = 0;

void note_long_movie(bool running) {
    static bool told = false;
    const ULONGLONG now = GetTickCount64();
    if (!running) g_movie_seen = 0;
    else if (!g_movie_seen) g_movie_seen = now;
    if (told || !g_movie_seen || now - g_movie_seen < 30000) return;
    told = true;
    HWND hwnd = nullptr;
    EnumWindows(&find_our_window, reinterpret_cast<LPARAM>(&hwnd));
    RECT rect{};
    if (hwnd) GetWindowRect(hwnd, &rect);
    logx::warn("test: the main menu's movie still runs after 30 s (state %d); our window: %s%s%s, (%ld, %ld)-(%ld, %ld)",
               movie_state(), hwnd ? "found" : "not found", hwnd && IsIconic(hwnd) ? ", minimized" : "",
               hwnd && GetForegroundWindow() == hwnd ? ", focused" : "", rect.left, rect.top, rect.right, rect.bottom);
}

void set_resolution_detour(int32_t width, int32_t height, int32_t mode, uint64_t refresh, const MethodInfo* method) {
    static int told = 0;
    if (told++ < 3)
        logx::info("test: window: TLD asked for %dx%d (mode %d); %dx%d in a window instead", width, height, mode,
                   g_window_w, g_window_h);
    g_set_resolution(g_window_w, g_window_h, kWindowed, refresh, method);  // RefreshRate: 8 bytes, by value
}

// The main menu, as last seen updating (Panel_MainMenu.Update).
using UpdateMenuFn = void (*)(Il2CppObject* self, const MethodInfo* method);
UpdateMenuFn g_main_menu_update = nullptr;
uintptr_t g_main_menu = 0;

void main_menu_update_detour(Il2CppObject* self, const MethodInfo* method) {
    g_main_menu_update(self, method);
    if (g_main_menu && il2::il2cpp_gchandle_get_target(g_main_menu) == self) return;
    if (g_main_menu) il2::il2cpp_gchandle_free(g_main_menu);
    g_main_menu = il2::il2cpp_gchandle_new(self, false);
}

// What a player does: SURVIVAL on the main menu...
void open_survival() {
    Il2CppObject* menu = g_main_menu ? il2::il2cpp_gchandle_get_target(g_main_menu) : nullptr;
    if (!menu) {
        logx::warn("test: no main menu to press SURVIVAL on");
        g_step = Step::Done;
        return;
    }
    il2::invoke(api.on_sandbox, menu);
    logx::info("test: SURVIVAL");
    g_step = Step::SurvivalOpen;
}

// The Survival save at `index` in TLD's list (newest first), or "" past the end.
std::string slot_at(int32_t index) {
    int32_t type = kSaveSlotSandbox;
    void* args[] = {&type, &index};
    Il2CppObject* info = il2::invoke(api.slot_info, nullptr, args);
    return info ? il2::to_utf8(il2::read_field<Il2CppString*>(info, api.slot_name)) : std::string();
}

// An older save: load it from TLD's list the way RESUME loads the newest (Panel_MainMenu.OnLoadSaveSlot).
void load_older(const std::string& slot, const std::string& newest) {
    int32_t type = kSaveSlotSandbox, count = 0;
    void* count_args[] = {&type};
    if (api.slot_count) il2::invoke_value(api.slot_count, nullptr, count, count_args);
    Il2CppObject* menu = g_main_menu ? il2::il2cpp_gchandle_get_target(g_main_menu) : nullptr;
    for (int32_t index = 1; index < count && menu && api.load_slot; ++index) {
        if (slot_at(index) != slot) continue;
        void* args[] = {&type, &index};
        il2::invoke(api.load_slot, menu, args);
        logx::info("test: LOAD: loading save '%s' (number %d of %d; the newest is '%s')", slot.c_str(), index + 1, count,
                   newest.c_str());
        return;
    }
    logx::warn("test: can't load '%s' (%s)", slot.c_str(),
               count == 0 ? "no Survival saves" : !menu || !api.load_slot ? "TLD's load isn't available" : "no such save");
}

// ...then RESUME, which loads the newest Survival save; an older one is loaded from the list.
void resume(const std::string& slot) {
    g_step = Step::Done;
    const std::string name = slot_at(0);
    if (name != slot) {
        load_older(slot, name);
        return;
    }
    if (!coop_menu::resume_with_tld()) {
        logx::warn("test: can't resume '%s' (the Survival menu didn't open)", slot.c_str());
        return;
    }
    logx::info("test: RESUME: loading save '%s'", slot.c_str());
}

}  // namespace

bool runs_unfocused() { return g_background_set; }

void charge_needs(bool on) {
    g_charge_needs = on;
    g_god_checked = 0;  // at once
    if (g_settings.test_god) keep_god_mode();
}

bool reload() {
    if (g_settings.test_resume.empty()) return false;
    if (game::active_scene() != "MainMenu") {
        if (!api.main_menu) return false;
        if (api.game_quit) il2::invoke(api.game_quit, nullptr);  // as Panel_PauseMenu.DoQuitGame (nosave: no save)
        il2::invoke(api.main_menu, nullptr);
        logx::info("test: reload: quit to the main menu (TLD's own); then SURVIVAL and RESUME again");
    } else {
        logx::info("test: reload: SURVIVAL and RESUME again");
    }
    g_step = Step::MainMenu;
    g_step_since = 0;
    return true;
}

bool init() {
    g_settings = config::load();
    Il2CppClass* main_menu = il2::find_class(kGame, "", "Panel_MainMenu");
    Il2CppClass* game_manager = il2::find_class(kGame, "", "GameManager");
    god_api.player_manager = il2::find_method(game_manager, "GetPlayerManagerComponent", 0);
    god_api.condition = il2::find_method(game_manager, "GetConditionComponent", 0);
    god_api.god = il2::field_offset(il2::find_class(kGame, "", "PlayerManager"), "m_God");
    god_api.invulnerable = il2::field_offset(il2::find_class(kGame, "", "Condition"), "m_Invulnerable");
    if (Il2CppClass* movie = il2::find_class(kGame, "", "MoviePlayer"))
        g_movie_state = il2::il2cpp_class_get_field_from_name(movie, "m_State");
    api.on_sandbox = il2::find_method(main_menu, "OnSandbox", 0);
    api.slot_info = il2::find_method(il2::find_class(kGame, "", "SaveGameSlotHelper"), "GetSaveSlotInfo", 2);
    api.slot_name = il2::field_offset(il2::find_class(kGame, "", "SaveSlotInfo"), "m_SaveSlotName");
    api.slot_count = il2::find_method(il2::find_class(kGame, "", "SaveGameSlotHelper"), "GetNumSaveSlots", 1);
    api.load_slot = il2::find_method(main_menu, "OnLoadSaveSlot", 2);
    api.game_quit = il2::find_method(game_manager, "OnGameQuit", 0);
    api.main_menu = il2::find_method(game_manager, "LoadMainMenu", 0);
    if (sscanf_s(g_settings.test_window.c_str(), "%dx%d+%d+%d", &g_window_w, &g_window_h, &g_window_x, &g_window_y) >= 2 &&
        g_window_w > 0 && g_window_h > 0) {
        const MethodInfo* set_resolution = il2::find_method_params(
            il2::find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Screen"), "SetResolution",
            {"System.Int32", "System.Int32", "UnityEngine.FullScreenMode", "UnityEngine.RefreshRate"});
        if (!set_resolution ||
            !hooks::install_method("Screen.SetResolution", set_resolution, reinterpret_cast<void*>(&set_resolution_detour),
                                   reinterpret_cast<void**>(&g_set_resolution)))
            logx::warn("test: window=%s requested but Screen.SetResolution can't be hooked", g_settings.test_window.c_str());
    }
    if (!g_settings.test_own_saves.empty()) {
        const MethodInfo* full_path = il2::find_method(il2::find_class(kGame, "", "SaveGameDataPC"), "GetFullPath", 1);
        if (!full_path || !hooks::install_method("SaveGameDataPC.GetFullPath", full_path, reinterpret_cast<void*>(&full_path_detour),
                                                 reinterpret_cast<void**>(&g_full_path)))
            g_full_path = nullptr;
        Il2CppClass* data_path = il2::find_class(kGame, "", "PersistentDataPath");
        g_path_survival = data_path ? il2::il2cpp_class_get_field_from_name(data_path, "m_PathSurvival") : nullptr;
        const MethodInfo* init_paths = il2::find_method(data_path, "Init", 0);
        if (!g_full_path || !g_path_survival || !init_paths ||
            !hooks::install_method("PersistentDataPath.Init", init_paths, reinterpret_cast<void*>(&init_paths_detour),
                                   reinterpret_cast<void**>(&g_init_paths)))
            logx::warn("test: own_saves requested but TLD's save paths can't be reached; this copy won't save");
    }
    if (g_settings.test_nosave || !g_settings.test_own_saves.empty()) {
        const MethodInfo* save = il2::find_method_params(il2::find_class(kGame, "", "SaveGameSystem"), "SaveGame",
                                                         {"System.String", "System.String"});
        if (!save || !hooks::install_method("SaveGameSystem.SaveGame", save, reinterpret_cast<void*>(&save_game_detour),
                                            reinterpret_cast<void**>(&g_save_game)))
            logx::warn("test: SaveGameSystem.SaveGame couldn't be hooked; this copy WILL save (to TLD's folder)");
        const MethodInfo* del = il2::find_method(il2::find_class(kGame, "", "SaveGameDataPC"), "DeleteData", 1);
        if (!del || !hooks::install_method("SaveGameDataPC.DeleteData", del, reinterpret_cast<void*>(&delete_data_detour),
                                           reinterpret_cast<void**>(&g_delete_data)))
            logx::warn("test: SaveGameDataPC.DeleteData couldn't be hooked; this copy MAY delete saves (a death deletes "
                       "its game)");
        const MethodInfo* del_slot = il2::find_method_params(il2::find_class(kGame, "", "SaveGameSlots"), "DeleteSlot",
                                                             {"SlotData"});
        if (!del_slot || !hooks::install_method("SaveGameSlots.DeleteSlot", del_slot,
                                                reinterpret_cast<void*>(&delete_slot_detour),
                                                reinterpret_cast<void**>(&g_delete_slot)))
            logx::warn("test: SaveGameSlots.DeleteSlot couldn't be hooked; after a death the test save may be unlisted");
    }
    if (!g_settings.test_resume.empty()) {
        g_set_run_in_background = il2::find_method(
            il2::find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Application"), "set_runInBackground", 1);
        Il2CppClass* input = il2::find_class(kGame, "", "InputManager");
        const MethodInfo* boot_update = il2::find_method(il2::find_class(kGame, "", "Panel_Boot"), "Update", 0);
        const MethodInfo* loading_update = il2::find_method(il2::find_class(kGame, "", "Panel_Loading"), "Update", 0);
        const MethodInfo* has_key = il2::find_method(input, "HasPressedKey", 0);
        const MethodInfo* any_key = il2::find_method(input, "AnyButtonsOrKeysPressed", 0);
        auto hook = [](const char* name, const MethodInfo* m, auto* detour, auto** original) {
            return m && hooks::install_method(name, m, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
        };
        if (!hook("Panel_Boot.Update", boot_update, &boot_update_detour, &g_boot_update) ||
            !hook("Panel_Loading.Update", loading_update, &loading_update_detour, &g_loading_update) ||
            !hook("InputManager.HasPressedKey", has_key, &has_pressed_key_detour, &g_has_pressed_key) ||
            !hook("InputManager.AnyButtonsOrKeysPressed", any_key, &any_key_detour, &g_any_key))
            logx::warn("test: can't press past the boot / loading screens; press a key in the game window");
        if (!hook("InputManager.GetEscapePressed", il2::find_method(input, "GetEscapePressed", 1), &escape_pressed_detour,
                  &g_escape_pressed) ||
            !hook("MoviePlayer.CheckForInterrupt",
                  il2::find_method(il2::find_class(kGame, "", "MoviePlayer"), "CheckForInterrupt", 0),
                  &movie_interrupt_detour, &g_movie_interrupt))
            logx::warn("test: the intro movie can't be skipped; it plays out");
    }
    const MethodInfo* menu_update = il2::find_method(main_menu, "Update", 0);
    const bool ready = api.on_sandbox && api.slot_info && api.slot_name >= 0 && menu_update;
    if (ready && !g_settings.test_resume.empty())
        hooks::install_method("Panel_MainMenu.Update", menu_update, reinterpret_cast<void*>(&main_menu_update_detour),
                              reinterpret_cast<void**>(&g_main_menu_update));
    if (!g_settings.test_resume.empty() && !ready) logx::warn("test: resume unavailable (TLD API missing)");
    return ready;
}

void on_frame(const std::string& scene) {
    if (g_own_survival.empty() && g_path_survival && g_init_paths) use_own_saves(false);  // Init ran before our hook
    keep_window_place();
    if (g_settings.test_god && game::is_gameplay_scene(scene) && game::read_player().valid) keep_god_mode();
    if (g_settings.test_resume.empty() || g_step == Step::Done) return;
    const ULONGLONG now = GetTickCount64();
    if (scene == "MainMenu") note_long_movie(movie_running());
    if (scene != "MainMenu" || movie_running()) {
        g_step_since = 0;
        return;
    }
    if (!g_step_since) g_step_since = now;
    if (g_step == Step::MainMenu && now - g_step_since >= kMenuSettleMs) {
        open_survival();
        g_step_since = now;
    } else if (g_step == Step::SurvivalOpen && now - g_step_since >= kSurvivalMenuMs) {
        if (g_settings.test_lobby) {
            g_step = Step::Done;
            logx::info("test: the Survival menu is open; the co-op screens are the test's (dev `menu`)");
            return;
        }
        resume(g_settings.test_resume);
    }
}

}  // namespace test_harness
