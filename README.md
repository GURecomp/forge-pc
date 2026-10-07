# Forge PC

A drag-and-drop mod loader for the recompiled PC build of Monster Hunter Generations Ultimate
(1.4.0), in the spirit of Stracker's Loader for MH World. A port of
[MHGU Forge](https://github.com/Fexty12573/forge) by Fexty, with the author's permission.

Forge PC is not part of the game exe. The exe only carries a small, permanent mod host
interface (`include/mod_host_api.h`); everything here is installed and updated by copying files.

## Install

Copy the `mods` folder from the release into the game folder (next to the game exe):

```
<game>/mods/0100770008DD8000/Forge/forge.dll        the loader (Linux: forge.so)
<game>/mods/0100770008DD8000/Forge/forge.ini        its settings
<game>/mods/0100770008DD8000/Forge/fonts/...
```

On Linux, also set the loader in the game's `game_settings.ini` (next to the game binary):

```
[Mods]
loader = Forge/forge.so
```

Start the game. `Forge/forge.log` shows what loaded. Press **Insert** for the Forge menu.
Uninstall: delete the `Forge` folder. Without it the game runs exactly as before.

## Mods

One folder per mod, next to `Forge`:

```
<game>/mods/0100770008DD8000/<Mod name>/plugins/        code (Forge PC plugins): *.dll on
                                                        Windows, *.so on Linux
<game>/mods/0100770008DD8000/<Mod name>/romfs/...       game files the mod replaces
```

File mods (`romfs`) and settings files work on both Windows and Linux as they are. Code plugins
are built once per system from the same source; a mod can ship its `.dll` and `.so` side by side
in `plugins/`, and each system loads only its own.

Plugins can be updated while the game runs: Forge loads a copy of each plugin dll
(`<name>.dll.<n>.live`), so the dll itself can be replaced, and a changed dll is reloaded in game
(also the **Reload** button in the menu). Turn it off with `[plugins] hot_reload = false` in
`Forge/forge.ini`.

## Example mod: HUD Fix (working)

`mods/HudFix` is a complete mod: it keeps the HUD and menus in proportion on screens wider than
16:9 (21:9, 32:9, ...), where the game stretches them.

Install: copy `dist/mods/0100770008DD8000/HudFix` (or the release zip's `HudFix` folder) next to
`Forge`:

```
<game>/mods/0100770008DD8000/HudFix/plugins/hudfix.dll          (Linux: hudfix.so)
<game>/mods/0100770008DD8000/HudFix/plugins/hudfix_layouts.ini   per-layout placement
```

In game, Insert opens the Forge menu, then **HUD Fix**:

- **Anchor to edges** (each element to its own side of the screen) or **Constrain UI to 16:9**
  (everything in a centred 16:9 area, like newer games).
- **Layouts on screen**: every loaded layout with its mode: `keep` (unchanged, for full-screen
  pictures), `left` / `center` / `right`, `follow`, and a **game-side** box for layouts whose
  scrolling icons get cut off otherwise. Saved to `hudfix_layouts.ini`; the shipped file covers
  the HUD, menus, maps and town screens.
- HUD size: normal, native (your resolution) or custom.

How it works: the plugin hooks the game's GUI drawing and squeezes each layout horizontally
around its anchor (16:9 / your aspect) when it is drawn, without changing any position the game
computes. See the comment at the top of `mods/HudFix/hudfix.cpp` for the details and addresses
(game version 1.4.0).

## Writing a plugin

Plugins are x64 libraries (Windows DLLs, Linux shared objects) built against
`include/forge_pc.h`. See `examples/hello/hello.c`.

```c
#include "forge_pc.h"
FORGE_PLUGIN_DEFINE_API

static ForgeHookAction my_pre(ForgeCpu* cpu, void* user) {
    return FORGE_HOOK_CONTINUE; /* or set cpu->r[0] and return FORGE_HOOK_RETURN to skip */
}

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info) {
    info->required_ver.major = FORGE_PC_VERSION_MAJOR;
    info->required_ver.minor = FORGE_PC_VERSION_MINOR;
    strcpy(info->name, "My mod");
}

FORGE_EXPORT void forge_onInit(void) {
    forge_api->hook_create(forge_api->mem_getModuleBase("main") + 0x1C358, my_pre, NULL, NULL);
}
```

Differences from Forge on the Switch: game addresses are 32-bit guest addresses (read and write
them through `forge_api->mem_*`); hooks are pre/post callbacks run by the recompiler's
dispatcher instead of ARM trampolines; byte patches of code have no effect (the code is
recompiled), so use hooks. ImGui: export `forge_onImGuiInit/Render/FreeRender` and build with
the same Dear ImGui version (`forge_api->imgui_version`).

## Building

- Windows: `build.bat` (Visual Studio 2022, Vulkan SDK headers). Output:
  `dist\mods\0100770008DD8000\`.
- Linux: `./build.sh` (g++ with C++20, Vulkan headers from `libvulkan-dev` or `VULKAN_SDK`).
  Output: `dist-linux/mods/0100770008DD8000/`. The libraries depend only on glibc (2.34+).
- Linux plugins: build with `-shared -fPIC -fvisibility=hidden` and export only the `forge_*`
  entry points (see the version script in `build.sh`), so each plugin keeps its own Dear ImGui.

## Credits and licenses

Forge PC is MIT licensed (`LICENSE`). It builds on:

- **[MHGU Forge](https://github.com/Fexty12573/forge)** by **Fexty**, with contributions by
  **jeffi2287**: the original Switch mod loader whose design, plugin API, menu and style this
  is a port of, done with the author's permission. MIT; its notice (Copyright (c) 2020 The
  Skyline Project, Copyright (c) 2026 Jeffi) is reproduced in `LICENSE-MHGU-Forge.txt`.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut: the menu UI, compiled into
  `forge.dll` and `hudfix.dll`. MIT, `third_party/imgui/LICENSE.txt`.
- **Roboto** by Christian Robertson / Google (`data/Roboto-Medium.ttf`), the menu font as
  shipped with Dear ImGui and MHGU Forge. Apache License 2.0, `data/LICENSE-Roboto.txt`.
- **[MHGU-Modding](https://github.com/RTHKKona/MHGU-Modding)** by Handburger: file format
  documentation and templates that HUD Fix's understanding of `.gui` layouts is based on
  (reference only, no code included).

Details: `THIRD_PARTY_NOTICES.md`. Release builds carry these files in `Forge/licenses/` (and
`HudFix/LICENSE*.txt`).
