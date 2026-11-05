"""Reject incomplete declared RM packets, preserving legacy raw .ra tails.

Raw .ra has no RM packet envelope and can append RaTC index objects after
audio. Its existing EOF handling must not classify those objects as a broken
declared audio packet. Strict physical packet checks apply to RM containers.
"""


def apply(source):
    path = source / 'libavformat/rmdec.c'
    text = path.read_text(encoding='utf-8')
    if 'TTP_RM_STRICT_PACKETS_RAW_RA_V2' in text:
        return
    if 'TTP_RM_STRICT_PACKETS' in text:
        # Upgrade the earlier local patch/source bundle without re-extracting
        # or replacing a caller-supplied FFmpeg source tree.
        for size in ('cfs', 'sps', 'w'):
            old = f'if (ret != {size})\n'
            if text.count(old) != 1:
                raise RuntimeError('RM short-read patch upgrade precondition changed')
            text = text.replace(old, f'if (ret != {size} && !rm->old_format)\n')
        text = text.replace('#if defined(TTP_RM_STRICT_PACKETS)',
                            '#if defined(TTP_RM_STRICT_PACKETS) /* TTP_RM_STRICT_PACKETS_RAW_RA_V2 */')
        path.write_text(text, encoding='utf-8')
        return
    replacements = [
        ('''                    for (x = 0; x < h/2; x++)
                        readfull(s, pb, ast->pkt.data+x*2*w+y*cfs, cfs);''',
         '''                    for (x = 0; x < h/2; x++) {
#if defined(TTP_RM_STRICT_PACKETS) /* TTP_RM_STRICT_PACKETS_RAW_RA_V2 */
                        ret = readfull(s, pb, ast->pkt.data+x*2*w+y*cfs, cfs);
                        if (ret != cfs && !rm->old_format)
                            return AVERROR_INVALIDDATA;
#else
                        readfull(s, pb, ast->pkt.data+x*2*w+y*cfs, cfs);
#endif
                    }'''),
        ('''                    for (x = 0; x < w/sps; x++)
                        readfull(s, pb, ast->pkt.data+sps*(h*x+((h+1)/2)*(y&1)+(y>>1)), sps);''',
         '''                    for (x = 0; x < w/sps; x++) {
#if defined(TTP_RM_STRICT_PACKETS) /* TTP_RM_STRICT_PACKETS_RAW_RA_V2 */
                        ret = readfull(s, pb, ast->pkt.data+sps*(h*x+((h+1)/2)*(y&1)+(y>>1)), sps);
                        if (ret != sps && !rm->old_format)
                            return AVERROR_INVALIDDATA;
#else
                        readfull(s, pb, ast->pkt.data+sps*(h*x+((h+1)/2)*(y&1)+(y>>1)), sps);
#endif
                    }'''),
        ('''                    readfull(s, pb, ast->pkt.data + y * w, w);''',
         '''#if defined(TTP_RM_STRICT_PACKETS) /* TTP_RM_STRICT_PACKETS_RAW_RA_V2 */
                    ret = readfull(s, pb, ast->pkt.data + y * w, w);
                    if (ret != w && !rm->old_format)
                        return AVERROR_INVALIDDATA;
#else
                    readfull(s, pb, ast->pkt.data + y * w, w);
#endif'''),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError('RM audio short-read patch precondition changed')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')
