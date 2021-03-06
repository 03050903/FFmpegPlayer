﻿#include "ffmpegdecoder.h"
#include <limits.h>
#include <stdint.h>

#include "parserunnable.h"
#include "displayrunnable.h"
#include "makeguard.h"

#include <boost/chrono.hpp>
#include <utility>

#include <boost/log/trivial.hpp>

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc  avcodec_alloc_frame
#endif

namespace
{
// https://gist.github.com/xlphs/9895065
class MyIOContext
{
   public:
    AVIOContext *ioCtx;
    uint8_t *buffer;  // internal buffer for ffmpeg
    int bufferSize;
    FILE *fh;

   public:
    MyIOContext(const PathType &datafile);
    ~MyIOContext();

    void initAVFormatContext(AVFormatContext *);

    bool valid() const { return fh != nullptr; }
};

// static
int IOReadFunc(void *data, uint8_t *buf, int buf_size)
{
    MyIOContext *hctx = (MyIOContext *)data;
    size_t len = fread(buf, 1, buf_size, hctx->fh);
    if (len == 0)
    {
        // Let FFmpeg know that we have reached EOF, or do something else
        return AVERROR_EOF;
    }
    return (int)len;
}

// whence: SEEK_SET, SEEK_CUR, SEEK_END (like fseek) and AVSEEK_SIZE
// static
int64_t IOSeekFunc(void *data, int64_t pos, int whence)
{
    MyIOContext *hctx = (MyIOContext *)data;

    if (whence == AVSEEK_SIZE)
    {
        // return the file size if you wish to
        auto current = _ftelli64(hctx->fh);
        int rs = _fseeki64(hctx->fh, 0, SEEK_END);
        if (rs != 0)
        {
            return -1LL;
        }
        int64_t result = _ftelli64(hctx->fh);
        _fseeki64(hctx->fh, current, SEEK_SET);  // reset to the saved position
        return result;
    }

    int rs = _fseeki64(hctx->fh, pos, whence);
    if (rs != 0)
    {
        return -1LL;
    }
    return _ftelli64(hctx->fh);  // int64_t is usually long long
}

MyIOContext::MyIOContext(const PathType &s)
{
    // allocate buffer
    bufferSize = 1024 * 64;                     // FIXME: not sure what size to use
    buffer = (uint8_t *)av_malloc(bufferSize);  // see destructor for details

    // open file
    auto err =
#ifdef _WIN32
        _wfopen_s(&fh, s.c_str(), L"rb");
#else
        fopen_s(&fh, s.c_str(), "rb");
#endif
    if (err)
    {
        // fprintf(stderr, "MyIOContext: failed to open file %s\n", s.c_str());
        BOOST_LOG_TRIVIAL(error) << "MyIOContext: failed to open file";
    }

    // allocate the AVIOContext
    ioCtx =
        avio_alloc_context(buffer, bufferSize,  // internal buffer and its size
                           0,                   // write flag (1=true,0=false)
                           (void *)this,  // user data, will be passed to our callback functions
                           IOReadFunc,
                           0,  // no writing
                           IOSeekFunc);
}

MyIOContext::~MyIOContext()
{
    if (fh)
        fclose(fh);

    // NOTE: ffmpeg messes up the buffer
    // so free the buffer first then free the context
    av_free(ioCtx->buffer);
    ioCtx->buffer = nullptr;
    av_free(ioCtx);
}

void MyIOContext::initAVFormatContext(AVFormatContext *pCtx)
{
    pCtx->pb = ioCtx;
    pCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // you can specify a format directly
    // pCtx->iformat = av_find_input_format("h264");

    // or read some of the file and let ffmpeg do the guessing
    size_t len = fread(buffer, 1, bufferSize, fh);
    if (len == 0)
        return;
    _fseeki64(fh, 0, SEEK_SET);  // reset to beginning of file

    AVProbeData probeData = {0};
    probeData.buf = buffer;
    probeData.buf_size = bufferSize - 1;
    probeData.filename = "";
    pCtx->iformat = av_probe_input_format(&probeData, 1);
}

//////////////////////////////////////////////////////////////////////////////

inline void call_avcodec_close(AVCodecContext** avctx)
{
    if (*avctx != nullptr)
    {
        avcodec_close(*avctx);
        *avctx = nullptr;
    }
}

}  // namespace

using boost::log::keywords::channel;

boost::log::sources::channel_logger_mt<> 
    channel_logger_ffmpeg_audio(channel = "ffmpeg_audio"),
    channel_logger_ffmpeg_closing(channel = "ffmpeg_closing"),
    channel_logger_ffmpeg_opening(channel = "ffmpeg_opening"),
    channel_logger_ffmpeg_pause(channel = "ffmpeg_pause"),
    channel_logger_ffmpeg_readpacket(channel = "ffmpeg_readpacket"),
    channel_logger_ffmpeg_seek(channel = "ffmpeg_seek"),
    channel_logger_ffmpeg_sync(channel = "ffmpeg_sync"),
    channel_logger_ffmpeg_threads(channel = "ffmpeg_threads"),
    channel_logger_ffmpeg_volume(channel = "ffmpeg_volume");

double GetHiResTime()
{
    return boost::chrono::duration_cast<boost::chrono::microseconds>(
               boost::chrono::high_resolution_clock::now().time_since_epoch())
               .count() /
           1000000.;
}

std::unique_ptr<IFrameDecoder> GetFrameDecoder(std::unique_ptr<IAudioPlayer> audioPlayer)
{
    return std::unique_ptr<IFrameDecoder>(new FFmpegDecoder(std::move(audioPlayer)));
}

//////////////////////////////////////////////////////////////////////////////

FFmpegDecoder::FFmpegDecoder(std::unique_ptr<IAudioPlayer> audioPlayer)
    : m_frameListener(nullptr),
      m_decoderListener(nullptr),
      m_audioSettings({48000, 2, av_get_default_channel_layout(2), AV_SAMPLE_FMT_S16}),
      m_pixelFormat(AV_PIX_FMT_YUV420P),
      m_audioPlayer(std::move(audioPlayer))
{
    m_audioPlayer->SetCallback(this);

    resetVariables();

    // init codecs
    avcodec_register_all();
    av_register_all();

    //avdevice_register_all();
    avformat_network_init();
}

FFmpegDecoder::~FFmpegDecoder() { close(); }

void FFmpegDecoder::resetVariables()
{
    m_videoCodec = nullptr;
    m_formatContext = nullptr;
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;
    m_videoFrame = nullptr;
    m_audioFrame = nullptr;
    m_audioSwrContext = nullptr;
    m_videoStream = nullptr;
    m_audioStream = nullptr;

    m_frameTotalCount = 0;
    m_duration = 0;

    m_imageCovertContext = nullptr;

    m_audioPTS = 0;

    m_frameDisplayingRequested = false;

    m_isPaused = false;

    m_seekDuration = -1;

    m_isAudioSeekingWhilePaused = false;
    m_isVideoSeekingWhilePaused = false;

    m_isPlaying = false;

    CHANNEL_LOG(ffmpeg_closing) << "Variables reset";
}

void FFmpegDecoder::close()
{
    CHANNEL_LOG(ffmpeg_closing) << "Start file closing";

    CHANNEL_LOG(ffmpeg_closing) << "Aborting threads";
    if (m_mainParseThread)  // controls other threads, hence stop first
    {
        m_mainParseThread->interrupt();
        m_mainParseThread->join();
    }
    if (m_mainVideoThread)
    {
        m_mainVideoThread->interrupt();
        m_mainVideoThread->join();
    }
    if (m_mainAudioThread)
    {
        m_mainAudioThread->interrupt();
        m_mainAudioThread->join();
    }
    if (m_mainDisplayThread)
    {
        m_mainDisplayThread->interrupt();
        m_mainDisplayThread->join();
    }

    m_audioPlayer->Close();

    closeProcessing();

    if (m_decoderListener)
        m_decoderListener->playingFinished();
}

void FFmpegDecoder::closeProcessing()
{
    m_audioPacketsQueue.clear();
    m_videoPacketsQueue.clear();

    CHANNEL_LOG(ffmpeg_closing) << "Closing old vars";

    m_mainVideoThread.reset();
    m_mainAudioThread.reset();
    m_mainParseThread.reset();
    m_mainDisplayThread.reset();

    m_audioPlayer->Reset();

    // Free videoFrames
    m_videoFramesQueue.clear();

    sws_freeContext(m_imageCovertContext);

    av_free(m_audioFrame);

    if (m_audioSwrContext)
    {
        swr_free(&m_audioSwrContext);
    }

    // Free the YUV frame
    av_free(m_videoFrame);

    // Close the codec
    call_avcodec_close(&m_videoCodecContext);

    // Close the audio codec
    call_avcodec_close(&m_audioCodecContext);

    bool isFileReallyClosed = false;

    // Close video file
    if (m_formatContext)
    {
        MyIOContext *hctx =
            m_formatContext->pb ? (MyIOContext *)m_formatContext->pb->opaque : nullptr;
        avformat_close_input(&m_formatContext);
        delete hctx;
        isFileReallyClosed = true;
    }

    CHANNEL_LOG(ffmpeg_closing) << "Old file closed";

    resetVariables();

    if (isFileReallyClosed)
    {
        CHANNEL_LOG(ffmpeg_closing) << "File was opened. Emit file closing signal";
        if (m_decoderListener)
            m_decoderListener->fileReleased();
    }

    if (m_decoderListener)
        m_decoderListener->decoderClosed();
}

bool FFmpegDecoder::openFile(const PathType& filename)
{
    return openDecoder(filename, std::string(), true);
}

bool FFmpegDecoder::openUrl(const std::string& url)
{
    return openDecoder(PathType(), url, false);
}

bool FFmpegDecoder::openDecoder(const PathType &file, const std::string& url, bool isFile)
{
    close();

    std::unique_ptr<MyIOContext> ioCtx;
    if (isFile)
    {
        ioCtx.reset(new MyIOContext(file));
        if (!ioCtx->valid())
        {
            BOOST_LOG_TRIVIAL(error) << "Couldn't open video/audio file";
            return false;
        }
    }

    AVDictionary *streamOpts = nullptr;
    auto avOptionsGuard = MakeGuard(&streamOpts, av_dict_free);

    m_formatContext = avformat_alloc_context();
    if (isFile)
    {
        ioCtx->initAVFormatContext(m_formatContext);
    }
    else
    {
        av_dict_set(&streamOpts, "stimeout", "5000000", 0); // 5 seconds timeout.
    }

    auto formatContextGuard = MakeGuard(&m_formatContext, avformat_close_input);

    // Open video file
    const int error = avformat_open_input(&m_formatContext, url.c_str(), nullptr, &streamOpts);
    if (error != 0)
    {
        BOOST_LOG_TRIVIAL(error) << "Couldn't open video/audio file error: " << error;
        return false;
    }
    CHANNEL_LOG(ffmpeg_opening) << "Opening video/audio file...";

    // Retrieve stream information
    if (avformat_find_stream_info(m_formatContext, nullptr) < 0)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Couldn't find stream information";
        return false;
    }

    // Find the first video stream
    m_videoStreamNumber = -1;
    m_audioStreamNumber = -1;
    for (unsigned i = 0; i < m_formatContext->nb_streams; ++i)
    {
        if (m_formatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            m_videoStream = m_formatContext->streams[i];
            m_videoStreamNumber = i;
            if (m_audioStreamNumber != -1)
            {
                break;
            }
        }
        else if (m_formatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            m_audioStream = m_formatContext->streams[i];
            m_audioStreamNumber = i;
            if (m_videoStreamNumber != -1)
            {
                break;
            }
        }
    }
    if (m_videoStreamNumber == -1)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Can't find video stream";
    }
    else if (m_videoStreamNumber >= 0)
    {
        const int64_t format_duration =
            int64_t((m_formatContext->duration / av_q2d(m_videoStream->time_base)) / 1000000LL);
        m_frameTotalCount = (m_videoStream->nb_frames > 0) ? m_videoStream->nb_frames : -1;
        m_duration = (m_videoStream->duration > 0) ? m_videoStream->duration : format_duration;
    }

    if (m_audioStreamNumber == -1)
    {
        CHANNEL_LOG(ffmpeg_opening) << "No audio stream";
    }
    else if (m_audioStreamNumber >= 0 && m_videoStreamNumber == -1)
    {
        // Changing video -> audio duration
        const int64_t format_duration =
            int64_t((m_formatContext->duration / av_q2d(m_audioStream->time_base)) / 1000000LL);
        m_duration = (m_audioStream->duration > 0) ? m_audioStream->duration : format_duration;
    }

    // Get a pointer to the codec context for the video stream
    if (m_videoStreamNumber >= 0)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Video steam number: " << m_videoStreamNumber;
        m_videoCodecContext = m_formatContext->streams[m_videoStreamNumber]->codec;
    }
    if (m_audioStreamNumber >= 0)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Audio steam number: " << m_audioStreamNumber;
        m_audioCodecContext = m_formatContext->streams[m_audioStreamNumber]->codec;
    }

    auto videoCodecContextGuard = MakeGuard(&m_videoCodecContext, call_avcodec_close);
    auto audioCodecContextGuard = MakeGuard(&m_audioCodecContext, call_avcodec_close);

    // Multithread decoding
    m_videoCodecContext->thread_count = 1;

    m_videoCodecContext->flags2 |= CODEC_FLAG2_FAST;

    // Find the decoder for the video stream
    if (m_videoStreamNumber >= 0)
    {
        m_videoCodec = avcodec_find_decoder(m_videoCodecContext->codec_id);
        if (m_videoCodec == nullptr)
        {
            assert(false && "No such codec found");
            return false;  // Codec not found
        }
    }

    // Find audio codec
    if (m_audioStreamNumber >= 0)
    {
        m_audioCodec = avcodec_find_decoder(m_audioCodecContext->codec_id);
        if (m_audioCodec == nullptr)
        {
            assert(false && "No such codec found");
            return false;  // Codec not found
        }
    }

    // Open codec
    if (m_videoStreamNumber >= 0)
    {
        if (avcodec_open2(m_videoCodecContext, m_videoCodec, nullptr) < 0)
        {
            assert(false && "Error on codec opening");
            return false;  // Could not open codec
        }

        // Some broken files can pass codec check but don't have width x height
        if (m_videoCodecContext->width <= 0 || m_videoCodecContext->height <= 0)
        {
            assert(false && "This file lacks resolution");
            return false;  // Could not open codec
        }
    }

    // Open audio codec
    if (m_audioStreamNumber >= 0)
    {
        if (avcodec_open2(m_audioCodecContext, m_audioCodec, nullptr) < 0)
        {
            assert(false && "Error on codec opening");
            return false;  // Could not open codec
        }
    }

    m_audioCurrentPref = m_audioSettings;

    if (m_audioStreamNumber >= 0 
            && !m_audioPlayer->Open(av_get_bytes_per_sample(m_audioSettings.format),
                m_audioSettings.frequency, m_audioSettings.channels))
    {
        return false;
    }

    m_videoFrame = av_frame_alloc();
    m_audioFrame = av_frame_alloc();

    audioCodecContextGuard.release();
    videoCodecContextGuard.release();
    formatContextGuard.release();
    ioCtx.release();

    if (m_decoderListener)
        m_decoderListener->fileLoaded();

    return true;
}

void FFmpegDecoder::play(bool isPaused)
{
    CHANNEL_LOG(ffmpeg_opening) << "Starting playing";

    m_isPaused = isPaused;

    if (isPaused)
    {
        m_pauseTimer = GetHiResTime();
    }

    if (!m_mainParseThread)
    {
        m_isPlaying = true;
        m_mainParseThread.reset(new boost::thread(ParseRunnable(this)));
        m_mainDisplayThread.reset(new boost::thread(DisplayRunnable(this)));
        CHANNEL_LOG(ffmpeg_opening) << "Playing";
    }
}

void FFmpegDecoder::AppendFrameClock(double frame_clock)
{
    for (double v = m_audioPTS;
         !m_audioPTS.compare_exchange_weak(v, v + frame_clock);)
    {
    }
}

void FFmpegDecoder::setVolume(double volume)
{
    if (volume < 0 || volume > 1.)
    {
        return;
    }

    CHANNEL_LOG(ffmpeg_volume) << "Volume: " << volume;

    m_audioPlayer->SetVolume(volume);

    if (m_decoderListener)
        m_decoderListener->volumeChanged(volume);
}

double FFmpegDecoder::volume() const { return m_audioPlayer->GetVolume(); }

FPicture *FFmpegDecoder::frameToImage(FPicture &videoFrameData)
{
    const int width = m_videoFrame->width;
    const int height = m_videoFrame->height;

    videoFrameData.reallocForSure(m_pixelFormat, width, height);

    // Prepare image conversion
    m_imageCovertContext =
        sws_getCachedContext(m_imageCovertContext, m_videoFrame->width, m_videoFrame->height,
                             (AVPixelFormat)m_videoFrame->format, width, height, m_pixelFormat,
                             0, nullptr, nullptr, nullptr);

    assert(m_imageCovertContext != nullptr);

    if (m_imageCovertContext == nullptr)
    {
        return nullptr;
    }

    // Doing conversion
    if (sws_scale(m_imageCovertContext, m_videoFrame->data, m_videoFrame->linesize, 0,
                  m_videoFrame->height, videoFrameData.data, videoFrameData.linesize) <= 0)
    {
        assert(false && "sws_scale failed");
        BOOST_LOG_TRIVIAL(error) << "sws_scale failed";
        return nullptr;
    }

    return &videoFrameData;
}

void FFmpegDecoder::SetFrameFormat(FrameFormat format)
{ 
    static_assert(PIX_FMT_YUV420P == AV_PIX_FMT_YUV420P, "FrameFormat and AVPixelFormat values must coincide.");
    static_assert(PIX_FMT_YUYV422 == AV_PIX_FMT_YUYV422, "FrameFormat and AVPixelFormat values must coincide.");
    static_assert(PIX_FMT_RGB24 == AV_PIX_FMT_RGB24,     "FrameFormat and AVPixelFormat values must coincide.");

    setPixelFormat((AVPixelFormat)format);
}

void FFmpegDecoder::finishedDisplayingFrame()
{
    {
        boost::lock_guard<boost::mutex> locker(m_videoFramesMutex);
        --m_videoFramesQueue.m_busy;
        assert(m_videoFramesQueue.m_busy >= 0);
        // avoiding assert in VideoParseRunnable
        m_videoFramesQueue.m_read_counter =
            (m_videoFramesQueue.m_read_counter + 1) %
            (sizeof(m_videoFramesQueue.m_frames) / sizeof(m_videoFramesQueue.m_frames[0]));
        m_frameDisplayingRequested = false;
    }
    m_videoFramesCV.notify_all();
}

bool FFmpegDecoder::seekDuration(int64_t duration)
{
    if (m_mainParseThread && m_seekDuration.exchange(duration) == -1)
    {
        boost::lock_guard<boost::mutex> locker(m_packetsQueueMutex);
        m_packetsQueueCV.notify_all();
    }

    return true;
}

void FFmpegDecoder::seekWhilePaused()
{
    if (m_isPaused)
    {
        for (double v = m_videoStartClock;
             !m_videoStartClock.compare_exchange_weak(v, v + GetHiResTime() - m_pauseTimer);)
        {
        }
        m_pauseTimer = GetHiResTime();

        m_isAudioSeekingWhilePaused = true;
        m_isVideoSeekingWhilePaused = true;
    }
    else
    {
        m_isAudioSeekingWhilePaused = false;
        m_isVideoSeekingWhilePaused = false;
    }
}

bool FFmpegDecoder::seekByPercent(double percent, int64_t totalDuration)
{
    if (totalDuration < 0)
    {
        totalDuration = m_duration;
    }

    return seekDuration(int64_t(totalDuration * percent));
}

bool FFmpegDecoder::getFrameRenderingData(FrameRenderingData *data)
{
    if (!m_frameDisplayingRequested || m_mainAudioThread == nullptr || m_mainVideoThread == nullptr ||
        m_mainParseThread == nullptr)
    {
        return false;
    }

    VideoFrame &current_frame = m_videoFramesQueue.m_frames[m_videoFramesQueue.m_read_counter];
    if (!current_frame.m_image.data)
    {
        return false;
    }
    data->image = current_frame.m_image.data;
    data->width = current_frame.m_image.width;
    data->height = current_frame.m_image.height;

    return true;
}

bool FFmpegDecoder::pauseResume()
{
    if (m_mainAudioThread == nullptr || m_mainVideoThread == nullptr || m_mainParseThread == nullptr)
    {
        return false;
    }

    if (m_isPaused)
    {
        CHANNEL_LOG(ffmpeg_pause) << "Unpause";
        CHANNEL_LOG(ffmpeg_pause) << "Move >> " << GetHiResTime() - m_pauseTimer;
        for (double v = m_videoStartClock;
             !m_videoStartClock.compare_exchange_weak(v, v + GetHiResTime() - m_pauseTimer);)
        {
        }
        {
            boost::unique_lock<boost::mutex> locker(m_isPausedMutex);
            m_isPaused = false;
        }
        m_isPausedCV.notify_all();
    }
    else
    {
        CHANNEL_LOG(ffmpeg_pause) << "Pause";
        m_isPaused = true;
        {
            boost::unique_lock<boost::mutex> locker(m_videoFramesMutex);
            m_videoFramesCV.notify_all();
        }
        {
            boost::unique_lock<boost::mutex> locker(m_packetsQueueMutex);
            m_packetsQueueCV.notify_all();
        }
        m_pauseTimer = GetHiResTime();
    }

    return true;
}
