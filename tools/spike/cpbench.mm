// cpbench: the command processor thread's per-frame CPU work, synthetic,
// built twice (x86_64 under Rosetta, arm64 native) to measure what a native
// GPU process would gain. Shaped on a race frame (DC_GPU_STATS, `sample`):
// ~100 render passes, ~2000 draws, ~300 dispatches, 12 command buffers,
// ~23 MB of texture hashing, descriptor-cache lookups and renderer locks.
//
//   clang++ -std=c++20 -O2 -fobjc-arc -arch <x86_64|arm64> cpbench.mm \
//       -framework Metal -framework Foundation -o cpbench_<arch>
//   ./cpbench_<arch> [frames]
//
// Prints the median thread CPU ms per frame of each part.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <mach/mach.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace {

constexpr u32 kPasses = 100;
constexpr u32 kDraws = 2000;
constexpr u32 kDispatches = 300;
constexpr u32 kCommits = 12;
constexpr u32 kPipelines = 64;
constexpr u32 kTextures = 96;
constexpr u64 kHashBytes = 23ull << 20;
constexpr u64 kChunk = 64 << 10;
constexpr u32 kLookups = 20000;

// Thread CPU time from Mach (CLOCK_THREAD_CPUTIME_ID is wrong under Rosetta).
double ThreadMs() {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    thread_info(self, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n);
    mach_port_deallocate(mach_task_self(), self);
    return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
           (info.user_time.microseconds + info.system_time.microseconds) * 1e-3;
}

double WallMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The renderer's Fnv and Hash64 (mtl_renderer.mm).
u64 Fnv(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

u64 Hash64(const u8* p, u64 size) {
    u64 h[4] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull,
                0x27d4eb2f165667c5ull};
    u64 off = 0;
    for (; off + 32 <= size; off += 32) {
        for (u32 k = 0; k < 4; ++k) {
            u64 v;
            std::memcpy(&v, p + off + k * 8, 8);
            h[k] = (h[k] ^ v) * 0x100000001b3ull;
            h[k] ^= h[k] >> 29;
        }
    }
    for (; off < size; ++off) {
        h[0] = (h[0] ^ p[off]) * 0x100000001b3ull;
    }
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
}

const char* kShaders = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs(uint vid [[vertex_id]], constant uint* ud [[buffer(0)]],
               device const float4* m0 [[buffer(1)]], device const float4* m1 [[buffer(2)]],
               constant uint* map [[buffer(3)]], constant float4* draw [[buffer(4)]],
               constant uint* lin [[buffer(8)]]) {
    VOut o;
    float4 p = m0[(vid + ud[0]) & 255] + m1[(vid + lin[0]) & 255] * float(map[0] & 1);
    o.pos = float4(p.xy * 0.001 + draw[0].xy, 0.5, 1.0);
    o.uv = p.zw;
    return o;
}
fragment float4 fs(VOut in [[stage_in]], constant uint* ud [[buffer(0)]],
                   texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],
                   texture2d<float> t2 [[texture(2)]], texture2d<float> t3 [[texture(3)]],
                   texture2d<float> t4 [[texture(4)]], texture2d<float> t5 [[texture(5)]],
                   sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],
                   sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]]) {
    float2 uv = in.uv;
    return t0.sample(s0, uv) + t1.sample(s1, uv) + t2.sample(s2, uv) + t3.sample(s3, uv) +
           t4.sample(s0, uv) + t5.sample(s1, uv) + float(ud[1] & 1);
}
kernel void cs(uint3 id [[thread_position_in_grid]], constant uint* ud [[buffer(0)]],
               device uint* m0 [[buffer(1)]], device uint* m1 [[buffer(2)]],
               constant uint* map [[buffer(3)]], device uint* gds [[buffer(5)]],
               texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],
               texture2d<float, access::write> t2 [[texture(2)]]) {
    if (id.x == 0 && ud[2] == 0xdeadbeef) {
        m0[0] = m1[0] + map[0] + gds[0] + uint(t0.read(uint2(0)).x + t1.read(uint2(0)).x);
        t2.write(float4(1), uint2(0));
    }
}
)";

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    std::vector<id<MTLRenderPipelineState>> pipes;
    std::vector<id<MTLComputePipelineState>> cpipes;
    std::vector<id<MTLTexture>> tex;
    std::vector<id<MTLSamplerState>> smp;
    id<MTLTexture> color, depth, storage;
    id<MTLBuffer> mem0, mem1, map, gds, index;
    id<MTLDepthStencilState> ds;
};

Gpu Setup() {
    Gpu g;
    g.dev = MTLCreateSystemDefaultDevice();
    g.queue = [g.dev newCommandQueue];
    NSError* err = nil;
    id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(kShaders) options:nil error:&err];
    if (!lib) {
        std::fprintf(stderr, "shaders: %s\n", err.localizedDescription.UTF8String);
        std::exit(1);
    }
    for (u32 k = 0; k < kPipelines; ++k) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        d.colorAttachments[0].blendingEnabled = (k & 1) != 0;
        d.colorAttachments[0].writeMask = MTLColorWriteMask(1 + (k % 15));
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        g.pipes.push_back([g.dev newRenderPipelineStateWithDescriptor:d error:&err]);
        if (!g.pipes.back()) {
            std::fprintf(stderr, "render pipeline: %s\n", err.localizedDescription.UTF8String);
            std::exit(1);
        }
    }
    for (u32 k = 0; k < 16; ++k) {
        g.cpipes.push_back([g.dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs"]
                                                                error:&err]);
    }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:64
                                                          height:64
                                                       mipmapped:NO];
    for (u32 k = 0; k < kTextures; ++k) {
        g.tex.push_back([g.dev newTextureWithDescriptor:td]);
    }
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    g.storage = [g.dev newTextureWithDescriptor:td];
    MTLTextureDescriptor* cd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                           width:256
                                                          height:256
                                                       mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget;
    cd.storageMode = MTLStorageModePrivate;
    g.color = [g.dev newTextureWithDescriptor:cd];
    cd.pixelFormat = MTLPixelFormatDepth32Float;
    g.depth = [g.dev newTextureWithDescriptor:cd];
    for (u32 k = 0; k < 4; ++k) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = k & 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        g.smp.push_back([g.dev newSamplerStateWithDescriptor:sd]);
    }
    g.mem0 = [g.dev newBufferWithLength:64 << 20 options:MTLResourceStorageModeShared];
    g.mem1 = [g.dev newBufferWithLength:64 << 20 options:MTLResourceStorageModeShared];
    g.map = [g.dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
    g.gds = [g.dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
    g.index = [g.dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
    std::memset(g.mem0.contents, 0, 1 << 20);
    std::memset(g.index.contents, 0, 1 << 20);
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dd.depthWriteEnabled = YES;
    g.ds = [g.dev newDepthStencilStateWithDescriptor:dd];
    return g;
}

// Part A: what the renderer hands Metal for one frame.
id<MTLCommandBuffer> last;

void EncodeFrame(Gpu& g, u32 frame) {
    u32 ud[16] = {frame};
    float consts[16] = {};
    u32 lin[8] = {};
    const u32 draws_per_pass = kDraws / kPasses;
    const u32 dispatch_every = kPasses / std::max<u32>(1, kDispatches / 3);
    const u32 passes_per_cmd = kPasses / kCommits;
    id<MTLCommandBuffer> cmd = nil;
    u32 dispatches = 0;
    for (u32 p = 0; p < kPasses; ++p) {
        @autoreleasepool {
            if (!cmd) {
                cmd = [g.queue commandBufferWithUnretainedReferences];
            }
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = g.color;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = g.depth;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
            [e setDepthStencilState:g.ds];
            for (u32 d = 0; d < draws_per_pass; ++d) {
                const u32 n = p * draws_per_pass + d;
                ud[1] = n;
                lin[0] = n * 3;
                if (d == 0 || n % 3 == 0) {
                    [e setRenderPipelineState:g.pipes[(n * 7) % kPipelines]];
                }
                [e setCullMode:(n & 1) ? MTLCullModeBack : MTLCullModeNone];
                [e setVertexBytes:ud length:64 atIndex:0];
                [e setVertexBuffer:g.mem0 offset:0 atIndex:1];
                [e setVertexBuffer:g.mem1 offset:0 atIndex:2];
                [e setVertexBuffer:g.map offset:0 atIndex:3];
                [e setVertexBytes:consts length:64 atIndex:4];
                [e setVertexBytes:lin length:32 atIndex:8];
                [e setFragmentBytes:ud length:64 atIndex:0];
                for (u32 t = 0; t < 6; ++t) {
                    [e setFragmentTexture:g.tex[(n * 5 + t * 11) % kTextures] atIndex:t];
                }
                for (u32 s = 0; s < 4; ++s) {
                    [e setFragmentSamplerState:g.smp[(n + s) & 3] atIndex:s];
                }
                [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                              indexCount:300
                               indexType:MTLIndexTypeUInt16
                             indexBuffer:g.index
                       indexBufferOffset:0];
            }
            [e endEncoding];
            // Async compute between passes, three dispatches at a time.
            if (dispatch_every && p % dispatch_every == 0) {
                id<MTLComputeCommandEncoder> c = [cmd computeCommandEncoder];
                for (u32 k = 0; k < 3 && dispatches < kDispatches; ++k, ++dispatches) {
                    ud[2] = dispatches;
                    [c setComputePipelineState:g.cpipes[dispatches % g.cpipes.size()]];
                    [c setBytes:ud length:64 atIndex:0];
                    [c setBuffer:g.mem0 offset:0 atIndex:1];
                    [c setBuffer:g.mem1 offset:0 atIndex:2];
                    [c setBuffer:g.map offset:0 atIndex:3];
                    [c setBuffer:g.gds offset:0 atIndex:5];
                    [c setTexture:g.tex[dispatches % kTextures] atIndex:0];
                    [c setTexture:g.tex[(dispatches + 1) % kTextures] atIndex:1];
                    [c setTexture:g.storage atIndex:2];
                    [c dispatchThreadgroups:MTLSizeMake(4, 4, 1)
                        threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                }
                [c endEncoding];
            }
            if ((p + 1) % passes_per_cmd == 0 || p + 1 == kPasses) {
                [cmd commit];
                if (p + 1 == kPasses) {
                    last = cmd;
                }
                cmd = nil;
            }
        }
    }
}

// Part B: texture change detection hashing.
u64 HashFrame(const std::vector<u8>& mem, u32 frame) {
    u64 acc = 0;
    const u64 span = mem.size() - kChunk;
    for (u64 done = 0; done < kHashBytes; done += kChunk) {
        const u64 off = ((done / kChunk) * 7919 + frame * 104729) * kChunk % span;
        acc ^= Hash64(mem.data() + off, kChunk);
    }
    return acc;
}

// Part C: descriptor cache lookups, keys, renderer locks.
struct Entry {
    u64 epoch;
    void* tex;
    u32 type[4];
};

u64 LogicFrame(std::unordered_map<u64, Entry>& cache, std::mutex& lock, u32 frame) {
    u64 acc = 0;
    u32 t[8];
    for (u32 k = 0; k < kLookups; ++k) {
        for (u32 w = 0; w < 8; ++w) {
            t[w] = (k % 700) * 2654435761u + w * 40503u;
        }
        u64 key = Fnv(t, 24);
        std::scoped_lock lk(lock);
        auto it = cache.find(key);
        if (it == cache.end()) {
            it = cache.emplace(key, Entry{frame, nullptr, {k, k, k, k}}).first;
        }
        acc += it->second.epoch + it->second.type[k & 3];
    }
    if (cache.size() > 4096) {
        cache.clear();
    }
    return acc;
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    const u32 frames = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 300;
    @autoreleasepool {
        Gpu g = Setup();
        std::vector<u8> mem(256ull << 20);
        std::mt19937_64 rng(1);
        for (size_t i = 0; i + 8 <= mem.size(); i += 8) {
            const u64 v = rng();
            std::memcpy(&mem[i], &v, 8);
        }
        std::unordered_map<u64, Entry> cache;
        std::mutex lock;
        std::vector<double> a, b, c, aw, bw, cw;
        u64 sink = 0;
        for (u32 f = 0; f < frames + 20; ++f) {
            double t0 = ThreadMs(), w0 = WallMs();
            EncodeFrame(g, f);
            double t1 = ThreadMs(), w1 = WallMs();
            sink ^= HashFrame(mem, f);
            double t2 = ThreadMs(), w2 = WallMs();
            sink ^= LogicFrame(cache, lock, f);
            double t3 = ThreadMs(), w3 = WallMs();
            [last waitUntilCompleted]; // not timed: the GPU's share
            if (f >= 20) { // warm-up: pipeline and driver caches
                a.push_back(t1 - t0);
                b.push_back(t2 - t1);
                c.push_back(t3 - t2);
                aw.push_back(w1 - w0);
                bw.push_back(w2 - w1);
                cw.push_back(w3 - w2);
            }
        }
#if defined(__x86_64__)
        const char* arch = "x86_64";
#else
        const char* arch = "arm64";
#endif
        std::printf("%s: CPU ms/frame encode %.2f hash %.2f logic %.2f | wall encode %.2f hash "
                    "%.2f logic %.2f (median of %u; sink %llx)\n",
                    arch, Median(a), Median(b), Median(c), Median(aw), Median(bw), Median(cw),
                    frames, static_cast<unsigned long long>(sink & 0xf));
    }
    return 0;
}
