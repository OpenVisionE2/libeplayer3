/*
 * player class
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

#ifndef __PLAYER_H__
#define __PLAYER_H__

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
}

#include <stdint.h>

#include <string>
#include <vector>
#include <map>

#include "input.h"
#include "output.h"
#include "manager.h"
#include "libthread.h"

struct Chapter
{
	std::string title;
	int64_t start;
	int64_t end;
};

class Player {
	friend class Input;
	friend class Output;
	friend class Manager;
	friend class eServiceLibpl;
	friend class eServiceEPlayer3;
	friend class WriterPCM;
	friend int interrupt_cb(void *arg);

	private:
		Input input;
		Output output;
		Manager manager;
		Mutex chapterMutex;
		std::vector<Chapter> chapters;
		pthread_t playThread;

		bool abortRequested;
		bool isHttp;
		bool isPaused;
		bool isSlowMotion;
		bool hasThreadStarted;
		bool isForwarding;
		bool isBackWard;
		bool isPlaying;

		int Speed;

		uint64_t readCount;

		std::string url;

		void SetChapters(std::vector<Chapter> &Chapters);
		static void* playthread(void*);
	public:
		bool SwitchAudio(int pid);
		bool SwitchVideo(int pid);
		bool SwitchSubtitle(int pid);

		int GetAudioPid();
		int GetVideoPid();
		int GetSubtitlePid();

		bool GetPts(int64_t &pts);
		bool GetFrameCount(int64_t &framecount);
		bool GetDuration(int64_t &duration);

		bool GetMetadata(std::map<std::string, std::string> &metadata);
		bool SlowMotion(int repeats);
		int FastBackward(int speed);
		int FastForward(int speed);

		bool Open(const char *Url, bool _isHttp = false, std::string headers = "");
		bool Close();
		bool Play();
		int Pause();
		int Continue();
		bool Stop();
		bool Seek(int64_t pos, bool absolute);
		void RequestAbort();
		bool GetChapters(std::vector<int> &positions);

		AVFormatContext *GetAVFormatContext() { return input.GetAVFormatContext(); }
		void ReleaseAVFormatContext() { input.ReleaseAVFormatContext(); }

		bool GetPrograms(std::vector<std::string> &keys, std::vector<std::string> &values);
		bool SelectProgram(int key);
		bool SelectProgram(std::string &key);
		std::vector<Track> getAudioTracks();
		std::vector<Track> getSubtitleTracks();
		bool GetSubtitles(std::map<uint32_t, subtitleData> &subtitles);
		void GetVideoInfo(DVBApiVideoInfo &video_info);

		Player();
};
#endif
