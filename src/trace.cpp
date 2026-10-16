// Forge PC: ini-driven hook tracing. A loader-side replacement for hand-written probe plugins:
// forge.ini lists guest functions (offsets in "main"), each gets a read-only pre hook that
// writes a line to trace.log per call. No plugin API: nothing for plugins to be compatible with.
// SPDX-License-Identifier: MIT
//
//   [trace]
//   enabled = true
//   0x1C358 = limit=50
//   0x2A410 = limit=10, args=true
//   0x3B000 = limit=20, ret=true
//
// Options: limit=N (lines written for that function; it keeps counting; 0 = no limit, default
// 100), args=true (r0-r3 at entry), ret=true (a second line when it returns: r0, r1, s0).
// The hooks are registered first on their addresses (before any plugin's), so they see every
// call, also the ones a plugin hook skips. forge.ini is watched: add, change or remove a line
// while the game runs and it applies within a second or two.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "forge_internal.h"

namespace forge {
namespace {

constexpr uint32_t kDefaultLimit = 100;
constexpr size_t kMaxQueued = 50000; // lines waiting for the writer; beyond that they're dropped

struct Trace {
    uint32_t offset{}; // relative to "main"
    uint32_t addr{};
    std::atomic<uint32_t> limit{kDefaultLimit}; // 0 = unlimited
    std::atomic<bool> args{false};
    std::atomic<bool> ret{false};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint32_t> logged{0};
    std::string spec; // as written, to see what changed
    ForgeHook* pre{};
    ForgeHook* post{};
};

std::mutex g_apply_lock;                // applying a config (game thread / shutdown)
std::deque<Trace> g_all;                // never freed: a hook may be running when it's removed
std::map<uint32_t, Trace*> g_active;    // by offset

std::mutex g_queue_lock;
std::vector<std::string> g_queue;
std::atomic<uint64_t> g_dropped{0};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};
std::atomic<bool> g_started{false};
std::thread g_writer;
std::ofstream g_out;
std::chrono::steady_clock::time_point g_t0;

std::filesystem::path IniPath() {
    return PathU8(LoaderDir() + "/forge.ini");
}

std::filesystem::file_time_type IniTime() {
    std::error_code ec;
    return std::filesystem::last_write_time(IniPath(), ec);
}

void Push(std::string line) {
    std::scoped_lock lk{g_queue_lock};
    if (g_queue.size() >= kMaxQueued) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_queue.push_back(std::move(line));
}

std::string Stamp() {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "[%9.3f f=%u]", s, FrameCount());
    return buf;
}

void WriteQueued() {
    std::vector<std::string> lines;
    {
        std::scoped_lock lk{g_queue_lock};
        lines.swap(g_queue);
    }
    const uint64_t dropped = g_dropped.exchange(0);
    if (lines.empty() && !dropped) {
        return;
    }
    if (!g_out.is_open()) {
        g_out.open(PathU8(LoaderDir() + "/trace.log"), std::ios::trunc); // first line of the run
        g_out << "# Forge PC trace log. [seconds f=game update] function #call thread lr args\n";
    }
    for (const auto& l : lines) {
        g_out << l << '\n';
    }
    if (dropped) {
        g_out << "# " << dropped << " lines dropped (the writer fell behind)\n";
    }
    g_out.flush();
}

void WriterMain() {
    int ticks = 0;
    auto known = IniTime();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        WriteQueued();
        if (++ticks % 5 == 0) {
            const auto t = IniTime();
            if (t != known) {
                known = t;
                g_reload = true; // applied on the game thread (TraceTick)
            }
        }
    }
    WriteQueued();
}

// --- the hooks ---------------------------------------------------------------------------------

ForgeHookAction OnTrace(ForgeCpu* cpu, void* user) {
    auto* t = static_cast<Trace*>(user);
    const uint64_t n = t->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t limit = t->limit.load(std::memory_order_relaxed);
    const uint32_t k = t->logged.load(std::memory_order_relaxed);
    if (limit && k >= limit) {
        return FORGE_HOOK_CONTINUE; // counting only
    }
    if (limit && t->logged.fetch_add(1, std::memory_order_relaxed) >= limit) {
        return FORGE_HOOK_CONTINUE; // another thread took the last line
    }
    char buf[300];
    int len = std::snprintf(buf, sizeof(buf), "%s main+%#x #%llu t=%#x lr=%s", Stamp().c_str(),
                            t->offset, static_cast<unsigned long long>(n), CurrentThreadKey(),
                            FormatGuestAddr(cpu->r[14]).c_str());
    if (t->args.load(std::memory_order_relaxed) && len > 0 && len < static_cast<int>(sizeof(buf))) {
        std::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len),
                      " r0=%08x r1=%08x r2=%08x r3=%08x", cpu->r[0], cpu->r[1], cpu->r[2],
                      cpu->r[3]);
    }
    Push(buf);
    if (limit && k + 1 == limit) {
        std::snprintf(buf, sizeof(buf), "%s main+%#x limit of %u lines reached, still counting",
                      Stamp().c_str(), t->offset, limit);
        Push(buf);
    }
    return FORGE_HOOK_CONTINUE;
}

void OnTraceRet(ForgeCpu* cpu, const uint32_t args[4], void* user) {
    auto* t = static_cast<Trace*>(user);
    const uint32_t limit = t->limit.load(std::memory_order_relaxed);
    if (limit && t->logged.load(std::memory_order_relaxed) > limit) {
        return;
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s main+%#x returned r0=%08x r1=%08x s0=%g (to %s)",
                  Stamp().c_str(), t->offset, cpu->r[0], cpu->r[1],
                  static_cast<double>(cpu->vfp.s[0]), FormatGuestAddr(args[4]).c_str());
    Push(buf);
}

// --- configuration -----------------------------------------------------------------------------

struct Spec {
    uint32_t limit{kDefaultLimit};
    bool args{false};
    bool ret{false};
    std::string text;
};

bool IsTrue(const std::string& v) {
    return v == "true" || v == "1" || v == "on" || v == "yes";
}

bool ParseSpec(const std::string& text, Spec* out) {
    out->text = text;
    std::string tok;
    bool ok = true;
    auto flush = [&] {
        if (tok.empty()) {
            return;
        }
        const auto eq = tok.find('=');
        const std::string key = eq == std::string::npos ? tok : tok.substr(0, eq);
        const std::string val = eq == std::string::npos ? "true" : tok.substr(eq + 1);
        if (key == "limit") {
            out->limit = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
        } else if (key == "args") {
            out->args = IsTrue(val);
        } else if (key == "ret") {
            out->ret = IsTrue(val);
        } else {
            Log(FORGE_LOG_WARN, "trace: unknown option '%s' in \"%s\"", tok.c_str(), text.c_str());
            ok = false;
        }
        tok.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ' ' || c == '\t') {
            flush();
        } else {
            tok.push_back(c);
        }
    }
    flush();
    return ok;
}

void Remove(Trace* t) {
    HookRemove(t->pre);
    HookRemove(t->post);
    t->pre = t->post = nullptr;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s main+%#x removed: %llu calls, %u lines written",
                  Stamp().c_str(), t->offset, static_cast<unsigned long long>(t->calls.load()),
                  t->logged.load());
    Push(buf);
}

bool Add(uint32_t offset, const Spec& spec) {
    const uint32_t addr = MainBase() + offset;
    if (!IsCodeBlock(addr)) {
        Log(FORGE_LOG_ERROR, "trace: main+%#x is not the start of a recompiled block, skipped",
            offset);
        return false;
    }
    Trace& t = g_all.emplace_back();
    t.offset = offset;
    t.addr = addr;
    t.limit = spec.limit;
    t.args = spec.args;
    t.ret = spec.ret;
    t.spec = spec.text;
    t.pre = HookAdd(addr, OnTrace, nullptr, &t, true);
    if (spec.ret) {
        t.post = HookAdd(addr, nullptr, OnTraceRet, &t, true);
    }
    if (!t.pre) {
        return false;
    }
    g_active[offset] = &t;
    Log(FORGE_LOG_INFO, "trace: main+%#x (%s)", offset, spec.text.empty() ? "defaults"
                                                                             : spec.text.c_str());
    return true;
}

/// Brings the registered traces in line with forge.ini's [trace] section.
void Apply() {
    std::scoped_lock lk{g_apply_lock};
    const IniMap ini = ReadIni(IniPath());
    std::map<uint32_t, Spec> want;
    const auto sec = ini.find("trace");
    bool enabled = false;
    if (sec != ini.end()) {
        for (const auto& [key, value] : sec->second) {
            if (key == "enabled") {
                enabled = IsTrue(value);
                continue;
            }
            char* end = nullptr;
            const unsigned long off = std::strtoul(key.c_str(), &end, 0);
            if (key.empty() || *end != '\0') {
                Log(FORGE_LOG_WARN, "trace: \"%s\" is not an offset (0x1C358 = limit=50)",
                    key.c_str());
                continue;
            }
            Spec spec;
            ParseSpec(value, &spec);
            want[static_cast<uint32_t>(off)] = spec;
        }
    }
    if (!enabled) {
        want.clear();
    }
    for (auto it = g_active.begin(); it != g_active.end();) {
        const auto w = want.find(it->first);
        if (w == want.end() || w->second.ret != it->second->ret.load()) {
            Remove(it->second); // gone (a changed ret= re-registers below)
            it = g_active.erase(it);
        } else {
            Trace* t = it->second;
            if (w->second.text != t->spec) { // new limit / args: start counting lines again
                t->limit = w->second.limit;
                t->args = w->second.args;
                t->logged = 0;
                t->spec = w->second.text;
                Log(FORGE_LOG_INFO, "trace: main+%#x now (%s)", t->offset, t->spec.c_str());
            }
            want.erase(w);
            ++it;
        }
    }
    for (const auto& [off, spec] : want) {
        Add(off, spec);
    }
}

} // namespace

void TraceStart() {
    g_t0 = std::chrono::steady_clock::now();
    g_started = true;
    Apply();
    g_writer = std::thread(WriterMain);
}

void TraceTick() {
    if (g_started.load() && g_reload.exchange(false)) {
        Apply();
    }
}

void TraceShutdown() {
    if (!g_started.exchange(false)) {
        return;
    }
    {
        std::scoped_lock lk{g_apply_lock};
        for (auto& [off, t] : g_active) {
            Remove(t);
        }
        g_active.clear();
    }
    g_stop = true;
    if (g_writer.joinable()) {
        g_writer.join();
    }
}

} // namespace forge
