// Forge PC: shared state between the loader (loader.cpp) and the menu overlay (overlay.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "forge_pc.h"
#include "mod_host_api.h"

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

void Log(ForgeLogLevel level, const char* format, ...);
const std::string& LoaderDir();

/// Plugins' ImGui entry points (present thread only).
struct PluginUi {
    std::string name;
    void (*on_imgui_init)(void* ctx, void* alloc, void* free, void* user);
    void (*on_imgui_render)();
    void (*on_imgui_free_render)();
    uint32_t generation; // new for every load (hot reload): ImGui init runs once per generation
};
/// Calls fn for every loaded plugin (under the plugin lock: don't load plugins inside).
void ForEachPluginUi(const std::function<void(size_t index, const PluginUi&)>& fn);
size_t PluginCount();
/// Hot reload: reload plugin `index` at the next game frame (menu button); file watcher on?
void RequestPluginReload(size_t index);
bool HotReloadOn();

// overlay.cpp
bool OnPresent(const ModHostPresent* p);
bool OnInput(const ModHostInput* e);
bool MenuIsOpen();
void OverlayConfigure(); // after forge.ini is read
void OverlayShutdown();
extern const char* const kImGuiVersion;

} // namespace forge
