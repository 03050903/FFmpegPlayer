#pragma once

#include "ffmpegdecoder.h"

class DisplayRunnable
{
public:
	explicit DisplayRunnable(FFmpegDecoder* parent) : m_ffmpeg(parent)
	{}
	void operator () ();
private:
	FFmpegDecoder* m_ffmpeg;
};
