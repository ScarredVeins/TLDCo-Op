#pragma once
// Milestone 13: handing an item to a friend. Look at a friend close by (TLD's crosshair: "Give to NAME") and interact:
// TLD's inventory opens, its Drop button says GIVE; pick anything (a stack: how many, TLD's own units screen) and our
// body holds it out to them. They see "NAME holds out X for you", look at us and interact ("Take X from NAME"): their
// body reaches for it and it's in their pack. Interacting again before they take it takes it back ("Take back X");
// so does walking away, or nobody taking it for net::kOfferTimeoutMs. One of a kind items are each player's own and
// can't be given. Every game sees the two bodies play the handover (net/handover.h for the timeline and the ledger;
// avatar::show_handover for the clips).
//   giver    - offers; once it's accepted, still holding the item, takes it (or the units given) out of its pack and
//              sends TLD's save record for it (GiveMsg Done), or says it no longer has it (Failed).
//   receiver - accepts; makes the item from the record (GearItem.InstantiateAndDeserializeGearItem) and puts it in its
//              pack the way a pickup does (PlayerManager.AddItemToPlayerInventory) once its hand has it (kTransferAt),
//              exactly once.
//   host     - passes each friend's messages on to the others (the record only to the receiver).
// Main thread only.

#include <string>

namespace coop_give {

bool init();
// Every frame, before the avatars update: messages, timeouts, and what each body shows.
void on_frame(bool in_game);

// Dev command `handover ...` (`give` is the one that puts gear in our own pack): status; look [ID] (face the nearest
// friend, or that one); face YAW [PITCH] (turn our camera); press (TLD's interact, as the key does); drop GEAR (the
// inventory's GIVE on that item of our pack); units N (the units screen's GIVE, N of them); close (shut the give
// screens); offer ID GEAR [UNITS] (straight away, no screens); accept; back. False: not done (the log says why).
bool dev(const std::string& action, const std::string& args);
void describe();

}  // namespace coop_give
