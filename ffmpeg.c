#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

#include "cons.h"
#include "ishio.h"

#define DEFAULT_RATE	30

#define DEFAULT_SAMPLERATE	48000
#define DEFAULT_BITRATE		128000

struct ffmpeg_cons {
	struct cons cons;

	uint16_t height;
	uint16_t width;

	AVFrame *argb_frame;
	AVFrame *yuv_frame;
	int64_t video_pts;

	AVAudioFifo *audio_fifo;
	AVFrame *audio_frame; /* Used to calculate frames */
	int64_t audio_pts;

	SwsContext *sws; /* Used to convert ARGB to YUV420P */
	SwrContext *swr; /* Used to convert PCM_16LE to AAC */

	AVFormatContext *fmt;
	AVCodecContext *vcctx;
	AVCodecContext *acctx;
	AVStream *vstream;
	AVStream *astream;

	pthread_mutex_t mlock;
};

struct cons*
cons_new(uint16_t width, uint16_t height, uint8_t type, const char *output)
{
	struct ffmpeg_cons *fcons = calloc(1, sizeof(struct ffmpeg_cons));

	/*
	 * Don't fill the screen
	 */
	av_log_set_level(AV_LOG_WARNING);

	if (fcons == NULL)
		return NULL;

	int ret = avformat_alloc_output_context2(&fcons->fmt, NULL, "matroska", output);
	if (ret < 0) {
		free(fcons);
		return NULL;
	}

	fcons->height = height;
	fcons->width = width;

	/*
	 * Acessing ressources
	 */
	ret = avio_open(&fcons->fmt->pb, output, AVIO_FLAG_WRITE);
	if (ret < 0) {
		avformat_free_context(fcons->fmt);
		free(fcons);
		return NULL;
	}

	/*
	 * Video Stream
	 */
	const AVCodec *vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
	fcons->vstream = avformat_new_stream(fcons->fmt, NULL);

	if (fcons->vstream == NULL) {
		avformat_free_context(fcons->fmt);
		free(fcons);
		return NULL;
	}

	fcons->vcctx = avcodec_alloc_context3(vcodec);
	fcons->vcctx->codec_id = AV_CODEC_ID_H264;
	fcons->vcctx->codec_type = AVMEDIA_TYPE_VIDEO;

	fcons->vcctx->width = width;
	fcons->vcctx->height = height;

	fcons->vcctx->pix_fmt = AV_PIX_FMT_YUV420P;
	fcons->vcctx->time_base = (AVRational){1, DEFAULT_RATE};
	fcons->vcctx->framerate = (AVRational){DEFAULT_RATE, 1};

	if (fcons->fmt->oformat->flags & AVFMT_GLOBALHEADER)
		fcons->vcctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(fcons->vcctx, vcodec, NULL);
	if (ret < 0)
		return NULL;
	avcodec_parameters_from_context( fcons->vstream->codecpar, fcons->vcctx);
	fcons->vstream->time_base = fcons->vcctx->time_base;
	fcons->video_pts = 0;

	/*
	 * Audio Stream
	 */
	const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	fcons->astream = avformat_new_stream(fcons->fmt, NULL);

	if (fcons->astream == NULL) {
		avformat_free_context(fcons->fmt);
		free(fcons);
		return NULL;
	}

	fcons->acctx = avcodec_alloc_context3(acodec);
	av_channel_layout_default(&fcons->acctx->ch_layout, 2);
	fcons->acctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	fcons->acctx->sample_rate = DEFAULT_SAMPLERATE;
	fcons->acctx->bit_rate = DEFAULT_BITRATE;
	fcons->acctx->time_base = (AVRational){1, DEFAULT_SAMPLERATE};

	if (fcons->fmt->oformat->flags & AVFMT_GLOBALHEADER)
		fcons->acctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(fcons->acctx, acodec, NULL);
	if (ret < 0)
		return NULL;
	avcodec_parameters_from_context(fcons->astream->codecpar, fcons->acctx);
	fcons->astream->time_base = fcons->acctx->time_base;

	fcons->audio_fifo = av_audio_fifo_alloc(fcons->acctx->sample_fmt, 
		fcons->acctx->ch_layout.nb_channels, 1024 * 4);

	/*
	 * Some options
	 */
	av_opt_set(fcons->vcctx->priv_data, "preset", "veryfast", 0);
	av_opt_set(fcons->acctx->priv_data, "tune", "zerolatency", 0);

	/*
	 * YUV context
	 */
	AVFrame *argb = av_frame_alloc();
	argb->format = AV_PIX_FMT_ARGB;
	argb->width = width;
	argb->height = height;

	av_frame_get_buffer(argb, 32);

	AVFrame *yuv = av_frame_alloc();
	yuv->format = AV_PIX_FMT_YUV420P;
	yuv->width = width;
	yuv->height = height;

	av_frame_get_buffer(yuv, 32);

	fcons->audio_frame = av_frame_alloc();
	fcons->audio_frame->format = AV_SAMPLE_FMT_FLTP;
	fcons->audio_frame->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
	fcons->audio_frame->sample_rate = 48000;
	fcons->audio_frame->nb_samples = 1024;
	fcons->audio_pts = 0;

	av_frame_get_buffer(fcons->audio_frame, 32);
	av_frame_make_writable(fcons->audio_frame);

	fcons->audio_frame->pts = 0;

	fcons->sws = sws_getContext(
		width,
		height,
		AV_PIX_FMT_ARGB,
		width,
		height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);

	ret = swr_alloc_set_opts2(&fcons->swr,
					&(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO, /* Output section */
					AV_SAMPLE_FMT_FLTP,
					48000,
					&(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO, /* Input section (must match sndio)*/
					AV_SAMPLE_FMT_S16,
					48000,
					0,
					NULL
	);
	swr_init(fcons->swr);

	if (ret != 0) {
		return NULL;
	}

	av_frame_make_writable(argb);
	av_frame_make_writable(yuv);

	fcons->argb_frame = argb;
	fcons->yuv_frame = yuv;

	/*
	 * Actual start
	 */
	ret = avformat_write_header(fcons->fmt, NULL);
	if (ret < 0) {
		avformat_free_context(fcons->fmt);
		sws_free_context(&fcons->sws);
		free(fcons);
		return NULL;
	}

	pthread_mutex_init(&fcons->mlock, NULL);

	return &(fcons->cons);
}

void
cons_free(struct cons *cons)
{
	struct ffmpeg_cons *fcons = (struct ffmpeg_cons *)cons;
	av_write_trailer(fcons->fmt);
	sws_free_context(&fcons->sws);
	av_audio_fifo_free(fcons->audio_fifo);
	avio_closep(&fcons->fmt->pb);
	avformat_free_context(fcons->fmt);
	free(fcons);
}

int
cons_write_video(struct cons *cons, struct ishio_video_buf *buf)
{
	struct ffmpeg_cons *fcons = (struct ffmpeg_cons *)cons;

	for (int y = 0; y < buf->height; y++) {
		memcpy(
			fcons->argb_frame->data[0] + y * fcons->argb_frame->linesize[0],
			buf->data + y * buf->width * buf->depth,
			buf->width * buf->depth
		);
	}

	sws_scale(
		fcons->sws,
		(const uint8_t * const *)fcons->argb_frame->data,
		fcons->argb_frame->linesize,
		0,
		fcons->height,
		fcons->yuv_frame->data,
		fcons->yuv_frame->linesize
	);

	/*
	 * Setting up the time
	 */
 	int64_t ns = (int64_t)buf->ts.tv_sec * 1000000000LL + buf->ts.tv_nsec;
	fcons->yuv_frame->pts = av_rescale_q( ns, (AVRational){1, 1000000000}, fcons->vcctx->time_base);

	int ret = avcodec_send_frame(fcons->vcctx, fcons->yuv_frame);
	if (ret < 0)
		return -1;

	AVPacket *pkt = av_packet_alloc();
	
	while ((ret = avcodec_receive_packet(fcons->vcctx, pkt)) == 0) {
		av_packet_rescale_ts(pkt, fcons->vcctx->time_base, fcons->vstream->time_base);
		pkt->stream_index = fcons->vstream->index;

		pthread_mutex_lock(&fcons->mlock);
		ret = av_interleaved_write_frame(fcons->fmt, pkt);
		pthread_mutex_unlock(&fcons->mlock);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	return 0;
}

int
cons_write_audio(struct cons *cons, struct ishio_audio_buf *buf)
{
	struct ffmpeg_cons *fcons = (struct ffmpeg_cons *)cons;

	const uint8_t *in[1] = { (const uint8_t *)buf->data };

	uint8_t **out = NULL;
	int out_count = swr_get_out_samples(fcons->swr, buf->count);

	if (out_count <= 0)
		return 0;

	int ret = av_samples_alloc_array_and_samples(
		&out,
		NULL,
		2,
		out_count,
		AV_SAMPLE_FMT_FLTP,
		0
	);

	if (ret < 0)
		return -1;
	
	ret = swr_convert(
		fcons->swr,
		out,
		out_count,
		in,
		buf->count
	);

	if (ret < 0) {
		av_freep(&out[0]);
		av_freep(&out);
		return -1;
	}

	if (av_audio_fifo_realloc(fcons->audio_fifo, av_audio_fifo_size(fcons->audio_fifo) + ret) < 0) {
		av_freep(&out[0]);
		av_freep(&out);
		return -1;
	}

	ret = av_audio_fifo_write(fcons->audio_fifo, (void **)out, ret );
	av_freep(&out[0]);
	av_freep(&out);

	while (av_audio_fifo_size(fcons->audio_fifo) >= 1024) {
		int ret = av_frame_make_writable(fcons->audio_frame);
		if (ret < 0)
			return ret;

		if (av_audio_fifo_read(fcons->audio_fifo, (void **)fcons->audio_frame->data, 1024) < 0)
			return -1;
		
		fcons->audio_frame->pts = fcons->audio_pts;
		fcons->audio_pts += 1024;
		
		ret = avcodec_send_frame(fcons->acctx, fcons->audio_frame);
		if (ret < 0)
			return -1;

		AVPacket *pkt = av_packet_alloc();
		if (pkt == NULL)
			return -1;

		while ((ret = avcodec_receive_packet(fcons->acctx, pkt)) == 0) {
			av_packet_rescale_ts(pkt, fcons->acctx->time_base, fcons->astream->time_base);
			pkt->stream_index = fcons->astream->index;

			pthread_mutex_lock(&fcons->mlock);
			ret = av_interleaved_write_frame(fcons->fmt, pkt);
			pthread_mutex_unlock(&fcons->mlock);
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
	}

	return 0;
}
