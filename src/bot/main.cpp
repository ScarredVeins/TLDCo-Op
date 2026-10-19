// tldbot: a fake second player for testing TLDCoop alone. It speaks the same protocol as the mod (shared tldnet
// library), follows the real player's scene and walks circles around them.
//
//   tldbot join [ip] [port]   connect to a game running with mode=host
//   tldbot host [port]        wait for a game running with mode=join
// options: --name N  --body B (how the game draws the bot)  --cycle B1,B2,.. --cycle-seconds S
//          --seconds S (exit after S seconds, print a summary)  --radius R  --speed DEG_PER_SEC
//          --game-version "2.55 181032 S" (default: read from ..\tld_Data\StreamingAssets\version.txt)
// scenes (milestone 6): --park-at X,Y,Z stays at that spot in the real player's scene; --repark parks again
//          where the player is after a teleport within a scene.
// scenes (milestone 6): --park stays where the real player first was (same scene, same spot) instead of
//          following them, to check the avatar hides when they go elsewhere and is back when they return.
//          Every scene change and loading state the real player reports is printed.
// animals (milestone 7): join: prints the host's animals; --record-animals FILE keeps the stream.
//                        host: --replay-animals FILE sends a recording, looping, with its original timing.
// attacks (milestone 7c): join: a fight from the host's animal is "fought" for --fight-seconds S (default 6, shown
//          as struggling), then answered with --fight-result fled|died|none (default fled; hp 40).
//          host: --attack-every S sends a fight from the replayed animal nearest the player every S seconds.
//          --struggle-loop ON,OFF: shown struggling for ON seconds, then standing for OFF, over and over (to watch
//          the body's struggle animation without an animal).
// world (milestone 5):
//   join: prints the host's clock, weather and wind; --record-world FILE saves the newest to FILE
//   host: sends a world: --world-file FILE (a recording) and/or --time HH:MM, --day-length SECONDS,
//         --weather-set NAME [--weather-progress 0..1], --weather-stage NAME, --wind STRENGTH[,ANGLE]

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
#include <sstream>
#include <string>

#include "net/interp.h"
#include "net/session.h"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

void print(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    printf("[bot %02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    fflush(stdout);
}

std::string version_next_to_exe() {
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const auto game = std::filesystem::path(std::wstring(exe, n)).parent_path().parent_path();
    std::ifstream in(game / L"tld_Data" / L"StreamingAssets" / L"version.txt", std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

const char* const kStages[] = {"DenseFog", "LightSnow", "HeavySnow", "PartlyCloudy", "Clear", "Cloudy",
                               "LightFog", "Blizzard", "ClearAurora", "ToxicFog", "ElectrostaticFog", "Undefined"};
const char* const kWinds[] = {"Calm", "SlightlyWindy", "Windy", "VeryWindy", "Blizzard"};

template <size_t N>
const char* name_in(const char* const (&names)[N], int32_t v) {
    return v >= 0 && static_cast<size_t>(v) < N ? names[v] : "?";
}

template <size_t N>
int32_t index_in(const char* const (&names)[N], const std::string& name) {
    for (size_t i = 0; i < N; ++i)
        if (_stricmp(names[i], name.c_str()) == 0) return static_cast<int32_t>(i);
    return -1;
}

std::string clock_text(float normalized) {
    const int minutes = static_cast<int>((normalized - std::floor(normalized)) * 1440.0f + 0.5f) % 1440;
    char s[8];
    snprintf(s, sizeof(s), "%02d:%02d", minutes / 60, minutes % 60);
    return s;
}

std::string describe(const net::WorldMsg& w) {
    char line[512];
    snprintf(line, sizeof(line),
             "%s day %d (day = %.0f s%s) | weather '%s' %.0f%% %s (prev %s, state %zu chars) | wind %s %.0f deg",
             clock_text(w.normalized_time).c_str(), w.day, w.day_length_s, w.time_lapse ? ", time lapse" : "",
             w.weather_set.c_str(), w.weather_progress * 100, name_in(kStages, w.weather_stage),
             name_in(kStages, w.weather_previous), w.weather_set_state.size(), name_in(kWinds, w.wind_strength),
             w.wind_angle);
    return line;
}

bool save_world(const std::string& path, const net::WorldMsg& w) {
    net::Writer out(net::Msg::World);
    net::write_world(out, w);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return out.ok() && f.good();
}

bool load_world(const std::string& path, net::WorldMsg& w) {
    std::ifstream f(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    net::Reader in(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    in.get<uint32_t>();  // magic
    in.get<uint16_t>();  // protocol version
    in.get<uint8_t>();   // message type
    return net::read_world(in, w);
}

const char* const kKinds[] = {"none", "wolf", "bear", "stag", "rabbit", "moose", "cougar"};

// Recorded animal stream, per entry: u8 type (1 = an Animals part, 2 = AnimalSounds), f64 ms since the first entry,
// u16 size, the message bytes.
struct RecordedPart {
    uint8_t type = 1;
    double at_ms = 0;
    net::AnimalsMsg msg;
    std::vector<net::AnimalSound> sounds;
};

bool load_animals(const std::string& path, std::vector<RecordedPart>& out) {
    std::ifstream f(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t at = 0;
    while (at + 11 <= bytes.size()) {
        RecordedPart part;
        uint16_t size = 0;
        part.type = static_cast<uint8_t>(bytes[at]);
        memcpy(&part.at_ms, bytes.data() + at + 1, 8);
        memcpy(&size, bytes.data() + at + 9, 2);
        at += 11;
        if (at + size > bytes.size()) break;
        net::Reader in(reinterpret_cast<const uint8_t*>(bytes.data() + at), size);
        in.get<uint32_t>();
        in.get<uint16_t>();
        in.get<uint8_t>();
        const bool ok = part.type == 2 ? net::read_sounds(in, part.sounds) : net::read_animals(in, part.msg);
        if (ok) out.push_back(std::move(part));
        at += size;
    }
    return !out.empty();
}

// One line per world message we get: which one, what it's about, where.
void print_world_packet(const net::Packet& p) {
    net::Reader r = net::packet_body(p);
    switch (p.type) {
        case net::Msg::ItemPut: {
            net::ItemPutMsg m;
            if (net::read_item_put(r, m))
                print("world: ItemPut %s %s in %s (seq %u%s)", m.prefab.c_str(), m.pdid.c_str(), m.scene.c_str(), m.seq,
                      (m.flags & net::kItemAnswer) ? ", an answer" : "");
            break;
        }
        case net::Msg::ItemGone: {
            net::ItemGoneMsg m;
            if (net::read_item_gone(r, m)) print("world: ItemGone %s in %s (seq %u)", m.pdid.c_str(), m.scene.c_str(), m.seq);
            break;
        }
        case net::Msg::ContainerPut: {
            net::ContainerPutMsg m;
            if (net::read_container_put(r, m))
                print("world: ContainerPut %s in %s (seq %u, %zu chars)", m.id.c_str(), m.scene.c_str(), m.seq, m.text.size());
            break;
        }
        case net::Msg::ObjectPut: {
            net::ObjectPutMsg m;
            if (net::read_object_put(r, m))
                print("world: ObjectPut %s in %s (seq %u, done %u)", m.object.id.c_str(), m.scene.c_str(), m.seq, m.object.done);
            break;
        }
        case net::Msg::FirePut: {
            net::FirePutMsg m;
            if (net::read_fire_put(r, m)) print("world: FirePut %s in %s (seq %u)", m.id.c_str(), m.scene.c_str(), m.seq);
            break;
        }
        case net::Msg::ItemList: {
            net::ItemHashesMsg m;
            if (net::read_item_hashes(r, m)) print("world: ItemList %s (%zu items)", m.scene.c_str(), m.hashes.size());
            break;
        }
        case net::Msg::ObjectList: {
            net::ObjectListMsg m;
            if (net::read_object_list(r, m)) print("world: ObjectList %s (%zu)", m.scene.c_str(), m.objects.size());
            break;
        }
        case net::Msg::FireList: {
            net::FireListMsg m;
            if (net::read_fire_list(r, m)) print("world: FireList %s (%zu chars)", m.scene.c_str(), m.text.size());
            break;
        }
        case net::Msg::GameMode: print("world: GameMode (%zu bytes)", p.data.size()); break;
        default: break;
    }
}

int usage() {
    printf(
        "usage: tldbot join [ip] [port] [options]\n"
        "       tldbot host [port] [options]\n"
        "options: --name N, --body B, --cycle B1,B2,... (switch body every --cycle-seconds, default 10),\n"
        "         --seconds S, --radius R, --speed DEG_PER_SEC, --game-version V\n"
        "         --park (stay where the real player first was instead of following them),\n"
        "         --repark (park again where they are after a teleport), --park-at X,Y,Z (stay there)\n"
        "animals: join: --record-animals FILE; host: --replay-animals FILE\n"
        "attacks: join: --fight-seconds S, --fight-result fled|died|none; host: --attack-every S\n"
        "         --struggle-loop ON,OFF (shown struggling ON seconds, standing OFF seconds, repeating)\n"
        "world:   join: --record-world FILE\n"
        "         host: --world-file FILE, --time HH:MM, --day-length S, --weather-set NAME,\n"
        "               --weather-progress F, --weather-stage NAME, --wind STRENGTH[,ANGLE]\n");
    return 2;
}

constexpr double kPi = 3.14159265358979323846;

std::vector<std::string> split(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream ss(list);
    for (std::string item; std::getline(ss, item, ',');)
        if (!item.empty()) out.push_back(item);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();

    net::Config cfg;
    cfg.name = "TestBot";
    double run_seconds = 0;
    std::vector<std::string> cycle;  // bodies to switch through, for checking several in one session
    double cycle_seconds = 10;
    float radius = 3.0f;
    float speed_deg = 30.0f;
    std::string record_world, world_file, time_opt, weather_set_opt, weather_stage_opt, wind_opt;
    double day_length_opt = 0, weather_progress_opt = -1;
    bool park = false, repark = false;
    bool park_at_set = false;
    float park_at[3] = {0, 0, 0};
    std::string record_animals, replay_animals;
    double fight_seconds = 6, attack_every = 0, struggle_on = 0, struggle_off = 0;
    std::string fight_result = "fled";

    const std::string mode = argv[1];
    if (mode == "join")
        cfg.role = net::Role::Client;
    else if (mode == "host")
        cfg.role = net::Role::Host;
    else
        return usage();

    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--name") cfg.name = next();
        else if (a == "--body") cfg.body = next();
        else if (a == "--cycle") cycle = split(next());
        else if (a == "--cycle-seconds") cycle_seconds = atof(next().c_str());
        else if (a == "--seconds") run_seconds = atof(next().c_str());
        else if (a == "--radius") radius = static_cast<float>(atof(next().c_str()));
        else if (a == "--speed") speed_deg = static_cast<float>(atof(next().c_str()));
        else if (a == "--game-version") cfg.game_version = next();
        else if (a == "--record-world") record_world = next();
        else if (a == "--world-file") world_file = next();
        else if (a == "--time") time_opt = next();
        else if (a == "--day-length") day_length_opt = atof(next().c_str());
        else if (a == "--weather-set") weather_set_opt = next();
        else if (a == "--weather-progress") weather_progress_opt = atof(next().c_str());
        else if (a == "--weather-stage") weather_stage_opt = next();
        else if (a == "--wind") wind_opt = next();
        else if (a == "--park") park = true;
        else if (a == "--repark") park = repark = true;
        else if (a == "--park-at") {
            park = park_at_set = sscanf(next().c_str(), "%f,%f,%f", &park_at[0], &park_at[1], &park_at[2]) == 3;
            if (!park_at_set) return usage();
        }
        else if (a == "--record-animals") record_animals = next();
        else if (a == "--replay-animals") replay_animals = next();
        else if (a == "--fight-seconds") fight_seconds = atof(next().c_str());
        else if (a == "--fight-result") fight_result = next();
        else if (a == "--attack-every") attack_every = atof(next().c_str());
        else if (a == "--struggle-loop") {
            if (sscanf(next().c_str(), "%lf,%lf", &struggle_on, &struggle_off) != 2 || struggle_on <= 0) return usage();
        }
        else if (cfg.role == net::Role::Client && positional == 0 && a.find('.') != std::string::npos) {
            cfg.host_ip = a;
            ++positional;
        } else {
            cfg.port = static_cast<uint16_t>(atoi(a.c_str()));
            ++positional;
        }
    }
    if (cfg.game_version.empty()) cfg.game_version = version_next_to_exe();
    if (cfg.game_version.empty()) {
        printf("cannot find the game version; run from <game>\\TLDCoop or pass --game-version\n");
        return 2;
    }

    // Host: the world we send. A recording first, then the options on top.
    net::WorldMsg world;
    bool sends_world = false;
    if (cfg.role == net::Role::Host) {
        if (!world_file.empty()) {
            if (!load_world(world_file, world)) {
                printf("cannot read a world from %s\n", world_file.c_str());
                return 2;
            }
            sends_world = true;
        }
        if (!time_opt.empty()) {
            int h = 0, m = 0;
            if (sscanf(time_opt.c_str(), "%d:%d", &h, &m) < 1) return usage();
            world.normalized_time = static_cast<float>((h * 60 + m) % 1440) / 1440.0f;
            sends_world = true;
        }
        if (day_length_opt > 0) world.day_length_s = static_cast<float>(day_length_opt);
        if (!weather_set_opt.empty()) {
            world.weather_set = weather_set_opt;
            world.weather_set_state.clear();  // the game rolls the set's stage lengths itself
            if (weather_progress_opt < 0) world.weather_progress = 0;
            sends_world = true;
        }
        if (weather_progress_opt >= 0) world.weather_progress = static_cast<float>(weather_progress_opt);
        if (!weather_stage_opt.empty()) {
            world.weather_stage = index_in(kStages, weather_stage_opt);
            if (world.weather_stage < 0) {
                printf("unknown weather stage '%s'\n", weather_stage_opt.c_str());
                return 2;
            }
            if (weather_set_opt.empty() && world_file.empty()) world.weather_set.clear();
            sends_world = true;
        }
        if (!wind_opt.empty()) {
            const auto parts = split(wind_opt);
            world.wind_strength = parts.empty() ? -1 : index_in(kWinds, parts[0]);
            if (world.wind_strength < 0) {
                printf("unknown wind strength '%s'\n", wind_opt.c_str());
                return 2;
            }
            if (parts.size() > 1) world.wind_angle = static_cast<float>(atof(parts[1].c_str()));
            sends_world = true;
        }
        if (sends_world && world.day_length_s <= 0) {
            world.day_length_s = 7200;
            printf("no day length given; assuming %.0f s per game day (pass --day-length)\n", world.day_length_s);
        }
        world.valid = true;
        if (sends_world) print("sending world: %s", describe(world).c_str());
    }
    std::vector<RecordedPart> replay;
    if (cfg.role == net::Role::Host && !replay_animals.empty()) {
        if (!load_animals(replay_animals, replay)) {
            printf("cannot read animals from %s\n", replay_animals.c_str());
            return 2;
        }
        print("replaying %zu animal parts (%.1f s, looping) from %s", replay.size(), replay.back().at_ms / 1000.0,
              replay_animals.c_str());
    }
    size_t replay_next = 0;
    double replay_start = -1;
    std::ofstream animals_file;
    if (cfg.role == net::Role::Client && !record_animals.empty())
        animals_file.open(record_animals, std::ios::binary | std::ios::trunc);
    double animals_first_ms = -1;
    std::vector<net::AnimalsMsg> animal_parts;
    std::vector<net::AnimalSound> animal_sounds;
    uint64_t sounds_total = 0;
    auto record = [&](uint8_t type, double received_ms, const net::Writer& w) {
        if (!animals_file) return;
        if (animals_first_ms < 0) animals_first_ms = received_ms;
        const double at = received_ms - animals_first_ms;
        const uint16_t size = static_cast<uint16_t>(w.size());
        animals_file.write(reinterpret_cast<const char*>(&type), 1);
        animals_file.write(reinterpret_cast<const char*>(&at), 8);
        animals_file.write(reinterpret_cast<const char*>(&size), 2);
        animals_file.write(reinterpret_cast<const char*>(w.data()), size);
    };
    std::map<uint32_t, net::AnimalRec> animals_seen;  // newest record per animal, for the summary
    uint64_t animal_parts_total = 0;
    // Attacks: the join side "fights"; the host side sends them and hears back.
    bool fighting = false;
    bool was_pretending = false;  // --struggle-loop
    double fight_until = 0;
    uint32_t fight_animal = 0;
    double last_attack = 0;
    std::map<uint32_t, net::AnimalRec> replayed;  // newest replayed record per animal, to pick the nearest
    std::map<uint32_t, double> replayed_at;       // when each was last in the replay (gone ones can't attack)
    std::vector<net::AttackMsg> attacks_in;
    std::vector<net::AttackResultMsg> results_in;
    const float world_start_time = world.normalized_time;
    uint32_t last_world_seq = 0;
    bool have_world = false;

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (!cycle.empty()) cfg.body = cycle[0];
    net::Session session([](const std::string& line) { print("%s", line.c_str()); });
    if (!session.start(cfg)) return 1;
    session.set_ready(true);  // a bot is always READY in a lobby
    size_t cycle_index = 0;
    double last_cycle = -1;  // starts once connected and the host is in game

    const double start = net::now_ms();
    double last_move = start, last_report = start;
    double angle = 0;
    uint32_t last_remote_seq = 0;
    bool have_last_remote = false;
    net::StateMsg last_remote;
    double host_distance = 0;
    double rtt_sum = 0, rtt_min = 1e9, rtt_max = 0;
    int rtt_samples = 0;
    bool ever_connected = false;
    // A short delay is enough here: the bot only needs a smooth path for its orbit centre.
    net::Interpolator host_track(50);
    bool parked = false;
    net::StateMsg anchor;            // --park: where we stay
    std::string reported_scene;      // the real player's scene and loading state as last printed
    bool reported_valid = false, reported_any = false;
    net::StateMsg last_valid;        // the real player's last in-game state, to spot teleports
    bool have_last_valid = false;
    std::vector<net::TimedState> incoming;
    std::vector<net::Packet> world_packets;

    while (!g_stop) {
        session.update();
        const double now = net::now_ms();
        const auto remote = session.remote_state();
        if (!remote && parked) {  // the real player left: park afresh when someone (re)connects
            parked = false;
            print("unparked (no player)");
        }

        // Track how far the real player walked. Same scene only, and jumps over 20 m in one 50 ms update are
        // teleports or region loads (the scene name changes a moment before the position does).
        if (remote && remote->seq != last_remote_seq) {
            if (have_last_remote && remote->valid && last_remote.valid && remote->scene == last_remote.scene) {
                const double dx = remote->x - last_remote.x, dy = remote->y - last_remote.y,
                             dz = remote->z - last_remote.z;
                const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (step < 20.0) host_distance += step;
            }
            last_remote = *remote;
            last_remote_seq = remote->seq;
            have_last_remote = true;
        }

        // Every scene change and loading state of the real player, with where they are once in.
        if (remote && (!reported_any || remote->scene != reported_scene || remote->valid != reported_valid)) {
            if (remote->valid)
                print("player: in '%s' at (%.1f, %.1f, %.1f)", remote->scene.c_str(), remote->x, remote->y, remote->z);
            else
                print("player: loading or in a menu (scene '%s')", remote->scene.c_str());
            reported_any = true;
            reported_scene = remote->scene;
            reported_valid = remote->valid;
        }

        session.drain_remote_states(incoming);
        for (const auto& ts : incoming)  // the player we follow (others' states come too, with 3-4 players)
            if (remote && ts.state.player == remote->player) host_track.add(ts.state, ts.received_ms);
        incoming.clear();

        // World changes and lists that reach us (a third player sees what the host passes on).
        world_packets.clear();
        session.drain_packets(world_packets);
        for (const net::Packet& p : world_packets) print_world_packet(p);

        // Move ~60 Hz (like the mod's capture rate) around the host's smoothed position.
        if (now - last_move >= 16) {
            const double dt = (now - last_move) / 1000.0;
            last_move = now;
            const bool pretending =
                struggle_on > 0 && std::fmod((now - start) / 1000.0, struggle_on + struggle_off) < struggle_on;
            if (pretending != was_pretending)
                print("struggle loop: %s", pretending ? "shown struggling" : "back on their feet");
            was_pretending = pretending;
            const bool struggling = fighting || pretending;
            if (!struggling) angle += speed_deg * kPi / 180.0 * dt;  // pinned during a fight, like a real player
            net::StateMsg me;
            me.t_ms = now;
            net::StateMsg host;
            const bool follow = remote && host_track.sample(now, host) && host.valid;
            // --park: a jump within the same scene is a teleport (a test moving the player); park there instead.
            if (repark && parked && remote && remote->valid && have_last_valid && remote->scene == last_valid.scene) {
                const double tx = remote->x - last_valid.x, tz = remote->z - last_valid.z;
                if (std::sqrt(tx * tx + tz * tz) > 50.0) {
                    parked = false;
                    print("the player was teleported; parking again where they are now");
                }
            }
            if (remote && remote->valid) {
                last_valid = *remote;
                have_last_valid = true;
            }
            if (park && !parked && remote && remote->valid) {  // the newest state: smoothing lags behind
                parked = true;
                anchor = *remote;
                if (park_at_set) {  // a chosen spot in the player's scene
                    anchor.x = park_at[0];
                    anchor.y = park_at[1];
                    anchor.z = park_at[2];
                }
                print("parked in '%s' at (%.1f, %.1f, %.1f); staying here", anchor.scene.c_str(), anchor.x, anchor.y,
                      anchor.z);
            }
            const net::StateMsg& centre = parked ? anchor : host;
            const bool here = parked || follow;
            const double cx = here ? centre.x : 0, cy = here ? centre.y : 0, cz = here ? centre.z : 0;
            me.valid = here;
            me.scene = here ? centre.scene : "";
            me.x = static_cast<float>(cx + radius * std::cos(angle));
            me.y = static_cast<float>(cy);
            me.z = static_cast<float>(cz + radius * std::sin(angle));
            // Face along the circle (Unity yaw: 0 = +Z, 90 = +X).
            const double vx = -std::sin(angle), vz = std::cos(angle);
            me.yaw = static_cast<float>(std::fmod(std::atan2(vx, vz) * 180.0 / kPi + 360.0, 360.0));
            me.normalized_time = follow ? host.normalized_time : 0;
            me.flags = struggling ? net::kStateStruggling : 0;
            me.day = follow ? host.day : 0;
            session.set_local_state(me);
        }

        if (!cycle.empty() && remote && remote->valid) {
            if (last_cycle < 0) {
                last_cycle = now;
                print("showing body '%s' (%zu/%zu)", cycle[0].c_str(), cycle_index + 1, cycle.size());
            } else if (now - last_cycle >= cycle_seconds * 1000) {
                last_cycle = now;
                cycle_index = (cycle_index + 1) % cycle.size();
                session.set_body(cycle[cycle_index]);
                print("showing body '%s' (%zu/%zu)", cycle[cycle_index].c_str(), cycle_index + 1, cycle.size());
            }
        }

        // Host: our clock runs at the game's rate.
        if (sends_world) {
            const double elapsed_s = (now - start) / 1000.0;
            world.normalized_time =
                static_cast<float>(std::fmod(world_start_time + elapsed_s / world.day_length_s, 1.0));
            session.set_world(world);
        }
        // Host: replay recorded animals with their original spacing, looping; parts of one frame go together.
        if (!replay.empty() && session.status().connected) {
            if (replay_start < 0) replay_start = now;
            if (replay_next >= replay.size()) {
                replay_next = 0;
                replay_start = now;
            }
            std::vector<net::AnimalsMsg> frame;
            while (replay_next < replay.size() && now - replay_start >= replay[replay_next].at_ms) {
                if (replay[replay_next].type == 2) {
                    session.send_animal_sounds(replay[replay_next].sounds);
                    ++replay_next;
                    continue;
                }
                if (!frame.empty() && replay[replay_next].msg.frame != frame.back().frame) {
                    session.send_animals(frame);
                    frame.clear();
                }
                net::AnimalsMsg m = replay[replay_next].msg;
                m.t_ms = now;
                for (const net::AnimalRec& a : m.animals) {
                    replayed[a.id] = a;
                    replayed_at[a.id] = now;
                }
                frame.push_back(std::move(m));
                ++replay_next;
            }
            if (!frame.empty()) session.send_animals(std::move(frame));
        }
        // Join: the host's animals attack us.
        attacks_in.clear();
        session.drain_attacks(attacks_in);
        for (const net::AttackMsg& a : attacks_in) {
            const char* kind = kKinds[a.kind < 7 ? a.kind : 0];
            if (a.type == net::AttackType::Struggle) {
                if (fighting) {
                    net::AttackResultMsg r;
                    r.animal = a.animal;
                    session.send_attack_result(r);  // NoFight: busy
                    continue;
                }
                fighting = true;
                fight_animal = a.animal;
                fight_until = now + fight_seconds * 1000;
                print("attack: the host's %s #%u jumped us; fighting for %.0f s", kind, a.animal, fight_seconds);
            } else if (a.type == net::AttackType::Damage) {
                print("attack: the host's %s #%u hurt us: %.1f damage (source %d)", kind, a.animal, a.damage, a.source);
            } else {
                print("attack: the host's %s #%u wounded us (options %d)", kind, a.animal, a.options);
            }
        }
        if (fighting && now >= fight_until) {
            fighting = false;
            net::AttackResultMsg r;
            r.animal = fight_animal;
            r.outcome = fight_result == "died" ? net::FightOutcome::Died
                        : fight_result == "none" ? net::FightOutcome::NoFight : net::FightOutcome::Fled;
            r.hp = r.outcome == net::FightOutcome::Died ? 0.0f : 40.0f;
            session.send_attack_result(r);
            print("attack: fight with #%u over: it %s", fight_animal, fight_result.c_str());
        }
        // Host: send a fight from the replayed animal nearest the player.
        if (attack_every > 0 && remote && remote->valid && now - last_attack >= attack_every * 1000 && !replayed.empty()) {
            const net::AnimalRec* best = nullptr;
            double best_d = 1e18;
            for (const auto& [id, a] : replayed) {
                if (now - replayed_at[id] > 1000) continue;  // no longer in the replay
                const double dx = a.x - remote->x, dz = a.z - remote->z, d = dx * dx + dz * dz;
                const bool predator = a.kind == 1 || a.kind == 2 || a.kind == 5 || a.kind == 6;  // wolf bear moose cougar
                if (predator && !(a.flags & net::kAnimalDead) && d < best_d) best = &a, best_d = d;
            }
            if (best && best_d > 80.0 * 80.0) best = nullptr;  // only one near the player can jump them
            if (best) {
                last_attack = now;
                net::AttackMsg m;
                m.animal = best->id;
                m.kind = best->kind;
                m.type = net::AttackType::Struggle;
                session.send_attack(m);
                print("attack: sent a fight from %s #%u (%.0f m from the player)", kKinds[best->kind < 7 ? best->kind : 0],
                      best->id, std::sqrt(best_d));
            }
        }
        results_in.clear();
        session.drain_attack_results(results_in);
        for (const net::AttackResultMsg& r : results_in)
            print("attack result: #%u %s, hp %.0f", r.animal,
                  r.outcome == net::FightOutcome::Died ? "died" : r.outcome == net::FightOutcome::Fled ? "fled" : "no fight",
                  r.hp);

        // Join: keep (and record) the host's animals.
        animal_parts.clear();
        session.drain_animals(animal_parts);
        for (const net::AnimalsMsg& part : animal_parts) {
            ++animal_parts_total;
            for (const net::AnimalRec& a : part.animals) animals_seen[a.id] = a;
            net::Writer w(net::Msg::Animals);
            net::write_animals(w, part);
            record(1, part.received_ms, w);
        }
        animal_sounds.clear();
        session.drain_animal_sounds(animal_sounds);
        if (!animal_sounds.empty()) {
            sounds_total += animal_sounds.size();
            net::Writer w(net::Msg::AnimalSounds);
            net::write_sounds(w, animal_sounds);
            record(2, now, w);
        }

        // Join: watch (and keep) the host's world.
        if (const auto w = session.remote_world(); w && (!have_world || w->seq != last_world_seq)) {
            if (!have_world || w->weather_set != world.weather_set || w->weather_stage != world.weather_stage ||
                w->valid != world.valid)
                print("host world: %s%s", w->valid ? "" : "(host not in game) ", describe(*w).c_str());
            have_world = true;
            last_world_seq = w->seq;
            world = *w;
            if (!record_world.empty() && w->valid && !save_world(record_world, *w))
                print("could not write %s", record_world.c_str());
        }

        if (now - last_report >= 1000) {
            last_report = now;
            const net::Status st = session.status();
            ever_connected |= st.connected;
            if (st.connected && st.rtt_ms >= 0) {
                rtt_sum += st.rtt_ms;
                rtt_min = std::min(rtt_min, st.rtt_ms);
                rtt_max = std::max(rtt_max, st.rtt_ms);
                ++rtt_samples;
            }
            if (!st.connected) print("%s", st.rejected ? "rejected by host" : "not connected yet");
            if (!animals_seen.empty()) {
                std::string list;
                for (const auto& [id, a] : animals_seen)
                    list += (list.empty() ? "" : "; ") + std::string(kKinds[a.kind < 7 ? a.kind : 0]) + " #" +
                            std::to_string(id) + " mode " + std::to_string(a.mode) + " at (" +
                            std::to_string(static_cast<int>(a.x)) + ", " + std::to_string(static_cast<int>(a.z)) +
                            ") layers " + std::to_string(a.layers.size()) + " params " +
                            std::to_string(a.params.size()) + "B";
                print("host animals (%llu parts, %llu sounds so far): %s",
                      static_cast<unsigned long long>(animal_parts_total), static_cast<unsigned long long>(sounds_total),
                      list.c_str());
                animals_seen.clear();
            }
            if (st.connected && remote && remote->valid)
                print("rtt %.2f ms | %s in %s pos=(%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f moved %.1f m | recv %llu "
                      "sent %llu",
                      st.rtt_ms, st.peer_name.c_str(), remote->scene.c_str(), remote->x, remote->y, remote->z,
                      remote->yaw, remote->pitch, host_distance, static_cast<unsigned long long>(st.states_received),
                      static_cast<unsigned long long>(st.states_sent));
            else if (st.connected)
                print("rtt %.2f ms | %s is in a menu or loading | recv %llu sent %llu", st.rtt_ms,
                      st.peer_name.c_str(), static_cast<unsigned long long>(st.states_received),
                      static_cast<unsigned long long>(st.states_sent));
        }

        if (run_seconds > 0 && now - start >= run_seconds * 1000) break;
        session.wait(5);
    }

    const net::Status st = session.status();
    if (have_world)
        print("last host world: %s%s", describe(world).c_str(),
              record_world.empty() ? "" : (" (saved to " + record_world + ")").c_str());
    session.stop();
    print("summary: ever connected=%s, states sent %llu, received %llu, rtt avg %.2f / min %.2f / max %.2f ms, "
          "real player moved %.1f m",
          ever_connected ? "yes" : "no", static_cast<unsigned long long>(st.states_sent),
          static_cast<unsigned long long>(st.states_received), rtt_samples ? rtt_sum / rtt_samples : -1.0,
          rtt_samples ? rtt_min : -1.0, rtt_samples ? rtt_max : -1.0, host_distance);
    return ever_connected ? 0 : 1;
}
