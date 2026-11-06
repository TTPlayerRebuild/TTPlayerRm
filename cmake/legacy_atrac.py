"""Accept RealAudio ATRAC3's nonnegative signed-16-bit priming count.

RealCodec 60a013f0 sign-extends the RM header field, and its wrapper consumes
that count before publishing PCM. Reader performs the same bounded skip. The
field does not configure the ATRAC transform; WAV/ATRAC3AL keep upstream rules.
"""
from pathlib import Path


def apply(source: Path) -> None:
    path = Path(source) / 'libavcodec/atrac3.c'
    text = path.read_text(encoding='utf-8')
    marker = 'TTPLAYER_LEGACY_ATRAC_PRIMING_V1'
    if marker in text:
        return
    old = '''    if (delay != 0x88E) {
        av_log(avctx, AV_LOG_ERROR, "Unknown amount of delay %x != 0x88E.\\n",
               delay);
        return AVERROR_INVALIDDATA;
    }
'''
    new = '''#if defined(TTP_RM_LEGACY_ATRAC)
    /* TTPLAYER_LEGACY_ATRAC_PRIMING_V1: RealAudio stores a signed 16-bit
     * priming count. RealCodec accepts nonnegative values; Reader consumes
     * them across decoded frames. Negative encodings are not valid skips. */
    if (q->scrambled_stream && delay > INT16_MAX) {
        av_log(avctx, AV_LOG_ERROR, "Invalid RealAudio priming count %x.\\n", delay);
        return AVERROR_INVALIDDATA;
    }
    if (!q->scrambled_stream && delay != 0x88E) {
#else
    if (delay != 0x88E) {
#endif
        av_log(avctx, AV_LOG_ERROR, "Unknown amount of delay %x != 0x88E.\\n",
               delay);
        return AVERROR_INVALIDDATA;
    }
'''
    if text.count(old) != 1:
        raise RuntimeError('Pinned ATRAC priming precondition changed')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')
