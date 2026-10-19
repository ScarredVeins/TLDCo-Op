#pragma once
// The other players as seen in our world, one avatar each. They pick a body (milestone 4b): the placeholder capsule
// (a colour per player), or one of the Episode 5 story characters from TLDCoop\tldcoop_bodies.bundle (built from the
// player's own install), animated idle/walk/run by their real speed. Drawn ~100 ms behind real time with snapshot
// interpolation, only while they're in our scene. Main thread only.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "il2cpp_api.h"
#include "net/session.h"

namespace avatar {

bool init();

// true: replace the body pack's own shaders with TLD's skinned shaders when loaded (coop.ini body_shader=tld).
void use_tld_shaders(bool on);

// Feed newly received player states (oldest first; state.player says whose).
void push(std::vector<net::TimedState>& states);

// Who else is in the game (Status.players): their names and the bodies they chose (ids from body_ids(); unknown ids
// fall back to the capsule). Anyone not listed any more is removed.
void set_players(const std::vector<net::PlayerInfo>& players);

// Every frame. `local_scene` is our active scene; `local_in_game` is false in menus/loading.
void update(const std::string& local_scene, bool local_in_game);

// Every frame after the animation step (GameManager.LateUpdate): what each holds (milestone 11) goes into their hand,
// their arms posed for it over the clip.
void late_update();

// What each avatar holds as shown here, for the log and dev command `held`: the copy, whether it's lit, how far its
// grip is from the hand, the hold in use.
std::string describe_held();

// The lights in `go` and its children, on or off: colour, intensity, range (dev command `hands`, to set the real
// item's light beside the copy's).
std::string describe_lights(Il2CppObject* go);

// Dev command `hold KEY FIELD VALUES`: changes how a kind is held, for tuning against screenshots (KEY as
// describe_held prints it, e.g. torch, rifle_aim; FIELD rh|lh x y z, dir|up x y z, grip G, left M, look|right|leftarm|
// hangs 0|1, flip 0|1; or `hold pitch 1|-1`; or `hold part NAME 0|1`, that part of each copy shown or hidden). Says
// the hold now.
std::string tune_hold(const std::string& args);

// Status for the log, e.g. "player 1 'Ann': wolf shown at (1.2, 0.0, 3.4) yaw 90; player 2: hidden: other scene".
std::string describe();

// A player's avatar as something the host's animals can perceive (its AiTarget), while it's shown in our scene.
// False when hidden (or there is no such player).
bool stand_in(uint8_t player, Il2CppObject*& go, Il2CppObject*& transform, Il2CppObject*& ai_target);
std::vector<uint8_t> shown();                    // the players whose avatars are shown in our scene now
std::optional<uint8_t> player_of(Il2CppObject* ai_target_or_root);  // whose avatar this is, if one

// The others as the HUD shows them (milestone 12: names over heads, the radar, the map): from their latest state, and
// where their avatar is drawn while it's in our scene.
struct Seen {
    uint8_t player = 0;
    std::string name;
    bool playing = false;  // their states are coming and they're in a game (not a menu or loading)
    bool here = false;     // their avatar is shown in our scene: x, y, z and yaw are where it's drawn
    bool dead = false;
    std::string scene;     // the scene they're in (when playing)
    float x = 0, y = 0, z = 0;  // their feet, world space
    float yaw = 0;              // which way they face (Unity's yaw)
    float at_x = 0, at_z = 0;   // where their latest state has them (not where a handover draws them)
    bool crouched = false;
};
std::vector<Seen> seen();

bool is_known_body(const std::string& id);
std::string body_ids();  // "capsule, wolf, greywolf, ..." for messages and the ini
bool female_body(const std::string& id);  // the body pack's female rig (the clips come per rig)
bool female(uint8_t player);              // their avatar's body is

// A handover a player's body plays (milestone 13), from coop_give every frame before update(): the give or receive
// clip at a time on the pair's shared timeline (net/handover.h), the item in their hand, facing the partner.
struct HandoverShow {
    bool giver = true;            // the give clip, else the receive clip
    float t = -1;                 // the clip's time (-1: not in it)
    bool fading = false;          // fading out of it
    std::string gear;             // what is handed over: its model is in their hand
    uint8_t item = 0;             // net::HandItem: which grip
    bool item_in_hand = false;    // in this body's hand now
    bool giver_female = false;    // the giver's rig (the receive clip is the one for it)
    bool partner_female = false;  // the partner's rig (how far apart they stand)
    float partner_x = 0, partner_z = 0;  // where the partner is (world): they face it at the clips' distance
    bool partner_moves = false;   // the partner is drawn here too (both are others): each goes half the way
};
void show_handover(uint8_t player, const HandoverShow* show);  // nullptr: none

// Dev command `pose bed|crouch FIELD VALUE`: how a body lies in bed (yaw, lift) and crouches (depth, back, tilt,
// lean, knees), for tuning against screenshots. Says the values now.
std::string tune_pose(const std::string& args);

// The character on the co-op screens (setup and lobby): body `id` from the body pack, idling in a three-quarter
// view, lit and filmed by a camera of its own far below the menu scene into a texture. Returns that texture (the same
// one while the preview lives; the body changes in place), or null when the body can't be shown (capsule, no pack).
Il2CppObject* preview(const std::string& id);
void end_preview();  // the stage, the body and the texture go

}  // namespace avatar
