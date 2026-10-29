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
    if text != original:
        file.write_text(text, encoding='utf-8')
