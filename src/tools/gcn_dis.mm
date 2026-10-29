// Developer tool: disassembles a raw GCN shader binary (as dumped by the
// renderer with DC_DUMP_SHADERS=dir), translates it to MSL and compiles that
// with Metal to catch translator bugs without running the game.
//
//   gcn_dis <shader.bin> [vs|ps] [--msl]

#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <iterator>
#include <vector>

#include "gpu/shader/gcn.h"
#include "gpu/shader/msl.h"
#include "core/memory.h"
#include "gpu/shader/program.h"

// The tool has no guest memory: nothing is mapped.
namespace Core::Memory {
bool Query(VAddr, bool, Vma*) {
    return false;
}
} // namespace Core::Memory

// key=value overrides of the MslOptions the generator is tested with.
static bool SetOption(Gpu::Shader::MslOptions& o, const std::string& key, unsigned long v) {
    if (key == "map_hoist") {
        o.map_hoist = v != 0;
        return true;
    }
    if (key == "linear_mask") {
        o.linear_mask = static_cast<u32>(v);
        return true;
    }
    if (key == "linear_aligned") {
        o.linear_aligned = static_cast<u32>(v);
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcn_dis <shader.bin> [vs|ps|cs] [--msl]\n");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<u32> code((bytes.size() + 3) / 4 + 2, 0);
    std::memcpy(code.data(), bytes.data(), bytes.size());

    using namespace Gpu::Shader;
    const bool ps = argc > 2 && std::strcmp(argv[2], "ps") == 0;
    const bool cs = argc > 2 && std::strcmp(argv[2], "cs") == 0;
    bool print_msl = false;
    u32 uint_tex = 0; // uint_tex=<mask>: images sampled as uint (default float)
    MslOptions o;
    // After the stage: --msl and MslOptions overrides as key=value.
    for (int k = 3; k < argc; ++k) {
        const char* arg = argv[k];
        if (std::strcmp(arg, "--msl") == 0) {
            print_msl = true;
            continue;
        }
        const char* eq = std::strchr(arg, '=');
        if (eq && std::string(arg, eq - arg) == "uint_tex") {
            uint_tex = static_cast<u32>(std::strtoul(eq + 1, nullptr, 0));
            continue;
        }
        if (!eq || !SetOption(o, std::string(arg, eq - arg), std::strtoul(eq + 1, nullptr, 0))) {
            std::fprintf(stderr, "unknown option %s\n", arg);
            return 1;
        }
    }

    u32 ud[16] = {};
    BuildInput in{cs ? Stage::Compute : ps ? Stage::Fragment : Stage::Vertex, 0x10000000, ud, 16, cs ? 2u : 3u,
                  code.data()};
    in.compute_rsrc2 = cs ? (7u << 7) : 0; // tests: all group ids enabled
    auto prog = BuildProgram(in);
    if (!prog) {
        std::fprintf(stderr, "cannot decode\n");
        return 1;
    }
    for (const auto& i : prog->code) {
        std::printf("%s\n", Gcn::Disassemble(i).c_str());
    }
    static const char* kKinds[] = {"pointer", "vbase", "vbuffer"};
    for (size_t k = 0; k < prog->linear_bases.size(); ++k) {
        const LinearBase& b = prog->linear_bases[k];
        u32 uses = 0;
        for (const auto& [pc, slot] : prog->linear_base_by_pc) {
            uses += slot == k;
        }
        std::printf("linear base %zu: %s, %u uses, extent %u%s%s\n", k, kKinds[b.kind], uses, b.extent,
                    b.stride0 ? ", stride 0 only" : "", b.unbounded ? ", unbounded" : "");
    }

    o.vs_param_mask = 0xFFFFFFFF;
    o.ps_input_addr = 0x2; // PERSP_CENTER
    o.col_format = 0x4;    // MRT0 FP16
    for (u32 k = 0; k < 32; ++k) {
        o.ps_input_cntl[k] = k;
    }
    std::vector<TextureType> textures(prog->images.size());
    for (size_t k = 0; k < textures.size(); ++k) {
        if (uint_tex & (1u << k)) {
            textures[k].sample = TexSample::Uint;
        }
    }
    o.textures = textures;
    std::vector<std::string> unsupported;
    const std::string src = GenerateMsl(*prog, o, &unsupported);
    if (print_msl) {
        std::printf("%s\n", src.c_str());
    }
    for (const auto& u : unsupported) {
        std::printf("unsupported: %s\n", u.c_str());
    }

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeSafe;
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                               options:opts
                                                 error:&err];
        if (!lib) {
            std::printf("MSL compile FAILED:\n%s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        std::printf("MSL compiled OK (%zu bytes of source)\n", src.size());
        id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
        if (cs) {
            id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
            std::printf("compute pipeline: %s\n", pso ? "OK" : err.localizedDescription.UTF8String);
            if (pso) {
                // Drops below 1024 as the shader needs more registers per thread.
                std::printf("max threads per group: %lu\n", (unsigned long)pso.maxTotalThreadsPerThreadgroup);
            }
        }
    }
    return 0;
}
