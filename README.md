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

## Example mod: UI Fix (working)

`mods/UIFix` is a complete mod, and a larger example of a plugin with its own Dear ImGui: it
undoes the stretching of the HUD and menus on screens wider than 16:9 (21:9, 32:9, ...), and lets
you scale and move single parts of the UI (health bar, map, item bar, menus...) by dragging them
on screen. See `mods/UIFix/README.md`.

Install: copy `dist/mods/0100770008DD8000/UIFix` (or the release zip's `UIFix` folder) next to
`Forge`:

```
<game>/mods/0100770008DD8000/UIFix/plugins/uifix.dll          (Linux: uifix.so)
<game>/mods/0100770008DD8000/UIFix/plugins/presets/*.ini      the defaults
```

In game, Insert opens the Forge menu, then **Plugin UI > UI Fix**.

How it works: the plugin hooks the game's GUI drawing and squeezes each layout horizontally
around its anchor when it is drawn, without changing any position the game computes. See the
comment at the top of `mods/UIFix/uifix.cpp` for the details and addresses (game version 1.4.0).

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

### Replacing a function (0.4)

`hook_override` runs your function instead of the game's; `call_original` runs the game's
function (or the next override down) from inside it:

```c
static void my_override(ForgeCpu* cpu, void* user) {
    uint32_t args[2] = {cpu->r[0], cpu->r[1] + 1};          /* change an argument */
    ForgeCallResult res;
    if (forge_api->call_original(forge_api->mem_getModuleBase("main") + 0x1C358,
                                 args, 2, NULL, 0, &res))
        cpu->r[0] = res.r0;                                  /* return value of the override */
}
/* in forge_onInit: */
forge_api->hook_override(forge_api->mem_getModuleBase("main") + 0x1C358, my_override, NULL);
```

Order of a call: pre hooks (the first to return `FORGE_HOOK_RETURN` ends it) -> override ->
post hooks, which run once, after the override returns. With several overrides on one function
the one loaded last is the outermost (a plugin that is hot reloaded or switched on again counts
as loaded last); its `call_original` reaches the next one down. Overrides go away with their
plugin. Calls made with `call` never fire hooks or overrides. Down a chain of several
overrides only the register arguments (r0-r3, s0-s15) are passed on.

## Settings files for plugins (0.5)

`config_open("mymod.ini")` opens `<mods dir>/<your mod folder>/mymod.ini` (relative names only).
The typed getters (`config_int`, `config_float`, `config_bool`, `config_string`) take a default
and remember it: `config_save` writes the keys the file doesn't have yet, so a first start
creates a complete file, and keeps comments and everything else in it. `config_set` +
`config_save` store a value. `config_changed` is true once after the file changed on disk (a
hand edit) and has re-read it: poll it in `forge_onUpdate` for live settings. Handles are freed
with the plugin. Rust: `forge_pc::Config`.

## Drawing with your own Vulkan commands (0.6)

Export `forge_onPresent(const ForgePresent* p)`: it runs once per presented frame on the
presentation thread, after the finished frame is in the swapchain image and before Forge's own
menu. `p` has the Vulkan instance, device, queue, the command buffer being recorded, the image,
format and size, and `lock_queue` / `unlock_queue` for submits of your own (the queue is shared
with the emulator). The image is in `TRANSFER_DST_OPTIMAL` on entry: transition it yourself to
draw and put it back before returning. You only see the final composed image (no game passes,
depth or shaders). Resources you create are yours to free (`forge_onUnload`, swapchain
recreation). Rust: set `WANTS_PRESENT = true` and implement `Plugin::present`; ask for minor 6.

## Menu: Reset scripts

**Loaded Plugins -> Reset scripts** looks for plugin files again and loads every plugin of an
enabled mod that is not loaded: a mod folder copied in while the game runs, plugins that were
off, plugins whose hot reload failed. Plugins already loaded are left alone (the per-plugin
**Reload** button restarts one) and mods switched off in the **Mods** list stay off. No restart
is needed to add a mod.

## Menu: mods on and off

The **Mods** list in the Forge menu (Insert) has a checkbox per mod folder. Switching a mod off
drops its hooks, waits until none of its callbacks is running, calls `forge_onUnload` and
unloads its plugins, the same path as a hot reload; switching it on loads them again. The state
is saved in `forge.ini` (`[mods]`, `<Mod folder> = off`), disabled mods stay listed. Game files
(`romfs/`) are applied by the game when it starts: Forge does not switch them (the menu says
"restart required").

## Tracing calls (forge.ini)

To see which game functions run when you do something in game, without writing a plugin, list
candidates in `forge.ini` (offsets in `main`):

```ini
[trace]
enabled = true
0x1C358 = limit=50
0x2A410 = limit=10, args=true
0x3B000 = limit=20, ret=true
```

`trace.log` (next to `forge.log`) gets one line per call: time, game update number, function,
call count, thread, caller (LR) and with `args=true` r0-r3 (`ret=true` adds a line with the
return value). `limit=` is the number of lines per function (it keeps counting, a line says when
the limit is hit; 0 = no limit, default 100). The trace hooks run before any plugin's, so they
also see calls a plugin skips. Edit `forge.ini` while the game runs: added, changed and removed
lines apply within a second or two.

## When a plugin crashes

If the game faults inside a plugin, or inside the game code one of a plugin's callbacks was
running, `forge.log` has a `[crash]` line with the plugin's name, its dll, the numbered
`.live` copy it was loaded as (`<name>.dll.<n>.live`, also logged when each plugin loads), the
hook address and the fault address. With `[crash] auto_disable = true` in `forge.ini` the mod
that was involved is switched off at the next launch if the game did not exit cleanly after
the report (switch it on again in the menu).

## Building

- Windows: `build.bat` (Visual Studio 2022, Vulkan SDK headers). Output:
  `dist\mods\0100770008DD8000\`.
- Linux: `./build.sh` (g++ with C++20, Vulkan headers from `libvulkan-dev` or `VULKAN_SDK`).
  Output: `dist-linux/mods/0100770008DD8000/`. The libraries depend only on glibc (2.34+).
- Linux plugins: build with `-shared -fPIC -fvisibility=hidden` and export only the `forge_*`
  entry points (see the version script in `build.sh`), so each plugin keeps its own Dear ImGui.

## Continuing elsewhere, checking for blocks, calling any code (0.7)

- **`FORGE_HOOK_JUMP`**: a pre hook sets `cpu->r[15]` to an address and returns it; the game
  continues there with every register as the hook left it (`FORGE_HOOK_RETURN` goes to `lr`
  instead). Post hooks of the jumped-over function don't run, hooks on the target do. Jumping to
  the hooked address itself would loop forever, so Forge logs a warning and treats it as
  `FORGE_HOOK_CONTINUE`. Rust: `Action::Jump`.
- **`is_block(addr)`**: true if `addr` starts a recompiled block, i.e. a hook can be put there.
  Logs nothing, unlike a failed `hook_create`. Rust: `forge_pc::is_block` (`None` on an older
  loader).
- **`call` into code that has no recompiled block** (Thumb, a module your plugin loaded, a
  region the recompiler did not reach) runs on the emulator's JIT until the code is back on a
  block or returns. This needs an exe built with that change; on an older exe such a call fails
  as before. Check with `is_block` if you need to know which path a call takes.
- A hook (or `hook_override`) with neither callback set fails with "no callback" before the
  block check, so that message does not mean the address is bad.
- Guest code a plugin loads itself is not reloaded when the plugin is hot reloaded: unload what
  you loaded in `forge_onUnload` and load it again in `forge_onInit`.

Ask for minor 7 (`required_ver.minor = 7`, Rust `min_minor = 7`) if you use any of this.

## Credits and licenses

Forge PC is MIT licensed (`LICENSE`). It builds on:

- **[MHGU Forge](https://github.com/Fexty12573/forge)** by **Fexty**, with contributions by
  **jeffi2287**: the original Switch mod loader whose design, plugin API, menu and style this
  is a port of, done with the author's permission. MIT; its notice (Copyright (c) 2020 The
  Skyline Project, Copyright (c) 2026 Jeffi) is reproduced in `LICENSE-MHGU-Forge.txt`.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut: the menu UI, compiled into
  `forge.dll` and `uifix.dll`. MIT, `third_party/imgui/LICENSE.txt`.
- **Roboto** by Christian Robertson / Google (`data/Roboto-Medium.ttf`), the menu font as
  shipped with Dear ImGui and MHGU Forge. Apache License 2.0, `data/LICENSE-Roboto.txt`.
- **[MHGU-Modding](https://github.com/RTHKKona/MHGU-Modding)** by Handburger: file format
  documentation and templates that UI Fix's understanding of `.gui` layouts is based on
  (reference only, no code included).

Details: `THIRD_PARTY_NOTICES.md`. Release builds carry these files in `Forge/licenses/` (and
`UIFix/LICENSE*.txt`).

## AI disclosure

Forge PC and UI Fix were written with the help of an AI coding assistant, directed, built and
tested in game by the maintainer. What the AI did, what it did not, and how it is checked:
[AI_DISCLOSURE.md](AI_DISCLOSURE.md) (also in the release zip as `Forge/licenses/`).
