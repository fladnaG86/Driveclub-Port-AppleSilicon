#pragma once

// Metal backend for the guest GPU: translates guest shaders to MSL, keeps
// render targets and textures as Metal textures, and records draws.

#include <atomic>
#include <memory>

#include "gpu/renderer.h"

namespace Gpu::Metal {

class Device;

// Any thread (F12): the next flip's frame is traced (DC_GPU_TRACE), with its
// passes and the frame dumped to DC_DUMP_FRAMES, or ../bench/f12_<frame>.
void RequestFrameTrace();

// Any thread (F11): the next two frames go into ../bench/capture_<flip>.gputrace
// (launch with MTL_CAPTURE_ENABLED=1).
void RequestGpuCapture();

class MetalRenderer : public Gpu::Renderer {
public:
    explicit MetalRenderer(Device& device);
    ~MetalRenderer() override;

    void Draw(const Liverpool::RegFile& regs, const DrawCall& draw) override;
    void Dispatch(const Liverpool::RegFile& regs, std::uint32_t x, std::uint32_t y,
                  std::uint32_t z) override;
    bool DispatchIndirect(const Liverpool::RegFile& regs, std::uint64_t args) override;
    void OnSubmit() override;
    void WaitIdle() override;
    void Signal(std::function<void()> done) override;
    void NoteConstDump(std::uint64_t addr, std::uint32_t bytes) override;
    void Flip(const FlipInfo& flip) override;
    void OnContextRegs(const std::uint32_t* body, std::uint32_t body_dwords) override;
    bool DmaData(std::uint32_t dst_sel, std::uint64_t dst, std::uint32_t src_sel, std::uint64_t src,
                 std::uint32_t bytes) override;
    std::uint8_t* Gds() override;

    // Main thread: the last flipped frame as id<MTLTexture> (void*), or null.
    // It stays valid until two more flips have happened.
    void* LatestFrame() const;

    // Any thread: frames the game has flipped so far (its real frame rate,
    // unlike how often the window presents).
    std::uint64_t FlipCount() const { return flips.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<std::uint64_t> flips{0};
};

} // namespace Gpu::Metal
