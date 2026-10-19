#pragma once
// Milestone 9c: what the host keeps of the world outside TLD's own save goes with that save. The world modules keep,
// for scenes the host isn't in, the friend's changes there (items, broken down / harvested, fires, containers) until
// the host gets there; without this they'd be gone when the host quits, and the next session the friend's taken items
// would be back in the host's world (copies once the friend sees them again).
//   save:  whenever TLD saves (SaveGameSystem.SaveGlobalData, part of every save; not called when a save is skipped)
//          and we're hosting, every module's kept entries are written, as the protocol messages they came in, to
//          <instance_dir>/worlds/<game::save_id()>.bin (written whole, then renamed over the old one).
//   load:  on the first frame in the game after the main menu, while hosting, that file (if any) is read back into
//          the modules' stores, the way the messages were kept when they came.
// Host only (the friend's game keeps nothing for later). Main thread only.

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace world_keep {

enum class Tag : uint8_t {
    ItemChange = 1,       // ItemPut / ItemGone (world_items g_pending)
    ObjectChange = 2,     // ObjectPut (world_objects g_store)
    ObjectGone = 3,       // ObjectPut of an object without a TLD id broken down (world_objects g_gone)
    FireChange = 4,       // FirePut (world_fires g_store)
    ContainerTheirs = 5,  // ContainerPut: the friend's change (world_containers g_store, from_peer)
    ContainerOurs = 6,    // ContainerPut: what we had there when we left
};

struct Record {
    Tag tag = Tag::ItemChange;
    std::vector<uint8_t> msg;  // a whole protocol message (a Writer's bytes)
};

inline Record record(Tag tag, const net::Writer& w) { return Record{tag, std::vector<uint8_t>(w.data(), w.data() + w.size())}; }

// The record's message as if it had just come in, for the read_* functions (packet_body).
inline net::Packet packet(const Record& r) {
    net::Packet p;
    p.type = static_cast<net::Msg>(r.msg.size() > 6 ? r.msg[6] : 0);
    p.data = r.msg;
    return p;
}

bool init();
void on_frame(const std::string& scene, bool in_game);

}  // namespace world_keep
