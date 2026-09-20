#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace speakerflow {

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) circular buffer for interleaved float audio.
 *
 * Thread-safe between one producer (e.g. WASAPI capture thread) and one consumer (e.g. render worker).
 * Preallocates all memory on initialization. Zero allocations in Write() and Read().
 */
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacityFrames, size_t channels)
        : m_channels(channels ? channels : 2),
          m_capacity(RoundUpPowerOfTwo(capacityFrames < 1024 ? 1024 : capacityFrames)),
          m_mask(m_capacity - 1),
          m_writePos(0),
          m_readPos(0),
          m_underrunCount(0),
          m_overrunCount(0),
          m_totalFramesWritten(0),
          m_totalFramesRead(0) {
        m_buffer.resize(m_capacity * m_channels, 0.0f);
    }

    ~AudioRingBuffer() = default;

    // Non-copyable, non-movable
    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    /**
     * @brief Writes up to framesToWrite interleaved frames into the ring buffer.
     * @param pData Pointer to interleaved float samples (size = framesToWrite * channels).
     * @param framesToWrite Number of frames to write.
     * @return Number of frames actually written.
     */
    size_t Write(const float* pData, size_t framesToWrite) {
        if (!pData || framesToWrite == 0) return 0;

        const size_t writePos = m_writePos.load(std::memory_order_relaxed);
        const size_t readPos = m_readPos.load(std::memory_order_acquire);
        const size_t occupied = writePos - readPos;

        if (occupied >= m_capacity) {
            // Buffer completely full
            m_overrunCount.fetch_add(framesToWrite, std::memory_order_relaxed);
            return 0;
        }

        const size_t spaceAvailable = m_capacity - occupied;
        const size_t framesToCopy = std::min(framesToWrite, spaceAvailable);

        if (framesToCopy < framesToWrite) {
            m_overrunCount.fetch_add(framesToWrite - framesToCopy, std::memory_order_relaxed);
        }

        const size_t writeIndex = writePos & m_mask;
        const size_t firstChunkFrames = std::min(framesToCopy, m_capacity - writeIndex);
        const size_t secondChunkFrames = framesToCopy - firstChunkFrames;

        std::memcpy(&m_buffer[writeIndex * m_channels],
                    pData,
                    firstChunkFrames * m_channels * sizeof(float));

        if (secondChunkFrames > 0) {
            std::memcpy(&m_buffer[0],
                        pData + (firstChunkFrames * m_channels),
                        secondChunkFrames * m_channels * sizeof(float));
        }

        m_writePos.store(writePos + framesToCopy, std::memory_order_release);
        m_totalFramesWritten.fetch_add(framesToCopy, std::memory_order_relaxed);
        return framesToCopy;
    }

    /**
     * @brief Reads up to framesToRead interleaved frames from the ring buffer.
     *        If fewer frames are available, the remainder of pDest is padded with silence (zeros).
     * @param pDest Output buffer for interleaved float samples (size >= framesToRead * channels).
     * @param framesToRead Number of frames requested.
     * @return Number of actual audio frames read (before any zero-padding).
     */
    size_t Read(float* pDest, size_t framesToRead) {
        if (!pDest || framesToRead == 0) return 0;

        const size_t readPos = m_readPos.load(std::memory_order_relaxed);
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t available = writePos - readPos;

        const size_t framesToCopy = std::min(framesToRead, available);
        const size_t framesUnder = framesToRead - framesToCopy;

        if (framesToCopy > 0) {
            const size_t readIndex = readPos & m_mask;
            const size_t firstChunkFrames = std::min(framesToCopy, m_capacity - readIndex);
            const size_t secondChunkFrames = framesToCopy - firstChunkFrames;

            std::memcpy(pDest,
                        &m_buffer[readIndex * m_channels],
                        firstChunkFrames * m_channels * sizeof(float));

            if (secondChunkFrames > 0) {
                std::memcpy(pDest + (firstChunkFrames * m_channels),
                            &m_buffer[0],
                            secondChunkFrames * m_channels * sizeof(float));
            }

            m_readPos.store(readPos + framesToCopy, std::memory_order_release);
            m_totalFramesRead.fetch_add(framesToCopy, std::memory_order_relaxed);
        }

        // Fill remaining requested frames with silence to prevent glitching
        if (framesUnder > 0) {
            std::memset(pDest + (framesToCopy * m_channels),
                        0,
                        framesUnder * m_channels * sizeof(float));
            m_underrunCount.fetch_add(framesUnder, std::memory_order_relaxed);
        }

        return framesToCopy;
    }

    /**
     * @brief Number of frames currently available to read.
     */
    size_t AvailableRead() const {
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t readPos = m_readPos.load(std::memory_order_relaxed);
        return (writePos >= readPos) ? (writePos - readPos) : 0;
    }

    /**
     * @brief Number of frames of free space available to write.
     */
    size_t AvailableWrite() const {
        const size_t occupied = AvailableRead();
        return (occupied < m_capacity) ? (m_capacity - occupied) : 0;
    }

    size_t GetCapacity() const { return m_capacity; }
    size_t GetChannels() const { return m_channels; }

    uint64_t GetUnderrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }
    uint64_t GetOverrunCount() const { return m_overrunCount.load(std::memory_order_relaxed); }
    uint64_t GetTotalFramesWritten() const { return m_totalFramesWritten.load(std::memory_order_relaxed); }
    uint64_t GetTotalFramesRead() const { return m_totalFramesRead.load(std::memory_order_relaxed); }

    void Reset() {
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
        m_underrunCount.store(0, std::memory_order_relaxed);
        m_overrunCount.store(0, std::memory_order_relaxed);
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
    }

private:
    static size_t RoundUpPowerOfTwo(size_t val) {
        if (val <= 1) return 1;
        size_t power = 1;
        while (power < val) {
            power <<= 1;
            if (power == 0) return val; // Overflow check
        }
        return power;
    }

    const size_t m_channels;
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<float> m_buffer;

    alignas(64) std::atomic<size_t> m_writePos;
    alignas(64) std::atomic<size_t> m_readPos;

    std::atomic<uint64_t> m_underrunCount;
    std::atomic<uint64_t> m_overrunCount;
    std::atomic<uint64_t> m_totalFramesWritten;
    std::atomic<uint64_t> m_totalFramesRead;
};

} // namespace speakerflow
