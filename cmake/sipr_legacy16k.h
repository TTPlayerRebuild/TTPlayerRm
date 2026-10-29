/* TTPlayer SIPR 16 kbps compatibility, reconstructed from the original
 * 614ac830 (spectral decoding), 614ac5d0 (interpolation), 614b0850/614b0950
 * (ISP polynomial conversion), and 614b10b0 (minimum frequency separation).
 * This contains no loaded legacy code or external codec dependency.
 * All prediction history belongs to SiprContext, including parallel readers.
 */
#ifndef TTPLAYER_SIPR_LEGACY16K_H
#define TTPLAYER_SIPR_LEGACY16K_H
#include "sipr_legacy16k_tables.h"

static void tt_sipr_legacy_spacing(float *f, int count)
{
    float floor = 50.0f;
    for (int i = 0; i < count; ++i) {
        if (f[i] < floor) f[i] = floor;
        floor = f[i] + 50.0f;
    }
}

static void tt_sipr_legacy_decode_isp(float *history, double *isp, const int *index, int pred)
{
    const float *cb[] = { legacy_cb1, legacy_cb2, legacy_cb3, legacy_cb4, legacy_cb5 };
    float q[16], hz[16], transformed[16];
    float weight = pred ? 0.5f : 0.12f;
    for (int i = 0; i < 4; ++i)
        memcpy(q + 3*i, cb[i] + 3*index[i], 3*sizeof(float));
    memcpy(q + 12, cb[4] + 4*index[4], 4*sizeof(float));
    for (int i = 0; i < 16; ++i)
        hz[i] = (float)((double)weight*history[i] + (double)(1.0f-weight)*q[i] + legacy_mean16[i]);
    memcpy(history, q, sizeof(q));
    tt_sipr_legacy_spacing(hz, 16);
    for (int i = 0; i < 16; ++i) {
        /* The original x87 loop accumulates all 16 products before storing.
         * Rounding after the eight-term unroll changes the high-Q LPC poles. */
        double sum = 0;
        for (int j = 0; j < 16; ++j)
            sum += (double)hz[j] * legacy_matrix16[i*16+j];
        transformed[i] = sum;
    }
    tt_sipr_legacy_spacing(transformed, 15);
    for (int i = 0; i < 15; ++i)
        isp[i] = (float)cos((double)transformed[i] * 0.0003926991f);
    isp[15] = av_clipf((float)cos(((double)transformed[15] - 5300.0) * 0.000785398163375), -0.8f, 0.5f);
}

static void tt_sipr_legacy_poly(const float *isp, float *p, int n)
{
    p[0] = 1;
    p[1] = -2 * isp[0];
    for (int i = 2; i <= n; ++i) {
        float factor = -2 * isp[2*(i-1)];
        p[i] = (float)(2.0 * p[i-2] + (double)factor * p[i-1]);
        for (int j = i-1; j >= 2; --j)
            p[j] = (float)((double)factor * p[j-1] + p[j] + p[j-2]);
        p[1] += factor;
    }
}

static void tt_sipr_legacy_isp2lpc(const float *isp, float *lp)
{
    /* 614b0850/614b0950 use float polynomial histories, then extended
     * intermediate sums. The last ISP is the last LPC coefficient, not a
     * sixteenth line spectral pair; the ordinary LSP transform is different. */
    float p[9], q[8];
    tt_sipr_legacy_poly(isp, p, 8);
    tt_sipr_legacy_poly(isp+1, q, 7);
    for (int i = 7; i >= 2; --i) q[i] -= q[i-2];
    for (int i = 0; i < 8; ++i) {
        p[i] = (float)((1.0 + isp[15]) * p[i]);
        q[i] = (float)((1.0 - isp[15]) * q[i]);
    }
    for (int i = 1; i < 8; ++i) {
        lp[i-1] = (float)(((double)p[i]+q[i]) * .5);
        lp[15-i] = (float)(((double)p[i]-q[i]) * .5);
    }
    lp[7] = (float)((1.0+isp[15]) * p[8] * .5);
    lp[15] = isp[15];
}

static void tt_sipr_legacy_lp_decode(float *lp_1st, float *lp_2nd,
                             const double *lsp_2nd, const double *lsp_prev)
{
    float isp_first[16], isp_second[16];
    for (int i = 0; i < 16; ++i) {
        isp_first[i] = (float)((lsp_2nd[i] + lsp_prev[i]) * .5);
        isp_second[i] = (float)lsp_2nd[i];
    }
    tt_sipr_legacy_isp2lpc(isp_first, lp_1st);
    tt_sipr_legacy_isp2lpc(isp_second, lp_2nd);
}

#endif
