"""Restore the original SIPR spectral reconstruction in pinned FFmpeg.

Enabled only with TTP_RM_LEGACY_SIPR. Preserve upstream's 5 kbps latency
compensation and valid final 240 samples, with the original primed ISP state.
"""
from pathlib import Path


def apply(source: Path) -> None:
    directory = Path(source) / 'libavcodec'
    file = directory / 'sipr16k.c'
    original = file.read_text(encoding='utf-8')
    marker = 'TTPLAYER_LEGACY_SIPR_16K_V1'
    text = original

    def replace(old: str, new: str) -> None:
        nonlocal text
        if text.count(old) != 1:
            raise RuntimeError('Pinned SIPR compatibility precondition changed: ' + old[:80])
        text = text.replace(old, new, 1)

    if marker not in text:
        replace('#include "sipr16kdata.h"\n', '''#include "sipr16kdata.h"
#if defined(TTP_RM_LEGACY_SIPR)
/* TTPLAYER_LEGACY_SIPR_16K_V1 */
#include "sipr_legacy16k.h"
#endif
''')
        replace('''    acelp_lp_decodef(Az[0], Az[1], lsp_new, ctx->lsp_history_16k);
''', '''#if defined(TTP_RM_LEGACY_SIPR)
    tt_sipr_legacy_lp_decode(Az[0], Az[1], lsp_new, ctx->lsp_history_16k);
#else
    acelp_lp_decodef(Az[0], Az[1], lsp_new, ctx->lsp_history_16k);
#endif
''')
        replace('''    lsf_decode_fp_16k(ctx->lsf_history, lsf_new, params->vq_indexes,
                      params->ma_pred_switch);

    ff_set_min_dist_lsf(lsf_new, LSFQ_DIFF_MIN / 2, LP_FILTER_ORDER_16k);

    lsf2lsp(lsf_new, lsp_new);
''', '''#if defined(TTP_RM_LEGACY_SIPR)
    tt_sipr_legacy_decode_isp(ctx->lsf_history, lsp_new, params->vq_indexes,
                             params->ma_pred_switch);
#else
    lsf_decode_fp_16k(ctx->lsf_history, lsf_new, params->vq_indexes,
                      params->ma_pred_switch);

    ff_set_min_dist_lsf(lsf_new, LSFQ_DIFF_MIN / 2, LP_FILTER_ORDER_16k);

    lsf2lsp(lsf_new, lsp_new);
#endif
''')
        replace('''        ctx->lsp_history_16k[i] = cos((i + 1) * M_PI/(LP_FILTER_ORDER_16k + 1));
''', '''#if defined(TTP_RM_LEGACY_SIPR)
        ctx->lsp_history_16k[i] = (float)cos((i + 1) * M_PI/(LP_FILTER_ORDER_16k + 1));
#else
        ctx->lsp_history_16k[i] = cos((i + 1) * M_PI/(LP_FILTER_ORDER_16k + 1));
#endif
''')

    if 'TTPLAYER_LEGACY_SIPR_16K_GAIN_V1' not in text:
        old = '''        gain_code = gain_corr_factor *
            acelp_decode_gain_codef(sqrt(L_SUBFR_16k), fixed_vector,
                                    19.0 - 15.0/(0.05*M_LN10/M_LN2),
                                    pred_16k, ctx->energy_history,
                                    L_SUBFR_16k, 2);

        ctx->energy_history[1] = ctx->energy_history[0];
        ctx->energy_history[0] = 20.0 * log10f(gain_corr_factor);
'''
        replace(old, '''#if defined(TTP_RM_LEGACY_SIPR)
        /* TTPLAYER_LEGACY_SIPR_16K_GAIN_V1 */
        gain_code = tt_sipr_legacy_gain(gain_corr_factor, fixed_vector, ctx->energy_history);
#else
''' + old + '''#endif
''')
    if 'TTPLAYER_LEGACY_SIPR_16K_PITCH_FALLBACK_V1' not in text:
        replace('''    } else
        return 3 * pitch_lag_prev;
}
''', '''    } else
#if defined(TTP_RM_LEGACY_SIPR)
        /* TTPLAYER_LEGACY_SIPR_16K_PITCH_FALLBACK_V1
         * Original decoder indices 62/63 advance the previous integer lag.
         * Ordinary indices 0..61 retain the existing fractional interval.
         */
        return 3 * (pitch_lag_prev + 1);
#else
        return 3 * pitch_lag_prev;
#endif
}
''')

    # Also refresh headers on a subsequent invocation; the build identity must
    # include these files so a compatibility change cannot reuse stale objects.
    for name in ('sipr_legacy16k.h', 'sipr_legacy16k_tables.h',
                 'sipr_legacylow.h', 'sipr_legacylow_tables.h'):
        contents = Path(__file__).with_name(name).read_bytes()
        destination = directory / name
        if not destination.exists() or destination.read_bytes() != contents:
            destination.write_bytes(contents)
    if text != original:
        file.write_text(text, encoding='utf-8')

    file = directory / 'sipr.c'
    original = file.read_text(encoding='utf-8')
    text = original
    if 'TTPLAYER_LEGACY_SIPR_LOW_V1' not in text:
        replace('#include "siprdata.h"\n', '''#include "siprdata.h"
#if defined(TTP_RM_LEGACY_SIPR)
/* TTPLAYER_LEGACY_SIPR_LOW_V1 */
#include "sipr_legacylow.h"
#endif
''')
        replace('''    lsf_decode_fp(lsf_new, ctx->lsf_history, params);

    sipr_decode_lp(lsf_new, ctx->lsp_history, Az, subframe_count);
''', '''#if defined(TTP_RM_LEGACY_SIPR)
    tt_sipr_low_decode_isp(lsf_new, ctx->lsf_history, params->vq_indexes);
    tt_sipr_low_decode_lp(lsf_new, ctx->lsp_history, Az, subframe_count);
#else
    lsf_decode_fp(lsf_new, ctx->lsf_history, params);

    sipr_decode_lp(lsf_new, ctx->lsp_history, Az, subframe_count);
#endif
''')
        replace('''    for (i = 0; i < LP_FILTER_ORDER; i++)
        ctx->lsp_history[i] = cos((i+1) * M_PI / (LP_FILTER_ORDER + 1));
''', '''#if defined(TTP_RM_LEGACY_SIPR)
    tt_sipr_low_init_isp(ctx->lsp_history, ctx->mode == MODE_5k0);
#else
    for (i = 0; i < LP_FILTER_ORDER; i++)
        ctx->lsp_history[i] = cos((i+1) * M_PI / (LP_FILTER_ORDER + 1));
#endif
''')
    if 'TTPLAYER_LEGACY_SIPR_LOW_INTERPOLATION_V1' not in text:
        replace('''        ff_acelp_interpolatef(excitation, excitation - T0 + (T0_frac <= 0),
                              ff_b60_sinc, 6,
                              2 * ((2 + T0_frac)%3 + 1), LP_FILTER_ORDER,
                              SUBFR_SIZE);
''', '''#if defined(TTP_RM_LEGACY_SIPR)
        /* TTPLAYER_LEGACY_SIPR_LOW_INTERPOLATION_V1 */
        tt_sipr_low_interpolate(excitation, T0, T0_frac);
#else
        ff_acelp_interpolatef(excitation, excitation - T0 + (T0_frac <= 0),
                              ff_b60_sinc, 6,
                              2 * ((2 + T0_frac)%3 + 1), LP_FILTER_ORDER,
                              SUBFR_SIZE);
#endif
''')
    if 'TTPLAYER_LEGACY_SIPR_LOW_GAIN_V1' not in text:
        old = '''        gain_code = ff_amr_set_fixed_gain(gain_cb[params->gc_index[i]][1],
                                          avg_energy, ctx->energy_history,
                                          34 - 15.0/(0.05*M_LN10/M_LN2),
                                          pred);
'''
        replace(old, '''#if defined(TTP_RM_LEGACY_SIPR)
        /* TTPLAYER_LEGACY_SIPR_LOW_GAIN_V1 */
        gain_code = tt_sipr_low_gain(gain_cb[params->gc_index[i]][1],
                                     fixed_vector, ctx->energy_history);
#else
''' + old + '''#endif
''')
    if 'TTPLAYER_LEGACY_SIPR_LOW_HIGHPASS_V1' not in text:
        old = '''    ff_acelp_apply_order_2_transfer_function(out_data, synth,
                                             (const float[2]) {-1.99997   , 1.000000000},
                                             (const float[2]) {-1.93307352, 0.935891986},
                                             0.939805806,
                                             ctx->highpass_filt_mem,
                                             frame_size);
'''
        replace(old, '''#if defined(TTP_RM_LEGACY_SIPR)
    /* TTPLAYER_LEGACY_SIPR_LOW_HIGHPASS_V1 */
    tt_sipr_low_highpass(out_data, synth, ctx->highpass_filt_mem, frame_size);
#else
''' + old + '''#endif
''')
    if 'TTPLAYER_LEGACY_SIPR_5K_AGC_INIT_V1' not in text:
        replace('''    for (i = 0; i < 4; i++)
        ctx->energy_history[i] = -14;
''', '''#if defined(TTP_RM_LEGACY_SIPR)
    /* TTPLAYER_LEGACY_SIPR_5K_AGC_INIT_V1
     * The original 5k decoder synthesizes a hidden startup frame. Upstream
     * removes its 240-sample latency; retain the resulting AGC state too.
     * Observed state bits 0x3f1119fe, independent of the first input frame.
     * This is a decoder initial condition, not an encoder gain adjustment.
     */
    if (ctx->mode == MODE_5k0)
        ctx->postfilter_agc = 0.5668028593063354f;
#endif

    for (i = 0; i < 4; i++)
        ctx->energy_history[i] = -14;
''')
    if text != original:
        file.write_text(text, encoding='utf-8')

    # Both sipr.c and sipr16k.c include this private context definition.
    # The RM build enables the macro for the entire backend, so every object
    # uses one layout; disabled builds retain the exact upstream layout.
    file = directory / 'sipr.h'
    original = file.read_text(encoding='utf-8')
    text = original
    if 'TTPLAYER_LEGACY_SIPR_HIGHPASS_STATE_V1' not in text:
        replace('    float highpass_filt_mem[2];\n', '''#if defined(TTP_RM_LEGACY_SIPR)
    /* TTPLAYER_LEGACY_SIPR_HIGHPASS_STATE_V1 */
    float highpass_filt_mem[4];
#else
    float highpass_filt_mem[2];
#endif
''')
    if text != original:
        file.write_text(text, encoding='utf-8')
