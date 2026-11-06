#include "encoder.h"
#include <shlobj.h>

namespace ttp::rm {
namespace {
std::wstring settings_path() {
    wchar_t module_path[MAX_PATH]{},probe[MAX_PATH]{};DWORD n=GetModuleFileNameW(module,module_path,MAX_PATH);
    require(n&&n<MAX_PATH,HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));
    std::wstring folder(module_path);const auto slash=folder.find_last_of(L"\\/");require(slash!=std::wstring::npos);folder.resize(slash);
    if(GetTempFileNameW(folder.c_str(),L"rm",0,probe)) { DeleteFileW(probe);return folder+L"\\ttp_rm.ini"; }
    wchar_t appdata[MAX_PATH]{};check(SHGetFolderPathW(nullptr,CSIDL_APPDATA|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,appdata));
    folder=std::wstring(appdata)+L"\\TTPlayer\\ttp_rm";auto error=SHCreateDirectoryExW(nullptr,folder.c_str(),nullptr);
    require(error==ERROR_SUCCESS||error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS,HRESULT_FROM_WIN32(error));
    return folder+L"\\ttp_rm.ini";
}
struct Preset { DWORD value;const wchar_t *name; };
constexpr Preset atrac_presets[]{
    {66150,L"66 kbps（联合立体声）"},{93713,L"94 kbps（联合立体声）"},{104738,L"105 kbps（立体声）"},
    {132300,L"132 kbps（立体声）"},{146081,L"146 kbps（立体声）"},{176400,L"176 kbps（立体声）"},
    {264600,L"264 kbps（立体声）"},{352800,L"352 kbps（立体声）"},
    {atrac_alias_flag|8,L"132 kbps（Surround）"},{atrac_alias_flag|9,L"146 kbps（Surround）"},
    {atrac_alias_flag|10,L"176 kbps（Surround）"},{atrac_alias_flag|11,L"264 kbps（Surround）"},
    {atrac_alias_flag|12,L"352 kbps（Surround）"}};
constexpr Preset cook_presets[]{
    {8,L"6 kbps／8 kHz／单声道"},{0,L"8 kbps／8 kHz／单声道"},
    {1,L"11 kbps／11.025 kHz／单声道"},{2,L"16 kbps／22.05 kHz／单声道"},
    {3,L"20 kbps／22.05 kHz／单声道"},{15,L"20 kbps／44.1 kHz／单声道（宽频带）"},
    {7,L"32 kbps／22.05 kHz／单声道"},{4,L"32 kbps／44.1 kHz／单声道（标准）"},
    {16,L"32 kbps／44.1 kHz／单声道（宽频带）"},{5,L"44 kbps／44.1 kHz／单声道"},
    {6,L"64 kbps／44.1 kHz／单声道"},{9,L"20 kbps／11.025 kHz／立体声"},
    {10,L"32 kbps／22.05 kHz／立体声"},{11,L"44 kbps／22.05 kHz／立体声"},
    {12,L"64 kbps／44.1 kHz／立体声"},{13,L"96 kbps／44.1 kHz／立体声"},
    {17,L"16 kbps／22.05 kHz／联合立体声"},
    {18,L"20 kbps／22.05 kHz／联合立体声（标准）"},
    {19,L"20 kbps／22.05 kHz／联合立体声（宽频带）"},
    {20,L"32 kbps／22.05 kHz／联合立体声"},
    {21,L"32 kbps／44.1 kHz／联合立体声"},
    {22,L"44 kbps／44.1 kHz／联合立体声（标准）"},
    {23,L"44 kbps／44.1 kHz／联合立体声（宽频带）"},
    {24,L"64 kbps／44.1 kHz／联合立体声"},
    {25,L"96 kbps／44.1 kHz／联合立体声"},
    {26,L"12 kbps／11.025 kHz／联合立体声"},
    {27,L"64 kbps／44.1 kHz／联合立体声（分离优先）"},
    {28,L"96 kbps／44.1 kHz／联合立体声（分离优先）"},
    {29,L"44 kbps／22.05 kHz／联合立体声（分离优先）"},
    {30,L"96 kbps／44.1 kHz／5.0 声道"},
    {31,L"132 kbps／44.1 kHz／5.1 声道"},
    {32,L"184 kbps／44.1 kHz／5.1 声道"},
    {33,L"268 kbps／44.1 kHz／5.1 声道"}};
constexpr Preset sipr_presets[]{
    {5000,L"5 kbps／8 kHz／单声道"},{6500,L"6.5 kbps／8 kHz／单声道"},
    {8500,L"8.5 kbps／8 kHz／单声道"},{16000,L"16 kbps／16 kHz／单声道"}};
struct Presets { const Preset *items;size_t count;DWORD default_value;const wchar_t *section,*key; };
Presets presets(EncoderKind kind) {
    if(kind==EncoderKind::Cook)return {cook_presets,std::size(cook_presets),12,L"Cook",L"Flavor"};
    if(kind==EncoderKind::Sipr)return {sipr_presets,std::size(sipr_presets),8500,L"SIPR",L"Mode"};
    return {atrac_presets,std::size(atrac_presets),132300,L"ATRAC3",L"Bitrate"};
}
DWORD read_preset(const std::wstring &path,EncoderKind kind) {
    const auto list=presets(kind);const auto value=GetPrivateProfileIntW(list.section,list.key,list.default_value,path.c_str());
    if(kind==EncoderKind::Atrac3){
        // Flavor is advisory: external/older clients may still edit Bitrate.
        // Never let a stale or malformed alias silently change that bitrate.
        const auto flavor=GetPrivateProfileIntW(list.section,L"Flavor",0xffffffffu,path.c_str());
        if(flavor>=8&&flavor<=12&&atrac_bitrates[flavor-5]==value)return atrac_alias_flag|flavor;
        for(const auto bitrate:atrac_bitrates)if(bitrate==value)return value;
        return list.default_value;
    }
    // The original 6/14 presets have identical codec parameters and output.
    // Preserve existing flavor 14 configuration, but show one meaningful item.
    if(kind==EncoderKind::Cook&&value==14)return value;
    for(size_t i=0;i<list.count;++i)if(list.items[i].value==value)return value;return list.default_value;
}
struct DialogState { std::wstring path;EncoderKind kind;DWORD preset; };
HRESULT write_preset(const std::wstring &path,EncoderKind kind,DWORD value) {
    return protect([&]()->HRESULT{
        const auto list=presets(kind);
        if(kind!=EncoderKind::Atrac3){
            auto text=std::to_wstring(value);
            if(WritePrivateProfileStringW(list.section,list.key,text.c_str(),path.c_str()))return S_OK;
            const auto error=GetLastError();return error?HRESULT_FROM_WIN32(error):E_FAIL;
        }
        // Bitrate and Flavor form one setting. Replace the section in a single
        // profile API operation so a failed second key write cannot leave an
        // active alias behind an error/cancelled dialog. Keep unrelated keys.
        std::vector<wchar_t> old(1024);
        for(;;){
            auto n=GetPrivateProfileSectionW(list.section,old.data(),DWORD(old.size()),path.c_str());
            if(n<old.size()-2)break;
            require(old.size()<32768,HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));old.resize(old.size()*2);
        }
        std::wstring section;
        for(const wchar_t *entry=old.data();*entry;entry+=wcslen(entry)+1){
            std::wstring key(entry,wcschr(entry,L'=')?size_t(wcschr(entry,L'=')-entry):wcslen(entry));
            const auto first=key.find_first_not_of(L" \t"),last=key.find_last_not_of(L" \t");
            if(first!=std::wstring::npos)key=key.substr(first,last-first+1);
            if(!_wcsicmp(key.c_str(),L"Bitrate")||!_wcsicmp(key.c_str(),L"Flavor"))continue;
            section+=entry;section.push_back(0);
        }
        section+=L"Bitrate="+std::to_wstring(atrac_preset_bitrate(value));section.push_back(0);
        section+=L"Flavor="+std::to_wstring(atrac_preset_flavor(value));section.push_back(0);
        if(!WritePrivateProfileSectionW(list.section,section.c_str(),path.c_str())){
            const auto error=GetLastError();return error?HRESULT_FROM_WIN32(error):E_FAIL;
        }
        return S_OK;
    });
}
void center(HWND window,HWND owner) {
    RECT area{};MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromWindow(owner?owner:window,MONITOR_DEFAULTTONEAREST),&monitor))area=monitor.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    RECT parent=area,rect{};if(owner)GetWindowRect(owner,&parent);GetWindowRect(window,&rect);
    const LONG width=rect.right-rect.left,height=rect.bottom-rect.top;
    LONG x=parent.left+(parent.right-parent.left-width)/2,y=parent.top+(parent.bottom-parent.top-height)/2;
    x=std::max(area.left,std::min(x,area.right-width));y=std::max(area.top,std::min(y,area.bottom-height));
    SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
INT_PTR CALLBACK dialog(HWND w,UINT message,WPARAM wp,LPARAM lp) {
    auto *state=reinterpret_cast<DialogState*>(GetWindowLongPtrW(w,DWLP_USER));
    if(message==WM_INITDIALOG){
        state=reinterpret_cast<DialogState*>(lp);SetWindowLongPtrW(w,DWLP_USER,lp);auto combo=GetDlgItem(w,1001);
        const auto list=presets(state->kind);
        const auto selected=(state->kind==EncoderKind::Cook&&state->preset==14)?6:state->preset;
        for(size_t i=0;i<list.count;++i){auto index=SendMessageW(combo,CB_ADDSTRING,0,LPARAM(list.items[i].name));if(list.items[i].value==selected)SendMessageW(combo,CB_SETCURSEL,index,0);}
        if(state->kind!=EncoderKind::Atrac3){
            SetWindowTextW(w,state->kind==EncoderKind::Cook?L"RealAudio Cook 编码设置":L"RealAudio SIPR 编码设置");
            SetDlgItemTextW(w,1002,L"按所选预设输出采样率和声道，保存为 RM 文件。");
            SetDlgItemTextW(w,1003,L"输入采样率、位深和声道布局会自动转换。");
        }
        center(w,GetParent(w));return TRUE;
    }
    if(message==WM_CLOSE){EndDialog(w,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL){EndDialog(w,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wp)==IDOK&&state){
        const auto list=presets(state->kind);auto index=SendDlgItemMessageW(w,1001,CB_GETCURSEL,0,0);if(index<0||size_t(index)>=list.count)return TRUE;
        if(FAILED(write_preset(state->path,state->kind,list.items[index].value))){
            MessageBoxW(w,L"无法保存编码设置，请检查设置目录是否可写。",L"RealAudio 编码设置",MB_OK|MB_ICONERROR);return TRUE;
        }
        EndDialog(w,IDOK);return TRUE;
    }
    return FALSE;
}
}
DWORD atrac_bitrate() {
    return atrac_preset_bitrate(encoder_preset(EncoderKind::Atrac3));
}
DWORD encoder_preset(EncoderKind kind) {return read_preset(settings_path(),kind);}
HRESULT configure_encoder(HWND parent,EncoderKind kind) {
    return protect([&]()->HRESULT{
        auto path=settings_path();const auto preset=read_preset(path,kind);DialogState state{std::move(path),kind,preset};auto result=DialogBoxParamW(module,MAKEINTRESOURCEW(201),parent,dialog,LPARAM(&state));
        if(result==-1)return HRESULT_FROM_WIN32(GetLastError());return result==IDOK?S_OK:S_FALSE;
    });
}
}
