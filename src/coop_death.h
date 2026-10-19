#pragma once
// Milestone 10 item 9: in co-op a death ends the character, not the game (the user's calls, 2026-10-18).
//   dying   - TLD's own death runs (GameManager.HandlePlayerDeath) but never deletes the save in co-op: the host's is
//             the shared world, a friend's has their place in it. First a body falls where the player died: TLD's
//             corpse (a CustomSpawnedItem, saved with the scene the way Cheat Death's ruined travois is) holding all
//             they carried and wore, their water too. It goes to the others as ObjectPut (ObjectKind Body: its TLD
//             record and what it holds); the host keeps it for scenes it isn't in, and its scene lists carry the
//             bodies there. Anyone can loot it: world_containers shares what's in it, by its id.
//   screen  - TLD's screen after a death (Panel_LifeAfterDeath), its first stage made co-op's: the cause, the next
//             survivor (the character preview of the co-op setup; NEXT SURVIVOR or the arrow keys change it) and
//             RESPAWN. Cheat Death's campfires and END GAME aren't there, and it never goes on to TLD's end-of-game
//             log, whatever the experience mode's lives.
//   respawn - in place: the afflictions TLD's own respawn cures, full condition, fed, rested and warm, no skills, and
//             the experience mode's start gear (PlayerManager's m_StartGearApplied cleared, as ResetStartGear does).
//             Beside another player in our scene; a friend with nobody here goes to the host (world_join); otherwise
//             where they died. The others see the new character's body from then on (coop.ini too). The game saves
//             a moment later.
// While dead, State carries kStateDead: the others hide our avatar and say who died. Main thread only.

#include <string>
#include <vector>

#include "net/protocol.h"

namespace coop_death {

bool init();
void on_frame(const std::string& scene, bool in_game);

bool dead();  // we died and haven't respawned yet

// world_objects: the players' bodies in our scene (for a friend coming in), and one from another game made here (true:
// made now; false: already here, or TLD couldn't make it).
std::vector<net::ObjectState> bodies_here();
bool apply_body(const net::ObjectState& s);
std::string body_name(const net::ObjectState& s);  // whose body it is, for the log

// Test helpers (dev commands).
bool respawn(const std::string& body);  // RESPAWN on the screen after a death, as `body` (empty: the one it shows)
bool next_survivor(int step);           // the screen's next (+1) or previous (-1) survivor
void describe();                        // the bodies here: whose, where, what they hold; and whether we're dead
bool set_lives(int lives);              // the experience mode's Cheat Death lives, for this game (0: a mode without)

}  // namespace coop_death
