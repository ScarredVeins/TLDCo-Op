#include "paths.h"

#include <windows.h>

#include <cwctype>
#include <fstream>
#include <sstream>

namespace paths {

std::filesystem::path game_dir() {
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return std::filesystem::path(std::wstring(exe, n)).parent_path();
}

std::filesystem::path mod_dir() {
    auto dir = game_dir() / L"TLDCoop";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::wstring profile() {
    static const std::wstring name = [] {
        // tld.exe's arguments, split on spaces outside quotes; the word after "-tldcoop" names the profile.
        const std::wstring line = GetCommandLineW();
        std::wstring current, previous, found;
        bool quoted = false;
        auto finish = [&] {
            if (current.empty()) return;
            if (previous == L"-tldcoop") found = current;
            previous = current;
            current.clear();
        };
        for (wchar_t c : line) {
            if (c == L'"')
                quoted = !quoted;
            else if (!quoted && iswspace(c))
                finish();
            else
                current.push_back(c);
        }
        finish();
        // It becomes a folder name: letters, digits, '-' and '_' only.
        for (wchar_t c : found)
            if (!iswalnum(c) && c != L'-' && c != L'_') return std::wstring();
        return found;
    }();
    return name;
}

std::filesystem::path instance_dir() {
    if (profile().empty()) return mod_dir();
    auto dir = mod_dir() / profile();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string game_version() {
    std::ifstream in(game_dir() / L"tld_Data" / L"StreamingAssets" / L"version.txt", std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    return s;
}

}  // namespace paths
