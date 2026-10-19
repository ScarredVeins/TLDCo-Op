#pragma once
// Milestone 8b: what's in a container (cupboards, lockers, corpses...) is the same for both players.
//   both   - a container whose contents change here (an item taken or put in: Container.AddGear / RemoveGear /
//            AddToExistingStackable, or anything else while it's on screen) goes to the other player as ContainerPut
//            with TLD's own save text for it (Container.Serialize). The other player's text goes in the way loading a
//            save does it (Container.DestroyAllGear + Container.Deserialize); an open container panel is refreshed.
//   client - opening a container (Container.BeginContainerOpen, the search) asks the host for it (ContainerWant); the
//            host's contents replace ours, before the panel shows them as long as the answer beats the search.
//   host   - answers from the container itself, or, for a scene it isn't in, from what it last knew; keeps the
//            client's changes for scenes it isn't in and puts them in when it gets there.
// Containers are named by TLD's persistent id (ObjectGuid.GetGuidFromGameObject, the id TLD saves them by), or by
// name and position when they have none. Main thread only.

#include <string>
#include <vector>

#include "il2cpp_api.h"
#include "world_keep.h"

namespace world_containers {

bool init();
void on_frame(bool in_game);

// coop_death: a body's container. fill: what another game's body holds (TLD's save text), put in as loading a save
// does; it's what the others have. told: ours, filled here, went to the others with the body: nothing more to send.
void fill(Il2CppObject* c, const std::string& contents);
void told(Il2CppObject* c);

// Test helpers (dev commands).
void describe(float metres);              // containers within `metres`: id, contents, state
// Open the nearest container (whose name contains `name`): its panel right away, or (search) the way holding the
// interact button does it: the search, then what it found, one item at a time (Take it / Leave it).
bool open(const std::string& name, bool search);
bool close();                             // close the container panel (Panel_Container.OnDone)
bool take(const std::string& name);       // container -> pack, as the panel does it
bool put(const std::string& name);        // pack -> container, as the panel does it

// Host (world_keep): what we keep for scenes we aren't in, as the messages it came in, to go with our save; and back.
void save_kept(std::vector<world_keep::Record>& out);
bool load_kept(const world_keep::Record& r);

}  // namespace world_containers
