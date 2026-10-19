#pragma once
// Milestone 8c: what one player breaks down (furniture, limbs, crates: BreakDown) or harvests (plants: Harvestable)
// is gone for the other player too.
//   both   - breaking something down (BreakDown.DoBreakDown) or harvesting a plant (Harvestable.Harvest) goes to the
//            other player as ObjectPut. The other game breaks it down the same way, without the yield (the wood or
//            cloth is in the breaker's pack, or lying in the world and shared as an item), and puts a harvested plant
//            the way loading a save does (Harvestable.Deserialize: m_Harvested, m_RefreshTime, hidden, its "after
//            harvest" object shown).
//   host   - when the client comes into the host's scene, the host sends everything broken down or harvested there
//            (ObjectList); the client matches it (and puts back what it broke that the host still has). The client's
//            changes in a scene the host isn't in are kept and made when the host gets there.
// Objects are named by TLD's persistent id (ObjectGuid.GetGuidFromGameObject), or by name and position when they have
// none. Those without one are limbs and branches a RadialObjectSpawner put there: their spawner hides them when the
// player is far away and shows them again later, so for them only what a player broke down counts; both games keep a
// list of those and break them down again whenever their spawner puts them back.
// Locks and safes (milestone 10): on containers, lockers and safes (OpenClose) and doors (LoadScene). Looked at twice a
// second (TLD opens them in several ways: forced, a keypad, a door's other side, a safe's dial); one that opened here
// goes to the other player as ObjectPut (a lock's state, a safe cracked) and is opened there as loading a save does
// (a lock's other side too: Lock.UnlockCompanionLock). They only ever open in play: another player's change never shuts
// one. The host's list has every lock and safe in the scene, locked or not (whether one is locked at all is rolled in
// each game).
// Bodies (milestone 10, coop_death): a player who dies leaves one (Body: TLD's record of the corpse and what it holds);
// it goes to the others, the host keeps it for scenes it isn't in, and its list has the bodies in the scene (what one
// holds comes when it's opened, world_containers). Main thread only.

#include <string>
#include <vector>

#include "net/protocol.h"
#include "world_keep.h"

namespace world_objects {

bool init();
void on_frame(bool in_game);

// A change another part of co-op made in our scene (coop_death: our body) goes to the others as ours do. False when
// there's no one to tell (not in a scene, co-op off).
bool share(const net::ObjectState& s);

// Test helpers (dev commands).
void describe(float metres, size_t most = 40);  // breakdowns and plants within `metres` (the nearest `most`): id, state
void describe_spawners(float metres);      // TLD's random sets and prefab spawners within `metres`, what each has
void describe_placements();                // TLD's placements in this scene (PlaceableManager), as JSON
bool break_down(const std::string& name);  // break down the nearest one (whose name contains `name`), with its yield
bool harvest(const std::string& name);     // harvest the nearest plant (whose name contains `name`)
bool force(const std::string& name);       // force the nearest locked lock open, as finishing TLD's prybar force does
bool crack(const std::string& name);       // crack the nearest locked safe, as the dial's last tumbler does
// Dev: decorations near us and carried (`what` empty), or "take" / "move" / "place" one (name contains `name`)
// Dev: GameObjects (active or not) whose name contains `name` (the first `most`): where, active, parents, components
void describe_game_objects(const std::string& name, size_t most);
bool decorations(const std::string& what, const std::string& name);

// Host (world_keep): what we keep for scenes we aren't in, as the messages it came in, to go with our save; and back.
void save_kept(std::vector<world_keep::Record>& out);
bool load_kept(const world_keep::Record& r);

}  // namespace world_objects
