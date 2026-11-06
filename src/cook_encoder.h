#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ttp::rm {
// Independently written Cook encoder using LGPL FFmpeg transforms/codebooks.
// Profiles0..33: mono, independent/joint stereo, 5.0 and 5.1;
// interleaved S16 at SampleRate(). Multichannel order follows Windows masks
// 0x37 (FL FR FC BL BR) and 0x3f (FL FR FC LFE BL BR).
// Profiles6/14 have identical coding parameters (original music/speech names).
// Profiles27..29 retain the original coding parameters with more independent
// low-frequency bands; no additional Surround preprocessing is implemented.
// Appends complete Cook packets, including Cook's mandatory 37c511f2 XOR.
// Packets can be written directly to RM; do not apply ATRAC scrambling.
class CookEncoder final {
public:
    explicit CookEncoder(uint32_t flavor = 0) noexcept;
    ~CookEncoder();
    CookEncoder(const CookEncoder&) = delete;
    CookEncoder& operator=(const CookEncoder&) = delete;
    bool Encode(const int16_t* interleaved, size_t frames,
                std::vector<uint8_t>& packets) noexcept;
    // Idempotent. Nonempty M-frame input emits ceil(M/N)+3 packets, N=FrameSamples().
    // Original Reader returns ceil(M/N)*N aligned PCM frames; the modern
    // decoder also publishes one final block driven by zero PCM. Lossy
    // transform quantization can leave a short synthesis tail in that block.
    bool Finish(std::vector<uint8_t>& packets) noexcept;
    uint32_t FrameBytes() const noexcept;
    uint32_t FrameSamples() const noexcept;
    uint32_t SampleRate() const noexcept;
    uint32_t Channels() const noexcept;
    uint32_t Bitrate() const noexcept;
    // Packet-level fixed decoder priming (3*N), already supplied internally.
    // Do not prepend PCM or skip these samples again; Cook extra has no delay.
    uint32_t DelaySamples() const noexcept;
    uint64_t InputFrames() const noexcept;
    const std::vector<uint8_t>& ExtraData() const noexcept;
    const char* Error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const char* startup_error_ = "";
};
}
