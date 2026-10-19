#pragma once
// Handing an item to a friend (milestone 13). The giver walks up to a friend, picks an item of theirs and holds it out
// (their arm goes out and waits); the friend takes it (their arm reaches, both hands on it, the giver lets go) and
// it's in the friend's pack. The animation is the user's pair of clips (D:\TLD Co-op Animation\Receiving,
// handover_pairs.json): give and receive on one 4.2 s timeline, for food, wood and tools, each receive clip matched to
// the giver's rig (male or female). This is the plain part: which clips, the timeline as a function of when the offer
// and the acceptance were heard, where the two stand, how the item sits in each hand, and a ledger of the handovers
// in the game (what may happen next to each). No IL2CPP (handover_test).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace net {

// Which grip the clips use: food (a bar held near one end), wood (a log held in the middle), anything else (a tool
// by its handle).
enum class HandItem : uint8_t { Tool = 0, Food = 1, Wood = 2 };
constexpr uint8_t kHandItems = 3;

inline HandItem hand_item(uint8_t wire) { return wire < kHandItems ? static_cast<HandItem>(wire) : HandItem::Tool; }

inline const char* hand_item_name(HandItem k) {
    switch (k) {
        case HandItem::Food: return "food";
        case HandItem::Wood: return "wood";
        default: return "tool";
    }
}

// From TLD's gear: food (it has a FoodItem: eaten or drunk) or one of TLD's wood fuels, else a tool's grip.
inline HandItem hand_item_for(const std::string& prefab, bool is_food) {
    if (is_food) return HandItem::Food;
    for (const char* wood : {"Firelog", "Hardwood", "Softwood", "ReclaimedWood", "Stick", "Limb", "Branch", "Wood"})
        if (prefab.find(wood) != std::string::npos) return HandItem::Wood;
    return HandItem::Tool;
}

// The clips' names on a body (the avatar adds them under these from tldcoop_handover.bundle). The receive clip is
// the one matched to the giver's rig.
inline const char* give_clip(HandItem k) {
    switch (k) {
        case HandItem::Food: return "give_food";
        case HandItem::Wood: return "give_wood";
        default: return "give";
    }
}
inline const char* receive_clip(HandItem k, bool giver_female) {
    switch (k) {
        case HandItem::Food: return giver_female ? "receive_food_from_f" : "receive_food_from_m";
        case HandItem::Wood: return giver_female ? "receive_wood_from_f" : "receive_wood_from_m";
        default: return giver_female ? "receive_from_f" : "receive_from_m";
    }
}

// The clips' marks (handover_pairs.json), seconds on their shared timeline.
constexpr float kHandoverLength = 4.2f;  // both clips
constexpr float kHoldAt = 2.17f;         // both hands on the item, the giver's not open yet: an offer waits here
constexpr float kTransferAt = 2.25f;     // it's the receiver's: from here in their hand
constexpr float kReachFrom = 0.35f;      // the receive clip stands still before this: an acceptance starts it here
constexpr float kHandoverFade = 0.3f;    // into and out of the clips
constexpr float kCarrySeconds = 0.5f;    // the receiver holds the clip's last pose this long, then stows it
constexpr double kOfferTimeoutMs = 30000;  // an offer nobody takes is taken back
constexpr float kGiveRange = 2.5f;        // how close a friend must be to give them something
constexpr float kOfferRange = 5.0f;       // farther apart than this, an offer is taken back

// What each body shows `now` (ms, any clock, the same as the others): an offer heard at `offered_ms`, accepted at
// `accepted_ms` (0: not yet), over at `ended_ms` (0: not yet; `done`: the item changed hands, else it was taken back
// or couldn't be given).
struct HandoverView {
    float giver_t = -1;        // the give clip's time on the giver (-1: not in it)
    float receiver_t = -1;     // the receive clip's time on the receiver (-1: not in it)
    bool in_receiver = false;  // the item is in the receiver's hand (past the transfer mark), else in the giver's
    bool item_shown = false;   // the item in a hand at all
    bool fading = false;       // the clips fade out (the handover ends): showing their last times
    bool over = false;         // nothing left to show: the ledger may forget it
};

// The giver's clip goes out and waits at kHoldAt. Accepted, the receiver's starts at kReachFrom; once it is at kHoldAt
// too (and the giver's is), both run on together from there: the item is the receiver's from kTransferAt, the giver's
// clip ends at kHandoverLength and the receiver holds its last pose kCarrySeconds more. Taken back (or failed) before
// the transfer: the clips fade from where they were.
inline HandoverView handover_view(double offered_ms, double accepted_ms, double ended_ms, bool done, double now) {
    HandoverView v;
    const auto s = [](double ms) { return static_cast<float>(ms / 1000.0); };
    const float age = s(now - offered_ms);
    if (accepted_ms <= 0) {
        const float shown = std::min(ended_ms > 0 ? s(ended_ms - offered_ms) : age, kHoldAt);
        if (ended_ms > 0) {
            if (s(now - ended_ms) >= kHandoverFade) {
                v.over = true;
                return v;
            }
            v.fading = true;
        }
        v.giver_t = std::max(0.0f, shown);
        v.item_shown = true;
        return v;
    }
    // When both run on from the hold: the receiver has reached it, and the giver too.
    const double go = std::max(accepted_ms + (kHoldAt - kReachFrom) * 1000.0, offered_ms + kHoldAt * 1000.0);
    const auto times_at = [&](double t, float& giver, float& receiver) {
        if (t < go) {
            giver = std::min(s(t - offered_ms), kHoldAt);
            receiver = std::min(kReachFrom + std::max(0.0f, s(t - accepted_ms)), kHoldAt);
        } else {
            giver = receiver = kHoldAt + s(t - go);
        }
    };
    float giver = 0, receiver = 0;
    times_at(now, giver, receiver);
    const bool transferred_by = [&](double t) {
        float g = 0, r = 0;
        times_at(t, g, r);
        return t >= go && g >= kTransferAt;
    }(ended_ms > 0 && !done ? ended_ms : now);
    if (ended_ms > 0 && !done && !transferred_by) {  // it couldn't be given: both fade from where they were
        if (s(now - ended_ms) >= kHandoverFade) {
            v.over = true;
            return v;
        }
        times_at(ended_ms, giver, receiver);
        v.fading = true;
        v.giver_t = giver;
        v.receiver_t = receiver;
        v.item_shown = true;
        return v;
    }
    const float past = giver - kHandoverLength;  // seconds since the clips ended
    if (past >= kCarrySeconds + kHandoverFade) {
        v.over = true;
        return v;
    }
    v.in_receiver = giver >= kTransferAt;
    v.item_shown = past < kCarrySeconds;
    v.giver_t = giver < kHandoverLength ? giver : -1;
    v.receiver_t = std::min(receiver, kHandoverLength);
    v.fading = past >= kCarrySeconds;
    return v;
}

// --- where they stand ----------------------------------------------------------------------------------------------

// How far apart the clips have them (handover_pairs.json receiver_root_position.z: the female rig is smaller).
inline float pair_distance(bool giver_female, bool receiver_female) {
    if (giver_female == receiver_female) return 0.88f;
    return giver_female ? 0.84f : 0.92f;
}

// Where a body is drawn during a handover (world x, z; yaw in degrees, Unity's: 0 towards +z, 90 towards +x): facing
// its partner, `distance` from them. `partner_moves`: the partner is drawn too (both are others' avatars, as a third
// player sees them): each goes half the way; else (the partner is us, in first person) this one goes all of it.
struct Spot {
    float x = 0, z = 0, yaw = 0;
};
inline Spot handover_spot(float self_x, float self_z, float partner_x, float partner_z, float distance,
                          bool partner_moves) {
    float dx = self_x - partner_x, dz = self_z - partner_z;
    float d = std::sqrt(dx * dx + dz * dz);
    if (d < 1e-3f) dx = 0, dz = 1, d = 1;
    dx /= d, dz /= d;
    Spot sp;
    if (partner_moves) {
        const float mx = (self_x + partner_x) * 0.5f, mz = (self_z + partner_z) * 0.5f;
        sp.x = mx + dx * distance * 0.5f;
        sp.z = mz + dz * distance * 0.5f;
    } else {
        sp.x = partner_x + dx * distance;
        sp.z = partner_z + dz * distance;
    }
    sp.yaw = std::atan2(-dx, -dz) * 57.2957795f;  // towards the partner
    return sp;
}

// Looking at a friend: where a ray (from `o`, along the unit `d`) meets an upright body (a cylinder `radius` round and
// `height` tall, its feet at x, y, z), metres along the ray; -1 if it passes by or is farther than `range`.
inline float ray_hits_body(const float o[3], const float d[3], float x, float y, float z, float radius, float height,
                           float range) {
    const float px = o[0] - x, pz = o[2] - z;
    const float a = d[0] * d[0] + d[2] * d[2], b = 2 * (px * d[0] + pz * d[2]), c = px * px + pz * pz - radius * radius;
    float t0 = 0, t1 = range;
    if (a < 1e-6f) {  // looking straight up or down
        if (c > 0) return -1;
    } else {
        const float disc = b * b - 4 * a * c;
        if (disc < 0) return -1;
        const float root = std::sqrt(disc);
        t0 = (-b - root) / (2 * a);
        t1 = (-b + root) / (2 * a);
    }
    if (t1 < 0) return -1;
    t0 = std::max(t0, 0.0f);
    t1 = std::min(t1, range);
    if (t0 > t1) return -1;
    // Within the cylinder's height somewhere between entering and leaving it.
    const float y0 = o[1] + d[1] * t0 - y, y1 = o[1] + d[1] * t1 - y;
    if (std::max(y0, y1) < 0 || std::min(y0, y1) > height) return -1;
    if (y0 >= 0 && y0 <= height) return t0;
    if (std::fabs(d[1]) < 1e-6f) return -1;
    const float t = (y0 > height ? height : 0) - (o[1] - y);  // through the top or the bottom
    return std::clamp(t / d[1], t0, t1);
}

// --- how the item sits in each hand --------------------------------------------------------------------------------

// Poses in a hand bone's space (metres; quaternion x, y, z, w). The clips' stand-in props lie along their own +x with
// the giver's grip at their origin; a real item is laid the same way (its long axis along the prop's +x, its grip at
// the origin), grip_along of a half length from its middle.
struct HandPose {
    float pos[3];
    float rot[4];
};
constexpr HandPose kGiverGrip = {{-0.05f, -0.105f, 0.0f}, {0.0f, -0.70710678f, 0.0f, 0.70710678f}};  // RightHand_jnt

// Where along the item the giver holds it, in half lengths from its middle (+: towards its front or top): food near
// its back end, wood in the middle, a tool by its handle.
inline float grip_along(HandItem k) {
    switch (k) {
        case HandItem::Food: return -0.6f;
        case HandItem::Wood: return 0.0f;
        default: return -0.45f;
    }
}

// The item in the receiver's left hand from the transfer on (LeftHand_jnt; handover_pairs.json
// receiver_item_local_*: the giver's grip carried over at kTransferAt), by both rigs and the grip.
inline HandPose receiver_grip(bool giver_female, bool receiver_female, HandItem k) {
    struct Entry {
        bool giver_female, receiver_female;
        HandItem item;
        HandPose pose;
    };
    static const Entry kPairs[] = {
        {false, false, HandItem::Food, {{0.0482f, 0.0686f, 0.1239f}, {-0.0244f, -0.6804f, -0.1713f, -0.7121f}}},
        {false, false, HandItem::Wood, {{0.0459f, 0.0295f, -0.1414f}, {0.2395f, -0.7092f, 0.1006f, -0.6554f}}},
        {false, true, HandItem::Food, {{0.0585f, 0.0628f, 0.1225f}, {-0.0040f, -0.6916f, -0.2332f, -0.6835f}}},
        {false, true, HandItem::Wood, {{0.0628f, 0.0422f, -0.1463f}, {0.2489f, -0.6770f, 0.0409f, -0.6914f}}},
        {true, false, HandItem::Food, {{0.0486f, 0.0885f, 0.1278f}, {0.0787f, -0.6831f, -0.1642f, -0.7072f}}},
        {true, false, HandItem::Wood, {{0.0453f, 0.0043f, -0.1245f}, {0.3433f, -0.7076f, 0.1186f, -0.6061f}}},
        {true, true, HandItem::Food, {{0.0556f, 0.0765f, 0.1264f}, {0.0771f, -0.6825f, -0.2356f, -0.6876f}}},
        {true, true, HandItem::Wood, {{0.0677f, 0.0188f, -0.1333f}, {0.3464f, -0.6642f, 0.0602f, -0.6597f}}},
    };
    const HandItem grip = k == HandItem::Tool ? HandItem::Wood : k;  // a tool's pose is the wood's (the same grip)
    for (const Entry& e : kPairs)
        if (e.giver_female == giver_female && e.receiver_female == receiver_female && e.item == grip) return e.pose;
    return kPairs[1].pose;
}

// --- the ledger ----------------------------------------------------------------------------------------------------

struct Handover {
    uint32_t id = 0;            // the giver's: their player id in the top byte, their count below
    uint8_t giver = 0, receiver = 0;
    HandItem item = HandItem::Tool;
    std::string gear;           // the prefab held out (GEAR_...)
    int32_t units = 1;          // how many of it (a stack's units)
    double offered_ms = 0;      // when we heard of it (our clock)
    double accepted_ms = 0;     // when we heard it was taken (0: not yet)
    double ended_ms = 0;        // when we heard it's over (0: still open)
    bool done = false;          // over because the item changed hands
    bool taken = false;         // the receiver: it went into our pack (once)
};

inline uint32_t handover_id(uint8_t giver, uint32_t count) { return (static_cast<uint32_t>(giver) << 24) | (count & 0xFFFFFF); }
inline uint8_t handover_giver(uint32_t id) { return static_cast<uint8_t>(id >> 24); }

// Every handover this game knows of: each player's own, and the others' it heard of (for their avatars).
class Handovers {
public:
    // A new offer. False: one we know already, or the giver or receiver is in another that's still open.
    bool offer(const Handover& h) {
        if (h.giver == h.receiver || all_.count(h.id)) return false;
        if (open_of(h.giver) || open_of(h.receiver)) return false;
        all_[h.id] = h;
        return true;
    }
    Handover* find(uint32_t id) {
        const auto it = all_.find(id);
        return it == all_.end() ? nullptr : &it->second;
    }
    const Handover* find(uint32_t id) const {
        const auto it = all_.find(id);
        return it == all_.end() ? nullptr : &it->second;
    }
    // It was taken: the receiver reaches. False: unknown, taken already, or over.
    bool accept(uint32_t id, double now) {
        Handover* h = find(id);
        if (!h || h->accepted_ms > 0 || h->ended_ms > 0) return false;
        h->accepted_ms = now;
        return true;
    }
    // It's over: `done` the item changed hands (only once accepted), else taken back or failed. False: unknown, or
    // over already.
    bool end(uint32_t id, double now, bool done) {
        Handover* h = find(id);
        if (!h || h->ended_ms > 0 || (done && h->accepted_ms <= 0)) return false;
        h->ended_ms = now;
        h->done = done;
        return true;
    }
    // The receiver: the item goes into our pack. True once, and only once it's done.
    bool take(uint32_t id) {
        Handover* h = find(id);
        if (!h || !h->done || h->taken) return false;
        h->taken = true;
        return true;
    }
    // The still open one this player gives or receives in (nullptr: none).
    const Handover* open_of(uint8_t player) const {
        for (const auto& [id, h] : all_)
            if (h.ended_ms <= 0 && (h.giver == player || h.receiver == player)) return &h;
        return nullptr;
    }
    // The one this player's body shows now (open, or still playing out), newest first.
    const Handover* shown_for(uint8_t player, double now) const {
        const Handover* best = nullptr;
        for (const auto& [id, h] : all_) {
            if (h.giver != player && h.receiver != player) continue;
            if (handover_view(h.offered_ms, h.accepted_ms, h.ended_ms, h.done, now).over) continue;
            if (!best || h.offered_ms > best->offered_ms) best = &h;
        }
        return best;
    }
    // Open offers by `giver` heard longer than `timeout_ms` ago and not taken: to take back.
    std::vector<uint32_t> stale(uint8_t giver, double now, double timeout_ms) const {
        std::vector<uint32_t> out;
        for (const auto& [id, h] : all_)
            if (h.giver == giver && h.ended_ms <= 0 && h.accepted_ms <= 0 && now - h.offered_ms > timeout_ms)
                out.push_back(id);
        return out;
    }
    // Ends every open one with this player in it (they left, or went elsewhere); returns their ids.
    std::vector<uint32_t> end_all_of(uint8_t player, double now) {
        std::vector<uint32_t> out;
        for (auto& [id, h] : all_)
            if (h.ended_ms <= 0 && (h.giver == player || h.receiver == player)) {
                h.ended_ms = now;
                out.push_back(id);
            }
        return out;
    }
    // Forgets the ones over for a while (a late message about one is then unknown, and ignored).
    void prune(double now, double keep_ms = 60000) {
        for (auto it = all_.begin(); it != all_.end();) {
            const Handover& h = it->second;
            if (h.ended_ms > 0 && now - h.ended_ms > keep_ms)
                it = all_.erase(it);
            else
                ++it;
        }
    }
    void clear() { all_.clear(); }
    const std::map<uint32_t, Handover>& all() const { return all_; }

private:
    std::map<uint32_t, Handover> all_;
};

}  // namespace net
