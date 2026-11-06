#include "common.h"
#include "sipr_decoder.h"

namespace ttp::rm {
namespace {
class PacketDecoder final : public SiprPacketDecoder {
    LONG refs_{1};SiprDecoder core_;bool failed_{};
public:
    PacketDecoder(DWORD bitrate,DWORD flags):core_(bitrate,flags&1?SiprDecoder::DelayMode::Legacy5k:SiprDecoder::DelayMode::Compensated){
        require(!*core_.Error(),E_OUTOFMEMORY);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out)override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!same(id,IID_IUnknown)&&!same(id,iid_sipr_packet_decoder))return E_NOINTERFACE;
        *out=static_cast<SiprPacketDecoder*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Format(SiprPacketFormat *out)override {
        if(!out)return E_POINTER;if(out->size<sizeof(*out))return E_INVALIDARG;
        *out={sizeof(*out),core_.SampleRate(),core_.FrameBytes(),core_.FrameSamples(),core_.CoreFrameSamples(),core_.DelaySamples()};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Decode(const BYTE *input,DWORD bytes,DWORD validity,Buffer *output)override {
        if(!output)return E_POINTER;
        return protect([&]()->HRESULT{
            check(output->SetLength(0));require(!failed_,E_UNEXPECTED);require(input||!bytes,E_POINTER);
            const DWORD bits=core_.Bitrate()==5000?148:core_.Bitrate()==6500?116:core_.Bitrate()==16000?160:152;
            const uint64_t frames=uint64_t(bytes)*8/bits;
            require(frames<=32,E_INVALIDARG);
            const auto maximum=DWORD(frames)*core_.CoreFrameSamples()*2;
            DWORD capacity{},unused{};BYTE *data{};
            check(output->Capacity(&capacity));require(capacity>=maximum,HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            check(output->Data(&data,&unused));require(data||!maximum,E_POINTER);
            std::vector<int16_t> pcm;pcm.reserve(maximum/2);
            failed_=true; // Synthesis/publication failures require explicit Reset.
            require(core_.Decode(input,bytes,validity,pcm));
            require(pcm.size()*2<=maximum,E_UNEXPECTED);
            if(!pcm.empty())memcpy(data,pcm.data(),pcm.size()*2);
            check(output->SetLength(DWORD(pcm.size()*2)));failed_=false;return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Reset()override{if(!core_.Reset()){failed_=true;return E_OUTOFMEMORY;}failed_=false;return S_OK;}
};
}
HRESULT make_sipr_packet_decoder(DWORD bitrate,DWORD flags,void **out){
    if(!out)return E_POINTER;*out=nullptr;
    return protect([&]()->HRESULT{
        require((bitrate==5000||bitrate==6500||bitrate==8500||bitrate==16000)&&!(flags&~1u),E_INVALIDARG);
        *out=static_cast<SiprPacketDecoder*>(new PacketDecoder(bitrate,flags));return S_OK;
    });
}
}
