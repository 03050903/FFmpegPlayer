#pragma once

#include <deque>
#include <boost/thread/mutex.hpp>

class FQueue
{
public:
	FQueue() : m_packetsSize(0) {}

	AVPacket dequeue()
	{
		assert(!m_queue.empty());
		AVPacket packet = m_queue.front();
		m_queue.pop_front();
		m_packetsSize -= packet.size;
		assert(m_packetsSize >= 0);
		return packet;
	}

	void enqueue(const AVPacket& packet)
	{
		m_packetsSize += packet.size;
		assert(m_packetsSize >= 0);
		m_queue.push_back(packet);
	}

	int size(boost::mutex& mutex) const
	{
		boost::lock_guard<boost::mutex> locker(mutex);
		return m_queue.size();
	}

	int size() const
	{
		return m_queue.size();
	}

	int64_t	packetsSize() const
	{
		return m_packetsSize;
	}

	void clear()
	{
		for (AVPacket& packet : m_queue)
		{
			av_free_packet(&packet);
		}
		m_packetsSize = 0;
		std::deque<AVPacket>().swap(m_queue);
	}

private:
	int64_t	m_packetsSize;
	std::deque<AVPacket> m_queue;
};
