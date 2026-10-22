#pragma once
// Small helpers shared by the measurement tools: argument parsing, aligned
// console tables (also saved as CSV), a minimal JSON writer and process CPU
// accounting. Header-only so every tool stays a single translation unit.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "hikari/log.h"

namespace hikari::tool {

inline std::string strf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

inline std::string strf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

inline std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string upper(std::string s)
{
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();
        std::string part = s.substr(start, end - start);
        while (!part.empty() && isspace(static_cast<unsigned char>(part.front()))) part.erase(part.begin());
        while (!part.empty() && isspace(static_cast<unsigned char>(part.back()))) part.pop_back();
        if (!part.empty()) out.push_back(part);
        start = end + 1;
    }
    return out;
}

inline bool parse_double(const std::string& s, double& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

// Console setup common to all tools: UTF-8 output (device names), and the
// library logger kept at warnings so tables stay readable (--verbose = info).
inline void init_console(bool verbose)
{
    SetConsoleOutputCP(CP_UTF8);
    log::set_level(verbose ? log::Level::Info : log::Level::Warn);
}

// "--key value" options and "--flag" switches; anything else is positional.
class Args {
public:
    // `options` take a value, `flags` do not. Unknown "--x" is an error.
    Args(int argc, char** argv, const std::set<std::string>& options, const std::set<std::string>& flags)
    {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-h" || a == "/?") a = "--help";
            if (a.size() > 2 && a.compare(0, 2, "--") == 0) {
                std::string key = a.substr(2);
                std::string val;
                const auto eq = key.find('=');
                if (eq != std::string::npos) {
                    val = key.substr(eq + 1);
                    key = key.substr(0, eq);
                    if (!options.count(key)) {
                        error_ = "unknown option --" + key;
                        continue;
                    }
                    values_[key] = val;
                } else if (flags.count(key) || key == "help") {
                    values_[key] = "1";
                } else if (options.count(key)) {
                    if (i + 1 >= argc) {
                        error_ = "--" + key + " needs a value";
                        continue;
                    }
                    values_[key] = argv[++i];
                } else {
                    error_ = "unknown option --" + key;
                }
            } else {
                positional_.push_back(a);
            }
        }
    }

    bool has(const std::string& k) const { return values_.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = {}) const
    {
        const auto it = values_.find(k);
        return it == values_.end() ? def : it->second;
    }
    double num(const std::string& k, double def) const
    {
        const auto it = values_.find(k);
        if (it == values_.end()) return def;
        double v = 0;
        if (!parse_double(it->second, v)) {
            bad_number_ = "--" + k + " expects a number, got '" + it->second + "'";
            return def;
        }
        return v;
    }
    const std::vector<std::string>& positional() const { return positional_; }
    // Error from parsing or from a malformed number read so far.
    std::string error() const { return !error_.empty() ? error_ : bad_number_; }

private:
    std::map<std::string, std::string> values_;
    std::vector<std::string> positional_;
    std::string error_;
    mutable std::string bad_number_;
};

// Column-aligned console table; numeric-looking cells are right-aligned.
class Table {
public:
    explicit Table(std::vector<std::string> headers) : headers_(std::move(headers)) {}

    void row(std::vector<std::string> cells)
    {
        cells.resize(headers_.size());
        rows_.push_back(std::move(cells));
    }
    bool empty() const { return rows_.empty(); }

    void print(FILE* f = stdout, int indent = 2) const
    {
        std::vector<size_t> w(headers_.size());
        for (size_t c = 0; c < headers_.size(); ++c) {
            w[c] = display_width(headers_[c]);
            for (const auto& r : rows_) w[c] = std::max(w[c], display_width(r[c]));
        }
        auto line = [&](const std::vector<std::string>& cells, bool header) {
            std::string out(static_cast<size_t>(indent), ' ');
            for (size_t c = 0; c < cells.size(); ++c) {
                const size_t pad = w[c] - display_width(cells[c]);
                const bool right = !header && numeric(cells[c]);
                if (right) out.append(pad, ' ');
                out += cells[c];
                if (!right && c + 1 < cells.size()) out.append(pad, ' ');
                if (c + 1 < cells.size()) out += "  ";
            }
            std::fprintf(f, "%s\n", out.c_str());
        };
        line(headers_, true);
        std::string rule(static_cast<size_t>(indent), ' ');
        for (size_t c = 0; c < w.size(); ++c) {
            rule.append(w[c], '-');
            if (c + 1 < w.size()) rule += "  ";
        }
        std::fprintf(f, "%s\n", rule.c_str());
        for (const auto& r : rows_) line(r, false);
    }

    bool save_csv(const std::string& path) const
    {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        write_csv_row(f, headers_);
        for (const auto& r : rows_) write_csv_row(f, r);
        std::fclose(f);
        return true;
    }

    static void write_csv_row(FILE* f, const std::vector<std::string>& cells)
    {
        for (size_t c = 0; c < cells.size(); ++c) {
            const std::string& v = cells[c];
            if (v.find_first_of(",\"\n") != std::string::npos) {
                std::string q = "\"";
                for (char ch : v) {
                    if (ch == '"') q += '"';
                    q += ch;
                }
                q += '"';
                std::fputs(q.c_str(), f);
            } else {
                std::fputs(v.c_str(), f);
            }
            std::fputc(c + 1 < cells.size() ? ',' : '\n', f);
        }
    }

private:
    // Counts UTF-8 code points so non-ASCII device names stay aligned.
    static size_t display_width(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char ch : s)
            if ((ch & 0xC0) != 0x80) ++n;
        return n;
    }
    static bool numeric(const std::string& s)
    {
        // Plain numbers only ("60.2", "-3", "5.4%"); "40 +- 2 ms" stays left.
        bool digit = false;
        for (unsigned char c : s) {
            if (isdigit(c))
                digit = true;
            else if (c != '.' && c != '-' && c != '+' && c != '%')
                return false;
        }
        return digit;
    }

    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;
};

// Minimal streaming JSON writer (pretty-printed, keys in insertion order).
class JsonWriter {
public:
    JsonWriter& begin_object(const char* key = nullptr) { return open(key, '{'); }
    JsonWriter& end_object() { return close('}'); }
    JsonWriter& begin_array(const char* key = nullptr) { return open(key, '['); }
    JsonWriter& end_array() { return close(']'); }

    JsonWriter& value(const char* key, const std::string& v)
    {
        prefix(key);
        out_ += quote(v);
        return *this;
    }
    JsonWriter& value(const char* key, const char* v) { return value(key, std::string(v ? v : "")); }
    JsonWriter& value(const char* key, bool v)
    {
        prefix(key);
        out_ += v ? "true" : "false";
        return *this;
    }
    JsonWriter& value(const char* key, double v)
    {
        prefix(key);
        out_ += std::isfinite(v) ? strf("%.6g", v) : std::string("null");
        return *this;
    }
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    JsonWriter& value(const char* key, T v)
    {
        prefix(key);
        if constexpr (std::is_signed_v<T>)
            out_ += std::to_string(static_cast<long long>(v));
        else
            out_ += std::to_string(static_cast<unsigned long long>(v));
        return *this;
    }

    const std::string& str() const { return out_; }
    bool save(const std::string& path) const
    {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        std::fputs(out_.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
        return true;
    }

    static std::string quote(const std::string& s)
    {
        std::string q = "\"";
        for (unsigned char c : s) {
            switch (c) {
            case '"': q += "\\\""; break;
            case '\\': q += "\\\\"; break;
            case '\n': q += "\\n"; break;
            case '\r': q += "\\r"; break;
            case '\t': q += "\\t"; break;
            default:
                if (c < 0x20)
                    q += strf("\\u%04x", c);
                else
                    q += static_cast<char>(c);
            }
        }
        return q + "\"";
    }

private:
    JsonWriter& open(const char* key, char brace)
    {
        prefix(key);
        out_ += brace;
        first_.push_back(true);
        return *this;
    }
    JsonWriter& close(char brace)
    {
        const bool was_empty = first_.empty() || first_.back();
        if (!first_.empty()) first_.pop_back();
        if (!was_empty) {
            out_ += '\n';
            out_.append(first_.size() * 2, ' ');
        }
        out_ += brace;
        return *this;
    }
    void prefix(const char* key)
    {
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
            out_ += '\n';
            out_.append(first_.size() * 2, ' ');
        }
        if (key) {
            out_ += quote(key);
            out_ += ": ";
        }
    }

    std::string out_;
    std::vector<bool> first_;
};

// Process CPU time (user + kernel) for "% CPU during this run" figures.
class CpuMeter {
public:
    void start()
    {
        cpu0_ = process_cpu_100ns();
        wall0_ = wall_100ns();
    }
    // Percent of the whole machine (all logical processors = 100%).
    double machine_percent() const { return one_core_percent() / logical_cpus(); }
    // Percent of a single core (can exceed 100 when several threads are busy).
    double one_core_percent() const
    {
        const double wall = static_cast<double>(wall_100ns() - wall0_);
        return wall > 0 ? 100.0 * static_cast<double>(process_cpu_100ns() - cpu0_) / wall : 0.0;
    }
    static int logical_cpus()
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwNumberOfProcessors ? static_cast<int>(si.dwNumberOfProcessors) : 1;
    }

private:
    static uint64_t to_u64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
    static uint64_t process_cpu_100ns()
    {
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
        return to_u64(k) + to_u64(u);
    }
    static uint64_t wall_100ns()
    {
        FILETIME f;
        GetSystemTimePreciseAsFileTime(&f);
        return to_u64(f);
    }
    uint64_t cpu0_ = 0;
    uint64_t wall0_ = 0;
};

inline std::string ms(uint32_t us) { return strf("%.2f", us / 1000.0); }
inline std::string ms(double us) { return strf("%.2f", us / 1000.0); }

}  // namespace hikari::tool
