#include "pack.hpp"
#include "compile.hpp"

#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace vgxc {

// ── Minimal INI parser (same pattern as fx/effect_desc.cpp) ──────────────────

using SectionMap = std::map<std::string, std::string>;
using IniMap     = std::map<std::string, SectionMap>;

static std::string trim(std::string_view sv) {
    auto a = sv.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    auto b = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(a, b - a + 1));
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static IniMap parse_ini(const fs::path& p) {
    IniMap out;
    std::ifstream f(p);
    if (!f.is_open()) return out;
    std::string section, line;
    while (std::getline(f, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') { section = t.substr(1, t.size()-2); continue; }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        out[section][trim(to_lower(t.substr(0, eq)))] = trim(t.substr(eq+1));
    }
    return out;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

static std::string json_str(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else r += c;
    }
    return r + "\"";
}

// ── Write bytes to file ───────────────────────────────────────────────────────

static bool write_binary(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return f.good();
}

static bool write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p);
    if (!f.is_open()) return false;
    f << text;
    return f.good();
}

// ── pack_effect ───────────────────────────────────────────────────────────────

PackResult pack_effect(const fs::path& effect_folder,
                        const fs::path& out_dir,
                        const PackOptions& opts) {
    PackResult res;

    if (!fs::exists(effect_folder / "effect.ini")) {
        res.ok = false;
        res.errors.push_back("effect.ini not found in " + effect_folder.string());
        return res;
    }

    auto ini = parse_ini(effect_folder / "effect.ini");

    // Collect pass sections: "Pass <name>" or "pass <name>"
    struct PassEntry {
        std::string name;
        std::string ps_file;
        std::string vs_file;
    };

    std::vector<PassEntry> passes;
    for (auto& [sec, kv] : ini) {
        bool is_pass = (sec.size() > 5) &&
                       (sec.rfind("Pass ", 0) == 0 || sec.rfind("pass ", 0) == 0);
        if (!is_pass) continue;
        PassEntry pe;
        pe.name    = sec.substr(5);
        pe.ps_file = kv.count("ps") ? kv.at("ps") : "";
        pe.vs_file = kv.count("vs") ? kv.at("vs") : "";
        passes.push_back(std::move(pe));
    }

    if (passes.empty()) {
        res.errors.push_back("no [Pass ...] sections in effect.ini");
        // Not fatal — still generate reflect.json
    }

    if (!opts.check_only)
        fs::create_directories(out_dir);

    // Build JSON buffer
    std::ostringstream json;
    json << "{\n";
    json << "  \"vgxc_version\": 1,\n";

    // Effect name
    std::string effect_name = effect_folder.filename().string();
    if (auto it_eff = ini.find("Effect"); it_eff == ini.end())
        it_eff = ini.find("effect");
    // just use folder name as effect name regardless
    json << "  \"effect\": " << json_str(effect_name) << ",\n";
    json << "  \"passes\": [\n";

    struct ShaderFile {
        std::string pass_name;
        std::string hlsl_file;
        std::string stage;    // "ps" or "vs"
    };

    std::vector<ShaderFile> shader_files;
    for (auto& pe : passes) {
        if (!pe.ps_file.empty()) shader_files.push_back({pe.name, pe.ps_file, "ps"});
        if (!pe.vs_file.empty()) shader_files.push_back({pe.name, pe.vs_file, "vs"});
    }

    for (size_t pi = 0; pi < passes.size(); ++pi) {
        auto& pe = passes[pi];
        json << "    {\n";
        json << "      \"name\": " << json_str(pe.name) << ",\n";
        if (!pe.ps_file.empty()) json << "      \"ps_file\": " << json_str(pe.ps_file) << ",\n";
        if (!pe.vs_file.empty()) json << "      \"vs_file\": " << json_str(pe.vs_file) << ",\n";
        json << "      \"bindings\": [";

        // Compile PS for reflection (DXBC is simplest)
        bool first_binding = true;
        if (!pe.ps_file.empty()) {
            auto ps_path = effect_folder / pe.ps_file;
            std::ifstream f(ps_path);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                CompileInput ci;
                ci.hlsl        = ss.str();
                ci.entry       = "main";
                ci.profile     = "ps_5_0";
                ci.source_name = pe.ps_file;

                auto dxbc_res = compile(ci, Target::DXBC);
                if (dxbc_res.ok) {
                    std::vector<Binding> bindings;
                    std::string refl_err;
                    reflect_dxbc(dxbc_res.bytecode, bindings, refl_err);
                    for (auto& b : bindings) {
                        if (!first_binding) json << ",";
                        first_binding = false;
                        json << "\n        {"
                             << " \"name\": " << json_str(b.name)
                             << ", \"type\": " << json_str(b.type)
                             << ", \"register\": " << json_str(b.reg_prefix + std::to_string(b.reg))
                             << " }";
                    }
                }
            }
        }
        if (!first_binding) json << "\n      ";
        json << "]\n";
        json << "    }";
        if (pi + 1 < passes.size()) json << ",";
        json << "\n";
    }

    json << "  ]\n";
    json << "}\n";

    if (!opts.check_only) {
        auto reflect_path = out_dir / "reflect.json";
        if (!write_text(reflect_path, json.str())) {
            res.errors.push_back("failed to write reflect.json");
            res.ok = false;
        } else {
            res.outputs.push_back("reflect.json");
        }
    }

    // Compile each shader file to all requested targets
    for (auto& sf : shader_files) {
        auto hlsl_path = effect_folder / sf.hlsl_file;
        std::ifstream f(hlsl_path);
        if (!f.is_open()) {
            res.errors.push_back("HLSL file not found: " + sf.hlsl_file);
            res.ok = false;
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        CompileInput ci;
        ci.hlsl        = ss.str();
        ci.entry       = "main";
        ci.source_name = sf.hlsl_file;
        ci.profile     = (sf.stage == "vs") ? "vs_5_0" : "ps_5_0";

        // Stem: e.g., "main.ps"
        fs::path stem = fs::path(sf.hlsl_file).stem();
        std::string base_name = stem.string() + "." + sf.stage;

        struct TargetSpec { Target target; bool enabled; const char* ext; };
        TargetSpec targets[] = {
            { Target::DXBC,  opts.dxbc,  ".dxbc" },
            { Target::DXIL,  opts.dxil,  ".dxil" },
            { Target::SPIRV, opts.spirv, ".spv"  },
            { Target::GLSL,  opts.glsl,  ".glsl" },
            { Target::ESSL,  opts.essl,  ".essl" },
            { Target::MSL,   opts.msl,   ".msl"  },
        };

        // For DXIL/SPIR-V we need SM 6 profile
        for (auto& ts : targets) {
            if (!ts.enabled) continue;

            CompileInput ci_t = ci;
            if (ts.target == Target::DXIL || ts.target == Target::SPIRV) {
                ci_t.profile = (sf.stage == "vs") ? "vs_6_0" : "ps_6_0";
            }

            auto out_file = base_name + ts.ext;
            if (opts.verbose) {
                // (logging handled by caller)
            }

            auto cr = compile(ci_t, ts.target);
            if (!cr.ok) {
                bool is_optional = (ts.target == Target::GLSL ||
                                    ts.target == Target::ESSL ||
                                    ts.target == Target::MSL);
                std::string msg = "compile " + out_file + ": " + cr.error;
                if (is_optional) {
                    // Don't fail the pack for optional targets
                    res.errors.push_back("[warn] " + msg);
                } else {
                    res.errors.push_back(msg);
                    res.ok = false;
                }
                continue;
            }

            if (!opts.check_only) {
                auto out_path = out_dir / out_file;
                bool wrote = cr.text.empty()
                    ? write_binary(out_path, cr.bytecode)
                    : write_text(out_path, cr.text);
                if (!wrote) {
                    res.errors.push_back("failed to write " + out_file);
                    res.ok = false;
                } else {
                    res.outputs.push_back(out_file);
                }
            }
        }
    }

    return res;
}

// ── parse_targets ─────────────────────────────────────────────────────────────

bool parse_targets(const std::string& csv, PackOptions& opts, std::string& error) {
    // Reset all targets
    opts.dxbc = opts.dxil = opts.spirv = false;
    opts.glsl = opts.essl = opts.msl   = false;

    std::istringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto t = trim(to_lower(tok));
        if      (t == "dxbc")  opts.dxbc  = true;
        else if (t == "dxil")  opts.dxil  = true;
        else if (t == "spirv" || t == "spv") opts.spirv = true;
        else if (t == "glsl")  opts.glsl  = true;
        else if (t == "essl")  opts.essl  = true;
        else if (t == "msl")   opts.msl   = true;
        else if (t == "all") {
            opts.dxbc = opts.dxil = opts.spirv = true;
            opts.glsl = opts.essl = opts.msl   = true;
        } else {
            error = "unknown target: " + t;
            return false;
        }
    }
    return true;
}

}  // namespace vgxc
