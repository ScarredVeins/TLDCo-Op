#pragma once
// Automated tests without menu clicks, from coop.ini's [test] section (see config.h):
//   resume=SLOT - press past the boot screens and the intro movie, then SURVIVAL and RESUME on the main menu (the
//                 newest save), or load SLOT
//                 from TLD's list the way RESUME does when it is an older one
//                 (e.g. sandbox5), then past the "press any key" after loading
//   nosave=1    - this copy never saves (a second copy of the game playing the same save next to the first), nor
//                 deletes a save (its files or its slot: TLD deletes the game when its player dies)
//   god=1       - nobody dies while a test runs (PlayerManager.m_God and Condition.m_Invulnerable kept on)
//   lobby=1     - with resume=: SURVIVAL only; the test drives the co-op screens (dev `menu`), and their RESUME
//                 then loads the newest save as TLD's own does
//   own_saves=SLOTS - this copy keeps its save slots in <instance_dir>/Survival, not in TLD's folder (which the
//                 other copy and the player's own games use), so it can save (and delete) for real; the first
//                 time, copies of SLOTS (e.g. sandbox5, comma-separated) and the survival profile files are put
//                 there
//   window=WxH[+X+Y] - a window this size (e.g. 960x540+970+0), kept at X,Y: TLD otherwise applies the player's
//                 own display settings at boot (full screen), and two copies would cover the screen
// Together with test profiles (tld.exe -tldcoop NAME, see paths.h) a host and a friend can run on one PC and be
// driven entirely through their own command.txt and coop.log. Main thread only.

#include <string>

namespace test_harness {

bool init();
void on_frame(const std::string& scene);
// A test copy keeps running behind the other's window from Panel_Boot on, co-op on or not (GameManager.Awake turns
// that off on each scene load; core turns it back on when this says so).
bool runs_unfocused();
// dev `reload`: the test save again without restarting the game, the way a player quits and loads it (TLD's
// pause-menu quit, then SURVIVAL and RESUME as at boot). False when this copy has no resume= (not a test copy).
bool reload();
// dev `needs on|off`: with god=1, whether our needs are paid (PlayerManager.m_God off; Condition.m_Invulnerable
// stays, so nobody dies). Off by default.
void charge_needs(bool on);

}  // namespace test_harness
