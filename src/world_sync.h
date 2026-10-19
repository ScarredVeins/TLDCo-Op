#pragma once
// Milestone 5: one world clock and one sky. The host publishes its time of day, weather and wind about once a
// second; the client follows them:
//   clock   - snapped to the host's when it drifts by more than a game minute (not while the client itself is
//             sleeping or passing time; it catches up after).
//   weather - the host's own weather set, restored the way TLD restores it from a save (same set by name, its
//             rolled stage lengths, its progress). Both games then run the same set in step; the next set the
//             host's game picks is copied again. A set this region doesn't have falls back to one of the same
//             weather type.
//   wind    - the host's strength and direction, blended in like TLD's own wind changes.
// Nothing here forces TLD into a state it wouldn't save on its own, so the client's saves stay ordinary and there
// is nothing to undo when co-op ends. Main thread only.

#include <string>

namespace world_sync {

bool init();
// scene: GameManager.m_ActiveScene; in_game: it's a gameplay scene. After each scene change of our own (a load
// may restore TLD's own weather) the client checks everything again straight away.
void on_frame(const std::string& scene, bool in_game);

}  // namespace world_sync
