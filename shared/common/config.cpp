#include "hikari/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hikari {

namespace {
std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

bool Config::load(const std::string& path)
{
    path_ = path;
    data_.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line, section = "general";
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        data_[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

bool Config::save() const { return save_as(path_); }

bool Config::save_as(const std::string& path) const
{
    // Write-then-rename so a crash never leaves a half-written config.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "# Hikari settings. Everything here is detected automatically;\n"
               "# edit only if you know you need to.\n";
        for (const auto& [section, kv] : data_) {
            out << "\n[" << section << "]\n";
            for (const auto& [k, v] : kv) out << k << " = " << v << "\n";
        }
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string Config::get(const std::string& section, const std::string& key, const std::string& def) const
{
    const auto s = data_.find(section);
    if (s == data_.end()) return def;
    const auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

int64_t Config::get_int(const std::string& section, const std::string& key, int64_t def) const
{
    const std::string v = get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    const long long r = std::strtoll(v.c_str(), &end, 0);
    return end && *end == 0 ? r : def;
}

double Config::get_double(const std::string& section, const std::string& key, double def) const
{
    const std::string v = get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    const double r = std::strtod(v.c_str(), &end);
    return end && *end == 0 ? r : def;
}

bool Config::get_bool(const std::string& section, const std::string& key, bool def) const
{
    const std::string v = get(section, key);
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void Config::set(const std::string& section, const std::string& key, const std::string& value)
{
    data_[section][key] = value;
}

void Config::set_int(const std::string& section, const std::string& key, int64_t value)
{
    set(section, key, std::to_string(value));
}

void Config::set_bool(const std::string& section, const std::string& key, bool value)
{
    set(section, key, value ? "true" : "false");
}

std::vector<std::string> Config::sections_with_prefix(const std::string& prefix) const
{
    std::vector<std::string> out;
    for (const auto& [s, kv] : data_)
        if (s.rfind(prefix, 0) == 0) out.push_back(s);
    return out;
}

void Config::apply_overrides(const std::vector<std::string>& args)
{
    for (const auto& a : args) {
        if (a.rfind("--", 0) != 0) continue;
        const auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string key = a.substr(2, eq - 2);
        std::string section = "general";
        const auto dot = key.find('.');
        if (dot != std::string::npos) {
            section = key.substr(0, dot);
            key = key.substr(dot + 1);
        }
        data_[section][key] = a.substr(eq + 1);
    }
}

}  // namespace hikari
