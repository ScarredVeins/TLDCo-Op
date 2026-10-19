#pragma once
// Catches the replies to TLDCoop's own Steam requests inside TLD's manual Steam dispatch, before TLD frees them
// (see net::steam_calls).

namespace steam_dispatch {

void init();      // lets the Steam transport install the hook the moment Steam is up (network thread)
void on_frame();  // fallback: installs the hook once TLD has loaded steam_api64.dll (main thread)

}  // namespace steam_dispatch
