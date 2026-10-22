#include "hikari/log.h"

#include <cstdio>
#include <ctime>
#include <mutex>

#include "hikari/system.h"

namespace hikari::log {

namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
Level g_level = Level::Info;
std::string g_dir;
std::string g_path;

const char* tag(Level l)
{
    switch (l) {
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warn: return "W";
    case Level::Error: return "E";
    }
    return "?";
}
}  // namespace

void init(const char* role)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    g_dir = app_data_dir() + "\\logs";
    ensure_directory(g_dir);
    g_path = g_dir + "\\" + role + ".log";
    // Keep one previous log for "it broke last time" reports.
    const std::string prev = g_dir + "\\" + role + ".prev.log";
    std::remove(prev.c_str());
    std::rename(g_path.c_str(), prev.c_str());
    g_file = std::fopen(g_path.c_str(), "w");
}

void set_level(Level l) { g_level = l; }
std::string log_directory() { return g_dir; }
std::string log_path() { return g_path; }

void write(Level l, const char* fmt, ...)
{
    if (l < g_level) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "%s %s %s\n", ts, tag(l), msg);
    if (g_file) {
        std::fprintf(g_file, "%s %s %s\n", ts, tag(l), msg);
        std::fflush(g_file);
    }
}

}  // namespace hikari::log
