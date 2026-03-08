/*
 * RAW Chinese AVS video demuxer
 * Copyright (c) 2009  Stefan Gehrer <stefan.gehrer@gmx.de>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavcodec/startcode.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "internal.h"
#include "rawdec.h"

#define CAVS_SEQ_START_CODE       0x000001b0
#define CAVS_PIC_I_START_CODE     0x000001b3
#define CAVS_UNDEF_START_CODE     0x000001b4
#define CAVS_PIC_PB_START_CODE    0x000001b6
#define CAVS_VIDEO_EDIT_CODE      0x000001b7
#define CAVS_PROFILE_JIZHUN       0x20
#define CAVS_PROFILE_GUANGDIAN    0x48
#define CAVS_SCAN_CHUNK           (128 * 1024)
#define CAVS_SEQ_SEARCH_WINDOW    (8 * 1024 * 1024)

typedef struct CAVSVideoDemuxerContext {
    const AVClass *class;
    int raw_packet_size;
    char *video_size;
    char *pixel_format;
    AVRational framerate;
    int64_t last_seek_pos;
    int64_t last_seek_ts;
} CAVSVideoDemuxerContext;

static int cavsvideo_probe(const AVProbeData *p)
{
    uint32_t code = -1;
    int pic = 0, seq = 0, slice_pos = 0;
    const uint8_t *ptr = p->buf, *end = p->buf + p->buf_size;

    while (ptr < end) {
        ptr = avpriv_find_start_code(ptr, end, &code);
        if ((code & 0xffffff00) == 0x100) {
            if (code < CAVS_SEQ_START_CODE) {
                if (code < slice_pos)
                    return 0;
                slice_pos = code;
            } else {
                slice_pos = 0;
            }
            if (code == CAVS_SEQ_START_CODE) {
                seq++;
                if (*ptr != CAVS_PROFILE_JIZHUN && *ptr != CAVS_PROFILE_GUANGDIAN)
                    return 0;
            } else if (code == CAVS_PIC_I_START_CODE || code == CAVS_PIC_PB_START_CODE) {
                pic++;
            } else if (code == CAVS_UNDEF_START_CODE || code > CAVS_VIDEO_EDIT_CODE) {
                return 0;
            }
        }
    }
    if (seq && seq * 9 <= pic * 10)
        return AVPROBE_SCORE_EXTENSION + 1;
    return 0;
}

static int64_t cavsvideo_find_prev_code(AVIOContext *pb, int64_t anchor_pos, uint8_t wanted)
{
    uint8_t *buffer;
    int64_t search_end;
    int64_t found = -1;

    buffer = av_malloc(CAVS_SCAN_CHUNK + 3);
    if (!buffer)
        return AVERROR(ENOMEM);

    search_end = anchor_pos > 0 ? anchor_pos - 1 : 0;
    while (search_end >= 0) {
        int64_t chunk_start = search_end > CAVS_SCAN_CHUNK ?
                              search_end - CAVS_SCAN_CHUNK : 0;
        int read_size = (int)(search_end - chunk_start + 1);
        int ret;

        if (avio_seek(pb, chunk_start, SEEK_SET) < 0)
            break;
        ret = avio_read(pb, buffer, read_size);
        if (ret <= 0)
            break;

        for (int i = ret - 4; i >= 0; i--) {
            if (buffer[i] == 0x00 && buffer[i + 1] == 0x00 &&
                buffer[i + 2] == 0x01 && buffer[i + 3] == wanted) {
                found = chunk_start + i;
                goto end;
            }
        }

        if (chunk_start == 0)
            break;
        search_end = chunk_start + 2;
    }

end:
    av_free(buffer);
    return found;
}

static int64_t cavsvideo_find_cut_pos(AVIOContext *pb, int64_t key_pos)
{
    uint8_t header[4];
    int64_t search_start = key_pos > CAVS_SEQ_SEARCH_WINDOW ?
                           key_pos - CAVS_SEQ_SEARCH_WINDOW : 0;
    int64_t cut_pos = key_pos;
    int64_t restore_pos;
    int64_t seq_pos;
    int ret;

    if (key_pos <= 0)
        return key_pos;

    restore_pos = avio_tell(pb);
    if (avio_seek(pb, key_pos, SEEK_SET) >= 0) {
        ret = avio_read(pb, header, sizeof(header));
        if (ret == sizeof(header) &&
            header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x01) {
            if (header[3] == 0xB0) {
                if (restore_pos >= 0)
                    avio_seek(pb, restore_pos, SEEK_SET);
                return key_pos;
            }
        }
    }
    if (restore_pos >= 0)
        avio_seek(pb, restore_pos, SEEK_SET);

    seq_pos = cavsvideo_find_prev_code(pb, key_pos, 0xB0);
    if (seq_pos >= search_start)
        cut_pos = seq_pos;

    return cut_pos;
}

static int cavsvideo_read_header(AVFormatContext *s)
{
    CAVSVideoDemuxerContext *cavs = s->priv_data;
    int ret = ff_raw_video_read_header(s);

    if (ret < 0)
        return ret;

    cavs->last_seek_pos = -1;
    cavs->last_seek_ts  = AV_NOPTS_VALUE;
    return 0;
}

static const AVIndexEntry *cavsvideo_select_entry(AVFormatContext *s, AVStream *st,
                                                  int64_t timestamp, int flags)
{
    CAVSVideoDemuxerContext *cavs = s->priv_data;
    const FFStream *sti = ffstream(st);
    int index = av_index_search_timestamp(st, timestamp, flags);
    int64_t cur_dts = sti->cur_dts;

    if (index < 0)
        return NULL;

    if (flags & AVSEEK_FLAG_BACKWARD) {
        const AVIndexEntry *entry = avformat_index_get_entry(st, index);

        while (entry) {
            if (entry->timestamp >= timestamp) {
                index--;
            } else if (cavs->last_seek_ts != AV_NOPTS_VALUE &&
                       entry->timestamp >= cavs->last_seek_ts) {
                index--;
            } else if (cavs->last_seek_pos >= 0 && entry->pos >= cavs->last_seek_pos) {
                index--;
            } else if (cur_dts != AV_NOPTS_VALUE && entry->timestamp >= cur_dts) {
                index--;
            } else {
                break;
            }
            entry = avformat_index_get_entry(st, index);
        }

        return entry;
    }

    return avformat_index_get_entry(st, index);
}

static int cavsvideo_read_seek(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    CAVSVideoDemuxerContext *cavs = s->priv_data;
    AVStream *st = s->streams[stream_index];
    const AVIndexEntry *entry;
    int64_t pos;

    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL))
        return AVERROR(ENOSYS);

    entry = cavsvideo_select_entry(s, st, timestamp, flags);
    if (entry) {
        pos = cavsvideo_find_cut_pos(s->pb, entry->pos);
        if (avio_seek(s->pb, pos, SEEK_SET) < 0)
            return AVERROR(EIO);
        cavs->last_seek_pos = entry->pos;
        cavs->last_seek_ts  = entry->timestamp;
        s->io_repositioned = 1;
        avpriv_update_cur_dts(s, st, entry->timestamp);
        return 0;
    }

    if (!(flags & AVSEEK_FLAG_BACKWARD))
        return -1;

    entry = avformat_index_get_entry_from_timestamp(st, timestamp, 0);
    if (entry)
        pos = cavsvideo_find_cut_pos(s->pb, entry->pos);
    else if (cavs->last_seek_pos >= 0)
        pos = cavs->last_seek_pos;
    else
        pos = avio_tell(s->pb);

    if (pos <= 0) {
        pos = avio_size(s->pb);
        if (pos <= 0)
            return -1;
    }

    pos = cavsvideo_find_prev_code(s->pb, pos, 0xB3);
    if (pos < 0)
        return -1;

    cavs->last_seek_pos = pos;
    pos = cavsvideo_find_cut_pos(s->pb, pos);
    if (avio_seek(s->pb, pos, SEEK_SET) < 0)
        return AVERROR(EIO);

    s->io_repositioned = 1;
    avpriv_update_cur_dts(s, st, timestamp);
    return 0;
}

const FFInputFormat ff_cavsvideo_demuxer = {
    .p.name         = "cavsvideo",
    .p.long_name    = NULL_IF_CONFIG_SMALL("raw Chinese AVS (Audio Video Standard)"),
    .p.extensions   = "avs",
    .p.flags        = AVFMT_GENERIC_INDEX | AVFMT_NOTIMESTAMPS,
    .p.priv_class   = &ff_rawvideo_demuxer_class,
    .read_probe     = cavsvideo_probe,
    .read_header    = cavsvideo_read_header,
    .read_packet    = ff_raw_read_partial_packet,
    .read_seek      = cavsvideo_read_seek,
    .raw_codec_id   = AV_CODEC_ID_CAVS,
    .priv_data_size = sizeof(CAVSVideoDemuxerContext),
};
