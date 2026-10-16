// Forge PC: crash attribution. When the game faults, forge.log says which plugin it was: the
// plugin whose code faulted, or the plugin whose hook / override / update / ImGui callback was
// running on the faulting thread (also when the fault was in the game code it called).
// SPDX-License-Identifier: MIT
//
// A fault in a plugin's own code is reported the moment it happens (a first-chance handler:
// the recompiled game handles faults of its own that must not be reported). A fault anywhere
// else is reported when nothing handled it (the game is going down) if a plugin callback was
// running. Reports leave a marker file (crash.txt) that a clean shutdown removes: if it is
// still there at the next launch, the last session died after such a report, and with
// [crash] auto_disable = true that plugin's mod is switched off (forge.ini [mods]).

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "forge_internal.h"
#include "platform.h"

#ifndef _WIN32
#include <signal.h>
#include <ucontext.h>
#endif

namespace forge {

thread_local CallbackCtx t_callback;

namespace {

struct Range {
    std::atomic<uintptr_t> begin{0};
    std::atomic<uintptr_t> end{0};
    std::atomic<ForgePluginState*> state{nullptr};
};
constexpr size_t kMaxRanges = 64;
Range g_ranges[kMaxRanges];
std::mutex g_range_lock; // writers only: the fault handlers read without a lock
uintptr_t g_forge_begin{}, g_forge_end{};
std::atomic<int> g_reports{0};
std::atomic<bool> g_marker_written{false};
std::atomic<bool> g_installed{false};

std::filesystem::path MarkerPath() {
    return PathU8(LoaderDir() + "/crash.txt");
}

const Range* RangeAt(uintptr_t a) {
    for (const Range& r : g_ranges) {
        const uintptr_t b = r.begin.load(std::memory_order_acquire);
        if (b && a >= b && a < r.end.load(std::memory_order_relaxed)) {
            return &r;
        }
    }
    return nullptr;
}

bool InForge(uintptr_t a) {
    return g_forge_begin && a >= g_forge_begin && a < g_forge_end;
}

/// Writes one report (forge.log + the exe log) and, for a plugin, the marker file.
/// `code_range` = the plugin whose code faulted (or null); `ctx` = what the thread was running.
void Report(const char* how, uint64_t code, uintptr_t addr, const char* access, uintptr_t target,
            const Range* code_range, const CallbackCtx& ctx, bool mark) {
    ForgePluginState* plugin = code_range ? code_range->state.load() : ctx.plugin;
    char where[200] = "";
    if (code_range) {
        std::snprintf(where, sizeof(where), "in the plugin's own code (+%#llx)",
                      static_cast<unsigned long long>(addr - code_range->begin.load()));
    } else if (InForge(addr)) {
        std::snprintf(where, sizeof(where), "inside Forge, called by the plugin");
    } else {
        std::snprintf(where, sizeof(where), "in game or system code (%p)",
                      reinterpret_cast<void*>(addr));
    }
    char what[160] = "";
    if (ctx.plugin == plugin && ctx.what) {
        if (ctx.target) {
            std::snprintf(what, sizeof(what), "; running its %s on %s", ctx.what,
                          FormatGuestAddr(ctx.target).c_str());
        } else {
            std::snprintf(what, sizeof(what), "; running its %s callback", ctx.what);
        }
    }
    char access_text[96] = "";
    if (access) {
        std::snprintf(access_text, sizeof(access_text), " (%s %p)", access,
                      reinterpret_cast<void*>(target));
    }
    char line[900];
    if (plugin) {
        std::snprintf(line, sizeof(line),
                      "%s: fault %#llx at %p%s %s%s%s. Plugin '%s' = %s (loaded as %s), mod %s",
                      how, static_cast<unsigned long long>(code), reinterpret_cast<void*>(addr),
                      access_text, where, what, "", plugin->name.c_str(), plugin->file.c_str(),
                      plugin->live.c_str(), plugin->mod.c_str());
    } else {
        std::snprintf(line, sizeof(line), "%s: fault %#llx at %p%s %s (no plugin callback was running)",
                      how, static_cast<unsigned long long>(code), reinterpret_cast<void*>(addr),
                      access_text, where);
    }
    LogFault(line);
    if (plugin && mark && !g_marker_written.exchange(true)) {
        std::ofstream out(MarkerPath(), std::ios::trunc);
        out << "mod=" << plugin->mod << "\nplugin=" << plugin->file << "\nlive=" << plugin->live
            << "\nname=" << plugin->name << "\nreport=" << line << "\n";
    }
}

bool CanReport() {
    return g_reports.fetch_add(1) < 16;
}

#ifdef _WIN32

bool IsFaultCode(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

const char* AccessOf(const EXCEPTION_RECORD* r, uintptr_t* target) {
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        r->NumberParameters >= 2) {
        *target = static_cast<uintptr_t>(r->ExceptionInformation[1]);
        switch (r->ExceptionInformation[0]) {
        case 0: return "reading";
        case 1: return "writing";
        case 8: return "executing";
        }
    }
    return nullptr;
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter{};

LONG CALLBACK FirstChance(PEXCEPTION_POINTERS ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    if (!IsFaultCode(rec->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    const Range* code_range = RangeAt(addr);
    const CallbackCtx ctx = t_callback;
    // only what is surely a plugin's: its own code, or Forge code running its callback
    if (!code_range && !(InForge(addr) && ctx.plugin)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (CanReport()) {
        uintptr_t target = 0;
        const char* access = AccessOf(rec, &target);
        Report("first-chance", rec->ExceptionCode, addr, access, target, code_range, ctx, true);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI Unhandled(PEXCEPTION_POINTERS ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    const Range* code_range = RangeAt(addr);
    const CallbackCtx ctx = t_callback;
    if (CanReport() || ctx.plugin || code_range) {
        uintptr_t target = 0;
        const char* access = AccessOf(rec, &target);
        Report("UNHANDLED (the game is going down)", rec->ExceptionCode, addr, access, target,
               code_range, ctx, true);
    }
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallHandlers() {
    static bool vectored = false;
    if (!vectored) {
        vectored = AddVectoredExceptionHandler(1, FirstChance) != nullptr; // before the exe's
    }
    // the exe or a plugin may have set its own filter since: ours goes on top, theirs is next
    const auto old = SetUnhandledExceptionFilter(Unhandled);
    if (old != Unhandled) {
        g_prev_filter = old;
    }
}

#else // Linux

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_old[4];

uintptr_t FaultPc(void* uctx) {
#if defined(__x86_64__)
    return static_cast<uintptr_t>(static_cast<ucontext_t*>(uctx)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<uintptr_t>(static_cast<ucontext_t*>(uctx)->uc_mcontext.pc);
#else
    (void)uctx;
    return 0;
#endif
}

void OnSignal(int sig, siginfo_t* si, void* uctx) {
    const uintptr_t pc = FaultPc(uctx);
    const Range* code_range = RangeAt(pc);
    const CallbackCtx ctx = t_callback;
    if ((code_range || (InForge(pc) && ctx.plugin)) && CanReport()) {
        const bool memory = sig == SIGSEGV || sig == SIGBUS;
        Report("fault", static_cast<uint64_t>(sig), pc, memory ? "accessing" : nullptr,
               reinterpret_cast<uintptr_t>(si->si_addr), code_range, ctx, true);
    }
    // on to whoever had the signal before (the exe may handle faults of its own)
    for (size_t i = 0; i < 4; ++i) {
        if (kSignals[i] != sig) {
            continue;
        }
        const struct sigaction& old = g_old[i];
        if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
            old.sa_sigaction(sig, si, uctx);
            return;
        }
        if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler != SIG_DFL &&
            old.sa_handler != SIG_IGN && old.sa_handler) {
            old.sa_handler(sig);
            return;
        }
        break;
    }
    signal(sig, SIG_DFL); // the fault happens again and ends the process the usual way
}

void InstallHandlers() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    for (size_t i = 0; i < 4; ++i) {
        struct sigaction sa {};
        sa.sa_sigaction = OnSignal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(kSignals[i], &sa, &g_old[i]);
    }
}

#endif

} // namespace

void CrashInit() {
    if (!g_installed.exchange(true)) {
        if (const void* mod = plat::ModuleOf(reinterpret_cast<const void*>(&CrashInit))) {
            g_forge_begin = reinterpret_cast<uintptr_t>(mod);
            g_forge_end = g_forge_begin + plat::ImageSize(mod);
        }
    }
    InstallHandlers(); // again after the plugins loaded: our filter goes back on top
}

void CrashPluginLoaded(const void* base, ForgePluginState* state) {
    const size_t size = plat::ImageSize(base);
    if (!base || !size) {
        return;
    }
    std::scoped_lock lk{g_range_lock};
    for (Range& r : g_ranges) {
        if (r.begin.load() == 0) {
            r.state.store(state);
            r.end.store(reinterpret_cast<uintptr_t>(base) + size);
            r.begin.store(reinterpret_cast<uintptr_t>(base), std::memory_order_release);
            return;
        }
    }
    Log(FORGE_LOG_WARN, "crash attribution: too many plugins, %s is not tracked",
        state->file.c_str());
}

void CrashPluginUnloaded(const void* base) {
    std::scoped_lock lk{g_range_lock};
    for (Range& r : g_ranges) {
        if (r.begin.load() == reinterpret_cast<uintptr_t>(base)) {
            r.begin.store(0, std::memory_order_release);
        }
    }
}

void CrashCheckLastSession() {
    const auto marker = MarkerPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(marker, ec)) {
        return;
    }
    std::string mod, plugin, report;
    {
        std::ifstream in(marker);
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "mod") {
                mod = value;
            } else if (key == "plugin") {
                plugin = value;
            } else if (key == "report") {
                report = value;
            }
        }
    }
    Log(FORGE_LOG_WARN, "the last session did not end cleanly after a fault in plugin %s "
                        "(mod %s)", plugin.c_str(), mod.c_str());
    if (!report.empty()) {
        Log(FORGE_LOG_WARN, "last session's report: %s", report.c_str());
    }
    if (!mod.empty()) {
        const std::string auto_off = ConfigValue("crash", "auto_disable");
        if (auto_off == "true" || auto_off == "1" || auto_off == "on") {
            ConfigSet("mods", mod.c_str(), "off");
            SetModNote(mod, "switched off: the last session crashed in it");
            Log(FORGE_LOG_WARN, "mod %s is switched off ([crash] auto_disable); switch it on in "
                                "the Forge menu to try again", mod.c_str());
        } else {
            SetModNote(mod, "the last session crashed in it");
        }
    }
    auto last = marker;
    last.replace_filename("crash_last.txt");
    std::filesystem::rename(marker, last, ec);
    if (ec) {
        std::filesystem::remove(marker, ec);
    }
}

void CrashClean() {
    std::error_code ec;
    std::filesystem::remove(MarkerPath(), ec);
}

} // namespace forge
