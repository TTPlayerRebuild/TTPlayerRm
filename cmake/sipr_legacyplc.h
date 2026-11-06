/* SIPR erasure state machine, reconstructed from 614a2020/614a3340,
 * 614a98f0 and 614a9660. State and PRNG belong to one decoder. */
#ifndef TTPLAYER_SIPR_LEGACYPLC_H
#define TTPLAYER_SIPR_LEGACYPLC_H
static void tt_sipr_plc_spectrum(SiprContext *ctx, float *out, const int *index)
{
    const float *cb[] = {low_cb1,low_cb2,low_cb3,low_cb4,low_cb5};
    float q[10], floor = 50;
    int error = ctx->plc_error;
    for (int i=0;i<5;++i) memcpy(q+2*i,cb[i]+2*index[i],2*sizeof(float));
    for (int i=0;i<10;++i) {
        if (!error) out[i]=(float)((double)ctx->lsf_history[i]*.33f+low_mean[i]+q[i]);
        else if (error==1) {
            out[i]=(float)((((double)ctx->plc_hz[i]-low_mean[i]+q[i])-
                (double)ctx->lsf_history[i]*.10890001f)*.5988024f+low_mean[i]);
            q[i]=(float)(((double)out[i]-(double)ctx->lsf_history[i]*.33f)-low_mean[i]);
        } else {
            out[i]=(float)((double)low_mean[i]*.1f+(double)ctx->plc_hz[i]*.9f);
            q[i]=0;
        }
    }
    if (error!=2) {
        for(int i=1;i<9;++i) if(out[i]<out[i-1]) FFSWAP(float,out[i],out[i-1]);
        for(int i=0;i<9;++i){if(out[i]<floor)out[i]=floor;floor=out[i]+50;}
        out[9]=av_clipf(out[9],-325,520);
    }
    memcpy(ctx->plc_hz,out,10*sizeof(float));
    memcpy(ctx->lsf_history,q,sizeof(q));
    for(int i=0;i<9;++i)out[i]=(float)cos((double)out[i]*.0007853981635);
    out[9]*=.0015384615398943424f;
}
static int16_t tt_sipr_plc_random(SiprContext *ctx)
{
    ctx->plc_seed=(uint16_t)(ctx->plc_seed*0x7c4du+0x3619u);
    return (int16_t)ctx->plc_seed;
}
static void tt_sipr_plc_pitch(SiprContext *ctx, int sf, int *lag, int *fraction)
{
    if (ctx->plc_error) {
        if (ctx->mode==MODE_5k0) {
            if(!sf&&ctx->plc_error==1)ctx->plc_pitch_step=((float)(3*(*lag)+*fraction)-ctx->plc_pitch3)*(1.0f/6);
            ctx->plc_pitch3+=ctx->plc_error==2?3:ctx->plc_pitch_step;
            int pitch=(int)ctx->plc_pitch3;
            *lag=(pitch+1)/3;*fraction=pitch-3*(*lag);
            *lag=FFMIN(*lag,143);
        } else if(ctx->plc_error==2) {
            *lag=FFMIN((int)ctx->plc_pitch3/3+1,143);*fraction=0;
            ctx->plc_pitch3=(float)(3*(*lag));
        } else {
            if(!sf){ctx->plc_pitch_step=((float)(3*(*lag)+*fraction)-ctx->plc_pitch3)*.25f;
                ctx->plc_pitch_running=ctx->plc_pitch3;}
            ctx->plc_pitch_running+=ctx->plc_pitch_step;
            int pitch=(int)ctx->plc_pitch_running;
            *lag=(pitch+1)/3;*fraction=pitch-3*(*lag);ctx->plc_pitch3=(float)pitch;
        }
    } else ctx->plc_pitch3=(float)(3*(*lag)+*fraction);
}
static float tt_sipr_plc_gain(SiprContext *ctx, int index, const float *vector, float *pitch, int sf)
{
    if(!ctx->plc_error){
        float result=tt_sipr_low_gain(gain_cb[index][1],vector,ctx->energy_history);
        ctx->plc_code_gain=result*32768;
        ctx->plc_pitch_gain=*pitch;
        return result;
    }
    if(ctx->plc_error==2)ctx->plc_pitch_gain=FFMIN(ctx->plc_pitch_gain*.9f,1);
    else {
        /* Common +0x2834 is 1/6 for 5k once decoding starts. For 6.5/8.5
         * it remains zero (verified through the RA export and state reads);
         * only their separate pitch trajectory uses a fixed quarter step. */
        if(!sf)ctx->plc_gain_step=(gain_cb[index][0]-ctx->plc_pitch_gain)*(ctx->mode==MODE_5k0?(1.0f/6):0);
        ctx->plc_pitch_gain+=ctx->plc_gain_step;
    }
    *pitch=FFMIN(ctx->plc_pitch_gain,1);
    ctx->plc_code_gain*=.8f;
    float e=(float)(((double)ctx->energy_history[3]+ctx->energy_history[2]+ctx->energy_history[1]+ctx->energy_history[0])*.2f-2.8f);
    memmove(ctx->energy_history,ctx->energy_history+1,3*sizeof(float));ctx->energy_history[3]=e;
    return ctx->plc_code_gain*(1.0f/32768);
}
#endif
