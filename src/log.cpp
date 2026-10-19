#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace logx {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

void write(const char* level, const char* fmt, va_list args) {
    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    fprintf(g_file, "[%02u:%02u:%02u.%03u] [%s] [t%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            level, GetCurrentThreadId(), message);
    fflush(g_file);  // keep the log useful if the game crashes
}

}  // namespace

void open(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
}

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("info", fmt, args);
    va_end(args);
}

void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("WARN", fmt, args);
    va_end(args);
}

void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("ERROR", fmt, args);
    va_end(args);
}

}  // namespace logx
