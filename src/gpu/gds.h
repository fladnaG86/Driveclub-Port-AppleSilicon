#pragma once

#include <cstdint>

namespace Gpu {

// Global data share (GDS) emulation.
//
// Real hardware has one 64 KB GDS shared by every queue. Driveclub's graphics
// ring and async compute queues all count from GDS offset 0, so running them
// concurrently on the host would make them clobber each other. Give every
// queue its own full 64 KB slice instead: M0[31:16] stays a real byte address
// and no access is silently aliased.
//
// Slice 0 is the graphics ring, slice n the compute queue with vqid n.
// DC_SHARED_GDS=1 puts every queue back in slice 0.
constexpr std::uint32_t kGdsPartition = 0x10000;
// Driveclub maps 32 compute queues at startup; leave headroom for remaps.
constexpr std::uint32_t kGdsPartitions = 64;
constexpr std::uint32_t kGdsDwords = kGdsPartition / sizeof(std::uint32_t);
constexpr std::uint32_t kGdsIndexMask = kGdsDwords - 1;
static_assert((kGdsPartition & (kGdsPartition - 1)) == 0, "GDS partition must be a power of two");

} // namespace Gpu
