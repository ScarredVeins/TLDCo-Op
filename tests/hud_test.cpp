// Finding each other (net/hud.h, milestone 12): a friend's name over their head or an arrow at the screen's edge, how
// it fades, distances in words, where friends are on the radar as it turns with us, when to chart again.
// Exit code 0 when every check holds.

#include <cmath>
#include <cstdio>
#include <string>

#include "net/hud.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
    using namespace net::hud;
    constexpr float W = 960, H = 540, M = 40;
    {  // Names over heads, or an arrow at the edge.
        Tag t = place_tag({480, 300, 20}, W, H, M);
        expect("in view: over their head, where they are", t.on_screen && near(t.x, 480) && near(t.y, 300));
        t = place_tag({2000, 270, 20}, W, H, M);
        expect("off to the right: an arrow on the right margin, pointing right",
               !t.on_screen && near(t.x, W - M) && near(t.y, 270) && near(t.angle_deg, 0));
        t = place_tag({480, -500, 20}, W, H, M);
        expect("below the view: on the bottom margin, pointing down",
               !t.on_screen && near(t.y, M) && near(t.x, 480) && near(t.angle_deg, -90));
        t = place_tag({30, 300, 20}, W, H, M);
        expect("inside the screen but in the margin: an arrow, not a name", !t.on_screen && near(t.x, M));
        t = place_tag({480, 270, -10}, W, H, M);
        expect("right behind us: the bottom edge, pointing down", !t.on_screen && near(t.y, M) && near(t.angle_deg, -90));
        t = place_tag({700, 270, -10}, W, H, M);  // mirrored: really to our left and behind
        expect("behind and to our left: the arrow is on the left half", !t.on_screen && t.x < W / 2);
        t = place_tag({480, 600, -10}, W, H, M);  // Unity puts someone behind and below at the top: still the bottom
        expect("behind: never the top edge", !t.on_screen && t.y < H / 2);
        t = place_tag({1500, 900, 20}, W, H, M);
        const bool on_rim = near(t.x, W - M, 0.5f) || near(t.y, H - M, 0.5f);
        expect("up and right: on the margin's rectangle, inside the screen",
               !t.on_screen && on_rim && t.x <= W - M + 0.5f && t.y <= H - M + 0.5f && t.angle_deg > 0 &&
                   t.angle_deg < 90);
    }
    {  // How strongly a name shows.
        expect("touching: not at all", tag_alpha(0.5f) == 0);
        expect("2 m: half", near(tag_alpha(2.0f), 0.5f));
        expect("10 m: fully", tag_alpha(10) == 1);
        expect("60 m: fully", tag_alpha(60) == 1);
        expect("far: fainter", tag_alpha(200) < 1 && tag_alpha(200) > 0.45f);
        expect("very far: never gone", near(tag_alpha(5000), 0.45f));
    }
    {  // A dot always; the name when close or looked at.
        expect("8 m away, off to the side: the name", name_wanted(8, {100, 500, 8}, 1920, 1080));
        expect("40 m away, at the screen's centre: the name", name_wanted(40, {960, 560, 40}, 1920, 1080));
        expect("40 m away, off to the side: only the dot", !name_wanted(40, {1500, 540, 40}, 1920, 1080));
        expect("40 m away, just off the centre (within the look): the name", name_wanted(40, {1050, 520, 40}, 1920, 1080));
        expect("40 m away, behind us: no name", !name_wanted(40, {960, 540, -40}, 1920, 1080));
        expect("fade in: 0.05 s of a 0.2 s fade is a quarter of it", near(fade(0, true, 0.05f, 0.2f), 0.25f));
        expect("fade out stops at hidden", fade(0.1f, false, 0.5f, 0.2f) == 0);
        expect("fade in stops at shown", fade(0.9f, true, 0.5f, 0.2f) == 1);
    }
    {  // Distances in words.
        expect("12 m", distance_text(12.4f) == "12 m");
        expect("0 m, not -0", distance_text(-0.2f) == "0 m");
        expect("999 m", distance_text(999.4f) == "999 m");
        expect("1.0 km from 999.5 m", distance_text(999.6f) == "1.0 km");
        expect("2.4 km", distance_text(2400) == "2.4 km");
    }
    {  // The radar turns with us: where we face is up.
        const V2 north{0, 1}, east{1, 0};
        Blip b = radar_blip({0, 50}, north, 100, 80);
        expect("facing north, someone 50 north: straight up, halfway", b.inside && near(b.x, 0) && near(b.y, 40));
        b = radar_blip({50, 0}, north, 100, 80);
        expect("facing north, someone east: to the right", b.inside && near(b.x, 40) && near(b.y, 0));
        b = radar_blip({50, 0}, east, 100, 80);
        expect("facing east, someone east: straight up", b.inside && near(b.x, 0) && near(b.y, 40));
        b = radar_blip({0, 50}, east, 100, 80);
        expect("facing east, someone north: to the left", b.inside && near(b.x, -40) && near(b.y, 0));
        b = radar_blip({0, -300}, north, 100, 80);
        expect("beyond the rim behind us: pinned to the bottom of the rim",
               !b.inside && near(b.x, 0) && near(b.y, -80) && near(b.angle_deg, -90) && near(b.distance, 300));
        b = radar_blip({3, 4}, {0, 0}, 100, 80);
        expect("no facing: north is up", b.inside && near(b.x, 2.4f) && near(b.y, 3.2f));
        b = radar_blip({0, 50}, {0, 7}, 100, 80);
        expect("the facing's length doesn't matter", near(b.y, 40));

        const V2 f = yaw_forward(90);
        expect("yaw 90 faces +x", near(f.x, 1) && near(f.y, 0));
        RadarView v = radar_view(east, 100, 80);
        expect("facing east: the map turns a quarter, counterclockwise", near(v.rotation_deg, 90) && near(v.scale, 0.8f));
        v = radar_view({-1, 0}, 50, 80);
        expect("facing west: the other way", near(v.rotation_deg, -90) && near(v.scale, 1.6f));
        // The map turned by the view puts what we face at the top: rotate `forward` by rotation_deg (counterclockwise).
        const V2 fw{0.6f, 0.8f};
        v = radar_view(fw, 100, 80);
        const float a = v.rotation_deg * kPi / 180.0f;
        const float ux = fw.x * std::cos(a) - fw.y * std::sin(a), uy = fw.x * std::sin(a) + fw.y * std::cos(a);
        expect("the turned map has our facing straight up", near(ux, 0, 1e-4f) && near(uy, 1, 1e-4f));
    }
    {  // Charting as we walk.
        expect("nothing charted yet: chart now", chart_here(false, {}, {5, 0, 5}, 25));
        expect("10 m on: not yet", !chart_here(true, {0, 0, 0}, {6, 0, 8}, 25));
        expect("30 m on: chart", chart_here(true, {0, 0, 0}, {18, 0, 24}, 25));
        expect("climbing in place: nothing new", !chart_here(true, {0, 0, 0}, {1, 40, 1}, 25));
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return g_failed ? 1 : 0;
}
