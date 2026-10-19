// Simulates the remote-player pipeline (game capture -> 20 Hz sender -> network -> render) for a peer walking
// in a straight line at constant speed, and measures how unevenly the receiver draws it.
//
//   old: capture on a 15.6 ms tick clock every 50 ms, interpolate by arrival time   (milestone 4, first try)
//   new: capture every 16 ms with exact timestamps, net::Interpolator on sender time (the fix)
//
// "speed wobble" = mean |drawn speed - true speed| / true speed, per rendered frame. 0% is perfectly smooth.
// Exit code 0 when the new pipeline is under 2% and clearly better than the old one.

#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

#include "net/interp.h"

namespace {

constexpr double kSpeed = 1.5e-3;          // m per ms (walking ~1.5 m/s)
constexpr double kFrameMs = 1000.0 / 700;  // ~700 fps, as measured in game
constexpr double kSendMs = 50;             // 20 Hz sender thread
constexpr double kRunMs = 8000, kWarmupMs = 2000;
constexpr double kClockOffset = 123456.0;  // the receiver's clock base differs from the sender's

struct Packet {
    double arrive;     // receiver clock
    double captured;   // sender clock
    double x;
};

std::vector<Packet> simulate_network(bool old_capture, std::mt19937& rng) {
    std::uniform_real_distribution<double> send_jitter(0, 5), latency(0.1, 2.0);
    std::vector<Packet> out;
    double last_capture = -1e9, captured_t = 0, captured_x = 0;
    double next_send = 7;  // sender clock phase differs from the game's frames
    for (double t = 0; t < kRunMs; t += kFrameMs) {
        if (old_capture) {
            const double tick = std::floor(t / 15.625) * 15.625;  // GetTickCount64 resolution
            if (tick - last_capture >= 50) {
                last_capture = tick;
                captured_t = t;
                captured_x = kSpeed * t;
            }
        } else if (t - last_capture >= 16) {
            last_capture = t;
            captured_t = t;
            captured_x = kSpeed * t;
        }
        while (next_send <= t) {
            out.push_back({next_send + latency(rng) + kClockOffset, captured_t, captured_x});
            next_send += kSendMs + send_jitter(rng);
        }
    }
    return out;
}

// The first milestone-4 interpolation: samples timed by arrival.
struct ArrivalInterpolator {
    std::deque<std::pair<double, double>> buf;  // (arrival, x)
    void add(double arrive, double x) { buf.emplace_back(arrive, x); }
    bool sample(double now, double& x) {
        const double t = now - 100;
        if (buf.empty()) return false;
        while (buf.size() >= 2 && buf[1].first <= t) buf.pop_front();
        if (buf.size() < 2 || t <= buf[0].first) {
            x = buf[0].second;
            return true;
        }
        const double f = (t - buf[0].first) / (buf[1].first - buf[0].first);
        x = buf[0].second + (buf[1].second - buf[0].second) * f;
        return true;
    }
};

double wobble(bool new_pipeline, unsigned seed) {
    std::mt19937 rng(seed);
    const std::vector<Packet> packets = simulate_network(!new_pipeline, rng);
    net::Interpolator interp(100);
    ArrivalInterpolator old_interp;
    size_t next = 0;
    double prev_x = 0, sum = 0;
    int frames = 0;
    bool have_prev = false;
    for (double now = kClockOffset + 0.37; now < kClockOffset + kRunMs; now += kFrameMs) {
        for (; next < packets.size() && packets[next].arrive <= now; ++next) {
            net::StateMsg s;
            s.valid = true;
            s.scene = "LakeRegion";
            s.t_ms = packets[next].captured;
            s.x = static_cast<float>(packets[next].x);
            interp.add(s, packets[next].arrive);
            old_interp.add(packets[next].arrive, packets[next].x);
        }
        double x = 0;
        if (new_pipeline) {
            net::StateMsg s;
            if (!interp.sample(now, s)) continue;
            x = s.x;
        } else if (!old_interp.sample(now, x)) {
            continue;
        }
        if (have_prev && now - kClockOffset > kWarmupMs) {
            sum += std::fabs((x - prev_x) / kFrameMs - kSpeed) / kSpeed;
            ++frames;
        }
        prev_x = x;
        have_prev = true;
    }
    return frames ? 100.0 * sum / frames : 1e9;
}

}  // namespace

int main() {
    double worst_new = 0, best_old = 1e9;
    for (unsigned seed = 1; seed <= 5; ++seed) {
        const double o = wobble(false, seed), n = wobble(true, seed);
        printf("seed %u: speed wobble old %.1f%%  new %.2f%%\n", seed, o, n);
        worst_new = std::max(worst_new, n);
        best_old = std::min(best_old, o);
    }
    const bool pass = worst_new < 2.0 && worst_new * 5 < best_old;
    printf("%s: worst new %.2f%%, best old %.1f%%\n", pass ? "PASS" : "FAIL", worst_new, best_old);
    return pass ? 0 : 1;
}
