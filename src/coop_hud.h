#pragma once
// Finding each other (milestone 12). Each part is the player's own and can be switched off ([hud] in coop.ini, the
// co-op setup screen, dev `hud`):
//   names - each friend's name over their head with how far they are, in TLD's hover text look on its HUD; out of
//           sight (off screen, behind us) an arrow at the screen's edge with the name; in another scene, where.
//   radar - a small round map of the area around us in the top right corner: TLD's own region map as we have
//           charted it, turned with where we face, friends on it (pinned to the rim beyond it). Indoors: friends only.
//   chart - our map charts itself where we walk, as TLD's charcoal survey does, without charcoal. Each player's own
//           map: nothing about it goes over the network.
// Friends are on TLD's map screen too, as markers that aren't saved with the map. Everything hides when TLD hides its
// HUD (its HUD setting off, screenshot mode, a menu over the game). Plain logic in net/hud.h (hud_test). Main thread.

#include <string>

namespace coop_hud {

bool init();

// The switches (coop.ini [hud], the co-op setup screen).
void set_switches(bool names, bool radar, bool chart);

// Every frame: charting as we walk, friends on the map screen.
void on_frame(const std::string& scene, bool in_game);

// Every frame after the camera and the avatars have moved (GameManager.LateUpdate): names and the radar.
void late_update();

// What's shown now, for the log and dev `hud`.
std::string describe();

// Dev `hud [names|radar|chart on|off]`: a switch for this session (coop.ini unchanged); `hud chart now` charts here
// at once. False if the action is unknown.
bool dev(const std::string& action, const std::string& arg);

}  // namespace coop_hud
