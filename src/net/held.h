#pragma once
// What a player holds, as State carries it (milestone 11), and what the other games make of it: what TLD's animals
// see when they ask about that player's hands (AiTarget.HoldingLitTorch / HoldingLitFlare / HoldingRaisedSpear),
// whether it lights the world around them, and its name for the log. Plain code, no IL2CPP (held_test).
//
// The sender reads it from its own player (game::read_held): the item in TLD's hands
// (PlayerManager.m_ItemInHandsInternal) and which of GearItem's parts it has (m_TorchItem, m_FlareItem...).

#include <cstdint>
#include <string>

namespace net {

enum class HeldKind : uint8_t {
    None = 0,    // empty hands (or the item hidden for an interaction)
    Other,       // anything else TLD lets a player hold
    Torch,       // TorchItem
    FlareRed,    // FlareItem, FlareType Red: the road flare
    FlareBlue,   // FlareItem, FlareType Blue: the marine flare
    Lantern,     // KeroseneLampItem
    Flashlight,  // FlashlightItem
    Rifle,       // GunItem, GunType Rifle
    Revolver,    // GunItem, GunType Revolver
    FlareGun,    // GunItem, GunType FlareGun
    Camera,      // GunItem, GunType Camera
    Bow,         // BowItem
    Spear,       // BearSpearItem
    Stone,       // StoneItem
    NoiseMaker,  // NoiseMakerItem
    Count
};

constexpr uint8_t kHeldLit = 1;     // a torch or flare burning, a lantern or flashlight on
constexpr uint8_t kHeldAiming = 2;  // raised to aim or throw (PlayerAnimation ToAiming, Aiming, FromAiming, Firing;
                                    // a bow: drawn, BowState ReadyToAim, Aim, Firing)
constexpr uint8_t kHeldRaised = 4;  // a bear spear set and raised (BearSpearItem SpearState Raised)
constexpr uint8_t kHeldHigh = 8;    // with kHeldLit: a flashlight on its high beam

constexpr int32_t kFlareRed = 0, kFlareBlue = 1;  // TLD's FlareType

inline HeldKind held_kind(uint8_t wire) {
    return wire < static_cast<uint8_t>(HeldKind::Count) ? static_cast<HeldKind>(wire) : HeldKind::Other;
}

inline const char* held_kind_name(HeldKind k) {
    constexpr const char* kNames[] = {"nothing", "item",     "torch",     "red flare", "blue flare",
                                      "lantern", "flashlight", "rifle",   "revolver",  "flare gun",
                                      "camera",  "bow",      "bear spear", "stone",    "noisemaker"};
    const auto i = static_cast<size_t>(k);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "item";
}

// What a wolf, bear or cougar sees in that player's hands (TLD asks the target it is after).
inline bool holds_lit_torch(HeldKind k, uint8_t flags) { return k == HeldKind::Torch && (flags & kHeldLit); }

inline bool holds_lit_flare(HeldKind k, uint8_t flags, int32_t flare_type) {
    if (!(flags & kHeldLit)) return false;
    return (k == HeldKind::FlareRed && flare_type == kFlareRed) || (k == HeldKind::FlareBlue && flare_type == kFlareBlue);
}

inline bool holds_raised_spear(HeldKind k, uint8_t flags) { return k == HeldKind::Spear && (flags & kHeldRaised); }

// It lights the world around them (the avatar turns the item's own lights on).
inline bool gives_light(HeldKind k, uint8_t flags) {
    if (!(flags & kHeldLit)) return false;
    return k == HeldKind::Torch || k == HeldKind::FlareRed || k == HeldKind::FlareBlue || k == HeldKind::Lantern ||
           k == HeldKind::Flashlight;
}

// "a lit torch (GEAR_Torch), aiming" for the log.
inline std::string describe_held(const std::string& gear, HeldKind k, uint8_t flags) {
    if (k == HeldKind::None) return "nothing";
    const std::string name = held_kind_name(k);
    std::string s = (flags & kHeldLit) ? std::string("a lit ") : std::string(std::string("aeiou").find(name[0]) != std::string::npos ? "an " : "a ");
    s += name;
    if (k == HeldKind::Flashlight && (flags & kHeldLit)) s += (flags & kHeldHigh) ? " (high beam)" : " (low beam)";
    if (!gear.empty()) s += " (" + gear + ")";
    if (flags & kHeldAiming) s += ", aiming";
    if (flags & kHeldRaised) s += ", raised";
    return s;
}

}  // namespace net
