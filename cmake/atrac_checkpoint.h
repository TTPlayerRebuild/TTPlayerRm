/* Internal, same-build ATRAC3 seek checkpoints. Included by atrac3.c.
 * Only numeric history is copied; decoder allocations, transforms, function
 * pointers, packet readers and temporary work buffers remain instance-owned.
 */
#ifndef TTPRM_ATRAC_CHECKPOINT_H
#define TTPRM_ATRAC_CHECKPOINT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct TtpRmAtracCheckpointShape {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;
    int codec_id;
    int sample_rate;
    int sample_format;
    int channels;
    int channel_order;
    uint64_t channel_mask;
    int channel_ids[MAX_CHANNELS];
    int block_align;
    int coding_mode;
    int scrambled_stream;
    int flags;
    int flags2;
    int extradata_size;
    uint8_t extradata[14];
} TtpRmAtracCheckpointShape;

typedef struct TtpRmAtracChannelHistory {
    float prev_frame[SAMPLES_PER_FRAME];
    GainBlock gain_block[2];
    int gc_blk_switch;
    float delay_buf1[46];
    float delay_buf2[46];
    float delay_buf3[46];
} TtpRmAtracChannelHistory;

typedef struct TtpRmAtracCheckpoint {
    TtpRmAtracCheckpointShape shape;
    int matrix_coeff_index_prev[MAX_JS_PAIRS][4];
    int matrix_coeff_index_now[MAX_JS_PAIRS][4];
    int matrix_coeff_index_next[MAX_JS_PAIRS][4];
    int weighting_delay[MAX_JS_PAIRS][6];
    TtpRmAtracChannelHistory units[MAX_CHANNELS];
} TtpRmAtracCheckpoint;

static int ttprm_atrac_checkpoint_shape(AVCodecContext *avctx,
                                      TtpRmAtracCheckpointShape *shape)
{
    ATRAC3Context *q;
    unsigned bytes;
    int i, channels;
    if (!avctx || !avctx->codec || !avctx->priv_data ||
        (avctx->codec_id != AV_CODEC_ID_ATRAC3 && avctx->codec_id != AV_CODEC_ID_ATRAC3AL) ||
        avctx->codec->id != avctx->codec_id)
        return AVERROR(EINVAL);
    channels = avctx->ch_layout.nb_channels;
    if (channels < MIN_CHANNELS || channels > MAX_CHANNELS ||
        avctx->sample_rate <= 0 || avctx->sample_fmt != AV_SAMPLE_FMT_FLTP ||
        avctx->block_align <= 0 || avctx->block_align > 4096 ||
        avctx->extradata_size < 0 || avctx->extradata_size > (int)sizeof(shape->extradata) ||
        (avctx->extradata_size && !avctx->extradata) ||
        !av_channel_layout_check(&avctx->ch_layout))
        return AVERROR(EINVAL);
    q = avctx->priv_data;
    if (!q->units || !q->mdct_ctx || !q->mdct_fn || !q->vector_fmul ||
        !q->decoded_bytes_buffer || (q->coding_mode != SINGLE && q->coding_mode != JOINT_STEREO) ||
        (q->coding_mode == JOINT_STEREO && (channels & 1)) ||
        (q->scrambled_stream != 0 && q->scrambled_stream != 1))
        return AVERROR(EINVAL);
    bytes = (unsigned)(offsetof(TtpRmAtracCheckpoint, units) +
                       channels * sizeof(TtpRmAtracChannelHistory));
    memset(shape, 0, sizeof(*shape));
    shape->magic = UINT32_C(0x31435441); /* ATC1 */
    shape->version = 1;
    shape->bytes = bytes;
    shape->codec_id = avctx->codec_id;
    shape->sample_rate = avctx->sample_rate;
    shape->sample_format = avctx->sample_fmt;
    shape->channels = channels;
    shape->channel_order = avctx->ch_layout.order;
    if (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
        shape->channel_mask = avctx->ch_layout.u.mask;
    for (i = 0; i < channels; ++i)
        shape->channel_ids[i] = av_channel_layout_channel_from_index(&avctx->ch_layout, i);
    shape->block_align = avctx->block_align;
    shape->coding_mode = q->coding_mode;
    shape->scrambled_stream = q->scrambled_stream;
    shape->flags = avctx->flags;
    shape->flags2 = avctx->flags2;
    shape->extradata_size = avctx->extradata_size;
    if (avctx->extradata_size)
        memcpy(shape->extradata, avctx->extradata, avctx->extradata_size);
    return (int)bytes;
}

static int ttprm_atrac_history_valid(const TtpRmAtracCheckpoint *state)
{
    int pair, i, channel, block, band, point;
    for (pair = 0; pair < MAX_JS_PAIRS; ++pair) {
        for (i = 0; i < 4; ++i) {
            if ((unsigned)state->matrix_coeff_index_prev[pair][i] > 3 ||
                (unsigned)state->matrix_coeff_index_now[pair][i] > 3 ||
                (unsigned)state->matrix_coeff_index_next[pair][i] > 3)
                return 0;
        }
        for (i = 0; i < 6; ++i)
            if ((unsigned)state->weighting_delay[pair][i] > (i & 1 ? 7u : 1u))
                return 0;
    }
    for (channel = 0; channel < state->shape.channels; ++channel) {
        const TtpRmAtracChannelHistory *unit = &state->units[channel];
        if ((unsigned)unit->gc_blk_switch > 1)
            return 0;
        for (i = 0; i < SAMPLES_PER_FRAME; ++i)
            if (!isfinite(unit->prev_frame[i]))
                return 0;
        for (i = 0; i < 46; ++i)
            if (!isfinite(unit->delay_buf1[i]) || !isfinite(unit->delay_buf2[i]) ||
                !isfinite(unit->delay_buf3[i]))
                return 0;
        for (block = 0; block < 2; ++block) {
            for (band = 0; band < 4; ++band) {
                const AtracGainInfo *gain = &unit->gain_block[block].g_block[band];
                if ((unsigned)gain->num_points > 7)
                    return 0;
                for (point = 0; point < gain->num_points; ++point)
                    if ((unsigned)gain->lev_code[point] > 15 ||
                        (unsigned)gain->loc_code[point] > 31 ||
                        (point && gain->loc_code[point] <= gain->loc_code[point - 1]))
                        return 0;
            }
        }
    }
    return 1;
}

int ttprm_atrac_checkpoint(AVCodecContext *avctx, void *buffer,
                           unsigned capacity, int restore)
{
    TtpRmAtracCheckpointShape expected;
    TtpRmAtracCheckpoint state;
    ATRAC3Context *q;
    int bytes, channel;
    if (restore != 0 && restore != 1)
        return AVERROR(EINVAL);
    bytes = ttprm_atrac_checkpoint_shape(avctx, &expected);
    if (bytes < 0)
        return bytes;
    if (!buffer)
        return restore ? AVERROR(EINVAL) : bytes;
    if (capacity < (unsigned)bytes)
        return AVERROR(ENOSPC);
    q = avctx->priv_data;
    if (restore) {
        /* The caller's byte vector may be unaligned. Validate the entire
         * typed local before changing any live decoder history. */
        memcpy(&state, buffer, bytes);
        if (memcmp(&state.shape, &expected, sizeof(expected)) ||
            !ttprm_atrac_history_valid(&state))
            return AVERROR_INVALIDDATA;
        memcpy(q->matrix_coeff_index_prev, state.matrix_coeff_index_prev, sizeof(state.matrix_coeff_index_prev));
        memcpy(q->matrix_coeff_index_now, state.matrix_coeff_index_now, sizeof(state.matrix_coeff_index_now));
        memcpy(q->matrix_coeff_index_next, state.matrix_coeff_index_next, sizeof(state.matrix_coeff_index_next));
        memcpy(q->weighting_delay, state.weighting_delay, sizeof(state.weighting_delay));
        for (channel = 0; channel < expected.channels; ++channel) {
            ChannelUnit *unit = &q->units[channel];
            const TtpRmAtracChannelHistory *history = &state.units[channel];
            memcpy(unit->prev_frame, history->prev_frame, sizeof(history->prev_frame));
            memcpy(unit->gain_block, history->gain_block, sizeof(history->gain_block));
            unit->gc_blk_switch = history->gc_blk_switch;
            memcpy(unit->delay_buf1, history->delay_buf1, sizeof(history->delay_buf1));
            memcpy(unit->delay_buf2, history->delay_buf2, sizeof(history->delay_buf2));
            memcpy(unit->delay_buf3, history->delay_buf3, sizeof(history->delay_buf3));
        }
    } else {
        memset(&state, 0, sizeof(state));
        memcpy(&state.shape, &expected, sizeof(expected));
        memcpy(state.matrix_coeff_index_prev, q->matrix_coeff_index_prev, sizeof(state.matrix_coeff_index_prev));
        memcpy(state.matrix_coeff_index_now, q->matrix_coeff_index_now, sizeof(state.matrix_coeff_index_now));
        memcpy(state.matrix_coeff_index_next, q->matrix_coeff_index_next, sizeof(state.matrix_coeff_index_next));
        memcpy(state.weighting_delay, q->weighting_delay, sizeof(state.weighting_delay));
        for (channel = 0; channel < expected.channels; ++channel) {
            const ChannelUnit *unit = &q->units[channel];
            TtpRmAtracChannelHistory *history = &state.units[channel];
            memcpy(history->prev_frame, unit->prev_frame, sizeof(history->prev_frame));
            memcpy(history->gain_block, unit->gain_block, sizeof(history->gain_block));
            history->gc_blk_switch = unit->gc_blk_switch;
            memcpy(history->delay_buf1, unit->delay_buf1, sizeof(history->delay_buf1));
            memcpy(history->delay_buf2, unit->delay_buf2, sizeof(history->delay_buf2));
            memcpy(history->delay_buf3, unit->delay_buf3, sizeof(history->delay_buf3));
        }
        if (!ttprm_atrac_history_valid(&state))
            return AVERROR_INVALIDDATA;
        memcpy(buffer, &state, bytes);
    }
    return bytes;
}
#endif
