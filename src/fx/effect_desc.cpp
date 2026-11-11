#include <vangfx/fx/effect_desc.hpp>
#include <fstream>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace vangfx::fx {

using SectionMap = std::map<std::string, std::string>;
using IniMap     = std::map<std::string, SectionMap>;

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string trim(std::string_view sv) {
    auto a = sv.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    auto b = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(a, b - a + 1));
}

static IniMap parse_ini(const std::filesystem::path& p) {
    IniMap out;
    std::ifstream f(p);
    if (!f.is_open()) return out;

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(to_lower(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        out[section][key] = std::move(val);
    }
    return out;
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto t = trim(tok);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

bool EffectDesc::load(const std::filesystem::path& effect_folder) {
    folder = effect_folder;
    auto ini = parse_ini(effect_folder / "effect.ini");

    auto it_eff = ini.find("Effect");
    if (it_eff == ini.end()) it_eff = ini.find("effect");
    if (it_eff == ini.end()) return false;

    SectionMap& eff = it_eff->second;
    name        = eff.count("name")        ? eff["name"]        : effect_folder.filename().string();
    description = eff.count("description") ? eff["description"] : "";
    order       = eff.count("order")       ? std::stoi(eff["order"]) : 0;

    for (auto& [sec, kv] : ini) {
        bool is_pass = (sec.size() > 5) &&
                       (sec.rfind("Pass ", 0) == 0 || sec.rfind("pass ", 0) == 0);
        if (!is_pass) continue;

        PassDesc pd;
        pd.name    = trim(sec.substr(5));
        pd.ps_file = kv.count("ps") ? kv.at("ps") : "";
        pd.vs_file = kv.count("vs") ? kv.at("vs") : "";
        if (kv.count("inputs"))
            pd.inputs = split_csv(kv.at("inputs"));
        pd.width  = kv.count("width")  ? static_cast<uint32_t>(std::stoul(kv.at("width")))  : 0;
        pd.height = kv.count("height") ? static_cast<uint32_t>(std::stoul(kv.at("height"))) : 0;
        passes.push_back(std::move(pd));
    }

    uint32_t idx = 0;
    auto it_params = ini.find("Params");
    if (it_params == ini.end()) it_params = ini.find("params");
    if (it_params != ini.end()) {
        for (auto& [k, v] : it_params->second) {
            if (idx >= 8) break;
            Param p;
            p.name  = k;
            p.index = idx++;
            try { p.value = std::stof(v); } catch (...) { p.value = 0.0f; }
            float4[p.index][0] = p.value;
            params.push_back(p);
        }
    }

    auto user_ini = parse_ini(effect_folder / "user.ini");
    auto it_up = user_ini.find("Params");
    if (it_up == user_ini.end()) it_up = user_ini.find("params");
    if (it_up != user_ini.end()) {
        for (auto& p : params) {
            auto kit = it_up->second.find(to_lower(p.name));
            if (kit != it_up->second.end()) {
                try { p.value = std::stof(kit->second); } catch (...) {}
                float4[p.index][0] = p.value;
            }
        }
    }

    return true;
}

bool EffectDesc::save_user_params() const {
    std::ofstream f(folder / "user.ini");
    if (!f.is_open()) return false;
    f << "[Params]\n";
    for (const auto& p : params)
        f << p.name << "=" << p.value << "\n";
    return true;
}

} // namespace vangfx::fx
