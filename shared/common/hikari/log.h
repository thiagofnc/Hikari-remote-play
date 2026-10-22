#pragma once
// Tiny thread-safe logger. Writes to stderr (when a console is attached) and
// to %LOCALAPPDATA%\Hikari\logs\<role>.log so "Export logs" can bundle them.
// Never call from the hottest paths (per-packet); stats are aggregated
// instead and logged once per second.

#include <cstdarg>
#include <string>

namespace hikari::log {

enum class Level { Debug, Info, Warn, Error };

void init(const char* role);
void set_level(Level l);
void write(Level l, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
std::string log_directory();
std::string log_path();

}  // namespace hikari::log

#define HK_LOGD(...) ::hikari::log::write(::hikari::log::Level::Debug, __VA_ARGS__)
#define HK_LOGI(...) ::hikari::log::write(::hikari::log::Level::Info, __VA_ARGS__)
#define HK_LOGW(...) ::hikari::log::write(::hikari::log::Level::Warn, __VA_ARGS__)
#define HK_LOGE(...) ::hikari::log::write(::hikari::log::Level::Error, __VA_ARGS__)
