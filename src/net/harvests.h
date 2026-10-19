#pragma once
// Milestone 10: carcass harvests made while the link is down. A friend's Harvest (and Quartered) is kept like a world
// change (Session::keep_offline) and goes once the link is back. One sent just before the link dropped goes again
// then (the session can't know whether the host got it), so the host counts each harvest once, by its number.
// Plain logic (no IL2CPP): unit test tests/harvests_test.cpp.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace net {

// Client: the first harvest number of a game run, from something that differs between runs (a clock). Numbers start
// somewhere new each run so a host that counted an earlier run's (or another friend's) never takes ours for those.
// Never 0 (0 is "none counted yet").
inline uint32_t first_harvest_seq(uint64_t entropy) {
    uint64_t z = entropy + 0x9e3779b97f4a7c15ull;  // splitmix64
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    const uint32_t seq = static_cast<uint32_t>(z);
    return seq ? seq : 1;
}

// Client: the number after `seq`, skipping 0.
inline uint32_t next_harvest_seq(uint32_t seq) { return seq + 1 ? seq + 1 : 1; }

// Client: the host's harvest_ack for a carcass (0: none of ours counted on this link yet) covers our harvest `pending`.
inline bool harvest_counted(uint32_t ack, uint32_t pending) {
    return ack != 0 && static_cast<int32_t>(ack - pending) >= 0;
}

// Host: the friends' harvests counted so far (the newest kMax).
class HarvestLedger {
public:
    static constexpr size_t kMax = 1024;

    // True the first time harvest `seq` comes (count it); false when it came before (already counted).
    bool first_time(uint32_t seq) {
        if (!seen_.insert(seq).second) return false;
        order_.push_back(seq);
        if (order_.size() > kMax) {
            seen_.erase(order_.front());
            order_.pop_front();
        }
        return true;
    }
    void clear() {
        seen_.clear();
        order_.clear();
    }
    size_t size() const { return seen_.size(); }

private:
    std::unordered_set<uint32_t> seen_;
    std::deque<uint32_t> order_;
};

}  // namespace net
