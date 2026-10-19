#pragma once
// What co-op tells the player in the game, the way TLD tells them things: a line on its HUD (HUDMessage.AddMessage,
// the top-of-screen message TLD uses for "Too dark to read" and the like).
//   - a friend joins or leaves the game (host and friends alike);
//   - a friend: the link to the host drops (the world goes on; changes go to the host once it's back) and comes back.
//   - hosting while we play: a friend asks to join; F9 lets them in, F10 turns them away (in the lobby: its buttons).
// Nothing is said for who is already there when a game loads. Main thread only.

#include <string>

namespace coop_notices {

bool init();
void on_frame(bool in_game);
// Test helper (dev command `answer let|away`): what F9 / F10 do, for the first friend asking. False: nobody asks.
bool answer(bool let_in);
// A line on TLD's HUD from another part of co-op (e.g. an item someone else took first).
void tell(const std::string& text);

}  // namespace coop_notices
