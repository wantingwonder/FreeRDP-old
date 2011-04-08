/*
   FreeRDP: A Remote Desktop Protocol client.
   Video Redirection Virtual Channel - FFmpeg Decoder

   Copyright 2010-2011 Vic Lee

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "tsmf_constants.h"
#include "tsmf_decoder.h"

/* Compatibility with older FFmpeg */
#if LIBAVUTIL_VERSION_MAJOR < 50
#define AVMEDIA_TYPE_VIDEO 0
#define AVMEDIA_TYPE_AUDIO 1
#endif

typedef struct _TSMFFFmpegDecoder
{
	ITSMFDecoder iface;

	int media_type;
	enum CodecID codec_id;
	AVCodecContext * codec_context;
	AVCodec * codec;
	AVFrame * frame;
	int prepared;

	uint8 * decoded_data;
	uint32 decoded_size;
	uint32 decoded_size_max;
} TSMFFFmpegDecoder;

static int
tsmf_ffmpeg_init_context(ITSMFDecoder * decoder)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	mdecoder->codec_context = avcodec_alloc_context();
	if (!mdecoder->codec_context)
	{
		LLOGLN(0, ("tsmf_ffmpeg_init_context: avcodec_alloc_context failed."));
		return 1;
	}

	return 0;
}

static int
tsmf_ffmpeg_init_video_stream(ITSMFDecoder * decoder, const TS_AM_MEDIA_TYPE * media_type)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	mdecoder->codec_context->width = media_type->Width;
	mdecoder->codec_context->height = media_type->Height;
	mdecoder->codec_context->bit_rate = media_type->BitRate;
	mdecoder->codec_context->time_base.den = media_type->SamplesPerSecond.Numerator;
	mdecoder->codec_context->time_base.num = media_type->SamplesPerSecond.Denominator;

	mdecoder->frame = avcodec_alloc_frame();

	return 0;
}

static int
tsmf_ffmpeg_init_audio_stream(ITSMFDecoder * decoder, const TS_AM_MEDIA_TYPE * media_type)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	mdecoder->codec_context->sample_rate = media_type->SamplesPerSecond.Numerator;
	mdecoder->codec_context->bit_rate = media_type->BitRate;
	mdecoder->codec_context->channels = media_type->Channels;
	mdecoder->codec_context->block_align = media_type->BlockAlign;

	/* FFmpeg's float_to_int16_interleave_sse2 would crash at least in WMA decoder.
	   We disable sse2 to workaround it, however this should be further investigated. */
	mdecoder->codec_context->dsp_mask = FF_MM_SSE2 | FF_MM_MMX2;

	return 0;
}

static int
tsmf_ffmpeg_init_stream(ITSMFDecoder * decoder, const TS_AM_MEDIA_TYPE * media_type)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	mdecoder->codec = avcodec_find_decoder(mdecoder->codec_id);
	if (!mdecoder->codec)
	{
		LLOGLN(0, ("tsmf_ffmpeg_init_stream: avcodec_find_decoder failed."));
		return 1;
	}

	mdecoder->codec_context->codec_id = mdecoder->codec_id;
	mdecoder->codec_context->codec_type = mdecoder->media_type;

	if (mdecoder->media_type == AVMEDIA_TYPE_VIDEO)
	{
		if (tsmf_ffmpeg_init_video_stream(decoder, media_type))
			return 1;
	}
	else if (mdecoder->media_type == AVMEDIA_TYPE_AUDIO)
	{
		if (tsmf_ffmpeg_init_audio_stream(decoder, media_type))
			return 1;
	}

	if (media_type->ExtraData)
	{
		mdecoder->codec_context->extradata_size = media_type->ExtraDataSize;
		mdecoder->codec_context->extradata = malloc(mdecoder->codec_context->extradata_size);
		memcpy(mdecoder->codec_context->extradata, media_type->ExtraData, media_type->ExtraDataSize);
	}

	if (mdecoder->codec->capabilities & CODEC_CAP_TRUNCATED)
		mdecoder->codec_context->flags |= CODEC_FLAG_TRUNCATED;

	return 0;
}

static int
tsmf_ffmpeg_prepare(ITSMFDecoder * decoder)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	if (avcodec_open(mdecoder->codec_context, mdecoder->codec) < 0)
	{
		LLOGLN(0, ("tsmf_ffmpeg_prepare: avcodec_open failed."));
		return 1;
	}

	mdecoder->prepared = 1;

	return 0;
}

static int
tsmf_ffmpeg_set_format(ITSMFDecoder * decoder, const TS_AM_MEDIA_TYPE * media_type)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	switch (media_type->MajorType)
	{
		case TSMF_MAJOR_TYPE_VIDEO:
			mdecoder->media_type = AVMEDIA_TYPE_VIDEO;
			break;
		case TSMF_MAJOR_TYPE_AUDIO:
			mdecoder->media_type = AVMEDIA_TYPE_AUDIO;
			break;
		default:
			return 1;
	}
	switch (media_type->SubType)
	{
		case TSMF_SUB_TYPE_WVC1:
			mdecoder->codec_id = CODEC_ID_VC1;
			break;
		case TSMF_SUB_TYPE_WMA2:
			mdecoder->codec_id = CODEC_ID_WMAV2;
			break;
		case TSMF_SUB_TYPE_WMA9:
			mdecoder->codec_id = CODEC_ID_WMAPRO;
			break;
		case TSMF_SUB_TYPE_MP3:
			mdecoder->codec_id = CODEC_ID_MP3;
			break;
		case TSMF_SUB_TYPE_MP2A:
			mdecoder->codec_id = CODEC_ID_MP2;
			break;
		case TSMF_SUB_TYPE_MP2V:
			mdecoder->codec_id = CODEC_ID_MPEG2VIDEO;
			break;
		default:
			return 1;
	}

	if (tsmf_ffmpeg_init_context(decoder))
		return 1;
	if (tsmf_ffmpeg_init_stream(decoder, media_type))
		return 1;
	if (tsmf_ffmpeg_prepare(decoder))
		return 1;

	return 0;
}

static int
tsmf_ffmpeg_decode_video(ITSMFDecoder * decoder, const uint8 * data, uint32 data_size, uint32 extensions)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;
	int decoded;
	int len;
	int ret = 0;
	AVFrame * frame;

#if LIBAVCODEC_VERSION_MAJOR < 52
	len = avcodec_decode_video(mdecoder->codec_context, mdecoder->frame, &decoded, data, data_size);
#else
	{
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = (uint8 *) data;
		pkt.size = data_size;
		if (extensions & TSMM_SAMPLE_EXT_CLEANPOINT)
			pkt.flags |= AV_PKT_FLAG_KEY;
		len = avcodec_decode_video2(mdecoder->codec_context, mdecoder->frame, &decoded, &pkt);
	}
#endif

	if (len < 0)
	{
		LLOGLN(0, ("tsmf_ffmpeg_decode_video: avcodec_decode_video failed (%d)", len));
		ret = 1;
	}
	else if (!decoded)
	{
		LLOGLN(0, ("tsmf_ffmpeg_decode_video: data_size %d, no frame is decoded.", data_size));
		ret = 1;
	}
	else
	{
		LLOGLN(10, ("tsmf_ffmpeg_decode_video: linesize[0] %d linesize[1] %d linesize[2] %d linesize[3] %d",
			mdecoder->frame->linesize[0], mdecoder->frame->linesize[1],
			mdecoder->frame->linesize[2], mdecoder->frame->linesize[3]));

		mdecoder->decoded_size = avpicture_get_size(mdecoder->codec_context->pix_fmt,
			mdecoder->codec_context->width, mdecoder->codec_context->height);
		mdecoder->decoded_data = malloc(mdecoder->decoded_size);
		frame = avcodec_alloc_frame();
		avpicture_fill((AVPicture *) frame, mdecoder->decoded_data,
			mdecoder->codec_context->pix_fmt,
			mdecoder->codec_context->width, mdecoder->codec_context->height);

		av_picture_copy((AVPicture *) frame, (AVPicture *) mdecoder->frame,
			mdecoder->codec_context->pix_fmt,
			mdecoder->codec_context->width, mdecoder->codec_context->height);

		av_free(frame);
	}

	return ret;
}

static int
tsmf_ffmpeg_decode_audio(ITSMFDecoder * decoder, const uint8 * data, uint32 data_size, uint32 extensions)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;
	int len;
	int frame_size;
	uint32 src_size;
	uint8 * pad_data;
	uint8 * src;
	uint8 * dst;

	if (mdecoder->decoded_size_max == 0)
		mdecoder->decoded_size_max = AVCODEC_MAX_AUDIO_FRAME_SIZE;
	mdecoder->decoded_data = malloc(mdecoder->decoded_size_max);
	dst = mdecoder->decoded_data;
	pad_data = malloc(data_size + FF_INPUT_BUFFER_PADDING_SIZE);
	memcpy(pad_data, data, data_size);
	memset(pad_data + data_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
	src = pad_data;
	src_size = data_size;

	while (src_size > 0)
	{
		/* Ensure enough space for decoding */
		if (mdecoder->decoded_size_max - mdecoder->decoded_size < AVCODEC_MAX_AUDIO_FRAME_SIZE)
		{
			mdecoder->decoded_size_max *= 2;
			mdecoder->decoded_data = realloc(mdecoder->decoded_data, mdecoder->decoded_size_max);
			dst = mdecoder->decoded_data + mdecoder->decoded_size;
		}
		frame_size = mdecoder->decoded_size_max - mdecoder->decoded_size;
#if LIBAVCODEC_VERSION_MAJOR < 52
		len = avcodec_decode_audio2(mdecoder->codec_context,
			(int16_t *) dst, &frame_size,
			src, src_size);
#else
		{
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = src;
			pkt.size = src_size;
			len = avcodec_decode_audio3(mdecoder->codec_context,
				(int16_t *) dst, &frame_size, &pkt);
		}
#endif
		if (len <= 0 || frame_size <= 0)
		{
			LLOGLN(0, ("tsmf_ffmpeg_decode_audio: erro decoding"));
			break;
		}
		src += len;
		src_size -= len;
		mdecoder->decoded_size += frame_size;
		dst += frame_size;
	}

	if (mdecoder->decoded_size == 0)
	{
		free(mdecoder->decoded_data);
		mdecoder->decoded_data = NULL;
	}
	free(pad_data);

	LLOGLN(10, ("tsmf_ffmpeg_decode_audio: data_size %d decoded_size %d",
		data_size, mdecoder->decoded_size));

	return 0;
}

static int
tsmf_ffmpeg_decode(ITSMFDecoder * decoder, const uint8 * data, uint32 data_size, uint32 extensions)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	if (mdecoder->decoded_data)
	{
		free(mdecoder->decoded_data);
		mdecoder->decoded_data = NULL;
	}
	mdecoder->decoded_size = 0;

	switch (mdecoder->media_type)
	{
		case AVMEDIA_TYPE_VIDEO:
			return tsmf_ffmpeg_decode_video(decoder, data, data_size, extensions);
		case AVMEDIA_TYPE_AUDIO:
			return tsmf_ffmpeg_decode_audio(decoder, data, data_size, extensions);
		default:
			LLOGLN(0, ("tsmf_ffmpeg_decode: unknown media type."));
			return 1;
	}
}

uint8 *
tsmf_ffmpeg_get_decoded_data(ITSMFDecoder * decoder, uint32 * size)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;
	uint8 * buf;

	*size = mdecoder->decoded_size;
	buf = mdecoder->decoded_data;
	mdecoder->decoded_data = NULL;
	mdecoder->decoded_size = 0;
	return buf;
}

uint32
tsmf_ffmpeg_get_decoded_format(ITSMFDecoder * decoder)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	return mdecoder->codec_context->pix_fmt;
}

static void
tsmf_ffmpeg_free(ITSMFDecoder * decoder)
{
	TSMFFFmpegDecoder * mdecoder = (TSMFFFmpegDecoder *) decoder;

	if (mdecoder->frame)
		av_free(mdecoder->frame);
	if (mdecoder->decoded_data)
		free(mdecoder->decoded_data);
	if (mdecoder->codec_context)
	{
		if (mdecoder->prepared)
			avcodec_close(mdecoder->codec_context);
		if (mdecoder->codec_context->extradata)
			free(mdecoder->codec_context->extradata);
		av_free(mdecoder->codec_context);
	}
	free(decoder);
}

static int initialized = 0;

ITSMFDecoder *
TSMFDecoderEntry(void)
{
	TSMFFFmpegDecoder * decoder;

	if (!initialized)
	{
		avcodec_init();
		avcodec_register_all();
		initialized = 1;
	}

	decoder = malloc(sizeof(TSMFFFmpegDecoder));
	memset(decoder, 0, sizeof(TSMFFFmpegDecoder));

	decoder->iface.SetFormat = tsmf_ffmpeg_set_format;
	decoder->iface.Decode = tsmf_ffmpeg_decode;
	decoder->iface.GetDecodedData = tsmf_ffmpeg_get_decoded_data;
	decoder->iface.GetDecodedFormat = tsmf_ffmpeg_get_decoded_format;
	decoder->iface.Free = tsmf_ffmpeg_free;

	return (ITSMFDecoder *) decoder;
}

