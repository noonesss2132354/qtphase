// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphaserenderpolicy_p.h"

#include <QtCore/qglobal.h>

#include <algorithm>
#include <cstdio>

QT_BEGIN_NAMESPACE

namespace {

inline bool checkForcedAffinity(const QPhaseSurfaceCaps &caps, QPhasePolicyDecision &out) noexcept
{
    if (caps.forcedAffinity == QPhaseRendererAffinity::CpuOnly) {
        out = { QPhaseBackendTarget::Cpu, false, false, true, "forced-cpu" };
        return true;
    }
    if (caps.forcedAffinity == QPhaseRendererAffinity::GpuOnly) {
        if (caps.vulkanFailed) {
            out = { QPhaseBackendTarget::Cpu, false, false, true, "forced-gpu-failed" };
            return true;
        }
        if (caps.forcedBlocking || caps.vulkanWarmReady) {
            out = { QPhaseBackendTarget::Gpu, false, false, true, "forced-gpu" };
            return true;
        }
        out = { QPhaseBackendTarget::Cpu, false, false, false, "forced-gpu-warmup" };
        return true;
    }
    if (caps.hasGpuOnlyViews && !caps.vulkanFailed && caps.vulkanWarmReady) {
        out = { QPhaseBackendTarget::Gpu, false, false, false, "gpu-only-view" };
        return true;
    }
    if (caps.vulkanFailed) {
        out = { QPhaseBackendTarget::Cpu, false, false, true, "vulkan-failed" };
        return true;
    }
    return false;
}

inline bool isSmallTransientSurface(const QPhaseSurfaceCaps &caps, const QPhaseFrameRequest &req) noexcept
{
    if (caps.isPopupOrTooltip)
        return true;
    const int area = req.logicalSize.width() * req.logicalSize.height();
    return req.frameIndex == 0 && area > 0 && area <= (320 * 240);
}

// ============================================================================
// 1. async-race (Default):
//    Zero-stall empirical CPU-vs-GPU race per window.
//    - Small popups/tooltips stay on CPU wl_shm (zero Vulkan ring churn).
//    - While Vulkan warms up: CPU wl_shm renders immediately.
//    - Once Vulkan is warm: submits Frame 1 on GPU asynchronously (0 ms stall)
//      and runs a 1-frame background CPU calibration sample on the exact same
//      scene snapshot if no CPU boot frame was timed yet.
//    - Once both empirical GPU timestamp (submit + hw) and CPU time arrive,
//      locks whichever pipeline genuinely won!
// ============================================================================
class AsyncRacePolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "async-race"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (m_locked)
            return { m_lockedTarget, false, false, true, m_lockedTarget == QPhaseBackendTarget::Gpu ? "locked-gpu" : "locked-cpu" };

        if (isSmallTransientSurface(caps, req) && !caps.hasGpuOnlyViews) {
            m_locked = true;
            m_lockedTarget = QPhaseBackendTarget::Cpu;
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };
        }

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        // Software Vulkan (e.g. llvmpipe): keep 1-frame sync probe if we haven't timed GPU yet
        if (!caps.isHardwareGpu && !m_hasGpuSample) {
            const bool needCpuCalib = !m_hasCpuSample && !m_cpuCalibRequested;
            if (needCpuCalib)
                m_cpuCalibRequested = true;
            return { QPhaseBackendTarget::Gpu, true, needCpuCalib, false, "sw-vk-sync-probe" };
        }

        // Wait until we have BOTH empirical measurements (or 6 GPU frames timeout if timestamps unsupported)
        if (m_hasCpuSample && (m_hasGpuSample || m_gpuFramesTested >= 6)) {
            const qint64 gpuCostUs = m_hasGpuSample
                    ? (m_gpuSubmitUs + m_gpuHwUs)
                    : (caps.isHardwareGpu ? (m_cpuSampleUs / 2) : (m_cpuSampleUs * 2));
            // Give GPU a modest margin on large/resizable surfaces because CPU cost scales
            // linearly with pixel count during resizes, while GPU stays nearly flat.
            const bool gpuWins = (gpuCostUs <= m_cpuSampleUs + 1500);
            m_locked = true;
            m_lockedTarget = gpuWins ? QPhaseBackendTarget::Gpu : QPhaseBackendTarget::Cpu;
            return { m_lockedTarget, false, false, true,
                     gpuWins ? "async-race-gpu-won" : "async-race-cpu-won" };
        }

        const bool needCpuCalib = !m_hasCpuSample && !m_cpuCalibRequested;
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false, "async-race-calibrating" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (!outcome.succeeded) {
            if (outcome.backend == QPhaseBackendTarget::Gpu) {
                m_locked = true;
                m_lockedTarget = QPhaseBackendTarget::Cpu;
            }
            return;
        }
        if (outcome.backend == QPhaseBackendTarget::Cpu) {
            if (!m_hasCpuSample || outcome.cpuWorkUs > 0) {
                m_cpuSampleUs = outcome.cpuWorkUs;
                m_hasCpuSample = true;
            }
        } else {
            ++m_gpuFramesTested;
            if (outcome.gpuHwUs > 0) {
                // Take the minimum of the first 1-2 completed GPU frames so a one-time
                // shader/VMA allocation doesn't permanently misclassify a fast GPU.
                const qint64 totalUs = outcome.cpuWorkUs + outcome.gpuHwUs;
                if (!m_hasGpuSample || totalUs < (m_gpuSubmitUs + m_gpuHwUs)) {
                    m_gpuSubmitUs = outcome.cpuWorkUs;
                    m_gpuHwUs = outcome.gpuHwUs;
                }
                m_hasGpuSample = true;
            }
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_cpuSampleUs = cpuSampleUs;
            m_hasCpuSample = true;
        }
    }

    void reset() override
    {
        m_locked = false;
        m_lockedTarget = QPhaseBackendTarget::Cpu;
        m_hasCpuSample = false;
        m_cpuCalibRequested = false;
        m_hasGpuSample = false;
        m_cpuSampleUs = 0;
        m_gpuSubmitUs = 0;
        m_gpuHwUs = 0;
        m_gpuFramesTested = 0;
    }

private:
    bool m_locked = false;
    QPhaseBackendTarget m_lockedTarget = QPhaseBackendTarget::Cpu;
    bool m_hasCpuSample = false;
    bool m_cpuCalibRequested = false;
    bool m_hasGpuSample = false;
    qint64 m_cpuSampleUs = 0;
    qint64 m_gpuSubmitUs = 0;
    qint64 m_gpuHwUs = 0;
    int m_gpuFramesTested = 0;
};

// ============================================================================
// 2. dynamic-hybrid:
//    Continuous hysteresis-gated dual-pipeline scheduler.
//    - Popups/tooltips stay on CPU wl_shm.
//    - Resizes, animations, and large viewport redraws use Vulkan GPU (unless
//      GPU is empirically losing to CPU under contention or heavy fallbacks).
//    - Sustained tiny localized edits (e.g. blinking cursor, single-line typing)
//      OR sustained GPU slowness demote smoothly to CPU wl_shm, keeping the GPU
//      asleep and avoiding full-screen GPU redraws for 2% dirty regions.
//    - Immediately promotes back to GPU on resize or large-area updates.
// ============================================================================
class DynamicHybridPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "dynamic-hybrid"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        // Software Vulkan: if CPU already beat GPU, stay CPU
        if (!caps.isHardwareGpu && m_hasCpuEwma && m_hasGpuEwma && m_ewmaGpuTotalUs > m_ewmaCpuUs)
            return { QPhaseBackendTarget::Cpu, false, false, false, "sw-vk-cpu-faster" };

        // Kick off 1 background CPU sample on first GPU frame so we have a real CPU baseline
        const bool needCpuCalib = !m_hasCpuEwma && !m_cpuCalibRequested && (m_active == QPhaseBackendTarget::Gpu || req.frameIndex == 0);
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        const bool heavyFrame = req.isResize || req.sizeChanged || req.hasActiveAnimations
                || req.dirtyAreaRatio >= 0.25f;
        const bool tinyEdit = !heavyFrame && req.dirtyAreaRatio > 0.0f && req.dirtyAreaRatio < 0.15f
                && req.pendingUploadBytes < (512 * 1024);

        // Track consecutive streaks for hysteresis
        if (tinyEdit)
            ++m_tinyEditStreak;
        else
            m_tinyEditStreak = 0;

        if (m_active == QPhaseBackendTarget::Gpu) {
            if (m_framesSinceSwitch >= kMinFramesBeforeDemote) {
                if (m_tinyEditStreak >= kTinyEditHysteresis) {
                    m_active = QPhaseBackendTarget::Cpu;
                    m_framesSinceSwitch = 0;
                    m_gpuLossStreak = 0;
                    return { QPhaseBackendTarget::Cpu, false, false, false, "demote-tiny-damage" };
                }
                if (m_gpuLossStreak >= kGpuLossHysteresis) {
                    m_active = QPhaseBackendTarget::Cpu;
                    m_framesSinceSwitch = 0;
                    m_gpuLossStreak = 0;
                    return { QPhaseBackendTarget::Cpu, false, false, false, "demote-gpu-slower" };
                }
            }
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     heavyFrame ? "gpu-heavy" : "gpu-steady" };
        } else {
            // Currently on CPU: promote immediately on resize/animation, or on large damage
            // if GPU isn't currently known to be slower than CPU.
            const bool gpuKnownSlower = m_hasCpuEwma && m_hasGpuEwma
                    && (m_ewmaGpuTotalUs > (m_ewmaCpuUs * 13) / 10);
            if (req.isResize || req.sizeChanged || req.hasActiveAnimations) {
                if (!gpuKnownSlower || req.isResize) {
                    m_active = QPhaseBackendTarget::Gpu;
                    m_framesSinceSwitch = 0;
                    m_tinyEditStreak = 0;
                    return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false, "promote-resize-anim" };
                }
            }
            if (req.dirtyAreaRatio >= 0.30f && m_framesSinceSwitch >= kMinFramesBeforePromote && !gpuKnownSlower) {
                m_active = QPhaseBackendTarget::Gpu;
                m_framesSinceSwitch = 0;
                m_tinyEditStreak = 0;
                return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false, "promote-large-damage" };
            }
            return { QPhaseBackendTarget::Cpu, false, false, false, "cpu-steady" };
        }
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        ++m_framesSinceSwitch;
        if (!outcome.succeeded) {
            if (outcome.backend == QPhaseBackendTarget::Gpu) {
                m_active = QPhaseBackendTarget::Cpu;
                m_framesSinceSwitch = 0;
            }
            return;
        }
        if (outcome.backend == QPhaseBackendTarget::Cpu) {
            if (outcome.cpuWorkUs > 0) {
                m_ewmaCpuUs = m_hasCpuEwma ? ((m_ewmaCpuUs * 3 + outcome.cpuWorkUs) / 4) : outcome.cpuWorkUs;
                m_hasCpuEwma = true;
            }
        } else {
            const qint64 totalUs = outcome.cpuWorkUs + outcome.gpuHwUs;
            if (outcome.gpuHwUs > 0) {
                m_ewmaGpuTotalUs = m_hasGpuEwma ? ((m_ewmaGpuTotalUs * 3 + totalUs) / 4) : totalUs;
                m_hasGpuEwma = true;
                if (m_hasCpuEwma && totalUs > (m_ewmaCpuUs * 5) / 4 + 1000)
                    ++m_gpuLossStreak;
                else
                    m_gpuLossStreak = 0;
            }
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_ewmaCpuUs = m_hasCpuEwma ? ((m_ewmaCpuUs * 3 + cpuSampleUs) / 4) : cpuSampleUs;
            m_hasCpuEwma = true;
        }
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_hasCpuEwma = false;
        m_hasGpuEwma = false;
        m_cpuCalibRequested = false;
        m_ewmaCpuUs = 0;
        m_ewmaGpuTotalUs = 0;
        m_tinyEditStreak = 0;
        m_gpuLossStreak = 0;
        m_framesSinceSwitch = 0;
    }

private:
    static constexpr int kTinyEditHysteresis = 4;
    static constexpr int kGpuLossHysteresis = 4;
    static constexpr int kMinFramesBeforeDemote = 4;
    static constexpr int kMinFramesBeforePromote = 2;

    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    bool m_hasCpuEwma = false;
    bool m_hasGpuEwma = false;
    bool m_cpuCalibRequested = false;
    qint64 m_ewmaCpuUs = 0;
    qint64 m_ewmaGpuTotalUs = 0;
    int m_tinyEditStreak = 0;
    int m_gpuLossStreak = 0;
    int m_framesSinceSwitch = 0;
};

// ============================================================================
// 3. sync-race:
//    Original synchronous 1-frame GPU-vs-CPU timing probe (baseline).
// ============================================================================
class SyncRacePolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "sync-race"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        Q_UNUSED(req);
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (m_locked)
            return { m_lockedTarget, false, false, true, m_lockedTarget == QPhaseBackendTarget::Gpu ? "locked-gpu" : "locked-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        if (!m_probedGpu) {
            m_probedGpu = true;
            return { QPhaseBackendTarget::Gpu, true, false, false, "sync-probe" };
        }

        const qint64 cpuRefUs = (m_lastCpuUs > 0) ? m_lastCpuUs : 4000;
        const bool gpuWins = m_probeOk && (m_probeHwUs > 0 && m_probeHwUs < cpuRefUs);
        m_locked = true;
        m_lockedTarget = gpuWins ? QPhaseBackendTarget::Gpu : QPhaseBackendTarget::Cpu;
        return { m_lockedTarget, false, false, true, gpuWins ? "sync-race-gpu-won" : "sync-race-cpu-won" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (outcome.backend == QPhaseBackendTarget::Cpu && outcome.succeeded) {
            m_lastCpuUs = outcome.cpuWorkUs;
        } else if (outcome.backend == QPhaseBackendTarget::Gpu) {
            m_probeOk = outcome.succeeded;
            m_probeHwUs = outcome.gpuHwUs;
        }
    }

    void reset() override
    {
        m_locked = false;
        m_probedGpu = false;
        m_probeOk = false;
        m_lastCpuUs = 0;
        m_probeHwUs = 0;
    }

private:
    bool m_locked = false;
    bool m_probedGpu = false;
    bool m_probeOk = false;
    QPhaseBackendTarget m_lockedTarget = QPhaseBackendTarget::Cpu;
    qint64 m_lastCpuUs = 0;
    qint64 m_probeHwUs = 0;
};

// ============================================================================
// 4. hw-gpu:
//    Control baseline from 64ca68ff (hardware GPUs lock immediately with 0 ms
//    probe; software Vulkan runs a 1-frame sync probe).
// ============================================================================
class HwGpuPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "hw-gpu"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        Q_UNUSED(req);
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (m_locked)
            return { m_lockedTarget, false, false, true, m_lockedTarget == QPhaseBackendTarget::Gpu ? "locked-gpu" : "locked-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        if (caps.isHardwareGpu) {
            m_locked = true;
            m_lockedTarget = QPhaseBackendTarget::Gpu;
            return { QPhaseBackendTarget::Gpu, false, false, true, "hw-gpu-lock" };
        }

        if (!m_probedSw) {
            m_probedSw = true;
            return { QPhaseBackendTarget::Gpu, true, false, false, "sw-vk-sync-probe" };
        }

        const qint64 cpuRefUs = (m_lastCpuUs > 0) ? m_lastCpuUs : 4000;
        const bool gpuWins = m_probeOk && (m_probeHwUs > 0 && m_probeHwUs < cpuRefUs);
        m_locked = true;
        m_lockedTarget = gpuWins ? QPhaseBackendTarget::Gpu : QPhaseBackendTarget::Cpu;
        return { m_lockedTarget, false, false, true, gpuWins ? "sw-vk-gpu-won" : "sw-vk-cpu-won" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (outcome.backend == QPhaseBackendTarget::Cpu && outcome.succeeded) {
            m_lastCpuUs = outcome.cpuWorkUs;
        } else if (outcome.backend == QPhaseBackendTarget::Gpu) {
            m_probeOk = outcome.succeeded;
            m_probeHwUs = outcome.gpuHwUs;
        }
    }

    void reset() override
    {
        m_locked = false;
        m_probedSw = false;
        m_probeOk = false;
        m_lastCpuUs = 0;
        m_probeHwUs = 0;
    }

private:
    bool m_locked = false;
    bool m_probedSw = false;
    bool m_probeOk = false;
    QPhaseBackendTarget m_lockedTarget = QPhaseBackendTarget::Cpu;
    qint64 m_lastCpuUs = 0;
    qint64 m_probeHwUs = 0;
};

// ============================================================================
// 5. cpu-only & 6. gpu-only (Single-backend experimental controls)
// ============================================================================
class CpuOnlyPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "cpu-only"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        Q_UNUSED(caps);
        Q_UNUSED(req);
        return { QPhaseBackendTarget::Cpu, false, false, true, "policy-cpu-only" };
    }

    void recordOutcome(const QPhaseFrameOutcome &) override {}
};

class GpuOnlyPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "gpu-only"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        Q_UNUSED(req);
        if (caps.vulkanFailed)
            return { QPhaseBackendTarget::Cpu, false, false, true, "vulkan-failed" };
        if (!caps.vulkanWarmReady && !caps.forcedBlocking)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };
        return { QPhaseBackendTarget::Gpu, false, false, true, "policy-gpu-only" };
    }

    void recordOutcome(const QPhaseFrameOutcome &) override {}
};

// ============================================================================
// 7. amortized-hybrid:
//    Breakeven-cost hybrid controller. Explicitly amortizes the ~2x wl_shm
//    buffer cold-init cost before demoting GPU -> CPU, and requires 2
//    consecutive heavy frames (or explicit resize/animation) before promoting
//    back CPU -> GPU so isolated compaction frames never cause thrashing.
// ============================================================================
class AmortizedHybridPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "amortized-hybrid"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        const bool needCpuCalib = !m_hasFullCpuSample && !m_cpuCalibRequested
                && (m_active == QPhaseBackendTarget::Gpu || req.frameIndex == 0);
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        const bool isHardGpuTrigger = req.isResize || req.sizeChanged || req.hasActiveAnimations;
        const bool isLargeDamage = isHardGpuTrigger || req.dirtyAreaRatio >= 0.30f;
        const bool isSmallDamage = !isLargeDamage && req.dirtyAreaRatio > 0.0f
                && req.dirtyAreaRatio < 0.16f && req.pendingUploadBytes < (512 * 1024);

        if (isLargeDamage) {
            ++m_largeStreak;
            m_smallStreak = 0;
            m_accumSavingsUs = 0;
        } else if (isSmallDamage) {
            ++m_smallStreak;
            m_largeStreak = 0;
            const qint64 fullCpu = m_hasFullCpuSample ? m_fullCpuUs : 8000;
            const qint64 estPartialCpu = std::clamp<qint64>(
                    qint64(double(fullCpu) * double(req.dirtyAreaRatio) * 2.5), 600, fullCpu);
            const qint64 gpuTot = m_hasGpuEwma ? m_ewmaGpuTotalUs : 3000;
            const qint64 frameSaving = std::max<qint64>(500, gpuTot - estPartialCpu);
            m_accumSavingsUs += frameSaving;
        } else {
            m_largeStreak = 0;
            m_smallStreak = std::max(0, m_smallStreak - 1);
            m_accumSavingsUs = (m_accumSavingsUs * 3) / 4;
        }

        if (m_active == QPhaseBackendTarget::Gpu) {
            const qint64 switchCostUs = (m_hasFullCpuSample ? m_fullCpuUs : 8000) * 2;
            if (m_framesSinceSwitch >= 8) {
                if (m_smallStreak >= 10 && m_accumSavingsUs >= switchCostUs) {
                    m_active = QPhaseBackendTarget::Cpu;
                    m_framesSinceSwitch = 0;
                    m_accumSavingsUs = 0;
                    m_gpuLossStreak = 0;
                    return { QPhaseBackendTarget::Cpu, false, false, false, "amortized-demote-small" };
                }
                if (m_gpuLossStreak >= 6) {
                    m_active = QPhaseBackendTarget::Cpu;
                    m_framesSinceSwitch = 0;
                    m_gpuLossStreak = 0;
                    return { QPhaseBackendTarget::Cpu, false, false, false, "amortized-demote-gpu-slow" };
                }
            }
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     isLargeDamage ? "gpu-heavy" : "gpu-steady" };
        }

        // Currently on CPU: promote immediately on resize/anim, or after 2 consecutive large frames
        const bool gpuKnownSlower = m_hasFullCpuSample && m_hasGpuEwma
                && (m_ewmaGpuTotalUs > (m_fullCpuUs * 13) / 10);
        if (isHardGpuTrigger || (m_largeStreak >= 2 && !gpuKnownSlower)) {
            m_active = QPhaseBackendTarget::Gpu;
            m_framesSinceSwitch = 0;
            m_smallStreak = 0;
            m_accumSavingsUs = 0;
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     isHardGpuTrigger ? "amortized-promote-resize" : "amortized-promote-burst" };
        }
        return { QPhaseBackendTarget::Cpu, false, false, false, "cpu-steady" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        ++m_framesSinceSwitch;
        if (!outcome.succeeded && outcome.backend == QPhaseBackendTarget::Gpu) {
            m_active = QPhaseBackendTarget::Cpu;
            m_framesSinceSwitch = 0;
            return;
        }
        if (outcome.backend == QPhaseBackendTarget::Cpu) {
            if (outcome.cpuWorkUs > 0 && outcome.dirtyAreaRatio >= 0.8f) {
                m_fullCpuUs = m_hasFullCpuSample ? ((m_fullCpuUs * 3 + outcome.cpuWorkUs) / 4) : outcome.cpuWorkUs;
                m_hasFullCpuSample = true;
            }
        } else if (outcome.gpuHwUs > 0) {
            const qint64 tot = outcome.cpuWorkUs + outcome.gpuHwUs;
            m_ewmaGpuTotalUs = m_hasGpuEwma ? ((m_ewmaGpuTotalUs * 3 + tot) / 4) : tot;
            m_hasGpuEwma = true;
            if (m_hasFullCpuSample && tot > (m_fullCpuUs * 13) / 10 + 1000)
                ++m_gpuLossStreak;
            else
                m_gpuLossStreak = 0;
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_fullCpuUs = cpuSampleUs;
            m_hasFullCpuSample = true;
        }
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_hasFullCpuSample = false;
        m_hasGpuEwma = false;
        m_cpuCalibRequested = false;
        m_fullCpuUs = 0;
        m_ewmaGpuTotalUs = 0;
        m_accumSavingsUs = 0;
        m_smallStreak = 0;
        m_largeStreak = 0;
        m_gpuLossStreak = 0;
        m_framesSinceSwitch = 0;
    }

private:
    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    bool m_hasFullCpuSample = false;
    bool m_hasGpuEwma = false;
    bool m_cpuCalibRequested = false;
    qint64 m_fullCpuUs = 0;
    qint64 m_ewmaGpuTotalUs = 0;
    qint64 m_accumSavingsUs = 0;
    int m_smallStreak = 0;
    int m_largeStreak = 0;
    int m_gpuLossStreak = 0;
    int m_framesSinceSwitch = 0;
};

// ============================================================================
// 8. cadence-power:
//    Interactive-burst vs sparse/idle-cadence scheduler (laptop/APU power win).
//    - Fast interactive bursts (< 65 ms cadence, resize, animations) run on GPU
//      while clocks are boosted.
//    - Sparse low-cadence small updates (>= 140 ms between frames: cursor blinks,
//      slow typing, 1 Hz ticks) demote after 3 frames to CPU partial-tile wl_shm
//      so the GPU stays in deep sleep (0 mW) instead of waking at 200 MHz!
// ============================================================================
class CadencePowerPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "cadence-power"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        const bool hardBurst = req.isResize || req.sizeChanged || req.hasActiveAnimations;
        const bool isSparseSmall = !hardBurst
                && req.frameIndex > 0
                && req.msSinceLastFrame >= 140
                && req.dirtyAreaRatio < 0.25f
                && req.pendingUploadBytes < (512 * 1024);
        const bool isFastBurst = hardBurst
                || (req.msSinceLastFrame < 65 && req.dirtyAreaRatio >= 0.20f);

        if (isSparseSmall) {
            ++m_sparseStreak;
            m_burstStreak = 0;
        } else if (isFastBurst) {
            ++m_burstStreak;
            m_sparseStreak = 0;
        } else {
            m_sparseStreak = 0;
            m_burstStreak = 0;
        }

        if (m_active == QPhaseBackendTarget::Gpu) {
            if (m_sparseStreak >= 3) {
                m_active = QPhaseBackendTarget::Cpu;
                m_sparseStreak = 0;
                return { QPhaseBackendTarget::Cpu, false, false, false, "cadence-demote-sparse" };
            }
            return { QPhaseBackendTarget::Gpu, false, false, false,
                     hardBurst ? "cadence-gpu-burst" : "cadence-gpu-active" };
        }

        if (hardBurst || m_burstStreak >= 2) {
            m_active = QPhaseBackendTarget::Gpu;
            m_burstStreak = 0;
            return { QPhaseBackendTarget::Gpu, false, false, false,
                     hardBurst ? "cadence-promote-resize" : "cadence-promote-burst" };
        }
        return { QPhaseBackendTarget::Cpu, false, false, false, "cadence-cpu-idle" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (!outcome.succeeded && outcome.backend == QPhaseBackendTarget::Gpu)
            m_active = QPhaseBackendTarget::Cpu;
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_sparseStreak = 0;
        m_burstStreak = 0;
    }

private:
    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    int m_sparseStreak = 0;
    int m_burstStreak = 0;
};

// ============================================================================
// 9. upload-aware:
//    Fallback & texture-churn guard. Keeps normal UI + one-off page loads
//    (like Okular) on GPU, but detects sustained multi-MB per-frame uploads
//    (animated emitRasterFallback paths / dynamic CPU canvases) and demotes to
//    CPU wl_shm so apps don't pay CPU raster + staging memcpy + GPU composite.
// ============================================================================
class UploadAwarePolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "upload-aware"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        const bool needCpuCalib = !m_hasCpuSample && !m_cpuCalibRequested
                && (m_active == QPhaseBackendTarget::Gpu || req.frameIndex == 0);
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        const bool bigUpload = !req.isResize && req.pendingUploadBytes >= (512 * 1024);
        if (bigUpload) {
            ++m_uploadChurnStreak;
            m_calmStreak = 0;
        } else if (req.pendingUploadBytes < (256 * 1024)) {
            m_uploadChurnStreak = 0;
            ++m_calmStreak;
        }

        if (m_active == QPhaseBackendTarget::Gpu) {
            if (m_uploadChurnStreak >= 4) {
                m_active = QPhaseBackendTarget::Cpu;
                m_calmStreak = 0;
                return { QPhaseBackendTarget::Cpu, false, false, false, "upload-churn-demote-cpu" };
            }
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false, "upload-aware-gpu" };
        }

        if (req.isResize || req.sizeChanged || m_calmStreak >= 12) {
            m_active = QPhaseBackendTarget::Gpu;
            m_uploadChurnStreak = 0;
            m_calmStreak = 0;
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     req.isResize ? "upload-aware-promote-resize" : "upload-aware-promote-calm" };
        }
        return { QPhaseBackendTarget::Cpu, false, false, false, "upload-aware-cpu" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (!outcome.succeeded && outcome.backend == QPhaseBackendTarget::Gpu)
            m_active = QPhaseBackendTarget::Cpu;
        if (outcome.backend == QPhaseBackendTarget::Cpu && outcome.cpuWorkUs > 0) {
            m_cpuUs = m_hasCpuSample ? ((m_cpuUs * 3 + outcome.cpuWorkUs) / 4) : outcome.cpuWorkUs;
            m_hasCpuSample = true;
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_cpuUs = cpuSampleUs;
            m_hasCpuSample = true;
        }
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_hasCpuSample = false;
        m_cpuCalibRequested = false;
        m_cpuUs = 0;
        m_uploadChurnStreak = 0;
        m_calmStreak = 0;
    }

private:
    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    bool m_hasCpuSample = false;
    bool m_cpuCalibRequested = false;
    qint64 m_cpuUs = 0;
    int m_uploadChurnStreak = 0;
    int m_calmStreak = 0;
};

// ============================================================================
// 10. contextual-bandit:
//     2-context online learner (SmallDamage vs LargeDamage) with zero hardcoded
//     cost assumptions. Measures empirical total latency per (context, backend)
//     arm, probes the untried arm cleanly, picks argmin with switch hysteresis,
//     and occasionally re-probes to adapt to changing GPU contention.
// ============================================================================
class ContextualBanditPolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "contextual-bandit"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "popup-cpu" };

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        const int ctx = (!req.isResize && !req.sizeChanged && !req.hasActiveAnimations && req.dirtyAreaRatio < 0.18f)
                ? kCtxSmall : kCtxLarge;
        m_lastCtx = ctx;

        // Large context: use zero-stall background CPU calibration once
        const bool needCpuCalib = (ctx == kCtxLarge) && !m_hasCost[kCtxLarge][kArmCpu] && !m_cpuCalibRequested;
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        // If we are mid-probe of CPU on SmallDamage (3 frames: 2 buffer warm-up + 1 measured partial), finish it
        if (m_smallCpuProbeRemaining > 0 && ctx == kCtxSmall) {
            --m_smallCpuProbeRemaining;
            m_active = QPhaseBackendTarget::Cpu;
            return { QPhaseBackendTarget::Cpu, false, false, false, "bandit-probe-small-cpu" };
        }
        if (ctx == kCtxLarge)
            m_smallCpuProbeRemaining = 0;

        // Once SmallDamage has empirical GPU cost (and >= 6 consecutive small frames), probe CPU once
        if (ctx == kCtxSmall)
            ++m_smallStreak;
        else
            m_smallStreak = 0;

        if (ctx == kCtxSmall && !m_hasCost[kCtxSmall][kArmCpu] && m_hasCost[kCtxSmall][kArmGpu]
            && m_smallStreak >= 6 && !m_probedSmallCpu) {
            m_probedSmallCpu = true;
            m_smallCpuProbeRemaining = 2; // This frame + 2 more = 3 total so 3rd frame hits partial damage!
            m_active = QPhaseBackendTarget::Cpu;
            m_framesSinceSwitch = 0;
            return { QPhaseBackendTarget::Cpu, false, false, false, "bandit-start-small-cpu-probe" };
        }

        // Periodic re-probe every 150 frames in Large context to detect mid-session GPU load changes
        if (ctx == kCtxLarge && m_framesInCtx[kCtxLarge] > 0 && (m_framesInCtx[kCtxLarge] % 150) == 0) {
            const QPhaseBackendTarget alt = (m_active == QPhaseBackendTarget::Gpu)
                    ? QPhaseBackendTarget::Cpu : QPhaseBackendTarget::Gpu;
            return { alt, false, false, false, "bandit-reprobe" };
        }

        ++m_framesInCtx[ctx];

        if (m_hasCost[ctx][kArmCpu] && m_hasCost[ctx][kArmGpu]) {
            const qint64 cpuC = m_ewmaUs[ctx][kArmCpu];
            const qint64 gpuC = m_ewmaUs[ctx][kArmGpu];
            const QPhaseBackendTarget best = (gpuC <= cpuC) ? QPhaseBackendTarget::Gpu : QPhaseBackendTarget::Cpu;
            if (best != m_active) {
                const qint64 curC = m_ewmaUs[ctx][m_active == QPhaseBackendTarget::Gpu ? kArmGpu : kArmCpu];
                const qint64 bestC = std::min(cpuC, gpuC);
                // Switch immediately on resize, or when best beats current by >= 15% after cooldown
                if (req.isResize || (m_framesSinceSwitch >= 8 && curC > (bestC * 115) / 100)) {
                    m_active = best;
                    m_framesSinceSwitch = 0;
                    return { m_active, false, needCpuCalib, false,
                             best == QPhaseBackendTarget::Gpu ? "bandit-choose-gpu" : "bandit-choose-cpu" };
                }
            }
            return { m_active, false, needCpuCalib, false,
                     m_active == QPhaseBackendTarget::Gpu ? "bandit-gpu" : "bandit-cpu" };
        }

        // Default while calibrating: GPU (zero stall!)
        m_active = QPhaseBackendTarget::Gpu;
        return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false, "bandit-init-gpu" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        ++m_framesSinceSwitch;
        if (!outcome.succeeded && outcome.backend == QPhaseBackendTarget::Gpu) {
            m_active = QPhaseBackendTarget::Cpu;
            m_framesSinceSwitch = 0;
            return;
        }
        const int ctx = m_lastCtx;
        if (outcome.backend == QPhaseBackendTarget::Cpu && outcome.cpuWorkUs > 0) {
            // For SmallDamage CPU probe, only record once warm-up finishes (last probe frame or steady)
            if (ctx == kCtxSmall && m_smallCpuProbeRemaining > 0)
                return;
            m_ewmaUs[ctx][kArmCpu] = m_hasCost[ctx][kArmCpu]
                    ? ((m_ewmaUs[ctx][kArmCpu] * 3 + outcome.cpuWorkUs) / 4)
                    : outcome.cpuWorkUs;
            m_hasCost[ctx][kArmCpu] = true;
        } else if (outcome.backend == QPhaseBackendTarget::Gpu && outcome.gpuHwUs > 0) {
            const qint64 tot = outcome.cpuWorkUs + outcome.gpuHwUs;
            m_ewmaUs[ctx][kArmGpu] = m_hasCost[ctx][kArmGpu]
                    ? ((m_ewmaUs[ctx][kArmGpu] * 3 + tot) / 4)
                    : tot;
            m_hasCost[ctx][kArmGpu] = true;
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_ewmaUs[kCtxLarge][kArmCpu] = cpuSampleUs;
            m_hasCost[kCtxLarge][kArmCpu] = true;
        }
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_lastCtx = kCtxLarge;
        m_cpuCalibRequested = false;
        m_probedSmallCpu = false;
        m_smallCpuProbeRemaining = 0;
        m_smallStreak = 0;
        m_framesSinceSwitch = 0;
        for (int c = 0; c < 2; ++c) {
            m_framesInCtx[c] = 0;
            for (int a = 0; a < 2; ++a) {
                m_hasCost[c][a] = false;
                m_ewmaUs[c][a] = 0;
            }
        }
    }

private:
    static constexpr int kCtxSmall = 0;
    static constexpr int kCtxLarge = 1;
    static constexpr int kArmCpu = 0;
    static constexpr int kArmGpu = 1;

    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    int m_lastCtx = kCtxLarge;
    bool m_cpuCalibRequested = false;
    bool m_probedSmallCpu = false;
    int m_smallCpuProbeRemaining = 0;
    int m_smallStreak = 0;
    int m_framesSinceSwitch = 0;
    int m_framesInCtx[2]{};
    bool m_hasCost[2][2]{};
    qint64 m_ewmaUs[2][2]{};
};

// ============================================================================
// 11. widget-aware:
//     Semantic widget-hint + cadence + empirical calibration scheduler.
//     - Popups, tooltips, and small dialogs (QDialog/QMessageBox <= 800x600
//       without scroll viewports) stay directly on CPU wl_shm (0 Vulkan rings).
//     - Main windows run empirical async GPU+CPU calibration (0 ms stall).
//     - Active ScrollViewport / HeavyCanvas / Resize / Animation bursts stay
//       on Vulkan GPU.
//     - Sparse localized TextInput / ControlChrome updates (cursor blinks,
//       slow typing, toolbar/status ticks) demote cleanly to CPU partial-tile
//       so the GPU sleeps during idle interactions.
// ============================================================================
class WidgetAwarePolicy final : public QPhaseRenderPolicy
{
public:
    const char *name() const noexcept override { return "widget-aware"; }

    QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                const QPhaseFrameRequest &req) override
    {
        QPhasePolicyDecision forced{};
        if (checkForcedAffinity(caps, forced))
            return forced;

        if (caps.isPopupOrTooltip && !caps.hasGpuOnlyViews)
            return { QPhaseBackendTarget::Cpu, false, false, true, "widget-popup-cpu" };

        const bool isDialog = (caps.surfaceHints & QPhaseHintDialogWindow) != 0u;
        const bool hasScrollViewport = (req.dirtyHints & QPhaseHintScrollViewportDirty) != 0u;
        const int area = req.logicalSize.width() * req.logicalSize.height();
        if (isDialog && !hasScrollViewport && !caps.hasGpuOnlyViews
            && area > 0 && area <= (800 * 600)) {
            return { QPhaseBackendTarget::Cpu, false, false, true, "widget-dialog-cpu" };
        }

        if (!caps.vulkanWarmReady)
            return { QPhaseBackendTarget::Cpu, false, false, false, "warmup-cpu-boot" };

        const bool needCpuCalib = !m_hasCpuSample && !m_cpuCalibRequested
                && (m_active == QPhaseBackendTarget::Gpu || req.frameIndex == 0);
        if (needCpuCalib)
            m_cpuCalibRequested = true;

        const bool hardGpu = req.isResize || req.sizeChanged || req.hasActiveAnimations;
        const bool heavyViewport = hasScrollViewport
                && (req.dirtyAreaRatio >= 0.18f || req.msSinceLastFrame < 80);
        const bool onlyLightChromeOrText = !hardGpu && !heavyViewport
                && (req.dirtyHints & (QPhaseHintTextInputDirty | QPhaseHintControlChromeDirty)) != 0u
                && (req.dirtyHints & (QPhaseHintScrollViewportDirty | QPhaseHintHeavyCanvasDirty)) == 0u
                && req.dirtyAreaRatio < 0.20f
                && req.pendingUploadBytes < (512 * 1024);
        const bool sparseLightUpdate = onlyLightChromeOrText
                && req.frameIndex > 0
                && req.msSinceLastFrame >= 120;

        if (sparseLightUpdate) {
            ++m_lightSparseStreak;
            m_heavyBurstStreak = 0;
        } else if (hardGpu || heavyViewport) {
            ++m_heavyBurstStreak;
            m_lightSparseStreak = 0;
        } else {
            m_lightSparseStreak = 0;
            m_heavyBurstStreak = 0;
        }

        // Software Vulkan fallback check once calibrated
        if (!caps.isHardwareGpu && m_hasCpuSample && m_hasGpuSample && m_ewmaGpuUs > m_cpuSampleUs) {
            m_active = QPhaseBackendTarget::Cpu;
            return { QPhaseBackendTarget::Cpu, false, false, false, "widget-swvk-cpu" };
        }

        if (m_active == QPhaseBackendTarget::Gpu) {
            if (m_lightSparseStreak >= 3) {
                m_active = QPhaseBackendTarget::Cpu;
                m_lightSparseStreak = 0;
                return { QPhaseBackendTarget::Cpu, false, false, false, "widget-demote-text-chrome" };
            }
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     hardGpu ? "widget-gpu-resize" : (heavyViewport ? "widget-gpu-viewport" : "widget-gpu-steady") };
        }

        if (hardGpu || m_heavyBurstStreak >= 2 || (req.dirtyHints & QPhaseHintHeavyCanvasDirty) != 0u) {
            m_active = QPhaseBackendTarget::Gpu;
            m_heavyBurstStreak = 0;
            return { QPhaseBackendTarget::Gpu, false, needCpuCalib, false,
                     hardGpu ? "widget-promote-resize" : "widget-promote-viewport" };
        }
        return { QPhaseBackendTarget::Cpu, false, false, false, "widget-cpu-light" };
    }

    void recordOutcome(const QPhaseFrameOutcome &outcome) override
    {
        if (!outcome.succeeded && outcome.backend == QPhaseBackendTarget::Gpu) {
            m_active = QPhaseBackendTarget::Cpu;
            return;
        }
        if (outcome.backend == QPhaseBackendTarget::Gpu && outcome.gpuHwUs > 0) {
            const qint64 tot = outcome.cpuWorkUs + outcome.gpuHwUs;
            m_ewmaGpuUs = m_hasGpuSample ? ((m_ewmaGpuUs * 3 + tot) / 4) : tot;
            m_hasGpuSample = true;
        }
    }

    void recordAsyncCpuSample(qint64 cpuSampleUs) override
    {
        if (cpuSampleUs > 0) {
            m_cpuSampleUs = cpuSampleUs;
            m_hasCpuSample = true;
        }
    }

    void reset() override
    {
        m_active = QPhaseBackendTarget::Gpu;
        m_hasCpuSample = false;
        m_hasGpuSample = false;
        m_cpuCalibRequested = false;
        m_cpuSampleUs = 0;
        m_ewmaGpuUs = 0;
        m_lightSparseStreak = 0;
        m_heavyBurstStreak = 0;
    }

private:
    QPhaseBackendTarget m_active = QPhaseBackendTarget::Gpu;
    bool m_hasCpuSample = false;
    bool m_hasGpuSample = false;
    bool m_cpuCalibRequested = false;
    qint64 m_cpuSampleUs = 0;
    qint64 m_ewmaGpuUs = 0;
    int m_lightSparseStreak = 0;
    int m_heavyBurstStreak = 0;
};

} // namespace

QList<QByteArray> QPhaseRenderPolicy::availablePolicies()
{
    return {
        QByteArrayLiteral("async-race"),
        QByteArrayLiteral("dynamic-hybrid"),
        QByteArrayLiteral("amortized-hybrid"),
        QByteArrayLiteral("cadence-power"),
        QByteArrayLiteral("upload-aware"),
        QByteArrayLiteral("contextual-bandit"),
        QByteArrayLiteral("widget-aware"),
        QByteArrayLiteral("sync-race"),
        QByteArrayLiteral("hw-gpu"),
        QByteArrayLiteral("cpu-only"),
        QByteArrayLiteral("gpu-only"),
    };
}

std::unique_ptr<QPhaseRenderPolicy> QPhaseRenderPolicy::create(QByteArrayView name)
{
    if (name.isEmpty() || name == "async-race")
        return std::make_unique<AsyncRacePolicy>();
    if (name == "dynamic-hybrid" || name == "hybrid")
        return std::make_unique<DynamicHybridPolicy>();
    if (name == "amortized-hybrid" || name == "amortized")
        return std::make_unique<AmortizedHybridPolicy>();
    if (name == "cadence-power" || name == "cadence")
        return std::make_unique<CadencePowerPolicy>();
    if (name == "upload-aware" || name == "upload")
        return std::make_unique<UploadAwarePolicy>();
    if (name == "contextual-bandit" || name == "bandit")
        return std::make_unique<ContextualBanditPolicy>();
    if (name == "widget-aware" || name == "widget")
        return std::make_unique<WidgetAwarePolicy>();
    if (name == "sync-race")
        return std::make_unique<SyncRacePolicy>();
    if (name == "hw-gpu")
        return std::make_unique<HwGpuPolicy>();
    if (name == "cpu-only" || name == "cpu")
        return std::make_unique<CpuOnlyPolicy>();
    if (name == "gpu-only" || name == "gpu")
        return std::make_unique<GpuOnlyPolicy>();
    return nullptr;
}

std::unique_ptr<QPhaseRenderPolicy> QPhaseRenderPolicy::createFromEnvironment()
{
    const QByteArray raw = qgetenv("QT_PHASE_POLICY").trimmed().toLower();
    if (raw == "list") {
        static bool sPrintedList = false;
        if (!sPrintedList) {
            sPrintedList = true;
            std::fprintf(stderr, "[QtPhase] Available QT_PHASE_POLICY algorithms: async-race (default), dynamic-hybrid, amortized-hybrid, cadence-power, upload-aware, contextual-bandit, widget-aware, sync-race, hw-gpu, cpu-only, gpu-only\n");
            std::fflush(stderr);
        }
        return std::make_unique<AsyncRacePolicy>();
    }
    if (auto p = create(raw))
        return p;

    static bool sWarnedUnknown = false;
    if (!sWarnedUnknown && !raw.isEmpty()) {
        sWarnedUnknown = true;
        std::fprintf(stderr, "[QtPhase] Unknown QT_PHASE_POLICY='%s', falling back to 'async-race'. Available: async-race, dynamic-hybrid, amortized-hybrid, cadence-power, upload-aware, contextual-bandit, widget-aware, sync-race, hw-gpu, cpu-only, gpu-only\n",
                     raw.constData());
        std::fflush(stderr);
    }
    return std::make_unique<AsyncRacePolicy>();
}

QT_END_NAMESPACE
