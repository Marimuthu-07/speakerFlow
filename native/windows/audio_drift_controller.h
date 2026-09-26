#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

#include "audio_drift_estimator.h"

namespace speakerflow {

/**
 * @brief Configuration parameters for AudioDriftController.
 */
struct DriftControllerConfig {
    // Maximum allowable correction in parts per million (default +/- 2000.0 ppm = +/- 0.0020 ratio offset).
    // Bounds the resample ratio multiplier to [1.0 - maxCorrectionPpm/1e6, 1.0 + maxCorrectionPpm/1e6].
    double maxCorrectionPpm = 2000.0;

    // Proportional feedback gain for occupancy error recovery (dimensionless ratio offset per unit fractional error).
    // A value of 0.003 means a 10% occupancy deficit adds -300 ppm correction to gently refill the buffer.
    double kpOccupancy = 0.003;

    // Time constant for low-pass filtering instantaneous occupancy error (seconds).
    // Rejects high-frequency packet arrival jitter without chasing individual packet bursts.
    double occupancyFilterTauSec = 2.0;

    // Time constant for low-pass filtering the estimated clock drift (seconds).
    // Rejects linear regression slope ripple caused by packet jitter.
    double driftFilterTauSec = 2.85;

    // Occupancy error deadband in frames.
    // Fluctuations within +/- deadbandFrames produce zero feedback correction.
    double deadbandFrames = 16.0;

    // Maximum rate of change of the resample ratio multiplier per second (slew rate limiting).
    // 0.000300 = 300 ppm per second max slew rate, ensuring pitch changes are inaudible and smooth.
    double maxRatioSlewRatePerSec = 0.000300;

    // Weight of feed-forward clock drift cancellation (1.0 = direct cancellation of estimated drift).
    double feedforwardGain = 1.0;
};

/**
 * @brief Comprehensive status snapshot of AudioDriftController for telemetry and unit tests.
 */
struct DriftControllerStatus {
    double resampleRatioMultiplier = 1.0;  // Currently applied resample ratio multiplier
    double targetMultiplier = 1.0;         // Un-slew-limited target multiplier
    double trueDriftPpm = 0.0;             // Reconstructed true hardware drift (measured + applied correction)
    double filteredDriftPpm = 0.0;         // Low-pass filtered drift in ppm
    double feedforwardCorrection = 0.0;    // u_ff (dimensionless ratio delta)
    double feedbackCorrection = 0.0;       // u_fb (dimensionless ratio delta)
    double filteredOccupancyError = 0.0;   // Low-pass filtered occupancy error in frames
    double effectiveOccupancyError = 0.0;  // Deadband-adjusted occupancy error in frames
    double maxCorrectionPpm = 2000.0;      // Active maximum correction limit
    double minRatioBound = 0.9980;         // Lower ratio clamp bound
    double maxRatioBound = 1.0020;         // Upper ratio clamp bound
    bool isClamped = false;                // True if target multiplier reached clamp bounds
    bool isReset = true;                   // True if controller is in neutral/reset state
    bool driftCorrectionEnabled = true;    // True if real-time drift correction is enabled
};

/**
 * @brief Real-time safe, deterministic drift correction controller for WASAPI render branches.
 *
 * SIGN CONVENTION (Phase 2E):
 * -------------------------------------------------------------------------
 * positive drift (+ppm):
 *   The render branch consumes audio FASTER than capture reference.
 *   Ring buffer occupancy decreases.
 *   Controller action: resample ratio multiplier MUST be < 1.0
 *   (each output frame consumes fewer input frames, slowing down ring consumption).
 *
 * negative drift (-ppm):
 *   The render branch consumes audio SLOWER than capture reference.
 *   Ring buffer occupancy increases.
 *   Controller action: resample ratio multiplier MUST be > 1.0
 *   (each output frame consumes more input frames, speeding up ring consumption).
 *
 * zero drift (0 ppm):
 *   Render consumption matches capture rate.
 *   Controller action: resample ratio multiplier MUST be == 1.0.
 *
 * REAL-TIME REQUIREMENTS:
 * -------------------------------------------------------------------------
 * - Zero heap allocations during operation.
 * - Zero file I/O, zero Node/V8 calls, zero WASAPI calls.
 * - Completely deterministic; no external random numbers or wall-clock dependencies in calculation.
 * - Strictly guarded against NaN, Infinity, and division by zero.
 * - Output ratio multiplier is always clamped to configured bounds [1.0 - maxDelta, 1.0 + maxDelta].
 */
class AudioDriftController {
public:
    explicit AudioDriftController(const DriftControllerConfig& config = {})
        : m_config(config) {
        Reset();
    }

    ~AudioDriftController() = default;

    AudioDriftController(const AudioDriftController&) = delete;
    AudioDriftController& operator=(const AudioDriftController&) = delete;

    /**
     * @brief Resets all controller state to a clean neutral state (ratio multiplier = 1.0).
     */
    void Reset() {
        m_currentRatio = 1.0;
        m_targetRatio = 1.0;
        m_filteredOccupancyError = 0.0;
        m_filteredDriftPpm = 0.0;
        m_lastElapsedSeconds = 0.0;
        m_hasLastElapsed = false;
        m_isReset = true;

        double maxDelta = std::abs(m_config.maxCorrectionPpm) / 1000000.0;
        m_status = DriftControllerStatus{};
        m_status.resampleRatioMultiplier = 1.0;
        m_status.targetMultiplier = 1.0;
        m_status.maxCorrectionPpm = m_config.maxCorrectionPpm;
        m_status.minRatioBound = 1.0 - maxDelta;
        m_status.maxRatioBound = 1.0 + maxDelta;
        m_status.isReset = true;
    }

    /**
     * @brief Updates the controller state from a DriftTelemetry snapshot.
     * @param telemetry Telemetry snapshot from AudioDriftEstimator.
     * @param dtSeconds Optional explicit time step in seconds (computed from elapsedSeconds if <= 0).
     * @return Bounded resample ratio multiplier to apply to AudioFormatPipeline.
     */
    double Update(const DriftTelemetry& telemetry, double dtSeconds = -1.0) {
        if (telemetry.isReset) {
            Reset();
            return 1.0;
        }

        double dt = dtSeconds;
        if (dt <= 0.0) {
            if (m_hasLastElapsed) {
                dt = telemetry.elapsedSeconds - m_lastElapsedSeconds;
                if (dt <= 0.0 || dt > 1.0) {
                    dt = 0.010; // Fallback to nominal 10ms
                }
            } else {
                dt = 0.010;
            }
        }
        m_lastElapsedSeconds = telemetry.elapsedSeconds;
        m_hasLastElapsed = true;

        size_t targetOccupancy = telemetry.targetOccupancy > 0 ? telemetry.targetOccupancy : 2400;
        return Update(telemetry.driftPpm,
                      telemetry.occupancyError,
                      targetOccupancy,
                      telemetry.isStable,
                      telemetry.confidence,
                      dt);
    }

    /**
     * @brief Updates the controller state from discrete parameter values.
     * @param driftPpm Estimated clock drift in ppm (from linear regression slope).
     * @param occupancyError Current ring buffer occupancy error (current - target) in frames.
     * @param targetOccupancy Target ring buffer occupancy in frames.
     * @param isStable True if drift estimator is stable.
     * @param confidence Confidence score in [0.0, 1.0].
     * @param dt Elapsed time step in seconds since last update.
     * @return Bounded resample ratio multiplier to apply to AudioFormatPipeline.
     */
    double Update(double driftPpm,
                  int64_t occupancyError,
                  size_t targetOccupancy,
                  bool isStable,
                  double confidence,
                  double dt) {
        // Defensive input sanitization
        if (std::isnan(driftPpm) || std::isinf(driftPpm)) driftPpm = 0.0;
        if (std::isnan(confidence) || std::isinf(confidence)) confidence = 0.0;
        confidence = std::clamp(confidence, 0.0, 1.0);

        if (std::isnan(dt) || std::isinf(dt) || dt <= 0.0) {
            dt = 0.010;
        }

        if (targetOccupancy == 0) {
            targetOccupancy = 2400;
        }

        // 1. Observer: Reconstruct the true underlying clock drift.
        // Because the resampler multiplier m_currentRatio actively slows or speeds consumption,
        // the ring buffer occupancy slope reflects residual drift:
        //   driftPpm = trueHardwareDriftPpm - appliedCorrectionPpm
        // where appliedCorrectionPpm = (1.0 - m_currentRatio) * 1e6.
        // Therefore:
        //   trueHardwareDriftPpm = driftPpm + (1.0 - m_currentRatio) * 1e6.
        double appliedCorrectionPpm = (1.0 - m_currentRatio) * 1000000.0;
        double rawTrueDriftPpm = isStable ? (driftPpm + appliedCorrectionPpm) : 0.0;
        if (std::isnan(rawTrueDriftPpm) || std::isinf(rawTrueDriftPpm)) {
            rawTrueDriftPpm = 0.0;
        }

        // 2. Filter the reconstructed drift to eliminate packet jitter ripple
        if (m_isReset) {
            m_filteredDriftPpm = rawTrueDriftPpm;
            m_filteredOccupancyError = static_cast<double>(occupancyError);
            m_currentRatio = 1.0;
            m_targetRatio = 1.0;
            m_isReset = false;
        } else {
            double tauDrift = (m_config.driftFilterTauSec > 0.01) ? m_config.driftFilterTauSec : 2.5;
            double alphaDrift = dt / (tauDrift + dt);
            alphaDrift = std::clamp(alphaDrift, 0.0, 1.0);
            m_filteredDriftPpm += alphaDrift * (rawTrueDriftPpm - m_filteredDriftPpm);

            double tauOcc = (m_config.occupancyFilterTauSec > 0.01) ? m_config.occupancyFilterTauSec : 2.0;
            double alphaOcc = dt / (tauOcc + dt);
            alphaOcc = std::clamp(alphaOcc, 0.0, 1.0);
            m_filteredOccupancyError += alphaOcc * (static_cast<double>(occupancyError) - m_filteredOccupancyError);
        }

        if (std::isnan(m_filteredDriftPpm) || std::isinf(m_filteredDriftPpm)) {
            m_filteredDriftPpm = 0.0;
        }
        if (std::isnan(m_filteredOccupancyError) || std::isinf(m_filteredOccupancyError)) {
            m_filteredOccupancyError = 0.0;
        }

        // 3. Deadband on filtered occupancy error
        double deadband = (m_config.deadbandFrames >= 0.0) ? m_config.deadbandFrames : 0.0;
        double effectiveError = 0.0;
        if (m_filteredOccupancyError > deadband) {
            effectiveError = m_filteredOccupancyError - deadband;
        } else if (m_filteredOccupancyError < -deadband) {
            effectiveError = m_filteredOccupancyError + deadband;
        }

        // 4. Feed-forward clock drift cancellation term
        // Sign convention:
        // positive drift (+ppm) -> u_ff < 0 -> ratio < 1.0
        // negative drift (-ppm) -> u_ff > 0 -> ratio > 1.0
        // zero drift (0 ppm)    -> u_ff = 0 -> ratio = 1.0
        double u_ff = 0.0;
        if (isStable && confidence > 0.0) {
            u_ff = - (m_filteredDriftPpm / 1000000.0) * m_config.feedforwardGain * confidence;
        }

        // 5. Feedback occupancy stabilization term
        // occupancyError = current - target
        // If current < target (deficit, error < 0): u_fb must be < 0 to consume slower and refill buffer.
        // If current > target (surplus, error > 0): u_fb must be > 0 to consume faster and drain surplus.
        double u_fb = 0.0;
        if (isStable && confidence > 0.1) {
            double normError = effectiveError / static_cast<double>(targetOccupancy);
            u_fb = normError * m_config.kpOccupancy * confidence;
        }

        // Total target ratio multiplier
        double rawTarget = 1.0 + u_ff + u_fb;
        if (std::isnan(rawTarget) || std::isinf(rawTarget)) {
            rawTarget = 1.0;
        }

        // 6. Configurable safety clamp bounds
        double maxDelta = std::abs(m_config.maxCorrectionPpm) / 1000000.0;
        double minBound = 1.0 - maxDelta;
        double maxBound = 1.0 + maxDelta;

        bool isClamped = false;
        double clampedTarget = rawTarget;
        if (clampedTarget < minBound) {
            clampedTarget = minBound;
            isClamped = true;
        } else if (clampedTarget > maxBound) {
            clampedTarget = maxBound;
            isClamped = true;
        }
        m_targetRatio = clampedTarget;

        // 7. Slew rate limiter to ensure smooth pitch transitions
        if (m_config.maxRatioSlewRatePerSec > 0.0 && dt > 0.0) {
            double maxStep = m_config.maxRatioSlewRatePerSec * dt;
            double step = clampedTarget - m_currentRatio;
            if (step > maxStep) {
                m_currentRatio += maxStep;
            } else if (step < -maxStep) {
                m_currentRatio -= maxStep;
            } else {
                m_currentRatio = clampedTarget;
            }
        } else {
            m_currentRatio = clampedTarget;
        }

        // Final safety bounds clamping and NaN guard
        m_currentRatio = std::clamp(m_currentRatio, minBound, maxBound);
        if (std::isnan(m_currentRatio) || std::isinf(m_currentRatio)) {
            m_currentRatio = 1.0;
        }

        // Snapshot telemetry
        m_status.resampleRatioMultiplier = m_currentRatio;
        m_status.targetMultiplier = m_targetRatio;
        m_status.trueDriftPpm = rawTrueDriftPpm;
        m_status.filteredDriftPpm = m_filteredDriftPpm;
        m_status.feedforwardCorrection = u_ff;
        m_status.feedbackCorrection = u_fb;
        m_status.filteredOccupancyError = m_filteredOccupancyError;
        m_status.effectiveOccupancyError = effectiveError;
        m_status.maxCorrectionPpm = m_config.maxCorrectionPpm;
        m_status.minRatioBound = minBound;
        m_status.maxRatioBound = maxBound;
        m_status.isClamped = isClamped;
        m_status.isReset = false;

        return m_currentRatio;
    }

    double GetResampleRatioMultiplier() const {
        return m_currentRatio;
    }

    DriftControllerStatus GetStatus() const {
        return m_status;
    }

    void SetConfig(const DriftControllerConfig& config) {
        m_config = config;
        double maxDelta = std::abs(m_config.maxCorrectionPpm) / 1000000.0;
        m_status.maxCorrectionPpm = m_config.maxCorrectionPpm;
        m_status.minRatioBound = 1.0 - maxDelta;
        m_status.maxRatioBound = 1.0 + maxDelta;
        m_currentRatio = std::clamp(m_currentRatio, m_status.minRatioBound, m_status.maxRatioBound);
        m_status.resampleRatioMultiplier = m_currentRatio;
    }

    const DriftControllerConfig& GetConfig() const {
        return m_config;
    }

    void SetMaxCorrectionPpm(double maxPpm) {
        m_config.maxCorrectionPpm = (maxPpm > 0.0) ? maxPpm : 0.0;
        double maxDelta = m_config.maxCorrectionPpm / 1000000.0;
        m_status.maxCorrectionPpm = m_config.maxCorrectionPpm;
        m_status.minRatioBound = 1.0 - maxDelta;
        m_status.maxRatioBound = 1.0 + maxDelta;
        m_currentRatio = std::clamp(m_currentRatio, m_status.minRatioBound, m_status.maxRatioBound);
        m_status.resampleRatioMultiplier = m_currentRatio;
    }

    double GetMaxCorrectionPpm() const {
        return m_config.maxCorrectionPpm;
    }

    bool IsReset() const {
        return m_isReset;
    }

private:
    DriftControllerConfig m_config;

    double m_currentRatio = 1.0;
    double m_targetRatio = 1.0;
    double m_filteredDriftPpm = 0.0;
    double m_filteredOccupancyError = 0.0;

    double m_lastElapsedSeconds = 0.0;
    bool m_hasLastElapsed = false;
    bool m_isReset = true;

    DriftControllerStatus m_status{};
};

} // namespace speakerflow
