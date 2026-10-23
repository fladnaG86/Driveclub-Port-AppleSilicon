// Benchmark mode, ported from driveclub-recomp (src/runtime_benchmark.c,
// GPL-2.0-or-later, https://github.com/OverkillLabs3/driveclub-recomp).
//
// The game ships a Lua autotest framework and runs the script given on its
// command line as -luaautotest=<path>. We write a small script of our own to
// <user>/benchmark (served over /app0), pass the argument, and the script
// skips the intro, starts the event with Automation.Go, skips the pre-race
// cameras and hands the car to the AI. It reports progress by opening
// "bb_benchmark/<name>" (no such file: Marker() sees the path, the open fails
// and the game carries on). After "racing", warmup seconds are skipped, then
// each flip interval is kept for `seconds`; the summary is printed and the
// process exits (0 = done, 3 = the race did not start or frames stopped).
//
// DC_BENCHMARK=1, or key=value pairs separated by ',' (values: letters,
// digits, '_'; they go into Lua source):
//   track    world name in newdata/tracks.csv (default uk_road_circuit_02_n)
//   car      vehicle name in newdata/vehicles.csv (default fe_458)
//   mode     TimeTrial (default) or Race (7 AI cars)
//   tod      hour 0-23 (default 12)   weather Clear, Cloudy, Overcast, Stormy
//   rain     empty = dry; Heavy, Mid, Light, VeryLight, Wet, Damp, DryToWet,
//            or a game preset name with '_' for spaces
//   laps 1-99 (5)   drive ai|none (ai)   view chase|driverhead|attach
//   warmup   s not measured after the race starts (default 10)
//   seconds  s measured (default 60)
//   timeout  s from start until the race must run (default 240)
// Result line: "benchmark: result ... avg_fps= low1_fps= low01_fps= p99_ms=
// max_ms= cpu_pct= load_s=" (cpu_pct: process CPU over the measured window,
// 100 = one core).

#include "core/benchmark.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "common/log.h"
#include "core/loader/linker.h"

namespace Core::Benchmark {

namespace {

namespace fs = std::filesystem;

constexpr const char* kGuestArg = "-luaautotest=scripts/autotest/lua/bb_benchmark.lua";
constexpr std::string_view kMarkerPrefix = "/app0/bb_benchmark/";

struct Config {
    std::string track = "uk_road_circuit_02_n", car = "fe_458", mode = "TimeTrial",
                weather = "Clear", view, drive = "ai", rain;
    int tod = 12, laps = 5, warmup = 10, seconds = 60, timeout = 240;
} cfg;

enum Phase { kOff, kWaiting, kWarmup, kMeasuring, kDone };
std::atomic<int> g_phase{kOff};
fs::path g_overlay;
double g_t_config, g_t_race, g_t_measure, g_t_last_flip, g_cpu_measure;
std::vector<double> g_intervals;
std::mutex g_flip_lock;

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

double ProcessCpuMs() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 +
           (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
}

bool SetWord(std::string& dst, std::string_view v) {
    if (v.empty() || v.size() > 47) return false;
    for (char c : v) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    dst = v;
    return true;
}

bool SetInt(int& dst, std::string_view v, int lo, int hi) {
    if (v.empty() || v.size() > 6) return false;
    const std::string s(v);
    char* end = nullptr;
    const long x = std::strtol(s.c_str(), &end, 10);
    if (*end || x < lo || x > hi) return false;
    dst = static_cast<int>(x);
    return true;
}

// Short names, or the game's preset name with '_' for each space.
bool SetRain(std::string_view v) {
    static constexpr std::pair<const char*, const char*> kAlias[] = {
        {"Heavy", "Heavy Precipitation"}, {"Mid", "Mid Precipitation"},
        {"Light", "Light Precipitation"}, {"VeryLight", "Very Light Precipitation"},
        {"Wet", "Wet Track"},             {"Damp", "Damp Track"},
        {"DryToWet", "Dry To Wet"}};
    if (v.empty()) {
        cfg.rain.clear();
        return true;
    }
    if (!SetWord(cfg.rain, v)) return false;
    for (auto [name, preset] : kAlias) {
        if (cfg.rain == name) {
            cfg.rain = preset;
            return true;
        }
    }
    std::replace(cfg.rain.begin(), cfg.rain.end(), '_', ' ');
    return true;
}

bool Parse(std::string_view s) {
    if (s == "1" || s == "on") return true;
    while (!s.empty()) {
        const size_t end = std::min(s.find(','), s.size());
        const std::string_view item = s.substr(0, end);
        s.remove_prefix(std::min(end + 1, s.size()));
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            DC_ERROR("benchmark: '%.*s' is not key=value", int(item.size()), item.data());
            return false;
        }
        const std::string_view k = item.substr(0, eq), v = item.substr(eq + 1);
        bool ok = false;
        if (k == "track") ok = SetWord(cfg.track, v);
        else if (k == "car") ok = SetWord(cfg.car, v);
        else if (k == "mode") ok = SetWord(cfg.mode, v);
        else if (k == "weather") ok = SetWord(cfg.weather, v);
        else if (k == "rain") ok = SetRain(v);
        else if (k == "view") ok = v.empty() ? (cfg.view.clear(), true) : SetWord(cfg.view, v);
        else if (k == "drive") ok = (v == "ai" || v == "none") && SetWord(cfg.drive, v);
        else if (k == "tod") ok = SetInt(cfg.tod, v, 0, 23);
        else if (k == "laps") ok = SetInt(cfg.laps, v, 1, 99);
        else if (k == "warmup") ok = SetInt(cfg.warmup, v, 0, 600);
        else if (k == "seconds") ok = SetInt(cfg.seconds, v, 1, 3600);
        else if (k == "timeout") ok = SetInt(cfg.timeout, v, 10, 3600);
        if (!ok) {
            DC_ERROR("benchmark: bad setting '%.*s'", int(item.size()), item.data());
            return false;
        }
    }
    return true;
}

// The test script, from driveclub-recomp. It runs as a coroutine of the
// game's autotest component; the AutoTest:Wait* helpers yield once per frame.
constexpr char kScript[] =
    "-- Written by Driveclub Native for DC_BENCHMARK (src/core/benchmark.cpp). Not game data.\n"
    "require(\"class\")\n"
    "require(\"autotesthelpers\")\n"
    "require(\"driveclub_testhelpers\")\n"
    "local track, car, mode, weather, view, rain = \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"\n"
    "local tod, laps, ai = %d, %d, %s\n"
    "local function mark(name) loadfile(\"bb_benchmark/\" .. name) end\n"
    "-- The game's print does not reach the log: messages go through a marker open.\n"
    "local function say(text)\n"
    "  text = tostring(text)\n"
    "  mark(\"say:\" .. (string.gsub(string.sub(text, 1, 400), \"[^%%w%%s%%.:=_,%%-%%(%%)%%[%%]]\", \"?\")))\n"
    "end\n"
    "say(\"script started\")\n"
    "-- Intro: skip the video, confirm the title page.\n"
    "DriveClubTestHelpers:AddAutoPageResponse(function(page)\n"
    "  say(\"page \" .. tostring(page))\n"
    "  if page == \"boot.introvideo\" then Event.SendEvent(global, \"GUI\", \"SkipVideo\") end\n"
    "  if page == \"boot.start\" then Event.SendEvent(global, \"GUI\", \"BOOT_SEQ_DONE\") end\n"
    "end)\n"
    "AutoTest:WaitForEvent(global, \"IsFrontEnd\")\n"
    "mark(\"frontend\")\n"
    "AutoTest:WaitForSeconds(1)\n"
    "say(\"starting \" .. mode .. \" on \" .. track .. \" with \" .. car)\n"
    "-- Precipitation: set before and after Go, which may reset it.\n"
    "local function set_rain() if rain ~= \"\" then GameConfig.data.precipitationType = rain end end\n"
    "set_rain()\n"
    "if mode == \"Race\" then\n"
    "  Automation.Go(mode, track, \"sky_mainline_v2\", laps, false, tod, weather, 1, car, 2, false, 7,\n"
    "    \"he_ve_gt\", \"ruf_rgt\", \"mi_jcw\", \"vw_beetle_gsr\", \"fe_f12\", \"lo_ex\", \"ma_mc_st\")\n"
    "else\n"
    "  Automation.Go(mode, track, \"sky_mainline_v2\", laps, false, tod, weather, 1, car, 2, true, 0)\n"
    "end\n"
    "set_rain()\n"
    "if rain ~= \"\" then mark(\"setrain\") end\n"
    "say(\"precipitation \" .. tostring(GameConfig:GetPrecipitationType()))\n"
    "AutoTest:WaitForEvent(global, \"AnimLibPlaySqMsg\", function(data)\n"
    "  return data == \"prerace\" or data == \"prerace_night\"\n"
    "end)\n"
    "-- The engine applies precipitation when these events fire.\n"
    "if rain ~= \"\" then\n"
    "  set_rain()\n"
    "  mark(\"setrain\")\n"
    "  Event.SendEvent(global, \"SetPrecipitationFromGameConfig\")\n"
    "  Event.SendEvent(global, \"SetInitialPrecipitationState\")\n"
    "end\n"
    "mark(\"loaded\")\n"
    "AutoTest:WaitForSeconds(2)\n"
    "Event.SendEvent(global, \"Advance\")\n"
    "AutoTest:WaitForEvent(global, \"RaceState\", function(data) return data == \"Racing\" end)\n"
    "if ai then DriveClubTestHelpers:SwitchToAI() end\n"
    "if rain ~= \"\" then Event.SendEvent(global, \"StartDrivingPrecipitation\") end\n"
    "if view ~= \"\" then DriveClubTestHelpers:ChangeView(view) end\n"
    "mark(\"racing\")\n"
    "while true do\n"
    "  AutoTest:WaitForSeconds(5)\n"
    "  if ai then Actor.GetPlayer().UnderPlayerControl = false end\n"
    "end\n";

double SinceConfig(double t) {
    return (t - g_t_config) / 1000.0;
}

void Watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double t = NowMs();
        const int p = g_phase.load();
        if (p == kWaiting && t - g_t_config > cfg.timeout * 1000.0) {
            DC_ERROR("benchmark: FAILED, the race did not start within %d s", cfg.timeout);
            std::fflush(nullptr);
            _exit(3);
        }
        std::scoped_lock lk(g_flip_lock);
        if ((p == kWarmup || p == kMeasuring) && g_t_last_flip > 0 && t - g_t_last_flip > 30000.0) {
            DC_ERROR("benchmark: FAILED, no frame for 30 s");
            std::fflush(nullptr);
            _exit(3);
        }
    }
}

// The race config's precipitation preset is C++ data (GameConfig.data in Lua
// is a copy), so the script cannot set it: on the "setrain" marker, on the
// game's thread, assign it with the game's own std::string assign. eboot
// 1.28 offsets from driveclub-recomp (Ghidra):
//   G = *(image + 0x1911100); base = *(*(G + 0x970) + 0xf8)
//   config = base[0x780] == 0 ? base + 0x3b8 : base + 8
//   config + 0x208 weather, config + 0x230 precipitation (string objects)
//   image + 0xe978b0 assigns (object, const char*)
const char* GuestString(const u8* object) {
    const u64 cap = *reinterpret_cast<const u64*>(object + 0x20);
    return cap > 15 ? *reinterpret_cast<const char* const*>(object + 8)
                    : reinterpret_cast<const char*>(object + 8);
}

void SetGuestRain() {
    if (cfg.rain.empty()) return;
    u8* image = reinterpret_cast<u8*>(Loader::Linker::Instance()->Executable()->Base());
    const u8* g = *reinterpret_cast<const u8* const*>(image + 0x1911100);
    const u8* p = g ? *reinterpret_cast<const u8* const*>(g + 0x970) : nullptr;
    u8* base = p ? *reinterpret_cast<u8* const*>(p + 0xf8) : nullptr;
    if (!base) {
        DC_WARN("benchmark: rain: no race config yet");
        return;
    }
    u8* config = base[0x780] == 0 ? base + 0x3b8 : base + 8;
    const std::string before = GuestString(config + 0x230);
    using Assign = void* (*)(void* object, const char* text);
    reinterpret_cast<Assign>(image + 0xe978b0)(config + 0x230, cfg.rain.c_str());
    DC_INFO("benchmark: rain: weather '%.40s', precipitation '%.40s' -> '%.40s'",
            GuestString(config + 0x208), before.c_str(), GuestString(config + 0x230));
}

// Average of the slowest share of the frames, as frames per second.
double LowFps(const std::vector<double>& sorted_desc, double share) {
    const size_t k = std::clamp<size_t>(size_t(sorted_desc.size() * share), 1, sorted_desc.size());
    double sum = 0;
    for (size_t i = 0; i < k; ++i) sum += sorted_desc[i];
    return sum > 0 ? 1000.0 * k / sum : 0;
}

void Finish(double t) {
    const double elapsed = t - g_t_measure;
    const double cpu_pct = elapsed > 0 ? (ProcessCpuMs() - g_cpu_measure) / elapsed * 100 : 0;
    std::vector<double> v = g_intervals;
    std::sort(v.begin(), v.end(), std::greater<>());
    const size_t n = v.size();
    std::string rain = cfg.rain.empty() ? "dry" : cfg.rain;
    std::replace(rain.begin(), rain.end(), ' ', '_');
    DC_INFO("benchmark: result mode=%s track=%s car=%s tod=%d weather=%s rain=%s drive=%s "
            "seconds=%.1f frames=%zu avg_fps=%.2f low1_fps=%.2f low01_fps=%.2f p99_ms=%.2f "
            "max_ms=%.2f cpu_pct=%.0f load_s=%.1f",
            cfg.mode.c_str(), cfg.track.c_str(), cfg.car.c_str(), cfg.tod, cfg.weather.c_str(),
            rain.c_str(), cfg.drive.c_str(), elapsed / 1000.0, n,
            elapsed > 0 ? 1000.0 * n / elapsed : 0, n ? LowFps(v, 0.01) : 0,
            n ? LowFps(v, 0.001) : 0, n ? v[n / 100] : 0, n ? v[0] : 0, cpu_pct,
            SinceConfig(g_t_race));
    std::fflush(nullptr);
    _exit(0); // GPU, audio and guest threads are still running
}

} // namespace

void Configure(const fs::path& user_root) {
    const char* s = std::getenv("DC_BENCHMARK");
    if (!s || !*s || !std::strcmp(s, "0") || !std::strcmp(s, "off")) return;
    if (!Parse(s)) {
        DC_ERROR("benchmark: DC_BENCHMARK ignored");
        return;
    }
    const fs::path root = user_root / "benchmark";
    const fs::path dir = root / "scripts/autotest/lua";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // The game's script loader opens the compiled name; it takes source too.
    FILE* f = std::fopen((dir / "bb_benchmark.lua.bin").c_str(), "wb");
    if (!f) {
        DC_ERROR("benchmark: cannot write %s", dir.c_str());
        return;
    }
    std::fprintf(f, kScript, cfg.track.c_str(), cfg.car.c_str(), cfg.mode.c_str(),
                 cfg.weather.c_str(), cfg.view.c_str(), cfg.rain.c_str(), cfg.tod, cfg.laps,
                 cfg.drive == "ai" ? "true" : "false");
    std::fclose(f);
    g_overlay = root;
    g_intervals.reserve(size_t(cfg.seconds + 1) * 240);
    g_t_config = NowMs();
    g_phase = kWaiting;
    std::thread(Watchdog).detach();
    DC_INFO("benchmark: mode=%s track=%s car=%s tod=%d weather=%s rain=%s laps=%d drive=%s "
            "view=%s warmup=%d s, %d s measured",
            cfg.mode.c_str(), cfg.track.c_str(), cfg.car.c_str(), cfg.tod, cfg.weather.c_str(),
            cfg.rain.empty() ? "default" : cfg.rain.c_str(), cfg.laps, cfg.drive.c_str(),
            cfg.view.empty() ? "default" : cfg.view.c_str(), cfg.warmup, cfg.seconds);
}

const char* GuestArg() {
    return g_phase.load() == kOff ? nullptr : kGuestArg;
}

const fs::path& Overlay() {
    return g_overlay;
}

bool Marker(std::string_view guest) {
    if (g_phase.load() == kOff || !guest.starts_with(kMarkerPrefix)) return false;
    const std::string_view name = guest.substr(kMarkerPrefix.size());
    const double t = NowMs();
    if (name.starts_with("say:")) {
        DC_INFO("benchmark: [lua %.1f s] %.*s", SinceConfig(t), int(name.size() - 4),
                name.data() + 4);
        return true;
    }
    if (name == "setrain") {
        SetGuestRain();
        return true;
    }
    DC_INFO("benchmark: %.*s at %.1f s", int(name.size()), name.data(), SinceConfig(t));
    if (name == "racing" && g_phase.load() == kWaiting) {
        std::scoped_lock lk(g_flip_lock);
        g_t_race = t;
        g_t_measure = t + cfg.warmup * 1000.0;
        g_phase = kWarmup;
    }
    return true;
}

void OnFlip() {
    const int p = g_phase.load(std::memory_order_acquire);
    if (p < kWarmup || p == kDone) return;
    std::scoped_lock lk(g_flip_lock);
    const double t = NowMs();
    if (p == kWarmup) {
        if (t >= g_t_measure) {
            g_t_measure = t;
            g_cpu_measure = ProcessCpuMs();
            g_phase = kMeasuring;
            DC_INFO("benchmark: measuring at %.1f s", SinceConfig(t));
        }
        g_t_last_flip = t;
        return;
    }
    g_intervals.push_back(t - g_t_last_flip);
    g_t_last_flip = t;
    if (t - g_t_measure >= cfg.seconds * 1000.0) {
        g_phase = kDone;
        Finish(t);
    }
}

} // namespace Core::Benchmark
