// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/system_service/system_service.h"

#include <atomic>
#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::SystemService {

namespace {

constexpr s32 ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT = static_cast<s32>(0x80A10004);

s32 sceSystemServiceGetStatus(OrbisSystemServiceStatus* status) {
    if (status) {
        std::memset(status, 0, sizeof(*status));
        status->event_num = 0;
        status->is_system_ui_overlaid = false;
        status->is_in_background_execution = false;
        status->is_cpu_mode7_cpu_normal = true;
        status->is_game_live_streaming_on_air = false;
        status->is_out_of_vr_play_area = false;
    }
    return 0;
}

s32 sceSystemServiceReceiveEvent(OrbisSystemServiceEvent* event) {
    return ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT;
}

s32 sceSystemServiceGetDisplaySafeAreaInfo(OrbisSystemServiceDisplaySafeAreaInfo* info) {
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->ratio = 1.0f;
    }
    return 0;
}

s32 sceSystemServiceHideSplashScreen() {
    return 0;
}

// A Western console: 0 for LANG would be Japanese and 0 for ENTER_BUTTON_ASSIGN
// makes Circle the confirm button (Cross presses are then ignored).
s32 sceSystemServiceParamGetInt(s32 paramId, s32* value) {
    s32 v = 0;
    switch (paramId) {
    case 1: v = 1; break;    // LANG: English (US)
    case 2: v = 1; break;    // DATE_FORMAT: DD/MM/YYYY
    case 3: v = 1; break;    // TIME_FORMAT: 24 hours
    case 4: v = 60; break;   // TIME_ZONE: minutes from UTC
    case 1000: v = 1; break; // ENTER_BUTTON_ASSIGN: Cross
    default: break;          // SUMMERTIME, GAME_PARENTAL_LEVEL, ...: 0
    }
    static std::atomic<u64> seen{0};
    const u64 bit = 1ull << (paramId == 1000 ? 63 : paramId & 62);
    if (!(seen.fetch_or(bit) & bit)) {
        DC_INFO("sceSystemServiceParamGetInt(%d) -> %d", paramId, v);
    }
    if (value) {
        *value = v;
    }
    return 0;
}

} // namespace

void RegisterSystemService() {
    constexpr const char* lib = "libSceSystemService";
    DC_HLE(lib, sceSystemServiceGetStatus);
    DC_HLE(lib, sceSystemServiceReceiveEvent);
    DC_HLE(lib, sceSystemServiceGetDisplaySafeAreaInfo);
    DC_HLE(lib, sceSystemServiceHideSplashScreen);
    DC_HLE(lib, sceSystemServiceParamGetInt);
}

} // namespace Hle::SystemService
