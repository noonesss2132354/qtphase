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
#include <QtGui/qguiapplication.h>
#include <QtGui/qscreen.h>
#include <QtGui/qwindow.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

QT_BEGIN_NAMESPACE

namespace {

constexpr qint64 kHardwareGpuFastUs = 8000;

enum class ProcessGpuState : uint8_t {
    Uninitialized = 0,
    WarmingUp = 1,
    WarmReady = 2,
    GpuLocked = 3,
    CpuLocked = 4
};

// Once-per-process background Vulkan 1.3 warmup + hardware/timing decision coordinator.
// Window #1 presents Frame 1 instantly on CPU wl_shm while this single thread warms up VkDevice + pipelines,
// then probes ONE real presented DMA-BUF frame to lock the process-wide winner (GPU Vulkan 1.3 vs CPU).
// Every subsequent window, dialog, popup, and re-shown surface attaches directly into steady state (~0.2 ms)!
struct ProcessGpuCoordinator {
    std::mutex mutex;
    ProcessGpuState state = ProcessGpuState::Uninitialized;
    std::unique_ptr<QPhaseVulkanBackend> warmBackend;
    qint64 warmupUs = 0;
    std::vector<QPointer<QPhaseSurfacePresenter>> waiters;
    std::thread warmupThread;

    static ProcessGpuCoordinator &instance()
    {
        static ProcessGpuCoordinator s_coord;
        return s_coord;
    }

    ~ProcessGpuCoordinator()
    {
        if (warmupThread.joinable())
            warmupThread.join();
        warmBackend.reset();
    }

    ProcessGpuState currentState()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }

    qint64 recordedWarmupUs()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return warmupUs;
    }

    void ensureWarmupStarted(QPhaseSurfacePresenter *waiter, std::function<void(QPointer<QPhaseSurfacePresenter>)> notifyFn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == ProcessGpuState::WarmReady
            || state == ProcessGpuState::GpuLocked
            || state == ProcessGpuState::CpuLocked) {
            return;
        }
        if (waiter)
            waiters.emplace_back(waiter);
        if (state == ProcessGpuState::WarmingUp)
            return;

        state = ProcessGpuState::WarmingUp;
        warmupThread = std::thread([this, notify = std::move(notifyFn)]() {
            QElapsedTimer wt;
            wt.start();
            auto backend = std::make_unique<QPhaseVulkanBackend>();
            const bool ok = backend->initializeDevice(true);
            const qint64 elapsedUs = wt.nsecsElapsed() / 1000;

            std::vector<QPointer<QPhaseSurfacePresenter>> toNotify;
            {
                std::lock_guard<std::mutex> innerLock(mutex);
                warmupUs = elapsedUs;
                if (ok) {
                    warmBackend = std::move(backend);
                    if (state == ProcessGpuState::WarmingUp)
                        state = ProcessGpuState::WarmReady;
                } else {
                    state = ProcessGpuState::CpuLocked;
                }
                toNotify.swap(waiters);
            }

            for (const auto &w : toNotify) {
                if (w)
                    notify(w);
            }
        });
    }

    std::unique_ptr<QPhaseVulkanBackend> takeWarmBackend()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(warmBackend);
    }

    void lockWinner(ProcessGpuState lockedState)
    {
        std::lock_guard<std::mutex> lock(mutex);
        state = lockedState;
        if (lockedState == ProcessGpuState::CpuLocked)
            warmBackend.reset();
    }
};

} // namespace

class QPhaseSurfacePresenterPrivate
{
public:
    QPointer<QWindow> window;
    QPhaseSceneArena uiArena;
    std::function<void(QPhaseSceneArena &)> syncCallback;

    // UI Reflex VSync-deadline coalescer
    QTimer reflexTimer;
    qint64 lastVsyncAnchorUs = 0;
    qint64 rollingBackendRenderUs = 1200;
    qint64 lastReflexTargetVsyncUs = 0;
    bool presentDirty = false;
    bool isShutDown = false;

    // Per-window renderer state (100% main-thread owned!)
    QPhaseSurfacePresenter::Phase phase = QPhaseSurfacePresenter::Phase::WorkerRace;
    QPhaseSurfacePresenter::Phase lastFinalizedPhase = QPhaseSurfacePresenter::Phase::WorkerRace;
    QPhaseRendererAffinity forcedAffinity = QPhaseRendererAffinity::Any;
    bool forcedBlocking = false;
    bool needReleaseBootResources = false;
    bool swapchainFallbackAttached = false;

    std::unique_ptr<QPhaseCpuRasterizer> cpuRasterizer;
    QPhaseDamageTracker cpuDamageTracker;
    std::unique_ptr<QPhaseVulkanBackend> vulkanBackend;

    uint64_t nextFrameSeq = 0;
    uint64_t lastDispatchedRev = 0;
    QSize lastDispatchedPxSize;
    qreal lastDispatchedDpr = 0.0;

    // Telemetry
    QElapsedTimer createTimer;
    quint64 cpuFrames = 0;
    quint64 gpuFrames = 0;
    qint64 lastCpuBootUs = 0;
    qint64 gpuWarmupUs = 0;
    qint64 totalGpuHwUs = 0;
    quint64 totalSyncedBytes = 0;
    bool dumpedDebugPng = false;
};

bool QPhaseSurfacePresenter::isPlatformSupported()
{
    const QString platform = QGuiApplication::platformName();
    return platform.startsWith(QLatin1String("wayland"));
}

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

    auto &coord = ProcessGpuCoordinator::instance();
    coord.ensureWarmupStarted(this, [](QPointer<QPhaseSurfacePresenter> safeThis) {
        if (!safeThis)
            return;
        QMetaObject::invokeMethod(safeThis.data(), [safeThis]() {
            if (safeThis && !safeThis->d->isShutDown)
                safeThis->onProcessWarmupFinished();
        }, Qt::QueuedConnection);
    });

    const ProcessGpuState st = coord.currentState();
    if (st == ProcessGpuState::GpuLocked) {
        d->phase = Phase::GpuSteadyState;
        d->lastFinalizedPhase = Phase::GpuSteadyState;
        d->uiArena.markEnteredGpuSteadyState();
        d->needReleaseBootResources = true;
    } else if (st == ProcessGpuState::CpuLocked) {
        d->phase = Phase::CpuSteadyState;
        d->lastFinalizedPhase = Phase::CpuSteadyState;
    }
}

QPhaseSurfacePresenter::~QPhaseSurfacePresenter()
{
    const bool printStats = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_STATS") > 0;
    const quint64 cpuN = d->cpuFrames;
    const quint64 gpuN = d->gpuFrames;
    const qint64 warmupUs = d->gpuWarmupUs;
    const qint64 hwUs = d->totalGpuHwUs;
    const quint64 bytes = d->totalSyncedBytes;
    const Phase finalPhase = (d->phase != Phase::WorkerRace) ? d->phase : d->lastFinalizedPhase;
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
    QPlatformWindow *pw = d->window ? d->window->handle() : nullptr;
    if (d->vulkanBackend) {
        d->vulkanBackend->releaseDmaBufRing(pw);
        d->vulkanBackend.reset();
    }
    d->swapchainFallbackAttached = false;
    d->lastDispatchedRev = 0;
    d->lastDispatchedPxSize = QSize();
    d->lastDispatchedDpr = 0.0;
    d->uiArena.resetGpuSteadyState();
    d->cpuDamageTracker.reset();

    const ProcessGpuState st = ProcessGpuCoordinator::instance().currentState();
    if (st == ProcessGpuState::GpuLocked && d->forcedAffinity != QPhaseRendererAffinity::CpuOnly) {
        d->phase = Phase::GpuSteadyState;
        d->lastFinalizedPhase = Phase::GpuSteadyState;
        d->uiArena.markEnteredGpuSteadyState();
        d->needReleaseBootResources = true;
    } else if (st == ProcessGpuState::CpuLocked || d->forcedAffinity == QPhaseRendererAffinity::CpuOnly) {
        d->phase = Phase::CpuSteadyState;
        d->lastFinalizedPhase = Phase::CpuSteadyState;
    } else {
        d->phase = Phase::WorkerRace;
        d->needReleaseBootResources = false;
    }
}

void QPhaseSurfacePresenter::shutdown()
{
    if (d->isShutDown)
        return;
    d->isShutDown = true;
    d->reflexTimer.stop();
    QPlatformWindow *pw = d->window ? d->window->handle() : nullptr;
    if (d->vulkanBackend) {
        d->vulkanBackend->releaseDmaBufRing(pw);
        d->vulkanBackend.reset();
    }
    d->cpuRasterizer.reset();
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
    d->forcedAffinity = affinity;
    d->forcedBlocking = blocking;
}

QPhaseSurfacePresenter::Phase QPhaseSurfacePresenter::currentPhase() const noexcept
{
    return d->phase;
}

quint64 QPhaseSurfacePresenter::cpuFramesPresented() const noexcept
{
    return d->cpuFrames;
}

quint64 QPhaseSurfacePresenter::gpuFramesPresented() const noexcept
{
    return d->gpuFrames;
}

void QPhaseSurfacePresenter::onProcessWarmupFinished()
{
    if (d->isShutDown)
        return;

    const ProcessGpuState st = ProcessGpuCoordinator::instance().currentState();
    if (st == ProcessGpuState::CpuLocked) {
        finalizeWinner(Phase::CpuSteadyState);
        if (d->presentDirty && d->window && d->window->isExposed())
            presentNow();
        return;
    }

    if (st == ProcessGpuState::GpuLocked && d->forcedAffinity != QPhaseRendererAffinity::CpuOnly)
        finalizeWinner(Phase::GpuSteadyState);

    if (d->window && d->window->isExposed()
        && (d->presentDirty || d->phase == Phase::WorkerRace || d->gpuFrames == 0)) {
        presentNow();
    }
}

bool QPhaseSurfacePresenter::ensureVulkanBackendReady()
{
    if (d->vulkanBackend && d->vulkanBackend->isDeviceReady())
        return true;

    auto &coord = ProcessGpuCoordinator::instance();
    if (auto adopted = coord.takeWarmBackend()) {
        d->vulkanBackend = std::move(adopted);
        d->gpuWarmupUs = coord.recordedWarmupUs();
        return d->vulkanBackend->isDeviceReady();
    }

    QElapsedTimer wt;
    wt.start();
    d->vulkanBackend = std::make_unique<QPhaseVulkanBackend>();
    const bool ok = d->vulkanBackend->initializeDevice(true);
    d->gpuWarmupUs = wt.nsecsElapsed() / 1000;
    if (!ok) {
        d->vulkanBackend.reset();
        return false;
    }
    return true;
}

void QPhaseSurfacePresenter::finalizeWinner(Phase winningPhase)
{
    if (winningPhase == Phase::GpuSteadyState) {
        d->phase = Phase::GpuSteadyState;
        d->lastFinalizedPhase = Phase::GpuSteadyState;
        if (d->uiArena.rendererRequirements().cpuOnlyViewCount == 0)
            d->cpuRasterizer.reset();
        d->cpuDamageTracker.reset();
        d->uiArena.markEnteredGpuSteadyState();
        if (d->window && d->window->handle()) {
            d->window->handle()->releasePhasedBootResources();
            d->needReleaseBootResources = false;
        } else {
            d->needReleaseBootResources = true;
        }
    } else if (winningPhase == Phase::CpuSteadyState) {
        d->phase = Phase::CpuSteadyState;
        d->lastFinalizedPhase = Phase::CpuSteadyState;
        QPlatformWindow *pw = d->window ? d->window->handle() : nullptr;
        if (d->vulkanBackend) {
            d->vulkanBackend->releaseDmaBufRing(pw);
            d->vulkanBackend.reset();
        }
        d->swapchainFallbackAttached = false;
    }
}

bool QPhaseSurfacePresenter::presentCpuFrame(QPlatformWindow *pw,
                                             const QSize &pxSize,
                                             qreal dpr,
                                             bool hasAlpha,
                                             uint64_t frameTimeNs,
                                             qint64 *outCpuWorkUs)
{
    if (!pw || pxSize.isEmpty())
        return false;

    QPlatformWindow::PhasedBootBuffer bootBuf{};
    if (!pw->acquirePhasedBootBuffer(pxSize, dpr, &bootBuf) || !bootBuf.data)
        return false;

    if (!d->cpuRasterizer)
        d->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();

    QElapsedTimer t;
    t.start();
    bool isFullDmg = true;
    const QRegion dmgRegion = d->cpuDamageTracker.computeAndAdvance(
            d->uiArena, pxSize, dpr, frameTimeNs, &isFullDmg, QPhaseRendererAffinity::CpuOnly);

    const bool finished = d->cpuRasterizer->renderToBuffer(
            d->uiArena,
            bootBuf.data,
            bootBuf.width,
            bootBuf.height,
            bootBuf.bytesPerLine,
            dpr,
            !hasAlpha,
            frameTimeNs,
            nullptr,
            0,
            nullptr);
    const qint64 cpuWorkUs = t.nsecsElapsed() / 1000;
    if (outCpuWorkUs)
        *outCpuWorkUs = cpuWorkUs;

    if (!finished) {
        d->cpuDamageTracker.reset();
        return false;
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
                           std::max(1, int(std::ceil(double(bootBuf.width) / dpr))),
                           std::max(1, int(std::ceil(double(bootBuf.height) / dpr))));
    }

    pw->presentPhasedBootBuffer(bootBuf.handle, logicalDmg);
    ++d->cpuFrames;
    d->lastCpuBootUs = cpuWorkUs;
    d->lastVsyncAnchorUs = d->createTimer.nsecsElapsed() / 1000;
    d->rollingBackendRenderUs = (d->rollingBackendRenderUs * 3 + cpuWorkUs) / 4;
    return true;
}

bool QPhaseSurfacePresenter::presentGpuFrame(QPlatformWindow *pw,
                                             const QSize &pxSize,
                                             const QSize &logicalSize,
                                             qreal dpr,
                                             bool hasAlpha,
                                             uint64_t frameTimeNs,
                                             bool waitForGpuCompletion,
                                             qint64 *outGpuHwUs)
{
    if (!pw || pxSize.isEmpty() || logicalSize.isEmpty() || !ensureVulkanBackendReady())
        return false;

    if (!d->dumpedDebugPng) {
        const QByteArray dumpPath = qgetenv("QT_WIDGETS_PHASE_DUMP_PNG");
        if (!dumpPath.isEmpty()) {
            const quint64 targetFrame = std::max<quint64>(1, qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_DUMP_FRAME"));
            if (d->gpuFrames + 1 >= targetFrame) {
                d->dumpedDebugPng = true;
                const QImage img = d->vulkanBackend->renderOffscreenToImage(d->uiArena, pxSize, dpr, frameTimeNs);
                img.save(QString::fromUtf8(dumpPath));
            }
        }
    }

    if (!d->uiArena.cpuBridges().empty()) {
        if (!d->cpuRasterizer)
            d->cpuRasterizer = std::make_unique<QPhaseCpuRasterizer>();
        d->uiArena.rasterizeDirtyCpuBridges(*d->cpuRasterizer, dpr, frameTimeNs);
    }

    QPhaseVulkanBackend::GpuFrameMetrics metrics{};
    bool okRender = false;
    if (pw->supportsPhasedDmaBuf() && d->vulkanBackend->supportsDmaBufExport()) {
        okRender = d->vulkanBackend->renderAndPresentDmaBuf(
                pw, d->uiArena, pxSize, logicalSize, dpr, hasAlpha, frameTimeNs,
                &metrics, waitForGpuCompletion);
    } else {
        if (!d->swapchainFallbackAttached)
            d->swapchainFallbackAttached = d->vulkanBackend->attachWindow(pw, pxSize, hasAlpha);
        if (d->swapchainFallbackAttached) {
            okRender = d->vulkanBackend->renderAndPresentWindow(
                    pw, d->uiArena, pxSize, dpr, hasAlpha, frameTimeNs,
                    &metrics, waitForGpuCompletion);
        }
    }

    if (!okRender)
        return false;

    ++d->gpuFrames;
    d->totalGpuHwUs += metrics.gpuHardwareUs;
    d->totalSyncedBytes += metrics.syncedSceneBytes;
    if (outGpuHwUs)
        *outGpuHwUs = metrics.gpuHardwareUs;

    // Immediately retire uploaded CPU pixel copies & advance FIFO bindless cooldown epoch!
    if (metrics.uploadedImageGen > 0)
        d->uiArena.acknowledgeGpuImagesUploaded(metrics.uploadedImageGen);
    d->uiArena.advanceFrameEpoch();

    d->lastVsyncAnchorUs = d->createTimer.nsecsElapsed() / 1000;
    const qint64 workUs = std::max<qint64>(metrics.cpuSubmitWorkUs, metrics.gpuHardwareUs);
    d->rollingBackendRenderUs = (d->rollingBackendRenderUs * 3 + workUs) / 4;
    return true;
}

void QPhaseSurfacePresenter::schedulePresent()
{
    if (d->isShutDown || !d->window || !d->window->isExposed())
        return;

    d->presentDirty = true;
    if (d->reflexTimer.isActive())
        return;

    if (d->phase == Phase::WorkerRace) {
        // Coalesce to 0 ms event-loop turn (never paint synchronously mid-layout inside QWidget::update()!)
        d->reflexTimer.start(0);
        return;
    }

    QScreen *scr = d->window->screen();
    const double hz = (scr && scr->refreshRate() > 1.0) ? double(scr->refreshRate()) : 60.0;
    const qint64 vsyncPeriodUs = std::clamp<qint64>(qint64(std::llround(1'000'000.0 / hz)), 2000, 33'333);

    const qint64 nowUs = d->createTimer.nsecsElapsed() / 1000;
    const qint64 anchorUs = d->lastVsyncAnchorUs;
    if (anchorUs <= 0 || nowUs < anchorUs) {
        d->reflexTimer.start(0);
        return;
    }

    const qint64 leadWindowUs = std::clamp<qint64>(d->rollingBackendRenderUs + 1500, 1800, vsyncPeriodUs - 800);
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
    if (QPlatformWindow *pw = d->window->handle(); pw && pw->isPhasedConfigureInProgress()) {
        // Inside QWaylandWindow::applyConfigure(): wait ~20 us for didApplyConfigure() + sendExposeEvent()!
        d->presentDirty = true;
        return;
    }
    presentNow();
}

void QPhaseSurfacePresenter::presentNow()
{
    if (d->isShutDown || !d->window || !d->window->isExposed())
        return;

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
        // QWaylandWindow::applyConfigure() already set destination to platformLogicalSize while
        // the queued QWindowSystemInterface::handleGeometryChange() hasn't updated QWindow::size() yet.
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

    const QSize finalLogicalSize = d->window->size();
    const QSize pxSize = (finalLogicalSize * dpr).expandedTo(QSize(1, 1));
    const bool hasAlpha = d->window->requestedFormat().hasAlpha();
    const uint64_t frameTimeNs = d->uiArena.monotonicTimestampNs();

    // Honor forced renderer affinity if explicitly set
    auto &coord = ProcessGpuCoordinator::instance();
    if (d->phase == Phase::WorkerRace) {
        if (d->forcedAffinity == QPhaseRendererAffinity::CpuOnly) {
            finalizeWinner(Phase::CpuSteadyState);
        } else if (d->forcedAffinity == QPhaseRendererAffinity::GpuOnly
                   && coord.currentState() != ProcessGpuState::CpuLocked) {
            if (d->forcedBlocking || coord.currentState() == ProcessGpuState::WarmReady
                || coord.currentState() == ProcessGpuState::GpuLocked) {
                finalizeWinner(Phase::GpuSteadyState);
            }
        }
    }

    // Check if another window already locked the process-wide winner
    if (d->phase == Phase::WorkerRace) {
        const ProcessGpuState st = coord.currentState();
        if (st == ProcessGpuState::GpuLocked)
            finalizeWinner(Phase::GpuSteadyState);
        else if (st == ProcessGpuState::CpuLocked)
            finalizeWinner(Phase::CpuSteadyState);
    }

    if (d->phase == Phase::GpuSteadyState) {
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

        const uint64_t seq = ++d->nextFrameSeq;
        if (sProfile) {
            std::fprintf(stderr, "[PhaseProfile UI] seq=%llu px=%dx%d sync=%.2fms prims=%zu imgs=%zu\n",
                         static_cast<unsigned long long>(seq),
                         pxSize.width(), pxSize.height(), double(syncUs) / 1000.0,
                         d->uiArena.drawOrder().size(), d->uiArena.activeImageSlotCount());
        }

        if (presentGpuFrame(pw, pxSize, finalLogicalSize, dpr, hasAlpha, frameTimeNs, false)) {
            d->lastDispatchedRev = d->uiArena.revision();
            d->lastDispatchedPxSize = pxSize;
            d->lastDispatchedDpr = dpr;
        } else {
            // Transient failure: allow retry on next schedule
            d->lastDispatchedRev = 0;
            d->presentDirty = true;
        }
        return;
    }

    if (d->phase == Phase::CpuSteadyState) {
        presentCpuFrame(pw, pxSize, dpr, hasAlpha, frameTimeNs);
        return;
    }

    // Phase::WorkerRace — Cold-start temporal handoff (once per process!):
    // 1. While the single background thread is still warming up Vulkan 1.3, render Frame 1+ instantly on CPU wl_shm.
    // 2. Once Vulkan 1.3 is warm (WarmReady):
    //    - On real hardware GPUs: present Frame 1 asynchronously (waitForGpuCompletion=false, 0 ms fence stall!)
    //      and lock GpuSteadyState immediately so cold startup matches upstream Qt Widgets!
    //    - On software Vulkan (e.g. llvmpipe): probe ONE real frame with waitForGpuCompletion=true and
    //      lock whichever backend is genuinely faster.
    if (coord.currentState() == ProcessGpuState::WarmReady) {
        const bool backendOk = ensureVulkanBackendReady();
        const bool hwGpu = backendOk && d->vulkanBackend && d->vulkanBackend->isHardwareGpu();
        qint64 probeHwUs = 0;
        const bool okGpu = backendOk && presentGpuFrame(
                pw, pxSize, finalLogicalSize, dpr, hasAlpha, frameTimeNs, !hwGpu, &probeHwUs);
        const qint64 cpuRefUs = (d->lastCpuBootUs > 0) ? d->lastCpuBootUs : 4000;
        const bool gpuWins = okGpu && (hwGpu || (probeHwUs > 0 && probeHwUs < cpuRefUs));

        if (gpuWins) {
            coord.lockWinner(ProcessGpuState::GpuLocked);
            d->lastDispatchedRev = d->uiArena.revision();
            d->lastDispatchedPxSize = pxSize;
            d->lastDispatchedDpr = dpr;
            finalizeWinner(Phase::GpuSteadyState);
            if (sProfile) {
                std::fprintf(stderr, "[PhaseProfile UI] seq=%llu px=%dx%d sync=%.2fms prims=%zu imgs=%zu (%s->GPU hw=%.2fms vs cpu=%.2fms)\n",
                             static_cast<unsigned long long>(++d->nextFrameSeq),
                             pxSize.width(), pxSize.height(), double(syncUs) / 1000.0,
                             d->uiArena.drawOrder().size(), d->uiArena.activeImageSlotCount(),
                             hwGpu ? "hw-async" : "probe",
                             double(probeHwUs) / 1000.0, double(cpuRefUs) / 1000.0);
            }
        } else {
            coord.lockWinner(ProcessGpuState::CpuLocked);
            finalizeWinner(Phase::CpuSteadyState);
            if (!okGpu)
                presentCpuFrame(pw, pxSize, dpr, hasAlpha, frameTimeNs);
        }
        return;
    }

    // Still WarmingUp: instant CPU wl_shm first-frame boot!
    presentCpuFrame(pw, pxSize, dpr, hasAlpha, frameTimeNs);
}

QT_END_NAMESPACE

#include "moc_qphasesurfacepresenter_p.cpp"
