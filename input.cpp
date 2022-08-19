/*
 * input class
 *
 * based on libeplayer3 container_ffmpeg.c, konfetti 2010; based on code from crow
 *
 * Copyright (C) 2014  martii
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define ENABLE_LOGGING 1

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "player.h"
#include "misc.h"
#include <avcodec.h>
#include <avformat.h>

static const char *FILENAME = "eplayer/input.cpp";

#define averror(_err,_fun) ({                                                                 \
	if (_err < 0) {                                                                           \
		char _error[512];                                                                     \
		av_strerror(_err, _error, sizeof(_error));                                            \
		fprintf(stderr, "[input.cpp] line %d: %s: %d (%s)\n", __LINE__, #_fun, _err, _error); \
	}                                                                                         \
	_err;                                                                                     \
})

Input::Input()
{
	videoTrack = NULL;
	audioTrack = NULL;
	subtitleTrack = NULL;

	hasPlayThreadStarted = 0;
	seek_avts_abs = INT64_MIN;
	seek_avts_rel = 0;
	abortPlayback = false;
}

Input::~Input()
{
}

int64_t Input::calcPts(AVStream * stream, int64_t pts)
{
	if (pts == AV_NOPTS_VALUE)
		return INVALID_PTS_VALUE;

	pts = 90000 * (double)pts * stream->time_base.num / stream->time_base.den;
	if (avfc->start_time != AV_NOPTS_VALUE)
		pts -= 90000 * avfc->start_time / AV_TIME_BASE;

	if (pts < 0)
		return INVALID_PTS_VALUE;

	return pts;
}

static std::string lastlog_message;
static unsigned int lastlog_repeats;

static void log_callback(void *ptr __attribute__ ((unused)), int lvl __attribute__ ((unused)), const char *format, va_list ap)
{
	char m[1024];
	if (sizeof(m) - 1 > (unsigned int) vsnprintf(m, sizeof(m), format, ap)) {
		if (lastlog_message.compare(m) || lastlog_repeats > 999) {
			if (lastlog_repeats)
			{
					fprintf(stderr, "[input.cpp] last message repeated %u times\n", lastlog_repeats);
			}
			lastlog_message = m;
			lastlog_repeats = 0;
			fprintf(stderr, "%s", m);
		} else
			lastlog_repeats++;
	}
}

static void logprintf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	log_callback(NULL, 0, format, ap);
	va_end(ap);
}

bool Input::Play()
{
	hasPlayThreadStarted = 1;

	int64_t showtime = 0;
	bool restart_audio_resampling = false;
	bool bof = false;

	// HACK: Dropping all video frames until the first audio frame was seen will keep player2 from stuttering.
	bool audioSeen = !audioTrack;

	while (player->isPlaying && !player->abortRequested) {

		//IF MOVIE IS PAUSED, WAIT
		if (player->isPaused) {
			fprintf(stderr, "paused\n");
			usleep(100000);
			continue;
		}

		int seek_target_flag = 0;
		int64_t seek_target = INT64_MIN; // in AV_TIME_BASE units

		if (seek_avts_rel) {
			if (avfc->iformat->flags & AVFMT_TS_DISCONT) {
				if (avfc->bit_rate) {
					seek_target_flag = AVSEEK_FLAG_BYTE;
					seek_target = avio_tell(avfc->pb) + av_rescale(seek_avts_rel, avfc->bit_rate, 8 * AV_TIME_BASE);
				}
			} else {
				int64_t pts;
				if(player->output.GetPts(pts))
					seek_target = av_rescale(pts, AV_TIME_BASE, 90000ll) + seek_avts_rel;
			}
			seek_avts_rel = 0;
		} else if (seek_avts_abs != INT64_MIN) {
			if (avfc->iformat->flags & AVFMT_TS_DISCONT) {
				if (avfc->bit_rate) {
					seek_target_flag = AVSEEK_FLAG_BYTE;
					seek_target = av_rescale(seek_avts_abs, avfc->bit_rate, 8 * AV_TIME_BASE);
				}
			} else {
				seek_target = seek_avts_abs;
			}
			seek_avts_abs = INT64_MIN;
		} else if (player->isBackWard && av_gettime_relative() >= showtime) {
			player->output.ClearVideo();

			if (bof) {
				showtime = av_gettime_relative();
				usleep(100000);
				continue;
			}
			seek_avts_rel = player->Speed * AV_TIME_BASE;
			showtime = av_gettime_relative() + 300000;	//jump back every 300ms
			continue;
		} else {
			bof = false;
		}

		if (seek_target > INT64_MIN) {
			int res;
			if (seek_target < 0)
				seek_target = 0;
			res = avformat_seek_file(avfc, -1, INT64_MIN, seek_target, INT64_MAX, seek_target_flag);

			if (res < 0 && player->isBackWard)
				bof = true;

			seek_target = INT64_MIN;
			restart_audio_resampling = true;

			// flush decoders
			if (videoTrack && videoTrack->avctx && videoTrack->avctx->codec)
				avcodec_flush_buffers(videoTrack->avctx);
			if (audioTrack && audioTrack->avctx && audioTrack->avctx->codec)
				avcodec_flush_buffers(audioTrack->avctx);
			if (subtitleTrack && subtitleTrack->avctx && subtitleTrack->avctx->codec)
				avcodec_flush_buffers(subtitleTrack->avctx);

			player->output.Clear();
		}

		AVPacket packet;
		av_init_packet(&packet);

		int err = av_read_frame(avfc, &packet);
		if (err == AVERROR(EAGAIN)) {
			av_packet_unref(&packet);
			continue;
		}
		if (averror(err, av_read_frame)) // EOF?
			break; // while


		player->readCount += packet.size;

		AVStream *stream = avfc->streams[packet.stream_index];
		Track *_videoTrack = videoTrack;
		Track *_audioTrack = audioTrack;
		Track *_subtitleTrack = subtitleTrack;

		if (_videoTrack && (_videoTrack->stream == stream)) {
			int64_t pts = calcPts(stream, packet.pts);
			if (audioSeen && !player->output.Write(stream, &packet, pts))
				logprintf("writing data to video device failed\n");
		} else if (_audioTrack && (_audioTrack->stream == stream)) {
			if (restart_audio_resampling) {
				restart_audio_resampling = false;
				player->output.Write(stream, NULL, 0);
			}
			if (!player->isBackWard) {
				int64_t pts = calcPts(stream, packet.pts);
				if (!player->output.Write(stream, &packet, _videoTrack ? pts : 0))
					logprintf("writing data to audio device failed\n");
			}
			audioSeen = true;
		} else if (_subtitleTrack && (_subtitleTrack->stream == stream)) {
			int64_t pts = calcPts(stream, packet.pts);
			if (audioSeen && !player->output.WriteSubtitle(stream, &packet, pts))
				logprintf("writing data to subtitle device failed\n");
		}
		av_packet_unref(&packet);
	} /* while */

	player->output.sendLibeplayerMessage(1); /* Tell enigma2 that we stop */

	if (player->abortRequested)
		player->output.Clear();
	else
		player->output.Flush();

	abortPlayback = true;
	hasPlayThreadStarted = false;

	return true;
}

/*static*/ int interrupt_cb(void *arg)
{
	Player *player = (Player *) arg;
	bool res = player->input.abortPlayback || player->abortRequested;
	if (res)
	{
		fprintf(stderr, "[input.cpp] %s line %d: abort requested (%d/%d)\n", __func__, __LINE__, player->input.abortPlayback, player->abortRequested);
	}
	return res;
}

bool Input::Init(const char *filename, std::string headers)
{
	bool noprobe = true;
	abortPlayback = false;
#if ENABLE_LOGGING
	av_log_set_flags(AV_LOG_SKIP_REPEATED);
	av_log_set_level(AV_LOG_INFO);
	/* out commented here for using ffmpeg default: av_log_default_callback
	because of better log level handling */
	//av_log_set_callback(log_callback);
#else
	av_log_set_level(AV_LOG_PANIC);
#endif

	if (!filename) {
		fprintf(stderr, "filename NULL\n");
		return false;
	}

	if (!headers.empty())
	{
		fprintf(stderr, "%s %s %d: %s\n%s\n", FILENAME, __func__, __LINE__, filename, headers.c_str());
		headers += "\r\n";
	}
	else
	{
		fprintf(stderr, "%s %s %d: %s\n", FILENAME, __func__, __LINE__, filename);
	}

	avformat_network_init();

	videoTrack = NULL;
	audioTrack = NULL;
	subtitleTrack = NULL;

again:
	avfc = avformat_alloc_context();
	if (!avfc) {
		fprintf(stderr, "context alloc failed\n");
		avformat_network_deinit();
		return false;
	}

	avfc->interrupt_callback.callback = interrupt_cb;
	avfc->interrupt_callback.opaque = (void *) player;
	avfc->flags |= AVFMT_FLAG_GENPTS;

	if (player->isHttp)
		avfc->flags |= AVFMT_FLAG_NONBLOCK | AVIO_FLAG_NONBLOCK | AVFMT_NO_BYTE_SEEK;

	AVDictionary *options = NULL;
	av_dict_set(&options, "auth_type", "basic", 0);
	if (!headers.empty())
	{
		av_dict_set(&options, "headers", headers.c_str(), 0);
	}
#if ENABLE_LOGGING
	av_log_set_level(AV_LOG_DEBUG);
#endif
	int err = avformat_open_input(&avfc, filename, NULL, &options);
#if ENABLE_LOGGING
	av_log_set_level(AV_LOG_INFO);
#endif
	av_dict_free(&options);
	if (averror(err, avformat_open_input)) {
		avformat_close_input(&avfc);
		avformat_network_deinit();
		return false;
	}

	//ffmpeg5: error: assignment of member 'flags' in read-only object -> so commented out next line
	//avfc->iformat->flags |= AVFMT_SEEK_TO_PTS;

	if (player->isHttp && noprobe)
		avfc->max_analyze_duration = 1 * AV_TIME_BASE;
	else
		avfc->max_analyze_duration = 0;

	err = avformat_find_stream_info(avfc, NULL);

	if (averror(err, avformat_find_stream_info) && noprobe) {
		fprintf(stderr, "try again with default probe size\n");
		avformat_close_input(&avfc);
		noprobe = false;
		goto again;
	}

	bool res = UpdateTracks();

	if (!videoTrack && !audioTrack) {
		avformat_close_input(&avfc);
		avformat_network_deinit();
		return false;
	}

	if (videoTrack)
		player->output.SwitchVideo(videoTrack);
	if (audioTrack)
		player->output.SwitchAudio(audioTrack);

	return res;
}

bool Input::UpdateTracks()
{
	if (abortPlayback)
		return true;

	std::vector<Chapter> chapters;
	for (unsigned int i = 0; i < avfc->nb_chapters; i++) {
		AVChapter *ch = avfc->chapters[i];
		AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", NULL, 0);
		Chapter chapter;
		chapter.title = title ? title->value : "";
		chapter.start = av_rescale(ch->time_base.num * AV_TIME_BASE, ch->start, ch->time_base.den);
		chapter.end = av_rescale(ch->time_base.num * AV_TIME_BASE, ch->end, ch->time_base.den);
		chapters.push_back(chapter);
	}
	player->SetChapters(chapters);

	player->manager.initTrackUpdate();

	av_dump_format(avfc, 0, player->url.c_str(), 0);

	for (unsigned int n = 0; n < avfc->nb_streams; n++) {
		AVStream *stream = avfc->streams[n];
		AVCodecContext *avctx = avcodec_alloc_context3(NULL);

		if (!avctx) {
			fprintf(stderr, "context3 alloc for stream %d failed\n", n);
			continue;
		}
		if (avcodec_parameters_to_context(avctx, stream->codecpar) < 0) {
			fprintf(stderr, "parameters to context for stream %d failed\n", n);
			avcodec_free_context(&avctx);
			continue;
		}
		avctx->pkt_timebase=stream->time_base;
		
		Track track;
		track.stream = stream;
		track.avctx = avctx;
		track.pid = n + 1;
		track.type = 0;
		AVDictionaryEntry *lang = av_dict_get(stream->metadata, "language", NULL, 0);
		track.title = lang ? lang->value : "";

		if(stream->duration != AV_NOPTS_VALUE)
			track.duration = stream->duration * av_q2d(stream->time_base) * AV_TIME_BASE;
		else
			track.duration = avfc->duration;

		switch (stream->codecpar->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
				player->manager.addVideoTrack(track);
				if (!videoTrack)
					videoTrack = player->manager.getVideoTrack(track.pid);
				break;
			case AVMEDIA_TYPE_AUDIO:
				switch(stream->codecpar->codec_id) {
					case AV_CODEC_ID_MP2:
						track.type = 1;
						break;
					case AV_CODEC_ID_MP3:
						track.type = 2;
						break;
					case AV_CODEC_ID_AC3:
						track.type = 3;
						break;
					case AV_CODEC_ID_DTS:
						track.type = 4;
						break;
					case AV_CODEC_ID_AAC: {
						unsigned int extradata_size = stream->codecpar->extradata_size;
						unsigned int object_type = 2;
						if(extradata_size >= 2)
							object_type = stream->codecpar->extradata[0] >> 3;
						if (extradata_size <= 1 || object_type == 1 || object_type == 5) {
							fprintf(stderr, "use resampling for AAC\n");
							track.type = 6;
						}
						else
							track.type = 5;
						break;
					}
					case AV_CODEC_ID_FLAC:
						track.type = 8;
						break;
					case AV_CODEC_ID_WMAV1:
					case AV_CODEC_ID_WMAV2:
					case AV_CODEC_ID_WMAVOICE:
					case AV_CODEC_ID_WMAPRO:
					case AV_CODEC_ID_WMALOSSLESS:
						track.type = 9;
						break;
					default:
						track.type = 0;
				}
				player->manager.addAudioTrack(track);
				if (!audioTrack)
					audioTrack = player->manager.getAudioTrack(track.pid);
				break;
			case AVMEDIA_TYPE_SUBTITLE:
				switch(stream->codecpar->codec_id) {
					case AV_CODEC_ID_SRT:
					case AV_CODEC_ID_SUBRIP:
					case AV_CODEC_ID_TEXT:
					case AV_CODEC_ID_DVD_SUBTITLE:
					case AV_CODEC_ID_DVB_SUBTITLE:
					case AV_CODEC_ID_XSUB:
					case AV_CODEC_ID_MOV_TEXT:
					case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
					case AV_CODEC_ID_DVB_TELETEXT:
						 track.type = 1;
						 break;
					default:
						track.type = 0;
				}
				player->manager.addSubtitleTrack(track);
				break;
			default:
				fprintf(stderr, "not handled or unknown codec_type %d\n", stream->codecpar->codec_type);
				break;
		}
	}

	for (unsigned int n = 0; n < avfc->nb_programs; n++) {
		AVProgram *p = avfc->programs[n];
		if (p->nb_stream_indexes) {
			AVDictionaryEntry *name = av_dict_get(p->metadata, "name", NULL, 0);
			Program program;
			program.title = name ? name->value : "";
			program.id = p->id;
			for (unsigned m = 0; m < p->nb_stream_indexes; m++)
				program.streams.push_back(avfc->streams[p->stream_index[m]]);
			player->manager.addProgram(program);
		}
	}

	return true;
}

bool Input::Stop()
{
	int wait_time = 20;
	abortPlayback = true;

	while (hasPlayThreadStarted != 0 && (--wait_time) > 0)
		usleep(100000);

	if (wait_time == 0)
		fprintf(stderr,"timeout waiting for thread stop in input\n");

	av_log(NULL, AV_LOG_QUIET, "%s", "");

	if (avfc) {
		ScopedLock lock(mutex);
		player->manager.clearTracks();
		avformat_close_input(&avfc);
	}

	avformat_network_deinit();

	return true;
}

AVFormatContext *Input::GetAVFormatContext()
{
	mutex.lock();
	if (avfc)
		return avfc;
	mutex.unlock();
	return NULL;
}

void Input::ReleaseAVFormatContext()
{
	if (avfc)
		mutex.unlock();
}

bool Input::Seek(int64_t avts, bool absolute)
{
	if (absolute)
		seek_avts_abs = avts, seek_avts_rel = 0;
	else
		seek_avts_abs = INT64_MIN, seek_avts_rel = avts;
	return true;
}

bool Input::GetDuration(int64_t &duration)
{
	if (videoTrack && videoTrack->duration) {
		duration = videoTrack->duration;
		return true;
	}
	else if (audioTrack && audioTrack->duration) {
		duration = audioTrack->duration;
		return true;
	}
	else if (subtitleTrack && subtitleTrack->duration) {
		duration = subtitleTrack->duration;
		return true;
	}

	duration = 0;
	return false;
}

bool Input::SwitchAudio(Track *track)
{
	if (audioTrack && audioTrack->avctx && audioTrack->avctx->codec)
		avcodec_flush_buffers(audioTrack->avctx);

	audioTrack = track;
	player->output.SwitchAudio(track ? track : NULL);

	return true;
}

bool Input::SwitchSubtitle(Track *track)
{
	if (subtitleTrack && subtitleTrack->avctx && subtitleTrack->avctx->codec)
		avcodec_flush_buffers(subtitleTrack->avctx);

	subtitleTrack = track;
	return true;
}

bool Input::SwitchVideo(Track *track)
{
	if (videoTrack && videoTrack->avctx && videoTrack->avctx->codec)
		avcodec_flush_buffers(videoTrack->avctx);

	videoTrack = track;
	player->output.SwitchVideo(track ? track : NULL);
	return true;
}

bool Input::GetMetadata(std::map<std::string, std::string> &metadata)
{
	if (avfc) {
		AVDictionaryEntry *tag = NULL;

		if (avfc->metadata)
			while ((tag = av_dict_get(avfc->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
				metadata.insert(std::pair<std::string, std::string>(tag->key, tag->value));

		if (videoTrack)
			while ((tag = av_dict_get(videoTrack->stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
				metadata.insert(std::pair<std::string, std::string>(tag->key, tag->value));

		if (audioTrack)
			while ((tag = av_dict_get(audioTrack->stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
				metadata.insert(std::pair<std::string, std::string>(tag->key, tag->value));

		if (!metadata.empty())
			return true;
	}
	return false;
}

bool Input::GetReadCount(uint64_t &readcount)
{
	readcount = readCount;
	return true;
}
