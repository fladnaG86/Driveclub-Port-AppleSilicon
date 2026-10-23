#pragma once

#include <filesystem>
#include <string_view>

// Benchmark mode (DC_BENCHMARK, off by default): the game's own Lua autotest
// starts an event, the AI drives, frame times are measured and the process
// exits with a summary line. See benchmark.cpp for the settings.

namespace Core::Benchmark {

// Reads DC_BENCHMARK and writes the test script under <user_root>/benchmark.
void Configure(const std::filesystem::path& user_root);

// The extra guest command-line argument (-luaautotest=...), or null.
const char* GuestArg();

// Folder whose files replace /app0 files of the same path, or empty.
const std::filesystem::path& Overlay();

// True for the script's progress markers (absolute guest path): the open
// fails, the marker is handled here.
bool Marker(std::string_view guest);

// Called after each flip.
void OnFlip();

} // namespace Core::Benchmark
