#include "parserunnable.h"
#include "videoparserunnable.h"
#include "audioparserunnable.h"
#include "makeguard.h"

bool ParseRunnable::readFrame(AVPacket* packet)
{
    int ret = av_read_frame(m_ffmpeg->m_formatContext, packet);
    if (ret >= 0)
    {
        reader_eof = false;
    }
    else
    {
        reader_eof = ret == AVERROR_EOF;
    }
    return ret >= 0;
}

void ParseRunnable::operator()()
{
    CHANNEL_LOG(ffmpeg_threads) << "Parse thread started";
    AVPacket packet;
    bool eof = false;

    // detect real framesize
    fixDuration();

    startAudioThread(m_ffmpeg);
    startVideoThread(m_ffmpeg);

    for (;;)
    {
        if (boost::this_thread::interruption_requested())
        {
            return;
        }

        // seeking
        sendSeekPacket();

        if (readFrame(&packet))
        {
            dispatchPacket(packet);

            eof = false;
        }
        else
        {
            if (eof)
            {
                const bool videoPacketsQueueIsEmpty =
                    m_ffmpeg->m_videoPacketsQueue.size(m_ffmpeg->m_packetsQueueMutex) == 0;
                const bool audioPacketsQueueIsEmpty =
                    m_ffmpeg->m_audioPacketsQueue.size(m_ffmpeg->m_packetsQueueMutex) == 0;
                if (videoPacketsQueueIsEmpty && audioPacketsQueueIsEmpty &&
                    m_ffmpeg->m_videoFramesQueue.m_busy == 0)
                {
                    if (m_ffmpeg->m_decoderListener)
                        m_ffmpeg->m_decoderListener->onEndOfStream();
                }

                boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
            }
            eof = reader_eof;
        }

        // Continue packet reading
    }

    CHANNEL_LOG(ffmpeg_threads) << "Decoding ended";
}

void ParseRunnable::dispatchPacket(AVPacket& packet)
{
    auto guard = MakeGuard(&packet, av_free_packet);

    if (m_ffmpeg->m_seekDuration >= 0)
    {
        return; // guard frees packet
    }

    if (packet.stream_index == m_ffmpeg->m_videoStreamNumber)
    { 
        {
            boost::unique_lock<boost::mutex> locker(m_ffmpeg->m_packetsQueueMutex);
            while (m_ffmpeg->m_videoPacketsQueue.packetsSize() >= MAX_QUEUE_SIZE ||
                m_ffmpeg->m_videoPacketsQueue.size() >= MAX_VIDEO_FRAMES)
            {
                if (m_ffmpeg->m_seekDuration >= 0)
                {
                    return; // guard frees packet
                }
                m_ffmpeg->m_packetsQueueCV.wait(locker);
            }
            m_ffmpeg->m_videoPacketsQueue.enqueue(packet);
        }
        m_ffmpeg->m_packetsQueueCV.notify_all();
    }
    else if (packet.stream_index == m_ffmpeg->m_audioStreamNumber)
    { 
        {
            boost::unique_lock<boost::mutex> locker(m_ffmpeg->m_packetsQueueMutex);
            while (m_ffmpeg->m_audioPacketsQueue.packetsSize() >= MAX_QUEUE_SIZE ||
                m_ffmpeg->m_audioPacketsQueue.size() >= MAX_AUDIO_FRAMES)
            {
                if (m_ffmpeg->m_seekDuration >= 0)
                {
                    return; // guard frees packet
                }
                m_ffmpeg->m_packetsQueueCV.wait(locker);
            }
            m_ffmpeg->m_audioPacketsQueue.enqueue(packet);
        }
        m_ffmpeg->m_packetsQueueCV.notify_all();
    }
    else
    {
        return; // guard frees packet
    }

    guard.release();
}

void ParseRunnable::startAudioThread(FFmpegDecoder* m_ffmpeg)
{
    if (m_ffmpeg->m_audioStreamNumber >= 0)
    {
        m_ffmpeg->m_mainAudioThread.reset(new boost::thread(AudioParseRunnable(m_ffmpeg)));
    }
}

void ParseRunnable::startVideoThread(FFmpegDecoder* m_ffmpeg)
{
    if (m_ffmpeg->m_videoStreamNumber >= 0)
    {
        m_ffmpeg->m_mainVideoThread.reset(new boost::thread(VideoParseRunnable(m_ffmpeg)));
    }
}

void ParseRunnable::sendSeekPacket()
{
    const int64_t seekDuration = m_ffmpeg->m_seekDuration.exchange(-1);
    if (seekDuration < 0)
    {
        return;
    }

    if (avformat_seek_file(m_ffmpeg->m_formatContext, m_ffmpeg->m_videoStreamNumber, 0,
                           seekDuration, seekDuration, AVSEEK_FLAG_FRAME) < 0)
    {
        CHANNEL_LOG(ffmpeg_seek) << "Seek failed";
        return;
    }

    const bool hasVideo = m_ffmpeg->m_mainVideoThread != 0;
    const bool hasAudio = m_ffmpeg->m_mainAudioThread != 0;

    if (hasVideo)
    {
        m_ffmpeg->m_mainVideoThread->interrupt();
    }
    if (hasAudio)
    {
        m_ffmpeg->m_mainAudioThread->interrupt();
    }

    if (hasVideo)
    {
        m_ffmpeg->m_mainVideoThread->join();
    }
    if (hasAudio)
    {
        m_ffmpeg->m_mainAudioThread->join();
    }

    // Reset stuff
    m_ffmpeg->m_videoPacketsQueue.clear();
    m_ffmpeg->m_audioPacketsQueue.clear();

    const auto currentTime = GetHiResTime();
    if (hasVideo)
    {
        if (m_ffmpeg->m_videoCodecContext)
            avcodec_flush_buffers(m_ffmpeg->m_videoCodecContext);
        m_ffmpeg->m_videoFramesQueue.setDisplayTime(currentTime);
    }
    if (hasAudio)
    {
        if (m_ffmpeg->m_audioCodecContext)
            avcodec_flush_buffers(m_ffmpeg->m_audioCodecContext);
        m_ffmpeg->m_audioPlayer->WaveOutReset();
    }

    m_ffmpeg->seekWhilePaused();

    // Restart
    if (hasVideo)
    {
        m_ffmpeg->m_mainVideoThread.reset(new boost::thread(VideoParseRunnable(m_ffmpeg)));
    }
    if (hasAudio)
    {
        m_ffmpeg->m_mainAudioThread.reset(new boost::thread(AudioParseRunnable(m_ffmpeg)));
    }
}

void ParseRunnable::fixDuration()
{
    AVPacket packet;
    if (m_ffmpeg->m_frameTotalCount <= 0 && m_ffmpeg->m_duration <= 0)
    {
        // Reset rechecking vars
        m_ffmpeg->m_frameTotalCount = 0;
        m_ffmpeg->m_duration = 0;
        while (av_read_frame(m_ffmpeg->m_formatContext, &packet) >= 0)
        {
            if (packet.stream_index == m_ffmpeg->m_videoStreamNumber)
            {
                ++m_ffmpeg->m_frameTotalCount;
                if (packet.pts != AV_NOPTS_VALUE)
                {
                    m_ffmpeg->m_duration = packet.pts;
                }
                else if (packet.dts != AV_NOPTS_VALUE)
                {
                    m_ffmpeg->m_duration = packet.dts;
                }
            }
            av_free_packet(&packet);

            if (boost::this_thread::interruption_requested())
            {
                CHANNEL_LOG(ffmpeg_threads) << "Parse thread broken";
                return;
            }
        }

        if (avformat_seek_file(m_ffmpeg->m_formatContext, m_ffmpeg->m_videoStreamNumber, 0, 0, 0,
                               AVSEEK_FLAG_FRAME) < 0)
        {
            CHANNEL_LOG(ffmpeg_seek) << "Seek failed";
            return;
        }
    }
}
