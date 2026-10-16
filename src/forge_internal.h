// Forge PC: shared state between the loader (loader.cpp), the menu overlay (overlay.cpp), the
// tracer (trace.cpp) and crash attribution (crash.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "forge_pc.h"
#include "mod_host_api.h"

/// A loaded plugin's lifetime guard: callbacks into it count themselves in `inflight`; once
/// `dead` is set (hot reload) no new callback starts, and the DLL is freed when the count is
/// back to 0. Never freed itself (hooks and pending frames may still point at it).
struct ForgePluginState {
    std::atomic<int> inflight{0};
    std::atomic<bool> dead{false};
    // for logs and crash attribution (set once after the plugin is loaded, before its hooks run)
    std::string mod;     // mod folder name
    std::string file;    // "<mod>/<name>.dll"
    std::string live;    // the loaded copy's file name: "<name>.dll.<n>.live"
    std::string name;    // name the plugin gave itself (forge_onLoad)
};

/// The C API's opaque hook handle.
struct ForgeHook {
    uint32_t target;
    ForgeHookPre pre;
    ForgeHookPost post;
    void* user;
    std::atomic<bool> enabled{true};
    ForgePluginState* owner{}; // the plugin that made it (nullptr = Forge itself)
    ForgeOverride override_fn{}; // set: this entry is an override (pre/post are NULL)
};

namespace forge {

/// UTF-8 std::string <-> path (C++20 paths speak char8_t).
inline std::filesystem::path PathU8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
inline std::string Utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

extern const ModHostApi* g_host;

/// forge.ini beside forge.dll ([log] level, [menu] key / font_size); "" if absent.
std::string ConfigValue(const char* section, const char* key);
/// Sets a value in forge.ini (the rest of the file, comments included, stays as it is).
bool ConfigSet(const char* section, const char* key, const char* value);
/// A whole ini file: section -> key -> value (same syntax as forge.ini).
using IniMap = std::map<std::string, std::map<std::string, std::string>>;
IniMap ReadIni(const std::filesystem::path& path);
/// Sets one value in any ini file in place (comments and other lines stay); adds the section.
bool IniSet(const std::filesystem::path& path, const char* section, const char* key,
            const char* value);
const std::string& ModsDirPath();

void Log(ForgeLogLevel level, const char* format, ...);
/// Error line from a fault handler (forge.log + the exe log; skips forge.log if it is locked).
void LogFault(const char* line);
const std::string& LoaderDir();

// --- guest helpers (loader.cpp) ---
uint32_t MainBase();
/// "main+0x1C358" for an address inside a game module, else "0x...".
std::string FormatGuestAddr(uint32_t addr);
/// Hook entry points for Forge's own hooks (owner = Forge). `front`: runs before every other
/// hook on that address (the tracer sees calls a plugin skips). Hooks are never freed; remove =
/// HookRemove (drops it from the table and disables it).
ForgeHook* HookAdd(uint32_t target, ForgeHookPre pre, ForgeHookPost post, void* user, bool front);
void HookRemove(ForgeHook* hook);
bool IsCodeBlock(uint32_t guest_addr);
/// Frame counter (game updates since start) and the guest thread key of the running callback.
uint32_t FrameCount();
uint32_t CurrentThreadKey();

/// Plugins' ImGui entry points (present thread only).
struct PluginUi {
    std::string name;
    void (*on_imgui_init)(void* ctx, void* alloc, void* free, void* user);
    void (*on_imgui_render)();
    void (*on_imgui_free_render)();
    void (*on_present)(const ForgePresent* p); // 0.6, raw Vulkan
    uint32_t generation; // new for every load (hot reload): ImGui init runs once per generation
    ForgePluginState* state{}; // for CallbackScope
};
/// Calls fn for every loaded plugin (under the plugin lock: don't load plugins inside).
void ForEachPluginUi(const std::function<void(size_t index, const PluginUi&)>& fn);
size_t PluginCount();
/// Hot reload: reload plugin `index` at the next game frame (menu button); file watcher on?
void RequestPluginReload(size_t index);
/// "Reset scripts": at the next game frame, look for plugin files again and load every plugin of
/// an enabled mod that is not loaded (new files, plugins that were off or whose reload failed).
void RequestResetScripts();
std::string ResetScriptsMessage(); // result of the last one, "" before the first
bool HotReloadOn();

/// One mod folder (<mods dir>/<name>) as the menu shows it.
struct ModInfo {
    std::string name;
    size_t plugins{};      // plugin files in plugins/
    size_t loaded{};       // of them loaded right now
    bool enabled{true};    // forge.ini [mods]; a mod is on unless it says off
    bool has_romfs{};      // has game files (romfs/): Forge can't switch those
    bool busy{};           // a switch is queued / unloading
    std::string note;      // e.g. "crashed last session"
};
std::vector<ModInfo> ListMods();
/// Turns a mod on / off: saved in forge.ini at once, applied between two game frames.
void SetModEnabled(const std::string& name, bool enabled);
/// A line the menu shows next to the mod (e.g. it crashed last session); "" clears it.
void SetModNote(const std::string& mod, const std::string& note);

// --- crash attribution (crash.cpp) ---
/// What the current thread is running on behalf of a plugin (innermost), for fault reports.
struct CallbackCtx {
    ForgePluginState* plugin{};
    uint32_t target{}; // hooked guest address (0 = not a hook)
    const char* what{}; // "pre" "post" "override" "init" "update" "unload" "imgui"
};
extern thread_local CallbackCtx t_callback;
struct CallbackScope {
    CallbackCtx saved;
    CallbackScope(ForgePluginState* plugin, uint32_t target, const char* what) : saved(t_callback) {
        t_callback = {plugin, target, what};
    }
    ~CallbackScope() { t_callback = saved; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
};
void CrashInit();                                          // install the fault handlers
void CrashPluginLoaded(const void* base, ForgePluginState* state);
void CrashPluginUnloaded(const void* base);
void CrashCheckLastSession();                              // log it, optional auto-disable
void CrashClean();                                         // clean shutdown: drop the marker

// --- plugin config files (config.cpp) ---
ForgeConfig* ConfigOpen(ForgePluginState* owner, const char* name);
int32_t ConfigGetInt(ForgeConfig* c, const char* section, const char* key, int32_t def);
float ConfigGetFloat(ForgeConfig* c, const char* section, const char* key, float def);
bool ConfigGetBool(ForgeConfig* c, const char* section, const char* key, bool def);
const char* ConfigGetString(ForgeConfig* c, const char* section, const char* key, const char* def);
bool ConfigSetValue(ForgeConfig* c, const char* section, const char* key, const char* value);
bool ConfigSave(ForgeConfig* c);
bool ConfigPoll(ForgeConfig* c);
void ConfigDropOwner(ForgePluginState* owner); // when a plugin is unloaded

// --- tracer (trace.cpp) ---
void TraceStart();      // game start: read [trace], register the hooks, start the log writer
void TraceTick();       // every game update (game thread): applies a changed forge.ini
void TraceShutdown();

// overlay.cpp
bool OnPresent(const ModHostPresent* p);
bool OnInput(const ModHostInput* e);
bool MenuIsOpen();
bool ScreenSize(uint32_t* width, uint32_t* height); // last presented swapchain size
void OverlayConfigure(); // after forge.ini is read
void OverlayShutdown();
extern const char* const kImGuiVersion;

} // namespace forge
