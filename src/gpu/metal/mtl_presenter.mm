#include "gpu/metal/mtl_presenter.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>

#include "common/log.h"
#include "gpu/metal/mtl_device.h"

namespace Gpu::Metal {

namespace {

constexpr const char* kPresentShaderSrc = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vs_present(uint vid [[vertex_id]]) {
    VertexOut out;
    out.uv = float2((vid << 1) & 2, vid & 2);
    out.position = float4(out.uv.x * 2.0 - 1.0, 1.0 - out.uv.y * 2.0, 0.0, 1.0);
    return out;
}

fragment float4 fs_present(VertexOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}

struct EdrParams {
    float gain, knee, limit;
};

// Output is extended linear sRGB: 1.0 is SDR white, above that is EDR.
fragment float4 fs_edr(VertexOut in [[stage_in]],
                       texture2d<float> tex [[texture(0)]],
                       texture2d<float> peak [[texture(1)]],
                       sampler smp [[sampler(0)]],
                       constant EdrParams& p [[buffer(0)]]) {
    float3 c = tex.sample(smp, in.uv).rgb;
    float3 lin = select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
    float over = max(peak.sample(smp, in.uv).r - p.knee, 0.0);
    return float4(lin * min(1.0 + p.gain * over, p.limit), 1.0);
}
)";

} // namespace

struct Presenter::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    id<MTLRenderPipelineState> pipeline;
    id<MTLSamplerState> sampler;
    dispatch_semaphore_t frames_in_flight;

    bool upscale = true; // DC_NO_METALFX or M key: plain bilinear present
    id<MTLFXSpatialScaler> scaler;
    id<MTLTexture> scaled; // Scaler output; the drawable is framebufferOnly

    // EDR: float layer, boost applied in fs_edr.
    bool edr = false;
    bool edr_boost = true;
    float edr_gain = 4.0f, edr_knee = 0.7f;
    id<MTLRenderPipelineState> edr_pipeline;
    id<MTLTexture> edr_black; // Stands in when a frame has no boost map

    // The scaler for this frame and drawable, nil when there is nothing to
    // upscale or MetalFX can't. Rebuilt when either size changes.
    id<MTLFXSpatialScaler> ScalerFor(id<MTLTexture> frame, id<MTLTexture> target) {
        if (!upscale || target.width < frame.width || target.height < frame.height ||
            (target.width == frame.width && target.height == frame.height)) {
            return nil;
        }
        if (scaler && scaled.width == target.width && scaled.height == target.height &&
            scaler.inputWidth == frame.width && scaler.inputHeight == frame.height) {
            return scaler;
        }
        scaler = nil;
        scaled = nil;
        if (![MTLFXSpatialScalerDescriptor supportsDevice:device]) {
            DC_INFO("present: MetalFX not supported, using bilinear");
            upscale = false;
            return nil;
        }
        MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = frame.width;
        d.inputHeight = frame.height;
        d.outputWidth = target.width;
        d.outputHeight = target.height;
        d.colorTextureFormat = frame.pixelFormat;
        d.outputTextureFormat = frame.pixelFormat;
        // Frames hold the gamma-encoded bytes the PS4 would scan out.
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        scaler = [d newSpatialScalerWithDevice:device];
        if (scaler) {
            MTLTextureDescriptor* t =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:frame.pixelFormat
                                                                   width:target.width
                                                                  height:target.height
                                                               mipmapped:NO];
            t.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
            t.storageMode = MTLStorageModePrivate;
            scaled = [device newTextureWithDescriptor:t];
        }
        if (!scaler || !scaled) {
            DC_INFO("present: MetalFX scaler creation failed, using bilinear");
            scaler = nil;
            upscale = false;
            return nil;
        }
        DC_INFO("present: MetalFX spatial %lux%lu -> %lux%lu", (unsigned long)frame.width,
                (unsigned long)frame.height, (unsigned long)target.width,
                (unsigned long)target.height);
        return scaler;
    }
};

bool Presenter::EdrAvailable() {
    return std::getenv("DC_NO_EDR") == nullptr &&
           NSScreen.mainScreen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
}

Presenter::Presenter(Device& device, void* layer, const PresenterConfig& config)
    : impl(std::make_unique<Impl>()) {
    impl->device = (__bridge id<MTLDevice>)device.Handle();
    impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
    impl->layer = (__bridge CAMetalLayer*)layer;
    impl->frames_in_flight = dispatch_semaphore_create(kFramesInFlight);
    impl->upscale = std::getenv("DC_NO_METALFX") == nullptr;

    CAMetalLayer* l = impl->layer;
    l.device = impl->device;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Only ever rendered to, never sampled: lets the driver skip extra copies.
    l.framebufferOnly = YES;
    l.maximumDrawableCount = kFramesInFlight;
    l.displaySyncEnabled = config.vsync;

    // Create fullscreen present pipeline
    NSError* err = nil;
    NSString* src = [NSString stringWithUTF8String:kPresentShaderSrc];
    id<MTLLibrary> lib = [impl->device newLibraryWithSource:src options:nil error:&err];
    if (err) {
        NSLog(@"Presenter shader compilation failed: %@", err);
    }
    id<MTLFunction> vs = [lib newFunctionWithName:@"vs_present"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"fs_present"];

    MTLRenderPipelineDescriptor* pDesc = [MTLRenderPipelineDescriptor new];
    pDesc.vertexFunction = vs;
    pDesc.fragmentFunction = fs;
    pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:pDesc error:&err];
    if (err) {
        NSLog(@"Presenter pipeline creation failed: %@", err);
    }

    MTLSamplerDescriptor* sDesc = [MTLSamplerDescriptor new];
    sDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sDesc.magFilter = MTLSamplerMinMagFilterLinear;
    impl->sampler = [impl->device newSamplerStateWithDescriptor:sDesc];

    if (config.edr) {
        impl->edr = true;
        if (const char* e = std::getenv("DC_EDR_GAIN")) {
            impl->edr_gain = std::strtof(e, nullptr);
        }
        if (const char* e = std::getenv("DC_EDR_KNEE")) {
            impl->edr_knee = std::strtof(e, nullptr);
        }
        l.pixelFormat = MTLPixelFormatRGBA16Float;
        l.wantsExtendedDynamicRangeContent = YES;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
        l.colorspace = cs;
        CGColorSpaceRelease(cs);

        pDesc.fragmentFunction = [lib newFunctionWithName:@"fs_edr"];
        pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        impl->edr_pipeline = [impl->device newRenderPipelineStateWithDescriptor:pDesc error:&err];
        if (err) {
            NSLog(@"Presenter EDR pipeline creation failed: %@", err);
        }
        MTLTextureDescriptor* t =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        impl->edr_black = [impl->device newTextureWithDescriptor:t];
        const uint16_t zero = 0;
        [impl->edr_black replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                           mipmapLevel:0
                             withBytes:&zero
                           bytesPerRow:2];
        DC_INFO("present: EDR gain %g knee %g, screen headroom potential %g",
                impl->edr_gain, impl->edr_knee,
                NSScreen.mainScreen.maximumPotentialExtendedDynamicRangeColorComponentValue);
    }
}

Presenter::~Presenter() {
    // Drain in-flight frames so their completion handlers don't outlive us.
    for (int i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);
    }
    // libdispatch traps if a semaphore dies below its initial value.
    for (int i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_signal(impl->frames_in_flight);
    }
}

void Presenter::Resize(int width_px, int height_px) {
    impl->layer.drawableSize = CGSizeMake(width_px, height_px);
}

void Presenter::SetVsync(bool enabled) {
    impl->layer.displaySyncEnabled = enabled;
}

bool Presenter::ToggleUpscaling() {
    impl->upscale = !impl->upscale;
    return impl->upscale;
}

bool Presenter::ToggleEdr() {
    impl->edr_boost = !impl->edr_boost;
    return impl->edr_boost;
}

void Presenter::PresentClear(float r, float g, float b) {
    dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(impl->frames_in_flight);
            return;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);

        id<MTLCommandBuffer> cmd = [impl->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];

        dispatch_semaphore_t sem = impl->frames_in_flight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
        }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

void Presenter::PresentTexture(void* texture_handle, void* boost_handle) {
    if (!texture_handle || !impl->pipeline) {
        return;
    }
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture_handle;

    dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(impl->frames_in_flight);
            return;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cmd = [impl->queue commandBuffer];
        if (id<MTLFXSpatialScaler> scaler = impl->ScalerFor(tex, drawable.texture)) {
            scaler.colorTexture = tex;
            scaler.outputTexture = impl->scaled;
            [scaler encodeToCommandBuffer:cmd];
            tex = impl->scaled;
        }
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        if (impl->edr && impl->edr_pipeline) {
            id<MTLTexture> boost = boost_handle ? (__bridge id<MTLTexture>)boost_handle : impl->edr_black;
            const float params[3] = {
                impl->edr_boost ? impl->edr_gain : 0.0f, impl->edr_knee,
                static_cast<float>(NSScreen.mainScreen.maximumExtendedDynamicRangeColorComponentValue)};
            [enc setRenderPipelineState:impl->edr_pipeline];
            [enc setFragmentTexture:boost atIndex:1];
            [enc setFragmentBytes:params length:sizeof(params) atIndex:0];
        } else {
            [enc setRenderPipelineState:impl->pipeline];
        }
        [enc setFragmentTexture:tex atIndex:0];
        [enc setFragmentSamplerState:impl->sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        dispatch_semaphore_t sem = impl->frames_in_flight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
        }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

} // namespace Gpu::Metal
