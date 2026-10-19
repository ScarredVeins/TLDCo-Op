#pragma once
// One-of-a-kind items (milestone 10 item 11): the technical backpack, notes and journals, keys, postcards, recipe
// cards, the named rifles and the like. TLD places each once (or one of a few spots), never in a loot table; in co-op
// each player takes their own where it lies, and what they drop stays theirs. So one is never shared: no ItemPut or
// ItemGone, never in the host's item lists, and kept out of a container's shared contents (each game keeps its own
// in its own copy of the container). This is the plain part: the names, and the container text without the other
// game's ones and with ours. No IL2CPP.
//   The list: every GEAR_ prefab of TLD 2.55 with a NarrativeCollectibleItem (234: notes, keys, postcards, recipes,
// the backpack, Wintermute's story items), none of them in a loot table or a spawn list; and unique gear without
// one, placed or spawned once per world (not craftable, in no loot table). Generated from the game's prefabs.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace net {

inline constexpr std::string_view kUniqueGear[] = {
    "GEAR_AirfieldCabinNote", "GEAR_AirfieldCargomasterNote", "GEAR_AirfieldControlNote",
    "GEAR_AirfieldGeologistNote1", "GEAR_AirfieldHangarNote", "GEAR_AirfieldJunkerNote", "GEAR_AirfieldSecChiefNote1",
    "GEAR_AirfieldTowerNote", "GEAR_BIKey1", "GEAR_BIKey2", "GEAR_BRKey1", "GEAR_BRKey2", "GEAR_BackerNote1A",
    "GEAR_BackerNote1B", "GEAR_BackerNote1C", "GEAR_BackerNote2A", "GEAR_BackerNote2B", "GEAR_BackerNote2C",
    "GEAR_BackerNote3A", "GEAR_BackerNote3B", "GEAR_BackerNote3C", "GEAR_BackerNote4A", "GEAR_BackerNote4B",
    "GEAR_BackerNote4C", "GEAR_BallisticVest", "GEAR_BankManagerHouseKey", "GEAR_BankVaultCode",
    "GEAR_BearSpearBrokenStory", "GEAR_BearSpearStory", "GEAR_BlackrockAdminNote", "GEAR_BlackrockAmmoRoomNote",
    "GEAR_BlackrockCodeNote", "GEAR_BlackrockSecurityNote", "GEAR_BlackrockTowerNote",
    "GEAR_BlueprintCard_BushcraftBow", "GEAR_BlueprintCard_WolfSkinHat", "GEAR_BlueprintCard_WolfSkinPants",
    "GEAR_BoltCutters", "GEAR_Bow_Manufactured", "GEAR_Bow_Woodwrights", "GEAR_BunkerClue1a", "GEAR_BunkerClue1b",
    "GEAR_BunkerClue1c", "GEAR_BunkerClue1d", "GEAR_BunkerPapers1", "GEAR_BunkerPapers2", "GEAR_BunkerPapers3",
    "GEAR_BunkerPapers4", "GEAR_BunkerSchematic_A", "GEAR_BunkerSchematic_B", "GEAR_BunkerSchematic_C",
    "GEAR_CanneryCodeNote", "GEAR_CanneryMemo", "GEAR_CannerySurvivalPath", "GEAR_CanyonClimbersCaveNote",
    "GEAR_CanyonDeadClimberNote", "GEAR_CanyonFishingHutJournal", "GEAR_CanyonMinersNote", "GEAR_ClimbersJournal",
    "GEAR_ClipBoardBunker_A", "GEAR_ClipBoardBunker_B", "GEAR_ClipBoardBunker_C", "GEAR_ClipBoardBunker_D",
    "GEAR_ClipBoardTale2_A", "GEAR_ClipBoardTale2_B", "GEAR_ClipBoardTale2_C", "GEAR_ClipBoardTale2_D",
    "GEAR_CougarPoster1", "GEAR_CougarPoster2", "GEAR_DamFenceKey", "GEAR_DamGarbageLetter", "GEAR_DamLockerKey",
    "GEAR_DamOfficeKey", "GEAR_DarkwalkerDiary1", "GEAR_DarkwalkerDiary10", "GEAR_DarkwalkerDiary11",
    "GEAR_DarkwalkerDiary2", "GEAR_DarkwalkerDiary3", "GEAR_DarkwalkerDiary4", "GEAR_DarkwalkerDiary5",
    "GEAR_DarkwalkerDiary6", "GEAR_DarkwalkerDiary7", "GEAR_DarkwalkerDiary8", "GEAR_DarkwalkerDiary9",
    "GEAR_DarkwalkerID", "GEAR_DeadmanNote1", "GEAR_DeadmanNote2", "GEAR_DeadmanNote3", "GEAR_DeadmanNote4",
    "GEAR_DeadmanNote5", "GEAR_Ep4LockerKey1", "GEAR_Ep4LockerKey2", "GEAR_Ep4LockerKey3", "GEAR_Ep4LockerKey8",
    "GEAR_Ep4PrisonGateKey", "GEAR_Ep4RumoursWolfReport", "GEAR_FarmerDepositBoxKey", "GEAR_Flashlight_LongLasting",
    "GEAR_HatchWheel", "GEAR_InsulatedFlask_F", "GEAR_Insulin", "GEAR_KeroseneLamp_Spelunkers", "GEAR_LakeCabinKey3",
    "GEAR_LakeTrailerKey1", "GEAR_LangstonMineKey1", "GEAR_LangstonMineLockboxKey1", "GEAR_LogbookTale2_A01",
    "GEAR_LogbookTale2_A02", "GEAR_LogbookTale2_A03", "GEAR_LogbookTale2_A04", "GEAR_Logbook_A01", "GEAR_Logbook_A02",
    "GEAR_Logbook_A03", "GEAR_Logbook_A04", "GEAR_MedicalSupplies", "GEAR_MiltonDepositBoxKey1",
    "GEAR_MiltonDepositBoxKey2", "GEAR_MiltonDepositBoxKey3", "GEAR_MineRegionBunkhouseNote",
    "GEAR_MineRegionPumphouseNote", "GEAR_MountainPassAvalancheNote", "GEAR_MountainPassWeatherStationNote",
    "GEAR_MountainTownFarmKey", "GEAR_MountainTownLockBoxKey", "GEAR_NaturalistNote1", "GEAR_NaturalistNote2",
    "GEAR_OldLadyStolenItem", "GEAR_PostCard_AC_CentralSpire", "GEAR_PostCard_AC_TopShelf", "GEAR_PostCard_AF_Cabin",
    "GEAR_PostCard_AF_Plateau", "GEAR_PostCard_BI_EchoOne-RadioTower", "GEAR_PostCard_BR_Canyon",
    "GEAR_PostCard_BR_Prison", "GEAR_PostCard_CR_AbandonedLookout", "GEAR_PostCard_FM_MuskegOverlook",
    "GEAR_PostCard_FM_ShortwaveTower", "GEAR_PostCard_ML_ForestryLookout", "GEAR_PostCard_ML_LakeOverlook",
    "GEAR_PostCard_MP1_ForwardCamp", "GEAR_PostCard_MP2_Basement", "GEAR_PostCard_MP3_HighPeak",
    "GEAR_PostCard_MP4_Bunker", "GEAR_PostCard_MP_BrokenRoad", "GEAR_PostCard_MP_OgresTeardrop",
    "GEAR_PostCard_MR_Settlement", "GEAR_PostCard_MR_Window", "GEAR_PostCard_MT_RadioTower",
    "GEAR_PostCard_PV_SignalHill", "GEAR_PostCard_RV_Pensive", "GEAR_PostCard_TM_AndresPeak",
    "GEAR_PostCard_TM_TailSection", "GEAR_PostCard_Tale02", "GEAR_RecipeCardBarPemmican", "GEAR_RecipeCardFishcakes",
    "GEAR_RecipeCardPancakePeach", "GEAR_RecipeCardPieFishermans", "GEAR_RecipeCardPieForagers",
    "GEAR_RecipeCardPieMeat", "GEAR_RecipeCardPiePredator", "GEAR_RecipeCardPorridgeFruit",
    "GEAR_RecipeCardSoupPotato", "GEAR_RecipeCardSoupPtarmigan", "GEAR_RecipeCardSoupRabbit",
    "GEAR_RecipeCardStewMeat", "GEAR_RecipeCardStewVegetables", "GEAR_Respirator", "GEAR_RevolverFancy",
    "GEAR_RevolverGreen", "GEAR_RevolverStubNosed", "GEAR_RifleScabbardA", "GEAR_Rifle_Barbs", "GEAR_Rifle_Curators",
    "GEAR_Rifle_Vaughns", "GEAR_Rudiger_Watch", "GEAR_RuralRegionFarmKey", "GEAR_SecurityChiefID",
    "GEAR_SteamPipeValve", "GEAR_SurvivalElevatorCrank", "GEAR_TacticalGloves", "GEAR_TacticalJacket",
    "GEAR_Tale1ChiefNote1", "GEAR_Tale1ChiefNote2", "GEAR_Tale1ChiefNote3", "GEAR_Tale1ChiefNote4",
    "GEAR_Tale1ChiefNote5", "GEAR_Tale1Transcript1", "GEAR_Tale1Transcript2", "GEAR_Tale1Transcript3",
    "GEAR_Tale1Transcript4", "GEAR_Tale2GeologistNote1", "GEAR_Tale2RudigerNote1", "GEAR_Tale2SecurityChiefBones",
    "GEAR_Tale2_CorpseKey", "GEAR_Tale2_LockerKey", "GEAR_Tale3ForemanLog1", "GEAR_Tale3ForemanLog2",
    "GEAR_Tale3ForemanLog3", "GEAR_Tale3ForemanLog4", "GEAR_Tale3RudigerNote1", "GEAR_Tale3RudigerNote2",
    "GEAR_Tale3RudigerNote3", "GEAR_Tale3_ForemanRefugeKey", "GEAR_Tale_03_PostCard_01", "GEAR_Tale_03_PostCard_02",
    "GEAR_Tale_03_PostCard_03", "GEAR_Tale_03_PostCard_04", "GEAR_Tale_03_PostCard_05", "GEAR_TalesTestNote1",
    "GEAR_TechnicalBackpack", "GEAR_ToolBelt", "GEAR_Trader_Pamphlet", "GEAR_VisorNoteAC1", "GEAR_VisorNoteAC2",
    "GEAR_VisorNoteAC3", "GEAR_VisorNoteACKey1", "GEAR_VisorNoteBI1", "GEAR_VisorNoteBI2", "GEAR_VisorNoteBI3",
    "GEAR_VisorNoteBR1", "GEAR_VisorNoteBR2", "GEAR_VisorNoteBR3", "GEAR_VisorNoteBlackrock1",
    "GEAR_VisorNoteBlackrock2", "GEAR_VisorNoteBlackrock3", "GEAR_VisorNoteBlackrockKey3", "GEAR_VisorNoteCH1",
    "GEAR_VisorNoteCH2", "GEAR_VisorNoteDP1", "GEAR_VisorNoteDP2", "GEAR_VisorNoteDP3", "GEAR_VisorNoteDPKey1",
    "GEAR_VisorNoteFM1", "GEAR_VisorNoteFM2", "GEAR_VisorNoteFM3", "GEAR_VisorNoteHRV1", "GEAR_VisorNoteHRV2",
    "GEAR_VisorNoteHRV3", "GEAR_VisorNoteHRVKey1", "GEAR_VisorNoteML1", "GEAR_VisorNoteML2", "GEAR_VisorNoteML3",
    "GEAR_VisorNoteMLKey2", "GEAR_VisorNoteMLKey3", "GEAR_VisorNoteMT1", "GEAR_VisorNoteMT2", "GEAR_VisorNoteMT3",
    "GEAR_VisorNoteMTKey1", "GEAR_VisorNotePV1", "GEAR_VisorNotePV2", "GEAR_VisorNotePV3", "GEAR_VisorNoteTWM1",
    "GEAR_VisorNoteTWM2",
};

// "GEAR_DarkwalkerDiary11 (3)" (a placed copy), "GEAR_TechnicalBackpack(Clone)" -> the prefab's name.
inline std::string_view gear_base_name(std::string_view name) {
    for (bool changed = true; changed;) {
        changed = false;
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        constexpr std::string_view kClone = "(Clone)";
        if (name.size() >= kClone.size() && name.substr(name.size() - kClone.size()) == kClone) {
            name.remove_suffix(kClone.size());
            changed = true;
        } else if (!name.empty() && name.back() == ')') {
            const size_t open = name.rfind(" (");
            if (open != std::string_view::npos && open + 3 < name.size() &&
                std::all_of(name.begin() + open + 2, name.end() - 1, [](char c) { return c >= '0' && c <= '9'; })) {
                name = name.substr(0, open);
                changed = true;
            }
        }
    }
    return name;
}

inline bool is_unique_gear(std::string_view name) {
    return std::binary_search(std::begin(kUniqueGear), std::end(kUniqueGear), gear_base_name(name));
}

namespace uniques_detail {

inline size_t skip_space(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    return i;
}

// Just past the JSON value at i (a string with its escapes, an object, an array, a number or a literal); npos if the
// text ends first.
inline size_t skip_value(const std::string& s, size_t i) {
    if (i >= s.size()) return std::string::npos;
    if (s[i] == '"') {
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '"') return i + 1;
        }
        return std::string::npos;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '"') {
                i = skip_value(s, i);
                if (i == std::string::npos) return i;
                --i;
            } else if (s[i] == '{' || s[i] == '[') {
                ++depth;
            } else if ((s[i] == '}' || s[i] == ']') && --depth == 0) {
                return i + 1;
            }
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return i;
}

struct Span {
    size_t begin = std::string::npos, end = std::string::npos;
    bool found() const { return begin != std::string::npos; }
};

// The value of `key` in the object that starts at `at` (its own keys only, not nested ones).
inline Span member(const std::string& s, std::string_view key, size_t at = 0) {
    size_t i = skip_space(s, at);
    if (i >= s.size() || s[i] != '{') return {};
    for (++i;;) {
        i = skip_space(s, i);
        if (i >= s.size() || s[i] != '"') return {};
        const size_t key_end = skip_value(s, i);
        if (key_end == std::string::npos) return {};
        const std::string_view k(s.data() + i + 1, key_end - i - 2);
        i = skip_space(s, key_end);
        if (i >= s.size() || s[i] != ':') return {};
        const size_t value = skip_space(s, i + 1);
        const size_t value_end = skip_value(s, value);
        if (value_end == std::string::npos) return {};
        if (k == key) return {value, value_end};
        i = skip_space(s, value_end);
        if (i >= s.size() || s[i] != ',') return {};
        ++i;
    }
}

// The elements of the array at `a` (each as written).
inline bool elements(const std::string& s, Span a, std::vector<std::string>& out) {
    if (!a.found() || s[a.begin] != '[') return false;
    size_t i = skip_space(s, a.begin + 1);
    if (i < a.end && s[i] == ']') return true;
    while (i < a.end) {
        const size_t e = skip_value(s, i);
        if (e == std::string::npos || e > a.end) return false;
        out.emplace_back(s, i, e - i);
        i = skip_space(s, e);
        if (i < a.end && s[i] == ']') return true;
        if (i >= a.end || s[i] != ',') return false;
        i = skip_space(s, i + 1);
    }
    return false;
}

// The gear an element names: a string (m_GearToInstantiateSerialized) or an object's m_PrefabName (m_SerializedItems).
inline std::string_view element_gear(const std::string& e) {
    if (e.size() >= 2 && e.front() == '"') return std::string_view(e).substr(1, e.size() - 2);
    const Span n = member(e, "m_PrefabName");
    if (!n.found() || e[n.begin] != '"') return {};
    return std::string_view(e).substr(n.begin + 1, n.end - n.begin - 2);
}

inline const char* const kLists[] = {"m_GearToInstantiateSerialized", "m_SerializedItems"};

}  // namespace uniques_detail

// A container's save text (Container.Serialize) without one-of-a-kind items (and, merged, with `ours`' own: the ones
// this game has in that container). The text unchanged if it isn't what's expected.
inline std::string merge_uniques(const std::string& text, const std::string& ours) {
    using namespace uniques_detail;
    std::string out = text;
    for (const char* list : kLists) {
        const Span a = member(out, list);
        std::vector<std::string> items, our_items;
        if (!elements(out, a, items)) continue;
        std::vector<std::string> kept;
        bool changed = false;
        for (std::string& e : items) {
            if (is_unique_gear(element_gear(e))) changed = true;
            else kept.push_back(std::move(e));
        }
        if (elements(ours, member(ours, list), our_items)) {
            for (std::string& e : our_items) {
                if (!is_unique_gear(element_gear(e))) continue;
                kept.push_back(std::move(e));
                changed = true;
            }
        }
        if (!changed) continue;
        std::string joined = "[";
        for (size_t i = 0; i < kept.size(); ++i) joined += (i ? "," : "") + kept[i];
        joined += "]";
        out.replace(a.begin, a.end - a.begin, joined);
    }
    return out;
}

inline std::string strip_uniques(const std::string& text) { return merge_uniques(text, std::string()); }

}  // namespace net
