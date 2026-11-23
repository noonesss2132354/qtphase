// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEVISUALS_H
#define QPHASEVISUALS_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphasestyles.h>
#include <QtCore/qlist.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qrect.h>
#include <QtCore/qstringview.h>
#include <QtGui/qcolor.h>
#include <QtGui/qfont.h>
#include <QtGui/qglyphrun.h>
#include <QtGui/qimage.h>

QT_BEGIN_NAMESPACE

class QPhaseVisualsPrivate;

class Q_PHASE_EXPORT QPhaseVisuals
{
public:
    Q_DISABLE_COPY_MOVE(QPhaseVisuals)

    // Stage 1: Solid rectangle (supports arbitrary 2x2 view rotation/skew!)
    void solidRect(const QRectF &rect, const QColor &color);

    // Stage 2: Analytical SDF rounded box (fill / linear gradient / border / erf drop shadow / GPU animation)
    void roundedBox(const QRectF &rect, const QPhaseBoxStyle &style, const QPhaseAnimation &anim = {});

    // Analytical anti-aliased line segment & polyline stroke (zero CPU tessellation / curve encoding!)
    void strokeLine(QPointF p0, QPointF p1, float width, const QColor &color, bool buttCap = false);
    void polyline(const QList<QPointF> &points, float width, const QColor &color, bool closed = false);

    // Isolated in-register compositing layer (exact group opacity & Normal/Multiply/Screen/Additive blend modes!)
    void pushLayer(const QRectF &bounds, float opacity = 1.0f, QPhaseBlendMode blendMode = QPhaseBlendMode::Normal);
    void popLayer(const QRectF &bounds, float opacity = 1.0f, QPhaseBlendMode blendMode = QPhaseBlendMode::Normal);

    // Stage 3: Bindless retained image & custom GPU surface slot
    void image(const QImage &img, const QRectF &targetRect,
               QPhaseCorners clipRadius = {}, const QRectF &sourceRect = {}, float opacity = 1.0f);
    void imageSlot(quint32 slot, const QRectF &targetRect,
                   QPhaseCorners clipRadius = {}, const QRectF &uvRect = QRectF(0.0, 0.0, 1.0, 1.0),
                   float opacity = 1.0f);

    // Stage 4: HarfBuzz 15 hb-gpu / hb-raster vector, outline text, COLRv0 & color emoji primitives
    void svgPath(QStringView svgPathData, const QRectF &viewBox, const QRectF &targetRect, const QColor &color);
    void glyphRun(const QGlyphRun &run, QPointF baselineOrigin, const QColor &color);
    void text(QStringView str, const QFont &font, const QRectF &bounds,
              Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter,
              const QColor &color = Qt::white);

private:
    friend class QPhaseView;
    friend class QPhaseViewPrivate;
    friend class QPhasePaintEngine;
    explicit QPhaseVisuals(QPhaseVisualsPrivate *d);
    QPhaseVisualsPrivate *d_ptr = nullptr;
};

QT_END_NAMESPACE

#endif // QPHASEVISUALS_H
