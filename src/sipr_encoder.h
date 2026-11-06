#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ttp::rm {
// Independent analysis-by-synthesis SIPR encoder, mono S16.
// 8000 Hz: 8500 bps = 144/19 samples/bytes, 6500 = 288/29, 5000 = 480/37.
// 16000 Hz: 16000 bps = 160 samples/20 bytes.
// No Real codec DLL dependency. Encoder decisions are not Real bit-exact.
class SiprEncoder final {
public:
    explicit SiprEncoder(uint32_t mode) noexcept;
    ~SiprEncoder();
    SiprEncoder(const SiprEncoder&) = delete;
    SiprEncoder& operator=(const SiprEncoder&) = delete;
    bool Encode(const int16_t* pcm, size_t frames,
                std::vector<uint8_t>& packets) noexcept;
    // Idempotent. Supplies analysis lookahead and pads the final packet.
    // Nonempty output: ceil((InputFrames()+FlushPaddingSamples())/FrameSamples()).
    // 5k drains its extra 240 samples by encoding zeros, so an old decoder's
    // existing startup delay cannot discard the last source samples.
    bool Finish(std::vector<uint8_t>& packets) noexcept;
    uint32_t FrameBytes() const noexcept { return mode_ == 16000 ? 20 : mode_ == 5000 ? 37 : mode_ == 6500 ? 29 : 19; }
    uint32_t FrameSamples() const noexcept { return mode_ == 16000 ? 160 : mode_ == 5000 ? 480 : mode_ == 6500 ? 288 : 144; }
    uint32_t SampleRate() const noexcept { return mode_ == 16000 ? 16000 : 8000; }
    uint32_t Channels() const noexcept { return 1; }
    uint32_t Bitrate() const noexcept { return mode_; }
    uint32_t Flavor() const noexcept { return mode_ == 16000 ? 3 : mode_ == 5000 ? 2 : mode_ == 6500 ? 0 : 1; }
    uint32_t DelaySamples() const noexcept { return 0; }
    uint32_t FlushPaddingSamples() const noexcept { return mode_ == 5000 ? 240 : 0; }
    uint64_t InputFrames() const noexcept;
    const char* Error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const char* startup_error_ = "";
    uint32_t mode_ = 0;
};
}
