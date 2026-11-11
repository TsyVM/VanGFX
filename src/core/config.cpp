#include <vangfx/core/config.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>

namespace vangfx {

// ── Internal storage ──────────────────────────────────────────────────────────

struct Config::Impl {
    // section -> (key -> value), all lowercased
    std::map<std::string, std::map<std::string, std::string>> data;
};

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    return s;
}

// ── Config impl ───────────────────────────────────────────────────────────────

bool Config::load(const std::filesystem::path& ini_path) {
    std::ifstream f(ini_path);
    if (!f.is_open()) return true; // absent is fine

    if (!impl_) impl_ = new Impl;

    std::string line;
    std::string cur_section;
    while (std::getline(f, line)) {
        std::string_view sv = trim(line);
        if (sv.empty() || sv[0] == ';' || sv[0] == '#') continue;
        if (sv[0] == '[') {
            auto end = sv.find(']');
            if (end == std::string_view::npos) continue;
            cur_section = to_lower(std::string(trim(sv.substr(1, end - 1))));
        } else {
            auto eq = sv.find('=');
            if (eq == std::string_view::npos) continue;
            auto k = to_lower(std::string(trim(sv.substr(0, eq))));
            auto v = std::string(trim(sv.substr(eq + 1)));
            impl_->data[cur_section][k] = std::move(v);
        }
    }
    return true;
}

std::string Config::env_key(std::string_view section, std::string_view key) const {
    std::string out = "VANGFX_";
    for (auto c : section) out += static_cast<char>(std::isalnum(static_cast<unsigned char>(c)) ? std::toupper(static_cast<unsigned char>(c)) : '_');
    out += '_';
    for (auto c : key)     out += static_cast<char>(std::isalnum(static_cast<unsigned char>(c)) ? std::toupper(static_cast<unsigned char>(c)) : '_');
    return out;
}

std::optional<std::string> Config::get_opt(std::string_view section,
                                             std::string_view key) const {
    // 1. Environment variable
    std::string env = env_key(section, key);
    char buf[4096];
    if (GetEnvironmentVariableA(env.c_str(), buf, sizeof(buf)) > 0)
        return std::string(buf);

    // 2. INI
    if (!impl_) return std::nullopt;
    auto s = impl_->data.find(to_lower(std::string(section)));
    if (s == impl_->data.end()) return std::nullopt;
    auto k = s->second.find(to_lower(std::string(key)));
    if (k == s->second.end()) return std::nullopt;
    return k->second;
}

std::string Config::get(std::string_view section, std::string_view key,
                         std::string_view default_value) const {
    if (auto v = get_opt(section, key)) return *v;
    return std::string(default_value);
}

int Config::get_int(std::string_view section, std::string_view key,
                    int default_value) const {
    auto v = get_opt(section, key);
    if (!v) return default_value;
    int result = default_value;
    std::from_chars(v->data(), v->data() + v->size(), result);
    return result;
}

bool Config::get_bool(std::string_view section, std::string_view key,
                      bool default_value) const {
    auto v = get_opt(section, key);
    if (!v) return default_value;
    std::string lv = to_lower(*v);
    if (lv == "1" || lv == "true" || lv == "yes" || lv == "on") return true;
    if (lv == "0" || lv == "false"|| lv == "no"  || lv == "off") return false;
    return default_value;
}

void Config::set(std::string_view section, std::string_view key,
                 std::string_view value) {
    if (!impl_) impl_ = new Impl;
    impl_->data[to_lower(std::string(section))][to_lower(std::string(key))] =
        std::string(value);
}

bool Config::save(const std::filesystem::path& ini_path) const {
    if (!impl_) return true;
    std::ofstream f(ini_path);
    if (!f.is_open()) return false;
    for (auto& [sec, keys] : impl_->data) {
        f << '[' << sec << "]\n";
        for (auto& [k, v] : keys) f << k << '=' << v << '\n';
        f << '\n';
    }
    return true;
}

bool Config::empty() const noexcept {
    return !impl_ || impl_->data.empty();
}

} // namespace vangfx
