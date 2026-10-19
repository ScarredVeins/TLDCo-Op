#include "net/session.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace net {

double now_ms() {
    static const double ticks_per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / ticks_per_ms;
}

namespace {

constexpr double kHelloIntervalMs = 500;
constexpr double kStateIntervalMs = 50;  // 20 Hz
constexpr double kPingIntervalMs = 1000;
constexpr double kTimeoutMs = 5000;
constexpr double kProfileIntervalMs = 2000;  // UDP can drop it; repeating is simpler than acks
constexpr double kRosterIntervalMs = 2000;   // host: who's in the game, likewise
constexpr double kBusyRetryMs = 1000;        // host full: ask again shortly (someone may leave)
constexpr double kWorldIntervalMs = 1000;    // the host's world, repeated (UDP can drop it)
constexpr double kWorldChangeMs = 200;       // ...and sooner when the weather or clock jumps
constexpr size_t kInboxLimit = 64;           // undrained states per player
constexpr double kReliableResendMs = 250;   // a Reliable not acked yet goes again this often...
constexpr double kReliableGiveUpMs = 15000; // ...until the peer answers or this long has passed
constexpr size_t kReliableSeen = 512;       // recent Reliable seqs remembered, so resends aren't handled twice
constexpr size_t kReliableWindow = 64;      // Reliables in flight at once; the rest wait their turn
constexpr size_t kReliableQueue = 4096;     // queued Reliables (a big message is many Chunks)
constexpr size_t kReliableHeader = 7 + 4 + 2;               // Reliable: header, seq, blob length
constexpr size_t kChunkHeader = 7 + 4 + 2 + 2 + 2;          // Chunk: header, transfer, index, count, blob length
constexpr size_t kChunkPart = kMaxPacket - kReliableHeader - kChunkHeader;
constexpr double kChunkGiveUpMs = 60000;    // a message whose parts stopped coming is dropped
constexpr size_t kOutboxMax = 1024;                 // client: world changes kept while the link is down...
constexpr size_t kOutboxBytes = 2 * 1024 * 1024;    // ...and their bytes (they go out as Reliables: kReliableQueue)
constexpr size_t kMaxFriends = kMaxPlayers - 1;
constexpr size_t kMaxAsking = 8;  // host: friends waiting to be let in at once

const char* reject_text(RejectReason reason) {
    switch (reason) {
        case RejectReason::Protocol: return "mod protocol version differs";
        case RejectReason::GameVersion: return "game version differs";
        case RejectReason::Full: return "the game is full (4 players)";
        case RejectReason::TurnedAway: return "the host turned us away";
    }
    return "unknown reason";
}

uint32_t make_nonce() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t v = static_cast<uint64_t>(c.QuadPart) * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
    return static_cast<uint32_t>(v ^ (v >> 32)) | 1;
}

}  // namespace

struct Session::Impl {
    LogFn log;
    Config cfg;
    std::unique_ptr<Transport> link;

    struct Pending {
        uint32_t seq = 0;
        std::vector<uint8_t> data;  // the whole Reliable datagram
        double first_ms = 0, last_ms = -1e18;
    };
    struct Assembly {  // a big message of theirs, part by part
        std::vector<std::string> parts;
        size_t have = 0, bytes = 0;
        double first_ms = 0;
    };

    // A game we talk to directly: the host (client side) or one friend (host side).
    struct Link {
        PeerId addr = 0;
        uint8_t id = 0;         // their player id: 0 for the host, 1..3 for a friend
        uint32_t nonce = 0;     // host side: the friend's session (from its Hello)
        bool connected = false;
        bool gone = false;      // host side: disconnected; removed at the end of the update
        std::string name, body;
        double rtt_ms = -1;
        double last_recv = 0, last_ping = 0;
        StateMsg remote;        // their own newest state
        bool have_remote = false;
        std::vector<Pending> reliable_out;  // sent (or about to be), waiting for their Ack
        uint32_t reliable_seq = 0;
        uint32_t chunk_transfer = 0;        // our big messages to them, numbered
        std::vector<std::pair<uint32_t, Assembly>> assemblies;
        std::vector<uint32_t> reliable_seen;  // ring of their recent Reliable seqs
        size_t reliable_seen_next = 0;
        std::vector<std::vector<uint8_t>> unreliable_out;  // newer message types, sent once on the next tick
        bool caught_up = false;  // host side: their CaughtUp went through drain_packets
        uint32_t serial = 0;     // which connection this is (PlayerInfo.link, Packet.link)
        bool ready = false;      // host side: the friend's READY (its Profile); client side: the host's START
    };

    // Client: the other friends, as the host tells us (Roster) and passes on their states.
    struct Other {
        uint32_t link = 0;  // the host's serial for their connection
        bool ready = false;
        std::string name, body;
        StateMsg state;
        bool have_state = false;
    };

    // Host (ask_to_join): a friend saying hello, not let in yet.
    struct Asking {
        PeerId addr = 0;
        uint32_t nonce = 0;
        uint32_t ticket = 0;  // from `serials`, like a link's serial
        std::string name;
        double last_hello = 0;
    };

    mutable std::mutex mu;  // guards everything below
    std::vector<Link> links;  // client: [0] the host, once the transport knows it; host: the friends
    std::vector<Asking> asking;           // host
    std::vector<PeerId> let_in, kept_out;  // host: addresses admitted / turned away while this session runs
    double last_wait = -1e18;              // client: the host's last Wait (we're waiting to be let in)
    bool turned_away = false;              // client
    std::map<uint8_t, Other> others;  // client
    bool rejected = false;
    uint8_t self_id = 0;
    uint32_t nonce = 0;  // client: this connection's (new each time we start connecting), in every Hello
    bool ready = false;  // ours (Session::set_ready)
    uint32_t serials = 0;  // connections so far
    std::string link_status;
    uint64_t states_sent = 0, states_received = 0;
    StateMsg local;
    std::vector<TimedState> inbox;  // undrained player states, oldest first
    WorldMsg local_world;           // host
    bool have_local_world = false, world_changed = false;
    uint32_t world_seq = 0;
    WorldMsg remote_world;          // client
    bool have_remote_world = false;
    std::vector<AnimalsMsg> animals_out;  // host: the newest frame, not sent yet
    std::vector<AnimalsMsg> animals_in;   // client: undrained parts, oldest first
    std::vector<AnimalSound> sounds_out;  // host: not sent yet
    std::vector<AnimalSound> sounds_in;   // client: undrained
    std::vector<AttackMsg> attacks_in;          // client: from the host
    std::vector<AttackResultMsg> results_in;    // host: from the friends
    std::vector<Packet> packets_in;             // newer message types, for the game thread
    double cut_until = 0;                 // test (cut_link): until then nothing goes in or out
    uint32_t send_seq = 0;
    double last_hello = -1e18, last_state = 0, last_profile = -1e18, last_roster = -1e18;
    double last_world = -1e18;
    bool roster_changed = false;   // host: someone came, went or changed body
    bool reported_busy = false;
    bool keep_offline = false;                 // client: world changes made while the link is down are kept...
    std::vector<std::vector<uint8_t>> outbox;  // ...here, oldest first, and go first once it's back
    size_t outbox_bytes = 0;
    bool outbox_full = false;
    struct Unconfirmed {                       // client: a world change sent but not acked yet
        uint32_t first_seq = 0, last_seq = 0;  // its Reliable (or its Chunks)
        std::vector<uint8_t> msg;
    };
    std::deque<Unconfirmed> unconfirmed;       // oldest first; back into the outbox if the link drops
    bool caught_up_due = false;                // client: CaughtUp goes once every Reliable up to caught_up_seq is acked
    uint32_t caught_up_seq = 0, caught_up_count = 0;

    bool hosting() const { return cfg.role == Role::Host; }

    void logf(const char* fmt, ...) {
        char line[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (log) log(line);
    }

    std::string who(PeerId p) const { return link ? link->describe(p) : "?"; }

    // --- links -----------------------------------------------------------------------------------------------------

    Link* find_link(PeerId addr) {
        for (Link& l : links)
            if (!l.gone && l.addr == addr) return &l;
        return nullptr;
    }
    Link* link_by_id(uint8_t id) {
        for (Link& l : links)
            if (!l.gone && l.connected && l.id == id) return &l;
        return nullptr;
    }
    const Link* link_by_id(uint8_t id) const { return const_cast<Impl*>(this)->link_by_id(id); }
    Link* host_link() { return !hosting() && !links.empty() ? &links[0] : nullptr; }
    const Link* first_connected() const {
        for (const Link& l : links)
            if (!l.gone && l.connected) return &l;
        return nullptr;
    }
    bool any_connected() const { return first_connected() != nullptr; }
    size_t friends() const {
        return static_cast<size_t>(std::count_if(links.begin(), links.end(), [](const Link& l) { return !l.gone; }));
    }
    uint8_t free_id() const {
        for (uint8_t id = 1; id <= kMaxFriends; ++id)
            if (std::none_of(links.begin(), links.end(), [&](const Link& l) { return !l.gone && l.id == id; })) return id;
        return 0;
    }

    void send(PeerId to, const Writer& w) {
        if (w.ok() && link && now_ms() >= cut_until) link->send(to, w.data(), w.size());
    }

    void send_hello(const Link& host) {
        Writer w(Msg::Hello);
        w.put_str(cfg.game_version, kVersionLen);
        w.put_str(cfg.name, kNameLen);
        w.put(nonce);
        send(host.addr, w);
    }

    void send_reject(PeerId to, RejectReason reason, const std::string& detail) {
        Writer w(Msg::Reject);
        w.put(static_cast<uint8_t>(reason));
        w.put_str(detail, kDetailLen);
        send(to, w);
        const char* why = reason == RejectReason::TurnedAway ? "turned away" : reject_text(reason);  // ours is the host's side
        logf("rejected %s: %s%s%s%s", who(to).c_str(), why, detail.empty() ? "" : " (", detail.c_str(), detail.empty() ? "" : ")");
    }

    static bool has(const std::vector<PeerId>& list, PeerId p) {
        return std::find(list.begin(), list.end(), p) != list.end();
    }

    // A friend's new link (once let in, if we ask): its Welcome follows.
    Link& add_link(PeerId addr, uint32_t their_nonce) {
        links.emplace_back();
        Link& l = links.back();
        l.addr = addr;
        l.id = free_id();
        l.nonce = their_nonce;
        return l;
    }

    void send_welcome(const Link& l) {
        Writer w(Msg::Welcome);
        w.put_str(cfg.name, kNameLen);
        w.put(l.id);
        send(l.addr, w);
    }

    // Host: a friend we haven't let in says hello; they wait (and keep saying hello) until admit / turn_away.
    void ask(PeerId from, const std::string& name, uint32_t their_nonce, double now) {
        auto it = std::find_if(asking.begin(), asking.end(), [&](const Asking& a) { return a.addr == from; });
        if (it == asking.end()) {
            if (asking.size() >= kMaxAsking) return;  // they keep trying; a place frees up when one is decided
            asking.push_back(Asking{from, their_nonce, ++serials, name, now});
            it = asking.end() - 1;
            logf("'%s' at %s asks to join; waiting for the host to let them in", name.c_str(), who(from).c_str());
        }
        it->nonce = their_nonce;
        it->name = name;
        it->last_hello = now;
        Writer w(Msg::Wait);
        w.put_str(cfg.name, kNameLen);
        send(from, w);
    }

    bool admit(uint32_t ticket, double now) {
        const auto it = std::find_if(asking.begin(), asking.end(), [&](const Asking& a) { return a.ticket == ticket; });
        if (it == asking.end()) return false;
        if (friends() >= kMaxFriends) {
            logf("can't let '%s' in: the game is full (4 players)", it->name.c_str());
            return false;
        }
        const Asking a = *it;
        asking.erase(it);
        if (!has(let_in, a.addr)) let_in.push_back(a.addr);
        logf("let '%s' in", a.name.c_str());
        Link& l = add_link(a.addr, a.nonce);
        on_connected(l, a.name, now);
        send_welcome(l);
        return true;
    }

    bool turn_away(uint32_t ticket) {
        const auto it = std::find_if(asking.begin(), asking.end(), [&](const Asking& a) { return a.ticket == ticket; });
        PeerId addr = 0;
        std::string name;
        if (it != asking.end()) {
            addr = it->addr;
            name = it->name;
            asking.erase(it);
            send_reject(addr, RejectReason::TurnedAway, "");
        } else {
            const auto l = std::find_if(links.begin(), links.end(),
                                        [&](const Link& x) { return !x.gone && x.connected && x.serial == ticket; });
            if (l == links.end()) return false;
            addr = l->addr;
            name = l->name;
            send_reject(addr, RejectReason::TurnedAway, "kicked");
            on_disconnected(*l, "kicked by the host");
        }
        if (!has(kept_out, addr)) kept_out.push_back(addr);
        let_in.erase(std::remove(let_in.begin(), let_in.end(), addr), let_in.end());
        logf("turned '%s' away; they can't come back while this session runs", name.c_str());
        return true;
    }

    void on_connected(Link& l, const std::string& name, double now) {
        l.connected = true;
        l.name = name;
        l.body.clear();
        l.rtt_ms = -1;
        l.have_remote = false;
        l.last_recv = now;
        l.last_ping = now - kPingIntervalMs;  // ping right away
        l.caught_up = false;
        l.serial = ++serials;
        reported_busy = false;
        last_profile = -1e18;  // client: introduce ourselves
        last_world = -1e18;    // host: show our world
        if (hosting()) {
            roster_changed = true;
            logf("connected: '%s' at %s (player %u; %zu friend(s) in the game)", name.c_str(), who(l.addr).c_str(), l.id,
                 friends());
            return;
        }
        logf("connected: '%s' at %s (we're player %u)", name.c_str(), who(l.addr).c_str(), self_id);
        // What we changed in the world while the link was down goes first; CaughtUp follows once it's all in.
        std::vector<std::vector<uint8_t>> kept;
        kept.swap(outbox);
        outbox_bytes = 0;
        outbox_full = false;
        for (const auto& m : kept) queue_world_change(l, m.data(), m.size());
        if (!kept.empty()) logf("sent the %zu world change(s) we made while the link was down", kept.size());
        caught_up_count = static_cast<uint32_t>(kept.size());
        caught_up_seq = l.reliable_seq;
        caught_up_due = true;
    }

    void on_disconnected(Link& l, const char* why) {
        logf("disconnected from '%s': %s", l.name.c_str(), why);
        if (!hosting()) {
            size_t again = 0;
            for (const Unconfirmed& u : unconfirmed) {
                if (!pending(l, u.first_seq, u.last_seq)) continue;
                keep_for_later(u.msg.data(), u.msg.size());
                ++again;
            }
            if (again) logf("link down: %zu world change(s) of ours weren't confirmed yet; they go again when it's back", again);
            unconfirmed.clear();
            caught_up_due = false;
            have_remote_world = false;
            others.clear();  // the friends come to us through the host
            nonce = make_nonce();  // a new connection: the host drops what it has of this one when we say hello
        }
        const PeerId addr = l.addr;
        const uint8_t id = l.id;
        l = Link{};
        l.addr = addr;  // client: the host stays where it is (we say hello again)
        l.id = id;
        if (hosting()) {
            l.gone = true;
            roster_changed = true;
        }
    }

    void drop_gone() {
        links.erase(std::remove_if(links.begin(), links.end(), [](const Link& l) { return l.gone; }), links.end());
    }

    // --- the reliable channel (per link) ---------------------------------------------------------------------------

    static bool world_change(const uint8_t* data, size_t size) {
        if (size < 7) return false;
        switch (static_cast<Msg>(data[6])) {
            case Msg::ItemPut:
            case Msg::ItemGone:
            case Msg::ContainerPut:
            case Msg::ObjectPut:
            case Msg::FirePut:
            case Msg::Harvest:     // a carcass harvest (the host counts each once: net::HarvestLedger)
            case Msg::Quartered: return true;
            default: return false;  // questions (ItemWant, ContainerWant) are asked again once the link is back
        }
    }

    // Client, link down: a world change waits in the outbox (keep_offline).
    void keep_for_later(const uint8_t* data, size_t size) {
        if (!keep_offline || hosting() || !world_change(data, size)) return;
        if (outbox.size() >= kOutboxMax || outbox_bytes + size > kOutboxBytes) {
            if (!outbox_full) logf("link down: too many world changes to keep; later ones won't reach the host");
            outbox_full = true;
            return;
        }
        if (outbox.empty()) logf("link down: keeping our world changes for when it's back");
        outbox.emplace_back(data, data + size);
        outbox_bytes += size;
    }

    // Some Reliable from first to last (serial numbers) isn't acked yet.
    static bool pending(const Link& l, uint32_t first, uint32_t last) {
        return std::any_of(l.reliable_out.begin(), l.reliable_out.end(), [&](const Pending& p) {
            return static_cast<int32_t>(p.seq - first) >= 0 && static_cast<int32_t>(last - p.seq) >= 0;
        });
    }

    // Queues a world change of ours. The client keeps a copy until the host acks it: a link that drops first would
    // lose it (on_disconnected), so it goes again once the link is back (twice is harmless: "already so" there).
    void queue_world_change(Link& l, const uint8_t* data, size_t size) {
        const uint32_t first = l.reliable_seq + 1;
        queue_reliable_raw(l, data, size);
        if (hosting() || !keep_offline || static_cast<int32_t>(l.reliable_seq - first) < 0) return;
        unconfirmed.push_back(Unconfirmed{first, l.reliable_seq, std::vector<uint8_t>(data, data + size)});
        while (!unconfirmed.empty() && !pending(l, unconfirmed.front().first_seq, unconfirmed.front().last_seq))
            unconfirmed.pop_front();
    }

    // Queues a message to go through the Reliable channel: numbered, resent until they ack it. A message too big for
    // one datagram goes as Chunks, each one Reliable.
    void queue_reliable(Link& l, const Writer& inner) {
        if (!l.connected || !inner.ok()) return;
        queue_reliable_raw(l, inner.data(), inner.size());
    }

    void queue_reliable_raw(Link& l, const uint8_t* data, size_t size) {
        if (size + kReliableHeader > kMaxPacket) {
            queue_chunks(l, data, size);
            return;
        }
        queue_reliable_bytes(l, data, size);
    }

    void queue_reliable_bytes(Link& l, const uint8_t* data, size_t size) {
        if (l.reliable_out.size() >= kReliableQueue) {
            logf("reliable queue to '%s' full; dropped a message (type %u)", l.name.c_str(), size > 6 ? data[6] : 0);
            return;
        }
        Writer w(Msg::Reliable);
        w.put(++l.reliable_seq);
        w.put_blob(std::string(reinterpret_cast<const char*>(data), size), kMaxPacket);
        if (!w.ok()) return;
        Pending p;
        p.seq = l.reliable_seq;
        p.data.assign(w.data(), w.data() + w.size());
        l.reliable_out.push_back(std::move(p));
    }

    void queue_chunks(Link& l, const uint8_t* data, size_t size) {
        const size_t count = (size + kChunkPart - 1) / kChunkPart;
        if (size > kMaxMessage || count > 0xFFFF || l.reliable_out.size() + count > kReliableQueue) {
            logf("message too big or queue full: dropped %zu bytes (type %u)", size, data[6]);
            return;
        }
        const uint32_t transfer = ++l.chunk_transfer;
        for (size_t i = 0; i < count; ++i) {
            const size_t at = i * kChunkPart, n = std::min(kChunkPart, size - at);
            Writer w(Msg::Chunk);
            w.put(transfer);
            w.put(static_cast<uint16_t>(i));
            w.put(static_cast<uint16_t>(count));
            w.put_blob(std::string(reinterpret_cast<const char*>(data + at), n), kChunkPart);
            queue_reliable_bytes(l, w.data(), w.size());
        }
    }

    // One of our messages to one link: reliable (a world change of a client's is kept until acked) or once.
    void queue(Link& l, const Writer& w, bool reliable) {
        if (!l.connected || !w.ok()) return;
        if (reliable && !hosting() && world_change(w.data(), w.size()))
            queue_world_change(l, w.data(), w.size());
        else if (reliable)
            queue_reliable(l, w);
        else if (l.unreliable_out.size() < 256 && w.size() <= kMaxPacket)
            l.unreliable_out.emplace_back(w.data(), w.data() + w.size());
    }

    // One part of a big message of theirs; once all are in, the message is handled like any other.
    void receive_chunk(PeerId from, Link& l, Reader& r, double now) {
        const auto transfer = r.get<uint32_t>();
        const auto index = r.get<uint16_t>();
        const auto count = r.get<uint16_t>();
        std::string part = r.get_blob(kChunkPart);
        if (!r.ok() || count == 0 || index >= count || static_cast<size_t>(count) * kChunkPart > kMaxMessage + kChunkPart)
            return;
        auto& assemblies = l.assemblies;
        assemblies.erase(std::remove_if(assemblies.begin(), assemblies.end(),
                                        [&](const auto& a) { return now - a.second.first_ms > kChunkGiveUpMs; }),
                         assemblies.end());
        auto it = std::find_if(assemblies.begin(), assemblies.end(), [&](const auto& a) { return a.first == transfer; });
        if (it == assemblies.end()) {
            if (assemblies.size() >= 64) assemblies.erase(assemblies.begin());  // the oldest never finished
            Assembly a;
            a.parts.resize(count);
            a.first_ms = now;
            assemblies.emplace_back(transfer, std::move(a));
            it = assemblies.end() - 1;
        }
        Assembly& a = it->second;
        if (a.parts.size() != count || !a.parts[index].empty() || part.empty()) return;
        a.bytes += part.size();
        a.parts[index] = std::move(part);
        if (++a.have < count) return;
        std::string whole;
        whole.reserve(a.bytes);
        for (const std::string& p : a.parts) whole += p;
        assemblies.erase(it);
        const auto* bytes = reinterpret_cast<const uint8_t*>(whole.data());
        if (whole.size() < 7) return;
        const auto inner = static_cast<Msg>(bytes[6]);
        if (inner == Msg::Reliable || inner == Msg::Chunk) return;  // no nesting
        handle(from, bytes, whole.size(), now);
    }

    // A message type the game thread parses itself (Hit, Carcasses, Harvest...).
    void keep_packet(Link& l, Msg type, const uint8_t* data, size_t size, double now) {
        if (packets_in.size() >= 1024 * kMaxFriends) packets_in.erase(packets_in.begin());
        Packet p;
        p.type = type;
        p.from = hosting() ? l.id : 0;
        p.link = l.serial;
        p.data.assign(data, data + size);
        packets_in.push_back(std::move(p));
        l.last_recv = now;
    }

    void keep_state(const StateMsg& s, double now) {
        size_t theirs = 0;
        for (const TimedState& t : inbox) theirs += t.state.player == s.player;
        if (theirs >= kInboxLimit) {
            const auto oldest = std::find_if(inbox.begin(), inbox.end(),
                                             [&](const TimedState& t) { return t.state.player == s.player; });
            inbox.erase(oldest);
        }
        inbox.push_back({s, now});
    }

    // --- receiving -------------------------------------------------------------------------------------------------

    void handle(PeerId from, const uint8_t* data, size_t size, double now) {
        Reader r(data, size);
        const auto magic = r.get<uint32_t>();
        const auto version = r.get<uint16_t>();
        const auto type = static_cast<Msg>(r.get<uint8_t>());
        if (!r.ok() || magic != kMagic) return;  // not ours (or a Steam session "knock")
        if (version != kProtocolVersion) {
            if (hosting() && type == Msg::Hello)
                send_reject(from, RejectReason::Protocol, "host speaks v" + std::to_string(kProtocolVersion));
            return;
        }
        Link* l = find_link(from);
        const bool linked = l && l->connected;

        switch (type) {
            case Msg::Hello: {
                if (!hosting()) return;
                const std::string game_version = r.get_str(kVersionLen);
                const std::string name = r.get_str(kNameLen);
                const auto their_nonce = r.get<uint32_t>();
                if (!r.ok()) return;
                if (game_version != cfg.game_version) {
                    send_reject(from, RejectReason::GameVersion, "host has " + cfg.game_version);
                    return;
                }
                if (has(kept_out, from)) {
                    send_reject(from, RejectReason::TurnedAway, "");
                    return;
                }
                if (linked && l->nonce != their_nonce) {  // they lost the link (or restarted): the old one is gone
                    on_disconnected(*l, "came back (a new connection)");
                    l = nullptr;
                }
                if (!l) {
                    if (cfg.ask_to_join && !has(let_in, from)) {
                        ask(from, name, their_nonce, now);
                        return;
                    }
                    if (friends() >= kMaxFriends) {
                        send_reject(from, RejectReason::Full, "");
                        return;
                    }
                    l = &add_link(from, their_nonce);
                }
                if (!l->connected) on_connected(*l, name, now);
                send_welcome(*l);  // also answers retried hellos
                l->last_recv = now;
                break;
            }
            case Msg::Welcome: {
                if (hosting() || !l) return;
                const std::string name = r.get_str(kNameLen);
                const auto id = r.get<uint8_t>();
                if (!r.ok()) return;
                if (!l->connected) {
                    self_id = id;
                    last_wait = -1e18;
                    on_connected(*l, name, now);
                }
                l->last_recv = now;
                break;
            }
            case Msg::Wait: {
                if (hosting() || !l || l->connected) return;
                const std::string name = r.get_str(kNameLen);
                if (!r.ok()) return;
                if (now - last_wait > kTimeoutMs) logf("asked '%s' to let us in; waiting for them", name.c_str());
                last_wait = now;
                l->name = name;
                break;
            }
            case Msg::Reject: {
                if (hosting() || !l) return;
                const auto reason = static_cast<RejectReason>(r.get<uint8_t>());
                const std::string detail = r.get_str(kDetailLen);
                if (l->connected && reason != RejectReason::TurnedAway) return;
                if (reason == RejectReason::TurnedAway) {
                    if (l->connected) on_disconnected(*l, "the host kicked us");
                    if (!turned_away) logf("the host turned us away; not retrying");
                    turned_away = true;
                    rejected = true;
                    last_wait = -1e18;
                    break;
                }
                if (reason == RejectReason::Full) {
                    // Four players already (one may be our own previous connection the host hasn't timed out yet).
                    if (!reported_busy) logf("host is busy (%s); retrying until it frees up", reject_text(reason));
                    reported_busy = true;
                    last_hello = now + kBusyRetryMs - kHelloIntervalMs;  // next hello in kBusyRetryMs
                    break;
                }
                rejected = true;
                logf("host refused us: %s (%s); not retrying", reject_text(reason), detail.c_str());
                break;
            }
            case Msg::Ping: {
                if (!linked) return;
                const auto t = r.get<double>();
                if (!r.ok()) return;
                Writer w(Msg::Pong);
                w.put(t);
                send(l->addr, w);
                l->last_recv = now;
                break;
            }
            case Msg::Pong: {
                if (!linked) return;
                const auto t = r.get<double>();
                if (!r.ok() || t > now) return;
                const double sample = now - t;
                l->rtt_ms = l->rtt_ms < 0 ? sample : l->rtt_ms * 0.8 + sample * 0.2;
                l->last_recv = now;
                break;
            }
            case Msg::State: {
                if (!linked) return;
                StateMsg s;
                if (!read_state(r, s)) return;
                ++states_received;
                l->last_recv = now;
                if (hosting()) {
                    s.player = l->id;
                    // Drop late or duplicate datagrams (serial-number comparison survives wrap-around).
                    if (l->have_remote && static_cast<int32_t>(s.seq - l->remote.seq) <= 0) return;
                    keep_state(s, now);
                    l->remote = s;
                    l->have_remote = true;
                    // The other friends see them too.
                    Writer w(Msg::State);
                    write_state(w, s);
                    for (Link& other : links)
                        if (&other != l && !other.gone && other.connected) send(other.addr, w);
                    return;
                }
                if (s.player == 0) {  // the host's own
                    if (l->have_remote && static_cast<int32_t>(s.seq - l->remote.seq) <= 0) return;
                    keep_state(s, now);
                    l->remote = std::move(s);
                    l->have_remote = true;
                    return;
                }
                if (s.player == self_id || s.player >= kMaxPlayers) return;
                Other& o = others[s.player];  // a friend the host passes on (the roster may come a moment later)
                if (o.have_state && static_cast<int32_t>(s.seq - o.state.seq) <= 0) return;
                keep_state(s, now);
                o.state = std::move(s);
                o.have_state = true;
                break;
            }
            case Msg::Bye: {
                if (linked) on_disconnected(*l, "left");
                break;
            }
            case Msg::Profile: {
                if (!hosting() || !linked) return;
                const std::string name = r.get_str(kNameLen);
                const std::string body = r.get_str(kBodyLen);
                const bool their_ready = r.get<uint8_t>() != 0;
                if (!r.ok()) return;
                if (body != l->body) logf("'%s' chose body '%s'", name.c_str(), body.c_str());
                if (their_ready != l->ready) logf("'%s' is %s", name.c_str(), their_ready ? "ready" : "not ready");
                if (body != l->body || name != l->name || their_ready != l->ready) roster_changed = true;
                l->name = name;
                l->body = body;
                l->ready = their_ready;
                l->last_recv = now;
                break;
            }
            case Msg::Roster: {
                if (hosting() || !linked) return;
                std::vector<RosterEntry> players;
                if (!read_roster(r, players)) return;
                l->last_recv = now;
                for (auto it = others.begin(); it != others.end();) {
                    // Gone, or someone new in their place (another connection: they came back, or a new friend).
                    const bool listed = std::any_of(players.begin(), players.end(), [&](const RosterEntry& p) {
                        return p.id == it->first && p.link == it->second.link;
                    });
                    if (!listed && !it->second.name.empty()) logf("'%s' left the game", it->second.name.c_str());
                    it = listed ? std::next(it) : others.erase(it);
                }
                for (const RosterEntry& p : players) {
                    if (p.id == 0) {  // the host
                        if (p.body != l->body) logf("'%s' chose body '%s'", p.name.c_str(), p.body.c_str());
                        if (p.ready != l->ready) logf("'%s' %s", p.name.c_str(), p.ready ? "started the game" : "hasn't started yet");
                        l->name = p.name;
                        l->body = p.body;
                        l->ready = p.ready;
                        continue;
                    }
                    if (p.id == self_id || p.id >= kMaxPlayers) continue;
                    Other& o = others[p.id];
                    if (o.name.empty()) logf("'%s' is in the game too (player %u)", p.name.c_str(), p.id);
                    if (!o.name.empty() && p.body != o.body) logf("'%s' chose body '%s'", p.name.c_str(), p.body.c_str());
                    if (!o.name.empty() && p.ready != o.ready) logf("'%s' is %s", p.name.c_str(), p.ready ? "ready" : "not ready");
                    o.link = p.link;
                    o.ready = p.ready;
                    o.name = p.name;
                    o.body = p.body;
                }
                break;
            }
            case Msg::Animals: {
                if (hosting() || !linked) return;
                AnimalsMsg m;
                if (!read_animals(r, m)) return;
                m.received_ms = now;
                if (animals_in.size() >= kInboxLimit * 4) animals_in.erase(animals_in.begin());
                animals_in.push_back(std::move(m));
                l->last_recv = now;
                break;
            }
            case Msg::Attack: {
                if (hosting() || !linked) return;
                AttackMsg a;
                if (read_attack(r, a) && attacks_in.size() < 256) attacks_in.push_back(a);
                l->last_recv = now;
                break;
            }
            case Msg::AttackResult: {
                if (!hosting() || !linked) return;
                AttackResultMsg a;
                a.player = l->id;
                if (read_attack_result(r, a) && results_in.size() < 256) {
                    a.player = l->id;
                    results_in.push_back(a);
                }
                l->last_recv = now;
                break;
            }
            case Msg::AnimalSounds: {
                if (hosting() || !linked) return;
                if (sounds_in.size() > 1024) sounds_in.clear();  // nobody is draining
                read_sounds(r, sounds_in);
                l->last_recv = now;
                break;
            }
            case Msg::Reliable: {
                if (!linked) return;
                const auto seq = r.get<uint32_t>();
                const std::string inner = r.get_blob(kMaxPacket);
                if (!r.ok() || inner.size() < 7) return;
                Writer ack(Msg::Ack);
                ack.put(seq);
                send(l->addr, ack);
                l->last_recv = now;
                if (std::find(l->reliable_seen.begin(), l->reliable_seen.end(), seq) != l->reliable_seen.end()) return;
                if (l->reliable_seen.size() < kReliableSeen)
                    l->reliable_seen.push_back(seq);
                else
                    l->reliable_seen[l->reliable_seen_next++ % kReliableSeen] = seq;
                const auto* bytes = reinterpret_cast<const uint8_t*>(inner.data());
                if (static_cast<Msg>(bytes[6]) == Msg::Reliable) return;  // no nesting
                handle(from, bytes, inner.size(), now);
                break;
            }
            case Msg::Ack: {
                if (!linked) return;
                const auto seq = r.get<uint32_t>();
                if (!r.ok()) return;
                auto& out = l->reliable_out;
                out.erase(std::remove_if(out.begin(), out.end(), [&](const Pending& p) { return p.seq == seq; }), out.end());
                l->last_recv = now;
                break;
            }
            case Msg::Hit:
            case Msg::Thrown:
            case Msg::Harvest:
            case Msg::Quartered:
            case Msg::CaughtUp:
            case Msg::ItemWant:
            case Msg::ContainerWant: {
                if (!hosting() || !linked) return;
                keep_packet(*l, type, data, size, now);
                break;
            }
            case Msg::Carcasses:
            case Msg::ItemList:
            case Msg::ObjectList:
            case Msg::FireList:
            case Msg::GameMode: {
                if (hosting() || !linked) return;
                keep_packet(*l, type, data, size, now);
                break;
            }
            case Msg::ItemPut:
            case Msg::ItemGone:
            case Msg::ContainerPut:
            case Msg::ObjectPut:
            case Msg::FirePut:
            case Msg::Sleep:
            case Msg::Give: {
                if (!linked) return;
                keep_packet(*l, type, data, size, now);
                break;
            }
            case Msg::Chunk: {
                if (!linked) return;
                l->last_recv = now;
                receive_chunk(from, *l, r, now);
                break;
            }
            case Msg::World: {
                if (hosting() || !linked) return;
                WorldMsg w;
                if (!read_world(r, w)) return;
                if (have_remote_world && static_cast<int32_t>(w.seq - remote_world.seq) <= 0) return;
                if (!have_remote_world) logf("receiving the host's world (clock, weather, wind)");
                w.received_ms = now;
                remote_world = std::move(w);
                have_remote_world = true;
                l->last_recv = now;
                break;
            }
        }
    }

    void receive_all(double now) {
        PeerId from = 0;
        std::vector<uint8_t> data;
        while (link->receive(from, data))
            if (now >= cut_until) handle(from, data.data(), data.size(), now);
    }

    // --- sending ---------------------------------------------------------------------------------------------------

    void send_roster() {
        std::vector<RosterEntry> players;
        players.push_back({0, 0, ready, cfg.name, cfg.body});
        for (const Link& l : links)
            if (!l.gone && l.connected) players.push_back({l.id, l.serial, l.ready, l.name, l.body});
        Writer w(Msg::Roster);
        write_roster(w, players);
        for (Link& l : links)
            if (!l.gone && l.connected) send(l.addr, w);
    }

    void tick_link(Link& l, double now) {
        if (now - l.last_ping >= kPingIntervalMs) {
            l.last_ping = now;
            Writer w(Msg::Ping);
            w.put(now);
            send(l.addr, w);
        }
        // Only the oldest kReliableWindow go out (and count towards giving up); the rest wait for acks to free a slot.
        size_t in_window = 0;
        for (auto it = l.reliable_out.begin(); it != l.reliable_out.end() && in_window < kReliableWindow;) {
            if (it->first_ms == 0) it->first_ms = now;
            if (now - it->first_ms > kReliableGiveUpMs) {
                logf("gave up on reliable message %u to '%s' (type %u): no answer", it->seq, l.name.c_str(),
                     it->data.size() > 19 ? it->data[19] : 0);
                it = l.reliable_out.erase(it);
                continue;
            }
            if (now - it->last_ms >= kReliableResendMs) {
                it->last_ms = now;
                if (link && now >= cut_until) link->send(l.addr, it->data.data(), it->data.size());
            }
            ++it;
            ++in_window;
        }
        for (const auto& data : l.unreliable_out)
            if (link && now >= cut_until) link->send(l.addr, data.data(), data.size());
        l.unreliable_out.clear();
    }

    void broadcast(const Writer& w) {
        for (Link& l : links)
            if (!l.gone && l.connected) send(l.addr, w);
    }

    void tick(double now) {
        link->poll(now);
        link_status = link->status();
        if (!hosting() && links.empty()) {
            PeerId host = 0;
            if (link->host_peer(host)) {
                links.emplace_back();
                links.back().addr = host;
            }
        }

        for (Link& l : links)
            if (!l.gone && l.connected && now - l.last_recv > kTimeoutMs) on_disconnected(l, "timed out (no packets for 5 s)");
        drop_gone();
        for (auto it = asking.begin(); it != asking.end();) {
            if (now - it->last_hello <= kTimeoutMs) {
                ++it;
                continue;
            }
            logf("'%s' stopped asking to join", it->name.c_str());
            it = asking.erase(it);
        }

        if (Link* host = host_link(); host && !host->connected && !rejected && now - last_hello >= kHelloIntervalMs) {
            last_hello = now;
            send_hello(*host);
        }
        if (!any_connected()) return;

        if (Link* host = host_link(); host && caught_up_due &&
                                      std::none_of(host->reliable_out.begin(), host->reliable_out.end(), [&](const Pending& p) {
                                          return static_cast<int32_t>(p.seq - caught_up_seq) <= 0;
                                      })) {
            caught_up_due = false;  // the host has everything we sent before (acked: in its inbox)
            Writer w(Msg::CaughtUp);
            w.put(caught_up_count);
            queue_reliable(*host, w);
        }

        if (now - last_state >= kStateIntervalMs) {
            last_state = now;
            local.seq = ++send_seq;
            local.player = self_id;
            Writer w(Msg::State);
            write_state(w, local);
            broadcast(w);
            ++states_sent;
        }
        if (hosting()) {
            if (roster_changed || now - last_roster >= kRosterIntervalMs) {
                last_roster = now;
                roster_changed = false;
                send_roster();
            }
        } else if (now - last_profile >= kProfileIntervalMs) {
            last_profile = now;
            Writer w(Msg::Profile);
            w.put_str(cfg.name, kNameLen);
            w.put_str(cfg.body, kBodyLen);
            w.put(static_cast<uint8_t>(ready ? 1 : 0));
            broadcast(w);
        }
        for (Link& l : links)
            if (!l.gone && l.connected) tick_link(l, now);
        if (!hosting()) return;
        if (!sounds_out.empty()) {
            constexpr size_t kPerPacket = (kMaxPacket - 8) / kAnimalSoundSize;
            for (size_t at = 0; at < sounds_out.size(); at += kPerPacket) {
                const std::vector<AnimalSound> chunk(
                    sounds_out.begin() + static_cast<ptrdiff_t>(at),
                    sounds_out.begin() + static_cast<ptrdiff_t>(std::min(sounds_out.size(), at + kPerPacket)));
                Writer w(Msg::AnimalSounds);
                write_sounds(w, chunk);
                broadcast(w);
            }
            sounds_out.clear();
        }
        if (!animals_out.empty()) {
            for (const AnimalsMsg& part : animals_out) {
                Writer w(Msg::Animals);
                write_animals(w, part);
                broadcast(w);
            }
            animals_out.clear();
        }
        if (have_local_world &&
            (now - last_world >= kWorldIntervalMs || (world_changed && now - last_world >= kWorldChangeMs))) {
            last_world = now;
            world_changed = false;
            local_world.seq = ++world_seq;
            Writer w(Msg::World);
            write_world(w, local_world);
            broadcast(w);
        }
    }

    // --- status ----------------------------------------------------------------------------------------------------

    Status status() const {
        Status st;
        st.rejected = rejected;
        st.self_id = self_id;
        st.link = link_status;
        st.states_sent = states_sent;
        st.states_received = states_received;
        st.connected = any_connected();
        st.caught_up = hosting() && st.connected;
        st.turned_away = turned_away;
        st.waiting = !hosting() && !st.connected && !rejected && now_ms() - last_wait <= kTimeoutMs;
        for (const Asking& a : asking) {
            PlayerInfo p;
            p.name = a.name;
            p.link = a.ticket;
            st.asking.push_back(std::move(p));
        }
        for (const Link& l : links) {
            if (l.gone || !l.connected) continue;
            PlayerInfo p;
            p.id = l.id;
            p.name = l.name;
            p.body = l.body;
            p.linked = true;
            p.link = l.serial;
            p.ready = l.ready;
            p.rtt_ms = l.rtt_ms;
            p.caught_up = l.caught_up;
            st.caught_up &= l.caught_up;
            st.players.push_back(std::move(p));
        }
        if (!hosting() && st.connected) {
            for (const auto& [id, o] : others) {
                PlayerInfo p;
                p.id = id;
                p.link = o.link;
                p.ready = o.ready;
                p.name = o.name;
                p.body = o.body;
                st.players.push_back(std::move(p));
            }
        }
        std::sort(st.players.begin(), st.players.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
        if (const Link* first = first_connected()) {
            st.peer_name = first->name;
            st.peer_body = first->body;
            st.rtt_ms = first->rtt_ms;
        } else if (st.waiting && !links.empty()) {
            st.peer_name = links[0].name;
        }
        return st;
    }
};

Session::Session(LogFn log) : impl_(std::make_unique<Impl>()) { impl_->log = std::move(log); }

Session::~Session() { stop(); }

bool Session::start(const Config& config) {
    std::lock_guard lock(impl_->mu);
    Impl& s = *impl_;
    s.cfg = config;
    s.nonce = make_nonce();
    s.link = config.transport == "steam" ? make_steam_transport() : make_udp_transport();
    if (!s.link->open(config, s.log)) {
        s.link_status = s.link->status();
        s.link.reset();
        return false;
    }
    s.link_status = s.link->status();
    s.logf("%s as '%s' over %s (game %s): %s", config.role == Role::Host ? "hosting" : "joining",
           config.name.c_str(), config.transport.c_str(), config.game_version.c_str(), s.link_status.c_str());
    return true;
}

void Session::update() {
    std::lock_guard lock(impl_->mu);
    if (!impl_->link) return;
    const double now = now_ms();
    impl_->receive_all(now);
    impl_->tick(now);
    impl_->drop_gone();
}

void Session::wait(int timeout_ms) {
    Transport* link;
    {
        std::lock_guard lock(impl_->mu);
        link = impl_->link.get();
    }
    if (link)
        link->wait(timeout_ms);
    else
        Sleep(static_cast<DWORD>(timeout_ms));
}

void Session::stop() {
    std::lock_guard lock(impl_->mu);
    Impl& s = *impl_;
    if (!s.link) return;
    s.broadcast(Writer(Msg::Bye));
    s.link->close();
    s.link.reset();
}

void Session::set_body(const std::string& body) {
    std::lock_guard lock(impl_->mu);
    if (impl_->cfg.body == body) return;
    impl_->cfg.body = body;
    impl_->last_profile = -1e18;  // client: tell the host on the next tick
    impl_->roster_changed = true;  // host: tell everyone
}

void Session::set_ready(bool ready) {
    std::lock_guard lock(impl_->mu);
    if (impl_->ready == ready) return;
    impl_->ready = ready;
    impl_->last_profile = -1e18;  // client: tell the host on the next tick
    impl_->roster_changed = true;  // host: tell everyone
}

bool Session::admit(uint32_t ticket) {
    std::lock_guard lock(impl_->mu);
    return impl_->hosting() && impl_->admit(ticket, now_ms());
}

bool Session::turn_away(uint32_t ticket) {
    std::lock_guard lock(impl_->mu);
    return impl_->hosting() && impl_->turn_away(ticket);
}

void Session::set_world(const WorldMsg& world) {
    std::lock_guard lock(impl_->mu);
    Impl& s = *impl_;
    const WorldMsg& old = s.local_world;
    float jump = world.normalized_time - old.normalized_time;
    if (jump < 0) jump = -jump;
    s.world_changed |= !s.have_local_world || world.valid != old.valid || world.weather_set != old.weather_set ||
                       world.weather_stage != old.weather_stage || world.wind_strength != old.wind_strength ||
                       world.time_lapse != old.time_lapse || world.region != old.region || world.world_id != old.world_id || (jump > 0.01f && jump < 0.99f);
    s.local_world = world;
    s.have_local_world = true;
}

void Session::set_local_state(const StateMsg& state) {
    std::lock_guard lock(impl_->mu);
    const uint32_t seq = impl_->local.seq;
    impl_->local = state;
    impl_->local.seq = seq;
}

std::optional<StateMsg> Session::remote_state() const {
    std::lock_guard lock(impl_->mu);
    const Impl::Link* l = impl_->first_connected();
    if (!l || !l->have_remote) return std::nullopt;
    return l->remote;
}

std::optional<StateMsg> Session::remote_state(uint8_t player) const {
    std::lock_guard lock(impl_->mu);
    const Impl& s = *impl_;
    if (s.hosting() || player == 0) {
        const Impl::Link* l = s.hosting() ? s.link_by_id(player) : s.first_connected();
        if (!l || !l->have_remote) return std::nullopt;
        return l->remote;
    }
    if (!s.any_connected()) return std::nullopt;
    const auto it = s.others.find(player);
    if (it == s.others.end() || !it->second.have_state) return std::nullopt;
    return it->second.state;
}

std::vector<StateMsg> Session::remote_states() const {
    std::lock_guard lock(impl_->mu);
    const Impl& s = *impl_;
    std::vector<StateMsg> out;
    for (const Impl::Link& l : s.links)
        if (!l.gone && l.connected && l.have_remote) out.push_back(l.remote);
    if (!s.hosting() && s.any_connected())
        for (const auto& [id, o] : s.others)
            if (o.have_state) out.push_back(o.state);
    return out;
}

void Session::send_animals(std::vector<AnimalsMsg> parts) {
    std::lock_guard lock(impl_->mu);
    impl_->animals_out = std::move(parts);  // an unsent older frame is simply replaced
}

void Session::send_animal_sounds(const std::vector<AnimalSound>& sounds) {
    std::lock_guard lock(impl_->mu);
    if (!impl_->any_connected()) return;
    if (impl_->sounds_out.size() > 1024) impl_->sounds_out.clear();
    impl_->sounds_out.insert(impl_->sounds_out.end(), sounds.begin(), sounds.end());
}

void Session::drain_animal_sounds(std::vector<AnimalSound>& out) {
    std::lock_guard lock(impl_->mu);
    out.insert(out.end(), impl_->sounds_in.begin(), impl_->sounds_in.end());
    impl_->sounds_in.clear();
}

void Session::send_attack(const AttackMsg& attack) {
    std::lock_guard lock(impl_->mu);
    Impl& s = *impl_;
    Impl::Link* l = attack.player ? s.link_by_id(attack.player) : const_cast<Impl::Link*>(s.first_connected());
    if (!l) return;
    Writer w(Msg::Attack);
    write_attack(w, attack);
    s.queue_reliable(*l, w);
}

void Session::drain_attacks(std::vector<AttackMsg>& out) {
    std::lock_guard lock(impl_->mu);
    out.insert(out.end(), impl_->attacks_in.begin(), impl_->attacks_in.end());
    impl_->attacks_in.clear();
}

void Session::send_attack_result(const AttackResultMsg& result) {
    std::lock_guard lock(impl_->mu);
    Impl::Link* host = impl_->host_link();
    if (!host) return;
    Writer w(Msg::AttackResult);
    write_attack_result(w, result);
    impl_->queue_reliable(*host, w);
}

void Session::send_packet(const Writer& w, bool reliable) {
    std::lock_guard lock(impl_->mu);
    Impl& s = *impl_;
    if (!w.ok()) return;
    if (!s.hosting()) {
        Impl::Link* host = s.host_link();
        if (!host || !host->connected) {
            if (reliable) s.keep_for_later(w.data(), w.size());
            return;
        }
        s.queue(*host, w, reliable);
        return;
    }
    for (Impl::Link& l : s.links)
        if (!l.gone && l.connected) s.queue(l, w, reliable);
}

void Session::send_packet_to(uint8_t player, const Writer& w, bool reliable) {
    std::lock_guard lock(impl_->mu);
    if (!impl_->hosting()) return;
    if (Impl::Link* l = impl_->link_by_id(player)) impl_->queue(*l, w, reliable);
}

void Session::send_packet_except(uint8_t player, const Writer& w, bool reliable) {
    std::lock_guard lock(impl_->mu);
    if (!impl_->hosting()) return;
    for (Impl::Link& l : impl_->links)
        if (!l.gone && l.connected && l.id != player) impl_->queue(l, w, reliable);
}

void Session::drain_packets(std::vector<Packet>& out) {
    std::lock_guard lock(impl_->mu);
    for (Packet& p : impl_->packets_in) {
        if (p.type != Msg::CaughtUp) {
            out.push_back(std::move(p));
            continue;
        }
        // Everything the friend sent before it was acked first, so it's in `out` by now.
        Reader r = packet_body(p);
        const auto count = r.get<uint32_t>();
        Impl::Link* l = impl_->link_by_id(p.from);
        if (!l || l->serial != p.link) continue;  // from an earlier connection of theirs
        l->caught_up = true;
        if (r.ok() && count)
            impl_->logf("the other player's %u world change(s) from while the link was down are in (player %u, '%s')",
                        count, l->id, l->name.c_str());
    }
    impl_->packets_in.clear();
}

void Session::keep_offline(bool on) {
    std::lock_guard lock(impl_->mu);
    impl_->keep_offline = on;
    if (!on) impl_->unconfirmed.clear();
    if (on || impl_->outbox.empty()) return;
    impl_->logf("dropped the %zu world change(s) kept from while the link was down", impl_->outbox.size());
    impl_->outbox.clear();
    impl_->outbox_bytes = 0;
    impl_->outbox_full = false;
}

void Session::drain_attack_results(std::vector<AttackResultMsg>& out) {
    std::lock_guard lock(impl_->mu);
    out.insert(out.end(), impl_->results_in.begin(), impl_->results_in.end());
    impl_->results_in.clear();
}

void Session::drain_animals(std::vector<AnimalsMsg>& out) {
    std::lock_guard lock(impl_->mu);
    out.insert(out.end(), std::make_move_iterator(impl_->animals_in.begin()),
               std::make_move_iterator(impl_->animals_in.end()));
    impl_->animals_in.clear();
}

std::optional<WorldMsg> Session::remote_world() const {
    std::lock_guard lock(impl_->mu);
    if (!impl_->any_connected() || !impl_->have_remote_world) return std::nullopt;
    return impl_->remote_world;
}

void Session::cut_link(double ms) {
    std::lock_guard lock(impl_->mu);
    impl_->cut_until = now_ms() + ms;
    impl_->logf("test: cutting the link for %.0f s", ms / 1000);
}

Status Session::status() const {
    std::lock_guard lock(impl_->mu);
    return impl_->status();
}

void Session::drain_remote_states(std::vector<TimedState>& out) {
    std::lock_guard lock(impl_->mu);
    out.insert(out.end(), std::make_move_iterator(impl_->inbox.begin()), std::make_move_iterator(impl_->inbox.end()));
    impl_->inbox.clear();
}

}  // namespace net
