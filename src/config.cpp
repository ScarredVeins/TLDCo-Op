#include "config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "log.h"
#include "paths.h"

namespace config {
namespace {

constexpr const char* kDefaultIni =
    "; TLDCoop settings. Restart the game after editing.\n"
    "[net]\n"
    "; Co-op starts when you pick CO-OP after NEW, RESUME or LOAD in the Survival menu; until then nothing goes online.\n"
    "; autostart=1 starts it at launch with the settings below instead (for testing).\n"
    "autostart=0\n"
    "\n"
    "; The role the CO-OP setup screen starts on (it saves your last choice here):\n"
    "; host = the other player joins you (your world, save, time and weather)\n"
    "; join = you join a host\n"
    "mode=off\n"
    "\n"
    "; steam = join codes through Steam (no IPs, no port forwarding); the co-op menu sets this up\n"
    "; local = UDP by IP, for testing on this PC (tldbot) or a LAN\n"
    "transport=steam\n"
    "\n"
    "; steam: code = your host code (your friend enters it); join_code = the code you were given\n"
    "code=\n"
    "join_code=\n"
    "\n"
    "; local only. bind_ip=0.0.0.0 to accept LAN players; host_ip is where 'join' connects.\n"
    "bind_ip=127.0.0.1\n"
    "host_ip=127.0.0.1\n"
    "port=27015\n"
    "\n"
    "name=Player\n"
    "\n"
    "; Hosting: each friend who joins asks first, and you LET IN or TURN AWAY (in the lobby, or while you play).\n"
    "; ask_to_join=0 lets anyone with your code straight in.\n"
    "ask_to_join=1\n"
    "\n"
    "; How the other player sees you (they need TLDCoop\\tldcoop_bodies.bundle for anything but capsule):\n"
    ";   capsule, mackenzie, mackenzie_indoor, astrid, astrid_outdoor, molly, jane, jane_outdoor,\n"
    ";   jeremiah, methuselah, heller, jace, mathis, terry, atwood, convict_c, convict_e, convict_f, convict_g,\n"
    ";   millworker, patron1, patron3, patron4, patron5\n"
    "body=capsule\n"
    "\n"
    "; Shaders for the other player's story body: own (default) or tld (the game's own; experimental)\n"
    "body_shader=own\n"
    "\n"
    "[struggle]\n"
    "; In co-op, how long TLD's weapon picker (fists, knife, hatchet...) stays up when an animal jumps you, as a\n"
    "; fraction of the game's own time: 0.5 = half (harder), 1 = unchanged\n"
    "picker_scale=0.5\n"
    "\n"
    "[hud]\n"
    "; Finding each other, each switch your own (the co-op setup screen sets these too):\n"
    "; names = friends' names over their heads, and an arrow at the screen's edge for those out of sight\n"
    "; radar = a small round map of the area around you in a corner, with your friends on it\n"
    "; chart = your map charts itself where you walk, as charcoal mapping does (your own map only)\n"
    "names=1\n"
    "radar=1\n"
    "chart=1\n";

std::string read(const std::filesystem::path& ini, const wchar_t* key, const std::string& fallback,
                 const wchar_t* section = L"net") {
    wchar_t value[256];
    std::wstring wide_fallback(fallback.begin(), fallback.end());
    GetPrivateProfileStringW(section, key, wide_fallback.c_str(), value, static_cast<DWORD>(std::size(value)),
                             ini.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

void save(const Settings& s) {
    const auto ini = paths::instance_dir() / L"coop.ini";
    auto put = [&](const wchar_t* key, const std::string& value) {
        int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(n > 0 ? n : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), n);
        WritePrivateProfileStringW(L"net", key, wide.c_str(), ini.c_str());
    };
    put(L"mode", s.mode);
    put(L"transport", s.transport);
    put(L"code", s.code);
    put(L"join_code", s.join_code);
    put(L"host_ip", s.host_ip);
    put(L"name", s.name);
    put(L"body", s.body);
    put(L"voice", s.voice);
    WritePrivateProfileStringW(L"hud", L"names", s.hud_names ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"hud", L"radar", s.hud_radar ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"hud", L"chart", s.hud_chart ? L"1" : L"0", ini.c_str());
    logx::info("settings saved: mode=%s transport=%s code=%s join_code=%s name=%s body=%s", s.mode.c_str(),
               s.transport.c_str(), s.code.c_str(), s.join_code.c_str(), s.name.c_str(), s.body.c_str());
}

Settings load() {
    const auto ini = paths::instance_dir() / L"coop.ini";
    if (!std::filesystem::exists(ini)) {
        std::ofstream(ini, std::ios::binary) << kDefaultIni;
        logx::info("wrote default %ls", ini.c_str());
    }

    Settings s;
    s.autostart = GetPrivateProfileIntW(L"net", L"autostart", 0, ini.c_str()) != 0;
    s.mode = read(ini, L"mode", s.mode);
    s.transport = read(ini, L"transport", s.transport);
    s.code = read(ini, L"code", s.code);
    s.join_code = read(ini, L"join_code", s.join_code);
    s.bind_ip = read(ini, L"bind_ip", s.bind_ip);
    s.host_ip = read(ini, L"host_ip", s.host_ip);
    s.port = static_cast<uint16_t>(GetPrivateProfileIntW(L"net", L"port", s.port, ini.c_str()));
    s.name = read(ini, L"name", s.name);
    s.body = read(ini, L"body", s.body);
    s.voice = read(ini, L"voice", s.voice);
    s.ask_to_join = GetPrivateProfileIntW(L"net", L"ask_to_join", 1, ini.c_str()) != 0;
    s.body_shader = read(ini, L"body_shader", s.body_shader);
    const std::string picker = read(ini, L"picker_scale", "", L"struggle");
    if (!picker.empty()) s.picker_scale = static_cast<float>(atof(picker.c_str()));
    s.hud_names = GetPrivateProfileIntW(L"hud", L"names", 1, ini.c_str()) != 0;
    s.hud_radar = GetPrivateProfileIntW(L"hud", L"radar", 1, ini.c_str()) != 0;
    s.hud_chart = GetPrivateProfileIntW(L"hud", L"chart", 1, ini.c_str()) != 0;
    s.test_resume = read(ini, L"resume", "", L"test");
    s.test_nosave = GetPrivateProfileIntW(L"test", L"nosave", 0, ini.c_str()) != 0;
    s.test_god = GetPrivateProfileIntW(L"test", L"god", 0, ini.c_str()) != 0;
    s.test_own_saves = read(ini, L"own_saves", "", L"test");
    s.test_lobby = GetPrivateProfileIntW(L"test", L"lobby", 0, ini.c_str()) != 0;
    s.test_window = read(ini, L"window", "", L"test");
    if (!s.test_resume.empty() || s.test_nosave || s.test_god || !s.test_own_saves.empty())
        logx::info("settings: TEST resume=%s nosave=%d god=%d own_saves=%s lobby=%d window=%s", s.test_resume.c_str(),
                   s.test_nosave ? 1 : 0, s.test_god ? 1 : 0, s.test_own_saves.c_str(), s.test_lobby ? 1 : 0,
                   s.test_window.c_str());
    logx::info("settings: mode=%s transport=%s code=%s join_code=%s bind=%s host=%s port=%u name=%s body=%s "
               "hud names=%d radar=%d chart=%d",
               s.mode.c_str(), s.transport.c_str(), s.code.c_str(), s.join_code.c_str(), s.bind_ip.c_str(),
               s.host_ip.c_str(), s.port, s.name.c_str(), s.body.c_str(), s.hud_names ? 1 : 0, s.hud_radar ? 1 : 0,
               s.hud_chart ? 1 : 0);
    return s;
}

}  // namespace config
