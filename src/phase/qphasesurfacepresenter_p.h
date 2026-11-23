// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASESURFACEPRESENTER_P_H
#define QPHASESURFACEPRESENTER_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphasestyles.h>
#include <QtCore/qobject.h>
#include <QtCore/qpointer.h>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE

class QWindow;
class QPhaseSceneArena;
class QPhaseSurfacePresenterPrivate;

class Q_PHASE_EXPORT QPhaseSurfacePresenter : public QObject
{
    Q_OBJECT
public:
    enum class Phase : quint8 {
        WorkerRace = 0,
        GpuSteadyState = 1,
        CpuSteadyState = 2
    };

    explicit QPhaseSurfacePresenter(QWindow *window, QObject *parent = nullptr);
    ~QPhaseSurfacePresenter() override;

    Q_DISABLE_COPY_MOVE(QPhaseSurfacePresenter)

    QPhaseSceneArena &arena() noexcept;
    const QPhaseSceneArena &arena() const noexcept;

    void setSyncCallback(std::function<void(QPhaseSceneArena &)> callback);
    void setForcedAffinity(QPhaseRendererAffinity affinity, bool blocking = false);

    // Schedules a UI-Reflex VSync-deadline coalesced sync+present
    void schedulePresent();

    // Immediately syncs and dispatches a frame (used on Expose / Resize / UpdateNow)
    void presentNow();

    // Called from QWidgetWindow::handleResizeEvent() to update expected Wayland size immediately
    // and dispatch a zero-delay resize frame without waiting on UI Reflex!
    void presentResizeNow();

    // Stops background workers before QWindow::destroy() tears down the platform surface
    void onSurfaceHidden();
    void shutdown();

    Phase currentPhase() const noexcept;
    quint64 cpuFramesPresented() const noexcept;
    quint64 gpuFramesPresented() const noexcept;

private:
    void startEarlyGpuWarmupThread();
    void ensureCpuBootThreadStarted();
    void finalizeWorkerWinner(Phase winningPhase);
    void onCpuWorkerFrameDone(quint64 seq,
                              quint8 wonStreak,
                              Phase lockedPhase,
                              void *bufferHandle,
                              qint64 cpuWorkUs,
                              const QRegion &logicalSurfaceDamage);
    void onGpuWorkerFrameDone(quint64 seq,
                              quint8 wonStreak,
                              Phase lockedPhase,
                              qint64 gpuWorkUs,
                              qint64 gpuHwUs,
                              quint64 syncedBytes,
                              quint64 uploadedImageGen);

    std::unique_ptr<QPhaseSurfacePresenterPrivate> d;
};

QT_END_NAMESPACE

#endif // QPHASESURFACEPRESENTER_P_H
