// Every message of the wire protocol (net/protocol.h), with every field set: written, read back and written again
// must give the same bytes, the reader must use up exactly what was written, and the same message cut short anywhere
// must be refused (not read past its end). Catches a field added on one side only, a changed order or size, and
// over-reads, in a second instead of in a game. Also: strings cut to their fixed size, a datagram that doesn't fit,
// and the item id hash staying what it is. Exit code 0 when everything holds.

#include <cstdio>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace {

constexpr size_t kHeader = 4 + 2 + 1;  // magic, version, type
int failures = 0;

void fail(const char* what, const std::string& why) {
    printf("FAIL: %s: %s\n", what, why.c_str());
    ++failures;
}

// `old_layout`: a shorter body this message must still accept (an older recording); 0 = none.
template <typename T, typename Write, typename Read>
void round_trip(const char* what, net::Msg type, const T& message, Write write, Read read, size_t old_layout = 0) {
    net::Writer w(type, net::kMaxMessage);
    write(w, message);
    if (!w.ok()) return fail(what, "didn't fit");
    const uint8_t* body = w.data() + kHeader;
    const size_t size = w.size() - kHeader;

    T back{};
    net::Reader r(body, size);
    if (!read(r, back)) return fail(what, "not read back");
    if (r.left() != 0) return fail(what, std::to_string(r.left()) + " byte(s) written but not read");
    net::Writer again(type, net::kMaxMessage);
    write(again, back);
    if (again.size() != w.size() || memcmp(again.data(), w.data(), w.size()) != 0)
        return fail(what, "read back, it writes different bytes (a field lost or changed on the way)");

    for (size_t cut = 0; cut < size; ++cut) {
        T part{};
        net::Reader short_reader(body, cut);
        const bool accepted = read(short_reader, part);
        if (accepted && cut != old_layout && !(old_layout && cut > old_layout))
            return fail(what, "accepted cut to " + std::to_string(cut) + " of " + std::to_string(size) + " bytes");
        if (cut == old_layout && old_layout && !accepted) return fail(what, "the older, shorter layout is refused");
    }
    printf("ok: %s (%zu bytes)\n", what, size);
}

std::string text(size_t n, char first) {
    std::string s(n, ' ');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(first + i % 26);
    return s;
}

}  // namespace

int main() {
    using namespace net;

    StateMsg state;
    state.seq = 7;
    state.t_ms = 1234.5;
    state.valid = true;
    state.scene = "LakeRegion";
    state.x = 1.5f, state.y = 2.5f, state.z = -3.5f, state.yaw = 90, state.pitch = -10;
    state.normalized_time = 0.25f;
    state.day = 3;
    state.flags = kStateStruggling | kStateDead | kStateInBed;
    state.player = 2;
    state.sleep_vote = 4;
    state.held = "GEAR_Torch";
    state.held_kind = 2;
    state.held_flags = 1 | 2;
    state.flags |= kStateEating | kStateBed;
    for (int i = 0; i < 7; ++i) state.bed[i] = 0.5f * static_cast<float>(i) - 1;
    round_trip("State", Msg::State, state, write_state, read_state);

    WorldMsg world;
    world.seq = 9;
    world.valid = true;
    world.normalized_time = 0.75f;
    world.day = 12;
    world.day_length_s = 7200;
    world.time_lapse = true;
    world.weather_set = "Cloudy_Lake";
    world.weather_set_state = text(300, 'a');
    world.weather_progress = 0.4f;
    world.weather_previous = 2;
    world.weather_stage = 5;
    world.wind_strength = 3;
    world.wind_angle = 213;
    world.region = "LakeRegion";
    world.world_id = "sandbox5|12345|678";
    round_trip("World", Msg::World, world, write_world, read_world,
               4 + 1 + 4 + 4 + 4 + 1 + kWeatherSetLen + 4 * 5 + 2 + world.weather_set_state.size());  // pre-v13

    AnimalsMsg animals;
    animals.frame = 44;
    animals.t_ms = 99.25;
    animals.part = 1;
    animals.parts = 2;
    animals.scene = "LakeRegion";
    for (uint32_t i = 0; i < 3; ++i) {
        AnimalRec a;
        a.id = 100 + i;
        a.kind = 1, a.mode = 2, a.flags = kAnimalDead;
        a.x = 1, a.y = 2, a.z = 3, a.yaw = 4, a.pitch = 5, a.roll = 6, a.hp = 7;
        a.layers = {{11, 0.5f}, {12, 0.25f}};
        a.params = text(20 + i, 'k');
        animals.animals.push_back(a);
    }
    round_trip("Animals", Msg::Animals, animals, write_animals, read_animals);

    std::vector<AnimalSound> sounds(2);
    sounds[0] = {5, 1, SoundOp::Post, 1001, 2002, 3, 400, 2};
    sounds[1] = {6, 2, SoundOp::Action, 1003, 2004, 1, 250, 4};
    round_trip("AnimalSounds", Msg::AnimalSounds, sounds, write_sounds, read_sounds);

    AttackMsg attack;
    attack.animal = 3, attack.kind = 2, attack.type = AttackType::Wounds;
    attack.damage = 12.5f, attack.source = 4, attack.options = 9;
    round_trip("Attack", Msg::Attack, attack, write_attack, read_attack);

    AttackResultMsg result;
    result.animal = 3, result.outcome = FightOutcome::Fled, result.hp = 40, result.damage = 22;
    result.flags = kFightBleeding, result.bleed_minutes = 35;
    round_trip("AttackResult", Msg::AttackResult, result, write_attack_result, read_attack_result);

    HitMsg hit;
    hit.animal = 8, hit.weapon = Weapon::HardenedArrow, hit.body_part = 3, hit.flags = kHitCritical | kHitStuck;
    hit.collider = "capsule_chest";
    hit.damage = 55, hit.bleed_minutes = 12;
    hit.world[0] = 1, hit.world[1] = 2, hit.world[2] = 3, hit.local[0] = 0.1f, hit.local[1] = 0.2f, hit.local[2] = 0.3f;
    round_trip("Hit", Msg::Hit, hit, write_hit, read_hit);

    ThrownMsg thrown;
    thrown.animal = 4, thrown.item = ThrownItem::Flare, thrown.flare_type = 1;
    thrown.pos[0] = 5, thrown.pos[1] = 6, thrown.pos[2] = 7, thrown.contact[0] = 8, thrown.contact[1] = 9;
    thrown.contact[2] = 10;
    round_trip("Thrown", Msg::Thrown, thrown, write_thrown, read_thrown);

    std::vector<CarcassRec> carcasses(2);
    for (size_t i = 0; i < carcasses.size(); ++i) {
        CarcassRec& c = carcasses[i];
        c.animal = static_cast<uint32_t>(20 + i);
        c.prefab = "WILDLIFE_Wolf";
        c.pos[0] = 1, c.pos[1] = 2, c.pos[2] = 3, c.rot[0] = 0.1f, c.rot[1] = 0.2f, c.rot[2] = 0.3f, c.rot[3] = 0.9f;
        c.damage_side = -1, c.meat_units = 3000000000ll, c.hide = 1, c.gut = 2;
        c.condition = 0.8f, c.percent_frozen = 10, c.flags = kCarcassFrozen | kCarcassHarvested;
        c.seconds_dead = 6, c.harvest_ack = 4;
    }
    round_trip("Carcasses", Msg::Carcasses, carcasses, write_carcasses, read_carcasses);

    HarvestMsg harvest;
    harvest.animal = 20, harvest.seq = 3, harvest.meat_units = 1000000000ll, harvest.hide = 1, harvest.gut = 0;
    harvest.flags = kHarvestSetHarvested;
    round_trip("Harvest", Msg::Harvest, harvest, write_harvest, read_harvest);

    ItemPutMsg put;
    put.scene = "LakeCabinC", put.seq = 15, put.flags = kItemAnswer;
    put.pdid = "8a4ececc-5825-5697-50d5-3ff8feb3a6fd", put.prefab = "GEAR_Prybar";
    put.pos[0] = 92.3f, put.pos[1] = 102.2f, put.pos[2] = 1424.3f;
    put.rot[0] = 0, put.rot[1] = 0.7f, put.rot[2] = 0, put.rot[3] = 0.7f;
    put.gear = text(5000, 'A');
    round_trip("ItemPut", Msg::ItemPut, put, write_item_put, read_item_put);

    ItemGoneMsg gone;
    gone.scene = "LakeRegion", gone.seq = 16, gone.reason = ItemGoneReason::NotInHostWorld, gone.pdid = "4eff63aa";
    round_trip("ItemGone", Msg::ItemGone, gone, write_item_gone, read_item_gone);

    ItemHashesMsg hashes;
    hashes.scene = "LakeRegion";
    for (uint64_t i = 0; i < 115; ++i) hashes.hashes.push_back(pdid_hash("item" + std::to_string(i)));
    round_trip("ItemList / ItemWant", Msg::ItemList, hashes, write_item_hashes, read_item_hashes);

    ContainerPutMsg container;
    container.scene = "LakeRegion", container.seq = 2, container.flags = kContainerAnswer | kContainerUnknown;
    container.id = "CONTAINER_Supply_CacheA|1|2|3", container.text = text(9000, 'c');
    round_trip("ContainerPut", Msg::ContainerPut, container, write_container_put, read_container_put);

    ContainerWantMsg want;
    want.scene = "LakeRegion", want.id = "CONTAINER_BackPack";
    round_trip("ContainerWant", Msg::ContainerWant, want, write_container_want, read_container_want);

    ObjectPutMsg object;
    object.scene = "LakeRegion", object.seq = 4;
    object.object = {ObjectKind::Harvestable, 1, 33.5f, "BirchSapling-guid"};
    round_trip("ObjectPut", Msg::ObjectPut, object, write_object_put, read_object_put);
    object.object = {ObjectKind::Placement, 2, 0, "decoration-guid", text(3000, 'd')};
    round_trip("ObjectPut (placement)", Msg::ObjectPut, object, write_object_put, read_object_put);
    object.object = {ObjectKind::Body, 1, 0, "corpse-guid", "Ann\n" + text(300, 'r') + "\n" + text(60000, 'b')};
    round_trip("ObjectPut (body)", Msg::ObjectPut, object, write_object_put, read_object_put);

    ObjectListMsg objects;
    objects.scene = "LakeRegion";
    objects.objects = {{ObjectKind::BreakDown, 1, 0, "limb-a"}, {ObjectKind::Harvestable, 0, 12, "beard"},
                       {ObjectKind::Lock, 2, 0, "locker-guid"}, {ObjectKind::Safe, 1, 0, "@safe-at-x-y-z"},
                       {ObjectKind::Placement, 0, 0, "crate-guid", text(400, 'c')},
                       {ObjectKind::Body, 1, 0, "corpse-guid", "Bob\n" + text(300, 'r') + "\n"}};
    objects.spawns = text(700, 's');
    objects.placements = text(900, 'p');
    round_trip("ObjectList", Msg::ObjectList, objects, write_object_list, read_object_list);

    FirePutMsg fire;
    fire.scene = "LakeRegion", fire.seq = 5, fire.flags = kFireGone, fire.id = "campfire-guid";
    fire.text = text(1500, 'f');
    round_trip("FirePut", Msg::FirePut, fire, write_fire_put, read_fire_put);

    FireListMsg fires;
    fires.scene = "LakeRegion", fires.text = text(13000, 'F');
    round_trip("FireList", Msg::FireList, fires, write_fire_list, read_fire_list);

    std::vector<RosterEntry> roster = {{0, 0, true, "TestHost", "astrid"}, {1, 7, false, "TestFriend", "terry"},
                                       {2, 9, true, "TestBot", "jace"}};
    round_trip("Roster", Msg::Roster, roster, write_roster, read_roster);

    struct GameMode {
        std::string text;
        int32_t seed = 0;
    } mode{"{\"m_CurrentModeType\":\"Stalker\"}", -123456};
    round_trip(
        "GameMode", Msg::GameMode, mode, [](Writer& w, const GameMode& m) { write_game_mode(w, m.text, m.seed); },
        [](Reader& r, GameMode& m) { return read_game_mode(r, m.text, m.seed); });

    SleepMsg sleep;
    sleep.op = SleepOp::End;
    sleep.id = 3;
    sleep.choice = 4;
    sleep.hours = 9.25f;
    sleep.end = SleepEnd::Woken;
    sleep.who = 2;
    sleep.why = SleepWhy::Cold;
    round_trip("Sleep", Msg::Sleep, sleep, write_sleep, read_sleep);

    GiveMsg give;
    give.op = GiveOp::Done;
    give.id = (2u << 24) | 5;
    give.giver = 2;
    give.receiver = 0;
    give.item = 1;
    give.units = 3;
    give.gear = "GEAR_CoffeeTin";
    give.prefab = "GEAR_CoffeeTin";
    give.record = text(3000, 'g');
    round_trip("Give", Msg::Give, give, write_give, read_give);
    {  // An op nobody sends is refused.
        GiveMsg odd = give;
        odd.op = static_cast<GiveOp>(9);
        Writer w(Msg::Give, kMaxMessage);
        write_give(w, odd);
        GiveMsg back;
        Reader r(w.data() + kHeader, w.size() - kHeader);
        if (read_give(r, back))
            fail("Give", "an unknown op was accepted");
        else
            printf("ok: a Give with an unknown op is refused\n");
    }

    // A string longer than its fixed size arrives cut (and NUL-ended), without spilling into the next field.
    {
        StateMsg longer = state;
        longer.scene = text(kSceneLen + 10, 'a');
        Writer w(Msg::State);
        write_state(w, longer);
        StateMsg back;
        Reader r(w.data() + kHeader, w.size() - kHeader);
        if (!read_state(r, back) || back.scene != longer.scene.substr(0, kSceneLen - 1) || back.x != longer.x)
            fail("long scene name", "not cut to its field: '" + back.scene + "'");
        else
            printf("ok: a scene name longer than its field arrives cut to %zu characters\n", kSceneLen - 1);
    }
    // More than a datagram holds: the writer says so (the sender then goes the reliable, chunked way).
    {
        Writer w(Msg::ItemPut);
        ItemPutMsg huge = put;
        huge.gear = text(kMaxPacket * 2, 'x');
        write_item_put(w, huge);
        if (w.ok())
            fail("datagram size", "an ItemPut bigger than a datagram was taken");
        else
            printf("ok: a message bigger than one datagram is refused by its writer\n");
    }
    // Item ids hash as they always have (ItemList / ItemWant; tldbot's recordings): FNV-1a 64's steps, from a basis
    // one digit short of the standard one (1469598103934665603). Both games use this function, so that's harmless;
    // changing it would be a protocol change.
    if (pdid_hash("") != 1469598103934665603ull || pdid_hash("a") != 0x44bd8ad473cd9906ull ||
        pdid_hash("8a4ececc-5825-5697-50d5-3ff8feb3a6fd") != 0xfd3f5de9f06ae1b8ull)
        fail("pdid_hash", "item ids hash differently than before (a protocol change)");
    else
        printf("ok: item ids hash as before\n");

    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
