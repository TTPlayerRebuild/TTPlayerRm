/* Private Cook checkpoint for TTPlayer's exact-seek cache.
 * Included after cook.c's definitions; not a public FFmpeg or plugin ABI.
 * The caller must be between complete packets, with no queued output frame.
 * Restore into an initialized, undrained codec with identical configuration.
 * Only numeric history is copied. Transform/VLC storage and pointers stay owned
 * by the destination decoder. Wire bytes are process-local, versioned data.
 */
#if defined(TTP_RM_LEGACY_COOK)
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct TTPrmCookShape {
    int32_t channels, version, bands, joint_start, coupling_bits, samples;
    int32_t vector_bits, joint, subpacket_divisor, total_bands, vectors;
    uint32_t channel_mask;
} TTPrmCookShape;

typedef struct TTPrmCookHistory {
    float overlap[2][1024];
    int32_t gains[4][9]; /* gains1 now/previous, gains2 now/previous */
    uint32_t noise[2];
} TTPrmCookHistory;

typedef struct TTPrmCookCheckpoint {
    uint32_t magic, version, bytes, extra_bytes;
    int32_t rate, channels, block_align, samples, subpackets, discarded;
    TTPrmCookShape shape[MAX_SUBPACKETS];
    TTPrmCookHistory history[MAX_SUBPACKETS];
    /* The exact extradata bytes follow this structure. */
} TTPrmCookCheckpoint;

static TTPrmCookShape ttprm_cook_shape(const COOKSubpacket *p)
{
    TTPrmCookShape s;
    memset(&s, 0, sizeof(s));
    s.channels = p->num_channels;
    s.version = p->cookversion;
    s.bands = p->subbands;
    s.joint_start = p->js_subband_start;
    s.coupling_bits = p->js_vlc_bits;
    s.samples = p->samples_per_channel;
    s.vector_bits = p->log2_numvector_size;
    s.joint = p->joint_stereo;
    s.subpacket_divisor = p->bits_per_subpdiv;
    s.total_bands = p->total_subbands;
    s.vectors = p->numvector_size;
    s.channel_mask = p->channel_mask;
    return s;
}

static int ttprm_cook_gains_valid(const COOKSubpacket *p)
{
    return ((p->gains1.now == p->gain_1 && p->gains1.previous == p->gain_2) ||
            (p->gains1.now == p->gain_2 && p->gains1.previous == p->gain_1)) &&
           ((p->gains2.now == p->gain_3 && p->gains2.previous == p->gain_4) ||
            (p->gains2.now == p->gain_4 && p->gains2.previous == p->gain_3));
}

int ttprm_cook_checkpoint(AVCodecContext *avctx, void *buffer,
                         unsigned capacity, int restore)
{
    COOKContext *q;
    TTPrmCookCheckpoint state;
    unsigned required;
    int pidx, ch, i, g, channel_total = 0;
    if (!avctx || avctx->codec_id != AV_CODEC_ID_COOK || !avctx->priv_data ||
        !avctx->codec || avctx->codec->id != AV_CODEC_ID_COOK ||
        avctx->extradata_size < 0 ||
        (avctx->extradata_size && !avctx->extradata) ||
        (uint64_t)sizeof(state) + avctx->extradata_size > INT_MAX)
        return AVERROR(EINVAL);
    q = avctx->priv_data;
    if (q->avctx != avctx || !q->mdct_ctx || !q->mdct_fn || !q->mlt_window ||
        !q->decoded_bytes_buffer || q->num_subpackets < 1 ||
        q->num_subpackets > MAX_SUBPACKETS ||
        (q->samples_per_channel != 256 && q->samples_per_channel != 512 &&
         q->samples_per_channel != 1024) ||
        q->discarded_packets < 0 || q->discarded_packets > 2)
        return AVERROR(EINVAL);
    for (pidx = 0; pidx < q->num_subpackets; ++pidx) {
        const COOKSubpacket *p = &q->subpacket[pidx];
        if (p->num_channels < 1 || p->num_channels > 2 ||
            p->samples_per_channel != q->samples_per_channel ||
            !ttprm_cook_gains_valid(p))
            return AVERROR(EINVAL);
        channel_total += p->num_channels;
    }
    if (channel_total != avctx->ch_layout.nb_channels)
        return AVERROR(EINVAL);
    required = (unsigned)sizeof(state) + (unsigned)avctx->extradata_size;
    if (!buffer)
        return restore ? AVERROR(EINVAL) : (int)required;
    if (capacity < required)
        return AVERROR(EINVAL);

    if (restore) {
        /* std::vector<BYTE> and other byte buffers need no extra alignment. */
        memcpy(&state, buffer, sizeof(state));
        if (state.magic != 0x314b4354u || state.version != 1 ||
            state.bytes != required ||
            state.extra_bytes != (unsigned)avctx->extradata_size ||
            state.rate != avctx->sample_rate ||
            state.channels != avctx->ch_layout.nb_channels ||
            state.block_align != avctx->block_align ||
            state.samples != q->samples_per_channel ||
            state.subpackets != q->num_subpackets ||
            state.discarded < 0 || state.discarded > 2 ||
            (state.extra_bytes && memcmp((const uint8_t *)buffer + sizeof(state),
                                        avctx->extradata, state.extra_bytes)))
            return AVERROR(EINVAL);
        /* Validate every subpacket before changing any destination history. */
        for (pidx = 0; pidx < q->num_subpackets; ++pidx) {
            const COOKSubpacket *p = &q->subpacket[pidx];
            TTPrmCookShape expected = ttprm_cook_shape(p);
            const TTPrmCookHistory *h = &state.history[pidx];
            if (memcmp(&state.shape[pidx], &expected, sizeof(expected)))
                return AVERROR(EINVAL);
            for (g = 0; g < 4; ++g) {
                if (h->gains[g][8])
                    return AVERROR(EINVAL);
                for (i = 0; i < 8; ++i)
                    if (h->gains[g][i] < -7 || h->gains[g][i] > 8)
                        return AVERROR(EINVAL);
            }
            for (ch = 0; ch < p->num_channels; ++ch)
                for (i = 0; i < q->samples_per_channel; ++i)
                    if (!isfinite(h->overlap[ch][i]))
                        return AVERROR(EINVAL);
        }
        for (pidx = 0; pidx < q->num_subpackets; ++pidx) {
            COOKSubpacket *p = &q->subpacket[pidx];
            const TTPrmCookHistory *h = &state.history[pidx];
            memcpy(p->mono_previous_buffer1, h->overlap[0], state.samples * sizeof(float));
            if (p->num_channels == 2)
                memcpy(p->mono_previous_buffer2, h->overlap[1], state.samples * sizeof(float));
            /* Normalize by logical role; keep the destination's valid pointers. */
            memcpy(p->gains1.now,      h->gains[0], sizeof(h->gains[0]));
            memcpy(p->gains1.previous, h->gains[1], sizeof(h->gains[1]));
            memcpy(p->gains2.now,      h->gains[2], sizeof(h->gains[2]));
            memcpy(p->gains2.previous, h->gains[3], sizeof(h->gains[3]));
            memcpy(p->legacy_noise, h->noise, sizeof(h->noise));
        }
        q->discarded_packets = state.discarded;
    } else {
        memset(&state, 0, sizeof(state));
        state.magic = 0x314b4354u;
        state.version = 1;
        state.bytes = required;
        state.extra_bytes = avctx->extradata_size;
        state.rate = avctx->sample_rate;
        state.channels = avctx->ch_layout.nb_channels;
        state.block_align = avctx->block_align;
        state.samples = q->samples_per_channel;
        state.subpackets = q->num_subpackets;
        state.discarded = q->discarded_packets;
        for (pidx = 0; pidx < q->num_subpackets; ++pidx) {
            const COOKSubpacket *p = &q->subpacket[pidx];
            TTPrmCookHistory *h = &state.history[pidx];
            state.shape[pidx] = ttprm_cook_shape(p);
            memcpy(h->overlap[0], p->mono_previous_buffer1, state.samples * sizeof(float));
            if (p->num_channels == 2)
                memcpy(h->overlap[1], p->mono_previous_buffer2, state.samples * sizeof(float));
            memcpy(h->gains[0], p->gains1.now,      sizeof(h->gains[0]));
            memcpy(h->gains[1], p->gains1.previous, sizeof(h->gains[1]));
            memcpy(h->gains[2], p->gains2.now,      sizeof(h->gains[2]));
            memcpy(h->gains[3], p->gains2.previous, sizeof(h->gains[3]));
            memcpy(h->noise, p->legacy_noise, sizeof(h->noise));
        }
        memcpy(buffer, &state, sizeof(state));
        if (state.extra_bytes)
            memcpy((uint8_t *)buffer + sizeof(state), avctx->extradata, state.extra_bytes);
    }
    return (int)required;
}
#endif
