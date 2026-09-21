#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

#include "wasapi_capture_client.h" // For SampleFormatType

namespace speakerflow {

/**
 * @brief AudioFormatPipeline implements a high-performance, real-time safe
 * audio processing pipeline for a single WASAPI render branch.
 *
 * It provides:
 *  1. Sample-rate conversion (linear interpolation with continuous fractional phase
 *     and sample history across arbitrary chunk boundaries).
 *  2. Equal-rate fast path (memcpy bypass when inRate == outRate and ratio == 1.0).
 *  3. Resampling ratio multiplier for future fine clock-drift adjustments.
 *  4. Channel conversions: N->N, Stereo->Mono, Mono->Stereo, Stereo->Multichannel,
 *     and Multichannel->Stereo (ITU-R BS.775 with center and surround contribution).
 *  5. Sample format conversion: Float32 -> PCM16, PCM24 in 32-bit container,
 *     PCM24 packed (3 bytes), PCM32, and Float32.
 *  6. Robust clamping and NaN handling consistent with SpeakerFlow conventions.
 *  7. Zero memory allocation, zero locks/mutexes, zero I/O, zero V8/Node calls
 *     in all processing paths.
 */
class AudioFormatPipeline {
public:
    AudioFormatPipeline() = default;
    ~AudioFormatPipeline() = default;

    // Non-copyable, movable
    AudioFormatPipeline(const AudioFormatPipeline&) = delete;
    AudioFormatPipeline& operator=(const AudioFormatPipeline&) = delete;
    AudioFormatPipeline(AudioFormatPipeline&&) noexcept = default;
    AudioFormatPipeline& operator=(AudioFormatPipeline&&) noexcept = default;

    /**
     * @brief Preallocates internal scratch buffers and configures format parameters.
     * Must be called before Process(). Safe to call repeatedly to reconfigure.
     *
     * @param inSampleRate Source sample rate (e.g. 48000).
     * @param inChannels Source channel count (e.g. 2).
     * @param outSampleRate Destination sample rate (e.g. 44100).
     * @param outChannels Destination channel count (e.g. 2, 6, 8).
     * @param outFormat Destination sample format (Float32, Pcm16, etc.).
     * @param maxFrames Maximum expected frame count per call (default 8192).
     * @return true if initialization succeeded.
     */
    bool Initialize(uint32_t inSampleRate,
                    uint32_t inChannels,
                    uint32_t outSampleRate,
                    uint32_t outChannels,
                    SampleFormatType outFormat,
                    size_t maxFrames = 8192) {
        if (inSampleRate == 0 || inChannels == 0 || outSampleRate == 0 || outChannels == 0) {
            return false;
        }

        m_inSampleRate = inSampleRate;
        m_inChannels = inChannels;
        m_outSampleRate = outSampleRate;
        m_outChannels = outChannels;
        m_outFormat = outFormat;
        m_maxFrames = (maxFrames > 0) ? maxFrames : 8192;

        // Sizing staging buffers: account for rate expansion (e.g. 44.1k -> 192k)
        // plus safety headroom.
        double maxRateRatio = static_cast<double>(outSampleRate) / static_cast<double>(inSampleRate);
        if (maxRateRatio < 1.0) maxRateRatio = 1.0;
        size_t allocFrames = static_cast<size_t>(std::ceil(m_maxFrames * maxRateRatio * 2.0));
        if (allocFrames < 16384) allocFrames = 16384;

        m_resampleStaging.assign(allocFrames * m_inChannels, 0.0f);
        m_channelStaging.assign(allocFrames * m_outChannels, 0.0f);
        m_historyFrame.assign(m_inChannels, 0.0f);

        Reset();
        return true;
    }

    /**
     * @brief Resets stream history and phase without reallocating buffers.
     */
    void Reset() {
        m_phase = 0.0;
        m_hasHistory = false;
        if (!m_historyFrame.empty()) {
            std::fill(m_historyFrame.begin(), m_historyFrame.end(), 0.0f);
        }
    }

    /**
     * @brief Set fine resampling ratio multiplier (e.g. 1.0005 for clock drift).
     * Defaults to 1.0. Does not trigger drift correction algorithms.
     */
    void SetResampleRatioMultiplier(double multiplier) {
        if (multiplier > 0.01 && multiplier < 100.0) {
            m_ratioMultiplier = multiplier;
        }
    }

    double GetResampleRatioMultiplier() const {
        return m_ratioMultiplier;
    }

    /**
     * @brief Calculates the number of input frames needed to produce outFrames output frames.
     * Takes into account the current resampler fractional phase.
     */
    size_t GetRequiredInFrames(size_t outFrames) const {
        if (outFrames == 0) return 0;

        if (m_inSampleRate == m_outSampleRate && m_ratioMultiplier == 1.0) {
            return outFrames;
        }

        double step = (static_cast<double>(m_inSampleRate) / static_cast<double>(m_outSampleRate)) * m_ratioMultiplier;
        if (step <= 0.0) step = 1.0;

        double posLast = m_phase + static_cast<double>(outFrames - 1) * step;
        if (posLast < 0.0) {
            return 1;
        }

        // To linearly interpolate at posLast, we need indices floor(posLast) and floor(posLast) + 1.
        // Number of frames needed is (floor(posLast) + 1) + 1 = floor(posLast) + 2.
        return static_cast<size_t>(std::floor(posLast)) + 2;
    }

    /**
     * @brief Calculates the maximum number of output frames that can be completely
     * produced from inFrames given the current resampler phase and history.
     */
    size_t GetExpectedOutFrames(size_t inFrames) const {
        if (inFrames == 0) return 0;

        if (m_inSampleRate == m_outSampleRate && m_ratioMultiplier == 1.0) {
            return inFrames;
        }

        double step = (static_cast<double>(m_inSampleRate) / static_cast<double>(m_outSampleRate)) * m_ratioMultiplier;
        if (step <= 0.0) step = 1.0;

        // The maximum input index available is inFrames - 1.
        // We can interpolate up to pos where floor(pos) + 1 < inFrames, i.e., pos < inFrames - 1 (or exact sample at inFrames - 1).
        double maxPos = static_cast<double>(inFrames - 1);
        if (maxPos <= m_phase) return 0;

        double span = maxPos - m_phase;
        size_t count = static_cast<size_t>(std::floor(span / step)) + 1;
        return count;
    }

    /**
     * @brief Executes full end-to-end processing pipeline:
     * Resample -> Channel Map -> Format Convert.
     *
     * Real-time safe: zero memory allocations, zero locks, zero I/O, zero V8.
     *
     * @param pIn Interleaved Float32 source samples.
     * @param inFrames Number of available source frames.
     * @param pOut Destination buffer for raw audio formatted according to outFormat.
     * @param outFramesRequested Number of output frames to write to pOut.
     * @param outFramesProduced Number of valid output frames generated (remainder zero-padded).
     * @return true if successful.
     */
    bool Process(const float* pIn,
                 size_t inFrames,
                 void* pOut,
                 size_t outFramesRequested,
                 size_t& outFramesProduced) {
        if (!pOut || outFramesRequested == 0) {
            outFramesProduced = 0;
            return false;
        }

        if (!pIn || inFrames == 0) {
            // Underflow / silence fill
            ConvertFormatSilence(pOut, outFramesRequested, m_outFormat, m_outChannels);
            outFramesProduced = 0;
            return true;
        }

        const bool needResample = (m_inSampleRate != m_outSampleRate || m_ratioMultiplier != 1.0);
        const bool needChannelMap = (m_inChannels != m_outChannels);

        const float* pSourceForChannels = pIn;
        size_t resampledFrames = 0;

        if (needResample) {
            resampledFrames = Resample(pIn,
                                       inFrames,
                                       m_resampleStaging.data(),
                                       outFramesRequested);
            pSourceForChannels = m_resampleStaging.data();
            outFramesProduced = resampledFrames;
        } else {
            // Equal-rate fast path
            size_t framesToCopy = std::min(inFrames, outFramesRequested);
            if (needChannelMap || m_outFormat != SampleFormatType::Float32) {
                std::memcpy(m_resampleStaging.data(), pIn, framesToCopy * m_inChannels * sizeof(float));
                if (outFramesRequested > framesToCopy) {
                    std::memset(m_resampleStaging.data() + framesToCopy * m_inChannels,
                                0,
                                (outFramesRequested - framesToCopy) * m_inChannels * sizeof(float));
                }
                pSourceForChannels = m_resampleStaging.data();
            } else {
                pSourceForChannels = pIn;
            }

            if (framesToCopy > 0) {
                for (size_t c = 0; c < m_inChannels; ++c) {
                    m_historyFrame[c] = pIn[(framesToCopy - 1) * m_inChannels + c];
                }
                m_hasHistory = true;
            }
            m_phase = 0.0;
            outFramesProduced = framesToCopy;
        }

        const float* pSourceForFormat = pSourceForChannels;
        if (needChannelMap) {
            MapChannels(pSourceForChannels,
                        m_inChannels,
                        m_channelStaging.data(),
                        m_outChannels,
                        outFramesRequested);
            pSourceForFormat = m_channelStaging.data();
        }

        ConvertFormat(pSourceForFormat,
                      pOut,
                      outFramesRequested,
                      m_outFormat,
                      m_outChannels);

        return true;
    }

    /**
     * @brief Standalone linear resampler with continuous phase and history.
     *
     * Processes interleaved Float32 audio. Preserves phase across chunk boundaries so
     * that processing chunk-by-chunk yields the identical output sequence as
     * processing the equivalent continuous audio stream.
     *
     * Real-time safe.
     *
     * @param pIn Source interleaved Float32 buffer.
     * @param inFrames Number of available source frames.
     * @param pOut Destination buffer (must hold at least outFramesRequested * channels floats).
     * @param outFramesRequested Number of frames to produce.
     * @param pInFramesConsumed Optional pointer receiving number of input frames consumed.
     * @return Number of valid frames generated. If fewer than outFramesRequested,
     *         the remainder of pOut is zero-padded.
     */
    size_t Resample(const float* pIn,
                    size_t inFrames,
                    float* pOut,
                    size_t outFramesRequested,
                    size_t* pInFramesConsumed = nullptr) {
        if (!pOut || outFramesRequested == 0) {
            if (pInFramesConsumed) *pInFramesConsumed = 0;
            return 0;
        }

        if (!pIn || inFrames == 0) {
            std::memset(pOut, 0, outFramesRequested * m_inChannels * sizeof(float));
            if (pInFramesConsumed) *pInFramesConsumed = 0;
            return 0;
        }

        // Equal-rate fast path
        if (m_inSampleRate == m_outSampleRate && m_ratioMultiplier == 1.0) {
            size_t framesToCopy = std::min(inFrames, outFramesRequested);
            std::memcpy(pOut, pIn, framesToCopy * m_inChannels * sizeof(float));
            if (outFramesRequested > framesToCopy) {
                std::memset(pOut + framesToCopy * m_inChannels,
                            0,
                            (outFramesRequested - framesToCopy) * m_inChannels * sizeof(float));
            }
            if (framesToCopy > 0) {
                for (size_t c = 0; c < m_inChannels; ++c) {
                    m_historyFrame[c] = pIn[(framesToCopy - 1) * m_inChannels + c];
                }
                m_hasHistory = true;
            }
            m_phase = 0.0;
            if (pInFramesConsumed) *pInFramesConsumed = framesToCopy;
            return framesToCopy;
        }

        const double step = (static_cast<double>(m_inSampleRate) / static_cast<double>(m_outSampleRate)) * m_ratioMultiplier;
        if (step <= 0.0) {
            std::memset(pOut, 0, outFramesRequested * m_inChannels * sizeof(float));
            if (pInFramesConsumed) *pInFramesConsumed = 0;
            return 0;
        }

        double pos = m_phase;
        size_t outProduced = 0;
        size_t maxInputIndexNeeded = 0;

        for (size_t i = 0; i < outFramesRequested; ++i) {
            if (pos < 0.0) {
                // Interpolating across previous chunk boundary:
                // pos is in [-1.0, 0.0). Sample -1 is m_historyFrame, sample 0 is pIn[0].
                if (!m_hasHistory) {
                    // Stream start with negative phase (e.g. rounding): clamp to sample 0
                    for (size_t c = 0; c < m_inChannels; ++c) {
                        pOut[i * m_inChannels + c] = pIn[0 * m_inChannels + c];
                    }
                } else {
                    float alpha = static_cast<float>(pos + 1.0);
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;

                    for (size_t c = 0; c < m_inChannels; ++c) {
                        float s0 = m_historyFrame[c];
                        float s1 = pIn[0 * m_inChannels + c];
                        pOut[i * m_inChannels + c] = s0 + alpha * (s1 - s0);
                    }
                }
                maxInputIndexNeeded = std::max<size_t>(maxInputIndexNeeded, 1);
                outProduced++;
            } else {
                size_t k = static_cast<size_t>(pos);
                float alpha = static_cast<float>(pos - static_cast<double>(k));
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;

                if (alpha == 0.0f && k < inFrames) {
                    for (size_t c = 0; c < m_inChannels; ++c) {
                        pOut[i * m_inChannels + c] = pIn[k * m_inChannels + c];
                    }
                    maxInputIndexNeeded = std::max<size_t>(maxInputIndexNeeded, k + 1);
                    outProduced++;
                } else if (k + 1 < inFrames) {
                    for (size_t c = 0; c < m_inChannels; ++c) {
                        float s0 = pIn[k * m_inChannels + c];
                        float s1 = pIn[(k + 1) * m_inChannels + c];
                        pOut[i * m_inChannels + c] = s0 + alpha * (s1 - s0);
                    }
                    maxInputIndexNeeded = std::max<size_t>(maxInputIndexNeeded, k + 2);
                    outProduced++;
                } else {
                    // Next input sample is in the subsequent chunk.
                    break;
                }
            }
            pos += step;
        }

        // Determine input frames consumed
        size_t consumed = inFrames;
        if (pInFramesConsumed) {
            consumed = std::min(inFrames, maxInputIndexNeeded);
            *pInFramesConsumed = consumed;
        }

        // Update persistent history and phase for next chunk
        if (inFrames > 0) {
            for (size_t c = 0; c < m_inChannels; ++c) {
                m_historyFrame[c] = pIn[(inFrames - 1) * m_inChannels + c];
            }
            m_hasHistory = true;
            m_phase = pos - static_cast<double>(inFrames);
        }

        // Zero-fill any starved output frames up to outFramesRequested
        if (outProduced < outFramesRequested) {
            std::memset(pOut + outProduced * m_inChannels,
                        0,
                        (outFramesRequested - outProduced) * m_inChannels * sizeof(float));
        }

        return outProduced;
    }

    /**
     * @brief Maps channel counts between source and destination configurations.
     *
     * Supports:
     *  - N -> N copy
     *  - Stereo -> Mono (0.5 * (L + R))
     *  - Mono -> Stereo (L = M, R = M)
     *  - Stereo -> Multichannel (FL = L, FR = R, other channels zeroed)
     *  - Multichannel (5.1/7.1/4.0/3.0) -> Stereo according to ITU-R BS.775
     *    with Center (+0.7071) and Surround (+0.7071) contributions, LFE omitted.
     *  - Generic channel count fallback.
     *
     * Real-time safe.
     */
    static void MapChannels(const float* pIn,
                            size_t inChannels,
                            float* pOut,
                            size_t outChannels,
                            size_t frames) {
        if (!pIn || !pOut || frames == 0) return;

        if (inChannels == outChannels) {
            std::memcpy(pOut, pIn, frames * inChannels * sizeof(float));
            return;
        }

        // Stereo -> Mono downmix
        if (inChannels == 2 && outChannels == 1) {
            for (size_t i = 0; i < frames; ++i) {
                pOut[i] = 0.5f * (pIn[i * 2] + pIn[i * 2 + 1]);
            }
            return;
        }

        // Mono -> Stereo upmix
        if (inChannels == 1 && outChannels == 2) {
            for (size_t i = 0; i < frames; ++i) {
                float s = pIn[i];
                pOut[i * 2]     = s;
                pOut[i * 2 + 1] = s;
            }
            return;
        }

        // Stereo -> Multichannel (> 2): Front Left and Front Right, silence in remaining
        if (inChannels == 2 && outChannels > 2) {
            for (size_t i = 0; i < frames; ++i) {
                pOut[i * outChannels]     = pIn[i * 2];
                pOut[i * outChannels + 1] = pIn[i * 2 + 1];
                for (size_t c = 2; c < outChannels; ++c) {
                    pOut[i * outChannels + c] = 0.0f;
                }
            }
            return;
        }

        // Multichannel -> Stereo downmix (ITU-R BS.775 design)
        if (inChannels > 2 && outChannels == 2) {
            constexpr float SQRT_HALF = 0.70710678f;

            if (inChannels == 6) {
                // 5.1: FL=0, FR=1, C=2, LFE=3, SL=4, SR=5
                for (size_t i = 0; i < frames; ++i) {
                    const float* in = pIn + i * 6;
                    float fl = in[0];
                    float fr = in[1];
                    float c  = in[2];
                    float sl = in[4];
                    float sr = in[5];
                    pOut[i * 2]     = 0.5f * (fl + SQRT_HALF * c + SQRT_HALF * sl);
                    pOut[i * 2 + 1] = 0.5f * (fr + SQRT_HALF * c + SQRT_HALF * sr);
                }
                return;
            }

            if (inChannels >= 8) {
                // 7.1: FL=0, FR=1, C=2, LFE=3, BL=4, BR=5, SL=6, SR=7
                for (size_t i = 0; i < frames; ++i) {
                    const float* in = pIn + i * inChannels;
                    float fl = in[0];
                    float fr = in[1];
                    float c  = in[2];
                    float bl = in[4];
                    float br = in[5];
                    float sl = in[6];
                    float sr = in[7];
                    pOut[i * 2]     = 0.5f * (fl + SQRT_HALF * c + 0.5f * bl + 0.5f * sl);
                    pOut[i * 2 + 1] = 0.5f * (fr + SQRT_HALF * c + 0.5f * br + 0.5f * sr);
                }
                return;
            }

            if (inChannels == 3) {
                // 3.0: FL=0, FR=1, C=2
                for (size_t i = 0; i < frames; ++i) {
                    const float* in = pIn + i * 3;
                    pOut[i * 2]     = 0.5f * (in[0] + SQRT_HALF * in[2]);
                    pOut[i * 2 + 1] = 0.5f * (in[1] + SQRT_HALF * in[2]);
                }
                return;
            }

            if (inChannels == 4) {
                // 4.0 Quad: FL=0, FR=1, SL=2, SR=3
                for (size_t i = 0; i < frames; ++i) {
                    const float* in = pIn + i * 4;
                    pOut[i * 2]     = 0.5f * (in[0] + SQRT_HALF * in[2]);
                    pOut[i * 2 + 1] = 0.5f * (in[1] + SQRT_HALF * in[3]);
                }
                return;
            }

            // Generic fallback for any other M > 2: take FL and FR
            for (size_t i = 0; i < frames; ++i) {
                pOut[i * 2]     = pIn[i * inChannels];
                pOut[i * 2 + 1] = pIn[i * inChannels + 1];
            }
            return;
        }

        // General fallback: copy min channels, zero out remaining
        size_t copyChannels = std::min(inChannels, outChannels);
        for (size_t i = 0; i < frames; ++i) {
            for (size_t c = 0; c < copyChannels; ++c) {
                pOut[i * outChannels + c] = pIn[i * inChannels + c];
            }
            for (size_t c = copyChannels; c < outChannels; ++c) {
                pOut[i * outChannels + c] = 0.0f;
            }
        }
    }

    /**
     * @brief Converts Float32 audio into the requested raw sample format with
     * strict clamping to [-1.0, 1.0] and NaN neutralization.
     *
     * Real-time safe.
     */
    static void ConvertFormat(const float* pIn,
                              void* pDest,
                              size_t frames,
                              SampleFormatType fmt,
                              size_t channels) {
        if (!pIn || !pDest || frames == 0 || channels == 0) return;

        const size_t totalSamples = frames * channels;
        uint8_t* pBytes = reinterpret_cast<uint8_t*>(pDest);

        switch (fmt) {
            case SampleFormatType::Float32: {
                if (pDest != pIn) {
                    std::memcpy(pDest, pIn, totalSamples * sizeof(float));
                }
                break;
            }

            case SampleFormatType::Pcm16: {
                int16_t* p16 = reinterpret_cast<int16_t*>(pDest);
                for (size_t i = 0; i < totalSamples; ++i) {
                    float f = pIn[i];
                    if (!std::isfinite(f)) f = 0.0f;
                    f = std::clamp(f, -1.0f, 1.0f);
                    p16[i] = static_cast<int16_t>(f * 32767.0f);
                }
                break;
            }

            case SampleFormatType::Pcm24In32: {
                int32_t* p32 = reinterpret_cast<int32_t*>(pDest);
                for (size_t i = 0; i < totalSamples; ++i) {
                    float f = pIn[i];
                    if (!std::isfinite(f)) f = 0.0f;
                    f = std::clamp(f, -1.0f, 1.0f);
                    int64_t sample = static_cast<int64_t>(f * 8388607.0f);
                    p32[i] = static_cast<int32_t>(sample * 256);
                }
                break;
            }

            case SampleFormatType::Pcm24Packed: {
                for (size_t i = 0; i < totalSamples; ++i) {
                    float f = pIn[i];
                    if (!std::isfinite(f)) f = 0.0f;
                    f = std::clamp(f, -1.0f, 1.0f);
                    int32_t val = static_cast<int32_t>(f * 8388607.0f);
                    pBytes[i * 3]     = static_cast<uint8_t>(val & 0xFF);
                    pBytes[i * 3 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
                    pBytes[i * 3 + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
                }
                break;
            }

            case SampleFormatType::Pcm32: {
                int32_t* p32 = reinterpret_cast<int32_t*>(pDest);
                for (size_t i = 0; i < totalSamples; ++i) {
                    float f = pIn[i];
                    if (!std::isfinite(f)) f = 0.0f;
                    f = std::clamp(f, -1.0f, 1.0f);
                    p32[i] = static_cast<int32_t>(static_cast<double>(f) * 2147483647.0);
                }
                break;
            }

            default: {
                // Zero out unknown formats
                std::memset(pDest, 0, totalSamples * sizeof(float));
                break;
            }
        }
    }

    /**
     * @brief Helper to fill a raw buffer with silence for a given format.
     */
    static void ConvertFormatSilence(void* pDest,
                                     size_t frames,
                                     SampleFormatType fmt,
                                     size_t channels) {
        if (!pDest || frames == 0 || channels == 0) return;
        const size_t totalSamples = frames * channels;

        switch (fmt) {
            case SampleFormatType::Float32:
                std::memset(pDest, 0, totalSamples * sizeof(float));
                break;
            case SampleFormatType::Pcm16:
                std::memset(pDest, 0, totalSamples * sizeof(int16_t));
                break;
            case SampleFormatType::Pcm24In32:
            case SampleFormatType::Pcm32:
                std::memset(pDest, 0, totalSamples * sizeof(int32_t));
                break;
            case SampleFormatType::Pcm24Packed:
                std::memset(pDest, 0, totalSamples * 3);
                break;
            default:
                std::memset(pDest, 0, totalSamples * sizeof(float));
                break;
        }
    }

    // Accessors
    uint32_t GetInSampleRate() const { return m_inSampleRate; }
    uint32_t GetInChannels() const { return m_inChannels; }
    uint32_t GetOutSampleRate() const { return m_outSampleRate; }
    uint32_t GetOutChannels() const { return m_outChannels; }
    SampleFormatType GetOutFormat() const { return m_outFormat; }
    double GetResamplePhase() const { return m_phase; }
    bool HasHistory() const { return m_hasHistory; }

private:
    uint32_t m_inSampleRate = 0;
    uint32_t m_inChannels = 0;
    uint32_t m_outSampleRate = 0;
    uint32_t m_outChannels = 0;
    SampleFormatType m_outFormat = SampleFormatType::Unknown;
    size_t m_maxFrames = 8192;

    double m_ratioMultiplier = 1.0;
    double m_phase = 0.0;
    bool m_hasHistory = false;
    std::vector<float> m_historyFrame;

    std::vector<float> m_resampleStaging;
    std::vector<float> m_channelStaging;
};

} // namespace speakerflow
