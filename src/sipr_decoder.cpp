// Four-mode parameter decoder and RA bitmap scheduling reconstructed from
// 614a26e0, 614a3a90 and 614a30a0. DSP uses the patched LGPL FFmpeg backend.
// No Real DLL is loaded. PRNG and prediction histories are per instance.
#include "sipr_decoder.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <new>
#define TTP_RM_LEGACY_SIPR 1
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/sipr.h"
#include "libavcodec/sipr_legacylow_tables.h"
}
namespace ttp::rm {
static_assert(sizeof(((SiprContext*)nullptr)->highpass_filt_mem) == 4*sizeof(float),
              "SIPR requires the matching RM compatibility backend");
static_assert(sizeof(((SiprContext*)nullptr)->plc_hz) == 16*sizeof(float),
              "SIPR requires bitmap/PLC context support");
namespace {
struct ScopedRounding { int old=std::fegetround();ScopedRounding(){std::fesetround(FE_TONEAREST);}~ScopedRounding(){std::fesetround(old);} };
struct Bits {
    const uint8_t* p;size_t bit=0;
    int Get(unsigned n){int v=0;for(unsigned k=0;k<n;++k,++bit)v|=((p[bit/8]>>(bit%8))&1)<<k;return v;}
};
SiprParameters Read(Bits& b,uint32_t mode){
    SiprParameters p{};bool wide=mode==16000;
    const unsigned low[5]={6,7,7,7,5},high[5]={7,8,7,7,7};
    p.ma_pred_switch=b.Get(wide?1:0);
    for(int k=0;k<5;++k)p.vq_indexes[k]=b.Get(wide?high[k]:low[k]);
    for(int sf=0;sf<(wide?2:mode==5000?5:3);++sf){
        p.pitch_delay[sf]=b.Get(wide?(sf?6:9):(!sf||(sf==2&&mode==5000)?8:5));
        p.gp_index[sf]=b.Get(wide?4:0);
        for(int k=0;k<(wide?10:mode==5000?1:3);++k)
            p.fc_indexes[sf][k]=(int16_t)b.Get(wide?(k%2?5:4):mode==5000?10:mode==6500?5:9);
        p.gc_index[sf]=b.Get(wide?5:7);
    }return p;
}
}
struct SiprDecoder::Impl {
    AVCodecContext* av=nullptr;SiprContext* ctx=nullptr;
    bool startup=true;SiprParameters pending{};int pending_error=2, previous_error=2;
    uint32_t mode;DelayMode delay;
    Impl(uint32_t m,DelayMode d):mode(m),delay(d){
        av=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_SIPR));
        if(!av)throw std::bad_alloc();
        av->block_align=m==5000?37:m==6500?29:m==16000?20:19;av->bit_rate=m;av->sample_rate=m==16000?16000:8000;
        if(avcodec_open2(av,avcodec_find_decoder(AV_CODEC_ID_SIPR),nullptr)<0){avcodec_free_context(&av);throw std::bad_alloc();}
        ctx=(SiprContext*)av->priv_data;ctx->plc_enabled=1;ctx->plc_seed=21845;ctx->plc_pitch3=180;
        if(m!=5000)std::memcpy(ctx->plc_hz,low_mean,sizeof(low_mean));
        if(m==16000){ctx->plc_seed=1;for(int i=0;i<16;++i)ctx->plc_hz[i]=(float)((i+1)*8000.0/17);}
        if(m==5000){for(int n=0;n<5;++n)ctx->plc_seed=(uint16_t)(ctx->plc_seed*0x7c4du+0x3619u);ctx->plc_pitch3=195;}
    }
    ~Impl(){avcodec_free_context(&av);}
    void Frame(SiprParameters& input,int error,std::vector<int16_t>& pcm){
        SiprParameters p=input;
        int count=mode==16000?160:mode==5000?240:144;
        // At lag 284 the first subframe's 20-tap interpolation would read
        // before FFmpeg's 292-sample history. Original increments without a
        // bound. Report this condition instead of emulating undefined reads.
        if(mode==16000&&error&&ctx->pitch_lag_prev>=283)throw "SIPR erasure pitch exceeds history";
        float samples[240]{};
        if(mode==5000&&delay==DelayMode::Legacy5k){
            if(startup){startup=false;pending=p;pending_error=error;pcm.insert(pcm.end(),240,0);return;}
            auto next=p;int next_error=error;
            p=pending;error=pending_error;
            if(error){error=2;if(!next_error&&!previous_error){error=1;p=next;}}
            pending=next;pending_error=next_error;previous_error=error;
        }
        ctx->plc_error=error;ctx->decode_frame(ctx,&p,samples);ctx->plc_error=0;
        if(mode==16000)input=p;
        for(int n=0;n<count;++n){double v=(double)samples[n]*32768+.5;if(!std::isfinite(v))throw "non-finite SIPR synthesis";int value=v>=32767?32767:v<=-32768?-32768:(int)v;pcm.push_back((int16_t)value);}
    }
};
SiprDecoder::SiprDecoder(uint32_t m,DelayMode d)noexcept:mode_(m),delay_(d){Reset();}
SiprDecoder::~SiprDecoder()=default;
bool SiprDecoder::Reset()noexcept{
    ScopedRounding rounding;
    if(mode_!=5000&&mode_!=6500&&mode_!=8500&&mode_!=16000){error_="unsupported SIPR bitrate";return false;}
    if(delay_!=DelayMode::Compensated&&delay_!=DelayMode::Legacy5k){error_="invalid SIPR delay mode";return false;}
    try{auto next=std::make_unique<Impl>(mode_,delay_);impl_=std::move(next);error_="";return true;}catch(...){error_="SIPR decoder allocation failed";return false;}
}
bool SiprDecoder::Decode(const uint8_t* data,size_t bytes,uint32_t validity,std::vector<int16_t>& pcm)noexcept{
    ScopedRounding rounding;if(!impl_||*error_)return false;
    // 32-bit validity is defined for arbitrary input length; bits after 31 are zero.
    if((!data&&bytes)||bytes>1024*1024){error_="invalid SIPR input";return false;}
    try{
        size_t frames=bytes*8/(mode_==5000?148:mode_==6500?116:mode_==16000?160:152);
        if(frames>(pcm.max_size()-pcm.size())/CoreFrameSamples())throw std::bad_alloc();
        pcm.reserve(pcm.size()+frames*CoreFrameSamples());Bits bits{data};unsigned missing=0;SiprParameters p{};
        for(size_t n=0;n<frames;++n){p=Read(bits,mode_);bool good=validity&1;validity>>=1;
            if(!good){missing=std::min(missing+1,5u);continue;}
            while(missing){impl_->Frame(p,1,pcm);--missing;}impl_->Frame(p,0,pcm);
        }
        while(missing){impl_->Frame(p,2,pcm);--missing;}return true;
    }catch(const char* reason){error_=reason;return false;}catch(...){error_="SIPR decoder allocation failed";return false;}
}
uint32_t SiprDecoder::FrameBytes()const noexcept{return mode_==5000?37:mode_==6500?29:mode_==16000?20:19;}
uint32_t SiprDecoder::FrameSamples()const noexcept{return mode_==5000?480:mode_==6500?288:mode_==16000?160:144;}
uint32_t SiprDecoder::CoreFrameSamples()const noexcept{return mode_==5000?240:mode_==16000?160:144;}
const char* SiprDecoder::Error()const noexcept{return error_;}
}
