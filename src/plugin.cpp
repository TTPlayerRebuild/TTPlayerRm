#include "ttpcomm/audio_format_probe_impl.h"
#include "common.h"
#include "encoder.h"
namespace ttp::rm {
HMODULE module{};
StandardContent standard_content() { return nullptr; }
class Factory final : public ReaderCreator, public ttp::format::ProbeSupport {
protected:
    DWORD ProbeFamilies() const noexcept override { return ttp::format::Real; }
private:

    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(id, ttp::format::iid_probe)) { *out=static_cast<ttp::format::Probe*>(this); AddRef(); return S_OK; }

        if(!out)return E_POINTER; *out=nullptr;
        if(!same(id,IID_IUnknown)&&!same(id,cat_reader))return E_NOINTERFACE;
        *out=static_cast<ReaderCreator*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Create(void **out) override{return make_reader(out);}
    HRESULT STDMETHODCALLTYPE Name(wchar_t **out) override{return text(L"RMF Reader",out);}
    HRESULT STDMETHODCALLTYPE Extensions(wchar_t **out) override{return text(L"Real 音频文件(*.ra;*.rm;*.ram;*.rmvb)",out);}
};
class EncodeFactory final : public EncoderCreator {
    LONG refs_{1};
    EncoderKind kind_;
public:
    explicit EncodeFactory(EncoderKind kind):kind_(kind){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out)override {
        if(!out)return E_POINTER;*out=nullptr;if(!same(id,IID_IUnknown)&&!same(id,cat_encoder))return E_NOINTERFACE;
        *out=static_cast<EncoderCreator*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Create(void **out)override{return make_encoder(out,kind_);}
    HRESULT STDMETHODCALLTYPE Name(wchar_t **out)override{return text(kind_==EncoderKind::Atrac3?L"RealAudio ATRAC3 编码器":kind_==EncoderKind::Cook?L"RealAudio Cook 编码器":L"RealAudio SIPR 编码器",out);}
    HRESULT STDMETHODCALLTYPE Extension(wchar_t **out)override{return text(L"rm",out);}
    HRESULT STDMETHODCALLTYPE Configurable()override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Configure(HWND parent)override{return configure_encoder(parent,kind_);}
};
HRESULT make_sipr_packet_decoder(DWORD,DWORD,void **);
class SoundAddIn final : public AddIn, public SiprPacketDecoderFactory {
    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(same(id,iid_sipr_packet_factory))*out=static_cast<SiprPacketDecoderFactory*>(this);
        else if(same(id,IID_IUnknown)||same(id,iid_addin))*out=static_cast<AddIn*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Create(DWORD bitrate,DWORD flags,void **out)override{return make_sipr_packet_decoder(bitrate,flags,out);}
    HRESULT STDMETHODCALLTYPE Enum(DWORD index,GUID *category,void **out) override{
        if(!out)return E_POINTER;*out=nullptr;if(!category)return E_POINTER;
        if(index>3)return E_INVALIDARG;
        return protect([&]()->HRESULT{
            if(index==0){*out=static_cast<ReaderCreator*>(new Factory);*category=cat_reader;}
            else {*out=static_cast<EncoderCreator*>(new EncodeFactory(index==1?EncoderKind::Atrac3:index==2?EncoderKind::Cook:EncoderKind::Sipr));*category=cat_encoder;}
            return S_OK;
        });
    }
};
}
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void **out){
    if(!out)return E_POINTER;*out=nullptr;
    return ttp::rm::protect([&]()->HRESULT{*out=static_cast<ttp::rm::AddIn*>(new ttp::rm::SoundAddIn);return S_OK;});
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ttp::rm::module=h;DisableThreadLibraryCalls(h);}return TRUE;
}
