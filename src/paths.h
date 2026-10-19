#pragma once
#include <filesystem>
#include <string>

namespace paths {

// Folder that contains tld.exe.
std::filesystem::path game_dir();

// <game>/TLDCoop, created on first use. Holds what every copy of the game shares: the type dump, the body pack.
std::filesystem::path mod_dir();

// The test profile this copy of the game runs as ("-tldcoop NAME" on tld.exe's command line), or "" normally.
// With a profile, two copies of the game can run side by side on one PC (host and friend), each with its own
// settings, log and dev commands.
std::wstring profile();

// Where this copy's coop.ini, coop.log and command.txt live: mod_dir(), or mod_dir()/<profile>. Created on first use.
std::filesystem::path instance_dir();

// Text of tld_Data/StreamingAssets/version.txt, trimmed (e.g. "2.55 181032 S"), or "" if missing.
std::string game_version();

}  // namespace paths
