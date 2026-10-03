/*
 * hello: Forge PC test plugin. Exercises every part of the loader:
 *   - forge_onLoad / forge_onInit / forge_onUpdate / forge_onUnload
 *   - singletons (resolved through guest calls of MtObject::getDti)
 *   - a pre + post hook on the camera projection function (main+0x1C358)
 * Writes to user/forge.log every 10 seconds of game time.
 *
 * SPDX-License-Identifier: MIT
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "forge_pc.h"

FORGE_PLUGIN_DEFINE_API

#define PLUGIN "hello"

static void logf_(ForgeLogLevel level, const char* fmt, ...);

static volatile long s_pre_calls;
static volatile long s_post_calls;
static float s_last_aspect;
static double s_time;
static double s_dt_sum;
static long s_frames;

static ForgeHookAction projection_pre(ForgeCpu* cpu, void* user)
{
    (void)user;
    float aspect = 0.0f;
    /* r1 = camera parameters, aspect ratio at +0x38 (see game_settings.h) */
    if (forge_api->mem_read(cpu->r[1] + 0x38, &aspect, 4)) {
        s_last_aspect = aspect;
    }
    s_pre_calls++;
    return FORGE_HOOK_CONTINUE;
}

static void projection_post(ForgeCpu* cpu, const uint32_t args[4], void* user)
{
    (void)cpu;
    (void)args;
    (void)user;
    s_post_calls++;
}

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info)
{
    info->required_ver.major = FORGE_PC_VERSION_MAJOR;
    info->required_ver.minor = FORGE_PC_VERSION_MINOR;
    info->required_ver.patch = 0;
    strncpy(info->name, "Hello (Forge PC test)", sizeof(info->name) - 1);
}

FORGE_EXPORT void forge_onInit(void)
{
    const uint32_t main = forge_api->mem_getModuleBase("main");
    logf_(FORGE_LOG_INFO, "init: main at %#x, size %#x, mods in %s", main,
        forge_api->mem_getModuleSize("main"), forge_api->mods_dir());

    uint32_t objs[256];
    const uint32_t n = forge_api->singleton_getAll(objs, 256);
    logf_(FORGE_LOG_INFO, "%u singletons:", n);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t dti = forge_api->dti_of(objs[i]);
        const char* name = dti ? forge_api->dti_name(dti) : NULL;
        logf_(FORGE_LOG_INFO, "  %-28s @ %#010x", name ? name : "?", objs[i]);
    }
    logf_(FORGE_LOG_INFO, "sMhGUI = %#x, sSavedata = %#x",
        forge_api->singleton_getByName("sMhGUI"), forge_api->singleton_getByName("sSavedata"));

    /* a known pattern: the projection function's FOV literal 0.0174533 (pi/180) at +0x24 */
    const uint32_t lit = forge_api->pattern_find("main", "35 FA 8E 3C", main);
    logf_(FORGE_LOG_INFO, "pattern pi/180 first at main+%#x", lit ? lit - main : 0);

    if (!forge_api->hook_create(main + 0x1C358, projection_pre, projection_post, NULL)) {
        logf_(FORGE_LOG_ERROR, "projection hook failed");
    }
}

FORGE_EXPORT void forge_onUpdate(float dt)
{
    s_frames++;
    s_dt_sum += dt;
    s_time += dt;
    if (s_time >= 10.0) {
        logf_(FORGE_LOG_INFO, "%ld updates, avg dt %.4f s; projection pre %ld / post %ld, aspect %.4f",
            s_frames, s_dt_sum / (s_frames ? s_frames : 1), s_pre_calls, s_post_calls,
            s_last_aspect);
        s_time = 0;
        s_frames = 0;
        s_dt_sum = 0;
    }
}

FORGE_EXPORT void forge_onUnload(void)
{
    logf_(FORGE_LOG_INFO, "unload");
}

static void logf_(ForgeLogLevel level, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    forge_api->log(level, PLUGIN, buf);
}
