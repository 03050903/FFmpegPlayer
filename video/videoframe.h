#pragma once

#include "fpicture.h"

struct VideoFrame
{
	double m_displayTime;
	int64_t m_duration;
	FPicture m_image;
};
