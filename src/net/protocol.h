#pragma once
// Wire protocol shared by the mod (winhttp.dll) and the test bot (tldbot.exe).
// One message per UDP datagram, little-endian, fixed-size strings (zero padded).
//
//   header:  u32 magic "TLDC" | u16 protocol version | u8 message type
// Up to 4 players: the host and 3 friends, each linked to the host only (the host relays what the friends need of each
// other: their states, who's there, and the world changes, as its own).
//
//   Hello    client->host  game_version[24] name[32] u32 nonce (new each time the client starts connecting: a known
//                          address saying hello with another nonce is a new connection, the old one is gone)
//   Welcome  host->client  name[32] u8 player (the id the host gave the client: 1..3; the host is 0)
//   Wait     host->client  name[32]: the answer to a Hello while the host decides whether to let them in (a host that
//                          asks: Config.ask_to_join); they keep saying hello until a Welcome or a Reject
//   Reject   host->client  u8 reason, detail[64]; TurnedAway also ends a connection that is up (the host kicked them)
//   Ping     either        f64 sender time (ms)
//   Pong     either        f64 echoed sender time
//   State    either        u32 seq, f64 capture time (sender clock, ms), u8 valid, scene[48],
//                          f32 x y z yaw pitch, f32 normalized_time, i32 day, u8 flags (kStateStruggling...), u8 player
//                          (whose state: 0 the host's, else a friend's the host passes on; a client sends 0),
//                          u8 sleep_vote (in bed or waiting: their SleepChoice, net/sleep_vote.h),
//                          held[40] (the gear in their hands, e.g. "GEAR_Torch"; "" empty hands), u8 held_kind
//                          (net::HeldKind, net/held.h), u8 held_flags (kHeldLit, kHeldAiming, kHeldRaised, kHeldHigh)
//   Bye      either        (empty)
//   Profile  client->host  name[32] body[32] u8 ready (sent on connect, every 2 s and on a change; body is an id
//                          like "wolf"; ready: READY in the lobby, or playing)
//   Roster   host->client  u8 n, then per player: u8 id, u32 link, u8 ready, name[32], body[32]: everyone in the
//                          game, the host first (every 2 s and when someone comes, goes or changes). link: which
//                          connection of theirs (another one in the same id is someone new); the host's ready: it
//                          pressed START in the lobby, or is playing
//   World    host->client  u32 seq, u8 valid, f32 normalized_time, i32 day, f32 day_length_s, u8 time_lapse,
//                          weather_set[64], f32 weather_progress, i32 weather_previous, i32 weather_stage,
//                          i32 wind_strength, f32 wind_angle, u16 n + n bytes weather_set_state, region[48]
//                          (TLD's last outdoor scene: where an interior the host is in lets out), world_id[64] (which
//                          game: save slot, TLD's game id, random seed) (about 1 Hz)
//   Animals  host->client  u32 frame, f64 capture time, u8 part, u8 parts, scene[48], u8 count, then per animal:
//                          u32 id, u8 kind, u8 mode, u8 flags, f32 x y z yaw pitch roll hp, u8 layers +
//                          (i32 state, f32 time) each, u16 n + n bytes animator parameter values (10 Hz; a frame
//                          may span parts)
//   AnimalSounds host->client  u8 count, then per sound: u32 animal id, u8 emitter, u8 op, u32 event, u32 playing id,
//                          i32 action, i32 fade ms, i32 curve (as soon as the host's game plays them)
//   Attack   host->client  u32 animal, u8 kind, u8 type, f32 damage, i32 damage source, i32 options: one of the
//                          host's animals attacks the client (a fight, damage, or wound events)
//   AttackResult client->host  u32 animal, u8 outcome, f32 hp, f32 damage, u8 flags, f32 bleed minutes: how the
//                          client's fight with it ended and what it did to the animal
//   Reliable either        u32 seq, u16 n + n bytes: another message (with its own header), resent until acked
//   Ack      either        u32 seq: a Reliable arrived
//   Hit      client->host  one of the client's projectiles hit the host's animal (rifle, revolver, arrow, flare
//                          round): u32 animal, u8 weapon, u8 body part, u8 flags, collider[24], f32 damage,
//                          f32 bleed minutes, f32 hit point xyz (world), f32 hit point xyz (collider space)
//   Thrown   client->host  a stone, flare, torch or noise maker the client threw landed: u32 animal it hit (0 none),
//                          u8 item, u8 flare type, f32 xyz where it landed, f32 xyz contact point
//   Carcasses host->client the host's dead animals as carcasses: u8 count, then per carcass u32 animal,
//                          prefab[48], f32 xyz, f32 quaternion xyzw, i8 damage side, i64 meat units, i32 hide,
//                          i32 gut, f32 condition, f32 percent frozen, u8 flags, f32 seconds dead, u32 harvest ack
//                          (about 2 Hz)
//   Harvest  client->host  u32 animal, u32 seq, i64 meat units taken, i32 hide taken, i32 gut taken, u8 flags
//   Quartered client->host u32 animal: the client quartered the carcass (it's gone)
//   Chunk    either        u32 transfer, u16 index, u16 count, u16 n + n bytes: one part of a message too big for one
//                          datagram (sent through Reliable; the receiver handles the message once all parts are in)
//   ItemPut  either        scene[48], u32 seq, u8 flags, pdid (u16 n + n), prefab (u16 n + n), f32 xyz, f32 xyzw,
//                          u32 n + n bytes serialized gear: a loose item in the sender's world appeared or came to
//                          rest somewhere new (TLD's own save record for it: GearItem.CreateSaveData)
//   ItemGone either        scene[48], u32 seq, u8 reason, pdid (u16 n + n): a loose item left the sender's world
//                          (picked up...). seq: the sender's change counter, so a late older change loses
//   ItemList host->client  scene[48], u32 n, then n u64 pdid hashes: every loose item in the host's scene
//   ItemWant client->host  scene[48], u32 n, then n u64 pdid hashes: send these as ItemPut
//   ContainerPut either    scene[48], u32 seq, u8 flags, container id (u16 n + n), u32 n + n bytes: a container's
//                          contents changed in the sender's world (TLD's own save text for it: Container.Serialize),
//                          or the host's answer to ContainerWant (flags 1); flags 2: the host has nothing for it
//   ContainerWant client->host scene[48], container id (u16 n + n): the client is opening this container
//   ObjectPut either       scene[48], u32 seq, then one object: u8 kind, u8 done, f32 refresh, id (u16 n + n):
//                          something in the sender's world was broken down (kind 1, BreakDown) or harvested (kind 2,
//                          Harvestable; refresh: the game hour it grows back, TLD's m_RefreshTime), or (done 0) is back;
//                          a placement (kind 5) or a dead player's body (kind 6) also carries u32 n + n bytes of text
//   ObjectList host->client scene[48], u32 n, then n objects: everything broken down or harvested in the host's scene,
//                          then u32 n + n bytes: which of TLD's random spawns are there (RandomSpawnObjectManager.Serialize())
//   FirePut  either        scene[48], u32 seq, u8 flags, fire id (u16 n + n), u32 n + n bytes: a fire changed in the
//                          sender's world (lit, fed, embers, out...): TLD's own save record for it (one entry of
//                          FireManager.Serialize's m_SerializedFires); flags 1: it's gone (a campfire taken apart)
//   FireList host->client  scene[48], u32 n + n bytes: all the host's fires there (FireManager.Serialize())
//   CaughtUp client->host  u32 n: once linked (again), after the n world changes the client made while the link was
//                          down (kept, sent first) are all acked; the host sends its lists of the world after this
//   GameMode host->client  u32 n + n bytes: the host's experience mode (ExperienceModeManager.Serialize()), i32 seed:
//                          its game's random seed (SceneTransitionData.m_GameRandomSeed), on connect and when either
//                          changes; the client plays at both while linked (its own save keeps its own)
//   Sleep    either        u8 op, u32 sleep id, u8 choice, f32 hours, u8 end, u8 who, u8 why: sleeping together
//                          (milestone 10 item 12). Start (host): everyone is in (in bed or waiting): sleep or pass
//                          time `choice`, `hours` of it.
//                          End (host): it is over (end Done, or Woken: player `who` woke, `why`). Woke (a friend):
//                          our part of sleep `id` ended early (`why`), or we weren't in bed for its Start

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace net {

constexpr uint32_t kMagic = 0x43444C54;  // bytes "TLDC"
constexpr uint16_t kProtocolVersion = 27;  // v4: World. v5: Animals. v6: sounds. v7: attacks, State flags. v8: hits,
                                           // carcasses, harvests, fuller fight results, reliable messages. v9: chunked
                                           // messages, world items. v10: containers. v11: broken down / harvested.
                                           // v12: fires. v13: the host's region in World, random spawns in ObjectList.
                                           // v14: CaughtUp (the client's changes while the link was down). v15: GameMode.
                                           // v16: up to 3 friends (Welcome's player id, State's player, Roster).
                                           // v17: the lobby (ready in Profile and Roster), link in Roster,
                                           // TLD's placements in ObjectList, the game seed in GameMode,
                                           // Wait and TurnedAway (the host lets each friend in).
                                           // v18: the host refuses a take of an item someone else took first
                                           // (ItemGone NotYours, ItemPut kItemNotYours)
                                           // v19: a friend's carcass harvests and quarters made while the link
                                           // is down go once it's back; the host counts each harvest once (seq)
                                           // v20: locks and safes (ObjectKind Lock, Safe)
                                           // v21: takes of one item at the same moment go to a coin toss
                                           // (ItemGone LostToss)
                                           // v22: decorations a player moves, picks up or puts down (ObjectKind
                                           // Placement)
                                           // v23: death: a dead player's body (ObjectKind Body), State's
                                           // kStateDead
                                           // v24: one-of-a-kind items (net/uniques.h) are never shared: not in
                                           // ItemPut / ItemGone / the lists, nor in a ContainerPut's text
                                           // v25: sleeping together: State's kStateInBed, kStateWaiting and
                                           // sleep_vote, Sleep
                                           // v26: what a player holds: State's held, held_kind, held_flags
                                           // v27: handing an item to a friend (Give), State's kStateEating
                                           // (the food in held), kStateBed with bed, kStateCrouched, and the
                                           // sleep list's REST
constexpr size_t kMaxPlayers = 4;  // the host and up to 3 friends
constexpr size_t kMaxPacket = 1200;  // fits one UDP datagram on any path
constexpr size_t kMaxMessage = 256 * 1024;  // a reliable message may be this big (sent as Chunks)

constexpr size_t kVersionLen = 24;
constexpr size_t kNameLen = 32;
constexpr size_t kSceneLen = 48;
constexpr size_t kWorldIdLen = 64;
constexpr size_t kDetailLen = 64;
constexpr size_t kBodyLen = 32;
constexpr size_t kHeldLen = 40;
constexpr size_t kWeatherSetLen = 64;
constexpr size_t kWeatherStateMax = 900;
constexpr size_t kMaxAnimalLayers = 4;
constexpr size_t kMaxAnimalParams = 256;  // bytes of animator parameter values per animal

enum class Msg : uint8_t {
    Hello = 1, Welcome = 2, Reject = 3, Ping = 4, Pong = 5, State = 6, Bye = 7, Profile = 8, World = 9, Animals = 10, AnimalSounds = 11, Attack = 12, AttackResult = 13,
    Reliable = 14, Ack = 15, Hit = 16, Thrown = 17, Carcasses = 18, Harvest = 19, Quartered = 20, Chunk = 21,
    ItemPut = 22, ItemGone = 23, ItemList = 24, ItemWant = 25, ContainerPut = 26, ContainerWant = 27, ObjectPut = 28,
    ObjectList = 29, FirePut = 30, FireList = 31, CaughtUp = 32, GameMode = 33, Roster = 34, Wait = 35, Sleep = 36,
    Give = 37
};
enum class RejectReason : uint8_t { Protocol = 1, GameVersion = 2, Full = 3, TurnedAway = 4 };

struct StateMsg {
    uint32_t seq = 0;
    double t_ms = 0;     // when the sender captured this state, on the sender's own monotonic clock
    bool valid = false;  // false while the sender is in a menu or loading
    std::string scene;
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    float roll = 0;  // not sent in State (players don't roll); animals use it locally for their tilt
    float normalized_time = 0;
    int32_t day = 0;
    uint8_t flags = 0;  // kStateStruggling, kStateDead, kStateInBed, kStateWaiting, kStateEating, kStateBed,
                        // kStateCrouched
    uint8_t player = 0;  // whose: 0 the host, 1..3 a friend
    uint8_t sleep_vote = 0;  // in bed: the hours they vote for (net::SleepChoice); 0 otherwise
    std::string held;         // the gear prefab in their hands ("" empty hands)
    uint8_t held_kind = 0;    // net::HeldKind (net/held.h)
    uint8_t held_flags = 0;   // kHeldLit, kHeldAiming, kHeldRaised, kHeldHigh
    float bed[7] = {0, 0, 0, 0, 0, 0, 1};  // kStateBed: where their body lies in bed (TLD's Bed.m_BodyPlacementTransform:
                                           // world x y z, rotation x y z w)
};
constexpr uint8_t kStateStruggling = 1;  // in a fight with an animal (TLD's PlayerStruggle)
constexpr uint8_t kStateDead = 2;        // dead, choosing their next survivor (their body lies where they died)
constexpr uint8_t kStateInBed = 4;       // in through co-op's sleep or pass time screen (in real time, or sleeping or
                                         // passing time with everyone)
constexpr uint8_t kStateWaiting = 8;     // with kStateInBed: in to pass time, not in bed
constexpr uint8_t kStateEating = 16;     // eating or drinking (TLD's progress bar): held is the food
constexpr uint8_t kStateBed = 32;        // with kStateInBed: lying in a bed, `bed` says where (not set while waiting)
constexpr uint8_t kStateCrouched = 64;   // crouching (PlayerManager.PlayerIsCrouched)

// The host's world, which the client follows: clock, weather and wind (milestone 5). Enum values are TLD's own
// (WeatherStage, WindStrength).
struct WorldMsg {
    uint32_t seq = 0;
    bool valid = false;            // false while the host is in a menu or loading
    float normalized_time = 0;     // TimeOfDay.GetNormalizedTime(): 0 = midnight, 0.5 = noon
    int32_t day = 0;               // TimeOfDay.GetDayNumber()
    float day_length_s = 0;        // real seconds per game day, to extrapolate the clock between messages
    bool time_lapse = false;       // the host is sleeping or passing time
    std::string weather_set;       // the active WeatherSetData's name; "" when there is none
    std::string weather_set_state; // WeatherSetData.Serialize(): the set's rolled stage lengths and progress
    float weather_progress = 0;    // WeatherSetData.GetProgressFrac()
    int32_t weather_previous = 11; // WeatherTransition.m_PreviousWeatherSetType (11 = Undefined)
    int32_t weather_stage = 11;    // Weather.GetWeatherStage()
    int32_t wind_strength = 0;     // Wind.GetStrength()
    float wind_angle = 0;          // Wind.GetWindAngle_Base(), degrees
    std::string region;            // SceneTransitionData.m_LastOutdoorScene: the host's region (in an interior, where it
                                   // lets out); "" from an older recording
    std::string world_id;          // game::save_id() of the host's game: which world this is
    double received_ms = 0;        // receiver's now_ms() on arrival; not sent
};

// The host's animals, which the client mirrors (milestone 7). kind = AiSubType, mode = AiMode.
struct AnimalLayer {
    int32_t state = 0;  // AnimatorStateInfo.fullPathHash
    float time = 0;     // AnimatorStateInfo.normalizedTime
};
struct AnimalRec {
    uint32_t id = 0;  // the host's own numbering, stable while the animal exists
    uint8_t kind = 0, mode = 0, flags = 0;  // flags: kAnimalDead
    float x = 0, y = 0, z = 0, yaw = 0, hp = 0;
    float pitch = 0, roll = 0;  // the body's tilt along the ground, degrees (-180..180)
    std::vector<AnimalLayer> layers;
    // Every non-trigger animator parameter, in the controller's order: float / int as 4 bytes, bool as 1. Both
    // games load the same controller, so the order is the same on both sides.
    std::string params;
};
constexpr uint8_t kAnimalDead = 1;
struct AnimalsMsg {
    uint32_t frame = 0;
    double t_ms = 0;  // when the host captured the frame, on its own clock
    uint8_t part = 0, parts = 1;
    std::string scene;
    std::vector<AnimalRec> animals;
    double received_ms = 0;  // receiver's now_ms() on arrival; not sent
};

// A sound one of the host's animals made (Wwise), replayed on the client's copy of it.
enum class SoundOp : uint8_t { Post = 1, StopPlaying = 2, Action = 3 };
struct AnimalSound {
    uint32_t animal = 0;
    uint8_t emitter = 0;  // which of the animal's Wwise sound objects (AkGameObj, in hierarchy order)
    SoundOp op = SoundOp::Post;
    uint32_t event = 0;     // Wwise event ID (Post, Action)
    uint32_t playing = 0;   // the host's playing ID (Post: the one it got; StopPlaying / Action: the one to stop)
    int32_t action = 0;     // AkActionOnEventType (Action)
    int32_t fade_ms = 0;    // StopPlaying / Action
    int32_t curve = 0;      // AkCurveInterpolation (StopPlaying / Action)
};
constexpr size_t kAnimalSoundSize = 4 + 1 + 1 + 4 + 4 + 4 + 4 + 4;

// One of the host's animals attacks the client (milestone 7c). The host never applies these to its own player.
enum class AttackType : uint8_t {
    Struggle = 1,  // the animal jumps them: TLD's fight (PlayerStruggle) runs on the client's game
    Damage = 2,    // direct damage (AiTarget.ApplyDamage): damage + source
    Wounds = 3,    // the animal's wound events (DamageEventTable.MaybeApplyAttack): bleeding, sprains, clothing; options
};
struct AttackMsg {
    uint32_t animal = 0;
    uint8_t kind = 0;
    AttackType type = AttackType::Struggle;
    float damage = 0;
    int32_t source = 0;   // DamageSource
    int32_t options = 0;  // DamageEventTable.ApplyAttackOpts
    uint8_t player = 0;   // host: the friend it's for (not sent; 0 = the first friend)
};
// How TLD ended the fight: the animal fled (MakePartnerFlee: tap bar full, flee roll, low health), died (a weapon
// took its health), or walked off (Wander: the fight timed out and knocked the player out, or the player died).
enum class FightOutcome : uint8_t { Fled = 1, Died = 2, NoFight = 3, WalkedOff = 4 };
constexpr uint8_t kFightBleeding = 1;  // AttackResultMsg.flags: the fight left it bleeding out
struct AttackResultMsg {
    uint32_t animal = 0;
    FightOutcome outcome = FightOutcome::NoFight;
    float hp = 0;             // the stand-in's health after the fight (for the log; the host applies damage)
    float damage = 0;         // health the fight took from it (TLD only takes any when the player has a weapon)
    uint8_t flags = 0;        // kFightBleeding
    float bleed_minutes = 0;  // game minutes until it bleeds out, when kFightBleeding
    uint8_t player = 0;       // host: the friend whose fight it was (not sent; set on arrival)
};

// A packet received through the reliable channel or as one of the newer message types, handed to the game thread
// whole (header included) to parse with the read_* functions below.
struct Packet {
    Msg type = Msg::Hello;
    uint8_t from = 0;  // host: the friend it came from (1..3); client: 0 (everything comes from the host)
    uint32_t link = 0; // which connection of theirs (a new one each time they link: their counters may start over)
    std::vector<uint8_t> data;
};

// One of the client's projectiles hit one of the host's animals (its copy, on the client). The client works the
// damage out with its own skills, as TLD does for the shooter; the host applies it to the real animal.
enum class Weapon : uint8_t { Arrow = 1, HardenedArrow = 2, FlareGun = 3, Revolver = 4, Rifle = 5 };  // WeaponSource
constexpr size_t kColliderLen = 24;
constexpr uint8_t kHitCritical = 1;  // HitMsg.flags: the shooter's kill chance came up (the host decides if it counts)
constexpr uint8_t kHitStuck = 2;     // the projectile stuck in it
struct HitMsg {
    uint32_t animal = 0;
    Weapon weapon = Weapon::Rifle;
    uint8_t body_part = 0;  // LocalizedDamage.m_BodyPart
    uint8_t flags = 0;
    std::string collider;   // the hit collider's name, e.g. "capsule_chest"
    float damage = 0;
    float bleed_minutes = 0;
    float world[3] = {};    // hit point
    float local[3] = {};    // hit point in the collider's space (the host's animal may stand a little elsewhere)
};

// A thrown item landed: stones can stun small game, and all of them can scare animals off.
enum class ThrownItem : uint8_t { Stone = 1, Flare = 2, Torch = 3, NoiseMaker = 4 };
struct ThrownMsg {
    uint32_t animal = 0;  // the animal it hit, 0 for none
    ThrownItem item = ThrownItem::Stone;
    uint8_t flare_type = 0;  // FlareItem.m_Type: 0 red, 1 blue
    float pos[3] = {};       // where the item is
    float contact[3] = {};   // where it touched the animal
};

// The host's dead animals, as carcasses the client can harvest (its own copy, kept to the host's numbers).
constexpr size_t kPrefabLen = 48;
constexpr uint8_t kCarcassFrozen = 1, kCarcassRavaged = 2, kCarcassHarvested = 4, kCarcassBonusTaken = 8;
struct CarcassRec {
    uint32_t animal = 0;
    std::string prefab;  // the animal's GameObject name, e.g. "WILDLIFE_Wolf"
    float pos[3] = {};
    float rot[4] = {0, 0, 0, 1};
    int8_t damage_side = 0;  // BaseAi.m_LastDamageSide: which way it fell
    int64_t meat_units = 0;  // ItemWeight units, 1 kg = 1e9
    int32_t hide = 0, gut = 0;
    float condition = 0, percent_frozen = 0;
    uint8_t flags = 0;
    float seconds_dead = 0;    // BaseAi.m_TimeInDeadMode: the death animation takes 5 s
    uint32_t harvest_ack = 0;  // the newest client Harvest seq applied to it
};
constexpr size_t kCarcassSize = 4 + kPrefabLen + 12 + 16 + 1 + 8 + 4 + 4 + 4 + 4 + 1 + 4 + 4;

// The client took meat, hide or guts from a carcass (TLD gave it the items); the host takes the same off its own.
constexpr uint8_t kHarvestSetHarvested = 1, kHarvestBonusTaken = 2;
struct HarvestMsg {
    uint32_t animal = 0;
    uint32_t seq = 0;  // the client's harvest number: never 0, starts somewhere new each game run (net/harvests.h)
    int64_t meat_units = 0;
    int32_t hide = 0, gut = 0;
    uint8_t flags = 0;
};

// Builds one message. `max` is kMaxPacket for anything sent as one datagram; a reliable message may be built up to
// kMaxMessage and is then sent in Chunks.
class Writer {
public:
    explicit Writer(Msg type, size_t max = kMaxPacket) : max_(max) {
        buf_.reserve(max < kMaxPacket ? max : kMaxPacket);
        put(kMagic);
        put(kProtocolVersion);
        put(static_cast<uint8_t>(type));
    }

    template <typename T>
    void put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        append(&value, sizeof(T));
    }

    void put_str(const std::string& s, size_t fixed) {
        if (!room(fixed)) return;
        const size_t at = buf_.size();
        buf_.resize(at + fixed, 0);
        memcpy(buf_.data() + at, s.data(), s.size() < fixed ? s.size() : fixed - 1);
    }

    // u16 length + bytes; an over-long blob is sent empty (callers check fits_blob first).
    void put_blob(const std::string& s, size_t max) {
        const uint16_t n = static_cast<uint16_t>(s.size() <= max && s.size() <= 0xFFFF ? s.size() : 0);
        put(n);
        append(s.data(), n);
    }

    // u32 length + bytes, for text that can outgrow a u16 (serialized gear and containers).
    void put_long_blob(const std::string& s) {
        put(static_cast<uint32_t>(s.size()));
        append(s.data(), s.size());
    }

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }
    bool ok() const { return !overflow_; }

private:
    bool room(size_t n) {
        if (overflow_ || buf_.size() + n > max_) overflow_ = true;
        return !overflow_;
    }
    void append(const void* p, size_t n) {
        if (!room(n)) return;
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    std::vector<uint8_t> buf_;
    size_t max_;
    bool overflow_ = false;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    template <typename T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (pos_ + sizeof(T) > size_) {
            bad_ = true;
            return value;
        }
        memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    std::string get_str(size_t fixed) {
        if (pos_ + fixed > size_) {
            bad_ = true;
            return {};
        }
        auto p = reinterpret_cast<const char*>(data_ + pos_);
        pos_ += fixed;
        return std::string(p, strnlen(p, fixed));
    }

    std::string get_blob(size_t max) {
        const uint16_t n = get<uint16_t>();
        if (bad_ || n > max || pos_ + n > size_) {
            bad_ = true;
            return {};
        }
        auto p = reinterpret_cast<const char*>(data_ + pos_);
        pos_ += n;
        return std::string(p, n);
    }

    std::string get_long_blob(size_t max) {
        const uint32_t n = get<uint32_t>();
        if (bad_ || n > max || pos_ + n > size_) {
            bad_ = true;
            return {};
        }
        auto p = reinterpret_cast<const char*>(data_ + pos_);
        pos_ += n;
        return std::string(p, n);
    }

    bool ok() const { return !bad_; }
    size_t left() const { return bad_ ? 0 : size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool bad_ = false;
};

inline void write_state(Writer& w, const StateMsg& s) {
    w.put(s.seq);
    w.put(s.t_ms);
    w.put(static_cast<uint8_t>(s.valid ? 1 : 0));
    w.put_str(s.scene, kSceneLen);
    w.put(s.x);
    w.put(s.y);
    w.put(s.z);
    w.put(s.yaw);
    w.put(s.pitch);
    w.put(s.normalized_time);
    w.put(s.day);
    w.put(s.flags);
    w.put(s.player);
    w.put(s.sleep_vote);
    w.put_str(s.held, kHeldLen);
    w.put(s.held_kind);
    w.put(s.held_flags);
    for (float v : s.bed) w.put(v);
}

inline bool read_state(Reader& r, StateMsg& s) {
    s.seq = r.get<uint32_t>();
    s.t_ms = r.get<double>();
    s.valid = r.get<uint8_t>() != 0;
    s.scene = r.get_str(kSceneLen);
    s.x = r.get<float>();
    s.y = r.get<float>();
    s.z = r.get<float>();
    s.yaw = r.get<float>();
    s.pitch = r.get<float>();
    s.normalized_time = r.get<float>();
    s.day = r.get<int32_t>();
    s.flags = r.get<uint8_t>();
    s.player = r.get<uint8_t>();
    s.sleep_vote = r.get<uint8_t>();
    s.held = r.get_str(kHeldLen);
    s.held_kind = r.get<uint8_t>();
    s.held_flags = r.get<uint8_t>();
    for (float& v : s.bed) v = r.get<float>();
    return r.ok();
}

// Bytes one animal takes in an Animals message.
inline size_t animal_size(const AnimalRec& a) { return 4 + 3 + 28 + 1 + a.layers.size() * 8 + 2 + a.params.size(); }
constexpr size_t kAnimalsHeader = 7 + 4 + 8 + 2 + kSceneLen + 1;

inline void write_animals(Writer& w, const AnimalsMsg& m) {
    w.put(m.frame);
    w.put(m.t_ms);
    w.put(m.part);
    w.put(m.parts);
    w.put_str(m.scene, kSceneLen);
    w.put(static_cast<uint8_t>(m.animals.size()));
    for (const AnimalRec& a : m.animals) {
        w.put(a.id);
        w.put(a.kind);
        w.put(a.mode);
        w.put(a.flags);
        w.put(a.x);
        w.put(a.y);
        w.put(a.z);
        w.put(a.yaw);
        w.put(a.pitch);
        w.put(a.roll);
        w.put(a.hp);
        w.put(static_cast<uint8_t>(a.layers.size()));
        for (const AnimalLayer& l : a.layers) {
            w.put(l.state);
            w.put(l.time);
        }
        w.put_blob(a.params, kMaxAnimalParams);
    }
}

inline bool read_animals(Reader& r, AnimalsMsg& m) {
    m.frame = r.get<uint32_t>();
    m.t_ms = r.get<double>();
    m.part = r.get<uint8_t>();
    m.parts = r.get<uint8_t>();
    m.scene = r.get_str(kSceneLen);
    const uint8_t count = r.get<uint8_t>();
    m.animals.clear();
    for (uint8_t i = 0; i < count && r.ok(); ++i) {
        AnimalRec a;
        a.id = r.get<uint32_t>();
        a.kind = r.get<uint8_t>();
        a.mode = r.get<uint8_t>();
        a.flags = r.get<uint8_t>();
        a.x = r.get<float>();
        a.y = r.get<float>();
        a.z = r.get<float>();
        a.yaw = r.get<float>();
        a.pitch = r.get<float>();
        a.roll = r.get<float>();
        a.hp = r.get<float>();
        const uint8_t layers = r.get<uint8_t>();
        if (layers > kMaxAnimalLayers) return false;
        for (uint8_t l = 0; l < layers; ++l) {
            AnimalLayer layer;
            layer.state = r.get<int32_t>();
            layer.time = r.get<float>();
            a.layers.push_back(layer);
        }
        a.params = r.get_blob(kMaxAnimalParams);
        m.animals.push_back(std::move(a));
    }
    return r.ok();
}

inline void write_sounds(Writer& w, const std::vector<AnimalSound>& sounds) {
    w.put(static_cast<uint8_t>(sounds.size()));
    for (const AnimalSound& s : sounds) {
        w.put(s.animal);
        w.put(s.emitter);
        w.put(static_cast<uint8_t>(s.op));
        w.put(s.event);
        w.put(s.playing);
        w.put(s.action);
        w.put(s.fade_ms);
        w.put(s.curve);
    }
}

inline bool read_sounds(Reader& r, std::vector<AnimalSound>& out) {
    const uint8_t count = r.get<uint8_t>();
    for (uint8_t i = 0; i < count && r.ok(); ++i) {
        AnimalSound s;
        s.animal = r.get<uint32_t>();
        s.emitter = r.get<uint8_t>();
        s.op = static_cast<SoundOp>(r.get<uint8_t>());
        s.event = r.get<uint32_t>();
        s.playing = r.get<uint32_t>();
        s.action = r.get<int32_t>();
        s.fade_ms = r.get<int32_t>();
        s.curve = r.get<int32_t>();
        out.push_back(s);
    }
    return r.ok();
}

inline void write_attack(Writer& w, const AttackMsg& a) {
    w.put(a.animal);
    w.put(a.kind);
    w.put(static_cast<uint8_t>(a.type));
    w.put(a.damage);
    w.put(a.source);
    w.put(a.options);
}

inline bool read_attack(Reader& r, AttackMsg& a) {
    a.animal = r.get<uint32_t>();
    a.kind = r.get<uint8_t>();
    a.type = static_cast<AttackType>(r.get<uint8_t>());
    a.damage = r.get<float>();
    a.source = r.get<int32_t>();
    a.options = r.get<int32_t>();
    return r.ok();
}

inline void write_attack_result(Writer& w, const AttackResultMsg& a) {
    w.put(a.animal);
    w.put(static_cast<uint8_t>(a.outcome));
    w.put(a.hp);
    w.put(a.damage);
    w.put(a.flags);
    w.put(a.bleed_minutes);
}

inline bool read_attack_result(Reader& r, AttackResultMsg& a) {
    a.animal = r.get<uint32_t>();
    a.outcome = static_cast<FightOutcome>(r.get<uint8_t>());
    a.hp = r.get<float>();
    a.damage = r.get<float>();
    a.flags = r.get<uint8_t>();
    a.bleed_minutes = r.get<float>();
    return r.ok();
}

// A Reader past a received packet's header (magic, version, type), for the read_* functions.
inline Reader packet_body(const Packet& p) {
    Reader r(p.data.data(), p.data.size());
    r.get<uint32_t>();
    r.get<uint16_t>();
    r.get<uint8_t>();
    return r;
}

inline void put_floats(Writer& w, const float* v, int n) {
    for (int i = 0; i < n; ++i) w.put(v[i]);
}
inline void get_floats(Reader& r, float* v, int n) {
    for (int i = 0; i < n; ++i) v[i] = r.get<float>();
}

inline void write_hit(Writer& w, const HitMsg& h) {
    w.put(h.animal);
    w.put(static_cast<uint8_t>(h.weapon));
    w.put(h.body_part);
    w.put(h.flags);
    w.put_str(h.collider, kColliderLen);
    w.put(h.damage);
    w.put(h.bleed_minutes);
    put_floats(w, h.world, 3);
    put_floats(w, h.local, 3);
}

inline bool read_hit(Reader& r, HitMsg& h) {
    h.animal = r.get<uint32_t>();
    h.weapon = static_cast<Weapon>(r.get<uint8_t>());
    h.body_part = r.get<uint8_t>();
    h.flags = r.get<uint8_t>();
    h.collider = r.get_str(kColliderLen);
    h.damage = r.get<float>();
    h.bleed_minutes = r.get<float>();
    get_floats(r, h.world, 3);
    get_floats(r, h.local, 3);
    return r.ok();
}

inline void write_thrown(Writer& w, const ThrownMsg& t) {
    w.put(t.animal);
    w.put(static_cast<uint8_t>(t.item));
    w.put(t.flare_type);
    put_floats(w, t.pos, 3);
    put_floats(w, t.contact, 3);
}

inline bool read_thrown(Reader& r, ThrownMsg& t) {
    t.animal = r.get<uint32_t>();
    t.item = static_cast<ThrownItem>(r.get<uint8_t>());
    t.flare_type = r.get<uint8_t>();
    get_floats(r, t.pos, 3);
    get_floats(r, t.contact, 3);
    return r.ok();
}

inline void write_carcasses(Writer& w, const std::vector<CarcassRec>& list) {
    w.put(static_cast<uint8_t>(list.size()));
    for (const CarcassRec& c : list) {
        w.put(c.animal);
        w.put_str(c.prefab, kPrefabLen);
        put_floats(w, c.pos, 3);
        put_floats(w, c.rot, 4);
        w.put(c.damage_side);
        w.put(c.meat_units);
        w.put(c.hide);
        w.put(c.gut);
        w.put(c.condition);
        w.put(c.percent_frozen);
        w.put(c.flags);
        w.put(c.seconds_dead);
        w.put(c.harvest_ack);
    }
}

inline bool read_carcasses(Reader& r, std::vector<CarcassRec>& out) {
    const uint8_t count = r.get<uint8_t>();
    for (uint8_t i = 0; i < count && r.ok(); ++i) {
        CarcassRec c;
        c.animal = r.get<uint32_t>();
        c.prefab = r.get_str(kPrefabLen);
        get_floats(r, c.pos, 3);
        get_floats(r, c.rot, 4);
        c.damage_side = r.get<int8_t>();
        c.meat_units = r.get<int64_t>();
        c.hide = r.get<int32_t>();
        c.gut = r.get<int32_t>();
        c.condition = r.get<float>();
        c.percent_frozen = r.get<float>();
        c.flags = r.get<uint8_t>();
        c.seconds_dead = r.get<float>();
        c.harvest_ack = r.get<uint32_t>();
        out.push_back(std::move(c));
    }
    return r.ok();
}

inline void write_harvest(Writer& w, const HarvestMsg& h) {
    w.put(h.animal);
    w.put(h.seq);
    w.put(h.meat_units);
    w.put(h.hide);
    w.put(h.gut);
    w.put(h.flags);
}

inline bool read_harvest(Reader& r, HarvestMsg& h) {
    h.animal = r.get<uint32_t>();
    h.seq = r.get<uint32_t>();
    h.meat_units = r.get<int64_t>();
    h.hide = r.get<int32_t>();
    h.gut = r.get<int32_t>();
    h.flags = r.get<uint8_t>();
    return r.ok();
}

// Loose items lying in the world (milestone 8). TLD's persistent id (ObjectGuid, "PDID") names an item in both games.
constexpr size_t kPdidMax = 64;
constexpr size_t kGearPrefabMax = 64;
constexpr size_t kGearTextMax = 64 * 1024;  // one serialized GearItem
constexpr uint8_t kItemAnswer = 1;          // ItemPutMsg.flags: an answer to ItemWant (the host's list), not a change
constexpr uint8_t kItemNotYours = 2;        // host -> the taker: your take is refused (the host has it in its hands):
                                            // out of your pack, and it lies here again (the host's record and spot)
struct ItemPutMsg {
    std::string scene;
    uint32_t seq = 0;  // the sender's change counter (reliable messages can arrive out of order)
    uint8_t flags = 0;
    std::string pdid;
    std::string prefab;  // GearSaveData.m_PrefabName, e.g. "GEAR_Hatchet"
    float pos[3] = {};
    float rot[4] = {0, 0, 0, 1};
    std::string gear;    // GearSaveData.m_SearializedGear: everything TLD saves about the item (its pose included)
};
// NotYours: host -> the taker: your take is refused (someone else's came first): out of your pack. LostToss: the same,
// someone took it at the same moment and won the coin toss.
enum class ItemGoneReason : uint8_t { Taken = 1, Destroyed = 2, NotInHostWorld = 3, NotYours = 4, LostToss = 5 };
struct ItemGoneMsg {
    std::string scene;
    uint32_t seq = 0;
    ItemGoneReason reason = ItemGoneReason::Taken;
    std::string pdid;
};
// ItemList (host: everything loose in its scene) and ItemWant (client: send me these), as 64-bit hashes of the pdids.
struct ItemHashesMsg {
    std::string scene;
    std::vector<uint64_t> hashes;
};

inline uint64_t pdid_hash(const std::string& pdid) {  // FNV-1a
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : pdid) h = (h ^ c) * 1099511628211ull;
    return h;
}

inline void write_item_put(Writer& w, const ItemPutMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(m.seq);
    w.put(m.flags);
    w.put_blob(m.pdid, kPdidMax);
    w.put_blob(m.prefab, kGearPrefabMax);
    put_floats(w, m.pos, 3);
    put_floats(w, m.rot, 4);
    w.put_long_blob(m.gear);
}

inline bool read_item_put(Reader& r, ItemPutMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.seq = r.get<uint32_t>();
    m.flags = r.get<uint8_t>();
    m.pdid = r.get_blob(kPdidMax);
    m.prefab = r.get_blob(kGearPrefabMax);
    get_floats(r, m.pos, 3);
    get_floats(r, m.rot, 4);
    m.gear = r.get_long_blob(kGearTextMax);
    return r.ok();
}

inline void write_item_gone(Writer& w, const ItemGoneMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(m.seq);
    w.put(static_cast<uint8_t>(m.reason));
    w.put_blob(m.pdid, kPdidMax);
}

inline bool read_item_gone(Reader& r, ItemGoneMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.seq = r.get<uint32_t>();
    m.reason = static_cast<ItemGoneReason>(r.get<uint8_t>());
    m.pdid = r.get_blob(kPdidMax);
    return r.ok();
}

inline void write_item_hashes(Writer& w, const ItemHashesMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(static_cast<uint32_t>(m.hashes.size()));
    for (uint64_t h : m.hashes) w.put(h);
}

inline bool read_item_hashes(Reader& r, ItemHashesMsg& m) {
    m.scene = r.get_str(kSceneLen);
    const uint32_t n = r.get<uint32_t>();
    if (!r.ok() || n > kMaxMessage / 8) return false;
    m.hashes.resize(n);
    for (uint32_t i = 0; i < n; ++i) m.hashes[i] = r.get<uint64_t>();
    return r.ok();
}

// Containers (milestone 8b), named by TLD's persistent id for them (or "@name|x|y|z" in cm when they have none).
constexpr size_t kContainerTextMax = 200 * 1024;  // one Container.Serialize()
constexpr uint8_t kContainerAnswer = 1;           // ContainerPutMsg.flags: the host's answer to ContainerWant
constexpr uint8_t kContainerUnknown = 2;          // with kContainerAnswer: the host has no contents for it (keep yours)
struct ContainerPutMsg {
    std::string scene;
    uint32_t seq = 0;  // the sender's change counter (reliable messages can arrive out of order)
    uint8_t flags = 0;
    std::string id;
    std::string text;  // Container.Serialize(): everything TLD saves about the container and what's in it
};
struct ContainerWantMsg {
    std::string scene;
    std::string id;
};

inline void write_container_put(Writer& w, const ContainerPutMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(m.seq);
    w.put(m.flags);
    w.put_blob(m.id, kPdidMax);
    w.put_long_blob(m.text);
}

inline bool read_container_put(Reader& r, ContainerPutMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.seq = r.get<uint32_t>();
    m.flags = r.get<uint8_t>();
    m.id = r.get_blob(kPdidMax);
    m.text = r.get_long_blob(kContainerTextMax);
    return r.ok();
}

inline void write_container_want(Writer& w, const ContainerWantMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put_blob(m.id, kPdidMax);
}

inline bool read_container_want(Reader& r, ContainerWantMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.id = r.get_blob(kPdidMax);
    return r.ok();
}

// Things in the world that are broken down or harvested (milestone 8c); locks and safes, placements (decorations,
// movable containers) a player moved, and dead players' bodies (milestone 10). Named by TLD's persistent id for them
// (or "@name|x|y|z" in cm when they have none).
enum class ObjectKind : uint8_t { BreakDown = 1, Harvestable = 2, Lock = 3, Safe = 4, Placement = 5, Body = 6 };
constexpr size_t kPlacementEntryMax = 16 * 1024;  // one placement's record
constexpr size_t kBodyTextMax = 192 * 1024;       // one body: who, its TLD record and what it holds
inline bool has_text(ObjectKind k) { return k == ObjectKind::Placement || k == ObjectKind::Body; }
struct ObjectState {
    ObjectKind kind = ObjectKind::BreakDown;
    uint8_t done = 0;      // broken down / harvested; a lock: its LockState (0 locked, 1 unlocked, 2 forced); a safe:
                           // cracked; a placement: its PlacementState (0 there, 1 gone, 2 carried, 3 hidden)
    float refresh = 0;     // Harvestable.m_RefreshTime: the game hour it grows back (0: never)
    std::string id;
    std::string text;      // a placement: its record (PlaceableInfoSaveData, as TLD's save writes it); a body: whose,
                           // its record (CustomSpawnedItemSaveData) and its contents (Container.Serialize); sent for
                           // those only
};
struct ObjectPutMsg {
    std::string scene;
    uint32_t seq = 0;  // the sender's change counter (reliable messages can arrive out of order)
    ObjectState object;
};
constexpr size_t kSpawnTextMax = 200 * 1024;      // RandomSpawnObjectManager.Serialize()
constexpr size_t kPlacementTextMax = 200 * 1024;  // PlaceableManager.SerializeScenePlacements(), as TLD's save writes it
struct ObjectListMsg {
    std::string scene;
    std::vector<ObjectState> objects;
    std::string spawns;      // RandomSpawnObjectManager.Serialize(): which of each random set is there
    std::string placements;  // the scene's placements: which movable containers, corpses... are where, or gone
};

inline void write_object_state(Writer& w, const ObjectState& s) {
    w.put(static_cast<uint8_t>(s.kind));
    w.put(s.done);
    w.put(s.refresh);
    w.put_blob(s.id, kPdidMax);
    if (has_text(s.kind)) w.put_long_blob(s.text);
}

inline bool read_object_state(Reader& r, ObjectState& s) {
    const uint8_t kind = r.get<uint8_t>();
    s.kind = static_cast<ObjectKind>(kind);
    s.done = r.get<uint8_t>();
    s.refresh = r.get<float>();
    s.id = r.get_blob(kPdidMax);
    if (has_text(s.kind)) s.text = r.get_long_blob(s.kind == ObjectKind::Body ? kBodyTextMax : kPlacementEntryMax);
    return r.ok() && kind >= 1 && kind <= 6;
}

inline void write_object_put(Writer& w, const ObjectPutMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(m.seq);
    write_object_state(w, m.object);
}

inline bool read_object_put(Reader& r, ObjectPutMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.seq = r.get<uint32_t>();
    return read_object_state(r, m.object) && r.ok();
}

inline void write_object_list(Writer& w, const ObjectListMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(static_cast<uint32_t>(m.objects.size()));
    for (const ObjectState& s : m.objects) write_object_state(w, s);
    w.put_long_blob(m.spawns);
    w.put_long_blob(m.placements);
}

inline bool read_object_list(Reader& r, ObjectListMsg& m) {
    m.scene = r.get_str(kSceneLen);
    const uint32_t n = r.get<uint32_t>();
    if (!r.ok() || n > kMaxMessage / 8) return false;
    m.objects.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        if (!read_object_state(r, m.objects[i])) return false;
    m.spawns = r.get_long_blob(kSpawnTextMax);
    m.placements = r.get_long_blob(kPlacementTextMax);
    return r.ok();
}

// Fires (milestone 8d), named by TLD's persistent id for them.
constexpr size_t kFireTextMax = 64 * 1024;    // one fire's save record
constexpr size_t kFireListMax = 240 * 1024;   // FireManager.Serialize()
constexpr uint8_t kFireGone = 1;              // FirePutMsg.flags: the fire is gone
struct FirePutMsg {
    std::string scene;
    uint32_t seq = 0;  // the sender's change counter (reliable messages can arrive out of order)
    uint8_t flags = 0;
    std::string id;
    std::string text;  // {"m_Position":..., "m_PrefabName":..., "m_SearializedFire":..., "m_Guid":...}
};
struct FireListMsg {
    std::string scene;
    std::string text;  // FireManager.Serialize()
};

inline void write_fire_put(Writer& w, const FirePutMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put(m.seq);
    w.put(m.flags);
    w.put_blob(m.id, kPdidMax);
    w.put_long_blob(m.text);
}

inline bool read_fire_put(Reader& r, FirePutMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.seq = r.get<uint32_t>();
    m.flags = r.get<uint8_t>();
    m.id = r.get_blob(kPdidMax);
    m.text = r.get_long_blob(kFireTextMax);
    return r.ok();
}

inline void write_fire_list(Writer& w, const FireListMsg& m) {
    w.put_str(m.scene, kSceneLen);
    w.put_long_blob(m.text);
}

inline bool read_fire_list(Reader& r, FireListMsg& m) {
    m.scene = r.get_str(kSceneLen);
    m.text = r.get_long_blob(kFireListMax);
    return r.ok();
}

inline void write_world(Writer& w, const WorldMsg& m) {
    w.put(m.seq);
    w.put(static_cast<uint8_t>(m.valid ? 1 : 0));
    w.put(m.normalized_time);
    w.put(m.day);
    w.put(m.day_length_s);
    w.put(static_cast<uint8_t>(m.time_lapse ? 1 : 0));
    w.put_str(m.weather_set, kWeatherSetLen);
    w.put(m.weather_progress);
    w.put(m.weather_previous);
    w.put(m.weather_stage);
    w.put(m.wind_strength);
    w.put(m.wind_angle);
    w.put_blob(m.weather_set_state, kWeatherStateMax);
    w.put_str(m.region, kSceneLen);
    w.put_str(m.world_id, kWorldIdLen);
}

inline bool read_world(Reader& r, WorldMsg& m) {
    m.seq = r.get<uint32_t>();
    m.valid = r.get<uint8_t>() != 0;
    m.normalized_time = r.get<float>();
    m.day = r.get<int32_t>();
    m.day_length_s = r.get<float>();
    m.time_lapse = r.get<uint8_t>() != 0;
    m.weather_set = r.get_str(kWeatherSetLen);
    m.weather_progress = r.get<float>();
    m.weather_previous = r.get<int32_t>();
    m.weather_stage = r.get<int32_t>();
    m.wind_strength = r.get<int32_t>();
    m.wind_angle = r.get<float>();
    m.weather_set_state = r.get_blob(kWeatherStateMax);
    if (r.left() >= kSceneLen + kWorldIdLen) {  // tldbot's recordings from before v13 end before these
        m.region = r.get_str(kSceneLen);
        m.world_id = r.get_str(kWorldIdLen);
    }
    return r.ok();
}

// Everyone in the game (Roster), the host first.
struct RosterEntry {
    uint8_t id = 0;     // 0 the host, 1..3 the friends
    uint32_t link = 0;  // the host's serial for their connection (0 for the host itself)
    bool ready = false;
    std::string name, body;
};
inline void write_roster(Writer& w, const std::vector<RosterEntry>& players) {
    w.put(static_cast<uint8_t>(players.size()));
    for (const RosterEntry& p : players) {
        w.put(p.id);
        w.put(p.link);
        w.put(static_cast<uint8_t>(p.ready ? 1 : 0));
        w.put_str(p.name, kNameLen);
        w.put_str(p.body, kBodyLen);
    }
}
inline bool read_roster(Reader& r, std::vector<RosterEntry>& out) {
    const auto n = r.get<uint8_t>();
    if (!r.ok() || n > kMaxPlayers) return false;
    out.clear();
    for (uint8_t i = 0; i < n; ++i) {
        RosterEntry p;
        p.id = r.get<uint8_t>();
        p.link = r.get<uint32_t>();
        p.ready = r.get<uint8_t>() != 0;
        p.name = r.get_str(kNameLen);
        p.body = r.get_str(kBodyLen);
        out.push_back(std::move(p));
    }
    return r.ok();
}

// The host's experience mode: ExperienceModeManager.Serialize(), e.g. {"m_CurrentModeType":"Stalker",...}, and its
// game's random seed (what TLD's rolls for spawn chances, container contents and gear condition start from).
constexpr size_t kGameModeMax = 8 * 1024;
inline void write_game_mode(Writer& w, const std::string& text, int32_t seed) {
    w.put_long_blob(text);
    w.put<int32_t>(seed);
}
inline bool read_game_mode(Reader& r, std::string& text, int32_t& seed) {
    text = r.get_long_blob(kGameModeMax);
    seed = r.get<int32_t>();
    return r.ok();
}

// Sleeping together (milestone 10 item 12). The host decides from everyone's State (kStateInBed, sleep_vote) and
// starts the sleep in every game; each game sleeps it TLD's own way.
enum class SleepOp : uint8_t { Start = 1, End = 2, Woke = 3 };
enum class SleepEnd : uint8_t { Done = 1, Woken = 2 };
// Stopped: a player passing time stopped it (Escape, STOP).
enum class SleepWhy : uint8_t { None = 0, Animal = 1, Cold = 2, Other = 3, NotInBed = 4, Stopped = 5 };
struct SleepMsg {
    SleepOp op = SleepOp::Start;
    uint32_t id = 0;      // the host's count of shared sleeps
    uint8_t choice = 0;   // Start: the SleepChoice that won
    float hours = 0;      // Start: how long (exact: till dawn needn't be whole hours)
    SleepEnd end = SleepEnd::Done;  // End
    uint8_t who = 0;      // End Woken: who woke
    SleepWhy why = SleepWhy::None;  // End Woken, Woke
};
inline void write_sleep(Writer& w, const SleepMsg& m) {
    w.put(m.op);
    w.put(m.id);
    w.put(m.choice);
    w.put(m.hours);
    w.put(m.end);
    w.put(m.who);
    w.put(m.why);
}
inline bool read_sleep(Reader& r, SleepMsg& m) {
    m.op = r.get<SleepOp>();
    m.id = r.get<uint32_t>();
    m.choice = r.get<uint8_t>();
    m.hours = r.get<float>();
    m.end = r.get<SleepEnd>();
    m.who = r.get<uint8_t>();
    m.why = r.get<SleepWhy>();
    return r.ok();
}

// Handing an item to a friend (milestone 13, net/handover.h). Each player's pack is their own: the giver offers, the
// receiver accepts, and the giver, still holding it, takes it out of their pack and sends it (Done, TLD's save
// record for what changes hands); the receiver puts that in theirs, once. A friend's go to the host, which passes
// them on to everyone (each game shows the two avatars). Withdraw: taken back (by the giver, or the receiver walked
// off); Failed: the giver couldn't give it after all (it's gone from their pack).
enum class GiveOp : uint8_t { Offer = 1, Withdraw = 2, Accept = 3, Done = 4, Failed = 5 };
struct GiveMsg {
    GiveOp op = GiveOp::Offer;
    uint32_t id = 0;        // the giver's (net::handover_id): unique in the game
    uint8_t giver = 0, receiver = 0;
    uint8_t item = 0;       // net::HandItem: which grip the clips use
    int32_t units = 1;      // how many of it (a stack's units; 1 otherwise)
    std::string gear;       // the prefab held out (GEAR_...): the copy in the hand, the notices
    std::string prefab;     // Done: the save record's prefab (GearItemSaveDataProxy.m_PrefabName)
    std::string record;     // Done: the save record (m_SearializedGear) of what changes hands
};
inline void write_give(Writer& w, const GiveMsg& m) {
    w.put(m.op);
    w.put(m.id);
    w.put(m.giver);
    w.put(m.receiver);
    w.put(m.item);
    w.put(m.units);
    w.put_blob(m.gear, kGearPrefabMax);
    w.put_blob(m.prefab, kGearPrefabMax);
    w.put_long_blob(m.record);
}
inline bool read_give(Reader& r, GiveMsg& m) {
    m.op = r.get<GiveOp>();
    m.id = r.get<uint32_t>();
    m.giver = r.get<uint8_t>();
    m.receiver = r.get<uint8_t>();
    m.item = r.get<uint8_t>();
    m.units = r.get<int32_t>();
    m.gear = r.get_blob(kGearPrefabMax);
    m.prefab = r.get_blob(kGearPrefabMax);
    m.record = r.get_long_blob(kGearTextMax);
    return r.ok() && m.op >= GiveOp::Offer && m.op <= GiveOp::Failed;
}

}  // namespace net
