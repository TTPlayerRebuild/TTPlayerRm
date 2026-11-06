"""Bound RA4 audio by its header, independently of its RaTC chunk directory.

The .ra4 chunk length includes the audio header and alignment padding; the
separate field at byte 28 is the encoded audio length. Treating either the
whole file or the .ra4 chunk length as audio feeds the RaTC directory into
the interleaver and conceals a physically truncated final group.
"""


def apply(source):
    path = source / 'libavformat/rmdec.c'
    text = path.read_text(encoding='utf-8')
    if 'TTP_RM_RAW_RA_BOUNDS_V1' in text:
        optional_io(path)
        return

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise RuntimeError('RA4 boundary patch precondition changed: ' + old[:70])
        text = text.replace(old, new)

    replace('    int data_end;\n} RMDemuxContext;', '''    int data_end;
    /* TTP_RM_RAW_RA_BOUNDS_V1: offsets are relative to this AVIO stream. */
    int raw_bounded;
    int64_t raw_size, raw_read, raw_chunk_end;
} RMDemuxContext;''')
    replace('        avio_rb32(pb); /* data size */', '''        {
            unsigned chunk_size = avio_rb32(pb);
            if (read_all && version == 4 && chunk_size != UINT32_MAX)
                ((RMDemuxContext *)s->priv_data)->raw_chunk_end = 16LL + chunk_size;
        }''')
    replace('        avio_rb32(pb); /* ??? */\n        bytes_per_minute = avio_rb32(pb);', '''        {
            unsigned audio_size = avio_rb32(pb);
            RMDemuxContext *raw = s->priv_data;
            if (read_all && version == 4 && audio_size != UINT32_MAX &&
                raw->raw_chunk_end) {
                raw->raw_bounded = 1;
                raw->raw_size = audio_size;
            }
        }
        bytes_per_minute = avio_rb32(pb);''')
    # Directory entries are (FourCC, absolute offset), not time/sample pairs.
    # They are optional for playing the declared audio, and never authoritative
    # over the audio length. A damaged optional table must not truncate music.
    marker = 'static int rm_read_header_old(AVFormatContext *s)'
    replace(marker, '''static int ttp_rm_read_ratc(AVFormatContext *s, int64_t audio_start)
{
    RMDemuxContext *rm = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t directory = rm->raw_chunk_end, size;
    unsigned table_size, entries, i;
    int valid = 1, have_audio = 0, have_self = 0;
    uint8_t entry[8], tag[4];
    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL))
        return 0;
    size = avio_size(pb);
    if (size < directory + 8)
        return 0;
    if (avio_seek(pb, directory, SEEK_SET) < 0)
        return AVERROR(EIO);
    if (avio_rl32(pb) != MKTAG('R','a','T','C'))
        goto restore;
    table_size = avio_rb32(pb);
    if (table_size < 16 || (table_size & 7) || table_size > 1024 * 1024 ||
        table_size > size - directory - 8) {
        valid = 0;
        goto restore;
    }
    entries = table_size / 8;
    for (i = 0; i < entries; ++i) {
        uint32_t offset, fourcc;
        if (avio_seek(pb, directory + 8 + 8LL*i, SEEK_SET) < 0 ||
            avio_read(pb, entry, 8) != 8) {
            valid = 0;
            break;
        }
        fourcc = AV_RL32(entry);
        offset = AV_RB32(entry + 4);
        if (offset > size - 4 || avio_seek(pb, offset, SEEK_SET) < 0 ||
            avio_read(pb, tag, 4) != 4 || AV_RL32(tag) != fourcc) {
            valid = 0;
            break;
        }
        if (fourcc == MKTAG('.','r','a','4')) {
            if (have_audio || offset != 8) valid = 0;
            have_audio = 1;
        }
        if (fourcc == MKTAG('R','a','T','C')) {
            if (have_self || offset != directory) valid = 0;
            have_self = 1;
        }
    }
    valid &= have_audio && have_self;
restore:
    if (avio_seek(pb, audio_start, SEEK_SET) != audio_start)
        return AVERROR(EIO);
    if (!valid)
        av_log(s, AV_LOG_WARNING, "Ignoring invalid optional RaTC directory\\n");
    return 0;
}

''' + marker)
    replace('    return rm_read_audio_stream_info(s, s->pb, st, st->priv_data, 1);', '''    {
        int ret = rm_read_audio_stream_info(s, s->pb, st, st->priv_data, 1);
        int64_t audio_start = avio_tell(s->pb);
        if (ret < 0)
            return ret;
        if (rm->raw_bounded) {
            /* The chunk directory starts after at most 3 alignment bytes. */
            int64_t pad = rm->raw_chunk_end - audio_start - rm->raw_size;
            if (pad < 0 || pad > 3)
                return AVERROR_INVALIDDATA;
            if (st->codecpar->bit_rate > 0) {
                avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
                st->duration = av_rescale(rm->raw_size * 8,
                                         st->codecpar->sample_rate,
                                         st->codecpar->bit_rate);
            }
            ret = ttp_rm_read_ratc(s, audio_start);
            if (ret < 0)
                return ret;
        }
        return 0;
    }''')
    for size in ('cfs', 'sps', 'w'):
        replace(f'if (ret != {size} && !rm->old_format)',
                f'if (ret != {size} && (!rm->old_format || rm->raw_bounded))')
    replace('                pos = avio_tell(s->pb);\n            } else {', '''                pos = avio_tell(s->pb);
                if (rm->raw_bounded) {
                    int64_t remaining = rm->raw_size - rm->raw_read;
                    if (!remaining)
                        return ast->sub_packet_cnt ? AVERROR_INVALIDDATA : AVERROR_EOF;
                    if (remaining < len) {
                        if (ast->audio_framesize)
                            return AVERROR_INVALIDDATA;
                        len = remaining;
                    }
                }
            } else {''')
    replace('            if (avio_feof(s->pb))\n                return AVERROR_EOF;', '''            if (avio_feof(s->pb))
                return rm->old_format && rm->raw_bounded && rm->raw_read < rm->raw_size
                       ? AVERROR_INVALIDDATA : AVERROR_EOF;''')
    replace('            if (res < -1)\n                return res;', '''            if (res < -1)
                return res;
            if (rm->old_format && rm->raw_bounded) {
                int64_t consumed = avio_tell(s->pb) - pos;
                if (consumed != len)
                    return AVERROR_INVALIDDATA;
                rm->raw_read += consumed;
            }''')
    path.write_text(text, encoding='utf-8')
    optional_io(path)


def optional_io(path):
    text = path.read_text(encoding='utf-8')
    if 'TTP_RM_OPTIONAL_RATC_IO_V1' in text:
        return
    changes = [
        ('    int raw_bounded;', '    int raw_bounded, raw_optional_io;'),
        ('    size = avio_size(pb);\n    if (size < directory + 8)\n        return 0;\n    if (avio_seek(pb, directory, SEEK_SET) < 0)\n        return AVERROR(EIO);',
         '''    rm->raw_optional_io = 1; /* TTP_RM_OPTIONAL_RATC_IO_V1 */
    size = avio_size(pb);
    if (size < directory + 8)
        goto restore;
    if (avio_seek(pb, directory, SEEK_SET) < 0) {
        valid = 0;
        goto restore;
    }'''),
        ('restore:\n    if (avio_seek(pb, audio_start, SEEK_SET) != audio_start)\n        return AVERROR(EIO);',
         '''restore:
    rm->raw_optional_io = 0;
    {
        /* A failed optional read/seek may have consumed bytes. Force a real
         * stream seek rather than trusting AVIO's previous buffered position.
         * Failure restoring required audio I/O remains fatal. */
        int direct = pb->direct;
        int64_t restored;
        pb->direct = 1;
        pb->buf_ptr = pb->buf_ptr_max = pb->buf_end = pb->buffer;
        restored = avio_seek(pb, audio_start, SEEK_SET);
        pb->direct = direct;
        if (restored != audio_start)
            return AVERROR(EIO);
        pb->error = 0;
    }''')]
    for old, new in changes:
        if text.count(old) != 1:
            raise RuntimeError('Optional RaTC I/O patch precondition changed')
        text = text.replace(old, new)
    text += '''
int ttprm_optional_io(AVFormatContext *s)
{
    if (!s || !s->iformat || strcmp(s->iformat->name, "rm") || !s->priv_data)
        return 0;
    return ((RMDemuxContext *)s->priv_data)->raw_optional_io;
}
'''
    path.write_text(text, encoding='utf-8')
