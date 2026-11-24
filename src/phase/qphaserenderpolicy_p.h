// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASERENDERPOLICY_P_H
#define QPHASERENDERPOLICY_P_H

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
#include <QtCore/qbytearray.h>
#include <QtCore/qbytearrayview.h>
#include <QtCore/qlist.h>
#include <QtCore/qsize.h>

#include <cstdint>
#include <memory>

QT_BEGIN_NAMESPACE

enum class QPhaseBackendTarget : quint8 {
    Cpu = 0,
    Gpu = 1,
};

// Toolkit-agnostic semantic widget hints (bitmask) passed from upper UI layers
// (e.g. QPhaseWidgetBridge) to QPhaseRenderPolicy without leaking QtWidgets into QtPhase.
enum QPhaseSurfaceHint : uint32_t {
    QPhaseHintNone                = 0u,
    QPhaseHintDialogWindow        = 1u << 0, // QDialog / QMessageBox / settings dialog
    QPhaseHintTextInputDirty      = 1u << 1, // QLineEdit / QSpinBox / QTextEdit localized edit or cursor
    QPhaseHintControlChromeDirty  = 1u << 2, // Buttons, tabs, status bar, progress bar, scrollbars
    QPhaseHintScrollViewportDirty = 1u << 3, // QAbstractScrollArea viewport (lists, trees, PDF, terminal)
    QPhaseHintHeavyCanvasDirty    = 1u << 4, // Per-widget cached CPU raster or heavy fallback canvas
    QPhaseHintComplexPaintWidget  = 1u << 5, // CompositionMode / curved clipPath / QGraphicsEffect present
};

struct QPhaseSurfaceCaps {
    bool isPopupOrTooltip = false;
    bool vulkanWarmReady = false;
    bool vulkanFailed = false;
    bool isHardwareGpu = false;
    QPhaseRendererAffinity forcedAffinity = QPhaseRendererAffinity::Any;
    bool forcedBlocking = false;
    bool hasCpuOnlyViews = false;
    bool hasGpuOnlyViews = false;
    uint32_t surfaceHints = QPhaseHintNone;
};

struct QPhaseFrameRequest {
    QSize pixelSize;
    QSize logicalSize;
    qreal devicePixelRatio = 1.0;
    bool isResize = false;
    bool sizeChanged = false;
    bool hasActiveAnimations = false;
    uint32_t primitiveCount = 0;
    uint32_t activeImageCount = 0;
    quint64 pendingUploadBytes = 0;
    float dirtyAreaRatio = 1.0f; // 0.0..1.0 fraction of surface area modified this frame
    quint64 frameIndex = 0;
    qint64 msSinceLastFrame = 0;
    uint32_t dirtyHints = QPhaseHintNone;
};

struct QPhaseFrameOutcome {
    QPhaseBackendTarget backend = QPhaseBackendTarget::Cpu;
    bool succeeded = true;
    bool wasResize = false;
    qint64 cpuWorkUs = 0;       // CPU raster time (Cpu) or CPU submit time (Gpu)
    qint64 gpuHwUs = 0;         // Measured Vulkan GPU timestamp (0 if not yet completed)
    quint64 uploadedBytes = 0;
    float dirtyAreaRatio = 1.0f;
};

struct QPhasePolicyDecision {
    QPhaseBackendTarget target = QPhaseBackendTarget::Cpu;
    bool waitForGpuCompletion = false;   // Synchronous fence wait (for sync-race probes)
    bool requestAsyncCpuSample = false;  // Run 1 background offscreen CPU sample (0 ms UI stall)
    bool lockPermanent = false;          // Safe to release losing backend resources permanently
    const char *reason = "";             // Short diagnostic tag for QT_WIDGETS_PHASE_PROFILE
};

// Pluggable dual-pipeline scheduling / racing algorithm.
// Select at runtime via QT_PHASE_POLICY=<name> (set QT_PHASE_POLICY=list to print options)
// or inject a custom instance via QPhaseSurfacePresenter::setRenderPolicy().
class Q_PHASE_EXPORT QPhaseRenderPolicy
{
public:
    virtual ~QPhaseRenderPolicy() = default;

    virtual const char *name() const noexcept = 0;
    virtual QPhasePolicyDecision decide(const QPhaseSurfaceCaps &caps,
                                        const QPhaseFrameRequest &req) = 0;
    virtual void recordOutcome(const QPhaseFrameOutcome &outcome) = 0;
    virtual void recordAsyncCpuSample(qint64 cpuSampleUs) { Q_UNUSED(cpuSampleUs); }
    virtual void reset() {}

    static std::unique_ptr<QPhaseRenderPolicy> create(QByteArrayView name);
    static std::unique_ptr<QPhaseRenderPolicy> createFromEnvironment();
    static QList<QByteArray> availablePolicies();
};

QT_END_NAMESPACE

#endif // QPHASERENDERPOLICY_P_H
