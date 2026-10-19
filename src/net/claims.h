#pragma once
// Who gets a loose item two players took at about the same time (milestone 10). Each game takes it as soon as its
// player does and tells the others (ItemGone Taken), so two takes can cross: both would keep one (a copy). The host
// decides. Takes at the same moment (each player took it before hearing of the other's take: heard within
// kSameMomentMs of the first) go to a coin toss, fair to each (the user's call, 2026-10-17: not always the host); a
// take heard later than that was second and is refused (the item leaves that player's pack). The host's own take
// counts from the moment it happens in its game, even before it's sent. The host having it in its hands (TLD's Take
// it / Leave it) counts as its take: picked up at the same moment, a toss; before, it was first.
//
// Kept until the item lies in the world again (someone drops it) or the session's world is forgotten (main menu), so a
// friend's take made while its link was down (sent when it's back) loses to one the host heard of meanwhile.
// Plain code (no IL2CPP): the host's world_items feeds it TLD's facts and does what it answers.

#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <utility>

namespace net {

enum class TakeAnswer : uint8_t {
    Yours,     // the taker keeps it (and the others hear it's gone)
    PutBack,   // the host has it in its hands, undecided (TLD's Take it / Leave it): back where it lay for the taker
    NotYours,  // someone else's take came first, or won the coin toss: out of the taker's pack
};

// What the host's game says about the item when a friend's take arrives.
struct TakeFacts {
    bool host_scene = false;  // the item's scene is the one the host is in (the rest is about the host's game)
    bool loose = false;       // it lies there, nobody holding it
    bool held = false;        // the host has it in its hands (picked up, not yet taken or left)
    bool known = false;       // the host saw it lying there at its last look (so it left since: the host took it)
    double held_since_ms = -1e18;  // held: since when (the same clock as take's now_ms)
};

struct TakeDecision {
    TakeAnswer answer = TakeAnswer::Yours;
    bool tossed = false;  // takes at the same moment: the coin decided
    int loser = -1;       // Yours by the coin: whose earlier take is undone (Claims::kHost or a friend); else -1
};

class Claims {
public:
    static constexpr uint8_t kHost = 0;
    // Each game sees its player's take within half a second (its look at the items) and the link adds its delay.
    static constexpr double kSameMomentMs = 1000;

    // pick(n): a number in [0, n), each as likely (tests give their own).
    explicit Claims(std::function<uint32_t(uint32_t)> pick = {}) : pick_(std::move(pick)) {
        if (!pick_) pick_ = [rng = std::mt19937(std::random_device{}())](uint32_t n) mutable {
            return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng);
        };
    }

    // A friend's take, heard at now_ms. Yours: remembered as theirs (and `loser`'s earlier take is undone).
    TakeDecision take(const std::string& scene, const std::string& id, uint8_t player, const TakeFacts& facts,
                      double now_ms) {
        const auto key = std::make_pair(scene, id);
        auto it = taker_.find(key);
        if (it == taker_.end()) {
            if (facts.host_scene && facts.held) {
                // In the host's hands (picked up, not yet taken or left): it was first, unless that was at the same moment.
                if (now_ms - facts.held_since_ms > kSameMomentMs) return {TakeAnswer::PutBack};
                if (pick(2) == 0) return {TakeAnswer::PutBack, true};  // the host's (it may still leave it)
                taker_[key] = Claim{player, now_ms, bit(kHost) | bit(player)};
                return {TakeAnswer::Yours, true, kHost};  // the host leaves it
            }
            if (!(facts.host_scene && !facts.loose && facts.known)) {
                taker_[key] = Claim{player, now_ms, bit(player)};
                return {TakeAnswer::Yours};
            }
            // The host took it just now (its own word is on the way): the same moment as this one.
            it = taker_.emplace(key, Claim{kHost, now_ms, bit(kHost)}).first;
        }
        Claim& c = it->second;
        if (c.taker == player) return {TakeAnswer::Yours};          // the same player again: resent
        if (c.contenders & bit(player)) return {TakeAnswer::NotYours};  // resent after losing the toss
        if (now_ms - c.first_ms > kSameMomentMs) return {TakeAnswer::NotYours};  // the first take was before this one
        // The newcomer wins with 1 chance in (takers so far + 1): with two players a coin, and fair to each with more.
        const uint32_t before = static_cast<uint32_t>(std::popcount(c.contenders));
        c.contenders |= bit(player);
        TakeDecision d;
        d.tossed = true;
        if (pick(before + 1) == before) {
            d.loser = c.taker;
            c.taker = player;
        } else {
            d.answer = TakeAnswer::NotYours;
        }
        return d;
    }

    // The host's own take (its look found it in the pack), at now_ms. One already decided stays as it is.
    void taken_by_host(const std::string& scene, const std::string& id, double now_ms) {
        taker_.try_emplace(std::make_pair(scene, id), Claim{kHost, now_ms, bit(kHost)});
    }

    // It lies in the world again (dropped, put back): the next take is a new one.
    void lies_again(const std::string& scene, const std::string& id) { taker_.erase(std::make_pair(scene, id)); }

    // Whose take it was, or -1 if nobody's is remembered.
    int taker(const std::string& scene, const std::string& id) const {
        const auto it = taker_.find(std::make_pair(scene, id));
        return it == taker_.end() ? -1 : it->second.taker;
    }

    void clear() { taker_.clear(); }
    size_t size() const { return taker_.size(); }

    // Tests in game (dev `coin`): -1 a fair coin, 0 the earlier take (or hold) keeps it, 1 the newcomer gets it.
    void force(int which) { forced_ = which; }

private:
    struct Claim {
        uint8_t taker = kHost;
        double first_ms = 0;      // when the first take was heard
        uint32_t contenders = 0;  // everyone whose take was in the toss (bit per player)
    };
    static uint32_t bit(uint8_t player) { return 1u << player; }
    uint32_t pick(uint32_t n) { return forced_ < 0 ? pick_(n) : forced_ ? n - 1 : 0; }

    std::map<std::pair<std::string, std::string>, Claim> taker_;  // (scene, item id) -> who took it
    std::function<uint32_t(uint32_t)> pick_;
    int forced_ = -1;
};

}  // namespace net
