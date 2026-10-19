#pragma once
// The mod's network thread. It keeps the session alive (pings, timeouts, 20 Hz sends) even while the game's
// main loop is paused, e.g. during loading screens. Main thread only for start/stop/restart.

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/session.h"

namespace netrt {

bool start(const net::Config& config);
void stop();                              // says goodbye to the peer and ends the thread
bool restart(const net::Config& config);  // e.g. host -> join; keeps the session if only the body changed
bool active();

void set_body(const std::string& body);  // tells the peer right away
std::string body();                      // ours, as the others see us
void set_ready(bool ready);              // READY in the lobby / START pressed, or playing (Session::set_ready)
bool admit(uint32_t ticket);             // host: let a friend asking to join in (Status.asking's link)
bool turn_away(uint32_t ticket);         // host: turn a friend asking away, or kick one in the game (PlayerInfo.link)
void cut_link(double ms);                 // test: drop everything in and out for `ms` (both sides time out)
void set_local_state(const net::StateMsg& state);
void set_world(const net::WorldMsg& world);     // host
std::optional<net::WorldMsg> remote_world();    // client
std::optional<net::Role> role();                // while a session runs
void send_animals(std::vector<net::AnimalsMsg> parts);  // host
void drain_animals(std::vector<net::AnimalsMsg>& out);  // client
void send_animal_sounds(const std::vector<net::AnimalSound>& sounds);  // host
void drain_animal_sounds(std::vector<net::AnimalSound>& out);         // client
void send_attack(const net::AttackMsg& attack);                       // host
void drain_attacks(std::vector<net::AttackMsg>& out);                 // client
void send_attack_result(const net::AttackResultMsg& result);          // client
void drain_attack_results(std::vector<net::AttackResultMsg>& out);    // host
void send_packet(const net::Writer& w, bool reliable);              // client -> host; host -> every friend
void send_packet_to(uint8_t player, const net::Writer& w, bool reliable);      // host -> one friend
void send_packet_except(uint8_t player, const net::Writer& w, bool reliable);  // host -> every friend but one
void drain_packets(net::Msg type, std::vector<net::Packet>& out);   // received packets of one type (main thread)
// Client: a link that drops mid-game. While on, the world modules keep going and what they change goes to the host
// once the link is back (Session::keep_offline); core turns it on in the game once linked, off at the main menu.
void keep_offline(bool on);
// The world modules go on without a link: the client's (keep_offline) changes go to the host once it's back; the host
// (nobody linked, e.g. playing on alone between sessions) applies what it kept for a scene it walks into, and its
// scene lists bring the friend up to date when they meet again (what it sends meanwhile is dropped).
bool offline();
std::optional<net::StateMsg> remote_state();  // client: the host's; host: the first friend's
std::optional<net::StateMsg> remote_state(uint8_t player);
std::vector<net::StateMsg> remote_states();    // everyone else's newest (state.player: whose)
net::Status status();
void drain_remote_states(std::vector<net::TimedState>& out);

// Host: friends with us in our scene, ready for our lists of it (linked, in game there, their changes from while the
// link was down in). arrived() gives the ones who weren't last time; one who linked again since counts again.
class Together {
public:
    std::vector<uint8_t> arrived(const std::string& scene);
    void clear() { link_.fill(0); }

private:
    std::array<uint32_t, net::kMaxPlayers> link_{};  // the link of each friend with us; 0: not with us
};

// The newest change applied per thing, per sender, so a late older one is dropped. Each player counts its own changes;
// a new link starts over (their game may have restarted).
class Newest {
public:
    bool take(const net::Packet& p, const std::string& key, uint32_t seq);  // newer: remembered, true
    void clear();
    bool empty() const;

private:
    std::array<std::unordered_map<std::string, uint32_t>, net::kMaxPlayers> seen_;
    std::array<uint32_t, net::kMaxPlayers> link_{};
};

}  // namespace netrt
