// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphaseview.h"
#include "qphasevisuals_p.h"
#include "qphasescenearena_p.h"

#include <QtCore/qvariant.h>
#include <QtGui/qevent.h>
#include <QtGui/private/qgridlayoutengine_p.h>

#include <cfloat>

QT_BEGIN_NAMESPACE

namespace {

class PhaseLayoutStyleInfo final : public QAbstractLayoutStyleInfo
{
public:
    explicit PhaseLayoutStyleInfo(qreal defaultSpacing)
        : m_defaultSpacing(defaultSpacing)
    {
    }

    qreal spacing(Qt::Orientation) const override { return m_defaultSpacing; }
    qreal windowMargin(Qt::Orientation) const override { return 0.0; }

private:
    qreal m_defaultSpacing = 8.0;
};

class PhaseGridLayoutItem final : public QGridLayoutItem
{
public:
    PhaseGridLayoutItem(QPhaseView *view, int row, int col)
        : QGridLayoutItem(row, col)
        , m_view(view)
    {
    }

    QLayoutPolicy::Policy sizePolicy(Qt::Orientation) const override
    {
        return m_view->stretch() > 0 ? QLayoutPolicy::Expanding : QLayoutPolicy::Preferred;
    }

    QSizeF sizeHint(Qt::SizeHint which, const QSizeF &constraint) const override
    {
        return m_view->sizeHint(which, constraint);
    }

    bool isEmpty() const override
    {
        return !m_view->isVisible();
    }

    void setGeometry(const QRectF &rect) override
    {
        m_view->setGeometry(rect);
    }

private:
    QPhaseView *m_view = nullptr;
};

} // namespace

class QPhaseViewPrivate
{
public:
    QPhaseView *parent = nullptr;
    QPhaseSceneArena *arena = nullptr;
    QPhaseWindow *window = nullptr;
    std::vector<std::unique_ptr<QPhaseView>> children;

    QRectF geometry{0.0, 0.0, 0.0, 0.0};
    QSizeF minSize{0.0, 0.0};
    QSizeF prefSize{-1.0, -1.0};
    QSizeF maxSize{FLT_MAX, FLT_MAX};
    int stretch = 0;

    bool hasBoxLayout = false;
    Qt::Orientation boxOrientation = Qt::Vertical;
    QMarginsF boxPadding{};
    qreal boxSpacing = 8.0;

    QPointF scrollOffset{0.0, 0.0};
    float rotationDeg = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float opacity = 1.0f;
    bool isolatedGroup = false;
    QPhaseBlendMode blendMode = QPhaseBlendMode::Normal;
    QPhaseRendererAffinity rendererAffinity = QPhaseRendererAffinity::Any;
    bool backendBlocking = false;
    bool visible = true;
    bool clipToBounds = false;
    bool focusable = false;
    bool focused = false;
    QPhaseCorners clipCorners{};

    uint32_t selfTransformSlot = 0;
    uint32_t childContentTransformSlot = 0;
    uint32_t selfClipSlot = 0;
    uint32_t pushLayerSlot = UINT32_MAX;
    uint32_t popLayerSlot = UINT32_MAX;
    uint32_t gpuSurfaceSlot = UINT32_MAX;
    uint32_t cpuBridgeImageSlot = UINT32_MAX;
    uint32_t cpuBridgePrimSlot = UINT32_MAX;
    std::vector<uint32_t> primitiveSlots;

    bool visualsDirty = true;
    bool layoutDirty = true;
    bool transformDirty = true;
    bool clipDirty = true;
    bool wasUsingLayerGroup = false;

    bool needsIsolatedLayerPass() const noexcept
    {
        return isolatedGroup
                || blendMode != QPhaseBlendMode::Normal
                || (!children.empty() && opacity < 0.9999f);
    }

    void applySelfTransformToArena() const
    {
        if (!arena || selfTransformSlot == 0)
            return;
        const float effSelfOpacity = needsIsolatedLayerPass() ? 1.0f : opacity;
        const float rad = rotationDeg * (3.14159265358979323846f / 180.0f);
        const float cosA = (std::abs(rotationDeg) < 1e-5f) ? 1.0f : std::cos(rad);
        const float sinA = (std::abs(rotationDeg) < 1e-5f) ? 0.0f : std::sin(rad);
        const float m00 = cosA * scaleX;
        const float m01 = -sinA * scaleY;
        const float m10 = sinA * scaleX;
        const float m11 = cosA * scaleY;
        const float cx = float(geometry.width() * 0.5);
        const float cy = float(geometry.height() * 0.5);
        const float tx = float(geometry.x()) + cx - (m00 * cx + m01 * cy);
        const float ty = float(geometry.y()) + cy - (m10 * cx + m11 * cy);
        arena->updateTransform2D(selfTransformSlot, m00, m01, m10, m11, tx, ty, effSelfOpacity);
    }

    void notifyChanged();
};

extern void qt_phase_notify_window_dirty(QPhaseWindow *window);
extern void qt_phase_set_focused_view(QPhaseWindow *window, QPhaseView *view);

void QPhaseViewPrivate::notifyChanged()
{
    if (window)
        qt_phase_notify_window_dirty(window);
}

QPhaseView::QPhaseView(QPhaseView *parent)
    : d(std::make_unique<QPhaseViewPrivate>())
{
    if (parent) {
        d->parent = parent;
        d->window = parent->d->window;
    }
}

QPhaseView::~QPhaseView()
{
    if (d->focused && d->window)
        qt_phase_set_focused_view(d->window, nullptr);
    detachFromArena();
}

QPhaseView *QPhaseView::parentView() const noexcept
{
    return d->parent;
}

QPhaseWindow *QPhaseView::window() const noexcept
{
    return d->window;
}

const std::vector<std::unique_ptr<QPhaseView>> &QPhaseView::children() const noexcept
{
    return d->children;
}

void QPhaseView::addChild(std::unique_ptr<QPhaseView> child)
{
    if (!child)
        return;
    child->d->parent = this;
    child->setOwningWindow(d->window);
    d->children.push_back(std::move(child));
    markLayoutDirty();
}

void QPhaseView::setOwningWindow(QPhaseWindow *window)
{
    d->window = window;
    for (const auto &ch : d->children)
        ch->setOwningWindow(window);
}

QPointF QPhaseView::mapFromWindow(QPointF windowPos) const noexcept
{
    QPointF p = windowPos;
    std::vector<const QPhaseView *> chain;
    for (const QPhaseView *cur = this; cur != nullptr; cur = cur->d->parent)
        chain.push_back(cur);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const QPhaseView *v = *it;
        p -= v->d->geometry.topLeft();
        if (v != this)
            p -= v->d->scrollOffset;
    }
    return p;
}

void QPhaseView::setGeometry(const QRectF &geometry)
{
    const bool sizeChanged = d->geometry.size() != geometry.size();
    const bool posChanged = d->geometry.topLeft() != geometry.topLeft();
    if (!sizeChanged && !posChanged)
        return;

    d->geometry = geometry;
    if (posChanged || std::abs(d->rotationDeg) > 1e-5f || d->scaleX != 1.0f || d->scaleY != 1.0f) {
        d->transformDirty = true;
        d->applySelfTransformToArena();
    }
    if (sizeChanged) {
        d->visualsDirty = true;
        d->layoutDirty = true;
        d->clipDirty = true;
    }
    d->notifyChanged();
}

QRectF QPhaseView::geometry() const noexcept
{
    return d->geometry;
}

void QPhaseView::setPosition(QPointF pos)
{
    setGeometry(QRectF(pos, d->geometry.size()));
}

void QPhaseView::setSize(QSizeF size)
{
    setGeometry(QRectF(d->geometry.topLeft(), size));
}

void QPhaseView::setMinimumSize(QSizeF minSize)
{
    d->minSize = minSize;
    if (d->parent)
        d->parent->markLayoutDirty();
}

void QPhaseView::setPreferredSize(QSizeF prefSize)
{
    d->prefSize = prefSize;
    if (d->parent)
        d->parent->markLayoutDirty();
}

void QPhaseView::setMaximumSize(QSizeF maxSize)
{
    d->maxSize = maxSize;
    if (d->parent)
        d->parent->markLayoutDirty();
}

void QPhaseView::setStretch(int stretchFactor)
{
    d->stretch = stretchFactor;
    if (d->parent)
        d->parent->markLayoutDirty();
}

int QPhaseView::stretch() const noexcept
{
    return d->stretch;
}

void QPhaseView::setBoxLayout(Qt::Orientation orientation, QMarginsF padding, qreal spacing)
{
    d->hasBoxLayout = true;
    d->boxOrientation = orientation;
    d->boxPadding = padding;
    d->boxSpacing = spacing;
    markLayoutDirty();
}

QSizeF QPhaseView::sizeHint(Qt::SizeHint which, const QSizeF &constraint) const
{
    Q_UNUSED(constraint);
    if (which == Qt::MinimumSize)
        return d->minSize;
    if (which == Qt::MaximumSize)
        return d->maxSize;
    if (d->prefSize.width() >= 0.0 || d->prefSize.height() >= 0.0) {
        return QSizeF(d->prefSize.width() >= 0.0 ? d->prefSize.width() : d->minSize.width(),
                      d->prefSize.height() >= 0.0 ? d->prefSize.height() : d->minSize.height());
    }
    return d->minSize;
}

void QPhaseView::setScrollOffset(QPointF offset)
{
    if (d->scrollOffset == offset)
        return;
    d->scrollOffset = offset;
    d->transformDirty = true;
    if (d->arena && d->childContentTransformSlot != 0) {
        d->arena->updateTransform(d->childContentTransformSlot,
                                  float(offset.x()), float(offset.y()),
                                  1.0f, 1.0f, 1.0f);
    }
    d->notifyChanged();
}

QPointF QPhaseView::scrollOffset() const noexcept
{
    return d->scrollOffset;
}

void QPhaseView::setRotationDegrees(float degrees)
{
    if (qFuzzyCompare(d->rotationDeg, degrees))
        return;
    d->rotationDeg = degrees;
    d->transformDirty = true;
    d->applySelfTransformToArena();
    d->notifyChanged();
}

float QPhaseView::rotationDegrees() const noexcept
{
    return d->rotationDeg;
}

void QPhaseView::setScale(float sx, float sy)
{
    if (qFuzzyCompare(d->scaleX, sx) && qFuzzyCompare(d->scaleY, sy))
        return;
    d->scaleX = sx;
    d->scaleY = sy;
    d->transformDirty = true;
    d->applySelfTransformToArena();
    d->notifyChanged();
}

void QPhaseView::setOpacity(float opacity)
{
    if (qFuzzyCompare(d->opacity, opacity))
        return;
    d->opacity = opacity;
    d->transformDirty = true;
    d->applySelfTransformToArena();
    d->notifyChanged();
}

float QPhaseView::opacity() const noexcept
{
    return d->opacity;
}

void QPhaseView::setIsolatedGroup(bool isolated, QPhaseBlendMode blendMode)
{
    if (d->isolatedGroup == isolated && d->blendMode == blendMode)
        return;
    d->isolatedGroup = isolated;
    d->blendMode = blendMode;
    d->transformDirty = true;
    d->visualsDirty = true;
    d->applySelfTransformToArena();
    d->notifyChanged();
}

bool QPhaseView::isIsolatedGroup() const noexcept
{
    return d->isolatedGroup;
}

void QPhaseView::setRendererAffinity(QPhaseRendererAffinity affinity, bool blocking)
{
    if (d->rendererAffinity == affinity && d->backendBlocking == blocking)
        return;
    d->rendererAffinity = affinity;
    d->backendBlocking = blocking;
    d->visualsDirty = true;
    d->notifyChanged();
}

QPhaseRendererAffinity QPhaseView::rendererAffinity() const noexcept
{
    return d->rendererAffinity;
}

void QPhaseView::setBackendBlocking(bool blocking)
{
    if (d->backendBlocking == blocking)
        return;
    d->backendBlocking = blocking;
    d->notifyChanged();
}

bool QPhaseView::isBackendBlocking() const noexcept
{
    return d->backendBlocking;
}

QPhaseBlendMode QPhaseView::blendMode() const noexcept
{
    return d->blendMode;
}

void QPhaseView::setVisible(bool visible)
{
    if (d->visible == visible)
        return;
    d->visible = visible;
    if (d->parent)
        d->parent->markLayoutDirty();
    d->notifyChanged();
}

bool QPhaseView::isVisible() const noexcept
{
    return d->visible;
}

void QPhaseView::setClipsToBounds(bool clip, QPhaseCorners corners)
{
    d->clipToBounds = clip;
    d->clipCorners = corners;
    d->clipDirty = true;
    d->notifyChanged();
}

bool QPhaseView::clipsToBounds() const noexcept
{
    return d->clipToBounds;
}

void QPhaseView::setFocusable(bool focusable)
{
    d->focusable = focusable;
}

bool QPhaseView::isFocusable() const noexcept
{
    return d->focusable;
}

bool QPhaseView::hasFocus() const noexcept
{
    return d->focused;
}

void QPhaseView::requestFocus()
{
    if (d->focusable && d->window)
        qt_phase_set_focused_view(d->window, this);
}

void QPhaseView::setFocusedInternal(bool focused)
{
    if (d->focused == focused)
        return;
    d->focused = focused;
    onFocusChanged(focused);
    markVisualsDirty();
}

void QPhaseView::markVisualsDirty()
{
    d->visualsDirty = true;
    d->notifyChanged();
}

void QPhaseView::markLayoutDirty()
{
    d->layoutDirty = true;
    d->notifyChanged();
}

void QPhaseView::describeVisuals(QPhaseVisuals &)
{
}

void QPhaseView::layoutChildren()
{
    if (!d->hasBoxLayout || d->children.empty())
        return;

    // Use QtGui's non-QWidget QGridLayoutEngine to solve min/preferred/max + stretch layout!
    QGridLayoutEngine engine(Qt::Alignment{}, true);
    engine.setSpacing(d->boxSpacing, Qt::Horizontal | Qt::Vertical);

    int slot = 0;
    for (const auto &ch : d->children) {
        if (!ch->isVisible())
            continue;
        const int r = (d->boxOrientation == Qt::Vertical) ? slot : 0;
        const int c = (d->boxOrientation == Qt::Horizontal) ? slot : 0;
        auto *item = new PhaseGridLayoutItem(ch.get(), r, c);
        if (ch->stretch() > 0)
            item->setStretchFactor(ch->stretch(), d->boxOrientation);
        engine.addItem(item);
        if (ch->stretch() > 0)
            engine.setRowStretchFactor(slot, ch->stretch(), d->boxOrientation);
        ++slot;
    }

    const QRectF contentRect = localRect().marginsRemoved(d->boxPadding);
    if (contentRect.width() > 0.0 && contentRect.height() > 0.0) {
        PhaseLayoutStyleInfo styleInfo(d->boxSpacing);
        engine.setGeometries(contentRect, &styleInfo);
    }
}

QVariant QPhaseView::inputMethodQuery(Qt::InputMethodQuery) const
{
    return {};
}

QPhaseView *QPhaseView::hitTestMousePress(QPointF parentLocalPos, QMouseEvent *origEvent)
{
    if (!d->visible)
        return nullptr;

    const QPointF localPos = parentLocalPos - d->geometry.topLeft();
    if (d->clipToBounds && !localRect().contains(localPos))
        return nullptr;

    // Walk children front-to-back (reverse draw order)
    const QPointF childSpacePos = localPos - d->scrollOffset;
    for (auto it = d->children.rbegin(); it != d->children.rend(); ++it) {
        if (QPhaseView *hit = (*it)->hitTestMousePress(childSpacePos, origEvent))
            return hit;
    }

    if (!localRect().contains(localPos))
        return nullptr;

    QMouseEvent localEv(origEvent->type(), localPos, origEvent->scenePosition(), origEvent->globalPosition(),
                        origEvent->button(), origEvent->buttons(), origEvent->modifiers(), origEvent->pointingDevice());
    if (d->focusable)
        requestFocus();
    if (onMousePress(&localEv))
        return this;
    return nullptr;
}

bool QPhaseView::dispatchWheel(QPointF parentLocalPos, QWheelEvent *origEvent)
{
    if (!d->visible)
        return false;

    const QPointF localPos = parentLocalPos - d->geometry.topLeft();
    if (d->clipToBounds && !localRect().contains(localPos))
        return false;

    const QPointF childSpacePos = localPos - d->scrollOffset;
    for (auto it = d->children.rbegin(); it != d->children.rend(); ++it) {
        if ((*it)->dispatchWheel(childSpacePos, origEvent))
            return true;
    }

    if (!localRect().contains(localPos))
        return false;

    QWheelEvent localEv(localPos, origEvent->globalPosition(), origEvent->pixelDelta(), origEvent->angleDelta(),
                        origEvent->buttons(), origEvent->modifiers(), origEvent->phase(), origEvent->inverted(),
                        origEvent->source(), origEvent->pointingDevice());
    return onWheel(&localEv);
}

void QPhaseView::detachFromArena()
{
    for (auto &ch : d->children)
        ch->detachFromArena();

    if (!d->arena)
        return;

    for (uint32_t slot : d->primitiveSlots)
        d->arena->freePrimitive(slot);
    d->primitiveSlots.clear();

    if (d->gpuSurfaceSlot != UINT32_MAX) {
        d->arena->unregisterImage(d->gpuSurfaceSlot);
        d->gpuSurfaceSlot = UINT32_MAX;
    }
    if (d->cpuBridgePrimSlot != UINT32_MAX) {
        d->arena->freePrimitive(d->cpuBridgePrimSlot);
        d->cpuBridgePrimSlot = UINT32_MAX;
    }
    if (d->cpuBridgeImageSlot != UINT32_MAX) {
        d->arena->unregisterImage(d->cpuBridgeImageSlot);
        d->cpuBridgeImageSlot = UINT32_MAX;
    }
    if (d->pushLayerSlot != UINT32_MAX) {
        d->arena->freePrimitive(d->pushLayerSlot);
        d->pushLayerSlot = UINT32_MAX;
    }
    if (d->popLayerSlot != UINT32_MAX) {
        d->arena->freePrimitive(d->popLayerSlot);
        d->popLayerSlot = UINT32_MAX;
    }

    if (d->selfClipSlot != 0) {
        d->arena->freeClip(d->selfClipSlot);
        d->selfClipSlot = 0;
    }
    if (d->childContentTransformSlot != 0) {
        d->arena->freeTransform(d->childContentTransformSlot);
        d->childContentTransformSlot = 0;
    }
    if (d->selfTransformSlot != 0) {
        d->arena->freeTransform(d->selfTransformSlot);
        d->selfTransformSlot = 0;
    }
    d->arena = nullptr;
}

void QPhaseView::syncToArena(QPhaseSceneArena &arena, uint32_t parentTx, uint32_t parentClip,
                             std::vector<uint32_t> &outDrawOrder, bool &drawOrderChanged,
                             QPhaseRendererAffinity inheritedAffinity, bool inheritedBlocking)
{
    static thread_local int s_isolatedGroupDepth = 0;

    if (!d->visible)
        return;

    const QPhaseRendererAffinity effAffinity = (inheritedAffinity != QPhaseRendererAffinity::Any)
            ? inheritedAffinity
            : d->rendererAffinity;
    const bool effBlocking = inheritedBlocking || d->backendBlocking;
    const uint32_t affinityBits = (effAffinity == QPhaseRendererAffinity::GpuOnly)
            ? kPhaseAnimFlagGpuOnly
            : ((effAffinity == QPhaseRendererAffinity::CpuOnly) ? kPhaseAnimFlagCpuOnly : 0u);

    if (d->rendererAffinity != QPhaseRendererAffinity::Any) {
        PhaseRendererRequirements reqs = arena.rendererRequirements();
        if (effAffinity == QPhaseRendererAffinity::GpuOnly) {
            ++reqs.gpuOnlyViewCount;
            if (effBlocking)
                ++reqs.gpuBlockingViewCount;
        } else if (effAffinity == QPhaseRendererAffinity::CpuOnly) {
            ++reqs.cpuOnlyViewCount;
            if (effBlocking)
                ++reqs.cpuBlockingViewCount;
        }
        arena.setRendererRequirements(reqs);
    }

    const bool useLayer = d->needsIsolatedLayerPass();
    if (useLayer != d->wasUsingLayerGroup) {
        d->wasUsingLayerGroup = useLayer;
        d->transformDirty = true;
        d->visualsDirty = true;
        drawOrderChanged = true;
    }

    if (d->arena != &arena) {
        d->arena = &arena;
        d->selfTransformSlot = arena.allocateTransform(parentTx);
        d->childContentTransformSlot = arena.allocateTransform(d->selfTransformSlot);
        d->transformDirty = true;
        d->clipDirty = true;
        d->visualsDirty = true;
        drawOrderChanged = true;
    } else {
        arena.setTransformParent(d->selfTransformSlot, parentTx);
    }

    if (d->layoutDirty) {
        layoutChildren();
        d->layoutDirty = false;
    }

    if (d->transformDirty) {
        d->applySelfTransformToArena();
        arena.updateTransform(d->childContentTransformSlot,
                              float(d->scrollOffset.x()), float(d->scrollOffset.y()),
                              1.0f, 1.0f, 1.0f);
        d->transformDirty = false;
    }

    uint32_t activeClip = parentClip;
    if (d->clipToBounds) {
        if (d->selfClipSlot == 0) {
            d->selfClipSlot = arena.allocateClip(parentClip, d->selfTransformSlot, localRect(), d->clipCorners);
            d->clipDirty = false;
            d->visualsDirty = true;
        } else if (d->clipDirty) {
            arena.updateClip(d->selfClipSlot, localRect(), d->clipCorners);
            d->clipDirty = false;
        }
        activeClip = d->selfClipSlot;
    } else if (d->selfClipSlot != 0) {
        arena.freeClip(d->selfClipSlot);
        d->selfClipSlot = 0;
        d->visualsDirty = true;
    }

    const bool isCpuBridgeRoot = (d->rendererAffinity == QPhaseRendererAffinity::CpuOnly)
            && (inheritedAffinity == QPhaseRendererAffinity::Any)
            && width() > 0.0 && height() > 0.0;
    const size_t bridgeOrderStart = outDrawOrder.size();

    if (useLayer) {
        ++s_isolatedGroupDepth;
        QRectF grpRect = localRect();
        if (!d->clipToBounds)
            grpRect = grpRect.adjusted(-48.0, -48.0, 48.0, 48.0);

        PhaseGpuPrimitive pushP{};
        pushP.kind = uint32_t(PhasePrimKind::PushLayer);
        pushP.transformIndex = d->selfTransformSlot;
        pushP.clipIndex = activeClip;
        pushP.resourceIndex = uint32_t(d->blendMode);
        pushP.x = float(grpRect.x());
        pushP.y = float(grpRect.y());
        pushP.w = float(grpRect.width());
        pushP.h = float(grpRect.height());
        pushP.c0a = std::clamp(d->opacity, 0.0f, 1.0f);
        pushP.animMode = kPhaseAnimFlagInsideLayer | affinityBits;

        PhaseGpuPrimitive popP = pushP;
        popP.kind = uint32_t(PhasePrimKind::PopLayer);

        if (d->pushLayerSlot == UINT32_MAX) {
            d->pushLayerSlot = arena.allocatePrimitive(pushP);
            drawOrderChanged = true;
        } else {
            arena.updatePrimitive(d->pushLayerSlot, pushP);
        }
        if (d->popLayerSlot == UINT32_MAX) {
            d->popLayerSlot = arena.allocatePrimitive(popP);
            drawOrderChanged = true;
        } else {
            arena.updatePrimitive(d->popLayerSlot, popP);
        }

        outDrawOrder.push_back(d->pushLayerSlot);
    } else {
        if (d->pushLayerSlot != UINT32_MAX) {
            arena.freePrimitive(d->pushLayerSlot);
            d->pushLayerSlot = UINT32_MAX;
            drawOrderChanged = true;
        }
        if (d->popLayerSlot != UINT32_MAX) {
            arena.freePrimitive(d->popLayerSlot);
            d->popLayerSlot = UINT32_MAX;
            drawOrderChanged = true;
        }
    }

    if (d->visualsDirty) {
        QPhaseVisualsPrivate vp{};
        vp.arena = &arena;
        vp.transformIndex = d->selfTransformSlot;
        vp.clipIndex = activeClip;
        vp.activeLayerDepth = s_isolatedGroupDepth;
        QPhaseVisuals builder(&vp);
        describeVisuals(builder);

        const size_t newCount = vp.recordedPrimitives.size();
        const size_t oldCount = d->primitiveSlots.size();

        if (newCount != oldCount)
            drawOrderChanged = true;

        for (size_t i = newCount; i < oldCount; ++i)
            arena.freePrimitive(d->primitiveSlots[i]);
        if (newCount < oldCount)
            d->primitiveSlots.resize(newCount);

        for (size_t i = 0; i < newCount; ++i) {
            vp.recordedPrimitives[i].animMode =
                    (vp.recordedPrimitives[i].animMode & ~kPhaseAnimAffinityMask) | affinityBits;
            if (i < oldCount)
                arena.updatePrimitive(d->primitiveSlots[i], vp.recordedPrimitives[i]);
            else
                d->primitiveSlots.push_back(arena.allocatePrimitive(vp.recordedPrimitives[i]));
        }

        d->visualsDirty = false;
    } else {
        const uint32_t requiredFlags = (s_isolatedGroupDepth > 0 ? kPhaseAnimFlagInsideLayer : 0u) | affinityBits;
        for (uint32_t slot : d->primitiveSlots) {
            const PhaseGpuPrimitive &existing = arena.primitives()[slot];
            const uint32_t desiredMode =
                    (existing.animMode & ~(kPhaseAnimFlagInsideLayer | kPhaseAnimAffinityMask)) | requiredFlags;
            if (existing.animMode != desiredMode) {
                PhaseGpuPrimitive copy = existing;
                copy.animMode = desiredMode;
                arena.updatePrimitive(slot, copy);
            }
        }
    }

    for (uint32_t slot : d->primitiveSlots)
        outDrawOrder.push_back(slot);

    for (auto &ch : d->children) {
        ch->syncToArena(arena, d->childContentTransformSlot, activeClip,
                        outDrawOrder, drawOrderChanged, effAffinity, effBlocking);
    }

    if (useLayer) {
        outDrawOrder.push_back(d->popLayerSlot);
        --s_isolatedGroupDepth;
    }

    if (isCpuBridgeRoot) {
        std::vector<uint32_t> subOrder(outDrawOrder.begin() + ptrdiff_t(bridgeOrderStart), outDrawOrder.end());
        if (!subOrder.empty()) {
            if (d->cpuBridgeImageSlot == UINT32_MAX)
                d->cpuBridgeImageSlot = arena.allocateCpuBridgeSlot();

            PhaseGpuPrimitive bp{};
            bp.kind = uint32_t(PhasePrimKind::Image);
            bp.transformIndex = d->selfTransformSlot;
            bp.clipIndex = activeClip;
            bp.resourceIndex = d->cpuBridgeImageSlot;
            bp.x = 0.0f;
            bp.y = 0.0f;
            bp.w = float(width());
            bp.h = float(height());
            bp.c0r = 1.0f;
            bp.c0g = 1.0f;
            bp.c0b = 1.0f;
            bp.c0a = 1.0f;
            bp.p0 = 0.0f;
            bp.p1 = 0.0f;
            bp.p2 = 1.0f;
            bp.p3 = 1.0f;
            bp.animMode = kPhaseAnimFlagGpuOnly
                    | (s_isolatedGroupDepth > 0 ? kPhaseAnimFlagInsideLayer : 0u);

            if (d->cpuBridgePrimSlot == UINT32_MAX) {
                d->cpuBridgePrimSlot = arena.allocatePrimitive(bp);
                drawOrderChanged = true;
            } else {
                const PhaseGpuPrimitive &curBp = arena.primitives()[d->cpuBridgePrimSlot];
                if (curBp.transformIndex != bp.transformIndex
                    || curBp.clipIndex != bp.clipIndex
                    || curBp.resourceIndex != bp.resourceIndex
                    || curBp.w != bp.w || curBp.h != bp.h
                    || curBp.animMode != bp.animMode) {
                    arena.updatePrimitive(d->cpuBridgePrimSlot, bp);
                }
            }
            outDrawOrder.push_back(d->cpuBridgePrimSlot);

            bool hasSubAnim = false;
            for (uint32_t pid : subOrder) {
                if (pid < arena.primitives().size()
                    && (arena.primitives()[pid].animMode & 0x00FFFFFFu) != 0u) {
                    hasSubAnim = true;
                    break;
                }
            }

            arena.registerCpuBridge({
                d->cpuBridgeImageSlot,
                d->selfTransformSlot,
                parentClip,
                float(width()),
                float(height()),
                std::move(subOrder),
                hasSubAnim
            });
        }
    } else {
        if (d->cpuBridgePrimSlot != UINT32_MAX) {
            arena.freePrimitive(d->cpuBridgePrimSlot);
            d->cpuBridgePrimSlot = UINT32_MAX;
            drawOrderChanged = true;
        }
        if (d->cpuBridgeImageSlot != UINT32_MAX) {
            arena.unregisterImage(d->cpuBridgeImageSlot);
            d->cpuBridgeImageSlot = UINT32_MAX;
        }
    }
}

// ============================================================================
// Built-in Convenience Primitive Views
// ============================================================================

QPhaseBox::QPhaseBox(QPhaseView *parent)
    : QPhaseView(parent)
{
}

void QPhaseBox::setColor(const QColor &color)
{
    m_style.fill = QPhaseFill::solid(color);
    markVisualsDirty();
}

void QPhaseBox::setBoxStyle(const QPhaseBoxStyle &style)
{
    m_style = style;
    markVisualsDirty();
}

void QPhaseBox::setAnimation(const QPhaseAnimation &anim)
{
    m_anim = anim;
    markVisualsDirty();
}

void QPhaseBox::describeVisuals(QPhaseVisuals &visuals)
{
    visuals.roundedBox(localRect(), m_style, m_anim);
}

QPhaseImage::QPhaseImage(QPhaseView *parent)
    : QPhaseView(parent)
{
}

QPhaseImage::QPhaseImage(const QImage &image, QPhaseView *parent)
    : QPhaseView(parent)
    , m_image(image)
{
}

void QPhaseImage::setImage(const QImage &image)
{
    m_image = image;
    markVisualsDirty();
}

void QPhaseImage::setCornerRadius(QPhaseCorners corners)
{
    m_corners = corners;
    markVisualsDirty();
}

void QPhaseImage::describeVisuals(QPhaseVisuals &visuals)
{
    visuals.image(m_image, localRect(), m_corners);
}

QPhaseIcon::QPhaseIcon(QPhaseView *parent)
    : QPhaseView(parent)
{
}

QPhaseIcon::QPhaseIcon(const QString &svgPathData, QRectF viewBox, const QColor &color, QPhaseView *parent)
    : QPhaseView(parent)
    , m_svgPath(svgPathData)
    , m_viewBox(viewBox)
    , m_color(color)
{
}

void QPhaseIcon::setSvgPath(const QString &svgPathData, QRectF viewBox)
{
    m_svgPath = svgPathData;
    m_viewBox = viewBox;
    markVisualsDirty();
}

void QPhaseIcon::setColor(const QColor &color)
{
    if (m_color != color) {
        m_color = color;
        markVisualsDirty();
    }
}

void QPhaseIcon::describeVisuals(QPhaseVisuals &visuals)
{
    visuals.svgPath(m_svgPath, m_viewBox, localRect(), m_color);
}

QPhaseGpuView::QPhaseGpuView(QPhaseView *parent)
    : QPhaseView(parent)
{
    setRendererAffinity(QPhaseRendererAffinity::GpuOnly, false);
    m_callbackDispatcher = std::make_shared<RenderCallback>([this](const QPhaseGpuSurfaceContext &ctx) {
        renderGpu(ctx);
    });
}

QPhaseGpuView::~QPhaseGpuView()
{
    if (m_callbackDispatcher)
        *m_callbackDispatcher = nullptr;
}

void QPhaseGpuView::setDepthBufferEnabled(bool enabled)
{
    if (m_depthEnabled == enabled)
        return;
    m_depthEnabled = enabled;
    m_gpuContentDirty = true;
    markVisualsDirty();
}

void QPhaseGpuView::setCornerRadius(QPhaseCorners corners)
{
    m_corners = corners;
    markVisualsDirty();
}

void QPhaseGpuView::setRenderCallback(RenderCallback callback)
{
    m_userCallback = std::move(callback);
    m_gpuContentDirty = true;
    markVisualsDirty();
}

void QPhaseGpuView::markGpuDirty()
{
    m_gpuContentDirty = true;
    if (d->arena && d->gpuSurfaceSlot != UINT32_MAX) {
        d->arena->markGpuSurfaceDirty(d->gpuSurfaceSlot);
        m_gpuContentDirty = false;
    }
    d->notifyChanged();
}

void QPhaseGpuView::renderGpu(const QPhaseGpuSurfaceContext &context)
{
    if (m_userCallback)
        m_userCallback(context);
}

void QPhaseGpuView::describeVisuals(QPhaseVisuals &visuals)
{
    if (!d->arena || width() <= 0.0 || height() <= 0.0)
        return;

    const int lw = std::max(1, int(std::ceil(width())));
    const int lh = std::max(1, int(std::ceil(height())));
    if (d->gpuSurfaceSlot == UINT32_MAX) {
        d->gpuSurfaceSlot = d->arena->allocateGpuSurface(lw, lh, m_depthEnabled, m_callbackDispatcher);
    } else {
        d->arena->updateGpuSurface(d->gpuSurfaceSlot, lw, lh, m_depthEnabled,
                                   m_callbackDispatcher, m_gpuContentDirty);
    }
    m_gpuContentDirty = false;
    visuals.imageSlot(d->gpuSurfaceSlot, localRect(), m_corners);
}

QT_END_NAMESPACE
