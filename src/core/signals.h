#pragma once

// Host signal handling for guest code:
//  - SIGILL: patches AMD-only SSE4a instructions (the PS4's Jaguar CPU has
//    them, Intel CPUs do not) into jumps to equivalent SSE2 sequences, once
//    per site, so they then run natively.
//  - SIGSEGV/SIGBUS from write-watched pages: handed to the renderer.
//  - SIGSEGV/SIGBUS/SIGILL that we cannot handle: prints a crash report with
//    the guest module and offset.

#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Signals {

// Describes a host address as "module+offset" for crash reports.
using AddressDescriber = std::string (*)(u64 address);

void Install(AddressDescriber describer);

// Gives the patcher room for trampolines near a code range (rel32 jumps must
// reach). `area` must be mapped RWX and within 2 GB of the code.
void AddPatchArea(VAddr code_begin, VAddr code_end, VAddr area, u64 area_size);

// Runs first on SIGSEGV/SIGBUS with the ucontext_t* and the fault address;
// returning true resumes the faulting code (the GPU renderer write-watches
// guest pages this way).
using FaultHandler = bool (*)(void* ucontext, void* fault_address);
void SetFaultHandler(FaultHandler handler);

// Debugging: counts entries of a guest function that starts with `push rbp`
// (replaced by int3; the trap handler emulates the push). False if the code
// there does not start with push rbp or the probe table is full.
bool AddProbe(VAddr function);
// Entries so far of each probe, in the order they were added.
std::vector<u64> ProbeHits();
// rdi (first argument, `this` for methods) of each probe's latest entry.
std::vector<u64> ProbeArgs();
// Up to 8 distinct rdi (or, with `second`, rsi) values a probe has seen.
std::vector<u64> ProbeDistinctArgs(u32 probe, bool second = false);
// Up to 8 distinct return addresses (callers) a probe has seen.
std::vector<u64> ProbeCallers(u32 probe);

// Debugging: records the code that reads or writes [addr, addr + size). The
// page is protected; a fault there notes the instruction and unprotects it
// until the next WatchRearm(). Returns the sites seen so far.
void WatchAccess(VAddr addr, u64 size);
void WatchRearm();
std::vector<u64> WatchSites();

} // namespace Core::Signals
