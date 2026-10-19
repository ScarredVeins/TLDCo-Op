#pragma once
// Milestone 8a: loose items lying in the world are the same for both players.
//   both   - about twice a second, every loose item in our scene (what TLD's own save counts as gear lying in the world:
//            GearManager.Serialize's rule), by its persistent id (ObjectGuid, "PDID"). An item that left the world
//            (picked up, used, destroyed) goes to the other player as ItemGone; one that appeared or came to rest
//            somewhere new (dropped, placed, thrown, quartered) goes as ItemPut with TLD's save record for it
//            (GearItem.CreateSaveData), once it has stopped moving for a second.
//            The other player's changes go in with TLD's own calls: GearItem.InstantiateAndDeserializeGearItem (how a
//            save brings an item back) and GearManager.DestroyGearObject. Changes for a scene we aren't in wait until we
//            load it. An id we already use for something else (both games loaded the same save, so our pack holds the
//            same ids) gets a new id here and an alias.
//   host   - when the other player gets into our scene: the list of every loose item here. The client removes what the
//            host doesn't have and asks for what it lacks: the host's world is the world.
//            Two players taking the same item at about the same time (milestone 10): the host decides, in the order it
//            hears of the takes (net::Claims). A later take is refused (ItemGone NotYours: out of that player's pack);
//            one of an item the host has in its hands (TLD's Take it / Leave it) too, and it lies where it was again
//            for that player (ItemPut kItemNotYours). A player holding an item someone took first leaves it, and it
//            goes; one whose own take hadn't gone out yet gives it up (out of the pack). TLD's HUD line tells the
//            player who lost it.
// Not shared: sticks and stones TLD scatters around each player by itself (RadialObjectSpawner).
// Main thread only.

#include <string>
#include <vector>

#include "il2cpp_api.h"
#include "world_keep.h"

namespace world_items {

bool init();
void on_frame(bool in_game);

// Test helpers (dev commands): the loose items within `metres` of us; pick up the nearest loose item (whose name
// contains `name`, if given) the way TLD does (PlayerManager.ProcessPickupItemInteraction); drop `units` of an item
// from our pack (GearItem.Drop); press TLD's inspect screen buttons (Take it / Leave it).
void describe(float metres);
void describe_pack();  // what's in our pack, with ids
bool pickup(const std::string& name);
bool drop(const std::string& name, int units);
bool put_back();
bool take_it();
bool inspect(const std::string& name);  // the nearest loose one into our hands: TLD's Take it / Leave it screen
bool coin(const std::string& how);      // host: the coin toss for takes at the same moment: fair, first or last
bool snare(const std::string& what);    // the nearest snare lying here: its state; set, catch, break or roll it
bool record(const std::string& name);   // the nearest loose item's save record (what ItemPut sends)
// Pots and food on fires near us: how far along; `what`: H (they cook H hours more), take, put NAME, boil L, melt L.
bool cooking(const std::string& what, const std::string& arg);

// Handing an item to a friend (milestone 13, coop_give): TLD's save record for an item (GearItem.CreateSaveData, as
// ItemPut carries it) under a new persistent id (the one given is another item than any left here); an item made from
// one (GearItem.InstantiateAndDeserializeGearItem; in no pack or place yet); what's in our pack; whether an item is in
// it; a stack's units (0: not a stack); one of a kind (never handed over); a prefab's name as TLD shows it; an item's
// prefab (Utils.SanitizePrefabName of its name).
bool gear_record(Il2CppObject* gi, std::string& prefab, std::string& record);
Il2CppObject* gear_from_record(const std::string& prefab, const std::string& record);
std::vector<Il2CppObject*> pack();
bool in_pack(Il2CppObject* gi);
int units(Il2CppObject* gi);
bool one_of_a_kind(Il2CppObject* gi);
std::string gear_name(const std::string& prefab);
std::string prefab_of(Il2CppObject* gi);

// Host (world_keep): what we keep for scenes we aren't in, as the messages it came in, to go with our save; and back.
void save_kept(std::vector<world_keep::Record>& out);
bool load_kept(const world_keep::Record& r);

}  // namespace world_items
