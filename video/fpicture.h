#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}


struct FPicture : public AVPicture
{
	int width;
	int height;
	AVPixelFormat pix_fmt;

	FPicture() 
		: AVPicture()
		, width(0)
		, height(0)
		, pix_fmt(AV_PIX_FMT_NONE)
	{
	}

	FPicture(const FPicture&) = delete;
	FPicture& operator=(const FPicture&) = delete;

	~FPicture()
	{
		free();
	}

	void free()
	{
		avpicture_free(this);
        width = 0;
        height = 0;
        pix_fmt = AV_PIX_FMT_NONE;
	}

	void alloc(AVPixelFormat pix_fmt, int width, int height)
	{
		avpicture_alloc(this, pix_fmt, width, height);
		this->width = width;
		this->height = height;
		this->pix_fmt = pix_fmt;
	}

	void realloc(AVPixelFormat pix_fmt, int width, int height)
	{
		free();
		alloc(pix_fmt, width, height);
	}

	void reallocForSure(AVPixelFormat pix_fmt, int width, int height)
	{
		if (pix_fmt != this->pix_fmt || width != this->width || height != this->height)
		{
			realloc(pix_fmt, width, height);
		}
	}
};
