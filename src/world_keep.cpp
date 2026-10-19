#include "world_keep.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "game_state.h"
#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "net_runtime.h"
#include "paths.h"
#include "world_containers.h"
#include "world_fires.h"
#include "world_items.h"
#include "world_objects.h"

namespace world_keep {
namespace {

constexpr const char* kGame = "Assembly-CSharp.dll";
constexpr uint32_t kFileMagic = 0x57444C54;  // bytes "TLDW"
constexpr uint32_t kFileVersion = 1;

bool g_ready = false;
bool g_loaded = false;  // this game's file was read in (since the main menu); saves write only after that

bool hosting() {
    const auto role = netrt::role();
    return role && *role == net::Role::Host;
}

// "sandbox5:1234:5678" -> <instance_dir>/worlds/sandbox5_1234_5678.bin
std::filesystem::path file_for(const std::string& id) {
    std::string name;
    for (char c : id) name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
    return paths::instance_dir() / "worlds" / (name + ".bin");
}

void put_u32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); }

bool get_u32(const std::string& in, size_t& at, uint32_t& v) {
    if (at + sizeof(v) > in.size()) return false;
    memcpy(&v, in.data() + at, sizeof(v));
    at += sizeof(v);
    return true;
}

void save() {
    if (!hosting()) return;
    const std::string id = game::save_id();
    if (id.empty()) return;
    if (!g_loaded) {
        logx::info("keep: TLD saved before we read what's kept with this game; not written this time");
        return;
    }
    std::vector<Record> records;
    world_items::save_kept(records);
    world_objects::save_kept(records);
    world_fires::save_kept(records);
    world_containers::save_kept(records);
    std::string data;
    put_u32(data, kFileMagic);
    put_u32(data, kFileVersion);
    put_u32(data, static_cast<uint32_t>(records.size()));
    for (const Record& r : records) {
        data += static_cast<char>(r.tag);
        put_u32(data, static_cast<uint32_t>(r.msg.size()));
        data.append(reinterpret_cast<const char*>(r.msg.data()), r.msg.size());
    }
    const auto path = file_for(id);
    auto tmp = path;
    tmp += L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            logx::warn("keep: couldn't write %s", tmp.string().c_str());
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        logx::warn("keep: couldn't replace %s (%s)", path.string().c_str(), ec.message().c_str());
        return;
    }
    logx::info("keep: saved with our game: %zu change(s) kept for scenes we weren't in (%s)", records.size(),
               path.filename().string().c_str());
}

void load() {
    const std::string id = game::save_id();
    if (id.empty()) return;  // not yet: next frame
    g_loaded = true;
    const auto path = file_for(id);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        logx::info("keep: nothing kept with this game (%s)", id.c_str());
        return;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t at = 0;
    uint32_t magic = 0, version = 0, count = 0;
    if (!get_u32(data, at, magic) || !get_u32(data, at, version) || !get_u32(data, at, count) || magic != kFileMagic ||
        version != kFileVersion) {
        logx::warn("keep: %s isn't ours (or another version); ignored", path.string().c_str());
        return;
    }
    size_t loaded = 0, bad = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        if (at >= data.size()) break;
        Record r;
        r.tag = static_cast<Tag>(static_cast<uint8_t>(data[at++]));
        if (!get_u32(data, at, size) || at + size > data.size()) break;
        r.msg.assign(data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(at + size));
        at += size;
        bool ok = false;
        switch (r.tag) {
            case Tag::ItemChange: ok = world_items::load_kept(r); break;
            case Tag::ObjectChange:
            case Tag::ObjectGone: ok = world_objects::load_kept(r); break;
            case Tag::FireChange: ok = world_fires::load_kept(r); break;
            case Tag::ContainerTheirs:
            case Tag::ContainerOurs: ok = world_containers::load_kept(r); break;
        }
        ++(ok ? loaded : bad);
    }
    logx::info("keep: loaded %zu change(s) kept with this game for scenes we weren't in (%s)%s", loaded,
               path.filename().string().c_str(), bad || loaded < count ? "; some were unreadable" : "");
}

using SaveGlobalFn = void (*)(Il2CppObject* slot, const MethodInfo* method);
SaveGlobalFn g_save_global = nullptr;

void save_global_detour(Il2CppObject* slot, const MethodInfo* method) {
    g_save_global(slot, method);
    save();
}

}  // namespace

bool init() {
    const MethodInfo* save_global = il2::find_method(il2::find_class(kGame, "", "SaveGameSystem"), "SaveGlobalData", 1);
    g_ready = save_global && hooks::install_method("SaveGameSystem.SaveGlobalData", save_global,
                                                   reinterpret_cast<void*>(&save_global_detour),
                                                   reinterpret_cast<void**>(&g_save_global));
    if (!g_ready)
        logx::warn("world keep: UNAVAILABLE (TLD API missing): the friend's changes kept for later end with the session");
    else
        logx::info("world keep: ready (the friend's changes kept for later go with the host's save)");
    return g_ready;
}

void on_frame(const std::string& scene, bool in_game) {
    if (!g_ready) return;
    if (scene == "MainMenu") {
        g_loaded = false;
        return;
    }
    if (!g_loaded && in_game && hosting()) load();
}

}  // namespace world_keep
