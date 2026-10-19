#pragma once
// Milestone 7b: the host's animals, mirrored in the client's game.
//   host   - about 10 times a second, every active animal (BaseAi) within range of either player: kind, AI mode,
//            health, position, facing, and its animator's state (each layer's state and time, every parameter).
//   client - while both players are in the same scene, the client's own animals there are hidden and the host's are
//            shown instead: copies of the same animal (this region's spawn prefab for that kind) with the AI and
//            colliders removed, moved (with the body's tilt along the ground) and animated to match the host's.
//            Everything is restored when mirroring stops (different scenes, disconnect).
//   sounds - every Wwise sound the host's animals make (howls, growls, footsteps, struggle; starts and stops) is
//            caught where all of TLD's sounds go (AK::SoundEngine::PostEvent / StopPlayingID / ExecuteActionOnEvent
//            in AkSoundEngine.dll) and played again on the copy's matching sound object.
// Main thread only.

#include <cstdint>
#include <string>

#include "il2cpp_api.h"

namespace wildlife_sync {

bool init();
void on_frame(const std::string& scene, bool in_game);
std::string describe_sounds();  // what the Wwise hooks see (for the 'sounds' dev command)
uint32_t host_animal_id(Il2CppObject* ai);  // host: the id an animal goes by in Animals (0 = not shared)
Il2CppObject* host_animal(uint32_t id);     // host: the animal (BaseAi) with that id, if it's still shared
float host_animal_hp(Il2CppObject* ai);     // BaseAi.m_CurrentHP
// Client: whether this Transform belongs to one of our copies of the host's animals (its hit colliders), and which;
// dead = the host's animal is dead.
bool puppet_at(Il2CppObject* transform, uint32_t& id, bool& dead);
// Client, for wildlife_carcass: whether we show the host's animals now; a real carcass took the place of the copy of
// the host's animal `id` (the copy hides; our own animals' hiding leaves the carcass alone); it went again.
bool mirroring();
void carcass_made(uint32_t id, Il2CppObject* carcass_go);
void carcass_gone(uint32_t id, Il2CppObject* carcass_go);

}  // namespace wildlife_sync
