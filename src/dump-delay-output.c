/*
 * Dump Delay for OBS Studio
 * Copyright (C) 2026 Valerio Enrico Salvo (Ves)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version. See LICENSE.
 */

/*
 * dump_delay_output: an RTMP output with its own delay buffer of *encoded*
 * packets and a dump button.
 *
 * Normal operation: packets are held for delay_ms, then muxed to FLV/RTMP
 * through libavformat. The connection to the server never drops.
 *
 * On dump:
 *   1. every buffered packet is discarded (viewers never see them);
 *   2. the UI switches to the slate scene;
 *   3. packets are dropped until the first video keyframe rendered after the
 *      cut (dump time + cut margin), so nothing from before the cut leaks;
 *   4. the slate is sent at half speed (timestamps x2). A static slate in
 *      slow motion looks identical, and since output time runs twice as fast
 *      as capture time, the buffer grows back to delay_ms after delay_ms of
 *      capture. Audio is replaced by AAC silence during this phase;
 *   5. the first slate keyframe that will have been held for delay_ms is
 *      chosen as the switch point; when it arrives from the encoder the
 *      previous scene is restored, and when it is sent timestamps go back to
 *      1x and real audio resumes. Switching only at a keyframe keeps
 *      B-frames from straddling the 2x/1x boundary.
 *
 * No second encoder and no re-encoding: the only extra work is copying
 * packets. Viewers see the slate for about 2 * delay + one keyframe interval.
 *
 * Twitch VOD track: when OBS attaches a second audio encoder (Settings >
 * Output > Streaming > "Twitch VOD Track"), it is sent as Enhanced RTMP
 * multitrack audio, track 1, byte for byte as OBS's own rtmp_output does.
 * Twitch plays track 0 live and archives track 1 in the VOD, so music kept
 * off the VOD mixer stays out of the VOD. Both tracks go through the same
 * delay, dump and silence logic.
 *
 * Timestamps: "e" = encoder time (ms), "o" = output time (ms). A packet is
 * sent when o + k <= wall clock (ms); k is fixed when the first packet
 * arrives so that o + k = arrival + delay.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <media-io/audio-io.h>
#include <util/deque.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include "dump-delay.h"

#define do_log(level, format, ...) \
	blog(level, "[dump-delay] " format, ##__VA_ARGS__)

/* Connect this long before the first buffered packet is due. */
#define CONNECT_LEAD_MS 2000

/* Output time of the first packet. Small and positive, so no packet is ever
 * negative (B-frame dts, rounding between tracks): libavformat would shift
 * its own streams to fix that, but not the VOD tags written by hand. */
#define FIRST_PACKET_O_MS 100

/* audio track 0 = live, track 1 = Twitch VOD */
#define MAX_TRACKS 2

enum dd_state {
	DD_NORMAL,
	DD_WAIT_KEYFRAME,
	DD_STRETCH,
};

/* o = o_base + scale * (e - e_base) */
struct ts_map {
	int64_t o_base;
	int64_t e_base;
	int64_t scale;
};

struct queued_packet {
	struct encoder_packet pkt;
	int64_t arrival_ms;
};

struct dump_delay {
	obs_output_t *output;

	pthread_t thread;
	bool thread_active;
	bool capturing;
	volatile bool stopping;
	volatile bool dump_requested;
	volatile long status; /* enum dump_delay_status, for the dock */

	pthread_mutex_t mutex;
	struct deque queue;
	/* newest video keyframe received (protected by mutex) */
	bool have_last_kf;
	int64_t last_kf_e;
	int64_t last_kf_arrival_ms;

	struct dstr url;
	char *slate_scene;
	int64_t delay_ms;
	int64_t cut_margin_ms;

	AVFormatContext *fmt;
	AVStream *vstream;
	AVStream *astream;
	AVPacket *avpkt;
	bool header_written;
	bool vod_track; /* second audio encoder attached, sent as track 1 */
	volatile long vod_state;  /* enum dump_delay_vod, for the dock */
	volatile long vod_mixer;  /* 1-based mixer track of the VOD encoder */
	uint8_t *silence;
	int silence_size;
	double silence_ms;
	uint64_t total_bytes;

	/* worker thread only */
	bool have_k;
	int64_t k;
	enum dd_state state;
	struct ts_map map;
	int64_t cut_sys_usec;
	bool have_switch;
	int64_t switch_e;
	bool restore_requested;
	int64_t last_v_o;     /* last video dts sent */
	int64_t last_v_pts_o; /* highest video pts sent */
	/* per audio track */
	int64_t last_a_o[MAX_TRACKS];
	bool silence_active[MAX_TRACKS];
	bool silence_started[MAX_TRACKS];
	double next_silence_o[MAX_TRACKS];
};

static pthread_mutex_t active_mutex;
static struct dump_delay *active_dd;

/* UI thread only */
static char *restore_scene;

/* ------------------------------------------------------------------------ */
/* Scene switching (UI thread)                                               */

static void ui_switch_to_slate(void *param)
{
	char *slate_name = param;
	obs_source_t *slate = obs_get_source_by_name(slate_name);

	if (!slate) {
		do_log(LOG_WARNING,
		       "slate scene '%s' not found, viewers will see live content in slow motion while the delay rebuilds",
		       slate_name);
		bfree(slate_name);
		return;
	}

	obs_source_t *current = obs_frontend_get_current_scene();
	if (current != slate) {
		if (!restore_scene && current)
			restore_scene = bstrdup(obs_source_get_name(current));
		obs_frontend_set_current_scene(slate);
	}

	obs_source_release(current);
	obs_source_release(slate);
	bfree(slate_name);
}

static void ui_restore_scene(void *param)
{
	UNUSED_PARAMETER(param);

	if (!restore_scene)
		return;

	obs_source_t *scene = obs_get_source_by_name(restore_scene);
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(restore_scene);
	restore_scene = NULL;
}

void dump_delay_trigger(void)
{
	char *slate = NULL;

	pthread_mutex_lock(&active_mutex);
	if (active_dd) {
		os_atomic_set_bool(&active_dd->dump_requested, true);
		slate = bstrdup(active_dd->slate_scene ? active_dd->slate_scene
						       : "");
	}
	pthread_mutex_unlock(&active_mutex);

	if (slate)
		obs_queue_task(OBS_TASK_UI, ui_switch_to_slate, slate, false);
	else
		do_log(LOG_INFO, "dump requested, but no dump-delay stream is active");
}

enum dump_delay_status dump_delay_get_status(void)
{
	enum dump_delay_status status = DUMP_DELAY_OFF;

	pthread_mutex_lock(&active_mutex);
	if (active_dd)
		status = (enum dump_delay_status)os_atomic_load_long(
			&active_dd->status);
	pthread_mutex_unlock(&active_mutex);
	return status;
}

enum dump_delay_vod dump_delay_get_vod(int *mixer_track)
{
	enum dump_delay_vod vod = DUMP_DELAY_VOD_NONE;

	pthread_mutex_lock(&active_mutex);
	if (active_dd) {
		vod = (enum dump_delay_vod)os_atomic_load_long(
			&active_dd->vod_state);
		if (mixer_track)
			*mixer_track =
				(int)os_atomic_load_long(&active_dd->vod_mixer);
	}
	pthread_mutex_unlock(&active_mutex);
	return vod;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

static inline int64_t now_ms(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

static inline int64_t ts_to_ms(int64_t ts, const struct encoder_packet *p)
{
	return ts * 1000 * p->timebase_num / p->timebase_den;
}

static inline int64_t sys_pts_usec(const struct encoder_packet *p)
{
	return p->sys_dts_usec +
	       (p->pts - p->dts) * 1000000 * p->timebase_num / p->timebase_den;
}

static inline int64_t map_ts(const struct ts_map *m, int64_t e)
{
	return m->o_base + m->scale * (e - m->e_base);
}

static void drop_front(struct dump_delay *dd)
{
	struct queued_packet q;

	pthread_mutex_lock(&dd->mutex);
	deque_pop_front(&dd->queue, &q, sizeof(q));
	pthread_mutex_unlock(&dd->mutex);
	obs_encoder_packet_release(&q.pkt);
}

static void clear_queue(struct dump_delay *dd)
{
	struct queued_packet q;

	pthread_mutex_lock(&dd->mutex);
	while (dd->queue.size) {
		deque_pop_front(&dd->queue, &q, sizeof(q));
		obs_encoder_packet_release(&q.pkt);
	}
	pthread_mutex_unlock(&dd->mutex);
}

/* ------------------------------------------------------------------------ */
/* Muxer                                                                     */

static bool set_extradata(AVCodecParameters *par, obs_encoder_t *enc)
{
	uint8_t *data;
	size_t size;

	if (!obs_encoder_get_extra_data(enc, &data, &size) || !size)
		return false;

	par->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!par->extradata)
		return false;
	memcpy(par->extradata, data, size);
	par->extradata_size = (int)size;
	return true;
}

static int interrupt_cb(void *opaque)
{
	struct dump_delay *dd = opaque;
	return os_atomic_load_bool(&dd->stopping);
}

static void log_av_error(const char *what, int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(err, buf, sizeof(buf));
	do_log(LOG_WARNING, "%s: %s", what, buf);
}

/* Encode a few frames of silence with FFmpeg's AAC encoder and keep one
 * steady-state packet. Same sample rate and channels as the stream, so the
 * AudioSpecificConfig matches and it can be spliced in freely. */
static bool make_silence(struct dump_delay *dd, int sample_rate, int channels)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	bool ok = false;

	if (!ctx || !frame || !pkt)
		goto done;

	ctx->sample_rate = sample_rate;
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ctx->bit_rate = 128000;
	ctx->time_base = (AVRational){1, sample_rate};
	av_channel_layout_default(&ctx->ch_layout, channels);
	if (avcodec_open2(ctx, codec, NULL) < 0)
		goto done;

	frame->nb_samples = ctx->frame_size;
	frame->format = ctx->sample_fmt;
	frame->sample_rate = sample_rate;
	if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 ||
	    av_frame_get_buffer(frame, 0) < 0)
		goto done;
	av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
			       channels, ctx->sample_fmt);

	for (int i = 0; i < 8; i++) {
		frame->pts = (int64_t)i * frame->nb_samples;
		if (avcodec_send_frame(ctx, frame) < 0)
			goto done;
		while (avcodec_receive_packet(ctx, pkt) == 0) {
			bfree(dd->silence);
			dd->silence = bmemdup(pkt->data, pkt->size);
			dd->silence_size = pkt->size;
			av_packet_unref(pkt);
		}
	}

	dd->silence_ms = frame->nb_samples * 1000.0 / sample_rate;
	ok = dd->silence != NULL;

done:
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	return ok;
}

/* One FLV audio tag for the VOD track, laid out exactly like OBS's
 * flv_packet_audio_ex() for idx > 0 (Enhanced RTMP v2 multitrack):
 *   ExHeader|Multitrack, OneTrack|packet type, "mp4a", track id, payload.
 * Written into the same AVIO stream as the muxer's own tags. */
#define FLV_AUDIO_EX_MULTITRACK ((9 << 4) | 5)
#define FLV_AUDIO_PKT_SEQ_START 0
#define FLV_AUDIO_PKT_FRAMES 1
#define VOD_TRACK_ID 1

static bool write_vod_tag(struct dump_delay *dd, const uint8_t *data,
			  size_t size, int64_t ts_ms, int pkt_type)
{
	AVIOContext *pb = dd->fmt->pb;
	uint32_t body = (uint32_t)size + 7;

	avio_w8(pb, 8); /* audio */
	avio_wb24(pb, body);
	avio_wb24(pb, (uint32_t)(ts_ms & 0xFFFFFF));
	avio_w8(pb, (uint8_t)((ts_ms >> 24) & 0x7F));
	avio_wb24(pb, 0); /* stream id */
	avio_w8(pb, FLV_AUDIO_EX_MULTITRACK);
	avio_w8(pb, pkt_type); /* MULTITRACKTYPE_ONE_TRACK (0) | type */
	avio_write(pb, (const unsigned char *)"mp4a", 4);
	avio_w8(pb, VOD_TRACK_ID);
	avio_write(pb, data, (int)size);
	avio_wb32(pb, body + 11); /* previous tag size */

	if (pb->error < 0) {
		log_av_error("VOD track write failed", pb->error);
		return false;
	}
	dd->total_bytes += size;
	return true;
}

/* Called at stream start, once OBS has attached its encoders, so the dock can
 * show the VOD track state before the delayed connection is made. */
static void setup_vod_track(struct dump_delay *dd)
{
	obs_encoder_t *aenc = obs_output_get_audio_encoder(dd->output, 0);
	obs_encoder_t *vod = obs_output_get_audio_encoder(dd->output, 1);

	dd->vod_track = false;
	os_atomic_set_long(&dd->vod_mixer, 0);
	if (!vod) {
		os_atomic_set_long(&dd->vod_state, DUMP_DELAY_VOD_OFF);
		do_log(LOG_INFO, "Twitch VOD track: off (OBS attached no VOD track)");
		return;
	}

	os_atomic_set_long(&dd->vod_mixer,
			   (long)obs_encoder_get_mixer_index(vod) + 1);

	if (obs_encoder_get_sample_rate(vod) != obs_encoder_get_sample_rate(aenc) ||
	    audio_output_get_channels(obs_encoder_audio(vod)) !=
		    audio_output_get_channels(obs_encoder_audio(aenc))) {
		os_atomic_set_long(&dd->vod_state, DUMP_DELAY_VOD_MISMATCH);
		do_log(LOG_WARNING,
		       "VOD track encoder format differs from the stream track, VOD track not sent");
		return;
	}

	dd->vod_track = true;
	os_atomic_set_long(&dd->vod_state, DUMP_DELAY_VOD_ON);
	do_log(LOG_INFO, "Twitch VOD track: mixer track %zu sent as audio track 1",
	       obs_encoder_get_mixer_index(vod) + 1);
}

static bool open_muxer(struct dump_delay *dd)
{
	obs_encoder_t *venc = obs_output_get_video_encoder(dd->output);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(dd->output, 0);
	int sample_rate = (int)obs_encoder_get_sample_rate(aenc);
	int channels = (int)audio_output_get_channels(obs_encoder_audio(aenc));
	int ret;

	if (!make_silence(dd, sample_rate, channels)) {
		do_log(LOG_WARNING, "could not create AAC silence");
		return false;
	}

	ret = avformat_alloc_output_context2(&dd->fmt, NULL, "flv",
					     dd->url.array);
	if (ret < 0) {
		log_av_error("avformat_alloc_output_context2", ret);
		return false;
	}
	dd->fmt->interrupt_callback.callback = interrupt_cb;
	dd->fmt->interrupt_callback.opaque = dd;

	dd->vstream = avformat_new_stream(dd->fmt, NULL);
	dd->astream = avformat_new_stream(dd->fmt, NULL);
	if (!dd->vstream || !dd->astream)
		return false;

	AVCodecParameters *vpar = dd->vstream->codecpar;
	vpar->codec_type = AVMEDIA_TYPE_VIDEO;
	vpar->codec_id = AV_CODEC_ID_H264;
	vpar->width = (int)obs_encoder_get_width(venc);
	vpar->height = (int)obs_encoder_get_height(venc);
	dd->vstream->time_base = (AVRational){1, 1000};
	if (!set_extradata(vpar, venc)) {
		do_log(LOG_WARNING, "video encoder has no extradata");
		return false;
	}

	AVCodecParameters *apar = dd->astream->codecpar;
	apar->codec_type = AVMEDIA_TYPE_AUDIO;
	apar->codec_id = AV_CODEC_ID_AAC;
	apar->sample_rate = sample_rate;
	apar->frame_size = (int)obs_encoder_get_frame_size(aenc);
	av_channel_layout_default(&apar->ch_layout, channels);
	dd->astream->time_base = (AVRational){1, 1000};
	if (!set_extradata(apar, aenc)) {
		do_log(LOG_WARNING, "audio encoder has no extradata");
		return false;
	}

	/* the URL contains the stream key, never log it */
	ret = avio_open2(&dd->fmt->pb, dd->url.array, AVIO_FLAG_WRITE,
			 &dd->fmt->interrupt_callback, NULL);
	if (ret < 0) {
		log_av_error("connect failed", ret);
		return false;
	}

	ret = avformat_write_header(dd->fmt, NULL);
	if (ret < 0) {
		log_av_error("avformat_write_header", ret);
		return false;
	}
	dd->header_written = true;

	if (dd->vod_track) {
		uint8_t *asc;
		size_t asc_size;
		obs_encoder_t *vod = obs_output_get_audio_encoder(dd->output, 1);

		if (!obs_encoder_get_extra_data(vod, &asc, &asc_size) ||
		    !asc_size) {
			do_log(LOG_WARNING, "VOD track encoder has no extradata");
			return false;
		}
		if (!write_vod_tag(dd, asc, asc_size, 0,
				   FLV_AUDIO_PKT_SEQ_START))
			return false;
	}

	dd->avpkt = av_packet_alloc();
	return dd->avpkt != NULL;
}

static void close_muxer(struct dump_delay *dd)
{
	if (dd->fmt) {
		if (dd->header_written)
			av_write_trailer(dd->fmt);
		avio_closep(&dd->fmt->pb);
		avformat_free_context(dd->fmt);
		dd->fmt = NULL;
	}
	dd->header_written = false;
	av_packet_free(&dd->avpkt);
	bfree(dd->silence);
	dd->silence = NULL;
}

static bool write_av(struct dump_delay *dd, AVStream *st, const uint8_t *data,
		     size_t size, int64_t dts_ms, int64_t pts_ms, bool keyframe)
{
	AVPacket *pkt = dd->avpkt;
	int ret;

	if (av_new_packet(pkt, (int)size) < 0)
		return false;
	memcpy(pkt->data, data, size);
	pkt->stream_index = st->index;
	pkt->dts = dts_ms;
	pkt->pts = pts_ms;
	if (keyframe)
		pkt->flags |= AV_PKT_FLAG_KEY;
	av_packet_rescale_ts(pkt, (AVRational){1, 1000}, st->time_base);

	ret = av_write_frame(dd->fmt, pkt);
	av_packet_unref(pkt);
	if (ret < 0) {
		log_av_error("write failed", ret);
		return false;
	}

	dd->total_bytes += size;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Delay / dump logic (worker thread)                                        */

static bool send_packet(struct dump_delay *dd, struct encoder_packet *p)
{
	int64_t dts = map_ts(&dd->map, ts_to_ms(p->dts, p));
	int64_t pts = map_ts(&dd->map, ts_to_ms(p->pts, p));

	if (p->type == OBS_ENCODER_VIDEO) {
		if (dts <= dd->last_v_o) {
			pts += dd->last_v_o + 1 - dts;
			dts = dd->last_v_o + 1;
		}
		dd->last_v_o = dts;
		if (pts > dd->last_v_pts_o)
			dd->last_v_pts_o = pts;
		return write_av(dd, dd->vstream, p->data, p->size, dts, pts,
				p->keyframe);
	}

	size_t t = p->track_idx;
	if (t > 0 && !dd->vod_track)
		return true;

	/* overlaps the silence already sent */
	if (dts <= dd->last_a_o[t])
		return true;

	dd->last_a_o[t] = dts;
	dd->silence_active[t] = false;
	dd->silence_started[t] = false;
	if (t == 0)
		return write_av(dd, dd->astream, p->data, p->size, dts, pts,
				true);
	return write_vod_tag(dd, p->data, p->size, dts, FLV_AUDIO_PKT_FRAMES);
}

static void begin_dump(struct dump_delay *dd)
{
	clear_queue(dd);

	dd->state = DD_WAIT_KEYFRAME;
	dd->cut_sys_usec = (int64_t)(os_gettime_ns() / 1000) +
			   dd->cut_margin_ms * 1000;
	dd->have_switch = false;
	dd->restore_requested = false;
	for (size_t t = 0; t < MAX_TRACKS; t++)
		dd->silence_active[t] = true;
	os_atomic_set_long(&dd->status, DUMP_DELAY_DUMPING);

	do_log(LOG_INFO, "dump: buffer discarded, waiting for a slate keyframe");
}

static void request_restore(struct dump_delay *dd)
{
	if (dd->restore_requested)
		return;
	obs_queue_task(OBS_TASK_UI, ui_restore_scene, NULL, false);
	dd->restore_requested = true;
}

/* While stretching: pick the first slate keyframe that, sent at half speed,
 * will have been held for delay_ms. Restore the live scene as soon as it has
 * arrived, so every frame before it is slate and every live frame comes after
 * it (at 1x). */
static void choose_switch_keyframe(struct dump_delay *dd)
{
	if (dd->state != DD_STRETCH || dd->have_switch)
		return;

	pthread_mutex_lock(&dd->mutex);
	bool have = dd->have_last_kf;
	int64_t kf_e = dd->last_kf_e;
	int64_t kf_arrival = dd->last_kf_arrival_ms;
	pthread_mutex_unlock(&dd->mutex);

	if (!have || kf_e <= dd->map.e_base)
		return;
	if (map_ts(&dd->map, kf_e) + dd->k - kf_arrival < dd->delay_ms)
		return;

	dd->switch_e = kf_e;
	dd->have_switch = true;
	request_restore(dd);
}

static bool process_queue(struct dump_delay *dd, int64_t now)
{
	for (;;) {
		struct queued_packet q;

		pthread_mutex_lock(&dd->mutex);
		if (!dd->queue.size) {
			pthread_mutex_unlock(&dd->mutex);
			return true;
		}
		deque_peek_front(&dd->queue, &q, sizeof(q));
		pthread_mutex_unlock(&dd->mutex);

		bool video = q.pkt.type == OBS_ENCODER_VIDEO;
		int64_t e = ts_to_ms(q.pkt.dts, &q.pkt);

		if (dd->state == DD_WAIT_KEYFRAME) {
			if (!video || !q.pkt.keyframe ||
			    sys_pts_usec(&q.pkt) < dd->cut_sys_usec) {
				drop_front(dd);
				continue;
			}

			/* First slate keyframe: continue the output timeline from
			 * "now", at half speed. Speed changes are anchored on the
			 * keyframe's pts: with B-frames its dts is earlier, and
			 * anchoring there would let frames on either side of the
			 * keyframe swap display order. */
			int64_t pts_e = ts_to_ms(q.pkt.pts, &q.pkt);
			int64_t o0 = now - dd->k;
			if (o0 <= dd->last_v_pts_o)
				o0 = dd->last_v_pts_o + 1;
			if (o0 - 2 * (pts_e - e) <= dd->last_v_o)
				o0 = dd->last_v_o + 1 + 2 * (pts_e - e);
			dd->map = (struct ts_map){o0, pts_e, 2};
			dd->state = DD_STRETCH;
		} else if (dd->state == DD_STRETCH) {
			if (!video) {
				drop_front(dd);
				continue;
			}

			if (dd->have_switch && q.pkt.keyframe &&
			    e >= dd->switch_e) {
				int64_t pts_e = ts_to_ms(q.pkt.pts, &q.pkt);
				dd->map = (struct ts_map){
					map_ts(&dd->map, pts_e), pts_e, 1};
				dd->state = DD_NORMAL;
				os_atomic_set_long(&dd->status, DUMP_DELAY_LIVE);
				do_log(LOG_INFO, "dump: delay rebuilt");
			}
		}

		if (map_ts(&dd->map, e) + dd->k > now)
			return true;

		pthread_mutex_lock(&dd->mutex);
		deque_pop_front(&dd->queue, NULL, sizeof(q));
		pthread_mutex_unlock(&dd->mutex);

		bool ok = send_packet(dd, &q.pkt);
		obs_encoder_packet_release(&q.pkt);
		if (!ok)
			return false;
	}
}

/* The same silence packet serves both tracks: same sample rate, channels
 * and AAC-LC profile as each track's own AudioSpecificConfig. */
static bool emit_silence_track(struct dump_delay *dd, size_t t, int64_t now)
{
	if (!dd->silence_active[t])
		return true;

	if (!dd->silence_started[t]) {
		dd->next_silence_o[t] =
			dd->last_a_o[t] != INT64_MIN
				? (double)dd->last_a_o[t] + dd->silence_ms
				: (double)(now - dd->k);
		dd->silence_started[t] = true;
	}

	while ((int64_t)dd->next_silence_o[t] + dd->k <= now) {
		int64_t o = (int64_t)dd->next_silence_o[t];
		bool ok;

		dd->next_silence_o[t] += dd->silence_ms;
		if (o <= dd->last_a_o[t])
			continue;

		dd->last_a_o[t] = o;
		if (t == 0)
			ok = write_av(dd, dd->astream, dd->silence,
				      dd->silence_size, o, o, true);
		else
			ok = write_vod_tag(dd, dd->silence, dd->silence_size, o,
					   FLV_AUDIO_PKT_FRAMES);
		if (!ok)
			return false;
	}
	return true;
}

static bool emit_silence(struct dump_delay *dd, int64_t now)
{
	if (!dd->have_k)
		return true;
	if (!emit_silence_track(dd, 0, now))
		return false;
	return !dd->vod_track || emit_silence_track(dd, 1, now);
}

static void reset_worker_state(struct dump_delay *dd)
{
	dd->have_k = false;
	dd->state = DD_NORMAL;
	dd->map = (struct ts_map){0, 0, 1};
	dd->have_switch = false;
	dd->have_last_kf = false;
	dd->restore_requested = false;
	dd->last_v_o = INT64_MIN;
	dd->last_v_pts_o = INT64_MIN;
	for (size_t t = 0; t < MAX_TRACKS; t++) {
		dd->last_a_o[t] = INT64_MIN;
		dd->silence_active[t] = false;
		dd->silence_started[t] = false;
	}
	dd->total_bytes = 0;
	os_atomic_set_bool(&dd->dump_requested, false);
}

/* Servers drop a publisher that sends no media for a while (MediaMTX after
 * 10 s), so connect only shortly before the first buffered packet is due. */
static bool first_packet_due(struct dump_delay *dd, int64_t now)
{
	struct queued_packet q;

	pthread_mutex_lock(&dd->mutex);
	bool have = dd->queue.size > 0;
	if (have)
		deque_peek_front(&dd->queue, &q, sizeof(q));
	pthread_mutex_unlock(&dd->mutex);

	if (!have)
		return false;

	if (!dd->have_k) {
		/* packets arrive in dts order across all tracks, so anchoring
		 * the front one keeps every later packet non-negative */
		dd->map = (struct ts_map){FIRST_PACKET_O_MS,
					  ts_to_ms(q.pkt.dts, &q.pkt), 1};
	}

	int64_t o = map_ts(&dd->map, ts_to_ms(q.pkt.dts, &q.pkt));
	if (!dd->have_k) {
		dd->k = q.arrival_ms + dd->delay_ms - o;
		dd->have_k = true;
	}
	return o + dd->k - CONNECT_LEAD_MS <= now;
}

static void *worker_thread(void *data)
{
	struct dump_delay *dd = data;
	bool ok = true;
	bool connect_failed = false;

	os_set_thread_name("dump-delay-output");

	reset_worker_state(dd);
	os_atomic_set_long(&dd->status, DUMP_DELAY_BUFFERING);
	if (!obs_output_begin_data_capture(dd->output, 0))
		return NULL;
	dd->capturing = true;

	pthread_mutex_lock(&active_mutex);
	active_dd = dd;
	pthread_mutex_unlock(&active_mutex);

	do_log(LOG_INFO, "buffering with a %lld ms delay",
	       (long long)dd->delay_ms);

	while (ok && !os_atomic_load_bool(&dd->stopping)) {
		int64_t now = now_ms();

		if (!dd->fmt && first_packet_due(dd, now)) {
			if (!open_muxer(dd)) {
				ok = false;
				connect_failed = true;
				break;
			}
			do_log(LOG_INFO, "connected");
			if (dd->state == DD_NORMAL)
				os_atomic_set_long(&dd->status, DUMP_DELAY_LIVE);
		}

		if (os_atomic_exchange_bool(&dd->dump_requested, false))
			begin_dump(dd);

		if (dd->fmt)
			ok = process_queue(dd, now) && emit_silence(dd, now);

		choose_switch_keyframe(dd);

		os_sleep_ms(2);
	}

	pthread_mutex_lock(&active_mutex);
	active_dd = NULL;
	pthread_mutex_unlock(&active_mutex);

	/* don't leave the user on the slate if the stream ends mid-dump */
	obs_queue_task(OBS_TASK_UI, ui_restore_scene, NULL, false);

	close_muxer(dd);

	if (!ok && !os_atomic_load_bool(&dd->stopping)) {
		dd->capturing = false;
		obs_output_signal_stop(dd->output,
				       connect_failed ? OBS_OUTPUT_CONNECT_FAILED
						      : OBS_OUTPUT_DISCONNECTED);
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* obs_output_info                                                           */

static const char *dd_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Output.Name");
}

static void *dd_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct dump_delay *dd = bzalloc(sizeof(*dd));
	dd->output = output;
	if (pthread_mutex_init(&dd->mutex, NULL) != 0) {
		bfree(dd);
		return NULL;
	}
	return dd;
}

static void join_worker(struct dump_delay *dd)
{
	if (!dd->thread_active)
		return;
	os_atomic_set_bool(&dd->stopping, true);
	pthread_join(dd->thread, NULL);
	dd->thread_active = false;
}

static void dd_destroy(void *data)
{
	struct dump_delay *dd = data;

	join_worker(dd);
	clear_queue(dd);
	deque_free(&dd->queue);
	pthread_mutex_destroy(&dd->mutex);
	dstr_free(&dd->url);
	bfree(dd->slate_scene);
	bfree(dd);
}

static bool load_settings(struct dump_delay *dd)
{
	obs_service_t *service = obs_output_get_service(dd->output);
	if (!service)
		return false;

	const char *server = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	if (!server || !*server || !key || !*key) {
		obs_output_set_last_error(dd->output,
					  "Server or stream key missing");
		return false;
	}

	dstr_copy(&dd->url, server);
	if (dstr_end(&dd->url) != '/')
		dstr_cat_ch(&dd->url, '/');
	dstr_cat(&dd->url, key);

	obs_data_t *settings = obs_service_get_settings(service);
	dd->delay_ms = obs_data_get_int(settings, "delay_sec") * 1000;
	dd->cut_margin_ms = obs_data_get_int(settings, "cut_margin_ms");
	bfree(dd->slate_scene);
	dd->slate_scene = bstrdup(obs_data_get_string(settings, "slate_scene"));
	obs_data_release(settings);

	if (dd->delay_ms < 1000)
		dd->delay_ms = 1000;
	return true;
}

static bool dd_start(void *data)
{
	struct dump_delay *dd = data;

	/* a previous run may have ended on its own (disconnect) */
	join_worker(dd);

	if (!obs_output_can_begin_data_capture(dd->output, 0))
		return false;
	if (!obs_output_initialize_encoders(dd->output, 0))
		return false;
	if (!load_settings(dd))
		return false;
	setup_vod_track(dd);

	if (obs_output_get_delay(dd->output) > 0)
		do_log(LOG_WARNING,
		       "OBS's own stream delay is enabled too, the delays add up; disable it in Settings > Advanced");

	clear_queue(dd);
	os_atomic_set_bool(&dd->stopping, false);
	if (pthread_create(&dd->thread, NULL, worker_thread, dd) != 0)
		return false;
	dd->thread_active = true;
	return true;
}

static void dd_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct dump_delay *dd = data;

	join_worker(dd);
	if (dd->capturing) {
		dd->capturing = false;
		obs_output_end_data_capture(dd->output);
	}
	clear_queue(dd);
}

static void dd_encoded_packet(void *data, struct encoder_packet *packet)
{
	struct dump_delay *dd = data;
	struct queued_packet q = {0};

	if (!packet) {
		obs_output_signal_stop(dd->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}
	if (packet->type == OBS_ENCODER_AUDIO && packet->track_idx >= MAX_TRACKS)
		return;

	obs_encoder_packet_ref(&q.pkt, packet);
	q.arrival_ms = now_ms();

	pthread_mutex_lock(&dd->mutex);
	deque_push_back(&dd->queue, &q, sizeof(q));
	if (packet->type == OBS_ENCODER_VIDEO && packet->keyframe) {
		dd->have_last_kf = true;
		dd->last_kf_e = ts_to_ms(packet->dts, packet);
		dd->last_kf_arrival_ms = q.arrival_ms;
	}
	pthread_mutex_unlock(&dd->mutex);
}

static uint64_t dd_get_total_bytes(void *data)
{
	struct dump_delay *dd = data;
	return dd->total_bytes;
}

static struct obs_output_info dump_delay_output = {
	.id = "dump_delay_output",
	/* MULTI_TRACK_AUDIO lets OBS attach its Twitch VOD track encoder at
	 * index 1, as it does for rtmp_output */
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE |
		 OBS_OUTPUT_MULTI_TRACK_AUDIO,
	.protocols = "RTMP",
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "aac",
	.get_name = dd_get_name,
	.create = dd_create,
	.destroy = dd_destroy,
	.start = dd_start,
	.stop = dd_stop,
	.encoded_packet = dd_encoded_packet,
	.get_total_bytes = dd_get_total_bytes,
};

void dump_delay_register_output(void)
{
	pthread_mutex_init(&active_mutex, NULL);
	obs_register_output(&dump_delay_output);
}
