#include "atrac3_encoder.h"
#include "atrac3denc.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <limits>
#include <stdexcept>
#include <string>

namespace ttp::rm {
namespace {
constexpr uint32_t kBitrates[] = {66150, 93713, 104738, 132300, 146081, 176400, 264600, 352800};
constexpr uint32_t kFrameBytes[] = {192, 272, 304, 384, 424, 512, 768, 1024};
// RealAudio's per-coding-channel bandwidth policy. Joint stereo operates in
// M/S order; independent stereo uses L/R. The five Surround names use the
// same parameters as the corresponding 132..352 kbps stereo presets.
constexpr uint32_t kBandwidthHz[][2] = {
    {12400, 3100}, {15100, 7500}, {13700, 13700}, {16500, 16500},
    {16500, 16500}, {19200, 19200}, {22000, 22000}, {22000, 22000}
};
uint16_t BandwidthBfus(uint32_t hz) {
    // Original conversion: floor(hz * 2048 / 44100), then the first BFU
    // whose exclusive upper spectral boundary is >= that integer index.
    using NAtracDEnc::NAtrac3::TAtrac3Data;
    const uint32_t coefficient = hz * 2048 / 44100;
    uint16_t count = 1;
    while (count < TAtrac3Data::MaxBfus && TAtrac3Data::BlockSizeTab[count] < coefficient)
        ++count;
    return count;
}
// AtracDEnc's QMF/MDCT delay is 1162. Prefixing 1028 silent samples produces
// the standard RealAudio ATRAC3 delay 2190 without discarding source samples.
constexpr size_t kPrefixFrames = 1028;
class ScopedRounding {
    int previous_;
public:
    ScopedRounding() : previous_(std::fegetround()) { std::fesetround(FE_TONEAREST); }
    ~ScopedRounding() { if (previous_ != -1) std::fesetround(previous_); }
};
}

struct Atrac3Encoder::Impl {
    struct Output final : ICompressedOutput {
        Impl& owner;
        explicit Output(Impl& value) : owner(value) {}
        std::string GetName() const override { return {}; }
        size_t GetChannelNum() const override { return 2; }
        void WriteFrame(std::vector<char> data) override {
            if (!owner.destination || data.size() != owner.frame_bytes)
                throw std::runtime_error("invalid ATRAC3 output frame");
            owner.destination->insert(owner.destination->end(), data.begin(), data.end());
            ++owner.output_frames;
        }
    };
    uint32_t bitrate, frame_bytes, coding_mode;
    uint64_t input_frames = 0, output_frames = 0;
    size_t buffered = 0;
    bool started = false, finished = false;
    const char* error = "";
    std::array<float, 2048> pcm{};
    std::vector<uint8_t>* destination = nullptr;
    std::unique_ptr<NAtracDEnc::TAtrac3Encoder> encoder;
    TPCMEngine::TProcessLambda process;

    explicit Impl(size_t mode)
        : bitrate(kBitrates[mode]), frame_bytes(kFrameBytes[mode]),
          coding_mode(mode < 2 ? 0x12u : 2u) {
        ScopedRounding rounding;
        NAtracDEnc::NAtrac3::TAtrac3EncoderSettings settings(bitrate, false, false, 2, 0);
        settings.MaxBfus[0] = BandwidthBfus(kBandwidthHz[mode][0]);
        settings.MaxBfus[1] = BandwidthBfus(kBandwidthHz[mode][1]);
        encoder.reset(new NAtracDEnc::TAtrac3Encoder(
            TCompressedOutputPtr(new Output(*this)), std::move(settings)));
        process = encoder->GetLambda();
    }
    void Block() {
        process(pcm.data(), TPCMEngine::ProcessMeta{2});
        buffered = 0;
    }
    void Silence(size_t frames) {
        while (frames) {
            const size_t count = std::min(frames, size_t(1024) - buffered);
            std::fill_n(pcm.data() + buffered * 2, count * 2, 0.0f);
            buffered += count;
            frames -= count;
            if (buffered == 1024) Block();
        }
    }
};

Atrac3Encoder::Atrac3Encoder(uint32_t bitrate, uint32_t channels) noexcept {
    if (channels != 2) { startup_error_ = "ATRAC3 backend requires stereo PCM"; return; }
    size_t mode = 0;
    while (mode < 8 && kBitrates[mode] != bitrate) ++mode;
    if (mode == 8) { startup_error_ = "unsupported ATRAC3 bitrate"; return; }
    try { impl_.reset(new Impl(mode)); }
    catch (...) { startup_error_ = "cannot initialize ATRAC3 encoder"; }
}
Atrac3Encoder::~Atrac3Encoder() = default;

bool Atrac3Encoder::Encode(const int16_t* data, size_t frames,
                          std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    auto& s = *impl_;
    if (s.finished) { s.error = "ATRAC3 encoder already finished"; return false; }
    if ((!data && frames) || frames > std::numeric_limits<size_t>::max() / 2 ||
        frames > std::numeric_limits<uint64_t>::max() - s.input_frames - 3214) {
        s.error = "invalid ATRAC3 PCM input"; return false;
    }
    if (!frames) return true;
    const size_t initial_size = packets.size();
    try {
        ScopedRounding rounding;
        s.destination = &packets;
        if (!s.started) { s.started = true; s.Silence(kPrefixFrames); }
        const size_t submitted = frames;
        while (frames) {
            const size_t count = std::min(frames, size_t(1024) - s.buffered);
            for (size_t i = 0; i < count * 2; ++i)
                s.pcm[s.buffered * 2 + i] = data[i] * (1.0f / 32768.0f);
            s.buffered += count;
            data += count * 2;
            frames -= count;
            if (s.buffered == 1024) s.Block();
        }
        s.input_frames += submitted;
        s.destination = nullptr;
        return true;
    } catch (...) {
        s.destination = nullptr;
        packets.resize(initial_size);
        s.error = "ATRAC3 encoding failed";
        return false;
    }
}

bool Atrac3Encoder::Finish(std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    auto& s = *impl_;
    if (s.finished) return true;
    const size_t initial_size = packets.size();
    try {
        ScopedRounding rounding;
        s.destination = &packets;
        if (s.input_frames) {
            const uint64_t target = (s.input_frames + DelaySamples() + 1023) / 1024;
            if (s.buffered) s.Silence(1024 - s.buffered);
            while (s.output_frames < target) s.Silence(1024);
        }
        s.finished = true;
        s.destination = nullptr;
        return true;
    } catch (...) {
        s.destination = nullptr;
        packets.resize(initial_size);
        s.error = "ATRAC3 drain failed";
        return false;
    }
}
uint32_t Atrac3Encoder::FrameBytes() const noexcept { return impl_ ? impl_->frame_bytes : 0; }
uint32_t Atrac3Encoder::Bitrate() const noexcept { return impl_ ? impl_->bitrate : 0; }
uint32_t Atrac3Encoder::CodingMode() const noexcept { return impl_ ? impl_->coding_mode : 0; }
uint64_t Atrac3Encoder::InputFrames() const noexcept { return impl_ ? impl_->input_frames : 0; }
const char* Atrac3Encoder::Error() const noexcept { return impl_ ? impl_->error : startup_error_; }
}
