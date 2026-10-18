#pragma once

// Write tracking of guest pages for the renderer's texture cache: a watched
// page is mapped read-only, and the first CPU write to it faults, counts as a
// write and makes the page writable again until it is watched once more. A
// texture then knows it may have changed by comparing the write counts of its
// pages instead of hashing its memory every frame.
//
// The counts only grow. Writes that do not fault (the kernel writing guest
// memory for read()/pread(), remaps, protection changes) must be reported
// with BeforeHostWrite / Invalidate.

#include <optional>

#include "common/types.h"

namespace Core::WriteTrack {

constexpr u64 kPageShift = 14; // 16 KB, the guest mapping granularity

// Installs the fault handler. DC_NO_WRITE_TRACK leaves tracking off (Watch
// then always fails).
void Init();

// Arms [addr, addr + size): its pages fault on the next write. False when a
// page cannot be watched (not mapped writable, table full, tracking off).
bool Watch(VAddr addr, u64 size);

// Sum of the write counts of the pages of [addr, addr + size); nullopt if one
// of them is not tracked.
std::optional<u64> Version(VAddr addr, u64 size);

// The host is about to write [addr, addr + size) without faulting (a syscall
// filling guest memory): counts a write on its watched pages and disarms them.
void BeforeHostWrite(VAddr addr, u64 size);

// The mapping or protection of [addr, addr + size) changes: same as a write.
void Invalidate(VAddr addr, u64 size);

} // namespace Core::WriteTrack
