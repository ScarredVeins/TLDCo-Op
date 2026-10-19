// Actions that don't move the clock in co-op (net/lapse.h, milestone 10 item 10): which scale the world and the
// acting body get, the hours a frame is, and the condition a body keeps. Exit code 0 when every check holds.

#include <cmath>
#include <cstdio>

#include "net/lapse.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

bool near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

}  // namespace

int main() {
    using lapse::Body;
    using lapse::Kind;
    {  // TLD's Accelerate: SetDayLengthScale(1), then the action's scale; the caller restores 1 at the end.
        Body body;
        auto a = lapse::on_set(body, 1.f, Kind::Action, true);
        expect("the reset to 1 before an action is passed on, no body lapse", a.world == 1.f && !a.body.active);
        a = lapse::on_set(a.body, 0.02f, Kind::Action, true);
        expect("an action's speed-up: the world stays at 1", a.world == 1.f);
        expect("...and the body lives it at the scale TLD asked for", a.body.active && a.body.arg == 0.02f);
        auto r = lapse::on_set(a.body, 1.f, Kind::Reapply, true);
        expect("TLD's per-frame re-apply keeps the body lapse and the world at 1", r.world == 1.f && r.body.active);
        auto end = lapse::on_set(r.body, 1.f, Kind::Action, true);
        expect("the restore to 1 ends the body lapse", end.world == 1.f && !end.body.active);
        auto resumed = lapse::on_set(end.body, 0.02f, Kind::Action, true);
        expect("a resume (the bar's accelerated scale again) starts it again", resumed.world == 1.f && resumed.body.active);
    }
    {  // Not co-op, sleep, passing time: TLD's own way.
        Body on{true, 0.02f};
        auto solo = lapse::on_set({}, 0.02f, Kind::Action, false);
        expect("outside co-op an action speeds the world up as TLD does", solo.world == 0.02f && !solo.body.active);
        auto sleep = lapse::on_set({}, 0.01f, Kind::Sleep, true);
        expect("sleep's speed-up is passed on (item 12's)", sleep.world == 0.01f && !sleep.body.active);
        auto pass = lapse::on_set(on, 0.01f, Kind::PassTime, true);
        expect("passing time is passed on and ends a body lapse", pass.world == 0.01f && !pass.body.active);
        auto idle = lapse::on_set({}, 1.f, Kind::Reapply, true);
        expect("a re-apply with no body lapse changes nothing", idle.world == 1.f && !idle.body.active);
        auto debug = lapse::on_set({}, 0.5f, Kind::Reapply, true);
        expect("a re-apply of another world scale (TLD's debug speed-up) is passed on", debug.world == 0.5f);
        expect("a scale within TLD's tolerance of 1 isn't a speed-up",
               !lapse::on_set({}, 1.00005f, Kind::Action, true).body.active);
    }
    {  // Hours: an action of 1 game hour over 10 real seconds, at 60 frames a second.
        const double day = 7200, debug = 1;  // a 2-hour real day
        const double arg = lapse::accelerate_arg(10, 1, debug, day);
        double body = 0, world = 0, part = 0;
        for (int i = 0; i < 600; ++i) {
            body += lapse::frame_hours(1.0 / 60, arg, debug, day);
            world += lapse::frame_hours(1.0 / 60, 1, debug, day);
            if (i == 239) part = body;
        }
        expect("the body lives the action's 1 game hour in its 10 real seconds", near(body, 1.0, 1e-9));
        expect("...0.4 h if it stops at 40%", near(part, 0.4, 1e-9));
        expect("the world lives 10 real seconds' worth (24*10/D h)", near(world, 24.0 * 10 / day, 1e-9));
        expect("frame_hours is never infinite", std::isfinite(lapse::frame_hours(1, 0, 1, day)));
    }
    {  // Condition: losses whole, gains cut to the world's share.
        expect("a loss at the body's scale stays", lapse::condition_after(80, 70, 0.02f) == 70);
        expect("a gain is cut to the real time's share", near(lapse::condition_after(80, 90, 0.02f), 80.2, 1e-4));
        expect("no change stays no change", lapse::condition_after(80, 80, 0.02f) == 80);
        expect("a scale above 1 (a slower clock) keeps a gain as it is", lapse::condition_after(80, 90, 3.f) == 90);
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "ALL PASS");
    return g_failed ? 1 : 0;
}
