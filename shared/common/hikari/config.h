#pragma once
// INI-style settings persisted under %LOCALAPPDATA%\Hikari.
//
// Users never need to edit these: every value has an automatic default and
// the apps write back what they detect (chosen capture mode, benchmarked
// encoder, ESP32 port, paired hosts) so the next start is instant.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hikari {

class Config {
public:
    bool load(const std::string& path);
    bool save() const;
    bool save_as(const std::string& path) const;
    const std::string& path() const { return path_; }

    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    int64_t get_int(const std::string& section, const std::string& key, int64_t def) const;
    double get_double(const std::string& section, const std::string& key, double def) const;
    bool get_bool(const std::string& section, const std::string& key, bool def) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    void set_int(const std::string& section, const std::string& key, int64_t value);
    void set_bool(const std::string& section, const std::string& key, bool value);

    bool has_section(const std::string& section) const { return data_.count(section) != 0; }
    std::vector<std::string> sections_with_prefix(const std::string& prefix) const;
    void remove_section(const std::string& section) { data_.erase(section); }

    // Applies "--section.key=value" / "--key=value" (section "general")
    // command-line overrides without persisting them.
    void apply_overrides(const std::vector<std::string>& args);

private:
    std::string path_;
    std::map<std::string, std::map<std::string, std::string>> data_;
};

}  // namespace hikari
