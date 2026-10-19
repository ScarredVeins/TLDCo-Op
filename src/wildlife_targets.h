#pragma once
// Milestone 7c: the host's animals deal with the other player too.
//
// Step 1, perception: TLD's AI is written around one player: BaseAi keeps the player's AiTarget (m_PlayerTarget) and
// asks GameManager.GetPlayerObject / GetPlayerTransform where they are. So each animal picks *its* player: the one it
// is already after, or else the nearest (the host or a friend whose avatar is shown here). While an animal that picked the other player thinks (BaseAi.Update
// / LateUpdate), its m_PlayerTarget is the avatar's AiTarget and those two calls answer with the avatar, so TLD's own
// detection, stalking, chasing and fleeing work on the other player unchanged.
//
// Step 2, what such an animal does to the other player goes to their game, never to the host:
//   fight  - PlayerStruggle.Begin is refused here; their game runs the fight with an animal of the same kind and
//            sends back how it ended. Meanwhile the animal waits on the spot (its AI paused); then it flees or dies
//            with the health it has left.
//   damage - AiTarget.ApplyDamage on their stand-in, and the animal's wound events (DamageEventTable.MaybeApplyAttack:
//            bleeding, sprains, torn clothing), are sent to them to apply.
// Every animal kind goes through the same calls. Host only; main thread.

#include <cstdint>
#include <functional>

#include "il2cpp_api.h"

namespace wildlife_targets {

bool init();
void on_frame();

// Runs `action` on one of the host's animals as if friend `player` were TLD's player: PlayerManager.m_AiTarget,
// GetPlayerObject and GetPlayerTransform are their avatar's meanwhile (so a shot animal turns on, or flees from, the
// one who shot it), and the animal keeps after them. False (nothing run) if their avatar isn't shown. Host only.
struct FriendStandIn {
    Il2CppObject* go;
    Il2CppObject* transform;
    Il2CppObject* ai_target;
    float position[3];
    uint8_t player;
};
bool as_friends_doing(Il2CppObject* ai, uint8_t player, const std::function<void(const FriendStandIn&)>& action);

// Test helpers (dev commands), host only: this animal goes for the other player now (Attack mode, at their avatar), or
// jumps them right where it is (Struggle, as if it had reached them: their game runs the fight).
bool test_attack(Il2CppObject* ai);
bool test_fight(Il2CppObject* ai);
// The same for the host, TLD's own player (what an animal does with no friend in it, to compare).
bool test_attack_host(Il2CppObject* ai);

}  // namespace wildlife_targets
