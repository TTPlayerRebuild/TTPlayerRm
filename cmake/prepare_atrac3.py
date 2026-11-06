"""Make the pinned LGPL ATRAC3-only build tree; no closed codec dependency."""
from pathlib import Path
import argparse

FILES = '''atrac3denc.cpp atrac3denc.h pcmengin.h compressed_io.h aea.h delay_buffer.h
util.h config.h gain_processor.h yaml_log.h transient_detector.cpp transient_detector.h
transient_spectral_upsampler.cpp transient_spectral_upsampler.h env.cpp env.h
atrac/at3/atrac3.cpp atrac/at3/atrac3.h atrac/at3/atrac3_bitstream.cpp
atrac/at3/atrac3_bitstream.h atrac/at3/atrac3_qmf.h qmf/qmf.cpp qmf/qmf.h
lib/mdct/common.h lib/mdct/dct.h lib/mdct/mdct.cpp lib/mdct/mdct.h
lib/bitstream/bitstream.cpp lib/bitstream/bitstream.h lib/bs_encode/encode.cpp
lib/bs_encode/encode.h lib/fft/kissfft_impl/kiss_fft.c lib/fft/kissfft_impl/kiss_fft.h
lib/fft/kissfft_impl/_kiss_fft_guts.h lib/fft/kissfft_impl/tools/kiss_fftr.c
lib/fft/kissfft_impl/tools/kiss_fftr.h atrac/atrac_scale.h atrac/atrac_scale.cpp
atrac/atrac_psy_common.h atrac/atrac_psy_common.cpp atrac/atrac_enc_cache.h
atrac/atrac_enc_cache.cpp'''.split()

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('pinned AtracDEnc patch context changed: ' + old[:60])
    return text.replace(old, new)

def prepare(source, output):
    for name in FILES + ['LICENSE']:
        original = source / ('src/' + name if name != 'LICENSE' else name)
        text = original.read_text(encoding='utf-8')
        if name == 'atrac/atrac_scale.cpp':
            for old in ['#include "at1/atrac1.h"\n', '#include "atrac/at3p/at3p_tables.h"\n',
                        'template\nclass TScaler<NAtrac1::TAtrac1Data>;\n',
                        'template\nclass TScaler<NAt3p::TScaleTable>;\n']:
                text = replace_once(text, old, '')
        if name == 'atrac/at3/atrac3.h':
            text = replace_once(text, '    const uint32_t BfuIdxConst;\n',
                '    const uint32_t BfuIdxConst;\n'
                '    // RealAudio preset caps, in M/S order for joint stereo.\n'
                '    uint16_t MaxBfus[2] = {32, 32};\n')
        if name == 'atrac/at3/atrac3_bitstream.h':
            text = replace_once(text, '        float Loudness;\n',
                '        float Loudness;\n'
                '        uint16_t MaxBfus = TAtrac3Data::MaxBfus;\n')
        if name == 'atrac3denc.cpp':
            text = replace_once(text, '#include <limits>\n', '#include <limits>\n#include <stdexcept>\n')
            # Limit the spectrum before tonal extraction as well as ordinary
            # quantization. A writer-only BFU cap would leave tonal escape data
            # above the preset bandwidth, notably on the joint side channel.
            text = replace_once(text, '            vector<float> mdctEnergy(specs.size(), 0.0f);\n',
                '            sce->MaxBfus = Params.MaxBfus[channel];\n'
                '            if (sce->MaxBfus < 1 || sce->MaxBfus > TAtrac3Data::MaxBfus)\n'
                '                throw std::runtime_error("invalid ATRAC3 bandwidth limit");\n'
                '            std::fill(specs.begin() + TAtrac3Data::BlockSizeTab[sce->MaxBfus],\n'
                '                      specs.end(), 0.0f);\n\n'
                '            vector<float> mdctEnergy(specs.size(), 0.0f);\n')
        if name == 'atrac/at3/atrac3_bitstream.cpp':
            text = replace_once(text,
                '            ctx->NumBfu = CalcInitialNumBfu(ctx->BfuIdxConst, ctx->TargetBits);\n',
                '            ctx->NumBfu = std::min<uint16_t>(ctx->Sce->MaxBfus,\n'
                '                CalcInitialNumBfu(ctx->BfuIdxConst, ctx->TargetBits));\n')
            # The upstream lazily mutable ATH vector races between independent
            # encoder constructors. Build this immutable table at module startup.
            first = text.index('    ATH.reserve(TAtrac3Data::MaxBfus);')
            last = text.index('\n}\n\nvoid WriteJsParams', first)
            body = text[first:last]
            text = text[:first] + text[last:]
            text = replace_once(text, '    if (!ATH.empty()) {\n        return;\n    }\n', '')
            text = replace_once(text, 'std::vector<float> ATH;',
                'const std::vector<float> ATH = [] {\n    std::vector<float> ATH;\n' + body +
                '\n    return ATH;\n}();')
        # Convert internal invariant errors into the backend's sticky failure;
        # a plugin must not terminate its host process.
        if 'abort();' in text:
            text = '#include <stdexcept>\n' + text.replace('std::abort();', 'abort();').replace('abort();',
                'throw std::runtime_error("ATRAC3 internal invariant failed");')
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_text(encoding='utf-8') != text:
            dest.write_text(text, encoding='utf-8', newline='\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output)
