#pragma once
// Snapshot interpolation for a peer's state stream, timed by the sender's capture timestamps (StateMsg::t_ms).
//
// Timing by arrival time instead makes motion stop-and-go: sender sampling and the 20 Hz send clock aren't in
// step, so some packets repeat a sample and others skip one. With sender timestamps, duplicates are dropped and
// uneven spacing is replayed exactly. The sender->local clock offset is the smallest (receive - send) seen in a
// recent window, i.e. the fastest delivery; `delay_ms` on top absorbs jitter.
//
// Used by both the mod (avatar) and the test bot, so the bot can't add wobble of its own.

#include <algorithm>
#include <cmath>
#include <deque>

#include "net/protocol.h"

namespace net {

class Interpolator {
public:
    explicit Interpolator(double delay_ms) : delay_ms_(delay_ms) {}

    void add(const StateMsg& s, double received_ms) {
        // A long silence means a new connection, maybe from a restarted peer with a different clock base.
        if (!buf_.empty() && received_ms - last_received_ms_ > kGapResetMs) clear();
        if (!buf_.empty() && s.t_ms <= buf_.back().t_ms) return;  // duplicate or out of order

        offsets_.push_back(received_ms - s.t_ms);
        if (offsets_.size() > kOffsetWindow) offsets_.pop_front();
        offset_ = *std::min_element(offsets_.begin(), offsets_.end());

        buf_.push_back(s);
        if (buf_.size() > kBufferLimit) buf_.pop_front();
        last_received_ms_ = received_ms;
    }

    // The peer's state `delay_ms` before local time `now_ms`. False until something has been received.
    bool sample(double now_ms, StateMsg& out) {
        if (buf_.empty()) return false;
        const double t = now_ms - offset_ - delay_ms_;  // on the sender's clock
        while (buf_.size() >= 2 && buf_[1].t_ms <= t) buf_.pop_front();

        const StateMsg& a = buf_.front();
        if (buf_.size() < 2 || t <= a.t_ms) {
            out = a;  // before the first sample, or holding the newest while waiting for more
            return true;
        }
        const StateMsg& b = buf_[1];
        if (a.scene != b.scene || !a.valid || !b.valid || distance(a, b) > kTeleportMetres) {
            out = b;  // region change or teleport: jump, don't glide through the world
            return true;
        }
        const float f = static_cast<float>((t - a.t_ms) / (b.t_ms - a.t_ms));
        out = b;
        out.x = a.x + (b.x - a.x) * f;
        out.y = a.y + (b.y - a.y) * f;
        out.z = a.z + (b.z - a.z) * f;
        out.yaw = lerp_angle(a.yaw, b.yaw, f);
        out.pitch = a.pitch + (b.pitch - a.pitch) * f;
        out.roll = a.roll + (b.roll - a.roll) * f;
        return true;
    }

    double last_received_ms() const { return last_received_ms_; }

    void clear() {
        buf_.clear();
        offsets_.clear();
    }

private:
    static constexpr double kGapResetMs = 2000;
    static constexpr float kTeleportMetres = 10.0f;
    static constexpr size_t kBufferLimit = 64;
    static constexpr size_t kOffsetWindow = 100;  // ~5 s at 20 Hz: adapts if the clocks drift

    static float distance(const StateMsg& a, const StateMsg& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static float lerp_angle(float a, float b, float f) {
        const float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;  // shortest way round
        return a + d * f;
    }

    double delay_ms_;
    double offset_ = 0;
    double last_received_ms_ = 0;
    std::deque<StateMsg> buf_;
    std::deque<double> offsets_;
};

}  // namespace net
