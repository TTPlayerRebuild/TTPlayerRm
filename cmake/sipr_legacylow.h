/* Low-rate SIPR numerical compatibility. Original routines 614a98f0,
 * 614aae10, 614b0490 and the 5 kbps pipeline 614a3340.
 * Float state stores and double arithmetic follow the observed x87 path.
 * No legacy code is loaded by production; all state belongs to the reader.
 */
#ifndef TTPLAYER_SIPR_LEGACYLOW_H
#define TTPLAYER_SIPR_LEGACYLOW_H
#include "sipr_legacylow_tables.h"
static void tt_sipr_low_decode_isp(float *out, float *history, const int *index)
{
    const float *cb[] = { low_cb1, low_cb2, low_cb3, low_cb4, low_cb5 };
    float q[10];
    float floor = 50.0f;
    for (int i = 0; i < 5; ++i)
        memcpy(q + 2*i, cb[i] + 2*index[i], 2*sizeof(float));
    for (int i = 0; i < 10; ++i)
        out[i] = (float)((double)history[i]*0.33f + low_mean[i] + q[i]);
    memcpy(history, q, sizeof(q));
    /* 614b02d0 makes one adjacent pass, then enforces 50 Hz spacing. */
    for (int i = 1; i < 9; ++i)
        if (out[i] < out[i-1]) FFSWAP(float, out[i], out[i-1]);
    for (int i = 0; i < 9; ++i) {
        if (out[i] < floor) out[i] = floor;
        floor = out[i] + 50;
    }
    out[9] = av_clipf(out[9], -325.0f, 520.0f);
    for (int i = 0; i < 9; ++i)
        out[i] = (float)cos((double)out[i] * 0.0007853981635);
    out[9] *= 0.0015384615398943424f;
}

static void tt_sipr_low_poly(const float *isp, float *p, int n)
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

static void tt_sipr_low_isp2lpc(const float *isp, float *lp)
{
    float p[6], q[5];
    tt_sipr_low_poly(isp, p, 5);
    tt_sipr_low_poly(isp+1, q, 4);
    for (int i = 4; i >= 2; --i) q[i] -= q[i-2];
    for (int i = 0; i < 5; ++i) {
        p[i] = (float)((1.0+isp[9]) * p[i]);
        q[i] = (float)((1.0-isp[9]) * q[i]);
    }
    for (int i = 1; i < 5; ++i) {
        lp[i-1] = (float)(((double)p[i]+q[i]) * .5);
        lp[9-i] = (float)(((double)p[i]-q[i]) * .5);
    }
    lp[4] = (float)((1.0+isp[9]) * p[5] * .5);
    lp[9] = isp[9];
}

static void tt_sipr_low_decode_lp(const float *current, const float *previous,
                                  float *Az, int num_subfr)
{
    float interval = (float)(1.0/num_subfr), t = (float)(.5/num_subfr), isp[10];
    for (int i = 0; i < num_subfr; ++i) {
        for (int j = 0; j < 10; ++j)
            isp[j] = (float)((1.0-t)*previous[j] + (double)t*current[j]);
        tt_sipr_low_isp2lpc(isp, Az);
        Az += 10;
        t += interval;
    }
}

/* The 5 kbps original decodes an initial concealed silent frame before
 * the first real frame. Its persistent ISP becomes mean-frequency ISP.
 * Initialize that state directly while retaining modern delay compensation:
 * do not insert 240 zero samples or discard the final 240 valid samples. */
static void tt_sipr_low_init_isp(float *history, int delayed_5k)
{
    static const float initial[10] = {
        .9595f, .8413f, .6549f, .4154f, .1423f,
        -.1423f, -.4154f, -.6549f, -.8413f, -.9595f
    };
    memcpy(history, initial, sizeof(initial));
    if (delayed_5k) {
        for (int j = 0; j < 9; ++j)
            history[j] = (float)cos((double)low_mean[j] * 0.0007853981635);
        history[9] = low_mean[9] * 0.0015384615398943424f;
    }
}

/* 614aade0/614aa6f0 consume 20 consecutive history samples. The three
 * fractional phases use the two interleaved coefficient sequences in
 * opposite directions. Accumulate left-to-right before the float store;
 * out can overlap the history, as required for short adaptive pitch lags. */
static void tt_sipr_low_interpolate(float *out, int delay, int fraction)
{
    int phase = -fraction;
    int step = phase < 0 ? -2 : 2;
    int start = phase < 0 ? 39 : phase;
    const float *input = out - delay - (phase < 0 ? 10 : 9);
    for (int n = 0; n < 48; ++n) {
        double sum = 0;
        for (int i = 0; i < 20; ++i)
            sum += (double)input[n+i] * low_interp[start+i*step];
        out[n] = (float)sum;
    }
}

/* 614a9660 stores the measured and predicted dB energies separately as float.
 * The original excitation uses PCM units; convert only the final stored gain
 * to FFmpeg's normalized units. All four predictor entries remain per-reader. */
static float tt_sipr_low_gain(float correction, const float *vector, float *history)
{
    double energy = .01f;
    for (int i = 0; i < 48; ++i)
        energy += (double)vector[i] * vector[i];
    float measured = (float)(10.0 * log10(energy / 48));
    float predicted = (float)((double)history[3] * .691f + 34.0f +
                              (double)history[2] * .504f +
                              (double)history[1] * .334f +
                              (double)history[0] * .2f);
    float gain = (float)(pow(10.0, ((double)predicted - measured) * .05) * correction);
    memmove(history, history + 1, 3 * sizeof(float));
    history[3] = (float)(20.0 * log10((double)correction));
    return gain * (1.0f / 32768);
}

/* 614aa020 keeps the previous two inputs and outputs, storing each output
 * as float after the five products have been accumulated in the original
 * order. The algebraically equivalent two-state upstream filter has different
 * rounding history. mem is SiprContext::highpass_filt_mem, never shared. */
static void tt_sipr_low_highpass(float *out, const float *in, float *mem, int count)
{
    float x1 = mem[0], x2 = mem[1], y1 = mem[2], y2 = mem[3];
    for (int i = 0; i < count; ++i) {
        float x = in[i];
        double sum = (double)x2 * .939805805683136f;
        sum += (double)y2 * -.9358919858932495f;
        sum += (double)x * .939805805683136f;
        sum += (double)x1 * -1.8795833587646484f;
        sum += (double)y1 * 1.9330735206604004f;
        float y = (float)sum;
        out[i] = y;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
    }
    mem[0] = x1; mem[1] = x2; mem[2] = y1; mem[3] = y2;
}


/* Original 614aa210 adds 1e-9f in PCM units before each ordered LPC sum.
 * Even a PCM-silent startup frame therefore has nonzero filter state.
 * 614ace20 keeps its AGC accumulator extended until the subframe ends;
 * 614b0220 computes the inverse root with five Newton steps. */
static void tt_sipr_low_synth(float *out,const float *a,const float *in,int count,int order)
{
    for(int n=0;n<count;++n){
        double sum=(double)in[n]+(double)1.e-9f/32768;
        for(int j=1;j<=order;++j)sum-=(double)a[j-1]*out[n-j];
        out[n]=(float)sum;
    }
}
static void tt_sipr_low_agc(float *out,const float *speech,int count,float *memory)
{
    double e=0,reference=0;
    for(int i=0;i<count;++i){e+=(double)out[i]*out[i];reference+=(double)speech[i]*speech[i];}
    if(e==0){*memory=0;return;}
    float increment=0;
    if(reference!=0){
        double ratio=e/reference;int exponent;frexp(ratio,&exponent);
        double root=ldexp(1.0,-(exponent>>1));
        for(int i=0;i<5;++i)root*=1.5-.5*ratio*root*root;
        increment=(float)((1.0-.9f)*root);
    }
    double gain=*memory;
    for(int i=0;i<count;++i){gain=gain*.9f+increment;out[i]=(float)(gain*out[i]);}
    *memory=(float)gain;
}

#endif
