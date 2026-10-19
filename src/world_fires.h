#pragma once
// Milestone 8d: fires are the same for both players: lit, fed, down to embers, out; a campfire one player makes is
// there for the other.
//   both   - each game checks its fires twice a second. A fire whose state changes (lit, out, embers, fuel added: its
//            longest burn time; charcoal; the campfire's state) goes to the other player as FirePut with TLD's own save
//            record for it (its entry in FireManager.Serialize()). The other game loads it the way loading a save does
//            (FireManager.Deserialize with just that fire: found by id or position, or the campfire is made). A fire
//            that goes out in one game only (the wind cuts its burn time there) goes when it's out. A fire that's
//            gone (dismantled: TLD counts it destroyed) goes as gone, and the other game removes it the way a
//            dismantle does (its save keeps it gone too). A fire being lit (TLD's starting steps) or a campfire
//            being placed waits until it's lit: the other game can't follow those steps.
//   host   - when the client comes into the host's scene, the host sends all its fires there (FireList: the whole
//            FireManager.Serialize()); the client loads them. The client's changes in a scene the host isn't in are
//            kept and loaded when the host gets there.
// Fires are named by TLD's persistent id (ObjectGuid.GetGuidFromGameObject, what FireManager saves them by). Main thread
// only.

#include <string>
#include <vector>

#include "world_keep.h"

namespace world_fires {

bool init();
void on_frame(bool in_game);

// Test helpers (dev commands).
void describe(float metres);           // fires within `metres`: id, state, minutes left; the first one's save record
bool light(const std::string& name);   // light the nearest unlit fire the way TLD's fire panel does it
bool feed(const std::string& name);    // put fuel (whose name contains `name`) from the pack on the nearest burning fire
bool campfire();                       // make a campfire 1.5 m ahead of us (FireManager.InstantiateCampFire), unlit
bool burn_out(float seconds);          // the nearest burning fire burns `seconds` more, here only (what the wind does)
bool dismantle(const std::string& name);  // dismantle the nearest fire that isn't burning, as TLD's action does
bool cook(const std::string& name);    // put food or a pot (name contains `name`) from the pack on the nearest burning fire

// Host (world_keep): what we keep for scenes we aren't in, as the messages it came in, to go with our save; and back.
void save_kept(std::vector<world_keep::Record>& out);
bool load_kept(const world_keep::Record& r);

}  // namespace world_fires
