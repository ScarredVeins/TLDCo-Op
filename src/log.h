#pragma once
#include <filesystem>

namespace logx {

// Opens <mod dir>/coop.log (truncated each launch). Safe to call once from DllMain.
void open(const std::filesystem::path& file);

void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

}  // namespace logx
