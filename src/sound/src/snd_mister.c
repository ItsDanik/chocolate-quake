/*
 * Copyright (C) 1996-1997 Id Software, Inc.
 * Copyright (C) Henrique Barateli, <henriquejb194@gmail.com>, et al.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

// snd_mister.c -- the music codecs of the MiSTer build (MISTER_HYBRID).
//
// The MiSTer has no libvorbis, libFLAC or libmad: Ogg Vorbis, FLAC and MP3
// are decoded with the single-file decoders SDL2_mixer brings (stb_vorbis,
// dr_flac, minimp3; public domain, MIT-0 and CC0), from the copy in the
// hybrid framework's SDL2_mixer source (MISTER_CODECS in CMakeLists.txt).
// A track is read into memory whole when it starts: a few megabytes.

#include "snd_flac.h"
#include "snd_mp3.h"
#include "snd_vorbis.h"
#include "console.h"
#include "zone.h"
#include <stdlib.h>

#include "stb_vorbis/stb_vorbis.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_libs/dr_flac.h"

// its implementation is in snd_minimp3.c: names of its own are the same as
// stb_vorbis's
#define MINIMP3_NO_STDIO
#include "minimp3/minimp3_ex.h"

#define SAMPLE_WIDTH 2 // signed 16 bit, the host's byte order

// The private data of a stream: the whole file and the codec's decoder
typedef struct {
    byte* data;
    size_t size;
    void* decoder;
} track_t;

static track_t* S_MiSTer_LoadTrack(snd_stream_t* stream) {
    track_t* track = (track_t*) Z_Malloc(sizeof(track_t));
    track->size = (size_t) stream->fh.length;
    track->data = (byte*) malloc(track->size ? track->size : 1);
    if (!track->data
        || Q_fread(track->data, 1, track->size, &stream->fh) != track->size) {
        Con_Printf("Couldn't read %s\n", stream->name);
        free(track->data);
        Z_Free(track);
        return NULL;
    }
    stream->priv = track;
    return track;
}

static void S_MiSTer_FreeTrack(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    free(track->data);
    Z_Free(track);
    S_CodecUtilClose(&stream);
}

static qboolean S_MiSTer_SetInfo(snd_stream_t* stream, i32 rate,
                                 i32 channels) {
    if (channels != 1 && channels != 2) {
        Con_Printf("Unsupported number of channels %d in %s\n", channels,
                   stream->name);
        return false;
    }
    stream->info.rate = rate;
    stream->info.channels = channels;
    stream->info.bits = SAMPLE_WIDTH * 8;
    stream->info.width = SAMPLE_WIDTH;
    return true;
}

static qboolean S_MiSTer_CodecInitialize(void) {
    return true;
}

static void S_MiSTer_CodecShutdown(void) {
}


/*
================================================================================

OGG VORBIS (stb_vorbis)

================================================================================
*/

static qboolean S_VORBIS_CodecOpenStream(snd_stream_t* stream) {
    track_t* track = S_MiSTer_LoadTrack(stream);
    if (!track) {
        return false;
    }
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(track->data, (int) track->size,
                                           &error, NULL);
    if (!v) {
        Con_Printf("%s is not a valid Ogg Vorbis file (error %i).\n",
                   stream->name, error);
        free(track->data);
        Z_Free(track);
        return false;
    }
    track->decoder = v;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    if (!S_MiSTer_SetInfo(stream, (i32) info.sample_rate, info.channels)) {
        stb_vorbis_close(v);
        free(track->data);
        Z_Free(track);
        return false;
    }
    return true;
}

static i32 S_VORBIS_CodecReadStream(snd_stream_t* stream, i32 bytes,
                                    void* buffer) {
    track_t* track = (track_t*) stream->priv;
    i32 channels = stream->info.channels;
    i32 frames = stb_vorbis_get_samples_short_interleaved(
        track->decoder, channels, (short*) buffer,
        bytes / SAMPLE_WIDTH
    );
    return frames * channels * SAMPLE_WIDTH;
}

static i32 S_VORBIS_CodecRewindStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    return stb_vorbis_seek_start(track->decoder) ? 0 : -1;
}

static void S_VORBIS_CodecCloseStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    stb_vorbis_close(track->decoder);
    S_MiSTer_FreeTrack(stream);
}

snd_codec_t vorbis_codec = {
    .type = CODECTYPE_VORBIS,
    .initialized = true,
    .ext = "ogg",
    .initialize = S_MiSTer_CodecInitialize,
    .shutdown = S_MiSTer_CodecShutdown,
    .codec_open = S_VORBIS_CodecOpenStream,
    .codec_read = S_VORBIS_CodecReadStream,
    .codec_rewind = S_VORBIS_CodecRewindStream,
    .codec_jump = NULL,
    .codec_close = S_VORBIS_CodecCloseStream,
    .next = NULL,
};


/*
================================================================================

FLAC (dr_flac)

================================================================================
*/

static qboolean S_FLAC_CodecOpenStream(snd_stream_t* stream) {
    track_t* track = S_MiSTer_LoadTrack(stream);
    if (!track) {
        return false;
    }
    drflac* f = drflac_open_memory(track->data, track->size, NULL);
    if (!f) {
        Con_Printf("%s is not a valid FLAC file.\n", stream->name);
        free(track->data);
        Z_Free(track);
        return false;
    }
    track->decoder = f;
    if (!S_MiSTer_SetInfo(stream, (i32) f->sampleRate, f->channels)) {
        drflac_close(f);
        free(track->data);
        Z_Free(track);
        return false;
    }
    return true;
}

static i32 S_FLAC_CodecReadStream(snd_stream_t* stream, i32 bytes,
                                  void* buffer) {
    track_t* track = (track_t*) stream->priv;
    i32 frame_size = stream->info.channels * SAMPLE_WIDTH;
    drflac_uint64 frames = drflac_read_pcm_frames_s16(
        track->decoder, bytes / frame_size, (drflac_int16*) buffer
    );
    return (i32) frames * frame_size;
}

static i32 S_FLAC_CodecRewindStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    return drflac_seek_to_pcm_frame(track->decoder, 0) ? 0 : -1;
}

static void S_FLAC_CodecCloseStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    drflac_close(track->decoder);
    S_MiSTer_FreeTrack(stream);
}

snd_codec_t flac_codec = {
    .type = CODECTYPE_FLAC,
    .initialized = true,
    .ext = "flac",
    .initialize = S_MiSTer_CodecInitialize,
    .shutdown = S_MiSTer_CodecShutdown,
    .codec_open = S_FLAC_CodecOpenStream,
    .codec_read = S_FLAC_CodecReadStream,
    .codec_rewind = S_FLAC_CodecRewindStream,
    .codec_jump = NULL,
    .codec_close = S_FLAC_CodecCloseStream,
    .next = NULL,
};


/*
================================================================================

MP3 (minimp3)

================================================================================
*/

static qboolean S_MP3_CodecOpenStream(snd_stream_t* stream) {
    track_t* track = S_MiSTer_LoadTrack(stream);
    if (!track) {
        return false;
    }
    mp3dec_ex_t* dec = (mp3dec_ex_t*) malloc(sizeof(mp3dec_ex_t));
    if (!dec || mp3dec_ex_open_buf(dec, track->data, track->size,
                                   MP3D_SEEK_TO_SAMPLE) != 0) {
        Con_Printf("%s is not a valid MP3 file.\n", stream->name);
        free(dec);
        free(track->data);
        Z_Free(track);
        return false;
    }
    track->decoder = dec;
    if (!S_MiSTer_SetInfo(stream, dec->info.hz, dec->info.channels)) {
        mp3dec_ex_close(dec);
        free(dec);
        free(track->data);
        Z_Free(track);
        return false;
    }
    return true;
}

static i32 S_MP3_CodecReadStream(snd_stream_t* stream, i32 bytes,
                                 void* buffer) {
    track_t* track = (track_t*) stream->priv;
    size_t samples = mp3dec_ex_read(track->decoder, (mp3d_sample_t*) buffer,
                                    bytes / SAMPLE_WIDTH);
    return (i32) samples * SAMPLE_WIDTH;
}

static i32 S_MP3_CodecRewindStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    return mp3dec_ex_seek(track->decoder, 0) == 0 ? 0 : -1;
}

static void S_MP3_CodecCloseStream(snd_stream_t* stream) {
    track_t* track = (track_t*) stream->priv;
    mp3dec_ex_close(track->decoder);
    free(track->decoder);
    S_MiSTer_FreeTrack(stream);
}

snd_codec_t mp3_codec = {
    .type = CODECTYPE_MP3,
    .initialized = true,
    .ext = "mp3",
    .initialize = S_MiSTer_CodecInitialize,
    .shutdown = S_MiSTer_CodecShutdown,
    .codec_open = S_MP3_CodecOpenStream,
    .codec_read = S_MP3_CodecReadStream,
    .codec_rewind = S_MP3_CodecRewindStream,
    .codec_jump = NULL,
    .codec_close = S_MP3_CodecCloseStream,
    .next = NULL,
};

// Not needed: minimp3 skips ID3 tags by itself
i32 MP3_SkipTags(snd_stream_t* stream) {
    (void) stream;
    return 0;
}
