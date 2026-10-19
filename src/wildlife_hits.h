#pragma once
// Milestone 7d: the other player's weapons on the host's animals.
//   client - its copies of the host's animals keep their hit colliders (wildlife_sync), so its rifle and revolver
//            bullets, arrows and flare-gun rounds hit them with TLD's own blood, decals and sticking arrows, and TLD's
//            hit code stops there (no BaseAi). Hooks just before that point work the hit out the way TLD would for the
//            shooter (their skills, the body part, range falloff, the kill-chance roll) and send it to the host. Thrown
//            stones, flares, torches and noise makers are sent too (they scare, and stones stun small game).
//   host   - applies each hit to its real animal with TLD's own calls (SetupDamageForAnim, ApplyDamage,
//            MaybeFleeOrAttackFromProjectileHit), with the other player standing in as TLD's player, so the animal
//            bleeds, reacts, flees from or turns on the one who shot it, and dies there and for both.
// Main thread only.

#include <string>

namespace wildlife_hits {

bool init();
void on_frame();

// Test helpers (dev commands), client only: stand some metres from the nearest host animal; aim at one of its hit
// colliders (e.g. capsule_chest) and fire the weapon in our hands through TLD's own vp_FPSShooter.Fire.
bool approach(float metres);
bool shoot(const std::string& collider);
bool look(float x, float y, float z);  // point the camera at a world position (either side)
// The aim button (TLD's "alt fire") held down from now until hold_aim(false), as a player holding it: a gun raised to
// the shoulder with TLD's own animations (a bow isn't drawn by it). False if the input hooks are missing.
bool hold_aim(bool on);

}  // namespace wildlife_hits
