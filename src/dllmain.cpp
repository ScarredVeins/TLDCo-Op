// winhttp.dll proxy: forwards every winhttp export to the real System32 DLL, and bootstraps the co-op mod.
//
// Load order: tld.exe -> UnityPlayer.dll imports WINHTTP.dll -> this DLL (game folder wins the search order).
// We hook LoadLibraryExW to see GameAssembly.dll arrive, hook its il2cpp_init, and run core::on_runtime_ready on
// the main thread once the IL2CPP runtime is up.
//
// Kill switch: create an empty file <game>/TLDCoop/disabled and the DLL only forwards winhttp calls.
//
// Test profiles: "tld.exe -tldcoop NAME" runs this copy with its own settings, log and dev commands in
// <game>/TLDCoop/NAME, and lets it start next to another copy: Unity refuses a second copy of the game through a
// named mutex ("...-SingleInstanceMutex-"), so with a profile that mutex gets the profile's name. A profile copy
// also keeps its window to itself: Unity saves the window's size and mode as TLD's screen preferences
// (HKCU\Software\Hinterland\TheLongDark, "Screenmanager ..."), which the player's own game starts with.

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "core.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "paths.h"
#include "proxy_exports.h"

namespace {

std::atomic<bool> g_game_assembly_hooked{false};
std::atomic<bool> g_runtime_ready{false};

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using il2cpp_init_t = int (*)(const char*);
using il2cpp_init_utf16_t = int (*)(const wchar_t*);

LoadLibraryExW_t g_load_library_ex_w = nullptr;
il2cpp_init_t g_il2cpp_init = nullptr;
il2cpp_init_utf16_t g_il2cpp_init_utf16 = nullptr;

using CreateMutexW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using CreateMutexExW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
CreateMutexW_t g_create_mutex_w = nullptr;
CreateMutexExW_t g_create_mutex_ex_w = nullptr;

// Unity's single-instance mutex, renamed for this profile; other names pass through.
std::wstring profile_mutex_name(LPCWSTR name) {
    if (!name || !wcsstr(name, L"SingleInstanceMutex")) return {};
    const std::wstring suffix = L"-" + paths::profile();
    std::wstring renamed = name;
    if (renamed.size() >= suffix.size() && renamed.compare(renamed.size() - suffix.size(), suffix.size(), suffix) == 0)
        return {};  // already ours (CreateMutexW can go through CreateMutexExW)
    renamed += suffix;
    logx::info("profile '%ls': single-instance mutex '%ls' -> '%ls'", paths::profile().c_str(), name, renamed.c_str());
    return renamed;
}

HANDLE WINAPI create_mutex_w_detour(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name) {
    const std::wstring renamed = profile_mutex_name(name);
    return g_create_mutex_w(attributes, owner, renamed.empty() ? name : renamed.c_str());
}

HANDLE WINAPI create_mutex_ex_w_detour(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access) {
    const std::wstring renamed = profile_mutex_name(name);
    return g_create_mutex_ex_w(attributes, renamed.empty() ? name : renamed.c_str(), flags, access);
}

void allow_second_copy() {
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"kernelbase", "CreateMutexW", reinterpret_cast<void*>(&create_mutex_w_detour),
                           reinterpret_cast<void**>(&g_create_mutex_w), &target) == MH_OK)
        MH_EnableHook(target);
    if (MH_CreateHookApiEx(L"kernelbase", "CreateMutexExW", reinterpret_cast<void*>(&create_mutex_ex_w_detour),
                           reinterpret_cast<void**>(&g_create_mutex_ex_w), &target) == MH_OK)
        MH_EnableHook(target);
    logx::info("profile '%ls': settings, log and commands in %ls; can run next to another copy (%s)",
               paths::profile().c_str(), paths::instance_dir().c_str(),
               g_create_mutex_w && g_create_mutex_ex_w ? "mutex hooks in" : "mutex hooks FAILED");
}

using RegSetValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
using RegSetValueExA_t = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
RegSetValueExW_t g_reg_set_value_w = nullptr;
RegSetValueExA_t g_reg_set_value_a = nullptr;
std::atomic<int> g_screen_prefs_kept{0};

// Unity's screen preferences (PlayerPrefs names: "Screenmanager Resolution Width_h182942802", ...).
template <typename Char>
bool screen_pref(const Char* name) {
    static const char* const kPrefixes[] = {"Screenmanager", "UnitySelectMonitor"};
    if (!name) return false;
    for (const char* prefix : kPrefixes) {
        size_t i = 0;
        while (prefix[i] && name[i] == static_cast<Char>(prefix[i])) ++i;
        if (!prefix[i]) {
            if (g_screen_prefs_kept++ == 0)
                logx::info("profile '%ls': Unity's screen preferences aren't saved (they're the player's game's)",
                           paths::profile().c_str());
            return true;
        }
    }
    return false;
}

LSTATUS WINAPI reg_set_value_w_detour(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    return screen_pref(name) ? ERROR_SUCCESS : g_reg_set_value_w(key, name, reserved, type, data, size);
}

LSTATUS WINAPI reg_set_value_a_detour(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    return screen_pref(name) ? ERROR_SUCCESS : g_reg_set_value_a(key, name, reserved, type, data, size);
}

void keep_screen_prefs() {
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"kernelbase", "RegSetValueExW", reinterpret_cast<void*>(&reg_set_value_w_detour),
                           reinterpret_cast<void**>(&g_reg_set_value_w), &target) == MH_OK)
        MH_EnableHook(target);
    if (MH_CreateHookApiEx(L"kernelbase", "RegSetValueExA", reinterpret_cast<void*>(&reg_set_value_a_detour),
                           reinterpret_cast<void**>(&g_reg_set_value_a), &target) == MH_OK)
        MH_EnableHook(target);
    if (!g_reg_set_value_w || !g_reg_set_value_a)
        logx::warn("profile '%ls': registry hooks FAILED; Unity may save this copy's window as the player's",
                   paths::profile().c_str());
}

int log_crash(unsigned long code, EXCEPTION_POINTERS* info) {
    logx::error("exception 0x%08lx at %p during mod start-up; mod disabled for this session, game continues", code,
                info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects in this frame, so SEH can guard the whole start-up and keep the game alive if we fault.
void run_core_guarded() {
    __try {
        core::on_runtime_ready();
    } __except (log_crash(GetExceptionCode(), GetExceptionInformation())) {
    }
}

void after_runtime_init() {
    bool expected = false;
    if (!g_runtime_ready.compare_exchange_strong(expected, true)) return;
    run_core_guarded();
}

int il2cpp_init_detour(const char* domain) {
    int result = g_il2cpp_init(domain);
    logx::info("il2cpp_init(\"%s\") returned %d", domain ? domain : "", result);
    after_runtime_init();
    return result;
}

int il2cpp_init_utf16_detour(const wchar_t* domain) {
    int result = g_il2cpp_init_utf16(domain);
    logx::info("il2cpp_init_utf16(\"%ls\") returned %d", domain ? domain : L"", result);
    after_runtime_init();
    return result;
}

void on_game_assembly_loaded(HMODULE game_assembly) {
    bool expected = false;
    if (!g_game_assembly_hooked.compare_exchange_strong(expected, true)) return;

    logx::info("GameAssembly.dll loaded at %p", game_assembly);
    if (!il2::resolve(game_assembly)) {
        logx::error("IL2CPP API incomplete; mod stays inactive");
        return;
    }
    hooks::install("il2cpp_init", reinterpret_cast<void*>(GetProcAddress(game_assembly, "il2cpp_init")),
                   reinterpret_cast<void*>(&il2cpp_init_detour), reinterpret_cast<void**>(&g_il2cpp_init));
    hooks::install("il2cpp_init_utf16", reinterpret_cast<void*>(GetProcAddress(game_assembly, "il2cpp_init_utf16")),
                   reinterpret_cast<void*>(&il2cpp_init_utf16_detour),
                   reinterpret_cast<void**>(&g_il2cpp_init_utf16));
}

HMODULE WINAPI load_library_ex_w_detour(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE module = g_load_library_ex_w(name, file, flags);
    if (module && !g_game_assembly_hooked.load() && module == GetModuleHandleW(L"GameAssembly.dll"))
        on_game_assembly_loaded(module);
    return module;
}

void start() {
    const auto dir = paths::mod_dir();
    if (std::filesystem::exists(dir / L"disabled")) return;

    logx::open(paths::instance_dir() / L"coop.log");
    logx::info("TLDCoop loader %s %s, proxy %ls (%d forwards)", __DATE__, __TIME__, PROXY_MODULE_NAME,
               PROXY_EXPORT_COUNT);
    logx::info("game dir: %ls", paths::game_dir().c_str());

    if (!hooks::init()) return;
    if (!paths::profile().empty()) {
        allow_second_copy();
        keep_screen_prefs();
    }

    if (HMODULE already = GetModuleHandleW(L"GameAssembly.dll")) {
        on_game_assembly_loaded(already);  // unexpected this early, but harmless
        return;
    }
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"kernelbase", "LoadLibraryExW", reinterpret_cast<void*>(&load_library_ex_w_detour),
                           reinterpret_cast<void**>(&g_load_library_ex_w), &target) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        logx::error("could not hook LoadLibraryExW; mod inactive");
        return;
    }
    logx::info("waiting for GameAssembly.dll");
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        start();
    }
    return TRUE;
}
