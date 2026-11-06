/* Process-local numeric SIPR history. No SiprContext/function/AVCodecContext
 * pointers are serialized. filt_mem is represented by a buffer selector. */
#if defined(TTP_RM_LEGACY_SIPR)
typedef struct TtpSiprCheckpoint {
    uint32_t magic, size;
    int mode, rate, channels, block_align, filter_selector;
    int64_t bitrate;
    float past_pitch_gain, lsf_history[LP_FILTER_ORDER_16k];
    float excitation[L_INTERPOL + PITCH_MAX + 2*L_SUBFR_16k];
    float synth_buf[LP_FILTER_ORDER + 5*SUBFR_SIZE + 6];
    float lsp_history[LP_FILTER_ORDER], gain_mem, energy_history[4];
    float highpass_filt_mem[4], postfilter_mem[PITCH_DELAY_MAX + LP_FILTER_ORDER];
    float tilt_mem, postfilter_agc;
    float postfilter_mem5k0[PITCH_DELAY_MAX + LP_FILTER_ORDER];
    float postfilter_syn5k0[LP_FILTER_ORDER + SUBFR_SIZE*5];
    int pitch_lag_prev;
    float iir_mem[LP_FILTER_ORDER_16k+1], filt_buf[2][LP_FILTER_ORDER_16k+1];
    float mem_preemph[LP_FILTER_ORDER_16k], synth[LP_FILTER_ORDER_16k];
    double lsp_history_16k[16];
    int plc_enabled, plc_error;
    uint32_t plc_seed;
    float plc_hz[16], plc_pitch3, plc_pitch_step, plc_pitch_running;
    float plc_pitch_gain, plc_code_gain, plc_gain_step;
} TtpSiprCheckpoint;
#define TTP_SIPR_FLOAT_FIELDS(X) \
    X(past_pitch_gain) X(lsf_history) X(excitation) X(synth_buf) \
    X(lsp_history) X(gain_mem) X(energy_history) X(highpass_filt_mem) \
    X(postfilter_mem) X(tilt_mem) X(postfilter_agc) X(postfilter_mem5k0) \
    X(postfilter_syn5k0) X(iir_mem) X(filt_buf) X(mem_preemph) X(synth) \
    X(plc_hz) X(plc_pitch3) X(plc_pitch_step) X(plc_pitch_running) \
    X(plc_pitch_gain) X(plc_code_gain) X(plc_gain_step)
#define TTP_SIPR_OTHER_FIELDS(X) \
    X(pitch_lag_prev) X(lsp_history_16k) X(plc_enabled) X(plc_error) X(plc_seed)

int ttprm_sipr_checkpoint(AVCodecContext *av, void *buffer,
                         unsigned capacity, int restore)
{
    SiprContext *ctx;
    TtpSiprCheckpoint state;
    if (!av || av->codec_id != AV_CODEC_ID_SIPR || !(ctx = av->priv_data))
        return AVERROR(EINVAL);
    if (!restore && !buffer) return sizeof(state);
    if (!buffer || capacity != sizeof(state)) return AVERROR(EINVAL);
    if (restore) {
        memcpy(&state, buffer, sizeof(state));
        if (state.magic != 0x31504953u || state.size != sizeof(state) ||
            state.mode != ctx->mode || state.rate != av->sample_rate ||
            state.channels != av->ch_layout.nb_channels || state.block_align != av->block_align ||
            state.bitrate != av->bit_rate || state.filter_selector < 0 || state.filter_selector > 1 ||
            state.plc_enabled != ctx->plc_enabled)
            return AVERROR(EINVAL);
#define CHECK_FLOAT(n) { const float *p = (const float *)&state.n; \
    unsigned k; for (k = 0; k < sizeof(state.n)/sizeof(float); ++k) \
        if (!isfinite(p[k])) return AVERROR(EINVAL); }
        TTP_SIPR_FLOAT_FIELDS(CHECK_FLOAT)
#undef CHECK_FLOAT
        for (unsigned k = 0; k < 16; ++k)
            if (!isfinite(state.lsp_history_16k[k])) return AVERROR(EINVAL);
#define RESTORE_FIELD(n) memcpy(&ctx->n, &state.n, sizeof(state.n));
        TTP_SIPR_FLOAT_FIELDS(RESTORE_FIELD)
        TTP_SIPR_OTHER_FIELDS(RESTORE_FIELD)
#undef RESTORE_FIELD
        if (ctx->mode == MODE_16k) {
            ctx->filt_mem[0] = ctx->filt_buf[state.filter_selector];
            ctx->filt_mem[1] = ctx->filt_buf[1-state.filter_selector];
        }
    } else {
        memset(&state, 0, sizeof(state));
        state.magic = 0x31504953u; state.size = sizeof(state);
        state.mode = ctx->mode; state.rate = av->sample_rate;
        state.channels = av->ch_layout.nb_channels; state.block_align = av->block_align;
        state.bitrate = av->bit_rate;
        state.filter_selector = ctx->filt_mem[0] == ctx->filt_buf[1];
        if (ctx->mode == MODE_16k &&
            (ctx->filt_mem[0] != ctx->filt_buf[state.filter_selector] ||
             ctx->filt_mem[1] != ctx->filt_buf[1-state.filter_selector]))
            return AVERROR(EINVAL);
#define SAVE_FIELD(n) memcpy(&state.n, &ctx->n, sizeof(state.n));
        TTP_SIPR_FLOAT_FIELDS(SAVE_FIELD)
        TTP_SIPR_OTHER_FIELDS(SAVE_FIELD)
#undef SAVE_FIELD
        memcpy(buffer, &state, sizeof(state));
    }
    return sizeof(state);
}
#undef TTP_SIPR_FLOAT_FIELDS
#undef TTP_SIPR_OTHER_FIELDS
#endif
