#pragma once
// Co-op plays at the host's experience mode (the player chose this: loot, what's in the world, wildlife and survival
// rates follow the host's difficulty). Each player's own save keeps its own mode for solo play.
//   host:   sends its ExperienceModeManager.Serialize() (GameMode) on connect and when it changes.
//   client: switches to it with ExperienceModeManager.Deserialize, as loading a save does. TLD restores the save's own
//           mode again on every scene load (RestoreGlobalData); a hook on Deserialize puts the host's back right after.
//           Saving: ExperienceModeManager.Serialize (the global save data's copy) returns our own mode while the
//           host's is in place, and SaveGameSystem.SaveInfoData (the slot's header, m_XPMode) runs with our own mode
//           config put back for the moment it takes. Back at the main menu nothing is overridden any more.
// Main thread only.

#include <string>

namespace world_mode {

bool init();
void on_frame(const std::string& scene, bool in_game);

// Client: the host's mode is in place in this game (or the host never said: an old host, or 10 s passed). world_join
// waits for it, so a first join loads the host's scene at the host's mode.
bool ready();

// Dev command `mode`: the experience mode in place (GetCurrentExperienceModeType), and what a save would write.
void describe();

}  // namespace world_mode
