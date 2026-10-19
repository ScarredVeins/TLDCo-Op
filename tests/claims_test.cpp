// Who gets a loose item two players took at about the same time (net::Claims, milestone 10): takes at the same moment
// go to a coin toss the host makes, fair to each player; a later take is refused. Each case is a race the game can run
// into; exit code 0 when every answer is right.

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

#include "net/claims.h"

namespace {

int g_failed = 0;

const char* name(net::TakeAnswer a) {
    switch (a) {
        case net::TakeAnswer::Yours: return "Yours";
        case net::TakeAnswer::PutBack: return "PutBack";
        case net::TakeAnswer::NotYours: return "NotYours";
    }
    return "?";
}

void check(const char* what, bool ok, const std::string& got) {
    if (!ok) ++g_failed;
    std::printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", what, got.c_str());
}

std::string describe(const net::TakeDecision& d) {
    std::string s = name(d.answer);
    if (d.tossed) s += ", tossed";
    if (d.loser >= 0) s += ", loser " + std::to_string(d.loser);
    return s;
}

void expect(const char* what, const net::TakeDecision& got, net::TakeAnswer answer, bool tossed = false, int loser = -1) {
    check(what, got.answer == answer && got.tossed == tossed && got.loser == loser, describe(got));
}

void expect_taker(const char* what, int got, int want) {
    check(what, got == want, std::to_string(got) + (got == want ? "" : ", wanted " + std::to_string(want)));
}

net::TakeFacts lying_at_host() {
    net::TakeFacts f;
    f.host_scene = f.loose = f.known = true;
    return f;
}

net::TakeFacts gone_at_host() {  // the host's world doesn't have it lying any more (someone's take removed it)
    net::TakeFacts f;
    f.host_scene = true;
    return f;
}

// A coin that says what the test wants: each toss takes the next answer (an index in [0, n)).
struct Script {
    std::deque<uint32_t> answers;
    int asked = 0;
    net::Claims claims() {
        return net::Claims([this](uint32_t n) {
            ++asked;
            const uint32_t a = answers.empty() ? 0 : answers.front();
            if (!answers.empty()) answers.pop_front();
            return a < n ? a : n - 1;
        });
    }
};

}  // namespace

int main() {
    using net::TakeAnswer;
    const std::string lake = "LakeRegion", cabin = "LakeCabinC", knife = "4eff63aa-knife", rope = "rope-1";

    {  // Two friends take the knife at the same moment; the coin keeps it with the first.
        Script s{{0}};
        net::Claims c = s.claims();
        expect("friend 1 takes the knife lying at the host", c.take(lake, knife, 1, lying_at_host(), 0), TakeAnswer::Yours);
        expect("friend 2's take, heard 300 ms later: the coin says friend 1",
               c.take(lake, knife, 2, gone_at_host(), 300), TakeAnswer::NotYours, true);
        expect_taker("it's friend 1's", c.taker(lake, knife), 1);
        expect("friend 1's take again (sent twice)", c.take(lake, knife, 1, gone_at_host(), 400), TakeAnswer::Yours);
        expect("friend 2's take again: lost, no second toss", c.take(lake, knife, 2, gone_at_host(), 500),
               TakeAnswer::NotYours);
        expect_taker("one toss only", s.asked, 1);
    }
    {  // The same, and the coin says the second.
        Script s{{1}};
        net::Claims c = s.claims();
        c.take(lake, knife, 1, lying_at_host(), 0);
        expect("friend 2's take at the same moment: the coin says friend 2", c.take(lake, knife, 2, gone_at_host(), 300),
               TakeAnswer::Yours, true, 1);
        expect_taker("it's friend 2's", c.taker(lake, knife), 2);
        expect("friend 1's take again: lost now", c.take(lake, knife, 1, gone_at_host(), 400), TakeAnswer::NotYours);
    }
    {  // Not the same moment: the first take heard wins, no coin.
        Script s;
        net::Claims c = s.claims();
        c.take(lake, knife, 1, lying_at_host(), 0);
        expect("friend 2's take heard 1.5 s later", c.take(lake, knife, 2, gone_at_host(), 1500), TakeAnswer::NotYours);
        expect_taker("no toss", s.asked, 0);
    }
    {  // The host took the knife a moment before a friend's take came in, its own word not out yet: the same moment.
        Script s{{0}};
        net::Claims c = s.claims();
        net::TakeFacts just_taken = lying_at_host();
        just_taken.loose = false;  // in the host's pack now; still on its list from the last look
        expect("friend takes what the host just took: the coin says the host",
               c.take(lake, knife, 1, just_taken, 0), TakeAnswer::NotYours, true);
        expect_taker("it's the host's", c.taker(lake, knife), net::Claims::kHost);
        c.taken_by_host(lake, knife, 200);  // its look finds it in the pack now
        expect_taker("still the host's", c.taker(lake, knife), net::Claims::kHost);
    }
    {  // The same, and the coin says the friend: the host's take is undone.
        Script s{{1}};
        net::Claims c = s.claims();
        net::TakeFacts just_taken = lying_at_host();
        just_taken.loose = false;
        expect("friend takes what the host just took: the coin says the friend", c.take(lake, knife, 1, just_taken, 0),
               TakeAnswer::Yours, true, net::Claims::kHost);
        expect_taker("it's the friend's", c.taker(lake, knife), 1);
        c.taken_by_host(lake, knife, 100);  // (the host's look won't count it now, but if it did) decided already
        expect_taker("still the friend's", c.taker(lake, knife), 1);
    }
    {  // The host's own take, told: a friend's take crossing it (the friend took it before hearing) is a toss.
        Script s{{1}};
        net::Claims c = s.claims();
        c.taken_by_host(lake, knife, 0);
        expect("friend's take crossing the host's: the coin says the friend", c.take(lake, knife, 1, gone_at_host(), 400),
               TakeAnswer::Yours, true, net::Claims::kHost);
    }
    {  // Three players at the same moment: each newcomer wins 1 time in (takers so far + 1).
        Script s{{1, 0}};
        net::Claims c = s.claims();
        c.taken_by_host(lake, knife, 0);
        expect("friend 1 (1 in 2)", c.take(lake, knife, 1, gone_at_host(), 200), TakeAnswer::Yours, true, net::Claims::kHost);
        expect("friend 2 (1 in 3)", c.take(lake, knife, 2, gone_at_host(), 400), TakeAnswer::NotYours, true);
        expect_taker("friend 1 keeps it", c.taker(lake, knife), 1);
    }
    {  // The host has the knife in its hands (Take it / Leave it) when the friend's take comes in: it was first.
        Script s;
        net::Claims c = s.claims();
        net::TakeFacts held = lying_at_host();
        held.loose = false;
        held.held = true;
        expect("friend takes what the host holds", c.take(lake, knife, 1, held, 0), TakeAnswer::PutBack);
        expect_taker("nobody's yet (the host may leave it)", c.taker(lake, knife), -1);
        expect("the host left it; the friend takes it again", c.take(lake, knife, 1, lying_at_host(), 3000),
               TakeAnswer::Yours);
        expect_taker("no toss", s.asked, 0);
    }
    {  // The host picked the knife up (Take it / Leave it) at the same moment as the friend's take.
        Script s{{0, 1}};
        net::Claims c = s.claims();
        net::TakeFacts held = lying_at_host();
        held.loose = false;
        held.held = true;
        held.held_since_ms = 4800;
        expect("friend's take 200 ms after the host picked it up: the coin says the host", c.take(lake, knife, 1, held, 5000),
               TakeAnswer::PutBack, true);
        expect_taker("nobody's yet (the host may still leave it)", c.taker(lake, knife), -1);
        expect("the same, and the coin says the friend: the host leaves it", c.take(lake, rope, 1, held, 5000),
               TakeAnswer::Yours, true, net::Claims::kHost);
        expect_taker("the rope is the friend's", c.taker(lake, rope), 1);
    }
    {  // Dropped again, the item is free: a later take is a new one.
        Script s;
        net::Claims c = s.claims();
        expect("friend 1 takes the rope", c.take(lake, rope, 1, lying_at_host(), 0), TakeAnswer::Yours);
        c.lies_again(lake, rope);
        expect("friend 1 dropped it; friend 2 takes it", c.take(lake, rope, 2, lying_at_host(), 100), TakeAnswer::Yours);
        c.lies_again(lake, rope);
        c.taken_by_host(lake, rope, 5000);
        expect("dropped, then the host took it; friend 1 much later", c.take(lake, rope, 1, gone_at_host(), 9000),
               TakeAnswer::NotYours);
    }
    {  // Two friends in a cabin the host isn't in: the same rules, by when the host hears of the takes.
        Script s;
        net::Claims c = s.claims();
        net::TakeFacts away;  // not the host's scene: its game knows nothing about it
        expect("friend 2 takes the knife in the cabin", c.take(cabin, knife, 2, away, 0), TakeAnswer::Yours);
        expect("friend 1's take of it, heard 2 s later", c.take(cabin, knife, 1, away, 2000), TakeAnswer::NotYours);
        expect("the same id in another scene is another item", c.take(lake, knife, 1, lying_at_host(), 2000),
               TakeAnswer::Yours);
    }
    {  // A take the friend made while its link was down, sent when it's back: the host heard of another one meanwhile.
        Script s;
        net::Claims c = s.claims();
        c.taken_by_host(lake, knife, 0);
        expect("an offline take arriving late", c.take(lake, knife, 1, gone_at_host(), 20000), TakeAnswer::NotYours);
        c.clear();
        expect_taker("forgotten at the main menu", c.taker(lake, knife), -1);
        expect_taker("nothing kept", static_cast<int>(c.size()), 0);
    }
    {  // Dev `coin`: forced either way, then fair again.
        Script s{{0, 0}};
        net::Claims c = s.claims();
        c.force(1);
        c.taken_by_host(lake, knife, 0);
        expect("forced: the newcomer gets it", c.take(lake, knife, 1, gone_at_host(), 100), TakeAnswer::Yours, true,
               net::Claims::kHost);
        c.force(0);
        c.taken_by_host(lake, rope, 0);
        expect("forced: the earlier take keeps it", c.take(lake, rope, 1, gone_at_host(), 100), TakeAnswer::NotYours, true);
        c.force(-1);
        c.taken_by_host(cabin, rope, 0);
        expect("fair again (the script's coin)", c.take(cabin, rope, 1, gone_at_host(), 100), TakeAnswer::NotYours, true);
        expect_taker("the script was asked once", s.asked, 1);
    }
    {  // The real coin is fair: two players, and four.
        const int kRounds = 40000;
        int friend_wins = 0;
        int wins4[4] = {};
        net::Claims c;
        for (int i = 0; i < kRounds; ++i) {
            const std::string id = "item-" + std::to_string(i);
            c.taken_by_host(lake, id, 0);
            if (c.take(lake, id, 1, gone_at_host(), 300).answer == TakeAnswer::Yours) ++friend_wins;
            const std::string id4 = "four-" + std::to_string(i);
            c.taken_by_host(lake, id4, 0);
            for (uint8_t p = 1; p < 4; ++p) c.take(lake, id4, p, gone_at_host(), 100.0 * p);
            ++wins4[c.taker(lake, id4)];
        }
        const double share = static_cast<double>(friend_wins) / kRounds;
        check("two players: the friend wins about half", std::fabs(share - 0.5) < 0.02, std::to_string(share));
        bool fair = true;
        std::string shares;
        for (int p = 0; p < 4; ++p) {
            const double s4 = static_cast<double>(wins4[p]) / kRounds;
            fair = fair && std::fabs(s4 - 0.25) < 0.02;
            shares += (p ? ", " : "") + std::to_string(s4);
        }
        check("four players: each wins about a quarter", fair, shares);
    }
    std::printf("%s: %d failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
