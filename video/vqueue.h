#pragma once

// Video frame struct for RGB24 frame (used by displays)
struct VQueue
{
    VideoFrame m_frames[VIDEO_PICTURE_QUEUE_SIZE];
    int m_write_counter;
    int m_read_counter;
    int m_busy;

    VQueue() : m_write_counter(0),
               m_read_counter(0),
               m_busy(0)
    {
    }

    void clear()
    {
        for (auto& frame : m_frames)
        {
            frame.m_image.free();
        }

        // Reset readers
        m_write_counter = 0;
        m_read_counter = 0;
        m_busy = 0;
    }

    void setDisplayTime(double displayTime)
    {
        for (auto& frame : m_frames)
        {
            frame.m_displayTime = displayTime;
        }
    }
};
