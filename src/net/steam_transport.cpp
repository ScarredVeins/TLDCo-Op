// Steam transport: the host creates a Steam lobby tagged with a short join code; the client finds it by that
// code, joins, and both talk through Steam P2P messages (relayed by Steam when a direct path isn't possible, so
// nobody needs IPs or port forwarding). Uses the game's own steam_api64.dll through its flat C API.

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "net/session.h"
#include "net/transport.h"

namespace net {

namespace steam_calls {
namespace {
struct Pending {
    int callback_id = 0;
    int size = 0;
    bool ready = false;
    std::vector<uint8_t> data;
};
std::mutex g_mutex;
std::map<uint64_t, Pending> g_pending;
std::atomic<InstallFn> g_installer{nullptr};
}  // namespace

void set_dispatch_installer(InstallFn install) { g_installer = install; }

void install_dispatch_hook() {
    if (InstallFn install = g_installer.load()) install();
}

void expect(uint64_t call, int callback_id, int size) {
    std::lock_guard lock(g_mutex);
    g_pending[call] = Pending{callback_id, size, false, {}};
}

bool take(uint64_t call, void* out, int size) {
    std::lock_guard lock(g_mutex);
    auto it = g_pending.find(call);
    if (it == g_pending.end() || !it->second.ready || static_cast<int>(it->second.data.size()) != size) return false;
    memcpy(out, it->second.data.data(), static_cast<size_t>(size));
    g_pending.erase(it);
    return true;
}

void forget(uint64_t call) {
    std::lock_guard lock(g_mutex);
    g_pending.erase(call);
}

void on_completed(int32_t pipe, uint64_t call, FetchFn fetch) {
    std::lock_guard lock(g_mutex);
    auto it = g_pending.find(call);
    if (it == g_pending.end() || it->second.ready || !fetch) return;
    std::vector<uint8_t> buf(static_cast<size_t>(it->second.size));
    bool failed = false;
    if (fetch(pipe, call, buf.data(), it->second.size, it->second.callback_id, &failed) && !failed) {
        it->second.data = std::move(buf);
        it->second.ready = true;
    }
}
}  // namespace steam_calls

namespace {

constexpr const char* kCodeKey = "tldcoop_code";
constexpr const char* kVersionKey = "tldcoop_version";
constexpr int kChannel = 0;
constexpr int kSendFlags = 32;  // k_nSteamNetworkingSend_Unreliable | AutoRestartBrokenSession
constexpr double kSearchRetryMs = 4000;  // the host may not be up yet: keep looking
constexpr double kMemberPollMs = 1000;

constexpr int kLobbyTypePublic = 2;  // searchable by our code filter (no one else queries TLDCoop lobbies)
constexpr int kComparisonEqual = 0;
constexpr int kDistanceWorldwide = 3;
constexpr int kResultOk = 1;
constexpr int kEnterSuccess = 1;

// Steam callback ids (k_iSteamMatchmakingCallbacks = 500) and structs (VALVE_CALLBACK_PACK_LARGE on Win64).
constexpr int kLobbyEnterId = 504, kLobbyMatchListId = 510, kLobbyCreatedId = 513;
#pragma pack(push, 8)
struct LobbyCreated {
    int32_t result;
    uint64_t lobby;
};
struct LobbyMatchList {
    uint32_t count;
};
struct LobbyEnter {
    uint64_t lobby;
    uint32_t permissions;
    bool locked;
    uint32_t response;
};
#pragma pack(pop)

// SteamNetworkingIdentity is 136 bytes; we only touch it through Steam's own helpers.
struct Identity {
    alignas(8) uint8_t bytes[136];
};
// SteamNetworkingMessage_t starts with data, size, connection, then the sender identity.
struct Message {
    void* data;
    int32_t size;
    uint32_t conn;
    Identity peer;
};

struct Api {
    void* (*matchmaking)() = nullptr;
    void* (*utils)() = nullptr;
    void* (*user)() = nullptr;
    void* (*messages)() = nullptr;
    void* (*net_utils)() = nullptr;
    void* (*friends)() = nullptr;
    uint64_t (*create_lobby)(void*, int, int) = nullptr;
    bool (*set_lobby_data)(void*, uint64_t, const char*, const char*) = nullptr;
    void (*add_string_filter)(void*, const char*, const char*, int) = nullptr;
    void (*add_distance_filter)(void*, int) = nullptr;
    uint64_t (*request_lobby_list)(void*) = nullptr;
    uint64_t (*lobby_by_index)(void*, int) = nullptr;
    uint64_t (*join_lobby)(void*, uint64_t) = nullptr;
    void (*leave_lobby)(void*, uint64_t) = nullptr;
    uint64_t (*lobby_owner)(void*, uint64_t) = nullptr;
    int (*member_count)(void*, uint64_t) = nullptr;
    uint64_t (*member_by_index)(void*, uint64_t, int) = nullptr;
    bool (*call_completed)(void*, uint64_t, bool*) = nullptr;
    bool (*call_result)(void*, uint64_t, void*, int, int, bool*) = nullptr;
    uint64_t (*steam_id)(void*) = nullptr;
    int (*send_message)(void*, const Identity*, const void*, uint32_t, int, int) = nullptr;
    int (*receive_messages)(void*, int, Message**, int) = nullptr;
    bool (*close_session)(void*, const Identity*) = nullptr;
    void (*release)(Message*) = nullptr;
    void (*set_id)(Identity*, uint64_t) = nullptr;
    uint64_t (*get_id)(const Identity*) = nullptr;
    const char* (*friend_name)(void*, uint64_t) = nullptr;
    void (*init_relay)(void*) = nullptr;
    // Optional: manual-dispatch reply collection (TLD's mode) and failure diagnostics.
    int32_t (*get_pipe)() = nullptr;
    bool (*manual_call_result)(int32_t, uint64_t, void*, int, int, bool*) = nullptr;
    int (*failure_reason)(void*, uint64_t) = nullptr;

    bool load(std::string& missing) {
        HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
        if (!dll) {
            missing = "steam_api64.dll (Steam not loaded)";
            return false;
        }
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
            if (!fn) {
                missing += std::string(missing.empty() ? "" : ", ") + name;
                ok = false;
            }
        };
        get(matchmaking, "SteamAPI_SteamMatchmaking_v009");
        get(utils, "SteamAPI_SteamUtils_v010");
        get(user, "SteamAPI_SteamUser_v021");
        get(messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        get(net_utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
        get(friends, "SteamAPI_SteamFriends_v017");
        get(create_lobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
        get(set_lobby_data, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        get(add_string_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
        get(add_distance_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
        get(request_lobby_list, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
        get(lobby_by_index, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
        get(join_lobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
        get(leave_lobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
        get(lobby_owner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        get(member_count, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
        get(member_by_index, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
        get(call_completed, "SteamAPI_ISteamUtils_IsAPICallCompleted");
        get(call_result, "SteamAPI_ISteamUtils_GetAPICallResult");
        get(steam_id, "SteamAPI_ISteamUser_GetSteamID");
        get(send_message, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
        get(receive_messages, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
        get(close_session, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
        get(release, "SteamAPI_SteamNetworkingMessage_t_Release");
        get(set_id, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
        get(get_id, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
        get(friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
        get(init_relay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
        get_pipe = reinterpret_cast<decltype(get_pipe)>(GetProcAddress(dll, "SteamAPI_GetHSteamPipe"));
        manual_call_result =
            reinterpret_cast<decltype(manual_call_result)>(GetProcAddress(dll, "SteamAPI_ManualDispatch_GetAPICallResult"));
        failure_reason = reinterpret_cast<decltype(failure_reason)>(GetProcAddress(dll, "SteamAPI_ISteamUtils_GetAPICallFailureReason"));
        return ok;
    }
};

class SteamTransport final : public Transport {
public:
    ~SteamTransport() override { close(); }

    bool open(const Config& cfg, const LogFn& log) override {
        log_ = log;
        host_role_ = cfg.role == Role::Host;
        code_ = cfg.code;
        version_ = cfg.game_version + " / protocol " + std::to_string(kProtocolVersion);
        if (code_.empty()) return fail("no join code set");
        // At launch the mod starts before TLD has loaded and initialised Steam; wait for it.
        stage_ = Stage::WaitingForSteam;
        set_status("waiting for Steam to start");
        return true;
    }

    void poll(double now) override {
        switch (stage_) {
            case Stage::WaitingForSteam:
                if (now < retry_at_) break;
                retry_at_ = now + kSteamCheckMs;
                if (steam_started()) {
                    begin();
                } else if (++steam_checks_ * kSteamCheckMs > kSteamWaitMs) {
                    stage_ = Stage::Failed;
                    set_status("Steam didn't start; is the Steam client running and signed in?");
                }
                break;
            case Stage::Creating: {
                LobbyCreated r{};
                const Reply reply = result(r, kLobbyCreatedId);
                if (reply == Reply::Pending) break;
                if (reply == Reply::Failed || r.result != kResultOk) {
                    if (++create_attempts_ < kCreateAttempts) {
                        stage_ = Stage::CreateWait;
                        retry_at_ = now + kSearchRetryMs;
                        set_status("Steam couldn't create the lobby yet; trying again");
                    } else {
                        stage_ = Stage::Failed;
                        set_status("Steam couldn't create a lobby (error " + std::to_string(r.result) + ")");
                    }
                    break;
                }
                lobby_ = r.lobby;
                api_.set_lobby_data(mm_, lobby_, kCodeKey, code_.c_str());
                api_.set_lobby_data(mm_, lobby_, kVersionKey, version_.c_str());
                set_status("hosting; your code is " + code_ + " (checking it's findable on Steam)");
                // Self-check: look our own code up the way a friend will, so problems show up before they try.
                api_.add_string_filter(mm_, kCodeKey, code_.c_str(), kComparisonEqual);
                api_.add_distance_filter(mm_, kDistanceWorldwide);
                issue(api_.request_lobby_list(mm_), kLobbyMatchListId, sizeof(LobbyMatchList));
                stage_ = Stage::Verifying;
                break;
            }
            case Stage::CreateWait:
                if (now >= retry_at_) begin();
                break;
            case Stage::Verifying: {
                LobbyMatchList r{};
                const Reply reply = result(r, kLobbyMatchListId);
                if (reply == Reply::Pending) break;
                if (reply == Reply::Failed) r.count = 0;  // count it as "not seen yet" and try again
                bool found = false;
                for (uint32_t i = 0; i < r.count; ++i)
                    if (api_.lobby_by_index(mm_, static_cast<int>(i)) == lobby_) found = true;
                ++verify_attempts_;
                if (log_)
                    log_("steam: self-check " + std::to_string(verify_attempts_) + ": Steam returned " +
                         std::to_string(r.count) + " lobby(ies) for " + code_ + (found ? ", including ours" : ""));
                if (found) {
                    stage_ = Stage::Hosting;
                    set_status("hosting; your code is " + code_ + " (verified: findable on Steam)");
                } else if (verify_attempts_ < kVerifyAttempts) {
                    stage_ = Stage::VerifyWait;  // Steam's search index takes a moment to pick up new lobbies
                    retry_at_ = now + kVerifyDelayMs;
                } else {
                    stage_ = Stage::Hosting;
                    set_status("hosting; your code is " + code_ + " (not seen in our own Steam search; a friend's "
                               "search may still find it)");
                }
                break;
            }
            case Stage::VerifyWait:
                if (now >= retry_at_) {
                    api_.add_string_filter(mm_, kCodeKey, code_.c_str(), kComparisonEqual);
                    api_.add_distance_filter(mm_, kDistanceWorldwide);
                    issue(api_.request_lobby_list(mm_), kLobbyMatchListId, sizeof(LobbyMatchList));
                    stage_ = Stage::Verifying;
                }
                break;
            case Stage::Hosting:
                // Our side must message a new member once for Steam to accept their session.
                if (now - last_member_poll_ < kMemberPollMs) break;
                last_member_poll_ = now;
                for (int i = 0, n = api_.member_count(mm_, lobby_); i < n; ++i) {
                    const uint64_t member = api_.member_by_index(mm_, lobby_, i);
                    if (member && member != self_ && knocked_.insert(member).second) {
                        const uint8_t knock = 0;
                        send(member, &knock, 1);
                        set_status("hosting; " + describe(member) + " found your game");
                    }
                }
                break;
            case Stage::Searching: {
                LobbyMatchList r{};
                const Reply reply = result(r, kLobbyMatchListId);
                if (reply == Reply::Pending) break;
                if (reply == Reply::Failed || r.count == 0) {
                    stage_ = Stage::Retrying;
                    retry_at_ = now + kSearchRetryMs;
                    set_status("no game with code " + code_ + " yet; still looking");
                    break;
                }
                lobby_ = api_.lobby_by_index(mm_, 0);
                issue(api_.join_lobby(mm_, lobby_), kLobbyEnterId, sizeof(LobbyEnter));
                stage_ = Stage::Joining;
                set_status("found code " + code_ + "; joining");
                break;
            }
            case Stage::Retrying:
                if (now >= retry_at_) start_search();
                break;
            case Stage::Joining: {
                LobbyEnter r{};
                const Reply reply = result(r, kLobbyEnterId);
                if (reply == Reply::Pending) break;
                if (reply == Reply::Failed || r.response != kEnterSuccess) {
                    stage_ = Stage::Retrying;
                    retry_at_ = now + kSearchRetryMs;
                    set_status("couldn't join the game with code " + code_ + " (it may be full); retrying");
                    break;
                }
                host_ = api_.lobby_owner(mm_, lobby_);
                stage_ = Stage::Joined;
                set_status("connecting to " + describe(host_));
                break;
            }
            default:
                break;
        }
    }

    bool host_peer(PeerId& out) override {
        if (stage_ != Stage::Joined || !host_) return false;
        out = host_;
        return true;
    }

    void send(PeerId to, const uint8_t* data, size_t size) override {
        if (!msgs_) return;
        Identity id{};
        api_.set_id(&id, to);
        api_.send_message(msgs_, &id, data, static_cast<uint32_t>(size), kSendFlags, kChannel);
    }

    bool receive(PeerId& from, std::vector<uint8_t>& data) override {
        if (!msgs_) return false;
        Message* msg = nullptr;
        if (api_.receive_messages(msgs_, kChannel, &msg, 1) != 1 || !msg) return false;
        from = api_.get_id(&msg->peer);
        const auto* bytes = static_cast<const uint8_t*>(msg->data);
        data.assign(bytes, bytes + msg->size);
        api_.release(msg);
        return true;
    }

    void wait(int timeout_ms) override { Sleep(static_cast<DWORD>(timeout_ms < 2 ? timeout_ms : 2)); }

    void close() override {
        if (lobby_ && mm_) api_.leave_lobby(mm_, lobby_);
        lobby_ = 0;
        if (msgs_) {
            for (uint64_t peer : knocked_) {
                Identity id{};
                api_.set_id(&id, peer);
                api_.close_session(msgs_, &id);
            }
            if (host_) {
                Identity id{};
                api_.set_id(&id, host_);
                api_.close_session(msgs_, &id);
            }
        }
        knocked_.clear();
        host_ = 0;
        stage_ = Stage::Closed;
    }

    std::string describe(PeerId peer) const override {
        const char* name = friends_ && api_.friend_name ? api_.friend_name(friends_, peer) : nullptr;
        return name && *name ? std::string(name) : "Steam user " + std::to_string(peer);
    }

    std::string status() const override { return status_; }

private:
    enum class Stage {
        Idle, WaitingForSteam, Creating, CreateWait, Verifying, VerifyWait, Hosting,
        Searching, Retrying, Joining, Joined, Failed, Closed
    };
    static constexpr int kCreateAttempts = 3;
    int create_attempts_ = 0;
    static constexpr double kSteamCheckMs = 500;
    static constexpr double kSteamWaitMs = 120000;
    int steam_checks_ = 0;
    void* user_ = nullptr;
    static constexpr int kVerifyAttempts = 5;
    static constexpr double kVerifyDelayMs = 2000;
    int verify_attempts_ = 0;

    // True once steam_api64.dll is loaded and the game has initialised Steam (the interfaces exist).
    bool steam_started() {
        if (!api_.matchmaking) {
            std::string missing;
            if (!api_.load(missing)) {
                if (!GetModuleHandleW(L"steam_api64.dll")) return false;  // not loaded yet: keep waiting
                stage_ = Stage::Failed;
                set_status("Steam API incomplete: " + missing);
                return false;
            }
        }
        mm_ = api_.matchmaking();
        utils_ = api_.utils();
        msgs_ = api_.messages();
        friends_ = api_.friends();
        user_ = api_.user();
        return mm_ && utils_ && msgs_ && user_;
    }

    void begin() {
        steam_calls::install_dispatch_hook();  // before our first request, so no reply can be lost
        api_.init_relay(api_.net_utils());     // warm up Steam's relay network early
        self_ = api_.steam_id(user_);
        if (host_role_) {
            issue(api_.create_lobby(mm_, kLobbyTypePublic, static_cast<int>(kMaxPlayers)), kLobbyCreatedId, sizeof(LobbyCreated));
            stage_ = Stage::Creating;
            set_status("creating a Steam lobby for code " + code_);
        } else {
            start_search();
        }
    }

    void start_search() {
        api_.add_string_filter(mm_, kCodeKey, code_.c_str(), kComparisonEqual);
        api_.add_distance_filter(mm_, kDistanceWorldwide);
        issue(api_.request_lobby_list(mm_), kLobbyMatchListId, sizeof(LobbyMatchList));
        stage_ = Stage::Searching;
        if (status_.empty()) set_status("looking for code " + code_);
    }

    enum class Reply { Pending, Ready, Failed };

    // Poll the pending Steam call. TLD runs Steam in "manual dispatch" mode, where replies are collected with
    // SteamAPI_ManualDispatch_GetAPICallResult; ISteamUtils::GetAPICallResult is the fallback. Failures are
    // logged with Steam's reason and left to the caller to retry.
    // Every request goes through here so the dispatch hook knows to collect its reply.
    void issue(uint64_t call, int callback_id, size_t size) {
        call_ = call;
        steam_calls::expect(call, callback_id, static_cast<int>(size));
    }

    template <typename T>
    Reply result(T& out, int callback_id) {
        if (steam_calls::take(call_, &out, static_cast<int>(sizeof(T)))) return Reply::Ready;  // caught at dispatch
        bool failed = false;
        if (!api_.call_completed(utils_, call_, &failed)) return Reply::Pending;
        if (steam_calls::take(call_, &out, static_cast<int>(sizeof(T)))) return Reply::Ready;  // just caught
        steam_calls::forget(call_);
        bool got = false;
        if (!failed && api_.manual_call_result && api_.get_pipe)
            got = api_.manual_call_result(api_.get_pipe(), call_, &out, static_cast<int>(sizeof(T)), callback_id, &failed) && !failed;
        if (!got && !failed)
            got = api_.call_result(utils_, call_, &out, static_cast<int>(sizeof(T)), callback_id, &failed) && !failed;
        if (got) return Reply::Ready;
        static const char* kReasons[] = {"Steam gone", "network failure", "invalid handle", "mismatched callback"};
        const int reason = api_.failure_reason ? api_.failure_reason(utils_, call_) : -1;
        if (log_)
            log_("steam: request " + std::to_string(callback_id) + " failed: " +
                 (reason >= 0 && reason <= 3 ? kReasons[reason] : "reason " + std::to_string(reason)));
        return Reply::Failed;
    }

    bool fail(const std::string& why) {
        stage_ = Stage::Failed;
        set_status(why);
        return false;
    }

    void set_status(const std::string& s) {
        if (s == status_) return;
        status_ = s;
        if (log_) log_("steam: " + s);
    }

    Api api_;
    LogFn log_;
    void* mm_ = nullptr;
    void* utils_ = nullptr;
    void* msgs_ = nullptr;
    void* friends_ = nullptr;
    uint64_t self_ = 0;
    bool host_role_ = false;
    std::string code_, version_;
    Stage stage_ = Stage::Idle;
    uint64_t call_ = 0;
    uint64_t lobby_ = 0;
    uint64_t host_ = 0;
    double retry_at_ = 0, last_member_poll_ = 0;
    std::set<uint64_t> knocked_;
    std::string status_;
};

}  // namespace

std::unique_ptr<Transport> make_steam_transport() { return std::make_unique<SteamTransport>(); }

}  // namespace net
