#pragma once
// A UDP (or Steam) session: one host and up to 3 friends, each friend linked to the host only. Shared by the mod and
// the test bot so both always speak the same protocol.
//   host:   accepts friends (player ids 1..3), sends them its state, its world, animals..., and passes on to each what
//           the others are (Roster) and where (their States). World changes the friends make reach the host; the
//           host's world modules pass them on to the others as their own (see world_items).
//   client: talks to the host only; the other friends come to it through the host.
//
// Threading: update(), wait() and stop() belong to one thread (the network thread). Everything else is safe from any
// thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/protocol.h"
#include "net/transport.h"

namespace net {

enum class Role { Host, Client };

struct Config {
    Role role = Role::Host;
    std::string transport = "udp";      // "udp" (IP:port) or "steam" (join code + Steam P2P)
    std::string bind_ip = "127.0.0.1";  // UDP: local interface; "0.0.0.0" for LAN play
    std::string host_ip = "127.0.0.1";  // UDP client: where the host listens
    uint16_t port = 27015;              // UDP host: listen port; UDP client: the host's port
    std::string code;                   // Steam: the host's code, or the code to join
    std::string name = "Player";
    std::string body = "capsule";  // how the others see us; an id such as "mackenzie"
    std::string game_version;      // must match exactly on both sides
    bool ask_to_join = false;      // host: a new friend waits until admit() (or turn_away()); one let in before comes
                                   // back by itself after a dropped link, one turned away can't for this session
};

// Someone else in the game.
struct PlayerInfo {
    uint8_t id = 0;           // 0 the host, 1..3 the friends (in the order they came)
    std::string name;
    std::string body;         // "" until their profile arrives
    bool linked = false;      // we talk to them directly (host: every friend; client: the host)
    uint32_t link = 0;        // which connection (a new one each time they link; see Packet.link). Client: the
                              // host's own for the other friends (another one in the same id is someone new)
    bool ready = false;       // in the lobby: READY (a friend), START pressed (the host); or playing
    double rtt_ms = -1;       // linked: smoothed round trip; -1 until the first pong
    bool caught_up = false;   // host: this friend's world changes from while the link was down are in (its CaughtUp
                              // went through drain_packets); our lists of the world go to it only after that
};

struct Status {
    bool connected = false;  // client: linked to the host; host: at least one friend is linked
    bool rejected = false;
    uint8_t self_id = 0;     // our player id (host 0; a client's comes in the host's Welcome)
    std::vector<PlayerInfo> players;  // everyone else in the game, by id
    // The other end for two players (client: the host; host: the first friend), as before 3-4 players.
    std::string peer_name;
    std::string peer_body;
    double rtt_ms = -1;
    uint64_t states_sent = 0;
    uint64_t states_received = 0;
    std::string link;  // the transport's own status, e.g. "hosting; your code is K7QX2B"
    bool caught_up = false;  // host: every linked friend is caught up (see PlayerInfo)
    // Host (ask_to_join): friends asking to join, oldest first (link: their ticket for admit / turn_away; id 0).
    std::vector<PlayerInfo> asking;
    bool waiting = false;      // client: the host has us waiting to be let in (peer_name: the host)
    bool turned_away = false;  // client: the host didn't let us in, or kicked us (rejected is set too)
};

// A player's state plus when it arrived (now_ms clock), for interpolation.
struct TimedState {
    StateMsg state;  // state.player: whose
    double received_ms = 0;
};

// Monotonic milliseconds.
double now_ms();

class Session {
public:
    explicit Session(LogFn log);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool start(const Config& config);
    void update();               // receive everything pending, then handshake, timeouts and periodic sends
    void wait(int timeout_ms);   // sleep until a packet arrives or the timeout passes
    void stop();                 // say goodbye and close the socket

    void set_local_state(const StateMsg& state);  // what we send at 20 Hz (seq is assigned here)
    void set_body(const std::string& body);       // change our body; the others are told right away
    void set_ready(bool ready);  // the lobby: READY (client) / START pressed (host), or playing; told right away
    // Host: let in a friend asking to join (Status.asking's link). False if unknown or the game is full.
    bool admit(uint32_t ticket);
    // Host: turn away a friend asking to join, or kick one in the game (PlayerInfo.link). They can't come back while
    // this session runs. False if unknown.
    bool turn_away(uint32_t ticket);
    void set_world(const WorldMsg& world);        // host: our world, sent about once a second (seq set here)
    void send_animals(std::vector<AnimalsMsg> parts);  // host: one frame of our animals, sent on the next tick
    void send_animal_sounds(const std::vector<AnimalSound>& sounds);  // host: queued, sent on the next tick
    // Client: the host's newest state; host: the first friend's (two players). Any player: remote_state(id).
    std::optional<StateMsg> remote_state() const;
    std::optional<StateMsg> remote_state(uint8_t player) const;
    std::vector<StateMsg> remote_states() const;  // everyone else's newest (state.player says whose)
    std::optional<WorldMsg> remote_world() const;  // client: the host's newest world, if connected
    void drain_animals(std::vector<AnimalsMsg>& out);  // client: animal parts received since the last call
    void drain_animal_sounds(std::vector<AnimalSound>& out);  // client: sounds received since the last call
    void send_attack(const AttackMsg& attack);                // host: to friend attack.player; on the next tick
    void drain_attacks(std::vector<AttackMsg>& out);          // client
    void send_attack_result(const AttackResultMsg& result);   // client: queued, sent on the next tick
    void drain_attack_results(std::vector<AttackResultMsg>& out);  // host (result.player: whose fight)
    // The newer message types (Hit, Thrown, Carcasses, Harvest, Quartered, Item*...): sent on the next tick, through
    // the Reliable channel (resent until acked, handled once; up to kMaxMessage, split into Chunks when needed) or
    // once like the 10 Hz states (one datagram at most). Reliable messages can arrive out of order.
    // send_packet: client -> the host; host -> every friend. send_packet_to: host -> one friend. send_packet_except:
    // host -> every friend but one (passing on what that one did).
    void send_packet(const Writer& w, bool reliable);
    void send_packet_to(uint8_t player, const Writer& w, bool reliable);
    void send_packet_except(uint8_t player, const Writer& w, bool reliable);
    void drain_packets(std::vector<Packet>& out);  // received since the last call, oldest first (Packet.from: whose)
    // Client: while on, the world changes sent while the link is down (ItemPut, ItemGone, ContainerPut, ObjectPut,
    // FirePut, Harvest, Quartered) are kept and go first once it's back, then CaughtUp when they're all acked. Off
    // drops what was kept.
    void keep_offline(bool on);
    Status status() const;
    void cut_link(double ms);  // test: nothing goes in or out for `ms` (a dropped connection; both sides time out)

    // Moves every player state received since the last call into `out` (oldest first). Keeps at most the last 64
    // per player if nobody drains (e.g. the bot).
    void drain_remote_states(std::vector<TimedState>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
