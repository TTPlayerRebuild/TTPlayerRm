#include "rm_muxer.h"

namespace ttp::rm {
namespace {
void u16(Bytes &b, unsigned n) { require(n<=65535); b.push_back(BYTE(n>>8)); b.push_back(BYTE(n)); }
void str8(Bytes &b, const char *s) { const auto n=std::strlen(s); require(n<=255); b.push_back(BYTE(n)); b.insert(b.end(),s,s+n); }
void object(Bytes &out, const char *id, const Bytes &body) {
    require(body.size()<=MAXDWORD-10); out.insert(out.end(),id,id+4); put32(out,DWORD(body.size()+10)); u16(out,0);
    out.insert(out.end(),body.begin(),body.end());
}
DWORD narrow(uint64_t n) { require(n<=MAXDWORD,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)); return DWORD(n); }
}
Bytes RmMuxer::header(DWORD duration,DWORD index_offset,DWORD data_bytes) const {
    Bytes ra; ra.insert(ra.end(),{'.','r','a',0xfd}); u16(ra,5); u16(ra,0);
    put32(ra,fourcc('.','r','a','5')); put32(ra,0); u16(ra,5); put32(ra,78); u16(ra,spec_.flavor);
    put32(ra,spec_.frame_bytes); put32(ra,0); put32(ra,narrow(uint64_t(spec_.bitrate)*60/8)); put32(ra,0);
    u16(ra,1); u16(ra,spec_.frame_bytes); u16(ra,spec_.frame_bytes); u16(ra,0);
    u16(ra,0); u16(ra,spec_.sample_rate); u16(ra,0); u16(ra,spec_.sample_rate); u16(ra,0);
    u16(ra,16); u16(ra,spec_.channels); put32(ra,fourcc('g','e','n','r')); put32(ra,spec_.codec);
    ra.insert(ra.end(),{1,7,0,0}); put32(ra,DWORD(spec_.extra.size())); ra.insert(ra.end(),spec_.extra.begin(),spec_.extra.end());
    Bytes mdpr; u16(mdpr,0);
    for(auto v:{spec_.bitrate,spec_.bitrate,spec_.frame_bytes,spec_.frame_bytes,0UL,0UL,duration})put32(mdpr,v);
    str8(mdpr,"TTPlayer RealAudio encoder"); str8(mdpr,"audio/x-pn-realaudio"); put32(mdpr,DWORD(ra.size())); mdpr.insert(mdpr.end(),ra.begin(),ra.end());
    const DWORD data_offset=narrow(18ULL+50+10+content_.size()+10+mdpr.size());
    Bytes prop;
    for(auto v:{spec_.bitrate,spec_.bitrate,spec_.frame_bytes+12,spec_.frame_bytes+12,packet_count_,duration,0UL,index_offset,data_offset})put32(prop,v);
    u16(prop,1); u16(prop,3); // save allowed, perfect play; not live broadcast
    Bytes file, body; put32(body,0); put32(body,index_offset?6:5); object(file,".RMF",body);
    object(file,"PROP",prop); object(file,"CONT",content_); object(file,"MDPR",mdpr);
    file.insert(file.end(),{'D','A','T','A'}); put32(file,data_bytes); u16(file,0); put32(file,packet_count_); put32(file,0);
    require(file.size()==uint64_t(data_offset)+18,E_UNEXPECTED); return file;
}
RmMuxer::RmMuxer(IStream *stream,RmAudioSpec spec,const Bytes &content):stream_(stream,true),spec_(std::move(spec)),content_(content) {
    require(stream&&spec_.sample_rate&&spec_.sample_rate<=65535&&spec_.channels&&spec_.channels<=8&&
            spec_.frame_bytes&&spec_.frame_bytes<=65535-12&&spec_.frame_samples&&spec_.frame_samples<=65536&&spec_.extra.size()<65536,E_INVALIDARG);
    require(tell(stream)==0,E_INVALIDARG);
    auto h=header(0,0,18); data_offset_=DWORD(h.size()-18); write_exact(stream,h.data(),DWORD(h.size()));
    ULARGE_INTEGER size{}; size.QuadPart=h.size(); check(stream->SetSize(size));
}
void RmMuxer::packet(const BYTE *data,size_t bytes) {
    require(!finished_&&data&&bytes==spec_.frame_bytes,E_INVALIDARG);
    require(packet_count_!=MAXDWORD,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    const auto offset=narrow(tell(stream_.p)); narrow(uint64_t(offset)+bytes+12);
    const auto time=narrow(uint64_t(packet_count_)*spec_.frame_samples*1000/spec_.sample_rate);
    if(index_.empty()||time>=next_index_ms_) {
        require(index_.size()<1000000,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
        index_.push_back({time,offset,packet_count_}); next_index_ms_=time>MAXDWORD-1000?MAXDWORD:time+1000;
    }
    Bytes h; u16(h,0); u16(h,unsigned(bytes+12)); u16(h,0); put32(h,time); h.push_back(0); h.push_back(2);
    write_exact(stream_.p,h.data(),DWORD(h.size())); write_exact(stream_.p,data,DWORD(bytes)); ++packet_count_;
}
void RmMuxer::finish(uint64_t samples,const Bytes &metadata) {
    require(!finished_,E_UNEXPECTED); finished_=true;
    const DWORD end=narrow(tell(stream_.p)), data_bytes=end-data_offset_;
    const auto seconds=samples/spec_.sample_rate;
    require(seconds<=MAXDWORD/1000,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    const DWORD duration=narrow(seconds*1000+(samples%spec_.sample_rate)*1000/spec_.sample_rate);
    DWORD index_offset{};
    if(!index_.empty()) {
        index_offset=end; Bytes body; put32(body,DWORD(index_.size())); u16(body,0); put32(body,0);
        for(const auto &i:index_) { u16(body,0); put32(body,i.time); put32(body,i.offset); put32(body,i.packet); }
        Bytes object_bytes; object(object_bytes,"INDX",body); narrow(uint64_t(end)+object_bytes.size()+metadata.size());
        write_exact(stream_.p,object_bytes.data(),DWORD(object_bytes.size()));
    }
    if(!metadata.empty()) { narrow(tell(stream_.p)+metadata.size()); write_exact(stream_.p,metadata.data(),DWORD(metadata.size())); }
    const auto final_size=tell(stream_.p); auto h=header(duration,index_offset,data_bytes);
    seek(stream_.p,0); write_exact(stream_.p,h.data(),DWORD(h.size())); seek(stream_.p,final_size);
    ULARGE_INTEGER size{};size.QuadPart=final_size;check(stream_->SetSize(size));commit_stream(stream_.p);
}
}
