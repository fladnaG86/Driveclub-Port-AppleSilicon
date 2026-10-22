#include "gpu/metal/mtl_compiler.h"

#include <pthread.h>
#include <cstdlib>
#include <cstring>

#include "common/log.h"

namespace Gpu::Metal {

ShaderCompiler::ShaderCompiler(id<MTLDevice> device, u32 count) : device(device) {
    for (u32 k = 0; k < count; ++k) {
        threads.emplace_back([this] { Worker(false); });
    }
    threads.emplace_back([this] { Worker(true); });
}

ShaderCompiler::~ShaderCompiler() {
    {
        std::scoped_lock lk(lock);
        stop = true;
    }
    cv.notify_all();
    for (auto& t : threads) {
        t.join();
    }
}

void ShaderCompiler::Submit(std::function<void()> job, bool urgent) {
    // DC_NO_URGENT_SHADERS: one FIFO queue for everything (A/B).
    static const bool no_urgent = std::getenv("DC_NO_URGENT_SHADERS") != nullptr;
    urgent = urgent && !no_urgent;
    {
        std::scoped_lock lk(lock);
        (urgent ? urgent_jobs : jobs).push_back(std::move(job));
    }
    // Wake everyone: a waiting worker may be the urgent-only one.
    cv.notify_all();
}

void ShaderCompiler::Worker(bool urgent_only) {
    pthread_setname_np(urgent_only ? "GPU shader compiler (urgent)" : "GPU shader compiler");
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(lock);
            cv.wait(lk, [&] {
                return stop || !urgent_jobs.empty() || (!urgent_only && !jobs.empty());
            });
            if (stop) {
                return;
            }
            auto& q = !urgent_jobs.empty() ? urgent_jobs : jobs;
            job = std::move(q.front());
            q.pop_front();
        }
        @autoreleasepool {
            job();
        }
    }
}

id<MTLFunction> ShaderCompiler::Function(u64 key, const std::string& src, u64 address) {
    {
        std::scoped_lock lk(fn_lock);
        if (auto it = functions.find(key); it != functions.end()) {
            return it->second;
        }
    }
    MTLCompileOptions* opts = [MTLCompileOptions new];
    // Relaxed keeps Inf/NaN semantics but lets the compiler fuse and reorder:
    // ~5% less GPU time per frame in a race. DC_MATH_MODE=safe|fast for A/B.
    static const MTLMathMode math_mode = [] {
        const char* m = std::getenv("DC_MATH_MODE");
        if (m && std::strcmp(m, "safe") == 0) {
            return MTLMathModeSafe;
        }
        if (m && std::strcmp(m, "fast") == 0) {
            return MTLMathModeFast;
        }
        return MTLMathModeRelaxed;
    }();
    opts.mathMode = math_mode;
    // 3.1 only where needed (texture atomics): other shaders keep their cached binaries.
    opts.languageVersion = src.find(".atomic_fetch_") != std::string::npos ? MTLLanguageVersion3_1
                                                                           : MTLLanguageVersion3_0;
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(src.c_str()) options:opts error:&err];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"main0"] : nil;
    if (!fn) {
        DC_ERROR("gpu: MSL compile failed for shader %#llx: %s", address,
                 err ? err.localizedDescription.UTF8String : "?");
    }
    std::scoped_lock lk(fn_lock);
    return functions.emplace(key, fn).first->second;
}

} // namespace Gpu::Metal
