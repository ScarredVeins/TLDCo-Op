#include "coop_notices.h"

#include <windows.h>

#include <map>
#include <string>
#include <utility>

#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"

namespace coop_notices {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr double kCheckMs = 250;
constexpr double kAskAgainMs = 20000;  // a friend still asking to join is mentioned again this often
constexpr int kLetInKey = VK_F9, kTurnAwayKey = VK_F10;

const MethodInfo* g_add_message = nullptr;  // static HUDMessage.AddMessage(string, bool highPriority, bool ignoreOverlay)

// Who was in the game when we last looked: id -> (link, name). The host's line on a friend's side is id 0.
std::map<uint8_t, std::pair<uint32_t, std::string>> g_known;
bool g_was_connected = false;
bool g_was_in_game = false;
bool g_was_client = false;
double g_last_check = -1e18;
std::map<uint32_t, double> g_asked;  // host: a friend asking (ticket) -> when we last said so
bool g_keys_down[2] = {};

void say(const std::string& text) {
    if (!g_add_message) return;
    bool high = false, over_overlay = false;
    void* args[] = {il2::il2cpp_string_new(text.c_str()), &high, &over_overlay};
    il2::invoke(g_add_message, nullptr, args);
    logx::info("notice: %s", text.c_str());
}

bool our_window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// A key that went down since the last frame (and the game window has the focus).
bool pressed(int index, int vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fresh = down && !g_keys_down[index];
    g_keys_down[index] = down;
    return fresh && our_window_focused();
}

// The host's answer to the first friend asking.
void answer_first(const net::PlayerInfo& first, bool let_in) {
    const bool done = let_in ? netrt::admit(first.link) : netrt::turn_away(first.link);
    say(!done ? (let_in ? "The game is full: " + first.name + " can't join" : first.name + " already left")
        : let_in ? "You let " + first.name + " in"
                 : "You turned " + first.name + " away");
    logx::info("notices: %s '%s'%s", let_in ? "let in" : "turned away", first.name.c_str(), done ? "" : " (couldn't)");
    g_asked.erase(first.link);
}

// Hosting while we play: a friend asks to join. F9 lets the first one asking in, F10 turns them away.
void answer_asking(const net::Status& st, double now) {
    const bool let_in = pressed(0, kLetInKey), turn_away = pressed(1, kTurnAwayKey);
    if (st.asking.empty()) {
        g_asked.clear();
        return;
    }
    const net::PlayerInfo& first = st.asking.front();
    if (let_in || turn_away) {
        answer_first(first, let_in);
        return;
    }
    const auto said = g_asked.find(first.link);
    if (said != g_asked.end() && now - said->second < kAskAgainMs) return;
    g_asked[first.link] = now;
    say(first.name + " wants to join your game. F9: let them in. F10: turn them away.");
}

}  // namespace

void tell(const std::string& text) { say(text); }

bool answer(bool let_in) {
    const auto role = netrt::role();
    if (!role || *role == net::Role::Client) return false;
    const net::Status st = netrt::status();
    if (st.asking.empty()) return false;
    answer_first(st.asking.front(), let_in);
    return true;
}

bool init() {
    g_add_message = il2::find_method_params(il2::find_class(kGame, "", "HUDMessage"), "AddMessage",
                                            {"System.String", "System.Boolean", "System.Boolean"});
    if (!g_add_message) logx::warn("notices: TLD's HUD messages unavailable; joins and leaves only go to coop.log");
    return g_add_message != nullptr;
}

void on_frame(bool in_game) {
    const auto role = netrt::role();
    const bool client = role && *role == net::Role::Client;
    const double now = net::now_ms();
    if (in_game && role && !client) answer_asking(netrt::status(), now);
    if (now - g_last_check < kCheckMs) return;
    g_last_check = now;
    const net::Status st = role ? netrt::status() : net::Status{};

    std::map<uint8_t, std::pair<uint32_t, std::string>> seen;
    for (const net::PlayerInfo& p : st.players)
        if (!p.name.empty()) seen[p.id] = {p.link, p.name};

    // Only what changes while we play: a game loading in (or co-op starting) takes everyone in quietly.
    const bool speak = in_game && g_was_in_game && role && client == g_was_client;
    if (speak) {
        const auto host = g_known.find(0);
        const std::string host_name = host != g_known.end() ? host->second.second : std::string("the host");
        if (client && g_was_connected && !st.connected) {
            say(st.turned_away ? host_name + " kicked you from the game"
                               : "Lost the connection to " + host_name + ". Reconnecting...");
        } else if (client && !g_was_connected && st.connected && !g_known.empty()) {
            say("Reconnected to " + (seen.count(0) ? seen[0].second : host_name));
        } else if (st.connected || !client) {
            for (const auto& [id, who] : seen) {
                if (id == 0 && client) continue;  // the host: the connection lines above
                const auto was = g_known.find(id);
                if (was == g_known.end() || was->second.first != who.first) say(who.second + " joined the game");
            }
            for (const auto& [id, who] : g_known) {
                if (id == 0 && client) continue;
                const auto is = seen.find(id);
                if (is == seen.end() || is->second.first != who.first) say(who.second + " left the game");
            }
        }
    }
    // A friend's lost link keeps who was there, so coming back isn't "everyone joined".
    if (!client || st.connected || !role) g_known = std::move(seen);
    g_was_connected = st.connected;
    g_was_in_game = in_game;
    g_was_client = client;
}

}  // namespace coop_notices
