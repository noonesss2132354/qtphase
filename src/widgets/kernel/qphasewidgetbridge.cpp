// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasewidgetbridge_p.h"
#include "qwidget_p.h"

#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qpainter.h>
#include <QtGui/qwindow.h>
#include <QtPhase/private/qphasescenearena_p.h>
#include <QtPhase/private/qphasesurfacepresenter_p.h>

QT_BEGIN_NAMESPACE

// ============================================================================
// QWidgetPhaseNode
// ============================================================================

QWidgetPhaseNode::~QWidgetPhaseNode()
{
    detach();
}

void QWidgetPhaseNode::detach()
{
    if (!arena)
        return;
    if (imageCache && !ownedImageSlots.empty()) {
        imageCache->releaseImageSlots(*arena, ownedImageEpoch, ownedImageSlots);
        ownedImageSlots.clear();
    }
    ownedImageEpoch = 0;
    for (uint32_t s : primitiveSlots)
        arena->freePrimitive(s);
    primitiveSlots.clear();
    for (uint32_t s : transientTransforms)
        arena->freeTransform(s);
    transientTransforms.clear();
    for (uint32_t s : transientClips)
        arena->freeClip(s);
    transientClips.clear();
    if (clipSlot != 0) {
        arena->freeClip(clipSlot);
        clipSlot = 0;
    }
    if (transformSlot != 0) {
        arena->freeTransform(transformSlot);
        transformSlot = 0;
    }
    parentTransformSlot = 0;
    parentClipSlot = 0;
    arena = nullptr;
    imageCache = nullptr;
    visualsDirty = true;
}

// ============================================================================
// QPhaseWidgetBridge
// ============================================================================

bool QPhaseWidgetBridge::isEnabledFor(const QWidget *topLevelWidget)
{
    if (!topLevelWidget || !topLevelWidget->isWindow())
        return false;
    if (QCoreApplication::testAttribute(Qt::AA_ForceRasterWidgets))
        return false;
    if (topLevelWidget->testAttribute(Qt::WA_DontShowOnScreen))
        return false;
    if (topLevelWidget->windowType() == Qt::ForeignWindow)
        return false;

    // Allow explicit opt-out via QT_WIDGETS_PHASE=0 for A/B benchmarking
    const QByteArray env = qgetenv("QT_WIDGETS_PHASE");
    if (!env.isEmpty() && env == "0")
        return false;

    // Only activate on Wayland where QWaylandPhasedWindow provides zero-tear PhasedSurface
    const QString platform = QGuiApplication::platformName();
    return platform.startsWith(QLatin1String("wayland"));
}

QPhaseWidgetBridge::QPhaseWidgetBridge(QWidget *topLevelWidget, QWindow *widgetWindow)
    : m_tlw(topLevelWidget)
    , m_window(widgetWindow)
    , m_presenter(std::make_unique<QPhaseSurfacePresenter>(widgetWindow))
{
    m_presenter->arena().setTextStemDarkening(0.28f);
    m_presenter->setSyncCallback([this](QPhaseSceneArena &arena) {
        syncSceneTree(arena);
    });
}

QPhaseWidgetBridge::~QPhaseWidgetBridge()
{
    shutdown();
}

void QPhaseWidgetBridge::shutdown()
{
    if (m_presenter) {
        m_presenter->shutdown();
        m_imageCache.clear(m_presenter->arena());
    }
    // Detach any QWidgetPhaseNodes still pointing to this arena before m_presenter is destroyed
    if (m_tlw) {
        auto detachTree = [](auto &self, QWidget *w) -> void {
            if (!w)
                return;
            QWidgetPrivate *wd = QWidgetPrivate::get(w);
            if (wd && wd->phaseNode)
                wd->phaseNode->detach();
            for (QObject *obj : wd->children) {
                if (auto *ch = qobject_cast<QWidget *>(obj)) {
                    if (!ch->isWindow())
                        self(self, ch);
                }
            }
        };
        detachTree(detachTree, m_tlw);
    }
    m_presenter.reset();
}

void QPhaseWidgetBridge::markWidgetDirty(QWidget *widget)
{
    if (!widget)
        return;
    QWidgetPrivate *wd = QWidgetPrivate::get(widget);
    if (!wd)
        return;
    if (wd->phaseNode)
        wd->phaseNode->visualsDirty = true;
}

void QPhaseWidgetBridge::markIntersectingDirty(const QRegion &tlwRegion)
{
    if (!m_tlw || tlwRegion.isEmpty())
        return;
    const QRect dirtyBounds = tlwRegion.boundingRect();
    auto walk = [&](auto &self, QWidget *w, const QPoint &offset) -> void {
        if (!w || w->isHidden())
            return;
        const QRect wRect(offset, w->size());
        if (!wRect.intersects(dirtyBounds))
            return;
        QWidgetPrivate *wd = QWidgetPrivate::get(w);
        if (wd->phaseNode)
            wd->phaseNode->visualsDirty = true;
        for (QObject *obj : wd->children) {
            if (auto *ch = qobject_cast<QWidget *>(obj)) {
                if (!ch->isWindow() && !ch->isHidden())
                    self(self, ch, offset + ch->pos());
            }
        }
    };
    walk(walk, m_tlw, QPoint(0, 0));
}

void QPhaseWidgetBridge::markAllDirty()
{
    m_allDirty = true;
}

void QPhaseWidgetBridge::scheduleUpdate(bool immediate)
{
    if (!m_presenter)
        return;
    if (immediate)
        m_presenter->presentNow();
    else
        m_presenter->schedulePresent();
}

void QPhaseWidgetBridge::onExpose()
{
    if (!m_presenter)
        return;
    m_presenter->presentNow();
}

void QPhaseWidgetBridge::onHide()
{
    if (!m_presenter)
        return;
    // Clear cached bindless image slots and force full re-registration on next expose,
    // because onSurfaceHidden() destroys the Vulkan backend and resets to WorkerRace.
    m_imageCache.clear(m_presenter->arena());
    m_allDirty = true;
    m_presenter->onSurfaceHidden();
}

void QPhaseWidgetBridge::onResize()
{
    // Do NOT set m_allDirty = true! syncWidgetRecursive() already marks only widgets whose
    // size() actually changed as visualsDirty, while moved-only widgets update their 32-byte
    // PhaseTransformNode with zero paintEvent() calls.
    // Present immediately (bypassing UI Reflex VSync sleep) so Wayland resize commits in ~2-3 ms!
    if (m_presenter)
        m_presenter->presentResizeNow();
}

void QPhaseWidgetBridge::syncSceneTree(QPhaseSceneArena &arena)
{
    if (!m_tlw || !m_tlw->isVisible() || m_inSyncSceneTree)
        return;
    if (QWidgetPrivate *tlwD = QWidgetPrivate::get(m_tlw); !tlwD || tlwD->data.in_destructor)
        return;

    m_inSyncSceneTree = true;

    // Flush pending QLayout activations before syncing widgets so a resize renders the final
    // laid-out widget positions in 1 single frame instead of a stale pre-layout frame + 2 extra commits!
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);

    m_imageCache.sweepDeadEntries(arena);

    const QColor bg = m_tlw->testAttribute(Qt::WA_TranslucentBackground)
            ? QColor(0, 0, 0, 0)
            : m_tlw->palette().color(QPalette::Window);
    arena.setClearColor(bg);

    std::vector<uint32_t> drawOrder;
    drawOrder.reserve(m_lastDrawOrder.size() + 32);
    bool drawOrderChanged = false;

    syncWidgetRecursive(m_tlw, arena, 0u, 0u, drawOrder, drawOrderChanged);
    m_allDirty = false;

    if (drawOrderChanged || drawOrder != m_lastDrawOrder) {
        m_lastDrawOrder = drawOrder;
        arena.setDrawOrder(std::move(drawOrder));
    }

    m_inSyncSceneTree = false;
}

void QPhaseWidgetBridge::syncWidgetRecursive(QWidget *w,
                                             QPhaseSceneArena &arena,
                                             uint32_t parentTransformSlot,
                                             uint32_t parentClipSlot,
                                             std::vector<uint32_t> &outDrawOrder,
                                             bool &drawOrderChanged)
{
    if (!w || w->isHidden())
        return;

    QWidgetPrivate *wd = QWidgetPrivate::get(w);
    if (!wd || wd->data.in_destructor)
        return;

    if (!wd->phaseNode)
        wd->phaseNode = std::make_unique<QWidgetPhaseNode>();

    QWidgetPhaseNode &node = *wd->phaseNode;
    if (node.arena && node.arena != &arena)
        node.detach();

    const QRect geom = (w == m_tlw) ? QRect(QPoint(0, 0), w->size()) : w->geometry();
    const bool needAlloc = (node.arena == nullptr || node.transformSlot == 0 || node.clipSlot == 0);
    const bool parentChanged = (node.parentTransformSlot != parentTransformSlot
                                || node.parentClipSlot != parentClipSlot);

    if (needAlloc || parentChanged) {
        if (!needAlloc)
            node.detach();
        node.arena = &arena;
        node.imageCache = &m_imageCache;
        node.parentTransformSlot = parentTransformSlot;
        node.parentClipSlot = parentClipSlot;
        node.transformSlot = arena.allocateTransform(parentTransformSlot);
        node.clipSlot = arena.allocateClip(parentClipSlot, node.transformSlot,
                                           QRectF(0.0, 0.0, std::max(0, geom.width()), std::max(0, geom.height())),
                                           QPhaseCorners{});
        node.visualsDirty = true;
        drawOrderChanged = true;
    }

    // Update transform & clip whenever geometry moved/resized (moving a widget costs 0 paintEvents!)
    if (needAlloc || node.lastGeometry != geom) {
        const bool sizeChanged = (node.lastGeometry.size() != geom.size());
        arena.updateTransform(node.transformSlot, float(geom.x()), float(geom.y()), 1.0f, 1.0f, 1.0f);
        arena.updateClip(node.clipSlot,
                         QRectF(0.0, 0.0, std::max(0, geom.width()), std::max(0, geom.height())),
                         QPhaseCorners{});
        node.lastGeometry = geom;
        if (sizeChanged)
            node.visualsDirty = true;
    }

    if (m_allDirty || node.visualsDirty) {
        paintSingleWidget(w, node, arena);
        node.visualsDirty = false;
        drawOrderChanged = true;
    }

    for (uint32_t slot : node.primitiveSlots)
        outDrawOrder.push_back(slot);

    // Recurse into non-window children in exact Qt stacking order
    for (QObject *obj : wd->children) {
        if (auto *ch = qobject_cast<QWidget *>(obj)) {
            if (!ch->isWindow() && !ch->isHidden()) {
                syncWidgetRecursive(ch, arena, node.transformSlot, node.clipSlot,
                                    outDrawOrder, drawOrderChanged);
            }
        }
    }
}

void QPhaseWidgetBridge::paintSingleWidget(QWidget *w, QWidgetPhaseNode &node, QPhaseSceneArena &arena)
{
    QWidgetPrivate *wd = QWidgetPrivate::get(w);
    if (!wd)
        return;

    // Release previous transient transforms & clips emitted by this widget's last paintEvent
    for (uint32_t s : node.transientTransforms)
        arena.freeTransform(s);
    node.transientTransforms.clear();
    for (uint32_t s : node.transientClips)
        arena.freeClip(s);
    node.transientClips.clear();

    if (w->width() <= 0 || w->height() <= 0) {
        if (!node.ownedImageSlots.empty()) {
            m_imageCache.releaseImageSlots(arena, node.ownedImageEpoch, node.ownedImageSlots);
            node.ownedImageSlots.clear();
        }
        for (uint32_t s : node.primitiveSlots)
            arena.freePrimitive(s);
        node.primitiveSlots.clear();
        return;
    }

    std::vector<PhaseGpuPrimitive> recorded;
    recorded.reserve(std::max<size_t>(node.primitiveSlots.size(), 8));

    // Record into new ownedImageSlots first so unchanged textures stay ref-counted & hot in VRAM,
    // then immediately release stale resize-width pixmaps/fallbacks from the previous paint!
    std::vector<uint32_t> prevOwnedImageSlots = std::move(node.ownedImageSlots);
    const uint64_t prevOwnedImageEpoch = node.ownedImageEpoch;
    node.ownedImageSlots.clear();
    node.ownedImageEpoch = m_imageCache.epoch;

    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1.0;
    QPhasePaintEngine engine(arena, m_imageCache,
                             node.transformSlot, node.clipSlot, dpr,
                             recorded, node.transientTransforms, node.transientClips,
                             node.ownedImageSlots);
    QPhasePaintDevice pdev(w, &engine);

    const bool asRoot = (w == m_tlw);
    const QRegion fullRgn(w->rect());

    w->setAttribute(Qt::WA_WState_InPaintEvent, true);
    wd->setRedirected(&pdev, QPoint(0, 0));

    // QAbstractScrollArea::viewportEvent(QEvent::Paint) forwards to QFrame::event(e) on parentWidget(),
    // which opens QPainter(parentWidget()). Redirect parent temporarily so QWidget::paintEngine() is never hit!
    QWidget *redirectedParent = nullptr;
    if (QWidget *p = w->parentWidget(); p && p != m_tlw
        && w->objectName() == QLatin1StringView("qt_scrollarea_viewport")
        && !p->testAttribute(Qt::WA_WState_InPaintEvent)) {
        redirectedParent = p;
        p->setAttribute(Qt::WA_WState_InPaintEvent, true);
        QWidgetPrivate::get(p)->setRedirected(&pdev, w->pos());
    }

    // 1. Standard Qt Widget background (autoFillBackground / WA_StyledBackground / root palette)
    if ((asRoot || w->autoFillBackground() || w->testAttribute(Qt::WA_StyledBackground))
        && !w->testAttribute(Qt::WA_OpaquePaintEvent)
        && !w->testAttribute(Qt::WA_NoSystemBackground)) {
        QPainter bgPainter(w);
        if (bgPainter.isActive()) {
            wd->paintBackground(&bgPainter, fullRgn,
                                asRoot ? QWidgetPrivate::DrawAsRoot : QWidgetPrivate::DrawWidgetFlags());
        }
    }

    // 2. Dispatch standard QWidget::paintEvent(QPaintEvent *) -> redirects automatically to QPhasePaintEngine!
    wd->sendPaintEvent(fullRgn);

    if (redirectedParent) {
        QWidgetPrivate::get(redirectedParent)->restoreRedirected();
        redirectedParent->setAttribute(Qt::WA_WState_InPaintEvent, false);
    }
    wd->restoreRedirected();
    w->setAttribute(Qt::WA_WState_InPaintEvent, false);

    if (!prevOwnedImageSlots.empty())
        m_imageCache.releaseImageSlots(arena, prevOwnedImageEpoch, prevOwnedImageSlots);

    // 3. Reconcile retained arena primitive slots in-place (reusing existing slots where possible!)
    const size_t newCount = recorded.size();
    const size_t oldCount = node.primitiveSlots.size();

    for (size_t i = newCount; i < oldCount; ++i)
        arena.freePrimitive(node.primitiveSlots[i]);
    if (newCount < oldCount)
        node.primitiveSlots.resize(newCount);

    for (size_t i = 0; i < newCount; ++i) {
        if (i < oldCount)
            arena.updatePrimitive(node.primitiveSlots[i], recorded[i]);
        else
            node.primitiveSlots.push_back(arena.allocatePrimitive(recorded[i]));
    }
}

QT_END_NAMESPACE
