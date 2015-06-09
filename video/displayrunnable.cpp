#include "displayrunnable.h"

void DisplayRunnable::operator()()
{
    CHANNEL_LOG(ffmpeg_threads) << "Displaying thread started";
    FFmpegDecoder* ff = m_ffmpeg;

    for (;;)
    {
        {
            boost::unique_lock<boost::mutex> locker(ff->m_videoFramesMutex);
            ff->m_videoFramesCV.wait(locker, [ff]()
                                     {
                                         return !ff->m_frameDisplayingRequested &&
                                                ff->m_videoFramesQueue.m_busy != 0;
                                     });
        }

        VideoFrame* current_frame =
            &ff->m_videoFramesQueue.m_frames[ff->m_videoFramesQueue.m_read_counter];

        // Frame skip
        if (ff->m_videoFramesQueue.m_busy > 1 && current_frame->m_displayTime < GetHiResTime())
        {
            CHANNEL_LOG(ffmpeg_threads) << __FUNCTION__ << " Framedrop";
            ff->finishedDisplayingFrame();
            continue;
        }

        ff->m_frameDisplayingRequested = true;

        // Possibly give it time to render frame
        if (ff->m_frameListener)
        {
            ff->m_frameListener->updateFrame();
        }

        for (;;)
        {
            double current_time = GetHiResTime();
            double delay = current_frame->m_displayTime - current_time;
            if (delay < 0.005)
                break;
            if (delay > 3.)
            {
                boost::this_thread::sleep_for(boost::chrono::milliseconds(3000));
                continue;
            }

            boost::this_thread::sleep_for(boost::chrono::milliseconds(int(delay * 1000.)));
            break;
        }

        // It's time to display converted frame
        if (ff->m_decoderListener)
            ff->m_decoderListener->changedFramePosition(current_frame->m_duration, ff->m_duration);

        if (ff->m_frameListener)
        {
            ff->m_frameListener->drawFrame();
        }
        else
        {
            ff->finishedDisplayingFrame();
        }
    }
}
