#pragma once

// A decoded guest shader plus what the backend needs to know about it:
// which user-data words feed its resource descriptors, what it exports,
// which PS inputs it reads.
//
// Descriptors (T#, S#) live in scalar registers that the shader fills from
// user data or loads from memory pointed to by user data. The host API has to
// bind textures before the draw, so each image instruction records where its
// descriptor words came from ("provenance"); the renderer evaluates that
// against the draw's user data to find the actual texture.

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "gpu/shader/gcn.h"

namespace Gpu::Shader {

enum class Stage : u8 { Vertex, Fragment, Compute };

struct SgprSource {
    enum Kind : u8 {
        Unknown,
        UserData,    // user SGPR `ud`
        PointerLoad, // dword at (64-bit pointer in user SGPRs ud, ud+1) + offset
        BufferLoad,  // dword at V#(user SGPRs ud..ud+3).base + offset
    };
    Kind kind = Unknown;
    u8 ud = 0;
    u32 offset = 0; // bytes

    bool operator==(const SgprSource&) const = default;
};

// A fetch-subroutine buffer load indexed by the vertex index (v0): the host
// can fetch it with a Metal vertex descriptor instead of the shader.
struct VertexInput {
    u32 pc = 0;
    std::array<SgprSource, 4> vsharp{};
    u32 offset = 0;    // the instruction's immediate offset (bytes)
    u8 count = 4;      // components loaded
    bool typed = false; // MTBUF: dfmt/nfmt come from the instruction
    u8 dfmt = 0, nfmt = 0;
};

struct ImageResource {
    std::array<SgprSource, 8> tsharp{};
    std::array<SgprSource, 4> ssharp{};
    bool has_sampler = false;
    bool storage = false; // image_store / atomics
    bool reads = false;   // image_load / atomics: a storage image then needs read_write access
    bool compare = false; // depth-compare sampling
    u32 sampler_index = 0;
};

// A guest memory base the host can translate per draw: memory instructions
// on it then index the memory buffer directly instead of walking the page
// table (MslOptions::linear_mask). Its registers are written at most once in
// the program, so their provenance holds wherever they are read.
struct LinearBase {
    enum Kind : u8 {
        Pointer, // 64-bit pointer (scalar loads)
        VBase,   // V# base, words 0-1 (scalar buffer loads)
        VBuffer, // V# base, words 0-1, bounded by words 1-2 (vector buffer access)
    };
    Kind kind = Pointer;
    std::array<SgprSource, 4> words{}; // pointer lo/hi, or V# words 0-3
    u32 extent = 0;        // bytes read past the base (VBuffer: past the records)
    bool stride0 = false;  // VBuffer: some access is bounded only when the stride is 0
    bool unbounded = false; // VBuffer: some access has no bound at all
    bool scalar = false;    // VBuffer: scalar loads, bounded by num_records bytes whatever the stride

    bool operator==(const LinearBase& b) const {
        return kind == b.kind && words == b.words;
    }
};
constexpr u32 kMaxLinearBases = 32;

// Fetch-subroutine instructions get pcs in their own range so they never
// collide with branch targets of the main program.
constexpr u32 kFetchPcBase = 0x100000;

struct Program {
    Stage stage = Stage::Vertex;
    u64 address = 0;      // guest address of the main program
    u64 fetch_address = 0; // inlined fetch subroutine, if any
    int fetch_ud = -1;     // user-data index holding its address
    u32 fetch_bytes = 0;   // length of the fetch subroutine (with its S_SETPC)
    u64 hash = 0;         // of the code bytes (plus fetch code)
    u32 num_user_sgprs = 0;
    u32 vgpr_comp_cnt = 0; // VS: system VGPRs after the vertex index; CS: TIDIG_COMP_CNT
    u32 compute_rsrc2 = 0; // CS: COMPUTE_PGM_RSRC2 (system SGPR enables, LDS size)

    std::vector<Gcn::Inst> code;
    std::vector<u32> labels; // byte offsets that need a case label (sorted)
    bool has_branches = false;
    bool writes_exec = false;

    // Image instruction pc -> index into `images`.
    std::vector<std::pair<u32, u32>> image_slot_by_pc;
    std::vector<ImageResource> images;
    std::vector<std::array<SgprSource, 4>> samplers;

    // VS: fetch loads the host may do (see VertexInput).
    std::vector<VertexInput> vertex_inputs;

    // Memory instruction pc -> index into `linear_bases`.
    std::vector<std::pair<u32, u32>> linear_base_by_pc;
    std::vector<LinearBase> linear_bases;

    // VS exports.
    u32 param_mask = 0;
    bool exports_position = false;
    // PS usage.
    u32 attr_mask = 0;      // PS attributes read by v_interp_*
    u32 attr_flat_mask = 0; // ... of which read through v_interp_mov
    u8 mrt_mask = 0;
    bool exports_depth = false;
    bool uses_quad_ops = false;

    std::vector<std::string> unsupported; // opcodes we could not translate

    u32 ImageSlot(u32 pc) const;
    u32 LinearBaseOf(u32 pc) const; // ~0u: none
};

struct BuildInput {
    Stage stage;
    u64 address;
    const u32* user_data; // the stage's 16 user-data registers
    u32 num_user_sgprs;
    u32 vgpr_comp_cnt;
    const u32* code = nullptr; // tools: decode from here instead of guest memory
    u32 compute_rsrc2 = 0;
};

// Decodes and analyzes a program. Returns null if the code is unreadable.
std::unique_ptr<Program> BuildProgram(const BuildInput& in);

// The last guest mapping a descriptor read went through: descriptor words
// sit next to each other, so one lookup serves a whole draw's worth.
struct SgprReadCache {
    u64 start = 0, end = 0;
};

// Evaluates a descriptor dword at draw time. `cache` (optional) must not
// outlive the draw: a mapping may go away between draws.
u32 ResolveSgpr(const SgprSource& src, const u32* user_data, SgprReadCache* cache = nullptr);

// Guest range [va, va + bytes) a linear base covers at draw time, or false
// when its access is not bounded with these descriptor values.
bool LinearBaseRange(const LinearBase& b, const u32* user_data, SgprReadCache* cache, u64* va, u64* bytes);

} // namespace Gpu::Shader
