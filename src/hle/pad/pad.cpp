// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "hle/pad/pad.h"

#include <atomic>
#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Pad {

namespace {

constexpr s32 kPadHandle = 1;
constexpr s32 kErrorInvalidPortType = -2137980926; // 0x80920002
constexpr s32 kErrorInvalidHandle = static_cast<s32>(0x80920003);

s32 scePadInit() {
    DC_INFO("scePadInit()");
    return 0;
}

s32 scePadOpen(s32 userId, s32 type, s32 index, const void* param) {
    if (type != 0) {
        return kErrorInvalidPortType;
    }
    DC_INFO("scePadOpen(userId=%#x, type=%d, index=%d) -> handle %d", userId, type, index, kPadHandle);
    return kPadHandle;
}

s32 scePadOpenExt(s32 userId, s32 type, s32 index, const void* param) {
    if (type != 0) {
        return kErrorInvalidPortType;
    }
    DC_INFO("scePadOpenExt(userId=%#x, type=%d, index=%d) -> handle %d", userId, type, index, kPadHandle);
    return kPadHandle;
}

s32 scePadClose(s32 handle) {
    DC_INFO("scePadClose(handle=%d)", handle);
    return 0;
}

static std::atomic<u32> g_buttons{0};
static std::atomic<u8> g_left_x{128};
static std::atomic<u8> g_left_y{128};
static std::atomic<u8> g_right_x{128};
static std::atomic<u8> g_right_y{128};
static std::atomic<u8> g_l2{0};
static std::atomic<u8> g_r2{0};

// Microseconds, like sceKernelGetProcessTime: games skip samples whose time
// did not move.
static u64 PadTimestamp() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - t0).count();
}

// Only the handle scePadOpen gave out is a pad: logiWheel.prx probes with -1
// and, served data, reports a wheel the game then gives player 1.
s32 scePadReadState(s32 handle, OrbisPadData* data) {
    if (handle != kPadHandle) {
        return kErrorInvalidHandle;
    }
    if (data) {
        std::memset(data, 0, sizeof(*data));
        data->buttons = g_buttons.load();
        data->leftStick = {g_left_x.load(), g_left_y.load()};
        data->rightStick = {g_right_x.load(), g_right_y.load()};
        data->analogButtons.l2 = g_l2.load();
        data->analogButtons.r2 = g_r2.load();
        data->orientation[3] = 1.0f;
        data->acceleration[2] = 9.8f;
        data->connected = true;
        data->connectedCount = 1;
        data->timestamp = PadTimestamp();
    }
    return 0;
}

s32 scePadRead(s32 handle, OrbisPadData* data, s32 count) {
    if (handle != kPadHandle) {
        return kErrorInvalidHandle;
    }
    if (data && count > 0) {
        for (s32 i = 0; i < count; ++i) {
            std::memset(&data[i], 0, sizeof(OrbisPadData));
            data[i].buttons = g_buttons.load();
            data[i].leftStick = {g_left_x.load(), g_left_y.load()};
            data[i].rightStick = {g_right_x.load(), g_right_y.load()};
            data[i].analogButtons.l2 = g_l2.load();
            data[i].analogButtons.r2 = g_r2.load();
            data[i].orientation[3] = 1.0f;
            data[i].acceleration[2] = 9.8f;
            data[i].connected = true;
            data[i].connectedCount = 1;
            data[i].timestamp = PadTimestamp();
        }
    }
    return count;
}

s32 scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* info) {
    if (handle != kPadHandle) {
        return kErrorInvalidHandle;
    }
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->touchPadInfo.pixelDensity = 1.0f;
        info->touchPadInfo.resX = 1920;
        info->touchPadInfo.resY = 942;
        info->connected = true;
        info->connectedCount = 1;
    }
    return 0;
}

s32 scePadGetExtControllerInformation(s32 handle, void* pInfo) {
    return scePadGetControllerInformation(handle, reinterpret_cast<OrbisPadControllerInformation*>(pInfo));
}

s32 scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    return 0;
}

s32 scePadSetVibration(s32 handle, const void* param) {
    return 0;
}

s32 scePadResetOrientation(s32 handle) {
    return 0;
}

} // namespace

void SetButton(u32 button, bool pressed) {
    if (pressed) {
        g_buttons.fetch_or(button);
    } else {
        g_buttons.fetch_and(~button);
    }
}

void SetStick(bool right, u8 x, u8 y) {
    if (right) {
        g_right_x.store(x);
        g_right_y.store(y);
    } else {
        g_left_x.store(x);
        g_left_y.store(y);
    }
}

void SetStickAxis(bool right, bool y, u8 value) {
    (right ? (y ? g_right_y : g_right_x) : (y ? g_left_y : g_left_x)).store(value);
}

void SetTrigger(bool right, u8 value) {
    (right ? g_r2 : g_l2).store(value);
    SetButton(right ? kR2 : kL2, value > 30);
}

// DC_AUTOPRESS=<sec>,<sec>,...: presses Cross at those times after start
// (unattended runs that need to get past "Start Event").
static void StartAutoPress() {
    const char* e = std::getenv("DC_AUTOPRESS");
    if (!e) {
        return;
    }
    std::vector<double> times;
    for (const char* p = e; *p;) {
        char* q = nullptr;
        times.push_back(std::strtod(p, &q));
        p = *q == ',' ? q + 1 : q;
        if (q == p && *p) break;
    }
    // DC_AUTOPRESS_BUTTON=<hex mask> (default Cross), DC_AUTOPRESS_HOLD=<ms>.
    const char* be = std::getenv("DC_AUTOPRESS_BUTTON");
    const u32 button = be ? static_cast<u32>(std::strtoul(be, nullptr, 16)) : 0x4000;
    const char* he = std::getenv("DC_AUTOPRESS_HOLD");
    const s64 hold = he ? std::strtoll(he, nullptr, 10) : 250;
    std::thread([times, button, hold] {
        const auto t0 = std::chrono::steady_clock::now();
        for (double t : times) {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(static_cast<s64>(t * 1000)));
            DC_INFO("pad: auto-press %#x at %.1f s", button, t);
            SetButton(button, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
            SetButton(button, false);
        }
    }).detach();
}

// DC_AUTO_THROTTLE=<sec>,<ms>: full R2 for <ms> from <sec> after start.
static void StartAutoThrottle() {
    const char* e = std::getenv("DC_AUTO_THROTTLE");
    if (!e) {
        return;
    }
    double at = 0;
    s64 ms = 0;
    std::sscanf(e, "%lf,%lld", &at, &ms);
    std::thread([at, ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<s64>(at * 1000)));
        DC_INFO("pad: auto-throttle for %lld ms", ms);
        SetTrigger(true, 255);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        SetTrigger(true, 0);
    }).detach();
}

void RegisterPad() {
    StartAutoPress();
    StartAutoThrottle();
    constexpr const char* lib = "libScePad";
    DC_HLE(lib, scePadInit);
    DC_HLE(lib, scePadOpen);
    DC_HLE(lib, scePadOpenExt);
    DC_HLE(lib, scePadClose);
    DC_HLE(lib, scePadReadState);
    DC_HLE(lib, scePadRead);
    DC_HLE(lib, scePadGetControllerInformation);
    DC_HLE(lib, scePadGetExtControllerInformation);
    DC_HLE(lib, scePadSetTiltCorrectionState);
    DC_HLE(lib, scePadSetVibration);
    DC_HLE(lib, scePadResetOrientation);
}

} // namespace Hle::Pad
