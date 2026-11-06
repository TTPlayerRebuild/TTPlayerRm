#include "encoder.h"
#include "encoder_backend.h"
#include "native_metadata.h"
#include "rm_muxer.h"
#include <cmath>
#include <ks.h>
#include <ksmedia.h>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace ttp::rm {
namespace {
DWORD standard_mask(unsigned channels) {
    constexpr DWORD masks[]{0,4,3,7,0x33,0x37,0x3f,0x70f,0x63f};
    require(channels>0&&channels<std::size(masks),E_NOTIMPL);return masks[channels];
}
WAVEFORMATEX validate(const WAVEFORMATEX *input,DWORD &mask) {
    require(input,E_POINTER); auto f=*input;mask=0;
    if(f.wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        require(f.cbSize>=22,E_INVALIDARG); const auto *e=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(input);
        if(same(e->SubFormat,KSDATAFORMAT_SUBTYPE_PCM))f.wFormatTag=WAVE_FORMAT_PCM;
        else if(same(e->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))f.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;
        else throw Failure{E_NOTIMPL};
        require(!e->Samples.wValidBitsPerSample||e->Samples.wValidBitsPerSample<=f.wBitsPerSample,E_INVALIDARG);
        mask=e->dwChannelMask;
    }
    require(f.nChannels>=1&&f.nChannels<=8&&f.nSamplesPerSec>=8000&&f.nSamplesPerSec<=384000,E_NOTIMPL);
    if(!mask)mask=standard_mask(f.nChannels);
    require(!(mask&~0x3ffffu),E_NOTIMPL);
    unsigned count=0;for(DWORD bits=mask;bits;bits&=bits-1)++count;require(count==f.nChannels,E_INVALIDARG);
    if(f.nChannels<=2)require(mask==standard_mask(f.nChannels),E_NOTIMPL);
    require((f.wFormatTag==WAVE_FORMAT_PCM&&(f.wBitsPerSample==8||f.wBitsPerSample==16||f.wBitsPerSample==24||f.wBitsPerSample==32))||
            (f.wFormatTag==WAVE_FORMAT_IEEE_FLOAT&&(f.wBitsPerSample==32||f.wBitsPerSample==64)),E_NOTIMPL);
    require(f.nBlockAlign==f.nChannels*(f.wBitsPerSample/8)&&f.nAvgBytesPerSec==f.nSamplesPerSec*f.nBlockAlign,E_INVALIDARG);
    f.cbSize=0;return f;
}
float normalized(const BYTE *p,const WAVEFORMATEX &f) {
    double v{};
    if(f.wFormatTag==WAVE_FORMAT_IEEE_FLOAT) {
        if(f.wBitsPerSample==64)std::memcpy(&v,p,8);
        else { float x{};std::memcpy(&x,p,4);v=x; }
    }else switch(f.wBitsPerSample) {
    case 8:v=(int(*p)-128)/128.0;break;
    case 16:{int16_t x{};std::memcpy(&x,p,2);v=x/32768.0;break;}
    case 24:{int32_t x=int32_t(uint32_t(p[0])<<8|uint32_t(p[1])<<16|uint32_t(p[2])<<24);v=x/2147483648.0;break;}
    case 32:{int32_t x{};std::memcpy(&x,p,4);v=x/2147483648.0;break;}
    }
    return std::isfinite(v)?float(std::clamp(v,-1.0,1.0)):0.0f;
}
Bytes cont(const NativeMetadata &metadata) {
    Bytes result;
    for(const char *key:{"Title","Artist","Copyright","Comment"}) {
        Bytes value;
        for(const auto &t:metadata.tags)if(!_stricmp(t.key.c_str(),key)){value=legacy_bytes(t.value);value.pop_back();break;}
        require(value.size()<=65535,E_INVALIDARG);result.push_back(BYTE(value.size()>>8));result.push_back(BYTE(value.size()));
        result.insert(result.end(),value.begin(),value.end());
    }
    return result;
}
class Instance final:public StreamEncoder,public Metadata,public Thumbnail {
    LONG refs_{1};
    enum class State { Fresh,Open,Active,Finished,Failed } state_{State::Fresh};
    HRESULT failure_{S_OK};
    WAVEFORMATEX format_{};
    DWORD input_mask_{},conversion_channels_{};
    bool layout_mix_{};
    ComPtr<IStream> stream_;
    NativeMetadata metadata_;
    EncoderKind kind_;
    std::unique_ptr<EncoderBackend> encoder_;
    RmAudioSpec spec_;
    std::unique_ptr<RmMuxer> mux_;
    SwrContext *resampler_{};
    DWORD preset_{};
    uint64_t input_samples_{},output_samples_{};
    Bytes packets_;
    std::vector<float> input_;
    std::vector<int16_t> pcm_;
    void release_engine() { mux_.reset();encoder_.reset();swr_free(&resampler_); }
    void packetize(DWORD *written) {
        const auto bytes=spec_.frame_bytes;require(bytes&&packets_.size()%bytes==0,E_UNEXPECTED);
        constexpr BYTE xor_key[]{0x53,0x7f,0x61,0x03};
        for(size_t offset=0;offset<packets_.size();offset+=bytes) {
            if(kind_==EncoderKind::Atrac3)for(size_t i=0;i<bytes;++i)packets_[offset+i]^=xor_key[i&3];
            mux_->packet(packets_.data()+offset,bytes);
            if(written){require(*written<=MAXDWORD-bytes-12,E_UNEXPECTED);*written+=bytes+12;}
        }
        packets_.clear();
    }
    void convert(const float *input,int frames,DWORD *written,uint64_t limit=UINT64_MAX) {
        int capacity=swr_get_out_samples(resampler_,frames);require(capacity>=0&&capacity<=1024*1024,E_UNEXPECTED);
        capacity=std::max(capacity,64);pcm_.resize(size_t(capacity)*spec_.channels);
        uint8_t *output=reinterpret_cast<uint8_t*>(pcm_.data());const uint8_t *source=reinterpret_cast<const uint8_t*>(input);
        int n=swr_convert(resampler_,&output,capacity,input?&source:nullptr,frames);require(n>=0,E_FAIL);
        require(output_samples_<=limit,E_UNEXPECTED);n=int(std::min<uint64_t>(unsigned(n),limit-output_samples_));
        if(n){output_samples_+=unsigned(n);require(encoder_->Encode(pcm_.data(),size_t(n),packets_),E_FAIL);packetize(written);}
        else pcm_.clear();
    }
public:
    explicit Instance(EncoderKind kind):kind_(kind){}
    ~Instance(){if(state_==State::Active)Finish();release_engine();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out)override {
        if(!out)return E_POINTER;*out=nullptr;
        if(same(id,IID_IUnknown)||same(id,iid_encoder)||same(id,iid_stream_encoder))*out=static_cast<StreamEncoder*>(this);
        else if(same(id,iid_metadata))*out=static_cast<Metadata*>(this);
        else if(same(id,iid_thumbnail))*out=static_cast<Thumbnail*>(this);else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Open(const wchar_t *path,WAVEFORMATEX *format)override {
        return protect([&]()->HRESULT {
            require(state_==State::Fresh,E_UNEXPECTED);require(path&&*path,E_INVALIDARG);DWORD mask{};auto validated=validate(format,mask);const auto preset=encoder_preset(kind_);
            ComPtr<IStream> target;check(SHCreateStreamOnFileEx(path,STGM_CREATE|STGM_READWRITE|STGM_SHARE_DENY_WRITE,0,TRUE,nullptr,target.put()));
            stream_=std::move(target);format_=validated;input_mask_=mask;preset_=preset;state_=State::Open;return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE OpenStream(IStream *stream,WAVEFORMATEX *format)override {
        return protect([&]()->HRESULT {
            require(state_==State::Fresh,E_UNEXPECTED);require(stream,E_POINTER);DWORD mask{};auto validated=validate(format,mask);require(tell(stream)==0,E_INVALIDARG);
            preset_=encoder_preset(kind_);stream_=ComPtr<IStream>(stream,true);format_=validated;input_mask_=mask;state_=State::Open;return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Start()override {
        if(state_==State::Active)return S_OK;if(state_==State::Failed)return failure_;
        auto result=protect([&]()->HRESULT {
            require(state_==State::Open,E_UNEXPECTED);encoder_=make_encoder_backend(kind_,preset_);require(!*encoder_->Error(),E_FAIL);
            spec_=encoder_->Specification();require(spec_.sample_rate&&spec_.channels>=1&&spec_.channels<=8,E_UNEXPECTED);
            layout_mix_=format_.nChannels>2||spec_.channels>2;
            conversion_channels_=layout_mix_?format_.nChannels:spec_.channels;
            AVChannelLayout output_layout{},input_layout{};
            // Cook 5.0/5.1 uses FL/FR/FC[/LFE]/BL/BR. Explicit side-channel
            // input is declared to swresample, which maps side to back; it is
            // never silently reinterpreted. Legacy cbSize=0 uses the standard
            // Windows ordering, as supplied by the original conversion host.
            require(av_channel_layout_from_mask(&output_layout,standard_mask(spec_.channels))>=0,E_INVALIDARG);
            require(av_channel_layout_from_mask(&input_layout,layout_mix_?input_mask_:standard_mask(spec_.channels))>=0,E_INVALIDARG);
            const int allocated=swr_alloc_set_opts2(&resampler_,&output_layout,AV_SAMPLE_FMT_S16,int(spec_.sample_rate),&input_layout,AV_SAMPLE_FMT_FLT,int(format_.nSamplesPerSec),0,nullptr);
            av_channel_layout_uninit(&output_layout);av_channel_layout_uninit(&input_layout);require(allocated>=0,E_OUTOFMEMORY);
            require(swr_init(resampler_)>=0,E_FAIL);
            mux_=std::make_unique<RmMuxer>(stream_.p,spec_,cont(metadata_));state_=State::Active;return S_OK;
        });
        if(FAILED(result)&&state_==State::Open){state_=State::Failed;failure_=result;release_engine();}return result;
    }
    HRESULT STDMETHODCALLTYPE Write(Buffer *buffer,DWORD *written)override {
        if(written)*written=0;if(state_==State::Failed)return failure_;
        auto result=protect([&]()->HRESULT {
            require(state_==State::Active,E_UNEXPECTED);require(buffer,E_POINTER);BYTE *source{};DWORD size{};check(buffer->Data(&source,&size));
            DWORD capacity{};check(buffer->Capacity(&capacity));require(size<=capacity,E_INVALIDARG);
            require(!size||source,E_POINTER);require(size%format_.nBlockAlign==0,E_INVALIDARG);
            while(size) {
                DWORD frames=std::min<DWORD>(size/format_.nBlockAlign,4096);input_.resize(size_t(frames)*conversion_channels_);const DWORD sample_bytes=format_.wBitsPerSample/8;
                for(DWORD i=0;i<frames;++i){
                    const auto *p=source+size_t(i)*format_.nBlockAlign;
                    if(layout_mix_){for(DWORD c=0;c<conversion_channels_;++c)input_[size_t(i)*conversion_channels_+c]=normalized(p+size_t(c)*sample_bytes,format_);}
                    else {
                        const float left=normalized(p,format_),right=normalized(p+(format_.nChannels==2?sample_bytes:0),format_);
                        if(spec_.channels==1)input_[i]=(left+right)*0.5f;
                        else {input_[size_t(i)*2]=left;input_[size_t(i)*2+1]=right;}
                    }
                }
                require(input_samples_<=UINT64_MAX-frames,E_UNEXPECTED);input_samples_+=frames;
                convert(input_.data(),int(frames),written);const DWORD bytes=frames*format_.nBlockAlign;source+=bytes;size-=bytes;
            }
            return S_OK;
        });
        if(FAILED(result)&&state_==State::Active){state_=State::Failed;failure_=result;release_engine();}return result;
    }
    HRESULT STDMETHODCALLTYPE Finish()override {
        if(state_==State::Finished)return S_OK;if(state_==State::Failed)return failure_;
        auto result=protect([&]()->HRESULT {
            require(state_==State::Active,E_UNEXPECTED);
            const auto rate=format_.nSamplesPerSec;
            const uint64_t tail=(input_samples_%rate*spec_.sample_rate+rate-1)/rate;
            require(input_samples_/rate<=(UINT64_MAX-tail)/spec_.sample_rate,E_UNEXPECTED);
            const uint64_t target=input_samples_/rate*spec_.sample_rate+tail;
            require(output_samples_<=target,E_UNEXPECTED);
            // swresample cannot initialize its reflected FIR history when a
            // complete clip is shorter than its filter. Null-input flushing
            // alone then returns zero forever. Supply a silent end extension,
            // but emit only the exact ceil(input_frames*output_rate/input_rate)
            // logical samples, so padding never changes duration or tail size.
            // 4096 source frames cover the filter even at 384 kHz -> 8 kHz.
            if(output_samples_<target){input_.assign(size_t(4096)*conversion_channels_,0.0f);convert(input_.data(),4096,nullptr,target);}
            while(output_samples_<target){const auto before=output_samples_;convert(nullptr,0,nullptr,target);require(output_samples_>before,E_FAIL);}
            require(encoder_->Finish(packets_),E_FAIL);packetize(nullptr);require(encoder_->InputFrames()==output_samples_,E_UNEXPECTED);
            mux_->finish(output_samples_,metadata_.dirty?metadata_.output():Bytes{});state_=State::Finished;return S_OK;
        });
        if(FAILED(result)&&state_==State::Active){state_=State::Failed;failure_=result;}
        release_engine();return result;
    }
    HRESULT STDMETHODCALLTYPE Extension(wchar_t **out)override{return text(L"rm",out);}
    HRESULT STDMETHODCALLTYPE Count(DWORD *out)override{if(!out)return E_POINTER;*out=DWORD(metadata_.tags.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE At(DWORD index,wchar_t **key,wchar_t **value)override {
        if(key)*key=nullptr;if(value)*value=nullptr;return protect([&]()->HRESULT{
            require(key&&value,E_POINTER);require(index<metadata_.tags.size(),E_INVALIDARG);const auto &t=metadata_.tags[index];check(text(wide(t.key),key));auto hr=text(t.value,value);if(FAILED(hr)){CoTaskMemFree(*key);*key=nullptr;}return hr;
        });
    }
    HRESULT STDMETHODCALLTYPE Get(const char *key,wchar_t **value)override {
        if(value)*value=nullptr;if(!key||!value)return E_POINTER;for(const auto &t:metadata_.tags)if(!_stricmp(t.key.c_str(),key))return text(t.value,value);return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE Set(const char *key,const wchar_t *value)override {
        return protect([&]()->HRESULT{
            require(state_==State::Fresh||state_==State::Open,E_UNEXPECTED);require(key&&*key,E_POINTER);require(!value||wcslen(value)<=8192,E_INVALIDARG);
            auto next=metadata_;next.set(key,value);require(next.tags.size()<=256,E_INVALIDARG);metadata_=std::move(next);return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE PictureCount(DWORD *out)override{if(!out)return E_POINTER;*out=metadata_.cover.empty()?0:1;return S_OK;}
    HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD *out)override{if(!out)return E_POINTER;*out=60000;return S_OK;}
    HRESULT STDMETHODCALLTYPE MaximumCount(DWORD *out)override{if(!out)return E_POINTER;*out=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE PictureAt(DWORD n,const Picture **out)override {
        if(!out)return E_POINTER;*out=nullptr;if(n||metadata_.cover.empty())return E_INVALIDARG;
        metadata_.view={sizeof(Picture),metadata_.mime.c_str(),metadata_.description.c_str(),DWORD(metadata_.cover.size()),metadata_.cover.data(),3};
        *out=&metadata_.view;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD n,const Picture *p,DWORD mask)override {
        return protect([&]()->HRESULT{
            require(state_==State::Fresh||state_==State::Open,E_UNEXPECTED);require(p,E_POINTER);
            require(p->size>=sizeof(Picture)&&!(mask&~15u),E_INVALIDARG);auto next=metadata_;require(!n&&!next.cover.empty(),E_INVALIDARG);
            Picture merged{sizeof(Picture),(mask&1)?p->mime:next.mime.c_str(),(mask&2)?p->description:next.description.c_str(),
                (mask&8)?p->bytes:DWORD(next.cover.size()),(mask&8)?p->data:next.cover.data(),3};
            next.picture(&merged);metadata_=std::move(next);return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE AddPicture(const Picture *p)override {
        return protect([&]()->HRESULT{
            require(state_==State::Fresh||state_==State::Open,E_UNEXPECTED);require(p,E_POINTER);auto next=metadata_;
            require(next.cover.empty(),E_INVALIDARG);next.picture(p);metadata_=std::move(next);return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE RemovePicture(DWORD n)override {
        return protect([&]()->HRESULT{
            require(state_==State::Fresh||state_==State::Open,E_UNEXPECTED);auto next=metadata_;require(!n&&!next.cover.empty(),E_INVALIDARG);
            next.picture(nullptr);metadata_=std::move(next);return S_OK;
        });
    }
};
}
HRESULT make_encoder(void **out,EncoderKind kind) {if(!out)return E_POINTER;*out=nullptr;return protect([&]()->HRESULT{*out=static_cast<StreamEncoder*>(new Instance(kind));return S_OK;});}
}
