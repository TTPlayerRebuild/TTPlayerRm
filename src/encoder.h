#pragma once
#include "common.h"
namespace ttp::rm {
enum class EncoderKind { Atrac3, Cook, Sipr };
inline constexpr DWORD atrac_bitrates[]{66150,93713,104738,132300,146081,176400,264600,352800};
// Only the internal preset ID carries this flag. Persist the actual Bitrate
// and the optional original Flavor separately, preserving old INI files.
inline constexpr DWORD atrac_alias_flag=0x80000000u;
inline DWORD atrac_preset_flavor(DWORD preset) {
    if((preset&atrac_alias_flag)&&((preset&~atrac_alias_flag)>=8)&&((preset&~atrac_alias_flag)<=12))
        return preset&~atrac_alias_flag;
    for(DWORD i=0;i<std::size(atrac_bitrates);++i)if(atrac_bitrates[i]==preset)return i;
    throw Failure{E_INVALIDARG};
}
inline DWORD atrac_preset_bitrate(DWORD preset) {
    const auto flavor=atrac_preset_flavor(preset);return atrac_bitrates[flavor>=8?flavor-5:flavor];
}
DWORD atrac_bitrate();
DWORD encoder_preset(EncoderKind);
HRESULT configure_encoder(HWND,EncoderKind kind=EncoderKind::Atrac3);
HRESULT make_encoder(void **,EncoderKind kind=EncoderKind::Atrac3);
}
