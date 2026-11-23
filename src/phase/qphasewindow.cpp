// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasewindow.h"
#include "qphasescenearena_p.h"
#include "cpu/qphasecpurasterizer_p.h"
#include "vulkan/qphasevulkanbackend_p.h"

#include <qpa/qplatformwindow.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qpointer.h>
#include <QtCore/qthread.h>
#include <QtCore/qtimer.h>
#include <QtCore/qvariant.h>
#include <QtGui/qevent.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qinputmethod.h>
#include <QtGui/qscreen.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

QT_BEGIN_NAMESPACE

class QPhaseImeFocusProxy final : public QObject
{
    Q_OBJECT
public:
    explicit QPhaseImeFocusProxy(QPhaseWindow *win)
        : QObject(win)
        , m_window(win)
    {
    }

    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery query) const
    {
        if (QPhaseView *fv = m_window->focusedView())
            return fv->inputMethodQuery(query);
        return false;
    }

protected:
    bool event(QEvent *ev) override
    {
        if (ev->type() == QEvent::InputMethod) {
            if (QPhaseView *fv = m_window->focusedView()) {
                fv->onInputMethod(static_cast<QInputMethodEvent *>(ev));
                return true;
            }
        } else if (ev->type() == QEvent::InputMethodQuery) {
            auto *qe = static_cast<QInputMethodQueryEvent *>(ev);
            QPhaseView *fv = m_window->focusedView();
            const Qt::InputMethodQueries queries = qe->queries();
            for (int bit = 0; bit < 31; ++bit) {
                const auto q = Qt::InputMethodQuery(1u << bit);
                if (queries & q)
                    qe->setValue(q, fv ? fv->inputMethodQuery(q) : QVariant(false));
            }
            qe->accept();
            return true;
        }
        return QObject::event(ev);
    }

private:
    QPhaseWindow *m_window = nullptr;
};

struct CpuWorkerJob {
    uint64_t seq = 0;
    bool bothWereReady = false;
    QPlatformWindow::PhasedBootBuffer bootBuf{};
    bool forceOpaqueAlpha = true;
    uint64_t frameTimeNs = 0;
    qint64 sampleTimestampUs = 0;
};

class QPhaseWindowPrivate
{
public:
    // Main UI-thread scene arena (mutated exclusively on UI thread)
    QPhaseSceneArena uiArena;
    std::unique_ptr<QPhaseView> rootView;
    QPhaseView *focusedView = nullptr;
    QPhaseView *mouseGrabber = nullptr;
    QPhaseImeFocusProxy *imeProxy = nullptr;

    // =========================================================================
    // VSync-Deadline Input/Frame Pacer ("UI Reflex") State
    // =========================================================================
    QTimer reflexDeadlineTimer;
    std::atomic<bool> lowLatencyReflexEnabled{true};
    std::atomic<qint64> lastVsyncAnchorUs{0};
    std::atomic<qint64> rollingBackendRenderUs{1800};
    qint64 lastReflexTargetVsyncUs = 0;
    bool reflexUpdatePending = false;
    bool reflexPendingAfterPresent = false;
    bool continuousSampleRequested = false;
    bool insideBeforeFrameSample = false;

    // =========================================================================
    // Competitive Worker Coordinator State (Leaf Mutex: raceMutex)
    // =========================================================================
    std::atomic<QPhaseWindow::Phase> phase{QPhaseWindow::Phase::WorkerRace};
    std::mutex raceMutex;
    uint64_t lastClaimedSeq = 0;
    std::atomic<uint64_t> latestDispatchedSeq{0};
    std::atomic<uint64_t> lastClaimedAtomicSeq{0};
    std::atomic<uint64_t> abortCpuUpToSeq{0};
    std::atomic<uint64_t> abortGpuUpToSeq{0};
    std::atomic<bool> gpuOwnsSurface{false};
    std::atomic<bool> winnerStillPresenting{false};
    std::atomic<bool> raceDirtyWhileInFlight{false};
    quint8 gpuWinStreak = 0;
    quint8 cpuWinStreak = 0;

    // =========================================================================
    // Worker A: Multi-Threaded CPU wl_shm Worker + Adaptive Tile Pool
    // =========================================================================
    std::unique_ptr<QPhaseCpuRasterizer> cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
    std::unique_ptr<QPhaseSceneArena> cpuRenderArena = std::make_unique<QPhaseSceneArena>();
    QHash<void *, QPhaseDamageTracker> cpuBufferTrackers;
    QPhaseDamageTracker cpuSurfaceTracker;
    std::thread cpuBootThread;
    std::mutex cpuMutex;
    std::condition_variable cpuCv;
    CpuWorkerJob pendingCpuJob{};
    bool cpuThreadStarted = false;
    bool cpuJobReady = false;
    bool cpuJobInFlight = false;
    bool cpuDirtyWhileInFlight = false;
    bool cpuStopRequested = false;

    // =========================================================================
    // Worker B: Persistent Vulkan 1.3 Worker (Started ASAP in Constructor)
    // =========================================================================
    std::unique_ptr<QPhaseVulkanBackend> vulkanBackend;
    QPhaseSceneArena gpuSharedArena;
    QPhaseSceneArena gpuWorkerArena;
    std::thread gpuRenderThread;
    std::mutex gpuMutex;
    std::condition_variable gpuCv;
    std::condition_variable gpuStateAckCv;

    QPlatformWindow *gpuPlatformWindow = nullptr;
    QSize gpuPixelSize{0, 0};
    qreal gpuDpr = 1.0;
    bool gpuHasAlpha = false;
    uint64_t gpuTargetSeq = 0;
    uint64_t gpuLastProcessedSeq = 0;
    qint64 gpuSampleTimestampUs = 0;
    bool gpuTargetBothReady = false;
    uint32_t gpuWarmFrames = 0;
    std::atomic<bool> gpuDeviceReady{false};
    std::atomic<bool> gpuSceneWarm{false};
    bool gpuThreadStarted = false;
    bool gpuSwapchainAttached = false;
    bool gpuCanPresent = false;
    bool gpuFrameRequested = false;
    bool gpuRenderingNow = false;
    bool gpuStopRequested = false;

    // Lifecycle & Per-Frame Timing Telemetry
    QElapsedTimer windowCreateTimer;
    QElapsedTimer exposeTimer;
    std::atomic<bool> exposeTimerStarted{false};

    std::atomic<qint64> showToFirstFrameCommitUs{0};
    std::atomic<qint64> showToFirstCpuCommitUs{0};
    std::atomic<qint64> showToGpuCutoverUs{0};
    std::atomic<qint64> gpuAsyncWarmupUs{0};
    std::atomic<quint64> cpuBootFramesPresented{0};
    std::atomic<quint64> gpuSteadyFramesPresented{0};
    std::atomic<quint64> cpuFramesPresented{0};
    std::atomic<quint64> gpuFramesPresented{0};
    std::atomic<quint64> abortedCpuFrames{0};
    std::atomic<quint64> abortedGpuFrames{0};
    std::atomic<int> simulatedGpuDelayMs{0};
    std::atomic<int> simulatedGpuFrameDelayMs{0};
    quint32 recordedCpuTileThreads = 1;

    mutable std::mutex timingsMutex;
    std::vector<QPhaseWindow::FrameTiming> recordedTimings;
    qint64 lastPresentedSinceCreateUs = -1;

    bool uiSetupDone = false;
    bool sceneTreeDirty = true;
    QPhaseRendererAffinity windowForcedAffinity = QPhaseRendererAffinity::Any;
    bool windowForcedBlocking = false;
    std::atomic<QPhaseRendererAffinity> activeAffinityConstraint{QPhaseRendererAffinity::Any};
    std::atomic<bool> activeConstraintBlocking{false};
    std::atomic<bool> gpuInitFailed{false};
    bool pendingDeferredCutoverSignal = false;
    bool warnedAffinityConflict = false;

    void updateRollingRenderEstimate(qint64 renderCostUs) noexcept
    {
        const qint64 clamped = std::clamp<qint64>(renderCostUs, 200, 50'000);
        const qint64 prev = rollingBackendRenderUs.load(std::memory_order_relaxed);
        rollingBackendRenderUs.store((prev * 3 + clamped) / 4, std::memory_order_relaxed);
    }

    bool tryClaimRaceFrame(uint64_t seq,
                           QPhaseWindow::Worker worker,
                           bool bothWereReady,
                           quint8 *outWinStreak,
                           QPhaseWindow::Phase *outLockedPhase)
    {
        std::lock_guard<std::mutex> lock(raceMutex);
        if (seq <= lastClaimedSeq)
            return false;

        lastClaimedSeq = seq;
        lastClaimedAtomicSeq.store(seq, std::memory_order_release);
        winnerStillPresenting.store(true, std::memory_order_release);

        if (worker == QPhaseWindow::Worker::GpuVulkan) {
            abortCpuUpToSeq.store(seq, std::memory_order_release);
            gpuOwnsSurface.store(true, std::memory_order_release);
            ++gpuWinStreak;
            cpuWinStreak = 0;
            if (outWinStreak)
                *outWinStreak = gpuWinStreak;
            if (outLockedPhase) {
                *outLockedPhase = (gpuWinStreak >= QPhaseWindow::kWorkerLockInStreak)
                        ? QPhaseWindow::Phase::GpuSteadyState
                        : QPhaseWindow::Phase::WorkerRace;
            }
        } else {
            abortGpuUpToSeq.store(seq, std::memory_order_release);
            if (bothWereReady) {
                ++cpuWinStreak;
                gpuWinStreak = 0;
            }
            if (outWinStreak)
                *outWinStreak = bothWereReady ? cpuWinStreak : 0;
            if (outLockedPhase) {
                *outLockedPhase = (bothWereReady && cpuWinStreak >= QPhaseWindow::kWorkerLockInStreak)
                        ? QPhaseWindow::Phase::CpuSteadyState
                        : QPhaseWindow::Phase::WorkerRace;
            }
        }
        return true;
    }

    void recordFrameCommit(QPhaseWindow::Phase framePhase,
                           QPhaseWindow::Worker presentedBy,
                           quint8 streak,
                           bool bothReady,
                           bool opponentAborted,
                           qint64 workUs,
                           qint64 gpuHwUs,
                           qint64 sampleStartUs,
                           quint64 syncedBytes,
                           quint64 damagedPx,
                           quint64 totalPx)
    {
        const qint64 nowCreateUs = windowCreateTimer.nsecsElapsed() / 1000;
        const qint64 nowExposeUs = exposeTimerStarted.load(std::memory_order_acquire)
                ? (exposeTimer.nsecsElapsed() / 1000)
                : 0;

        lastVsyncAnchorUs.store(nowCreateUs, std::memory_order_release);
        updateRollingRenderEstimate(presentedBy == QPhaseWindow::Worker::GpuVulkan
                                    ? std::max(workUs, gpuHwUs)
                                    : workUs);

        qint64 expectedZero = 0;
        showToFirstFrameCommitUs.compare_exchange_strong(expectedZero, nowExposeUs, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(timingsMutex);
        if (recordedTimings.size() >= 512)
            return;

        QPhaseWindow::FrameTiming ft{};
        ft.frameIndex = quint32(recordedTimings.size() + 1);
        ft.phase = framePhase;
        ft.presentedBy = presentedBy;
        ft.winStreak = streak;
        ft.bothWorkersReady = bothReady;
        ft.opponentAborted = opponentAborted;
        ft.sinceWindowCreateUs = nowCreateUs;
        ft.sinceExposeUs = nowExposeUs;
        ft.interFrameDeltaUs = (lastPresentedSinceCreateUs >= 0) ? (nowCreateUs - lastPresentedSinceCreateUs) : 0;
        ft.backendWorkUs = workUs;
        ft.gpuHardwareUs = gpuHwUs;
        ft.sampleToPresentUs = (sampleStartUs > 0 && nowCreateUs >= sampleStartUs)
                ? (nowCreateUs - sampleStartUs)
                : workUs;
        ft.gpuSyncedBytes = syncedBytes;
        ft.damagedPixels = damagedPx;
        ft.totalViewportPixels = totalPx;
        lastPresentedSinceCreateUs = nowCreateUs;
        recordedTimings.push_back(ft);
    }

    void stopCpuBootThread()
    {
        if (cpuBootThread.joinable()) {
            abortCpuUpToSeq.store(UINT64_MAX, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(cpuMutex);
                cpuStopRequested = true;
                cpuJobReady = false;
            }
            cpuCv.notify_all();
            cpuBootThread.join();
        }
        cpuJobInFlight = false;
        cpuThreadStarted = false;
        cpuBufferTrackers.clear();
        cpuSurfaceTracker.reset();
        cpuRenderArena.reset();
    }

    void pauseGpuPresentation()
    {
        if (!gpuThreadStarted)
            return;
        std::unique_lock<std::mutex> lock(gpuMutex);
        gpuCanPresent = false;
        gpuStateAckCv.wait(lock, [this]() { return !gpuRenderingNow; });
    }

    void stopGpuRenderThread()
    {
        if (gpuRenderThread.joinable()) {
            abortGpuUpToSeq.store(UINT64_MAX, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(gpuMutex);
                gpuCanPresent = false;
                gpuStopRequested = true;
            }
            gpuCv.notify_all();
            gpuRenderThread.join();
        }
        gpuThreadStarted = false;
        gpuDeviceReady.store(false, std::memory_order_release);
        gpuSceneWarm.store(false, std::memory_order_release);
    }
};

void qt_phase_notify_window_dirty(QPhaseWindow *window)
{
    if (!window)
        return;
    window->d_phase->sceneTreeDirty = true;
    if (window->isExposed() && !window->d_phase->insideBeforeFrameSample)
        window->scheduleReflexOrImmediateUpdate();
}

void qt_phase_set_focused_view(QPhaseWindow *window, QPhaseView *view)
{
    if (!window || window->d_phase->focusedView == view)
        return;

    if (QPhaseView *old = window->d_phase->focusedView)
        old->setFocusedInternal(false);

    window->d_phase->focusedView = view;
    if (view)
        view->setFocusedInternal(true);

    if (QInputMethod *im = QGuiApplication::inputMethod()) {
        im->update(Qt::ImQueryAll);
        if (view)
            im->show();
    }
    Q_EMIT window->focusObjectChanged(window->focusObject());
}

QPhaseWindow::QPhaseWindow(QWindow *parent)
    : QWindow(parent)
    , d_phase(std::make_unique<QPhaseWindowPrivate>())
{
    d_phase->windowCreateTimer.start();
    setSurfaceType(QSurface::PhasedSurface);

    d_phase->reflexDeadlineTimer.setTimerType(Qt::PreciseTimer);
    d_phase->reflexDeadlineTimer.setSingleShot(true);
    QObject::connect(&d_phase->reflexDeadlineTimer, &QTimer::timeout, this, [this]() {
        d_phase->reflexUpdatePending = false;
        if (isExposed())
            renderFrame();
    });

    d_phase->recordedCpuTileThreads = d_phase->cpuRasterizer ? d_phase->cpuRasterizer->maxTileThreadCount() : 1u;
    d_phase->recordedTimings.reserve(256);
    d_phase->imeProxy = new QPhaseImeFocusProxy(this);
    d_phase->rootView = std::make_unique<QPhaseView>();
    d_phase->rootView->setOwningWindow(this);

    bool envOk = false;
    const int envDelay = qEnvironmentVariableIntValue("QT_PHASE_GPU_DELAY_MS", &envOk);
    if (envOk && envDelay > 0)
        d_phase->simulatedGpuDelayMs.store(envDelay, std::memory_order_relaxed);

    bool envFrameDelayOk = false;
    const int envFrameDelay = qEnvironmentVariableIntValue("QT_PHASE_GPU_FRAME_DELAY_MS", &envFrameDelayOk);
    if (envFrameDelayOk && envFrameDelay > 0)
        d_phase->simulatedGpuFrameDelayMs.store(envFrameDelay, std::memory_order_relaxed);

    if (qEnvironmentVariableIntValue("QT_PHASE_NO_REFLEX") > 0)
        d_phase->lowLatencyReflexEnabled.store(false, std::memory_order_relaxed);

    startEarlyGpuWarmupThread();
}

QPhaseWindow::~QPhaseWindow()
{
    d_phase->reflexDeadlineTimer.stop();
    d_phase->stopCpuBootThread();
    d_phase->stopGpuRenderThread();
    d_phase->cpuRasterizer.reset();
    d_phase->vulkanBackend.reset();

    d_phase->focusedView = nullptr;
    d_phase->mouseGrabber = nullptr;
    if (d_phase->rootView)
        d_phase->rootView->detachFromArena();
}

void QPhaseWindow::setLowLatencyReflexMode(bool enabled)
{
    d_phase->lowLatencyReflexEnabled.store(enabled, std::memory_order_release);
    if (!enabled && d_phase->reflexDeadlineTimer.isActive()) {
        d_phase->reflexDeadlineTimer.stop();
        d_phase->reflexUpdatePending = false;
        if (isExposed())
            requestUpdate();
    }
}

bool QPhaseWindow::isLowLatencyReflexMode() const noexcept
{
    return d_phase->lowLatencyReflexEnabled.load(std::memory_order_acquire);
}

void QPhaseWindow::requestFrameSample()
{
    d_phase->continuousSampleRequested = true;
    if (isExposed() && !d_phase->insideBeforeFrameSample)
        scheduleReflexOrImmediateUpdate();
}

void QPhaseWindow::setForcedRendererAffinity(QPhaseRendererAffinity affinity, bool blocking)
{
    d_phase->windowForcedAffinity = affinity;
    d_phase->windowForcedBlocking = blocking;
    if (isExposed()) {
        applyRendererAffinityConstraints();
        scheduleReflexOrImmediateUpdate();
    }
}

QPhaseRendererAffinity QPhaseWindow::forcedRendererAffinity() const noexcept
{
    return d_phase->windowForcedAffinity;
}

bool QPhaseWindow::isForcedBackendBlocking() const noexcept
{
    return d_phase->windowForcedBlocking;
}

void QPhaseWindow::beforeFrameSample()
{
    Q_EMIT frameSampleRequested();
}

void QPhaseWindow::scheduleReflexOrImmediateUpdate()
{
    if (!isExposed())
        return;

    const Phase curPhase = d_phase->phase.load(std::memory_order_acquire);
    const bool useReflex = d_phase->lowLatencyReflexEnabled.load(std::memory_order_acquire)
            && (curPhase != Phase::WorkerRace);

    if (!useReflex) {
        requestUpdate();
        return;
    }

    // Strict zero-frame-queue rule ("UI Reflex"): never sample the next frame while the GPU worker
    // is still rendering/presenting the current frame, preventing 1-frame swapchain queue buildup!
    if (curPhase == Phase::GpuSteadyState) {
        std::lock_guard<std::mutex> lock(d_phase->gpuMutex);
        if (d_phase->gpuRenderingNow || d_phase->gpuFrameRequested) {
            d_phase->reflexPendingAfterPresent = true;
            return;
        }
    } else if (curPhase == Phase::CpuSteadyState) {
        std::lock_guard<std::mutex> lock(d_phase->cpuMutex);
        if (d_phase->cpuJobInFlight) {
            d_phase->cpuDirtyWhileInFlight = true;
            return;
        }
    }

    if (d_phase->reflexDeadlineTimer.isActive())
        return;

    QScreen *scr = screen();
    const double hz = (scr && scr->refreshRate() > 1.0) ? double(scr->refreshRate()) : 60.0;
    const qint64 vsyncPeriodUs = std::clamp<qint64>(qint64(std::llround(1'000'000.0 / hz)), 2000, 33'333);

    const qint64 nowUs = d_phase->windowCreateTimer.nsecsElapsed() / 1000;
    const qint64 anchorUs = d_phase->lastVsyncAnchorUs.load(std::memory_order_acquire);
    if (anchorUs <= 0 || nowUs < anchorUs) {
        renderFrame();
        return;
    }

    // Lead window = rolling backend render cost + 1.8 ms compositor commit margin
    const qint64 estRenderUs = d_phase->rollingBackendRenderUs.load(std::memory_order_relaxed);
    const qint64 leadWindowUs = std::clamp<qint64>(estRenderUs + 1800, 2200, vsyncPeriodUs - 800);

    const qint64 elapsedSinceAnchorUs = nowUs - anchorUs;
    const qint64 periods = (elapsedSinceAnchorUs / vsyncPeriodUs) + 1;
    qint64 nextVsyncUs = anchorUs + periods * vsyncPeriodUs;
    if (nextVsyncUs <= d_phase->lastReflexTargetVsyncUs + (vsyncPeriodUs / 2))
        nextVsyncUs = d_phase->lastReflexTargetVsyncUs + vsyncPeriodUs;
    // Never schedule more than 1.5 VSync intervals into the future
    if (nextVsyncUs > nowUs + vsyncPeriodUs + (vsyncPeriodUs / 2))
        nextVsyncUs = nowUs + vsyncPeriodUs;

    d_phase->lastReflexTargetVsyncUs = nextVsyncUs;
    const qint64 sampleDeadlineUs = nextVsyncUs - leadWindowUs;
    const qint64 waitUs = sampleDeadlineUs - nowUs;

    if (waitUs <= 800) {
        renderFrame();
        return;
    }

    d_phase->reflexUpdatePending = true;
    const int delayMs = std::max(1, int((waitUs + 300) / 1000));
    d_phase->reflexDeadlineTimer.start(delayMs);
}

void QPhaseWindow::startEarlyGpuWarmupThread()
{
    if (d_phase->gpuThreadStarted)
        return;

    d_phase->gpuThreadStarted = true;
    d_phase->vulkanBackend = std::make_unique<QPhaseVulkanBackend>();
    QPhaseWindowPrivate *priv = d_phase.get();
    QPointer<QPhaseWindow> safeThis(this);

    d_phase->gpuRenderThread = std::thread([priv, safeThis]() {
        QElapsedTimer gpuTimer;
        gpuTimer.start();

        const bool ok = priv->vulkanBackend->initializeDevice(true);
        const qint64 realHwWarmupUs = gpuTimer.nsecsElapsed() / 1000;
        priv->gpuAsyncWarmupUs.store(realHwWarmupUs, std::memory_order_relaxed);
        if (!ok) {
            priv->gpuInitFailed.store(true, std::memory_order_release);
            if (safeThis) {
                QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                    if (!safeThis)
                        return;
                    // Even if Blocking GpuOnly had pre-locked GpuSteadyState, recover gracefully onto CPU!
                    if (!safeThis->d_phase->cpuRasterizer)
                        safeThis->d_phase->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
                    if (!safeThis->d_phase->cpuRenderArena)
                        safeThis->d_phase->cpuRenderArena = std::make_unique<QPhaseSceneArena>();
                    safeThis->d_phase->phase.store(Phase::WorkerRace, std::memory_order_release);
                    safeThis->finalizeWorkerWinner(Phase::CpuSteadyState);
                    if (safeThis->isExposed())
                        safeThis->renderFrame();
                }, Qt::QueuedConnection);
            }
            return;
        }

        const int simDelayMs = priv->simulatedGpuDelayMs.load(std::memory_order_relaxed);
        if (simDelayMs > 0) {
            std::unique_lock<std::mutex> lock(priv->gpuMutex);
            priv->gpuCv.wait_for(lock, std::chrono::milliseconds(simDelayMs), [priv]() {
                return priv->gpuStopRequested;
            });
            if (priv->gpuStopRequested)
                return;
        }

        priv->gpuDeviceReady.store(true, std::memory_order_release);

        if (safeThis) {
            QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                if (!safeThis)
                    return;
                if (safeThis->d_phase->pendingDeferredCutoverSignal) {
                    safeThis->d_phase->pendingDeferredCutoverSignal = false;
                    safeThis->d_phase->showToGpuCutoverUs.store(
                            safeThis->d_phase->windowCreateTimer.nsecsElapsed() / 1000,
                            std::memory_order_relaxed);
                    Q_EMIT safeThis->cutoverCompleted();
                } else if (safeThis->currentPhase() == Phase::WorkerRace) {
                    safeThis->applyRendererAffinityConstraints();
                    if (safeThis->isExposed())
                        safeThis->renderFrame();
                }
            }, Qt::QueuedConnection);
        }

        while (true) {
            QPlatformWindow *pw = nullptr;
            QSize pxSize;
            qreal dpr = 1.0;
            bool hasAlpha = false;
            uint64_t seq = 0;
            qint64 sampleTsUs = 0;
            bool bothReady = false;
            Phase curPhase = Phase::WorkerRace;

            {
                std::unique_lock<std::mutex> lock(priv->gpuMutex);
                priv->gpuCv.wait(lock, [priv]() {
                    if (priv->gpuStopRequested)
                        return true;
                    if (!priv->gpuCanPresent || !priv->gpuPlatformWindow)
                        return false;
                    if (priv->phase.load(std::memory_order_acquire) == Phase::GpuSteadyState)
                        return priv->gpuFrameRequested || priv->gpuTargetSeq > priv->gpuLastProcessedSeq;
                    return priv->gpuTargetSeq > priv->gpuLastProcessedSeq;
                });
                if (priv->gpuStopRequested)
                    break;

                curPhase = priv->phase.load(std::memory_order_acquire);
                if (curPhase == Phase::CpuSteadyState)
                    break;

                seq = priv->gpuTargetSeq;
                bothReady = priv->gpuTargetBothReady;
                sampleTsUs = priv->gpuSampleTimestampUs;
                priv->gpuLastProcessedSeq = seq;
                priv->gpuFrameRequested = false;
                priv->gpuRenderingNow = true;
                pw = priv->gpuPlatformWindow;
                pxSize = priv->gpuPixelSize;
                dpr = priv->gpuDpr;
                hasAlpha = priv->gpuHasAlpha;
                priv->gpuWorkerArena.syncRenderSnapshotFrom(priv->gpuSharedArena);
            }

            const uint64_t frameNs = priv->gpuWorkerArena.monotonicTimestampNs();
            const quint64 totalVpPixels = quint64(std::max(0, pxSize.width()))
                    * quint64(std::max(0, pxSize.height()));

            if (curPhase == Phase::GpuSteadyState) {
                if (!priv->gpuSwapchainAttached) {
                    priv->vulkanBackend->releaseRaceResources();
                    priv->gpuSwapchainAttached = priv->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);
                }

                QPhaseVulkanBackend::GpuFrameMetrics gm{};
                const bool presented = priv->gpuSwapchainAttached
                        && priv->vulkanBackend->renderAndPresentWindow(
                                pw, priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameNs, &gm);
                if (presented) {
                    priv->recordFrameCommit(Phase::GpuSteadyState, Worker::GpuVulkan,
                                            kWorkerLockInStreak, true, false,
                                            gm.cpuSubmitWorkUs, gm.gpuHardwareUs, sampleTsUs,
                                            gm.syncedSceneBytes, gm.damagedPixels, totalVpPixels);
                    priv->gpuFramesPresented.fetch_add(1, std::memory_order_relaxed);
                    priv->gpuSteadyFramesPresented.fetch_add(1, std::memory_order_relaxed);
                }

                const bool animKeepAlive = presented && priv->gpuCanPresent && priv->gpuWorkerArena.hasActiveAnimations();
                const bool useReflex = priv->lowLatencyReflexEnabled.load(std::memory_order_acquire);

                {
                    std::lock_guard<std::mutex> lock(priv->gpuMutex);
                    priv->gpuRenderingNow = false;
                    // When Reflex is OFF and only GPU animations are running, self-loop directly on GPU thread;
                    // when Reflex is ON, notify UI thread to schedule the next VSync-deadline frame sample!
                    if (animKeepAlive && !useReflex)
                        priv->gpuFrameRequested = true;
                }
                priv->gpuStateAckCv.notify_all();

                if (presented && useReflex && safeThis) {
                    QMetaObject::invokeMethod(safeThis.data(), [safeThis, animKeepAlive]() {
                        if (!safeThis || !safeThis->isExposed())
                            return;
                        const bool hadPending = safeThis->d_phase->reflexPendingAfterPresent;
                        safeThis->d_phase->reflexPendingAfterPresent = false;
                        if (hadPending || animKeepAlive
                            || safeThis->d_phase->continuousSampleRequested
                            || safeThis->d_phase->sceneTreeDirty) {
                            safeThis->scheduleReflexOrImmediateUpdate();
                        }
                    }, Qt::QueuedConnection);
                }
                continue;
            }

            // =====================================================================
            // Competitive Worker Race Path (curPhase == Phase::WorkerRace)
            // =====================================================================
            if (priv->lastClaimedAtomicSeq.load(std::memory_order_acquire) >= seq) {
                priv->abortedGpuFrames.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(priv->gpuMutex);
                    priv->gpuRenderingNow = false;
                }
                priv->gpuStateAckCv.notify_all();
                continue;
            }

            const int simFrameDelayMs = priv->simulatedGpuFrameDelayMs.load(std::memory_order_relaxed);
            const bool ownsSurf = priv->gpuOwnsSurface.load(std::memory_order_acquire);
            Phase lockedPhase = Phase::WorkerRace;

            if (!ownsSurf) {
                QPhaseVulkanBackend::GpuFrameMetrics gm{};
                const bool rendered = priv->vulkanBackend->benchmarkFrameOffscreen(
                        priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameNs, &gm);
                if (rendered && ++priv->gpuWarmFrames >= 2u)
                    priv->gpuSceneWarm.store(true, std::memory_order_release);

                qint64 extraDelayUs = 0;
                if (rendered && simFrameDelayMs > 0) {
                    QElapsedTimer delayTimer;
                    delayTimer.start();
                    std::unique_lock<std::mutex> lock(priv->gpuMutex);
                    priv->gpuCv.wait_for(lock, std::chrono::milliseconds(simFrameDelayMs), [priv, seq]() {
                        return priv->gpuStopRequested
                                || priv->abortGpuUpToSeq.load(std::memory_order_acquire) >= seq;
                    });
                    extraDelayUs = delayTimer.nsecsElapsed() / 1000;
                }

                bool won = false;
                quint8 wonStreak = 0;
                if (rendered && !priv->gpuStopRequested
                    && priv->abortGpuUpToSeq.load(std::memory_order_acquire) < seq) {
                    won = priv->tryClaimRaceFrame(seq, Worker::GpuVulkan, bothReady, &wonStreak, &lockedPhase);
                }

                if (won) {
                    priv->vulkanBackend->releaseRaceResources();
                    if (!priv->gpuSwapchainAttached)
                        priv->gpuSwapchainAttached = priv->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);

                    bool presented = false;
                    QPhaseVulkanBackend::GpuFrameMetrics presGm{};
                    if (priv->gpuSwapchainAttached) {
                        presented = priv->vulkanBackend->renderAndPresentWindow(
                                pw, priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameNs, &presGm);
                    }
                    if (presented) {
                        priv->recordFrameCommit(Phase::WorkerRace, Worker::GpuVulkan,
                                                wonStreak, bothReady, true,
                                                gm.cpuSubmitWorkUs + extraDelayUs,
                                                gm.gpuHardwareUs + extraDelayUs,
                                                sampleTsUs,
                                                gm.syncedSceneBytes,
                                                presGm.damagedPixels,
                                                totalVpPixels);
                        priv->gpuFramesPresented.fetch_add(1, std::memory_order_relaxed);
                        if (lockedPhase == Phase::GpuSteadyState)
                            priv->gpuSteadyFramesPresented.fetch_add(1, std::memory_order_relaxed);
                    }
                    priv->winnerStillPresenting.store(false, std::memory_order_release);
                } else {
                    priv->abortedGpuFrames.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                qint64 extraDelayUs = 0;
                if (simFrameDelayMs > 0) {
                    QElapsedTimer delayTimer;
                    delayTimer.start();
                    std::unique_lock<std::mutex> lock(priv->gpuMutex);
                    priv->gpuCv.wait_for(lock, std::chrono::milliseconds(simFrameDelayMs), [priv, seq]() {
                        return priv->gpuStopRequested
                                || priv->abortGpuUpToSeq.load(std::memory_order_acquire) >= seq;
                    });
                    extraDelayUs = delayTimer.nsecsElapsed() / 1000;
                }

                if (!priv->gpuSwapchainAttached)
                    priv->gpuSwapchainAttached = priv->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);

                bool won = false;
                quint8 wonStreak = 0;
                QPhaseVulkanBackend::GpuFrameMetrics gm{};

                const bool presented = priv->gpuSwapchainAttached
                        && priv->vulkanBackend->renderAndPresentWindow(
                                pw, priv->gpuWorkerArena, pxSize, dpr, hasAlpha, frameNs,
                                nullptr,
                                /*waitForGpuCompletion=*/true,
                                [&](const QPhaseVulkanBackend::GpuFrameMetrics &readyGm) {
                                    gm = readyGm;
                                    gm.cpuSubmitWorkUs += extraDelayUs;
                                    gm.gpuHardwareUs += extraDelayUs;
                                    won = priv->tryClaimRaceFrame(seq, Worker::GpuVulkan, bothReady,
                                                                  &wonStreak, &lockedPhase);
                                });

                if (won) {
                    if (presented) {
                        priv->recordFrameCommit(Phase::WorkerRace, Worker::GpuVulkan,
                                                wonStreak, bothReady, true,
                                                gm.cpuSubmitWorkUs, gm.gpuHardwareUs, sampleTsUs,
                                                gm.syncedSceneBytes, gm.damagedPixels, totalVpPixels);
                        priv->gpuFramesPresented.fetch_add(1, std::memory_order_relaxed);
                        if (lockedPhase == Phase::GpuSteadyState)
                            priv->gpuSteadyFramesPresented.fetch_add(1, std::memory_order_relaxed);
                    }
                    priv->winnerStillPresenting.store(false, std::memory_order_release);
                } else {
                    priv->abortedGpuFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }

            {
                std::lock_guard<std::mutex> lock(priv->gpuMutex);
                priv->gpuRenderingNow = false;
            }
            priv->gpuStateAckCv.notify_all();

            if (lockedPhase != Phase::WorkerRace && safeThis) {
                QMetaObject::invokeMethod(safeThis.data(), [safeThis, lockedPhase]() {
                    if (safeThis)
                        safeThis->finalizeWorkerWinner(lockedPhase);
                }, Qt::QueuedConnection);
            } else if ((priv->raceDirtyWhileInFlight.exchange(false, std::memory_order_acq_rel)
                        || priv->gpuWorkerArena.hasActiveAnimations()
                        || priv->phase.load(std::memory_order_acquire) == Phase::WorkerRace)
                       && safeThis) {
                QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
                    if (safeThis && safeThis->isExposed())
                        safeThis->requestUpdate();
                }, Qt::QueuedConnection);
            }
        }
    });
}

void QPhaseWindow::ensureCpuWorkerThread()
{
    if (d_phase->cpuThreadStarted || !d_phase->cpuRasterizer)
        return;

    d_phase->cpuThreadStarted = true;
    QPhaseWindowPrivate *priv = d_phase.get();
    QPointer<QPhaseWindow> safeThis(this);

    d_phase->cpuBootThread = std::thread([priv, safeThis]() {
        while (true) {
            CpuWorkerJob job{};
            {
                std::unique_lock<std::mutex> lock(priv->cpuMutex);
                priv->cpuCv.wait(lock, [priv]() {
                    return priv->cpuStopRequested || priv->cpuJobReady;
                });
                if (priv->cpuStopRequested)
                    break;
                job = priv->pendingCpuJob;
                priv->cpuJobReady = false;
            }

            const Phase curPhase = priv->phase.load(std::memory_order_acquire);
            if (curPhase == Phase::GpuSteadyState) {
                std::lock_guard<std::mutex> lock(priv->cpuMutex);
                priv->cpuJobInFlight = false;
                break;
            }

            QElapsedTimer cpuWorkTimer;
            cpuWorkTimer.start();
            bool completed = false;
            QRegion logicalSurfaceDmg;
            quint64 damagedPx = 0;
            const QSize pxSize(job.bootBuf.width, job.bootBuf.height);
            const quint64 totalPx = quint64(std::max(0, pxSize.width()))
                    * quint64(std::max(0, pxSize.height()));

            if (priv->cpuRasterizer && priv->cpuRenderArena && job.bootBuf.data) {
                bool isFullBufDmg = true;
                QPhaseDamageTracker &bufTracker = priv->cpuBufferTrackers[job.bootBuf.handle];
                const QRegion bufDmg = bufTracker.computeAndAdvance(
                        *priv->cpuRenderArena, pxSize, job.bootBuf.devicePixelRatio,
                        job.frameTimeNs, &isFullBufDmg, QPhaseRendererAffinity::CpuOnly);

                const std::atomic<uint64_t> *abortPtr =
                        (curPhase == Phase::WorkerRace) ? &priv->abortCpuUpToSeq : nullptr;
                completed = priv->cpuRasterizer->renderToBuffer(
                        *priv->cpuRenderArena,
                        job.bootBuf.data,
                        job.bootBuf.width,
                        job.bootBuf.height,
                        job.bootBuf.bytesPerLine,
                        job.bootBuf.devicePixelRatio,
                        job.forceOpaqueAlpha,
                        job.frameTimeNs,
                        abortPtr,
                        job.seq,
                        isFullBufDmg ? nullptr : &bufDmg);

                if (!completed) {
                    bufTracker.reset();
                } else {
                    const QRegion surfDmg = priv->cpuSurfaceTracker.computeAndAdvance(
                            *priv->cpuRenderArena, pxSize, job.bootBuf.devicePixelRatio,
                            job.frameTimeNs, nullptr, QPhaseRendererAffinity::CpuOnly);
                    damagedPx = QPhaseDamageTracker::regionPixelArea(surfDmg);
                    const QSize logicalLim(
                            int(std::ceil(qreal(pxSize.width()) / std::max<qreal>(job.bootBuf.devicePixelRatio, 0.01))),
                            int(std::ceil(qreal(pxSize.height()) / std::max<qreal>(job.bootBuf.devicePixelRatio, 0.01))));
                    logicalSurfaceDmg = QPhaseDamageTracker::deviceToLogicalRegion(
                            surfDmg, job.bootBuf.devicePixelRatio, logicalLim);
                }
            }
            const qint64 cpuWorkUs = cpuWorkTimer.nsecsElapsed() / 1000;
            const qint64 sampleTsUs = job.sampleTimestampUs;

            if (curPhase == Phase::WorkerRace) {
                if (!completed) {
                    priv->abortedCpuFrames.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(priv->cpuMutex);
                    priv->cpuJobInFlight = false;
                    continue;
                }

                quint8 wonStreak = 0;
                Phase lockedPhase = Phase::WorkerRace;
                const bool won = priv->tryClaimRaceFrame(
                        job.seq, Worker::CpuShm, job.bothWereReady, &wonStreak, &lockedPhase);
                if (!won) {
                    priv->abortedCpuFrames.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(priv->cpuMutex);
                    priv->cpuJobInFlight = false;
                    continue;
                }

                priv->gpuCv.notify_all();

                if (safeThis) {
                    void *doneHandle = job.bootBuf.handle;
                    const uint64_t seq = job.seq;
                    const bool bothReady = job.bothWereReady;
                    QMetaObject::invokeMethod(safeThis.data(),
                            [safeThis, seq, wonStreak, bothReady, lockedPhase, doneHandle,
                             cpuWorkUs, sampleTsUs, logicalSurfaceDmg, damagedPx, totalPx]() {
                        if (safeThis) {
                            safeThis->onCpuWorkerFrameDone(
                                    seq, wonStreak, bothReady, lockedPhase, doneHandle,
                                    cpuWorkUs, sampleTsUs, logicalSurfaceDmg, damagedPx, totalPx);
                        }
                    }, Qt::QueuedConnection);
                }
            } else {
                if (safeThis) {
                    void *doneHandle = job.bootBuf.handle;
                    const uint64_t seq = job.seq;
                    QMetaObject::invokeMethod(safeThis.data(),
                            [safeThis, seq, doneHandle, cpuWorkUs, sampleTsUs, logicalSurfaceDmg, damagedPx, totalPx]() {
                        if (safeThis) {
                            safeThis->onCpuWorkerFrameDone(
                                    seq, kWorkerLockInStreak, false, Phase::CpuSteadyState, doneHandle,
                                    cpuWorkUs, sampleTsUs, logicalSurfaceDmg, damagedPx, totalPx);
                        }
                    }, Qt::QueuedConnection);
                }
            }
        }
    });
}

void QPhaseWindow::setBackgroundColor(const QColor &color)
{
    d_phase->uiArena.setClearColor(color);
    d_phase->sceneTreeDirty = true;
    if (isExposed())
        scheduleReflexOrImmediateUpdate();
}

QColor QPhaseWindow::backgroundColor() const noexcept
{
    return d_phase->uiArena.clearColor();
}

QPhaseView *QPhaseWindow::rootView() const noexcept
{
    return d_phase->rootView.get();
}

QPhaseView *QPhaseWindow::focusedView() const noexcept
{
    return d_phase->focusedView;
}

QObject *QPhaseWindow::focusObject() const
{
    return d_phase->focusedView ? d_phase->imeProxy : nullptr;
}

QPhaseWindow::Phase QPhaseWindow::currentPhase() const noexcept
{
    return d_phase->phase.load(std::memory_order_acquire);
}

QPhaseWindow::Telemetry QPhaseWindow::telemetry() const noexcept
{
    Telemetry t{};
    const Phase p = d_phase->phase.load(std::memory_order_acquire);
    t.showToFirstFrameCommitUs = d_phase->showToFirstFrameCommitUs.load(std::memory_order_relaxed);
    t.showToFirstCpuCommitUs = d_phase->showToFirstCpuCommitUs.load(std::memory_order_relaxed);
    t.showToGpuCutoverUs = d_phase->showToGpuCutoverUs.load(std::memory_order_relaxed);
    t.gpuAsyncWarmupUs = d_phase->gpuAsyncWarmupUs.load(std::memory_order_relaxed);
    t.cpuBootFramesPresented = d_phase->cpuBootFramesPresented.load(std::memory_order_relaxed);
    t.gpuSteadyFramesPresented = d_phase->gpuSteadyFramesPresented.load(std::memory_order_relaxed);
    t.cpuFramesPresented = d_phase->cpuFramesPresented.load(std::memory_order_relaxed);
    t.gpuFramesPresented = d_phase->gpuFramesPresented.load(std::memory_order_relaxed);
    t.abortedCpuFrames = d_phase->abortedCpuFrames.load(std::memory_order_relaxed);
    t.abortedGpuFrames = d_phase->abortedGpuFrames.load(std::memory_order_relaxed);
    if (d_phase->vulkanBackend) {
        t.gpuGlyphCacheHits = d_phase->vulkanBackend->glyphCacheHits();
        t.gpuGlyphCacheMisses = d_phase->vulkanBackend->glyphCacheMisses();
    }
    t.cpuBootTileThreads = d_phase->recordedCpuTileThreads;
    t.lockedWinner = p;
    t.activeAffinityConstraint = d_phase->activeAffinityConstraint.load(std::memory_order_relaxed);
    t.activeConstraintWasBlocking = d_phase->activeConstraintBlocking.load(std::memory_order_relaxed);
    t.dedicatedGpuRenderThread = (p == Phase::GpuSteadyState);
    t.lowLatencyReflexEnabled = d_phase->lowLatencyReflexEnabled.load(std::memory_order_relaxed);
    return t;
}

std::vector<QPhaseWindow::FrameTiming> QPhaseWindow::frameTimings() const
{
    std::lock_guard<std::mutex> lock(d_phase->timingsMutex);
    return d_phase->recordedTimings;
}

void QPhaseWindow::setSimulatedGpuInitDelayMs(int delayMs)
{
    d_phase->simulatedGpuDelayMs.store(std::max(0, delayMs), std::memory_order_relaxed);
}

void QPhaseWindow::setSimulatedGpuFrameDelayMs(int delayMs)
{
    d_phase->simulatedGpuFrameDelayMs.store(std::max(0, delayMs), std::memory_order_relaxed);
}

void QPhaseWindow::setupUi(QPhaseView *)
{
}

void QPhaseWindow::exposeEvent(QExposeEvent *event)
{
    Q_UNUSED(event);
    if (!isExposed()) {
        d_phase->reflexDeadlineTimer.stop();
        d_phase->pauseGpuPresentation();
        return;
    }

    if (!d_phase->exposeTimerStarted.load(std::memory_order_relaxed)) {
        d_phase->exposeTimer.start();
        d_phase->exposeTimerStarted.store(true, std::memory_order_release);
    }

    if (!d_phase->uiSetupDone) {
        d_phase->rootView->setGeometry(QRectF(0.0, 0.0, width(), height()));
        setupUi(d_phase->rootView.get());
        d_phase->uiSetupDone = true;
        d_phase->sceneTreeDirty = true;
    }

    syncSceneTreeIfNeeded();
    applyRendererAffinityConstraints();

    {
        std::lock_guard<std::mutex> lock(d_phase->gpuMutex);
        d_phase->gpuPlatformWindow = handle();
        d_phase->gpuCanPresent = true;
        if (d_phase->phase.load(std::memory_order_acquire) == Phase::GpuSteadyState)
            d_phase->gpuFrameRequested = true;
    }
    d_phase->gpuCv.notify_all();

    renderFrame();
}

void QPhaseWindow::resizeEvent(QResizeEvent *event)
{
    QWindow::resizeEvent(event);
    if (d_phase->rootView)
        d_phase->rootView->setGeometry(QRectF(0.0, 0.0, event->size().width(), event->size().height()));
    d_phase->sceneTreeDirty = true;
    if (isExposed())
        renderFrame();
}

void QPhaseWindow::mousePressEvent(QMouseEvent *event)
{
    if (!d_phase->rootView)
        return;
    d_phase->mouseGrabber = d_phase->rootView->hitTestMousePress(event->position(), event);
}

void QPhaseWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (QPhaseView *grab = d_phase->mouseGrabber) {
        const QPointF localPos = grab->mapFromWindow(event->position());
        QMouseEvent localEv(event->type(), localPos, event->scenePosition(), event->globalPosition(),
                            event->button(), event->buttons(), event->modifiers(), event->pointingDevice());
        grab->onMouseMove(&localEv);
    }
}

void QPhaseWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (QPhaseView *grab = d_phase->mouseGrabber) {
        const QPointF localPos = grab->mapFromWindow(event->position());
        QMouseEvent localEv(event->type(), localPos, event->scenePosition(), event->globalPosition(),
                            event->button(), event->buttons(), event->modifiers(), event->pointingDevice());
        grab->onMouseRelease(&localEv);
        d_phase->mouseGrabber = nullptr;
    }
}

void QPhaseWindow::wheelEvent(QWheelEvent *event)
{
    if (d_phase->rootView)
        d_phase->rootView->dispatchWheel(event->position(), event);
}

void QPhaseWindow::keyPressEvent(QKeyEvent *event)
{
    if (d_phase->focusedView && d_phase->focusedView->onKeyPress(event))
        return;
    QWindow::keyPressEvent(event);
}

bool QPhaseWindow::event(QEvent *ev)
{
    if (ev->type() == QEvent::UpdateRequest) {
        renderFrame();
        return true;
    }
    if (ev->type() == QEvent::PlatformSurface) {
        auto *se = static_cast<QPlatformSurfaceEvent *>(ev);
        if (se->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            d_phase->reflexDeadlineTimer.stop();
            d_phase->stopCpuBootThread();
            d_phase->stopGpuRenderThread();
            d_phase->cpuRasterizer.reset();
            d_phase->vulkanBackend.reset();
        }
    }
    return QWindow::event(ev);
}

void QPhaseWindow::syncSceneTreeIfNeeded()
{
    if (d_phase->sceneTreeDirty && d_phase->rootView) {
        d_phase->uiArena.setRendererRequirements(PhaseRendererRequirements{});
        d_phase->uiArena.syncRootView(d_phase->rootView.get());
        d_phase->sceneTreeDirty = false;
    }
}

void QPhaseWindow::applyRendererAffinityConstraints()
{
    const PhaseRendererRequirements &reqs = d_phase->uiArena.rendererRequirements();
    // Note: when both GpuOnly and CpuOnly views exist (reqs.hasConflict()), GpuOnly owns the window
    // swapchain while QPhaseCpuRasterizer stays alive to rasterize CpuOnly subtrees into bridged QImages!

    const bool forceGpu = (d_phase->windowForcedAffinity == QPhaseRendererAffinity::GpuOnly)
            || reqs.forcesGpu();
    const bool forceCpu = !forceGpu
            && ((d_phase->windowForcedAffinity == QPhaseRendererAffinity::CpuOnly) || reqs.forcesCpu());
    const bool blocking = forceGpu
            ? ((d_phase->windowForcedAffinity == QPhaseRendererAffinity::GpuOnly && d_phase->windowForcedBlocking)
               || reqs.blocksUntilGpuReady())
            : ((d_phase->windowForcedAffinity == QPhaseRendererAffinity::CpuOnly && d_phase->windowForcedBlocking)
               || reqs.blocksUntilCpuReady());

    const QPhaseRendererAffinity effAffinity = forceGpu
            ? QPhaseRendererAffinity::GpuOnly
            : (forceCpu ? QPhaseRendererAffinity::CpuOnly : QPhaseRendererAffinity::Any);

    d_phase->activeAffinityConstraint.store(effAffinity, std::memory_order_release);
    d_phase->activeConstraintBlocking.store(blocking, std::memory_order_release);

    Phase curPhase = d_phase->phase.load(std::memory_order_acquire);

    if (forceCpu) {
        if (curPhase == Phase::WorkerRace)
            finalizeWorkerWinner(Phase::CpuSteadyState);
        return;
    }

    if (forceGpu) {
        if (d_phase->gpuInitFailed.load(std::memory_order_acquire))
            return;

        if (curPhase == Phase::CpuSteadyState) {
            // Dynamically mounted a GpuOnly view after an earlier CPU lock-in: wake GPU up!
            d_phase->phase.store(Phase::WorkerRace, std::memory_order_release);
            d_phase->gpuStopRequested = false;
            startEarlyGpuWarmupThread();
            curPhase = Phase::WorkerRace;
        }

        if (curPhase == Phase::WorkerRace) {
            if (blocking || d_phase->gpuDeviceReady.load(std::memory_order_acquire)) {
                // Blocking: lock GpuSteadyState immediately before frame #1 (CPU never runs!).
                // NonBlocking + GPU ready: shift immediately to GPU with zero race!
                finalizeWorkerWinner(Phase::GpuSteadyState);
            }
        }
    }
}

void QPhaseWindow::onCpuWorkerFrameDone(quint64 seq,
                                        quint8 wonStreak,
                                        bool bothWereReady,
                                        Phase lockedPhase,
                                        void *bufferHandle,
                                        qint64 cpuWorkUs,
                                        qint64 sampleTimestampUs,
                                        const QRegion &logicalSurfaceDamage,
                                        quint64 damagedPx,
                                        quint64 totalPx)
{
    Q_UNUSED(seq);
    const Phase curPhase = d_phase->phase.load(std::memory_order_acquire);
    if (curPhase == Phase::GpuSteadyState) {
        d_phase->winnerStillPresenting.store(false, std::memory_order_release);
        return;
    }

    QPlatformWindow *pw = handle();
    if (pw && isExposed() && bufferHandle) {
        if (!d_phase->gpuOwnsSurface.load(std::memory_order_acquire))
            pw->presentPhasedBootBuffer(bufferHandle, logicalSurfaceDamage);

        const Phase recordedPhase = (curPhase == Phase::CpuSteadyState)
                ? Phase::CpuSteadyState
                : Phase::WorkerRace;
        d_phase->recordFrameCommit(recordedPhase, Worker::CpuShm, wonStreak,
                                   bothWereReady, bothWereReady, cpuWorkUs, 0, sampleTimestampUs,
                                   0, damagedPx, totalPx);

        d_phase->cpuFramesPresented.fetch_add(1, std::memory_order_relaxed);
        if (curPhase == Phase::WorkerRace) {
            if (d_phase->cpuBootFramesPresented.fetch_add(1, std::memory_order_relaxed) == 0) {
                d_phase->showToFirstCpuCommitUs.store(
                        d_phase->exposeTimer.nsecsElapsed() / 1000, std::memory_order_relaxed);
            }
        }
    }
    d_phase->winnerStillPresenting.store(false, std::memory_order_release);

    bool renderAgain = false;
    {
        std::lock_guard<std::mutex> lock(d_phase->cpuMutex);
        d_phase->cpuJobInFlight = false;
        if (d_phase->cpuDirtyWhileInFlight
            || d_phase->raceDirtyWhileInFlight.exchange(false, std::memory_order_acq_rel)
            || d_phase->uiArena.hasActiveAnimations()
            || d_phase->continuousSampleRequested) {
            d_phase->cpuDirtyWhileInFlight = false;
            renderAgain = true;
        }
    }

    if (lockedPhase == Phase::CpuSteadyState && curPhase == Phase::WorkerRace) {
        finalizeWorkerWinner(Phase::CpuSteadyState);
        if (isExposed() && renderAgain)
            scheduleReflexOrImmediateUpdate();
        return;
    }

    if (renderAgain && d_phase->phase.load(std::memory_order_acquire) != Phase::GpuSteadyState && isExposed())
        scheduleReflexOrImmediateUpdate();
}

void QPhaseWindow::finalizeWorkerWinner(Phase winningPhase)
{
    if (d_phase->phase.load(std::memory_order_acquire) != Phase::WorkerRace)
        return;

    if (winningPhase == Phase::GpuSteadyState) {
        d_phase->phase.store(Phase::GpuSteadyState, std::memory_order_release);

        d_phase->stopCpuBootThread();
        if (d_phase->uiArena.rendererRequirements().cpuOnlyViewCount == 0)
            d_phase->cpuRasterizer.reset();
        d_phase->uiArena.markEnteredGpuSteadyState();
        {
            std::lock_guard<std::mutex> lock(d_phase->gpuMutex);
            d_phase->gpuSharedArena.markEnteredGpuSteadyState();
        }
        d_phase->gpuCv.notify_all();

        if (QPlatformWindow *pw = handle())
            pw->releasePhasedBootResources();

        if (d_phase->gpuDeviceReady.load(std::memory_order_acquire)) {
            d_phase->showToGpuCutoverUs.store(
                    d_phase->windowCreateTimer.nsecsElapsed() / 1000, std::memory_order_relaxed);
            Q_EMIT cutoverCompleted();
        } else {
            d_phase->pendingDeferredCutoverSignal = true;
        }

        if (isExposed() && (d_phase->uiArena.hasActiveAnimations()
                            || d_phase->continuousSampleRequested
                            || d_phase->sceneTreeDirty)) {
            scheduleReflexOrImmediateUpdate();
        }
    } else if (winningPhase == Phase::CpuSteadyState) {
        d_phase->phase.store(Phase::CpuSteadyState, std::memory_order_release);

        d_phase->stopGpuRenderThread();
        d_phase->vulkanBackend.reset();
        d_phase->gpuOwnsSurface.store(false, std::memory_order_release);

        d_phase->showToGpuCutoverUs.store(
                d_phase->windowCreateTimer.nsecsElapsed() / 1000, std::memory_order_relaxed);

        Q_EMIT cutoverCompleted();

        if (isExposed() && (d_phase->uiArena.hasActiveAnimations()
                            || d_phase->continuousSampleRequested
                            || d_phase->sceneTreeDirty)) {
            scheduleReflexOrImmediateUpdate();
        }
    }
}

void QPhaseWindow::renderFrame()
{
    if (!isExposed())
        return;

    QPlatformWindow *pw = handle();
    if (!pw)
        return;

    const qint64 sampleTimestampUs = d_phase->windowCreateTimer.nsecsElapsed() / 1000;
    d_phase->continuousSampleRequested = false;
    d_phase->insideBeforeFrameSample = true;
    beforeFrameSample();
    d_phase->insideBeforeFrameSample = false;

    syncSceneTreeIfNeeded();
    applyRendererAffinityConstraints();

    const qreal dpr = devicePixelRatio();
    const QSize pixelSize = (size() * dpr).expandedTo(QSize(1, 1));
    const bool hasAlpha = requestedFormat().hasAlpha();
    const Phase curPhase = d_phase->phase.load(std::memory_order_acquire);
    const QPhaseRendererAffinity effConstraint =
            d_phase->activeAffinityConstraint.load(std::memory_order_acquire);

    if (curPhase == Phase::GpuSteadyState) {
        if (!d_phase->uiArena.cpuBridges().empty()) {
            if (!d_phase->cpuRasterizer)
                d_phase->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
            d_phase->uiArena.rasterizeDirtyCpuBridges(
                    *d_phase->cpuRasterizer, dpr, d_phase->uiArena.monotonicTimestampNs());
        }
        {
            std::lock_guard<std::mutex> lock(d_phase->gpuMutex);
            d_phase->gpuSharedArena.syncRenderSnapshotFrom(d_phase->uiArena);
            d_phase->gpuPlatformWindow = pw;
            d_phase->gpuPixelSize = pixelSize;
            d_phase->gpuDpr = dpr;
            d_phase->gpuHasAlpha = hasAlpha;
            d_phase->gpuSampleTimestampUs = sampleTimestampUs;
            d_phase->gpuFrameRequested = true;
        }
        d_phase->gpuCv.notify_one();
        return;
    }

    // Non-blocking GpuOnly boot path while GPU is still warming up:
    // Run ONLY the CPU worker (which skips GpuOnly views) with ZERO worker racing,
    // then shift immediately to GpuSteadyState the instant gpuDeviceReady becomes true!
    if (curPhase == Phase::CpuSteadyState || effConstraint == QPhaseRendererAffinity::GpuOnly) {
        ensureCpuWorkerThread();
        std::unique_lock<std::mutex> lock(d_phase->cpuMutex);
        if (d_phase->cpuJobInFlight) {
            d_phase->cpuDirtyWhileInFlight = true;
            return;
        }

        QPlatformWindow::PhasedBootBuffer bootBuf{};
        if (!pw->acquirePhasedBootBuffer(pixelSize, dpr, &bootBuf))
            return;

        const uint64_t seq = d_phase->latestDispatchedSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (d_phase->cpuRenderArena)
            d_phase->cpuRenderArena->syncRenderSnapshotFrom(d_phase->uiArena);
        d_phase->pendingCpuJob.seq = seq;
        d_phase->pendingCpuJob.bothWereReady = false;
        d_phase->pendingCpuJob.bootBuf = bootBuf;
        d_phase->pendingCpuJob.forceOpaqueAlpha = !hasAlpha;
        d_phase->pendingCpuJob.frameTimeNs = d_phase->uiArena.monotonicTimestampNs();
        d_phase->pendingCpuJob.sampleTimestampUs = sampleTimestampUs;
        d_phase->cpuJobInFlight = true;
        d_phase->cpuJobReady = true;
        lock.unlock();
        d_phase->cpuCv.notify_one();
        return;
    }

    // =========================================================================
    // Phase::WorkerRace — Dispatch frame `seq` to BOTH competing workers!
    // =========================================================================
    const uint64_t prevDispatched = d_phase->latestDispatchedSeq.load(std::memory_order_acquire);
    const uint64_t prevClaimed = d_phase->lastClaimedAtomicSeq.load(std::memory_order_acquire);
    if (prevDispatched > prevClaimed || d_phase->winnerStillPresenting.load(std::memory_order_acquire)) {
        d_phase->raceDirtyWhileInFlight.store(true, std::memory_order_release);
        return;
    }

    const uint64_t seq = d_phase->latestDispatchedSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    bool bothReady = false;

    // 1. Dispatch `seq` to GPU worker (only score head-to-head strikes when GPU starts at the starting line!)
    {
        std::lock_guard<std::mutex> lock(d_phase->gpuMutex);
        bothReady = d_phase->gpuSceneWarm.load(std::memory_order_acquire) && !d_phase->gpuRenderingNow;
        d_phase->gpuSharedArena.syncRenderSnapshotFrom(d_phase->uiArena);
        d_phase->gpuPlatformWindow = pw;
        d_phase->gpuPixelSize = pixelSize;
        d_phase->gpuDpr = dpr;
        d_phase->gpuHasAlpha = hasAlpha;
        d_phase->gpuTargetSeq = seq;
        d_phase->gpuSampleTimestampUs = sampleTimestampUs;
        d_phase->gpuTargetBothReady = bothReady;
    }
    d_phase->gpuCv.notify_one();

    // 2. Dispatch `seq` to CPU worker
    ensureCpuWorkerThread();
    {
        std::unique_lock<std::mutex> lock(d_phase->cpuMutex);
        if (!d_phase->cpuJobInFlight) {
            QPlatformWindow::PhasedBootBuffer bootBuf{};
            if (pw->acquirePhasedBootBuffer(pixelSize, dpr, &bootBuf)) {
                if (d_phase->cpuRenderArena)
                    d_phase->cpuRenderArena->syncRenderSnapshotFrom(d_phase->uiArena);
                d_phase->pendingCpuJob.seq = seq;
                d_phase->pendingCpuJob.bothWereReady = bothReady;
                d_phase->pendingCpuJob.bootBuf = bootBuf;
                d_phase->pendingCpuJob.forceOpaqueAlpha = !hasAlpha;
                d_phase->pendingCpuJob.frameTimeNs = d_phase->uiArena.monotonicTimestampNs();
                d_phase->pendingCpuJob.sampleTimestampUs = sampleTimestampUs;
                d_phase->cpuJobInFlight = true;
                d_phase->cpuJobReady = true;
                lock.unlock();
                d_phase->cpuCv.notify_one();
            }
        }
    }
}

QT_END_NAMESPACE

#include "qphasewindow.moc"
#include "moc_qphasewindow.cpp"
