#include "steam_dispatch.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "hooks.h"
#include "log.h"
#include "net/transport.h"

namespace steam_dispatch {
namespace {

constexpr int kApiCallCompleted = 703;  // SteamAPICallCompleted_t (k_iSteamUtilsCallbacks + 3)

// CallbackMsg_t and SteamAPICallCompleted_t from the Steamworks SDK.
struct CallbackMsg {
    int32_t user;
    int32_t callback;
    uint8_t* param;
    int32_t param_size;
};
#pragma pack(push, 8)
struct ApiCallCompleted {
    uint64_t call;
    int32_t callback;
    uint32_t param_size;
};
#pragma pack(pop)

using NextCallbackFn = bool (*)(int32_t pipe, CallbackMsg* msg);
NextCallbackFn g_next_callback = nullptr;
net::steam_calls::FetchFn g_fetch = nullptr;  // SteamAPI_ManualDispatch_GetAPICallResult
std::mutex g_install_mutex;
std::atomic<bool> g_done{false};

// TLD pulls each Steam callback here, then frees it. When the notice is for one of our requests, collect the
// reply now; this is the moment manual dispatch expects GetAPICallResult to be called.
bool next_callback_detour(int32_t pipe, CallbackMsg* msg) {
    const bool got = g_next_callback(pipe, msg);
    if (got && msg && msg->callback == kApiCallCompleted && msg->param &&
        msg->param_size >= static_cast<int32_t>(sizeof(ApiCallCompleted))) {
        const auto* done = reinterpret_cast<const ApiCallCompleted*>(msg->param);
        net::steam_calls::on_completed(pipe, done->call, g_fetch);
    }
    return got;
}

// Idempotent; safe from the main thread or the network thread. Does nothing until TLD has loaded Steam.
void install() {
    if (g_done) return;
    std::lock_guard lock(g_install_mutex);
    if (g_done) return;
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return;
    g_done = true;
    void* next = reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_ManualDispatch_GetNextCallback"));
    g_fetch = reinterpret_cast<net::steam_calls::FetchFn>(GetProcAddress(steam, "SteamAPI_ManualDispatch_GetAPICallResult"));
    if (!next || !g_fetch) {
        logx::warn("steam: manual-dispatch functions missing; relying on polling for Steam replies");
        return;
    }
    hooks::install("SteamAPI_ManualDispatch_GetNextCallback", next, reinterpret_cast<void*>(&next_callback_detour),
                   reinterpret_cast<void**>(&g_next_callback));
}

}  // namespace

void init() { net::steam_calls::set_dispatch_installer(&install); }

void on_frame() { install(); }

}  // namespace steam_dispatch
