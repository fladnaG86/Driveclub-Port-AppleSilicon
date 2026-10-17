#include "gpu/metal/mtl_compiler.h"

#include <pthread.h>
#include <cstdlib>
#include <cstring>

#include "common/log.h"

namespace Gpu::Metal {

ShaderCompiler::ShaderCompiler(id<MTLDevice> device, u32 count) : device(device) {
    for (u32 k = 0; k < count; ++k) {
        threads.emplace_back([this] { Worker(); });
    }
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

void ShaderCompiler::Submit(std::function<void()> job) {
    {
        std::scoped_lock lk(lock);
        jobs.push_back(std::move(job));
    }
    cv.notify_one();
}

void ShaderCompiler::Worker() {
    pthread_setname_np("GPU shader compiler");
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(lock);
            cv.wait(lk, [this] { return stop || !jobs.empty(); });
            if (stop) {
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
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
    opts.languageVersion = MTLLanguageVersion3_0;
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
