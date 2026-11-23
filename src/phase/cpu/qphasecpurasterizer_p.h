// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASECPURASTERIZER_P_H
#define QPHASECPURASTERIZER_P_H

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
#include <QtCore/qsize.h>
#include <QtGui/qimage.h>
#include <QtGui/qregion.h>

#include <atomic>
#include <cstdint>
#include <memory>

QT_BEGIN_NAMESPACE

class QPhaseSceneArena;
class QPhaseCpuTilePool;

class Q_PHASE_EXPORT QPhaseCpuRasterizer
{
public:
    QPhaseCpuRasterizer();
    ~QPhaseCpuRasterizer();

    Q_DISABLE_COPY_MOVE(QPhaseCpuRasterizer)

    // Hardware-adaptive tile thread count selected for the current machine (1..16)
    uint32_t maxTileThreadCount() const noexcept;

    // Returns false if aborted mid-frame because a competing worker already finished jobSeq.
    // When damageRegion is non-null, only 16x16 tiles intersecting *damageRegion are recomputed!
    bool renderToBuffer(QPhaseSceneArena &arena,
                        uchar *dstPixels,
                        int width,
                        int height,
                        int bytesPerLine,
                        qreal devicePixelRatio,
                        bool forceOpaqueAlpha,
                        uint64_t frameTimeNs = 0,
                        const std::atomic<uint64_t> *abortUpToSeq = nullptr,
                        uint64_t jobSeq = 0,
                        const QRegion *damageRegion = nullptr);

    QImage renderToImage(QPhaseSceneArena &arena,
                         const QSize &pixelSize,
                         qreal devicePixelRatio = 1.0,
                         uint64_t frameTimeNs = 0);

private:
    std::unique_ptr<QPhaseCpuTilePool> m_tilePool;
};

QT_END_NAMESPACE

#endif // QPHASECPURASTERIZER_P_H
