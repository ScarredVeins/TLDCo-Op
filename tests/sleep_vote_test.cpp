// Sleeping together (net/sleep_vote.h, milestone 10 item 12): the hours each choice sleeps, and what the host decides
// from who is in bed and how they voted. Exit code 0 when every check holds.

#include <cmath>
#include <cstdio>
#include <string>

#include "net/sleep_vote.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

int main() {
    using net::SleepChoice;
    using Kind = net::SleepOutcome::Kind;
    {  // Hours.
        const net::SleepClock evening{22, 7, 19};
        expect("4, 8 and 12 hours are those", net::sleep_hours(SleepChoice::H4, evening) == 4 &&
                                                   net::sleep_hours(SleepChoice::H8, evening) == 8 &&
                                                   net::sleep_hours(SleepChoice::H12, evening) == 12);
        expect("till dawn from 22:00 (dawn 07:00) is 9 hours", near(net::sleep_hours(SleepChoice::Dawn, evening), 9));
        expect("till dusk from 10:00 (dusk 19:00) is 9 hours", near(net::sleep_hours(SleepChoice::Dusk, {10, 7, 19}), 9));
        expect("till dawn from 23:30 is 7.5 hours (past midnight)", near(net::sleep_hours(SleepChoice::Dawn, {23.5f, 7, 19}), 7.5f));
        expect("till dawn from 07:30 is the next one, 23.5 hours", near(net::sleep_hours(SleepChoice::Dawn, {7.5f, 7, 19}), 23.5f));
        expect("at dawn exactly, till dawn is a whole day, never 0", near(net::sleep_hours(SleepChoice::Dawn, {7, 7, 19}), 24));
        expect("no choice sleeps 0", net::sleep_hours(SleepChoice::None, evening) == 0);
    }
    const net::SleepClock k{22, 7, 19};  // till dawn: 9 h; till dusk: 21 h
    {  // Who is in bed.
        auto o = net::decide_sleep({{0, false, SleepChoice::None}, {1, false, SleepChoice::None}}, k);
        expect("nobody in bed: nothing to do", o.kind == Kind::Nobody && o.awake.empty());
        o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, false, SleepChoice::None}}, k);
        expect("one of two in bed: they wait, sleeping in real time", o.kind == Kind::Waiting);
        expect("...for the other", o.awake.size() == 1 && o.awake[0] == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, true, SleepChoice::None}}, k);
        expect("in bed without a vote yet counts as not yet", o.kind == Kind::Waiting && o.awake.size() == 1 && o.awake[0] == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H4, true}, {1, true, SleepChoice::Dawn}}, k);
        expect("one waiting to pass time, one in bed: both count, 4 hours", o.kind == Kind::Shared && o.choice == SleepChoice::H4);
        o = net::decide_sleep({{0, true, SleepChoice::H4, true}, {1, false, SleepChoice::None}}, k);
        expect("one waiting, one up: they wait for the other", o.kind == Kind::Waiting && o.awake.size() == 1 && o.awake[0] == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H8}}, k);
        expect("a host alone sleeps its choice", o.kind == Kind::Shared && o.choice == SleepChoice::H8 && o.hours == 8);
        o = net::decide_sleep({}, k);
        expect("nobody in the game: nothing", o.kind == Kind::Nobody);
    }
    {  // The vote.
        auto o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, true, SleepChoice::H8}, {2, true, SleepChoice::H4}}, k);
        expect("8, 8, 4: 8 hours", o.kind == Kind::Shared && o.choice == SleepChoice::H8 && o.hours == 8);
        expect("...with the votes counted", o.votes[2] == 2 && o.votes[1] == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H12}, {1, true, SleepChoice::H4}}, k);
        expect("4 against 12: the shorter, 4", o.choice == SleepChoice::H4 && o.hours == 4);
        o = net::decide_sleep({{0, true, SleepChoice::Dawn}, {1, true, SleepChoice::H8}}, k);
        expect("till dawn at 22:00 (9 h) against 8: 8", o.choice == SleepChoice::H8);
        o = net::decide_sleep({{0, true, SleepChoice::Dawn}, {1, true, SleepChoice::H8}}, {1, 7, 19});
        expect("till dawn at 01:00 (6 h) against 8: dawn, 6 hours", o.choice == SleepChoice::Dawn && near(o.hours, 6));
        o = net::decide_sleep({{0, true, SleepChoice::Dawn}, {1, true, SleepChoice::H8}}, {23, 7, 19});
        expect("till dawn 8 h away against 8: the same hours go to the fixed choice", o.choice == SleepChoice::H8);
        o = net::decide_sleep({{0, true, SleepChoice::Dusk}, {1, true, SleepChoice::Dusk}, {2, true, SleepChoice::H4},
                               {3, true, SleepChoice::H4}}, {10, 7, 19});
        expect("two for dusk (9 h), two for 4: 4", o.choice == SleepChoice::H4);
        o = net::decide_sleep({{0, true, SleepChoice::Dusk}, {1, true, SleepChoice::Dusk}, {2, true, SleepChoice::H4}}, {10, 7, 19});
        expect("two for dusk, one for 4: dusk, 9 hours", o.choice == SleepChoice::Dusk && near(o.hours, 9));
        o = net::decide_sleep({{0, true, static_cast<SleepChoice>(9)}, {1, true, SleepChoice::H4}}, k);
        expect("a vote that isn't a choice counts as no vote", o.kind == Kind::Waiting && o.awake[0] == 0);
    }
    {  // Resting (milestone 13): in bed to heal, no vote for a sleep.
        expect("REST is a vote on the wire, and its name", net::is_sleep_choice(6) && !net::is_sleep_choice(7) &&
                                                               std::string(net::sleep_choice_name(SleepChoice::Rest)) == "REST");
        expect("REST doesn't skip time", !net::skips_time(SleepChoice::Rest) && net::skips_time(SleepChoice::H4) &&
                                             !net::skips_time(SleepChoice::None));
        expect("a rest lasts kRestHours at most", net::sleep_hours(SleepChoice::Rest, k) == net::kRestHours);
        auto o = net::decide_sleep({{0, true, SleepChoice::Rest}, {1, false, SleepChoice::None}}, k);
        expect("one resting, one up: nothing to do (nobody is in for a sleep)",
               o.kind == Kind::Nobody && o.resting.size() == 1 && o.resting[0] == 0);
        o = net::decide_sleep({{0, true, SleepChoice::Rest}}, k);
        expect("a host alone resting doesn't sleep", o.kind == Kind::Nobody);
        o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, true, SleepChoice::Rest}}, k);
        expect("one in for 8 hours, one resting: the sleep waits for the one resting",
               o.kind == Kind::Waiting && o.awake.size() == 1 && o.awake[0] == 1 && o.resting.size() == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, true, SleepChoice::H4}, {2, true, SleepChoice::Rest}}, k);
        expect("REST is no vote: none counted for it", o.kind == Kind::Waiting && o.votes[6] == 0 && o.votes[2] == 1);
        o = net::decide_sleep({{0, true, SleepChoice::H8}, {1, true, SleepChoice::H8}}, k);
        expect("once the one resting picks a sleep: everyone sleeps", o.kind == Kind::Shared && o.choice == SleepChoice::H8);
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "ALL PASS");
    return g_failed ? 1 : 0;
}
