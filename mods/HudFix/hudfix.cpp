// HUD Fix: keeps MHGU's HUD and menus in proportion on wide screens (a Forge PC plugin).
// SPDX-License-Identifier: MIT
//
// The game lays every screen out in a 1280x720 canvas (one rGUI layout per HUD element / menu,
// view size set by the factory main+0xB7F1D4) that is stretched over the whole window, so on
// 21:9 everything is too wide. The canvas is left alone (the game places some pieces from its
// centre); instead each layout is squeezed horizontally by 16:9 / aspect around an anchor
// (left edge, centre or right edge) chosen per layout in hudfix_layouts.ini:
// - at draw time (default): while a layout draws (0xA34070, nested per container level from the
//   layout root), every world matrix the GUI uploads for the GPU (0xA3E3B0, 0xA4EB9C, text
//   0xA3DA50) and every clip rectangle (0xA37988, 0xA4F420) is squeezed. The game's own
//   positions never change, so things that track the world don't drift.
// - game-side (" game" after the mode): the matrices the game builds for a layout's top-level
//   pieces (0xA36FA4, 0xA4AD5C, 0xA4C2B4) are squeezed in place, so what the game derives from
//   positions (masks of scrolling icons) follows. Item bar, ammo and Prowler item bar use it.
// - keep: unchanged (full-screen backgrounds, name tags, the 3D gear preview).
// "Constrain UI to 16:9" centres everything that isn't kept (equal margins).
//
// The menu also holds two research tools used to find all this: a layout explorer and a memory
// scanner for 1280x720 canvas values (results in hudfix.ini).

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <chrono>
#include <thread>
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

// The game's render size: sFestaRender +0x44BAC0 / +0x44BAC4 (what game_settings' native
// render writes).
constexpr uint32_t kRenderW = 0x44BAC0;
constexpr uint32_t kRenderH = 0x44BAC4;

// rGUI (GUI layout resource)
constexpr uint32_t kGuiFactory = 0xB7F1D4; // returns a new rGUI in r0
constexpr uint32_t kGuiDtor1 = 0xB7F260;   // r0 = rGUI
constexpr uint32_t kGuiDtor2 = 0xB7F2E0;
constexpr uint32_t kGuiVptr = 0x1794C9C;   // main offset stored at +0
constexpr uint32_t kViewW = 0x70;
constexpr uint32_t kViewH = 0x74;

void Logf(ForgeLogLevel level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    forge_api->log(level, "hudfix", buf);
}

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float Float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

/// What a hit looks like and how "widen" changes it (f = game aspect / (16/9) > 1).
enum class Kind {
    SizeF,    // float width, height: width *= f
    SizeI,    // int width, height: width *= f
    RecipF,   // float 1/w or 2/w style, then the height form: first /= f
    MatrixF,  // 4x4 float ortho: m[0] = 2/w ... m[5] = -2/h: m[0] /= f
};

struct Pattern {
    const char* name;
    Kind kind;
    uint32_t a;     // word at +0
    uint32_t b;     // word at +b_off
    uint32_t b_off; // bytes
};

const Pattern kPatterns[] = {
    {"1280.0, 720.0", Kind::SizeF, Bits(1280.0f), Bits(720.0f), 4},
    {"1720.0, 720.0", Kind::SizeF, Bits(1720.0f), Bits(720.0f), 4},
    {"640.0, 360.0", Kind::SizeF, Bits(640.0f), Bits(360.0f), 4},
    {"int 1280, 720", Kind::SizeI, 1280, 720, 4},
    {"int 1720, 720", Kind::SizeI, 1720, 720, 4},
    {"1/1280, 1/720", Kind::RecipF, Bits(1.0f / 1280.0f), Bits(1.0f / 720.0f), 4},
    {"2/1280, -2/720", Kind::RecipF, Bits(2.0f / 1280.0f), Bits(-2.0f / 720.0f), 4},
    {"2/1280, 2/720", Kind::RecipF, Bits(2.0f / 1280.0f), Bits(2.0f / 720.0f), 4},
    {"matrix 2/1280 .. -2/720", Kind::MatrixF, Bits(2.0f / 1280.0f), Bits(-2.0f / 720.0f), 20},
    {"matrix 2/1280 .. 2/720", Kind::MatrixF, Bits(2.0f / 1280.0f), Bits(2.0f / 720.0f), 20},
    // with the HUD Fix widening at 21:9 (view 1720 wide): where the GUI's screen matrix lives
    {"matrix 2/1720 .. -2/720", Kind::MatrixF, Bits(2.0f / 1720.0f), Bits(-2.0f / 720.0f), 20},
    {"matrix 2/1720 .. 2/720", Kind::MatrixF, Bits(2.0f / 1720.0f), Bits(2.0f / 720.0f), 20},
    {"2/1720, -2/720", Kind::RecipF, Bits(2.0f / 1720.0f), Bits(-2.0f / 720.0f), 4},
    {"1/1720, 1/720", Kind::RecipF, Bits(1.0f / 1720.0f), Bits(1.0f / 720.0f), 4},
    {"860.0, 360.0", Kind::SizeF, Bits(860.0f), Bits(360.0f), 4},
    {"scale 2.0 .. 2.0 (matrix)", Kind::MatrixF, Bits(2.0f), Bits(2.0f), 20},
};

struct Hit {
    uint32_t addr;
    int pattern;
    uint32_t original_a; // value of the first word when found
    bool hold{false};
    bool test{false};    // widened by the bisection, not saved
};

std::mutex g_lock;
std::vector<Hit> g_hits;               // guarded by g_lock
std::set<std::pair<uint32_t, int>> g_saved_holds; // from hudfix.ini, re-applied after a scan
std::atomic<bool> g_scanning{false};
std::atomic<uint32_t> g_scan_progress{0}; // pages done (of 1M)
std::atomic<float> g_aspect{16.0f / 9.0f};
std::atomic<int> g_place{0}; // 0 = anchor to edges (per layout), 1 = constrain UI to 16:9
std::atomic<int> g_hud_height{720}; // view height: 720 normal, -1 native, else custom
std::atomic<uint32_t> g_render_h{0};
std::string g_ini_path;
std::string g_status = "not scanned yet";

// Bisection: candidates (indices into g_hits); the first half is widened while the player
// looks. "Changed" keeps that half, "no change" the other one.
std::vector<size_t> g_cand;
int g_bisect_pattern{-1}; // -1 = all patterns
int g_rounds{};
std::string g_bisect_msg;

void Restore(const struct Hit& h);
void Inspect(const struct Hit& h);

float WidenFactor() {
    return g_aspect.load() / (16.0f / 9.0f);
}

void Save() {
    FILE* f = std::fopen(g_ini_path.c_str(), "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "; HUD Fix settings, written by the menu\n");
    std::fprintf(f, "; hud_height: 720 = normal HUD size, -1 = native (render height, smaller)\n");
    std::fprintf(f, "hud_height = %d\n", g_hud_height.load());
    std::fprintf(f, "; placement: 0 = anchor to edges (per layout), 1 = constrain UI to 16:9\n");
    std::fprintf(f, "placement = %d\n", g_place.load());
    for (const auto& h : g_hits) {
        if (h.hold) {
            std::fprintf(f, "hold = %x %d\n", h.addr, h.pattern);
        }
    }
    std::fclose(f);
}

void Load() {
    FILE* f = std::fopen(g_ini_path.c_str(), "r");
    if (!f) {
        return;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        uint32_t addr;
        int pattern;
        int hh = 0;
        if (std::sscanf(line, "hud_height = %d", &hh) == 1) {
            g_hud_height = hh;
        } else if (std::sscanf(line, "placement = %d", &hh) == 1) {
            g_place = hh;
        } else if (std::sscanf(line, "hold = %x %d", &addr, &pattern) == 2) {
            g_saved_holds.insert({addr, pattern});
        }
    }
    std::fclose(f);
}

/// Every mapped page of the 32-bit guest address space, 4-byte aligned hits of every pattern.
void Scan() {
    std::vector<Hit> hits;
    constexpr uint32_t kPage = 0x1000;
    for (uint64_t page = 0; page < 0x100000000ull; page += kPage) {
        g_scan_progress = static_cast<uint32_t>(page / kPage);
        const auto* p = static_cast<const uint32_t*>(
            forge_api->mem_ptr(static_cast<uint32_t>(page), kPage));
        if (!p) {
            continue;
        }
        for (uint32_t i = 0; i < kPage / 4; ++i) {
            const uint32_t w = p[i];
            for (int k = 0; k < static_cast<int>(std::size(kPatterns)); ++k) {
                const Pattern& pt = kPatterns[k];
                if (w != pt.a) {
                    continue;
                }
                const uint32_t addr = static_cast<uint32_t>(page) + i * 4;
                uint32_t b = 0;
                const uint32_t j = i + pt.b_off / 4;
                if (j < kPage / 4) {
                    b = p[j];
                } else {
                    forge_api->mem_read(addr + pt.b_off, &b, 4);
                }
                if (b == pt.b && hits.size() < 4000) {
                    hits.push_back({addr, k, w});
                }
            }
        }
    }
    std::scoped_lock lk{g_lock};
    for (auto& h : hits) {
        h.hold = g_saved_holds.count({h.addr, h.pattern}) != 0;
    }
    for (const auto& h : g_hits) {
        if (h.hold) {
            g_saved_holds.insert({h.addr, h.pattern}); // keep holds found since the last scan
        }
        if (h.test && !h.hold) {
            Restore(h);
        }
    }
    g_cand.clear(); // a running bisection refers to the old list
    g_hits = std::move(hits);
    for (auto& h : g_hits) {
        h.hold = h.hold || g_saved_holds.count({h.addr, h.pattern}) != 0;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%zu hits", g_hits.size());
    g_status = buf;
    Logf(FORGE_LOG_INFO, "scan: %zu hits", g_hits.size());
    for (const auto& h : g_hits) {
        Logf(FORGE_LOG_INFO, "  hit %#010x  %s", h.addr, kPatterns[h.pattern].name);
    }
    for (const auto& h : g_hits) {
        if (h.hold) {
            Inspect(h);
        }
    }
    g_scanning = false;
}

void StartScan() {
    if (g_scanning.exchange(true)) {
        return;
    }
    g_status = "scanning...";
    std::thread(Scan).detach();
}

/// Writes the widened value of a held hit (every frame: the game may rewrite it).
void Apply(const Hit& h) {
    const Pattern& pt = kPatterns[h.pattern];
    const float f = WidenFactor();
    uint32_t v = h.original_a;
    switch (pt.kind) {
    case Kind::SizeF:
        v = Bits(Float(h.original_a) * f);
        break;
    case Kind::SizeI:
        v = static_cast<uint32_t>(std::lround(static_cast<double>(h.original_a) * f));
        break;
    case Kind::RecipF:
    case Kind::MatrixF:
        v = Bits(Float(h.original_a) / f);
        break;
    }
    forge_api->mem_write(h.addr, &v, 4);
}

/// Logs where a held hit sits: the nearest word before it that points into main (a vtable,
/// i.e. the start of the object holding the pair) and the 0x40 bytes around it.
void Inspect(const Hit& h) {
    const uint32_t main = forge_api->mem_getModuleBase("main");
    const uint32_t main_size = forge_api->mem_getModuleSize("main");
    std::string objs;
    int found = 0;
    for (uint32_t back = 4; back <= 0x800 && found < 3; back += 4) {
        uint32_t w = 0;
        if (!forge_api->mem_read(h.addr - back, &w, 4)) {
            break;
        }
        if (w >= main + 0x1400000 && w < main + main_size && (w & 3) == 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " [-%#x: main+%#x]", back, w - main);
            objs += buf;
            ++found;
        }
    }
    uint32_t around[16]{};
    forge_api->mem_read(h.addr - 0x20, around, sizeof(around));
    std::string hex;
    for (int i = 0; i < 16; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s%08x", i == 8 ? " |" : " ", around[i]);
        hex += buf;
    }
    Logf(FORGE_LOG_INFO, "inspect %08X (%s): vtable-like words before:%s", h.addr,
         kPatterns[h.pattern].name, objs.empty() ? " none" : objs.c_str());
    Logf(FORGE_LOG_INFO, "  bytes -0x20..+0x20:%s", hex.c_str());
}

void Restore(const Hit& h) {
    forge_api->mem_write(h.addr, &h.original_a, 4);
}

/// Sets the bisection's test flags: the first half of the candidates widened, the rest restored.
void BisectShow() {
    for (auto& h : g_hits) {
        if (h.test && !h.hold) {
            Restore(h);
        }
        h.test = false;
    }
    const size_t half = (g_cand.size() + 1) / 2;
    for (size_t i = 0; i < half; ++i) {
        g_hits[g_cand[i]].test = true;
    }
}

void BisectStart() {
    g_cand.clear();
    for (size_t i = 0; i < g_hits.size(); ++i) {
        if (!g_hits[i].hold && (g_bisect_pattern < 0 || g_hits[i].pattern == g_bisect_pattern)) {
            g_cand.push_back(i);
        }
    }
    g_rounds = 0;
    g_bisect_msg = g_cand.empty() ? "no candidates" : "";
    BisectShow();
}

void BisectAnswer(bool changed) {
    const size_t half = (g_cand.size() + 1) / 2;
    if (changed) {
        g_cand.resize(half);
    } else {
        g_cand.erase(g_cand.begin(), g_cand.begin() + half);
    }
    ++g_rounds;
    if (g_cand.size() == 1) {
        Hit& h = g_hits[g_cand[0]];
        h.hold = true; // found: keep it widened and save it
        char buf[160];
        std::snprintf(buf, sizeof(buf), "found %08X (%s) after %d answers", h.addr,
                      kPatterns[h.pattern].name, g_rounds);
        g_bisect_msg = buf;
        Logf(FORGE_LOG_INFO, "bisect: %s", buf);
        g_cand.clear();
        Save();
    } else if (g_cand.empty()) {
        g_bisect_msg = "none of them changed the HUD";
    }
    BisectShow();
}

void UpdateAspect() {
    const uint32_t render = forge_api->singleton_getByName("sFestaRender");
    uint32_t w = 0, h = 0;
    if (render && forge_api->mem_read(render + kRenderW, &w, 4) &&
        forge_api->mem_read(render + kRenderH, &h, 4) && w >= 320 && h >= 240 && w <= 16384 &&
        h <= 16384) {
        g_aspect = static_cast<float>(w) / static_cast<float>(h);
        g_render_h = h;
    }
}

int g_frames{};

// --- the fix ---
std::atomic<bool> g_fix_on{true};
std::mutex g_gui_lock;
std::set<uint32_t> g_guis;      // live rGUI objects
std::set<uint32_t> g_bump;      // layouts to re-lay out: canvas 2 narrower for one frame
std::map<uint32_t, std::string> g_gui_keys; // rGUI -> key (guarded by g_gui_lock)
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_widened; // ours -> size we wrote
uint32_t g_vptr{};              // main + kGuiVptr
std::atomic<uint32_t> g_widened_count{0};
std::atomic<bool> g_sweep_started{false};
std::atomic<bool> g_sweep_done{false};

void OnGuiCreated(ForgeCpu* cpu, const uint32_t[4], void*) {
    uint32_t vt = 0;
    if (forge_api->mem_read(cpu->r[0], &vt, 4) && vt == g_vptr) {
        std::scoped_lock lk{g_gui_lock};
        g_guis.insert(cpu->r[0]);
    }
}

ForgeHookAction OnGuiDestroyed(ForgeCpu* cpu, void*) {
    std::scoped_lock lk{g_gui_lock};
    g_guis.erase(cpu->r[0]);
    g_widened.erase(cpu->r[0]);
    g_gui_keys.erase(cpu->r[0]);
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

// --- placement: what each layout does on a wide screen ---
// keep = original (stretched, for full-screen pictures), left/center/right = widened canvas and
// the layout's top-level containers shifted by 0 / half / all of the extra width.
// follow = for layouts the game places on things in the world (name tags): X scaled by W/1280.
enum class Mode { Keep, Left, Center, Right, Follow };
constexpr const char* kModeNames[] = {"keep", "left", "center", "right", "follow"};
std::mutex g_mode_lock;
std::map<std::string, Mode> g_modes; // layout key ("GUI\01_quest\hud\hud0") -> mode
// game-side layouts are squeezed by changing the positions the game computes (clip boxes, masks
// and 3D viewports it derives from them follow) instead of at draw time; only for layouts that
// don't track anything (they would drift)
std::set<std::string> g_gameside;
std::string g_layouts_path;

std::string LayoutKey(std::string p) {
    const auto pos = p.find("GUI\\");
    return pos == std::string::npos ? p : p.substr(pos);
}

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
    return Mode::Keep;
}

/// The mode the player chose for a layout (unlisted: keep).
Mode ListedMode(const std::string& key) {
    std::scoped_lock lk{g_mode_lock};
    const auto it = g_modes.find(key);
    return it == g_modes.end() ? Mode::Keep : it->second;
}

bool GameSide(const std::string& key) {
    std::scoped_lock lk{g_mode_lock};
    return g_gameside.count(key) != 0;
}

/// The mode in effect (constrain to 16:9 centres everything that isn't kept).
Mode ModeOf(const std::string& key) {
    const Mode m = ListedMode(key);
    return (g_place.load() == 1 && m != Mode::Keep && m != Mode::Follow) ? Mode::Center : m;
}

void LoadLayouts() {
    FILE* f = std::fopen(g_layouts_path.c_str(), "r");
    if (!f) {
        return;
    }
    char line[512];
    std::scoped_lock lk{g_mode_lock};
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '[' || line[0] == '#') {
            continue;
        }
        char* eq = std::strchr(line, '=');
        if (!eq) {
            continue;
        }
        std::string key(line, eq - line);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        const char* v = eq + 1;
        while (*v == ' ' || *v == '\t') {
            ++v;
        }
        if (!key.empty()) {
            g_modes[key] = ParseMode(v);
            if (std::strstr(v, "game")) {
                g_gameside.insert(key);
            } else {
                g_gameside.erase(key);
            }
        }
    }
    std::fclose(f);
}

void SaveLayouts() {
    FILE* f = std::fopen(g_layouts_path.c_str(), "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "; HUD Fix: what each GUI layout does on a wide screen (edited from the menu).\n"
                    "; keep = original (stretched), left / center / right = unstretched and\n"
                    "; pinned to that part of the screen. Unlisted layouts are kept.\n"
                    "; \" game\" after the mode = squeeze the game's own positions (clip boxes and\n"
                    "; 3D views follow), for layouts that don't track anything.\n[Layouts]\n");
    std::scoped_lock lk{g_mode_lock};
    for (const auto& [key, m] : g_modes) {
        std::fprintf(f, "%s = %s%s\n", key.c_str(), kModeNames[static_cast<int>(m)],
                     g_gameside.count(key) ? " game" : "");
    }
    std::fclose(f);
}

/// Layout key of a tracked rGUI (its path is set when the file loads; cached once known).
const std::string& GuiKey(uint32_t gui) {
    std::string& k = g_gui_keys[gui];
    if (k.empty()) {
        char path[65]{};
        if (forge_api->mem_read(gui + 0x08, path, 64) && path[0]) {
            k = LayoutKey(path);
        }
    }
    return k;
}

/// Target view height: 720 = normal size, the render height = native (HUD at its pixel size,
/// smaller), anything else = custom. Width follows the game's aspect.
uint32_t TargetHeight() {
    const int h = g_hud_height.load();
    if (h < 0) {
        return g_render_h.load() ? g_render_h.load() : 720;
    }
    return h >= 360 ? static_cast<uint32_t>(h) : 720;
}

/// The canvas stays 1280x720 (pieces the game places from the canvas centre, like window
/// backgrounds, drift when it is widened). It is only nudged 2 narrower for one frame: a
/// changed placement shows once the game re-lays the layout out, which a canvas size change
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
            if (it == g_widened.end()) {
                ++g_widened_count;
            }
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

// --- layout explorer (tool): live cGUIInstNull objects, the containers of a layout ---
// cGUIInstNull: DTI create fn main+0xA357D4 (new 0xC0-byte object in r0), vptr main+0x178F408.
// Constructor: +0x10..+0x4C identity matrix, +0x80..+0x8C 1.0 (colour?), +0x90..+0x9C zero
// (position?), +0xA0..+0xA8 1.0 (scale?). The layout file's instance record (36 bytes:
// id, attr, next sibling, first child, init param count, name, hash, first init param, extend)
// is found from the caller's registers when the object is made; header +0x24 = root index.
constexpr uint32_t kInstNullNew = 0xA357D4;  // DTI create: new object in r0
constexpr uint32_t kInstNullCtor = 0xA35950; // in-place constructor: this in r0
// The second container family (vptr main+0x178F5B8, 0xE0 bytes; red menu windows, the
// minimap): same +0x68 parent / +0x70 rGUI / +0x90 X layout. Create fns return the object in r0.
constexpr uint32_t kObjCreate[] = {0xA37F58, 0xA3A394, 0xA3A8B4};
constexpr uint32_t kObjCtor = 0xA38104; // in-place: this in r0
constexpr uint32_t kInstNullVptr = 0x178F408;
constexpr uint32_t kGuiFile = 0x64; // rGUI: loaded .gui file
constexpr uint32_t kGuiPath = 0x08; // MtResource path, char[64]

struct Inst {
    uint32_t obj;
    std::string name;   // from the layout file, "" if not found
    std::string layout; // rGUI path
    bool root{false};
    int field{0x90};
    float offset{0.0f};
    float base{0.0f};
    bool have_base{false};
    int tries{0};
    uint32_t vptr{0}; // final class (read a frame after construction)
};

/// The true root of a layout (cGUIInstRoot, not a cGUIInstNull): the object every top-level
/// container's parent pointer (+0x68) leads to.
struct Mother {
    uint32_t obj;
    uint32_t vptr{0};
    std::string layout;
    int children{0};
    int field{0x40}; // matrix translation x (identity matrix at +0x10)
    float offset{0.0f};
    float base{0.0f};
    bool have_base{false};
    bool seen{false};
};

std::mutex g_inst_lock;
std::map<uint32_t, Mother> g_mothers; // guarded by g_inst_lock
int g_mother_frames{};
std::vector<Inst> g_insts; // guarded by g_inst_lock
std::atomic<uint32_t> g_inst_made{0};
uint32_t g_inst_vptr{};
char g_inst_filter[64] = "Nul_";
bool g_inst_roots_only{true};

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
    if (const auto* t = static_cast<const uint32_t*>(forge_api->mem_ptr(g.table, g.count * 36))) {
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

void SetName(Inst& out, const GuiFile& g, uint32_t rec, uint32_t index) {
    uint32_t text = 0;
    forge_api->mem_read(rec + 0x14, &text, 4);
    out.name = ReadStr(g.strings + text);
    out.layout = ReadStr(g.gui + kGuiPath);
    out.root = index == g.root;
}

/// Names a container: its id (+0x04) is the instance id in its layout file. The layout is the
/// tracked rGUI that its owner pointers (+0x5C/+0x6C/+0x70, or the objects they point to) lead
/// to; failing that, the only layout with a record of that id at the container's position.
bool ResolveName(Inst& out) {
    uint32_t w[0xC0 / 4]{};
    if (!forge_api->mem_read(out.obj, w, sizeof(w))) {
        return false;
    }
    const uint32_t id = w[1];
    const float x = Float(w[0x90 / 4]), y = Float(w[0x94 / 4]);
    std::vector<GuiFile> files;
    {
        std::scoped_lock lk{g_gui_lock};
        for (const uint32_t gui : g_guis) {
            GuiFile g;
            if (ReadGuiFile(gui, g)) {
                files.push_back(g);
            }
        }
    }
    // pointer chain to the layout (depth 2)
    std::vector<uint32_t> linked;
    for (const uint32_t off : {0x5Cu, 0x6Cu, 0x70u}) {
        const uint32_t p = w[off / 4];
        linked.push_back(p);
        uint32_t v[0x100 / 4]{};
        if (p && forge_api->mem_read(p, v, sizeof(v))) {
            linked.insert(linked.end(), std::begin(v), std::end(v));
        }
    }
    for (const auto& g : files) {
        for (const uint32_t v : linked) {
            if (v != g.gui && v != g.file) {
                continue;
            }
            uint32_t index = 0;
            if (const uint32_t rec = FindRecord(g, id, &index)) {
                SetName(out, g, rec, index);
                return true;
            }
        }
    }
    // id + position
    const GuiFile* match = nullptr;
    uint32_t match_rec = 0, match_index = 0;
    int matches = 0;
    for (const auto& g : files) {
        uint32_t index = 0;
        const uint32_t rec = FindRecord(g, id, &index);
        if (rec && RecordFloat(g, rec, "PosX") == x && RecordFloat(g, rec, "PosY") == y) {
            if (!match || ReadStr(g.gui + kGuiPath) != ReadStr(match->gui + kGuiPath)) {
                ++matches;
            }
            match = &g;
            match_rec = rec;
            match_index = index;
        }
    }
    if (matches == 1) {
        SetName(out, *match, match_rec, match_index);
        return true;
    }
    return false;
}

void AddInst(uint32_t obj);

void OnInstNullMade(ForgeCpu* cpu, const uint32_t[4], void*) {
    AddInst(cpu->r[0]);
}

void OnInstNullBuilt(ForgeCpu*, const uint32_t args[4], void*) {
    AddInst(args[0]);
}

void AddInst(uint32_t obj) {
    Inst inst{obj};
    ++g_inst_made;
    std::scoped_lock lk{g_inst_lock};
    for (auto& i : g_insts) {
        if (i.obj == inst.obj) {
            i = inst; // the address was reused
            return;
        }
    }
    g_insts.push_back(inst);
}

/// Containers of every class built on cGUIInstNull: the vptr seen after construction must
/// stay (freed or reused memory changes it).
bool InstAlive(const Inst& i) {
    uint32_t vt = 0;
    return forge_api->mem_read(i.obj, &vt, 4) && i.vptr && vt == i.vptr;
}

void SettleVptrs() {
    const uint32_t main = forge_api->mem_getModuleBase("main");
    const uint32_t size = forge_api->mem_getModuleSize("main");
    for (auto& i : g_insts) {
        uint32_t vt = 0;
        if (!i.vptr && forge_api->mem_read(i.obj, &vt, 4) && vt >= main && vt < main + size) {
            i.vptr = vt;
        }
    }
}

void ApplyInstOffsets() {
    std::scoped_lock lk{g_inst_lock};
    std::erase_if(g_insts, [](const Inst& i) { return i.vptr && !InstAlive(i); });
    SettleVptrs();
    int budget = 4; // name lookups per frame
    for (auto& i : g_insts) {
        if (i.name.empty() && i.tries < 30 && budget > 0) {
            --budget;
            ++i.tries;
            ResolveName(i);
        }
    }
    if (++g_mother_frames % 30 == 0) {
        std::set<uint32_t> tracked;
        for (const auto& i : g_insts) {
            if (i.vptr) {
                tracked.insert(i.obj);
            }
        }
        for (auto& [obj, m] : g_mothers) {
            m.seen = false;
            m.children = 0;
        }
        for (const auto& i : g_insts) {
            if (!i.vptr) {
                continue;
            }
            uint32_t p = 0;
            forge_api->mem_read(i.obj + 0x68, &p, 4);
            if (!p || tracked.count(p)) {
                continue; // not top-level
            }
            uint32_t vt = 0;
            if (!forge_api->mem_read(p, &vt, 4)) {
                continue;
            }
            auto [it, fresh] = g_mothers.try_emplace(p, Mother{p});
            Mother& m = it->second;
            if (fresh || m.vptr != vt) {
                m = Mother{p};
                m.vptr = vt;
            }
            m.seen = true;
            ++m.children;
            if (m.layout.empty() && !i.layout.empty()) {
                m.layout = i.layout;
            }
        }
        std::erase_if(g_mothers, [](const auto& kv) { return !kv.second.seen; });
    }
    for (auto& [obj, m] : g_mothers) {
        if (m.offset == 0.0f && !m.have_base) {
            continue;
        }
        uint32_t vt = 0;
        if (!forge_api->mem_read(obj, &vt, 4) || vt != m.vptr) {
            continue;
        }
        if (!m.have_base) {
            uint32_t v = 0;
            forge_api->mem_read(obj + m.field, &v, 4);
            m.base = Float(v);
            m.have_base = true;
        }
        const uint32_t v = Bits(m.base + m.offset);
        forge_api->mem_write(obj + m.field, &v, 4);
    }
    for (auto& i : g_insts) {
        if (i.offset == 0.0f && !i.have_base) {
            continue;
        }
        if (!InstAlive(i)) {
            continue;
        }
        if (!i.have_base) {
            uint32_t v = 0;
            forge_api->mem_read(i.obj + i.field, &v, 4);
            i.base = Float(v);
            i.have_base = true;
        }
        const uint32_t v = Bits(i.base + i.offset);
        forge_api->mem_write(i.obj + i.field, &v, 4);
    }
}

void DumpInst(const Inst& i) {
    uint32_t w[0xC0 / 4]{};
    forge_api->mem_read(i.obj, w, sizeof(w));
    Logf(FORGE_LOG_INFO, "inst %08X '%s' (%s)%s:", i.obj, i.name.c_str(), i.layout.c_str(),
         i.root ? " root" : "");
    for (int row = 0; row < 0xC0 / 16; ++row) {
        Logf(FORGE_LOG_INFO, "  +%02X: %08x %08x %08x %08x  (%g %g %g %g)", row * 16,
             w[row * 4], w[row * 4 + 1], w[row * 4 + 2], w[row * 4 + 3], Float(w[row * 4]),
             Float(w[row * 4 + 1]), Float(w[row * 4 + 2]), Float(w[row * 4 + 3]));
    }
}

void ExplorerUi() {
    if (!ImGui::TreeNodeEx("Layout explorer (tool)", 0)) {
        return;
    }
    std::scoped_lock lk{g_inst_lock};
    size_t alive = 0, named = 0;
    for (const auto& i : g_insts) {
        alive += InstAlive(i);
        named += !i.name.empty();
    }
    ImGui::Text("%u containers made since start, %zu tracked, %zu alive, %zu named",
                g_inst_made.load(), g_insts.size(), alive, named);
    ImGui::TextWrapped("Drag 'X' on a layout's root container (e.g. Nul_hud0): if the whole "
                       "layout slides sideways, that field is its position.");
    ImGui::InputText("name filter", g_inst_filter, sizeof(g_inst_filter));
    ImGui::SameLine();
    ImGui::Checkbox("roots only", &g_inst_roots_only);
    if (ImGui::CollapsingHeader("Layout mothers (true roots)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("One row per live layout. Drag X: if the WHOLE layout (frames too) "
                           "moves, this is the per-layout shift. Field 40 = matrix x.");
        if (ImGui::Button("Dump mothers to forge.log")) {
            for (const auto& [obj, m] : g_mothers) {
                uint32_t w[0x80 / 4]{};
                forge_api->mem_read(obj, w, sizeof(w));
                Logf(FORGE_LOG_INFO, "mother %08X '%s' %d top-level, vptr main+%#x:", obj,
                     m.layout.c_str(), m.children, m.vptr - forge_api->mem_getModuleBase("main"));
                for (int row = 0; row < 0x80 / 16; ++row) {
                    Logf(FORGE_LOG_INFO, "  +%02X: %08x %08x %08x %08x  (%g %g %g %g)", row * 16,
                         w[row * 4], w[row * 4 + 1], w[row * 4 + 2], w[row * 4 + 3],
                         Float(w[row * 4]), Float(w[row * 4 + 1]), Float(w[row * 4 + 2]),
                         Float(w[row * 4 + 3]));
                }
            }
        }
        if (ImGui::BeginTable("mothers", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, 250))) {
            ImGui::TableSetupColumn("layout");
            ImGui::TableSetupColumn("field");
            ImGui::TableSetupColumn("X offset");
            ImGui::TableHeadersRow();
            for (auto& [obj, m] : g_mothers) {
                ImGui::PushID(static_cast<int>(obj));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s  (%d top, %08X)", m.layout.empty() ? "?" : m.layout.c_str(),
                            m.children, obj);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(70);
                if (ImGui::InputInt("##f", &m.field, 4, 16, ImGuiInputTextFlags_CharsHexadecimal)) {
                    m.field = std::clamp(m.field & ~3, 0, 0x7C);
                    m.have_base = false;
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("0")) {
                    m.offset = 0.0f;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##x", &m.offset, -1000.0f, 1000.0f, "%.0f");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::Button("Reset all X offsets")) {
        for (auto& i : g_insts) {
            i.offset = 0.0f;
        }
        for (auto& [obj, m] : g_mothers) {
            m.offset = 0.0f;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump shown to forge.log")) {
        for (const auto& i : g_insts) {
            if (InstAlive(i) && (!g_inst_roots_only || i.root) &&
                i.name.find(g_inst_filter) != std::string::npos) {
                DumpInst(i);
            }
        }
    }
    if (ImGui::BeginTable("insts", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, 400))) {
        ImGui::TableSetupColumn("container");
        ImGui::TableSetupColumn("layout");
        ImGui::TableSetupColumn("field");
        ImGui::TableSetupColumn("X offset");
        ImGui::TableHeadersRow();
        for (auto& i : g_insts) {
            if (!InstAlive(i) || (g_inst_roots_only && !i.root) ||
                i.name.find(g_inst_filter) == std::string::npos) {
                continue;
            }
            ImGui::PushID(static_cast<int>(i.obj));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s  %08X", i.name.empty() ? "?" : i.name.c_str(),
                        i.root ? " (root)" : "", i.obj);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(i.layout.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(70);
            if (ImGui::InputInt("##f", &i.field, 4, 16, ImGuiInputTextFlags_CharsHexadecimal)) {
                i.field = std::clamp(i.field & ~3, 0, 0xBC);
                i.have_base = false;
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("0")) {
                i.offset = 0.0f;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##x", &i.offset, -1000.0f, 1000.0f, "%.0f");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

// --- squeezing at draw time ---
// The game's positions are never changed (trackers that read them back would drift). The GUI
// draw uploads each piece's world matrix (r1 = piece + 0x10, canvas units, rows; x column =
// +0x00/+0x10/+0x20, translation +0x30) into a fresh constant-buffer slot through 0xA3E3B0 or
// 0xA4EB9C (r0 = draw context); the slot is at [[ctx + 4] + 0x18BC] afterwards. That copy is
// squeezed horizontally by 16:9 / aspect (and scaled by the HUD size) around the layout's anchor:
// x' = a + (x - a) * s.
// Which layout a draw belongs to: 0xA34070 (r0 = node, r1 = draw context) draws a node's children
// (first child +0x60, next sibling +0x64) and nests once per container level, starting at the
// layout root (cGUIInstRoot, vptr main+0x178F470; its children's +0x70 = the rGUI). A per-thread
// stack of those calls says which layout and which top-level container / tag is being drawn.
// follow (game-placed tags): anchored at the tag, the container just below top level.
constexpr uint32_t kInstRootVptr = 0x178F470;
constexpr uint32_t kDrawChildren = 0xA34070;
constexpr uint32_t kWorldUploads[] = {0xA3E3B0, 0xA4EB9C};
constexpr uint32_t kTextDraw = 0xA3DA50;
constexpr uint32_t kClipRects[] = {0xA37988, 0xA4F420};
struct LayoutShift {
    float ax, sx, sy; // x anchor (canvas units; < 0 = the piece's tag), scales
    bool game;        // squeezed game-side (positions) instead of at draw time
    bool operator==(const LayoutShift&) const = default;
};
std::shared_mutex g_table_lock;
std::unordered_map<uint32_t, LayoutShift> g_table; // rGUI -> squeeze (only placed layouts)
uint32_t g_root_vptr{};
std::atomic<uint32_t> g_shifted_builds{0};   // uploads squeezed
std::atomic<uint32_t> g_inherited_builds{0}; // uploads outside any layout draw
struct DrawLevel {
    uint32_t node, gui;
};
thread_local std::vector<DrawLevel> t_draw; // nested 0xA34070 calls on this thread

// draw-hook diagnostics, logged every ~10 s while the counts change
std::atomic<uint32_t> g_dg_calls{0}, g_dg_notop{0}, g_dg_unplaced{0}, g_dg_nodst{0}, g_dg_layouts{0};
std::atomic<uint32_t> g_rects{0}; // clip rectangles squeezed
uint32_t g_dg_last{0};

void LogDrawDiag() {
    const uint32_t calls = g_dg_calls.load();
    if (calls == g_dg_last) {
        return;
    }
    g_dg_last = calls;
    Logf(FORGE_LOG_INFO,
         "draws: %u uploads: %u outside a layout draw, %u in kept layouts, %u no slot, %u squeezed; "
         "%u layout draws, %u clip rects",
         calls, g_dg_notop.load(), g_dg_unplaced.load(), g_dg_nodst.load(), g_shifted_builds.load(),
         g_dg_layouts.load(), g_rects.load());
}

ForgeHookAction OnDrawChildren(ForgeCpu* cpu, void*) {
    const uint32_t node = cpu->r[0];
    uint32_t vp = 0, gui = 0;
    forge_api->mem_read(node, &vp, 4);
    if (vp == g_root_vptr) {
        t_draw.clear(); // a layout starts (also drops anything a missed exit left)
        ++g_dg_layouts;
        uint32_t child = 0;
        if (forge_api->mem_read(node + 0x60, &child, 4) && child) {
            forge_api->mem_read(child + 0x70, &gui, 4);
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

thread_local uint32_t t_last_slot{0}; // the slot squeezed last (never squeeze one twice)

/// The squeeze for what is being drawn now: the layout from the draw stack, or (outside a layout
/// draw) from a container's own rGUI field (+0x70). Anchors in canvas units.
bool CurrentShift(uint32_t container, LayoutShift& sh, float& ax, float& ay) {
    uint32_t gui = 0;
    if (!t_draw.empty() && t_draw.front().gui) {
        gui = t_draw.front().gui;
    } else if (!container || !forge_api->mem_read(container + 0x70, &gui, 4) || !gui) {
        ++g_dg_notop;
        return false;
    }
    {
        std::shared_lock lk{g_table_lock};
        const auto it = g_table.find(gui);
        if (it == g_table.end()) {
            ++g_dg_unplaced;
            return false;
        }
        sh = it->second;
    }
    if (sh.game) {
        return false; // squeezed game-side already
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

/// Squeezes the world-matrix slot the draw context's renderer uploaded last.
void SqueezeSlot(uint32_t ctx) {
    ++g_dg_calls;
    LayoutShift sh;
    float ax, ay;
    if (!CurrentShift(0, sh, ax, ay)) {
        return;
    }
    uint32_t renderer = 0, dst = 0;
    float m[16];
    if (!forge_api->mem_read(ctx + 0x04, &renderer, 4) ||
        !forge_api->mem_read(renderer + 0x18BC, &dst, 4) || !dst || dst == t_last_slot ||
        !forge_api->mem_read(dst, m, sizeof(m))) {
        ++g_dg_nodst;
        return;
    }
    t_last_slot = dst;
    for (int r = 0; r < 3; ++r) {
        m[r * 4] *= sh.sx;
        m[r * 4 + 1] *= sh.sy;
    }
    m[12] = ax + (m[12] - ax) * sh.sx;
    m[13] = ay + (m[13] - ay) * sh.sy;
    forge_api->mem_write(dst, m, sizeof(m));
    ++g_shifted_builds;
}

void OnWorldUploaded(ForgeCpu*, const uint32_t args[4], void*) {
    SqueezeSlot(args[0]);
}

// text (0xA3DA50, r0 = piece, r1 = draw context) copies its matrix into a new slot inline
void OnTextDrawn(ForgeCpu*, const uint32_t args[4], void*) {
    SqueezeSlot(args[1]);
}

// clip rectangles: 0xA37988 (scissor-mask container: r0 = it, matrix +0x10, int size +0xC8/+0xCC)
// and 0xA4F420 (mask object) write the box of the transformed corners as ints {x0, y0, x1, y1}
// (canvas units) to r1. Squeezed like the pieces so masks still cover what they clip.
void OnClipRect(ForgeCpu*, const uint32_t args[4], void*) {
    LayoutShift sh;
    float ax, ay;
    int32_t r[4];
    if (!args[1] || !CurrentShift(args[0], sh, ax, ay) ||
        !forge_api->mem_read(args[1], r, sizeof(r))) {
        return;
    }
    const auto sq = [](int32_t v, float a, float k) {
        return static_cast<int32_t>(std::floor(a + (static_cast<float>(v) - a) * k + 0.5f));
    };
    r[0] = sq(r[0], ax, sh.sx);
    r[2] = sq(r[2], ax, sh.sx);
    r[1] = sq(r[1], ay, sh.sy);
    r[3] = sq(r[3], ay, sh.sy);
    forge_api->mem_write(args[1], r, sizeof(r));
    ++g_rects;
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

/// Rebuilds the squeeze table from the modes (every frame; draws pick it up right away).
void ApplyShifts() {
    std::unordered_map<uint32_t, LayoutShift> table;
    const float aspect = g_aspect.load();
    const float g = 720.0f / static_cast<float>(TargetHeight()); // HUD size
    const float sx = g * (16.0f / 9.0f) / aspect;
    const bool on = g_fix_on.load() && (std::fabs(sx - 1.0f) > 0.005f || g != 1.0f);
    std::scoped_lock lk{g_gui_lock};
    if (on) {
        for (const uint32_t gui : g_guis) {
            const std::string& key = GuiKey(gui);
            if (key.empty()) {
                continue;
            }
            LayoutShift sh{0.0f, sx, g, GameSide(key)};
            if (sh.game && ModeOf(key) == Mode::Follow) {
                sh.game = false; // tags track things: game-side would drift
            }
            switch (ModeOf(key)) {
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

void LayoutsUi() {
    if (!ImGui::TreeNodeEx("Layouts on screen", 0)) {
        return;
    }
    ImGui::TextWrapped("keep = original (stretched; for full-screen pictures). left / center / "
                       "right = unstretched, pinned there. follow = unstretched, for things the "
                       "game places on the world (name tags). game-side = squeeze the game's own "
                       "positions so clip boxes and 3D views follow (not for tags). Saved to "
                       "hudfix_layouts.ini.");
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
            if (ImGui::Combo("##m", &m, kModeNames, 5)) {
                std::scoped_lock lk{g_mode_lock};
                g_modes[key] = static_cast<Mode>(m);
                changed = true;
            }
            ImGui::TableNextColumn();
            bool game = GameSide(key);
            if (ImGui::Checkbox("##g", &game)) {
                std::scoped_lock lk{g_mode_lock};
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
        SaveLayouts();
    }
    ImGui::TreePop();
}

} // namespace

extern "C" {

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info) {
    info->required_ver.major = FORGE_PC_VERSION_MAJOR;
    info->required_ver.minor = FORGE_PC_VERSION_MINOR;
    std::strncpy(info->name, "HUD Fix", sizeof(info->name) - 1);
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
    const std::string dir = g_ini_path.substr(0, g_ini_path.find_last_of("\\/") + 1);
    g_ini_path = dir + "hudfix.ini";
    g_layouts_path = dir + "hudfix_layouts.ini";
    LoadLayouts();
    Load();
    UpdateAspect();
    const uint32_t main = forge_api->mem_getModuleBase("main");
    g_vptr = main + kGuiVptr;
    bool ok = forge_api->hook_create(main + kGuiFactory, nullptr, OnGuiCreated, nullptr) &&
              forge_api->hook_create(main + kGuiDtor1, OnGuiDestroyed, nullptr, nullptr) &&
              forge_api->hook_create(main + kGuiDtor2, OnGuiDestroyed, nullptr, nullptr);
    if (!ok) {
        Logf(FORGE_LOG_ERROR, "layout hooks failed: the fix is off");
        g_fix_on = false;
    }
    g_inst_vptr = main + kInstNullVptr;
    if (!forge_api->hook_create(main + kInstNullNew, nullptr, OnInstNullMade, nullptr) ||
        !forge_api->hook_create(main + kInstNullCtor, nullptr, OnInstNullBuilt, nullptr) ||
        !forge_api->hook_create(main + kObjCtor, nullptr, OnInstNullBuilt, nullptr) ||
        !forge_api->hook_create(main + kObjCreate[0], nullptr, OnInstNullMade, nullptr) ||
        !forge_api->hook_create(main + kObjCreate[1], nullptr, OnInstNullMade, nullptr) ||
        !forge_api->hook_create(main + kObjCreate[2], nullptr, OnInstNullMade, nullptr)) {
        Logf(FORGE_LOG_WARN, "container hook failed: the layout explorer stays empty");
    }
    g_root_vptr = main + kInstRootVptr;
    if (!forge_api->hook_create(main + kDrawChildren, OnDrawChildren, OnDrawChildrenDone, nullptr)) {
        Logf(FORGE_LOG_ERROR, "draw hook %#x failed: layouts won't be placed", kDrawChildren);
    }
    if (!forge_api->hook_create(main + kTextDraw, nullptr, OnTextDrawn, nullptr)) {
        Logf(FORGE_LOG_ERROR, "text hook %#x failed: text won't be placed", kTextDraw);
    }
    for (const uint32_t fn : kMatrixBuilders) {
        if (!forge_api->hook_create(main + fn, nullptr, OnMatrixBuilt, nullptr)) {
            Logf(FORGE_LOG_ERROR, "matrix hook %#x failed: game-side layouts won't be placed", fn);
        }
    }
    for (const uint32_t fn : kClipRects) {
        if (!forge_api->hook_create(main + fn, nullptr, OnClipRect, nullptr)) {
            Logf(FORGE_LOG_ERROR, "clip hook %#x failed: masks won't be placed", fn);
        }
    }
    for (const uint32_t fn : kWorldUploads) {
        if (!forge_api->hook_create(main + fn, nullptr, OnWorldUploaded, nullptr)) {
            Logf(FORGE_LOG_ERROR, "draw hook %#x failed: layouts won't be placed", fn);
        }
    }
    g_sweep_started = true;
    std::thread(Sweep).detach();
    Logf(FORGE_LOG_INFO, "ready (game aspect %.4f, %zu saved holds); scan from the Forge menu",
         g_aspect.load(), g_saved_holds.size());
}

FORGE_EXPORT void forge_onUpdate(float) {
    if (++g_frames % 60 == 0) {
        UpdateAspect();
    }
    if (g_frames % 600 == 0) {
        LogDrawDiag();
    }
    ApplyFix();
    ApplyInstOffsets();
    ApplyShifts();
    std::scoped_lock lk{g_lock};
    for (const auto& h : g_hits) {
        if (h.hold || h.test) {
            Apply(h);
        }
    }
}

/// Exit and hot reload: the background threads (startup sweep, scanner) must be done before the
/// dll goes away; canvases nudged for a re-layout go back to 1280x720.
FORGE_EXPORT void forge_onUnload(void) {
    while (g_scanning.load() || (g_sweep_started.load() && !g_sweep_done.load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // past their last instructions
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
    bool fix = g_fix_on.load();
    if (ImGui::Checkbox("HUD Fix: keep HUD and menus in proportion", &fix)) {
        g_fix_on = fix;
    }
    {
        int h = g_hud_height.load();
        const int mode = h == 720 ? 0 : h < 0 ? 1 : 2;
        int pick = mode;
        ImGui::RadioButton("Normal size", &pick, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Native (your resolution)", &pick, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Custom", &pick, 2);
        if (pick != mode) {
            h = pick == 0 ? 720 : pick == 1 ? -1 : static_cast<int>(TargetHeight());
        }
        if (pick == 2) {
            ImGui::SliderInt("HUD canvas height (bigger = smaller HUD)", &h, 360, 2160);
        }
        if (h != g_hud_height.load()) {
            g_hud_height = h;
            std::scoped_lock lk{g_lock};
            Save();
        }
        ImGui::Text("HUD canvas now %ux%u (game %ux%u)",
                    static_cast<uint32_t>(std::lround(TargetHeight() * g_aspect.load() / 2.0f)) * 2,
                    TargetHeight(), static_cast<uint32_t>(std::lround(g_render_h.load() * g_aspect.load())),
                    g_render_h.load());
    }
    {
        std::scoped_lock lk{g_gui_lock};
        ImGui::Text("%zu layouts tracked%s", g_guis.size(), g_sweep_done ? "" : "; sweeping...");
    }
    {
        std::shared_lock lk{g_table_lock};
        ImGui::Text("%zu layouts placed; draws squeezed %u, layout not found %u", g_table.size(),
                    g_shifted_builds.load(), g_inherited_builds.load());
    }
    {
        int place = g_place.load();
        bool pick = ImGui::RadioButton("Anchor to edges (per layout)", &place, 0);
        ImGui::SameLine();
        pick |= ImGui::RadioButton("Constrain UI to 16:9", &place, 1);
        if (pick) {
            g_place = place;
            std::scoped_lock lk{g_lock};
            Save();
        }
    }
    LayoutsUi();
    ExplorerUi();
    if (!ImGui::TreeNodeEx("Scanner (tool)", 0)) {
        return;
    }
    ImGui::Text("Game aspect %.3f (widen factor %.3f)", g_aspect.load(), WidenFactor());
    if (g_scanning) {
        ImGui::Text("Scanning... %.0f%%", g_scan_progress.load() / 10485.76);
    } else {
        if (ImGui::Button("Scan memory")) {
            StartScan();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(g_status.c_str());
    }
    std::scoped_lock lk{g_lock};
    bool changed = false;
    if (!g_hits.empty() && ImGui::Button("Inspect (held hits, or all if none held) to forge.log")) {
        bool any = false;
        for (const auto& h : g_hits) {
            any |= h.hold;
        }
        for (const auto& h : g_hits) {
            if (h.hold || !any) {
                Inspect(h);
            }
        }
    }
    if (!g_hits.empty() && ImGui::CollapsingHeader("Bisect (find the HUD's value fast)",
                                                   ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* preview = g_bisect_pattern < 0 ? "all patterns"
                                                   : kPatterns[g_bisect_pattern].name;
        if (ImGui::BeginCombo("patterns", preview)) {
            if (ImGui::Selectable("all patterns", g_bisect_pattern < 0)) {
                g_bisect_pattern = -1;
            }
            for (int k = 0; k < static_cast<int>(std::size(kPatterns)); ++k) {
                if (ImGui::Selectable(kPatterns[k].name, g_bisect_pattern == k)) {
                    g_bisect_pattern = k;
                }
            }
            ImGui::EndCombo();
        }
        if (g_cand.empty()) {
            if (ImGui::Button("Start")) {
                BisectStart();
            }
        } else {
            ImGui::Text("%zu candidates left; %zu of them widened now. Did the HUD change?",
                        g_cand.size(), (g_cand.size() + 1) / 2);
            if (ImGui::Button("HUD changed")) {
                BisectAnswer(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("No change")) {
                BisectAnswer(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                g_cand.clear();
                BisectShow();
                g_bisect_msg = "stopped";
            }
        }
        if (!g_bisect_msg.empty()) {
            ImGui::TextUnformatted(g_bisect_msg.c_str());
        }
    }
    ImGui::TextWrapped("Or tick 'widen' on a single hit and watch the HUD. Untick to restore.");
    if (!g_hits.empty() && ImGui::BeginTable("hits", 4,
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_ScrollY,
                                             ImVec2(0, 400))) {
        ImGui::TableSetupColumn("widen");
        ImGui::TableSetupColumn("address");
        ImGui::TableSetupColumn("pattern");
        ImGui::TableSetupColumn("now");
        ImGui::TableHeadersRow();
        for (auto& h : g_hits) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(&h);
            if (ImGui::Checkbox("##w", &h.hold)) {
                if (!h.hold) {
                    Restore(h);
                }
                changed = true;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%08X", h.addr);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kPatterns[h.pattern].name);
            ImGui::TableNextColumn();
            uint32_t now[2]{};
            forge_api->mem_read(h.addr, &now[0], 4);
            forge_api->mem_read(h.addr + kPatterns[h.pattern].b_off, &now[1], 4);
            if (kPatterns[h.pattern].kind == Kind::SizeI) {
                ImGui::Text("%u, %u", now[0], now[1]);
            } else {
                ImGui::Text("%g, %g", Float(now[0]), Float(now[1]));
            }
        }
        ImGui::EndTable();
    }
    if (changed) {
        Save();
    }
    ImGui::TreePop();
}

} // extern "C"
