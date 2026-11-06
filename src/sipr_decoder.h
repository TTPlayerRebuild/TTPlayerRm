#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
namespace ttp::rm {
class SiprDecoder final {
public:
    enum class DelayMode { Compensated, Legacy5k };
    explicit SiprDecoder(uint32_t bitrate, DelayMode delay = DelayMode::Compensated) noexcept;
    ~SiprDecoder();
    SiprDecoder(const SiprDecoder&) = delete;
    SiprDecoder& operator=(const SiprDecoder&) = delete;
    // Appends PCM. Bitmap is LSB-first per core frame (2/packet at 5k/6.5k).
    // The original caps a run of missing core frames to five output frames.
    // Partial final core bits are ignored, not retained across calls.
    // Compensated 5k emits each frame immediately; Legacy5k retains one
    // parameter frame, reproducing original cross-call lookahead and delay.
    // Input is limited to 1 MiB per call. After failure, discard that call's
    // appended PCM and call Reset before decoding again. Reset drops pending
    // 5k parameters; a following encoded frame is needed to drain Legacy5k.
    bool Decode(const uint8_t* data, size_t bytes, uint32_t validity,
                std::vector<int16_t>& pcm) noexcept;
    bool Reset() noexcept;
    uint32_t FrameBytes() const noexcept;
    uint32_t FrameSamples() const noexcept;
    uint32_t CoreFrameSamples() const noexcept;
    uint32_t SampleRate() const noexcept { return mode_ == 16000 ? 16000 : 8000; }
    uint32_t Bitrate() const noexcept { return mode_; }
    uint32_t Channels() const noexcept { return 1; }
    uint32_t DelaySamples() const noexcept { return mode_ == 5000 && delay_ == DelayMode::Legacy5k ? 240 : 0; }
    const char* Error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t mode_;
    DelayMode delay_;
    const char* error_ = "";
};
}
