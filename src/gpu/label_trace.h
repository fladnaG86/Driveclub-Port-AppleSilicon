#pragma once

// DC_LABEL_TRACE=<label addr hex>,<start s>,<duration s>: logs a timeline
// ("lt <us> ...") of guest submits, CP jobs, command buffers and writes of one
// label, to see where a frame waits. Times are CLOCK_UPTIME_RAW.

#include <cstdlib>
#include <ctime>

#include <mach/mach.h>

#include "common/types.h"

namespace Gpu {

inline u64 LtNow() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

struct LabelTrace {
    u64 addr = 0;
    u64 t0 = 0, t1 = 0;
};

inline const LabelTrace& Lt() {
    static const LabelTrace lt = [] {
        LabelTrace l;
        const char* v = std::getenv("DC_LABEL_TRACE");
        if (!v) {
            return l;
        }
        char* end = nullptr;
        l.addr = std::strtoull(v, &end, 16);
        double start = 60, dur = 5;
        if (end && *end == ',') {
            start = std::strtod(end + 1, &end);
            if (end && *end == ',') {
                dur = std::strtod(end + 1, &end);
            }
        }
        l.t0 = LtNow() + static_cast<u64>(start * 1e9);
        l.t1 = l.t0 + static_cast<u64>(dur * 1e9);
        return l;
    }();
    return lt;
}

inline bool LtOn() {
    const LabelTrace& l = Lt();
    if (!l.t1) {
        return false;
    }
    const u64 n = LtNow();
    return n >= l.t0 && n < l.t1;
}

// Microseconds since the trace window opened.
inline double LtUs(u64 ns) {
    return (static_cast<double>(ns) - static_cast<double>(Lt().t0)) / 1e3;
}

// User and system CPU time of the calling thread, microseconds.
inline void LtThreadCpu(double* user_us, double* sys_us) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    *user_us = *sys_us = 0;
    if (thread_info(self, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS) {
        *user_us = info.user_time.seconds * 1e6 + info.user_time.microseconds;
        *sys_us = info.system_time.seconds * 1e6 + info.system_time.microseconds;
    }
    mach_port_deallocate(mach_task_self(), self);
}

// Time the calling thread spent blocked on the renderer lock (trace only).
inline thread_local u64 lt_lock_ns = 0;

template <typename M>
struct LtLock {
    M& m;
    explicit LtLock(M& mu) : m(mu) {
        if (m.try_lock()) {
            return;
        }
        const u64 t = LtNow();
        m.lock();
        lt_lock_ns += LtNow() - t;
    }
    ~LtLock() { m.unlock(); }
    LtLock(const LtLock&) = delete;
    LtLock& operator=(const LtLock&) = delete;
};

} // namespace Gpu
