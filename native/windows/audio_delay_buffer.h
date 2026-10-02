#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>

namespace speakerflow {

/**
 * @brief Real-time safe, deterministic acoustic delay buffer for WASAPI render branches.
 *
 * Requirements:
 * - Bounded delay: 0.0 ms <= delay <= 500.0 ms. Default: 0.0 ms.
 * - delayFrames = round(delayMs * sampleRate / 1000.0).
 * - Pre-allocates all memory on Initialize() based on maxDelayMs (default 500.0 ms).
 * - Zero allocations, zero mutexes/locks, zero I/O in Process().
 * - Frame-based indexing: handles arbitrary interleaved PCM formats (Float32, PCM16, PCM24, PCM32)
 *   via bytesPerFrame (blockAlign = channels * bytesPerSample).
 * - Zero-delay bypass: when delayFrames == 0, audio passes through with zero unnecessary delay.
 * - Recovery/reset safe: Reset() clears all stale audio and resets priming.
 */
class AudioDelayBuffer {
public:
    static constexpr double MAX_SUPPORTED_DELAY_MS = 500.0;
    static constexpr double MIN_SUPPORTED_DELAY_MS = 0.0;

    AudioDelayBuffer()
        : m_sampleRate(48000),
          m_bytesPerFrame(8), // default stereo Float32
          m_capacityFrames(0),
          m_mask(0),
          m_writePos(0),
          m_configuredDelayMs(0.0),
          m_delayFrames(0),
          m_primedFrames(0),
          m_isInitialized(false) {}

    ~AudioDelayBuffer() = default;

    AudioDelayBuffer(const AudioDelayBuffer&) = delete;
    AudioDelayBuffer& operator=(const AudioDelayBuffer&) = delete;

    /**
     * @brief Preallocates internal storage for the maximum delay at the specified sample rate.
     * @param sampleRate Native endpoint sample rate (e.g. 44100, 48000, 96000, 192000).
     * @param bytesPerFrame Block align in bytes (channels * bytesPerSample).
     * @param maxDelayMs Maximum supported delay in milliseconds (default 500.0).
     * @return true if initialization succeeded.
     */
    bool Initialize(uint32_t sampleRate, size_t bytesPerFrame, double maxDelayMs = MAX_SUPPORTED_DELAY_MS) {
        if (sampleRate == 0 || bytesPerFrame == 0) return false;

        m_sampleRate = sampleRate;
        m_bytesPerFrame = bytesPerFrame;

        // Calculate maximum required frames + 8192 frames safety margin
        double clampedMaxMs = std::max(MIN_SUPPORTED_DELAY_MS, std::min(maxDelayMs, MAX_SUPPORTED_DELAY_MS));
        size_t requiredFrames = static_cast<size_t>(std::ceil((clampedMaxMs * sampleRate) / 1000.0)) + 8192;

        // Capacity rounded up to power of two for fast bitwise masking
        m_capacityFrames = RoundUpPowerOfTwo(requiredFrames);
        m_mask = m_capacityFrames - 1;

        m_buffer.assign(m_capacityFrames * m_bytesPerFrame, 0);

        m_writePos = 0;
        m_primedFrames = 0;
        m_isInitialized = true;

        // Recompute delay frames if delay was configured before Initialize
        UpdateDelayFrames();
        return true;
    }

    /**
     * @brief Configures target delay in milliseconds.
     * @param delayMs Delay duration in milliseconds (0.0 <= delayMs <= 500.0).
     * @param outError Error message if validation fails.
     * @return true if valid, false otherwise.
     */
    bool SetDelayMs(double delayMs, std::string& outError) {
        if (std::isnan(delayMs) || std::isinf(delayMs)) {
            outError = "Delay value cannot be NaN or Infinite.";
            return false;
        }
        if (delayMs < MIN_SUPPORTED_DELAY_MS) {
            outError = "Delay value cannot be negative (minimum is 0 ms).";
            return false;
        }
        if (delayMs > MAX_SUPPORTED_DELAY_MS) {
            outError = "Delay value exceeds maximum limit of " + std::to_string(static_cast<int>(MAX_SUPPORTED_DELAY_MS)) + " ms.";
            return false;
        }

        m_configuredDelayMs.store(delayMs, std::memory_order_release);
        UpdateDelayFrames();
        return true;
    }

    double GetConfiguredDelayMs() const {
        return m_configuredDelayMs.load(std::memory_order_acquire);
    }

    size_t GetEffectiveDelayFrames() const {
        return m_delayFrames.load(std::memory_order_acquire);
    }

    uint32_t GetSampleRate() const {
        return m_sampleRate;
    }

    size_t GetBytesPerFrame() const {
        return m_bytesPerFrame;
    }

    size_t GetCapacityFrames() const {
        return m_capacityFrames;
    }

    bool IsInitialized() const {
        return m_isInitialized;
    }

    /**
     * @brief Resets buffer contents and write/prime state.
     * Clears all stale audio and resets priming so no stale pre-loss audio is replayed on recovery.
     */
    void Reset() {
        if (!m_buffer.empty()) {
            std::fill(m_buffer.begin(), m_buffer.end(), 0);
        }
        m_writePos = 0;
        m_primedFrames = 0;
    }

    /**
     * @brief Real-time processing method.
     * Writes incoming audio frames into the circular buffer and reads audio delayed by delayFrames into pDst.
     *
     * Real-time guarantees:
     * - Zero heap allocations.
     * - Zero mutex/lock acquisitions.
     * - Zero file I/O or system calls.
     *
     * @param pSrc Input audio buffer containing frameCount interleaved frames.
     * @param pDst Output audio buffer receiving frameCount interleaved frames.
     * @param frameCount Number of frames to process.
     */
    void Process(const void* pSrc, void* pDst, size_t frameCount) {
        if (!pDst || frameCount == 0) return;
        if (m_capacityFrames > 0 && frameCount > m_capacityFrames) return;

        const size_t targetDelay = m_delayFrames.load(std::memory_order_acquire);

        // Zero-delay fast path: direct copy without delay buffer
        if (targetDelay == 0) {
            if (pSrc && pSrc != pDst) {
                std::memcpy(pDst, pSrc, frameCount * m_bytesPerFrame);
            } else if (!pSrc) {
                std::memset(pDst, 0, frameCount * m_bytesPerFrame);
            }
            m_primedFrames = 0;
            return;
        }

        // If delay is active but buffer not initialized, output silence
        if (!m_isInitialized || m_buffer.empty()) {
            std::memset(pDst, 0, frameCount * m_bytesPerFrame);
            return;
        }

        const uint8_t* pSrcBytes = static_cast<const uint8_t*>(pSrc);
        uint8_t* pDstBytes = static_cast<uint8_t*>(pDst);

        // 1. Write incoming frames into circular buffer at m_writePos
        if (pSrcBytes) {
            WriteChunk(pSrcBytes, frameCount);
        } else {
            WriteSilence(frameCount);
        }

        // 2. Determine output frames from delayed read position
        // Read position is (m_writePos - targetDelay)
        // If buffer has accumulated fewer than targetDelay frames since reset, output silence
        if (m_primedFrames < targetDelay) {
            size_t silenceNeeded = targetDelay - m_primedFrames;
            size_t silenceFrames = std::min(frameCount, silenceNeeded);
            std::memset(pDstBytes, 0, silenceFrames * m_bytesPerFrame);

            m_primedFrames += frameCount;

            if (frameCount > silenceFrames) {
                size_t audioFrames = frameCount - silenceFrames;
                size_t readStartPos = m_writePos - targetDelay - audioFrames;
                ReadChunk(pDstBytes + (silenceFrames * m_bytesPerFrame), readStartPos, audioFrames);
            }
        } else {
            size_t readStartPos = m_writePos - targetDelay - frameCount;
            ReadChunk(pDstBytes, readStartPos, frameCount);
            m_primedFrames += frameCount;
        }
    }

    /**
     * @brief Computes frame count for a given millisecond delay at a specified sample rate.
     * Helper for deterministic testing and validation.
     */
    static size_t CalculateDelayFrames(double delayMs, uint32_t sampleRate) {
        if (delayMs <= 0.0 || sampleRate == 0) return 0;
        return static_cast<size_t>(std::round((delayMs * sampleRate) / 1000.0));
    }

private:
    void UpdateDelayFrames() {
        double delayMs = m_configuredDelayMs.load(std::memory_order_acquire);
        if (delayMs <= 0.0 || m_sampleRate == 0) {
            m_delayFrames.store(0, std::memory_order_release);
        } else {
            m_delayFrames.store(CalculateDelayFrames(delayMs, m_sampleRate), std::memory_order_release);
        }
    }

    void WriteChunk(const uint8_t* pData, size_t frames) {
        size_t writeIdx = m_writePos & m_mask;
        size_t firstChunk = std::min(frames, m_capacityFrames - writeIdx);
        size_t secondChunk = frames - firstChunk;

        std::memcpy(&m_buffer[writeIdx * m_bytesPerFrame],
                    pData,
                    firstChunk * m_bytesPerFrame);

        if (secondChunk > 0) {
            std::memcpy(&m_buffer[0],
                        pData + (firstChunk * m_bytesPerFrame),
                        secondChunk * m_bytesPerFrame);
        }

        m_writePos += frames;
    }

    void WriteSilence(size_t frames) {
        size_t writeIdx = m_writePos & m_mask;
        size_t firstChunk = std::min(frames, m_capacityFrames - writeIdx);
        size_t secondChunk = frames - firstChunk;

        std::memset(&m_buffer[writeIdx * m_bytesPerFrame], 0, firstChunk * m_bytesPerFrame);
        if (secondChunk > 0) {
            std::memset(&m_buffer[0], 0, secondChunk * m_bytesPerFrame);
        }

        m_writePos += frames;
    }

    void ReadChunk(uint8_t* pDest, size_t readPos, size_t frames) {
        size_t readIdx = readPos & m_mask;
        size_t firstChunk = std::min(frames, m_capacityFrames - readIdx);
        size_t secondChunk = frames - firstChunk;

        std::memcpy(pDest,
                    &m_buffer[readIdx * m_bytesPerFrame],
                    firstChunk * m_bytesPerFrame);

        if (secondChunk > 0) {
            std::memcpy(pDest + (firstChunk * m_bytesPerFrame),
                        &m_buffer[0],
                        secondChunk * m_bytesPerFrame);
        }
    }

    static size_t RoundUpPowerOfTwo(size_t val) {
        if (val <= 1) return 1;
        size_t power = 1;
        while (power < val) {
            power <<= 1;
            if (power == 0) return val; // Overflow guard
        }
        return power;
    }

    uint32_t m_sampleRate;
    size_t m_bytesPerFrame;
    size_t m_capacityFrames;
    size_t m_mask;

    std::vector<uint8_t> m_buffer;

    size_t m_writePos;
    std::atomic<double> m_configuredDelayMs{0.0};
    std::atomic<size_t> m_delayFrames{0};
    size_t m_primedFrames;
    bool m_isInitialized;
};

} // namespace speakerflow
