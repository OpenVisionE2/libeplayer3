/*
 * output class
 *
 * based on libeplayer3 LinuxDVB Output handling.
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/stm_ioctls.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>

#include "player.h"
#include "output.h"
#include "writer.h"
#include "misc.h"
#include "pes.h"

#include <avcodec.h>
#include <avformat.h>

static const char *FILENAME = "eplayer/output.cpp";

/* Send message to enigma2 */
extern void libeplayerMessage(int);

#define dioctl(fd,req,arg) ({		\
	int _r = ioctl(fd,req,arg); \
	if (_r)				\
		fprintf(stderr, "%s %d: ioctl '%s' failed: %d (%s)\n", FILENAME, __LINE__, #req, errno, strerror(errno)); \
	_r; \
})

#define VIDEODEV "/dev/dvb/adapter0/video0"
#define AUDIODEV "/dev/dvb/adapter0/audio0"

Output::Output()
{
	videofd = audiofd = -1;
	videoWriter = audioWriter = NULL;
	videoTrack = audioTrack = NULL;
}

Output::~Output()
{
	Close();
}

bool Output::Open()
{
	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd < 0)
		videofd = open(VIDEODEV, O_RDWR);

	if (videofd < 0)
		return false;

	ioctl(videofd, VIDEO_CLEAR_BUFFER, NULL);
	dioctl(videofd, VIDEO_SELECT_SOURCE, (void *) VIDEO_SOURCE_MEMORY);
	dioctl(videofd, VIDEO_SET_STREAMTYPE, (void *) STREAM_TYPE_PROGRAM);
	dioctl(videofd, VIDEO_SET_SPEED, DVB_SPEED_NORMAL_PLAY);

	if (audiofd < 0)
		audiofd = open(AUDIODEV, O_RDWR);

	if (audiofd < 0) {
		close(videofd);
		videofd = -1;
		return false;
	}

	ioctl(audiofd, AUDIO_CLEAR_BUFFER, NULL);
	dioctl(audiofd, AUDIO_SELECT_SOURCE, (void *) AUDIO_SOURCE_MEMORY);
	dioctl(audiofd, AUDIO_SET_STREAMTYPE, (void *) STREAM_TYPE_PROGRAM);

	return true;
}

bool Output::Close()
{
	Stop();

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd > -1) {
		close(videofd);
		videofd = -1;
	}
	if (audiofd > -1) {
		close(audiofd);
		audiofd = -1;
	}

	videoTrack = NULL;
	audioTrack = NULL;

	return true;
}

bool Output::Play()
{
	bool ret = true;

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	AVCodecParameters *avcc;

	if (videoTrack && videoTrack->stream && videofd > -1 && (avcc = videoTrack->stream->codecpar)) {
		videoWriter = Writer::GetWriter(avcc->codec_id, avcc->codec_type, videoTrack->type);
		videoWriter->Init(videofd, videoTrack, player);
		if (dioctl(videofd, VIDEO_SET_ENCODING, videoWriter->GetVideoEncoding(avcc->codec_id))
		||  dioctl(videofd, VIDEO_PLAY, NULL))
			ret = false;
	}

	if (audioTrack && audioTrack->stream && audiofd > -1 && (avcc = audioTrack->stream->codecpar)) {
		audioWriter = Writer::GetWriter(avcc->codec_id, avcc->codec_type, audioTrack->type);
		audioWriter->Init(audiofd, audioTrack, player);
		audio_encoding_t audioEncoding = AUDIO_ENCODING_LPCMA;
		if (audioTrack->type != 6)
			audioEncoding = audioWriter->GetAudioEncoding(avcc->codec_id);
		if (dioctl(audiofd, AUDIO_SET_ENCODING, audioEncoding)
		||  dioctl(audiofd, AUDIO_PLAY, NULL))
			ret = false;
	}
	return ret;
}

bool Output::Stop()
{
	bool ret = true;

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd > -1) {
		ioctl(videofd, VIDEO_CLEAR_BUFFER, NULL);
		/* set back to normal speed (end trickmodes) */
		dioctl(videofd, VIDEO_SET_SPEED, DVB_SPEED_NORMAL_PLAY);
		if (dioctl(videofd, VIDEO_STOP, NULL))
			ret = false;
	}

	if (audiofd > -1) {
		ioctl(audiofd, AUDIO_CLEAR_BUFFER, NULL);
		/* set back to normal speed (end trickmodes) */
		dioctl(audiofd, AUDIO_SET_SPEED, DVB_SPEED_NORMAL_PLAY);
		if (dioctl(audiofd, AUDIO_STOP, NULL))
			ret = false;
	}

	return ret;
}

bool Output::Pause()
{
	bool ret = true;

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd > -1) {
		if (dioctl(videofd, VIDEO_FREEZE, NULL))
			ret = false;
	}

	if (audiofd > -1) {
		if (dioctl(audiofd, AUDIO_PAUSE, NULL))
			ret = false;
	}

	return ret;
}

bool Output::Continue()
{
	bool ret = true;

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd > -1 && dioctl(videofd, VIDEO_CONTINUE, NULL))
		ret = false;

	if (audiofd > -1 && dioctl(audiofd, AUDIO_CONTINUE, NULL))
		ret = false;

	return ret;
}

bool Output::Mute(bool b)
{
	ScopedLock a_lock(audioMutex);
	//AUDIO_SET_MUTE has no effect with new player
	return audiofd > -1 && !dioctl(audiofd, b ? AUDIO_STOP : AUDIO_PLAY, NULL);
}


bool Output::Flush()
{
	bool ret = true;

	ScopedLock v_lock(videoMutex);
	ScopedLock a_lock(audioMutex);

	if (videofd > -1 && ioctl(videofd, VIDEO_FLUSH, NULL))
		ret = false;

	if (audiofd > -1 && audioWriter) {
		// flush audio decoder
		AVPacket packet;
		packet.data = NULL;
		packet.size = 0;
		audioWriter->Write(&packet, 0);

		if (ioctl(audiofd, AUDIO_FLUSH, NULL))
			ret = false;
	}

	return ret;
}

bool Output::FastForward(int speed)
{
	ScopedLock v_lock(videoMutex);
	return videofd > -1 && !dioctl(videofd, VIDEO_FAST_FORWARD, speed);
}

bool Output::SlowMotion(int speed)
{
	ScopedLock v_lock(videoMutex);
	return videofd > -1 && !dioctl(videofd, VIDEO_SLOWMOTION, speed);
}

bool Output::AVSync(bool b)
{
	ScopedLock a_lock(audioMutex);
	return audiofd > -1 && !dioctl(audiofd, AUDIO_SET_AV_SYNC, b);
}

bool Output::ClearAudio()
{
	ScopedLock a_lock(audioMutex);
	return audiofd > -1 && !ioctl(audiofd, AUDIO_CLEAR_BUFFER, NULL);
}

bool Output::ClearVideo()
{
	ScopedLock v_lock(videoMutex);
	return videofd > -1 && !ioctl(videofd, VIDEO_CLEAR_BUFFER, NULL);
}

bool Output::Clear()
{
	bool aret = ClearAudio();
	bool vret = ClearVideo();
	return aret && vret;
}

bool Output::GetPts(int64_t &pts)
{
	pts = 0;
	return ((videofd > -1 && !ioctl(videofd, VIDEO_GET_PTS, (void *) &pts)) ||
		(audiofd > -1 && !ioctl(audiofd, AUDIO_GET_PTS, (void *) &pts)));
}

bool Output::GetFrameCount(int64_t &framecount)
{
	dvb_play_info_t playInfo;

	if ((videofd > -1 && !dioctl(videofd, VIDEO_GET_PLAY_INFO, (void *) &playInfo)) ||
	    (audiofd > -1 && !dioctl(audiofd, AUDIO_GET_PLAY_INFO, (void *) &playInfo))) {
		framecount = playInfo.frame_count;
		return true;
	}
	return false;
}

bool Output::SwitchAudio(Track *track)
{
	ScopedLock a_lock(audioMutex);
	if (audioTrack && track->stream == audioTrack->stream)
		return true;
	if (audiofd > -1) {
		dioctl(audiofd, AUDIO_STOP, NULL);
		ioctl(audiofd, AUDIO_CLEAR_BUFFER, NULL);
	}
	audioTrack = track;
	if (track->stream) {
		AVCodecParameters *avcc = track->stream->codecpar;
		if (!avcc)
			return false;
		audioWriter = Writer::GetWriter(avcc->codec_id, avcc->codec_type, audioTrack->type);
		audioWriter->Init(audiofd, audioTrack, player);
		if (audiofd > -1) {
			audio_encoding_t audioEncoding = AUDIO_ENCODING_LPCMA;
			if (audioTrack->type != 6)
				audioEncoding = Writer::GetAudioEncoding(avcc->codec_id);
			dioctl(audiofd, AUDIO_SET_ENCODING, audioEncoding);
			dioctl(audiofd, AUDIO_PLAY, NULL);
		}
	}
	return true;
}

bool Output::SwitchVideo(Track *track)
{
	ScopedLock v_lock(videoMutex);
	if (videoTrack && track->stream == videoTrack->stream)
		return true;
	if (videofd > -1) {
		dioctl(videofd, VIDEO_STOP, NULL);
		ioctl(videofd, VIDEO_CLEAR_BUFFER, NULL);
	}
	videoTrack = track;
	if (track->stream) {
		AVCodecParameters *avcc = track->stream->codecpar;
		if (!avcc)
			return false;
		videoWriter = Writer::GetWriter(avcc->codec_id, avcc->codec_type, videoTrack->type);
		videoWriter->Init(videofd, videoTrack, player);
		if (videofd > -1) {
			dioctl(videofd, VIDEO_SET_ENCODING, Writer::GetVideoEncoding(avcc->codec_id));
			dioctl(videofd, VIDEO_PLAY, NULL);
		}
	}
	return true;
}

bool Output::GetEvent()
{
	struct pollfd pfd[1];
	pfd[0].fd = videofd;
	pfd[0].events = POLLPRI;
	int pollret = poll(pfd, 1, 0);
	if (pollret > 0 && pfd[0].revents & POLLPRI)
	{
		struct video_event evt;
		if (ioctl(videofd, VIDEO_GET_EVENT, &evt) == -1) {
			fprintf(stderr, "ioctl failed with errno %d\n", errno);
			fprintf(stderr, "VIDEO_GET_EVENT: %s\n", strerror(errno));
		}
		else {
			if (evt.type == VIDEO_EVENT_SIZE_CHANGED) {
				videoInfo.aspect = evt.u.size.aspect_ratio == 0 ? 2 : 3;  // convert dvb api to etsi
				videoInfo.width = evt.u.size.w;
				videoInfo.height = evt.u.size.h;
				fprintf(stderr, "aspect: %d\n", evt.u.size.aspect_ratio);
				libeplayerMessage(2);
			}
			else if (evt.type == VIDEO_EVENT_FRAME_RATE_CHANGED) {
				videoInfo.frame_rate = evt.u.frame_rate;
				libeplayerMessage(3);
			}
			else if (evt.type == 16 /*VIDEO_EVENT_PROGRESSIVE_CHANGED*/) {
				videoInfo.progressive = evt.u.frame_rate;
				libeplayerMessage(4);
			}
		}
	}
	return true;
}

void Output::sendLibeplayerMessage(int msg)
{
	libeplayerMessage(msg);
}

bool Output::GetSubtitles(std::map<uint32_t, subtitleData> &subtitles)
{
	ScopedLock s_lock(subtitleMutex);
	if (embedded_subtitle.empty())
		return false;
	subtitles = embedded_subtitle;
	embedded_subtitle.clear();
	return true;
}

void Output::GetVideoInfo(DVBApiVideoInfo &video_info)
{
	video_info = videoInfo;
}

const char *Output::ass_get_text(char *str)
{
	/*
	ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text
	91,0,Default,,0,0,0,,maar hij smaakt vast tof.
	*/

	int i = 0;
	char *p_str = str;
	while(i < 8 && *p_str != '\0')
	{
		if (*p_str == ',')
			i++;
		p_str++;
	}

	/* standardize hard break: '\N' -> '\n' http://docs.aegisub.org/3.2/ASS_Tags/ */
	char *p_newline = NULL;
	while((p_newline = strstr(p_str, "\\N")) != NULL)
		*(p_newline + 1) = 'n';
	return p_str;
}

bool Output::WriteSubtitle(AVStream *stream, AVPacket *packet, int64_t pts)
{
	ScopedLock s_lock(subtitleMutex);
	int64_t duration = 0;
	if(packet->duration != 0) {
		duration = av_rescale(packet->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
	}

	if (!duration || !packet->data)
		return false;

	subtitleData sub;
	sub.start_ms = pts  / 90;
	sub.duration_ms = duration;
	sub.end_ms = sub.start_ms + sub.duration_ms;
	switch(stream->codecpar->codec_id) {
		case AV_CODEC_ID_ASS:
		case AV_CODEC_ID_SSA:
			sub.text = ass_get_text((char *)packet->data);
			break;
		default:
			sub.text = (const char *)packet->data;
	}

	embedded_subtitle.insert(std::pair<uint32_t, subtitleData>(sub.end_ms, sub));
	libeplayerMessage(0);

	return true;
}

bool Output::Write(AVStream *stream, AVPacket *packet, int64_t pts)
{
	switch (stream->codecpar->codec_type) {
		case AVMEDIA_TYPE_VIDEO: {
			ScopedLock v_lock(videoMutex);
			return videofd > -1 && videoWriter && videoWriter->Write(packet, pts) && GetEvent();
		}
		case AVMEDIA_TYPE_AUDIO: {
			ScopedLock a_lock(audioMutex);
			return audiofd > -1 && audioWriter && audioWriter->Write(packet, pts);
		}
		case AVMEDIA_TYPE_SUBTITLE: {
			ScopedLock s_lock(subtitleMutex);
			return videofd > -1 && WriteSubtitle(stream, packet, pts);
		}
		default:
			return false;
	}
}
