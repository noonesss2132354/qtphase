// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasesurfacepresenter_p.h"
#include "qphaserenderpolicy_p.h"
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

enum class ProcessGpuState : uint8_t {
    Uninitialized = 0,
    WarmingUp = 1,
    WarmReady = 2,
    Failed = 3
};

// Once-per-process background Vulkan 1.3 warmup coordinator.
// Shares VkDevice initialization across all windows while letting each window's
// QPhaseRenderPolicy independently decide CPU vs GPU (and race empirically!).
struct ProcessGpuCoordinator {
    std::mutex mutex;
    ProcessGpuState state = ProcessGpuState::Uninitialized;
    bool hardwareGpu = false;
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

    bool isHardwareGpu()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return hardwareGpu;
    }

    qint64 recordedWarmupUs()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return warmupUs;
    }

    void ensureWarmupStarted(QPhaseSurfacePresenter *waiter, std::function<void(QPointer<QPhaseSurfacePresenter>)> notifyFn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == ProcessGpuState::WarmReady || state == ProcessGpuState::Failed)
            return;
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
            const bool hw = ok && backend->isHardwareGpu();
            const qint64 elapsedUs = wt.nsecsElapsed() / 1000;

            std::vector<QPointer<QPhaseSurfacePresenter>> toNotify;
            {
                std::lock_guard<std::mutex> innerLock(mutex);
                warmupUs = elapsedUs;
                if (ok) {
                    hardwareGpu = hw;
                    warmBackend = std::move(backend);
                    state = ProcessGpuState::WarmReady;
                } else {
                    hardwareGpu = false;
                    state = ProcessGpuState::Failed;
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
};

inline bool isPopupOrTooltipWindow(const QWindow *win) noexcept
{
    if (!win)
        return false;
    const Qt::WindowType type = win->type();
    return type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen;
}

} // namespace

class QPhaseSurfacePresenterPrivate
{
public:
    QPointer<QWindow> window;
    QPhaseSceneArena uiArena;
    std::function<void(QPhaseSceneArena &)> syncCallback;
    std::function<void(QPhaseSceneArena &)> restoreCpuImagesCallback;
    std::unique_ptr<QPhaseRenderPolicy> policy;

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
    bool permanentLockActive = false;
    bool hasPresentedFrame = false;
    QPhaseBackendTarget lastPresentedBackend = QPhaseBackendTarget::Cpu;
    bool needReleaseBootResources = false;
    bool swapchainFallbackAttached = false;
    float reportedDirtyAreaRatio = 1.0f;

    std::unique_ptr<QPhaseCpuRasterizer> cpuRasterizer;
    QPhaseDamageTracker cpuDamageTracker;
    std::unique_ptr<QPhaseVulkanBackend> vulkanBackend;
    std::thread cpuCalibThread;

    uint64_t nextFrameSeq = 0;
    uint64_t lastDispatchedRev = 0;
    QSize lastDispatchedPxSize;
    qreal lastDispatchedDpr = 0.0;
    qint64 lastFrameTimeUs = 0;

    // Telemetry
    QElapsedTimer createTimer;
    quint64 cpuFrames = 0;
    quint64 gpuFrames = 0;
    quint64 backendSwitches = 0;
    qint64 lastCpuBootUs = 0;
    qint64 lastAsyncCpuSampleUs = 0;
    qint64 gpuWarmupUs = 0;
    qint64 totalCpuWorkUs = 0;
    qint64 totalGpuSubmitUs = 0;
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
    d->policy = QPhaseRenderPolicy::createFromEnvironment();
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
}

QPhaseSurfacePresenter::~QPhaseSurfacePresenter()
{
    const bool printStats = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_STATS") > 0;
    const quint64 cpuN = d->cpuFrames;
    const quint64 gpuN = d->gpuFrames;
    const quint64 switches = d->backendSwitches;
    const qint64 warmupUs = d->gpuWarmupUs > 0 ? d->gpuWarmupUs : ProcessGpuCoordinator::instance().recordedWarmupUs();
    const qint64 cpuUs = d->totalCpuWorkUs;
    const qint64 gpuSubUs = d->totalGpuSubmitUs;
    const qint64 hwUs = d->totalGpuHwUs;
    const qint64 asyncCalibUs = d->lastAsyncCpuSampleUs;
    const quint64 bytes = d->totalSyncedBytes;
    const Phase finalPhase = (d->phase != Phase::WorkerRace) ? d->phase : d->lastFinalizedPhase;
    const QByteArray polName = d->policy ? QByteArray(d->policy->name()) : QByteArrayLiteral("<none>");
    const QString winName = d->window ? d->window->objectName() : QStringLiteral("<null>");

    shutdown();

    if (printStats && (cpuN > 0 || gpuN > 0)) {
        const char *phaseStr = (finalPhase == Phase::GpuSteadyState)
                ? "GPU Vulkan 1.3"
                : (finalPhase == Phase::CpuSteadyState ? "CPU Tile Pool" : "Dynamic");
        const double avgCpuMs = cpuN > 0 ? (double(cpuUs) / double(cpuN)) / 1000.0 : 0.0;
        const double avgSubMs = gpuN > 0 ? (double(gpuSubUs) / double(gpuN)) / 1000.0 : 0.0;
        const double avgHwMs = gpuN > 0 ? (double(hwUs) / double(gpuN)) / 1000.0 : 0.0;
        const double avgKb = gpuN > 0 ? (double(bytes) / double(gpuN)) / 1024.0 : 0.0;
        std::fprintf(stderr,
                     "[QtWidgets+QtPhase] %s | policy=%s | mode=%s | switches=%llu | warmup=%.2f ms | cpu_calib=%.2f ms | frames: CPU=%llu GPU=%llu | avg_cpu=%.2f ms avg_gpu_sub=%.2f ms avg_gpu_hw=%.2f ms | avg_uma=%.1f KB\n",
                     qPrintable(winName),
                     polName.constData(),
                     phaseStr,
                     static_cast<unsigned long long>(switches),
                     double(warmupUs) / 1000.0,
                     double(asyncCalibUs) / 1000.0,
                     static_cast<unsigned long long>(cpuN),
                     static_cast<unsigned long long>(gpuN),
                     avgCpuMs,
                     avgSubMs,
                     avgHwMs,
                     avgKb);
        std::fflush(stderr);
    }
}

void QPhaseSurfacePresenter::onSurfaceHidden()
{
    d->reflexTimer.stop();
    d->presentDirty = false;
    if (d->cpuCalibThread.joinable())
        d->cpuCalibThread.join();
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
    d->permanentLockActive = false;
    d->hasPresentedFrame = false;
    d->phase = Phase::WorkerRace;
    d->needReleaseBootResources = false;
    if (d->policy)
        d->policy->reset();
}

void QPhaseSurfacePresenter::shutdown()
{
    if (d->isShutDown)
        return;
    d->isShutDown = true;
    d->reflexTimer.stop();
    if (d->cpuCalibThread.joinable())
        d->cpuCalibThread.join();
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

void QPhaseSurfacePresenter::setRestoreCpuImagesCallback(std::function<void(QPhaseSceneArena &)> callback)
{
    d->restoreCpuImagesCallback = std::move(callback);
}

void QPhaseSurfacePresenter::setRenderPolicy(std::unique_ptr<QPhaseRenderPolicy> policy)
{
    if (!policy)
        return;
    d->policy = std::move(policy);
    d->permanentLockActive = false;
    d->phase = Phase::WorkerRace;
}

const char *QPhaseSurfacePresenter::renderPolicyName() const noexcept
{
    return d->policy ? d->policy->name() : "";
}

void QPhaseSurfacePresenter::reportDirtyAreaRatio(float ratio) noexcept
{
    d->reportedDirtyAreaRatio = std::clamp(ratio, 0.0f, 1.0f);
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

    if (d->window && d->window->isExposed()
        && (d->presentDirty || (!d->permanentLockActive && d->gpuFrames == 0))) {
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
    d->permanentLockActive = true;
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

void QPhaseSurfacePresenter::launchAsyncCpuCalibration(const QSize &pxSize,
                                                       qreal dpr,
                                                       bool hasAlpha,
                                                       uint64_t frameTimeNs)
{
    if (d->cpuCalibThread.joinable() || pxSize.isEmpty())
        return;

    // Clone POD vectors + O(1) QImage refcounts BEFORE GPU upload clears cpuRgbaPremul!
    auto snapshot = d->uiArena.cloneForCpuCalibration();
    QPointer<QPhaseSurfacePresenter> safeThis(this);
    const int w = pxSize.width();
    const int h = pxSize.height();

    d->cpuCalibThread = std::thread([safeThis, snap = std::move(snapshot), w, h, dpr, hasAlpha, frameTimeNs]() mutable {
        std::vector<uchar> scratch(size_t(w) * size_t(h) * 4);
        QPhaseCpuRasterizer calibRaster;
        QElapsedTimer t;
        t.start();
        calibRaster.renderToBuffer(*snap, scratch.data(), w, h, w * 4, dpr, !hasAlpha, frameTimeNs);
        const qint64 elapsedUs = t.nsecsElapsed() / 1000;

        if (!safeThis)
            return;
        QMetaObject::invokeMethod(safeThis.data(), [safeThis, elapsedUs]() {
            if (!safeThis || safeThis->d->isShutDown)
                return;
            safeThis->d->lastAsyncCpuSampleUs = elapsedUs;
            if (safeThis->d->policy)
                safeThis->d->policy->recordAsyncCpuSample(elapsedUs);
            if (qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0) {
                std::fprintf(stderr, "[PhaseProfile Policy=%s] async_cpu_calib=%.2fms\n",
                             safeThis->renderPolicyName(), double(elapsedUs) / 1000.0);
            }
        }, Qt::QueuedConnection);
    });
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
    d->totalCpuWorkUs += cpuWorkUs;
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
                                             qint64 *outGpuSubmitUs,
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
    d->totalGpuSubmitUs += metrics.cpuSubmitWorkUs;
    d->totalGpuHwUs += metrics.gpuHardwareUs;
    d->totalSyncedBytes += metrics.syncedSceneBytes;
    if (outGpuSubmitUs)
        *outGpuSubmitUs = metrics.cpuSubmitWorkUs;
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

    if (!d->hasPresentedFrame) {
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
    presentFrameInternal(true);
}

void QPhaseSurfacePresenter::presentNow()
{
    presentFrameInternal(false);
}

void QPhaseSurfacePresenter::presentFrameInternal(bool isResize)
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
        d->presentDirty = true;
        return;
    }

    d->reflexTimer.stop();
    d->presentDirty = false;
    d->reportedDirtyAreaRatio = 1.0f;

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
    const bool sizeChanged = (pxSize != d->lastDispatchedPxSize || !qFuzzyCompare(dpr, d->lastDispatchedDpr));

    auto &coord = ProcessGpuCoordinator::instance();
    const ProcessGpuState coordState = coord.currentState();
    const PhaseRendererRequirements &reqs = d->uiArena.rendererRequirements();

    QPhaseSurfaceCaps caps{};
    caps.isPopupOrTooltip = isPopupOrTooltipWindow(d->window);
    caps.vulkanWarmReady = (coordState == ProcessGpuState::WarmReady);
    caps.vulkanFailed = (coordState == ProcessGpuState::Failed);
    caps.isHardwareGpu = coord.isHardwareGpu();
    caps.forcedAffinity = d->forcedAffinity;
    caps.forcedBlocking = d->forcedBlocking;
    caps.hasCpuOnlyViews = (reqs.cpuOnlyViewCount > 0);
    caps.hasGpuOnlyViews = (reqs.gpuOnlyViewCount > 0);

    const qint64 nowUs = d->createTimer.nsecsElapsed() / 1000;
    const quint64 pendingUploads = d->uiArena.pendingGpuUploadBytes();

    QPhaseFrameRequest freq{};
    freq.pixelSize = pxSize;
    freq.logicalSize = finalLogicalSize;
    freq.devicePixelRatio = dpr;
    freq.isResize = isResize;
    freq.sizeChanged = sizeChanged;
    freq.hasActiveAnimations = d->uiArena.hasActiveAnimations();
    freq.primitiveCount = uint32_t(d->uiArena.drawOrder().size());
    freq.activeImageCount = uint32_t(d->uiArena.activeImageSlotCount());
    freq.pendingUploadBytes = pendingUploads;
    freq.dirtyAreaRatio = (isResize || sizeChanged || !d->hasPresentedFrame) ? 1.0f : d->reportedDirtyAreaRatio;
    freq.frameIndex = d->cpuFrames + d->gpuFrames;
    freq.msSinceLastFrame = d->lastFrameTimeUs > 0 ? std::max<qint64>(0, (nowUs - d->lastFrameTimeUs) / 1000) : 0;
    d->lastFrameTimeUs = nowUs;

    if (!d->policy)
        d->policy = QPhaseRenderPolicy::createFromEnvironment();

    const QPhasePolicyDecision decision = d->policy->decide(caps, freq);
    const bool switchingBackend = d->hasPresentedFrame && (decision.target != d->lastPresentedBackend);

    if (switchingBackend) {
        ++d->backendSwitches;
        if (sProfile) {
            std::fprintf(stderr, "[PhaseProfile Policy=%s] switch %s -> %s (%s, dirty=%.0f%%)\n",
                         d->policy->name(),
                         d->lastPresentedBackend == QPhaseBackendTarget::Gpu ? "GPU" : "CPU",
                         decision.target == QPhaseBackendTarget::Gpu ? "GPU" : "CPU",
                         decision.reason ? decision.reason : "",
                         double(freq.dirtyAreaRatio) * 100.0);
        }
    }

    if (decision.target == QPhaseBackendTarget::Gpu) {
        if (d->needReleaseBootResources) {
            d->needReleaseBootResources = false;
            pw->releasePhasedBootResources();
        }

        if (!switchingBackend
            && d->hasPresentedFrame
            && d->lastPresentedBackend == QPhaseBackendTarget::Gpu
            && d->uiArena.revision() == d->lastDispatchedRev
            && !sizeChanged
            && !freq.hasActiveAnimations) {
            return;
        }

        // Snapshot scene for zero-stall background CPU calibration BEFORE GPU upload frees cpuRgbaPremul!
        if (decision.requestAsyncCpuSample)
            launchAsyncCpuCalibration(pxSize, dpr, hasAlpha, frameTimeNs);

        const uint64_t seq = ++d->nextFrameSeq;
        if (sProfile) {
            std::fprintf(stderr, "[PhaseProfile UI] seq=%llu px=%dx%d sync=%.2fms prims=%zu imgs=%zu [%s:%s]\n",
                         static_cast<unsigned long long>(seq),
                         pxSize.width(), pxSize.height(), double(syncUs) / 1000.0,
                         d->uiArena.drawOrder().size(), d->uiArena.activeImageSlotCount(),
                         d->policy->name(), decision.reason ? decision.reason : "");
        }

        qint64 gpuSubmitUs = 0;
        qint64 gpuHwUs = 0;
        const bool okGpu = presentGpuFrame(pw, pxSize, finalLogicalSize, dpr, hasAlpha, frameTimeNs,
                                           decision.waitForGpuCompletion, &gpuSubmitUs, &gpuHwUs);

        QPhaseFrameOutcome outcome{};
        outcome.backend = QPhaseBackendTarget::Gpu;
        outcome.succeeded = okGpu;
        outcome.wasResize = isResize;
        outcome.cpuWorkUs = gpuSubmitUs;
        outcome.gpuHwUs = gpuHwUs;
        outcome.uploadedBytes = pendingUploads;
        outcome.dirtyAreaRatio = freq.dirtyAreaRatio;
        d->policy->recordOutcome(outcome);

        if (okGpu) {
            d->hasPresentedFrame = true;
            d->lastPresentedBackend = QPhaseBackendTarget::Gpu;
            d->lastDispatchedRev = d->uiArena.revision();
            d->lastDispatchedPxSize = pxSize;
            d->lastDispatchedDpr = dpr;
            if (decision.lockPermanent && !d->permanentLockActive)
                finalizeWinner(Phase::GpuSteadyState);
        } else {
            // Immediate safe fallback to CPU so we never drop a frame
            qint64 fallbackCpuUs = 0;
            if (presentCpuFrame(pw, pxSize, dpr, hasAlpha, frameTimeNs, &fallbackCpuUs)) {
                d->hasPresentedFrame = true;
                d->lastPresentedBackend = QPhaseBackendTarget::Cpu;
                d->lastDispatchedPxSize = pxSize;
                d->lastDispatchedDpr = dpr;
            } else {
                d->lastDispatchedRev = 0;
                d->presentDirty = true;
            }
        }
        return;
    }

    // Target == Cpu
    if (switchingBackend && d->lastPresentedBackend == QPhaseBackendTarget::Gpu) {
        d->uiArena.resetGpuSteadyState();
        d->cpuDamageTracker.reset();
        // Re-populate CPU QImage references (zero-copy!) if GPU steady-state already cleared them
        if (d->restoreCpuImagesCallback && d->syncCallback
            && d->uiArena.activeImageSlotCount() > 0 && d->uiArena.activeCpuImageBytes() == 0) {
            d->restoreCpuImagesCallback(d->uiArena);
            d->syncCallback(d->uiArena);
        }
    }

    qint64 cpuWorkUs = 0;
    const bool okCpu = presentCpuFrame(pw, pxSize, dpr, hasAlpha, frameTimeNs, &cpuWorkUs);

    QPhaseFrameOutcome outcome{};
    outcome.backend = QPhaseBackendTarget::Cpu;
    outcome.succeeded = okCpu;
    outcome.wasResize = isResize;
    outcome.cpuWorkUs = cpuWorkUs;
    outcome.dirtyAreaRatio = freq.dirtyAreaRatio;
    d->policy->recordOutcome(outcome);

    if (okCpu) {
        d->hasPresentedFrame = true;
        d->lastPresentedBackend = QPhaseBackendTarget::Cpu;
        d->lastDispatchedRev = d->uiArena.revision();
        d->lastDispatchedPxSize = pxSize;
        d->lastDispatchedDpr = dpr;
        if (sProfile) {
            std::fprintf(stderr, "[PhaseProfile CPU] seq=%llu px=%dx%d sync=%.2fms raster=%.2fms [%s:%s]\n",
                         static_cast<unsigned long long>(++d->nextFrameSeq),
                         pxSize.width(), pxSize.height(), double(syncUs) / 1000.0,
                         double(cpuWorkUs) / 1000.0,
                         d->policy->name(), decision.reason ? decision.reason : "");
        }
        if (decision.lockPermanent && !d->permanentLockActive)
            finalizeWinner(Phase::CpuSteadyState);
    }
}

QT_END_NAMESPACE

#include "moc_qphasesurfacepresenter_p.cpp"
