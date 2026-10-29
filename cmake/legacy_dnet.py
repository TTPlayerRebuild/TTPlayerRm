"""Recover TTPlayer's AC-3 zero-mantissa noise sequence in the RM backend.

Only the TTP_RM_LEGACY_DNET profile enables the change. E-AC-3 is unchanged.
Original 60055110 / 600553ca use the same 16-bit LFSR, with one independent
noise draw per coupled channel and coefficient. The arithmetic is generated
from that recurrence; no binary lookup table is included.
"""
from pathlib import Path

def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError('Pinned AC-3 compatibility precondition changed: '+old[:80])
    return text.replace(old,new)

def apply(source: Path):
    header=source/'libavcodec/ac3dec.h'
    h=header.read_text('utf8')
    if 'ttp_dnet_noise_state' not in h:
        h=replace(h,'    AVLFG dith_state;                       ///< for dither generation',
'''    AVLFG dith_state;                       ///< for dither generation
#if defined(TTP_RM_LEGACY_DNET)
    unsigned ttp_dnet_noise_state;
#endif''')
        header.write_text(h,'utf8')
    file=source/'libavcodec/ac3dec.c';s=file.read_text('utf8')
    if 'static int ttp_dnet_noise(' in s:return
    s=replace(s,'    av_lfg_init(&s->dith_state, 0);',
'''    av_lfg_init(&s->dith_state, 0);
#if defined(TTP_RM_LEGACY_DNET)
    s->ttp_dnet_noise_state = 1;
#endif''',2)
    s=replace(s,'static void calc_transform_coeffs_cpl(AC3DecodeContext *s)',
'''#if defined(TTP_RM_LEGACY_DNET)
static int ttp_dnet_noise(AC3DecodeContext *s)
{
    unsigned value = s->ttp_dnet_noise_state;
    int bit;
    for (bit = 0; bit < 8; ++bit)
        value = ((value << 1) ^ ((value & 0x8000) ? 0xa011 : 0)) & 0xffff;
    s->ttp_dnet_noise_state = value;
    return (int)((int16_t)value * (0.7071067811865476 * 256.0));
}
#endif

static void calc_transform_coeffs_cpl(AC3DecodeContext *s)''')
    s=replace(s,'''                    s->fixed_coeffs[ch][bin] =
                        MULH(s->fixed_coeffs[CPL_CH][bin] * (1 << 4), cpl_coord);''',
'''                    int value = s->fixed_coeffs[CPL_CH][bin];
#if defined(TTP_RM_LEGACY_DNET)
                    if (!s->eac3 && !s->bap[CPL_CH][bin])
                        value = s->fixed_coeffs[ch][bin];
#endif
                    s->fixed_coeffs[ch][bin] = MULH(value * (1 << 4), cpl_coord);''')
    s=replace(s,'''        case 0:
            /* random noise with approximate range of -0.707 to 0.707 */''',
'''        case 0:
#if defined(TTP_RM_LEGACY_DNET)
            if (!s->eac3) {
                if (ch_index == CPL_CH) {
                    int channel;
                    /* The original draws by bin, then coupled channel. Do not
                     * share one noise value or consume it for disabled dither. */
                    for (channel = 1; channel <= s->fbw_channels; ++channel)
                        if (s->channel_in_cpl[channel])
                            s->fixed_coeffs[channel][freq] = s->dither_flag[channel]
                                ? ttp_dnet_noise(s) >> exps[freq] : 0;
                    coeffs[freq] = 0;
                    continue;
                }
                mantissa = dither ? ttp_dnet_noise(s) : 0;
                break;
            }
#endif
            /* random noise with approximate range of -0.707 to 0.707 */''')
    file.write_text(s,'utf8')
