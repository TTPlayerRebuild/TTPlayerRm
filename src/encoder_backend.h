#pragma once
#include "encoder.h"
#include "rm_muxer.h"

namespace ttp::rm {
// Shared streaming contract; individual codec implementations retain their
// own lookahead, framing and tail rules. The host handles PCM conversion/I/O.
class EncoderBackend {
public:
    virtual ~EncoderBackend() = default;
    virtual bool Encode(const int16_t *,size_t,std::vector<uint8_t> &) noexcept = 0;
    virtual bool Finish(std::vector<uint8_t> &) noexcept = 0;
    virtual uint64_t InputFrames() const noexcept = 0;
    virtual const char *Error() const noexcept = 0;
    virtual RmAudioSpec Specification() const = 0;
};
std::unique_ptr<EncoderBackend> make_encoder_backend(EncoderKind,DWORD preset);
}
