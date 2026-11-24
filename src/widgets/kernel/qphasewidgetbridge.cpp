// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasewidgetbridge_p.h"
#include "qwidget_p.h"

#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qpainter.h>
#include <QtGui/qwindow.h>
#include <QtPhase/private/qphaserenderpolicy_p.h>
#include <QtPhase/private/qphasescenearena_p.h>
#include <QtPhase/private/qphasesurfacepresenter_p.h>

#include <cmath>
#include <cstdio>

QT_BEGIN_NAMESPACE

namespace {

enum class WidgetHybridMode : uint8_t {
    Off = 0,
    Correctness = 1, // Default: route only CompositionMode / complex clipPath / QGraphicsEffect to QRasterPaintEngine
    Aggressive = 2,  // Experimental: also route ultra-dense glyph (>2000) & dynamic fallback widgets to CachedCpuRaster
};

inline WidgetHybridMode widgetHybridMode() noexcept
{
    static const WidgetHybridMode mode = []() {
        const QByteArray env = qgetenv("QT_PHASE_WIDGET_HYBRID").trimmed().toLower();
        if (env == "0" || env == "off" || env == "none")
            return WidgetHybridMode::Off;
        if (env == "aggressive" || env == "all" || env == "2")
            return WidgetHybridMode::Aggressive;
        return WidgetHybridMode::Correctness;
    }();
    return mode;
}

inline bool isWidgetHybridEnabled() noexcept
{
    return widgetHybridMode() != WidgetHybridMode::Off;
}

uint32_t classifyDirtyWidgetHints(const QWidget *w, const QWidgetPhaseNode &node, qint64 widgetDirtyArea)
{
    if (!w)
        return QPhaseHintNone;

    uint32_t hints = QPhaseHintNone;
    if (node.renderMode == QWidgetPhaseNode::WidgetRenderMode::CachedCpuRaster)
        hints |= QPhaseHintHeavyCanvasDirty;
    if (node.forcedCpuForCorrectness)
        hints |= QPhaseHintComplexPaintWidget;

    const qint64 wArea = std::max<qint64>(1, qint64(w->width()) * qint64(w->height()));
    const bool isViewport = (w->objectName() == QLatin1StringView("qt_scrollarea_viewport"));
    if (isViewport) {
        if (const QWidget *p = w->parentWidget()) {
            if ((p->inherits("QPlainTextEdit") || p->inherits("QTextEdit"))
                && (widgetDirtyArea * 10 < wArea)) {
                hints |= QPhaseHintTextInputDirty;
                return hints;
            }
        }
        hints |= QPhaseHintScrollViewportDirty;
        return hints;
    }

    if (w->inherits("QLineEdit") || w->inherits("QAbstractSpinBox")
        || w->inherits("QTextEdit") || w->inherits("QPlainTextEdit")) {
        hints |= QPhaseHintTextInputDirty;
    } else if (w->inherits("QAbstractScrollArea")
               || w->inherits("Konsole::TerminalDisplay")
               || w->inherits("KItemListView")) {
        hints |= QPhaseHintScrollViewportDirty;
    } else if (w->inherits("QAbstractButton") || w->inherits("QTabBar")
               || w->inherits("QScrollBar") || w->inherits("QProgressBar")
               || w->inherits("QStatusBar") || w->inherits("QLabel")
               || w->inherits("QComboBox") || w->inherits("QToolBar")
               || w->inherits("QMenuBar") || w->inherits("QHeaderView")) {
        hints |= QPhaseHintControlChromeDirty;
    }
    return hints;
}

} // namespace

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
    primLocalBounds.clear();
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
    dirtyRegion = QRegion();
    visualsDirty = true;
    fullRepaintNeeded = true;
    partialPatchCount = 0;
    renderMode = WidgetRenderMode::NativePhase;
    forcedCpuForCorrectness = false;
    heavyNativeStreak = 0;
    cachedRasterSurface = QImage();
    cachedRasterDpr = 0.0;
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

    return QPhaseSurfacePresenter::isPlatformSupported();
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
    m_presenter->setRestoreCpuImagesCallback([this](QPhaseSceneArena &arena) {
        m_imageCache.clear(arena);
        m_allDirty = true;
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

void QPhaseWidgetBridge::markWidgetDirty(QWidget *widget, const QRegion &localRegion)
{
    if (!widget)
        return;
    QWidgetPrivate *wd = QWidgetPrivate::get(widget);
    if (!wd)
        return;
    if (wd->phaseNode) {
        wd->phaseNode->visualsDirty = true;
        if (localRegion.isEmpty()) {
            wd->phaseNode->fullRepaintNeeded = true;
            wd->phaseNode->dirtyRegion = QRegion();
        } else if (!wd->phaseNode->fullRepaintNeeded) {
            wd->phaseNode->dirtyRegion += localRegion;
        }
    }
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
        if (wd->phaseNode) {
            wd->phaseNode->visualsDirty = true;
            wd->phaseNode->fullRepaintNeeded = true;
            wd->phaseNode->dirtyRegion = QRegion();
        }
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
    qint64 dirtyArea = 0;
    uint32_t dirtyHints = QPhaseHintNone;

    syncWidgetRecursive(m_tlw, arena, 0u, 0u, drawOrder, drawOrderChanged, dirtyArea, dirtyHints);
    m_allDirty = false;

    const qint64 tlwArea = std::max<qint64>(1, qint64(m_tlw->width()) * qint64(m_tlw->height()));
    if (m_presenter) {
        m_presenter->reportDirtyAreaRatio(float(std::min<double>(1.0, double(dirtyArea) / double(tlwArea))));
        uint32_t surfaceHints = QPhaseHintNone;
        if (m_tlw->inherits("QDialog") || (m_tlw->windowFlags() & Qt::WindowType_Mask) == Qt::Dialog)
            surfaceHints |= QPhaseHintDialogWindow;
        m_presenter->reportFrameHints(surfaceHints, dirtyHints);
    }

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
                                             bool &drawOrderChanged,
                                             qint64 &outDirtyArea,
                                             uint32_t &outDirtyHints)
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
        node.fullRepaintNeeded = true;
        node.dirtyRegion = QRegion();
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
        if (sizeChanged) {
            node.visualsDirty = true;
            node.fullRepaintNeeded = true;
            node.dirtyRegion = QRegion();
        } else {
            outDirtyArea += qint64(std::max(0, geom.width())) * qint64(std::max(0, geom.height()));
        }
    }

    if (m_allDirty || node.visualsDirty) {
        if (m_allDirty) {
            node.fullRepaintNeeded = true;
            node.dirtyRegion = QRegion();
        }
        qint64 widgetDirtyArea = 0;
        if (node.fullRepaintNeeded || node.dirtyRegion.isEmpty()) {
            widgetDirtyArea = qint64(std::max(0, w->width())) * qint64(std::max(0, w->height()));
        } else {
            const QRect dbr = node.dirtyRegion.boundingRect() & w->rect();
            widgetDirtyArea = qint64(std::max(0, dbr.width())) * qint64(std::max(0, dbr.height()));
        }
        outDirtyArea += widgetDirtyArea;
        paintSingleWidget(w, node, arena);
        outDirtyHints |= classifyDirtyWidgetHints(w, node, widgetDirtyArea);
        node.visualsDirty = false;
        node.fullRepaintNeeded = false;
        node.dirtyRegion = QRegion();
        drawOrderChanged = true;
    }

    for (uint32_t slot : node.primitiveSlots)
        outDrawOrder.push_back(slot);

    // Recurse into non-window children in exact Qt stacking order
    for (QObject *obj : wd->children) {
        if (auto *ch = qobject_cast<QWidget *>(obj)) {
            if (!ch->isWindow() && !ch->isHidden()) {
                syncWidgetRecursive(ch, arena, node.transformSlot, node.clipSlot,
                                    outDrawOrder, drawOrderChanged, outDirtyArea, outDirtyHints);
            }
        }
    }
}

void QPhaseWidgetBridge::paintSingleWidget(QWidget *w, QWidgetPhaseNode &node, QPhaseSceneArena &arena)
{
    QWidgetPrivate *wd = QWidgetPrivate::get(w);
    if (!wd)
        return;

    if (w->width() <= 0 || w->height() <= 0) {
        for (uint32_t s : node.transientTransforms)
            arena.freeTransform(s);
        node.transientTransforms.clear();
        for (uint32_t s : node.transientClips)
            arena.freeClip(s);
        node.transientClips.clear();
        if (!node.ownedImageSlots.empty()) {
            m_imageCache.releaseImageSlots(arena, node.ownedImageEpoch, node.ownedImageSlots);
            node.ownedImageSlots.clear();
        }
        for (uint32_t s : node.primitiveSlots)
            arena.freePrimitive(s);
        node.primitiveSlots.clear();
        node.primLocalBounds.clear();
        node.partialPatchCount = 0;
        return;
    }

    const QRect wRect = w->rect();
    const qint64 wArea = qint64(wRect.width()) * qint64(wRect.height());
    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1.0;
    const bool asRoot = (w == m_tlw);

    // Level 2 Per-Widget Hybrid: if this non-root widget has been routed to direct QRasterPaintEngine
    // (for CompositionMode/complex clipPath/QGraphicsEffect correctness, heavy dynamic fallback, or
    // ultra-dense >2000 glyph viewports), render directly into its cached BGRA surface -> 1 Vulkan quad!
    const bool hasGfxEffect =
#if QT_CONFIG(graphicseffect)
            (wd->graphicsEffect != nullptr);
#else
            false;
#endif
    if (!asRoot && isWidgetHybridEnabled()
        && (node.renderMode == QWidgetPhaseNode::WidgetRenderMode::CachedCpuRaster || hasGfxEffect)) {
        if (hasGfxEffect) {
            node.renderMode = QWidgetPhaseNode::WidgetRenderMode::CachedCpuRaster;
            node.forcedCpuForCorrectness = true;
        }
        paintSingleWidgetCachedCpu(w, node, arena, m_allDirty || node.fullRepaintNeeded);
        return;
    }

    // Fast Sub-Rect Partial Patch Path for large viewports (e.g. Dolphin KItemListView, QTreeView, QTextEdit):
    // When only a small sub-rect changed (like moving row selection or blinking cursor), clip the new patch
    // to the dirty rect and splice only the changed primitives in place.
    // Safety guarantee: if the patch paints an opaque background covering `expanded` (e.g. QAbstractScrollArea
    // viewports, QTreeView, KItemListView), any straddling old primitives (like vertical branch lines or column
    // dividers) are cleanly overdrawn inside `expanded` and untouched outside. If the widget is transparent
    // (no opaque cover), we only accept the patch when zero non-background primitives straddle `expanded`.
    if (!node.fullRepaintNeeded
        && !node.dirtyRegion.isEmpty()
        && !node.primitiveSlots.empty()
        && node.primLocalBounds.size() == node.primitiveSlots.size()
        && node.partialPatchCount < 16
        && wRect.width() >= 256 && wRect.height() >= 128) {
        const QRect expanded = node.dirtyRegion.boundingRect() & wRect;
        if (!expanded.isEmpty()
            && qint64(expanded.width()) * qint64(expanded.height()) * 100 < wArea * 35) {
            const uint32_t patchClipSlot = arena.allocateClip(
                    node.clipSlot, node.transformSlot, QRectF(expanded), QPhaseCorners{});
            const size_t prevTransTxCount = node.transientTransforms.size();
            const size_t prevTransClipCount = node.transientClips.size();
            const size_t prevOwnedImgCount = node.ownedImageSlots.size();
            node.transientClips.push_back(patchClipSlot);

            std::vector<PhaseGpuPrimitive> recorded;
            std::vector<QRectF> recordedBounds;
            recorded.reserve(32);
            recordedBounds.reserve(32);

            QPhasePaintEngine engine(arena, m_imageCache,
                                     node.transformSlot, patchClipSlot, dpr,
                                     recorded, node.transientTransforms, node.transientClips,
                                     node.ownedImageSlots, &recordedBounds);
            QPhasePaintDevice pdev(w, &engine);
            const QRegion patchRgn(expanded);

            w->setAttribute(Qt::WA_WState_InPaintEvent, true);
            wd->setRedirected(&pdev, QPoint(0, 0));

            QWidget *redirectedParent = nullptr;
            if (QWidget *p = w->parentWidget(); p && p != m_tlw
                && w->objectName() == QLatin1StringView("qt_scrollarea_viewport")
                && !p->testAttribute(Qt::WA_WState_InPaintEvent)) {
                redirectedParent = p;
                p->setAttribute(Qt::WA_WState_InPaintEvent, true);
                QWidgetPrivate::get(p)->setRedirected(&pdev, w->pos());
            }

            if ((asRoot || w->autoFillBackground() || w->testAttribute(Qt::WA_StyledBackground))
                && !w->testAttribute(Qt::WA_OpaquePaintEvent)
                && !w->testAttribute(Qt::WA_NoSystemBackground)) {
                QPainter bgPainter(w);
                if (bgPainter.isActive()) {
                    wd->paintBackground(&bgPainter, patchRgn,
                                        asRoot ? QWidgetPrivate::DrawAsRoot : QWidgetPrivate::DrawWidgetFlags());
                }
            }

            wd->sendPaintEvent(patchRgn);

            if (redirectedParent) {
                QWidgetPrivate::get(redirectedParent)->restoreRedirected();
                redirectedParent->setAttribute(Qt::WA_WState_InPaintEvent, false);
            }
            wd->restoreRedirected();
            w->setAttribute(Qt::WA_WState_InPaintEvent, false);

            // Check whether the recorded patch opaquely covers `expanded`
            QRegion opaqueCovered;
            for (const PhaseGpuPrimitive &p : recorded) {
                const bool isOpaqueSolid = (p.kind == uint32_t(PhasePrimKind::SolidRect)
                                            || (p.kind == uint32_t(PhasePrimKind::RoundedBox)
                                                && p.rTL == 0.0f && p.rTR == 0.0f
                                                && p.rBR == 0.0f && p.rBL == 0.0f))
                        && p.transformIndex == node.transformSlot
                        && p.clipIndex == patchClipSlot
                        && p.c0a >= 0.999f
                        && (p.resourceIndex == 0u || p.c1a >= 0.999f);
                if (isOpaqueSolid) {
                    const QRect r = QRectF(p.x, p.y, p.w, p.h).toRect() & expanded;
                    if (!r.isEmpty()) {
                        opaqueCovered += r;
                        if ((patchRgn - opaqueCovered).isEmpty())
                            break;
                    }
                }
            }
            const bool hasOpaqueCover = (patchRgn - opaqueCovered).isEmpty();

            const QRectF expF(expanded);
            const QRectF containmentBox = expF.adjusted(-2.0, -2.0, 2.0, 2.0);
            auto isFullBgPrim = [wArea](const QRectF &b) noexcept {
                return (b.width() * b.height()) >= double(wArea) * 0.5;
            };

            bool canSplice = hasOpaqueCover;
            if (!canSplice) {
                canSplice = true;
                for (const QRectF &b : node.primLocalBounds) {
                    if (isFullBgPrim(b))
                        continue;
                    if (b.intersects(expF) && !containmentBox.contains(b)) {
                        canSplice = false;
                        break;
                    }
                }
            }

            if (canSplice) {
                // Free every old non-background primitive contained inside `expanded`
                size_t writeIdx = 0;
                for (size_t i = 0; i < node.primitiveSlots.size(); ++i) {
                    const QRectF &b = node.primLocalBounds[i];
                    if (!isFullBgPrim(b) && containmentBox.contains(b)) {
                        arena.freePrimitive(node.primitiveSlots[i]);
                    } else {
                        if (writeIdx != i) {
                            node.primitiveSlots[writeIdx] = node.primitiveSlots[i];
                            node.primLocalBounds[writeIdx] = node.primLocalBounds[i];
                        }
                        ++writeIdx;
                    }
                }
                node.primitiveSlots.resize(writeIdx);
                node.primLocalBounds.resize(writeIdx);

                for (size_t i = 0; i < recorded.size(); ++i) {
                    node.primitiveSlots.push_back(arena.allocatePrimitive(recorded[i]));
                    node.primLocalBounds.push_back(recordedBounds[i] & expF);
                }
                ++node.partialPatchCount;
                return;
            }

            // Transparent widget with straddling primitives: roll back transient allocations & do clean full repaint
            for (size_t i = prevTransTxCount; i < node.transientTransforms.size(); ++i)
                arena.freeTransform(node.transientTransforms[i]);
            node.transientTransforms.resize(prevTransTxCount);
            for (size_t i = prevTransClipCount; i < node.transientClips.size(); ++i)
                arena.freeClip(node.transientClips[i]);
            node.transientClips.resize(prevTransClipCount);
            if (node.ownedImageSlots.size() > prevOwnedImgCount) {
                std::vector<uint32_t> extraImgs(node.ownedImageSlots.begin() + ptrdiff_t(prevOwnedImgCount),
                                                node.ownedImageSlots.end());
                node.ownedImageSlots.resize(prevOwnedImgCount);
                m_imageCache.releaseImageSlots(arena, node.ownedImageEpoch, extraImgs);
            }
        }
    }

    // Full widget repaint path (resizes, first paint, small widgets, or periodic compaction)
    node.partialPatchCount = 0;
    for (uint32_t s : node.transientTransforms)
        arena.freeTransform(s);
    node.transientTransforms.clear();
    for (uint32_t s : node.transientClips)
        arena.freeClip(s);
    node.transientClips.clear();

    std::vector<PhaseGpuPrimitive> recorded;
    std::vector<QRectF> recordedBounds;
    recorded.reserve(std::max<size_t>(node.primitiveSlots.size(), 8));
    recordedBounds.reserve(std::max<size_t>(node.primitiveSlots.size(), 8));

    // Record into new ownedImageSlots first so unchanged textures stay ref-counted & hot in VRAM,
    // then immediately release stale resize-width pixmaps/fallbacks from the previous paint!
    std::vector<uint32_t> prevOwnedImageSlots = std::move(node.ownedImageSlots);
    const uint64_t prevOwnedImageEpoch = node.ownedImageEpoch;
    node.ownedImageSlots.clear();
    node.ownedImageEpoch = m_imageCache.epoch;

    QPhaseWidgetPaintStats paintStats{};
    QPhasePaintEngine engine(arena, m_imageCache,
                             node.transformSlot, node.clipSlot, dpr,
                             recorded, node.transientTransforms, node.transientClips,
                             node.ownedImageSlots, &recordedBounds, &paintStats);
    QPhasePaintDevice pdev(w, &engine);
    const QRegion fullRgn(wRect);

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

    // Level 2 Per-Widget Hybrid classification:
    // - Default (Correctness): immediate switch to CachedCpuRaster for unsupported CompositionMode /
    //   curved clipPath (exact native Qt raster fidelity while siblings stay native Vulkan!).
    // - Experimental (QT_PHASE_WIDGET_HYBRID=aggressive): also route heavy dynamic fallbacks or
    //   ultra-dense (>2000) glyph viewports to CachedCpuRaster.
    if (!asRoot && isWidgetHybridEnabled()) {
        const bool needsCorrectnessCpu = paintStats.usedUnsupportedCompMode || paintStats.usedComplexClipPath;
        const bool allowPerfRouting = (widgetHybridMode() == WidgetHybridMode::Aggressive);
        const bool isHeavyDynamicFallback = allowPerfRouting
                && (paintStats.fallbackCount >= 1 && paintStats.fallbackPixels >= 120000ull);
        const bool isUltraDenseGlyphs = allowPerfRouting
                && (recorded.size() >= 2000 && paintStats.glyphPrimCount >= 1500);

        if (isHeavyDynamicFallback || isUltraDenseGlyphs)
            ++node.heavyNativeStreak;
        else
            node.heavyNativeStreak = 0;

        if (needsCorrectnessCpu || node.heavyNativeStreak >= 2) {
            node.renderMode = QWidgetPhaseNode::WidgetRenderMode::CachedCpuRaster;
            node.forcedCpuForCorrectness = needsCorrectnessCpu;
            if (qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0) {
                std::fprintf(stderr, "[PhaseWidgetHybrid] %s (%dx%d) -> CachedCpuRaster (reason=%s, prims=%zu, glyphs=%u, fb=%u/%llu px)\n",
                             w->metaObject()->className(), w->width(), w->height(),
                             needsCorrectnessCpu ? "correctness" : (isHeavyDynamicFallback ? "dynamic-fallback" : "ultra-dense-glyphs"),
                             recorded.size(), paintStats.glyphPrimCount, paintStats.fallbackCount,
                             static_cast<unsigned long long>(paintStats.fallbackPixels));
            }
            // If correctness triggered, re-render cleanly via QRasterPaintEngine on this very frame!
            if (needsCorrectnessCpu) {
                paintSingleWidgetCachedCpu(w, node, arena, true);
                return;
            }
        }
    }

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
    node.primLocalBounds = std::move(recordedBounds);
}

void QPhaseWidgetBridge::paintSingleWidgetCachedCpu(QWidget *w,
                                                    QWidgetPhaseNode &node,
                                                    QPhaseSceneArena &arena,
                                                    bool forceFull)
{
    QWidgetPrivate *wd = QWidgetPrivate::get(w);
    if (!wd)
        return;

    const QRect wRect = w->rect();
    const qreal dpr = m_window ? std::max<qreal>(1.0, m_window->devicePixelRatio()) : 1.0;
    const int pxW = std::max(1, int(std::ceil(double(wRect.width()) * dpr)));
    const int pxH = std::max(1, int(std::ceil(double(wRect.height()) * dpr)));

    const bool needNewSurface = node.cachedRasterSurface.isNull()
            || node.cachedRasterSurface.width() != pxW
            || node.cachedRasterSurface.height() != pxH
            || !qFuzzyCompare(node.cachedRasterDpr, dpr);

    if (needNewSurface) {
        node.cachedRasterSurface = QImage(pxW, pxH, QImage::Format_ARGB32_Premultiplied);
        node.cachedRasterSurface.setDevicePixelRatio(dpr);
        node.cachedRasterSurface.fill(Qt::transparent);
        node.cachedRasterDpr = dpr;
        forceFull = true;
    }

    const QRegion paintRgn = (forceFull || node.dirtyRegion.isEmpty())
            ? QRegion(wRect)
            : (node.dirtyRegion & wRect);
    if (paintRgn.isEmpty())
        return;

    // Clear dirty sub-region before painting translucent widgets so stale pixels never bleed
    const bool opaqueBg = w->autoFillBackground() || w->testAttribute(Qt::WA_OpaquePaintEvent);
    if (forceFull && !needNewSurface) {
        node.cachedRasterSurface.fill(Qt::transparent);
    } else if (!forceFull && !opaqueBg) {
        QPainter clearP(&node.cachedRasterSurface);
        clearP.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &r : paintRgn)
            clearP.fillRect(r, Qt::transparent);
    }

    w->setAttribute(Qt::WA_WState_InPaintEvent, true);
    wd->setRedirected(&node.cachedRasterSurface, QPoint(0, 0));

    QWidget *redirectedParent = nullptr;
    if (QWidget *p = w->parentWidget(); p && p != m_tlw
        && w->objectName() == QLatin1StringView("qt_scrollarea_viewport")
        && !p->testAttribute(Qt::WA_WState_InPaintEvent)) {
        redirectedParent = p;
        p->setAttribute(Qt::WA_WState_InPaintEvent, true);
        QWidgetPrivate::get(p)->setRedirected(&node.cachedRasterSurface, w->pos());
    }

    if ((w->autoFillBackground() || w->testAttribute(Qt::WA_StyledBackground))
        && !w->testAttribute(Qt::WA_OpaquePaintEvent)
        && !w->testAttribute(Qt::WA_NoSystemBackground)) {
        QPainter bgPainter(w);
        if (bgPainter.isActive())
            wd->paintBackground(&bgPainter, paintRgn, QWidgetPrivate::DrawWidgetFlags());
    }

    wd->sendPaintEvent(paintRgn);

    if (redirectedParent) {
        QWidgetPrivate::get(redirectedParent)->restoreRedirected();
        redirectedParent->setAttribute(Qt::WA_WState_InPaintEvent, false);
    }
    wd->restoreRedirected();
    w->setAttribute(Qt::WA_WState_InPaintEvent, false);

    // Clean up any previous native transient slots and publish ONE zero-copy BGRA Image quad!
    node.partialPatchCount = 0;
    for (uint32_t s : node.transientTransforms)
        arena.freeTransform(s);
    node.transientTransforms.clear();
    for (uint32_t s : node.transientClips)
        arena.freeClip(s);
    node.transientClips.clear();

    std::vector<uint32_t> prevOwnedImageSlots = std::move(node.ownedImageSlots);
    const uint64_t prevOwnedImageEpoch = node.ownedImageEpoch;
    node.ownedImageSlots.clear();
    node.ownedImageEpoch = m_imageCache.epoch;

    const uint32_t imgSlot = m_imageCache.acquireImage(arena, node.cachedRasterSurface);
    if (imgSlot != UINT32_MAX)
        node.ownedImageSlots.push_back(imgSlot);

    if (!prevOwnedImageSlots.empty())
        m_imageCache.releaseImageSlots(arena, prevOwnedImageEpoch, prevOwnedImageSlots);

    if (imgSlot == UINT32_MAX)
        return;

    PhaseGpuPrimitive prim{};
    prim.kind = uint32_t(PhasePrimKind::Image);
    prim.transformIndex = node.transformSlot;
    prim.clipIndex = node.clipSlot;
    prim.resourceIndex = imgSlot;
    prim.x = 0.0f;
    prim.y = 0.0f;
    prim.w = float(double(pxW) / dpr);
    prim.h = float(double(pxH) / dpr);
    prim.c0r = 1.0f;
    prim.c0g = 1.0f;
    prim.c0b = 1.0f;
    prim.c0a = 1.0f;
    prim.p0 = 0.0f;
    prim.p1 = 0.0f;
    prim.p2 = 1.0f;
    prim.p3 = 1.0f;

    for (size_t i = 1; i < node.primitiveSlots.size(); ++i)
        arena.freePrimitive(node.primitiveSlots[i]);
    if (node.primitiveSlots.empty()) {
        node.primitiveSlots.push_back(arena.allocatePrimitive(prim));
    } else {
        node.primitiveSlots.resize(1);
        arena.updatePrimitive(node.primitiveSlots[0], prim);
    }
    node.primLocalBounds.assign(1, QRectF(0.0, 0.0, prim.w, prim.h));
}

QT_END_NAMESPACE
