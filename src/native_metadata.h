#pragma once
#include "io.h"
#include <shlwapi.h>
namespace ttp::rm {
struct NativeTag {std::string key;std::wstring value;};
inline std::wstring legacy_wide(const Bytes &b){
    auto size=std::find(b.begin(),b.end(),BYTE(0))-b.begin();if(!size)return {};
    UINT cp=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(b.data()),int(size),nullptr,0)>0?CP_UTF8:CP_ACP;
    int n=MultiByteToWideChar(cp,0,reinterpret_cast<const char*>(b.data()),int(size),nullptr,0);
    std::wstring s(n,L'\0');if(n)MultiByteToWideChar(cp,0,reinterpret_cast<const char*>(b.data()),int(size),s.data(),n);return s;
}
inline Bytes legacy_bytes(const std::wstring &s){
    const UINT cp=GetACP();const DWORD flags=cp==CP_UTF8?0x80u /* WC_ERR_INVALID_CHARS; modern UTF-8 ACP */:WC_NO_BEST_FIT_CHARS;
    BOOL bad=FALSE;BOOL *used=cp==CP_UTF8?nullptr:&bad;
    int n=WideCharToMultiByte(cp,flags,s.c_str(),int(s.size()),nullptr,0,nullptr,used);
    require((n>0||s.empty())&&!bad,E_INVALIDARG);Bytes b(size_t(n)+1);
    if(n)WideCharToMultiByte(cp,flags,s.c_str(),int(s.size()),reinterpret_cast<char*>(b.data()),n,nullptr,used);
    require(!bad,E_INVALIDARG);return b;
}
struct RmNode {
    DWORD type{1},flags{},attribute{1};std::string name;Bytes value{0};std::vector<RmNode> children;
    RmNode &child(const std::string &key){for(auto &c:children)if(c.name==key)return c;children.emplace_back();children.back().name=key;return children.back();}
    const RmNode *find(const std::string &key)const{for(const auto &c:children)if(c.name==key)return &c;return nullptr;}
    static RmNode parse(const Bytes &b,size_t at,size_t limit,unsigned depth=0){
        require(depth<16&&at<=limit&&limit-at>=28);const auto *p=b.data()+at;
        auto length=be32(p),v=be32(p+12),table=be32(p+16),count=be32(p+20),key=be32(p+24);
        require(length>=28&&length<=limit-at&&key<=length-28&&count<=4096&&v<=length-4);
        RmNode n;n.type=be32(p+4);n.flags=be32(p+8);require(key&&p[28+key-1]==0);
        n.name.assign(reinterpret_cast<const char*>(p+28),key-1);auto bytes=be32(p+v);
        require(bytes<=length-v-4);n.value.assign(p+v+4,p+v+4+bytes);
        require(!count||(table>=28+key&&table<=length&&count<=(length-table)/8));
        std::vector<std::pair<size_t,size_t>> ranges;
        for(DWORD i=0;i<count;++i){auto off=be32(p+table+i*8);require(off>=table+count*8&&off<=length-28);
            auto childLength=be32(p+off);require(childLength<=length-off);
            for(const auto &r:ranges)require(off>=r.second||off+childLength<=r.first);
            ranges.emplace_back(off,off+childLength);auto c=parse(b,at+off,at+length,depth+1);c.attribute=be32(p+table+i*8+4);n.children.push_back(std::move(c));}
        return n;
    }
    Bytes encode()const{
        require(name.size()<65536&&value.size()<=32*1024*1024&&children.size()<=4096);
        Bytes b(28);set32(b,4,type);set32(b,8,flags);set32(b,24,DWORD(name.size()+1));
        b.insert(b.end(),name.begin(),name.end());b.push_back(0);set32(b,12,DWORD(b.size()));
        const size_t data=b.size();b.resize(data+4);set32(b,data,DWORD(value.size()));b.insert(b.end(),value.begin(),value.end());
        if(!children.empty()){const size_t table=b.size();set32(b,16,DWORD(table));set32(b,20,DWORD(children.size()));b.resize(table+children.size()*8);
            for(size_t i=0;i<children.size();++i){set32(b,table+i*8,DWORD(b.size()));set32(b,table+i*8+4,children[i].attribute);auto child=children[i].encode();require(child.size()<=32*1024*1024-b.size());b.insert(b.end(),child.begin(),child.end());}}
        set32(b,0,DWORD(b.size()));return b;
    }
};
struct NativeMetadata {
    RmNode root;uint64_t replace_at{},original_size{};bool dirty{},valid{true};HRESULT parse_error{S_OK};
    std::vector<NativeTag> tags;Bytes cover;std::wstring mime,description;Picture view{};
    static std::pair<std::string,std::string> path(const char *key){
        if(!_stricmp(key,"Title"))return {"Track","Name"};
        if(!_stricmp(key,"Artist"))return {"Artist","Name"};
        if(!_stricmp(key,"Album"))return {"Album","Name"};
        if(!_stricmp(key,"Tracknumber")||!_stricmp(key,"Track"))return {"Track","CD Track #"};
        if(!_stricmp(key,"Date")||!_stricmp(key,"Year"))return {"Track","Year"};
        if(!_stricmp(key,"Comment"))return {"Track","Comments"};
        return {"Track",key};
    }
    void collect(){
        tags.clear();cover.clear();mime.clear();description.clear();
        for(const auto &group:root.children)for(const auto &entry:group.children){
            if(entry.name=="Cover Art"&&entry.type==10){cover=entry.value;if(auto n=entry.find("MimeType"))mime=legacy_wide(n->value);if(auto n=entry.find("Description"))description=legacy_wide(n->value);continue;}
            if(entry.type!=1)continue;std::string name=entry.name;
            if(name=="Name"){if(group.name=="Track")name="Title";else name=group.name;}
            else if(group.name=="Track"&&name=="CD Track #")name="Tracknumber";
            else if(group.name=="Track"&&name=="Year")name="Date";
            else if(group.name=="Track"&&name=="Comments")name="Comment";
            tags.push_back({name,legacy_wide(entry.value)});
        }
    }
    void read(Input &in){
        if(!in.seekable||!in.size)return;const auto old=tell(in.stream.p);original_size=replace_at=in.size;
        parse_error=protect([&]()->HRESULT{
            uint64_t end=in.size;
            if(end>=128){auto id3=read_at(in.stream.p,end-128,3);if(!memcmp(id3.data(),"TAG",3))end-=128;}
            replace_at=end;
            if(end>=12){auto footer=read_at(in.stream.p,end-12,12);
                if(!memcmp(footer.data(),"RMJE",4)){
                    auto bytes=be32(footer.data()+8);require(be32(footer.data()+4)==1&&bytes>=36&&bytes<=32*1024*1024&&uint64_t(bytes)+20<=end);
                    auto start=end-12-bytes;auto prefix=read_at(in.stream.p,start-8,16);
                    require(!memcmp(prefix.data(),"RMMD",4)&&!memcmp(prefix.data()+8,"RJMD",4)&&be32(prefix.data()+12)==1);
                    replace_at=start-8;auto data=read_at(in.stream.p,start+8,bytes-8);root=RmNode::parse(data,0,data.size());collect();
                }
            }
            return S_OK;
        });valid=SUCCEEDED(parse_error);seek(in.stream.p,old);
    }
    void set(const char *key,const wchar_t *value){
        require(valid,parse_error);require(key&&*key,E_POINTER);require(strlen(key)<=256,E_INVALIDARG);
        auto p=path(key);auto bytes=legacy_bytes(value?value:L"");auto &g=root.child(p.first);if(p.first=="Album")g.type=9;
        auto &n=g.child(p.second);n.type=1;n.value=std::move(bytes);dirty=true;collect();
    }
    void picture(const Picture *p){
        require(valid,parse_error);auto &album=root.child("Album");album.type=9;
        if(!p){album.children.erase(std::remove_if(album.children.begin(),album.children.end(),[](const auto &n){return n.name=="Cover Art";}),album.children.end());}
        else{
            require(p->size>=sizeof(Picture)&&p->data&&p->bytes&&p->bytes<=60000,E_INVALIDARG);
            auto &n=album.child("Cover Art");n.type=10;n.value.assign(p->data,p->data+p->bytes);
            auto &size=n.child("DataSize");size.type=4;size.value.resize(4);for(unsigned i=0;i<4;++i)size.value[i]=BYTE(p->bytes>>(i*8));
            n.child("MimeType").value=legacy_bytes(p->mime?p->mime:L"application/octet-stream");
            n.child("Description").value=legacy_bytes(p->description?p->description:L"");
            const auto type=p->mime?std::wstring(p->mime):std::wstring();
            n.child("Extension").value=legacy_bytes(type==L"image/png"?L".png":type==L"image/jpeg"?L".jpg":type==L"image/bmp"?L".bmp":type==L"image/gif"?L".gif":L"");
        }
        dirty=true;collect();
    }
    Bytes output()const{
        auto node=root.encode();Bytes b(16);memcpy(b.data(),"RMMD",4);memcpy(b.data()+8,"RJMD",4);set32(b,12,1);b.insert(b.end(),node.begin(),node.end());
        auto n=b.size();b.resize(n+12);memcpy(b.data()+n,"RMJE",4);set32(b,n+4,1);set32(b,n+8,DWORD(node.size()+8));
        // The original plugin appends ID3v1 after RMJE. The full Unicode/ANSI
        // values stay in RJMD; ID3v1 remains a bounded compatibility mirror.
        const auto at=b.size();b.resize(at+128);memcpy(b.data()+at,"TAG",3);b.back()=255;
        for(const auto &t:tags){size_t offset=0,bytes=0;
            if(!_stricmp(t.key.c_str(),"Title")){offset=3;bytes=30;}
            else if(!_stricmp(t.key.c_str(),"Artist")){offset=33;bytes=30;}
            else if(!_stricmp(t.key.c_str(),"Album")){offset=63;bytes=30;}
            else if(!_stricmp(t.key.c_str(),"Date")){offset=93;bytes=4;}
            else if(!_stricmp(t.key.c_str(),"Comment")){offset=97;bytes=28;}
            else if(!_stricmp(t.key.c_str(),"Tracknumber")){b[at+126]=BYTE(_wtoi(t.value.c_str()));continue;}
            if(bytes){auto text=legacy_bytes(t.value);memcpy(b.data()+at+offset,text.data(),std::min(bytes,text.size()-1));}}
        set32(b,4,DWORD(b.size()));return b;
    }
    struct Temporary {
        wchar_t path[MAX_PATH]{};ComPtr<IStream> stream;bool keep{};
        Temporary(){wchar_t folder[MAX_PATH]{};require(GetTempPathW(MAX_PATH,folder)>0,E_FAIL);require(GetTempFileNameW(folder,L"trm",0,path)!=0,E_FAIL);
            auto hr=SHCreateStreamOnFileEx(path,STGM_READWRITE|STGM_SHARE_DENY_WRITE,0,FALSE,nullptr,stream.put());if(FAILED(hr)){DeleteFileW(path);check(hr);}}
        void preserve(){keep=true;OutputDebugStringW(L"ttp_rm recovery copy: ");OutputDebugStringW(path);OutputDebugStringW(L"\n");}
        ~Temporary(){stream.reset();if(*path&&!keep)DeleteFileW(path);}
    };
    static void copy(IStream *from,IStream *to,uint64_t count){
        Bytes b(65536);while(count){auto n=DWORD(std::min<uint64_t>(count,b.size()));read_exact(from,b.data(),n);write_exact(to,b.data(),n);count-=n;}
    }
    static void clear_container_tags(IStream *stream,uint64_t limit){
        if(limit<18)return;auto signature=read_at(stream,0,4);if(memcmp(signature.data(),".RMF",4))return;
        uint64_t at=0;
        for(unsigned objects=0;objects<65536&&at+10<=limit;++objects){
            auto h=read_at(stream,at,10);auto length=be32(h.data()+4);
            require(length>=10&&length<=limit-at);
            if(!memcmp(h.data(),"DATA",4))break;
            if(!memcmp(h.data(),"CONT",4)){
                // The old reader falls back to CONT when an RJMD value is empty.
                // Clear its four strings in the staged transaction; leave object
                // lengths and all media/index offsets unchanged.
                require(length>=18&&length<=32*1024*1024);auto payload=read_at(stream,at+10,length-10);
                size_t offset=0;for(unsigned field=0;field<4;++field){
                    require(offset+2<=payload.size());const size_t count=(size_t(payload[offset])<<8)|payload[offset+1];offset+=2;
                    require(count<=payload.size()-offset);std::fill(payload.begin()+offset,payload.begin()+offset+count,BYTE(0));offset+=count;
                }
                // Keep the four length prefixes: FFmpeg reads strings directly
                // and does not skip padding remaining inside a CONT object.
                seek(stream,at+10);write_exact(stream,payload.data(),DWORD(payload.size()));
            }
            at+=length;
        }
    }
    HRESULT save(Input &in){
        if(!dirty)return S_OK;
        return protect([&]()->HRESULT{
            require(valid,parse_error);require(in.seekable&&!in.remote&&!in.base,E_ACCESSDENIED);
            if(!in.path.empty()){auto attributes=GetFileAttributesW(in.path.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_READONLY),E_ACCESSDENIED);}
            require(stream_size(in.stream.p)==original_size,HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
            const auto tail=output();Temporary backup,staged;seek(in.stream.p,0);copy(in.stream.p,backup.stream.p,original_size);
            seek(backup.stream.p,0);copy(backup.stream.p,staged.stream.p,replace_at);write_exact(staged.stream.p,tail.data(),DWORD(tail.size()));clear_container_tags(staged.stream.p,replace_at);commit_stream(staged.stream.p);
            ComPtr<IStream> opened;IStream *destination=in.stream.p;
            if((in.mode&3)==STGM_READ){require(!in.path.empty(),E_ACCESSDENIED);check(SHCreateStreamOnFileEx(in.path.c_str(),STGM_READWRITE|STGM_SHARE_DENY_NONE,0,FALSE,nullptr,opened.put()));destination=opened.p;}
            HRESULT result=protect([&]()->HRESULT{seek(destination,0);seek(staged.stream.p,0);copy(staged.stream.p,destination,replace_at+tail.size());ULARGE_INTEGER size{};size.QuadPart=replace_at+tail.size();check(destination->SetSize(size));commit_stream(destination);return S_OK;});
            if(FAILED(result)){
                // Preserve the first write/commit error even if best-effort
                // rollback itself fails. No retry is performed by Release.
                const auto restored=protect([&]()->HRESULT{seek(backup.stream.p,0);seek(destination,0);copy(backup.stream.p,destination,original_size);ULARGE_INTEGER size{};size.QuadPart=original_size;check(destination->SetSize(size));commit_stream(destination);return S_OK;});
                if(FAILED(restored)){backup.preserve();staged.preserve();OutputDebugStringW(L"ttp_rm: write and rollback failed; recovery files retained.\n");}
                return result;
            }
            dirty=false;return S_OK;
        });
    }
};
}
