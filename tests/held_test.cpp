// What a player holds (net/held.h, milestone 11): what TLD's animals see in a friend's hands (their questions about
// the target's torch, flare and spear), what lights the world around them, and the wire kind read back safely.
// Exit code 0 when every check holds.

#include <cstdio>
#include <string>

#include "net/held.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

}  // namespace

int main() {
    using namespace net;
    {  // A wolf asks whether the one it's after holds a lit torch.
        expect("a burning torch scares", holds_lit_torch(HeldKind::Torch, kHeldLit));
        expect("...one that isn't lit doesn't", !holds_lit_torch(HeldKind::Torch, 0));
        expect("...aiming with it changes nothing", holds_lit_torch(HeldKind::Torch, kHeldLit | kHeldAiming));
        expect("a lit lantern isn't a torch", !holds_lit_torch(HeldKind::Lantern, kHeldLit));
        expect("a lit flare isn't a torch", !holds_lit_torch(HeldKind::FlareRed, kHeldLit));
        expect("empty hands aren't", !holds_lit_torch(HeldKind::None, kHeldLit));
    }
    {  // ...a lit flare of one colour (TLD asks red and blue apart: wolves and bears react to them differently).
        expect("a lit red flare, asked red", holds_lit_flare(HeldKind::FlareRed, kHeldLit, kFlareRed));
        expect("...asked blue: no", !holds_lit_flare(HeldKind::FlareRed, kHeldLit, kFlareBlue));
        expect("a lit blue flare, asked blue", holds_lit_flare(HeldKind::FlareBlue, kHeldLit, kFlareBlue));
        expect("...asked red: no", !holds_lit_flare(HeldKind::FlareBlue, kHeldLit, kFlareRed));
        expect("an unlit flare: no", !holds_lit_flare(HeldKind::FlareRed, 0, kFlareRed));
        expect("a lit torch isn't a flare", !holds_lit_flare(HeldKind::Torch, kHeldLit, kFlareRed));
        expect("a flare gun isn't a flare", !holds_lit_flare(HeldKind::FlareGun, kHeldLit | kHeldAiming, kFlareRed));
    }
    {  // ...a bear spear set and raised.
        expect("a raised spear", holds_raised_spear(HeldKind::Spear, kHeldRaised));
        expect("...one held low isn't", !holds_raised_spear(HeldKind::Spear, 0));
        expect("a raised rifle isn't a spear", !holds_raised_spear(HeldKind::Rifle, kHeldRaised | kHeldAiming));
    }
    {  // What lights the world around them.
        for (HeldKind k : {HeldKind::Torch, HeldKind::FlareRed, HeldKind::FlareBlue, HeldKind::Lantern,
                           HeldKind::Flashlight}) {
            const std::string what = std::string("a lit ") + held_kind_name(k) + " gives light, unlit it doesn't";
            expect(what.c_str(), gives_light(k, kHeldLit) && !gives_light(k, 0));
        }
        expect("a rifle never does", !gives_light(HeldKind::Rifle, kHeldLit));
        expect("a noisemaker's fuse doesn't", !gives_light(HeldKind::NoiseMaker, kHeldLit));
    }
    {  // The wire's kind byte: one from a newer game reads as "an item", never past the names.
        expect("known kinds read back", held_kind(static_cast<uint8_t>(HeldKind::Bow)) == HeldKind::Bow);
        expect("an unknown kind is an item", held_kind(200) == HeldKind::Other);
        expect("every kind has a name", std::string(held_kind_name(HeldKind::NoiseMaker)) == "noisemaker" &&
                                            std::string(held_kind_name(HeldKind::None)) == "nothing");
        expect("the log line", describe_held("GEAR_Flashlight", HeldKind::Flashlight, kHeldLit | kHeldHigh) ==
                                   "a lit flashlight (high beam) (GEAR_Flashlight)" &&
                                   describe_held("GEAR_Rifle", HeldKind::Rifle, kHeldAiming) ==
                                       "a rifle (GEAR_Rifle), aiming" &&
                                   describe_held("", HeldKind::None, 0) == "nothing" &&
                                   describe_held("GEAR_PackMatches", HeldKind::Other, 0) == "an item (GEAR_PackMatches)");
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
