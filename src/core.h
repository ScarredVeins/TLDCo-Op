#pragma once

#include <string>

#include "config.h"

namespace core {

// Runs once on the game's main thread, right after il2cpp_init returns and before any game code has run.
void on_runtime_ready();

// Start, restart or stop co-op to match `s` (from the co-op menu). Main thread only.
void apply_settings(const config::Settings& s);

// Ends co-op if it is on (e.g. back at the Survival menu). Main thread only.
void stop_coop(const char* why);

// The scene we're in once it has finished loading and TLD has put the player in place (what we report to the other
// player as "in"); "" while loading, in menus, or with co-op off.
const std::string& settled_scene();

}  // namespace core
