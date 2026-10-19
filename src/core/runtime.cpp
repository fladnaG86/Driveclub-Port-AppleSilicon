#include "core/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <pthread.h>
#include <sys/mman.h>

#include "common/log.h"
#include "core/signals.h"
#include "core/loader/linker.h"
#include "core/tls.h"

namespace Core::Runtime {

namespace {

// Layout the PS4 kernel hands to the executable's entry point.
struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[33];
};

constexpr size_t kMainStackSize = 16ull << 20;

// Sony modules libkernel would have started before the game's entry point.
constexpr const char* kPreloadedModules[] = {"libc", "libSceFios2"};

std::mutex g_start_lock;
std::set<std::string> g_started;
EntryParams g_params{};

void ProgramExit() {
    DC_INFO("guest called its exit function");
    std::exit(0);
}

[[noreturn]] void JumpToEntry(EntryParams* params, VAddr entry, void (*exit_fn)()) {
    // Mimic the PS4 kernel: the stack holds argc/argv-ish data on entry, is
    // misaligned by 8 like after a call, and rdi/rsi carry params/exit func.
    asm volatile("andq $-16, %%rsp\n"
                 "subq $8, %%rsp\n"
                 "pushq 8(%%rdi)\n"
                 "pushq 0(%%rdi)\n"
                 "jmp *%%rax\n"
                 :
                 : "D"(params), "S"(exit_fn), "a"(entry)
                 : "memory");
    __builtin_unreachable();
}

// Rosetta reserves 0xFC0000000-0x7000000000, so the game's fixed heap at
// 0x1000000000 cannot be mapped. Rewrite the movabs that hold that base (v1.28
// eboot) to 0x9000000000, which keeps bits 36-38 for the game's range checks.
constexpr u64 kFixedHeapOld = 0x1000000000, kFixedHeapNew = 0x9000000000;

std::vector<VAddr> HeapSites(const Loader::Module& exe) {
    const char* env = std::getenv("DC_HEAP_SITES");
    const std::string sites = env ? env : "748d85,748d8f,749247,74925a,5fa001";
    std::vector<VAddr> out;
    size_t pos = 0;
    while (pos < sites.size()) {
        const size_t comma = std::min(sites.find(',', pos), sites.size());
        out.push_back(exe.Base() + std::strtoull(sites.substr(pos, comma - pos).c_str(), nullptr, 16));
        pos = comma + 1;
    }
    return out;
}

bool IsHeapMovabs(VAddr site) {
    const auto* code = reinterpret_cast<const u8*>(site);
    u64 imm;
    std::memcpy(&imm, code + 2, 8);
    return (code[0] & 0xFE) == 0x48 && (code[1] & 0xF8) == 0xB8 && imm == kFixedHeapOld;
}

void RelocateFixedHeap(const Loader::Module& exe) {
    for (const VAddr site : HeapSites(exe)) {
        if (!IsHeapMovabs(site)) {
            DC_WARN("heap remap: no movabs %#llx at %#llx", kFixedHeapOld, site);
            continue;
        }
        auto* code = reinterpret_cast<u8*>(site);
        const VAddr page = site & ~0xFFFull;
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        std::memcpy(code + 2, &kFixedHeapNew, 8);
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_EXEC);
        DC_INFO("heap remap: %#llx -> %#llx at %#llx", kFixedHeapOld, kFixedHeapNew, site);
    }
}

// DC_PROBE_FN=<eboot offset>,...: logs every 5 s how often those functions ran.
void StartProbes(const Loader::Module& exe) {
    const char* env = std::getenv("DC_PROBE_FN");
    if (!env) {
        return;
    }
    std::vector<u64> offsets;
    for (const char* p = env; *p;) {
        char* q = nullptr;
        const u64 off = std::strtoull(p, &q, 16);
        if (Signals::AddProbe(exe.Base() + off)) {
            offsets.push_back(off);
        } else {
            DC_WARN("probe: no push rbp at eboot+%#llx", off);
        }
        p = *q == ',' ? q + 1 : q;
        if (q == p && *p) {
            break;
        }
    }
    // DC_PROBE_WATCH=<object index>,<offset of a pointer in it>,<bytes>,<sec>: objects are
    // the distinct rdi values of the first probe;
    // after <sec> seconds, also record which code touches the memory that
    // pointer points to.
    // DC_DUMP_STRTAB=<eboot offset>,<count>,<stride>,<offset of the char* in an entry>:
    // prints the strings of a table of entries once the game runs.
    if (const char* t = std::getenv("DC_DUMP_STRTAB")) {
        u64 off = 0, count = 0, stride = 0, ptr = 0;
        std::sscanf(t, "%llx,%llu,%llx,%llx", &off, &count, &stride, &ptr);
        const VAddr base = exe.Base();
        std::thread([=] {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            for (u64 k = 0; k < count; ++k) {
                const u8* e = reinterpret_cast<const u8*>(base + off + k * stride);
                const char* str = *reinterpret_cast<const char* const*>(e + ptr);
                DC_INFO("strtab %llu: byte0=%u %s", k, e[0], str ? str : "(null)");
            }
        }).detach();
    }
    std::thread([offsets] {
        u32 wk = 0, wbytes = 0;
        u64 woff = 0, winner = 0;
        double wsec = -1;
        if (const char* w = std::getenv("DC_PROBE_WATCH")) { // ...,<sec>[,<offset in the target>]
            std::sscanf(w, "%u,%llx,%x,%lf,%llx", &wk, &woff, &wbytes, &wsec, &winner);
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool watching = false;
        auto last = t0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto now = std::chrono::steady_clock::now();
            if (wsec >= 0 && !watching && now - t0 > std::chrono::duration<double>(wsec)) {
                // DC_PROBE_WATCH_RSI: objects are the rsi values; pointer offset ffffffff: no
                // dereference, watch the object itself
                const auto args = Signals::ProbeDistinctArgs(0, std::getenv("DC_PROBE_WATCH_RSI") != nullptr);
                if (wk < args.size() && args[wk]) {
                    const u64 target = (woff == 0xffffffff ? args[wk] : *reinterpret_cast<u64*>(args[wk] + woff)) + winner;
                    DC_INFO("probe: watching %#llx (%u bytes) = [%#llx + %#llx]", target, wbytes, args[wk], woff);
                    Signals::WatchAccess(target, wbytes);
                    watching = true;
                }
            }
            if (watching) {
                Signals::WatchRearm();
            }
            if (now - last < std::chrono::seconds(5)) {
                continue;
            }
            last = now;
            if (static const bool objs = std::getenv("DC_PROBE_OBJECTS") != nullptr; objs) {
                // DC_PROBE_OBJECTS=<hex offsets>: dwords at those offsets of every object
                // the first probe saw
                std::string line;
                for (u64 o : Signals::ProbeDistinctArgs(0)) {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), " %#llx[", o);
                    line += buf;
                    const char* e = std::getenv("DC_PROBE_OBJECTS");
                    for (const char* q = e; *q;) {
                        char* r = nullptr;
                        const u64 off = std::strtoull(q, &r, 16);
                        std::snprintf(buf, sizeof(buf), "%llx=%x ", off, *reinterpret_cast<u32*>(o + off));
                        line += buf;
                        q = *r == ',' ? r + 1 : r;
                        if (r == q && *q) break;
                    }
                    line += "]";
                }
                DC_INFO("probe: objects%s", line.c_str());
            }
            // DC_PROBE_SCAN=<start>,<end>: offsets of the non-zero dwords in that range of
            // each rsi object of the first probe
            if (const char* sc = std::getenv("DC_PROBE_SCAN")) {
                u64 lo = 0, hi = 0, step = 4; // ,<step> optional
                std::sscanf(sc, "%llx,%llx,%llx", &lo, &hi, &step);
                for (u64 o : Signals::ProbeDistinctArgs(0, true)) {
                    std::string line;
                    for (u64 off = lo; off + 4 <= hi; off += step) {
                        const u32 v = *reinterpret_cast<const u32*>(o + off);
                        if (v & 0x7FFFFFFF) {
                            char buf[40];
                            std::snprintf(buf, sizeof(buf), " %llx=%x", off, v);
                            line += buf;
                        }
                    }
                    DC_INFO("probe: scan %#llx:%s", o, line.c_str());
                }
            }
            if (watching) {
                std::string sites;
                for (u64 site : Signals::WatchSites()) {
                    sites += " " + Loader::Linker::Instance()->Describe(site);
                }
                DC_INFO("probe: watch sites:%s", sites.c_str());
            }
            const auto hits = Signals::ProbeHits();
            std::string line;
            for (size_t k = 0; k < hits.size() && k < offsets.size(); ++k) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " %llx:%llu", offsets[k], hits[k]);
                line += buf;
            }
            DC_INFO("probe:%s", line.c_str());
            for (size_t k = 0; k < offsets.size(); ++k) {
                std::string callers;
                for (u64 r : Signals::ProbeCallers(static_cast<u32>(k))) {
                    callers += " " + Loader::Linker::Instance()->Describe(r);
                }
                DC_INFO("probe: %llx called from%s", offsets[k], callers.c_str());
            }
        }
    }).detach();
}

void* GuestMain(void*) {
    Tls::SetupThread();
    RelocateFixedHeap(*Loader::Linker::Instance()->Executable());
    StartProbes(*Loader::Linker::Instance()->Executable());

    for (const char* name : kPreloadedModules) {
        StartModule(name);
    }

    auto* exe = Loader::Linker::Instance()->Executable();
    g_params.argc = 1;
    g_params.argv[0] = "/app0/eboot.bin";
    DC_INFO("jumping to entry point %#llx", exe->Entry());
    JumpToEntry(&g_params, exe->Entry(), &ProgramExit);
}

} // namespace

int StartModule(const char* name, u64 args, const void* argp) {
    Loader::Module* m = Loader::Linker::Instance()->FindModule(name);
    if (!m) {
        DC_WARN("module_start: %s is not loaded", name);
        return -1;
    }
    {
        std::scoped_lock lk(g_start_lock);
        if (!g_started.insert(name).second) {
            return 0;
        }
    }
    if (!m->HasInit()) {
        return 0;
    }
    DC_INFO("starting module %s with args=%llu argp=%p", name, args, argp);
    // The PRX init stub tail-calls its third argument when it is non-null, so
    // passing the proc param here jumps into data (seen with libc of v1.28).
    using ModuleStart = int (*)(u64 args, const void* argp, void* param);
    const int ret = reinterpret_cast<ModuleStart>(m->InitAddr())(args, argp, nullptr);
    DC_INFO("module %s started: %d", name, ret);
    return ret;
}

void StartGame(Loader::Linker&) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainStackSize);
    pthread_t thread;
    pthread_create(&thread, &attr, GuestMain, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
}

bool IsSupportedExecutable(const Loader::Module& exe) {
    const auto sites = HeapSites(exe);
    return std::all_of(sites.begin(), sites.end(), IsHeapMovabs);
}

} // namespace Core::Runtime
