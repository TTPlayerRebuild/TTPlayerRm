"""Restore Cook's observed per-decoder noise generator in pinned FFmpeg.

The patch is enabled only by TTP_RM_LEGACY_COOK. It does not load the old
RealCodec DLL, change transform/quantizer tables, or trim decoded audio.
"""
from pathlib import Path


def apply(source: Path) -> None:
    file = Path(source) / 'libavcodec/cook.c'
    original = file.read_text(encoding='utf-8')
    marker = 'TTPLAYER_LEGACY_COOK_NOISE_V1'
    if marker in original:
        return
    text = original

    def replace(old: str, new: str) -> None:
        nonlocal text
        if text.count(old) != 1:
            raise RuntimeError('Pinned Cook compatibility precondition changed: ' + old[:80])
        text = text.replace(old, new, 1)

    replace('    int                 gain_4[9];\n', '''    int                 gain_4[9];
#if defined(TTP_RM_LEGACY_COOK)
    /* TTPLAYER_LEGACY_COOK_NOISE_V1: RealCodec gives independent mono
     * decoders separate RNG histories; a joint-stereo decoder shares one. */
    uint32_t            legacy_noise[2];
#endif
''')
    replace('    AVLFG               random_state;\n', '''#if defined(TTP_RM_LEGACY_COOK)
    uint32_t           *legacy_noise;
#else
    AVLFG               random_state;
#endif
''')
    replace('            if (av_lfg_get(&q->random_state) < 0x80000000)\n', '''#if defined(TTP_RM_LEGACY_COOK)
            /* Original Cook 60f14ab0 advances only zero coefficients and
             * takes bit 24, not the sign bit or the CRT rand() low bit. */
            *q->legacy_noise = *q->legacy_noise * 214013u + 2531011u;
            if ((*q->legacy_noise >> 24) & 1u)
#else
            if (av_lfg_get(&q->random_state) < 0x80000000)
#endif
''')
    replace('    av_lfg_init(&q->random_state, 0);\n', '''#if !defined(TTP_RM_LEGACY_COOK)
    av_lfg_init(&q->random_state, 0);
#endif
''')
    replace('        /* Initialize default data states. */\n', '''        /* Initialize default data states. */
#if defined(TTP_RM_LEGACY_COOK)
        /* Original Cook constructor 60f12a50 initializes each state to 1. */
        q->subpacket[s].legacy_noise[0] = 1;
        q->subpacket[s].legacy_noise[1] = 1;
#endif
''')
    replace('    memset(q->decode_buffer_1, 0, sizeof(q->decode_buffer_1));\n', '''#if defined(TTP_RM_LEGACY_COOK)
    q->legacy_noise = &p->legacy_noise[0];
#endif
    memset(q->decode_buffer_1, 0, sizeof(q->decode_buffer_1));
''')
    replace('            decode_bytes_and_gain(q, p, inbuffer + sub_packet_size / 2, &p->gains2);\n', '''#if defined(TTP_RM_LEGACY_COOK)
            q->legacy_noise = &p->legacy_noise[1];
#endif
            decode_bytes_and_gain(q, p, inbuffer + sub_packet_size / 2, &p->gains2);
''')
    file.write_text(text, encoding='utf-8')
