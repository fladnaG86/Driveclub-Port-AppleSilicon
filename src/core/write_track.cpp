#include "core/write_track.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdlib>

#include "common/log.h"
#include "core/memory.h"
#include "core/signals.h"

namespace Core::WriteTrack {
namespace {

// Open-addressed page table, read by the signal handler: lock free, slots are
// never removed (a page stays known once watched).
constexpr u32 kSlots = 1u << 20;
struct Slot {
    std::atomic<u64> page{0}; // page number + 1, 0 = empty
    std::atomic<u32> writes{0};
    std::atomic<u8> armed{0};
    std::atomic<int> host_prot{0}; // protection to restore on a write
};
Slot* g_slots = nullptr;
bool g_enabled = false;

u32 HashPage(u64 page) {
    return static_cast<u32>((page * 0x9E3779B97F4A7C15ull) >> 44) & (kSlots - 1);
}

Slot* Find(u64 page) {
    for (u32 i = HashPage(page), n = 0; n < 64; i = (i + 1) & (kSlots - 1), ++n) {
        const u64 k = g_slots[i].page.load(std::memory_order_acquire);
        if (k == page + 1) {
            return &g_slots[i];
        }
        if (k == 0) {
            return nullptr;
        }
    }
    return nullptr;
}

Slot* FindOrInsert(u64 page) {
    for (u32 i = HashPage(page), n = 0; n < 64; i = (i + 1) & (kSlots - 1), ++n) {
        u64 k = g_slots[i].page.load(std::memory_order_acquire);
        if (k == 0 && g_slots[i].page.compare_exchange_strong(k, page + 1, std::memory_order_acq_rel)) {
            return &g_slots[i];
        }
        if (k == page + 1) {
            return &g_slots[i];
        }
    }
    return nullptr;
}

// Counts a write and leaves the page writable. Returns true if it was armed.
bool Disarm(Slot* s, u64 page, bool unprotect) {
    if (!s->armed.exchange(0, std::memory_order_acq_rel)) {
        return false;
    }
    s->writes.fetch_add(1, std::memory_order_release);
    if (unprotect) {
        mprotect(reinterpret_cast<void*>(page << kPageShift), u64(1) << kPageShift,
                 s->host_prot.load(std::memory_order_relaxed));
    }
    return true;
}

bool OnFault(void*, void* fault_address) {
    const u64 page = reinterpret_cast<u64>(fault_address) >> kPageShift;
    Slot* s = Find(page);
    if (!s) {
        return false;
    }
    // Armed: ours. Already disarmed: another thread got here first and the
    // page is writable again, so retrying the access succeeds.
    Disarm(s, page, true);
    return true;
}

template <typename F> void ForEachPage(VAddr addr, u64 size, F&& fn) {
    if (!size) {
        return;
    }
    for (u64 p = addr >> kPageShift, last = (addr + size - 1) >> kPageShift; p <= last; ++p) {
        fn(p);
    }
}

} // namespace

void Init() {
    if (std::getenv("DC_NO_WRITE_TRACK")) {
        return;
    }
    g_slots = new Slot[kSlots];
    g_enabled = true;
    Core::Signals::SetFaultHandler(OnFault);
}

bool Watch(VAddr addr, u64 size) {
    if (!g_enabled || !size) {
        return false;
    }
    Core::Memory::Vma vma;
    if (!Core::Memory::Query(addr, false, &vma) || addr + size > vma.start + vma.size ||
        (vma.type != Core::Memory::VmaType::Direct && vma.type != Core::Memory::VmaType::Flexible)) {
        return false;
    }
    const int host = Core::Memory::HostProtection(vma.prot);
    if (!(host & PROT_WRITE)) {
        return false;
    }
    bool ok = true;
    u64 run_start = 0, run_len = 0; // contiguous pages to protect in one call
    auto flush = [&] {
        if (run_len) {
            mprotect(reinterpret_cast<void*>(run_start << kPageShift), run_len << kPageShift,
                     host & ~PROT_WRITE);
            run_len = 0;
        }
    };
    ForEachPage(addr, size, [&](u64 p) {
        Slot* s = FindOrInsert(p);
        if (!s) {
            ok = false;
            return;
        }
        s->host_prot.store(host, std::memory_order_relaxed);
        if (s->armed.load(std::memory_order_acquire)) {
            return;
        }
        // Armed before the protection: a write racing in faults (counted) or
        // lands before mprotect and is seen by the caller's read that follows.
        s->armed.store(1, std::memory_order_release);
        if (run_len && run_start + run_len == p) {
            ++run_len;
        } else {
            flush();
            run_start = p;
            run_len = 1;
        }
    });
    flush();
    return ok;
}

std::optional<u64> Version(VAddr addr, u64 size) {
    if (!g_enabled) {
        return std::nullopt;
    }
    u64 sum = 0;
    bool ok = true;
    ForEachPage(addr, size, [&](u64 p) {
        const Slot* s = ok ? Find(p) : nullptr;
        if (!s) {
            ok = false;
            return;
        }
        sum += s->writes.load(std::memory_order_acquire);
    });
    return ok ? std::optional<u64>(sum) : std::nullopt;
}

void BeforeHostWrite(VAddr addr, u64 size) {
    if (!g_enabled) {
        return;
    }
    ForEachPage(addr, size, [&](u64 p) {
        if (Slot* s = Find(p)) {
            Disarm(s, p, true);
        }
    });
}

void Invalidate(VAddr addr, u64 size) {
    if (!g_enabled) {
        return;
    }
    ForEachPage(addr, size, [&](u64 p) {
        if (Slot* s = Find(p)) {
            // The new mapping or protection replaces ours: no mprotect back.
            if (!Disarm(s, p, false)) {
                s->writes.fetch_add(1, std::memory_order_release);
            }
        }
    });
}

} // namespace Core::WriteTrack
