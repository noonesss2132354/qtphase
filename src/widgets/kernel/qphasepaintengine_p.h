// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEPAINTENGINE_P_H
#define QPHASEPAINTENGINE_P_H

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
#include <QtGui/qpaintdevice.h>
#include <QtGui/qpaintengine.h>
#include <QtGui/qtransform.h>
#include <QtCore/qhash.h>

#include <cstdint>
#include <functional>
#include <vector>

QT_BEGIN_NAMESPACE

class QWidget;
class QPhaseSceneArena;
struct PhaseGpuPrimitive;

// Per-arena slot-ref-counted image/pixmap & complex-path raster cache shared across all widgets in a window
struct QPhaseWidgetImageCache {
    struct CachedImageSlot {
        uint32_t slot = UINT32_MAX;
        uint64_t generation = 0;
    };

    uint64_t epoch = 1;
    QHash<qint64, CachedImageSlot> pixmapSlots;
    QHash<qint64, CachedImageSlot> imageSlots;
    QHash<uint64_t, CachedImageSlot> rasterFallbackSlots;
    QHash<uint32_t, uint32_t> slotRefCount;

    uint32_t acquireImage(QPhaseSceneArena &arena, const QImage &img);
    uint32_t acquirePixmap(QPhaseSceneArena &arena, const QPixmap &pm);
    uint32_t acquireHashedRaster(QPhaseSceneArena &arena, uint64_t contentHash,
                                 const QImage &premulRgba);
    void releaseImageSlots(QPhaseSceneArena &arena, uint64_t ownerEpoch,
                           const std::vector<uint32_t> &slotsToRelease);
    void sweepDeadEntries(const QPhaseSceneArena &arena);
    void clear(QPhaseSceneArena &arena);
};

// Per-paint telemetry emitted by QPhasePaintEngine so QPhaseWidgetBridge can
// adaptively route complex or ultra-dense widgets to direct QRasterPaintEngine!
struct QPhaseWidgetPaintStats {
    uint32_t fallbackCount = 0;           // Cache-miss emitRasterFallback() calls this paint
    uint64_t fallbackPixels = 0;          // Total physical pixels rasterized in fallback misses
    uint32_t glyphPrimCount = 0;          // Total glyph primitives emitted this paint
    bool usedUnsupportedCompMode = false; // QPainter::CompositionMode != SourceOver
    bool usedComplexClipPath = false;     // Non-rectangular curved/polygon clip path
};

class QPhasePaintEngine final : public QPaintEngine
{
public:
    QPhasePaintEngine(QPhaseSceneArena &arena,
                      QPhaseWidgetImageCache &imageCache,
                      uint32_t baseTransformSlot,
                      uint32_t baseClipSlot,
                      qreal devicePixelRatio,
                      std::vector<PhaseGpuPrimitive> &outPrimitives,
                      std::vector<uint32_t> &outTransientTransforms,
                      std::vector<uint32_t> &outTransientClips,
                      std::vector<uint32_t> &outOwnedImageSlots,
                      std::vector<QRectF> *outPrimLocalBounds = nullptr,
                      QPhaseWidgetPaintStats *outStats = nullptr);
    ~QPhasePaintEngine() override = default;

    bool begin(QPaintDevice *pdev) override;
    bool end() override;

    void updateState(const QPaintEngineState &state) override;

    void drawRects(const QRect *rects, int rectCount) override;
    void drawRects(const QRectF *rects, int rectCount) override;
    void drawLines(const QLine *lines, int lineCount) override;
    void drawLines(const QLineF *lines, int lineCount) override;
    void drawEllipse(const QRectF &r) override;
    void drawEllipse(const QRect &r) override;
    void drawPath(const QPainterPath &path) override;
    void drawPoints(const QPointF *points, int pointCount) override;
    void drawPoints(const QPoint *points, int pointCount) override;
    void drawPolygon(const QPointF *points, int pointCount, PolygonDrawMode mode) override;
    void drawPolygon(const QPoint *points, int pointCount, PolygonDrawMode mode) override;
    void drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &sr) override;
    void drawTextItem(const QPointF &p, const QTextItem &textItem) override;
    void drawTiledPixmap(const QRectF &r, const QPixmap &pixmap, const QPointF &s) override;
    void drawImage(const QRectF &r, const QImage &pm, const QRectF &sr,
                   Qt::ImageConversionFlags flags = Qt::AutoColor) override;

    Type type() const override { return User; }

private:
    uint32_t ensureCurrentTransformSlot();
    const std::vector<uint32_t> &ensureCurrentClipSlots();
    void pushPrimitiveClipped(const PhaseGpuPrimitive &prim);
    void emitRasterFallback(const QRectF &localBounds,
                            uint64_t extraHash,
                            const std::function<void(QPainter &)> &drawFn);
    void emitRectOrRounded(const QRectF &r, float rx, float ry);
    bool tryEmitFastPath(const QPainterPath &path);

    QPhaseSceneArena &m_arena;
    QPhaseWidgetImageCache &m_imageCache;
    uint32_t m_baseTransformSlot = 0;
    uint32_t m_baseClipSlot = 0;
    qreal m_dpr = 1.0;

    std::vector<PhaseGpuPrimitive> &m_outPrimitives;
    std::vector<uint32_t> &m_outTransientTransforms;
    std::vector<uint32_t> &m_outTransientClips;
    std::vector<uint32_t> &m_outOwnedImageSlots;
    std::vector<QRectF> *m_outPrimLocalBounds = nullptr;
    QPhaseWidgetPaintStats *m_outStats = nullptr;

    QTransform m_transform;
    QPen m_pen{Qt::black};
    QBrush m_brush{Qt::NoBrush};
    qreal m_opacity = 1.0;
    bool m_antialiasing = false;
    QPainter::CompositionMode m_compMode = QPainter::CompositionMode_SourceOver;
    bool m_clipEnabled = false;
    QRegion m_clipRegion;

    // Cached slots for consecutive draws sharing identical transform/clip
    bool m_transformSlotValid = true;
    uint32_t m_activeTransformSlot = 0;
    bool m_clipSlotValid = true;
    std::vector<uint32_t> m_activeClipSlots;
};

class QPhasePaintDevice final : public QPaintDevice
{
public:
    QPhasePaintDevice(const QWidget *widget, QPhasePaintEngine *engine);
    ~QPhasePaintDevice() override = default;

    QPaintEngine *paintEngine() const override { return m_engine; }

protected:
    int metric(PaintDeviceMetric metric) const override;

private:
    const QWidget *m_widget = nullptr;
    QPhasePaintEngine *m_engine = nullptr;
};

QT_END_NAMESPACE

#endif // QPHASEPAINTENGINE_P_H
