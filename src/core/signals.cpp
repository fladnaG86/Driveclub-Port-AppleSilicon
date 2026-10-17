#include "core/signals.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <execinfo.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "common/log.h"

namespace Core::Signals {

namespace {

struct PatchArea {
    VAddr code_begin, code_end;
    VAddr next, end;
};

std::mutex g_patch_lock;
std::vector<PatchArea> g_areas;
AddressDescriber g_describer = nullptr;
std::atomic<u64> g_patched_sites{0};

constexpr u64 kHostPage = 0x1000;

// Decoded `extrq xmm, imm8 len, imm8 idx`: 66 [REX] 0F 78 /0 ib ib, register form.
struct Extrq {
    int length; // bytes of the instruction
    int xmm;    // 0..15
    int len;    // field length in bits (0 means 64)
    int idx;    // field start bit
};

bool DecodeExtrq(const u8* p, Extrq& out) {
    if (p[0] != 0x66) {
        return false;
    }
    int i = 1;
    int rex_b = 0;
    if ((p[i] & 0xF0) == 0x40) {
        rex_b = p[i] & 1;
        ++i;
    }
    if (p[i] != 0x0F || p[i + 1] != 0x78) {
        return false;
    }
    const u8 modrm = p[i + 2];
    if ((modrm >> 6) != 3 || ((modrm >> 3) & 7) != 0) {
        return false;
    }
    out.xmm = (modrm & 7) | (rex_b << 3);
    out.len = p[i + 3] & 0x3F;
    out.idx = p[i + 4] & 0x3F;
    out.length = i + 5;
    return true;
}

// psllq/psrlq xmmN, imm8: 66 [41] 0F 73 /6 or /2 ib
u8* EmitShift(u8* c, int xmm, int ext, int amount) {
    *c++ = 0x66;
    if (xmm >= 8) {
        *c++ = 0x41;
    }
    *c++ = 0x0F;
    *c++ = 0x73;
    *c++ = static_cast<u8>(0xC0 | (ext << 3) | (xmm & 7));
    *c++ = static_cast<u8>(amount);
    return c;
}

u8* EmitJmp(u8* c, VAddr target) {
    const s64 rel = static_cast<s64>(target) - static_cast<s64>(reinterpret_cast<VAddr>(c) + 5);
    *c++ = 0xE9;
    const s32 rel32 = static_cast<s32>(rel);
    std::memcpy(c, &rel32, 4);
    return c + 4;
}

// Replaces the EXTRQ at `site` with a jump to an SSE2 equivalent:
//   psllq xmm, 64 - idx - len   ; drop bits above the field
//   psrlq xmm, 64 - len         ; move the field to bit 0, zero-extended
// The upper qword is undefined after EXTRQ, so shifting it too is fine.
bool PatchExtrq(VAddr site) {
    Extrq e;
    if (!DecodeExtrq(reinterpret_cast<const u8*>(site), e)) {
        return false;
    }
    PatchArea* area = nullptr;
    for (PatchArea& a : g_areas) {
        if (site >= a.code_begin && site < a.code_end) {
            area = &a;
        }
    }
    if (!area || area->next + 32 > area->end) {
        DC_ERROR("no trampoline space for EXTRQ at %#llx", site);
        return false;
    }

    const int len = e.len == 0 ? 64 : e.len;
    u8* tramp = reinterpret_cast<u8*>(area->next);
    u8* c = tramp;
    c = EmitShift(c, e.xmm, 6, 64 - e.idx - len);
    c = EmitShift(c, e.xmm, 2, 64 - len);
    c = EmitJmp(c, site + e.length);
    area->next = AlignUp(reinterpret_cast<VAddr>(c), 16);

    // jmp tramp + NOP padding, written while the code page is briefly writable.
    u8 bytes[8];
    const s32 rel32 = static_cast<s32>(static_cast<s64>(reinterpret_cast<VAddr>(tramp)) -
                                       static_cast<s64>(site + 5));
    bytes[0] = 0xE9;
    std::memcpy(bytes + 1, &rel32, 4);
    for (int i = 5; i < e.length; ++i) {
        bytes[i] = 0x90;
    }

    const VAddr page_begin = AlignDown(site, kHostPage);
    const VAddr page_end = AlignUp(site + e.length, kHostPage);
    void* pages = reinterpret_cast<void*>(page_begin);
    mprotect(pages, page_end - page_begin, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(reinterpret_cast<void*>(site), bytes, e.length);
    mprotect(pages, page_end - page_begin, PROT_READ | PROT_EXEC);

    ++g_patched_sites;
    return true;
}

void Describe(const char* what, u64 rip, u64 fault, u64 rsp) {
    const std::string where = g_describer ? g_describer(rip) : std::string();
    DC_ERROR("%s at rip %#llx (%s), fault address %#llx", what, rip, where.c_str(), fault);
    // Host frames (our code and system libraries).
    void* frames[32];
    const int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    if (!g_describer) {
        return;
    }
    // Guest code is usually built without frame pointers, so scan the stack
    // for values pointing into module images: likely return addresses.
    const u64* stack = reinterpret_cast<const u64*>(rsp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        const std::string s = g_describer(stack[i]);
        if (s.find('+') != std::string::npos) {
            DC_ERROR("  stack[%3d] %s", i, s.c_str());
            ++shown;
        }
    }
}

// Prints printable ASCII runs found on the stack: the game formats its fatal
// error messages into a stack buffer right before breaking.
void DumpStackStrings(u64 rsp) {
    const char* p = reinterpret_cast<const char*>(rsp);
    int run = 0;
    for (int i = 0; i < 0x3000; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t') {
            ++run;
            continue;
        }
        if (c == 0 && run >= 12) {
            DC_ERROR("  message: %.*s", run, p + i - run);
        }
        run = 0;
    }
}

[[noreturn]] void Die(int sig) {
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

std::atomic<FaultHandler> g_fault_handler{nullptr};

bool WatchFault(ucontext_t* uc, u64 fault); // DC_PROBE_WATCH, below

void Handler(int sig, siginfo_t* info, void* raw) {
    if (sig != SIGILL) {
        if (WatchFault(static_cast<ucontext_t*>(raw), reinterpret_cast<u64>(info->si_addr))) {
            return;
        }
        const FaultHandler fault = g_fault_handler.load(std::memory_order_acquire);
        if (fault && fault(raw, info->si_addr)) {
            return;
        }
    }
    auto* uc = static_cast<ucontext_t*>(raw);
    const u64 rip = uc->uc_mcontext->__ss.__rip;
    const u64 rsp = uc->uc_mcontext->__ss.__rsp;

    if (sig == SIGILL) {
        std::scoped_lock lk(g_patch_lock);
        const u8* code = reinterpret_cast<const u8*>(rip);
        // Another thread may have patched this site while we were faulting.
        if (code[0] == 0xE9 || PatchExtrq(rip)) {
            return; // re-executes the now-patched site
        }
        Describe("illegal instruction", rip, rip, rsp);
        DC_ERROR("  bytes: %02x %02x %02x %02x %02x %02x %02x %02x", code[0], code[1], code[2],
                 code[3], code[4], code[5], code[6], code[7]);
        Die(sig);
    }

    // `int 0x41` is the PS4 debugger break the game uses on fatal errors.
    const u8* code = reinterpret_cast<const u8*>(rip);
    if (sig == SIGSEGV && code[0] == 0xCD && code[1] == 0x41) {
        Describe("guest debug break (int 0x41)", rip, 0, rsp);
        // Driveclub's fatal handler formats into [caller rsp + 0x30].
        DC_ERROR("  fatal message: %.512s", reinterpret_cast<const char*>(rsp + 8 + 0x30));
        DumpStackStrings(rsp);
        Die(sig);
    }

    Describe(sig == SIGSEGV ? "segmentation fault" : "bus error", rip,
             reinterpret_cast<u64>(info->si_addr), rsp);
    Die(sig);
}

struct Probe {
    std::atomic<u64> addr{0};
    std::atomic<u64> hits{0};
    std::atomic<u64> rdi{0};
    std::atomic<u64> rdis[8]{}; // distinct rdi values seen
    std::atomic<u64> rets[8]{}; // distinct return addresses (callers)
    std::atomic<u64> rsis[8]{}; // distinct rsi values (second argument)
};

void NoteDistinct(std::atomic<u64> (&slots)[8], u64 v) {
    for (auto& slot : slots) {
        u64 cur = slot.load(std::memory_order_relaxed);
        if (cur == v || (!cur && slot.compare_exchange_strong(cur, v))) {
            return;
        }
    }
}

std::atomic<u64> g_watch_lo{0}, g_watch_hi{0};
thread_local bool t_watch_step = false;
void WatchProtect();
std::mutex g_watch_lock;
std::vector<u64> g_watch_sites;

bool WatchFault(ucontext_t* uc, u64 fault) {
    const u64 lo = g_watch_lo.load(std::memory_order_acquire);
    if (!lo || fault < (lo & ~0xFFFull) || fault >= ((g_watch_hi.load() + 0xFFF) & ~0xFFFull)) {
        return false;
    }
    if (fault >= lo && fault < g_watch_hi.load()) {
        std::scoped_lock lk(g_watch_lock);
        const u64 rip = uc->uc_mcontext->__ss.__rip;
        if (std::find(g_watch_sites.begin(), g_watch_sites.end(), rip) == g_watch_sites.end()) {
            g_watch_sites.push_back(rip);
            const auto& ss = uc->uc_mcontext->__ss;
            DC_INFO("watch: new site %s addr %#llx rdi %#llx rsi %#llx rdx %#llx rbx %#llx r12 %#llx r14 %#llx ret %s",
                    g_describer ? g_describer(rip).c_str() : "?", fault, ss.__rdi, ss.__rsi, ss.__rdx,
                    ss.__rbx, ss.__r12, ss.__r14,
                    g_describer ? g_describer(*reinterpret_cast<u64*>(ss.__rsp)).c_str() : "?");
        }
    }
    const u64 page = lo & ~0xFFFull;
    mprotect(reinterpret_cast<void*>(page), ((g_watch_hi.load() + 0xFFF) & ~0xFFFull) - page, PROT_READ | PROT_WRITE);
    uc->uc_mcontext->__ss.__rflags |= 0x100; // single-step: protect again right after
    t_watch_step = true;
    return true;
}
Probe g_probes[16];
std::atomic<u32> g_probe_count{0};

void TrapHandler(int sig, siginfo_t*, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    auto& ss = uc->uc_mcontext->__ss;
    if (t_watch_step) {
        t_watch_step = false;
        ss.__rflags &= ~0x100ull;
        WatchProtect();
        return;
    }
    const u32 n = g_probe_count.load(std::memory_order_acquire);
    for (u32 k = 0; k < n; ++k) {
        if (g_probes[k].addr.load(std::memory_order_relaxed) == ss.__rip - 1) {
            g_probes[k].hits.fetch_add(1, std::memory_order_relaxed);
            g_probes[k].rdi.store(ss.__rdi, std::memory_order_relaxed);
            NoteDistinct(g_probes[k].rdis, ss.__rdi);
            NoteDistinct(g_probes[k].rets, *reinterpret_cast<u64*>(ss.__rsp));
            NoteDistinct(g_probes[k].rsis, ss.__rsi);
            ss.__rsp -= 8; // the push rbp the int3 replaced
            *reinterpret_cast<u64*>(ss.__rsp) = ss.__rbp;
            return;
        }
    }
    Describe("trap", ss.__rip, 0, ss.__rsp);
    Die(sig);
}

} // namespace

bool AddProbe(VAddr function) {
    auto* code = reinterpret_cast<u8*>(function);
    const u32 k = g_probe_count.load();
    if (k >= std::size(g_probes) || code[0] != 0x55) {
        return false;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_sigaction = TrapHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTRAP, &sa, nullptr);
    });
    g_probes[k].addr = function;
    g_probe_count.store(k + 1, std::memory_order_release);
    const VAddr page = function & ~0xFFFull;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    code[0] = 0xCC;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_EXEC);
    return true;
}

std::vector<u64> ProbeArgs() {
    std::vector<u64> v;
    for (u32 k = 0; k < g_probe_count.load(); ++k) {
        v.push_back(g_probes[k].rdi.load());
    }
    return v;
}

std::vector<u64> ProbeDistinctArgs(u32 probe, bool second) {
    std::vector<u64> v;
    if (probe < g_probe_count.load()) {
        for (const auto& slot : second ? g_probes[probe].rsis : g_probes[probe].rdis) {
            if (const u64 x = slot.load()) {
                v.push_back(x);
            }
        }
    }
    return v;
}

void WatchAccess(VAddr addr, u64 size) {
    struct sigaction sa{};
    sa.sa_sigaction = TrapHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, nullptr);
    g_watch_hi = addr + size;
    g_watch_lo.store(addr, std::memory_order_release);
    WatchRearm();
}

void WatchRearm() {
    WatchProtect();
}

namespace {
void WatchProtect() {
    const u64 lo = g_watch_lo.load();
    if (lo) {
        const u64 page = lo & ~0xFFFull;
        const int r = mprotect(reinterpret_cast<void*>(page), ((g_watch_hi.load() + 0xFFF) & ~0xFFFull) - page, PROT_NONE);
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            DC_INFO("watch: mprotect(%#llx) -> %d (errno %d)", page, r, r ? errno : 0);
        }
    }
}
} // namespace

std::vector<u64> WatchSites() {
    std::scoped_lock lk(g_watch_lock);
    return g_watch_sites;
}

std::vector<u64> ProbeCallers(u32 probe) {
    std::vector<u64> v;
    if (probe < g_probe_count.load()) {
        for (const auto& slot : g_probes[probe].rets) {
            if (const u64 x = slot.load()) {
                v.push_back(x);
            }
        }
    }
    return v;
}

std::vector<u64> ProbeHits() {
    std::vector<u64> v;
    for (u32 k = 0; k < g_probe_count.load(); ++k) {
        v.push_back(g_probes[k].hits.load());
    }
    return v;
}

void Install(AddressDescriber describer) {
    g_describer = describer;
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void SetFaultHandler(FaultHandler handler) {
    g_fault_handler.store(handler, std::memory_order_release);
}

void AddPatchArea(VAddr code_begin, VAddr code_end, VAddr area, u64 area_size) {
    std::scoped_lock lk(g_patch_lock);
    g_areas.push_back({code_begin, code_end, area, area + area_size});
}

} // namespace Core::Signals
