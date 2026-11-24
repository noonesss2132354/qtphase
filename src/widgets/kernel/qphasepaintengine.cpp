// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasepaintengine_p.h"

#include <QtWidgets/qwidget.h>
#include <QtPhase/qphasevisuals.h>
#include <QtPhase/private/qphasescenearena_p.h>
#include <QtPhase/private/qphasevisuals_p.h>

#include <QtGui/qglyphrun.h>
#include <QtGui/qbrush.h>
#include <QtGui/qpainter.h>
#include <QtGui/qpainterpath.h>
#include <QtGui/qpixmap.h>
#include <QtGui/qrawfont.h>
#include <QtGui/private/qpaintengine_p.h>
#include <QtGui/private/qrawfont_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qtextengine_p.h>

#include <algorithm>
#include <cmath>
#include <cstring>

QT_BEGIN_NAMESPACE

namespace {

inline uint64_t hashMix64(uint64_t h, const void *data, size_t len) noexcept
{
    const auto *p = static_cast<const uint8_t *>(data);
    while (len >= 8) {
        uint64_t w = 0;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 32);
        p += 8;
        len -= 8;
    }
    while (len > 0) {
        h = (h ^ uint64_t(*p++)) * 1099511628211ULL;
        --len;
    }
    return h;
}

template <typename T>
inline uint64_t hashVal(uint64_t h, const T &v) noexcept
{
    return hashMix64(h, &v, sizeof(T));
}

inline uint64_t hashPenBrush(uint64_t h, const QPen &pen, const QBrush &brush) noexcept
{
    const int ps = int(pen.style());
    const qreal pw = pen.widthF();
    const QRgb pc = pen.color().rgba();
    const int cap = int(pen.capStyle());
    const int join = int(pen.joinStyle());
    const int bs = int(brush.style());
    const QRgb bc = brush.color().rgba();
    h = hashVal(h, ps);
    h = hashVal(h, pw);
    h = hashVal(h, pc);
    h = hashVal(h, cap);
    h = hashVal(h, join);
    if (ps == int(Qt::CustomDashLine)) {
        const auto dp = pen.dashPattern();
        for (qreal d : dp)
            h = hashVal(h, d);
    }
    h = hashVal(h, bs);
    h = hashVal(h, bc);
    if (const QGradient *g = brush.gradient()) {
        const int gt = int(g->type());
        const int cm = int(g->coordinateMode());
        const int sp = int(g->spread());
        h = hashVal(h, gt);
        h = hashVal(h, cm);
        h = hashVal(h, sp);
        if (g->type() == QGradient::LinearGradient) {
            const auto *lg = static_cast<const QLinearGradient *>(g);
            const QPointF p0 = lg->start();
            const QPointF p1 = lg->finalStop();
            h = hashVal(h, p0);
            h = hashVal(h, p1);
        } else if (g->type() == QGradient::RadialGradient) {
            const auto *rg = static_cast<const QRadialGradient *>(g);
            const QPointF c = rg->center();
            const QPointF fp = rg->focalPoint();
            const qreal r = rg->radius();
            const qreal fr = rg->focalRadius();
            h = hashVal(h, c);
            h = hashVal(h, fp);
            h = hashVal(h, r);
            h = hashVal(h, fr);
        } else if (g->type() == QGradient::ConicalGradient) {
            const auto *cg = static_cast<const QConicalGradient *>(g);
            const QPointF c = cg->center();
            const qreal ang = cg->angle();
            h = hashVal(h, c);
            h = hashVal(h, ang);
        }
        const auto stops = g->stops();
        for (const auto &s : stops) {
            h = hashVal(h, s.first);
            const QRgb sc = s.second.rgba();
            h = hashVal(h, sc);
        }
    }
    if (!brush.transform().isIdentity()) {
        const QTransform bt = brush.transform();
        const qreal vals[6] = {bt.m11(), bt.m12(), bt.m21(), bt.m22(), bt.dx(), bt.dy()};
        h = hashMix64(h, vals, sizeof(vals));
    }
    return h;
}

inline bool tryBuildPhaseFill(const QBrush &brush, const QRectF &bounds, QPhaseFill &outFill)
{
    const Qt::BrushStyle st = brush.style();
    if (st == Qt::NoBrush) {
        outFill = QPhaseFill::solid(QColor(0, 0, 0, 0));
        return true;
    }
    if (st == Qt::SolidPattern) {
        outFill = QPhaseFill::solid(brush.color());
        return true;
    }
    if (st == Qt::LinearGradientPattern) {
        const auto *lg = static_cast<const QLinearGradient *>(brush.gradient());
        if (!lg || !brush.transform().isIdentity())
            return false;
        const QGradientStops stops = lg->stops();
        if (stops.size() < 2)
            return false;
        // Exact 2-stop analytical linear gradient in BOX-LOCAL coordinates (0..w, 0..h)
        if (stops.size() == 2 && lg->spread() == QGradient::PadSpread) {
            QPointF p0 = lg->start();
            QPointF p1 = lg->finalStop();
            const QGradient::CoordinateMode cm = lg->coordinateMode();
            if (cm == QGradient::ObjectBoundingMode || cm == QGradient::ObjectMode) {
                p0 = QPointF(p0.x() * bounds.width(), p0.y() * bounds.height());
                p1 = QPointF(p1.x() * bounds.width(), p1.y() * bounds.height());
            } else {
                // QGradient::LogicalMode -> convert from widget space to box-local space!
                p0 -= bounds.topLeft();
                p1 -= bounds.topLeft();
            }
            outFill = QPhaseFill::linearGradient(p0, p1, stops.first().second, stops.last().second);
            return true;
        }
    }
    return false;
}

inline bool isSimpleSolidPen(const QPen &pen) noexcept
{
    if (pen.style() == Qt::NoPen)
        return true;
    return pen.style() == Qt::SolidLine && pen.brush().style() == Qt::SolidPattern;
}

inline float effectivePenWidth(const QPen &pen) noexcept
{
    if (pen.style() == Qt::NoPen)
        return 0.0f;
    const qreal w = pen.widthF();
    return w <= 0.0 ? 1.0f : float(w);
}

} // namespace

// ============================================================================
// QPhaseWidgetImageCache
// ============================================================================

uint32_t QPhaseWidgetImageCache::acquireImage(QPhaseSceneArena &arena, const QImage &img)
{
    if (img.isNull())
        return UINT32_MAX;
    const qint64 key = img.cacheKey();
    const auto &imgs = arena.images();
    const auto it = imageSlots.constFind(key);
    if (it != imageSlots.cend()) {
        const CachedImageSlot cached = it.value();
        if (cached.slot < imgs.size()
            && imgs[cached.slot].active
            && imgs[cached.slot].generation == cached.generation) {
            ++slotRefCount[cached.slot];
            return cached.slot;
        }
        imageSlots.erase(it);
    }
    const uint32_t slot = arena.registerOrGetImage(img);
    if (slot != UINT32_MAX && slot < arena.images().size()) {
        imageSlots.insert(key, { slot, arena.images()[slot].generation });
        ++slotRefCount[slot];
    }
    return slot;
}

uint32_t QPhaseWidgetImageCache::acquirePixmap(QPhaseSceneArena &arena, const QPixmap &pm)
{
    if (pm.isNull())
        return UINT32_MAX;
    const qint64 key = pm.cacheKey();
    const auto &imgs = arena.images();
    const auto it = pixmapSlots.constFind(key);
    if (it != pixmapSlots.cend()) {
        const CachedImageSlot cached = it.value();
        if (cached.slot < imgs.size()
            && imgs[cached.slot].active
            && imgs[cached.slot].generation == cached.generation) {
            ++slotRefCount[cached.slot];
            return cached.slot;
        }
        pixmapSlots.erase(it);
    }
    const QImage img = pm.toImage();
    const uint32_t slot = arena.registerOrGetImage(img);
    if (slot != UINT32_MAX && slot < arena.images().size()) {
        pixmapSlots.insert(key, { slot, arena.images()[slot].generation });
        ++slotRefCount[slot];
    }
    return slot;
}

uint32_t QPhaseWidgetImageCache::acquireHashedRaster(QPhaseSceneArena &arena,
                                                     uint64_t contentHash,
                                                     const QImage &premulRgba)
{
    const auto &imgs = arena.images();
    auto it = rasterFallbackSlots.find(contentHash);
    if (it != rasterFallbackSlots.end()) {
        const CachedImageSlot cached = it.value();
        if (cached.slot < imgs.size()
            && imgs[cached.slot].active
            && imgs[cached.slot].generation == cached.generation) {
            ++slotRefCount[cached.slot];
            return cached.slot;
        }
        rasterFallbackSlots.erase(it);
    }
    if (premulRgba.isNull())
        return UINT32_MAX;
    const uint32_t slot = arena.registerOrGetImage(premulRgba);
    if (slot != UINT32_MAX && slot < arena.images().size()) {
        rasterFallbackSlots.insert(contentHash, { slot, arena.images()[slot].generation });
        ++slotRefCount[slot];
    }
    return slot;
}

void QPhaseWidgetImageCache::releaseImageSlots(QPhaseSceneArena &arena,
                                               uint64_t ownerEpoch,
                                               const std::vector<uint32_t> &slotsToRelease)
{
    if (ownerEpoch != epoch)
        return;
    for (uint32_t slot : slotsToRelease) {
        auto it = slotRefCount.find(slot);
        if (it == slotRefCount.end())
            continue;
        if (it.value() > 1u) {
            --it.value();
        } else {
            slotRefCount.erase(it);
            arena.unregisterImage(slot);
        }
    }
}

void QPhaseWidgetImageCache::sweepDeadEntries(const QPhaseSceneArena &arena)
{
    const qsizetype mapTotal = pixmapSlots.size() + imageSlots.size() + rasterFallbackSlots.size();
    if (mapTotal <= slotRefCount.size() * 2 + 256)
        return;

    const auto &imgs = arena.images();
    auto isAlive = [&](const CachedImageSlot &entry) {
        return entry.slot < imgs.size()
                && imgs[entry.slot].active
                && imgs[entry.slot].generation == entry.generation
                && slotRefCount.contains(entry.slot);
    };
    for (auto it = pixmapSlots.begin(); it != pixmapSlots.end(); )
        it = isAlive(it.value()) ? std::next(it) : pixmapSlots.erase(it);
    for (auto it = imageSlots.begin(); it != imageSlots.end(); )
        it = isAlive(it.value()) ? std::next(it) : imageSlots.erase(it);
    for (auto it = rasterFallbackSlots.begin(); it != rasterFallbackSlots.end(); )
        it = isAlive(it.value()) ? std::next(it) : rasterFallbackSlots.erase(it);
}

void QPhaseWidgetImageCache::clear(QPhaseSceneArena &arena)
{
    for (auto it = slotRefCount.cbegin(); it != slotRefCount.cend(); ++it)
        arena.unregisterImage(it.key());
    slotRefCount.clear();
    pixmapSlots.clear();
    imageSlots.clear();
    rasterFallbackSlots.clear();
    ++epoch;
}

// ============================================================================
// QPhasePaintEngine
// ============================================================================

QPhasePaintEngine::QPhasePaintEngine(QPhaseSceneArena &arena,
                                     QPhaseWidgetImageCache &imageCache,
                                     uint32_t baseTransformSlot,
                                     uint32_t baseClipSlot,
                                     qreal devicePixelRatio,
                                     std::vector<PhaseGpuPrimitive> &outPrimitives,
                                     std::vector<uint32_t> &outTransientTransforms,
                                     std::vector<uint32_t> &outTransientClips,
                                     std::vector<uint32_t> &outOwnedImageSlots)
    : QPaintEngine(PaintEngineFeatures(
            PrimitiveTransform
            | PixmapTransform
            | AlphaBlend
            | Antialiasing
            | BrushStroke
            | ConstantOpacity
            | LinearGradientFill
            | RadialGradientFill
            | ConicalGradientFill
            | PainterPaths
            | PaintOutsidePaintEvent
            | BlendModes))
    , m_arena(arena)
    , m_imageCache(imageCache)
    , m_baseTransformSlot(baseTransformSlot)
    , m_baseClipSlot(baseClipSlot)
    , m_dpr(devicePixelRatio > 0.0 ? devicePixelRatio : 1.0)
    , m_outPrimitives(outPrimitives)
    , m_outTransientTransforms(outTransientTransforms)
    , m_outTransientClips(outTransientClips)
    , m_outOwnedImageSlots(outOwnedImageSlots)
    , m_activeTransformSlot(baseTransformSlot)
    , m_activeClipSlots({baseClipSlot})
{
}

bool QPhasePaintEngine::begin(QPaintDevice *pdev)
{
    Q_UNUSED(pdev);
    setActive(true);
    m_transform = QTransform();
    m_pen = QPen(Qt::black);
    m_brush = QBrush(Qt::NoBrush);
    m_opacity = 1.0;
    m_antialiasing = false;
    m_compMode = QPainter::CompositionMode_SourceOver;
    m_clipEnabled = false;
    m_clipRegion = QRegion();
    m_activeTransformSlot = m_baseTransformSlot;
    m_activeClipSlots = {m_baseClipSlot};
    m_transformSlotValid = true;
    m_clipSlotValid = true;
    return true;
}

bool QPhasePaintEngine::end()
{
    setActive(false);
    return true;
}

void QPhasePaintEngine::updateState(const QPaintEngineState &s)
{
    const QPaintEngine::DirtyFlags flags = s.state();

    if (flags & DirtyTransform) {
        m_transform = s.transform();
        m_transformSlotValid = false;
    }
    if (flags & DirtyOpacity) {
        m_opacity = s.opacity();
        m_transformSlotValid = false;
    }
    if (flags & DirtyPen)
        m_pen = s.pen();
    if (flags & DirtyBrush)
        m_brush = s.brush();
    if (flags & DirtyCompositionMode)
        m_compMode = s.compositionMode();
    if (flags & DirtyHints)
        m_antialiasing = s.renderHints().testFlag(QPainter::Antialiasing);

    if (flags & (DirtyClipEnabled | DirtyClipRegion | DirtyClipPath)) {
        const bool hasClipShape = bool(flags & (DirtyClipRegion | DirtyClipPath));
        const Qt::ClipOperation op = hasClipShape ? s.clipOperation() : Qt::ReplaceClip;
        const bool explicitlyDisabled = ((flags & DirtyClipEnabled) && !s.isClipEnabled())
                || (hasClipShape && op == Qt::NoClip);
        if (explicitlyDisabled) {
            m_clipEnabled = false;
            m_clipRegion = QRegion();
        } else {
            const bool wasClipEnabled = m_clipEnabled;
            m_clipEnabled = true;
            if (flags & DirtyClipPath) {
                const QPainterPath mapped = m_transform.map(s.clipPath());
                const QRegion rgn(mapped.boundingRect().toAlignedRect());
                if (op == Qt::ReplaceClip || !wasClipEnabled)
                    m_clipRegion = rgn;
                else
                    m_clipRegion &= rgn;
            } else if (flags & DirtyClipRegion) {
                const QRegion mapped = m_transform.map(s.clipRegion());
                if (op == Qt::ReplaceClip || !wasClipEnabled)
                    m_clipRegion = mapped;
                else
                    m_clipRegion &= mapped;
            } else if (m_clipRegion.isEmpty()) {
                const QRegion rgn = s.clipRegion();
                if (!rgn.isEmpty()) {
                    m_clipRegion = m_transform.map(rgn);
                } else {
                    const QPainterPath cp = s.clipPath();
                    if (!cp.isEmpty())
                        m_clipRegion = QRegion(m_transform.map(cp).boundingRect().toAlignedRect());
                    else
                        m_clipEnabled = false;
                }
            }
        }
        m_clipSlotValid = false;
    }
}

uint32_t QPhasePaintEngine::ensureCurrentTransformSlot()
{
    if (m_transformSlotValid)
        return m_activeTransformSlot;

    const bool isIdent = m_transform.isIdentity() && qFuzzyCompare(m_opacity, 1.0);
    if (isIdent) {
        m_activeTransformSlot = m_baseTransformSlot;
        m_transformSlotValid = true;
        return m_activeTransformSlot;
    }

    const uint32_t slot = m_arena.allocateTransform(m_baseTransformSlot);
    m_arena.updateTransform2D(slot,
                              float(m_transform.m11()),
                              float(m_transform.m21()),
                              float(m_transform.m12()),
                              float(m_transform.m22()),
                              float(m_transform.dx()),
                              float(m_transform.dy()),
                              float(std::clamp(m_opacity, 0.0, 1.0)));
    m_outTransientTransforms.push_back(slot);
    m_activeTransformSlot = slot;
    m_transformSlotValid = true;
    return slot;
}

void QPhasePaintEngine::ensureCurrentClipSlots(std::vector<uint32_t> &outClipSlots)
{
    if (!m_clipSlotValid) {
        m_activeClipSlots.clear();
        if (!m_clipEnabled) {
            m_activeClipSlots.push_back(m_baseClipSlot);
        } else if (!m_clipRegion.isEmpty()) {
            m_activeClipSlots.reserve(size_t(m_clipRegion.rectCount()));
            for (const QRect &cr : m_clipRegion) {
                if (cr.width() <= 0 || cr.height() <= 0)
                    continue;
                const uint32_t slot = m_arena.allocateClip(m_baseClipSlot, m_baseTransformSlot,
                                                           QRectF(cr), QPhaseCorners{});
                m_outTransientClips.push_back(slot);
                m_activeClipSlots.push_back(slot);
            }
        }
        m_clipSlotValid = true;
    }
    outClipSlots = m_activeClipSlots;
}

void QPhasePaintEngine::pushPrimitiveClipped(const PhaseGpuPrimitive &prim)
{
    std::vector<uint32_t> clips;
    ensureCurrentClipSlots(clips);
    for (uint32_t c : clips) {
        PhaseGpuPrimitive copy = prim;
        copy.clipIndex = c;
        m_outPrimitives.push_back(copy);
    }
}

void QPhasePaintEngine::emitRasterFallback(const QRectF &localBounds,
                                           uint64_t extraHash,
                                           const std::function<void(QPainter &)> &drawFn)
{
    if (localBounds.width() <= 0.0 && localBounds.height() <= 0.0 && m_pen.style() == Qt::NoPen)
        return;

    const QRectF mappedBounds = m_transform.mapRect(localBounds);
    const float pad = std::max(2.0f, effectivePenWidth(m_pen) * 1.5f);
    const QRectF padded = mappedBounds.adjusted(-pad, -pad, pad, pad);
    if (padded.width() <= 0.0 || padded.height() <= 0.0)
        return;

    // Never truncate wide/tall fallback primitives (e.g. wide item-view selections on HiDPI):
    // support up to 8192px natively and gracefully scale down if a primitive exceeds 8192px.
    constexpr int kMaxFallbackDim = 8192;
    const double rawW = std::ceil(padded.width() * m_dpr);
    const double rawH = std::ceil(padded.height() * m_dpr);
    const int pxW = std::clamp(int(rawW), 1, kMaxFallbackDim);
    const int pxH = std::clamp(int(rawH), 1, kMaxFallbackDim);
    const double scaleX = double(pxW) / padded.width();
    const double scaleY = double(pxH) / padded.height();

    uint64_t h = extraHash;
    h = hashVal(h, pxW);
    h = hashVal(h, pxH);
    h = hashVal(h, scaleX);
    h = hashVal(h, scaleY);
    h = hashVal(h, m_antialiasing);
    const QPointF lbOrigin = localBounds.topLeft();
    h = hashVal(h, lbOrigin);
    const qreal txVals[6] = {
        m_transform.m11(), m_transform.m12(), m_transform.m21(), m_transform.m22(),
        m_transform.dx() - padded.left(), m_transform.dy() - padded.top()
    };
    h = hashMix64(h, txVals, sizeof(txVals));
    h = hashPenBrush(h, m_pen, m_brush);

    uint32_t slot = m_imageCache.acquireHashedRaster(m_arena, h, QImage());
    if (slot == UINT32_MAX) {
        QImage img(pxW, pxH, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter rp(&img);
            rp.setRenderHint(QPainter::Antialiasing, m_antialiasing);
            rp.scale(scaleX, scaleY);
            rp.translate(-padded.topLeft());
            rp.setTransform(m_transform, true);
            rp.setPen(m_pen);
            rp.setBrush(m_brush);
            drawFn(rp);
        }
        slot = m_imageCache.acquireHashedRaster(m_arena, h, img);
    }

    if (slot == UINT32_MAX)
        return;

    m_outOwnedImageSlots.push_back(slot);

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::Image);
    p.transformIndex = m_baseTransformSlot;
    p.resourceIndex = slot;
    p.x = float(padded.x());
    p.y = float(padded.y());
    p.w = float(padded.width());
    p.h = float(padded.height());
    p.c0r = 1.0f;
    p.c0g = 1.0f;
    p.c0b = 1.0f;
    p.c0a = float(std::clamp(m_opacity, 0.0, 1.0));
    p.p0 = 0.0f;
    p.p1 = 0.0f;
    p.p2 = 1.0f;
    p.p3 = 1.0f;
    pushPrimitiveClipped(p);
}

void QPhasePaintEngine::emitRectOrRounded(const QRectF &r, float rx, float ry)
{
    if (r.width() <= 0.0 && r.height() <= 0.0)
        return;

    const bool hasFill = (m_brush.style() != Qt::NoBrush);
    const bool hasStroke = (m_pen.style() != Qt::NoPen);
    if (!hasFill && !hasStroke)
        return;

    QPhaseFill fill{};
    const bool fastFill = !hasFill || tryBuildPhaseFill(m_brush, r, fill);
    const bool fastPen = isSimpleSolidPen(m_pen);
    const bool uniformRadius = std::abs(rx - ry) < 0.25f;

    if (!fastFill || !fastPen || !uniformRadius) {
        uint64_t h = 0x52454354ULL;
        const double rw = r.width(), rh = r.height();
        h = hashVal(h, rw);
        h = hashVal(h, rh);
        h = hashVal(h, rx);
        h = hashVal(h, ry);
        emitRasterFallback(r, h, [&](QPainter &rp) {
            if (rx > 0.0f || ry > 0.0f)
                rp.drawRoundedRect(r, rx, ry);
            else
                rp.drawRect(r);
        });
        return;
    }

    const uint32_t txSlot = ensureCurrentTransformSlot();

    // Pure opaque/translucent solid rect without border or radius -> fastest SolidRect path!
    if (hasFill && !hasStroke && rx <= 0.0f && m_brush.style() == Qt::SolidPattern) {
        if (fill.color0.alpha() == 0)
            return;
        PhaseGpuPrimitive p{};
        p.kind = uint32_t(PhasePrimKind::SolidRect);
        p.transformIndex = txSlot;
        p.x = float(r.x());
        p.y = float(r.y());
        p.w = float(r.width());
        p.h = float(r.height());
        p.c0r = fill.color0.redF();
        p.c0g = fill.color0.greenF();
        p.c0b = fill.color0.blueF();
        p.c0a = fill.color0.alphaF();
        pushPrimitiveClipped(p);
        return;
    }

    QPhaseVisualsPrivate vp{};
    vp.arena = &m_arena;
    vp.transformIndex = txSlot;
    vp.clipIndex = m_baseClipSlot;
    QPhaseVisuals vis(&vp);

    QPhaseBoxStyle style{};
    QRectF boxR = r;
    float outerR = std::max(0.0f, rx);
    style.fill = hasFill ? fill : QPhaseFill::solid(QColor(0, 0, 0, 0));
    if (hasStroke) {
        // QPainter strokes centered on the rect boundary while QPhaseBoxStyle insets the border
        const float bw = effectivePenWidth(m_pen);
        const float halfW = bw * 0.5f;
        boxR = r.adjusted(-halfW, -halfW, halfW, halfW);
        if (outerR > 0.0f)
            outerR += halfW;
        style.border.width = bw;
        style.border.color = m_pen.color();
    }
    style.radius = QPhaseCorners::all(outerR);
    vis.roundedBox(boxR, style);
    for (const PhaseGpuPrimitive &p : vp.recordedPrimitives)
        pushPrimitiveClipped(p);
}

void QPhasePaintEngine::drawRects(const QRect *rects, int rectCount)
{
    const bool hasStroke = (m_pen.style() != Qt::NoPen);
    for (int i = 0; i < rectCount; ++i) {
        QRectF rf(rects[i]);
        if (hasStroke)
            rf.adjust(0.5, 0.5, 0.5, 0.5);
        emitRectOrRounded(rf, 0.0f, 0.0f);
    }
}

void QPhasePaintEngine::drawRects(const QRectF *rects, int rectCount)
{
    for (int i = 0; i < rectCount; ++i)
        emitRectOrRounded(rects[i], 0.0f, 0.0f);
}

void QPhasePaintEngine::drawLines(const QLine *lines, int lineCount)
{
    if (m_pen.style() == Qt::NoPen || lineCount <= 0)
        return;

    if (!isSimpleSolidPen(m_pen)) {
        QRectF bounds;
        for (int i = 0; i < lineCount; ++i)
            bounds = bounds.united(QRectF(lines[i].p1(), lines[i].p2()).normalized());
        uint64_t h = 0x4C494E45ULL;
        for (int i = 0; i < lineCount; ++i)
            h = hashVal(h, lines[i]);
        emitRasterFallback(bounds, h, [&](QPainter &rp) { rp.drawLines(lines, lineCount); });
        return;
    }

    const uint32_t txSlot = ensureCurrentTransformSlot();
    const float w = effectivePenWidth(m_pen);
    const QColor c = m_pen.color();
    if (c.alpha() == 0)
        return;

    QPhaseVisualsPrivate vp{};
    vp.arena = &m_arena;
    vp.transformIndex = txSlot;
    vp.clipIndex = m_baseClipSlot;
    QPhaseVisuals vis(&vp);
    const bool butt = (m_pen.capStyle() != Qt::RoundCap);
    for (int i = 0; i < lineCount; ++i) {
        vis.strokeLine(QPointF(lines[i].x1() + 0.5, lines[i].y1() + 0.5),
                       QPointF(lines[i].x2() + 0.5, lines[i].y2() + 0.5),
                       w, c, butt);
    }
    for (const PhaseGpuPrimitive &p : vp.recordedPrimitives)
        pushPrimitiveClipped(p);
}

void QPhasePaintEngine::drawLines(const QLineF *lines, int lineCount)
{
    if (m_pen.style() == Qt::NoPen || lineCount <= 0)
        return;

    if (!isSimpleSolidPen(m_pen)) {
        QRectF bounds;
        for (int i = 0; i < lineCount; ++i)
            bounds = bounds.united(QRectF(lines[i].p1(), lines[i].p2()).normalized());
        uint64_t h = 0x4C494E46ULL;
        for (int i = 0; i < lineCount; ++i)
            h = hashVal(h, lines[i]);
        emitRasterFallback(bounds, h, [&](QPainter &rp) { rp.drawLines(lines, lineCount); });
        return;
    }

    const uint32_t txSlot = ensureCurrentTransformSlot();
    const float w = effectivePenWidth(m_pen);
    const QColor c = m_pen.color();
    if (c.alpha() == 0)
        return;

    QPhaseVisualsPrivate vp{};
    vp.arena = &m_arena;
    vp.transformIndex = txSlot;
    vp.clipIndex = m_baseClipSlot;
    QPhaseVisuals vis(&vp);
    const bool butt = (m_pen.capStyle() != Qt::RoundCap);
    for (int i = 0; i < lineCount; ++i)
        vis.strokeLine(lines[i].p1(), lines[i].p2(), w, c, butt);
    for (const PhaseGpuPrimitive &p : vp.recordedPrimitives)
        pushPrimitiveClipped(p);
}

void QPhasePaintEngine::drawEllipse(const QRectF &r)
{
    if (r.width() <= 0.0 || r.height() <= 0.0)
        return;
    // Exact circle or pill -> analytical SDF RoundedBox!
    if (std::abs(r.width() - r.height()) < 0.5) {
        const float rad = float(std::min(r.width(), r.height()) * 0.5);
        emitRectOrRounded(r, rad, rad);
        return;
    }
    uint64_t h = 0x454C4C50ULL;
    const double rw = r.width(), rh = r.height();
    h = hashVal(h, rw);
    h = hashVal(h, rh);
    emitRasterFallback(r, h, [&](QPainter &rp) { rp.drawEllipse(r); });
}

void QPhasePaintEngine::drawEllipse(const QRect &r)
{
    drawEllipse(QRectF(r));
}

bool QPhasePaintEngine::tryEmitFastPath(const QPainterPath &path)
{
    const int n = path.elementCount();
    if (n == 0)
        return true;

    // 1. Stroked polyline fast path (no fill, only MoveTo + LineTo)
    if (m_brush.style() == Qt::NoBrush && isSimpleSolidPen(m_pen) && m_pen.style() != Qt::NoPen) {
        bool onlyLines = true;
        for (int i = 0; i < n; ++i) {
            const auto t = path.elementAt(i).type;
            if (t != QPainterPath::MoveToElement && t != QPainterPath::LineToElement) {
                onlyLines = false;
                break;
            }
        }
        if (onlyLines) {
            const uint32_t txSlot = ensureCurrentTransformSlot();
            const float w = effectivePenWidth(m_pen);
            const QColor c = m_pen.color();
            if (c.alpha() == 0)
                return true;

            QPhaseVisualsPrivate vp{};
            vp.arena = &m_arena;
            vp.transformIndex = txSlot;
            vp.clipIndex = m_baseClipSlot;
            QPhaseVisuals vis(&vp);
            const bool butt = (m_pen.capStyle() != Qt::RoundCap);
            QPointF cur;
            for (int i = 0; i < n; ++i) {
                const auto el = path.elementAt(i);
                const QPointF pt(el.x, el.y);
                if (el.type == QPainterPath::LineToElement)
                    vis.strokeLine(cur, pt, w, c, butt);
                cur = pt;
            }
            for (const PhaseGpuPrimitive &p : vp.recordedPrimitives)
                pushPrimitiveClipped(p);
            return true;
        }
    }

    // 2. Orientation-independent rounded rectangle detector (handles Qt 6 QPainterPath::addRoundedRect
    //    which emits 18 elements starting at br.left(), 17-element variants, and capsules/pills!).
    if (n >= 13 && n <= 19 && path.elementAt(0).type == QPainterPath::MoveToElement) {
        const QRectF br = path.boundingRect();
        if (br.width() > 0.0 && br.height() > 0.0) {
            constexpr qreal kTol = 0.1;
            auto onVertical = [&](qreal x) {
                return std::abs(x - br.left()) < kTol || std::abs(x - br.right()) < kTol;
            };
            auto onHorizontal = [&](qreal y) {
                return std::abs(y - br.top()) < kTol || std::abs(y - br.bottom()) < kTol;
            };

            bool valid = true;
            int cubicCount = 0;
            qreal detectedRx = -1.0;
            qreal detectedRy = -1.0;
            QPointF cur(path.elementAt(0).x, path.elementAt(0).y);
            if (!onVertical(cur.x()) && !onHorizontal(cur.y()))
                valid = false;

            for (int i = 1; i < n && valid; ) {
                const auto el = path.elementAt(i);
                if (el.type == QPainterPath::LineToElement) {
                    const QPointF next(el.x, el.y);
                    // Must stay along a single outer edge of br (or zero-length move/arc join)
                    const bool sameEdge = (std::abs(next.x() - cur.x()) < kTol && onVertical(next.x()))
                            || (std::abs(next.y() - cur.y()) < kTol && onHorizontal(next.y()));
                    if (!sameEdge)
                        valid = false;
                    cur = next;
                    ++i;
                } else if (el.type == QPainterPath::CurveToElement && i + 2 < n
                           && path.elementAt(i + 1).type == QPainterPath::CurveToDataElement
                           && path.elementAt(i + 2).type == QPainterPath::CurveToDataElement) {
                    const QPointF c1(el.x, el.y);
                    const QPointF c2(path.elementAt(i + 1).x, path.elementAt(i + 1).y);
                    const QPointF end(path.elementAt(i + 2).x, path.elementAt(i + 2).y);
                    const bool vToH = onVertical(cur.x()) && onHorizontal(end.y());
                    const bool hToV = onHorizontal(cur.y()) && onVertical(end.x());
                    if (!vToH && !hToV) {
                        valid = false;
                        break;
                    }
                    // Verify control points stay tangent to the corner edges
                    const bool tangentsOk = vToH
                            ? (std::abs(c1.x() - cur.x()) < kTol && std::abs(c2.y() - end.y()) < kTol)
                            : (std::abs(c1.y() - cur.y()) < kTol && std::abs(c2.x() - end.x()) < kTol);
                    const qreal rx = std::abs(end.x() - cur.x());
                    const qreal ry = std::abs(end.y() - cur.y());
                    if (!tangentsOk || rx <= 0.0 || ry <= 0.0) {
                        valid = false;
                        break;
                    }
                    if (cubicCount == 0) {
                        detectedRx = rx;
                        detectedRy = ry;
                    } else if (std::abs(rx - detectedRx) > 0.25 || std::abs(ry - detectedRy) > 0.25) {
                        valid = false;
                        break;
                    }
                    ++cubicCount;
                    cur = end;
                    i += 3;
                } else {
                    valid = false;
                }
            }

            if (valid && cubicCount == 4 && detectedRx > 0.0 && detectedRy > 0.0
                && std::abs(detectedRx - detectedRy) < 0.25) {
                emitRectOrRounded(br, float(detectedRx), float(detectedRy));
                return true;
            }
        }
    }

    if (n == 5 && path.elementAt(0).type == QPainterPath::MoveToElement) {
        const QRectF br = path.boundingRect();
        bool isAxisRect = true;
        for (int i = 0; i < 5; ++i) {
            const auto el = path.elementAt(i);
            if (i > 0 && el.type != QPainterPath::LineToElement) {
                isAxisRect = false;
                break;
            }
            const bool onX = (std::abs(el.x - br.left()) < 0.01 || std::abs(el.x - br.right()) < 0.01);
            const bool onY = (std::abs(el.y - br.top()) < 0.01 || std::abs(el.y - br.bottom()) < 0.01);
            if (!onX || !onY) {
                isAxisRect = false;
                break;
            }
        }
        if (isAxisRect) {
            emitRectOrRounded(br, 0.0f, 0.0f);
            return true;
        }
    }

    return false;
}

void QPhasePaintEngine::drawPath(const QPainterPath &path)
{
    if (path.isEmpty())
        return;
    if (tryEmitFastPath(path))
        return;

    const QRectF br = path.boundingRect();
    if (br.width() <= 0.0 || br.height() <= 0.0)
        return;

    uint64_t h = 0x50415448ULL;
    const int n = path.elementCount();
    h = hashVal(h, n);
    const int fr = int(path.fillRule());
    h = hashVal(h, fr);
    const qreal ox = br.left(), oy = br.top();
    for (int i = 0; i < n; ++i) {
        const auto el = path.elementAt(i);
        const int t = int(el.type);
        const float rx = float(el.x - ox);
        const float ry = float(el.y - oy);
        h = hashVal(h, t);
        h = hashVal(h, rx);
        h = hashVal(h, ry);
    }

    emitRasterFallback(br, h, [&](QPainter &rp) { rp.drawPath(path); });
}

void QPhasePaintEngine::drawPoints(const QPointF *points, int pointCount)
{
    if (m_pen.style() == Qt::NoPen || pointCount <= 0)
        return;
    const float w = effectivePenWidth(m_pen);
    const QColor c = m_pen.color();
    if (c.alpha() == 0)
        return;

    const uint32_t txSlot = ensureCurrentTransformSlot();
    const float half = w * 0.5f;
    for (int i = 0; i < pointCount; ++i) {
        PhaseGpuPrimitive p{};
        p.kind = uint32_t(PhasePrimKind::SolidRect);
        p.transformIndex = txSlot;
        p.x = float(points[i].x()) - half;
        p.y = float(points[i].y()) - half;
        p.w = w;
        p.h = w;
        p.c0r = c.redF();
        p.c0g = c.greenF();
        p.c0b = c.blueF();
        p.c0a = c.alphaF();
        pushPrimitiveClipped(p);
    }
}

void QPhasePaintEngine::drawPoints(const QPoint *points, int pointCount)
{
    QVarLengthArray<QPointF, 16> pts(pointCount);
    for (int i = 0; i < pointCount; ++i)
        pts[i] = QPointF(points[i]);
    drawPoints(pts.constData(), pointCount);
}

void QPhasePaintEngine::drawPolygon(const QPointF *points, int pointCount, PolygonDrawMode mode)
{
    if (pointCount <= 1)
        return;

    if (mode == PolylineMode && isSimpleSolidPen(m_pen) && m_pen.style() != Qt::NoPen) {
        const uint32_t txSlot = ensureCurrentTransformSlot();
        QPhaseVisualsPrivate vp{};
        vp.arena = &m_arena;
        vp.transformIndex = txSlot;
        vp.clipIndex = m_baseClipSlot;
        QPhaseVisuals vis(&vp);
        QList<QPointF> list;
        list.reserve(pointCount);
        for (int i = 0; i < pointCount; ++i)
            list.append(points[i]);
        vis.polyline(list, effectivePenWidth(m_pen), m_pen.color(), false);
        for (const PhaseGpuPrimitive &p : vp.recordedPrimitives)
            pushPrimitiveClipped(p);
        return;
    }

    QPainterPath path;
    path.moveTo(points[0]);
    for (int i = 1; i < pointCount; ++i)
        path.lineTo(points[i]);
    if (mode != PolylineMode)
        path.closeSubpath();
    path.setFillRule(mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
    drawPath(path);
}

void QPhasePaintEngine::drawPolygon(const QPoint *points, int pointCount, PolygonDrawMode mode)
{
    QVarLengthArray<QPointF, 16> pts(pointCount);
    for (int i = 0; i < pointCount; ++i)
        pts[i] = QPointF(points[i]);
    drawPolygon(pts.constData(), pointCount, mode);
}

void QPhasePaintEngine::drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &sr)
{
    if (pm.isNull() || r.isEmpty())
        return;
    const uint32_t slot = m_imageCache.acquirePixmap(m_arena, pm);
    if (slot == UINT32_MAX)
        return;
    m_outOwnedImageSlots.push_back(slot);

    // QPaintEngine::drawPixmap receives sr ALREADY in physical pixels of pm!
    const QSizeF imgSize(double(pm.width()), double(pm.height()));
    QRectF uv(0.0, 0.0, 1.0, 1.0);
    if (!sr.isEmpty() && imgSize.width() > 0.0 && imgSize.height() > 0.0) {
        uv = QRectF(sr.left() / imgSize.width(),
                    sr.top() / imgSize.height(),
                    sr.width() / imgSize.width(),
                    sr.height() / imgSize.height());
    }

    const uint32_t txSlot = ensureCurrentTransformSlot();
    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::Image);
    p.transformIndex = txSlot;
    p.resourceIndex = slot;
    p.x = float(r.x());
    p.y = float(r.y());
    p.w = float(r.width());
    p.h = float(r.height());
    p.c0r = 1.0f;
    p.c0g = 1.0f;
    p.c0b = 1.0f;
    p.c0a = 1.0f;
    p.p0 = float(uv.left());
    p.p1 = float(uv.top());
    p.p2 = float(uv.right());
    p.p3 = float(uv.bottom());
    pushPrimitiveClipped(p);
}

void QPhasePaintEngine::drawImage(const QRectF &r, const QImage &img, const QRectF &sr,
                                  Qt::ImageConversionFlags flags)
{
    Q_UNUSED(flags);
    if (img.isNull() || r.isEmpty())
        return;
    const uint32_t slot = m_imageCache.acquireImage(m_arena, img);
    if (slot == UINT32_MAX)
        return;
    m_outOwnedImageSlots.push_back(slot);

    QRectF uv(0.0, 0.0, 1.0, 1.0);
    if (!sr.isEmpty() && img.width() > 0 && img.height() > 0) {
        uv = QRectF(sr.left() / double(img.width()),
                    sr.top() / double(img.height()),
                    sr.width() / double(img.width()),
                    sr.height() / double(img.height()));
    }

    const uint32_t txSlot = ensureCurrentTransformSlot();
    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::Image);
    p.transformIndex = txSlot;
    p.resourceIndex = slot;
    p.x = float(r.x());
    p.y = float(r.y());
    p.w = float(r.width());
    p.h = float(r.height());
    p.c0r = 1.0f;
    p.c0g = 1.0f;
    p.c0b = 1.0f;
    p.c0a = 1.0f;
    p.p0 = float(uv.left());
    p.p1 = float(uv.top());
    p.p2 = float(uv.right());
    p.p3 = float(uv.bottom());
    pushPrimitiveClipped(p);
}

void QPhasePaintEngine::drawTiledPixmap(const QRectF &r, const QPixmap &pixmap, const QPointF &s)
{
    if (pixmap.isNull() || r.isEmpty())
        return;
    const qreal dpr = pixmap.devicePixelRatio() > 0.0 ? pixmap.devicePixelRatio() : 1.0;
    const qreal tw = double(pixmap.width()) / dpr;
    const qreal th = double(pixmap.height()) / dpr;
    if (tw <= 0.0 || th <= 0.0)
        return;

    const qreal startX = r.left() - std::fmod(s.x(), tw);
    const qreal startY = r.top() - std::fmod(s.y(), th);
    for (qreal y = startY; y < r.bottom(); y += th) {
        for (qreal x = startX; x < r.right(); x += tw)
            drawPixmap(QRectF(x, y, tw, th), pixmap, QRectF());
    }
}

void QPhasePaintEngine::drawTextItem(const QPointF &p, const QTextItem &textItem)
{
    const auto &ti = static_cast<const QTextItemInt &>(textItem);
    if (!ti.fontEngine || ti.glyphs.numGlyphs <= 0)
        return;

    const QColor color = m_pen.color();
    if (color.alpha() == 0)
        return;

    QRawFont rawFont;
    QRawFontPrivate::get(rawFont)->setFontEngine(ti.fontEngine);
    if (!rawFont.isValid())
        return;

    // Reuse QFontEngine::getGlyphPositions() directly so RightToLeft (Arabic/Hebrew),
    // kashidas, combining mark offsets, and dontPrint attributes match Qt's raster engine 1:1!
    QVarLengthArray<glyph_t> outGlyphs;
    QVarLengthArray<QFixedPoint> outPositions;
    ti.fontEngine->getGlyphPositions(ti.glyphs, QTransform(), ti.flags, outGlyphs, outPositions);
    if (outGlyphs.isEmpty())
        return;

    const int n = outGlyphs.size();
    QList<quint32> glyphIndexes;
    QList<QPointF> positions;
    glyphIndexes.reserve(n);
    positions.reserve(n);
    for (int i = 0; i < n; ++i) {
        glyphIndexes.append(outGlyphs[i]);
        positions.append(outPositions[i].toPointF());
    }

    QGlyphRun run;
    run.setRawFont(rawFont);
    run.setGlyphIndexes(glyphIndexes);
    run.setPositions(positions);

    uint32_t txSlot = ensureCurrentTransformSlot();
    if (ti.fontEngine->synthesized() & QFontEngine::SynthesizedItalic) {
        // Apply analytical 2x2 synthetic oblique slant anchored at baseline p.y()!
        const uint32_t slantSlot = m_arena.allocateTransform(txSlot);
        constexpr float kSlant = -0.21f;
        m_arena.updateTransform2D(slantSlot,
                                  1.0f, kSlant,
                                  0.0f, 1.0f,
                                  -kSlant * float(p.y()), 0.0f,
                                  1.0f);
        m_outTransientTransforms.push_back(slantSlot);
        txSlot = slantSlot;
    }

    QPhaseVisualsPrivate vp{};
    vp.arena = &m_arena;
    vp.transformIndex = txSlot;
    vp.clipIndex = m_baseClipSlot;
    QPhaseVisuals vis(&vp);
    vis.glyphRun(run, p, color);

    // Handle Underline / Overline / StrikeOut decorations analytically
    const qreal runWidth = ti.width.toReal();
    if (runWidth > 0.0) {
        const qreal lineThick = std::max(1.0, rawFont.lineThickness());
        if (ti.flags & QTextItem::Underline) {
            vis.solidRect(QRectF(p.x(), p.y() + rawFont.underlinePosition(), runWidth, lineThick), color);
        }
        if (ti.flags & QTextItem::StrikeOut) {
            vis.solidRect(QRectF(p.x(), p.y() - rawFont.ascent() * 0.35, runWidth, lineThick), color);
        }
        if (ti.flags & QTextItem::Overline) {
            vis.solidRect(QRectF(p.x(), p.y() - rawFont.ascent(), runWidth, lineThick), color);
        }
    }

    for (const PhaseGpuPrimitive &prim : vp.recordedPrimitives)
        pushPrimitiveClipped(prim);
}

// ============================================================================
// QPhasePaintDevice
// ============================================================================

QPhasePaintDevice::QPhasePaintDevice(const QWidget *widget, QPhasePaintEngine *engine)
    : m_widget(widget)
    , m_engine(engine)
{
}

int QPhasePaintDevice::metric(PaintDeviceMetric metric) const
{
    if (!m_widget)
        return 0;
    // QPhaseSceneArena operates in device-independent logical coordinates and applies window DPR
    // natively in the Vulkan 1.3 shaders & CPU tile rasterizer (while QPhasePaintEngine uses m_dpr
    // for HiDPI raster fallbacks). Reporting logical 1:1 DPR here prevents QPainter from double-scaling
    // or shrinking clip/viewport bounds by 1/DPR!
    switch (metric) {
    case PdmWidth:
        return m_widget->width();
    case PdmHeight:
        return m_widget->height();
    case PdmWidthMM:
        return m_widget->widthMM();
    case PdmHeightMM:
        return m_widget->heightMM();
    case PdmNumColors:
        return INT_MAX;
    case PdmDepth:
        return 32;
    case PdmDpiX:
        return m_widget->logicalDpiX();
    case PdmDpiY:
        return m_widget->logicalDpiY();
    case PdmPhysicalDpiX:
        return m_widget->physicalDpiX();
    case PdmPhysicalDpiY:
        return m_widget->physicalDpiY();
    case PdmDevicePixelRatio:
        return 1;
    case PdmDevicePixelRatioScaled:
        return int(QPaintDevice::devicePixelRatioFScale());
    case PdmDevicePixelRatioF_EncodedA:
    case PdmDevicePixelRatioF_EncodedB:
        return QPaintDevice::encodeMetricF(metric, 1.0);
    }
    return 0;
}

QT_END_NAMESPACE
