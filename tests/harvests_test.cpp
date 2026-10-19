// Carcass harvests made while the link is down (net/harvests.h, milestone 10): the client's harvest numbers and the
// host counting each harvest once when one comes again after the link is back. Exit code 0 when every check holds.

#include <cstdio>
#include <set>

#include "net/harvests.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

}  // namespace

int main() {
    {  // Numbers: never 0, a new start each run, and counting on past the top.
        bool zero = false;
        std::set<uint32_t> starts;
        for (uint64_t clock = 0; clock < 1000; ++clock) {
            const uint32_t s = net::first_harvest_seq(clock);
            zero |= s == 0;
            starts.insert(s);
        }
        expect("a run's first harvest number is never 0", !zero);
        expect("1000 runs start at 1000 different numbers", starts.size() == 1000);
        expect("the next number counts up", net::next_harvest_seq(41) == 42);
        expect("after the top comes 1, not 0", net::next_harvest_seq(0xffffffffu) == 1);
    }
    {  // What the host's ack says about our harvest still waiting to be counted.
        expect("no ack yet (a new link): not counted", !net::harvest_counted(0, 7));
        expect("no ack yet, a number past half the range: still not counted", !net::harvest_counted(0, 0x90000000u));
        expect("the ack is our harvest: counted", net::harvest_counted(7, 7));
        expect("the ack is a later harvest of ours: counted", net::harvest_counted(9, 7));
        expect("the ack is an earlier one: not yet", !net::harvest_counted(6, 7));
        expect("counted across the wrap (ours 0xffffffff, the ack 1)", net::harvest_counted(1, 0xffffffffu));
        expect("not counted across the wrap (ours 1, the ack 0xffffffff)", !net::harvest_counted(0xffffffffu, 1));
    }
    {  // The host: a harvest sent just before the link dropped comes again once it's back.
        net::HarvestLedger ledger;
        const uint32_t first = net::first_harvest_seq(12345);
        const uint32_t second = net::next_harvest_seq(first);
        expect("the friend's harvest: counted", ledger.first_time(first));
        expect("the one made while the link was down: counted", ledger.first_time(second));
        expect("the first one again (its ack was lost): not counted again", !ledger.first_time(first));
        expect("another friend's harvest (another run's numbers): counted",
               ledger.first_time(net::first_harvest_seq(67890)));
        expect("three harvests kept", ledger.size() == 3);
        ledger.clear();
        expect("cleared", ledger.size() == 0 && ledger.first_time(first));
    }
    {  // Only the newest kMax are kept: the oldest is forgotten.
        net::HarvestLedger ledger;
        for (uint32_t s = 1; s <= net::HarvestLedger::kMax + 1; ++s) ledger.first_time(s);
        expect("at most kMax kept", ledger.size() == net::HarvestLedger::kMax);
        expect("the newest is still known", !ledger.first_time(net::HarvestLedger::kMax + 1));
        expect("the oldest is forgotten", ledger.first_time(1));
    }
    std::printf("%s: %d failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
