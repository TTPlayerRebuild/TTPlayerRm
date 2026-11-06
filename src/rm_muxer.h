#pragma once
#include "common.h"

namespace ttp::rm {
struct RmAudioSpec {
    DWORD codec{}, sample_rate{}, channels{}, bitrate{}, frame_bytes{}, frame_samples{}, flavor{};
    Bytes extra;
};
// A seekable, single-audio-stream RM writer. Packet offsets and all lengths are
// checked before narrowing to RM's 32-bit fields. No whole-file buffering.
class RmMuxer {
    struct Index { DWORD time, offset, packet; };
    ComPtr<IStream> stream_;
    RmAudioSpec spec_;
    Bytes content_;
    std::vector<Index> index_;
    DWORD packet_count_{}, data_offset_{}, next_index_ms_{};
    bool finished_{};
    Bytes header(DWORD duration, DWORD index_offset, DWORD data_bytes) const;
public:
    RmMuxer(IStream *, RmAudioSpec, const Bytes &content);
    void packet(const BYTE *, size_t);
    void finish(uint64_t logical_samples, const Bytes &metadata);
};
}
