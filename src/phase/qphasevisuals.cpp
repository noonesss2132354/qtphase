// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasevisuals.h"
#include "qphasevisuals_p.h"

#include <QtGui/qfontmetrics.h>
#include <QtGui/qtextlayout.h>
#include <QtGui/qtextoption.h>

#include <algorithm>
#include <cmath>

QT_BEGIN_NAMESPACE

QPhaseVisuals::QPhaseVisuals(QPhaseVisualsPrivate *d)
    : d_ptr(d)
{
}

static inline uint32_t applyLayerFlag(uint32_t animMode, const QPhaseVisualsPrivate *d) noexcept
{
    return (d && d->activeLayerDepth > 0) ? (animMode | kPhaseAnimFlagInsideLayer) : animMode;
}

void QPhaseVisuals::solidRect(const QRectF &rect, const QColor &color)
{
    if (!d_ptr || rect.isEmpty() || color.alpha() == 0)
        return;

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::SolidRect);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.x = float(rect.x());
    p.y = float(rect.y());
    p.w = float(rect.width());
    p.h = float(rect.height());
    p.c0r = color.redF();
    p.c0g = color.greenF();
    p.c0b = color.blueF();
    p.c0a = color.alphaF();
    p.animMode = applyLayerFlag(0u, d_ptr);
    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::roundedBox(const QRectF &rect, const QPhaseBoxStyle &style, const QPhaseAnimation &anim)
{
    if (!d_ptr || rect.isEmpty())
        return;

    const uint64_t nowNs = d_ptr->arena ? d_ptr->arena->monotonicTimestampNs() : 0u;
    const uint32_t animBits = applyLayerFlag(uint32_t(anim.mode), d_ptr);

    if (style.shadow.blurRadius > 0.0f && style.shadow.color.alpha() > 0) {
        PhaseGpuPrimitive sh{};
        sh.kind = uint32_t(PhasePrimKind::BoxShadow);
        sh.transformIndex = d_ptr->transformIndex;
        sh.clipIndex = d_ptr->clipIndex;
        sh.x = float(rect.x() + style.shadow.offset.x());
        sh.y = float(rect.y() + style.shadow.offset.y());
        sh.w = float(rect.width());
        sh.h = float(rect.height());
        sh.c0r = style.shadow.color.redF();
        sh.c0g = style.shadow.color.greenF();
        sh.c0b = style.shadow.color.blueF();
        sh.c0a = style.shadow.color.alphaF();
        sh.rTL = style.radius.topLeft;
        sh.rTR = style.radius.topRight;
        sh.rBR = style.radius.bottomRight;
        sh.rBL = style.radius.bottomLeft;
        sh.p0 = style.shadow.blurRadius;
        sh.p1 = style.shadow.spread;
        sh.animMode = animBits;
        sh.animDurationMs = anim.durationMs;
        sh.animStartNs = nowNs;
        sh.animFromOpacity = anim.fromOpacity;
        sh.animToOpacity = anim.toOpacity;
        d_ptr->recordedPrimitives.push_back(sh);
    }

    const bool hasFill = style.fill.color0.alpha() > 0
            || (style.fill.type == QPhaseFill::Type::LinearGradient && style.fill.color1.alpha() > 0);
    const bool hasBorder = style.border.width > 0.0f && style.border.color.alpha() > 0;
    if (!hasFill && !hasBorder)
        return;

    PhaseGpuPrimitive p{};
    p.kind = (style.radius.isZero() && !hasBorder && style.fill.type == QPhaseFill::Type::Solid && anim.mode == QPhaseAnimation::Mode::None)
            ? uint32_t(PhasePrimKind::SolidRect)
            : uint32_t(PhasePrimKind::RoundedBox);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.x = float(rect.x());
    p.y = float(rect.y());
    p.w = float(rect.width());
    p.h = float(rect.height());

    p.c0r = style.fill.color0.redF();
    p.c0g = style.fill.color0.greenF();
    p.c0b = style.fill.color0.blueF();
    p.c0a = style.fill.color0.alphaF();

    p.c1r = style.fill.color1.redF();
    p.c1g = style.fill.color1.greenF();
    p.c1b = style.fill.color1.blueF();
    p.c1a = style.fill.color1.alphaF();

    p.br = style.border.color.redF();
    p.bg = style.border.color.greenF();
    p.bb = style.border.color.blueF();
    p.ba = style.border.color.alphaF();

    p.rTL = style.radius.topLeft;
    p.rTR = style.radius.topRight;
    p.rBR = style.radius.bottomRight;
    p.rBL = style.radius.bottomLeft;

    p.p0 = style.border.width;
    p.p1 = float(uint8_t(style.fill.type));
    p.p2 = float(style.fill.gradientStart.x());
    p.p3 = float(style.fill.gradientStart.y());
    p.p4 = float(style.fill.gradientEnd.x());
    p.p5 = float(style.fill.gradientEnd.y());

    p.animMode = animBits;
    p.animDurationMs = anim.durationMs;
    p.animStartNs = nowNs;
    p.animFromOpacity = anim.fromOpacity;
    p.animToOpacity = anim.toOpacity;

    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::strokeLine(QPointF p0, QPointF p1, float width, const QColor &color, bool buttCap)
{
    if (!d_ptr || width <= 0.0f || color.alpha() == 0)
        return;

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::StrokeSegment);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.x = float(p0.x());
    p.y = float(p0.y());
    p.w = float(p1.x()); // Endpoint B stored in (w, h)
    p.h = float(p1.y());
    p.c0r = color.redF();
    p.c0g = color.greenF();
    p.c0b = color.blueF();
    p.c0a = color.alphaF();
    p.p0 = width;
    p.p1 = buttCap ? 1.0f : 0.0f;
    p.animMode = applyLayerFlag(0u, d_ptr);
    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::polyline(const QList<QPointF> &points, float width, const QColor &color, bool closed)
{
    if (!d_ptr || points.size() < 2 || width <= 0.0f || color.alpha() == 0)
        return;

    for (qsizetype i = 0; i + 1 < points.size(); ++i)
        strokeLine(points[i], points[i + 1], width, color, false);
    if (closed && points.size() > 2)
        strokeLine(points.back(), points.front(), width, color, false);
}

void QPhaseVisuals::pushLayer(const QRectF &bounds, float opacity, QPhaseBlendMode blendMode)
{
    if (!d_ptr || bounds.isEmpty())
        return;

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::PushLayer);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.resourceIndex = uint32_t(blendMode);
    p.x = float(bounds.x());
    p.y = float(bounds.y());
    p.w = float(bounds.width());
    p.h = float(bounds.height());
    p.c0a = std::clamp(opacity, 0.0f, 1.0f);
    p.animMode = kPhaseAnimFlagInsideLayer;
    d_ptr->recordedPrimitives.push_back(p);
    ++d_ptr->activeLayerDepth;
}

void QPhaseVisuals::popLayer(const QRectF &bounds, float opacity, QPhaseBlendMode blendMode)
{
    if (!d_ptr || bounds.isEmpty() || d_ptr->activeLayerDepth <= 0)
        return;

    --d_ptr->activeLayerDepth;
    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::PopLayer);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.resourceIndex = uint32_t(blendMode);
    p.x = float(bounds.x());
    p.y = float(bounds.y());
    p.w = float(bounds.width());
    p.h = float(bounds.height());
    p.c0a = std::clamp(opacity, 0.0f, 1.0f);
    p.animMode = kPhaseAnimFlagInsideLayer;
    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::image(const QImage &img, const QRectF &targetRect,
                          QPhaseCorners clipRadius, const QRectF &sourceRect, float opacity)
{
    if (!d_ptr || !d_ptr->arena || img.isNull() || targetRect.isEmpty() || opacity <= 0.0f)
        return;

    const uint32_t imgSlot = d_ptr->arena->registerOrGetImage(img);
    const float iw = float(std::max(1, img.width()));
    const float ih = float(std::max(1, img.height()));

    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (!sourceRect.isEmpty()) {
        u0 = float(sourceRect.x()) / iw;
        v0 = float(sourceRect.y()) / ih;
        u1 = float(sourceRect.right()) / iw;
        v1 = float(sourceRect.bottom()) / ih;
    }

    imageSlot(imgSlot, targetRect, clipRadius, QRectF(u0, v0, u1 - u0, v1 - v0), opacity);
}

void QPhaseVisuals::imageSlot(quint32 slot, const QRectF &targetRect,
                              QPhaseCorners clipRadius, const QRectF &uvRect, float opacity)
{
    if (!d_ptr || targetRect.isEmpty() || opacity <= 0.0f)
        return;

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::Image);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.resourceIndex = slot;
    p.x = float(targetRect.x());
    p.y = float(targetRect.y());
    p.w = float(targetRect.width());
    p.h = float(targetRect.height());
    p.c0r = 1.0f;
    p.c0g = 1.0f;
    p.c0b = 1.0f;
    p.c0a = std::clamp(opacity, 0.0f, 1.0f);
    p.rTL = clipRadius.topLeft;
    p.rTR = clipRadius.topRight;
    p.rBR = clipRadius.bottomRight;
    p.rBL = clipRadius.bottomLeft;
    p.p0 = float(uvRect.x());
    p.p1 = float(uvRect.y());
    p.p2 = float(uvRect.x() + uvRect.width());
    p.p3 = float(uvRect.y() + uvRect.height());
    p.animMode = applyLayerFlag(0u, d_ptr);
    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::svgPath(QStringView svgPathData, const QRectF &viewBox,
                            const QRectF &targetRect, const QColor &color)
{
    if (!d_ptr || !d_ptr->arena || svgPathData.isEmpty() || targetRect.isEmpty() || color.alpha() == 0)
        return;

    const uint32_t curveId = d_ptr->arena->registerOrGetSvgPath(svgPathData, viewBox);
    if (curveId == 0)
        return;

    const PhaseCurveEntry &ce = d_ptr->arena->curves()[curveId];
    const float scaleX = float(targetRect.width()) / ce.unitsPerEm;
    const float scaleY = float(targetRect.height()) / ce.unitsPerEm;

    const float quadX = float(targetRect.x()) + ce.xMin * scaleX;
    const float quadY = float(targetRect.y()) + (ce.unitsPerEm - ce.yMax) * scaleY;
    const float quadW = (ce.xMax - ce.xMin) * scaleX;
    const float quadH = (ce.yMax - ce.yMin) * scaleY;
    if (quadW <= 0.0f || quadH <= 0.0f)
        return;

    PhaseGpuPrimitive p{};
    p.kind = uint32_t(PhasePrimKind::HbGpuCurve);
    p.transformIndex = d_ptr->transformIndex;
    p.clipIndex = d_ptr->clipIndex;
    p.resourceIndex = ce.texelOffset;
    p.x = quadX;
    p.y = quadY;
    p.w = quadW;
    p.h = quadH;
    p.c0r = color.redF();
    p.c0g = color.greenF();
    p.c0b = color.blueF();
    p.c0a = color.alphaF();
    p.p0 = ce.xMin;
    p.p1 = ce.yMin;
    p.p2 = ce.xMax;
    p.p3 = ce.yMax;
    p.p4 = ce.unitsPerEm;
    p.p5 = float(curveId);
    p.animMode = applyLayerFlag(0u, d_ptr);
    d_ptr->recordedPrimitives.push_back(p);
}

void QPhaseVisuals::glyphRun(const QGlyphRun &run, QPointF baselineOrigin, const QColor &color)
{
    if (!d_ptr || !d_ptr->arena || color.alpha() == 0)
        return;

    const QRawFont rawFont = run.rawFont();
    if (!rawFont.isValid() || rawFont.pixelSize() <= 0.0)
        return;

    const QList<quint32> glyphIds = run.glyphIndexes();
    const QList<QPointF> positions = run.positions();
    const qsizetype count = std::min(glyphIds.size(), positions.size());
    std::vector<PhaseColorGlyphLayer> colrLayers;

    auto emitCurveGlyph = [&](uint32_t curveId, float baseX, float baseY, const QColor &c) {
        if (curveId == 0)
            return;
        const PhaseCurveEntry &ce = d_ptr->arena->curves()[curveId];
        const float scale = float(rawFont.pixelSize()) / ce.unitsPerEm;
        const float quadX = baseX + ce.xMin * scale;
        const float quadY = baseY - ce.yMax * scale;
        const float quadW = (ce.xMax - ce.xMin) * scale;
        const float quadH = (ce.yMax - ce.yMin) * scale;
        if (quadW <= 0.0f || quadH <= 0.0f)
            return;

        PhaseGpuPrimitive p{};
        p.kind = uint32_t(PhasePrimKind::HbGpuCurve);
        p.transformIndex = d_ptr->transformIndex;
        p.clipIndex = d_ptr->clipIndex;
        p.resourceIndex = ce.texelOffset;
        p.x = quadX;
        p.y = quadY;
        p.w = quadW;
        p.h = quadH;
        p.c0r = c.redF();
        p.c0g = c.greenF();
        p.c0b = c.blueF();
        p.c0a = c.alphaF();
        p.p0 = ce.xMin;
        p.p1 = ce.yMin;
        p.p2 = ce.xMax;
        p.p3 = ce.yMax;
        p.p4 = ce.unitsPerEm;
        p.p5 = float(curveId);
        p.animMode = applyLayerFlag(0u, d_ptr);
        d_ptr->recordedPrimitives.push_back(p);
    };

    for (qsizetype i = 0; i < count; ++i) {
        const float baseX = float(baselineOrigin.x() + positions[i].x());
        const float baseY = float(baselineOrigin.y() + positions[i].y());
        const uint32_t gid = glyphIds[i];

        // 1. OpenType COLRv0 multi-layer vector color glyphs
        colrLayers.clear();
        if (d_ptr->arena->tryGetColorGlyphLayers(rawFont, gid, colrLayers)) {
            for (const PhaseColorGlyphLayer &layer : colrLayers) {
                QColor lc = layer.useForegroundColor ? color : layer.color;
                lc.setAlphaF(lc.alphaF() * color.alphaF());
                emitCurveGlyph(layer.curveId, baseX, baseY, lc);
            }
            continue;
        }

        // 2. CBDT / SBIX / COLRv1 color bitmap emoji glyphs -> bindless RGBA texture slot!
        const PhaseBitmapGlyphEntry bmp = d_ptr->arena->tryRegisterBitmapColorGlyph(rawFont, gid);
        if (bmp.valid) {
            PhaseGpuPrimitive p{};
            p.kind = uint32_t(PhasePrimKind::Image);
            p.transformIndex = d_ptr->transformIndex;
            p.clipIndex = d_ptr->clipIndex;
            p.resourceIndex = bmp.imageSlot;
            p.x = baseX + float(bmp.pixelBounds.x());
            p.y = baseY + float(bmp.pixelBounds.y());
            p.w = float(bmp.pixelBounds.width());
            p.h = float(bmp.pixelBounds.height());
            p.c0r = 1.0f;
            p.c0g = 1.0f;
            p.c0b = 1.0f;
            p.c0a = color.alphaF();
            p.p0 = 0.0f;
            p.p1 = 0.0f;
            p.p2 = 1.0f;
            p.p3 = 1.0f;
            p.animMode = applyLayerFlag(0u, d_ptr);
            d_ptr->recordedPrimitives.push_back(p);
            continue;
        }

        // 3. Standard monochrome outline glyph -> unscaled HarfBuzz 15 hb-gpu curve!
        const uint32_t curveId = d_ptr->arena->registerOrGetGlyph(rawFont, gid);
        emitCurveGlyph(curveId, baseX, baseY, color);
    }
}

void QPhaseVisuals::text(QStringView str, const QFont &font, const QRectF &bounds,
                         Qt::Alignment alignment, const QColor &color)
{
    if (!d_ptr || !d_ptr->arena || str.isEmpty() || bounds.isEmpty() || color.alpha() == 0)
        return;

    QFont unhinted = font;
    unhinted.setHintingPreference(QFont::PreferNoHinting);

    QTextOption opt(alignment);
    opt.setWrapMode((alignment & Qt::TextWordWrap) ? QTextOption::WordWrap : QTextOption::NoWrap);

    QTextLayout layout(str.toString(), unhinted);
    layout.setTextOption(opt);
    layout.beginLayout();
    qreal y = 0.0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(bounds.width());
        line.setPosition(QPointF(0.0, y));
        y += line.height();
    }
    layout.endLayout();

    qreal offsetY = bounds.y();
    if (alignment & Qt::AlignVCenter)
        offsetY += (bounds.height() - y) * 0.5;
    else if (alignment & Qt::AlignBottom)
        offsetY += (bounds.height() - y);

    const QList<QGlyphRun> runs = layout.glyphRuns();
    for (const QGlyphRun &r : runs)
        glyphRun(r, QPointF(bounds.x(), offsetY), color);
}

QT_END_NAMESPACE
