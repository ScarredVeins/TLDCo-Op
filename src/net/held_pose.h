#pragma once
// How an avatar holds what's in its hands (milestone 11): for each kind of item, where each hand goes and which way
// the item points, and the math to put them there after the walk clip has posed the body (a two-bone reach for each
// arm, rotations). The body pack's clips hold nothing (idle, walk, run, down), so the arms are placed in code: decent,
// not hand-animated. Unity's conventions: left-handed, y up, z forward, x right; quaternions (x, y, z, w), a * b
// turns by b first. Plain code, no IL2CPP (held_pose_test).

#include <cmath>
#include <cstdint>

#include "net/held.h"

namespace net::pose {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(Vec3 a) {
    const float n = length(a);
    return n > 1e-6f ? a * (1.0f / n) : Vec3{0, 0, 0};
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat inverse(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }  // unit quaternions
inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline Quat normalized(Quat q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-9f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}

// Quaternion.FromToRotation: the shortest turn from one direction to another.
inline Quat from_to(Vec3 from, Vec3 to) {
    const Vec3 a = normalized(from), b = normalized(to);
    const float d = dot(a, b);
    if (d < -0.99999f) {  // opposite: half a turn about any axis square to them
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-3f) axis = cross({0, 1, 0}, a);
        axis = normalized(axis);
        return {axis.x, axis.y, axis.z, 0};
    }
    const Vec3 c = cross(a, b);
    return normalized(Quat{c.x, c.y, c.z, 1 + d});
}

// Quaternion.LookRotation: z along `forward`, y as near `up` as that allows.
inline Quat look_rotation(Vec3 forward, Vec3 up) {
    const Vec3 f = normalized(forward);
    Vec3 r = normalized(cross(up, f));
    if (length(r) < 1e-6f) r = normalized(cross(std::fabs(f.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{0, 0, 1}, f));
    const Vec3 u = cross(f, r);
    // The rotation's matrix has columns r, u, f.
    const float m00 = r.x, m10 = r.y, m20 = r.z, m01 = u.x, m11 = u.y, m21 = u.z, m02 = f.x, m12 = f.y, m22 = f.z;
    Quat q;
    const float trace = m00 + m11 + m22;
    if (trace > 0) {
        const float s = 0.5f / std::sqrt(trace + 1);
        q = {(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25f / s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2 * std::sqrt(1 + m00 - m11 - m22);
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = 2 * std::sqrt(1 + m11 - m00 - m22);
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = 2 * std::sqrt(1 + m22 - m00 - m11);
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return normalized(q);
}

constexpr float kDegToRad = 3.14159265f / 180.0f;

// A hold is described from a shoulder: x out from the body (right for the right arm, left for the left), y up, z
// forward, along the body's facing (yaw) or, for an aimed item, along where they look (yaw and pitch).
struct Frame {
    Vec3 origin, out, up, forward;
    Vec3 at(Vec3 local) const { return origin + out * local.x + up * local.y + forward * local.z; }
    Vec3 dir(Vec3 local) const { return out * local.x + up * local.y + forward * local.z; }
};

// pitch_deg: up positive (0 = level). right_side: the right shoulder's frame (else the left's: `out` is to the left).
inline Frame frame(Vec3 origin, float yaw_deg, float pitch_deg, bool right_side) {
    const float y = yaw_deg * kDegToRad, p = pitch_deg * kDegToRad;
    Frame f;
    f.origin = origin;
    f.forward = {std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p)};
    const Vec3 right{std::cos(y), 0, -std::sin(y)};
    f.up = normalized(cross(f.forward, right));  // square to forward, towards the sky
    f.out = right_side ? right : right * -1.0f;
    return f;
}

// Two-bone reach: where the elbow goes so the hand gets to `target` (or as near as the arm reaches, `hand`), bent
// towards `pole`. upper = shoulder to elbow, lower = elbow to hand.
inline Vec3 elbow_for(Vec3 shoulder, float upper, float lower, Vec3 target, Vec3 pole, Vec3* hand = nullptr) {
    Vec3 d = target - shoulder;
    float dist = length(d);
    const Vec3 dir = dist > 1e-5f ? d * (1.0f / dist) : Vec3{0, -1, 0};
    const float shortest = std::fabs(upper - lower) + 0.01f, longest = upper + lower - 0.001f;
    dist = dist < shortest ? shortest : dist > longest ? longest : dist;
    if (hand) *hand = shoulder + dir * dist;
    // The angle at the shoulder between the reach and the upper arm (law of cosines).
    float c = (upper * upper + dist * dist - lower * lower) / (2 * upper * dist);
    c = c < -1 ? -1 : c > 1 ? 1 : c;
    const float s = std::sqrt(1 - c * c);
    // The bend's side: the pole, square to the reach.
    const Vec3 p = pole - shoulder;
    Vec3 side = p - dir * dot(p, dir);
    if (length(side) < 1e-4f) side = cross(dir, std::fabs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    side = normalized(side);
    return shoulder + dir * (upper * c) + side * (upper * s);
}

struct Hold {
    bool look = false;        // the frame follows where they look (pitch), not only their facing
    bool right = false;       // the right hand is placed (else the clip moves that arm)
    bool left = false;        // the left hand is placed
    bool in_left = false;     // the item is in the left hand (a bow)
    Vec3 right_hand;          // the right hand's target, from the right shoulder (out, up, forward; metres)
    Vec3 left_hand;           // the left hand's, from the left shoulder, unless on the item
    float left_on_item = -1;  // >= 0: the left hand on the item instead, this far (m) along it from the right hand
    Vec3 item_dir{0, 0, 1};   // where the item's front (or top) points, in the right shoulder's frame
    Vec3 item_up{0, 1, 0};    // and its up, for its roll
    bool hangs = false;       // it hangs straight down from the hand whatever the arm does (a lantern)
    float grip = 0;           // where along it the hand is: -1 its back (or bottom) end, 0 its middle, 1 its front
};

// The default hold for each kind (tuned against screenshots of the body pack's characters).
inline Hold hold_for(HeldKind kind, uint8_t flags) {
    const bool aiming = (flags & kHeldAiming) != 0;
    Hold h;
    switch (kind) {
        case HeldKind::None:
            return h;
        case HeldKind::Torch:
        case HeldKind::FlareRed:
        case HeldKind::FlareBlue:  // held out before the chest, flame up and leaning forward, at head height
            h.right = true;
            h.right_hand = {0.10f, -0.25f, 0.38f};
            h.item_dir = {0, 0.85f, 0.53f};
            h.item_up = {0, -0.53f, 0.85f};
            h.grip = -0.55f;
            return h;
        case HeldKind::Lantern:  // down at the side, hanging from its handle
            h.right = true;
            h.right_hand = {0.10f, -0.48f, 0.12f};
            h.hangs = true;
            h.grip = 1;
            return h;
        case HeldKind::Flashlight:  // out in front, along where they look
            h.look = true;
            h.right = true;
            h.right_hand = {0.05f, -0.10f, 0.42f};
            h.grip = -0.4f;
            return h;
        case HeldKind::Rifle:
            h.right = h.left = true;
            if (aiming) {  // at the shoulder, along where they look
                h.look = true;
                h.right_hand = {-0.12f, -0.05f, 0.30f};
                h.grip = -0.45f;
                h.left_on_item = 0.40f;
            } else {  // low across the body, muzzle forward and down
                h.right_hand = {-0.05f, -0.40f, 0.22f};
                h.item_dir = {-0.23f, -0.32f, 0.92f};
                h.grip = -0.15f;
                h.left_on_item = 0.38f;
            }
            return h;
        case HeldKind::Revolver:
        case HeldKind::FlareGun:
            h.right = true;
            if (aiming) {  // arm out along where they look, the other hand under it
                h.look = true;
                h.left = true;
                h.right_hand = {-0.12f, -0.02f, 0.52f};
                h.left_on_item = 0;
                h.grip = -0.3f;
            } else {
                h.right_hand = {0.05f, -0.50f, 0.15f};
                h.item_dir = {0, -0.8f, 0.6f};
                h.item_up = {0, 0.6f, 0.8f};
                h.grip = -0.3f;
            }
            return h;
        case HeldKind::Camera:
            h.right = true;
            if (aiming) {  // up at the face in both hands
                h.look = true;
                h.left = true;
                h.right_hand = {-0.10f, 0.02f, 0.30f};
                h.left_on_item = 0.10f;
            } else {
                h.right_hand = {0.05f, -0.40f, 0.25f};
            }
            return h;
        case HeldKind::Bow:
            h.in_left = h.left = true;
            h.item_dir = {0, 1, 0.15f};
            h.item_up = {0, -0.15f, 1};
            if (aiming) {  // bow arm out along where they look, the other hand drawing at the cheek
                h.look = true;
                h.right = true;
                h.left_hand = {-0.02f, 0, 0.54f};
                h.right_hand = {-0.12f, 0.05f, 0.05f};
                h.item_dir = {0, 1, 0};
                h.item_up = {0, 0, 1};
            } else {
                h.left_hand = {0.05f, -0.48f, 0.12f};
            }
            return h;
        case HeldKind::Spear:
            h.right = true;
            if (flags & kHeldRaised) {  // set: low, point forward, in both hands
                h.left = true;
                h.right_hand = {-0.05f, -0.30f, 0.15f};
                h.item_dir = {0, -0.24f, 0.97f};
                h.grip = -0.2f;
                h.left_on_item = 0.35f;
            } else {  // upright at the side like a staff
                h.right_hand = {0.12f, -0.42f, 0.10f};
                h.item_dir = {0, 1, 0.1f};
                h.item_up = {0, -0.1f, 1};
                h.grip = -0.1f;
            }
            return h;
        case HeldKind::Stone:
        case HeldKind::NoiseMaker:
            h.right = true;
            h.right_hand = aiming ? Vec3{0.10f, 0.25f, -0.05f} : Vec3{0.05f, -0.35f, 0.28f};  // raised to throw
            return h;
        default:  // anything else: in the right hand, a little in front
            h.right = true;
            h.right_hand = {0.05f, -0.35f, 0.28f};
            return h;
    }
}

}  // namespace net::pose
