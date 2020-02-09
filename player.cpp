/*
 * linuxdvb output/writer handling
 *
 * Copyright (C) 2010  duckbox
 * Copyright (C) 2014  martii   (based on code from libeplayer3)
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sstream>

#include "player.h"
#include "misc.h"

static const char *FILENAME = "eplayer/player.cpp";

#define cMaxSpeed_ff   128	/* fixme: revise */
#define cMaxSpeed_fr   -320	/* fixme: revise */

Player::Player()
{
	input.player = this;
	output.player = this;
	manager.player = this;
	hasThreadStarted = false;

	isPaused = false;
	isPlaying = false;
	isForwarding = false;
	isBackWard = false;
	isSlowMotion = false;
	Speed = 0;
}

void *Player::playthread(void *arg)
{
	char threadname[17];
	strncpy(threadname, __func__, sizeof(threadname));
	threadname[16] = 0;
	prctl(PR_SET_NAME, (unsigned long) threadname);

	Player *player = (Player *) arg;
	player->hasThreadStarted = true;
	player->input.Play();
	player->hasThreadStarted = false;
	player->Stop();
	pthread_exit(NULL);
}

bool Player::Open(const char *Url, bool _isHttp, std::string headers)
{
	fprintf(stderr, "URL=%s\n", Url);

	isHttp = _isHttp;
	abortRequested = false;

	manager.clearTracks();

	if (!strncmp("mms://", Url, 6)) {
		url = "mmst";
		url += Url + 3;
	}
	else if (!strncmp("myts://", Url, 7)) {
		url = "file";
		url += Url + 4;
	}
	else
		url = Url;

	return input.Init(url.c_str(), headers);
}

bool Player::Close()
{
	isPaused = false;
	isPlaying = false;
	isForwarding = false;
	isBackWard = false;
	isSlowMotion = false;
	Speed = 0;
	url.clear();
	output.Close();

	return true;
}

bool Player::Play()
{
	if (!output.Open()) {
		fprintf(stderr, "error when open output\n");
		return false;
	}

	bool ret = true;

	if (!isPlaying) {
		output.AVSync(true);

		ret = output.Play();

		if (ret) {
			isPlaying = true;
			isPaused = false;
			isForwarding = false;
			if (isBackWard) {
				isBackWard = false;
				output.Mute(false);
			}
			isSlowMotion = false;
			Speed = 1;

			if (!hasThreadStarted) {
				int err = pthread_create(&playThread, NULL, playthread, this);

				if (err) {
					fprintf(stderr, "%s %s %d: pthread_create: %d (%s)\n", FILENAME, __func__, __LINE__, err, strerror(err));
					ret = false;
					isPlaying = false;
				} else {
					pthread_detach(playThread);
				}
			}
		}
	}
	else
	{
		fprintf(stderr,"[player.cpp] playback already running\n");
		ret = false;
	}
	return ret;
}

int Player::Pause()
{
	if (isPlaying && !isPaused) {

		if (isSlowMotion)
			output.Clear();

		output.Pause();

		isPaused = true;
		//isPlaying  = 1;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 1;
	} else {
		fprintf(stderr,"playback not playing or already in pause mode\n");
		return -1;
	}
	return 0;
}

int Player::Continue()
{
	if (isPlaying && (isPaused || isForwarding || isBackWard || isSlowMotion)) {

		if (isSlowMotion)
			output.Clear();

		output.Continue();

		isPaused = false;
		//isPlaying  = 1;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 1;
	} else {
		fprintf(stderr,"continue not possible\n");
		return -1;
	}

	return 0;
}

bool Player::Stop()
{
	bool ret = true;
	int wait_time = 20;

	if (isPlaying) {
		isPaused = false;
		isPlaying = false;
		isForwarding = false;
		if (isBackWard) {
			isBackWard = false;
			output.Mute(false);
		}
		isSlowMotion = false;
		Speed = 0;

		output.Stop();
		input.Stop();
	}
	else
	{
		fprintf(stderr,"[player.cpp] stop not possible\n");
		ret = false;
	}

	while (hasThreadStarted && (--wait_time) > 0)
		usleep(100000);
	if (wait_time == 0)
	{
		fprintf(stderr,"[player.cpp] timeout waiting for thread stop\n");
		ret = false;
	}

	return ret;
}

int Player::FastForward(int speed)
{
	/* Audio only forwarding not supported */
	if (input.videoTrack && !isHttp && !isBackWard && (!isPaused ||  isPlaying))
	{
		if ((speed <= 0) || (speed > cMaxSpeed_ff))
		{
			fprintf(stderr, "[player.cpp] speed %d out of range (1 - %d) \n", speed, cMaxSpeed_ff);
			return -1;
		}

		isForwarding = 1;
		Speed = speed;
		output.FastForward(speed);
	}
	else
	{
		fprintf(stderr,"[player.cpp] fast forward not possible\n");
		return -1;
	}

	return 0;
}

int Player::FastBackward(int speed)
{
	int ret = 0;

	/* Audio only reverse play not supported */
	if (input.videoTrack && !isForwarding && (!isPaused || isPlaying))
	{
		if ((speed > 0) || (speed < cMaxSpeed_fr))
		{
			fprintf(stderr, "[player.cpp] speed %d out of range (0 - %d) \n", speed, cMaxSpeed_fr);
			return -1;
		}

		if (speed == 0) {
			isBackWard = false;
			Speed = 0;	/* reverse end */
		} else {
			Speed = speed;
			isBackWard = true;
		}

		output.Clear();
#if 0
		if (output->Command(player, OUTPUT_REVERSE, NULL) < 0) {
			fprintf(stderr,"OUTPUT_REVERSE failed\n");
			isBackWard = false;
			Speed = 1;
			ret = -1;
		}
#endif
	}
	else
	{
		fprintf(stderr,"[player.cpp] fast backward not possible\n");
		ret = -1;
	}

	if (isBackWard)
		output.Mute(true);

	return ret;
}

bool Player::SlowMotion(int repeats)
{
	if (input.videoTrack && !isHttp && isPlaying) {
		if (isPaused)
			Continue();

		switch (repeats) {
			case 2:
			case 4:
			case 8:
				isSlowMotion = true;
				break;
			default:
				repeats = 0;
		}

		output.SlowMotion(repeats);
		return true;
	}
	fprintf(stderr, "[player.cpp] slowmotion not possible\n");
	return false;
}

bool Player::Seek(int64_t pos, bool absolute)
{
	output.Clear();
	return input.Seek(pos, absolute);
}

bool Player::GetPts(int64_t &pts)
{
	pts = INVALID_PTS_VALUE;
	return isPlaying && output.GetPts(pts);
}

bool Player::GetFrameCount(int64_t &frameCount)
{
	return isPlaying && output.GetFrameCount(frameCount);
}

bool Player::GetDuration(int64_t &duration)
{
	duration = -1;
	return isPlaying && input.GetDuration(duration);
}

bool Player::SwitchVideo(int pid)
{
	Track *track = manager.getVideoTrack(pid);
	return input.SwitchVideo(track);
}

bool Player::SwitchAudio(int pid)
{
	Track *track = manager.getAudioTrack(pid);
	return input.SwitchAudio(track);
}

bool Player::SwitchSubtitle(int pid)
{
	Track *track = manager.getSubtitleTrack(pid);
	return input.SwitchSubtitle(track);
}

bool Player::GetMetadata(std::map<std::string, std::string> &metadata)
{
	return input.GetMetadata(metadata);
}

bool Player::GetChapters(std::vector<int> &positions)
{
	positions.clear();
	ScopedLock m_lock(chapterMutex);
	for (std::vector<Chapter>::iterator it = chapters.begin(); it != chapters.end(); ++it) {
		positions.push_back(it->start / 10);
	}
	return true;
}

void Player::SetChapters(std::vector<Chapter> &Chapters)
{
	ScopedLock m_lock(chapterMutex);
	chapters = Chapters;

	if (!chapters.empty())
		output.sendLibeplayerMessage(5);
}

void Player::RequestAbort()
{
	abortRequested = true;
}

int Player::GetVideoPid()
{
	Track *track = input.videoTrack;
	return track ? track->pid : 0;
}

int Player::GetAudioPid()
{
	Track *track = input.audioTrack;
	return track ? track->pid : 0;
}

int Player::GetSubtitlePid()
{
	Track *track = input.subtitleTrack;
	return track ? track->pid : 0;
}

bool Player::GetPrograms(std::vector<std::string> &keys, std::vector<std::string> &values)
{
	keys.clear();
	values.clear();

	std::vector<Program> p = manager.getPrograms();

	if (p.empty())
		return false;

	for (std::vector<Program>::iterator it = p.begin(); it != p.end(); ++it) {
		std::stringstream s;
		s << it->id;
		keys.push_back(s.str());
		values.push_back(it->title);
	}

	return true;
}

bool Player::SelectProgram(int key)
{
	return manager.selectProgram(key);
}

bool Player::SelectProgram(std::string &key)
{
	return manager.selectProgram(atoi(key.c_str()));
}

std::vector<Track> Player::getAudioTracks()
{
	return manager.getAudioTracks();
}

std::vector<Track> Player::getSubtitleTracks()
{
	return manager.getSubtitleTracks();
}

bool Player::GetSubtitles(std::map<uint32_t, subtitleData> &subtitles)
{
	return output.GetSubtitles(subtitles);
}

void Player::GetVideoInfo(DVBApiVideoInfo &video_info)
{
	output.GetVideoInfo(video_info);
}
