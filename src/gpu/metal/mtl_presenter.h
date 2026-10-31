#pragma once

// Presents finished frames to a CAMetalLayer. Up to kFramesInFlight frames
// are queued on the GPU so the CPU never waits on the GPU unless it is a
// full three frames ahead.

#include <memory>

namespace Gpu::Metal {

class Device;

struct PresenterConfig {
    bool vsync = true;
    bool edr = false; // Float layer with extended range, see EdrAvailable
};

class Presenter {
public:
    static constexpr int kFramesInFlight = 3;

    // The main screen can show more than SDR white, and DC_NO_EDR is not set.
    static bool EdrAvailable();

    // `layer` is a CAMetalLayer* (e.g. from SDL_Metal_GetLayer).
    Presenter(Device& device, void* layer, const PresenterConfig& config);
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    void Resize(int width_px, int height_px);
    void SetVsync(bool enabled);

    // MetalFX spatial upscaling on/off (when the drawable is larger than the
    // frame); returns the new state.
    bool ToggleUpscaling();

    // Placeholder frame: clears the drawable.
    void PresentClear(float r, float g, float b);

    // Draws a finished frame (id<MTLTexture> as void*) scaled to the drawable.
    // `boost_handle` (EDR, may be null): R16Float highlights for this frame.
    void PresentTexture(void* texture_handle, void* boost_handle = nullptr);

    // EDR highlight boost on/off; returns the new state.
    bool ToggleEdr();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gpu::Metal
