// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEWINDOW_H
#define QPHASEWINDOW_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphaseview.h>
#include <QtGui/qwindow.h>
#include <QtGui/qcolor.h>
#include <QtGui/qregion.h>

#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE

class QPhaseWindowPrivate;

class Q_PHASE_EXPORT QPhaseWindow : public QWindow
{
    Q_OBJECT
public:
    enum class Phase : quint8 {
        WorkerRace = 0,
        CpuBoot = WorkerRace,
        GpuSteadyState = 1,
        CpuSteadyState = 2
    };

    enum class Worker : quint8 {
        CpuShm = 0,
        GpuVulkan = 1
    };

    static constexpr quint8 kWorkerLockInStreak = 5;

    struct Telemetry {
        qint64 showToFirstFrameCommitUs = 0;
        qint64 showToFirstCpuCommitUs = 0;
        qint64 showToGpuCutoverUs = 0;
        qint64 gpuAsyncWarmupUs = 0;
        quint64 cpuBootFramesPresented = 0;
        quint64 gpuSteadyFramesPresented = 0;
        quint64 cpuFramesPresented = 0;
        quint64 gpuFramesPresented = 0;
        quint64 abortedCpuFrames = 0;
        quint64 abortedGpuFrames = 0;
        quint64 gpuGlyphCacheHits = 0;
        quint64 gpuGlyphCacheMisses = 0;
        quint32 cpuBootTileThreads = 0;
        Phase lockedWinner = Phase::WorkerRace;
        QPhaseRendererAffinity activeAffinityConstraint = QPhaseRendererAffinity::Any;
        bool activeConstraintWasBlocking = false;
        bool dedicatedGpuRenderThread = false;
        bool lowLatencyReflexEnabled = true;
    };

    struct FrameTiming {
        quint32 frameIndex = 0;
        Phase phase = Phase::WorkerRace;
        Worker presentedBy = Worker::CpuShm;
        quint8 winStreak = 0;
        bool bothWorkersReady = false;
        bool opponentAborted = false;
        qint64 sinceWindowCreateUs = 0;
        qint64 sinceExposeUs = 0;
        qint64 interFrameDeltaUs = 0;
        qint64 backendWorkUs = 0;
        qint64 gpuHardwareUs = 0;
        qint64 sampleToPresentUs = 0;
        quint64 gpuSyncedBytes = 0;
        quint64 damagedPixels = 0;
        quint64 totalViewportPixels = 0;
    };

    explicit QPhaseWindow(QWindow *parent = nullptr);
    ~QPhaseWindow() override;

    void setBackgroundColor(const QColor &color);
    QColor backgroundColor() const noexcept;

    QPhaseView *rootView() const noexcept;
    QPhaseView *focusedView() const noexcept;

    // VSync-Deadline Input/Frame Pacer ("UI Reflex"): coalesces updates within a monitor
    // refresh interval and samples input/scroll state right before VSync scanout!
    void setLowLatencyReflexMode(bool enabled);
    bool isLowLatencyReflexMode() const noexcept;
    void requestFrameSample();

    // Optional window-wide renderer affinity & startup blocking policy (combines with per-widget
    // QPhaseView::setRendererAffinity constraints in the scene tree!)
    void setForcedRendererAffinity(QPhaseRendererAffinity affinity, bool blocking = false);
    QPhaseRendererAffinity forcedRendererAffinity() const noexcept;
    bool isForcedBackendBlocking() const noexcept;

    Phase currentPhase() const noexcept;
    Telemetry telemetry() const noexcept;
    std::vector<FrameTiming> frameTimings() const;

    void setSimulatedGpuInitDelayMs(int delayMs);
    void setSimulatedGpuFrameDelayMs(int delayMs);

    QObject *focusObject() const override;

Q_SIGNALS:
    void cutoverCompleted();
    void frameSampleRequested();

protected:
    virtual void setupUi(QPhaseView *root);
    virtual void beforeFrameSample();

    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;

private:
    friend void qt_phase_notify_window_dirty(QPhaseWindow *window);
    friend void qt_phase_set_focused_view(QPhaseWindow *window, QPhaseView *view);
    void startEarlyGpuWarmupThread();
    void ensureCpuWorkerThread();
    void scheduleReflexOrImmediateUpdate();
    void syncSceneTreeIfNeeded();
    void applyRendererAffinityConstraints();
    void renderFrame();
    void onCpuWorkerFrameDone(quint64 seq,
                              quint8 wonStreak,
                              bool bothWereReady,
                              Phase lockedPhase,
                              void *bufferHandle,
                              qint64 cpuWorkUs,
                              qint64 sampleTimestampUs,
                              const QRegion &logicalSurfaceDamage,
                              quint64 damagedPx,
                              quint64 totalPx);
    void finalizeWorkerWinner(Phase winningPhase);

    std::unique_ptr<QPhaseWindowPrivate> d_phase;
};

QT_END_NAMESPACE

#endif // QPHASEWINDOW_H
