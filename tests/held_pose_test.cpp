// How an avatar holds things (net/held_pose.h, milestone 11): the rotations match Unity's (left-handed, y up, z
// forward), the arm's reach puts the hand on its target without stretching a bone, and every kind's hold is one a
// body-pack arm (0.28 m + 0.28 m) can reach. Exit code 0 when every check holds.

#include <cstdio>
#include <string>

#include "net/held_pose.h"

namespace {

using namespace net::pose;
int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

bool near(Vec3 a, Vec3 b, float eps = 1e-3f) { return length(a - b) < eps; }
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

}  // namespace

int main() {
    {  // Rotations, as Unity's.
        const float h = std::sqrt(0.5f);
        const Quat yaw90{0, h, 0, h};  // Quaternion.Euler(0, 90, 0)
        expect("90 degrees of yaw turns forward to the right", near(rotate(yaw90, {0, 0, 1}), {1, 0, 0}));
        const Quat pitch90{h, 0, 0, h};  // Quaternion.Euler(90, 0, 0): pitch down
        expect("90 degrees of pitch turns forward down", near(rotate(pitch90, {0, 0, 1}), {0, -1, 0}));
        expect("a * b turns by b first", near(rotate(yaw90 * pitch90, {0, 0, 1}), {0, -1, 0}) &&
                                             near(rotate(pitch90 * yaw90, {0, 0, 1}), {1, 0, 0}));
        expect("inverse undoes", near(rotate(inverse(yaw90), rotate(yaw90, {0.3f, -2, 5})), {0.3f, -2, 5}));
        const Vec3 f = normalized(Vec3{1, -0.5f, 2});
        const Quat look = look_rotation(f, {0, 1, 0});
        expect("look_rotation: z along forward", near(rotate(look, {0, 0, 1}), f));
        expect("...x level, y towards the sky", near(rotate(look, {1, 0, 0}).y, 0) && rotate(look, {0, 1, 0}).y > 0);
        expect("...looking straight up still works", near(rotate(look_rotation({0, 1, 0}, {0, 1, 0}), {0, 0, 1}),
                                                          {0, 1, 0}));
        for (const auto& [a, b] : {std::pair<Vec3, Vec3>{{0, -1, 0}, {0, 0, 1}}, {{1, 2, 3}, {-3, 1, 0.5f}},
                                   {{0, 1, 0}, {0, -1, 0}}, {{1, 0, 0}, {1, 0, 0}}}) {
            const Quat q = from_to(a, b);
            expect("from_to turns one direction onto the other", near(rotate(q, normalized(a)), normalized(b)));
        }
    }
    {  // Frames: the right shoulder's out is the body's right; the left's is its left; pitch lifts forward.
        const Frame r = frame({0, 0, 0}, 90, 0, true), l = frame({0, 0, 0}, 90, 0, false);
        expect("facing +x: forward is +x", near(r.forward, {1, 0, 0}));
        expect("...the right shoulder's out is -z (the right)", near(r.out, {0, 0, -1}));
        expect("...the left shoulder's out is +z", near(l.out, {0, 0, 1}));
        const Frame up = frame({0, 0, 0}, 0, 30, true);
        expect("pitch up 30: forward rises", near(up.forward, {0, 0.5f, 0.866f}) && near(up.up, {0, 0.866f, -0.5f}));
    }
    {  // The reach.
        const Vec3 shoulder{0, 1.4f, 0}, pole{0.3f, 0.8f, -0.3f};
        for (const Vec3 target : {Vec3{0.1f, 1.3f, 0.4f}, Vec3{0.25f, 1.0f, 0.1f}, Vec3{-0.1f, 1.45f, 0.2f}}) {
            Vec3 hand{};
            const Vec3 elbow = elbow_for(shoulder, 0.28f, 0.28f, target, pole, &hand);
            expect("the hand gets to a target in reach", near(hand, target));
            expect("...neither bone stretched",
                   near(length(elbow - shoulder), 0.28f) && near(length(hand - elbow), 0.28f));
            const Vec3 bend = elbow - (shoulder + (hand - shoulder) * 0.5f);
            expect("...bent towards the pole", dot(bend, pole - shoulder) > 0);
        }
        Vec3 hand{};
        const Vec3 elbow = elbow_for(shoulder, 0.28f, 0.28f, {0, 1.4f, 2}, pole, &hand);
        expect("out of reach: the arm points at it, nearly straight",
               near(normalized(hand - shoulder), {0, 0, 1}) && length(hand - shoulder) > 0.55f &&
                   near(length(elbow - shoulder), 0.28f));
    }
    {  // Every hold is reachable and points somewhere.
        for (int k = 1; k < static_cast<int>(net::HeldKind::Count); ++k) {
            for (uint8_t flags : {uint8_t{0}, net::kHeldAiming, net::kHeldRaised}) {
                const Hold h = hold_for(static_cast<net::HeldKind>(k), flags);
                const bool reach = (!h.right || length(h.right_hand) < 0.555f) &&
                                   (!h.left || h.left_on_item >= 0 || length(h.left_hand) < 0.555f);
                const std::string what = std::string("the ") + net::held_kind_name(static_cast<net::HeldKind>(k)) +
                                         (flags & net::kHeldAiming ? " aimed" : flags ? " raised" : "") +
                                         ": in reach, a hand on it, pointing somewhere";
                expect(what.c_str(), reach && (h.right || h.left) && (h.hangs || length(h.item_dir) > 0.5f) &&
                                         h.grip >= -1 && h.grip <= 1);
            }
        }
        expect("empty hands: nothing placed", !hold_for(net::HeldKind::None, 0).right &&
                                                  !hold_for(net::HeldKind::None, 0).left);
        expect("a bow is in the left hand", hold_for(net::HeldKind::Bow, 0).in_left);
        expect("an aimed rifle follows the look, a lowered one the body",
               hold_for(net::HeldKind::Rifle, net::kHeldAiming).look && !hold_for(net::HeldKind::Rifle, 0).look);
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
