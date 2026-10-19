#pragma once
// Actions don't move the clock in co-op (milestone 10 item 10). TLD speeds the clock up while a player harvests,
// breaks something down, crafts, reads, repairs, eats or drinks (TimeOfDay.SetDayLengthScale, from Accelerate and the
// panels): in co-op that would move the shared world (the host's) or run a friend's clock ahead until the host's
// snaps it back. Here the world keeps the normal scale, and only our body lives the action's hours: hunger, thirst,
// fatigue, warmth, condition losses and buffs running down update at the scale TLD asked for. Condition gains (rest
// while healthy) stay at the real time's (the body pays for the hours, it doesn't heal from them). Every action's own
// progress runs on real time in TLD, so the bar and what it yields are as before.
//   Sleep and passing time are TLD's own way still (item 12). Ice fishing's catch timer runs at the body's hours
// while fishing actively. Not changed by the hours: the fires, the pot, decay, a lamp's fuel, the weather.
// Main thread only.

#include <string>

namespace coop_lapse {

bool init();
void on_frame(const std::string& scene, bool in_game);
// An action's hours are being lived by our body now (co-op).
bool body_lapse();
// coop_sleep: a sleep's speed-up (TLD's Rest.BeginSleeping, or a shared sleep starting) is the world's, as in TLD:
// true before it, false after.
void world_lapse(bool begin);
// dev `needs`: our needs, their rates, the clock and the scale.
void describe();
// dev `lapse S MIN`: TLD's generic progress bar (what eating, drinking and first aid use) for S real seconds and MIN
// game minutes. False outside a game.
bool launch(float seconds, float minutes);
// coop_sleep's REST: condition comes back this much faster (1: as TLD has it). Every frame.
void heal_boost(float factor);
// What we eat or drink now, while TLD's progress bar for it is up (milestone 13: the others see our avatar eat it,
// State's kStateEating): its gear prefab. "" otherwise.
std::string eating();

}  // namespace coop_lapse
