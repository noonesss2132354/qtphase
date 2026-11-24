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

} // namespace

QList<QByteArray> QPhaseRenderPolicy::availablePolicies()
{
    return {
        QByteArrayLiteral("async-race"),
        QByteArrayLiteral("dynamic-hybrid"),
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
            std::fprintf(stderr, "[QtPhase] Available QT_PHASE_POLICY algorithms: async-race (default), dynamic-hybrid, sync-race, hw-gpu, cpu-only, gpu-only\n");
            std::fflush(stderr);
        }
        return std::make_unique<AsyncRacePolicy>();
    }
    if (auto p = create(raw))
        return p;

    static bool sWarnedUnknown = false;
    if (!sWarnedUnknown && !raw.isEmpty()) {
        sWarnedUnknown = true;
        std::fprintf(stderr, "[QtPhase] Unknown QT_PHASE_POLICY='%s', falling back to 'async-race'. Available: async-race, dynamic-hybrid, sync-race, hw-gpu, cpu-only, gpu-only\n",
                     raw.constData());
        std::fflush(stderr);
    }
    return std::make_unique<AsyncRacePolicy>();
}

QT_END_NAMESPACE
