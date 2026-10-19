#include "gpu/shader/program.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "core/memory.h"

namespace Gpu::Shader {

using namespace Gcn;

namespace {

bool Readable(u64 addr, u64 size) {
    Core::Memory::Vma vma;
    return addr && Core::Memory::Query(addr, false, &vma) && addr + size <= vma.start + vma.size;
}

u64 HashCode(const std::vector<Inst>& insts, const u32* base, u64 seed) {
    // FNV-1a over the instruction words.
    u64 h = seed ^ 0xcbf29ce484222325ull;
    for (const Inst& i : insts) {
        for (u32 k = 0; k < i.size; ++k) {
            h ^= base[i.pc / 4 + k];
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

bool IsSgpr(u16 code) {
    return code <= Operand::SgprMax;
}

// Tracks where each scalar register's value came from.
class Provenance {
public:
    explicit Provenance(u32 num_user_sgprs) {
        for (u32 i = 0; i < num_user_sgprs && i < state.size(); ++i) {
            state[i] = {SgprSource::UserData, static_cast<u8>(i), 0};
        }
    }

    const SgprSource& Get(u32 reg) const {
        static const SgprSource unknown{};
        return reg < state.size() ? state[reg] : unknown;
    }

    void Clobber(u32 reg, u32 count = 1) {
        for (u32 k = 0; k < count; ++k) {
            if (reg + k < state.size()) {
                state[reg + k] = {};
            }
        }
    }

    void Step(const Inst& i) {
        switch (i.enc) {
        case Enc::SMRD: {
            const auto op = static_cast<Smrd>(i.op);
            u32 count = 0;
            bool buffer = false;
            switch (op) {
            case Smrd::S_LOAD_DWORD: count = 1; break;
            case Smrd::S_LOAD_DWORDX2: count = 2; break;
            case Smrd::S_LOAD_DWORDX4: count = 4; break;
            case Smrd::S_LOAD_DWORDX8: count = 8; break;
            case Smrd::S_LOAD_DWORDX16: count = 16; break;
            case Smrd::S_BUFFER_LOAD_DWORD: count = 1; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX2: count = 2; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX4: count = 4; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX8: count = 8; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX16: count = 16; buffer = true; break;
            default: return;
            }
            const SgprSource& lo = Get(i.sbase);
            const SgprSource& hi = Get(i.sbase + 1u);
            const bool base_is_ud = lo.kind == SgprSource::UserData &&
                                    hi.kind == SgprSource::UserData && hi.ud == lo.ud + 1;
            for (u32 k = 0; k < count; ++k) {
                SgprSource s{};
                if (base_is_ud && i.imm) {
                    s.kind = buffer ? SgprSource::BufferLoad : SgprSource::PointerLoad;
                    s.ud = lo.ud;
                    s.offset = i.smrd_offset * 4 + k * 4;
                }
                if (i.dst + k < state.size()) {
                    state[i.dst + k] = s;
                }
            }
            return;
        }
        case Enc::SOP1: {
            const auto op = static_cast<Sop1>(i.op);
            if (op == Sop1::S_MOV_B32 && IsSgpr(i.src[0])) {
                Set(i.dst, Get(i.src[0]));
                return;
            }
            if (op == Sop1::S_MOV_B64 && IsSgpr(i.src[0])) {
                const SgprSource a = Get(i.src[0]), b = Get(i.src[0] + 1u);
                Set(i.dst, a);
                Set(i.dst + 1u, b);
                return;
            }
            Clobber(i.dst, Width(i));
            return;
        }
        case Enc::SOP2:
            Clobber(i.dst, Width(i));
            return;
        case Enc::SOPK:
            if (i.op >= static_cast<u16>(Sopk::S_CMPK_EQ_I32) && i.op <= static_cast<u16>(Sopk::S_CMPK_LE_U32)) {
                return; // compares write SCC only
            }
            Clobber(i.dst, 1);
            return;
        case Enc::VOPC:
        case Enc::VOP2:
        case Enc::VOP1:
        case Enc::VOP3:
            if (i.op < 256 || i.vop3) {
                if (i.sdst <= Operand::SgprMax) {
                    Clobber(i.sdst, 2);
                }
            }
            if (static_cast<Vop>(i.op) == Vop::V_READFIRSTLANE_B32 ||
                static_cast<Vop>(i.op) == Vop::V_READLANE_B32) {
                Clobber(i.dst);
            }
            return;
        default:
            return;
        }
    }

private:
    // Scalar registers an SALU instruction writes: two for the 64-bit forms.
    static u32 Width(const Inst& i) {
        const char* n = OpName(i.enc, i.op);
        return n && std::strstr(n, "64") ? 2 : 1;
    }

    void Set(u32 reg, const SgprSource& s) {
        if (reg < state.size()) {
            state[reg] = s;
        }
    }

    std::array<SgprSource, 104> state{};
};

// Calls f(first, count) for the SGPRs an instruction writes, as
// Provenance::Step sees writes.
template <typename F>
void ForEachSgprWrite(const Inst& i, F f) {
    auto width = [](const Inst& in) {
        const char* name = OpName(in.enc, in.op);
        return name && std::strstr(name, "64") ? 2u : 1u;
    };
    switch (i.enc) {
    case Enc::SMRD: {
        const auto op = static_cast<Smrd>(i.op);
        const u32 count = op == Smrd::S_LOAD_DWORD || op == Smrd::S_BUFFER_LOAD_DWORD       ? 1
                          : op == Smrd::S_LOAD_DWORDX4 || op == Smrd::S_BUFFER_LOAD_DWORDX4 ? 4
                          : op == Smrd::S_LOAD_DWORDX8 || op == Smrd::S_BUFFER_LOAD_DWORDX8 ? 8
                          : op == Smrd::S_LOAD_DWORDX16 || op == Smrd::S_BUFFER_LOAD_DWORDX16 ? 16
                                                                                                : 2;
        f(i.dst, count);
        break;
    }
    case Enc::SOP1:
    case Enc::SOP2:
        f(i.dst, width(i));
        break;
    case Enc::SOPK:
        if (i.op < static_cast<u16>(Sopk::S_CMPK_EQ_I32) || i.op > static_cast<u16>(Sopk::S_CMPK_LE_U32)) {
            f(i.dst, 1);
        }
        break;
    case Enc::VOPC:
    case Enc::VOP2:
    case Enc::VOP1:
    case Enc::VOP3:
        if ((i.op < 256 || i.vop3) && i.sdst <= Operand::SgprMax) {
            f(i.sdst, 2);
        }
        if (static_cast<Vop>(i.op) == Vop::V_READFIRSTLANE_B32 || static_cast<Vop>(i.op) == Vop::V_READLANE_B32) {
            f(i.dst, 1);
        }
        break;
    default:
        break;
    }
}

// Which SGPRs hold a value whose provenance, as tracked in program order,
// is the value on every path. Without loops, the last write before a read
// (in program order) reaches it on every path when it dominates the read:
// each branch region (the instructions a forward branch can skip) holding
// the write also holds the read. The write's own sources must have been
// exact. With loops, reaching definitions over the basic blocks: a read is
// exact when the only write reaching it is the last one before it in
// program order (whose provenance is the one tracked) and that write's
// sources were exact, or when only the value at entry reaches it.
class Exactness {
public:
    explicit Exactness(const std::vector<Inst>& code) : n(code.size()) {
        def.fill(-1);
        exact.fill(true);
        for (size_t k = 0; k < code.size(); ++k) {
            if (!IsBranch(code[k])) {
                continue;
            }
            const u32 target = BranchTarget(code[k]);
            if (target <= code[k].pc) {
                loops = true;
                continue;
            }
            size_t t = k + 1;
            while (t < code.size() && code[t].pc < target) {
                ++t;
            }
            regions.emplace_back(k, t); // instructions k+1 .. t-1 may be skipped
        }
        if (loops) {
            Reaching(code);
        }
    }

    // Reads from here on are at instruction `index`.
    void At(size_t index) { cur = static_cast<s64>(index); }

    bool Exact(u32 reg) const {
        if (reg >= exact.size()) {
            return false;
        }
        if (!loops) {
            return exact[reg] && Dominates(def[reg], cur);
        }
        // The write reaching `cur` within its block, else what enters it.
        const size_t b = block_of[cur];
        s32 v = in[b * kRegs + reg];
        for (size_t k = block_start[b]; k < size_t(cur); ++k) {
            if (writes_reg[k * kRegs + reg]) {
                v = s32(k);
            }
        }
        if (v == kEntry) {
            return def[reg] < 0; // nothing written yet in program order either
        }
        return v >= 0 && v == def[reg] && src_exact[size_t(v)];
    }

    // Instruction `index` (the current one) writes its SGPRs; `sources_exact`:
    // the registers its provenance came from were exact.
    void Step(const Inst& i, bool sources_exact) {
        if (loops) {
            src_exact[size_t(cur)] = sources_exact;
        }
        ForEachSgprWrite(i, [&](u32 reg, u32 count) {
            for (u32 r = reg; r < reg + count && r < exact.size(); ++r) {
                exact[r] = sources_exact;
                def[r] = cur;
            }
        });
    }

private:
    static constexpr u32 kRegs = 104;
    static constexpr s32 kNone = -3, kMany = -2, kEntry = -1; // else: the writing instruction

    static s32 Meet(s32 a, s32 b) {
        if (a == kNone) {
            return b;
        }
        if (b == kNone || a == b) {
            return a;
        }
        return kMany;
    }

    void Reaching(const std::vector<Inst>& code) {
        src_exact.assign(n, false);
        writes_reg.assign(n * kRegs, false);
        for (size_t k = 0; k < n; ++k) {
            ForEachSgprWrite(code[k], [&](u32 reg, u32 count) {
                for (u32 r = reg; r < reg + count && r < kRegs; ++r) {
                    writes_reg[k * kRegs + r] = true;
                }
            });
        }
        // Basic blocks: they start at 0, at branch targets and after branches.
        std::vector<bool> leader(n + 1, false);
        leader[0] = true;
        auto index_of = [&](u32 pc) { // inlined fetch code makes pcs non-monotonic
            for (size_t t = 0; t < n; ++t) {
                if (code[t].pc == pc) {
                    return t;
                }
            }
            return n;
        };
        for (size_t k = 0; k < n; ++k) {
            const bool end = code[k].enc == Enc::SOPP && static_cast<Sopp>(code[k].op) == Sopp::S_ENDPGM;
            if (IsBranch(code[k])) {
                leader[index_of(BranchTarget(code[k]))] = true;
            }
            if (IsBranch(code[k]) || end) {
                leader[k + 1] = true;
            }
        }
        block_of.assign(n, 0);
        for (size_t k = 0; k < n; ++k) {
            if (leader[k]) {
                block_start.push_back(k);
            }
            block_of[k] = block_start.size() - 1;
        }
        const size_t nb = block_start.size();
        std::vector<std::vector<size_t>> preds(nb);
        for (size_t b = 0; b < nb; ++b) {
            const size_t last = (b + 1 < nb ? block_start[b + 1] : n) - 1;
            const Inst& i = code[last];
            const bool end = i.enc == Enc::SOPP && static_cast<Sopp>(i.op) == Sopp::S_ENDPGM;
            const bool jump = IsBranch(i) && static_cast<Sopp>(i.op) == Sopp::S_BRANCH;
            if (IsBranch(i)) {
                const size_t t = index_of(BranchTarget(i));
                if (t < n) {
                    preds[block_of[t]].push_back(b);
                }
            }
            if (!end && !jump && last + 1 < n) {
                preds[b + 1].push_back(b);
            }
        }
        // Per block and register: the write(s) entering it.
        in.assign(nb * kRegs, kNone);
        std::vector<s32> out(nb * kRegs, kNone);
        for (u32 r = 0; r < kRegs; ++r) {
            in[r] = kEntry;
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t b = 0; b < nb; ++b) {
                const size_t end = b + 1 < nb ? block_start[b + 1] : n;
                for (u32 r = 0; r < kRegs; ++r) {
                    s32 v = b == 0 ? kEntry : kNone;
                    for (size_t p : preds[b]) {
                        v = Meet(v, out[p * kRegs + r]);
                    }
                    in[b * kRegs + r] = v;
                    for (size_t k = block_start[b]; k < end; ++k) {
                        if (writes_reg[k * kRegs + r]) {
                            v = s32(k);
                        }
                    }
                    if (out[b * kRegs + r] != v) {
                        out[b * kRegs + r] = v;
                        changed = true;
                    }
                }
            }
        }
    }

    bool Dominates(s64 d, s64 r) const {
        if (d < 0) {
            return true; // the value at entry
        }
        for (const auto& [k, t] : regions) {
            const bool in_d = s64(k) < d && d < s64(t);
            const bool in_r = s64(k) < r && r < s64(t);
            if (in_d && !in_r) {
                return false;
            }
        }
        return true;
    }

    size_t n;
    std::vector<std::pair<size_t, size_t>> regions;
    std::array<s64, 104> def{};
    std::array<bool, 104> exact{};
    s64 cur = 0;
    bool loops = false;
    // With loops (see Reaching).
    std::vector<size_t> block_start, block_of;
    std::vector<s32> in;           // block * kRegs + reg
    std::vector<bool> writes_reg;  // instruction * kRegs + reg
    std::vector<bool> src_exact;   // per instruction
};

// Whether the SGPRs an instruction's provenance result depends on are exact:
// a scalar load's base, a move's source. Other writes have no provenance.
bool SourcesExact(const Inst& i, const Exactness& exact) {
    if (i.enc == Enc::SMRD) {
        return exact.Exact(i.sbase) && exact.Exact(i.sbase + 1u);
    }
    if (i.enc == Enc::SOP1 && IsSgpr(i.src[0])) {
        const auto op = static_cast<Sop1>(i.op);
        if (op == Sop1::S_MOV_B32) {
            return exact.Exact(i.src[0]);
        }
        if (op == Sop1::S_MOV_B64) {
            return exact.Exact(i.src[0]) && exact.Exact(i.src[0] + 1u);
        }
    }
    return true;
}

// Records the base of a scalar or buffer memory instruction when its
// registers hold the same value wherever they are read.
void CollectLinearBase(Program& p, const Provenance& prov, const Exactness& exact, const Inst& i) {
    auto stable = [&](u32 reg, const SgprSource& s) {
        return exact.Exact(reg) && (s.kind == SgprSource::UserData || s.kind == SgprSource::PointerLoad);
    };
    LinearBase b;
    if (i.enc == Enc::SMRD) {
        const auto op = static_cast<Smrd>(i.op);
        if (op == Smrd::S_MEMTIME || op == Smrd::S_DCACHE_INV || op == Smrd::S_DCACHE_INV_VOL || !i.imm) {
            return;
        }
        const bool buffer = op >= Smrd::S_BUFFER_LOAD_DWORD;
        const u32 count = op == Smrd::S_LOAD_DWORDX2 || op == Smrd::S_BUFFER_LOAD_DWORDX2   ? 2
                          : op == Smrd::S_LOAD_DWORDX4 || op == Smrd::S_BUFFER_LOAD_DWORDX4 ? 4
                          : op == Smrd::S_LOAD_DWORDX8 || op == Smrd::S_BUFFER_LOAD_DWORDX8 ? 8
                          : op == Smrd::S_LOAD_DWORDX16 || op == Smrd::S_BUFFER_LOAD_DWORDX16 ? 16
                                                                                                : 1;
        b.kind = buffer ? LinearBase::VBase : LinearBase::Pointer;
        for (u32 k = 0; k < 2; ++k) {
            b.words[k] = prov.Get(i.sbase + k);
            if (!stable(i.sbase + k, b.words[k])) {
                return;
            }
        }
        b.extent = i.smrd_offset * 4 + count * 4;
    } else if (i.enc == Enc::MUBUF || i.enc == Enc::MTBUF) {
        if (i.addr64 || i.lds || i.soffset < 128 || i.soffset > 192) {
            return; // only inline non-negative constant soffsets have a known bound
        }
        b.kind = LinearBase::VBuffer;
        for (u32 k = 0; k < 4; ++k) {
            b.words[k] = prov.Get(i.srsrc + k);
            if (k < 3 && !stable(i.srsrc + k, b.words[k])) {
                return;
            }
        }
        const u32 off = i.offset + (i.soffset - 128u) + 16;
        if (i.idxen && i.offen) {
            b.unbounded = true;
        } else if (i.offen) {
            b.stride0 = true; // bounded by num_records only when the stride is 0
            b.extent = 16;
        } else {
            b.extent = off;
        }
    } else {
        return;
    }
    u32 slot = 0;
    for (; slot < p.linear_bases.size(); ++slot) {
        if (p.linear_bases[slot] == b) {
            break;
        }
    }
    if (slot == p.linear_bases.size()) {
        if (slot >= kMaxLinearBases) {
            return;
        }
        p.linear_bases.push_back(b);
    } else {
        LinearBase& e = p.linear_bases[slot];
        e.extent = std::max(e.extent, b.extent);
        e.stride0 |= b.stride0;
        e.unbounded |= b.unbounded;
    }
    p.linear_base_by_pc.emplace_back(i.pc, slot);
}

bool WritesExec(const Inst& i) {
    auto is_exec = [](u16 code) { return code == Operand::ExecLo || code == Operand::ExecHi; };
    switch (i.enc) {
    case Enc::SOP2:
    case Enc::SOPK:
        return is_exec(i.dst);
    case Enc::SOP1: {
        const auto op = static_cast<Sop1>(i.op);
        if (op >= Sop1::S_AND_SAVEEXEC_B64 && op <= Sop1::S_XNOR_SAVEEXEC_B64) {
            return true;
        }
        return is_exec(i.dst);
    }
    case Enc::VOPC:
    case Enc::VOP3:
        if (i.op < 256) {
            const u16 base = i.op < 0x80 ? (i.op & 0x70) : (i.op & 0xF0);
            // V_CMPX_* families: 0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0.
            return (base & 0x10) != 0 || is_exec(i.sdst);
        }
        return i.vop3 && is_exec(i.sdst);
    default:
        return false;
    }
}

bool IsImageSample(u16 op) {
    return op >= static_cast<u16>(Mimg::IMAGE_SAMPLE) && op <= static_cast<u16>(Mimg::IMAGE_GET_LOD);
}

bool IsImageCompare(u16 op) {
    switch (static_cast<Mimg>(op)) {
    case Mimg::IMAGE_SAMPLE_C: case Mimg::IMAGE_SAMPLE_C_CL: case Mimg::IMAGE_SAMPLE_C_D:
    case Mimg::IMAGE_SAMPLE_C_D_CL: case Mimg::IMAGE_SAMPLE_C_L: case Mimg::IMAGE_SAMPLE_C_B:
    case Mimg::IMAGE_SAMPLE_C_B_CL: case Mimg::IMAGE_SAMPLE_C_LZ: case Mimg::IMAGE_SAMPLE_C_O:
    case Mimg::IMAGE_SAMPLE_C_CL_O: case Mimg::IMAGE_SAMPLE_C_D_O: case Mimg::IMAGE_SAMPLE_C_D_CL_O:
    case Mimg::IMAGE_SAMPLE_C_L_O: case Mimg::IMAGE_SAMPLE_C_B_O: case Mimg::IMAGE_SAMPLE_C_B_CL_O:
    case Mimg::IMAGE_SAMPLE_C_LZ_O: case Mimg::IMAGE_GATHER4_C: case Mimg::IMAGE_GATHER4_C_CL:
    case Mimg::IMAGE_GATHER4_C_L: case Mimg::IMAGE_GATHER4_C_B: case Mimg::IMAGE_GATHER4_C_B_CL:
    case Mimg::IMAGE_GATHER4_C_LZ: case Mimg::IMAGE_GATHER4_C_LZ_O:
        return true;
    default:
        return false;
    }
}

// Replaces S_SWAPPC_B64 (call into the fetch subroutine) with the subroutine's
// code. The callee address comes from user data, so it is known per draw.
bool InlineFetch(Program& p, const BuildInput& in) {
    for (size_t idx = 0; idx < p.code.size(); ++idx) {
        const Inst& call = p.code[idx];
        if (call.enc != Enc::SOP1 || static_cast<Sop1>(call.op) != Sop1::S_SWAPPC_B64) {
            continue;
        }
        Provenance prov(in.num_user_sgprs);
        for (size_t k = 0; k < idx; ++k) {
            prov.Step(p.code[k]);
        }
        const SgprSource lo = prov.Get(call.src[0]);
        const SgprSource hi = prov.Get(call.src[0] + 1u);
        if (lo.kind != SgprSource::UserData || hi.kind != SgprSource::UserData) {
            DC_WARN("shader %#llx: fetch subroutine address is not in user data", p.address);
            return false;
        }
        const u64 target = u64(in.user_data[lo.ud]) | (u64(in.user_data[hi.ud]) << 32);
        if (!Readable(target, 16)) {
            DC_WARN("shader %#llx: fetch subroutine %#llx is not mapped", p.address, target);
            return false;
        }
        const u32* fcode = reinterpret_cast<const u32*>(target);
        auto fetch = DecodeProgram(fcode, 4096, true);
        p.fetch_bytes = fetch.empty() ? 0 : fetch.back().pc + fetch.back().size * 4;
        if (!fetch.empty() && fetch.back().enc == Enc::SOP1 &&
            static_cast<Sop1>(fetch.back().op) == Sop1::S_SETPC_B64) {
            fetch.pop_back();
        }
        p.hash = HashCode(fetch, fcode, p.hash);
        for (Inst& f : fetch) {
            f.pc += kFetchPcBase;
        }
        p.fetch_address = target;
        p.fetch_ud = lo.ud;
        p.code.erase(p.code.begin() + static_cast<long>(idx));
        p.code.insert(p.code.begin() + static_cast<long>(idx), fetch.begin(), fetch.end());
        return true;
    }
    return true;
}

} // namespace

u32 Program::LinearBaseOf(u32 pc) const {
    for (const auto& [lpc, slot] : linear_base_by_pc) {
        if (lpc == pc) {
            return slot;
        }
    }
    return ~0u;
}

u32 Program::ImageSlot(u32 pc) const {
    for (const auto& [ipc, slot] : image_slot_by_pc) {
        if (ipc == pc) {
            return slot;
        }
    }
    return 0;
}

std::unique_ptr<Program> BuildProgram(const BuildInput& in) {
    if (!in.code && !Readable(in.address, 64)) {
        return nullptr;
    }
    auto p = std::make_unique<Program>();
    p->stage = in.stage;
    p->address = in.address;
    p->num_user_sgprs = in.num_user_sgprs;
    p->vgpr_comp_cnt = in.vgpr_comp_cnt;
    p->compute_rsrc2 = in.compute_rsrc2;

    const u32* code = in.code ? in.code : reinterpret_cast<const u32*>(in.address);
    p->code = DecodeProgram(code);
    if (p->code.empty()) {
        return nullptr;
    }
    p->hash = HashCode(p->code, code, static_cast<u64>(in.stage));
    if (!InlineFetch(*p, in)) {
        return nullptr;
    }

    Exactness exact(p->code);
    Provenance prov(in.num_user_sgprs);
    bool v0_written = false; // the vertex index is still in v0
    for (const Inst& i : p->code) {
        if (in.stage == Stage::Vertex && !v0_written && i.pc >= kFetchPcBase &&
            (i.enc == Enc::MUBUF || i.enc == Enc::MTBUF) && i.idxen && !i.offen && i.vaddr == 0 &&
            i.soffset == 128 && !i.addr64 && !i.lds && !i.tfe &&
            (i.enc == Enc::MTBUF ? i.op <= static_cast<u16>(Mtbuf::TBUFFER_LOAD_FORMAT_XYZW)
                                 : i.op <= static_cast<u16>(Mubuf::BUFFER_LOAD_FORMAT_XYZW))) {
            VertexInput v;
            v.pc = i.pc;
            for (u32 k = 0; k < 4; ++k) {
                v.vsharp[k] = prov.Get(i.srsrc + k);
            }
            v.offset = i.offset;
            v.count = static_cast<u8>(i.op + 1);
            v.typed = i.enc == Enc::MTBUF;
            v.dfmt = i.dfmt;
            v.nfmt = i.nfmt;
            if (std::all_of(v.vsharp.begin(), v.vsharp.end(),
                            [](const SgprSource& s) { return s.kind != SgprSource::Unknown; })) {
                p->vertex_inputs.push_back(v);
            }
        }
        // Anything that may write v0 ends the inputs we can hand to the host.
        if (((i.enc == Enc::VOP1 || i.enc == Enc::VOP2 || i.enc == Enc::VOP3) && i.dst == 0) ||
            ((i.enc == Enc::MUBUF || i.enc == Enc::MTBUF || i.enc == Enc::MIMG || i.enc == Enc::DS) &&
             i.vdata == 0) ||
            i.enc == Enc::VINTRP) {
            v0_written = true;
        }
        if (IsBranch(i)) {
            p->has_branches = true;
            p->labels.push_back(BranchTarget(i));
        }
        p->writes_exec |= WritesExec(i);

        switch (i.enc) {
        case Enc::EXP:
            if (i.target >= ExpTarget::Param0) {
                p->param_mask |= 1u << (i.target - ExpTarget::Param0);
            } else if (i.target == ExpTarget::Pos0) {
                p->exports_position = true;
            } else if (i.target < 8) {
                p->mrt_mask |= static_cast<u8>(1u << i.target);
            } else if (i.target == ExpTarget::MrtZ) {
                p->exports_depth = true;
            }
            break;
        case Enc::VINTRP:
            p->attr_mask |= 1u << i.attr;
            if (static_cast<Vintrp>(i.op) == Vintrp::V_INTERP_MOV_F32) {
                p->attr_flat_mask |= 1u << i.attr;
            }
            break;
        case Enc::DS:
            if (static_cast<Ds>(i.op) == Ds::DS_SWIZZLE_B32) {
                p->uses_quad_ops = true;
            }
            break;
        case Enc::MIMG: {
            ImageResource r;
            for (u32 k = 0; k < 8; ++k) {
                r.tsharp[k] = prov.Get(i.srsrc + k);
            }
            r.has_sampler = IsImageSample(i.op);
            if (r.has_sampler) {
                for (u32 k = 0; k < 4; ++k) {
                    r.ssharp[k] = prov.Get(i.ssamp + k);
                }
                u32 s_idx = 0;
                for (; s_idx < p->samplers.size(); ++s_idx) {
                    if (p->samplers[s_idx] == r.ssharp) {
                        break;
                    }
                }
                if (s_idx == p->samplers.size()) {
                    p->samplers.push_back(r.ssharp);
                }
                r.sampler_index = s_idx;
            }
            r.storage = i.op >= static_cast<u16>(Mimg::IMAGE_STORE) &&
                        i.op < static_cast<u16>(Mimg::IMAGE_SAMPLE) &&
                        static_cast<Mimg>(i.op) != Mimg::IMAGE_GET_RESINFO;
            r.reads = i.op < static_cast<u16>(Mimg::IMAGE_STORE) ||
                      (i.op > static_cast<u16>(Mimg::IMAGE_GET_RESINFO) && i.op < static_cast<u16>(Mimg::IMAGE_SAMPLE));
            r.compare = IsImageCompare(i.op);
            u32 slot = 0;
            for (; slot < p->images.size(); ++slot) {
                const ImageResource& e = p->images[slot];
                if (e.tsharp == r.tsharp && e.ssharp == r.ssharp) {
                    break;
                }
            }
            if (slot == p->images.size()) {
                p->images.push_back(r);
            } else {
                p->images[slot].storage |= r.storage;
                p->images[slot].reads |= r.reads;
                p->images[slot].compare |= r.compare;
                p->images[slot].has_sampler |= r.has_sampler;
                if (r.has_sampler) {
                    p->images[slot].sampler_index = r.sampler_index;
                }
            }
            p->image_slot_by_pc.emplace_back(i.pc, slot);
            break;
        }
        default:
            break;
        }
        exact.At(static_cast<size_t>(&i - p->code.data()));
        CollectLinearBase(*p, prov, exact, i);
        exact.Step(i, SourcesExact(i, exact));
        prov.Step(i);
    }
    std::sort(p->labels.begin(), p->labels.end());
    p->labels.erase(std::unique(p->labels.begin(), p->labels.end()), p->labels.end());
    return p;
}

namespace {

bool ReadableCached(u64 addr, u64 size, SgprReadCache* cache) {
    if (!cache) {
        return Readable(addr, size);
    }
    if (addr >= cache->start && addr + size <= cache->end) {
        return true;
    }
    Core::Memory::Vma vma;
    if (!addr || !Core::Memory::Query(addr, false, &vma) || addr + size > vma.start + vma.size) {
        return false;
    }
    cache->start = vma.start;
    cache->end = vma.start + vma.size;
    return true;
}

} // namespace

u32 ResolveSgpr(const SgprSource& src, const u32* ud, SgprReadCache* cache) {
    switch (src.kind) {
    case SgprSource::UserData:
        return ud[src.ud];
    case SgprSource::PointerLoad: {
        const u64 base = (u64(ud[src.ud]) | (u64(ud[src.ud + 1]) << 32)) & 0xFFFFFFFFFFFFull;
        const u64 addr = base + src.offset;
        if (!ReadableCached(addr, 4, cache)) {
            return 0;
        }
        u32 v;
        std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        return v;
    }
    case SgprSource::BufferLoad: {
        const u64 base = u64(ud[src.ud]) | (u64(ud[src.ud + 1] & 0xFFF) << 32);
        const u64 addr = base + src.offset;
        if (!ReadableCached(addr, 4, cache)) {
            return 0;
        }
        u32 v;
        std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        return v;
    }
    default:
        return 0;
    }
}

bool LinearBaseRange(const LinearBase& b, const u32* ud, SgprReadCache* cache, u64* va, u64* bytes) {
    const u32 w0 = ResolveSgpr(b.words[0], ud, cache);
    const u32 w1 = ResolveSgpr(b.words[1], ud, cache);
    switch (b.kind) {
    case LinearBase::Pointer:
        *va = (u64(w0) | (u64(w1) << 32)) & 0xFFFFFFFFFFFFull;
        *bytes = b.extent;
        break;
    case LinearBase::VBase:
        *va = u64(w0) | (u64(w1 & 0xFFF) << 32);
        *bytes = b.extent;
        break;
    case LinearBase::VBuffer: {
        const u32 stride = (w1 >> 16) & 0x3FFF;
        if (b.unbounded || (b.stride0 && stride != 0)) {
            return false;
        }
        const u32 records = ResolveSgpr(b.words[2], ud, cache);
        *va = u64(w0) | (u64(w1 & 0xFFF) << 32);
        *bytes = (stride ? u64(records) * stride : u64(records)) + b.extent;
        break;
    }
    }
    return *va != 0 && (*va & 3) == 0;
}

} // namespace Gpu::Shader
