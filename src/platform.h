// Forge PC: the few OS calls the loader needs (Windows: LoadLibrary; Linux: dlopen).
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#define FORGE_RETURN_ADDRESS() _ReturnAddress()
#else
#include <dlfcn.h>
#define FORGE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace forge::plat {

/// Plugin file extension: plugins are mods/<TID>/<mod>/plugins/*<kLibExt>.
#ifdef _WIN32
inline constexpr const char* kLibExt = ".dll";
#else
inline constexpr const char* kLibExt = ".so";
#endif

using Lib = void*;

/// Loads a plugin (its own folder is searched first for its dependencies on Windows). On
/// failure returns nullptr and puts the reason in `error`.
inline Lib LibOpen(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
    HMODULE h = LoadLibraryExW(path.wstring().c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        error = "error " + std::to_string(GetLastError());
    }
    return reinterpret_cast<Lib>(h);
#else
    // RTLD_LOCAL: every plugin keeps its own copy of Dear ImGui etc.
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
    }
    return h;
#endif
}

inline void* LibSym(Lib lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

inline void LibClose(Lib lib) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

/// Base address of the loaded module containing `addr` (nullptr if none): identifies which
/// plugin a code address belongs to.
inline const void* ModuleOf(const void* addr) {
#ifdef _WIN32
    HMODULE mod{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod)) {
        return nullptr;
    }
    return mod;
#else
    Dl_info info{};
    if (!dladdr(addr, &info)) {
        return nullptr;
    }
    return info.dli_fbase;
#endif
}

} // namespace forge::plat
