#pragma once
#include "common.h"
namespace ttp::rm {
enum class EncoderKind { Atrac3, Cook, Sipr };
inline constexpr DWORD atrac_bitrates[]{66150,93713,104738,132300,146081,176400,264600,352800};
DWORD atrac_bitrate();
DWORD encoder_preset(EncoderKind);
HRESULT configure_encoder(HWND,EncoderKind kind=EncoderKind::Atrac3);
HRESULT make_encoder(void **,EncoderKind kind=EncoderKind::Atrac3);
}
