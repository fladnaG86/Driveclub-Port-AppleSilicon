// SPDX-License-Identifier: GPL-2.0-or-later
//
// libSceAjm: the PS4 audio decoder (ATRAC9 here) that FMOD feeds with the
// game's compressed sounds. Without it only raw-PCM sounds played (the
// player's engine); music, ambience and other cars were silent.
//
// The batch format, job semantics and sideband layouts follow shadPS4
// (src/core/libraries/ajm, GPL-2.0-or-later, Copyright 2024-2026 shadPS4
// Emulator Project). Decoding uses LibAtrac9 (third_party/libatrac9, MIT).
// Batches run synchronously in sceAjmBatchStartBuffer: a few frames of
// ATRAC9 decode in well under a millisecond, and sceAjmBatchWait then
// returns at once.

#include "hle/ajm/ajm.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

#include "common/log.h"
#include "common/types.h"
#include "hle/hle.h"

extern "C" {
#include "libatrac9.h"
}

namespace Hle::Ajm {

namespace {

constexpr s32 kErrorInvalidInstance = static_cast<s32>(0x80930003);
constexpr s32 kErrorInvalidParameter = static_cast<s32>(0x80930005);
constexpr s32 kErrorOutOfResources = static_cast<s32>(0x80930007);
constexpr s32 kErrorWrongRevisionFlag = static_cast<s32>(0x8093000B);
constexpr s32 kErrorMalformedBatch = static_cast<s32>(0x80930011);

constexpr s32 kResultNotInitialized = 0x00000001;
constexpr s32 kResultPartialInput = 0x00000008;
constexpr s32 kResultNotEnoughRoom = 0x00000010;
constexpr s32 kResultCodecError = 0x40000000;
constexpr s32 kResultFatal = static_cast<s32>(0x80000000);

constexpr u32 kCodecAt9 = 1;
constexpr u32 kInstanceStatistics = 0x80000;
constexpr u32 kInstanceIndexMask = 0x3FFF;

// Job flags: version [2:0], codec [10:3], run [12:11], control [15:13],
// sideband [47:45].
constexpr u64 kRunGetCodecInfo = 1ull << 11;
constexpr u64 kRunMultipleFrames = 1ull << 12;
constexpr u64 kControlReset = 1ull << 13;
constexpr u64 kControlInitialize = 1ull << 14;
constexpr u64 kControlResample = 1ull << 15;
constexpr u64 kSidebandGapless = 1ull << 45;
constexpr u64 kSidebandFormat = 1ull << 46;
constexpr u64 kSidebandStream = 1ull << 47;

// ATRAC9 codec flags (instance flags [59:32]).
constexpr u32 kAt9ParseRiff = 1u << 0;
constexpr u32 kAt9NonInterleaved = 1u << 8;

enum Ident : u32 {
    kIdentJob = 0,
    kIdentInputRun = 1,
    kIdentInputControl = 2,
    kIdentControlFlags = 3,
    kIdentRunFlags = 4,
    kIdentReturnAddress = 6,
    kIdentInline = 7,
    kIdentOutputRun = 17,
    kIdentOutputControl = 18,
};

// Chunk header: ident [5:0], payload [25:6].
u32 ChunkHeader(u32 ident, u32 payload) {
    return (ident & 0x3F) | ((payload & 0xFFFFF) << 6);
}
u32 ChunkIdent(u32 header) {
    return header & 0x3F;
}
u32 ChunkPayload(u32 header) {
    return (header >> 6) & 0xFFFFF;
}

struct ChunkBuffer {
    u32 header;
    u32 size;
    void* address;
};
static_assert(sizeof(ChunkBuffer) == 16);

struct SidebandResult {
    s32 result;
    s32 internal_result;
};
struct SidebandStream {
    s32 input_consumed;
    s32 output_written;
    u64 total_decoded_samples;
};
struct SidebandFormat {
    u32 num_channels;
    u32 channel_mask;
    u32 sample_rate;
    u32 sample_encoding; // 0 S16, 1 S32, 2 float
    u32 bitrate;
    u32 reserved;
};
struct SidebandGapless {
    u32 total_samples;
    u16 skip_samples;
    u16 skipped_samples;
};
struct SidebandMultipleFrames {
    u32 num_frames;
    u32 reserved;
};
struct SidebandResample {
    float ratio;
    u32 flags;
};
struct At9InitParameters {
    u8 config_data[4];
    u32 reserved;
};
struct At9CodecInfo {
    u32 super_frame_size;
    u32 frames_in_super_frame;
    u32 next_frame_size;
    u32 frame_samples;
};

u32 ChannelMask(u32 channels) {
    switch (channels) {
    case 1: return 0x0004;
    case 2: return 0x0003;
    case 4: return 0x0033;
    case 6: return 0x060F;
    case 8: return 0x063F;
    default: return 0;
    }
}

u32 PcmSize(u32 encoding) {
    return encoding == 0 ? 2 : 4;
}

// Output run buffers, filled in order.
class Output {
public:
    void Add(u8* p, size_t n) {
        if (p && n) {
            chunks_.push_back({p, n});
        }
    }
    size_t Size() const {
        size_t n = 0;
        for (size_t k = cur_; k < chunks_.size(); ++k) {
            n += chunks_[k].size();
        }
        return n;
    }
    size_t Write(const u8* src, size_t n) {
        size_t done = 0;
        while (done < n && cur_ < chunks_.size()) {
            auto& c = chunks_[cur_];
            const size_t m = std::min(n - done, c.size());
            std::memcpy(c.data(), src + done, m);
            done += m;
            c = c.subspan(m);
            if (c.empty()) {
                ++cur_;
            }
        }
        return done;
    }

private:
    std::vector<std::span<u8>> chunks_;
    size_t cur_ = 0;
};

struct Gapless {
    SidebandGapless init{};
    SidebandGapless current{};
    bool IsEnd() const {
        return init.total_samples != 0 && current.total_samples == 0;
    }
    void Reset() {
        current.total_samples = init.total_samples;
        current.skip_samples = init.skip_samples;
        current.skipped_samples = 0;
    }
};

struct Job {
    std::vector<u8> input;
    Output output;
    u64 flags = 0;
    ChunkBuffer input_control{};
    ChunkBuffer output_control{};
};

class At9Instance {
public:
    At9Instance(u32 encoding, u32 codec_flags, bool gapless_loop)
        : encoding_(encoding), flags_(codec_flags), gapless_loop_(gapless_loop) {}
    ~At9Instance() {
        if (handle_) {
            Atrac9ReleaseHandle(handle_);
        }
    }

    void Execute(Job& job) {
        SidebandResult dummy_result{};
        SidebandResult* result = &dummy_result;
        SidebandStream* stream = nullptr;
        SidebandFormat* format = nullptr;
        SidebandGapless* gapless_out = nullptr;
        SidebandMultipleFrames* mframe = nullptr;
        u8* codec_info = nullptr;

        // Sideband inputs.
        if (job.input_control.address && job.input_control.size) {
            const u8* p = static_cast<const u8*>(job.input_control.address);
            const u8* const end = p + job.input_control.size;
            if ((job.flags & kSidebandFormat) && p + sizeof(SidebandFormat) <= end) {
                p += sizeof(SidebandFormat); // output format changes: unimplemented
            }
            if ((job.flags & kSidebandGapless) && p + sizeof(SidebandGapless) <= end) {
                SidebandGapless g;
                std::memcpy(&g, p, sizeof(g));
                p += sizeof(g);
                SetGapless(g);
            }
            if ((job.flags & kControlResample) && p + sizeof(SidebandResample) <= end) {
                p += sizeof(SidebandResample);
            }
            if ((job.flags & kControlInitialize) && p + sizeof(At9InitParameters) <= end) {
                At9InitParameters init;
                std::memcpy(&init, p, sizeof(init));
                Initialize(init.config_data);
            }
        }
        // Sideband outputs, in the order the guest laid them out.
        if (job.output_control.address && job.output_control.size >= sizeof(SidebandResult)) {
            u8* p = static_cast<u8*>(job.output_control.address);
            u8* const end = p + job.output_control.size;
            auto take = [&](auto*& out) {
                using T = std::remove_reference_t<decltype(*out)>;
                if (p + sizeof(T) <= end) {
                    out = reinterpret_cast<T*>(p);
                    *out = T{};
                    p += sizeof(T);
                }
            };
            take(result);
            if (job.flags & kSidebandStream) take(stream);
            if (job.flags & kSidebandFormat) take(format);
            if (job.flags & kSidebandGapless) take(gapless_out);
            if (job.flags & kRunMultipleFrames) take(mframe);
            if ((job.flags & kRunGetCodecInfo) && p + sizeof(At9CodecInfo) <= end) {
                codec_info = p;
            }
        }

        *result = {};
        if (job.flags & kControlReset) {
            Reset();
        }

        std::span<const u8> in(job.input);
        const size_t in_size = in.size();
        const size_t out_size = job.output.Size();
        u32 frames = 0;
        if (!in.empty()) {
            for (;;) {
                if (gapless_loop_ && gapless_.IsEnd()) {
                    gapless_.Reset();
                    total_samples_ = 0;
                }
                if (!gapless_.IsEnd() && job.output.Size() < NextFrameSize()) {
                    result->result |= kResultNotEnoughRoom;
                }
                if (in.size() < superframe_remain_) {
                    result->result |= kResultPartialInput;
                }
                if (result->result != 0) {
                    break;
                }
                const s32 r = DecodeFrame(in, job.output, frames, result->internal_result);
                if (r != 0) {
                    result->result |= r;
                    break;
                }
                if (!(job.flags & kRunMultipleFrames)) {
                    break;
                }
            }
        }

        if (mframe) {
            mframe->num_frames = frames;
        }
        if (stream) {
            stream->input_consumed = static_cast<s32>(in_size - in.size());
            stream->output_written = static_cast<s32>(out_size - job.output.Size());
            stream->total_decoded_samples = total_samples_;
        }
        if (format) {
            format->num_channels = u32(info_.channels);
            format->channel_mask = ChannelMask(u32(info_.channels));
            format->sample_rate = u32(info_.samplingRate);
            format->sample_encoding = encoding_;
            format->bitrate = info_.framesInSuperframe && info_.frameSamples
                                  ? u32(u64(info_.samplingRate) * info_.superframeSize * 8 /
                                        (u64(info_.framesInSuperframe) * info_.frameSamples))
                                  : 0;
        }
        if (gapless_out) {
            *gapless_out = gapless_.current;
        }
        if (codec_info) {
            const At9CodecInfo ci{u32(info_.superframeSize), u32(info_.framesInSuperframe), superframe_remain_,
                                  u32(info_.frameSamples)};
            std::memcpy(codec_info, &ci, sizeof(ci));
        }
    }

private:
    void Initialize(const u8 config[4]) {
        std::memcpy(config_, config, 4);
        Reset();
        pcm_.assign(size_t(info_.frameSamples) * info_.channels * PcmSize(encoding_), 0);
        initialized_ = true;
    }

    void Reset() {
        if (handle_) {
            Atrac9ReleaseHandle(handle_);
        }
        handle_ = Atrac9GetHandle();
        Atrac9InitDecoder(handle_, config_);
        Atrac9GetCodecInfo(handle_, &info_);
        frames_ = 0;
        superframe_remain_ = u32(info_.superframeSize);
        total_samples_ = 0;
        gapless_.Reset();
    }

    void SetGapless(const SidebandGapless& g) {
        const u32 processed = gapless_.init.total_samples - gapless_.current.total_samples;
        if ((g.total_samples != 0 || g.skip_samples == 0) && g.total_samples >= processed) {
            const s64 diff = s64(gapless_.init.total_samples) - g.total_samples;
            gapless_.init.total_samples = g.total_samples;
            gapless_.current.total_samples = u32(s64(gapless_.current.total_samples) - diff);
        }
        const u32 skipped = u32(gapless_.init.skip_samples) - gapless_.current.skip_samples;
        if ((g.skip_samples != 0 || g.total_samples == 0) && g.skip_samples >= skipped) {
            const s32 diff = s32(gapless_.init.skip_samples) - g.skip_samples;
            gapless_.init.skip_samples = g.skip_samples;
            gapless_.current.skip_samples = u16(s32(gapless_.current.skip_samples) - diff);
        }
    }

    u32 NextFrameSize() const {
        const u32 skip = std::min<u32>(gapless_.current.skip_samples, u32(info_.frameSamples));
        const u32 samples = gapless_.init.total_samples != 0
                                ? std::min<u32>(gapless_.current.total_samples, u32(info_.frameSamples) - skip)
                                : u32(info_.frameSamples) - skip;
        return samples * u32(info_.channels) * PcmSize(encoding_);
    }

    // "RIFF....WAVE" with fmt (ATRAC9 GUID, config data) and fact chunks:
    // initializes the decoder and the gapless range, leaves `in` at the data.
    bool ParseRiff(std::span<const u8>& in) {
        static const u8 kAt9Guid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                        0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};
        if (in.size() < 20 || std::memcmp(in.data(), "RIFF", 4) || std::memcmp(in.data() + 8, "WAVE", 4)) {
            return false;
        }
        in = in.subspan(12);
        bool ok = false;
        while (in.size() >= 8) {
            u32 len;
            std::memcpy(&len, in.data() + 4, 4);
            const bool data = !std::memcmp(in.data(), "data", 4);
            const bool fmt = !std::memcmp(in.data(), "fmt ", 4);
            const bool fact = !std::memcmp(in.data(), "fact", 4);
            in = in.subspan(8);
            if (data) {
                break;
            }
            if (in.size() < len) {
                return false;
            }
            if (fmt) {
                // WAVEFORMATEXTENSIBLE (fmt_type 0xFFFE) + version + config data.
                u16 type;
                std::memcpy(&type, in.data(), 2);
                if (len != 52 || type != 0xFFFE || std::memcmp(in.data() + 24, kAt9Guid, 16)) {
                    return false;
                }
                Initialize(in.data() + 44);
                ok = true;
            } else if (fact && len >= 12) {
                u32 sample_length, encoder_delay;
                std::memcpy(&sample_length, in.data(), 4);
                std::memcpy(&encoder_delay, in.data() + 4, 4);
                gapless_.init.total_samples = sample_length;
                gapless_.init.skip_samples = u16(encoder_delay);
                gapless_.Reset();
            }
            in = in.subspan(std::min<size_t>(in.size(), (len + 1) & ~1u));
        }
        return ok;
    }

    template <class T>
    size_t WriteSamples(Output& out, u32 skip, u32 max_values) {
        const size_t channels = size_t(info_.channels);
        const size_t total = pcm_.size() / sizeof(T);
        const size_t first = std::min(total, size_t(skip) * channels);
        const size_t count = std::min(total - first, size_t(max_values));
        return out.Write(pcm_.data() + first * sizeof(T), count * sizeof(T)) / sizeof(T);
    }

    // One frame; 0 or the AJM result bits on failure.
    s32 DecodeFrame(std::span<const u8>& in, Output& out, u32& frames, s32& internal) {
        if ((flags_ & kAt9ParseRiff) && in.size() >= 12 && !std::memcmp(in.data(), "RIFF", 4)) {
            if (!ParseRiff(in)) {
                return kResultCodecError | kResultFatal;
            }
            total_samples_ = 0;
        }
        if (!initialized_) {
            return kResultNotInitialized;
        }
        if (in.empty()) {
            return 0;
        }
        const int nointerleave = (flags_ & kAt9NonInterleaved) ? 1 : 0;
        int used = 0;
        int ret;
        const u8* buf = in.data();
        switch (encoding_) {
        case 1:
            ret = Atrac9DecodeS32(handle_, buf, int(in.size()), reinterpret_cast<int*>(pcm_.data()), &used,
                                  nointerleave);
            break;
        case 2:
            ret = Atrac9DecodeF32(handle_, buf, int(in.size()), reinterpret_cast<float*>(pcm_.data()), &used,
                                  nointerleave);
            break;
        default:
            ret = Atrac9Decode(handle_, buf, int(in.size()), reinterpret_cast<short*>(pcm_.data()), &used,
                               nointerleave);
            break;
        }
        if (ret != 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                DC_WARN("ajm: ATRAC9 decode failed (%d)", ret);
            }
            internal = ret;
            return kResultCodecError | kResultFatal;
        }
        ++frames;
        in = in.subspan(size_t(used));
        superframe_remain_ -= u32(used);

        u32 skip = 0;
        if (gapless_.current.skip_samples > 0) {
            skip = std::min<u32>(u32(info_.frameSamples), gapless_.current.skip_samples);
            gapless_.current.skip_samples = u16(gapless_.current.skip_samples - skip);
        }
        const u32 max_values = gapless_.init.total_samples != 0
                                   ? gapless_.current.total_samples * u32(info_.channels)
                                   : ~0u;
        size_t written;
        switch (encoding_) {
        case 1: written = WriteSamples<s32>(out, skip, max_values); break;
        case 2: written = WriteSamples<float>(out, skip, max_values); break;
        default: written = WriteSamples<s16>(out, skip, max_values); break;
        }
        const u32 samples = u32(written / size_t(info_.channels));
        total_samples_ += samples;
        gapless_.current.skipped_samples =
            u16(gapless_.current.skipped_samples + (u32(info_.frameSamples) - samples));
        if (gapless_.init.total_samples != 0) {
            gapless_.current.total_samples -= samples;
        }

        ++frames_;
        if (frames_ % u32(info_.framesInSuperframe) == 0) {
            in = in.subspan(std::min<size_t>(in.size(), superframe_remain_));
            superframe_remain_ = u32(info_.superframeSize);
            frames_ = 0;
        } else if (gapless_.IsEnd()) {
            // Drain the rest of the superframe.
            std::vector<short> scratch(size_t(info_.frameSamples) * info_.channels);
            while (frames_ % u32(info_.framesInSuperframe) != 0 && !in.empty()) {
                used = 0;
                if (Atrac9Decode(handle_, in.data(), int(in.size()), scratch.data(), &used, nointerleave) != 0 ||
                    used == 0) {
                    break;
                }
                in = in.subspan(size_t(used));
                superframe_remain_ -= u32(used);
                ++frames;
                ++frames_;
            }
            in = in.subspan(std::min<size_t>(in.size(), superframe_remain_));
            superframe_remain_ = u32(info_.superframeSize);
            frames_ = 0;
        }
        return 0;
    }

    const u32 encoding_;
    const u32 flags_;
    const bool gapless_loop_;
    void* handle_ = nullptr;
    u8 config_[4] = {};
    bool initialized_ = false;
    Atrac9CodecInfo info_{};
    u32 superframe_remain_ = 0;
    u32 frames_ = 0;
    u64 total_samples_ = 0;
    Gapless gapless_;
    std::vector<u8> pcm_;
};

std::mutex g_lock;
std::vector<std::unique_ptr<At9Instance>> g_instances; // index 0 unused
u32 g_next_batch = 1;

At9Instance* FindInstance(u32 id) {
    const u32 index = id & kInstanceIndexMask;
    return index < g_instances.size() ? g_instances[index].get() : nullptr;
}

// Writes a job chunk header at `p`; the job body starts right after it.
u8* BeginJob(void* p, u32 instance, u32** size_field) {
    u32* h = static_cast<u32*>(p);
    h[0] = ChunkHeader(kIdentJob, instance);
    *size_field = &h[1];
    return reinterpret_cast<u8*>(h + 2);
}

u8* PutBuffer(u8* p, u32 ident, const void* address, size_t size) {
    const ChunkBuffer c{ChunkHeader(ident, 0), u32(size), const_cast<void*>(address)};
    std::memcpy(p, &c, sizeof(c));
    return p + sizeof(c);
}

u8* PutFlags(u8* p, u32 ident, u64 flags) {
    const u32 v[2] = {ChunkHeader(ident, u32(flags >> 32)), u32(flags)};
    std::memcpy(p, v, sizeof(v));
    return p + sizeof(v);
}

// The chunks of one job (`body` .. `end`).
Job ParseJob(const u8* body, const u8* end) {
    Job job;
    while (body + 4 <= end) {
        u32 header;
        std::memcpy(&header, body, 4);
        const u32 ident = ChunkIdent(header);
        if (ident == kIdentControlFlags || ident == kIdentRunFlags) {
            u32 low;
            std::memcpy(&low, body + 4, 4);
            job.flags = (u64(ChunkPayload(header)) << 32) | low;
            body += 8;
            continue;
        }
        if (body + sizeof(ChunkBuffer) > end) {
            break;
        }
        ChunkBuffer c;
        std::memcpy(&c, body, sizeof(c));
        body += sizeof(c);
        switch (ident) {
        case kIdentInputRun:
            if (c.address && c.size) {
                const u8* p = static_cast<const u8*>(c.address);
                job.input.insert(job.input.end(), p, p + c.size);
            }
            break;
        case kIdentOutputRun: job.output.Add(static_cast<u8*>(c.address), c.size); break;
        case kIdentInputControl: job.input_control = c; break;
        case kIdentOutputControl: job.output_control = c; break;
        default: break; // return address
        }
    }
    return job;
}

s32 sceAjmInitialize(s64 reserved, u32* p_ctx) {
    if (p_ctx) {
        *p_ctx = 1;
    }
    DC_INFO("sceAjmInitialize() -> ctx 1");
    return 0;
}

s32 sceAjmFinalize() {
    return 0;
}

s32 sceAjmModuleRegister(u32 ctx, u32 type, s64 reserved) {
    DC_INFO("ajm: module register codec %u", type);
    return 0;
}

s32 sceAjmModuleUnregister() {
    return 0;
}

s32 sceAjmInstanceCreate(u32 ctx, u32 type, u64 flags, u32* out_inst) {
    if ((flags & 7) == 0) {
        return kErrorWrongRevisionFlag;
    }
    if (type != kCodecAt9) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            DC_WARN("ajm: codec %u is not supported (only ATRAC9)", type);
        }
        return kErrorInvalidParameter;
    }
    // Instance flags: version [2:0], channels [6:3], format [9:7],
    // gapless loop [10], codec flags [59:32].
    const u32 encoding = u32(flags >> 7) & 7;
    const u32 codec_flags = u32(flags >> 32) & 0x0FFFFFFF;
    const bool gapless_loop = (flags >> 10) & 1;
    std::scoped_lock lk(g_lock);
    if (g_instances.empty()) {
        g_instances.emplace_back();
    }
    size_t index = 1;
    while (index < g_instances.size() && g_instances[index]) {
        ++index;
    }
    if (index > kInstanceIndexMask) {
        return kErrorOutOfResources;
    }
    if (index == g_instances.size()) {
        g_instances.emplace_back();
    }
    g_instances[index] = std::make_unique<At9Instance>(encoding, codec_flags, gapless_loop);
    if (out_inst) {
        *out_inst = u32(index) | (type << 14);
    }
    return 0;
}

s32 sceAjmInstanceDestroy(u32 ctx, u32 inst) {
    std::scoped_lock lk(g_lock);
    const u32 index = inst & kInstanceIndexMask;
    if (index >= g_instances.size() || !g_instances[index]) {
        return kErrorInvalidInstance;
    }
    g_instances[index].reset();
    return 0;
}

s32 sceAjmMemoryRegister(u32 ctx, void* ptr, size_t num_pages) {
    return 0;
}

s32 sceAjmMemoryUnregister(u32 ctx, void* ptr) {
    return 0;
}

void* sceAjmBatchJobControlBufferRa(void* p, u32 instance, u64 flags, void* sideband_in, size_t sideband_in_size,
                                    void* sideband_out, size_t sideband_out_size, void* return_address) {
    u32* size_field;
    u8* const body = BeginJob(p, instance, &size_field);
    u8* q = body;
    if (return_address) {
        q = PutBuffer(q, kIdentReturnAddress, return_address, 0);
    }
    q = PutBuffer(q, kIdentInputControl, sideband_in, sideband_in_size);
    flags &= instance == kInstanceStatistics ? 0x0000'0000'C001'8007ull : 0x0000'6000'0000'E7FFull;
    q = PutFlags(q, kIdentControlFlags, flags);
    q = PutBuffer(q, kIdentOutputControl, sideband_out, sideband_out_size);
    *size_field = u32(q - body);
    return q;
}

void* sceAjmBatchJobRunBufferRa(void* p, u32 instance, u64 flags, void* in, size_t in_size, void* out,
                                size_t out_size, void* sideband_out, size_t sideband_out_size,
                                void* return_address) {
    u32* size_field;
    u8* const body = BeginJob(p, instance, &size_field);
    u8* q = body;
    if (return_address) {
        q = PutBuffer(q, kIdentReturnAddress, return_address, 0);
    }
    q = PutBuffer(q, kIdentInputRun, in, in_size);
    q = PutFlags(q, kIdentRunFlags, flags & 0x0000'E000'0000'1FFFull);
    q = PutBuffer(q, kIdentOutputRun, out, out_size);
    q = PutBuffer(q, kIdentOutputControl, sideband_out, sideband_out_size);
    *size_field = u32(q - body);
    return q;
}

struct AjmBuffer {
    void* address;
    u64 size;
};

void* sceAjmBatchJobRunSplitBufferRa(void* p, u32 instance, u64 flags, const AjmBuffer* ins, size_t num_ins,
                                     const AjmBuffer* outs, size_t num_outs, void* sideband_out,
                                     size_t sideband_out_size, void* return_address) {
    u32* size_field;
    u8* const body = BeginJob(p, instance, &size_field);
    u8* q = body;
    if (return_address) {
        q = PutBuffer(q, kIdentReturnAddress, return_address, 0);
    }
    for (size_t k = 0; k < num_ins; ++k) {
        q = PutBuffer(q, kIdentInputRun, ins[k].address, ins[k].size);
    }
    q = PutFlags(q, kIdentRunFlags, flags & 0x0000'E000'0000'1FFFull);
    for (size_t k = 0; k < num_outs; ++k) {
        q = PutBuffer(q, kIdentOutputRun, outs[k].address, outs[k].size);
    }
    q = PutBuffer(q, kIdentOutputControl, sideband_out, sideband_out_size);
    *size_field = u32(q - body);
    return q;
}

void* sceAjmBatchJobInlineBuffer(void* p, const void* data, size_t size, const void** batch_address) {
    u32* h = static_cast<u32*>(p);
    const u32 aligned = u32(AlignUp(size, 8));
    h[0] = ChunkHeader(kIdentInline, 0);
    h[1] = aligned;
    u8* q = reinterpret_cast<u8*>(h + 2);
    if (batch_address) {
        *batch_address = q;
    }
    std::memcpy(q, data, size);
    return q + aligned;
}

s32 sceAjmBatchStartBuffer(u32 ctx, u8* batch, u32 size, s32 priority, void* batch_error, u32* out_batch_id) {
    if (size & 7) {
        return kErrorMalformedBatch;
    }
    std::scoped_lock lk(g_lock);
    const u8* p = batch;
    const u8* const end = batch + size;
    while (p + 8 <= end) {
        u32 header, job_size;
        std::memcpy(&header, p, 4);
        std::memcpy(&job_size, p + 4, 4);
        p += 8;
        const u8* const job_end = std::min(end, p + job_size);
        const u32 instance = ChunkPayload(header);
        if (ChunkIdent(header) == kIdentJob && instance != kInstanceStatistics) {
            Job job = ParseJob(p, job_end);
            if (At9Instance* inst = FindInstance(instance)) {
                inst->Execute(job);
            } else if (job.output_control.address && job.output_control.size >= sizeof(SidebandResult)) {
                const SidebandResult r{kResultNotInitialized, 0};
                std::memcpy(job.output_control.address, &r, sizeof(r));
            }
        }
        p = job_end;
    }
    if (out_batch_id) {
        *out_batch_id = g_next_batch++;
    }
    return 0;
}

s32 sceAjmBatchWait(u32 ctx, u32 batch_id, u32 timeout, void* batch_error) {
    return 0; // batches complete inside sceAjmBatchStartBuffer
}

s32 sceAjmBatchErrorDump() {
    return 0;
}

} // namespace

void RegisterAjm() {
    DC_HLE("libSceAjm", sceAjmInitialize);
    DC_HLE("libSceAjm", sceAjmFinalize);
    DC_HLE("libSceAjm", sceAjmModuleRegister);
    DC_HLE("libSceAjm", sceAjmModuleUnregister);
    DC_HLE("libSceAjm", sceAjmInstanceCreate);
    DC_HLE("libSceAjm", sceAjmInstanceDestroy);
    DC_HLE("libSceAjm", sceAjmMemoryRegister);
    DC_HLE("libSceAjm", sceAjmMemoryUnregister);
    DC_HLE("libSceAjm", sceAjmBatchJobControlBufferRa);
    DC_HLE("libSceAjm", sceAjmBatchJobRunBufferRa);
    DC_HLE("libSceAjm", sceAjmBatchJobRunSplitBufferRa);
    DC_HLE("libSceAjm", sceAjmBatchJobInlineBuffer);
    DC_HLE("libSceAjm", sceAjmBatchStartBuffer);
    DC_HLE("libSceAjm", sceAjmBatchWait);
    DC_HLE("libSceAjm", sceAjmBatchErrorDump);
}

} // namespace Hle::Ajm
