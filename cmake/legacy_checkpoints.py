"""Add internal typed checkpoint functions to the fixed FFmpeg backend."""
from pathlib import Path


def apply(source):
    here = Path(__file__).resolve().parent
    path = source / 'libavformat/rmdec.c'
    text = path.read_text(encoding='utf-8')
    include = '#include "ttp_rm_checkpoint.h"'
    if include not in text:
        text += '\n' + include + '\n'
        path.write_text(text, encoding='utf-8')
    target = source / 'libavformat/ttp_rm_checkpoint.h'
    data = (here / 'rm_checkpoint.h').read_bytes()
    if not target.exists() or target.read_bytes() != data:
        target.write_bytes(data)
    # The generic layer owns these queues. Expose only an emptiness predicate;
    # do not include/copy FormatContextInternal in the individual RM demuxer.
    path = source / 'libavformat/demux.c'
    text = path.read_text(encoding='utf-8')
    if 'int ttprm_avformat_checkpoint_ready(' not in text:
        text += '''
int ttprm_avformat_checkpoint_ready(AVFormatContext *s)
{
    const FormatContextInternal *f = ff_fc_internal(s);
    return !f->fc.packet_buffer.head && !f->raw_packet_buffer.head && !f->parse_queue.head;
}
'''
        path.write_text(text, encoding='utf-8')
    path = source / 'libavcodec/sipr.c'
    text = path.read_text(encoding='utf-8')
    include = '#include "ttp_sipr_checkpoint.h"'
    if include not in text:
        path.write_text(text + '\n' + include + '\n', encoding='utf-8')
    target = source / 'libavcodec/ttp_sipr_checkpoint.h'
    data = (here / 'sipr_checkpoint.h').read_bytes()
    if not target.exists() or target.read_bytes() != data:
        target.write_bytes(data)
