// Forge PC: settings files (ini) for plugins: ForgeApi config_*.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "forge_internal.h"

struct ForgeConfig {
    std::mutex lock;
    std::filesystem::path path;
    ForgePluginState* owner{};
    forge::IniMap file;    // what the file said when last read
    forge::IniMap extra;   // defaults handed out and values set, not in the file yet / changed
    std::set<std::pair<std::string, std::string>> dirty; // (section, key) set by config_set
    std::string scratch;   // config_string's return value for a default
    std::filesystem::file_time_type stamp{};
    bool stamp_ok{};
};

namespace forge {
namespace {

std::mutex g_lock;
std::vector<std::unique_ptr<ForgeConfig>> g_configs;

void Stamp(ForgeConfig& c) {
    std::error_code ec;
    c.stamp = std::filesystem::last_write_time(c.path, ec);
    c.stamp_ok = !ec;
}

/// The value as text: a hand-edited file wins over a default; a set value over the file.
const std::string* Find(ForgeConfig& c, const char* section, const char* key) {
    for (const IniMap* m : {&c.extra, &c.file}) {
        if (const auto s = m->find(section); s != m->end()) {
            if (const auto k = s->second.find(key); k != s->second.end()) {
                if (m == &c.extra && !c.dirty.count({section, key})) {
                    continue; // only a remembered default: the file is newer
                }
                return &k->second;
            }
        }
    }
    return nullptr;
}

/// Looks the key up; if there is none, remembers the default so config_save writes it.
std::string Lookup(ForgeConfig& c, const char* section, const char* key, const std::string& def) {
    if (const std::string* v = Find(c, section, key)) {
        return *v;
    }
    if (const auto s = c.extra.find(section); s == c.extra.end() ||
                                              s->second.find(key) == s->second.end()) {
        c.extra[section][key] = def;
    }
    return def;
}

bool IsTrue(const std::string& v) {
    return v == "1" || v == "true" || v == "on" || v == "yes" || v == "True" || v == "TRUE";
}

} // namespace

ForgeConfig* ConfigOpen(ForgePluginState* owner, const char* name) {
    if (!name || !*name || !owner) {
        return nullptr; // not called from a plugin, or no name
    }
    const std::filesystem::path rel = PathU8(name);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        Log(FORGE_LOG_ERROR, "config_open(\"%s\"): the name must be relative to the mod folder", name);
        return nullptr;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            Log(FORGE_LOG_ERROR, "config_open(\"%s\"): \"..\" is not allowed", name);
            return nullptr;
        }
    }
    auto c = std::make_unique<ForgeConfig>();
    c->owner = owner;
    c->path = PathU8(ModsDirPath()) / PathU8(owner->mod) / rel;
    c->file = ReadIni(c->path);
    Stamp(*c);
    ForgeConfig* raw = c.get();
    std::scoped_lock lk{g_lock};
    g_configs.push_back(std::move(c));
    return raw;
}

int32_t ConfigGetInt(ForgeConfig* c, const char* section, const char* key, int32_t def) {
    if (!c || !section || !key) {
        return def;
    }
    std::scoped_lock lk{c->lock};
    const std::string v = Lookup(*c, section, key, std::to_string(def));
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);
    return end == v.c_str() ? def : static_cast<int32_t>(n);
}

float ConfigGetFloat(ForgeConfig* c, const char* section, const char* key, float def) {
    if (!c || !section || !key) {
        return def;
    }
    std::scoped_lock lk{c->lock};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(def));
    const std::string v = Lookup(*c, section, key, buf);
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? def : f;
}

bool ConfigGetBool(ForgeConfig* c, const char* section, const char* key, bool def) {
    if (!c || !section || !key) {
        return def;
    }
    std::scoped_lock lk{c->lock};
    return IsTrue(Lookup(*c, section, key, def ? "true" : "false"));
}

const char* ConfigGetString(ForgeConfig* c, const char* section, const char* key, const char* def) {
    if (!c || !section || !key) {
        return def;
    }
    std::scoped_lock lk{c->lock};
    c->scratch = Lookup(*c, section, key, def ? def : "");
    return c->scratch.c_str();
}

bool ConfigSetValue(ForgeConfig* c, const char* section, const char* key, const char* value) {
    if (!c || !section || !key || !value) {
        return false;
    }
    std::scoped_lock lk{c->lock};
    c->extra[section][key] = value;
    c->dirty.insert({section, key});
    return true;
}

bool ConfigSave(ForgeConfig* c) {
    if (!c) {
        return false;
    }
    std::scoped_lock lk{c->lock};
    std::error_code ec;
    std::filesystem::create_directories(c->path.parent_path(), ec);
    bool ok = true;
    for (const auto& [section, keys] : c->extra) {
        for (const auto& [key, value] : keys) {
            const auto s = c->file.find(section);
            const bool in_file = s != c->file.end() && s->second.count(key);
            const bool set = c->dirty.count({section, key}) != 0;
            if (in_file && !set) {
                continue; // a default the file already has: the file's value stays
            }
            if (in_file && s->second.at(key) == value) {
                continue;
            }
            ok &= IniSet(c->path, section.c_str(), key.c_str(), value.c_str());
        }
    }
    c->file = ReadIni(c->path);
    c->extra.clear();
    c->dirty.clear();
    Stamp(*c);
    return ok;
}

bool ConfigPoll(ForgeConfig* c) {
    if (!c) {
        return false;
    }
    std::scoped_lock lk{c->lock};
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(c->path, ec);
    const bool exists = !ec;
    if (exists == c->stamp_ok && (!exists || t == c->stamp)) {
        return false;
    }
    c->file = ReadIni(c->path);
    c->dirty.clear(); // what was set but not saved is dropped; remembered defaults stay
    for (auto it = c->extra.begin(); it != c->extra.end();) {
        it = it->second.empty() ? c->extra.erase(it) : std::next(it);
    }
    Stamp(*c);
    return true;
}

void ConfigDropOwner(ForgePluginState* owner) {
    std::scoped_lock lk{g_lock};
    std::erase_if(g_configs, [&](const auto& c) { return c->owner == owner; });
}

} // namespace forge
