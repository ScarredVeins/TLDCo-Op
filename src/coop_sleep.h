#pragma once
// Sleeping together (milestone 10 item 12). With friends in the game, time passes for sleep or waiting only when
// everyone is in (in bed, or waiting to pass time), for the hours most of them vote for:
//   - A bed opens TLD's own sleep screen, and passing time (the radial) its pass time screen, made co-op's: a list of
//     4 HOURS, 8 HOURS, 12 HOURS, TILL DAWN and TILL DUSK (the next dawn or dusk by TLD's sky), each with who voted
//     for it. A click picks one (TLD's keys and buttons still step through them). TLD's checks still apply to SLEEP
//     and PASS TIME (too afraid, thin ice...), but not "not tired": anyone can vote.
//   - In while someone is still up, the screen stays: in bed the player rests in real time, TLD's own sleep without
//     the skip (afflictions heal a little faster); waiting, they wait in real time. A click changes the vote; GET UP
//     or STOP WAITING (or Escape) leaves.
//   - Everyone in: the host picks the choice with the most votes (a tie: the shorter) and starts it in every game
//     (Sleep Start). Each game sleeps it or passes the time TLD's own way, over the same real seconds. Whoever is
//     woken (a wolf, the cold) or stops passing time ends it for everyone (Woke, then End Woken).
// Players who are up are told on TLD's HUD who went to bed or is waiting. Alone in a game, TLD's own sleep and
// passing time are untouched. Main thread only.

#include <cstdint>
#include <string>

namespace coop_sleep {

bool init();
void on_frame(const std::string& scene, bool in_game);
// In through co-op's screen (in bed or waiting; in real time, or in the shared skip): State's kStateInBed.
bool in_bed();
// In to pass time, not in bed: State's kStateWaiting.
bool waiting();
// Our vote while in (net::SleepChoice), else 0.
uint8_t vote();
// REST (milestone 13): in bed in real time to heal a little faster (afflictions and condition), no vote for a sleep;
// a player resting is up as far as the shared skip goes.
bool resting();
// In bed (not waiting): where TLD lays our body (Bed.m_BodyPlacementTransform: world x y z, rotation x y z w), for
// State's kStateBed. False when not in a bed.
bool bed_pose(float out[7]);
// dev `sleep`: what we're doing, everyone's votes, what the host would decide.
void describe();
// dev `sleep ACTION [ARG]`: what a player does: open (the nearest bed's screen), wait (the pass time screen), pick
// 4|8|12|dawn|dusk (as TLD's arrows do), click 4|8|12|dawn|dusk (that row of the list, as the mouse does), go (SLEEP
// or PASS TIME), up (GET UP / STOP WAITING), wake (woken in our sleep, as a wolf or the cold does; stopped while
// passing time). False: it couldn't.
bool dev(const std::string& action, const std::string& arg);

}  // namespace coop_sleep
