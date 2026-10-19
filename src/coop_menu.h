#pragma once
// The co-op screens of the new-game flow (milestone 4c), both built from TLD's own menus: the screen is cloned,
// stripped of its game logic and re-labelled.
//   SOLO / CO-OP - after NEW in the Survival menu. SOLO is the normal game, offline. CO-OP opens the Steam lobby
//                  right away and leads to the setup screen.
//   CO-OP setup  - straight after CO-OP, in place of TLD's difficulty screen: mode (host / join), character, voice
//                  (TLD's Male / Female choice), name and join code. Confirm saves to coop.ini, applies it live and
//                  shows TLD's difficulty, map... TLD's Male / Female screen is then answered with the chosen voice
//                  and never shown. BACK walks the same way back: difficulty -> setup -> SOLO / CO-OP.
//   LOBBY        - after CONFIRM (milestone 4d): everyone in the game with their character and READY. Hosting,
//                  a friend asks to join first: LET IN or TURN AWAY; one in can be KICKED. The host's START loads the
//                  world once every friend in is READY; a READY friend goes on once the host has started.
// Back at the Survival menu, co-op picked there ends. Main thread only.

#include <string>

namespace coop_menu {

bool init();      // hooks Panel_Sandbox, Panel_SelectExperience, Panel_SelectSurvivor and UIButton.OnClick
void on_frame();  // hover highlight and keyboard while a screen is open; deferred screen steps

// TLD's own RESUME on the Survival menu, without the SOLO / CO-OP question (automated tests). False if the Survival
// menu hasn't been opened.
bool resume_with_tld();

// READY in the lobby (a friend), or the lobby let us go on (START, or READY once the host started). With playing,
// what the others see as our READY.
bool lobby_ready();

// Test helper (dev command `menu`): what a player clicks, by name: new / resume / load (the Survival menu), solo /
// coop, mode host|join, code X, select ROW, change +1|-1, confirm, ready, start, admit NAME, kick NAME, back, status.
bool dev(const std::string& action, const std::string& arg);

// A body's name as the CHARACTER row shows it, e.g. "Mackenzie (indoors)" for mackenzie_indoor.
std::string body_name(const std::string& id);

}  // namespace coop_menu
