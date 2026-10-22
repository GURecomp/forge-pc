/*
 * forge_pc.h: plugin API of Forge PC, the plugin loader of the recompiled MHGU (PC exports).
 *
 * SPDX-License-Identifier: MIT
 *
 * A port of MHGU Forge (https://github.com/Fexty12573/forge, MIT, by Fexty) to the statically
 * recompiled game. On the Switch, Forge is an ARM module that patches ARM instructions in the
 * game; in a recompiled export the game's code is native x64 compiled from those instructions,
 * so patching them does nothing. Forge PC hooks through the recompiler's dispatcher instead, and
 * plugins are native x64 libraries: DLLs on Windows, shared objects (.so) on Linux. Forge PC
 * itself is a separate install (forge.dll / forge.so), not part of the game exe: it lives in
 * the export's mod folder like any other mod:
 *
 *   <game>/mods/0100770008DD8000/Forge/forge.dll            the loader (Linux: forge.so)
 *   <game>/mods/0100770008DD8000/<Mod name>/plugins/        code of a mod: *.dll (Linux: *.so)
 *   <game>/mods/0100770008DD8000/<Mod name>/romfs/...       game files the mod replaces
 *
 * The game itself still is a 32-bit ARM program: every game address (code or data) is a u32
 * guest address, not a pointer of the plugin. Read and write game memory through forge_mem_*;
 * forge_mem_ptr gives a real pointer to a range that is contiguous on the host.
 *
 * Plugin entry points (export them with FORGE_EXPORT; all but forge_onLoad are optional):
 *   void forge_onLoad(ForgePluginInfo* info)   fill in name + required API version
 *   void forge_onInit(void)                    after the game's systems exist (sApp::run)
 *   void forge_onUpdate(float dt)              once per game update, dt = frame time (s)
 *   void forge_onUnload(void)                  at exit, and before a hot reload (see below)
 *   void forge_onImGuiInit(void* ctx, void* alloc, void* free, void* user)
 *                                              ImGuiContext + allocator: call
 *                                              ImGui::SetAllocatorFunctions(alloc, free, user) and
 *                                              ImGui::SetCurrentContext(ctx). Build the plugin
 *                                              with the same Dear ImGui version (api->imgui_version)
 *   void forge_onImGuiRender(void)             inside Forge's menu window ("Plugin UI")
 *   void forge_onImGuiFreeRender(void)         every frame, for windows/overlays of your own
 *   void forge_onPresent(const ForgePresent* p) (0.6) every presented frame, raw Vulkan: record
 *                                              your own draw commands (see ForgePresent)
 * ImGui and present callbacks run on the presentation thread (not a game thread: api->call
 * fails there).
 * One translation unit of the plugin must contain FORGE_PLUGIN_DEFINE_API (it exports the
 * function through which the loader hands over the API table).
 *
 * Threads: callbacks run on the emulated CPU cores' host threads, concurrently for hooks that
 * different game threads reach. forge_onInit and forge_onUpdate run on the game's main thread.
 *
 * Hot reload (0.2): Forge loads a copy of the plugin (<name>.dll.<n>.live, same folder), so the
 * dll can be rebuilt while the game runs; a changed dll (or the menu's Reload button) is
 * reloaded between two game frames. Forge drops the plugin's hooks and waits until none of
 * its callbacks is running, then calls forge_onUnload (game thread), unloads it, loads the new
 * build and calls forge_onLoad + forge_onInit (game thread, the game already running) and
 * forge_onImGuiInit again. In forge_onUnload: stop and join threads you started, free
 * mem_alloc blocks, and put back game memory you changed that the game won't rewrite itself.
 * Hooks need nothing. Plugins that can't do this: set [plugins] hot_reload = false in
 * forge.ini.
 */
#ifndef FORGE_PC_H
#define FORGE_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_PC_VERSION_MAJOR 0
#define FORGE_PC_VERSION_MINOR 7
#define FORGE_PC_VERSION_PATCH 0

#ifdef _WIN32
#define FORGE_EXPORT __declspec(dllexport)
#else
#define FORGE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct ForgeVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} ForgeVersion;

typedef struct ForgePluginInfo {
    ForgeVersion required_ver; /* the loader must have the same major and at least this minor */
    char name[64];
} ForgePluginInfo;

/* One presented frame (0.6), passed to forge_onPresent on the presentation thread (always the
 * same thread). The finished frame is already blitted into the swapchain image. Vulkan handles
 * are void* / uint64_t so plugins need no Vulkan headers (load functions through
 * vk_get_instance_proc).
 *   - Record into vk_command_buffer (it is being recorded for this present; do not end or
 *     submit it). To draw, transition vk_image from TRANSFER_DST_OPTIMAL (its layout on entry)
 *     yourself and put it back to TRANSFER_DST_OPTIMAL before returning, whatever you did.
 *   - Anything that needs a submit of your own (a texture upload) goes to vk_queue between
 *     lock_queue and unlock_queue: the queue is shared with the emulator.
 *   - Resources you create are yours to destroy (forge_onUnload, or when image_count / the
 *     size changes the swapchain is recreated). Callbacks run before Forge's own menu, which
 *     is drawn on top. Only the final composed image is visible: no game render passes, depth
 *     or shaders. */
typedef struct ForgePresent {
    uint32_t size;                 /* sizeof of this struct in the loader: check before new fields */
    uint32_t vk_api_version;
    void* vk_instance;             /* VkInstance */
    void* vk_get_instance_proc;    /* PFN_vkGetInstanceProcAddr */
    void* vk_physical_device;      /* VkPhysicalDevice */
    void* vk_device;               /* VkDevice */
    void* vk_queue;                /* VkQueue */
    uint32_t vk_queue_family;
    void* vk_command_buffer;       /* VkCommandBuffer */
    uint64_t vk_image;             /* VkImage, TRANSFER_DST_OPTIMAL */
    uint32_t vk_format;            /* VkFormat of views on that image */
    uint32_t width, height;
    uint32_t image_count;
    uint32_t image_index;
    void (*lock_queue)(void);
    void (*unlock_queue)(void);
} ForgePresent;

/* The guest CPU state at a hook (same layout as the recompiler's context). r[13] = sp,
 * r[14] = lr, r[15] = pc. VFP: s[0..31] / d[0..31] overlap like on ARM (d0 = s0:s1). */
typedef struct ForgeCpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q;
    uint32_t ge;
    uint32_t thumb;
    uint32_t fpscr;
    union {
        float s[64];
        double d[32];
        uint32_t sw[64];
        uint64_t dw[32];
    } vfp;
} ForgeCpu;

/* Hook callbacks.
 * pre:  runs when the game enters the hooked function (arguments in r0-r3, then the stack).
 *       Return FORGE_HOOK_CONTINUE to run the function, or FORGE_HOOK_RETURN to skip it and
 *       return to its caller at once (set the return value first: r0 (r1) for integers and
 *       pointers, vfp.s[0] / vfp.d[0] for float / double: the game uses the hard-float ABI,
 *       float arguments arrive in s0-s15 / d0-d7), or FORGE_HOOK_JUMP (0.7) to continue at
 *       cpu->r[15] with every register as the callback left it (set r15 to the target; bit 0
 *       selects Thumb; lr is not used). Post hooks don't run after a jump; hooks on the target
 *       address do.
 * post: runs when the function returns (return value in r0/r1 or s0/d0); args[0..3] = r0-r3
 *       at entry. May change the return value. Since 0.2 (check forge_api->version) the array
 *       has two more entries: args[4] = the caller's return address (LR at entry) and
 *       args[5] = SP at entry (stack arguments 5+ are at args[5], args[5] + 4, ...). */
typedef enum ForgeHookAction {
    FORGE_HOOK_CONTINUE = 0,
    FORGE_HOOK_RETURN = 1,
    FORGE_HOOK_JUMP = 2, /* 0.7: continue at cpu->r[15], registers untouched */
} ForgeHookAction;

typedef ForgeHookAction (*ForgeHookPre)(ForgeCpu* cpu, void* user);
typedef void (*ForgeHookPost)(ForgeCpu* cpu, const uint32_t args[4], void* user);

/* Override (0.4): runs instead of the hooked function. cpu is the state at entry (arguments in
 * r0-r3 / the stack / s0-s15); leave the return value in it (r0 (r1), vfp.s[0] / vfp.d[0]). The
 * function itself runs only if the override calls forge_api->call_original. */
typedef void (*ForgeOverride)(ForgeCpu* cpu, void* user);

typedef struct ForgeHook ForgeHook;

/* A plugin's settings file (0.5), see config_open. Opaque. */
typedef struct ForgeConfig ForgeConfig;

typedef struct ForgeCallResult {
    uint32_t r0, r1; /* integer / pointer / 64-bit (r0 low) return */
    float s0;        /* float return */
    double d0;       /* double return */
} ForgeCallResult;

typedef enum ForgeLogLevel {
    FORGE_LOG_DEBUG = 0,
    FORGE_LOG_INFO = 1,
    FORGE_LOG_WARN = 2,
    FORGE_LOG_ERROR = 3,
} ForgeLogLevel;

/* The table the loader hands to the plugin. Never reorder: only append (minor version bump). */
typedef struct ForgeApi {
    ForgeVersion version;
    uint32_t size; /* sizeof(ForgeApi) of the loader */

    /* log (game log + user/forge.log) */
    void (*log)(ForgeLogLevel level, const char* plugin, const char* message);
    ForgeLogLevel (*log_getLevel)(void);

    /* modules: base address of a loaded game module ("main", "sdk", "subsdk0", "rtld");
     * 0 when unknown. Offsets in Forge's (Switch) plugins are relative to "main". */
    uint32_t (*mem_getModuleBase)(const char* name);
    uint32_t (*mem_getModuleSize)(const char* name); /* up to the next module */

    /* guest memory */
    bool (*mem_isValid)(uint32_t addr, uint32_t size);
    bool (*mem_read)(uint32_t addr, void* out, uint32_t size);
    bool (*mem_write)(uint32_t addr, const void* data, uint32_t size);
    void* (*mem_ptr)(uint32_t addr, uint32_t size); /* NULL if not contiguous / unmapped */

    /* hooks: target = guest address of a function's first instruction ("main" base + offset).
     * Either callback may be NULL. Returns NULL if the address isn't the start of a
     * recompiled block (hooks only work on function entries / branch targets). Needs at least
     * one callback: both NULL fails with "no callback" before the block check (use is_block to
     * probe an address). */
    ForgeHook* (*hook_create)(uint32_t target, ForgeHookPre pre, ForgeHookPost post, void* user);
    void (*hook_enable)(ForgeHook* hook, bool enabled);

    /* calls a guest function from a hook or callback (same game thread). Only for functions
     * that never wait (no locks under contention, no file/IPC/sleep): a system call inside
     * makes the call fail (returns false, registers restored, memory changes stay).
     * Integer/pointer args go to r0-r3 then the stack, float args to s0-s15 (hard-float ABI;
     * fargs may be NULL). out may be NULL. Calls made from inside a call don't fire hooks.
     * 0.7: code without a recompiled block (a module the plugin loaded into mem_alloc memory,
     * an untranslated instruction) runs on the JIT instead of failing the call (exe with mod
     * host support for it; older exes fail such calls). Hot reload: guest code and patched
     * words a plugin put in guest memory stay there, so such a plugin should not be reloaded. */
    bool (*call)(uint32_t function, const uint32_t* args, uint32_t count, const float* fargs,
                 uint32_t fcount, ForgeCallResult* out);

    /* MT Framework singletons (cSystem subclasses), by DTI class name ("sMhGUI", "sSavedata"...)
     * or DTI id. Available from forge_onInit on. getAll returns the count when out == NULL. */
    uint32_t (*singleton_getByName)(const char* name);
    uint32_t (*singleton_getById)(uint32_t id);
    uint32_t (*singleton_getAll)(uint32_t* out, uint32_t max);
    /* DTI of a guest MtObject (0 if unknown) and a DTI's class name (NULL if unknown) */
    uint32_t (*dti_of)(uint32_t object);
    const char* (*dti_name)(uint32_t dti);
    uint32_t (*dti_makeId)(const char* name);

    /* byte patterns over a module's code/data ("E9 2D ?? 40"); 0 = not found. Changing code
     * bytes in guest memory does NOT change the recompiled game: use hooks for code. */
    uint32_t (*pattern_find)(const char* module, const char* pattern, uint32_t from);

    /* the export's mod folder, <game>/mods/<title id> (UTF-8). A plugin's own folder is
     * <mods_dir>/<its mod name>; GetModuleFileName (Linux: dladdr) of the plugin gives the exact
     * path. */
    const char* (*mods_dir)(void);

    /* Dear ImGui of the menu: version string (IMGUI_VERSION) the plugin must match, and
     * whether Forge's menu is open now. */
    const char* imgui_version;
    bool (*menu_isOpen)(void);

    /* --- 0.2 --- (check forge_api->size or version before use) */

    /* guest memory for the plugin's own data: a region the exe maps for mods, readable and
     * writable by game code like any other memory (pass these addresses to the game, point
     * registers at them, build structs there). Zeroed; align = a power of two (16 at least).
     * Returns 0 when there is none (full, or an exe older than mod host ABI 2). Thread-safe;
     * valid until mem_free or the end of the process: free your blocks in forge_onUnload. */
    uint32_t (*mem_alloc)(uint32_t size, uint32_t align);
    void (*mem_free)(uint32_t addr);

    /* --- 0.3 --- (check forge_api->size before use) */

    /* size in pixels of the picture the game is shown in (the window / swapchain image), as of
     * the last frame presented; false before the first present. The game itself may render at
     * another size and be stretched to this (e.g. 1920x1080 shown on 3440x1440). */
    bool (*screen_size)(uint32_t* width, uint32_t* height);

    /* --- 0.4 --- (check forge_api->size before use; list "required_ver" 0.4) */

    /* Calls a guest function like `call`, but when `function` is overridden and the caller is
     * that override, the call goes to the next override down (the one registered before it)
     * and, once there are none, to the real function. Only the calling thread is affected: no
     * hook, override or trace fires inside the call, and the post hooks of the outer call fire
     * once, after the override returns. Call it from the override (same game thread). Same
     * rules and limits as `call` (no system calls inside; fails on the ImGui thread). Down a
     * chain of several overrides only the register arguments (r0-r3, s0-s15) are passed on;
     * stack arguments (5th and later) are those of the original call. */
    bool (*call_original)(uint32_t function, const uint32_t* args, uint32_t count,
                          const float* fargs, uint32_t fcount, ForgeCallResult* out);

    /* Replaces the function at `target` (same address rules as hook_create; NULL if it isn't
     * the start of a recompiled block). Order of a call: pre hooks (the first to return
     * FORGE_HOOK_RETURN ends it) -> override -> post hooks, which run once, after the override
     * returns, and see its return value. With several overrides on one function the one
     * registered last (loaded last) is the outermost; its call_original reaches the next.
     * Removed with the plugin on unload / hot reload; hook_enable(false) lets the function run
     * as if there were no override. The override does not apply to calls made with `call`
     * (hooks never fire inside those). */
    ForgeHook* (*hook_override)(uint32_t target, ForgeOverride fn, void* user);

    /* --- 0.5: settings files (ini) --- (check forge_api->size before use)
     *
     * config_open("mymod.ini") opens <mods dir>/<your mod folder>/mymod.ini (a relative name,
     * no "..": subfolders are fine). A missing file is fine: the getters then return their
     * defaults, and config_save creates it. Syntax: [section] key = value, ';' or '#' lines are
     * comments. Returns NULL for a bad name. The handle belongs to the plugin and is freed with
     * it (unload / hot reload); thread-safe.
     *
     * Every getter takes a default and remembers it: config_save writes the keys the file
     * doesn't have yet (a first start makes a complete file), changes the ones set with
     * config_set, and leaves the rest of the file (comments, order, other keys) alone.
     * config_changed returns true once after the file changed on disk (a hand edit) and has
     * re-read it: poll it from forge_onUpdate for live settings. A value you set but did not
     * save is lost then. config_string's pointer is valid until the next config_set /
     * config_changed / config_string call on that handle from any thread: copy it. */
    ForgeConfig* (*config_open)(const char* name);
    int32_t (*config_int)(ForgeConfig* cfg, const char* section, const char* key, int32_t def);
    float (*config_float)(ForgeConfig* cfg, const char* section, const char* key, float def);
    bool (*config_bool)(ForgeConfig* cfg, const char* section, const char* key, bool def);
    const char* (*config_string)(ForgeConfig* cfg, const char* section, const char* key,
                                 const char* def);
    bool (*config_set)(ForgeConfig* cfg, const char* section, const char* key, const char* value);
    bool (*config_save)(ForgeConfig* cfg);
    bool (*config_changed)(ForgeConfig* cfg);

    /* --- 0.7 --- (check forge_api->size before use) */

    /* true when a recompiled block starts at `addr`, i.e. hook_create / hook_override accept
     * it. No log output, unlike a failed hook_create. */
    bool (*is_block)(uint32_t addr);
} ForgeApi;

extern const ForgeApi* forge_api;

#ifdef __cplusplus
#define FORGE_EXTERN_C extern "C"
#else
#define FORGE_EXTERN_C
#endif

/* C and C++ plugins alike: the exports must have unmangled (C) names. In C++, also put the
 * forge_on* entry points in an extern "C" block (or prefix them with FORGE_EXTERN_C). */
#define FORGE_PLUGIN_DEFINE_API                                                                    \
    const ForgeApi* forge_api = NULL; /* C linkage from the declaration above */                  \
    FORGE_EXTERN_C FORGE_EXPORT void forge_setApi(const ForgeApi* api) { forge_api = api; }

#ifdef __cplusplus
}
#endif

#endif /* FORGE_PC_H */
