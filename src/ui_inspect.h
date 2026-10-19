#pragma once
// UI reconnaissance for the co-op menu (milestone 4c). The first time each TLD screen (Panel_*) opens, its full
// NGUI hierarchy is written to <game>/TLDCoop/ui/<Panel>.txt: objects, components, label texts, sprite names,
// widget depths/sizes, positions and the methods its buttons call. Main thread only.

namespace ui_inspect {

bool init();          // resolves the NGUI API and hooks Panel_Base.Enable
void on_frame();      // writes queued dumps once their screens have settled

}  // namespace ui_inspect
