// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEWIDGETBRIDGE_P_H
#define QPHASEWIDGETBRIDGE_P_H

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

#include <QtWidgets/private/qtwidgetsglobal_p.h>
#include "qphasepaintengine_p.h"
#include <QtCore/qrect.h>
#include <QtGui/qregion.h>

#include <cstdint>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE

class QWidget;
class QWindow;
class QPhaseSceneArena;
class QPhaseSurfacePresenter;

// Retained per-QWidget scene arena slots (owned by QWidgetPrivate)
struct QWidgetPhaseNode {
    QPhaseSceneArena *arena = nullptr;
    QPhaseWidgetImageCache *imageCache = nullptr;
    uint32_t transformSlot = 0;
    uint32_t clipSlot = 0;
    uint32_t parentTransformSlot = 0;
    uint32_t parentClipSlot = 0;
    std::vector<uint32_t> primitiveSlots;
    std::vector<QRectF> primLocalBounds;
    std::vector<uint32_t> transientTransforms;
    std::vector<uint32_t> transientClips;
    std::vector<uint32_t> ownedImageSlots;
    uint64_t ownedImageEpoch = 0;
    QRect lastGeometry;
    QRegion dirtyRegion;
    bool visualsDirty = true;
    bool fullRepaintNeeded = true;
    uint8_t partialPatchCount = 0;

    ~QWidgetPhaseNode();
    void detach();
};

// Per-top-level-window coordinator connecting QWidgetWindow + QWidgetRepaintManager to QtPhase
class QPhaseWidgetBridge
{
public:
    static bool isEnabledFor(const QWidget *topLevelWidget);

    QPhaseWidgetBridge(QWidget *topLevelWidget, QWindow *widgetWindow);
    ~QPhaseWidgetBridge();

    Q_DISABLE_COPY_MOVE(QPhaseWidgetBridge)

    void markWidgetDirty(QWidget *widget, const QRegion &localRegion = QRegion());
    void markIntersectingDirty(const QRegion &tlwRegion);
    void markAllDirty();

    void scheduleUpdate(bool immediate = false);
    void onExpose();
    void onHide();
    void onResize();
    void shutdown();

    QPhaseSurfacePresenter *presenter() const noexcept { return m_presenter.get(); }

private:
    void syncSceneTree(QPhaseSceneArena &arena);
    void syncWidgetRecursive(QWidget *w,
                             QPhaseSceneArena &arena,
                             uint32_t parentTransformSlot,
                             uint32_t parentClipSlot,
                             std::vector<uint32_t> &outDrawOrder,
                             bool &drawOrderChanged);
    void paintSingleWidget(QWidget *w, QWidgetPhaseNode &node, QPhaseSceneArena &arena);

    QWidget *m_tlw = nullptr;
    QWindow *m_window = nullptr;
    std::unique_ptr<QPhaseSurfacePresenter> m_presenter;
    QPhaseWidgetImageCache m_imageCache;
    std::vector<uint32_t> m_lastDrawOrder;
    bool m_allDirty = true;
    bool m_inSyncSceneTree = false;
};

QT_END_NAMESPACE

#endif // QPHASEWIDGETBRIDGE_P_H
