// Forge PC loader: plugins, hooks, MT Framework singletons. A port of MHGU Forge (Fexty, MIT).
// SPDX-License-Identifier: MIT
//
// Lives in forge.dll, loaded by the game exe's mod host (mod_host_api.h) from
// <game>/mods/<title id>/Forge/. Nothing of it is part of the exe.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "forge_internal.h"
#include "platform.h"

namespace {
/// Enters a plugin callback; false if the plugin is being unloaded (then don't call it).
bool Enter(ForgePluginState* s) {
    if (!s) {
        return true;
    }
    s->inflight.fetch_add(1, std::memory_order_acq_rel);
    if (s->dead.load(std::memory_order_acquire)) {
        s->inflight.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}
void Leave(ForgePluginState* s) {
    if (s) {
        s->inflight.fetch_sub(1, std::memory_order_acq_rel);
    }
}
} // namespace

namespace forge {

const ModHostApi* g_host{};

namespace {

using u8 = uint8_t;
using u32 = uint32_t;

constexpr const char* kMhguTitle = "0100770008DD8000";

// MHGU 1.4.0 main offsets, the ones Forge (Switch) uses.
constexpr u32 kSystemCtor = 0x887810; // cSystem::cSystem(this): every singleton system
constexpr u32 kSystemDtor = 0x88784C; // cSystem::~cSystem(this)
constexpr u32 kAppRun = 0xB8692C;     // sApp::run(this): systems exist, plugins load here
constexpr u32 kUpdate = 0x3DA8FC;     // per-frame update(main object); dt float at +0x68
constexpr u32 kUpdateDt = 0x68;
constexpr u32 kGetDtiSlot = 0x14;     // MtObject vtable: D1, D0, createUi, isEnable,
                                      // createProperty, getDti

std::string g_loader_dir;
std::string g_mods_dir;
std::atomic<int> g_level{FORGE_LOG_INFO};
std::mutex g_log_lock;
std::ofstream g_log_file;
std::mutex g_config_lock;
IniMap g_config; // forge.ini

// Per game thread: a call_original in progress (no hook / override / trace fires inside; belt
// and braces: the exe's guest calls never dispatch hooks anyway), the guest thread key of the
// running dispatch, and the override being run (so call_original can reach the next one down).
thread_local int t_bypass{0};
thread_local uint32_t t_thread_key{0};
struct OverrideChain {
    u32 target{};
    ForgeCpu entry{};                   // state at entry (copied for the next override down)
    std::array<ForgeHook*, 8> list{};   // outermost first
    size_t count{};
    size_t index{};                     // the override running now
};
thread_local OverrideChain* t_chain{nullptr};
std::atomic<u32> g_frame_count{0};

struct Module {
    std::string name;
    std::string alias;
    u32 base;
    u32 size;
};
std::vector<Module> g_modules;
u32 g_main{};

std::shared_mutex g_hooks_lock;
std::unordered_map<u32, std::vector<ForgeHook*>> g_hooks; // never freed: disable instead

struct Frame {
    u32 lr;
    u32 sp;
    std::array<u32, 6> args; // r0-r3, LR, SP at entry (post hooks see all six)
    std::vector<ForgeHook*> posts;
};
std::mutex g_frames_lock;
std::unordered_map<u32, std::vector<Frame>> g_frames; // per guest thread, innermost last

std::mutex g_single_lock;
std::vector<u32> g_pending;                       // constructed cSystems not resolved yet
std::unordered_map<u32, u32> g_by_id;             // DTI id -> object
std::unordered_map<u32, std::string> g_dti_names; // DTI -> class name (stable c_str)
int g_dti_layout{-1}; // offset of the name field: 4 (vptr first) or 0; -1 = not checked yet

struct Plugin {
    std::string file;             // mod folder / dll name, for logs
    std::string mod;              // mod folder name (the unit that is switched on / off)
    std::filesystem::path source; // <mod>/plugins/<name>.dll (never loaded itself: stays writable)
    std::filesystem::path live;   // the copy actually loaded: <name>.dll.<n>.live, same folder
    ForgePluginState* state{};
    plat::Lib handle{};
    const void* base{};           // load address (identifies its code, e.g. a hook's caller)
    ForgePluginInfo info{};
    void (*on_init)(){};
    void (*on_update)(float){};
    void (*on_unload)(){};
    PluginUi ui{};
};
std::mutex g_plugins_lock;
std::vector<Plugin> g_plugins;
std::atomic<bool> g_plugins_loaded{false};

// hot reload: which plugin a code address belongs to (hooks remember their plugin)
std::mutex g_owner_lock;
std::unordered_map<const void*, ForgePluginState*> g_owners; // module base -> plugin

std::atomic<u32> g_live_counter{0};
std::atomic<u32> g_ui_generation{0};

// watched plugin files (also ones that failed to load: a fixed build loads then)
struct Watch {
    std::filesystem::path source;
    std::filesystem::file_time_type time{};
    uintmax_t size{};
    std::filesystem::file_time_type seen_time{}; // last poll: wait until the build is done
    uintmax_t seen_size{};
};
std::mutex g_reload_lock;
std::vector<Watch> g_watch;
std::vector<std::filesystem::path> g_reload_requests;
struct Unloading {
    Plugin plugin;
    size_t index{};
    u32 frames{0};
    bool reload{true}; // false: a mod switched off, unload and leave it unloaded
};
std::vector<Unloading> g_unloading; // game thread only

// mods (folders): the menu lists all of them, switched off ones too
struct ModEntry {
    std::string name;
    std::filesystem::path dir;
    std::vector<std::filesystem::path> files; // plugin files
    bool has_romfs{};
    std::string note;
};
std::mutex g_mods_lock;
std::vector<ModEntry> g_mod_list;
std::map<std::string, std::string> g_mod_notes; // mod -> note for the menu (under g_mods_lock)
std::vector<std::pair<std::string, bool>> g_mod_requests; // under g_reload_lock: (mod, on)
std::atomic<bool> g_reset_req{false};
std::string g_reset_msg; // result of the last "Reset scripts" (under g_mods_lock)
std::atomic<bool> g_watch_stop{false};
std::thread g_watcher;

// --- config + logging -------------------------------------------------------------------------

std::string Trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r");
    const auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

IniMap ParseIni(const std::filesystem::path& path) {
    IniMap map;
    std::ifstream in(path);
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            map[section][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
        }
    }
    return map;
}

void LoadConfig() {
    IniMap map = ParseIni(PathU8(g_loader_dir + "/forge.ini"));
    std::scoped_lock lk{g_config_lock};
    g_config = std::move(map);
}

/// Rewrites one `key = value` of forge.ini in place (or adds it, and the section if needed):
/// comments and every other line stay as they are. Written to a temp file and renamed over.
bool WriteIniValue(const std::filesystem::path& path, const std::string& section,
                   const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
    }
    const std::string entry = key + " = " + value;
    bool in_section = false, section_found = false, done = false;
    size_t insert_at = lines.size();
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string t = Trim(lines[i]);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            if (in_section) {
                break; // next section: the key wasn't there
            }
            in_section = Trim(t.substr(1, t.size() - 2)) == section;
            section_found |= in_section;
            insert_at = i + 1;
            continue;
        }
        if (!in_section) {
            continue;
        }
        if (!t.empty() && t[0] != ';' && t[0] != '#') {
            insert_at = i + 1; // after the last real line of the section
            const auto eq = t.find('=');
            if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) {
                lines[i] = entry;
                done = true;
            }
        }
    }
    if (!done) {
        if (section_found) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), entry);
        } else {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.push_back("[" + section + "]");
            lines.push_back(entry);
        }
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& l : lines) {
            out << l << '\n';
        }
        if (!out.good()) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

void WriteLog(ForgeLogLevel level, const char* who, const char* message) {
    if (level < g_level.load(std::memory_order_relaxed)) {
        return;
    }
    char line[1200];
    std::snprintf(line, sizeof(line), "[forge] [%s] %s", who, message);
    if (g_host) {
        g_host->log(static_cast<ModHostLogLevel>(level), line);
    }
    static constexpr const char* kNames[] = {"debug", "info", "warn", "error"};
    std::scoped_lock lk{g_log_lock};
    if (g_log_file.is_open()) {
        g_log_file << '[' << kNames[std::clamp(static_cast<int>(level), 0, 3)] << "] [" << who
                   << "] " << message << std::endl;
    }
}

// --- guest memory -----------------------------------------------------------------------------

bool MemIsValid(u32 addr, u32 size) {
    return g_host->mem_valid(addr, size);
}
bool MemRead(u32 addr, void* out, u32 size) {
    return g_host->mem_read(addr, out, size);
}
bool MemWrite(u32 addr, const void* data, u32 size) {
    return g_host->mem_write(addr, data, size);
}
void* MemPtr(u32 addr, u32 size) {
    return g_host->mem_ptr(addr, size);
}

u32 Read32(u32 addr) {
    u32 v = 0;
    MemRead(addr, &v, 4);
    return v;
}

std::string ReadString(u32 addr) {
    std::string s;
    for (u32 i = 0; i < 256; ++i) {
        char c = 0;
        if (!MemRead(addr + i, &c, 1) || c == 0) {
            break;
        }
        s.push_back(c);
    }
    return s;
}

const Module* FindModule(const char* name) {
    if (!name) {
        return nullptr;
    }
    for (const auto& m : g_modules) {
        if (m.alias == name || m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

u32 ModuleBase(const char* name) {
    const Module* m = FindModule(name);
    return m ? m->base : 0;
}

u32 ModuleSize(const char* name) {
    const Module* m = FindModule(name);
    return m ? m->size : 0;
}

// --- guest calls ------------------------------------------------------------------------------

bool Call(u32 function, const u32* args, u32 count, const float* fargs, u32 fcount,
          ForgeCallResult* out) {
    u32 r[2]{};
    double d0{};
    if (!g_host->call(function, args, count, fargs, fcount, r, &d0)) {
        return false;
    }
    if (out) {
        out->r0 = r[0];
        out->r1 = r[1];
        out->d0 = d0;
        out->s0 = std::bit_cast<float>(static_cast<u32>(std::bit_cast<uint64_t>(d0)));
    }
    return true;
}

// --- MT Framework DTI / singletons ------------------------------------------------------------

u32 MakeId(const char* name) {
    // MtDti::makeId: CRC-32 (ARM crc32b, reflected 0x04C11DB7), init ~0, no final xor, 31 bits.
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    u32 crc = 0xFFFFFFFFu;
    for (const char* p = name; p && *p; ++p) {
        crc = table[(crc ^ static_cast<u8>(*p)) & 0xFF] ^ (crc >> 8);
    }
    return crc & 0x7FFFFFFFu;
}

u32 DtiOf(u32 object) {
    if (!MemIsValid(object, 4)) {
        return 0;
    }
    const u32 vtable = Read32(object);
    if (!MemIsValid(vtable + kGetDtiSlot, 4)) {
        return 0;
    }
    ForgeCallResult res{};
    if (!Call(Read32(vtable + kGetDtiSlot), &object, 1, nullptr, 0, &res) ||
        !MemIsValid(res.r0, 0x20)) {
        return 0;
    }
    return res.r0;
}

/// Name and id offsets of MtDti, checked once against makeId (Forge's header: vptr, name,
/// next, child, parent, link, size bits, id). Call with g_single_lock held.
bool DtiFields(u32 dti, u32* name_off, u32* id_off) {
    if (g_dti_layout < 0) {
        for (const int off : {4, 0}) {
            const std::string name = ReadString(Read32(dti + off));
            if (!name.empty() && MakeId(name.c_str()) == Read32(dti + off + 0x18)) {
                g_dti_layout = off;
                Log(FORGE_LOG_DEBUG, "DTI layout: name at +%d, id at +%#x (%s)", off, off + 0x18,
                    name.c_str());
                break;
            }
        }
        if (g_dti_layout < 0) {
            return false;
        }
    }
    *name_off = static_cast<u32>(g_dti_layout);
    *id_off = static_cast<u32>(g_dti_layout) + 0x18;
    return true;
}

const char* DtiName(u32 dti) {
    if (!MemIsValid(dti, 0x20)) {
        return nullptr;
    }
    std::scoped_lock lk{g_single_lock};
    if (const auto it = g_dti_names.find(dti); it != g_dti_names.end()) {
        return it->second.c_str();
    }
    u32 name_off{}, id_off{};
    if (!DtiFields(dti, &name_off, &id_off)) {
        return nullptr;
    }
    return g_dti_names.emplace(dti, ReadString(Read32(dti + name_off))).first->second.c_str();
}

/// Resolves the systems constructed so far (getDti is virtual: needs a game thread).
void ResolveSingletons() {
    std::vector<u32> pending;
    {
        std::scoped_lock lk{g_single_lock};
        pending.swap(g_pending);
    }
    std::vector<u32> unresolved;
    for (const u32 obj : pending) {
        const u32 dti = DtiOf(obj);
        const char* name = dti ? DtiName(dti) : nullptr;
        u32 name_off{}, id_off{};
        std::scoped_lock lk{g_single_lock};
        if (!name || !DtiFields(dti, &name_off, &id_off)) {
            unresolved.push_back(obj);
            continue;
        }
        g_by_id[Read32(dti + id_off)] = obj;
        Log(FORGE_LOG_DEBUG, "singleton %s @ %#x", name, obj);
    }
    if (!unresolved.empty()) {
        std::scoped_lock lk{g_single_lock};
        g_pending.insert(g_pending.end(), unresolved.begin(), unresolved.end());
    }
}

u32 SingletonById(u32 id) {
    {
        std::scoped_lock lk{g_single_lock};
        if (const auto it = g_by_id.find(id); it != g_by_id.end()) {
            return it->second;
        }
        if (g_pending.empty()) {
            return 0;
        }
    }
    ResolveSingletons(); // systems constructed since (only works on a game thread)
    std::scoped_lock lk{g_single_lock};
    const auto it = g_by_id.find(id);
    return it != g_by_id.end() ? it->second : 0;
}

u32 SingletonByName(const char* name) {
    return name ? SingletonById(MakeId(name)) : 0;
}

u32 SingletonAll(u32* out, u32 max) {
    ResolveSingletons();
    std::scoped_lock lk{g_single_lock};
    if (!out) {
        return static_cast<u32>(g_by_id.size());
    }
    u32 n = 0;
    for (const auto& [id, obj] : g_by_id) {
        if (n >= max) {
            break;
        }
        out[n++] = obj;
    }
    return n;
}

// --- patterns ---------------------------------------------------------------------------------

u32 PatternFind(const char* module, const char* pattern, u32 from) {
    const Module* m = FindModule(module ? module : "main");
    if (!m || !pattern) {
        return 0;
    }
    std::vector<std::pair<u8, u8>> pat; // value, mask
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?') {
            pat.emplace_back(u8{0}, u8{0});
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        const char hex[3] = {p[0], p[1] ? p[1] : '\0', 0};
        pat.emplace_back(static_cast<u8>(std::strtoul(hex, nullptr, 16)), u8{0xFF});
        p += p[1] ? 2 : 1;
    }
    if (pat.empty()) {
        return 0;
    }
    const u32 lo = std::max(m->base, from);
    const u32 hi = m->base + m->size;
    constexpr u32 kPage = 0x1000;
    std::vector<u8> bytes;
    std::vector<u8> page(kPage);
    u32 start = lo & ~(kPage - 1);
    for (u32 a = start; a < hi; a += kPage) {
        if (!MemRead(a, page.data(), kPage)) {
            if (!bytes.empty()) {
                break; // end of the mapped part
            }
            start = a + kPage;
            continue;
        }
        bytes.insert(bytes.end(), page.begin(), page.end());
    }
    const size_t n = pat.size();
    for (size_t i = (lo > start ? lo - start : 0); i + n <= bytes.size(); ++i) {
        size_t k = 0;
        while (k < n && (bytes[i + k] & pat[k].second) == pat[k].first) {
            ++k;
        }
        if (k == n) {
            return start + static_cast<u32>(i);
        }
    }
    return 0;
}

// --- hooks ------------------------------------------------------------------------------------

/// The plugin whose code is at `addr` (the caller of an API function): the owner of the hooks
/// it makes, dropped on hot reload. nullptr = not a plugin (Forge itself).
ForgePluginState* PluginOfCode(const void* addr) {
    if (const void* mod = plat::ModuleOf(addr)) {
        std::scoped_lock lk{g_owner_lock};
        const auto it = g_owners.find(mod);
        if (it != g_owners.end()) {
            return it->second;
        }
    }
    return nullptr;
}

ForgeHook* AddHook(u32 target, ForgeHookPre pre, ForgeHookPost post, ForgeOverride override_fn,
                   void* user, ForgePluginState* owner, bool front) {
    if (!pre && !post && !override_fn) {
        Log(FORGE_LOG_ERROR, "hook at %#x: no callback", target);
        return nullptr;
    }
    if (!g_host->is_block(target)) {
        Log(FORGE_LOG_ERROR, "hook at %#x: not the start of a recompiled block", target);
        return nullptr;
    }
    auto* hook = new ForgeHook{target, pre, post, user};
    hook->owner = owner;
    hook->override_fn = override_fn;
    {
        std::unique_lock lk{g_hooks_lock};
        auto& list = g_hooks[target];
        if (front) {
            list.insert(list.begin(), hook);
        } else {
            list.push_back(hook);
        }
    }
    g_host->mark(target);
    Log(FORGE_LOG_DEBUG, "%s at %#x (main+%#x)", override_fn ? "override" : "hook", target,
        target - g_main);
    return hook;
}

ForgeHook* HookCreate(u32 target, ForgeHookPre pre, ForgeHookPost post, void* user) {
    return AddHook(target, pre, post, nullptr, user, PluginOfCode(FORGE_RETURN_ADDRESS()), false);
}

ForgeHook* HookOverride(u32 target, ForgeOverride fn, void* user) {
    return AddHook(target, nullptr, nullptr, fn, user, PluginOfCode(FORGE_RETURN_ADDRESS()),
                   false);
}

void HookEnable(ForgeHook* hook, bool enabled) {
    if (hook) {
        hook->enabled.store(enabled);
    }
}

/// Runs the first usable override of `chain` from position `start` on (enabled, its plugin not
/// being unloaded) on `cpu`. False if there is none: the original function is next.
bool RunOverrideFrom(OverrideChain& chain, size_t start, ForgeCpu* cpu) {
    for (size_t i = start; i < chain.count; ++i) {
        ForgeHook* h = chain.list[i];
        if (!h->enabled.load(std::memory_order_relaxed) || !Enter(h->owner)) {
            continue;
        }
        const size_t saved = chain.index;
        chain.index = i;
        {
            CallbackScope scope{h->owner, h->target, "override"};
            h->override_fn(cpu, h->user);
        }
        chain.index = saved;
        Leave(h->owner);
        return true;
    }
    return false;
}

bool CallOriginal(u32 function, const u32* args, u32 count, const float* fargs, u32 fcount,
                  ForgeCallResult* out) {
    if (OverrideChain* chain = t_chain; chain && chain->target == function) {
        // an override calls the function it replaced: the override below it, if any
        ForgeCpu cpu = chain->entry;
        for (u32 i = 0; i < 4; ++i) {
            cpu.r[i] = i < count ? args[i] : 0;
        }
        for (u32 i = 0; i < 16; ++i) {
            cpu.vfp.s[i] = i < fcount && fargs ? fargs[i] : 0.0f;
        }
        const size_t from = chain->index == static_cast<size_t>(-1) ? 0 : chain->index + 1;
        if (RunOverrideFrom(*chain, from, &cpu)) {
            if (count > 4) {
                static std::atomic<int> warned{0};
                if (warned.fetch_add(1) < 3) {
                    Log(FORGE_LOG_WARN, "call_original(%#x): stack arguments don't reach the "
                                        "next override down", function);
                }
            }
            if (out) {
                out->r0 = cpu.r[0];
                out->r1 = cpu.r[1];
                out->s0 = cpu.vfp.s[0];
                out->d0 = cpu.vfp.d[0];
            }
            return true;
        }
    }
    ++t_bypass;
    const bool ok = Call(function, args, count, fargs, fcount, out);
    --t_bypass;
    return ok;
}

ForgeLogLevel LogGetLevel() {
    return static_cast<ForgeLogLevel>(g_level.load());
}

void PluginLog(ForgeLogLevel level, const char* plugin, const char* message) {
    WriteLog(level, plugin ? plugin : "plugin", message ? message : "");
}

const char* ModsDir() {
    return g_mods_dir.c_str();
}

bool MenuOpen() {
    return MenuIsOpen();
}

// guest memory for plugins (mod host ABI 2)
uint32_t MemAlloc(uint32_t size, uint32_t align) {
    if (!g_host || !MODHOST_HAS(ModHostApi, g_host, guest_alloc) || !g_host->guest_alloc) {
        return 0;
    }
    return g_host->guest_alloc(size, align < 16 ? 16 : align);
}

void MemFree(uint32_t addr) {
    if (addr && g_host && MODHOST_HAS(ModHostApi, g_host, guest_free) && g_host->guest_free) {
        g_host->guest_free(addr);
    }
}

// config files of plugins (src/config.cpp); the caller's module says whose mod folder it is
ForgeConfig* ConfigOpenApi(const char* name) {
    return ConfigOpen(PluginOfCode(FORGE_RETURN_ADDRESS()), name);
}
int32_t ConfigIntApi(ForgeConfig* c, const char* sec, const char* key, int32_t def) {
    return ConfigGetInt(c, sec, key, def);
}
float ConfigFloatApi(ForgeConfig* c, const char* sec, const char* key, float def) {
    return ConfigGetFloat(c, sec, key, def);
}
bool ConfigBoolApi(ForgeConfig* c, const char* sec, const char* key, bool def) {
    return ConfigGetBool(c, sec, key, def);
}
const char* ConfigStringApi(ForgeConfig* c, const char* sec, const char* key, const char* def) {
    return ConfigGetString(c, sec, key, def);
}
bool ConfigSetApi(ForgeConfig* c, const char* sec, const char* key, const char* value) {
    return ConfigSetValue(c, sec, key, value);
}
bool ConfigSaveApi(ForgeConfig* c) {
    return ConfigSave(c);
}
bool ConfigChangedApi(ForgeConfig* c) {
    return ConfigPoll(c);
}

const ForgeApi kApi = {
    .version = {FORGE_PC_VERSION_MAJOR, FORGE_PC_VERSION_MINOR, FORGE_PC_VERSION_PATCH},
    .size = static_cast<uint32_t>(sizeof(ForgeApi)),
    .log = PluginLog,
    .log_getLevel = LogGetLevel,
    .mem_getModuleBase = ModuleBase,
    .mem_getModuleSize = ModuleSize,
    .mem_isValid = MemIsValid,
    .mem_read = MemRead,
    .mem_write = MemWrite,
    .mem_ptr = MemPtr,
    .hook_create = HookCreate,
    .hook_enable = HookEnable,
    .call = Call,
    .singleton_getByName = SingletonByName,
    .singleton_getById = SingletonById,
    .singleton_getAll = SingletonAll,
    .dti_of = DtiOf,
    .dti_name = DtiName,
    .dti_makeId = MakeId,
    .pattern_find = PatternFind,
    .mods_dir = ModsDir,
    .imgui_version = kImGuiVersion,
    .menu_isOpen = MenuOpen,
    .mem_alloc = MemAlloc,
    .mem_free = MemFree,
    .screen_size = ScreenSize,
    .call_original = CallOriginal,
    .hook_override = HookOverride,
    .config_open = ConfigOpenApi,
    .config_int = ConfigIntApi,
    .config_float = ConfigFloatApi,
    .config_bool = ConfigBoolApi,
    .config_string = ConfigStringApi,
    .config_set = ConfigSetApi,
    .config_save = ConfigSaveApi,
    .config_changed = ConfigChangedApi,
};

// --- plugins ----------------------------------------------------------------------------------

template <typename T>
T Symbol(const Plugin& p, const char* name) {
    return reinterpret_cast<T>(plat::LibSym(p.handle, name));
}

/// Every <mods dir>/<mod>/plugins/*.dll (*.so on Linux), mods in name order.
std::vector<std::filesystem::path> FindPluginFiles() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const auto mods = PathU8(g_mods_dir);
    std::vector<std::filesystem::path> mod_dirs;
    for (const auto& e : std::filesystem::directory_iterator(mods, ec)) {
        if (e.is_directory(ec)) {
            mod_dirs.push_back(e.path());
        }
    }
    std::sort(mod_dirs.begin(), mod_dirs.end());
    for (const auto& dir : mod_dirs) {
        std::vector<std::filesystem::path> dlls;
        for (const auto& e : std::filesystem::directory_iterator(dir / "plugins", ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == plat::kLibExt) {
                dlls.push_back(e.path());
            }
        }
        std::sort(dlls.begin(), dlls.end());
        files.insert(files.end(), dlls.begin(), dlls.end());
    }
    return files;
}

/// Removes leftover live copies (<name>.dll.<n>.live) from a plugins folder.
void RemoveLiveCopies(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".live") {
            std::filesystem::remove(e.path(), ec); // fails while loaded: fine
        }
    }
}

void FreePlugin(Plugin& p) {
    if (p.handle) {
        {
            std::scoped_lock lk{g_owner_lock};
            g_owners.erase(p.base);
        }
        CrashPluginUnloaded(p.base);
        ConfigDropOwner(p.state);
        plat::LibClose(p.handle);
        p.handle = nullptr;
        p.base = nullptr;
    }
    std::error_code ec;
    if (!p.live.empty()) {
        std::filesystem::remove(p.live, ec);
    }
}

/// Loads one plugin from a copy of `path` (the dll itself stays writable for builds) and calls
/// forge_setApi / forge_onLoad. False if it isn't loadable or wants another Forge version.
bool LoadOne(const std::filesystem::path& path, Plugin& p) {
    p.mod = Utf8(path.parent_path().parent_path().filename());
    p.file = Utf8(path.parent_path().parent_path().filename() / path.filename());
    p.source = path;
    // the copy sits in the same folder: the plugin's own folder (GetModuleFileName / dladdr) and the
    // dependencies next to it stay the same
    std::error_code ec;
    p.live = path;
    p.live += "." + std::to_string(++g_live_counter) + ".live";
    if (!std::filesystem::copy_file(path, p.live,
                                    std::filesystem::copy_options::overwrite_existing, ec)) {
        Log(FORGE_LOG_ERROR, "%s: could not be copied for loading (%s)", p.file.c_str(),
            ec.message().c_str());
        p.live.clear();
        return false;
    }
    std::string error;
    p.handle = plat::LibOpen(p.live, error);
    if (!p.handle) {
        Log(FORGE_LOG_ERROR, "%s: could not be loaded (%s)", p.file.c_str(), error.c_str());
        FreePlugin(p);
        return false;
    }
    p.state = new ForgePluginState{};
    p.state->mod = p.mod;
    p.state->file = p.file;
    p.state->live = Utf8(p.live.filename());
    p.state->name = p.file;
    const auto set_api = Symbol<void (*)(const ForgeApi*)>(p, "forge_setApi");
    const auto on_load = Symbol<void (*)(ForgePluginInfo*)>(p, "forge_onLoad");
    p.base = on_load ? plat::ModuleOf(reinterpret_cast<const void*>(on_load)) : nullptr;
    if (p.base) {
        {
            std::scoped_lock lk{g_owner_lock};
            g_owners[p.base] = p.state;
        }
        CrashPluginLoaded(p.base, p.state);
    }
    // logs and crash reports name the dll behind the numbered live copy
    Log(FORGE_LOG_INFO, "%s is loaded as %s", p.file.c_str(), p.state->live.c_str());
    if (!set_api || !on_load) {
        Log(FORGE_LOG_WARN, "%s: not a Forge PC plugin (no forge_setApi/forge_onLoad)",
            p.file.c_str());
        p.state->dead = true;
        FreePlugin(p);
        return false;
    }
    p.on_init = Symbol<void (*)()>(p, "forge_onInit");
    p.on_update = Symbol<void (*)(float)>(p, "forge_onUpdate");
    p.on_unload = Symbol<void (*)()>(p, "forge_onUnload");
    p.ui.on_imgui_init = Symbol<void (*)(void*, void*, void*, void*)>(p, "forge_onImGuiInit");
    p.ui.on_imgui_render = Symbol<void (*)()>(p, "forge_onImGuiRender");
    p.ui.on_imgui_free_render = Symbol<void (*)()>(p, "forge_onImGuiFreeRender");
    p.ui.on_present = Symbol<void (*)(const ForgePresent*)>(p, "forge_onPresent");
    p.ui.generation = ++g_ui_generation;
    p.ui.state = p.state;
    set_api(&kApi);
    {
        CallbackScope scope{p.state, 0, "onLoad"};
        on_load(&p.info);
    }
    p.info.name[sizeof(p.info.name) - 1] = '\0';
    if (p.info.name[0]) {
        p.state->name = p.info.name;
    }
    const ForgeVersion& want = p.info.required_ver;
    if (want.major != FORGE_PC_VERSION_MAJOR || want.minor > FORGE_PC_VERSION_MINOR) {
        Log(FORGE_LOG_ERROR, "%s requires Forge PC %u.%u.*, this is %u.%u.%u: not loaded",
            p.file.c_str(), want.major, want.minor, FORGE_PC_VERSION_MAJOR,
            FORGE_PC_VERSION_MINOR, FORGE_PC_VERSION_PATCH);
        p.state->dead = true;
        FreePlugin(p);
        return false;
    }
    p.ui.name = p.info.name[0] ? p.info.name : p.file;
    return true;
}

void StampOf(const std::filesystem::path& path, std::filesystem::file_time_type& time,
             uintmax_t& size) {
    std::error_code ec;
    time = std::filesystem::last_write_time(path, ec);
    size = std::filesystem::file_size(path, ec);
    if (ec) {
        size = 0;
    }
}

bool IsOff(const std::string& value) {
    return value == "off" || value == "false" || value == "0" || value == "disabled";
}

/// A mod is on unless forge.ini [mods] says <name> = off.
bool ModEnabled(const std::string& mod) {
    return !IsOff(ConfigValue("mods", mod.c_str()));
}

std::string ModOf(const std::filesystem::path& plugin_file) {
    return Utf8(plugin_file.parent_path().parent_path().filename());
}

/// Lists the mod folders (with plugins or game files) for the menu.
void ScanMods() {
    std::vector<ModEntry> mods;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(PathU8(g_mods_dir), ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        ModEntry m;
        m.name = Utf8(e.path().filename());
        m.dir = e.path();
        for (const auto& f : std::filesystem::directory_iterator(e.path() / "plugins", ec)) {
            if (f.is_regular_file(ec) && f.path().extension() == plat::kLibExt) {
                m.files.push_back(f.path());
            }
        }
        std::sort(m.files.begin(), m.files.end());
        m.has_romfs = std::filesystem::is_directory(e.path() / "romfs", ec);
        if (!m.files.empty() || m.has_romfs) {
            mods.push_back(std::move(m));
        }
    }
    std::sort(mods.begin(), mods.end(),
              [](const ModEntry& a, const ModEntry& b) { return a.dir < b.dir; });
    std::scoped_lock lk{g_mods_lock};
    for (auto& m : mods) {
        if (const auto n = g_mod_notes.find(m.name); n != g_mod_notes.end()) {
            m.note = n->second;
        }
    }
    g_mod_list = std::move(mods);
}

void LoadPlugins() {
    ScanMods();
    const auto files = FindPluginFiles();
    for (const auto& path : files) {
        RemoveLiveCopies(path.parent_path());
    }
    for (const auto& path : files) {
        {
            Watch w{path};
            StampOf(path, w.time, w.size);
            w.seen_time = w.time;
            w.seen_size = w.size;
            std::scoped_lock lk{g_reload_lock};
            g_watch.push_back(w);
        }
        if (!ModEnabled(ModOf(path))) {
            Log(FORGE_LOG_INFO, "%s: mod switched off in forge.ini [mods], not loaded",
                Utf8(path.parent_path().parent_path().filename() / path.filename()).c_str());
            continue;
        }
        Plugin p;
        if (!LoadOne(path, p)) {
            continue;
        }
        Log(FORGE_LOG_INFO, "loaded %s (%s)", p.file.c_str(), p.ui.name.c_str());
        std::scoped_lock lk{g_plugins_lock};
        g_plugins.push_back(std::move(p));
    }
}

// --- hot reload -------------------------------------------------------------------------------
// A plugin dll that changed (a build finished: same time and size on two polls a second apart)
// or the menu's Reload button: on the game thread the plugin leaves the plugin list (no more
// menu / update calls), its hooks are dropped and it is marked dead (hook callbacks in flight
// finish, no new ones start). Once nothing runs inside it: forge_onUnload, unload it, load
// the new copy, forge_onInit, back in its place in the list.

void RequestReload(const std::filesystem::path& source) {
    std::scoped_lock lk{g_reload_lock};
    if (std::find(g_reload_requests.begin(), g_reload_requests.end(), source) ==
        g_reload_requests.end()) {
        g_reload_requests.push_back(source);
    }
}

void WatchPlugins() {
    while (!g_watch_stop.load()) {
        for (int i = 0; i < 10 && !g_watch_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::scoped_lock lk{g_reload_lock};
        for (auto& w : g_watch) {
            std::filesystem::file_time_type t{};
            uintmax_t n = 0;
            StampOf(w.source, t, n);
            if (n == 0) {
                continue; // being written / gone
            }
            const bool changed = t != w.time || n != w.size;
            const bool stable = t == w.seen_time && n == w.seen_size;
            w.seen_time = t;
            w.seen_size = n;
            if (changed && stable) {
                w.time = t;
                w.size = n;
                if (std::find(g_reload_requests.begin(), g_reload_requests.end(), w.source) ==
                    g_reload_requests.end()) {
                    g_reload_requests.push_back(w.source);
                }
            }
        }
    }
}

bool UnloadingHas(const std::filesystem::path& source) {
    return std::any_of(g_unloading.begin(), g_unloading.end(),
                       [&](const Unloading& u) { return u.plugin.source == source; });
}

/// Takes a loaded plugin out of the list for unloading: no more menu / update calls, its hooks
/// are dropped and it is marked dead (callbacks in flight finish, no new ones start).
bool BeginUnload(const std::filesystem::path& source, bool reload) {
    Unloading u{};
    u.reload = reload;
    {
        std::scoped_lock lk{g_plugins_lock};
        const auto it = std::find_if(g_plugins.begin(), g_plugins.end(),
                                     [&](const Plugin& p) { return p.source == source; });
        if (it == g_plugins.end()) {
            return false;
        }
        u.index = static_cast<size_t>(it - g_plugins.begin());
        u.plugin = std::move(*it);
        g_plugins.erase(it);
    }
    u.plugin.state->dead = true;
    {
        std::unique_lock lk{g_hooks_lock};
        for (auto& [pc, list] : g_hooks) {
            std::erase_if(list, [&](ForgeHook* h) { return h->owner == u.plugin.state; });
        }
    }
    g_unloading.push_back(std::move(u));
    return true;
}

/// Mod switches from the menu (and reload requests): off = unload the mod's plugins and leave
/// them unloaded, on = load them (a plugin already being unloaded is kept instead).
void ProcessModRequests(const std::vector<std::pair<std::string, bool>>& mod_requests) {
    for (const auto& [mod, on] : mod_requests) {
        std::vector<std::filesystem::path> files;
        {
            std::scoped_lock lk{g_mods_lock};
            for (const auto& m : g_mod_list) {
                if (m.name == mod) {
                    files = m.files;
                }
            }
        }
        for (const auto& source : files) {
            if (on) {
                const auto pending = std::find_if(
                    g_unloading.begin(), g_unloading.end(),
                    [&](const Unloading& u) { return u.plugin.source == source; });
                if (pending != g_unloading.end()) {
                    pending->reload = true; // switched off and on again before it was gone
                    continue;
                }
                bool loaded = false;
                {
                    std::scoped_lock lk{g_plugins_lock};
                    loaded = std::any_of(g_plugins.begin(), g_plugins.end(),
                                         [&](const Plugin& p) { return p.source == source; });
                }
                if (!loaded) {
                    Unloading u{};
                    u.plugin.source = source;
                    {
                        std::scoped_lock lk{g_plugins_lock};
                        u.index = g_plugins.size();
                    }
                    g_unloading.push_back(std::move(u)); // loads once there is nothing to unload
                }
            } else {
                const auto pending = std::find_if(
                    g_unloading.begin(), g_unloading.end(),
                    [&](const Unloading& u) { return u.plugin.source == source; });
                if (pending != g_unloading.end()) {
                    pending->reload = false;
                } else {
                    BeginUnload(source, false);
                }
            }
        }
        Log(FORGE_LOG_INFO, "mod %s switched %s", mod.c_str(), on ? "on" : "off");
    }
}

/// "Reset scripts" (menu): looks for plugin files again and loads every one of an enabled mod
/// that is not loaded: new files, plugins that were off, plugins whose reload failed. Loaded
/// plugins are left alone (the Reload button restarts one). Game thread.
void ResetScripts() {
    ScanMods();
    size_t fresh = 0, retried = 0;
    for (const auto& path : FindPluginFiles()) {
        bool known = false;
        {
            std::scoped_lock lk{g_reload_lock};
            known = std::any_of(g_watch.begin(), g_watch.end(),
                                [&](const Watch& w) { return w.source == path; });
            if (!known) { // new file: watch it for hot reload too
                Watch w{path};
                StampOf(path, w.time, w.size);
                w.seen_time = w.time;
                w.seen_size = w.size;
                g_watch.push_back(w);
            }
        }
        if (!ModEnabled(ModOf(path)) || UnloadingHas(path)) {
            continue;
        }
        bool loaded = false;
        {
            std::scoped_lock lk{g_plugins_lock};
            loaded = std::any_of(g_plugins.begin(), g_plugins.end(),
                                 [&](const Plugin& p) { return p.source == path; });
        }
        if (loaded) {
            continue;
        }
        Unloading u{};
        u.plugin.source = path;
        {
            std::scoped_lock lk{g_plugins_lock};
            u.index = g_plugins.size();
        }
        g_unloading.push_back(std::move(u)); // loaded by the loop in ProcessReloads
        known ? ++retried : ++fresh;
        Log(FORGE_LOG_INFO, "reset scripts: loading %s (%s)", Utf8(path).c_str(),
            known ? "was not loaded" : "new");
    }
    char msg[128];
    if (fresh + retried == 0) {
        std::snprintf(msg, sizeof msg, "Nothing to load: every plugin of an enabled mod is loaded.");
    } else {
        std::snprintf(msg, sizeof msg, "Loading %zu plugin%s (%zu new, %zu retried).",
                      fresh + retried, fresh + retried == 1 ? "" : "s", fresh, retried);
    }
    Log(FORGE_LOG_INFO, "reset scripts: %s", msg);
    std::scoped_lock lk{g_mods_lock};
    g_reset_msg = msg;
}

void ProcessReloads() {
    if (g_reset_req.exchange(false)) {
        ResetScripts();
    }
    std::vector<std::filesystem::path> requests;
    std::vector<std::pair<std::string, bool>> mod_requests;
    {
        std::scoped_lock lk{g_reload_lock};
        requests.swap(g_reload_requests);
        mod_requests.swap(g_mod_requests);
    }
    ProcessModRequests(mod_requests);
    for (const auto& source : requests) {
        if (!ModEnabled(ModOf(source)) || UnloadingHas(source)) {
            continue; // a mod that is off stays unloaded; already on its way
        }
        if (!BeginUnload(source, true)) {
            // not loaded (failed before): try it now
            Unloading u{};
            u.plugin.source = source;
            {
                std::scoped_lock lk{g_plugins_lock};
                u.index = g_plugins.size();
            }
            g_unloading.push_back(std::move(u));
        }
    }
    for (auto it = g_unloading.begin(); it != g_unloading.end();) {
        Plugin& old = it->plugin;
        if (old.state && old.state->inflight.load() != 0) {
            if (++it->frames == 600) {
                Log(FORGE_LOG_WARN, "%s: still busy after 600 frames, waiting to reload",
                    old.file.c_str());
            }
            ++it;
            continue;
        }
        if (old.handle) {
            if (old.on_unload) {
                CallbackScope scope{old.state, 0, "unload"};
                old.on_unload();
            }
            FreePlugin(old);
        }
        if (!it->reload) {
            Log(FORGE_LOG_INFO, "unloaded %s (mod switched off)", old.file.c_str());
            it = g_unloading.erase(it);
            continue;
        }
        Plugin p;
        if (LoadOne(it->plugin.source, p)) {
            Log(FORGE_LOG_INFO, "reloaded %s (%s)", p.file.c_str(), p.ui.name.c_str());
            const auto init = p.on_init;
            ForgePluginState* state = p.state;
            {
                std::scoped_lock lk{g_plugins_lock};
                const size_t at = std::min(it->index, g_plugins.size());
                g_plugins.insert(g_plugins.begin() + static_cast<std::ptrdiff_t>(at),
                                 std::move(p));
            }
            if (init && Enter(state)) {
                CallbackScope scope{state, 0, "init"};
                init();
                Leave(state);
            }
        } else {
            Log(FORGE_LOG_ERROR, "%s: reload failed, it stays unloaded until the file changes",
                Utf8(it->plugin.source).c_str());
        }
        it = g_unloading.erase(it);
    }
}

// --- built-in hooks ---------------------------------------------------------------------------

ForgeHookAction OnSystemCtor(ForgeCpu* cpu, void*) {
    std::scoped_lock lk{g_single_lock};
    g_pending.push_back(cpu->r[0]);
    return FORGE_HOOK_CONTINUE;
}

ForgeHookAction OnSystemDtor(ForgeCpu* cpu, void*) {
    const u32 obj = cpu->r[0];
    std::scoped_lock lk{g_single_lock};
    std::erase(g_pending, obj);
    std::erase_if(g_by_id, [obj](const auto& kv) { return kv.second == obj; });
    return FORGE_HOOK_CONTINUE;
}

ForgeHookAction OnAppRun(ForgeCpu*, void*) {
    if (g_plugins_loaded.exchange(true)) {
        return FORGE_HOOK_CONTINUE;
    }
    ResolveSingletons();
    Log(FORGE_LOG_INFO, "%u singletons resolved; loading plugins from %s/*/plugins",
        SingletonAll(nullptr, 0), g_mods_dir.c_str());
    LoadPlugins();
    CrashInit(); // plugins may have set exception filters of their own: ours goes back on top
    std::vector<std::pair<void (*)(), ForgePluginState*>> inits;
    {
        std::scoped_lock lk{g_plugins_lock};
        for (const auto& p : g_plugins) {
            if (p.on_init) {
                inits.emplace_back(p.on_init, p.state);
            }
        }
    }
    for (const auto& [init, state] : inits) {
        if (Enter(state)) {
            {
                CallbackScope scope{state, 0, "init"};
                init();
            }
            Leave(state);
        }
    }
    const std::string hot = ConfigValue("plugins", "hot_reload");
    if (hot != "false" && hot != "0") {
        g_watcher = std::thread(WatchPlugins);
        Log(FORGE_LOG_INFO, "hot reload on: a rebuilt plugin dll is reloaded in game");
    }
    return FORGE_HOOK_CONTINUE;
}

void OnUpdateReturn(ForgeCpu*, const u32 args[4], void*) {
    float dt = 0.0f;
    MemRead(args[0] + kUpdateDt, &dt, 4);
    g_frame_count.fetch_add(1, std::memory_order_relaxed);
    TraceTick();
    ProcessReloads();
    std::vector<std::pair<void (*)(float), ForgePluginState*>> updates;
    {
        std::scoped_lock lk{g_plugins_lock};
        for (const auto& p : g_plugins) {
            if (p.on_update) {
                updates.emplace_back(p.on_update, p.state);
            }
        }
    }
    for (const auto& [update, state] : updates) {
        if (Enter(state)) {
            {
                CallbackScope scope{state, 0, "update"};
                update(dt);
            }
            Leave(state);
        }
    }
}

void SetReturnPc(ModHostCpu* cpu, u32 target) {
    cpu->r[15] = target & ~1u; // bx lr: bit 0 selects Thumb
    cpu->thumb = target & 1u;
}

void RunPosts(ForgeCpu* cpu, const std::vector<ForgeHook*>& posts, const u32* args) {
    for (ForgeHook* h : posts) {
        if (h->enabled.load(std::memory_order_relaxed) && Enter(h->owner)) {
            {
                CallbackScope scope{h->owner, h->target, "post"};
                h->post(cpu, args, h->user);
            }
            Leave(h->owner);
        }
    }
}

// --- mod host callbacks -----------------------------------------------------------------------

void OnGameStart() {
    const u32 count = g_host->module_count();
    for (u32 i = 0; i < count; ++i) {
        ModHostModule m{};
        m.size = sizeof(m);
        if (g_host->module_get(i, &m)) {
            g_modules.push_back({m.name ? m.name : "", m.alias ? m.alias : "", m.base, m.span});
        }
    }
    g_main = ModuleBase("main");
    if (g_main == 0) {
        Log(FORGE_LOG_ERROR, "no main module: Forge stays idle");
        return;
    }
    HookCreate(g_main + kSystemCtor, OnSystemCtor, nullptr, nullptr);
    HookCreate(g_main + kSystemDtor, OnSystemDtor, nullptr, nullptr);
    HookCreate(g_main + kAppRun, OnAppRun, nullptr, nullptr);
    HookCreate(g_main + kUpdate, nullptr, OnUpdateReturn, nullptr);
    // before any plugin hook exists: trace hooks run first on their addresses
    TraceStart();
    Log(FORGE_LOG_INFO, "Forge PC %d.%d.%d ready (main at %#x)", FORGE_PC_VERSION_MAJOR,
        FORGE_PC_VERSION_MINOR, FORGE_PC_VERSION_PATCH, g_main);
}

bool OnDispatch(uint32_t pc, ModHostCpu* mcpu, uint32_t thread_key) {
    static_assert(sizeof(ForgeCpu) == sizeof(ModHostCpu));
    auto* cpu = reinterpret_cast<ForgeCpu*>(mcpu);
    if (pc == MODHOST_RETURN_PC) {
        Frame frame;
        {
            std::scoped_lock lk{g_frames_lock};
            auto& stack = g_frames[thread_key];
            if (stack.empty()) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 8) {
                    Log(FORGE_LOG_ERROR, "return to the hook trampoline without a frame "
                                         "(thread %#x)", thread_key);
                }
                return false;
            }
            frame = std::move(stack.back());
            stack.pop_back();
        }
        t_thread_key = thread_key;
        RunPosts(cpu, frame.posts, frame.args.data());
        SetReturnPc(mcpu, frame.lr);
        return true;
    }
    if (t_bypass > 0) {
        return false; // inside call_original: the function runs as it is
    }
    t_thread_key = thread_key;

    std::array<ForgeHook*, 24> hooks{};
    size_t count = 0;
    OverrideChain chain;
    chain.target = pc;
    chain.index = static_cast<size_t>(-1);
    {
        std::shared_lock lk{g_hooks_lock};
        const auto it = g_hooks.find(pc);
        if (it == g_hooks.end()) {
            return false; // marked by the exe itself (its own hooks)
        }
        for (ForgeHook* h : it->second) {
            if (h->override_fn) {
                continue; // overrides are collected below, outermost (registered last) first
            }
            if (count < hooks.size()) {
                hooks[count++] = h;
            }
        }
        for (auto o = it->second.rbegin(); o != it->second.rend(); ++o) {
            if ((*o)->override_fn && chain.count < chain.list.size()) {
                chain.list[chain.count++] = *o;
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        ForgeHook* h = hooks[i];
        if (!h->pre || !h->enabled.load(std::memory_order_relaxed) || !Enter(h->owner)) {
            continue;
        }
        ForgeHookAction action;
        {
            CallbackScope scope{h->owner, pc, "pre"};
            action = h->pre(cpu, h->user);
        }
        Leave(h->owner);
        if (action == FORGE_HOOK_RETURN) {
            SetReturnPc(mcpu, mcpu->r[14]); // skipped: straight back to the caller
            return true;
        }
    }

    if (chain.count) {
        // the function is replaced: the override runs now (and calls the original if it wants
        // to), the post hooks then run once, here, as they would when the function returns
        chain.entry = *cpu;
        std::array<u32, 6> entry_args{};
        std::copy_n(mcpu->r, 4, entry_args.begin());
        entry_args[4] = mcpu->r[14];
        entry_args[5] = mcpu->r[13];
        OverrideChain* const outer = t_chain;
        t_chain = &chain;
        const bool ran = RunOverrideFrom(chain, 0, cpu);
        t_chain = outer;
        if (ran) {
            std::vector<ForgeHook*> posts;
            for (size_t i = 0; i < count; ++i) {
                if (hooks[i]->post) {
                    posts.push_back(hooks[i]);
                }
            }
            RunPosts(cpu, posts, entry_args.data());
            SetReturnPc(mcpu, entry_args[4]);
            return true;
        }
        // every override is off or being unloaded: the function runs normally below
    }

    Frame frame;
    for (size_t i = 0; i < count; ++i) {
        if (hooks[i]->post && hooks[i]->enabled.load(std::memory_order_relaxed)) {
            frame.posts.push_back(hooks[i]);
        }
    }
    if (frame.posts.empty()) {
        return false;
    }
    frame.lr = mcpu->r[14];
    frame.sp = mcpu->r[13];
    std::copy_n(mcpu->r, 4, frame.args.begin());
    frame.args[4] = frame.lr;
    frame.args[5] = frame.sp;
    {
        std::scoped_lock lk{g_frames_lock};
        auto& stack = g_frames[thread_key];
        // Frames of functions left without returning (longjmp) sit above the current stack.
        while (!stack.empty() && stack.back().sp < frame.sp) {
            stack.pop_back();
        }
        stack.push_back(std::move(frame));
    }
    mcpu->r[14] = MODHOST_RETURN_PC;
    return false;
}

void OnShutdown() {
    g_watch_stop = true;
    if (g_watcher.joinable()) {
        g_watcher.join();
    }
    OverlayShutdown();
    std::vector<Plugin> plugins;
    {
        std::scoped_lock lk{g_plugins_lock};
        plugins.swap(g_plugins);
    }
    for (auto& p : plugins) {
        if (p.on_unload) {
            CallbackScope scope{p.state, 0, "unload"};
            p.on_unload();
        }
    }
    TraceShutdown();
    CrashClean(); // reached the end without a crash: no marker for the next launch
    std::scoped_lock lk{g_log_lock};
    g_log_file.flush();
}

} // namespace

std::string ConfigValue(const char* section, const char* key) {
    std::scoped_lock lk{g_config_lock};
    const auto s = g_config.find(section);
    if (s == g_config.end()) {
        return {};
    }
    const auto k = s->second.find(key);
    return k == s->second.end() ? std::string{} : k->second;
}

bool ConfigSet(const char* section, const char* key, const char* value) {
    {
        std::scoped_lock lk{g_config_lock};
        g_config[section][key] = value;
    }
    if (!WriteIniValue(PathU8(g_loader_dir + "/forge.ini"), section, key, value)) {
        Log(FORGE_LOG_WARN, "forge.ini: could not save %s/%s", section, key);
        return false;
    }
    return true;
}

IniMap ReadIni(const std::filesystem::path& path) {
    return ParseIni(path);
}

bool IniSet(const std::filesystem::path& path, const char* section, const char* key,
            const char* value) {
    return WriteIniValue(path, section, key, value);
}

uint32_t MainBase() {
    return g_main;
}

std::string FormatGuestAddr(uint32_t addr) {
    char buf[48];
    for (const auto& m : g_modules) {
        if (addr >= m.base && addr - m.base < m.size) {
            std::snprintf(buf, sizeof(buf), "%s+%#x", m.alias.empty() ? m.name.c_str()
                                                                       : m.alias.c_str(),
                          addr - m.base);
            return buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "%#x", addr);
    return buf;
}

ForgeHook* HookAdd(uint32_t target, ForgeHookPre pre, ForgeHookPost post, void* user,
                   bool front) {
    return AddHook(target, pre, post, nullptr, user, nullptr, front);
}

void HookRemove(ForgeHook* hook) {
    if (!hook) {
        return;
    }
    hook->enabled.store(false);
    std::unique_lock lk{g_hooks_lock};
    if (const auto it = g_hooks.find(hook->target); it != g_hooks.end()) {
        std::erase(it->second, hook); // a dispatch that copied it already sees it disabled
    }
}

bool IsCodeBlock(uint32_t guest_addr) {
    return g_host && g_host->is_block(guest_addr);
}

uint32_t FrameCount() {
    return g_frame_count.load(std::memory_order_relaxed);
}

uint32_t CurrentThreadKey() {
    return t_thread_key;
}

std::vector<ModInfo> ListMods() {
    std::vector<ModEntry> mods;
    {
        std::scoped_lock lk{g_mods_lock};
        mods = g_mod_list;
    }
    std::vector<ModInfo> out;
    for (const auto& m : mods) {
        ModInfo info;
        info.name = m.name;
        info.plugins = m.files.size();
        info.has_romfs = m.has_romfs;
        info.note = m.note;
        info.enabled = ModEnabled(m.name);
        {
            std::scoped_lock lk{g_plugins_lock};
            for (const auto& p : g_plugins) {
                info.loaded += p.mod == m.name;
            }
        }
        // a switch is queued: it is applied between two game frames
        {
            std::scoped_lock lk{g_reload_lock};
            for (const auto& [mod, on] : g_mod_requests) {
                info.busy |= mod == m.name;
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

void SetModNote(const std::string& mod, const std::string& note) {
    std::scoped_lock lk{g_mods_lock};
    g_mod_notes[mod] = note;
    for (auto& m : g_mod_list) {
        if (m.name == mod) {
            m.note = note;
        }
    }
}

void RequestResetScripts() {
    g_reset_req = true;
}

std::string ResetScriptsMessage() {
    std::scoped_lock lk{g_mods_lock};
    return g_reset_msg;
}

void SetModEnabled(const std::string& name, bool enabled) {
    ConfigSet("mods", name.c_str(), enabled ? "on" : "off");
    std::scoped_lock lk{g_reload_lock};
    g_mod_requests.emplace_back(name, enabled);
}

void Log(ForgeLogLevel level, const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    WriteLog(level, "forge", buf);
}

const std::string& LoaderDir() {
    return g_loader_dir;
}

const std::string& ModsDirPath() {
    return g_mods_dir;
}

void LogFault(const char* line) {
    // from a fault handler: never wait for a lock the faulting thread may hold itself
    if (g_host) {
        const std::string text = std::string("[forge] [crash] ") + line;
        g_host->log(MODHOST_LOG_ERROR, text.c_str());
    }
    if (g_log_lock.try_lock()) {
        if (g_log_file.is_open()) {
            g_log_file << "[error] [crash] " << line << std::endl;
        }
        g_log_lock.unlock();
    }
}

void ForEachPluginUi(const std::function<void(size_t, const PluginUi&)>& fn) {
    std::scoped_lock lk{g_plugins_lock};
    for (size_t i = 0; i < g_plugins.size(); ++i) {
        fn(i, g_plugins[i].ui);
    }
}

size_t PluginCount() {
    std::scoped_lock lk{g_plugins_lock};
    return g_plugins.size();
}

void RequestPluginReload(size_t index) {
    std::filesystem::path source;
    {
        std::scoped_lock lk{g_plugins_lock};
        if (index >= g_plugins.size()) {
            return;
        }
        source = g_plugins[index].source;
    }
    RequestReload(source);
}

bool HotReloadOn() {
    return g_watcher.joinable();
}

} // namespace forge

extern "C" MODHOST_EXPORT bool modhost_attach(const ModHostApi* api, ModHostCallbacks* cb) {
    using namespace forge;
    if (!api || !cb || api->version < 1 || !MODHOST_HAS(ModHostApi, api, set_text_input)) {
        return false; // an exe older than this loader's ABI
    }
    if (!api->game_title_id || std::strcmp(api->game_title_id, kMhguTitle) != 0) {
        api->log(MODHOST_LOG_WARN, "[forge] not MHGU (0100770008DD8000): Forge stays off");
        return false;
    }
    g_host = api;
    g_loader_dir = api->loader_dir ? api->loader_dir : ".";
    g_mods_dir = api->mods_dir ? api->mods_dir : ".";
    LoadConfig();
    OverlayConfigure();
    const std::string level = ConfigValue("log", "level");
    g_level = level == "debug" ? FORGE_LOG_DEBUG
              : level == "warn" ? FORGE_LOG_WARN
              : level == "error" ? FORGE_LOG_ERROR
                                 : FORGE_LOG_INFO;
    {
        std::scoped_lock lk{g_log_lock};
        g_log_file.open(PathU8(g_loader_dir + "/forge.log"), std::ios::trunc);
    }
    CrashCheckLastSession(); // before any plugin loads: may switch the guilty mod off
    CrashInit();

    cb->version = MODHOST_API_VERSION;
    cb->on_game_start = OnGameStart;
    cb->on_dispatch = OnDispatch;
    cb->on_present = OnPresent;
    cb->on_input = OnInput;
    cb->on_shutdown = OnShutdown;
    Log(FORGE_LOG_INFO, "attached (exe ABI %u, game %s %s)", api->version, api->game_title_id,
        api->game_version ? api->game_version : "");
    if (MODHOST_HAS(ModHostApi, api, guest_heap_size)) {
        Log(FORGE_LOG_INFO, "guest memory for mods: %u MB at %#x", api->guest_heap_size >> 20,
            api->guest_heap_start);
    } else {
        Log(FORGE_LOG_INFO, "guest memory for mods: none (exe older than mod host ABI 2)");
    }
    return true;
}
