#include "net_runtime.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "log.h"

namespace netrt {
namespace {

// The session and its thread. Never destroyed at exit: the game exits without unloading us, and tearing the
// socket down under a running thread would crash. stop() (from the co-op menu) shuts them down cleanly instead.
net::Session* g_session = nullptr;
net::Config g_config;  // what g_session was started with
std::atomic<bool> g_running{false};
HANDLE g_thread_done = nullptr;  // signalled when the network thread has left its loop
bool g_keep_offline = false;     // what keep_offline() last told the session

}  // namespace

bool start(const net::Config& config) {
    if (g_session) return true;
    auto* session = new net::Session([](const std::string& line) { logx::info("net: %s", line.c_str()); });
    if (!session->start(config)) {
        logx::error("net: could not start; co-op disabled this session");
        delete session;
        return false;
    }
    if (!g_thread_done) g_thread_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_thread_done);
    g_session = session;
    g_config = config;
    g_keep_offline = false;
    g_running = true;
    std::thread([session] {
        while (g_running) {
            session->update();
            session->wait(5);
        }
        SetEvent(g_thread_done);
    }).detach();
    return true;
}

void stop() {
    if (!g_session) return;
    g_running = false;
    if (WaitForSingleObject(g_thread_done, 2000) != WAIT_OBJECT_0) {
        logx::warn("net: network thread didn't stop in time; leaking the old session");
        g_session = nullptr;
        return;
    }
    g_session->stop();  // says goodbye to the peer
    delete g_session;
    g_session = nullptr;
    logx::info("net: stopped");
}

// Settings that need a new session (a new lobby, socket or handshake) when they change; the body doesn't.
bool same_link(const net::Config& a, const net::Config& b) {
    return a.role == b.role && a.transport == b.transport && a.bind_ip == b.bind_ip && a.host_ip == b.host_ip &&
           a.port == b.port && a.code == b.code && a.name == b.name && a.game_version == b.game_version;
}

bool restart(const net::Config& config) {
    if (g_session && same_link(g_config, config)) {
        // Keep the lobby and anyone already in it.
        if (config.body != g_config.body) g_session->set_body(config.body);
        g_config = config;
        logx::info("net: same connection settings; session kept");
        return true;
    }
    stop();
    return start(config);
}

bool active() { return g_session != nullptr; }

void set_body(const std::string& body) {
    g_config.body = body;
    if (g_session) g_session->set_body(body);
}

std::string body() { return g_config.body; }

void set_ready(bool ready) {
    if (g_session) g_session->set_ready(ready);
}

bool admit(uint32_t ticket) { return g_session && g_session->admit(ticket); }

bool turn_away(uint32_t ticket) { return g_session && g_session->turn_away(ticket); }

void set_local_state(const net::StateMsg& state) {
    if (g_session) g_session->set_local_state(state);
}

void cut_link(double ms) {
    if (g_session) g_session->cut_link(ms);
}

void set_world(const net::WorldMsg& world) {
    if (g_session) g_session->set_world(world);
}

std::optional<net::WorldMsg> remote_world() {
    return g_session ? g_session->remote_world() : std::nullopt;
}

void send_animals(std::vector<net::AnimalsMsg> parts) {
    if (g_session) g_session->send_animals(std::move(parts));
}

void drain_animals(std::vector<net::AnimalsMsg>& out) {
    if (g_session) g_session->drain_animals(out);
}

void send_animal_sounds(const std::vector<net::AnimalSound>& sounds) {
    if (g_session) g_session->send_animal_sounds(sounds);
}

void drain_animal_sounds(std::vector<net::AnimalSound>& out) {
    if (g_session) g_session->drain_animal_sounds(out);
}

void send_attack(const net::AttackMsg& attack) {
    if (g_session) g_session->send_attack(attack);
}

void drain_attacks(std::vector<net::AttackMsg>& out) {
    if (g_session) g_session->drain_attacks(out);
}

void send_attack_result(const net::AttackResultMsg& result) {
    if (g_session) g_session->send_attack_result(result);
}

void drain_attack_results(std::vector<net::AttackResultMsg>& out) {
    if (g_session) g_session->drain_attack_results(out);
}

void send_packet(const net::Writer& w, bool reliable) {
    if (g_session) g_session->send_packet(w, reliable);
}

void send_packet_to(uint8_t player, const net::Writer& w, bool reliable) {
    if (g_session) g_session->send_packet_to(player, w, reliable);
}

void send_packet_except(uint8_t player, const net::Writer& w, bool reliable) {
    if (g_session) g_session->send_packet_except(player, w, reliable);
}

void keep_offline(bool on) {
    if (!g_session || on == g_keep_offline) return;
    g_keep_offline = on;
    g_session->keep_offline(on);
}

bool offline() {
    if (!g_session || g_session->status().connected) return false;
    return g_keep_offline || g_config.role == net::Role::Host;
}

// Several modules each take their own message types out of one session inbox (main thread only).
std::vector<net::Packet> g_packets;

void drain_packets(net::Msg type, std::vector<net::Packet>& out) {
    if (g_session) g_session->drain_packets(g_packets);
    for (auto it = g_packets.begin(); it != g_packets.end();) {
        if (it->type == type) {
            out.push_back(std::move(*it));
            it = g_packets.erase(it);
        } else {
            ++it;
        }
    }
    if (g_packets.size() > 2048) g_packets.erase(g_packets.begin(), g_packets.begin() + 1024);  // nobody wants these
}

std::optional<net::Role> role() {
    return g_session ? std::optional<net::Role>(g_config.role) : std::nullopt;
}

std::optional<net::StateMsg> remote_state() {
    return g_session ? g_session->remote_state() : std::nullopt;
}

std::optional<net::StateMsg> remote_state(uint8_t player) {
    return g_session ? g_session->remote_state(player) : std::nullopt;
}

std::vector<net::StateMsg> remote_states() {
    return g_session ? g_session->remote_states() : std::vector<net::StateMsg>{};
}

net::Status status() { return g_session ? g_session->status() : net::Status{}; }

void drain_remote_states(std::vector<net::TimedState>& out) {
    if (g_session) g_session->drain_remote_states(out);
}

std::vector<uint8_t> Together::arrived(const std::string& scene) {
    std::array<uint32_t, net::kMaxPlayers> now{};
    if (g_session && !scene.empty() && g_config.role == net::Role::Host) {
        for (const net::PlayerInfo& p : g_session->status().players) {
            if (!p.linked || !p.caught_up || p.id == 0 || p.id >= net::kMaxPlayers) continue;
            const auto r = g_session->remote_state(p.id);
            if (r && r->valid && r->scene == scene) now[p.id] = p.link;
        }
    }
    std::vector<uint8_t> out;
    for (uint8_t id = 1; id < net::kMaxPlayers; ++id)
        if (now[id] && now[id] != link_[id]) out.push_back(id);
    link_ = now;
    return out;
}

bool Newest::take(const net::Packet& p, const std::string& key, uint32_t seq) {
    if (p.from >= net::kMaxPlayers) return false;
    auto& seen = seen_[p.from];
    if (link_[p.from] != p.link) {
        seen.clear();
        link_[p.from] = p.link;
    }
    const auto last = seen.find(key);
    if (last != seen.end() && static_cast<int32_t>(seq - last->second) <= 0) return false;
    seen[key] = seq;
    return true;
}

void Newest::clear() {
    for (auto& s : seen_) s.clear();
    link_.fill(0);
}

bool Newest::empty() const {
    for (const auto& s : seen_)
        if (!s.empty()) return false;
    return true;
}

}  // namespace netrt
