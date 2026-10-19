#pragma once
// Finding each other (milestone 12): where a friend's name goes on the screen, or the arrow at its edge; how strongly
// it shows; where a friend is on the radar; when we chart the map again as we walk. Plain code, no IL2CPP (hud_test).
//
// Screen points are Unity's: pixels from the bottom left, z the depth in front of the camera (negative behind it), as
// Camera.WorldToScreenPoint gives them. Map points are TLD's region map (Panel_Map.WorldPositionToMapPosition: NGUI
// units, y up); indoors (no map) the radar uses the world's x and z instead.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace net::hud {

constexpr float kPi = 3.14159265f;

struct V2 {
    float x = 0, y = 0;
};
struct V3 {
    float x = 0, y = 0, z = 0;
};

// --- names over heads ------------------------------------------------------------------------------------------------

struct Tag {
    bool on_screen = false;  // the name sits over their head
    float x = 0, y = 0;      // screen pixels: the name's spot, or (off screen) the edge arrow's
    float angle_deg = 0;     // off screen: which way the arrow points (0 = right, 90 = up)
};

// A friend's head at `screen` (WorldToScreenPoint) on a `width` x `height` screen. In view (in front, inside the margin):
// the name over them. Otherwise an arrow on the margin's rectangle, from the screen's centre towards them; behind the
// camera, Unity mirrors the point through the centre, so it's mirrored back (someone right behind us: the bottom edge).
inline Tag place_tag(V3 screen, float width, float height, float margin) {
    Tag t;
    const float cx = width * 0.5f, cy = height * 0.5f;
    const bool in_front = screen.z > 0;
    if (in_front && screen.x >= margin && screen.x <= width - margin && screen.y >= margin &&
        screen.y <= height - margin) {
        t.on_screen = true;
        t.x = screen.x;
        t.y = screen.y;
        return t;
    }
    float dx = screen.x - cx, dy = screen.y - cy;
    if (!in_front) {
        dx = -dx;
        dy = -dy;
        if (dy > 0) dy = -dy;  // behind us reads as "turn round": below the centre
        if (std::fabs(dx) < 1e-3f && std::fabs(dy) < 1e-3f) dy = -1;
    }
    const float half_w = std::max(1.0f, cx - margin), half_h = std::max(1.0f, cy - margin);
    const float sx = std::fabs(dx) > 1e-6f ? half_w / std::fabs(dx) : 1e9f;
    const float sy = std::fabs(dy) > 1e-6f ? half_h / std::fabs(dy) : 1e9f;
    const float s = std::min(sx, sy);
    t.x = cx + dx * s;
    t.y = cy + dy * s;
    t.angle_deg = std::atan2(dy, dx) * 180.0f / kPi;
    return t;
}

// How strongly a name over a head shows at `distance_m`: out of the way up close (0 under 1 m, full from 3 m), full to
// 60 m, then fainter to 0.45 at 400 m and beyond. Edge arrows don't fade.
inline float tag_alpha(float distance_m) {
    if (distance_m <= 1.0f) return 0;
    if (distance_m < 3.0f) return (distance_m - 1.0f) / 2.0f;
    if (distance_m <= 60.0f) return 1;
    if (distance_m >= 400.0f) return 0.45f;
    return 1.0f - 0.55f * (distance_m - 60.0f) / 340.0f;
}

// A friend shows as a small dot over their head; their name comes up when they're within kNameNearM, or when we look
// at them: their middle (`body`, WorldToScreenPoint) within kLookRadius screen heights of the screen's centre.
constexpr float kNameNearM = 10.0f, kLookRadius = 0.1f;
inline bool name_wanted(float distance_m, V3 body, float width, float height) {
    if (distance_m <= kNameNearM) return true;
    if (body.z <= 0 || height < 1) return false;
    return std::hypot(body.x - width * 0.5f, body.y - height * 0.5f) <= kLookRadius * height;
}

// `current` (0 hidden .. 1 shown) moved towards shown or hidden, the whole way taking `seconds`.
inline float fade(float current, bool shown, float dt_s, float seconds) {
    const float step = seconds > 0 ? dt_s / seconds : 1;
    return shown ? std::min(1.0f, current + step) : std::max(0.0f, current - step);
}

// "12 m", "850 m", "1.2 km".
inline std::string distance_text(float m) {
    char s[24];
    if (m < 999.5f)
        std::snprintf(s, sizeof(s), "%d m", static_cast<int>(std::lround(std::max(0.0f, m))));
    else
        std::snprintf(s, sizeof(s), "%.1f km", m / 1000.0f);
    return s;
}

// --- the radar -------------------------------------------------------------------------------------------------------

// The map under the radar: turned so where we face is up, our spot at its centre, `range` (map units) to the rim.
struct RadarView {
    float rotation_deg = 0;  // the map's turn about the radar's centre (Unity: positive = counterclockwise)
    float scale = 1;         // map units to radar units
};
inline RadarView radar_view(V2 forward, float range, float radius) {
    RadarView v;
    const float len = std::hypot(forward.x, forward.y);
    if (len > 1e-6f) v.rotation_deg = std::atan2(forward.x / len, forward.y / len) * 180.0f / kPi;
    v.scale = range > 1e-6f ? radius / range : 1;
    return v;
}

struct Blip {
    bool inside = true;  // within the rim; else pinned to it, pointing their way
    float x = 0, y = 0;  // radar units from its centre (y up = where we face)
    float angle_deg = 0; // pinned: which way they are (0 = right, 90 = up)
    float distance = 0;  // in the units of `offset`
};
// Someone `offset` from us (map units, or world x/z indoors), us facing `forward` in the same units: their spot on a
// radar of `radius` showing `range` to the rim.
inline Blip radar_blip(V2 offset, V2 forward, float range, float radius) {
    Blip b;
    float fx = forward.x, fy = forward.y;
    const float len = std::hypot(fx, fy);
    if (len > 1e-6f) {
        fx /= len;
        fy /= len;
    } else {
        fx = 0;
        fy = 1;
    }
    const float right = offset.x * fy - offset.y * fx;  // along (fy, -fx): to our right
    const float ahead = offset.x * fx + offset.y * fy;
    b.distance = std::hypot(offset.x, offset.y);
    const float s = range > 1e-6f ? radius / range : 1;
    b.x = right * s;
    b.y = ahead * s;
    b.angle_deg = std::atan2(b.y, b.x) * 180.0f / kPi;
    const float r = std::hypot(b.x, b.y);
    if (r > radius) {
        b.inside = false;
        b.x *= radius / r;
        b.y *= radius / r;
    }
    return b;
}

// A heading (degrees, Unity's yaw: 0 = +z, 90 = +x) as a direction in world x / z.
inline V2 yaw_forward(float yaw_deg) {
    const float a = yaw_deg * kPi / 180.0f;
    return {std::sin(a), std::cos(a)};
}

// --- charting as we walk ---------------------------------------------------------------------------------------------

// A new survey spot (TLD's charcoal survey, without the charcoal) once we're `step_m` from the last one, measured on
// the ground (x / z: climbing in place charts nothing new). None yet: now.
inline bool chart_here(bool charted_before, V3 last, V3 now, float step_m) {
    if (!charted_before) return true;
    return std::hypot(now.x - last.x, now.z - last.z) >= step_m;
}

}  // namespace net::hud
