// UI Fix: destretches MHGU's UI on wide screens and scales / moves its elements (a Forge PC plugin).
// SPDX-License-Identifier: MIT
//
// One mod instead of the earlier HUD Fix + UI Scale pair (same hooks, one draw stack, one ini).
//
// The game lays every screen out in a 1280x720 canvas (one rGUI layout per HUD part / menu) that
// is stretched over the whole window, so on 21:9 everything is too wide. The canvas is left alone
// (the game places some pieces from its centre); the UI is changed where it is drawn:
//  1. Destretch: each layout is squeezed horizontally by 16:9 / aspect around an anchor (left edge,
//     centre or right edge) chosen per layout ([Layouts] in uifix.ini): the world matrix every
//     piece uploads for the GPU (0xA3E3B0, 0xA4EB9C, text 0xA3DA50) and every clip rectangle
//     (0xA37988, 0xA4F420) are squeezed, so the game's own positions never change and things that
//     track the world don't drift. A few layouts are squeezed game-side (" game"): the matrices
//     built for their top-level pieces (0xA36FA4, 0xA4AD5C, 0xA4C2B4) change, so what the game
//     derives from them (masks of scrolling icons) follows. "self" = squeezed around the point where
//     the game places the element (name tags, popups). "Constrain UI to 16:9" centres everything
//     that isn't kept / self (equal margins). The aspect is the window's (Forge 0.3 screen_size).
//  2. Scale / move: every screen, HUD part or menu is made of elements, the instances of the layout
//     file (Nul_* groups, Anm_* animations), named there ("Nul_hud0", "Nul_timer"...). The UI
//     inspector (Forge menu > UI Fix) shows them as boxes over the game: hover for the name, click
//     to select, then Scale / drag. Settings go to uifix.ini [Elements] as
//     "<layout> / <element> = scale move_x move_y [anchor_x anchor_y]" (unnamed: "#<instance id>").
//     A global scale shrinks or grows every layout's top-level elements, each around its own
//     corner/edge nearest the screen edge; an element's own scale replaces it. Full-screen pictures
//     and the layouts listed under exclude are left alone.
// First start (no uifix.ini): settings of the earlier HudFix / UIScale mods are taken over when
// they are installed; otherwise every screen gets the one default (presets/uifix_layouts.ini +
// presets/uifix_default.ini). The menu is simple by default; "Advanced" shows the element table
// and the debug part.

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <imgui.h>

#include "forge_pc.h"

FORGE_PLUGIN_DEFINE_API

namespace {

// rGUI (GUI layout resource)
constexpr uint32_t kGuiFactory = 0xB7F1D4; // returns a new rGUI in r0
constexpr uint32_t kGuiDtor1 = 0xB7F260;   // r0 = rGUI
constexpr uint32_t kGuiDtor2 = 0xB7F2E0;
constexpr uint32_t kGuiVptr = 0x1794C9C; // main offset stored at +0
constexpr uint32_t kViewW = 0x70;
constexpr uint32_t kViewH = 0x74;
constexpr uint32_t kGuiFile = 0x64; // rGUI: loaded .gui file
constexpr uint32_t kGuiPath = 0x08; // MtResource path, char[64]

void Logf(ForgeLogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    forge_api->log(level, "uifix", buf);
}

float Float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string g_ini_path; // uifix.ini beside the dll
std::string g_dir;      // the dll's folder (with trailing slash; dumps go there too)

// --- live layouts (rGUI objects) ---
std::mutex g_gui_lock;
std::set<uint32_t> g_guis;                  // live rGUI objects
std::map<uint32_t, std::string> g_gui_keys; // rGUI -> key (guarded by g_gui_lock)
std::atomic<uint32_t> g_gui_epoch{0};       // bumped when layouts come or go (caches keyed by rGUI)
uint32_t g_vptr{};                          // main + kGuiVptr
std::atomic<bool> g_sweep_started{false};
std::atomic<bool> g_sweep_done{false};
std::set<uint32_t> g_bump;                                     // layouts to re-lay out (g_gui_lock)
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_widened;   // ours -> canvas size we wrote (g_gui_lock)

void OnGuiCreated(ForgeCpu* cpu, const uint32_t[4], void*) {
    uint32_t vt = 0;
    if (forge_api->mem_read(cpu->r[0], &vt, 4) && vt == g_vptr) {
        std::scoped_lock lk{g_gui_lock};
        g_guis.insert(cpu->r[0]);
        ++g_gui_epoch;
    }
}

ForgeHookAction OnGuiDestroyed(ForgeCpu* cpu, void*) {
    std::scoped_lock lk{g_gui_lock};
    g_guis.erase(cpu->r[0]);
    ++g_gui_epoch;
    g_gui_keys.erase(cpu->r[0]);
    g_widened.erase(cpu->r[0]);
    g_bump.erase(cpu->r[0]);
    return FORGE_HOOK_CONTINUE;
}

/// Layouts made before Forge loaded: every object in memory that starts with the rGUI vptr and
/// still has a plausible view size.
void Sweep() {
    std::vector<uint32_t> found;
    constexpr uint32_t kPage = 0x1000;
    for (uint64_t page = 0; page < 0x100000000ull; page += kPage) {
        const auto* p = static_cast<const uint32_t*>(
            forge_api->mem_ptr(static_cast<uint32_t>(page), kPage));
        if (!p) {
            continue;
        }
        for (uint32_t i = 0; i + kViewH / 4 < kPage / 4; ++i) {
            if (p[i] == g_vptr && p[i + kViewH / 4] >= 240 && p[i + kViewH / 4] <= 4320) {
                found.push_back(static_cast<uint32_t>(page) + i * 4);
            }
        }
    }
    {
        std::scoped_lock lk{g_gui_lock};
        g_guis.insert(found.begin(), found.end());
    }
    Logf(FORGE_LOG_INFO, "sweep: %zu layouts already loaded", found.size());
    g_sweep_done = true;
}

std::string LayoutKey(std::string p) {
    const auto pos = p.find("GUI\\");
    return pos == std::string::npos ? p : p.substr(pos);
}

/// Layout key of a tracked rGUI (its path is set when the file loads; cached once known).
const std::string& GuiKey(uint32_t gui) {
    std::string& k = g_gui_keys[gui];
    if (k.empty()) {
        char path[65]{};
        if (forge_api->mem_read(gui + kGuiPath, path, 64) && path[0]) {
            k = LayoutKey(path);
        }
    }
    return k;
}

// The game moves the minimap itself while a loading screen is up: the map's rules and the global
// scale are left off it then, and apply again once the loading layout stops drawing (it snaps back).
std::atomic<int64_t> g_load_ms{-1000000};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 1 = the loading screen's layout, 2 = the minimap (cached per rGUI, per thread).
int GuiRole(uint32_t gui) {
    thread_local std::unordered_map<uint32_t, int> cache;
    thread_local uint32_t epoch = ~0u;
    if (epoch != g_gui_epoch.load(std::memory_order_relaxed)) {
        cache.clear();
        epoch = g_gui_epoch.load(std::memory_order_relaxed);
    }
    if (!gui) {
        return 0;
    }
    const auto it = cache.find(gui);
    if (it != cache.end()) {
        return it->second;
    }
    std::string low;
    {
        std::scoped_lock lk{g_gui_lock};
        low = GuiKey(gui);
    }
    if (low.empty()) {
        return 0; // path not known yet: don't cache
    }
    for (char& ch : low) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const int role = low.find("\\load\\load") != std::string::npos ? 1
                     : low.find("\\map\\") != std::string::npos && low.find("dn") != std::string::npos ? 2
                                                                                                        : 0;
    cache[gui] = role;
    return role;
}

/// True while a loading screen is up and the layout being drawn is the minimap.
bool MapLoading(uint32_t gui) {
    // the loading layout draws every frame while it is up: a short window (not ~0.4 s) keeps the
    // map in the game's hands only as long as it is really loading
    return NowMs() - g_load_ms.load(std::memory_order_relaxed) < 60 && GuiRole(gui) == 2;
}

/// (layout path hash, instance id) of an element: the same element in a layout the game freed and
/// loaded again (the map after a loading screen is a new rGUI at a new address).
uint64_t ElemIdent(uint32_t node) {
    thread_local std::unordered_map<uint32_t, uint64_t> cache;
    thread_local uint32_t epoch = ~0u;
    if (epoch != g_gui_epoch.load(std::memory_order_relaxed)) {
        cache.clear();
        epoch = g_gui_epoch.load(std::memory_order_relaxed);
    }
    const auto it = cache.find(node);
    if (it != cache.end()) {
        return it->second;
    }
    uint32_t gui = 0, id = 0;
    forge_api->mem_read(node + 0x70, &gui, 4);
    forge_api->mem_read(node + 0x04, &id, 4);
    std::string key;
    if (gui) {
        std::scoped_lock lk{g_gui_lock};
        key = GuiKey(gui);
    }
    if (key.empty()) {
        return 0; // path not known yet: ask again next time
    }
    uint64_t h = 1469598103934665603ull; // FNV-1a: the same in every run (the memory is saved to disk)
    for (const char ch : key) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    }
    const uint64_t ident = (h << 32) ^ id ^ (1ull << 63);
    cache[node] = ident;
    return ident;
}

// --- settings (uifix.ini) ---
// per element (an instance of a layout: "GUI\01_quest\hud\hud0 / Nul_timer"): scale around its
// own anchor, then move by dx/dy canvas units. The anchor is locked once (ax/ay = its offset from
// the element's own position), so parts popping in or out of the element can't shift it.
// The layout part of a key may hold wildcards: ? = one character, * = any ("map\m??\map??dn").
struct ElemCfg {
    float scale{1.0f}, dx{0.0f}, dy{0.0f};
    bool anchored{false};
    float ax{0.0f}, ay{0.0f};
    bool Identity() const {
        return scale == 1.0f && dx == 0.0f && dy == 0.0f;
    }
};
std::mutex g_mode_lock;
std::map<std::string, ElemCfg> g_elem_cfg; // g_mode_lock
std::set<std::string> g_exclude;           // layouts the global scale skips (g_mode_lock)
std::atomic<uint32_t> g_elem_ver{1};       // bumped on every change
std::atomic<bool> g_on{true};
std::atomic<bool> g_advanced{false}; // the full inspector (depth, dumps, element table)
std::atomic<float> g_global{1.0f};
std::atomic<float> g_global_eff{1.0f}; // g_global, or 1 while off

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

// Destretch: what each layout does on a wide screen. keep = original (stretched; full-screen
// pictures, the 3D gear preview), left / center / right = squeezed and pinned there, follow = old
// tag mode, self = squeezed around the point where the game places the element (name tags,
// popups on the world). game-side layouts are squeezed by changing the positions the game computes
// (clip boxes and masks derived from them follow) instead of at draw time; only for layouts that
// don't track anything (they would drift).
enum class Mode { Keep, Left, Center, Right, Follow, Self };
constexpr const char* kModeNames[] = {"keep", "left", "center", "right", "follow", "self"};
constexpr int kModeCount = 6;
std::mutex g_layout_lock;            // taken after g_mode_lock when both are needed
std::map<std::string, Mode> g_modes; // layout key ("GUI\01_quest\hud\hud0") -> mode (g_layout_lock)
std::set<std::string> g_gameside;    // g_layout_lock
std::atomic<bool> g_destretch{true};
std::atomic<bool> g_simple{true};   // destretch = one squeeze around the screen centre, at draw time only
std::atomic<bool> g_elements{true}; // the per-element scale / move rules are applied (the global scale is separate)
std::atomic<int> g_place{1}; // 0 = anchor to edges (per layout), 1 = constrain UI to 16:9
std::atomic<float> g_aspect{16.0f / 9.0f};

Mode ParseMode(const char* v) {
    if (!std::strncmp(v, "left", 4) || !std::strncmp(v, "split", 5)) {
        return Mode::Left;
    }
    if (!std::strncmp(v, "center", 6) || !std::strncmp(v, "centre", 6)) {
        return Mode::Center;
    }
    if (!std::strncmp(v, "right", 5)) {
        return Mode::Right;
    }
    if (!std::strncmp(v, "follow", 6)) {
        return Mode::Follow;
    }
    if (!std::strncmp(v, "self", 4)) {
        return Mode::Self;
    }
    return Mode::Keep;
}

/// The mode the player chose for a layout (unlisted: keep).
Mode ListedMode(const std::string& key) {
    std::scoped_lock lk{g_layout_lock};
    const auto it = g_modes.find(key);
    return it == g_modes.end() ? Mode::Keep : it->second;
}

bool GameSide(const std::string& key) {
    std::scoped_lock lk{g_layout_lock};
    return g_gameside.count(key) != 0;
}

/// The mode in effect (constrain to 16:9 centres everything that isn't kept).
Mode ModeOf(const std::string& key) {
    const Mode m = ListedMode(key);
    if (g_simple.load()) { // squeezed around the centre; the table is ignored except self (world-placed tags)
        return m == Mode::Self ? m : Mode::Center;
    }
    return (g_place.load() == 1 && m != Mode::Keep && m != Mode::Follow && m != Mode::Self)
               ? Mode::Center
               : m;
}

enum : int { kReadSettings = 1, kReadLayouts = 2, kReadElements = 4, kReadAll = 7 };

/// Sections of an ini ([Settings], [Layouts], [Elements]) into the settings; `what` picks which are
/// read. Settings lines outside any section (the old hudfix.ini) are read too. The caller holds
/// g_mode_lock when elements are read. False when the file can't be opened.
bool ReadIni(const std::string& path, int what) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        return false;
    }
    char line[512];
    std::string section;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            section = Trim(line);
            continue;
        }
        if (line[0] == ';' || line[0] == '#') {
            continue;
        }
        char* eq = std::strchr(line, '=');
        if (!eq) {
            continue;
        }
        const std::string key = Trim(std::string(line, eq - line));
        const std::string val = Trim(eq + 1);
        if (key.empty()) {
            continue;
        }
        if (section == "[Elements]") {
            if (what & kReadElements) {
                ElemCfg c;
                const int n = std::sscanf(val.c_str(), "%f %f %f %f %f", &c.scale, &c.dx, &c.dy, &c.ax, &c.ay);
                c.anchored = n >= 5;
                if (n >= 1 && c.scale >= 0.1f && c.scale <= 5.0f) {
                    g_elem_cfg[key] = c;
                }
            }
        } else if (section == "[Layouts]") {
            if (what & kReadLayouts) {
                std::scoped_lock lk{g_layout_lock};
                g_modes[key] = ParseMode(val.c_str());
                if (val.find("game") != std::string::npos) {
                    g_gameside.insert(key);
                } else {
                    g_gameside.erase(key);
                }
            }
        } else if (what & kReadSettings) {
            if (key == "enabled") {
                g_on = val != "false" && val != "0";
            } else if (key == "advanced") {
                g_advanced = val == "true" || val == "1";
            } else if (key == "destretch") {
                g_destretch = val != "false" && val != "0";
            } else if (key == "simple_destretch") {
                g_simple = val != "false" && val != "0";
            } else if (key == "elements") {
                g_elements = val != "false" && val != "0";
            } else if (key == "placement") {
                g_place = std::atoi(val.c_str()) == 0 ? 0 : 1;
            } else if (key == "global_scale") {
                const float s = static_cast<float>(std::atof(val.c_str()));
                if (s >= 0.25f && s <= 3.0f) {
                    g_global = s;
                }
            } else if (key == "exclude" && !val.empty()) {
                g_exclude.insert(val);
            }
        }
    }
    std::fclose(f);
    return true;
}

void SaveIni() {
    FILE* f = std::fopen(g_ini_path.c_str(), "w");
    if (!f) {
        return;
    }
    std::scoped_lock lk{g_mode_lock};
    std::fprintf(f, "; UI Fix settings (edited from the Forge menu > UI Fix; edit by hand only with\n"
                    "; the game closed: the menu rewrites this file).\n[Settings]\n");
    std::fprintf(f, "enabled = %s\n", g_on.load() ? "true" : "false");
    std::fprintf(f, "; advanced = the layout table and the full UI inspector in the menu\n");
    std::fprintf(f, "advanced = %s\n", g_advanced.load() ? "true" : "false");
    std::fprintf(f, "; destretch = undo the game's horizontal stretching on screens wider than 16:9\n");
    std::fprintf(f, "destretch = %s\n", g_destretch.load() ? "true" : "false");
    std::fprintf(f, "; simple_destretch = squeeze the whole UI evenly left and right around the screen centre\n"
                    "; (draw time only; keep / self layouts excepted); false = the per-layout modes below\n");
    std::fprintf(f, "simple_destretch = %s\n", g_simple.load() ? "true" : "false");
    std::fprintf(f, "; elements = apply the per-element rules in [Elements] below (false = ignore them, kept in the file); global_scale is separate\n");
    std::fprintf(f, "elements = %s\n", g_elements.load() ? "true" : "false");
    std::fprintf(f, "; placement: 0 = anchor each layout to its screen edge, 1 = constrain the UI to 16:9\n"
                    "; (equal margins)\n");
    std::fprintf(f, "placement = %d\n", g_place.load());
    std::fprintf(f, "; every layout's top-level elements, each around its corner/edge nearest the screen\n"
                    "; edge (an element's own scale below replaces it); 1 = original size\n");
    std::fprintf(f, "global_scale = %.3f\n", g_global.load());
    std::fprintf(f, "; layouts the global scale leaves alone (one per line; full-screen pictures are\n"
                    "; always left alone)\n");
    for (const auto& e : g_exclude) {
        std::fprintf(f, "exclude = %s\n", e.c_str());
    }
    std::fprintf(f, "\n; Destretch per layout: keep = original (stretched, for full-screen pictures),\n"
                    "; left / center / right = unstretched and pinned to that part of the screen, self =\n"
                    "; unstretched around its own position (name tags, popups). \" game\" after the mode =\n"
                    "; squeeze the game's own positions (clip boxes and 3D views follow), for layouts that\n"
                    "; don't track anything. Unlisted layouts are kept.\n[Layouts]\n");
    {
        std::scoped_lock lk2{g_layout_lock};
        for (const auto& [key, m] : g_modes) {
            std::fprintf(f, "%s = %s%s\n", key.c_str(), kModeNames[static_cast<int>(m)],
                         g_gameside.count(key) ? " game" : "");
        }
    }
    std::fprintf(f, "\n; Elements (from the UI inspector): <layout> / <element> = scale move_x move_y [anchor_x anchor_y]\n"
                    "; (scale around the element's own anchor; move in 1280x720 canvas units; the anchor\n"
                    "; is locked on first use, relative to the element's position; #N = the element with\n"
                    "; instance id N in its layout file; ? and * in the layout match several layouts)\n"
                    "[Elements]\n");
    for (const auto& [key, c] : g_elem_cfg) {
        if (c.Identity()) {
            continue;
        }
        if (c.anchored) {
            std::fprintf(f, "%s = %.3f %.1f %.1f %.1f %.1f\n", key.c_str(), c.scale, c.dx, c.dy, c.ax, c.ay);
        } else {
            std::fprintf(f, "%s = %.3f %.1f %.1f\n", key.c_str(), c.scale, c.dx, c.dy);
        }
    }
    std::fclose(f);
}

// The default (first start, or "Default"): the destretch layouts (presets/uifix_layouts.ini) and
// the global scale + element rules (presets/uifix_default.ini), the same for every screen.
std::atomic<bool> g_need_preset{false}; // no uifix.ini yet: pick a default once the window is known

std::string PresetPath(const char* name) {
    return g_dir + "presets/" + name;
}

/// Replaces all settings with the default.
void ApplyPreset(float aspect) {
    bool elems = false, layouts = false;
    {
        std::scoped_lock lk{g_mode_lock};
        g_elem_cfg.clear();
        g_exclude.clear();
        g_exclude.insert("GUI\\04_lobby\\shop\\shop_preview"); // 3D view: its viewport won't follow
        g_global = 0.75f;
        g_destretch = true;
        g_place = 1;
        {
            std::scoped_lock lk2{g_layout_lock};
            g_modes.clear();
            g_gameside.clear();
        }
        layouts = ReadIni(PresetPath("uifix_layouts.ini"), kReadLayouts);
        const bool on = g_on.load(), adv = g_advanced.load();
        elems = ReadIni(PresetPath("uifix_default.ini"), kReadSettings | kReadElements);
        g_on = on; // the preset's switches don't override the player's
        g_advanced = adv;
        g_destretch = true;
        g_place = 1;
    }
    ++g_elem_ver;
    Logf(FORGE_LOG_INFO, "default (screen %.3f): layouts %s, elements %s", aspect, layouts ? "ok" : "MISSING",
         elems ? "ok" : "MISSING, global 0.75");
    SaveIni();
}

/// The window's aspect, 0 while unknown (Forge older than 0.3, or nothing presented yet).
float WindowAspect() {
    uint32_t w = 0, h = 0;
    if (forge_api->size >= offsetof(ForgeApi, screen_size) + sizeof(void*) && forge_api->screen_size &&
        forge_api->screen_size(&w, &h) && w >= 320 && h >= 240) {
        return static_cast<float>(w) / static_cast<float>(h);
    }
    return 0.0f;
}

/// uifix.ini; without one, the earlier HudFix / UIScale settings are taken over, or a default is
/// chosen once the window size is known (onUpdate).
void LoadIni() {
    std::scoped_lock lk{g_mode_lock};
    if (!ReadIni(g_ini_path, kReadAll)) {
        g_need_preset = true;
    }
}

/// The settings of the two mods this one replaces, when they are still installed beside it.
bool MigrateOld() {
    const std::string hf = g_dir + "../../HudFix/plugins/", us = g_dir + "../../UIScale/plugins/";
    std::scoped_lock lk{g_mode_lock};
    bool any = ReadIni(hf + "hudfix_layouts.ini", kReadLayouts);
    any |= ReadIni(hf + "hudfix.ini", kReadSettings); // placement
    any |= ReadIni(us + "uiscale.ini", kReadSettings | kReadElements);
    bool empty;
    {
        std::scoped_lock lk2{g_layout_lock};
        empty = g_modes.empty();
    }
    // ReadIni takes g_layout_lock itself; calling it under the lock deadlocked the first start.
    if (empty) {
        ReadIni(PresetPath("uifix_layouts.ini"), kReadLayouts);
    }
    return any;
}

/// Game thread: first start takes over the old mods' settings, else picks the default for the
/// window (after ~10 s without a window size: the 16:9 default).
void PickDefault() {
    static int waited = 0;
    static bool migrated = false;
    if (!g_need_preset.load()) {
        return;
    }
    if (!migrated) {
        migrated = true;
        if (MigrateOld()) {
            g_need_preset = false;
            ++g_elem_ver;
            SaveIni();
            Logf(FORGE_LOG_INFO, "took over the settings of the earlier HudFix / UIScale mods");
            return;
        }
    }
    const float a = WindowAspect();
    if (a == 0.0f && ++waited < 600) {
        return;
    }
    g_need_preset = false;
    ApplyPreset(a == 0.0f ? 16.0f / 9.0f : a);
}

// --- draw stack (which layout / animation instance is drawing) ---
constexpr uint32_t kDrawChildren = 0xA34070;
constexpr uint32_t kWorldUploads[] = {0xA3E3B0, 0xA4EB9C};
constexpr uint32_t kTextDraw = 0xA3DA50;
constexpr uint32_t kClipRects[] = {0xA37988, 0xA4F420};
constexpr uint32_t kInstRootVptr = 0x178F470;
uint32_t g_root_vptr{};

// --- layout files ---
std::string ReadStr(uint32_t addr, size_t max = 63) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!forge_api->mem_read(addr + static_cast<uint32_t>(i), &c, 1) || c == 0) {
            break;
        }
        if (c < 0x20 || c > 0x7E) {
            return {};
        }
        s += c;
    }
    return s;
}

/// A header field of a loaded .gui: offset in the file or (after fix-up) a pointer.
uint32_t FileAddr(uint32_t file, uint32_t size, uint32_t field) {
    uint32_t v = 0;
    forge_api->mem_read(file + field, &v, 4);
    return v < size ? file + v : v;
}

struct GuiFile {
    uint32_t gui, file, size, count, root, table, strings, params, kv32;
};

bool ReadGuiFile(uint32_t gui, GuiFile& g) {
    g.gui = gui;
    if (!forge_api->mem_read(gui + kGuiFile, &g.file, 4) || !g.file ||
        !forge_api->mem_read(g.file + 0x08, &g.size, 4) || g.size < 0x124 ||
        g.size > 0x4000000 || !forge_api->mem_read(g.file + 0x44, &g.count, 4) ||
        !forge_api->mem_read(g.file + 0x24, &g.root, 4) || g.count > 100000) {
        return false;
    }
    uint32_t magic = 0;
    forge_api->mem_read(g.file, &magic, 4);
    if (magic != 0x00495547) { // "GUI\0"
        return false;
    }
    g.table = FileAddr(g.file, g.size, 0xBC);
    g.strings = FileAddr(g.file, g.size, 0x104);
    g.params = FileAddr(g.file, g.size, 0xB4);
    g.kv32 = FileAddr(g.file, g.size, 0x110);
    return true;
}

/// A record's name. `text` is a string-table offset on disk; in game the names came back empty
/// (2026-10-07), so also try `text` as a pointer (fixed up at load) and as a file offset.
std::string RecordName(const GuiFile& g, uint32_t text) {
    if (text < g.size) {
        std::string s = ReadStr(g.strings + text);
        if (!s.empty()) {
            return s;
        }
        if (g.strings != g.file && !(s = ReadStr(g.file + text)).empty()) {
            return s;
        }
    }
    return text >= 0x10000 ? ReadStr(text) : std::string{};
}

/// Hex bytes at `addr` for the unnamed-element log line.
std::string HexAt(uint32_t addr, size_t n = 16) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = 0;
        if (!forge_api->mem_read(addr + static_cast<uint32_t>(i), &b, 1)) {
            return s + "(unreadable)";
        }
        char t[4];
        std::snprintf(t, sizeof(t), "%02X", b);
        s += t;
    }
    return s;
}

/// Float init param (PosX / PosY) of instance record `rec`; NaN if absent.
float RecordFloat(const GuiFile& g, uint32_t rec, const char* name) {
    uint32_t n = 0, first = 0;
    forge_api->mem_read(rec + 0x10, &n, 4);
    forge_api->mem_read(rec + 0x1C, &first, 4);
    for (uint32_t k = 0; k < n && k < 64; ++k) {
        const uint32_t p = g.params + (first + k) * 20;
        uint8_t type = 0;
        uint32_t text = 0;
        int32_t value_off = -1;
        forge_api->mem_read(p + 4, &type, 1);
        forge_api->mem_read(p + 0x0C, &text, 4);
        forge_api->mem_read(p + 0x10, &value_off, 4);
        if ((type == 2 || type == 15) && value_off >= 0 && ReadStr(g.strings + text) == name) {
            uint32_t v = 0;
            forge_api->mem_read(g.kv32 + static_cast<uint32_t>(value_off), &v, 4);
            return Float(v);
        }
    }
    return NAN;
}

/// Record of instance `id` in a layout file, 0 if none.
uint32_t FindRecord(const GuiFile& g, uint32_t id, uint32_t* index) {
    // one read of the whole table (mem_ptr's fast path found nothing in game: the inspector's
    // instance names all fell back to "#id" although the same table read word by word matched)
    std::vector<uint32_t> t(static_cast<size_t>(g.count) * 9);
    if (!t.empty() && forge_api->mem_read(g.table, t.data(), g.count * 36)) {
        for (uint32_t i = 0; i < g.count; ++i) {
            if (t[i * 9] == id) {
                *index = i;
                return g.table + i * 36;
            }
        }
        return 0;
    }
    for (uint32_t i = 0; i < g.count; ++i) {
        uint32_t rid = 0;
        if (forge_api->mem_read(g.table + i * 36, &rid, 4) && rid == id) {
            *index = i;
            return g.table + i * 36;
        }
    }
    return 0;
}

struct DrawLevel {
    uint32_t node, gui;
};
thread_local std::vector<DrawLevel> t_draw; // nested 0xA34070 calls on this thread
// 0xA34070 walks a whole instance tree itself (it only nests where a node draws another layout).
// Animation instances (vtables main+0x178F5B8/62C/6A4, draw = slot 15 = 0xA39300) draw their
// object tree from the holder at +0xDC (0xA494A0: holder +0x68 first object; objects: first child
// +0x68, next sibling +0x6C, parent +0x70, the top objects' parent = the holder).
constexpr uint32_t kAnimDraw = 0xA39300;
thread_local std::vector<uint32_t> t_inst; // animation instances being drawn (inspector)
// probe (inspector on): how often each layout is drawn and how many hooked uploads it makes; a
// layout drawn with no uploads draws through something not hooked here
struct LayoutCount {
    uint32_t draws{}, uploads{}, root{};
};
std::mutex g_lc_lock;
std::map<uint32_t, LayoutCount> g_lc; // by rGUI
extern std::atomic<bool> g_insp_on;

ForgeHookAction OnDrawChildren(ForgeCpu* cpu, void*) {
    const uint32_t node = cpu->r[0];
    uint32_t vp = 0, gui = 0;
    forge_api->mem_read(node, &vp, 4);
    if (vp == g_root_vptr) {
        {   // probe: which layout draws inside which (nested), and which root drew just before
            static thread_local uint32_t last_root_gui = 0;
            static thread_local std::set<uint64_t> seen;
            uint32_t c = 0, g = 0, par = 0, owner = 0;
            if (forge_api->mem_read(node + 0x60, &c, 4) && c) {
                forge_api->mem_read(c + 0x70, &g, 4);
            }
            forge_api->mem_read(node + 0x68, &par, 4);
            forge_api->mem_read(node + 0x6C, &owner, 4);
            if (g && seen.size() < 400) {
                const uint32_t host = !t_draw.empty() ? t_draw.front().gui : 0;
                const uint64_t k = (static_cast<uint64_t>(g) << 32) | (host ? host : last_root_gui);
                if (seen.insert(k).second) {
                    std::scoped_lock lk{g_gui_lock};
                    Logf(FORGE_LOG_INFO, "layout %s starts %s %s (parent %08X owner %08X)", GuiKey(g).c_str(),
                         host ? "INSIDE" : "after", GuiKey(host ? host : last_root_gui).c_str(), par, owner);
                }
            }
            if (g) {
                last_root_gui = g;
            }
        }
        t_draw.clear(); // a layout starts (also drops anything a missed exit left)
        t_inst.clear();
        uint32_t child = 0;
        if (forge_api->mem_read(node + 0x60, &child, 4) && child) {
            forge_api->mem_read(child + 0x70, &gui, 4);
        }
        if (GuiRole(gui) == 1) {
            g_load_ms.store(NowMs(), std::memory_order_relaxed);
        }
        if (gui && g_insp_on.load(std::memory_order_relaxed)) {
            std::scoped_lock lk{g_lc_lock};
            ++g_lc[gui].draws;
            g_lc[gui].root = node;
        }
    } else if (!t_draw.empty()) {
        gui = t_draw.back().gui;
    }
    if (t_draw.size() < 64) {
        t_draw.push_back({node, gui});
    }
    return FORGE_HOOK_CONTINUE;
}

void OnDrawChildrenDone(ForgeCpu*, const uint32_t args[4], void*) {
    if (!t_draw.empty() && t_draw.back().node == args[0]) {
        t_draw.pop_back();
    }
}

ForgeHookAction OnAnimDraw(ForgeCpu* cpu, void*) {
    if (t_inst.size() < 32) {
        t_inst.push_back(cpu->r[0]);
    }
    return FORGE_HOOK_CONTINUE;
}

void OnAnimDrawDone(ForgeCpu*, const uint32_t[4], void*) {
    if (!t_inst.empty()) {
        t_inst.pop_back();
    }
}

thread_local uint32_t t_last_slot{0}; // the slot the element scale changed last (never twice)

// --- destretch: squeezing at draw time ---
// The game's positions are never changed (trackers that read them back would drift). The GUI
// draws each piece's world matrix (r1 = piece + 0x10, canvas units, rows; x column = +0x00/+0x10/
// +0x20, translation +0x30) from a fresh constant-buffer slot, filled by 0xA3E3B0 or 0xA4EB9C
// (r0 = draw context), or inline by the text draw 0xA3DA50; the slot is at [[ctx + 4] + 0x18BC]
// afterwards. That copy is squeezed horizontally by 16:9 / aspect around the layout's anchor:
// x' = a + (x - a) * s. Which layout is drawing comes from the draw stack above (t_draw).
// follow (game-placed tags): anchored at the tag, the container just below top level.
struct LayoutShift {
    float ax, sx, sy; // x anchor (canvas units; < 0 = the piece's tag), scales
    bool game;        // squeezed game-side (positions) instead of at draw time
    bool operator==(const LayoutShift&) const = default;
};
constexpr float kAnchorSelf = -2.0f; // LayoutShift::ax for self
std::shared_mutex g_table_lock;
std::unordered_map<uint32_t, LayoutShift> g_table; // rGUI -> squeeze (only placed layouts)

thread_local uint32_t t_last_squeezed{0}; // the slot squeezed last (never squeeze one twice)

/// The squeeze for what is being drawn now: the layout from the draw stack, or (outside a layout
/// draw) from a container's own rGUI field (+0x70). Anchors in canvas units.
bool CurrentShift(uint32_t container, LayoutShift& sh, float& ax, float& ay) {
    uint32_t gui = 0;
    if (!t_draw.empty() && t_draw.front().gui) {
        gui = t_draw.front().gui;
    } else if (!container || !forge_api->mem_read(container + 0x70, &gui, 4) || !gui) {
        return false;
    }
    {
        std::shared_lock lk{g_table_lock};
        const auto it = g_table.find(gui);
        if (it == g_table.end()) {
            return false;
        }
        sh = it->second;
    }
    if (sh.game) {
        return false; // squeezed game-side already
    }
    if (sh.ax == kAnchorSelf) {
        ax = ay = 0.0f; // resolved by the caller (SelfAnchor)
        return true;
    }
    // t_draw[0] = root, [1] = a top-level container, [2] = a container below it (a tag)
    if (sh.ax < 0.0f) {
        if (t_draw.empty()) {
            return false;
        }
        const uint32_t tag = t_draw[t_draw.size() > 2 ? 2 : t_draw.size() - 1].node;
        float t[2]{};
        forge_api->mem_read(tag + 0x40, t, sizeof(t));
        ax = t[0];
        ay = t[1];
    } else {
        float ty = 0.0f;
        if (t_draw.size() > 1) {
            forge_api->mem_read(t_draw[1].node + 0x44, &ty, 4);
        }
        ax = sh.ax;
        ay = ty < 360.0f ? 0.0f : 720.0f;
    }
    return true;
}

// --- self: squeeze around the point where the game places the element ---
// The element = the layout's top-level instance (below the root) the piece is drawn in; a
// top-level group sitting at the canvas origin (several tags under one group) hands over to the
// next instance down. Its anchor = its own world position (instance matrix +0x40/+0x44, game
// coordinates = what is seen before squeezing), so a tag stays exactly where the game put it
// whatever pops in or out around it.
std::mutex g_self_lock;
std::unordered_set<uint32_t> g_self_told; // layouts whose anchor choice was logged once

bool SelfAnchor(uint32_t start, float& ax, float& ay) {
    uint32_t chain[12];
    int n = 0;
    for (uint32_t node = start; node && n < 12;) {
        uint32_t vp = 0, parent = 0;
        if (!forge_api->mem_read(node, &vp, 4) || vp == g_root_vptr ||
            !forge_api->mem_read(node + 0x68, &parent, 4)) {
            break;
        }
        chain[n++] = node; // innermost first
        node = parent;
    }
    for (int i = n - 1; i >= 0; --i) {
        float t[2]{};
        if (!forge_api->mem_read(chain[i] + 0x40, t, sizeof(t)) || !std::isfinite(t[0]) ||
            !std::isfinite(t[1]) || std::fabs(t[0]) > 8192.0f || std::fabs(t[1]) > 8192.0f) {
            continue;
        }
        if (i > 0 && std::fabs(t[0]) + std::fabs(t[1]) < 1.0f) {
            continue; // a group at the origin: its tags carry the real positions
        }
        ax = t[0];
        ay = t[1];
        uint32_t gui = 0, id = 0;
        forge_api->mem_read(chain[i] + 0x70, &gui, 4);
        std::scoped_lock lk{g_self_lock};
        if (g_self_told.size() < 256 && g_self_told.insert(gui).second) {
            forge_api->mem_read(chain[i] + 0x04, &id, 4);
            Logf(FORGE_LOG_INFO, "self: layout %08X anchors on instance #%u (depth %d of %d) at %.1f, %.1f",
                 gui, id, n - i, n, ax, ay);
        }
        return true;
    }
    return false;
}

void PieceBox(uint32_t piece, const float m[16], float b[4]);

/// Squeezes the world-matrix slot the draw context's renderer uploaded last.
void SqueezeSlot(uint32_t ctx, uint32_t piece) {
    LayoutShift sh;
    float ax, ay;
    if (!CurrentShift(0, sh, ax, ay)) {
        return;
    }
    if (!t_draw.empty() && MapLoading(t_draw.front().gui)) {
        sh.ax = kAnchorSelf; // loading screen: squeezed about itself, wherever the game put it
    }
    uint32_t renderer = 0, dst = 0;
    float m[16];
    if (!forge_api->mem_read(ctx + 0x04, &renderer, 4) ||
        !forge_api->mem_read(renderer + 0x18BC, &dst, 4) || !dst || dst == t_last_squeezed ||
        !forge_api->mem_read(dst, m, sizeof(m))) {
        return;
    }
    t_last_squeezed = dst;
    {
        // a piece that covers the whole 16:9 canvas (backgrounds, pictures, fades) stays stretched
        float pb[4];
        PieceBox(piece, m, pb);
        if (pb[2] - pb[0] >= 1260.0f && pb[3] - pb[1] >= 700.0f) {
            return;
        }
    }
    if (sh.ax == kAnchorSelf && !SelfAnchor(t_inst.empty() ? piece : t_inst.back(), ax, ay)) {
        return;
    }
    if (sh.ax == kAnchorSelf) { // the element's position seen through this slot's matrix, as the scaler does
        float gm[16];
        if (forge_api->mem_read(piece + 0x10, gm, sizeof(gm)) && std::fabs(gm[0]) > 1e-4f &&
            std::fabs(gm[5]) > 1e-4f) {
            const float kx = m[0] / gm[0], ky = m[5] / gm[5];
            if (kx > 0.1f && kx < 10.0f && ky > 0.1f && ky < 10.0f) {
                ax = m[12] + (ax - gm[12]) * kx;
                ay = m[13] + (ay - gm[13]) * ky;
            }
        }
    }
    for (int r = 0; r < 3; ++r) {
        m[r * 4] *= sh.sx;
        m[r * 4 + 1] *= sh.sy;
    }
    m[12] = ax + (m[12] - ax) * sh.sx;
    m[13] = ay + (m[13] - ay) * sh.sy;
    forge_api->mem_write(dst, m, sizeof(m));
}

// clip rectangles: 0xA37988 (scissor-mask container: r0 = it, matrix +0x10, int size +0xC8/+0xCC)
// and 0xA4F420 (mask object) write the box of the transformed corners as ints {x0, y0, x1, y1}
// (canvas units) to r1. Squeezed like the pieces so masks still cover what they clip.
void SqueezeClip(const uint32_t args[4]) {
    LayoutShift sh;
    float ax, ay;
    int32_t r[4];
    if (!args[1] || !CurrentShift(args[0], sh, ax, ay)) {
        return;
    }
    if (!t_draw.empty() && MapLoading(t_draw.front().gui)) {
        sh.ax = kAnchorSelf;
    }
    if ((sh.ax == kAnchorSelf && !SelfAnchor(t_inst.empty() ? args[0] : t_inst.back(), ax, ay)) ||
        !forge_api->mem_read(args[1], r, sizeof(r))) {
        return;
    }
    if (r[2] - r[0] >= 1260 && r[3] - r[1] >= 700) {
        return; // a full-screen clip stays as the game made it, like the full-screen pieces it holds
    }
    float f[4];
    for (int i = 0; i < 4; ++i) {
        f[i] = static_cast<float>(r[i]);
    }
    f[0] = ax + (f[0] - ax) * sh.sx;
    f[2] = ax + (f[2] - ax) * sh.sx;
    f[1] = ay + (f[1] - ay) * sh.sy;
    f[3] = ay + (f[3] - ay) * sh.sy;
    for (int i = 0; i < 4; ++i) {
        r[i] = static_cast<int32_t>(std::floor(f[i] + 0.5f));
    }
    forge_api->mem_write(args[1], r, sizeof(r));
}

// game-side: after the game built a top-level piece's matrix (0xA36FA4 / 0xA4AD5C / 0xA4C2B4,
// r0 = the piece, r1 = the matrix it was built from; parent +0x68 = the layout root, +0x70 = rGUI)
// its matrix is squeezed in place; everything inside is built from it. Pieces built from an
// already squeezed matrix (x scale != 1) inherited it and are left alone.
constexpr uint32_t kMatrixBuilders[] = {0xA36FA4, 0xA4AD5C, 0xA4C2B4};
void OnMatrixBuilt(ForgeCpu*, const uint32_t args[4], void*) {
    const uint32_t node = args[0];
    uint32_t pg[3]{}; // +0x68 parent, +0x6C, +0x70 rGUI
    if (!forge_api->mem_read(node + 0x68, pg, sizeof(pg)) || !pg[2] || !pg[0]) {
        return;
    }
    LayoutShift sh;
    {
        std::shared_lock lk{g_table_lock};
        const auto it = g_table.find(pg[2]);
        if (it == g_table.end() || !it->second.game) {
            return;
        }
        sh = it->second;
    }
    uint32_t pv = 0;
    if (!forge_api->mem_read(pg[0], &pv, 4) || pv != g_root_vptr) {
        return;
    }
    float p[16];
    if (!args[1] || !forge_api->mem_read(args[1], p, sizeof(p)) ||
        std::fabs(p[0] - 1.0f) > 0.001f || std::fabs(p[12]) > 0.01f) {
        return;
    }
    float m[16];
    if (!forge_api->mem_read(node + 0x10, m, sizeof(m))) {
        return;
    }
    const float ay = m[13] < 360.0f ? 0.0f : 720.0f;
    for (int r = 0; r < 3; ++r) {
        m[r * 4] *= sh.sx;
        m[r * 4 + 1] *= sh.sy;
    }
    m[12] = sh.ax + (m[12] - sh.ax) * sh.sx;
    m[13] = ay + (m[13] - ay) * sh.sy;
    forge_api->mem_write(node + 0x10, m, sizeof(m));
}

// The game's render size: sFestaRender +0x44BAC0 / +0x44BAC4.
constexpr uint32_t kRenderW = 0x44BAC0;
constexpr uint32_t kRenderH = 0x44BAC4;
std::atomic<bool> g_screen_logged{false};

/// The aspect the UI is shown at: the window (swapchain) when Forge reports it, else the game's
/// render size. (The game may render 16:9 and be stretched to the window afterwards.)
void UpdateAspect() {
    const uint32_t render = forge_api->singleton_getByName("sFestaRender");
    uint32_t w = 0, h = 0;
    if (render && forge_api->mem_read(render + kRenderW, &w, 4) &&
        forge_api->mem_read(render + kRenderH, &h, 4) && w >= 320 && h >= 240 && w <= 16384 &&
        h <= 16384) {
        g_aspect = static_cast<float>(w) / static_cast<float>(h);
    }
    const float a = WindowAspect();
    if (a > 0.0f) {
        if (std::fabs(a - g_aspect.load()) > 0.001f && !g_screen_logged.exchange(true)) {
            Logf(FORGE_LOG_INFO, "window aspect %.4f (game renders %ux%u)", a, w, h);
        }
        g_aspect = a;
    }
}

/// The canvas stays 1280x720 (pieces the game places from the canvas centre, like window
/// backgrounds, drift when it is widened). It is only nudged 2 narrower for one frame: a changed
/// game-side placement shows once the game re-lays the layout out, which a canvas size change
/// triggers.
void ApplyFix() {
    std::scoped_lock lk{g_gui_lock};
    for (const uint32_t gui : g_guis) {
        uint32_t wh[2]{};
        if (!forge_api->mem_read(gui + kViewW, wh, 8)) {
            continue;
        }
        auto it = g_widened.find(gui);
        const bool ours = it != g_widened.end() && it->second.first == wh[0] &&
                          it->second.second == wh[1];
        const bool original = wh[0] == 1280 && wh[1] == 720;
        if (original && g_bump.erase(gui)) {
            const uint32_t v[2]{1278, 720};
            forge_api->mem_write(gui + kViewW, v, 8);
            g_widened[gui] = {1278, 720};
        } else if (it != g_widened.end()) {
            if (ours) {
                const uint32_t v[2]{1280, 720};
                forge_api->mem_write(gui + kViewW, v, 8);
            }
            g_widened.erase(it);
        }
    }
}

/// Rebuilds the squeeze table from the modes (every frame; draws pick it up right away). Squeeze
/// factor sx = (16/9) / aspect: screens narrower than 16:9 are left as they are.
void ApplyShifts() {
    std::unordered_map<uint32_t, LayoutShift> table;
    const float sx = std::min(1.0f, (16.0f / 9.0f) / g_aspect.load());
    const bool on = g_on.load() && g_destretch.load() && std::fabs(sx - 1.0f) > 0.005f;
    std::scoped_lock lk{g_gui_lock};
    if (on) {
        for (const uint32_t gui : g_guis) {
            const std::string& key = GuiKey(gui);
            if (key.empty()) {
                continue;
            }
            const Mode mode = ModeOf(key);
            LayoutShift sh{0.0f, sx, 1.0f, GameSide(key) && !g_simple.load()};
            if (sh.game && (mode == Mode::Follow || mode == Mode::Self)) {
                sh.game = false; // tags track things: game-side would drift
            }
            switch (mode) {
            case Mode::Keep:
                continue;
            case Mode::Center:
                sh.ax = 640.0f;
                break;
            case Mode::Right:
                sh.ax = 1280.0f;
                break;
            case Mode::Follow:
                sh.ax = -1.0f;
                break;
            case Mode::Self:
                sh.ax = kAnchorSelf;
                break;
            default:
                break;
            }
            table[gui] = sh;
        }
    }
    {
        // game-side squeezes only show once the game re-lays the layout out
        std::shared_lock lk2{g_table_lock};
        for (const uint32_t gui : g_guis) {
            const auto a = g_table.find(gui);
            const auto b = table.find(gui);
            const bool was = a != g_table.end() && a->second.game;
            const bool now = b != table.end() && b->second.game;
            if (was != now || (was && !(a->second == b->second))) {
                g_bump.insert(gui);
            }
        }
    }
    std::unique_lock lk3{g_table_lock};
    g_table.swap(table);
}

/// Advanced: the per-layout destretch table.
void LayoutsUi() {
    if (!ImGui::TreeNodeEx("Layouts on screen (destretch)", 0)) {
        return;
    }
    ImGui::TextWrapped("keep = original (stretched; for full-screen pictures). left / center / "
                       "right = unstretched, pinned there. self = unstretched where the game draws "
                       "it (name tags, popups on the world). follow = old tag mode. game-side = "
                       "squeeze the game's own positions so clip boxes and 3D views follow (not for "
                       "tags). Saved to uifix.ini [Layouts].");
    std::vector<std::string> live;
    {
        std::scoped_lock lk{g_gui_lock};
        std::set<std::string> keys;
        for (const uint32_t gui : g_guis) {
            const std::string& k = GuiKey(gui);
            if (!k.empty() && keys.insert(k).second) {
                live.push_back(k);
            }
        }
    }
    std::sort(live.begin(), live.end());
    bool changed = false;
    if (ImGui::BeginTable("layouts", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, 350))) {
        ImGui::TableSetupColumn("layout");
        ImGui::TableSetupColumn("mode");
        ImGui::TableSetupColumn("game-side", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& key : live) {
            ImGui::PushID(key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(key.c_str());
            ImGui::TableNextColumn();
            int m = static_cast<int>(ListedMode(key));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##m", &m, kModeNames, kModeCount)) {
                std::scoped_lock lk{g_layout_lock};
                g_modes[key] = static_cast<Mode>(m);
                changed = true;
            }
            ImGui::TableNextColumn();
            bool game = GameSide(key);
            if (ImGui::Checkbox("##g", &game)) {
                std::scoped_lock lk{g_layout_lock};
                if (game) {
                    g_gameside.insert(key);
                    g_modes.try_emplace(key, Mode::Keep);
                } else {
                    g_gameside.erase(key);
                }
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) {
        SaveIni();
    }
    ImGui::TreePop();
}

// --- per-element scale/move (draw time, after the destretch) ---
// An element = an instance of a layout (Nul_* group or Anm_*), named in the layout file. Rules
// are resolved to instance ids per loaded rGUI once per frame (RefreshElemTable). At an upload,
// the instances above the piece (from the drawing animation instance, or the piece itself, up
// via +0x68 to the root; each instance's +0x70 = its rGUI) are matched by id. Each matching
// element scales around its anchor: the side of its own box (as the layout placed it, measured
// from its pieces the frame before) nearest the screen edge, or its centre in the middle third.
struct ElemRule {
    uint32_t id;
    ElemCfg cfg;
    std::string key; // its uifix.ini key (to save the anchor once locked)
};
struct ElemLayout {
    uint32_t file{}, ver{};
    std::vector<ElemRule> rules;
};
std::shared_mutex g_elem_lock;
std::unordered_map<uint32_t, ElemLayout> g_elem_table; // rGUI -> rules (g_elem_lock)
std::atomic<bool> g_elem_any{false};
std::unordered_set<uint32_t> g_global_skip; // rGUIs the global scale leaves alone (g_elem_lock)
struct ElemBox {
    uint32_t gen{}, first{}; // first = the piece that starts its box each frame
    float box[4]{}, prev[4]{};
    bool has_prev{false};
    // the element's own position where it is drawn (canvas units, after the destretch) and the anchor
    // locked relative to it
    float origin[2]{}, off[2]{};
    bool has_origin{false}, locked{false};
    uint32_t epoch{};
    uint64_t ident{}; // (rGUI, instance id): the same element in another instance of the layout
};
// What each element looked like the last time it was seen, by identity: a screen that is opened
// again gets new instances, and without this its first frame draws full size (extent unknown yet).
struct LastPivot {
    float box[4], off[2];
};
std::map<uint64_t, LastPivot> g_last_piv;
std::atomic<bool> g_cache_dirty{false};
std::mutex g_ebox_lock;

/// The element memory is kept between games (uifix_cache.txt beside the dll): a menu seen before
/// is scaled from its first frame, even right after the game starts.
std::string CachePath() {
    return g_dir + "uifix_cache.txt";
}

void LoadCache() {
    FILE* f = std::fopen(CachePath().c_str(), "rb");
    if (!f) {
        return;
    }
    std::scoped_lock lk{g_ebox_lock};
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long id = 0;
        LastPivot p{};
        if (std::sscanf(line, "%llx %f %f %f %f %f %f", &id, &p.box[0], &p.box[1], &p.box[2], &p.box[3],
                        &p.off[0], &p.off[1]) == 7 &&
            id) {
            g_last_piv[id] = p;
        }
    }
    std::fclose(f);
}

void SaveCache() {
    std::map<uint64_t, LastPivot> copy;
    {
        std::scoped_lock lk{g_ebox_lock};
        copy = g_last_piv;
    }
    const std::string tmp = CachePath() + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return;
    }
    for (const auto& [id, p] : copy) {
        std::fprintf(f, "%llx %.2f %.2f %.2f %.2f %.2f %.2f\n", static_cast<unsigned long long>(id), p.box[0],
                     p.box[1], p.box[2], p.box[3], p.off[0], p.off[1]);
    }
    std::fclose(f);
    std::remove(CachePath().c_str());
    std::rename(tmp.c_str(), CachePath().c_str());
}
std::unordered_map<uint32_t, ElemBox> g_ebox; // instance -> its box before element transforms
std::atomic<uint32_t> g_frame_gen{1};
std::atomic<uint32_t> g_elem_applied{0};
std::atomic<uint32_t> g_anchor_epoch{1}; // bumped by "Re-anchor": runtime locks are redone
std::mutex g_pend_lock;
std::map<std::string, std::pair<float, float>> g_pend_anchor; // anchors locked, to save (game thread)

/// Wildcard match, case-insensitive: ? = one character, * = any run.
bool Glob(const char* p, const char* s) {
    const char* star = nullptr;
    const char* back = nullptr;
    while (*s) {
        if (*p == '*') {
            star = ++p;
            back = s;
        } else if (*p == '?' || std::tolower(static_cast<unsigned char>(*p)) ==
                                    std::tolower(static_cast<unsigned char>(*s))) {
            ++p;
            ++s;
        } else if (star) {
            p = star;
            s = ++back;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        ++p;
    }
    return !*p;
}

bool HasWild(const std::string& s) {
    return s.find_first_of("?*") != std::string::npos;
}

/// "GUI\01_quest\map\m01\map01dn" -> "GUI\01_quest\map\m??\map??dn" (every layout like it).
std::string SimilarLayouts(const std::string& layout) {
    std::string s = layout;
    const size_t from = s.find('\\', s.find('\\') + 1); // keep "GUI\01_quest" as it is
    for (size_t i = from == std::string::npos ? s.size() : from; i < s.size(); ++i) {
        if (s[i] >= '0' && s[i] <= '9') {
            s[i] = '?';
        }
    }
    return s;
}

/// Game thread, once per frame: element rules -> instance ids of the loaded layouts. A rule for
/// the exact layout wins over a wildcard rule for the same element.
void RefreshElemTable() {
    static std::unordered_map<uint32_t, ElemLayout> cache; // this thread only
    const uint32_t ver = g_elem_ver.load();
    struct Want {
        std::string name, key;
        ElemCfg cfg;
    };
    std::map<std::string, std::vector<Want>> by_layout; // exact layouts
    std::vector<std::pair<std::string, Want>> wild;     // (layout pattern, rule)
    {
        std::scoped_lock lk{g_mode_lock};
        for (const auto& [k, c] : g_elem_cfg) {
            const auto pos = k.find(" / ");
            if (c.Identity() || pos == std::string::npos) {
                continue;
            }
            Want w{k.substr(pos + 3), k, c};
            const std::string layout = k.substr(0, pos);
            if (HasWild(layout)) {
                wild.push_back({layout, std::move(w)});
            } else {
                by_layout[layout].push_back(std::move(w));
            }
        }
    }
    bool dirty = false;
    std::set<uint32_t> live;
    if (!by_layout.empty() || !wild.empty()) {
        std::scoped_lock lk{g_gui_lock};
        for (const uint32_t gui : g_guis) {
            const std::string& key = GuiKey(gui);
            if (key.empty()) {
                continue;
            }
            std::vector<const Want*> want;
            const auto exact = by_layout.find(key);
            if (exact != by_layout.end()) {
                for (const Want& w : exact->second) {
                    want.push_back(&w);
                }
            }
            for (const auto& [pat, w] : wild) {
                if (Glob(pat.c_str(), key.c_str())) {
                    want.push_back(&w);
                }
            }
            uint32_t file = 0;
            if (want.empty() || !forge_api->mem_read(gui + kGuiFile, &file, 4) || !file) {
                continue;
            }
            live.insert(gui);
            ElemLayout& el = cache[gui];
            if (el.file == file && el.ver == ver) {
                continue;
            }
            dirty = true;
            el = {file, ver, {}};
            GuiFile g;
            if (!ReadGuiFile(gui, g)) {
                el.file = 0; // retry next frame
                continue;
            }
            std::vector<uint32_t> t(static_cast<size_t>(g.count) * 9);
            if (t.empty() || !forge_api->mem_read(g.table, t.data(), g.count * 36)) {
                el.file = 0;
                continue;
            }
            for (uint32_t i = 0; i < g.count; ++i) {
                const std::string name = RecordName(g, t[i * 9 + 5]);
                const std::string by_id = "#" + std::to_string(t[i * 9]);
                for (const Want* w : want) { // exact rules come first: the first match wins
                    if (w->name == by_id || (!name.empty() && w->name == name)) {
                        el.rules.push_back({t[i * 9], w->cfg, w->key});
                        break;
                    }
                }
            }
        }
    }
    for (auto it = cache.begin(); it != cache.end();) {
        if (live.count(it->first)) {
            ++it;
        } else {
            it = cache.erase(it);
            dirty = true;
        }
    }
    std::unordered_set<uint32_t> skip;
    {
        std::scoped_lock lk{g_gui_lock};
        std::scoped_lock lk2{g_mode_lock};
        for (const uint32_t gui : g_guis) {
            const std::string& k = GuiKey(gui);
            if (k.empty() || g_exclude.count(k)) {
                skip.insert(gui);
            }
        }
    }
    {
        std::unique_lock lk{g_elem_lock};
        if (dirty) {
            g_elem_table = cache;
        }
        g_global_skip.swap(skip);
    }
    g_elem_any = g_on.load() && g_elements.load() && !cache.empty();
    g_global_eff = g_on.load() ? g_global.load() : 1.0f;
    const uint32_t gen = ++g_frame_gen;
    if (gen % 600 == 0) {
        std::scoped_lock lk{g_ebox_lock};
        for (auto it = g_ebox.begin(); it != g_ebox.end();) {
            it = gen - it->second.gen > 600 ? g_ebox.erase(it) : std::next(it);
        }
    }
}

/// Game thread: anchors locked at draw time go into the rules and uifix.ini.
void SavePendingAnchors() {
    std::map<std::string, std::pair<float, float>> pend;
    {
        std::scoped_lock lk{g_pend_lock};
        pend.swap(g_pend_anchor);
    }
    bool changed = false;
    {
        std::scoped_lock lk{g_mode_lock};
        for (const auto& [k, a] : pend) {
            const auto it = g_elem_cfg.find(k);
            if (it != g_elem_cfg.end() && !it->second.anchored) {
                it->second.anchored = true;
                it->second.ax = a.first;
                it->second.ay = a.second;
                changed = true;
            }
        }
    }
    if (changed) {
        ++g_elem_ver;
        SaveIni();
    }
}

struct ElemHit {
    uint32_t node;
    ElemCfg cfg;
    bool global; // the global scale on a top-level element (skipped for full-screen boxes)
    std::string key; // rules whose anchor isn't saved yet
};

/// The configured elements the piece is drawn inside, innermost first; then the global scale on
/// its top-level element (the instance just below the layout root), unless that one has a rule of
/// its own or any element in the chain has its own scale (an own scale replaces the global one,
/// it never stacks on it).
int ElemChain(uint32_t piece, ElemHit* out, int max) {
    const float global = g_global_eff.load(std::memory_order_relaxed);
    const bool elems = g_elem_any.load(std::memory_order_relaxed);
    if (!elems && global == 1.0f) {
        return 0;
    }
    uint32_t node = t_inst.empty() ? piece : t_inst.back();
    uint32_t top = 0, top_gui = 0;
    bool top_rule = false, own_scale = false;
    int n = 0;
    std::shared_lock lk{g_elem_lock};
    for (int i = 0; i < 12 && node && n < max; ++i) {
        uint32_t vp = 0, id = 0, w[3]{}; // +0x68 parent, +0x6C owner, +0x70 rGUI
        if (!forge_api->mem_read(node, &vp, 4)) {
            break;
        }
        if (vp == g_root_vptr) {
            break;
        }
        if (!forge_api->mem_read(node + 0x68, w, sizeof(w))) {
            break;
        }
        top = node;
        top_gui = w[2];
        top_rule = false;
        const auto it = elems ? g_elem_table.find(w[2]) : g_elem_table.end();
        if (it != g_elem_table.end() && forge_api->mem_read(node + 0x04, &id, 4)) {
            for (const ElemRule& r : it->second.rules) {
                if (r.id == id && n < max) {
                    out[n].node = node;
                    out[n].cfg = r.cfg;
                    out[n].global = false;
                    if (!r.cfg.anchored) {
                        out[n].key = r.key;
                    }
                    ++n;
                    top_rule = true;
                    own_scale |= r.cfg.scale != 1.0f;
                }
            }
        }
        node = w[0];
    }
    if (global != 1.0f && top && !top_rule && !own_scale && n < max && !g_global_skip.count(top_gui)) {
        out[n].node = top;
        out[n].cfg = {global, 0.0f, 0.0f};
        out[n].global = true;
        ++n;
    }
    return n;
}

bool PieceQuad(uint32_t piece, float q[4], uint32_t raw[12]);

/// A piece's box in canvas units: its local quad through matrix m, or just its origin.
void PieceBox(uint32_t piece, const float m[16], float b[4]) {
    float q[4];
    uint32_t raw[12];
    b[0] = b[2] = m[12];
    b[1] = b[3] = m[13];
    if (!PieceQuad(piece, q, raw)) {
        return;
    }
    b[0] = b[1] = INFINITY;
    b[2] = b[3] = -INFINITY;
    for (const float x : {q[0], q[2]}) {
        for (const float y : {q[1], q[3]}) {
            const float px = x * m[0] + y * m[4] + m[12];
            const float py = x * m[1] + y * m[5] + m[13];
            b[0] = std::min(b[0], px);
            b[1] = std::min(b[1], py);
            b[2] = std::max(b[2], px);
            b[3] = std::max(b[3], py);
        }
    }
}

/// Where an instance sits on screen now: its world position (+0x40/+0x44, game coordinates)
/// taken through the same squeeze the destretch gave the piece being drawn (the piece's own matrix
/// +0x10 against the uploaded copy m). False when the piece's matrix doesn't allow it.
bool ElemOrigin(uint32_t node, uint32_t piece, const float m[16], float o[2]) {
    float gm[16], t[2];
    if (!forge_api->mem_read(piece + 0x10, gm, sizeof(gm)) || !forge_api->mem_read(node + 0x40, t, sizeof(t)) ||
        !std::isfinite(t[0]) || !std::isfinite(t[1]) || std::fabs(gm[0]) < 1e-4f || std::fabs(gm[5]) < 1e-4f) {
        return false;
    }
    const float kx = m[0] / gm[0], ky = m[5] / gm[5];
    if (!(kx > 0.1f && kx < 10.0f && ky > 0.1f && ky < 10.0f)) {
        return false;
    }
    o[0] = m[12] + (t[0] - gm[12]) * kx;
    o[1] = m[13] + (t[1] - gm[13]) * ky;
    return std::isfinite(o[0]) && std::isfinite(o[1]);
}

/// Anchor of an element. Locked the first time it is known: the side of its box nearest the
/// screen edge (or its centre), stored relative to the element's own position, so the anchor
/// moves with the element but not when parts of it pop in or out. Rules keep it in uifix.ini.
/// A box is complete when its first piece comes round again (the draw thread's own frame), or
/// after ~2 s if that piece stopped drawing.
bool ElemPivot(const ElemHit& hit, uint32_t piece, const float* add_box, const float* origin, float& px,
               float& py, float* prev_box = nullptr) {
    const uint64_t ident = ElemIdent(hit.node);
    std::scoped_lock lk{g_ebox_lock};
    ElemBox& e = g_ebox[hit.node];
    const uint32_t gen = g_frame_gen.load(std::memory_order_relaxed);
    if (!e.ident) {
        e.ident = ident;
    }
    if (add_box) {
        if (!e.first || piece == e.first || gen - e.gen > 120) {
            if (e.first) {
                std::memcpy(e.prev, e.box, sizeof(e.box));
                e.has_prev = true;
                const auto lp = g_last_piv.find(e.ident);
                if (lp != g_last_piv.end()) {
                    std::memcpy(lp->second.box, e.prev, sizeof(e.prev));
                }
            }
            std::memcpy(e.box, add_box, sizeof(e.box));
            e.first = piece;
            e.gen = gen;
        } else {
            e.box[0] = std::min(e.box[0], add_box[0]);
            e.box[1] = std::min(e.box[1], add_box[1]);
            e.box[2] = std::max(e.box[2], add_box[2]);
            e.box[3] = std::max(e.box[3], add_box[3]);
        }
    }
    if (origin) {
        e.origin[0] = origin[0];
        e.origin[1] = origin[1];
        e.has_origin = true;
    }
    if (prev_box) {
        std::memcpy(prev_box, e.has_prev ? e.prev : e.box, sizeof(e.prev));
    }
    if (hit.cfg.anchored && e.has_origin) {
        px = e.origin[0] + hit.cfg.ax;
        py = e.origin[1] + hit.cfg.ay;
        return true;
    }
    const uint32_t epoch = g_anchor_epoch.load(std::memory_order_relaxed);
    if (e.locked && e.epoch == epoch && e.has_origin) {
        px = e.origin[0] + e.off[0];
        py = e.origin[1] + e.off[1];
        return true;
    }
    if (!e.has_prev) {
        // first frame of a new instance: take what the same element looked like last time
        const auto lp = g_last_piv.find(e.ident);
        if (!e.ident || lp == g_last_piv.end() || !e.has_origin) {
            return false; // never seen: its extent isn't known yet
        }
        std::memcpy(e.prev, lp->second.box, sizeof(e.prev));
        e.has_prev = true;
        e.off[0] = lp->second.off[0];
        e.off[1] = lp->second.off[1];
        e.locked = true;
        e.epoch = epoch;
        px = e.origin[0] + e.off[0];
        py = e.origin[1] + e.off[1];
        if (prev_box) {
            std::memcpy(prev_box, e.prev, sizeof(e.prev));
        }
        return true;
    }
    const auto side = [](float lo, float hi, float span) {
        const float c = (lo + hi) * 0.5f;
        return c < span / 3.0f ? lo : c > span * 2.0f / 3.0f ? hi : c;
    };
    px = side(e.prev[0], e.prev[2], 1280.0f);
    py = side(e.prev[1], e.prev[3], 720.0f);
    if (e.has_origin) {
        e.off[0] = px - e.origin[0];
        e.off[1] = py - e.origin[1];
        e.locked = true;
        e.epoch = epoch;
        if (e.ident) {
            std::memcpy(g_last_piv[e.ident].box, e.prev, sizeof(e.prev));
            std::memcpy(g_last_piv[e.ident].off, e.off, sizeof(e.off));
            g_cache_dirty = true;
        }
        if (!hit.global && !hit.key.empty()) {
            std::scoped_lock lk2{g_pend_lock};
            g_pend_anchor.emplace(hit.key, std::make_pair(e.off[0], e.off[1]));
        }
    }
    return true;
}

// --- UI inspector (read only): boxes and names of the UI elements on screen ---
// Every piece upload (after the squeeze, so boxes show where things really are) is filed under
// its ancestor at the chosen depth of its chain (PieceChain): the instances below the layout root
// (instance +0x04 = id in the file's instance table, which names them), then the objects of the
// animation instance drawing it, down to the piece. Objects are labelled by vtable and child index
// until their id field is known (the dump scans for it against the object table: header +0x30
// count, +0xAC offset; 36-byte records: id, ..., name +0x10, class hash +0x14). A piece's local
// quad: alignment +0xA0 (bits 0-1 x: 0 left, 1 centre, 2 right; bits 2-3 y likewise; higher bits
// are other flags), width +0xB4, height +0xB8, +0x56 bit 0x40 flips y (2D object draw 0xA4E7B4).
// Pieces without plausible fields (text) count as points.
struct PieceSample {
    uint32_t piece, vptr, lr, slot;
    bool quad;
    float q[4]; // local quad x0, y0, x1, y1
    float m[16];
    uint32_t raw[12]; // piece +0xA0..+0xCC
};
struct ChainNode {
    uint32_t node;
    bool obj; // an object (parent +0x70) rather than an instance (parent +0x68)
};
struct UiElem {
    uint32_t gui{}, node{};
    std::string key, path;
    std::vector<ChainNode> chain; // top-level instance .. this element (for the dump)
    uint32_t gen{}, pieces{}, prev_pieces{};
    float box[4]{}, prev[4]{}; // canvas units: x0, y0, x1, y1
    bool has_prev{false};
    std::vector<std::array<float, 4>> parts, prev_parts; // piece boxes (first 128)
    std::vector<PieceSample> samples; // first pieces seen, for the dump
};
std::atomic<bool> g_insp_on{false};
std::atomic<bool> g_insp_frozen{false};
std::atomic<int> g_insp_depth{1};
std::atomic<uint32_t> g_insp_gen{1};
std::atomic<uint32_t> g_insp_outside{0}; // uploads outside a layout draw
std::mutex g_insp_lock;
std::map<uint64_t, UiElem> g_elems;                        // (rGUI << 32 | node), g_insp_lock
std::map<std::pair<uint32_t, uint32_t>, std::string> g_node_names; // (rGUI, id), g_insp_lock
std::map<uint32_t, std::string> g_obj_labels; // object -> "o:<vtable low bits>#<child index>"
std::map<uint32_t, ChainNode> g_insp_nodes;   // every chain node seen -> kind (id-field scan)
std::map<uint32_t, uint32_t> g_insp_node_gui; // node -> rGUI
uint64_t g_insp_sel{0}, g_insp_hover{0}, g_insp_row_hover{0}; // present thread only
bool g_insp_parts{false}, g_insp_labels{false}, g_insp_big{false};
std::string g_insp_msg;
struct DragState { // moving the selected element with the mouse (present thread only)
    bool pending{false}, active{false};
    float m0[2]{}, b0[4]{}; // mouse and box at the press (canvas units)
    std::string key;
    ElemCfg c0, cur;
};
DragState g_drag;

bool PieceQuad(uint32_t piece, float q[4], uint32_t raw[12]) {
    if (!forge_api->mem_read(piece + 0xA0, raw, 48)) {
        return false;
    }
    uint8_t flags = 0;
    forge_api->mem_read(piece + 0x56, &flags, 1);
    const uint32_t al = raw[0];
    const float w = Float(raw[5]), h = Float(raw[6]) * ((flags & 0x40) ? -1.0f : 1.0f);
    if ((al & 3) == 3 || (al & 0xC) == 0xC || !std::isfinite(w) || !std::isfinite(h) ||
        w <= 0.0f || w > 4096.0f || h == 0.0f || std::fabs(h) > 4096.0f) {
        return false;
    }
    const float x0 = (al & 3) == 0 ? 0.0f : (al & 3) == 1 ? -w * 0.5f : -w;
    const float y0 = (al & 0xC) == 0 ? 0.0f : (al & 0xC) == 4 ? -h * 0.5f : -h;
    q[0] = x0;
    q[2] = x0 + w;
    q[1] = std::min(y0, y0 + h);
    q[3] = std::max(y0, y0 + h);
    return true;
}

std::string NodeName(uint32_t gui, uint32_t node) {
    uint32_t id = 0;
    if (!forge_api->mem_read(node + 0x04, &id, 4)) {
        return "?";
    }
    {
        std::scoped_lock lk{g_insp_lock};
        const auto it = g_node_names.find({gui, id});
        if (it != g_node_names.end()) {
            return it->second;
        }
    }
    std::string name;
    GuiFile g;
    uint32_t index = 0, text = 0;
    const char* why = nullptr;
    if (!ReadGuiFile(gui, g)) {
        why = "layout file unreadable";
    } else if (const uint32_t rec = FindRecord(g, id, &index)) {
        forge_api->mem_read(rec + 0x14, &text, 4);
        name = RecordName(g, text);
        if (name.empty()) {
            why = "empty name string";
        }
    } else {
        why = "no record with that id";
    }
    if (name.empty()) {
        // not cached: the file may just not be loaded yet; say why once per layout, with the raw
        // words (record, name field, string table and what sits there) to find the right form
        static std::set<uint32_t> told;
        std::scoped_lock lk{g_insp_lock};
        if (told.insert(gui).second) {
            uint32_t idx = 0;
            const uint32_t rec = g.file ? FindRecord(g, id, &idx) : 0;
            uint32_t h104 = 0;
            forge_api->mem_read(g.file + 0x104, &h104, 4);
            Logf(FORGE_LOG_WARN,
                 "inspector: instance %u of layout %08X unnamed (%s; file %08X size %X, table %08X, "
                 "%u records, rec %08X = %s, name field %X, header[104] %X, strings %08X: %s, at "
                 "strings+field: %s)",
                 id, gui, why, g.file, g.size, g.table, g.count, rec, rec ? HexAt(rec, 36).c_str() : "-",
                 text, h104, g.strings, HexAt(g.strings).c_str(), HexAt(g.strings + text).c_str());
        }
        char b[16];
        std::snprintf(b, sizeof(b), "#%u", id);
        return b;
    }
    std::scoped_lock lk{g_insp_lock};
    g_node_names[{gui, id}] = name;
    return name;
}

/// Name of an object: +0x7C points to its record in the layout file (36-byte records at file +
/// header[0xAC], count header[0x30]; name = strings + record[+0x10]). Failing that, its vtable
/// (low 16 bits) and index under its parent.
std::string ObjectLabel(uint32_t gui, uint32_t obj) {
    {
        std::scoped_lock lk{g_insp_lock};
        const auto it = g_obj_labels.find(obj);
        if (it != g_obj_labels.end()) {
            return it->second;
        }
    }
    GuiFile g;
    uint32_t rec = 0, n = 0, text = 0;
    if (ReadGuiFile(gui, g) && forge_api->mem_read(obj + 0x7C, &rec, 4) &&
        forge_api->mem_read(g.file + 0x30, &n, 4)) {
        const uint32_t lo = FileAddr(g.file, g.size, 0xAC);
        if (rec >= lo && rec < lo + std::min(n, 100000u) * 36 && (rec - lo) % 36 == 0 &&
            forge_api->mem_read(rec + 0x10, &text, 4)) {
            std::string name = RecordName(g, text);
            if (!name.empty()) {
                std::scoped_lock lk{g_insp_lock};
                return g_obj_labels[obj] = name;
            }
        }
    }
    uint32_t vp = 0, parent = 0, c = 0;
    forge_api->mem_read(obj, &vp, 4);
    forge_api->mem_read(obj + 0x70, &parent, 4);
    int index = -1;
    if (parent && forge_api->mem_read(parent + 0x68, &c, 4)) {
        for (int i = 0; c && i < 512; ++i) {
            if (c == obj) {
                index = i;
                break;
            }
            if (!forge_api->mem_read(c + 0x6C, &c, 4)) {
                break;
            }
        }
    }
    char b[32];
    std::snprintf(b, sizeof(b), "o:%04X#%d", vp & 0xFFFF, index);
    std::scoped_lock lk{g_insp_lock};
    return g_obj_labels[obj] = b;
}

/// The piece's ancestry, top first: the instances from below the layout root down to the
/// animation instance drawing it, then the objects from the top of its tree down to the piece.
std::vector<ChainNode> PieceChain(uint32_t piece) {
    std::vector<ChainNode> objs, insts;
    uint32_t inst = t_inst.empty() ? 0 : t_inst.back();
    if (inst) {
        uint32_t holder = 0, p = piece, parent = 0;
        forge_api->mem_read(inst + 0xDC, &holder, 4);
        bool reached = false;
        for (int i = 0; i < 24 && p; ++i) {
            objs.push_back({p, true});
            if (!forge_api->mem_read(p + 0x70, &parent, 4) || !parent) {
                break;
            }
            if (parent == holder || parent == inst) {
                reached = true;
                break;
            }
            p = parent;
        }
        if (!reached) { // not this instance's object: keep just the piece
            objs.assign(1, {piece, true});
        }
        std::reverse(objs.begin(), objs.end());
        for (int i = 0; i < 16 && inst; ++i) {
            uint32_t vp = 0;
            forge_api->mem_read(inst, &vp, 4);
            if (vp == g_root_vptr) {
                break;
            }
            insts.push_back({inst, false});
            forge_api->mem_read(inst + 0x68, &inst, 4);
        }
        std::reverse(insts.begin(), insts.end());
    } else {
        // drawn by the instance walk itself: the piece is an instance; its parents lead to the root
        uint32_t q = piece;
        bool rooted = false;
        for (int i = 0; i < 16 && q; ++i) {
            uint32_t vp = 0;
            forge_api->mem_read(q, &vp, 4);
            if (vp == g_root_vptr) {
                rooted = true;
                break;
            }
            insts.push_back({q, false});
            forge_api->mem_read(q + 0x68, &q, 4);
        }
        std::reverse(insts.begin(), insts.end());
        if (!rooted) {
            insts.clear();
            for (size_t i = 1; i < t_draw.size(); ++i) {
                insts.push_back({t_draw[i].node, false});
            }
            if (insts.empty() || insts.back().node != piece) {
                objs.push_back({piece, true});
            }
        }
    }
    insts.insert(insts.end(), objs.begin(), objs.end());
    return insts;
}

/// Files the piece just uploaded (slot [[ctx+4]+0x18BC]) under its element.
void DumpObjTree(FILE* f, uint32_t o, int depth, int& budget) {
    const uint32_t main = forge_api->mem_getModuleBase("main");
    for (; o && budget > 0; --budget) {
        uint32_t w[0x30]{};
        if (!forge_api->mem_read(o, w, sizeof(w))) {
            return;
        }
        float fw = 0, fh = 0;
        forge_api->mem_read(o + 0xB4, &fw, 4);
        forge_api->mem_read(o + 0xB8, &fh, 4);
        uint32_t a0 = 0, child = 0, next = 0;
        forge_api->mem_read(o + 0xA0, &a0, 4);
        forge_api->mem_read(o + 0x68, &child, 4);
        forge_api->mem_read(o + 0x6C, &next, 4);
        std::fprintf(f, "%*s  obj %08X vt main+%X +A0 %08X size %.1f x %.1f\n", depth * 2, "", o, w[0] - main,
                     a0, fw, fh);
        if (child) {
            DumpObjTree(f, child, depth + 1, budget);
        }
        o = next;
    }
}

void DumpInstTree(FILE* f, uint32_t n, int depth, int& budget) {
    const uint32_t main = forge_api->mem_getModuleBase("main");
    if (!n || budget <= 0 || depth > 12) {
        return;
    }
    --budget;
    uint32_t vp = 0, id = 0;
    float t[2]{};
    forge_api->mem_read(n, &vp, 4);
    forge_api->mem_read(n + 0x04, &id, 4);
    forge_api->mem_read(n + 0x40, t, sizeof(t));
    std::fprintf(f, "%*sinst %08X id %u vt main+%X pos %.1f %.1f\n", depth * 2, "", n, id, vp - main, t[0], t[1]);
    const uint32_t off = vp - main;
    if (off == 0x178F5B8 || off == 0x178F62C || off == 0x178F6A4) { // animation instance
        uint32_t holder = 0, first = 0;
        forge_api->mem_read(n + 0xDC, &holder, 4);
        if (holder && forge_api->mem_read(holder + 0x68, &first, 4) && first) {
            DumpObjTree(f, first, depth + 1, budget);
        }
    }
    uint32_t child = 0;
    if (forge_api->mem_read(n + 0x60, &child, 4)) {
        for (uint32_t c = child; c && budget > 0;) {
            DumpInstTree(f, c, depth + 1, budget);
            uint32_t next = 0;
            if (!forge_api->mem_read(c + 0x64, &next, 4)) {
                break;
            }
            c = next;
        }
    }
}

void InspectUpload(uint32_t ctx, uint32_t piece, uint32_t lr) {
    if (!g_insp_on.load(std::memory_order_relaxed) || g_insp_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    if (t_draw.empty() || !t_draw.front().gui) {
        ++g_insp_outside;
        return;
    }
    uint32_t renderer = 0, slot = 0;
    float m[16];
    if (!forge_api->mem_read(ctx + 0x04, &renderer, 4) ||
        !forge_api->mem_read(renderer + 0x18BC, &slot, 4) || !slot ||
        !forge_api->mem_read(slot, m, sizeof(m))) {
        return;
    }
    const uint32_t gui = t_draw.front().gui;
    std::vector<ChainNode> chain = PieceChain(piece);
    const size_t want = g_advanced.load(std::memory_order_relaxed) ? static_cast<size_t>(g_insp_depth.load()) : 1;
    chain.resize(std::min(want, chain.size())); // the element = the chain's node at that depth
    const uint32_t node = chain.back().node;
    // the piece's box: its local quad through the uploaded matrix, or just its origin
    PieceSample s{piece, 0, lr, slot};
    std::memcpy(s.m, m, sizeof(m));
    s.quad = PieceQuad(piece, s.q, s.raw);
    float b[4]{m[12], m[13], m[12], m[13]};
    if (s.quad) {
        b[0] = b[1] = INFINITY;
        b[2] = b[3] = -INFINITY;
        for (const float x : {s.q[0], s.q[2]}) {
            for (const float y : {s.q[1], s.q[3]}) {
                const float px = x * m[0] + y * m[4] + m[12];
                const float py = x * m[1] + y * m[5] + m[13];
                b[0] = std::min(b[0], px);
                b[1] = std::min(b[1], py);
                b[2] = std::max(b[2], px);
                b[3] = std::max(b[3], py);
            }
        }
    }
    const uint64_t k = (static_cast<uint64_t>(gui) << 32) | node;
    const uint32_t gen = g_insp_gen.load(std::memory_order_relaxed);
    bool known;
    {
        std::scoped_lock lk{g_insp_lock};
        known = g_elems.count(k) != 0;
    }
    std::string key, path;
    if (!known) {
        {
            std::scoped_lock lk{g_gui_lock};
            key = GuiKey(gui);
        }
        uint32_t file_gui = gui; // a node's own layout (+0x70 of instances; nested layouts differ)
        for (const ChainNode& c : chain) {
            uint32_t own = 0, vp = 0;
            if (!c.obj && forge_api->mem_read(c.node + 0x70, &own, 4) && own &&
                forge_api->mem_read(own, &vp, 4) && vp == g_vptr) {
                file_gui = own;
            }
            path += (path.empty() ? "" : " / ") +
                    (c.obj ? ObjectLabel(file_gui, c.node) : NodeName(file_gui, c.node));
        }
    }
    std::scoped_lock lk{g_insp_lock};
    for (const ChainNode& c : chain) {
        if (g_insp_nodes.size() < 8192 && g_insp_nodes.emplace(c.node, c).second) {
            g_insp_node_gui[c.node] = gui;
        }
    }
    UiElem& e = g_elems[k];
    if (!known && e.key.empty()) {
        e.gui = gui;
        e.node = node;
        e.key = key;
        e.path = path;
        e.chain = chain;
    }
    if (e.gen != gen) {
        if (e.pieces) {
            std::memcpy(e.prev, e.box, sizeof(e.box));
            e.prev_pieces = e.pieces;
            e.prev_parts.swap(e.parts);
            e.has_prev = e.gen + 1 == gen;
        }
        e.parts.clear();
        e.pieces = 0;
        e.gen = gen;
    }
    if (e.pieces++ == 0) {
        std::memcpy(e.box, b, sizeof(b));
    } else {
        e.box[0] = std::min(e.box[0], b[0]);
        e.box[1] = std::min(e.box[1], b[1]);
        e.box[2] = std::max(e.box[2], b[2]);
        e.box[3] = std::max(e.box[3], b[3]);
    }
    if (e.parts.size() < 128) {
        e.parts.push_back({b[0], b[1], b[2], b[3]});
    }
    if (e.samples.size() < 8 && std::none_of(e.samples.begin(), e.samples.end(),
                                             [&](const PieceSample& o) { return o.piece == piece; })) {
        forge_api->mem_read(piece, &s.vptr, 4);
        e.samples.push_back(std::move(s));
    }
}

/// Per game frame: a new generation; elements gone for ~2 s are dropped.
void InspectFrame() {
    if (!g_insp_on.load() || g_insp_frozen.load()) {
        return;
    }
    const uint32_t gen = ++g_insp_gen;
    if (gen % 60 == 0) {
        std::scoped_lock lk{g_insp_lock};
        for (auto it = g_elems.begin(); it != g_elems.end();) {
            it = gen - it->second.gen > 60 ? g_elems.erase(it) : std::next(it);
        }
    }
}

void InspectDump() {
    // a new file per save (uifix_ui_dump_1.txt, _2, ...) so earlier dumps are kept
    const std::string dir = g_ini_path.substr(0, g_ini_path.find_last_of("\\/") + 1);
    std::string path;
    for (int i = 1; i < 1000; ++i) {
        path = dir + "uifix_ui_dump_" + std::to_string(i) + ".txt";
        if (FILE* e = std::fopen(path.c_str(), "r")) {
            std::fclose(e);
            continue;
        }
        break;
    }
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        g_insp_msg = "couldn't write " + path;
        return;
    }
    const uint32_t main = forge_api->mem_getModuleBase("main");
    const uint32_t gen = g_insp_gen.load();
    std::fprintf(f, "# UI Fix dump: elements at depth %d (box = canvas units 1280x720, "
                    "after the squeeze). gen %u, %u uploads outside a layout draw\n",
                 g_insp_depth.load(), gen, g_insp_outside.load());
    // which field of a node holds its record id: every word of every node seen, against the ids
    // and record addresses of its layout file's object (+0x30/+0xAC) and instance tables
    {
        std::map<uint32_t, ChainNode> nodes;
        std::map<uint32_t, uint32_t> node_gui;
        {
            std::scoped_lock lk{g_insp_lock};
            nodes = g_insp_nodes;
            node_gui = g_insp_node_gui;
        }
        struct Tables {
            std::set<uint32_t> obj_ids, inst_ids;
            uint32_t obj_lo{}, obj_hi{}, inst_lo{}, inst_hi{}, file{}, file_end{};
        };
        std::map<uint32_t, Tables> tables;
        // [kind obj/inst][match: obj id, inst id, -> obj record, -> inst record, -> file][offset/4]
        static uint32_t hits[2][5][64];
        std::memset(hits, 0, sizeof(hits));
        uint32_t total[2]{};
        for (const auto& [n, c] : nodes) {
            const uint32_t gui = node_gui[n];
            auto it = tables.find(gui);
            if (it == tables.end()) {
                Tables t;
                GuiFile g;
                if (ReadGuiFile(gui, g)) {
                    uint32_t on = 0;
                    forge_api->mem_read(g.file + 0x30, &on, 4);
                    t.obj_lo = FileAddr(g.file, g.size, 0xAC);
                    t.obj_hi = t.obj_lo + std::min(on, 100000u) * 36;
                    t.inst_lo = g.table;
                    t.inst_hi = g.table + g.count * 36;
                    t.file = g.file;
                    t.file_end = g.file + g.size;
                    for (uint32_t i = 0; i < on && i < 100000; ++i) {
                        uint32_t id = 0;
                        forge_api->mem_read(t.obj_lo + i * 36, &id, 4);
                        t.obj_ids.insert(id);
                    }
                    for (uint32_t i = 0; i < g.count; ++i) {
                        uint32_t id = 0;
                        forge_api->mem_read(g.table + i * 36, &id, 4);
                        t.inst_ids.insert(id);
                    }
                }
                it = tables.emplace(gui, std::move(t)).first;
            }
            const Tables& t = it->second;
            uint32_t w[64]{};
            if (!t.file || !forge_api->mem_read(n, w, sizeof(w))) {
                continue;
            }
            const int kind = c.obj ? 0 : 1;
            ++total[kind];
            for (int o = 1; o < 64; ++o) {
                const uint32_t v = w[o];
                hits[kind][0][o] += t.obj_ids.count(v) != 0 && v > 1;
                hits[kind][1][o] += t.inst_ids.count(v) != 0 && v > 1;
                hits[kind][2][o] += v >= t.obj_lo && v < t.obj_hi;
                hits[kind][3][o] += v >= t.inst_lo && v < t.inst_hi;
                hits[kind][4][o] += v >= t.file && v < t.file_end;
            }
        }
        static const char* const kMatch[5] = {"object id", "instance id", "-> object record",
                                               "-> instance record", "-> layout file"};
        for (int kind = 0; kind < 2; ++kind) {
            std::fprintf(f, "# id-field scan, %s (%u nodes):", kind ? "instances" : "objects",
                         total[kind]);
            for (int m = 0; m < 5; ++m) {
                std::vector<std::pair<uint32_t, int>> best;
                for (int o = 1; o < 64; ++o) {
                    if (hits[kind][m][o]) {
                        best.push_back({hits[kind][m][o], o});
                    }
                }
                std::sort(best.rbegin(), best.rend());
                std::fprintf(f, "\n#   %-18s", kMatch[m]);
                for (size_t i = 0; i < best.size() && i < 6; ++i) {
                    std::fprintf(f, " +%02X:%u", best[i].second * 4, best[i].first);
                }
            }
            std::fprintf(f, "\n");
        }
    }
    std::set<uint32_t> chain_nodes;
    std::map<uint32_t, LayoutCount> lc_copy;
    std::scoped_lock lk{g_insp_lock};
    std::vector<const UiElem*> list;
    for (const auto& [k, e] : g_elems) {
        list.push_back(&e);
    }
    std::sort(list.begin(), list.end(), [](const UiElem* a, const UiElem* b) {
        return a->key != b->key ? a->key < b->key : a->path < b->path;
    });
    for (const UiElem* e : list) {
        const float* b = e->has_prev ? e->prev : e->box;
        std::fprintf(f, "\n%s | %s | gui %08X node %08X | box %.1f %.1f %.1f %.1f | %u pieces | "
                        "last seen %u frames ago\n",
                     e->key.c_str(), e->path.c_str(), e->gui, e->node, b[0], b[1], b[2], b[3],
                     e->has_prev ? e->prev_pieces : e->pieces, gen - e->gen);
        std::fprintf(f, "  chain:");
        for (const ChainNode& c : e->chain) {
            uint32_t vp = 0;
            forge_api->mem_read(c.node, &vp, 4);
            std::fprintf(f, " %s %08X (vt main+%X)", c.obj ? "obj" : "inst", c.node, vp - main);
            chain_nodes.insert(c.node);
        }
        std::fprintf(f, "\n");
        for (const auto& s : e->samples) {
            std::fprintf(f, "  piece %08X vptr main+%X caller main+%X quad %s "
                            "%.1f %.1f %.1f %.1f | m x %.3f %.3f y %.3f %.3f t %.1f %.1f | +A0:",
                         s.piece, s.vptr - main, s.lr - main, s.quad ? "yes" : "no", s.q[0], s.q[1],
                         s.q[2], s.q[3], s.m[0], s.m[1], s.m[4], s.m[5], s.m[12], s.m[13]);
            for (const uint32_t w : s.raw) {
                std::fprintf(f, " %08X", w);
            }
            std::fprintf(f, "\n");
        }
    }
    {
        std::map<uint32_t, LayoutCount>& lc = lc_copy;
        {
            std::scoped_lock lk{g_lc_lock};
            lc = g_lc;
        }
        std::fprintf(f, "\n# layouts drawn since the inspector was switched on (draws, hooked uploads):\n");
        for (const auto& [gui, c] : lc) {
            std::string key;
            {
                std::scoped_lock lk{g_gui_lock};
                key = GuiKey(gui);
            }
            std::fprintf(f, "#   %-60s gui %08X draws %u uploads %u%s\n", key.c_str(), gui, c.draws,
                         c.uploads, c.uploads == 0 ? "   <-- draws, no hooked upload" : "");
        }
    }
    for (const auto& [gui, c] : lc_copy) { // layouts that draw without a hooked upload: their trees
        if (!c.root) {
            continue;
        }
        std::string key;
        {
            std::scoped_lock lk{g_gui_lock};
            key = GuiKey(gui);
        }
        if (c.uploads && key.find("\\map\\") == std::string::npos) {
            continue; // only layouts with no hooked upload, and the map layouts
        }
        std::fprintf(f, "\n# tree of %s (no hooked upload): instances (id, vptr, pos) and objects\n", key.c_str());
        int budget = 400;
        uint32_t child = 0;
        forge_api->mem_read(c.root + 0x60, &child, 4);
        for (uint32_t n = child; n && budget > 0;) {
            DumpInstTree(f, n, 0, budget);
            uint32_t next = 0;
            if (!forge_api->mem_read(n + 0x64, &next, 4)) {
                break;
            }
            n = next;
        }
    }
    std::fprintf(f, "\n# chain nodes: words +00..+FC\n");
    for (const uint32_t n : chain_nodes) {
        uint32_t w[64]{};
        forge_api->mem_read(n, w, sizeof(w));
        std::fprintf(f, "%08X vt main+%X:", n, w[0] - main);
        for (int o = 1; o < 64; ++o) {
            std::fprintf(f, "%s%08X", o % 8 == 0 ? "  " : " ", w[o]);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    g_insp_msg = "saved " + std::to_string(list.size()) + " elements to " + path.substr(dir.size());
    Logf(FORGE_LOG_INFO, "UI dump: %zu elements -> %s", list.size(), path.c_str());
}

/// Sets one element's scale/move (identity removes it); saves when `save`.
void SetElem(const std::string& key, const ElemCfg& c, bool save) {
    {
        std::scoped_lock lk{g_mode_lock};
        if (c.Identity()) {
            g_elem_cfg.erase(key);
        } else {
            g_elem_cfg[key] = c;
        }
    }
    ++g_elem_ver;
    if (save) {
        SaveIni();
    }
}

/// The rule a selected element edits: its deepest instance (objects inside an animation aren't
/// separate elements), keyed "<layout> / <name>", or a wildcard rule covering its layout.
struct SelTarget {
    std::string layout, name, key, exact, similar; // similar = wildcard key for every layout like it
};
bool ResolveSel(uint32_t gui, const std::vector<ChainNode>& chain, SelTarget& t, std::string* why) {
    uint32_t node = 0, file_gui = gui;
    for (const ChainNode& c : chain) {
        if (c.obj) {
            break;
        }
        node = c.node;
        uint32_t own = 0, vp = 0;
        if (forge_api->mem_read(c.node + 0x70, &own, 4) && own && forge_api->mem_read(own, &vp, 4) &&
            vp == g_vptr) {
            file_gui = own;
        }
    }
    if (!node) {
        if (why) {
            *why = "(no element above this piece to scale)";
        }
        return false;
    }
    t.name = NodeName(file_gui, node);
    {
        std::scoped_lock lk{g_gui_lock};
        t.layout = GuiKey(file_gui);
    }
    if (t.name.empty() || t.name == "?" || t.layout.empty()) {
        if (why) {
            char b[160];
            std::snprintf(b, sizeof(b), "element %s of layout %08X can't be identified (layout not tracked)",
                          t.name.c_str(), file_gui);
            *why = b;
        }
        return false;
    }
    // unnamed elements are saved by their file instance id ("#26"); ids are fixed in the file
    t.exact = t.layout + " / " + t.name;
    const std::string sim = SimilarLayouts(t.layout);
    t.similar = sim != t.layout ? sim + " / " + t.name : std::string();
    t.key = t.exact;
    std::scoped_lock lk{g_mode_lock};
    if (!g_elem_cfg.count(t.exact)) {
        for (const auto& [k, c] : g_elem_cfg) {
            const auto pos = k.find(" / ");
            if (pos != std::string::npos && k.compare(pos + 3, std::string::npos, t.name) == 0 &&
                HasWild(k.substr(0, pos)) && Glob(k.substr(0, pos).c_str(), t.layout.c_str())) {
                t.key = k;
                break;
            }
        }
    }
    return true;
}

ElemCfg CfgOf(const std::string& key) {
    std::scoped_lock lk{g_mode_lock};
    const auto it = g_elem_cfg.find(key);
    return it != g_elem_cfg.end() ? it->second : ElemCfg{};
}

/// Scale/move controls for the selected element.
void ElementControls(uint32_t gui, const std::vector<ChainNode>& chain) {
    SelTarget t;
    std::string why;
    if (!ResolveSel(gui, chain, t, &why)) {
        ImGui::TextDisabled("%s", why.c_str());
        return;
    }
    ElemCfg c = CfgOf(t.key);
    if (g_advanced.load()) {
        ImGui::Text("Element: %s (%s)", t.name.c_str(), t.layout.c_str());
    } else {
        ImGui::Text("Element: %s", t.name.c_str());
    }
    if (!t.similar.empty()) {
        int which = t.key == t.exact ? 0 : 1;
        const int was = which;
        ImGui::RadioButton("This layout only", &which, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Every layout like it", &which, 1);
        if (g_advanced.load()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", t.similar.substr(0, t.similar.find(" / ")).c_str());
        }
        if (which != was) {
            if (which == 1) { // this layout's rule becomes the rule for all of them
                SetElem(t.exact, {}, false);
                SetElem(t.similar, c, true);
                t.key = t.similar;
            } else { // this layout gets its own copy; the shared rule stays for the others
                SetElem(t.exact, c, true);
                t.key = t.exact;
            }
        }
    } else if (t.key != t.exact) {
        ImGui::TextDisabled("(shared rule: %s)", t.key.c_str());
    }
    bool changed = ImGui::SliderFloat("Scale", &c.scale, 0.25f, 3.0f, "%.2f");
    bool save = ImGui::IsItemDeactivatedAfterEdit();
    const bool adv = g_advanced.load();
    if (adv) {
        changed |= ImGui::DragFloat2("Move x / y (canvas units)", &c.dx, 0.5f, -1280.0f, 1280.0f, "%.1f");
        save |= ImGui::IsItemDeactivatedAfterEdit();
    }
    if (ImGui::Button("Reset element")) {
        c = {};
        changed = save = true;
    }
    if (adv) {
        ImGui::SameLine();
        if (ImGui::Button("Re-anchor")) {
            c.anchored = false;
            ++g_anchor_epoch;
            changed = save = true;
        }
        ImGui::SameLine();
        if (c.anchored) {
            ImGui::TextDisabled("(scale point locked at %.0f, %.0f from the element)", c.ax, c.ay);
        } else {
            ImGui::TextDisabled("(scale point: its corner/edge nearest the screen edge, locked on first use)");
        }
        ImGui::TextDisabled("Drag the yellow box in the game to move it: edges snap to other boxes and the "
                            "screen edges (hold Alt to move freely).");
    }
    if (changed || save) {
        SetElem(t.key, c, save);
    }
}

/// Every element with a scale/move, also those not on screen now.
void ElementList() {
    std::vector<std::pair<std::string, ElemCfg>> list;
    {
        std::scoped_lock lk{g_mode_lock};
        list.assign(g_elem_cfg.begin(), g_elem_cfg.end());
    }
    if (!ImGui::TreeNodeEx("Adjusted elements", 0, "Adjusted elements (%zu)", list.size())) {
        return;
    }
    for (auto& [key, c] : list) {
        ImGui::PushID(key.c_str());
        if (ImGui::SmallButton("reset")) {
            SetElem(key, {}, true);
        }
        ImGui::SameLine();
        ImGui::Text("%s  x%.2f  move %.1f, %.1f", key.c_str(), c.scale, c.dx, c.dy);
        ImGui::PopID();
    }
    ImGui::Text("%u element draws adjusted", g_elem_applied.load());
    ImGui::TreePop();
}

/// Clears what the inspector collected (layouts and objects get freed and reused).
void InspectClear() {
    std::scoped_lock lk{g_insp_lock};
    g_elems.clear();
    g_obj_labels.clear();
    g_node_names.clear();
    g_insp_nodes.clear();
    g_insp_node_gui.clear();
}

/// Menu part + boxes over the game (drawn while the Forge menu is open). Simple: show the boxes
/// of whole elements, drag to move, scale the selected one. Advanced adds depth, piece boxes,
/// labels, dumps and the element tables.
void InspectorUi() {
    const bool adv = g_advanced.load();
    if (adv && !ImGui::TreeNodeEx("UI inspector", 0)) {
        return;
    }
    bool on = g_insp_on.load();
    if (ImGui::Checkbox(adv ? "Show UI elements" : "Edit the UI (show element boxes)", &on)) {
        g_insp_on = on;
        if (!on) {
            InspectClear();
            g_drag.pending = g_drag.active = false;
        }
    }
    bool frozen = g_insp_frozen.load();
    if (adv) {
        int depth = g_insp_depth.load();
        if (ImGui::SliderInt("Depth (1 = whole element, more = its parts down to single pieces)", &depth, 1, 10)) {
            g_insp_depth = depth;
            std::scoped_lock lk{g_insp_lock};
            g_elems.clear();
        }
        if (ImGui::TreeNodeEx("Debug", 0)) {
            if (ImGui::Checkbox("Freeze", &frozen)) {
                g_insp_frozen = frozen;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Piece boxes (selected)", &g_insp_parts);
            ImGui::SameLine();
            ImGui::Checkbox("All labels", &g_insp_labels);
            ImGui::SameLine();
            ImGui::Checkbox("Full-screen too", &g_insp_big);
            if (ImGui::Button("Save list (uifix_ui_dump_N.txt)")) {
                InspectDump();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(g_insp_msg.c_str());
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Hover the game to see an element's name; click to select it. Drag this "
                           "window aside to reach what it covers. Select an element to scale it; drag its "
                           "box to move it (edges snap; Alt = no snap).");
        ElementList();
    } else if (on) {
        ImGui::TextWrapped("Click an element in the game to select it (yellow box), drag it to move it: "
                           "its edges snap to other elements and the screen edges (hold Alt to move "
                           "freely). Drag this menu aside to reach what it covers.");
    }
    if (!on) {
        if (adv) {
            ImGui::TreePop();
        }
        return;
    }
    // what is on screen now
    struct Row {
        uint64_t k;
        std::string key, path;
        float b[4];
        uint32_t pieces;
        std::vector<std::array<float, 4>> parts;
        uint32_t gui{};
        std::vector<ChainNode> chain;
    };
    std::vector<Row> rows;
    {
        const uint32_t gen = g_insp_gen.load();
        std::scoped_lock lk{g_insp_lock};
        for (const auto& [k, e] : g_elems) {
            if (!frozen && gen - e.gen > 2) {
                continue;
            }
            const bool prev = e.has_prev && e.gen == gen;
            Row r{k, e.key, e.path, {}, prev ? e.prev_pieces : e.pieces};
            std::memcpy(r.b, prev ? e.prev : e.box, sizeof(r.b));
            if (k == g_insp_sel) {
                if (g_insp_parts) {
                    r.parts = prev ? e.prev_parts : e.parts;
                }
                r.gui = e.gui;
                r.chain = e.chain;
            }
            rows.push_back(std::move(r));
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    const float kx = io.DisplaySize.x / 1280.0f, ky = io.DisplaySize.y / 720.0f;
    const auto big = [](const float* b) { return b[2] - b[0] > 1200.0f && b[3] - b[1] > 650.0f; };
    const Row* sel_row = nullptr;
    for (const auto& r : rows) {
        if (r.k == g_insp_sel) {
            sel_row = &r;
        }
    }
    const float mx = io.MousePos.x / kx, my = io.MousePos.y / ky;
    const bool over_window = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    // hover: the smallest box under the mouse, not over a menu window
    g_insp_hover = g_insp_row_hover; // a table row hovered last frame
    g_insp_row_hover = 0;
    if (!over_window && !g_drag.active) {
        float best = INFINITY;
        for (const auto& r : rows) {
            if ((!g_insp_big && big(r.b)) || mx < r.b[0] - 2 || mx > r.b[2] + 2 || my < r.b[1] - 2 ||
                my > r.b[3] + 2) {
                continue;
            }
            const float area = (r.b[2] - r.b[0] + 4) * (r.b[3] - r.b[1] + 4);
            if (area < best) {
                best = area;
                g_insp_hover = r.k;
            }
        }
    }
    // click selects; pressing on the selected element and dragging moves it
    if (!over_window && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        SelTarget t;
        if (sel_row && mx >= sel_row->b[0] - 2 && mx <= sel_row->b[2] + 2 && my >= sel_row->b[1] - 2 &&
            my <= sel_row->b[3] + 2 && ResolveSel(sel_row->gui, sel_row->chain, t, nullptr)) {
            g_drag.pending = true;
            g_drag.m0[0] = mx;
            g_drag.m0[1] = my;
            std::memcpy(g_drag.b0, sel_row->b, sizeof(g_drag.b0));
            g_drag.key = t.key;
            g_drag.c0 = g_drag.cur = CfgOf(t.key);
        } else {
            g_insp_sel = g_insp_hover;
        }
    }
    if (g_drag.pending && !g_drag.active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        g_drag.active = true;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (g_drag.active) {
        const float d[2] = {mx - g_drag.m0[0], my - g_drag.m0[1]};
        const float c[4] = {g_drag.b0[0] + d[0], g_drag.b0[1] + d[1], g_drag.b0[2] + d[0], g_drag.b0[3] + d[1]};
        // snap: an edge within ~10 px of a screen edge, or of a neighbour's edge (touching it or in
        // line with it; neighbours = boxes beside it, not overlapping it), jumps onto it
        const float th = 10.0f / std::min(kx, ky);
        float off[2]{}, bestd[2]{th, th}, guide[2]{NAN, NAN};
        const auto try_at = [&](int axis, float target, float edge) {
            const float dd = target - edge;
            if (std::fabs(dd) < bestd[axis]) {
                bestd[axis] = std::fabs(dd);
                off[axis] = dd;
                guide[axis] = target;
            }
        };
        const auto overlaps = [](const float* a, const float* b) {
            return a[0] < b[2] - 1 && a[2] > b[0] + 1 && a[1] < b[3] - 1 && a[3] > b[1] + 1;
        };
        if (!io.KeyAlt) {
            try_at(0, 0.0f, c[0]);
            try_at(0, 1280.0f, c[2]);
            try_at(1, 0.0f, c[1]);
            try_at(1, 720.0f, c[3]);
            for (const auto& r : rows) {
                if (r.k == g_insp_sel || big(r.b) || overlaps(r.b, c) || overlaps(r.b, g_drag.b0)) {
                    continue;
                }
                const float gap_y = std::max(r.b[1] - c[3], c[1] - r.b[3]);
                const float gap_x = std::max(r.b[0] - c[2], c[0] - r.b[2]);
                if (gap_y <= 24.0f) { // side by side: left/right edges touch or line up
                    try_at(0, r.b[2], c[0]);
                    try_at(0, r.b[0], c[2]);
                    try_at(0, r.b[0], c[0]);
                    try_at(0, r.b[2], c[2]);
                }
                if (gap_x <= 24.0f) { // above/below: top/bottom edges touch or line up
                    try_at(1, r.b[3], c[1]);
                    try_at(1, r.b[1], c[3]);
                    try_at(1, r.b[1], c[1]);
                    try_at(1, r.b[3], c[3]);
                }
            }
        }
        ElemCfg cfg = g_drag.c0;
        cfg.dx = std::clamp(cfg.dx + d[0] + off[0], -1280.0f, 1280.0f);
        cfg.dy = std::clamp(cfg.dy + d[1] + off[1], -1280.0f, 1280.0f);
        if (cfg.dx != g_drag.cur.dx || cfg.dy != g_drag.cur.dy) {
            g_drag.cur = cfg;
            SetElem(g_drag.key, cfg, false);
        }
        const ImU32 gc = IM_COL32(255, 60, 220, 230);
        if (!std::isnan(guide[0])) {
            dl->AddLine({guide[0] * kx, 0.0f}, {guide[0] * kx, io.DisplaySize.y}, gc, 1.5f);
        }
        if (!std::isnan(guide[1])) {
            dl->AddLine({0.0f, guide[1] * ky}, {io.DisplaySize.x, guide[1] * ky}, gc, 1.5f);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (g_drag.active) {
            SetElem(g_drag.key, g_drag.cur, true);
        } else if (g_drag.pending) {
            g_insp_sel = g_insp_hover; // a click on the selected box without dragging
        }
        g_drag.pending = g_drag.active = false;
    }
    const auto rect = [&](const float* b, ImU32 col, float th) {
        dl->AddRect({b[0] * kx - 1, b[1] * ky - 1}, {b[2] * kx + 1, b[3] * ky + 1}, col, 0.0f, 0, th);
    };
    const Row* hover = nullptr;
    const Row* sel = nullptr;
    for (const auto& r : rows) {
        if (r.k == g_insp_hover) {
            hover = &r;
        }
        if (r.k == g_insp_sel) {
            sel = &r;
        }
        if (!g_insp_big && big(r.b)) {
            continue;
        }
        rect(r.b, IM_COL32(80, 200, 255, 110), 1.0f);
        if (g_insp_labels) {
            dl->AddText({r.b[0] * kx + 2, r.b[1] * ky + 1}, IM_COL32(80, 200, 255, 200), r.path.c_str());
        }
    }
    if (sel) {
        for (const auto& p : sel->parts) {
            rect(p.data(), IM_COL32(255, 140, 0, 160), 1.0f);
        }
        rect(sel->b, IM_COL32(255, 220, 0, 255), 3.0f);
    }
    if (hover) {
        rect(hover->b, IM_COL32(255, 255, 255, 255), 2.0f);
        const std::string label = hover->key + "\n" + hover->path;
        const ImVec2 at{hover->b[0] * kx, std::max(0.0f, hover->b[1] * ky - 36.0f)};
        const ImVec2 sz = ImGui::CalcTextSize(label.c_str());
        dl->AddRectFilled({at.x - 2, at.y - 2}, {at.x + sz.x + 2, at.y + sz.y + 2}, IM_COL32(0, 0, 0, 200));
        dl->AddText(at, IM_COL32(255, 255, 255, 255), label.c_str());
    }
    if (adv) {
        ImGui::Text("%zu elements on screen; %u uploads outside a layout draw", rows.size(),
                    g_insp_outside.load());
    }
    if (sel) {
        ImGui::SeparatorText("Selected");
        if (adv) {
            ImGui::TextUnformatted(sel->key.c_str());
            ImGui::TextUnformatted(sel->path.c_str());
            ImGui::Text("box %.0f, %.0f - %.0f, %.0f (canvas 1280x720), %u pieces", sel->b[0], sel->b[1],
                        sel->b[2], sel->b[3], sel->pieces);
        }
        if (ImGui::SmallButton("Clear selection")) {
            g_insp_sel = 0;
        }
        ElementControls(sel->gui, sel->chain);
    }
    if (!adv) {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.key != b.key ? a.key < b.key : a.path < b.path;
    });
    if (ImGui::BeginTable("elems", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0, 260))) {
        ImGui::TableSetupColumn("layout");
        ImGui::TableSetupColumn("element");
        ImGui::TableSetupColumn("box", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::PushID(static_cast<int>(r.k ^ (r.k >> 32)));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(r.key.c_str(), r.k == g_insp_sel, ImGuiSelectableFlags_SpanAllColumns)) {
                g_insp_sel = r.k;
            }
            if (ImGui::IsItemHovered()) {
                g_insp_row_hover = r.k; // highlighted next frame
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f,%.0f %.0fx%.0f", r.b[0], r.b[1], r.b[2] - r.b[0], r.b[3] - r.b[1]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

/// True when the top-level instance's layout is a `self` (world-placed) layout in the destretch table.
bool SelfLayoutOf(uint32_t node) {
    uint32_t gui = 0;
    if (!forge_api->mem_read(node + 0x70, &gui, 4) || !gui) {
        return false;
    }
    std::shared_lock lk{g_table_lock};
    const auto it = g_table.find(gui);
    return it != g_table.end() && it->second.ax == kAnchorSelf;
}

bool BigBox(const float* b) {
    return b[2] - b[0] > 1200.0f && b[3] - b[1] > 650.0f;
}

/// True for a layout that several screens share (the item set: one instance, drawn wherever the
/// screen showing it places it).
bool SharedLayoutOf(uint32_t node) {
    thread_local std::unordered_map<uint32_t, bool> cache;
    thread_local uint32_t epoch = ~0u;
    if (epoch != g_gui_epoch.load(std::memory_order_relaxed)) {
        cache.clear();
        epoch = g_gui_epoch.load(std::memory_order_relaxed);
    }
    uint32_t gui = 0;
    if (!forge_api->mem_read(node + 0x70, &gui, 4) || !gui) {
        return false;
    }
    const auto it = cache.find(gui);
    if (it != cache.end()) {
        return it->second;
    }
    bool shared = false;
    {
        std::scoped_lock lk{g_gui_lock};
        std::string low = GuiKey(gui);
        for (char& ch : low) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        shared = low.find("itemset") != std::string::npos;
    }
    cache[gui] = shared;
    return shared;
}

/// The global scale's pivot of every screen's top element (box, pivot, frame): a shared layout
/// takes the pivot of the screen whose box holds it (looked up from the last frames, before the
/// scale is applied, so draw order doesn't matter), so the two shrink about one point.
struct HostPivot {
    float box[4], px, py;
    uint32_t gen;
};
std::mutex g_host_lock;
std::map<uint32_t, HostPivot> g_hosts;
struct SharedPick {
    uint32_t host, ver;
};
std::map<uint32_t, SharedPick> g_shared_host; // shared layout instance -> the host it last picked
uint32_t g_host_ver; // bumped whenever a screen starts (or restarts) drawing

void SharedPivot(const ElemHit& hit, const float* box, float& px, float& py) {
    if (!hit.global) {
        return;
    }
    const bool shared = SharedLayoutOf(hit.node);
    const uint32_t gen = g_frame_gen.load(std::memory_order_relaxed);
    if (!shared) {
        // a host's box and pivot are the same for all its pieces: register once per game frame
        // (per-thread check, no lock) instead of on every upload
        thread_local std::unordered_map<uint32_t, uint32_t> done;
        thread_local uint32_t epoch = ~0u;
        const uint32_t ep = g_gui_epoch.load(std::memory_order_relaxed);
        if (epoch != ep) {
            done.clear();
            epoch = ep;
        }
        uint32_t& last = done.try_emplace(hit.node, ~0u).first->second;
        if (last == gen) {
            return;
        }
        last = gen;
        std::scoped_lock lk{g_host_lock};
        const auto [it, fresh] = g_hosts.try_emplace(hit.node);
        HostPivot& h = it->second;
        if (fresh || gen - h.gen > 2) {
            ++g_host_ver; // a screen appeared: the shared layout's pick may change
        }
        std::memcpy(h.box, box, sizeof(h.box));
        h.px = px;
        h.py = py;
        h.gen = gen;
        return;
    }
    std::scoped_lock lk{g_host_lock};
    // cached: keep following the host picked before while it is still drawn and no other screen
    // has started drawing since (a new screen may be the better host), so the pivot can't flip
    // between overlapping screens (that was the snapping)
    const auto ch = g_shared_host.find(hit.node);
    if (ch != g_shared_host.end() && ch->second.ver == g_host_ver) {
        const auto h = g_hosts.find(ch->second.host);
        if (h != g_hosts.end() && gen - h->second.gen <= 2) {
            px = h->second.px;
            py = h->second.py;
            return;
        }
    }
    const float cx = (box[0] + box[2]) * 0.5f, cy = (box[1] + box[3]) * 0.5f;
    float best = INFINITY;
    uint32_t pick = 0;
    for (const auto& [node, h] : g_hosts) {
        if (gen - h.gen > 3 || BigBox(h.box) || cx < h.box[0] || cx > h.box[2] || cy < h.box[1] || cy > h.box[3]) {
            continue;
        }
        const float area = (h.box[2] - h.box[0]) * (h.box[3] - h.box[1]);
        if (area < best) {
            best = area;
            pick = node;
            px = h.px;
            py = h.py;
        }
    }
    if (pick) {
        SharedPick& sp = g_shared_host[hit.node];
        const bool changed = sp.host != pick;
        sp = {pick, g_host_ver};
        if (changed) {
            uint32_t gui = 0;
            forge_api->mem_read(pick + 0x70, &gui, 4);
            std::string key;
            {
                std::scoped_lock lk2{g_gui_lock};
                key = GuiKey(gui);
            }
            Logf(FORGE_LOG_INFO, "shared layout %08X pivots with %s (%08X)", hit.node, key.c_str(), pick);
        }
    }
}

/// Scales/moves the world-matrix slot the draw context's renderer uploaded last, for the elements
/// the piece is part of (and the global scale).
void ApplySlot(uint32_t ctx, uint32_t piece) {
    if (!t_draw.empty() && MapLoading(t_draw.front().gui)) {
        return;
    }
    ElemHit hits[5];
    const int nh = (!t_draw.empty() || t_inst.size()) ? ElemChain(piece, hits, 5) : 0;
    if (!nh) {
        return;
    }
    uint32_t renderer = 0, dst = 0;
    float m[16];
    if (!forge_api->mem_read(ctx + 0x04, &renderer, 4) ||
        !forge_api->mem_read(renderer + 0x18BC, &dst, 4) || !dst || dst == t_last_slot ||
        !forge_api->mem_read(dst, m, sizeof(m))) {
        return;
    }
    t_last_slot = dst;
    float b[4];
    PieceBox(piece, m, b); // where the layout (and the destretch) put it: the elements' boxes
    float org[5][2];
    bool has_org[5];
    for (int i = 0; i < nh; ++i) { // from the matrix as the destretch left it, before any change below
        has_org[i] = ElemOrigin(hits[i].node, piece, m, org[i]);
    }
    bool any = false;
    for (int i = 0; i < nh; ++i) {
        float px, py, box[4];
        if (!ElemPivot(hits[i], piece, b, has_org[i] ? org[i] : nullptr, px, py, box) ||
            (hits[i].global && BigBox(box))) {
            continue;
        }
        if (hits[i].global && SelfLayoutOf(hits[i].node)) {
            // world-placed tags: the global scale pivots on the tag's own position, so it keeps
            // tracking what it is attached to (a box-edge pivot slid it off at scales below 1)
            float ax, ay, gm[16];
            if (SelfAnchor(t_inst.empty() ? piece : t_inst.back(), ax, ay) &&
                forge_api->mem_read(piece + 0x10, gm, sizeof(gm)) && std::fabs(gm[0]) > 1e-4f &&
                std::fabs(gm[5]) > 1e-4f) {
                const float kx = m[0] / gm[0], ky = m[5] / gm[5];
                if (kx > 0.1f && kx < 10.0f && ky > 0.1f && ky < 10.0f) {
                    px = m[12] + (ax - gm[12]) * kx;
                    py = m[13] + (ay - gm[13]) * ky;
                }
            }
        }
        SharedPivot(hits[i], box, px, py);
        const ElemCfg& c = hits[i].cfg;
        for (int r = 0; r < 3; ++r) {
            m[r * 4] *= c.scale;
            m[r * 4 + 1] *= c.scale;
        }
        m[12] = px + (m[12] - px) * c.scale + c.dx;
        m[13] = py + (m[13] - py) * c.scale + c.dy;
        any = true;
    }
    if (any) {
        forge_api->mem_write(dst, m, sizeof(m));
        ++g_elem_applied;
    }
}

void CountUpload() {
    if (g_insp_on.load(std::memory_order_relaxed) && !t_draw.empty() && t_draw.front().gui) {
        std::scoped_lock lk{g_lc_lock};
        ++g_lc[t_draw.front().gui].uploads;
    }
}

void OnWorldUploaded(ForgeCpu*, const uint32_t args[4], void*) {
    CountUpload();
    SqueezeSlot(args[0], args[1] - 0x10); // destretch first, elements on the destretched matrix
    ApplySlot(args[0], args[1] - 0x10);
    InspectUpload(args[0], args[1] - 0x10, args[4]);
}

// the minimap's map object (vtable +176C274, draw slot 0x540EAC) copies its matrix into a new slot
// inline, like text; 0x541254 is where that copy is done (r5 = object, r11 = draw context)
constexpr uint32_t kMapObjDone = 0x541254;
ForgeHookAction OnMapObjDrawn(ForgeCpu* cpu, void*) {
    CountUpload();
    SqueezeSlot(cpu->r[11], cpu->r[5]);
    ApplySlot(cpu->r[11], cpu->r[5]);
    InspectUpload(cpu->r[11], cpu->r[5], cpu->r[14]);
    return FORGE_HOOK_CONTINUE;
}

// text (0xA3DA50, r0 = piece, r1 = draw context) copies its matrix into a new slot inline
void OnTextDrawn(ForgeCpu*, const uint32_t args[4], void*) {
    CountUpload();
    SqueezeSlot(args[1], args[0]);
    ApplySlot(args[1], args[0]);
    InspectUpload(args[1], args[0], args[4]);
}

// clip rectangles: 0xA37988 (scissor-mask container: r0 = it) and 0xA4F420 (mask object) write
// the box of the transformed corners as ints {x0, y0, x1, y1} (canvas units) to r1; the elements
// they clip in follow their scale/move.
void OnClipRect(ForgeCpu*, const uint32_t args[4], void*) {
    if (!args[1]) {
        return;
    }
    SqueezeClip(args);
    if (!t_draw.empty() && MapLoading(t_draw.front().gui)) {
        return;
    }
    ElemHit hits[5];
    const int nh = ElemChain(args[0], hits, 5);
    int32_t r[4];
    if (!nh || !forge_api->mem_read(args[1], r, sizeof(r))) {
        return;
    }
    float f[4];
    for (int i = 0; i < 4; ++i) {
        f[i] = static_cast<float>(r[i]);
    }
    bool any = false;
    for (int i = 0; i < nh; ++i) {
        float px, py, box[4];
        if (!ElemPivot(hits[i], 0, nullptr, nullptr, px, py, box) || (hits[i].global && BigBox(box))) {
            continue;
        }
        SharedPivot(hits[i], box, px, py);
        const ElemCfg& c = hits[i].cfg;
        f[0] = px + (f[0] - px) * c.scale + c.dx;
        f[2] = px + (f[2] - px) * c.scale + c.dx;
        f[1] = py + (f[1] - py) * c.scale + c.dy;
        f[3] = py + (f[3] - py) * c.scale + c.dy;
        any = true;
    }
    if (!any) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        r[i] = static_cast<int32_t>(std::floor(f[i] + 0.5f));
    }
    forge_api->mem_write(args[1], r, sizeof(r));
}

/// The two mods this one replaces: both loaded would squeeze everything twice.
bool OldModsInstalled() {
    static const bool old = [] {
        const auto exists = [](const std::string& p) {
            FILE* f = std::fopen(p.c_str(), "rb");
            if (f) {
                std::fclose(f);
            }
            return f != nullptr;
        };
        return exists(g_dir + "../../HudFix/plugins/hudfix.dll") || exists(g_dir + "../../UIScale/plugins/uiscale.dll") ||
               exists(g_dir + "../../HudFix/plugins/hudfix.so") || exists(g_dir + "../../UIScale/plugins/uiscale.so");
    }();
    return old;
}

void SettingsUi() {
    bool on = g_on.load();
    bool save = false;
    if (ImGui::Checkbox("UI Fix on", &on)) {
        g_on = on;
        ++g_elem_ver;
        save = true;
    }
    if (OldModsInstalled()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "The old HudFix / UIScale mods are still installed: remove them (UI Fix replaces both).");
    }
    const float a = g_aspect.load();
    bool ds = g_destretch.load();
    if (ImGui::Checkbox("Undo the stretching on wide screens", &ds)) {
        g_destretch = ds;
        save = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(a > 16.0f / 9.0f + 0.01f ? "(screen %.2f:1, UI squeezed to %.0f%%)" : "(screen %.2f:1, nothing to undo)",
                        a, 100.0f * std::min(1.0f, (16.0f / 9.0f) / a));
    bool simple = g_simple.load();
    if (ImGui::Checkbox("Simple destretch (squeeze evenly left and right around the centre)", &simple)) {
        g_simple = simple;
        save = true;
    }
    bool el = g_elements.load();
    if (ImGui::Checkbox("Apply my element scale / move rules (the global scale works without them)", &el)) {
        g_elements = el;
        ++g_elem_ver;
        save = true;
    }
    float s = g_global.load();
    if (ImGui::SliderFloat("Global scale (every element; 1 = original)", &s, 0.25f, 2.0f, "%.2f")) {
        g_global = s;
    }
    save |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    if (ImGui::SmallButton("1.0")) {
        g_global = 1.0f;
        save = true;
    }
    ImGui::TextDisabled("An element's own scale replaces the global one. Full-screen pictures and the "
                        "excluded layouts in uifix.ini stay as they are.");
    // whole-layout resets, each asks first (they replace every element change)
    static int ask = 0; // 1 = default for my screen, 2 = original game UI
    if (ImGui::Button("Default")) {
        ask = 1;
        ImGui::OpenPopup("uifix_confirm");
    }
    ImGui::SameLine();
    if (ImGui::Button("Original game UI")) {
        ask = 2;
        ImGui::OpenPopup("uifix_confirm");
    }
    ImGui::SameLine();
    const float wa = WindowAspect();
    ImGui::TextDisabled("(the shipped default)");
    if (ImGui::BeginPopup("uifix_confirm")) {
        ImGui::TextUnformatted(ask == 1 ? "Replace all your UI changes with the default for this screen?"
                                        : "Remove all UI changes (original size and places, stretched)?");
        if (ImGui::Button("Yes")) {
            if (ask == 1) {
                ApplyPreset(wa == 0.0f ? 16.0f / 9.0f : wa);
            } else {
                {
                    std::scoped_lock lk{g_mode_lock};
                    g_elem_cfg.clear();
                    g_global = 1.0f;
                    g_destretch = false;
                }
                ++g_elem_ver;
                SaveIni();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    bool adv = g_advanced.load();
    if (ImGui::Checkbox("Advanced (depth, element table, debug)", &adv)) {
        g_advanced = adv;
        InspectClear(); // the element depth changes
        save = true;
    }
    if (save) {
        SaveIni();
    }
}

} // namespace

extern "C" {

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info) {
    info->required_ver.major = FORGE_PC_VERSION_MAJOR;
    info->required_ver.minor = 2; // screen_size (0.3) is used when present
    std::strncpy(info->name, "UI Fix", sizeof(info->name) - 1);
}

FORGE_EXPORT void forge_onInit(void) {
    if (std::strcmp(forge_api->imgui_version, IMGUI_VERSION) != 0) {
        Logf(FORGE_LOG_ERROR, "built with Dear ImGui %s, Forge has %s", IMGUI_VERSION,
             forge_api->imgui_version);
    }
#ifdef _WIN32
    char path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&forge_onInit), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    g_ini_path = path;
#else
    Dl_info self{};
    dladdr(reinterpret_cast<const void*>(&forge_onInit), &self);
    g_ini_path = self.dli_fname ? self.dli_fname : "";
#endif
    g_dir = g_ini_path.substr(0, g_ini_path.find_last_of("\\/") + 1);
    g_ini_path = g_dir + "uifix.ini";
    LoadIni();
    LoadCache();
    UpdateAspect();
    if (OldModsInstalled()) {
        Logf(FORGE_LOG_WARN, "the old HudFix / UIScale mods are still installed: remove them, this mod replaces both");
    }
    const uint32_t main = forge_api->mem_getModuleBase("main");
    g_vptr = main + kGuiVptr;
    g_root_vptr = main + kInstRootVptr;
    bool ok = forge_api->hook_create(main + kGuiFactory, nullptr, OnGuiCreated, nullptr) &&
              forge_api->hook_create(main + kGuiDtor1, OnGuiDestroyed, nullptr, nullptr) &&
              forge_api->hook_create(main + kGuiDtor2, OnGuiDestroyed, nullptr, nullptr) &&
              forge_api->hook_create(main + kDrawChildren, OnDrawChildren, OnDrawChildrenDone, nullptr) &&
              forge_api->hook_create(main + kAnimDraw, OnAnimDraw, OnAnimDrawDone, nullptr) &&
              forge_api->hook_create(main + kTextDraw, nullptr, OnTextDrawn, nullptr) &&
              forge_api->hook_create(main + kMapObjDone, OnMapObjDrawn, nullptr, nullptr);
    for (const uint32_t fn : kClipRects) {
        ok = forge_api->hook_create(main + fn, nullptr, OnClipRect, nullptr) && ok;
    }
    for (const uint32_t fn : kMatrixBuilders) {
        ok = forge_api->hook_create(main + fn, nullptr, OnMatrixBuilt, nullptr) && ok;
    }
    for (const uint32_t fn : kWorldUploads) {
        ok = forge_api->hook_create(main + fn, nullptr, OnWorldUploaded, nullptr) && ok;
    }
    if (!ok) {
        Logf(FORGE_LOG_ERROR, "some hooks failed: scaling may be incomplete");
    }
    g_sweep_started = true;
    std::thread(Sweep).detach();
    std::scoped_lock lk{g_mode_lock};
    Logf(FORGE_LOG_INFO, "ready (%s, destretch %s, aspect %.4f, global scale %.2f, %zu element settings, %zu excluded layouts%s)",
         g_on.load() ? "on" : "off", g_destretch.load() ? "on" : "off", g_aspect.load(), g_global.load(), g_elem_cfg.size(), g_exclude.size(),
         g_need_preset.load() ? "; first start: default picked once the window size is known" : "");
}

FORGE_EXPORT void forge_onUpdate(float) {
    static uint32_t tick = 0;
    if (++tick % 60 == 0) {
        UpdateAspect();
    }
    if (tick % 1800 == 0 && g_cache_dirty.exchange(false)) {
        SaveCache(); // every minute or so while something new was learned
    }
    ApplyFix();
    ApplyShifts();
    PickDefault();
    RefreshElemTable();
    SavePendingAnchors();
    InspectFrame();
    static uint32_t frames = 0, last = 0;
    if (++frames % 600 == 0 && g_elem_applied.load() != last) {
        last = g_elem_applied.load();
        Logf(FORGE_LOG_INFO, "%u element draws adjusted", last);
    }
}

/// Exit and hot reload: the startup sweep thread must be done before the dll goes away.
FORGE_EXPORT void forge_onUnload(void) {
    while (g_sweep_started.load() && !g_sweep_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // past their last instructions
    SaveCache(); // the boxes kept fresh since the last save too
    // canvases nudged for a re-layout go back to 1280x720
    std::scoped_lock lk{g_gui_lock};
    for (const auto& [gui, wh] : g_widened) {
        uint32_t cur[2]{};
        if (forge_api->mem_read(gui + kViewW, cur, 8) && cur[0] == wh.first && cur[1] == wh.second) {
            const uint32_t v[2]{1280, 720};
            forge_api->mem_write(gui + kViewW, v, 8);
        }
    }
    g_widened.clear();
}

FORGE_EXPORT void forge_onImGuiInit(void* ctx, void* alloc, void* free_fn, void* user) {
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(alloc),
                                 reinterpret_cast<ImGuiMemFreeFunc>(free_fn), user);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
}

FORGE_EXPORT void forge_onImGuiRender(void) {
    if (!ImGui::CollapsingHeader("UI Fix")) { // collapsed by default, inside Forge's "Plugin UI"
        return;
    }
    SettingsUi();
    if (g_advanced.load() && ImGui::TreeNodeEx("Debug: layouts", 0)) {
        {
            std::scoped_lock lk{g_gui_lock};
            ImGui::Text("%zu layouts tracked%s", g_guis.size(), g_sweep_done ? "" : "; sweeping...");
        }
        {
            std::shared_lock lk{g_table_lock};
            ImGui::Text("%zu layouts destretched", g_table.size());
        }
        LayoutsUi();
        ImGui::TreePop();
    }
    InspectorUi();
}

} // extern "C"
