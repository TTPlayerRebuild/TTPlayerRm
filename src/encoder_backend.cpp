#include "encoder_backend.h"
#include "atrac3_encoder.h"
#include "cook_encoder.h"
#include "sipr_encoder.h"

namespace ttp::rm {
namespace {
class AtracBackend final:public EncoderBackend {
    Atrac3Encoder core_;
public:
    explicit AtracBackend(DWORD preset):core_(preset,2){}
    bool Encode(const int16_t *p,size_t n,std::vector<uint8_t>&b)noexcept override{return core_.Encode(p,n,b);}
    bool Finish(std::vector<uint8_t>&b)noexcept override{return core_.Finish(b);}
    uint64_t InputFrames()const noexcept override{return core_.InputFrames();}
    const char *Error()const noexcept override{return core_.Error();}
    RmAudioSpec Specification()const override {
        RmAudioSpec spec{fourcc('a','t','r','c'),44100,2,core_.Bitrate(),core_.FrameBytes(),1024,0,{}};
        spec.extra={0,0,0,4,8,0,BYTE(core_.DelaySamples()>>8),BYTE(core_.DelaySamples()),0,BYTE(core_.CodingMode())};
        for(DWORD i=0;i<8;++i)if(atrac_bitrates[i]==spec.bitrate)spec.flavor=i;
        return spec;
    }
};
class CookBackend final:public EncoderBackend {
    DWORD flavor_;
    CookEncoder core_;
public:
    explicit CookBackend(DWORD preset):flavor_(preset),core_(preset){}
    bool Encode(const int16_t *p,size_t n,std::vector<uint8_t>&b)noexcept override{return core_.Encode(p,n,b);}
    bool Finish(std::vector<uint8_t>&b)noexcept override{return core_.Finish(b);}
    uint64_t InputFrames()const noexcept override{return core_.InputFrames();}
    const char *Error()const noexcept override{return core_.Error();}
    RmAudioSpec Specification()const override {
        return {fourcc('c','o','o','k'),core_.SampleRate(),core_.Channels(),core_.Bitrate(),
                core_.FrameBytes(),core_.FrameSamples(),flavor_,core_.ExtraData()};
    }
};
class SiprBackend final:public EncoderBackend {
    SiprEncoder core_;
public:
    explicit SiprBackend(DWORD preset):core_(preset){}
    bool Encode(const int16_t *p,size_t n,std::vector<uint8_t>&b)noexcept override{return core_.Encode(p,n,b);}
    bool Finish(std::vector<uint8_t>&b)noexcept override{return core_.Finish(b);}
    uint64_t InputFrames()const noexcept override{return core_.InputFrames();}
    const char *Error()const noexcept override{return core_.Error();}
    RmAudioSpec Specification()const override {
        // GENR h=1 accepts individual SIPR frames in both TTPlayer Readers;
        // no 48-frame nibble-interleave group or extra group padding is needed.
        return {fourcc('s','i','p','r'),core_.SampleRate(),core_.Channels(),core_.Bitrate(),
                core_.FrameBytes(),core_.FrameSamples(),core_.Flavor(),{}};
    }
};
}
std::unique_ptr<EncoderBackend> make_encoder_backend(EncoderKind kind,DWORD preset) {
    if(kind==EncoderKind::Atrac3)return std::make_unique<AtracBackend>(preset);
    if(kind==EncoderKind::Cook)return std::make_unique<CookBackend>(preset);
    if(kind==EncoderKind::Sipr)return std::make_unique<SiprBackend>(preset);
    throw Failure{E_NOTIMPL};
}
}
