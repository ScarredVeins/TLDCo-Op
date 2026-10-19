#pragma once
// Milestone 9a: a friend joining a host's world for the first time goes to where the host is. Each player keeps their
// own save for their character (pack, condition, skills, where they were); the world is the host's.
//   first time: when the client's game is in (its own save, anywhere) and it first sees the host in the game, and
//            this save has never joined this host's world (game::save_id() of both, kept in joined.txt in this copy's
//            folder), it goes to the host: loads the host's scene the way TLD's console `scene` command does
//            (GameAudioManager.StopAll + GameManager.LoadSceneWithLoadingScreen) if it's another one, then stands
//            beside the host. The world modules' lists (items, objects, fires; containers when opened) then make that
//            scene the host's.
//   later:   a save that has joined this world before stays where it was saved (the player chose this: a friend
//            coming back keeps their own spot). Within a session nothing moves anyone either: once per game load.
//   interior: if the host is in a building, TLD's next-scene override and last outdoor scene are set to the host's
//            region first (World carries it), so the building's door lets out where the host's does.
// Main thread only.

#include <string>

#include "net/protocol.h"

namespace world_join {

bool init();
void on_frame(const std::string& scene, bool in_game);

// Dev command `join` (and later a "go to the host" button): go to the host now, as on a first join.
void go_to_host();

// Stand beside another player in our scene (to their right, facing them), as a first join does beside the host
// (coop_death: a respawn). False when not in a game.
bool stand_beside(const net::StateMsg& other);

}  // namespace world_join
