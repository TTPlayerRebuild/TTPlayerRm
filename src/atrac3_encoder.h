#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ttp::rm {
// Independent LGPL AtracDEnc backend. Input is 44100 Hz interleaved stereo S16.
// Output appends complete, UNSCRAMBLED ATRAC3 frames; the RM muxer scrambles them.
// Supported exact bitrates: 66150, 93713, 104738, 132300, 146081, 176400,
// 264600, 352800. This does not reproduce Sony/Real encoder decisions.
class Atrac3Encoder final {
public:
    explicit Atrac3Encoder(uint32_t bitrate, uint32_t channels = 2) noexcept;
    ~Atrac3Encoder();
    Atrac3Encoder(const Atrac3Encoder&) = delete;
    Atrac3Encoder& operator=(const Atrac3Encoder&) = delete;
    bool Encode(const int16_t* interleaved, size_t frames,
                std::vector<uint8_t>& packets) noexcept;
    // Idempotent. Pads the final input block and drains filter/lookahead state.
    // Nonempty input emits ceil((InputFrames()+DelaySamples())/1024) frames.
    // The decoded tail has 0..1023 padding samples, never missing source audio.
    bool Finish(std::vector<uint8_t>& packets) noexcept;
    uint32_t FrameBytes() const noexcept;
    uint32_t Bitrate() const noexcept;
    uint32_t CodingMode() const noexcept; // RealAudio 0x12 joint / 0x02 separate
    uint32_t DelaySamples() const noexcept { return 2190; }
    uint64_t InputFrames() const noexcept;
    const char* Error() const noexcept; // empty on success; failures are sticky
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const char* startup_error_ = "";
};
}
