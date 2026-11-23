// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasesurfacepresenter_p.h"
#include "qphasescenearena_p.h"
#include "cpu/qphasecpurasterizer_p.h"
#include "vulkan/qphasevulkanbackend_p.h"

#include <qpa/qplatformwindow.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qtimer.h>
#include <QtGui/qscreen.h>
#include <QtGui/qwindow.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

QT_BEGIN_NAMESPACE

namespace {
constexpr uint8_t kSoftGpuLockInStreak = 3;
constexpr uint8_t kCpuLockInStreak = 5;
constexpr qint64 kHardwareGpuFastUs = 8000;
}

struct PresenterCpuJob {
    uint64_t seq = 0;
    QPlatformWindow::PhasedBootBuffer bootBuf{};
    bool forceOpaqueAlpha = true;
    uint64_t frameTimeNs = 0;
};

class QPhaseSurfacePresenterPrivate
{
public:
    QPointer<QWindow> window;
    QPhaseSceneArena uiArena;
    std::function<void(QPhaseSceneArena &)> syncCallback;

    // UI Reflex VSync-deadline pacer
    QTimer reflexTimer;
    std::atomic<qint64> lastVsyncAnchorUs{0};
    std::atomic<qint64> rollingBackendRenderUs{1800};
    qint64 lastReflexTargetVsyncUs = 0;
    bool reflexPendingAfterPresent = false;
    bool presentDirty = false;
    bool isShutDown = false;

    // Coordinator state
    std::atomic<QPhaseSurfacePresenter::Phase> phase{QPhaseSurfacePresenter::Phase::WorkerRace};
    QPhaseSurfacePresenter::Phase lastFinalizedPhase = QPhaseSurfacePresenter::Phase::WorkerRace;
    std::atomic<bool> gpuDeviceReady{false};
    std::atomic<bool> gpuInitFailed{false};
    std::atomic<bool> gpuOwnsSurface{false};
    std::atomic<bool> winnerStillPresenting{false};
    std::atomic<uint64_t> abortUpToSeq{0};
    std::atomic<QPhaseRendererAffinity> forcedAffinity{QPhaseRendererAffinity::Any};
    std::atomic<bool> forcedBlocking{false};

    std::mutex raceMutex;
    uint64_t nextFrameSeq = 0;
    uint64_t lastResolvedSeq = 0;
    uint8_t consecutiveCpuWins = 0;
    uint8_t consecutiveGpuWins = 0;
    bool raceFinalized = false;

    // CPU worker
    std::unique_ptr<QPhaseCpuRasterizer> cpuRasterizer;
    std::unique_ptr<QPhaseSceneArena> cpuRenderArena;
    QPhaseDamageTracker cpuDamageTracker;
    std::mutex cpuMutex;
    std::condition_variable cpuCv;
    std::thread cpuThread;
    bool cpuThreadStarted = false;
    bool cpuStopRequested = false;
    bool cpuJobPending = false;
    bool cpuJobInFlight = false;
    bool cpuDirtyWhileInFlight = false;
    PresenterCpuJob pendingCpuJob{};

    // GPU worker
    std::unique_ptr<QPhaseVulkanBackend> vulkanBackend;
    QPhaseSceneArena gpuSharedArena;
    QPhaseSceneArena gpuWorkerArena;
    std::mutex gpuMutex;
    std::condition_variable gpuCv;
    std::thread gpuThread;
    bool gpuThreadStarted = false;
    bool gpuStopRequested = false;
    bool gpuFrameRequested = false;
    bool gpuRenderingNow = false;
    bool gpuSwapchainAttached = false;
    bool needReleaseBootResources = false;
    uint64_t pendingGpuSeq = 0;
    QPlatformWindow *pendingGpuPw = nullptr;
    QSize pendingGpuPxSize;
    qreal pendingGpuDpr = 1.0;
    bool pendingGpuAlpha = false;
    uint64_t pendingGpuTimeNs = 0;
    std::atomic<uint64_t> expectedSurfaceSizeKey{0};
    uint64_t lastDispatchedSeq = 0;
    uint64_t lastDispatchedRev = 0;
    QSize lastDispatchedPxSize;
    qreal lastDispatchedDpr = 0.0;

    // Telemetry
    QElapsedTimer createTimer;
    std::atomic<quint64> cpuFrames{0};
    std::atomic<quint64> gpuFrames{0};
    std::atomic<qint64> gpuWarmupUs{0};
    std::atomic<qint64> totalGpuHwUs{0};
    std::atomic<quint64> totalSyncedBytes{0};
    bool dumpedDebugPng = false;

    void stopCpuThread()
    {
        if (!cpuThreadStarted)
            return;
        {
            std::lock_guard<std::mutex> lock(cpuMutex);
            cpuStopRequested = true;
            cpuJobPending = false;
        }
        cpuCv.notify_all();
        if (cpuThread.joinable())
            cpuThread.join();
        cpuThreadStarted = false;
    }

    void stopGpuThread()
    {
        if (!gpuThreadStarted)
            return;
        {
            std::lock_guard<std::mutex> lock(gpuMutex);
            gpuStopRequested = true;
            gpuFrameRequested = false;
        }
        gpuCv.notify_all();
        if (gpuThread.joinable())
            gpuThread.join();
        gpuThreadStarted = false;
    }
};

QPhaseSurfacePresenter::QPhaseSurfacePresenter(QWindow *window, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<QPhaseSurfacePresenterPrivate>())
{
    d->window = window;
    d->createTimer.start();

    d->reflexTimer.setSingleShot(true);
    d->reflexTimer.setTimerType(Qt::PreciseTimer);
    connect(&d->reflexTimer, &QTimer::timeout, this, [this]() {
        if (!d->isShutDown && d->presentDirty)
            presentNow();
    });

    startEarlyGpuWarmupThread();
}

QPhaseSurfacePresenter::~QPhaseSurfacePresenter()
{
    const bool printStats = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_STATS") > 0;
    const quint64 cpuN = d->cpuFrames.load(std::memory_order_relaxed);
    const quint64 gpuN = d->gpuFrames.load(std::memory_order_relaxed);
    const qint64 warmupUs = d->gpuWarmupUs.load(std::memory_order_relaxed);
    const qint64 hwUs = d->totalGpuHwUs.load(std::memory_order_relaxed);
    const quint64 bytes = d->totalSyncedBytes.load(std::memory_order_relaxed);
    const Phase curPhaseVal = d->phase.load(std::memory_order_relaxed);
    const Phase finalPhase = (curPhaseVal != Phase::WorkerRace) ? curPhaseVal : d->lastFinalizedPhase;
    const QString winName = d->window ? d->window->objectName() : QStringLiteral("<null>");

    shutdown();

    if (printStats && (cpuN > 0 || gpuN > 0)) {
        const char *phaseStr = (finalPhase == Phase::GpuSteadyState)
                ? "GPU Vulkan 1.3"
                : (finalPhase == Phase::CpuSteadyState ? "CPU Tile Pool" : "WorkerRace");
        const double avgHwMs = gpuN > 0 ? (double(hwUs) / double(gpuN)) / 1000.0 : 0.0;
        const double avgKb = gpuN > 0 ? (double(bytes) / double(gpuN)) / 1024.0 : 0.0;
        std::fprintf(stderr,
                     "[QtWidgets+QtPhase] %s | winner=%s | warmup=%.2f ms | frames: CPU=%llu GPU=%llu | avg_gpu_hw=%.2f ms | avg_uma=%.1f KB\n",
                     qPrintable(winName),
                     phaseStr,
                     double(warmupUs) / 1000.0,
                     static_cast<unsigned long long>(cpuN),
                     static_cast<unsigned long long>(gpuN),
                     avgHwMs,
                     avgKb);
        std::fflush(stderr);
    }
}

void QPhaseSurfacePresenter::onSurfaceHidden()
{
    d->reflexTimer.stop();
    d->presentDirty = false;
    d->abortUpToSeq.store(d->nextFrameSeq + 1, std::memory_order_release);
    d->stopCpuThread();
    d->stopGpuThread();
    d->cpuStopRequested = false;
    d->cpuJobPending = false;
    d->cpuJobInFlight = false;
    d->cpuDirtyWhileInFlight = false;
    d->gpuStopRequested = false;
    d->gpuFrameRequested = false;
    d->gpuRenderingNow = false;
    d->vulkanBackend.reset();
    d->gpuSwapchainAttached = false;
    d->needReleaseBootResources = false;
    d->expectedSurfaceSizeKey.store(0, std::memory_order_release);
    d->lastDispatchedSeq = 0;
    d->lastDispatchedRev = 0;
    d->lastDispatchedPxSize = QSize();
    d->lastDispatchedDpr = 0.0;
    d->gpuDeviceReady.store(false, std::memory_order_release);
    d->gpuOwnsSurface.store(false, std::memory_order_release);
    d->phase.store(Phase::WorkerRace, std::memory_order_release);
    d->uiArena.resetGpuSteadyState();
    d->gpuSharedArena.resetGpuSteadyState();
    d->gpuWorkerArena.resetGpuSteadyState();
    d->cpuDamageTracker.reset();
    {
        std::lock_guard<std::mutex> lock(d->raceMutex);
        d->consecutiveCpuWins = 0;
        d->consecutiveGpuWins = 0;
        d->raceFinalized = false;
    }
}

void QPhaseSurfacePresenter::shutdown()
{
    if (d->isShutDown)
        return;
    d->isShutDown = true;
    d->reflexTimer.stop();
    d->abortUpToSeq.store(UINT64_MAX, std::memory_order_release);
    d->stopCpuThread();
    d->stopGpuThread();
    d->cpuRasterizer.reset();
    d->cpuRenderArena.reset();
    d->vulkanBackend.reset();
}

QPhaseSceneArena &QPhaseSurfacePresenter::arena() noexcept
{
    return d->uiArena;
}

const QPhaseSceneArena &QPhaseSurfacePresenter::arena() const noexcept
{
    return d->uiArena;
}

void QPhaseSurfacePresenter::setSyncCallback(std::function<void(QPhaseSceneArena &)> callback)
{
    d->syncCallback = std::move(callback);
}

void QPhaseSurfacePresenter::setForcedAffinity(QPhaseRendererAffinity affinity, bool blocking)
{
    d->forcedAffinity.store(affinity, std::memory_order_release);
    d->forcedBlocking.store(blocking, std::memory_order_release);
}

QPhaseSurfacePresenter::Phase QPhaseSurfacePresenter::currentPhase() const noexcept
{
    return d->phase.load(std::memory_order_acquire);
}

quint64 QPhaseSurfacePresenter::cpuFramesPresented() const noexcept
{
    return d->cpuFrames.load(std::memory_order_relaxed);
}

quint64 QPhaseSurfacePresenter::gpuFramesPresented() const noexcept
{
    return d->gpuFrames.load(std::memory_order_relaxed);
}

void QPhaseSurfacePresenter::startEarlyGpuWarmupThread()
{
    if (d->gpuThreadStarted)
        return;
    d->gpuThreadStarted = true;
    d->vulkanBackend = std::make_unique<QPhaseVulkanBackend>();

    // Pillar 3: One-time process-wide hardware GPU probe!
    // Once window #1 has warmed up a real hardware Vulkan 1.3 WSI device, skip the dual-thread
    // CPU-vs-GPU WorkerRace entirely for secondary windows, dialogs, popups, and re-shown surfaces:
    // attach straight into GpuSteadyState (~0.2 ms shared device cache hit, 0 wl_shm boot buffers!).
    if (d->forcedAffinity.load(std::memory_order_acquire) != QPhaseRendererAffinity::CpuOnly
        && QPhaseVulkanBackend::isSharedHardwareDeviceReady()) {
        d->phase.store(Phase::GpuSteadyState, std::memory_order_release);
        d->lastFinalizedPhase = Phase::GpuSteadyState;
        d->gpuOwnsSurface.store(true, std::memory_order_release);
        d->uiArena.markEnteredGpuSteadyState();
        d->gpuSharedArena.markEnteredGpuSteadyState();
        d->needReleaseBootResources = true;
    }

    QPhaseSurfacePresenterPrivate *priv = d.get();
    QPointer<QPhaseSurfacePresenter> safeThis(this);

    d->gpuThread = std::thread([priv, safeThis]() {
        QElapsedTimer wt;
        wt.start();
        const bool ok = priv->vulkanBackend->initializeDevice(true);
        priv->gpuWarmupUs.store(wt.nsecsElapsed() / 1000, std::memory_order_relaxed);
        if (!ok) {
            priv->gpuInitFailed.store(true, std::memory_order_release);
            if (safeThis) {
                QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                    if (!safeThis || safeThis->d->isShutDown)
                        return;
                    safeThis->finalizeWorkerWinner(Phase::CpuSteadyState);
                    if (safeThis->d->presentDirty)
                        safeThis->presentNow();
                }, Qt::QueuedConnection);
            }
            return;
        }

        priv->gpuDeviceReady.store(true, std::memory_order_release);

        if (safeThis) {
            QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                if (!safeThis || safeThis->d->isShutDown)
                    return;
                if (safeThis->d->presentDirty
                    || (safeThis->d->phase.load(std::memory_order_acquire) == Phase::WorkerRace
                        && safeThis->d->window && safeThis->d->window->isExposed())) {
                    safeThis->presentNow();
                }
            }, Qt::QueuedConnection);
        }

        while (true) {
            QPlatformWindow *pw = nullptr;
            QSize pxSize;
            qreal dpr = 1.0;
            bool hasAlpha = false;
            uint64_t frameTimeNs = 0;
            uint64_t jobSeq = 0;

            {
                std::unique_lock<std::mutex> lock(priv->gpuMutex);
                priv->gpuCv.wait(lock, [priv]() {
                    return priv->gpuStopRequested || priv->gpuFrameRequested;
                });
                if (priv->gpuStopRequested)
                    break;

                priv->gpuFrameRequested = false;
                priv->gpuRenderingNow = true;
                pw = priv->pendingGpuPw;
                pxSize = priv->pendingGpuPxSize;
                dpr = priv->pendingGpuDpr;
                hasAlpha = priv->pendingGpuAlpha;
                frameTimeNs = priv->pendingGpuTimeNs;
                jobSeq = priv->pendingGpuSeq;
                priv->gpuWorkerArena.syncRenderSnapshotFrom(priv->gpuSharedArena);
            }

            if (!pw || pxSize.isEmpty()) {
                std::lock_guard<std::mutex> lock(priv->gpuMutex);
                priv->gpuRenderingNow = false;
                continue;
            }

            const Phase curPhase = priv->phase.load(std::memory_order_acquire);
            QPhaseVulkanBackend::GpuFrameMetrics metrics{};
            bool okRender = false;
            uint8_t wonStreak = 0;
            Phase lockedPhase = Phase::WorkerRace;

            if (curPhase == Phase::WorkerRace) {
                okRender = priv->vulkanBackend->benchmarkFrameOffscreen(
                        priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameTimeNs, &metrics);
                if (okRender) {
                    std::lock_guard<std::mutex> lock(priv->raceMutex);
                    const bool hwGpu = priv->vulkanBackend->isHardwareGpu();
                    const uint8_t neededStreak = hwGpu ? 1u : kSoftGpuLockInStreak;
                    const bool gpuFastEnough = hwGpu && (metrics.gpuHardwareUs > 0 && metrics.gpuHardwareUs < kHardwareGpuFastUs);
                    if (!priv->raceFinalized && (jobSeq > priv->lastResolvedSeq || gpuFastEnough)) {
                        if (jobSeq > priv->lastResolvedSeq)
                            priv->lastResolvedSeq = jobSeq;
                        priv->consecutiveCpuWins = 0;
                        if (priv->consecutiveGpuWins < neededStreak)
                            ++priv->consecutiveGpuWins;
                        wonStreak = priv->consecutiveGpuWins;
                        if (priv->consecutiveGpuWins >= neededStreak) {
                            priv->raceFinalized = true;
                            lockedPhase = Phase::GpuSteadyState;
                            // Only abort CPU presentation once GPU is actually taking over the swapchain!
                            priv->abortUpToSeq.store(jobSeq, std::memory_order_release);
                        }
                    }
                }
                if (lockedPhase == Phase::GpuSteadyState) {
                    priv->vulkanBackend->releaseRaceResources();
                    priv->gpuOwnsSurface.store(true, std::memory_order_release);
                    if (!priv->gpuSwapchainAttached)
                        priv->gpuSwapchainAttached = priv->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);
                    if (priv->gpuSwapchainAttached) {
                        okRender = priv->vulkanBackend->renderAndPresentWindow(
                                pw, priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameTimeNs,
                                &metrics, false, {}, &priv->expectedSurfaceSizeKey);
                    } else {
                        okRender = false;
                    }
                }
            } else if (curPhase == Phase::GpuSteadyState) {
                priv->gpuOwnsSurface.store(true, std::memory_order_release);
                if (!priv->gpuSwapchainAttached)
                    priv->gpuSwapchainAttached = priv->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);
                if (priv->gpuSwapchainAttached) {
                    okRender = priv->vulkanBackend->renderAndPresentWindow(
                            pw, priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameTimeNs,
                            &metrics, false, {}, &priv->expectedSurfaceSizeKey);
                }
            }

            {
                std::lock_guard<std::mutex> lock(priv->gpuMutex);
                priv->gpuRenderingNow = false;
            }

            if (okRender && (curPhase == Phase::GpuSteadyState || lockedPhase == Phase::GpuSteadyState)) {
                const qint64 nowUs = priv->createTimer.nsecsElapsed() / 1000;
                priv->lastVsyncAnchorUs.store(nowUs, std::memory_order_release);
                const qint64 workUs = std::max<qint64>(metrics.cpuSubmitWorkUs, metrics.gpuHardwareUs);
                const qint64 prev = priv->rollingBackendRenderUs.load(std::memory_order_relaxed);
                priv->rollingBackendRenderUs.store((prev * 3 + workUs) / 4, std::memory_order_relaxed);
            }

            if (safeThis) {
                QMetaObject::invokeMethod(safeThis.data(),
                        [safeThis, jobSeq, wonStreak, lockedPhase, okRender,
                         workUs = metrics.cpuSubmitWorkUs,
                         hwUs = metrics.gpuHardwareUs,
                         syncedBytes = metrics.syncedSceneBytes,
                         uploadedGen = metrics.uploadedImageGen]() {
                    if (safeThis && !safeThis->d->isShutDown)
                        safeThis->onGpuWorkerFrameDone(jobSeq, wonStreak, lockedPhase,
                                                       okRender ? workUs : -1, hwUs, syncedBytes, uploadedGen);
                }, Qt::QueuedConnection);
            }
        }
    });
}

void QPhaseSurfacePresenter::ensureCpuBootThreadStarted()
{
    if (d->cpuThreadStarted)
        return;

    if (!d->cpuRasterizer)
        d->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
    if (!d->cpuRenderArena)
        d->cpuRenderArena = std::make_unique<QPhaseSceneArena>();

    d->cpuThreadStarted = true;
    QPhaseSurfacePresenterPrivate *priv = d.get();
    QPointer<QPhaseSurfacePresenter> safeThis(this);

    d->cpuThread = std::thread([priv, safeThis]() {
        while (true) {
            PresenterCpuJob job{};
            {
                std::unique_lock<std::mutex> lock(priv->cpuMutex);
                priv->cpuCv.wait(lock, [priv]() {
                    return priv->cpuStopRequested || priv->cpuJobPending;
                });
                if (priv->cpuStopRequested)
                    break;
                job = priv->pendingCpuJob;
                priv->cpuJobPending = false;
            }

            if (!job.bootBuf.data || job.bootBuf.width <= 0 || job.bootBuf.height <= 0)
                continue;

            const qreal dpr = job.bootBuf.devicePixelRatio > 0.0 ? job.bootBuf.devicePixelRatio : 1.0;
            const QSize pxSize(job.bootBuf.width, job.bootBuf.height);

            QElapsedTimer t;
            t.start();
            bool isFullDmg = true;
            const QRegion dmgRegion = priv->cpuDamageTracker.computeAndAdvance(
                    *priv->cpuRenderArena, pxSize, dpr, job.frameTimeNs,
                    &isFullDmg, QPhaseRendererAffinity::CpuOnly);

            const bool finished = priv->cpuRasterizer->renderToBuffer(
                    *priv->cpuRenderArena,
                    job.bootBuf.data,
                    job.bootBuf.width,
                    job.bootBuf.height,
                    job.bootBuf.bytesPerLine,
                    dpr,
                    job.forceOpaqueAlpha,
                    job.frameTimeNs,
                    &priv->abortUpToSeq,
                    job.seq,
                    nullptr);
            const qint64 cpuWorkUs = t.nsecsElapsed() / 1000;

            if (!finished) {
                priv->cpuDamageTracker.reset();
                if (safeThis) {
                    QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                        if (!safeThis || safeThis->d->isShutDown)
                            return;
                        bool renderAgain = false;
                        {
                            std::lock_guard<std::mutex> lock(safeThis->d->cpuMutex);
                            safeThis->d->cpuJobInFlight = false;
                            if (safeThis->d->cpuDirtyWhileInFlight || safeThis->d->presentDirty) {
                                safeThis->d->cpuDirtyWhileInFlight = false;
                                renderAgain = true;
                            }
                        }
                        if (renderAgain && safeThis->d->window && safeThis->d->window->isExposed())
                            safeThis->schedulePresent();
                    }, Qt::QueuedConnection);
                }
                continue;
            }

            const Phase curPhase = priv->phase.load(std::memory_order_acquire);
            uint8_t wonStreak = 0;
            Phase lockedPhase = curPhase;
            bool shouldPresent = !priv->gpuOwnsSurface.load(std::memory_order_acquire);

            if (curPhase == Phase::WorkerRace) {
                const bool gpuReady = priv->gpuDeviceReady.load(std::memory_order_acquire);
                std::lock_guard<std::mutex> lock(priv->raceMutex);
                if (!priv->raceFinalized && job.seq > priv->lastResolvedSeq) {
                    priv->lastResolvedSeq = job.seq;
                    if (gpuReady) {
                        if (priv->consecutiveCpuWins < kCpuLockInStreak)
                            ++priv->consecutiveCpuWins;
                        wonStreak = priv->consecutiveCpuWins;
                        if (priv->consecutiveCpuWins >= kCpuLockInStreak) {
                            priv->consecutiveGpuWins = 0;
                            priv->raceFinalized = true;
                            lockedPhase = Phase::CpuSteadyState;
                        }
                    }
                }
            }

            if (!shouldPresent) {
                priv->cpuDamageTracker.reset();
                if (safeThis) {
                    QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                        if (!safeThis || safeThis->d->isShutDown)
                            return;
                        bool renderAgain = false;
                        {
                            std::lock_guard<std::mutex> lock(safeThis->d->cpuMutex);
                            safeThis->d->cpuJobInFlight = false;
                            if (safeThis->d->cpuDirtyWhileInFlight || safeThis->d->presentDirty) {
                                safeThis->d->cpuDirtyWhileInFlight = false;
                                renderAgain = true;
                            }
                        }
                        if (renderAgain && safeThis->d->window && safeThis->d->window->isExposed())
                            safeThis->schedulePresent();
                    }, Qt::QueuedConnection);
                }
                continue;
            }

            QRegion logicalDmg;
            if (!isFullDmg && !dmgRegion.isEmpty()) {
                for (const QRect &r : dmgRegion) {
                    const int x0 = int(std::floor(double(r.left()) / dpr));
                    const int y0 = int(std::floor(double(r.top()) / dpr));
                    const int x1 = int(std::ceil(double(r.right() + 1) / dpr));
                    const int y1 = int(std::ceil(double(r.bottom() + 1) / dpr));
                    logicalDmg += QRect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
                }
            } else {
                logicalDmg = QRect(0, 0,
                                   std::max(1, int(std::ceil(double(job.bootBuf.width) / dpr))),
                                   std::max(1, int(std::ceil(double(job.bootBuf.height) / dpr))));
            }

            if (safeThis) {
                QMetaObject::invokeMethod(safeThis.data(),
                        [safeThis, seq = job.seq, wonStreak, lockedPhase,
                         handle = job.bootBuf.handle, cpuWorkUs, logicalDmg]() {
                    if (safeThis && !safeThis->d->isShutDown)
                        safeThis->onCpuWorkerFrameDone(seq, wonStreak, lockedPhase, handle, cpuWorkUs, logicalDmg);
                }, Qt::QueuedConnection);
            }
        }
    });
}

void QPhaseSurfacePresenter::finalizeWorkerWinner(Phase winningPhase)
{
    const Phase cur = d->phase.load(std::memory_order_acquire);
    if (cur != Phase::WorkerRace
        && !(winningPhase == Phase::CpuSteadyState && d->gpuInitFailed.load(std::memory_order_acquire))) {
        return;
    }

    if (winningPhase == Phase::GpuSteadyState) {
        d->lastFinalizedPhase = Phase::GpuSteadyState;
        d->phase.store(Phase::GpuSteadyState, std::memory_order_release);
        d->stopCpuThread();
        if (d->uiArena.rendererRequirements().cpuOnlyViewCount == 0)
            d->cpuRasterizer.reset();
        d->cpuRenderArena.reset();
        d->uiArena.markEnteredGpuSteadyState();
        {
            std::lock_guard<std::mutex> lock(d->gpuMutex);
            d->gpuSharedArena.markEnteredGpuSteadyState();
        }
        d->gpuCv.notify_all();
        if (d->window && d->window->handle())
            d->window->handle()->releasePhasedBootResources();
    } else if (winningPhase == Phase::CpuSteadyState) {
        d->lastFinalizedPhase = Phase::CpuSteadyState;
        d->phase.store(Phase::CpuSteadyState, std::memory_order_release);
        d->stopGpuThread();
        d->vulkanBackend.reset();
        d->gpuOwnsSurface.store(false, std::memory_order_release);
    }
}

void QPhaseSurfacePresenter::onCpuWorkerFrameDone(quint64 seq,
                                                  quint8 wonStreak,
                                                  Phase lockedPhase,
                                                  void *bufferHandle,
                                                  qint64 cpuWorkUs,
                                                  const QRegion &logicalSurfaceDamage)
{
    Q_UNUSED(seq);
    Q_UNUSED(wonStreak);

    const Phase curPhase = d->phase.load(std::memory_order_acquire);
    if (curPhase == Phase::GpuSteadyState) {
        std::lock_guard<std::mutex> lock(d->cpuMutex);
        d->cpuJobInFlight = false;
        return;
    }

    if (d->window && d->window->isExposed() && d->window->handle() && bufferHandle) {
        if (!d->gpuOwnsSurface.load(std::memory_order_acquire))
            d->window->handle()->presentPhasedBootBuffer(bufferHandle, logicalSurfaceDamage);
        d->cpuFrames.fetch_add(1, std::memory_order_relaxed);
        const qint64 nowUs = d->createTimer.nsecsElapsed() / 1000;
        d->lastVsyncAnchorUs.store(nowUs, std::memory_order_release);
        const qint64 prev = d->rollingBackendRenderUs.load(std::memory_order_relaxed);
        d->rollingBackendRenderUs.store((prev * 3 + cpuWorkUs) / 4, std::memory_order_relaxed);
    }

    bool renderAgain = false;
    {
        std::lock_guard<std::mutex> lock(d->cpuMutex);
        d->cpuJobInFlight = false;
        if (d->cpuDirtyWhileInFlight || d->presentDirty) {
            d->cpuDirtyWhileInFlight = false;
            renderAgain = true;
        }
    }

    if (lockedPhase == Phase::CpuSteadyState && curPhase == Phase::WorkerRace)
        finalizeWorkerWinner(Phase::CpuSteadyState);

    if (renderAgain && d->window && d->window->isExposed())
        schedulePresent();
}

void QPhaseSurfacePresenter::onGpuWorkerFrameDone(quint64 seq,
                                                  quint8 wonStreak,
                                                  Phase lockedPhase,
                                                  qint64 gpuWorkUs,
                                                  qint64 gpuHwUs,
                                                  quint64 syncedBytes,
                                                  quint64 uploadedImageGen)
{
    Q_UNUSED(seq);
    Q_UNUSED(wonStreak);

    const Phase curPhase = d->phase.load(std::memory_order_acquire);
    const bool isLatestFailed = (gpuWorkUs < 0 && curPhase == Phase::GpuSteadyState && seq >= d->lastDispatchedSeq);
    if (gpuWorkUs >= 0 && (curPhase == Phase::GpuSteadyState || lockedPhase == Phase::GpuSteadyState)) {
        d->gpuFrames.fetch_add(1, std::memory_order_relaxed);
        d->totalGpuHwUs.fetch_add(gpuHwUs, std::memory_order_relaxed);
        d->totalSyncedBytes.fetch_add(syncedBytes, std::memory_order_relaxed);
    } else if (isLatestFailed) {
        d->lastDispatchedRev = 0;
        d->presentDirty = true;
    }

    if (lockedPhase == Phase::GpuSteadyState && curPhase == Phase::WorkerRace)
        finalizeWorkerWinner(Phase::GpuSteadyState);

    // Pillar 2: Explicit GPU upload receipt! Only free CPU pixel copies AFTER Vulkan confirms upload.
    if (gpuWorkUs >= 0 && uploadedImageGen > 0
        && d->phase.load(std::memory_order_acquire) == Phase::GpuSteadyState) {
        d->uiArena.acknowledgeGpuImagesUploaded(uploadedImageGen);
        std::lock_guard<std::mutex> lock(d->gpuMutex);
        d->gpuSharedArena.acknowledgeGpuImagesUploaded(uploadedImageGen);
    }

    if (d->reflexPendingAfterPresent || d->presentDirty
        || (d->phase.load(std::memory_order_acquire) == Phase::WorkerRace && wonStreak > 0)) {
        d->reflexPendingAfterPresent = false;
        if (d->window && d->window->isExposed()) {
            const QSize curPx = (d->window->size() * d->window->devicePixelRatio()).expandedTo(QSize(1, 1));
            if (curPx != d->lastDispatchedPxSize || isLatestFailed)
                presentNow();
            else
                schedulePresent();
        }
    }
}

void QPhaseSurfacePresenter::schedulePresent()
{
    if (d->isShutDown || !d->window || !d->window->isExposed())
        return;

    d->presentDirty = true;
    const Phase curPhase = d->phase.load(std::memory_order_acquire);

    if (curPhase == Phase::GpuSteadyState) {
        std::lock_guard<std::mutex> lock(d->gpuMutex);
        if (d->gpuRenderingNow || d->gpuFrameRequested) {
            d->reflexPendingAfterPresent = true;
            return;
        }
    } else if (curPhase == Phase::CpuSteadyState) {
        std::lock_guard<std::mutex> lock(d->cpuMutex);
        if (d->cpuJobInFlight) {
            d->cpuDirtyWhileInFlight = true;
            return;
        }
    } else {
        // WorkerRace: coalesce to 0 ms event-loop turn (never paint synchronously mid-layout inside QWidget::update()!)
        if (!d->reflexTimer.isActive())
            d->reflexTimer.start(0);
        return;
    }

    if (d->reflexTimer.isActive())
        return;

    QScreen *scr = d->window->screen();
    const double hz = (scr && scr->refreshRate() > 1.0) ? double(scr->refreshRate()) : 60.0;
    const qint64 vsyncPeriodUs = std::clamp<qint64>(qint64(std::llround(1'000'000.0 / hz)), 2000, 33'333);

    const qint64 nowUs = d->createTimer.nsecsElapsed() / 1000;
    const qint64 anchorUs = d->lastVsyncAnchorUs.load(std::memory_order_acquire);
    if (anchorUs <= 0 || nowUs < anchorUs) {
        d->reflexTimer.start(0);
        return;
    }

    const qint64 estRenderUs = d->rollingBackendRenderUs.load(std::memory_order_relaxed);
    const qint64 leadWindowUs = std::clamp<qint64>(estRenderUs + 1800, 2200, vsyncPeriodUs - 800);
    const qint64 periods = ((nowUs - anchorUs) / vsyncPeriodUs) + 1;
    qint64 nextVsyncUs = anchorUs + periods * vsyncPeriodUs;
    if (nextVsyncUs <= d->lastReflexTargetVsyncUs + (vsyncPeriodUs / 2))
        nextVsyncUs = d->lastReflexTargetVsyncUs + vsyncPeriodUs;
    if (nextVsyncUs > nowUs + vsyncPeriodUs + (vsyncPeriodUs / 2))
        nextVsyncUs = nowUs + vsyncPeriodUs;

    d->lastReflexTargetVsyncUs = nextVsyncUs;
    const qint64 waitUs = (nextVsyncUs - leadWindowUs) - nowUs;
    if (waitUs <= 800) {
        d->reflexTimer.start(0);
        return;
    }

    d->reflexTimer.start(std::max(1, int((waitUs + 300) / 1000)));
}

void QPhaseSurfacePresenter::presentResizeNow()
{
    if (d->isShutDown || !d->window || !d->window->isExposed())
        return;
    const qreal dpr = d->window->devicePixelRatio();
    const QSize pxSize = (d->window->size() * dpr).expandedTo(QSize(1, 1));
    const uint64_t key = (uint64_t(uint32_t(pxSize.width())) << 32) | uint64_t(uint32_t(pxSize.height()));
    d->expectedSurfaceSizeKey.store(key, std::memory_order_release);
    if (QPlatformWindow *pw = d->window->handle(); pw && pw->isPhasedConfigureInProgress()) {
        // Inside QWaylandWindow::applyConfigure(): wait ~20 us for didApplyConfigure() + sendExposeEvent()
        // so we don't hold m_waylandCommitMutex across syncSceneTree() or wake gpuThread into a guaranteed rejection!
        d->presentDirty = true;
        return;
    }
    presentNow();
}

void QPhaseSurfacePresenter::presentNow()
{
    if (d->isShutDown || !d->window || !d->window->isExposed())
        return;

    // Re-start background Vulkan thread if this window/popup was previously hidden and re-shown
    if (!d->gpuThreadStarted && !d->gpuInitFailed.load(std::memory_order_acquire))
        startEarlyGpuWarmupThread();

    QPlatformWindow *pw = d->window->handle();
    if (!pw)
        return;

    if (pw->isPhasedConfigureInProgress()) {
        d->presentDirty = true;
        return;
    }

    const qreal dpr = d->window->devicePixelRatio();
    const QSize logicalSize = d->window->size();
    const QSize platformLogicalSize = pw->geometry().size();
    if (!platformLogicalSize.isEmpty() && platformLogicalSize != logicalSize) {
        // QWaylandWindow::applyConfigure() already wrote ack_configure + wp_viewport.set_destination(platformLogicalSize),
        // while the queued QWindowSystemInterface::handleGeometryChange() hasn't updated QWindow::size() yet.
        // Update expectedSurfaceSizeKey immediately to abort any stale-size GPU present, and wait for handleResizeEvent!
        const QSize targetPx = (platformLogicalSize * dpr).expandedTo(QSize(1, 1));
        const uint64_t targetKey = (uint64_t(uint32_t(targetPx.width())) << 32) | uint64_t(uint32_t(targetPx.height()));
        d->expectedSurfaceSizeKey.store(targetKey, std::memory_order_release);
        d->presentDirty = true;
        return;
    }

    d->reflexTimer.stop();
    d->presentDirty = false;

    static const bool sProfile = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0;
    QElapsedTimer syncTimer;
    if (sProfile)
        syncTimer.start();

    if (d->syncCallback)
        d->syncCallback(d->uiArena);

    const qint64 syncUs = sProfile ? (syncTimer.nsecsElapsed() / 1000) : 0;

    const QSize pxSize = (d->window->size() * dpr).expandedTo(QSize(1, 1));
    const uint64_t sizeKey = (uint64_t(uint32_t(pxSize.width())) << 32) | uint64_t(uint32_t(pxSize.height()));
    d->expectedSurfaceSizeKey.store(sizeKey, std::memory_order_release);
    const bool hasAlpha = d->window->requestedFormat().hasAlpha();
    const uint64_t frameTimeNs = d->uiArena.monotonicTimestampNs();

    // Apply forced affinity if requested
    const QPhaseRendererAffinity aff = d->forcedAffinity.load(std::memory_order_acquire);
    const bool blocking = d->forcedBlocking.load(std::memory_order_acquire);
    if (d->phase.load(std::memory_order_acquire) == Phase::WorkerRace) {
        if (aff == QPhaseRendererAffinity::CpuOnly) {
            finalizeWorkerWinner(Phase::CpuSteadyState);
        } else if (aff == QPhaseRendererAffinity::GpuOnly && !d->gpuInitFailed.load(std::memory_order_acquire)) {
            if (blocking || d->gpuDeviceReady.load(std::memory_order_acquire))
                finalizeWorkerWinner(Phase::GpuSteadyState);
        }
    }

    const Phase curPhase = d->phase.load(std::memory_order_acquire);

    if (curPhase == Phase::GpuSteadyState) {
        if (d->needReleaseBootResources) {
            d->needReleaseBootResources = false;
            pw->releasePhasedBootResources();
        }
        if (d->uiArena.revision() == d->lastDispatchedRev
            && pxSize == d->lastDispatchedPxSize
            && qFuzzyCompare(dpr, d->lastDispatchedDpr)
            && !d->uiArena.hasActiveAnimations()) {
            return;
        }
        d->lastDispatchedRev = d->uiArena.revision();
        d->lastDispatchedPxSize = pxSize;
        d->lastDispatchedDpr = dpr;

        if (!d->dumpedDebugPng) {
            const QByteArray dumpPath = qgetenv("QT_WIDGETS_PHASE_DUMP_PNG");
            if (!dumpPath.isEmpty()) {
                d->dumpedDebugPng = true;
                QPhaseSceneArena dumpArena;
                dumpArena.syncRenderSnapshotFrom(d->uiArena);
                QPhaseVulkanBackend offscreen;
                if (offscreen.initializeDevice(false)) {
                    const QImage img = offscreen.renderOffscreenToImage(dumpArena, pxSize, dpr, frameTimeNs);
                    img.save(QString::fromUtf8(dumpPath));
                }
            }
        }
        if (!d->uiArena.cpuBridges().empty()) {
            if (!d->cpuRasterizer)
                d->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
            d->uiArena.rasterizeDirtyCpuBridges(*d->cpuRasterizer, dpr, frameTimeNs);
        }
        {
            std::lock_guard<std::mutex> lock(d->gpuMutex);
            d->gpuSharedArena.syncRenderSnapshotFrom(d->uiArena);
            d->pendingGpuPw = pw;
            d->pendingGpuPxSize = pxSize;
            d->pendingGpuDpr = dpr;
            d->pendingGpuAlpha = hasAlpha;
            d->pendingGpuTimeNs = frameTimeNs;
            d->pendingGpuSeq = ++d->nextFrameSeq;
            d->lastDispatchedSeq = d->pendingGpuSeq;
            d->gpuFrameRequested = true;
        }
        if (sProfile) {
            std::fprintf(stderr, "[PhaseProfile UI] seq=%llu px=%dx%d sync=%.2fms prims=%zu imgs=%zu\n",
                         static_cast<unsigned long long>(d->lastDispatchedSeq),
                         pxSize.width(), pxSize.height(), double(syncUs) / 1000.0,
                         d->uiArena.drawOrder().size(), d->uiArena.activeImageSlotCount());
        }
        d->gpuCv.notify_one();
        return;
    }

    // WorkerRace or CpuSteadyState
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(d->raceMutex);
        seq = ++d->nextFrameSeq;
    }

    // Dispatch CPU wl_shm worker
    ensureCpuBootThreadStarted();
    {
        std::lock_guard<std::mutex> lock(d->cpuMutex);
        if (d->cpuJobInFlight) {
            d->cpuDirtyWhileInFlight = true;
        } else {
            QPlatformWindow::PhasedBootBuffer bootBuf{};
            if (pw->acquirePhasedBootBuffer(pxSize, dpr, &bootBuf) && bootBuf.data) {
                d->cpuRenderArena->syncRenderSnapshotFrom(d->uiArena);
                d->pendingCpuJob = PresenterCpuJob{seq, bootBuf, !hasAlpha, frameTimeNs};
                d->cpuJobPending = true;
                d->cpuJobInFlight = true;
                d->cpuCv.notify_one();
            }
        }
    }

    // Dispatch competing GPU offscreen race job if Vulkan is ready
    if (curPhase == Phase::WorkerRace && d->gpuDeviceReady.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(d->gpuMutex);
        if (!d->gpuRenderingNow) {
            d->gpuSharedArena.syncRenderSnapshotFrom(d->uiArena);
            d->pendingGpuPw = pw;
            d->pendingGpuPxSize = pxSize;
            d->pendingGpuDpr = dpr;
            d->pendingGpuAlpha = hasAlpha;
            d->pendingGpuTimeNs = frameTimeNs;
            d->pendingGpuSeq = seq;
            d->gpuFrameRequested = true;
            d->gpuCv.notify_one();
        }
    }
}

QT_END_NAMESPACE

#include "moc_qphasesurfacepresenter_p.cpp"
