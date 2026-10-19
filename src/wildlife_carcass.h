#pragma once
// Milestone 7e: shared kills. A dead animal is the same carcass for both players, and harvesting counts once.
//   host   - about twice a second, each dead shared animal with its carcass ready (BodyHarvest enabled): prefab, pose,
//            fall side, meat, hide, guts, condition, frozen, harvested / ravaged, seconds dead. The other player's
//            harvests come back as "took N kg / N hide / N gut" and are taken off the real carcass; their quartering
//            removes it (BodyHarvestDestroy). The host's own harvests, wolves feeding and decay simply show in the state.
//   client - once the host's animal has finished dying (5 s, the copy shows the death), a real TLD carcass takes the
//            copy's place: built the way TLD restores a saved carcass (BodyHarvestManager.InstantiateBodyHarvestGameObject
//            + MaybeRestoreCorpseState), kept to the host's numbers, out of our own save, and opened with TLD's own
//            harvest panel. What we take there (Panel_BodyHarvest.HarvestSuccessful / QuarterSuccessful) goes to the host;
//            its state isn't applied over ours until it has counted it.
//   link down (milestone 10): the friend keeps its carcasses while the host's states don't come (netrt::offline) and
//            may harvest or quarter them; those go to the host once the link is back (the session keeps them as world
//            changes) and the host counts each harvest once (net::HarvestLedger). The host's carcass list goes to a
//            friend back from a dropped link only once it has caught up, and a carcass the friend quartered never
//            comes back for it.
// Main thread only.

#include <string>

namespace wildlife_carcass {

bool init();
void on_frame(bool in_game);

// Test helpers (dev commands), either side: take this much from the nearest carcass through TLD's harvest panel (as
// if harvested by hand), or quarter it; describe the carcasses around.
bool harvest(float meat_kg, int hide, int gut);
bool quarter();
void describe();

}  // namespace wildlife_carcass
