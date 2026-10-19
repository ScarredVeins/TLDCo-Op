#pragma once
// Read-only view of the local game: where the player is and what the world is doing. Main thread only.
// Milestone 2 logs these; later milestones send them to the other player.

#include <cstdint>
#include <string>

namespace game {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct PlayerState {
    bool valid = false;  // false in menus / loading
    Vec3 position;       // world space, metres
    float yaw = 0;       // camera, degrees
    float pitch = 0;     // camera, degrees
    float body_yaw = 0;  // player transform euler Y, degrees
    bool crouched = false;  // PlayerManager.PlayerIsCrouched()
};

struct WorldState {
    bool valid = false;
    int day = 0;  // TimeOfDay.GetDayNumber()
    int hour = 0;
    int minute = 0;
    float normalized_time = 0;  // 0..1 through the day
    int weather_stage = -1;     // WeatherStage enum value
    std::string weather_name;   // WeatherStage constant name, e.g. "LightSnow"
    float air_temp_c = 0;       // Weather.GetCurrentTemperature()
    float wind_mph = 0;         // Wind.GetSpeedMPH()
    bool indoors = false;       // Weather.IsIndoorEnvironment()
};

// Resolves every class/method/field used here. Call once after the IL2CPP runtime is ready.
bool init();

// GameManager.m_ActiveScene, e.g. "LakeRegion", "MainMenu". Cheap; safe in menus.
std::string active_scene();

// True for scenes where a player exists (not the boot scene or main menu).
bool is_gameplay_scene(const std::string& scene);

// Which game this is, to tell saves apart: "<save slot>:<TLD's game id>:<the game's random seed>"
// (SaveGameSystem.m_CurrentSaveName, m_CurrentGameId; SceneTransitionData.m_GameRandomSeed, rolled when the game was
// started). "" with no game loaded.
std::string save_id();

// GameManager.m_SecondsSinceLevelLoad: restarts from 0 when a scene finishes loading. -1 if unavailable.
float seconds_since_level_load();

// In a fight with an animal right now (PlayerStruggle.InStruggle).
bool in_struggle();

// What's in the player's hands (milestone 11): PlayerManager.m_ItemInHandsInternal, unless TLD hides it for an
// interaction (PlayerAnimation Hidden).
struct HeldState {
    std::string gear;  // its prefab name (Utils.SanitizePrefabName), "" for empty hands
    uint8_t kind = 0;  // net::HeldKind
    uint8_t flags = 0;  // net::kHeldLit, kHeldAiming, kHeldRaised, kHeldHigh
};
HeldState read_held();

PlayerState read_player();
WorldState read_world();

}  // namespace game
