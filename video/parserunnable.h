#pragma once

#include "ffmpegdecoder.h"

class ParseRunnable
{
	FFmpegDecoder* m_ffmpeg;

	bool reader_eof;

	bool readFrame(AVPacket* packet);
	void sendSeekPacket();
	void fixDuration();

    void dispatchPacket(AVPacket& packet);

public:
	explicit ParseRunnable(FFmpegDecoder* parent) :
		m_ffmpeg(parent),
		reader_eof(false)
	{}
	void operator() ();

	void startAudioThread(FFmpegDecoder* parent);
	void startVideoThread(FFmpegDecoder* parent);
};

