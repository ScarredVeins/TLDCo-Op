// One-of-a-kind items (net/uniques.h, milestone 10 item 11): which gear is one, and a container's save text without
// the other game's ones and with ours. Exit code 0 when every check holds.

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include "net/uniques.h"

namespace {

int g_failed = 0;

void expect(const char* what, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

// A container's save text as Container.Serialize writes it, with these lists.
std::string container(const std::string& rolled, const std::string& items) {
    return R"({"m_Position":[1.5,2.0,-3.25],"m_Rotation":[0.0,0.0,0.0,1.0],"m_ContainerFilterName":"GAMEPLAY_FilterAll",)"
           R"("m_RolledSpawnChanceProxy":true,"m_NotPopulated":false,"m_GearToInstantiateSerialized":[)" + rolled +
           R"(],"m_SerializedItems":[)" + items + R"(],"m_SerializedDecorationItems":[]})";
}

// A ContainerItemSaveData: the gear's own save text is a JSON string inside (escaped), here with brackets, braces,
// commas and an escaped quote in it to trip a careless scanner.
std::string item(const std::string& prefab, const std::string& tricky = "") {
    return R"({"m_PrefabName":")" + prefab + R"(","m_SerializedGear":"{\"m_Name\":\")" + prefab + tricky +
           R"(\",\"m_Hours\":[1,2],\"m_Text\":\"a, b} ]\\\" [{\"}"})";
}

}  // namespace

int main() {
    {  // The list.
        expect("the list is sorted, for the binary search",
               std::is_sorted(std::begin(net::kUniqueGear), std::end(net::kUniqueGear)));
        expect("...without doubles", std::adjacent_find(std::begin(net::kUniqueGear), std::end(net::kUniqueGear)) ==
                                         std::end(net::kUniqueGear));
        expect("the technical backpack is one of a kind", net::is_unique_gear("GEAR_TechnicalBackpack"));
        expect("...a Mystery Lake note", net::is_unique_gear("GEAR_VisorNoteML1"));
        expect("...a recipe card", net::is_unique_gear("GEAR_RecipeCardPieMeat"));
        expect("...Vaughn's rifle (no narrative, from the hand list)", net::is_unique_gear("GEAR_Rifle_Vaughns"));
        expect("a hatchet, a rifle, a moose-hide satchel (craftable) aren't",
               !net::is_unique_gear("GEAR_Hatchet") && !net::is_unique_gear("GEAR_Rifle") &&
                   !net::is_unique_gear("GEAR_MooseHideBag"));
        expect("a placed copy's name (\" (3)\") and a clone's are the prefab's",
               net::is_unique_gear("GEAR_DarkwalkerDiary11 (3)") && net::is_unique_gear("GEAR_TechnicalBackpack(Clone)") &&
                   net::is_unique_gear("GEAR_TechnicalBackpack(Clone) (12)"));
        expect("a name only starting like one isn't one", !net::is_unique_gear("GEAR_TechnicalBackpackX") &&
                                                             !net::is_unique_gear("GEAR_Technical") &&
                                                             !net::is_unique_gear(""));
        expect("\" (x)\" isn't a copy's number", net::gear_base_name("GEAR_A (x)") == "GEAR_A (x)");
    }
    {  // Stripping: what's sent never has a one-of-a-kind item.
        const std::string seed = R"({"m_Position":[791.5869,213.828,968.036],"m_NotPopulated":true,)"
                                 R"("m_GearToInstantiateSerialized":["GEAR_BeefJerky"],"m_SerializedItems":[],)"
                                 R"("m_SerializedDecorationItems":[]})";
        expect("a never-opened corpse's text (the seed save's) stays as it is", net::strip_uniques(seed) == seed);
        const std::string plain = container(R"("GEAR_Tinder","GEAR_Stick")", item("GEAR_Hatchet") + "," + item("GEAR_Knife"));
        expect("a container without one stays as it is", net::strip_uniques(plain) == plain);
        const std::string mixed = container(R"("GEAR_Tinder","GEAR_VisorNoteML1","GEAR_Stick")",
                                            item("GEAR_Hatchet", "]},") + "," + item("GEAR_TechnicalBackpack") + "," +
                                                item("GEAR_Knife"));
        const std::string want = container(R"("GEAR_Tinder","GEAR_Stick")", item("GEAR_Hatchet", "]},") + "," + item("GEAR_Knife"));
        expect("rolled and made ones go, the others stay (escaped gear text and all)", net::strip_uniques(mixed) == want);
        const std::string only = container(R"("GEAR_VisorNoteML1")", item("GEAR_TechnicalBackpack"));
        expect("lists of only those become empty", net::strip_uniques(only) == container("", ""));
        const std::string spaced = R"({ "m_GearToInstantiateSerialized" : [ "GEAR_VisorNoteML1" , "GEAR_Stick" ] })";
        expect("spaces are fine", net::strip_uniques(spaced) == R"({ "m_GearToInstantiateSerialized" : ["GEAR_Stick"] })");
        const std::string nested = R"({"m_Other":{"m_GearToInstantiateSerialized":["GEAR_VisorNoteML1"]},"m_X":1})";
        expect("a list of the same name nested deeper isn't touched", net::strip_uniques(nested) == nested);
        for (const std::string bad : {std::string(), std::string("not json"), std::string(R"({"m_SerializedItems":[{"m_Pref)"),
                                      container("", item("GEAR_TechnicalBackpack")).substr(0, 150)})
            if (net::strip_uniques(bad) != bad) expect("a text that isn't what's expected stays as it is", false);
        expect("texts that aren't what's expected stay as they are", true);
    }
    {  // Merging: the other game's contents, with this game's own one-of-a-kind items in them still.
        const std::string theirs = container(R"("GEAR_Tinder","GEAR_RecipeCardPieMeat")",
                                             item("GEAR_Hatchet") + "," + item("GEAR_TechnicalBackpack", "theirs"));
        const std::string ours = container(R"("GEAR_VisorNoteML1","GEAR_Stick")",
                                           item("GEAR_Knife") + "," + item("GEAR_TechnicalBackpack", "ours"));
        const std::string want = container(R"("GEAR_Tinder","GEAR_VisorNoteML1")",
                                           item("GEAR_Hatchet") + "," + item("GEAR_TechnicalBackpack", "ours"));
        expect("theirs without their uniques, with ours (our knife and stick were taken: gone)",
               net::merge_uniques(theirs, ours) == want);
        const std::string plain = container(R"("GEAR_Tinder")", item("GEAR_Hatchet"));
        expect("nothing unique on either side: theirs as it is", net::merge_uniques(plain, container(R"("GEAR_Stick")", "")) == plain);
        expect("ours unreadable: theirs stripped", net::merge_uniques(theirs, "junk") ==
                                                       container(R"("GEAR_Tinder")", item("GEAR_Hatchet")));
        expect("merging twice changes nothing more", net::merge_uniques(net::merge_uniques(theirs, ours), ours) == want);
    }
    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "ALL PASS");
    return g_failed ? 1 : 0;
}
