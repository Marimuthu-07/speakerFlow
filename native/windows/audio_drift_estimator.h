#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>

namespace speakerflow {

/**
 * @brief Telemetry snapshot from AudioDriftEstimator.
 */
struct DriftTelemetry {
    size_t currentOccupancy = 0;       // Current ring buffer occupancy in frames
    size_t targetOccupancy = 0;        // Target ring buffer occupancy in frames (e.g. 50ms)
    int64_t occupancyError = 0;        // currentOccupancy - targetOccupancy (positive = surplus, negative = deficit)
    double occupancySlope = 0.0;       // Rate of change of occupancy in frames/sec (dO/dt)
    uint64_t renderedFrames = 0;       // Total frames rendered by branch
    double elapsedSeconds = 0.0;       // Total time elapsed since estimator start/reset
    double driftPpm = 0.0;             // Estimated clock drift in parts per million
    bool isStable = false;             // True if estimator has gathered sufficient duration/samples
    double confidence = 0.0;           // Stability score in [0.0, 1.0]
    bool isReset = true;               // True if estimator is in clean neutral/reset state
};

/**
 * @brief Real-time safe, deterministic clock drift estimator for WASAPI render branches.
 *
 * SIGN CONVENTION (Phase 2E):
 * -------------------------------------------------------------------------
 * Reference timeline: WASAPI loopback capture stream.
 *
 * positive drift (+ppm):
 *   The render branch is effectively consuming audio FASTER than
 *   the capture reference timeline (ring buffer occupancy is decreasing).
 *
 * negative drift (-ppm):
 *   The render branch is effectively consuming audio SLOWER than
 *   the capture reference timeline (ring buffer occupancy is increasing).
 *
 * zero drift (0 ppm):
 *   The render branch consumption rate matches the capture reference rate.
 *
 * Formula:
 *   driftPpm = - (occupancySlope / sampleRate) * 1,000,000.0
 *
 * Occupancy Error convention:
 *   occupancyError = currentOccupancy - targetOccupancy
 *   - positive: buffer occupancy above nominal target (surplus)
 *   - negative: buffer occupancy below nominal target (deficit/starvation risk)
 *   - zero: buffer occupancy matches nominal target exactly
 *
 * REAL-TIME REQUIREMENTS:
 * -------------------------------------------------------------------------
 * - Zero heap allocations during operation (preallocated fixed-size circular buffer).
 * - Zero file I/O, zero Node/V8 calls, zero blocking locks.
 * - Numerically stable linear regression using centered relative timestamps.
 */
class AudioDriftEstimator {
public:
    static constexpr size_t MAX_HISTORY_SAMPLES = 512;

    /**
     * @brief Constructs an estimator configured for the specified sample rate and target occupancy.
     * @param sampleRate Active reference sample rate (e.g. 48000 Hz).
     * @param targetOccupancy Target occupancy in frames (defaults to 50ms = sampleRate * 0.050).
     * @param windowDurationSec Duration of the sliding linear regression window in seconds (default 3.0s).
     * @param minStableDurationSec Minimum observation duration required before isStable can be true (default 0.5s).
     */
    explicit AudioDriftEstimator(uint32_t sampleRate = 48000,
                                 size_t targetOccupancy = 0,
                                 double windowDurationSec = 3.0,
                                 double minStableDurationSec = 0.5)
        : m_sampleRate(sampleRate ? sampleRate : 48000),
          m_targetOccupancy(targetOccupancy ? targetOccupancy : static_cast<size_t>(m_sampleRate * 0.050)),
          m_windowDurationSec(windowDurationSec > 0.1 ? windowDurationSec : 3.0),
          m_minStableDurationSec(minStableDurationSec > 0.05 ? minStableDurationSec : 0.5) {
        Reset();
    }

    ~AudioDriftEstimator() = default;

    AudioDriftEstimator(const AudioDriftEstimator&) = delete;
    AudioDriftEstimator& operator=(const AudioDriftEstimator&) = delete;

    /**
     * @brief Resets all estimator state to a clean, neutral zero state.
     */
    void Reset() {
        m_head = 0;
        m_count = 0;
        m_startTime = 0.0;
        m_lastTime = 0.0;
        m_hasStartTime = false;
        m_currentOccupancy = 0;
        m_renderedFrames = 0;
        m_elapsedSeconds = 0.0;
        m_occupancyError = 0;
        m_occupancySlope = 0.0;
        m_driftPpm = 0.0;
        m_isStable = false;
        m_confidence = 0.0;
        m_isReset = true;
        m_steadyStart = std::chrono::steady_clock::time_point{};
    }

    /**
     * @brief Ingests an occupancy observation with an explicit timestamp (for deterministic unit tests).
     * @param timeSeconds Time in seconds (relative or absolute, must be monotonically non-decreasing).
     * @param currentOccupancy Current ring buffer occupancy in frames.
     * @param renderedFrames Total frames rendered by the branch so far.
     */
    void Update(double timeSeconds, size_t currentOccupancy, uint64_t renderedFrames = 0) {
        if (m_isReset || !m_hasStartTime) {
            m_startTime = timeSeconds;
            m_hasStartTime = true;
            m_isReset = false;
        }

        double elapsed = timeSeconds - m_startTime;
        if (elapsed < 0.0) elapsed = 0.0;
        m_elapsedSeconds = elapsed;
        m_currentOccupancy = currentOccupancy;
        m_renderedFrames = renderedFrames;
        m_occupancyError = static_cast<int64_t>(currentOccupancy) - static_cast<int64_t>(m_targetOccupancy);

        // Handle duplicate or backwards timestamp gracefully
        if (m_count > 0 && timeSeconds <= m_lastTime) {
            size_t lastIdx = (m_head + m_count - 1) % MAX_HISTORY_SAMPLES;
            m_samples[lastIdx].occupancy = static_cast<double>(currentOccupancy);
        } else {
            // Push new sample into circular buffer
            if (m_count < MAX_HISTORY_SAMPLES) {
                size_t insertIdx = (m_head + m_count) % MAX_HISTORY_SAMPLES;
                m_samples[insertIdx] = { timeSeconds, static_cast<double>(currentOccupancy) };
                m_count++;
            } else {
                m_samples[m_head] = { timeSeconds, static_cast<double>(currentOccupancy) };
                m_head = (m_head + 1) % MAX_HISTORY_SAMPLES;
            }
            m_lastTime = timeSeconds;
        }

        // Prune samples older than the sliding window duration
        while (m_count > 1) {
            double age = timeSeconds - m_samples[m_head].t;
            if (age > m_windowDurationSec) {
                m_head = (m_head + 1) % MAX_HISTORY_SAMPLES;
                m_count--;
            } else {
                break;
            }
        }

        // Calculate linear regression over window
        CalculateRegression();
    }

    /**
     * @brief Ingests an occupancy observation using the high-resolution steady clock.
     * @param currentOccupancy Current ring buffer occupancy in frames.
     * @param renderedFrames Total frames rendered by the branch so far.
     */
    void Update(size_t currentOccupancy, uint64_t renderedFrames = 0) {
        auto now = std::chrono::steady_clock::now();
        if (m_isReset || m_steadyStart.time_since_epoch().count() == 0) {
            m_steadyStart = now;
        }
        std::chrono::duration<double> diff = now - m_steadyStart;
        Update(diff.count(), currentOccupancy, renderedFrames);
    }

    void SetSampleRate(uint32_t sampleRate) {
        if (sampleRate > 0) {
            m_sampleRate = sampleRate;
        }
    }

    void SetTargetOccupancy(size_t targetFrames) {
        m_targetOccupancy = targetFrames;
        m_occupancyError = static_cast<int64_t>(m_currentOccupancy) - static_cast<int64_t>(m_targetOccupancy);
    }

    void SetWindowDuration(double windowSec) {
        if (windowSec > 0.1) {
            m_windowDurationSec = windowSec;
        }
    }

    size_t GetCurrentOccupancy() const { return m_currentOccupancy; }
    size_t GetTargetOccupancy() const { return m_targetOccupancy; }
    int64_t GetOccupancyError() const { return m_occupancyError; }
    double GetOccupancySlope() const { return m_occupancySlope; }
    uint64_t GetRenderedFrames() const { return m_renderedFrames; }
    double GetElapsedSeconds() const { return m_elapsedSeconds; }
    double GetDriftPpm() const { return m_driftPpm; }
    bool IsStable() const { return m_isStable; }
    double GetConfidence() const { return m_confidence; }
    bool IsReset() const { return m_isReset; }

    DriftTelemetry GetTelemetry() const {
        DriftTelemetry t;
        t.currentOccupancy = m_currentOccupancy;
        t.targetOccupancy = m_targetOccupancy;
        t.occupancyError = m_occupancyError;
        t.occupancySlope = m_occupancySlope;
        t.renderedFrames = m_renderedFrames;
        t.elapsedSeconds = m_elapsedSeconds;
        t.driftPpm = m_driftPpm;
        t.isStable = m_isStable;
        t.confidence = m_confidence;
        t.isReset = m_isReset;
        return t;
    }

private:
    struct SamplePoint {
        double t = 0.0;
        double occupancy = 0.0;
    };

    void CalculateRegression() {
        if (m_count < 2) {
            m_occupancySlope = 0.0;
            m_driftPpm = 0.0;
            m_isStable = false;
            m_confidence = 0.0;
            return;
        }

        double t0 = m_samples[m_head].t;
        size_t newestIdx = (m_head + m_count - 1) % MAX_HISTORY_SAMPLES;
        double tSpan = m_samples[newestIdx].t - t0;

        if (tSpan < 0.01) {
            m_occupancySlope = 0.0;
            m_driftPpm = 0.0;
            m_isStable = false;
            m_confidence = 0.0;
            return;
        }

        double sumTau = 0.0;
        double sumTau2 = 0.0;
        double sumO = 0.0;
        double sumTauO = 0.0;

        for (size_t i = 0; i < m_count; ++i) {
            size_t idx = (m_head + i) % MAX_HISTORY_SAMPLES;
            double tau = m_samples[idx].t - t0;
            double occ = m_samples[idx].occupancy;
            sumTau += tau;
            sumTau2 += tau * tau;
            sumO += occ;
            sumTauO += tau * occ;
        }

        double n = static_cast<double>(m_count);
        double meanTau = sumTau / n;
        double meanO = sumO / n;
        double varTau = sumTau2 - n * meanTau * meanTau;
        double covTauO = sumTauO - n * meanTau * meanO;

        if (varTau > 1e-12) {
            m_occupancySlope = covTauO / varTau; // frames/sec
        } else {
            m_occupancySlope = 0.0;
        }

        // Stability and confidence metrics
        double durationFactor = std::clamp(tSpan / m_windowDurationSec, 0.0, 1.0);
        double sampleFactor = std::clamp(n / 10.0, 0.0, 1.0);
        m_confidence = durationFactor * sampleFactor;
        m_isStable = (tSpan >= m_minStableDurationSec) && (m_count >= 10);

        // Apply sign convention: positive drift = consuming faster than capture reference.
        // Report 0.0 ppm until estimator has gathered sufficient duration/samples to be stable.
        if (m_isStable && m_sampleRate > 0) {
            m_driftPpm = - (m_occupancySlope / static_cast<double>(m_sampleRate)) * 1000000.0;
        } else {
            m_driftPpm = 0.0;
        }
    }

    uint32_t m_sampleRate;
    size_t m_targetOccupancy;
    double m_windowDurationSec;
    double m_minStableDurationSec;

    std::array<SamplePoint, MAX_HISTORY_SAMPLES> m_samples{};
    size_t m_head = 0;
    size_t m_count = 0;

    double m_startTime = 0.0;
    double m_lastTime = 0.0;
    bool m_hasStartTime = false;

    size_t m_currentOccupancy = 0;
    uint64_t m_renderedFrames = 0;
    double m_elapsedSeconds = 0.0;
    int64_t m_occupancyError = 0;
    double m_occupancySlope = 0.0;
    double m_driftPpm = 0.0;
    bool m_isStable = false;
    double m_confidence = 0.0;
    bool m_isReset = true;

    std::chrono::steady_clock::time_point m_steadyStart{};
};

} // namespace speakerflow
