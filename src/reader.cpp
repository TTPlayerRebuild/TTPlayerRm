#include "ttpcomm/audio_format_probe_impl.h"
#include "ttpcomm/pcm_format.h"
#include "ttpcomm/pcm_history.h"
#include "native_metadata.h"
#include <cmath>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
namespace ttp::rm {
namespace {
constexpr bool twin_vq=false;
struct Tag { std::string key; std::wstring value; };
class AudioReader final : public Reader, public Metadata, public Thumbnail, public MetadataCommit, public ttp::format::InfoSupport {
    LONG refs_{1}; Input input_;
    AVFormatContext *container_{}; AVIOContext *io_{};
    AVCodecContext *decoder_{}; AVPacket *packet_{av_packet_alloc()}; AVFrame *frame_{av_frame_alloc()};
    bool packet_ready_{};int stream_index_{-1}; WAVEFORMATEX format_{}; DWORD channel_mask_{};
    unsigned priming_remaining_{}; bool invert_atrac_{};
    std::vector<Tag> tags_; Bytes pcm_; size_t consumed_{};
    uint64_t media_end_{},position_{}; DWORD duration_{},bitrate_{};
    HRESULT io_error_{S_OK}; bool opened_{},draining_{},eof_{}; LONG stopped_{};
    bool resume_pending_{}; uint64_t resume_position_{};
    ttp::pcm::History history_;uint64_t decode_head_{};
    void io_failure(HRESULT hr) {
        io_error_=hr;
        if(hr==E_ABORT || hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            if(input_.seekable) {
                if(!resume_pending_) resume_position_=position_;
                resume_pending_=true;
            } else io_error_=STG_E_READFAULT;
        }
    }
    void replay_to(uint64_t target) {
        // A failed IStream read may have consumed input. Rebuild from a known
        // position instead of clearing AVIO's error and skipping those bytes.
        resume_pending_=true; resume_position_=target;
        close_container(); input_.at(input_.base); position_=0; open_container();
        while(position_<target) {
            require(consumed_!=pcm_.size() || next(),STG_E_READFAULT);
            auto n=std::min<uint64_t>(target-position_,(pcm_.size()-consumed_)/format_.nBlockAlign);
            consumed_+=size_t(n)*format_.nBlockAlign;position_+=n;
        }
        resume_pending_=false;history_.clear();decode_head_=position_;
    }
    std::wstring codec_name_;
    NativeMetadata native_;bool committed_{};HRESULT commit_result_{S_OK};
    void merge_native(){
        for(const auto &t:native_.tags){tags_.erase(std::remove_if(tags_.begin(),tags_.end(),[&](const auto &old){return !_stricmp(old.key.c_str(),t.key.c_str());}),tags_.end());
            if(!t.value.empty())tags_.push_back({t.key,t.value});}
    }
    NativeMetadata staged_metadata(){
        require(opened_&&!committed_,E_UNEXPECTED);require(input_.capabilities(true)&4,E_ACCESSDENIED);
        // First native edit imports existing CONT tags into the RJMD tree too.
        require(input_.base==0,E_ACCESSDENIED);auto next=native_;
        for(const auto &t:tags_){
            bool present=false;for(const auto &n:next.tags)if(!_stricmp(n.key.c_str(),t.key.c_str())){present=true;break;}
            if(!present)next.set(t.key.c_str(),t.value.c_str());
        }
        return next;
    }
    static int read_callback(void *opaque,uint8_t *data,int bytes) noexcept {
        auto &s=*static_cast<AudioReader*>(opaque);ULONG got{};
        HRESULT hr=protect([&]()->HRESULT{
            require(bytes>0,E_INVALIDARG);require(!InterlockedCompareExchange(&s.stopped_,0,0),HRESULT_FROM_WIN32(ERROR_CANCELLED));
            if(s.input_.seekable&&s.media_end_){
                const auto at=tell(s.input_.stream.p);if(at>=s.media_end_)return S_OK;
                bytes=int(std::min<uint64_t>(bytes,s.media_end_-at));
            }
            got=s.input_.read(data,DWORD(bytes));return S_OK;
        });
        if(FAILED(hr)){s.io_failure(hr);return AVERROR(EIO);}return got?int(got):AVERROR_EOF;
    }
    static int64_t seek_callback(void *opaque,int64_t offset,int whence) noexcept {
        auto &s=*static_cast<AudioReader*>(opaque);int64_t result=-1;
        HRESULT hr=protect([&]()->HRESULT{
            if(whence==AVSEEK_SIZE){if(!s.media_end_)return E_NOTIMPL;require(s.media_end_>=s.input_.base);result=int64_t(s.media_end_-s.input_.base);return result?S_OK:E_NOTIMPL;}
            require(s.input_.seekable,STG_E_INVALIDFUNCTION);
            LARGE_INTEGER distance{};distance.QuadPart=offset;ULARGE_INTEGER pos{};
            if((whence&~AVSEEK_FORCE)==SEEK_SET){require(offset>=0&&uint64_t(offset)<=uint64_t(INT64_MAX)-s.input_.base);distance.QuadPart+=s.input_.base;}
            DWORD origin=DWORD(whence&~AVSEEK_FORCE);
            if(origin==SEEK_END&&s.media_end_){
                require(s.media_end_<=INT64_MAX&&offset>=-int64_t(s.media_end_)&&offset<=INT64_MAX-int64_t(s.media_end_));
                distance.QuadPart=int64_t(s.media_end_)+offset;origin=STREAM_SEEK_SET;
            }
            check(s.input_.stream->Seek(distance,origin,&pos));
            require(pos.QuadPart>=s.input_.base&&pos.QuadPart-s.input_.base<=INT64_MAX);result=int64_t(pos.QuadPart-s.input_.base);return S_OK;
        });
        if(FAILED(hr)){if(hr!=E_NOTIMPL)s.io_error_=hr;return AVERROR(EIO);}return result;
    }
    void check_av(int result){
        check(io_error_);
        if(result<0)throw Failure{result==AVERROR(ENOMEM)?E_OUTOFMEMORY:HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
    }
    void close_codec(){avcodec_free_context(&decoder_);av_packet_unref(packet_);av_frame_unref(frame_);}
    void close_container(){
        close_codec();avformat_close_input(&container_);
        if(io_){av_freep(&io_->buffer);avio_context_free(&io_);}
    }
    void open_container(){
        io_error_=S_OK;draining_=eof_=packet_ready_=false;pcm_.clear();consumed_=0;
        priming_remaining_=0;invert_atrac_=false;
        auto *buffer=static_cast<unsigned char*>(av_malloc(32768));require(buffer,E_OUTOFMEMORY);
        io_=avio_alloc_context(buffer,32768,0,this,read_callback,nullptr,input_.seekable?seek_callback:nullptr);
        if(!io_){av_free(buffer);throw Failure{E_OUTOFMEMORY};}
        container_=avformat_alloc_context();require(container_,E_OUTOFMEMORY);
        container_->pb=io_;container_->flags|=AVFMT_FLAG_CUSTOM_IO;
        const auto *demux=av_find_input_format(twin_vq?"vqf":"rm");require(demux,E_NOTIMPL);
        check_av(avformat_open_input(&container_,nullptr,demux,nullptr));
        stream_index_=-1;
        for(unsigned i=0;i<container_->nb_streams;++i)
            if(container_->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){stream_index_=int(i);break;}
        require(stream_index_>=0);auto *p=container_->streams[stream_index_]->codecpar;
        if(p->codec_id==AV_CODEC_ID_MP3ADU&&!p->sample_rate){
            // The draft MIME has no codec header. Infer format from the first
            // ADU's MPEG header, retaining that packet for normal decoding.
            int result;do{av_packet_unref(packet_);result=av_read_frame(container_,packet_);check_av(result);}while(packet_->stream_index!=stream_index_);
            require(packet_->size>=4);const auto h=be32(packet_->data)|0xffe00000u;
            const auto version=(h>>19)&3,rate=(h>>10)&3;require(version!=1&&rate<3&&((h>>17)&3)==1);
            constexpr int rates[]{44100,48000,32000};p->sample_rate=rates[rate]>>(version==3?0:version==2?1:2);
            av_channel_layout_default(&p->ch_layout,((h>>6)&3)==3?1:2);packet_ready_=true;
        }
        require(p->sample_rate>0&&p->sample_rate<=384000&&p->ch_layout.nb_channels>0&&p->ch_layout.nb_channels<=8);
        if(p->codec_id==AV_CODEC_ID_ATRAC3 && p->extradata &&
           (p->extradata_size==10 || p->extradata_size==12) && be32(p->extradata)==4){
            // RealCodec consumes the RM header's priming samples internally.
            // FFmpeg validates this field but returns those samples. Its ATRAC3
            // synthesis also has the opposite polarity to the original backend.
            priming_remaining_=be16(p->extradata+6);invert_atrac_=true;
        }
        format_.wFormatTag=twin_vq?WAVE_FORMAT_IEEE_FLOAT:WAVE_FORMAT_PCM;
        format_.nChannels=WORD(p->ch_layout.nb_channels);format_.nSamplesPerSec=DWORD(p->sample_rate);
        format_.wBitsPerSample=twin_vq?32:16;format_.nBlockAlign=WORD(format_.nChannels*(format_.wBitsPerSample/8));
        format_.nAvgBytesPerSec=format_.nSamplesPerSec*format_.nBlockAlign;
        require(p->ch_layout.order==AV_CHANNEL_ORDER_NATIVE || p->ch_layout.order==AV_CHANNEL_ORDER_UNSPEC,E_NOTIMPL);
        require(p->ch_layout.order!=AV_CHANNEL_ORDER_NATIVE || p->ch_layout.u.mask<=0x3ffff,E_NOTIMPL);
        channel_mask_=p->ch_layout.order==AV_CHANNEL_ORDER_NATIVE?DWORD(p->ch_layout.u.mask):0;
        bitrate_=dword(std::max<int64_t>(p->bit_rate,0));
        const auto *st=container_->streams[stream_index_];
        if(st->duration!=AV_NOPTS_VALUE&&st->duration>0)
            duration_=dword(uint64_t(av_rescale_q(st->duration,st->time_base,{1,1000})));
        else if(container_->duration!=AV_NOPTS_VALUE&&container_->duration>0)
            duration_=dword(uint64_t(container_->duration/1000));
        else if(bitrate_&&media_end_>=input_.base&&media_end_-input_.base>uint64_t(avio_tell(io_)))
            duration_=dword((media_end_-input_.base-uint64_t(avio_tell(io_)))*8000/bitrate_);
        codec_name_=twin_vq?L"VQF|VQF Audio":L"RA|Real Audio ("+wide(avcodec_get_name(p->codec_id))+L")";
    }
    void prepare(){
        if(decoder_)return;
        const auto *p=container_->streams[stream_index_]->codecpar;
        auto *codec=avcodec_find_decoder(p->codec_id);require(codec,E_NOTIMPL);
        decoder_=avcodec_alloc_context3(codec);require(decoder_,E_OUTOFMEMORY);
        check_av(avcodec_parameters_to_context(decoder_,p));decoder_->thread_count=1;
        check_av(avcodec_open2(decoder_,codec,nullptr));
    }
    static float sample(const AVFrame *f,int index,int channel){
        const auto fmt=AVSampleFormat(f->format);const auto packed=av_get_packed_sample_fmt(fmt);
        const bool planar=av_sample_fmt_is_planar(fmt)!=0;
        const auto *p=f->extended_data[planar?channel:0];
        const size_t n=planar?size_t(index):size_t(index)*f->ch_layout.nb_channels+channel;
        switch(packed){
        case AV_SAMPLE_FMT_FLT:return reinterpret_cast<const float*>(p)[n];
        case AV_SAMPLE_FMT_DBL:return float(reinterpret_cast<const double*>(p)[n]);
        case AV_SAMPLE_FMT_S16:return reinterpret_cast<const int16_t*>(p)[n]*(1.0f/32768);
        case AV_SAMPLE_FMT_S32:return float(reinterpret_cast<const int32_t*>(p)[n]*(1.0/2147483648.0));
        case AV_SAMPLE_FMT_U8:return (int(p[n])-128)*(1.0f/128);
        default:throw Failure{E_NOTIMPL};
        }
    }
    void convert(){
        require(frame_->sample_rate==int(format_.nSamplesPerSec)&&frame_->ch_layout.nb_channels==format_.nChannels,E_NOTIMPL);
        require(frame_->nb_samples>0&&frame_->nb_samples<=65536);
        const auto skip=std::min<unsigned>(priming_remaining_,unsigned(frame_->nb_samples));
        priming_remaining_-=skip;
        pcm_.resize(size_t(frame_->nb_samples-skip)*format_.nBlockAlign);consumed_=0;
        for(int i=int(skip);i<frame_->nb_samples;++i)for(int c=0;c<format_.nChannels;++c){
            float value=sample(frame_,i,c);if(!std::isfinite(value))value=0;
            if(invert_atrac_)value=-value;
            size_t n=size_t(i-skip)*format_.nChannels+c;
            if(twin_vq){value=std::clamp(value,-32700.0f/32768,32700.0f/32768);memcpy(pcm_.data()+n*4,&value,4);}
            else{const auto v=int16_t(std::clamp(std::lrintf(value*32768.0f),-32768L,32767L));memcpy(pcm_.data()+n*2,&v,2);}
        }
    }
    bool next(){
        prepare();if(eof_)return false;
        for(unsigned attempts=0;attempts<100000;++attempts){
            auto result=avcodec_receive_frame(decoder_,frame_);
            if(result==0){convert();av_frame_unref(frame_);if(!pcm_.empty())return true;continue;}
            if(result==AVERROR_EOF){eof_=true;return false;}
            if(result!=AVERROR(EAGAIN))check_av(result);
            require(!draining_);
            if(packet_ready_){result=0;packet_ready_=false;}
            else{do{av_packet_unref(packet_);result=av_read_frame(container_,packet_);check(io_error_);}
            while(result>=0&&packet_->stream_index!=stream_index_);}
            if(result==AVERROR_EOF){draining_=true;check_av(avcodec_send_packet(decoder_,nullptr));}
            else{check_av(result);check_av(avcodec_send_packet(decoder_,packet_));}
        }
        throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
    }
    ~AudioReader(){close_container();if(!committed_)native_.save(input_);av_packet_free(&packet_);av_frame_free(&frame_);}
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override{
        if (!out) return E_POINTER;
        if (IsEqualIID(id, ttp::format::iid_info)) { *out=static_cast<ttp::format::SourceInfo*>(this); AddRef(); return S_OK; }

        if(!out)return E_POINTER;*out=nullptr;
        if(same(id,IID_IUnknown)||same(id,iid_reader))*out=static_cast<Reader*>(this);
        else if(same(id,iid_metadata))*out=static_cast<Metadata*>(this);
        else if(same(id,iid_thumbnail))*out=static_cast<Thumbnail*>(this);
        else if(same(id,iid_metadata_commit))*out=static_cast<MetadataCommit*>(this);else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Open(IStream* probe_stream, DWORD probe_flags) override {
        const auto probed = PrepareFormatInfo(probe_stream, ttp::format::Real);
        if (FAILED(probed)) return probed;
        const auto opened = OpenImpl(probe_stream, probe_flags);
        if (opened == S_OK) { if (container_ && stream_index_ >= 0) SetSourceCodec(avcodec_get_name(container_->streams[stream_index_]->codecpar->codec_id)); }
        return CompleteFormatInfo(opened);
    }
    HRESULT OpenImpl(IStream *stream,DWORD flags){
        if(!stream)return E_POINTER;if(input_.stream)return E_UNEXPECTED;
        return protect([&]()->HRESULT{
            require(packet_&&frame_,E_OUTOFMEMORY);input_.open(stream);
            // Legacy .ra has no container DATA length. Do not feed appended
            // RMMD/RJMD/ID3 tags into its demuxer or bitrate duration estimate.
            native_.read(input_);media_end_=input_.size;
            if(native_.replace_at>=input_.base&&native_.replace_at<media_end_)media_end_=native_.replace_at;
            open_container();
            AVDictionaryEntry *t=nullptr;
            while((t=av_dict_get(container_->metadata,"",t,AV_DICT_IGNORE_SUFFIX))!=nullptr){
                std::string key=t->key;if(key=="author")key="Artist";
                if(key=="title"||key=="Artist"||key=="copyright"||key=="comment")
                    tags_.push_back({key,legacy_wide(Bytes(t->value,t->value+strlen(t->value)))});
            }
            merge_native();
            if(flags&1)prepare();opened_=true;return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Capabilities(DWORD *out) override{if(!out)return E_POINTER;*out=input_.capabilities(true);if(!native_.valid)*out&=~20u;return S_OK;}
    HRESULT STDMETHODCALLTYPE Duration(DWORD *out) override{if(!out)return E_POINTER;*out=duration_;return opened_?S_OK:E_UNEXPECTED;}
    HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX **out) override {
        if(!out)return E_POINTER;*out=nullptr;if(!opened_)return E_UNEXPECTED;
        return ttp::pcm::copy_format(format_, channel_mask_, format_.wBitsPerSample, out);
    }
    HRESULT STDMETHODCALLTYPE BufferSize(DWORD *out) override{if(!out)return E_POINTER;*out=65536*format_.nBlockAlign;return opened_?S_OK:E_UNEXPECTED;}
    HRESULT STDMETHODCALLTYPE CodecName(wchar_t **out) override{return text(codec_name_,out);}
    HRESULT STDMETHODCALLTYPE Bitrate(DWORD *out) override{if(!out)return E_POINTER;*out=bitrate_;return S_OK;}
    HRESULT STDMETHODCALLTYPE BitDepth(WORD *out) override{if(!out)return E_POINTER;*out=format_.wBitsPerSample;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetCallback(IUnknown *p) override{return input_.stream?input_.callback(p):E_UNEXPECTED;}
    HRESULT STDMETHODCALLTYPE Start() override{InterlockedExchange(&stopped_,0);ResetEvent(input_.event);return S_OK;}
    HRESULT STDMETHODCALLTYPE Stop() override{InterlockedExchange(&stopped_,1);SetEvent(input_.event);return S_OK;}
    HRESULT STDMETHODCALLTYPE Read(Buffer *buffer) override{
        if(!buffer)return E_POINTER;
        return protect([&]()->HRESULT{
            check(buffer->SetLength(0));require(opened_,E_UNEXPECTED);require(!InterlockedCompareExchange(&stopped_,0,0),HRESULT_FROM_WIN32(ERROR_CANCELLED));
            DWORD capacity{},unused{};BYTE *out{};check(buffer->Capacity(&capacity));capacity-=capacity%format_.nBlockAlign;require(capacity,E_INVALIDARG);
            check(buffer->Data(&out,&unused));require(out,E_POINTER);
            if(resume_pending_)replay_to(resume_position_);
            if(position_<decode_head_){auto n=history_.read(position_,out,capacity,format_.nBlockAlign);require(n,E_UNEXPECTED);position_+=n/format_.nBlockAlign;return buffer->SetLength(n);}
            if(consumed_==pcm_.size()&&!next())return S_FALSE;
            auto n=DWORD(std::min<size_t>(capacity,pcm_.size()-consumed_));
            memcpy(out,pcm_.data()+consumed_,n);
            if(input_.seekable&&!input_.remote)history_.remember(position_,out,n,format_.nBlockAlign);
            consumed_+=n;position_+=n/format_.nBlockAlign;decode_head_=position_;return buffer->SetLength(n);
        });
    }
    HRESULT STDMETHODCALLTYPE Seek(DWORD *milliseconds) override{
        if(!milliseconds)return E_POINTER;
        const auto rollback=resume_pending_?resume_position_:position_;
        bool moving=false;
        const auto result=protect([&]()->HRESULT{
            require(opened_,E_UNEXPECTED);require(input_.seekable,STG_E_INVALIDFUNCTION);
            moving=true;
            uint64_t desired=uint64_t(*milliseconds)*format_.nSamplesPerSec/1000;
            if(resume_pending_ || FAILED(io_error_))replay_to(0);
            if(history_.contains(desired)){position_=desired;*milliseconds=ttp::rm::milliseconds(position_,format_.nSamplesPerSec);return S_OK;}
            position_=decode_head_;history_.clear();
            if(desired<position_){close_container();input_.at(input_.base);position_=0;open_container();}
            while(position_<desired){
                if(consumed_==pcm_.size()&&!next())break;
                auto n=std::min<uint64_t>(desired-position_,(pcm_.size()-consumed_)/format_.nBlockAlign);
                // Keep the exact PCM decoded while positioning too. Repeated
                // nearby seeks then reuse the same bounded history as Read.
                if(!input_.remote)history_.remember(position_,pcm_.data()+consumed_,size_t(n)*format_.nBlockAlign,format_.nBlockAlign);
                consumed_+=size_t(n)*format_.nBlockAlign;position_+=n;
            }
            decode_head_=position_;*milliseconds=ttp::rm::milliseconds(position_,format_.nSamplesPerSec);return S_OK;
        });
        if(FAILED(result)&&moving){
            // A failed seek did not publish a new position. A read or another
            // seek must recover the last published position, not a partial scan.
            resume_pending_=true;resume_position_=rollback;history_.clear();
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE Count(DWORD *out) override{if(!out)return E_POINTER;*out=DWORD(tags_.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE At(DWORD n,wchar_t **key,wchar_t **value) override{
        if(!key||!value)return E_POINTER;*key=*value=nullptr;if(n>=tags_.size())return E_INVALIDARG;
        return protect([&]()->HRESULT{check(text(wide(tags_[n].key),key));auto hr=text(tags_[n].value,value);if(FAILED(hr)){CoTaskMemFree(*key);*key=nullptr;}return hr;});
    }
    HRESULT STDMETHODCALLTYPE Get(const char *key,wchar_t **out) override{
        if(!key||!out)return E_POINTER;*out=nullptr;
        for(const auto &t:tags_)if(!_stricmp(key,t.key.c_str()))return text(t.value,out);return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE Set(const char *key,const wchar_t *value) override{
        return protect([&]()->HRESULT{auto next=staged_metadata();next.set(key,value);native_=std::move(next);merge_native();return S_OK;});
    }
    HRESULT STDMETHODCALLTYPE Commit() override{
        if(committed_)return commit_result_;if(!opened_)return E_UNEXPECTED;
        committed_=true;opened_=false;close_container();return commit_result_=native_.save(input_);
    }
    HRESULT STDMETHODCALLTYPE PictureCount(DWORD *out) override{if(!out)return E_POINTER;*out=native_.cover.empty()?0:1;return S_OK;}
    HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD *out) override{if(!out)return E_POINTER;*out=60000;return S_OK;}
    HRESULT STDMETHODCALLTYPE MaximumCount(DWORD *out) override{if(!out)return E_POINTER;*out=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE PictureAt(DWORD n,const Picture **out) override{
        if(!out)return E_POINTER;*out=nullptr;if(n||native_.cover.empty())return E_INVALIDARG;
        native_.view={sizeof(Picture),native_.mime.c_str(),native_.description.c_str(),DWORD(native_.cover.size()),native_.cover.data(),3};
        *out=&native_.view;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD n,const Picture *p,DWORD mask) override{
        if(!p)return E_POINTER;
        return protect([&]()->HRESULT{
            require(p->size>=sizeof(Picture)&&!(mask&~15u),E_INVALIDARG);
            auto next=staged_metadata();require(!n&&!next.cover.empty(),E_INVALIDARG);
            Picture merged{sizeof(Picture),(mask&1)?p->mime:next.mime.c_str(),(mask&2)?p->description:next.description.c_str(),
                (mask&8)?p->bytes:DWORD(next.cover.size()),(mask&8)?p->data:next.cover.data(),3};
            next.picture(&merged);native_=std::move(next);return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE AddPicture(const Picture *p) override{
        return protect([&]()->HRESULT{require(p,E_POINTER);auto next=staged_metadata();require(next.cover.empty(),E_INVALIDARG);next.picture(p);native_=std::move(next);return S_OK;});
    }
    HRESULT STDMETHODCALLTYPE RemovePicture(DWORD n) override{
        return protect([&]()->HRESULT{auto next=staged_metadata();require(!n&&!next.cover.empty(),E_INVALIDARG);next.picture(nullptr);native_=std::move(next);return S_OK;});
    }
};
}
HRESULT make_reader(void **out){
    if(!out)return E_POINTER;*out=nullptr;
    return protect([&]()->HRESULT{*out=static_cast<Reader*>(new AudioReader);return S_OK;});
}
}
