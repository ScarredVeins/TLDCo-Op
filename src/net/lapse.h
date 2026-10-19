#pragma once
// Actions in co-op don't move the clock (milestone 10 item 10): TLD speeds the clock up for an action's game minutes
// (TimeOfDay.SetDayLengthScale: a smaller scale is a shorter day); in co-op the world keeps the normal scale and only
// the acting player's body lives the action's hours (its needs run at the scale TLD asked for). This is the plain
// part: which scale each side gets, the game hours a frame is, and how much of a body's gain it keeps. No IL2CPP.

#include <algorithm>
#include <cmath>

namespace lapse {

constexpr float kEps = 1e-4f;  // TLD's own Mathf.Approximately-style tolerance on the scale

enum class Kind {
    Action,    // harvest, break down, craft, read, repair, eat... and the time lost after a struggle
    Sleep,     // Rest.BeginSleeping / AccelerateTimeOfDay: item 12's, passed through as TLD does it until then
    PassTime,  // PassTime.Begin / AccelerateTimeOfDay: the same
    Reapply,   // TLD's per-frame SetDayLengthScale(m_DayLengthScale) (DebugOnly_UpdateTimeOfDayScale): changes nothing
};

struct Body {
    bool active = false;
    float arg = 1.f;  // the scale TLD asked for: what the body's needs run at
};

struct Applied {
    float world;  // what TimeOfDay.SetDayLengthScale really gets
    Body body;
};

inline bool accelerated(float arg) { return std::fabs(arg - 1.f) > kEps; }

// What a SetDayLengthScale(arg) call does. Outside co-op, and for sleep and passing time, TLD's own way. In co-op an
// action's speed-up goes to the body and the world stays at 1; a call back to 1 (TLD's restore, or a pause) ends it.
inline Applied on_set(Body body, float arg, Kind kind, bool coop) {
    if (kind == Kind::Reapply) return {body.active ? 1.f : arg, body};
    if (!coop || kind != Kind::Action) return {arg, {}};
    if (accelerated(arg)) return {1.f, {true, arg}};
    return {arg, {}};
}

// The scale TLD's Accelerate(realSeconds, gameHours) asks for, after it resets the scale to 1: R/H*24 / (debug*D).
inline double accelerate_arg(double real_s, double game_h, double debug, double day_length_s) {
    return real_s / game_h * 24.0 / (debug * day_length_s);
}

// Game hours that dt real seconds are at a scale (TLD: dt * 24 / (UniStorm scale * day length), UniStorm scale =
// arg * debug).
inline double frame_hours(double dt, double arg, double debug, double day_length_s) {
    return dt * 24.0 / std::max(arg * debug * day_length_s, 1e-9);
}

// The body pays an action's hours but doesn't heal from them: of what one update of condition at the body's scale
// changed, a loss stays whole and a gain is cut to what the frame's real time gives (the world's scale, 1).
inline float condition_after(float before, float after, float body_arg) {
    if (after <= before) return after;
    return before + (after - before) * std::clamp(body_arg, 0.f, 1.f);
}

}  // namespace lapse
