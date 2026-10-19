// Two real sessions (host + client) over UDP on 127.0.0.1: connect, then check that big reliable messages (split
// into Chunks) and a burst of small ones (more than the send window) all arrive intact, once each; then drop the link
// and check that the client's world changes made meanwhile reach the host once it's back, before CaughtUp. Then two more
// friends join (4 players): ids, each other's states through the host, the roster, sends to one or all but one, a
// fifth turned away until someone leaves. Also: a link only the client saw drop (the host stalled) comes back as a
// new connection whose reliable messages all arrive, and READY / START reach everyone. And a host that asks: friends
// wait until let in, one let in comes back by itself after a dropped link, one turned away or kicked stays out.
// Exit code 0 when everything arrived.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "net/session.h"

namespace {

bool pump(net::Session& a, net::Session& b, double ms) {
    const double until = net::now_ms() + ms;
    while (net::now_ms() < until) {
        a.update();
        b.update();
        Sleep(1);
    }
    return true;
}

void pump_all(const std::vector<net::Session*>& all, double ms) {
    const double until = net::now_ms() + ms;
    while (net::now_ms() < until) {
        for (net::Session* s : all) s->update();
        Sleep(1);
    }
}

const net::PlayerInfo* player(const net::Status& st, uint8_t id) {
    for (const net::PlayerInfo& p : st.players)
        if (p.id == id) return &p;
    return nullptr;
}
// status() returns a copy: a pointer into a temporary one would dangle (keep the Status in a variable).
const net::PlayerInfo* player(const net::Status&& st, uint8_t id) = delete;

uint32_t link_of(const net::Status& st, uint8_t id) {
    const net::PlayerInfo* p = player(st, id);
    return p ? p->link : 0;
}

int gone_with(net::Session& s, const char* pdid, uint8_t* from = nullptr) {
    std::vector<net::Packet> got;
    s.drain_packets(got);
    int n = 0;
    for (const net::Packet& p : got) {
        if (p.type != net::Msg::ItemGone) continue;
        net::Reader r = net::packet_body(p);
        net::ItemGoneMsg g;
        if (!net::read_item_gone(r, g) || g.pdid != pdid) continue;
        ++n;
        if (from) *from = p.from;
    }
    return n;
}

net::Writer item_gone(const char* pdid) {
    net::ItemGoneMsg g;
    g.scene = "LakeRegion";
    g.pdid = pdid;
    net::Writer w(net::Msg::ItemGone);
    net::write_item_gone(w, g);
    return w;
}

std::string big_text(size_t n, char seed) {
    std::string s(n, ' ');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7 + seed) % 26);
    return s;
}

}  // namespace

int main() {
    auto quiet = [](const std::string& line) { printf("  net: %s\n", line.c_str()); };
    net::Config hc, cc;
    hc.role = net::Role::Host;
    hc.port = cc.port = 27991;
    hc.name = "host";
    cc.role = net::Role::Client;
    cc.name = "client";
    hc.game_version = cc.game_version = "test";
    net::Session host(quiet), client(quiet);
    if (!host.start(hc) || !client.start(cc)) {
        printf("FAIL: sessions didn't start\n");
        return 1;
    }
    for (int i = 0; i < 300 && !(host.status().connected && client.status().connected); ++i) pump(host, client, 10);
    if (!host.status().connected || !client.status().connected) {
        printf("FAIL: no connection\n");
        return 1;
    }

    // One 40 KB item each way, plus 300 small ItemGone messages from the client (more than the send window).
    net::ItemPutMsg big;
    big.scene = "LakeRegion";
    big.pdid = "pdid-big";
    big.prefab = "GEAR_Test";
    big.gear = big_text(40000, 3);
    {
        net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
        net::write_item_put(w, big);
        host.send_packet(w, true);
        client.send_packet(w, true);
    }
    for (int i = 0; i < 300; ++i) {
        net::ItemGoneMsg g;
        g.scene = "LakeRegion";
        g.pdid = "pdid-" + std::to_string(i);
        net::Writer w(net::Msg::ItemGone);
        net::write_item_gone(w, g);
        client.send_packet(w, true);
    }
    pump(host, client, 3000);

    int failures = 0;
    std::vector<net::Packet> at_host, at_client;
    host.drain_packets(at_host);
    client.drain_packets(at_client);
    auto check_big = [&](const std::vector<net::Packet>& got, const char* who) {
        int puts = 0;
        for (const net::Packet& p : got) {
            if (p.type != net::Msg::ItemPut) continue;
            net::Reader r = net::packet_body(p);
            net::ItemPutMsg m;
            if (!net::read_item_put(r, m) || m.gear != big.gear || m.pdid != big.pdid || m.prefab != big.prefab) {
                printf("FAIL: %s got a damaged ItemPut (%zu bytes of gear)\n", who, m.gear.size());
                ++failures;
            }
            ++puts;
        }
        if (puts != 1) {
            printf("FAIL: %s got %d copies of the 40 KB ItemPut (want 1)\n", who, puts);
            ++failures;
        } else {
            printf("ok: %s got the 40 KB ItemPut intact\n", who);
        }
    };
    check_big(at_host, "host");
    check_big(at_client, "client");
    std::set<std::string> gone;
    int gone_total = 0;
    for (const net::Packet& p : at_host) {
        if (p.type != net::Msg::ItemGone) continue;
        net::Reader r = net::packet_body(p);
        net::ItemGoneMsg g;
        if (net::read_item_gone(r, g)) gone.insert(g.pdid);
        ++gone_total;
    }
    if (gone.size() != 300 || gone_total != 300) {
        printf("FAIL: host got %zu distinct / %d ItemGone (want 300 / 300)\n", gone.size(), gone_total);
        ++failures;
    } else {
        printf("ok: host got all 300 ItemGone once each\n");
    }
    if (!host.status().caught_up) {
        printf("FAIL: the host isn't caught up with a client that was never offline\n");
        ++failures;
    }

    // The link drops; the client goes on changing its world (one change too big for a datagram), then it's back.
    client.keep_offline(true);
    client.cut_link(6000);
    {
        net::ItemGoneMsg g;  // sent before the session notices: unacked when it times out, so it must go again
        g.scene = "LakeRegion";
        g.pdid = "pdid-offline-early";
        net::Writer w(net::Msg::ItemGone);
        net::write_item_gone(w, g);
        client.send_packet(w, true);
    }
    pump(host, client, 5500);  // both time out after 5 s; the client says hello again only after 6
    if (host.status().connected || client.status().connected) {
        printf("FAIL: the link didn't drop\n");
        ++failures;
    }
    net::ItemPutMsg offline_put = big;
    offline_put.pdid = "pdid-offline-big";
    {
        net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
        net::write_item_put(w, offline_put);
        client.send_packet(w, true);
    }
    for (int i = 0; i < 5; ++i) {
        net::ItemGoneMsg g;
        g.scene = "LakeRegion";
        g.pdid = "pdid-offline-" + std::to_string(i);
        net::Writer w(net::Msg::ItemGone);
        net::write_item_gone(w, g);
        client.send_packet(w, true);
    }
    {
        net::HarvestMsg h;  // a carcass harvest and a quartering: kept like world changes
        h.animal = 7;
        h.seq = 99;
        h.meat_units = 1000000000;
        h.hide = 1;
        net::Writer w(net::Msg::Harvest);
        net::write_harvest(w, h);
        client.send_packet(w, true);
        net::Writer q(net::Msg::Quartered);
        q.put(static_cast<uint32_t>(7));
        client.send_packet(q, true);
    }
    {
        net::ItemHashesMsg want;  // a question: not kept
        want.scene = "LakeRegion";
        net::Writer w(net::Msg::ItemWant, net::kMaxMessage);
        net::write_item_hashes(w, want);
        client.send_packet(w, true);
    }
    std::vector<net::Packet> after;
    for (int i = 0; i < 1000 && !host.status().caught_up; ++i) {
        pump(host, client, 10);
        host.drain_packets(after);
    }
    int offline_puts = 0, offline_gone = 0, wants = 0, harvests = 0, quartered = 0;
    for (const net::Packet& p : after) {
        net::Reader r = net::packet_body(p);
        if (p.type == net::Msg::ItemPut) {
            net::ItemPutMsg m;
            if (net::read_item_put(r, m) && m.pdid == offline_put.pdid && m.gear == offline_put.gear) ++offline_puts;
        } else if (p.type == net::Msg::ItemGone) {
            net::ItemGoneMsg g;
            if (net::read_item_gone(r, g) && g.pdid.rfind("pdid-offline-", 0) == 0) ++offline_gone;
        } else if (p.type == net::Msg::ItemWant) {
            ++wants;
        } else if (p.type == net::Msg::Harvest) {
            net::HarvestMsg h;
            if (net::read_harvest(r, h) && h.animal == 7 && h.seq == 99 && h.hide == 1) ++harvests;
        } else if (p.type == net::Msg::Quartered) {
            if (r.get<uint32_t>() == 7 && r.ok()) ++quartered;
        }
    }
    if (!host.status().caught_up || offline_puts != 1 || offline_gone != 6 || wants != 0 || harvests != 1 ||
        quartered != 1) {
        printf("FAIL: back linked: caught up %d, offline ItemPut %d (want 1), ItemGone %d (want 6), ItemWant %d (want 0), "
               "Harvest %d (want 1), Quartered %d (want 1)\n",
               host.status().caught_up ? 1 : 0, offline_puts, offline_gone, wants, harvests, quartered);
        ++failures;
    } else {
        printf("ok: the client's changes while the link was down (items, a carcass harvest and quartering) reached the "
               "host before CaughtUp\n");
    }

    // The host stalls for 5.5 s: the client times out and says hello again, while the host (its socket full of the
    // client's packets) never noticed. The new connection numbers its reliable messages afresh; none may be lost.
    {
        const uint32_t before = link_of(host.status(), 1);
        const double until = net::now_ms() + 5500;
        while (net::now_ms() < until) {
            client.update();
            Sleep(1);
        }
        for (int i = 0; i < 300 && !(host.status().connected && client.status().connected &&
                                     link_of(host.status(), 1) != 0 && link_of(host.status(), 1) != before);
             ++i)
            pump(host, client, 10);
        std::vector<net::Packet> got;
        host.drain_packets(got);  // what came before
        const net::Status hs = host.status();
        const net::PlayerInfo* now = player(hs, 1);
        if (!now || now->link == before || hs.players.size() != 1) {
            printf("FAIL: after the host stalled: friend %s, link %u (was %u), %zu friend(s)\n", now ? "there" : "missing",
                   now ? now->link : 0, before, hs.players.size());
            ++failures;
        }
    }
    {
        for (int i = 0; i < 5; ++i) client.send_packet(item_gone(("again-" + std::to_string(i)).c_str()), true);
        pump(host, client, 1500);
        std::vector<net::Packet> got;
        host.drain_packets(got);
        int n = 0;
        for (const net::Packet& p : got) {
            net::Reader r = net::packet_body(p);
            net::ItemGoneMsg g;
            n += p.type == net::Msg::ItemGone && net::read_item_gone(r, g) && g.pdid.rfind("again-", 0) == 0;
        }
        if (n != 5) {
            printf("FAIL: after the host stalled, the host got %d of the client's 5 reliable messages\n", n);
            ++failures;
        } else {
            printf("ok: a link only the client saw drop came back as a new connection; its reliable messages arrive\n");
        }
    }

    // Two more friends: 4 players. A fifth waits (the host is full) until one of them leaves.
    net::Config c2 = cc, c3 = cc, c4 = cc;
    c2.name = "second";
    c3.name = "third";
    c4.name = "fourth";
    net::Session second(quiet), third(quiet), fourth(quiet);
    if (!second.start(c2) || !third.start(c3)) {
        printf("FAIL: more sessions didn't start\n");
        return 1;
    }
    std::vector<net::Session*> all{&host, &client, &second, &third};
    for (int i = 0; i < 300 && !(second.status().connected && third.status().connected); ++i) pump_all(all, 10);
    net::StateMsg here;
    here.valid = true;
    here.scene = "LakeRegion";
    here.x = 11;
    client.set_local_state(here);
    here.x = 22;
    host.set_local_state(here);
    pump_all(all, 2500);  // a roster goes every 2 s
    {
        const net::Status hs = host.status(), s2 = second.status();
        const auto c_at_second = second.remote_state(1);
        const auto h_at_second = second.remote_state(0);
        const net::PlayerInfo* first_at_second = player(s2, 1);
        const net::PlayerInfo* third_at_second = player(s2, 3);
        if (hs.players.size() != 3 || s2.self_id != 2 || third.status().self_id != 3 || !c_at_second ||
            c_at_second->x != 11 || c_at_second->player != 1 || !h_at_second || h_at_second->x != 22 ||
            !first_at_second || first_at_second->name != "client" || first_at_second->linked || !third_at_second ||
            third_at_second->name != "third" || s2.players.size() != 3) {
            printf("FAIL: 4 players: host sees %zu friends; second is %u and sees %zu players, first's x %.0f, host's x %.0f\n",
                   hs.players.size(), s2.self_id, s2.players.size(), c_at_second ? c_at_second->x : -1.f,
                   h_at_second ? h_at_second->x : -1.f);
            ++failures;
        } else {
            printf("ok: 4 players: ids 1-3, each sees the others (states through the host, names from the roster)\n");
        }
    }
    uint32_t third_link = link_of(second.status(), 3);
    // The lobby: a friend's READY and the host's START reach everyone.
    second.set_ready(true);
    host.set_ready(true);
    pump_all(all, 2500);  // told right away; a lost datagram is made up by the next roster (every 2 s)
    {
        const net::Status hs = host.status(), cs = client.status(), ts = third.status();
        const net::PlayerInfo* second_at_host = player(hs, 2);
        const net::PlayerInfo* second_at_first = player(cs, 2);
        const net::PlayerInfo* host_at_third = player(ts, 0);
        const net::PlayerInfo* first_at_host = player(hs, 1);
        if (!second_at_host || !second_at_host->ready || !second_at_first || !second_at_first->ready || !host_at_third ||
            !host_at_third->ready || !first_at_host || first_at_host->ready) {
            printf("FAIL: ready: second at host %d, second at first %d, host at third %d, first at host %d (want 1 1 1 0)\n",
                   second_at_host && second_at_host->ready, second_at_first && second_at_first->ready,
                   host_at_third && host_at_third->ready, first_at_host && first_at_host->ready);
            ++failures;
        } else {
            printf("ok: READY and START reach the host and the other friends\n");
        }
    }
    host.send_packet_to(2, item_gone("to-second"), true);
    host.send_packet_except(2, item_gone("not-second"), true);
    third.send_packet(item_gone("from-third"), true);
    pump_all(all, 1000);
    {
        uint8_t from = 0;
        const int from_third = gone_with(host, "from-third", &from);
        std::vector<net::Packet> c1, c2p, c3p;
        client.drain_packets(c1);
        second.drain_packets(c2p);
        third.drain_packets(c3p);
        auto count = [](const std::vector<net::Packet>& got, const char* pdid) {
            int n = 0;
            for (const net::Packet& p : got) {
                net::Reader r = net::packet_body(p);
                net::ItemGoneMsg g;
                n += p.type == net::Msg::ItemGone && net::read_item_gone(r, g) && g.pdid == pdid;
            }
            return n;
        };
        if (from_third != 1 || from != 3 || count(c2p, "to-second") != 1 || count(c1, "to-second") || count(c3p, "to-second") ||
            count(c2p, "not-second") || count(c1, "not-second") != 1 || count(c3p, "not-second") != 1) {
            printf("FAIL: targeted sends: host got third's %d (from %u); to-second %d/%d/%d, not-second %d/%d/%d\n", from_third,
                   from, count(c1, "to-second"), count(c2p, "to-second"), count(c3p, "to-second"),
                   count(c1, "not-second"), count(c2p, "not-second"), count(c3p, "not-second"));
            ++failures;
        } else {
            printf("ok: sends to one friend, to all but one, and Packet.from on the host\n");
        }
    }
    if (!fourth.start(c4)) {
        printf("FAIL: the fifth session didn't start\n");
        return 1;
    }
    all.push_back(&fourth);
    pump_all(all, 1500);
    if (fourth.status().connected || fourth.status().rejected) {
        printf("FAIL: a fifth player got in (or gave up) while the game was full\n");
        ++failures;
    } else {
        printf("ok: a fifth player waits while the game is full\n");
    }
    third.stop();
    all.erase(std::find(all.begin(), all.end(), &third));
    for (int i = 0; i < 300 && !fourth.status().connected; ++i) pump_all(all, 10);
    pump_all(all, 2500);
    {
        const net::Status s4 = fourth.status(), s2 = second.status();
        const net::PlayerInfo* fourth_at_second = player(s2, 3);
        if (!s4.connected || s4.self_id != 3 || host.status().players.size() != 3 || !fourth_at_second ||
            fourth_at_second->name != "fourth" || fourth_at_second->link == third_link || !third_link) {
            printf("FAIL: after third left: fourth connected %d as %u; second sees player 3 as '%s'\n", s4.connected ? 1 : 0,
                   s4.self_id, fourth_at_second ? fourth_at_second->name.c_str() : "nobody");
            ++failures;
        } else {
            printf("ok: third left; the fifth took its place (player 3) and the others see it\n");
        }
    }
    fourth.stop();
    second.stop();
    client.stop();
    host.stop();

    // A host that asks (the lobby's LET IN / TURN AWAY / KICK).
    {
        net::Config ah = hc, a1 = cc, a2 = cc;
        ah.port = a1.port = a2.port = 27993;
        ah.ask_to_join = true;
        a1.name = "asker";
        a2.name = "pest";
        net::Session ahost(quiet), asker(quiet), pest(quiet);
        if (!ahost.start(ah) || !asker.start(a1) || !pest.start(a2)) {
            printf("FAIL: the asking sessions didn't start\n");
            return 1;
        }
        std::vector<net::Session*> group{&ahost, &asker, &pest};
        pump_all(group, 1500);
        auto ticket = [&](const char* name) {
            for (const net::PlayerInfo& p : ahost.status().asking)
                if (p.name == name) return p.link;
            return 0u;
        };
        {
            const net::Status hs = ahost.status(), as = asker.status();
            if (hs.asking.size() != 2 || hs.connected || !as.waiting || as.connected || as.peer_name != "host" ||
                !ticket("asker") || !ticket("pest")) {
                printf("FAIL: asking: host has %zu asking (connected %d); asker waiting %d connected %d host '%s'\n",
                       hs.asking.size(), hs.connected ? 1 : 0, as.waiting ? 1 : 0, as.connected ? 1 : 0,
                       as.peer_name.c_str());
                ++failures;
            } else {
                printf("ok: with a host that asks, friends wait to be let in (and know who they wait for)\n");
            }
        }
        const bool let_in = ahost.admit(ticket("asker"));
        const bool turned = ahost.turn_away(ticket("pest"));
        for (int i = 0; i < 300 && !asker.status().connected; ++i) pump_all(group, 10);
        pump_all(group, 500);
        {
            const net::Status hs = ahost.status(), as = asker.status(), ps = pest.status();
            if (!let_in || !turned || !as.connected || as.self_id != 1 || hs.players.size() != 1 || !hs.asking.empty() ||
                !ps.turned_away || !ps.rejected || ps.waiting) {
                printf("FAIL: admit %d turn_away %d: asker connected %d as %u; host has %zu players, %zu asking; pest "
                       "turned away %d\n",
                       let_in ? 1 : 0, turned ? 1 : 0, as.connected ? 1 : 0, as.self_id, hs.players.size(),
                       hs.asking.size(), ps.turned_away ? 1 : 0);
                ++failures;
            } else {
                printf("ok: LET IN connects a friend; TURN AWAY tells the other one, which stops trying\n");
            }
        }
        asker.cut_link(5500);
        pump_all(group, 5500);
        for (int i = 0; i < 700 && !asker.status().connected; ++i) pump_all(group, 10);
        {
            const net::Status hs = ahost.status();
            if (!asker.status().connected || hs.players.size() != 1 || !hs.asking.empty()) {
                printf("FAIL: after a dropped link the friend let in is back %d (host: %zu players, %zu asking)\n",
                       asker.status().connected ? 1 : 0, hs.players.size(), hs.asking.size());
                ++failures;
            } else {
                printf("ok: a friend let in comes back by itself after a dropped link\n");
            }
        }
        const uint32_t serial = ahost.status().players.empty() ? 0 : ahost.status().players[0].link;
        const bool kicked = serial && ahost.turn_away(serial);
        pump_all(group, 7000);  // longer than a timeout and a hello: it must not come back
        {
            const net::Status hs = ahost.status(), as = asker.status();
            if (!kicked || as.connected || !as.turned_away || !hs.players.empty() || !hs.asking.empty()) {
                printf("FAIL: kick %d: asker connected %d turned away %d; host has %zu players, %zu asking\n",
                       kicked ? 1 : 0, as.connected ? 1 : 0, as.turned_away ? 1 : 0, hs.players.size(),
                       hs.asking.size());
                ++failures;
            } else {
                printf("ok: KICK ends a friend's connection and they stay out\n");
            }
        }
        pest.stop();
        asker.stop();
        ahost.stop();
    }
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
