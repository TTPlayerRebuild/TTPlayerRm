/* Typed, process-local RM demux checkpoints. Not a disk file format or ABI.
 * No AVFormatContext, RMStream, AVPacket, allocator or pointer is copied.
 * A checkpoint includes the unconsumed interleaver bytes, not just an INDX
 * offset: restarting a predictive audio decoder at an index loses state. */
#define TTP_RM_CHECKPOINT_MAGIC 0x31504d52u
int ttprm_avformat_checkpoint_ready(AVFormatContext *s);
typedef struct TtpRmCheckpoint {
    unsigned magic, size, streams;
    int64_t input_position;
    int nb_packets, old_format, current_stream, remaining_len;
    int audio_stream_num, audio_pkt_cnt, data_end, raw_bounded;
    int64_t raw_size, raw_read, raw_chunk_end;
} TtpRmCheckpoint;
typedef struct TtpRmStreamCheckpoint {
    int id, codec, block_align, rate, channels;
    int videobufsize, videobufpos, curpic_num, cur_slice, slices;
    int64_t pktpos, audiotimestamp;
    int sub_packet_cnt, sub_packet_size, sub_packet_h, coded_framesize;
    int audio_framesize, sub_packet_lengths[16];
    int32_t deint_id;
    int packet_size, packet_flags, packet_stream;
    int64_t pts, dts, duration, pos;
    AVRational time_base;
} TtpRmStreamCheckpoint;

int ttprm_demux_checkpoint(AVFormatContext *s, void *buffer,
                          unsigned capacity, int restore)
{
    RMDemuxContext *rm = s->priv_data;
    TtpRmCheckpoint head;
    unsigned i, total = sizeof(head), offset;
    uint8_t *bytes = buffer;
    int result;
    if (s->nb_streams == 0 || s->nb_streams > 32 ||
        strcmp(s->iformat->name, "rm") || !s->pb ||
        !(s->pb->seekable & AVIO_SEEKABLE_NORMAL))
        return AVERROR(EINVAL);
    if (!restore) {
        if (!ttprm_avformat_checkpoint_ready(s) || rm->remaining_len)
            return AVERROR(EINVAL);
        for (i = 0; i < s->nb_streams; ++i) {
            RMStream *st = s->streams[i]->priv_data;
            enum AVCodecID codec = s->streams[i]->codecpar->codec_id;
            if (s->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
                (codec != AV_CODEC_ID_COOK && codec != AV_CODEC_ID_ATRAC3 && codec != AV_CODEC_ID_SIPR))
                return AVERROR(EINVAL);
            if (!st || st->pkt.size < 0 || st->pkt.size > 4*1024*1024 ||
                st->pkt.side_data_elems || (st->pkt.size && !st->pkt.data))
                return AVERROR(EINVAL);
            if ((st->deint_id != DEINT_ID_GENR && st->deint_id != DEINT_ID_INT4 && st->deint_id != DEINT_ID_SIPR) ||
                st->sub_packet_h <= 0 || st->audio_framesize <= 0 ||
                st->sub_packet_cnt < 0 || st->sub_packet_cnt >= st->sub_packet_h ||
                s->streams[i]->codecpar->block_align <= 0 ||
                (int64_t)st->sub_packet_h * st->audio_framesize > st->pkt.size ||
                ((int64_t)st->sub_packet_h * st->audio_framesize) % s->streams[i]->codecpar->block_align)
                return AVERROR(EINVAL);
            total += sizeof(TtpRmStreamCheckpoint) + st->pkt.size;
            if (total > 4*1024*1024)
                return AVERROR(ENOMEM);
        }
        if (!buffer) return total;
        if (capacity < total) return AVERROR(ENOSPC);
        memset(&head, 0, sizeof(head));
        head.magic = TTP_RM_CHECKPOINT_MAGIC; head.size = total;
        head.streams = s->nb_streams; head.input_position = avio_tell(s->pb);
#define RM_FIELD(n) head.n = rm->n
        RM_FIELD(nb_packets); RM_FIELD(old_format); RM_FIELD(current_stream);
        RM_FIELD(remaining_len); RM_FIELD(audio_stream_num); RM_FIELD(audio_pkt_cnt);
        RM_FIELD(data_end); RM_FIELD(raw_bounded); RM_FIELD(raw_size);
        RM_FIELD(raw_read); RM_FIELD(raw_chunk_end);
#undef RM_FIELD
        memcpy(bytes, &head, sizeof(head)); offset = sizeof(head);
        for (i = 0; i < s->nb_streams; ++i) {
            AVStream *stream = s->streams[i];
            RMStream *st = stream->priv_data;
            TtpRmStreamCheckpoint item;
            memset(&item, 0, sizeof(item));
            item.id = stream->id; item.codec = stream->codecpar->codec_id;
            item.block_align = stream->codecpar->block_align;
            item.rate = stream->codecpar->sample_rate;
            item.channels = stream->codecpar->ch_layout.nb_channels;
#define ST_FIELD(n) item.n = st->n
            ST_FIELD(videobufsize); ST_FIELD(videobufpos); ST_FIELD(curpic_num);
            ST_FIELD(cur_slice); ST_FIELD(slices); ST_FIELD(pktpos);
            ST_FIELD(audiotimestamp); ST_FIELD(sub_packet_cnt); ST_FIELD(sub_packet_size);
            ST_FIELD(sub_packet_h); ST_FIELD(coded_framesize); ST_FIELD(audio_framesize);
            ST_FIELD(deint_id);
#undef ST_FIELD
            memcpy(item.sub_packet_lengths, st->sub_packet_lengths, sizeof(item.sub_packet_lengths));
            item.packet_size = st->pkt.size; item.packet_flags = st->pkt.flags;
            item.packet_stream = st->pkt.stream_index;
            item.pts = st->pkt.pts; item.dts = st->pkt.dts;
            item.duration = st->pkt.duration; item.pos = st->pkt.pos;
            item.time_base = st->pkt.time_base;
            memcpy(bytes + offset, &item, sizeof(item)); offset += sizeof(item);
            if (item.packet_size) memcpy(bytes + offset, st->pkt.data, item.packet_size);
            offset += item.packet_size;
        }
        return total;
    }
    if (!buffer || capacity < sizeof(head)) return AVERROR(EINVAL);
    memcpy(&head, bytes, sizeof(head));
    if (head.magic != TTP_RM_CHECKPOINT_MAGIC || head.size != capacity ||
        head.streams != s->nb_streams || head.size > 4*1024*1024 ||
        head.input_position < 0 || head.remaining_len || head.audio_pkt_cnt < 0 ||
        (head.audio_pkt_cnt && (head.audio_stream_num < 0 || head.audio_stream_num >= head.streams)) ||
        head.old_format != rm->old_format || head.raw_bounded != rm->raw_bounded ||
        head.raw_size != rm->raw_size || head.raw_chunk_end != rm->raw_chunk_end ||
        head.raw_read < 0 || (head.raw_bounded && head.raw_read > head.raw_size)) return AVERROR(EINVAL);
    /* Validate the complete schema/configuration before mutating anything. */
    offset = sizeof(head);
    for (i = 0; i < head.streams; ++i) {
        AVStream *stream = s->streams[i];
        RMStream *st = stream->priv_data;
        TtpRmStreamCheckpoint item;
        if (!st || capacity-offset < sizeof(item)) return AVERROR(EINVAL);
        memcpy(&item, bytes+offset, sizeof(item)); offset += sizeof(item);
        if (item.packet_size < 0 || item.packet_size > capacity-offset ||
            item.id != stream->id || item.codec != stream->codecpar->codec_id ||
            item.block_align != stream->codecpar->block_align ||
            item.rate != stream->codecpar->sample_rate ||
            item.channels != stream->codecpar->ch_layout.nb_channels ||
            item.sub_packet_size != st->sub_packet_size || item.sub_packet_h != st->sub_packet_h ||
            item.coded_framesize != st->coded_framesize ||
            item.audio_framesize != st->audio_framesize || item.deint_id != st->deint_id)
            return AVERROR(EINVAL);
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
            (item.codec != AV_CODEC_ID_COOK && item.codec != AV_CODEC_ID_ATRAC3 && item.codec != AV_CODEC_ID_SIPR) ||
            item.sub_packet_cnt < 0 || item.sub_packet_h <= 0 || item.sub_packet_cnt >= item.sub_packet_h ||
            item.audio_framesize <= 0 || item.block_align <= 0 ||
            (item.deint_id != DEINT_ID_GENR && item.deint_id != DEINT_ID_INT4 && item.deint_id != DEINT_ID_SIPR))
            return AVERROR(EINVAL);
        {
            int64_t group = (int64_t)item.sub_packet_h * item.audio_framesize;
            if (group > item.packet_size || group % item.block_align ||
                ((int)i == head.audio_stream_num && head.audio_pkt_cnt > group/item.block_align))
                return AVERROR(EINVAL);
        }
        offset += item.packet_size;
    }
    if (offset != capacity) return AVERROR(EINVAL);
    avformat_flush(s);
    if (avio_seek(s->pb, head.input_position, SEEK_SET) != head.input_position)
        return AVERROR(EIO);
    offset = sizeof(head);
    for (i = 0; i < head.streams; ++i) {
        RMStream *st = s->streams[i]->priv_data;
        TtpRmStreamCheckpoint item;
        memcpy(&item, bytes+offset, sizeof(item)); offset += sizeof(item);
        av_packet_unref(&st->pkt);
        if (item.packet_size) {
            result = av_new_packet(&st->pkt, item.packet_size);
            if (result < 0) return result;
            memcpy(st->pkt.data, bytes+offset, item.packet_size);
        }
        offset += item.packet_size;
        st->pkt.flags = item.packet_flags; st->pkt.stream_index = item.packet_stream;
        st->pkt.pts = item.pts; st->pkt.dts = item.dts;
        st->pkt.duration = item.duration; st->pkt.pos = item.pos;
        st->pkt.time_base = item.time_base;
#define ST_FIELD(n) st->n = item.n
        ST_FIELD(videobufsize); ST_FIELD(videobufpos); ST_FIELD(curpic_num);
        ST_FIELD(cur_slice); ST_FIELD(slices); ST_FIELD(pktpos);
        ST_FIELD(audiotimestamp); ST_FIELD(sub_packet_cnt); ST_FIELD(sub_packet_size);
        ST_FIELD(sub_packet_h); ST_FIELD(coded_framesize); ST_FIELD(audio_framesize);
        ST_FIELD(deint_id);
#undef ST_FIELD
        memcpy(st->sub_packet_lengths, item.sub_packet_lengths, sizeof(item.sub_packet_lengths));
    }
#define RM_FIELD(n) rm->n = head.n
    RM_FIELD(nb_packets); RM_FIELD(old_format); RM_FIELD(current_stream);
    RM_FIELD(remaining_len); RM_FIELD(audio_stream_num); RM_FIELD(audio_pkt_cnt);
    RM_FIELD(data_end); RM_FIELD(raw_bounded); RM_FIELD(raw_size);
    RM_FIELD(raw_read); RM_FIELD(raw_chunk_end);
#undef RM_FIELD
    return capacity;
}
