#include "core/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <pthread.h>
#include <sys/mman.h>

#include "common/log.h"
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
void RelocateFixedHeap(const Loader::Module& exe) {
    constexpr u64 kOld = 0x1000000000, kNew = 0x9000000000;
    const char* env = std::getenv("DC_HEAP_SITES");
    const std::string sites = env ? env : "748d85,748d8f,749247,74925a,5fa001";
    size_t pos = 0;
    while (pos < sites.size()) {
        const size_t comma = std::min(sites.find(',', pos), sites.size());
        const VAddr site = exe.Base() + std::strtoull(sites.substr(pos, comma - pos).c_str(), nullptr, 16);
        pos = comma + 1;
        auto* code = reinterpret_cast<u8*>(site);
        u64 imm;
        std::memcpy(&imm, code + 2, 8);
        if ((code[0] & 0xFE) != 0x48 || (code[1] & 0xF8) != 0xB8 || imm != kOld) {
            DC_WARN("heap remap: no movabs %#llx at %#llx", kOld, site);
            continue;
        }
        const VAddr page = site & ~0xFFFull;
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        std::memcpy(code + 2, &kNew, 8);
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_EXEC);
        DC_INFO("heap remap: %#llx -> %#llx at %#llx", kOld, kNew, site);
    }
}

void* GuestMain(void*) {
    Tls::SetupThread();
    RelocateFixedHeap(*Loader::Linker::Instance()->Executable());

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

} // namespace Core::Runtime
