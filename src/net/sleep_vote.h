#pragma once
// Sleeping together (milestone 10 item 12): the time skip only when every player is in (in bed, or waiting to pass
// time), for the hours they vote for on one shared screen (the user: 4 h, 8 h, 12 h, Till Dawn, Till Dusk). REST
// (milestone 13) lies in bed in real time to heal a little faster, for whoever wants that and not a sleep: it never
// skips time, and a player resting is up as far as the skip goes. This is the plain part: the hours each choice is,
// and what the host decides from who is in and how they voted. No IL2CPP.

#include <cmath>
#include <cstdint>
#include <vector>

namespace net {

enum class SleepChoice : uint8_t { None = 0, H4 = 1, H8 = 2, H12 = 3, Dawn = 4, Dusk = 5, Rest = 6 };
constexpr uint8_t kSleepChoices = 7;
constexpr float kRestHours = 12;  // a rest lasts this long at most (in real time), unless they get up first

inline bool is_sleep_choice(uint8_t v) { return v > 0 && v < kSleepChoices; }  // a vote (REST too)
inline bool skips_time(SleepChoice c) { return c != SleepChoice::None && c != SleepChoice::Rest; }

// As the screen shows them.
inline const char* sleep_choice_name(SleepChoice c) {
    switch (c) {
        case SleepChoice::H4: return "4 HOURS";
        case SleepChoice::H8: return "8 HOURS";
        case SleepChoice::H12: return "12 HOURS";
        case SleepChoice::Dawn: return "TILL DAWN";
        case SleepChoice::Dusk: return "TILL DUSK";
        case SleepChoice::Rest: return "REST";
        default: return "NO VOTE";
    }
}

// The host's clock: hours since midnight now, and TLD's dawn (daylight starts: UniStorm.GetDawnTime) and dusk (its
// dusk window starts) that day.
struct SleepClock {
    float now_h = 0, dawn_h = 7, dusk_h = 19;
};

// How long a choice sleeps from now: 4, 8 or 12, or until the next dawn or dusk (never 0: just past it, a day); a rest
// at most kRestHours.
inline float sleep_hours(SleepChoice c, const SleepClock& k) {
    switch (c) {
        case SleepChoice::H4: return 4;
        case SleepChoice::H8: return 8;
        case SleepChoice::H12: return 12;
        case SleepChoice::Dawn:
        case SleepChoice::Dusk: {
            float h = std::fmod((c == SleepChoice::Dawn ? k.dawn_h : k.dusk_h) - k.now_h + 48.0f, 24.0f);
            return h < 1.0f / 60 ? h + 24 : h;
        }
        case SleepChoice::Rest: return kRestHours;
        default: return 0;
    }
}

struct Sleeper {
    uint8_t player = 0;
    bool in_bed = false;           // in through co-op's screen: in bed or waiting (in real time while others are up)
    SleepChoice vote = SleepChoice::None;
    bool waiting = false;          // in to pass time, not in bed (the screen says so; the decision is the same)
};

struct SleepOutcome {
    enum class Kind : uint8_t {
        Nobody,   // nobody is in for a sleep (resting is no vote for one)
        Waiting,  // someone is: their sleep or waiting runs in real time until everyone is in (`awake`: who isn't)
        Shared,   // everyone is in and has voted: everyone sleeps or passes time `choice`, `hours` of it
    } kind = Kind::Nobody;
    SleepChoice choice = SleepChoice::None;
    float hours = 0;
    std::vector<uint8_t> awake;    // those not in for the skip: up, or resting
    std::vector<uint8_t> resting;  // of them, those resting
    uint8_t votes[kSleepChoices] = {};  // how many voted for each choice that skips time
};

// Who is in the game (the host too) and how they voted. The most votes wins; a tie goes to the shorter sleep, and
// the same hours to the fixed choice (the earlier one in the list). Resting is no vote for a sleep: nobody resting
// starts one, and while anyone rests the others' sleep waits.
inline SleepOutcome decide_sleep(const std::vector<Sleeper>& players, const SleepClock& k) {
    SleepOutcome o;
    bool anyone = false;
    for (const Sleeper& p : players) {
        const bool voted = p.in_bed && skips_time(p.vote) && is_sleep_choice(static_cast<uint8_t>(p.vote));
        if (voted) {
            anyone = true;
            ++o.votes[static_cast<uint8_t>(p.vote)];
        } else {
            o.awake.push_back(p.player);
            if (p.in_bed && p.vote == SleepChoice::Rest) o.resting.push_back(p.player);
        }
    }
    if (!anyone) {
        o.awake.clear();
        return o;
    }
    if (!o.awake.empty()) {
        o.kind = SleepOutcome::Kind::Waiting;
        return o;
    }
    for (uint8_t c = 1; c < kSleepChoices; ++c) {
        if (!o.votes[c] || !skips_time(static_cast<SleepChoice>(c))) continue;
        const auto choice = static_cast<SleepChoice>(c);
        const float hours = sleep_hours(choice, k);
        if (o.choice == SleepChoice::None || o.votes[c] > o.votes[static_cast<uint8_t>(o.choice)] ||
            (o.votes[c] == o.votes[static_cast<uint8_t>(o.choice)] && hours < o.hours)) {
            o.choice = choice;
            o.hours = hours;
        }
    }
    o.kind = SleepOutcome::Kind::Shared;
    return o;
}

}  // namespace net
