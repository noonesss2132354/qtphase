// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasecpurasterizer_p.h"
#include "qphasesharedmath_p.h"
#include "../qphasescenearena_p.h"

#include <harfbuzz/hb-raster.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE

namespace {

static uint32_t computeAdaptiveBootTileThreads() noexcept
{
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    if (hw <= 2u)
        return 1u;
    return std::min(hw - 2u, 16u);
}

struct RasterizedMask {
    int originX = 0;
    int originY = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> alpha;
};

struct GlyphSubpixelCacheKey {
    uint64_t arenaId = 0;
    uint32_t curveId = 0;
    int32_t m00Fixed = 0;
    int32_t m01Fixed = 0;
    int32_t m10Fixed = 0;
    int32_t m11Fixed = 0;
    uint8_t subX64 = 0;
    uint8_t subY64 = 0;

    bool operator==(const GlyphSubpixelCacheKey &o) const noexcept
    {
        return arenaId == o.arenaId && curveId == o.curveId
                && m00Fixed == o.m00Fixed && m01Fixed == o.m01Fixed
                && m10Fixed == o.m10Fixed && m11Fixed == o.m11Fixed
                && subX64 == o.subX64 && subY64 == o.subY64;
    }
};

struct GlyphSubpixelCacheHash {
    size_t operator()(const GlyphSubpixelCacheKey &k) const noexcept
    {
        uint64_t h = k.arenaId * 0x517CC1B727220A95ull;
        h ^= uint64_t(k.curveId) * 0x9E3779B97F4A7C15ull;
        h ^= (uint64_t(uint32_t(k.m00Fixed)) << 32) | uint64_t(uint32_t(k.m11Fixed));
        h ^= (uint64_t(uint32_t(k.m01Fixed)) << 32) | uint64_t(uint32_t(k.m10Fixed));
        h ^= (uint64_t(k.subX64) << 8) | uint64_t(k.subY64);
        return size_t(h);
    }
};

struct CachedSubpixelGlyph {
    int relOriginX = 0;
    int relOriginY = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> alpha;
};

struct PreparedDrawOp {
    const PhaseGpuPrimitive *prim = nullptr;
    const PhaseResolvedTransform *tx = nullptr;
    float devMinX = 0.0f, devMinY = 0.0f, devMaxX = 0.0f, devMaxY = 0.0f;
    float pxScale = 1.0f;
    float invDet = 0.0f;
    float rTL = 0.0f, rTR = 0.0f, rBR = 0.0f, rBL = 0.0f;
    float animOp = 1.0f;
    float baseOpacity = 1.0f;
    int px0 = 0, py0 = 0, px1 = 0, py1 = 0;
    int maskIndex = -1;
    bool isRotated = false;

    bool isOpaqueSolidFill = false;
    uint32_t opaqueSolidPacked = 0;
    float opaqueInMinX = 0.0f, opaqueInMinY = 0.0f, opaqueInMaxX = 0.0f, opaqueInMaxY = 0.0f;
};

inline void sampleBilinearPremulRgba8888(const QImage &img, float u, float v,
                                         float &outR, float &outG, float &outB, float &outA) noexcept
{
    const int w = img.width();
    const int h = img.height();
    const float txc = std::clamp(u * float(w) - 0.5f, 0.0f, float(w - 1));
    const float tyc = std::clamp(v * float(h) - 0.5f, 0.0f, float(h - 1));

    const int ix0 = int(std::floor(txc));
    const int iy0 = int(std::floor(tyc));
    const int ix1 = std::min(ix0 + 1, w - 1);
    const int iy1 = std::min(iy0 + 1, h - 1);
    const float fx = txc - float(ix0);
    const float fy = tyc - float(iy0);

    const uchar *row0 = img.constScanLine(iy0);
    const uchar *row1 = img.constScanLine(iy1);
    const uchar *p00 = row0 + ix0 * 4;
    const uchar *p10 = row0 + ix1 * 4;
    const uchar *p01 = row1 + ix0 * 4;
    const uchar *p11 = row1 + ix1 * 4;

    const float w00 = (1.0f - fx) * (1.0f - fy) * (1.0f / 255.0f);
    const float w10 = fx * (1.0f - fy) * (1.0f / 255.0f);
    const float w01 = (1.0f - fx) * fy * (1.0f / 255.0f);
    const float w11 = fx * fy * (1.0f / 255.0f);

    outR = float(p00[0]) * w00 + float(p10[0]) * w10 + float(p01[0]) * w01 + float(p11[0]) * w11;
    outG = float(p00[1]) * w00 + float(p10[1]) * w10 + float(p01[1]) * w01 + float(p11[1]) * w11;
    outB = float(p00[2]) * w00 + float(p10[2]) * w10 + float(p01[2]) * w01 + float(p11[2]) * w11;
    outA = float(p00[3]) * w00 + float(p10[3]) * w10 + float(p01[3]) * w01 + float(p11[3]) * w11;
}

inline float evalCpuClipChainCoverage(float fragX, float fragY,
                                      uint32_t clipIdx,
                                      const std::vector<PhaseResolvedClip> &clips,
                                      float dpr) noexcept
{
    if (clipIdx == 0u || clipIdx >= clips.size())
        return 1.0f;
    const PhaseResolvedClip &cl = clips[clipIdx];
    float cov = QtPhaseMath::boxPixelCoverage(
            fragX, fragY,
            cl.cullMinX * dpr, cl.cullMinY * dpr,
            cl.cullMaxX * dpr, cl.cullMaxY * dpr);
    if (cov <= 0.0f || cl.hasRoundedAncestorOrSelf == 0u)
        return cov;

    uint32_t cur = clipIdx;
    for (int hop = 0; hop < 4 && cur != 0u && cur < clips.size() && cov > 0.0f; ++hop) {
        const PhaseResolvedClip &rc = clips[cur];
        const float rTL = rc.rTL * dpr;
        const float rTR = rc.rTR * dpr;
        const float rBR = rc.rBR * dpr;
        const float rBL = rc.rBL * dpr;
        if (rTL > 0.0f || rTR > 0.0f || rBR > 0.0f || rBL > 0.0f) {
            const float d = QtPhaseMath::sdRoundedBox(
                    fragX, fragY,
                    rc.selfMinX * dpr, rc.selfMinY * dpr,
                    rc.selfMaxX * dpr, rc.selfMaxY * dpr,
                    rTL, rTR, rBR, rBL);
            cov *= std::clamp(0.5f - d, 0.0f, 1.0f);
        }
        cur = rc.parentRoundedClipIndex;
    }
    return cov;
}

} // namespace

class QPhaseCpuTilePool
{
public:
    QPhaseCpuTilePool()
        : m_maxTotalThreads(computeAdaptiveBootTileThreads())
    {
        const uint32_t extra = m_maxTotalThreads > 1u ? (m_maxTotalThreads - 1u) : 0u;
        m_workers.reserve(extra);
        for (uint32_t i = 0; i < extra; ++i)
            m_workers.emplace_back([this, i]() { workerLoop(i); });
    }

    ~QPhaseCpuTilePool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_generation;
        }
        m_startCv.notify_all();
        for (std::thread &t : m_workers) {
            if (t.joinable())
                t.join();
        }
    }

    uint32_t maxTotalThreads() const noexcept { return m_maxTotalThreads; }

    template <typename TileFn>
    bool parallelForTiles(uint32_t totalTiles,
                          const std::atomic<uint64_t> *abortUpToSeq,
                          uint64_t jobSeq,
                          TileFn &&fn)
    {
        if (totalTiles == 0)
            return true;

        auto isAborted = [abortUpToSeq, jobSeq]() noexcept -> bool {
            return abortUpToSeq && jobSeq > 0
                    && abortUpToSeq->load(std::memory_order_relaxed) >= jobSeq;
        };

        if (isAborted())
            return false;

        static constexpr uint32_t kMinTilesPerThread = 24u;
        const uint32_t desiredThreads = std::clamp(totalTiles / kMinTilesPerThread, 1u, m_maxTotalThreads);
        if (desiredThreads <= 1u || m_workers.empty()) {
            for (uint32_t idx = 0; idx < totalTiles; ++idx) {
                if ((idx & 7u) == 0u && isAborted())
                    return false;
                fn(idx);
            }
            return !isAborted();
        }

        const uint32_t activeHelpers = std::min<uint32_t>(desiredThreads - 1u, uint32_t(m_workers.size()));
        m_nextTile.store(0u, std::memory_order_relaxed);
        m_totalTiles = totalTiles;
        std::atomic<bool> canceled{false};

        auto drainChunks = [&]() {
            static constexpr uint32_t kChunk = 8u;
            while (true) {
                if (isAborted()) {
                    canceled.store(true, std::memory_order_relaxed);
                    break;
                }
                const uint32_t base = m_nextTile.fetch_add(kChunk, std::memory_order_relaxed);
                if (base >= totalTiles)
                    break;
                const uint32_t end = std::min(base + kChunk, totalTiles);
                for (uint32_t idx = base; idx < end; ++idx)
                    fn(idx);
            }
        };

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_taskFn = drainChunks;
            m_activeHelperLimit = activeHelpers;
            m_pendingHelpers = activeHelpers;
            ++m_generation;
        }
        m_startCv.notify_all();

        drainChunks();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_doneCv.wait(lock, [this]() { return m_pendingHelpers == 0u; });
        m_taskFn = nullptr;
        return !canceled.load(std::memory_order_relaxed) && !isAborted();
    }

    std::unordered_map<GlyphSubpixelCacheKey, CachedSubpixelGlyph, GlyphSubpixelCacheHash> glyphCache;

private:
    void workerLoop(uint32_t workerIdx)
    {
        uint64_t seenGen = 0;
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_startCv.wait(lock, [this, &seenGen]() { return m_stop || m_generation != seenGen; });
                if (m_stop)
                    return;
                seenGen = m_generation;
                if (workerIdx >= m_activeHelperLimit)
                    continue;
                fn = m_taskFn;
            }

            if (fn)
                fn();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_pendingHelpers == 0u)
                    m_doneCv.notify_one();
            }
        }
    }

    uint32_t m_maxTotalThreads = 1;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_startCv;
    std::condition_variable m_doneCv;
    uint64_t m_generation = 0;
    uint32_t m_activeHelperLimit = 0;
    uint32_t m_pendingHelpers = 0;
    uint32_t m_totalTiles = 0;
    std::atomic<uint32_t> m_nextTile{0};
    std::function<void()> m_taskFn;
    bool m_stop = false;
};

QPhaseCpuRasterizer::QPhaseCpuRasterizer()
    : m_tilePool(std::make_unique<QPhaseCpuTilePool>())
{
}

QPhaseCpuRasterizer::~QPhaseCpuRasterizer() = default;

uint32_t QPhaseCpuRasterizer::maxTileThreadCount() const noexcept
{
    return m_tilePool ? m_tilePool->maxTotalThreads() : 1u;
}

bool QPhaseCpuRasterizer::renderToBuffer(QPhaseSceneArena &arena,
                                         uchar *dstPixels,
                                         int width,
                                         int height,
                                         int bytesPerLine,
                                         qreal devicePixelRatio,
                                         bool forceOpaqueAlpha,
                                         uint64_t frameTimeNs,
                                         const std::atomic<uint64_t> *abortUpToSeq,
                                         uint64_t jobSeq,
                                         const QRegion *damageRegion)
{
    if (!dstPixels || width <= 0 || height <= 0 || !m_tilePool)
        return false;

    if (damageRegion && damageRegion->isEmpty())
        return true;

    const bool hasPartialDamage = (damageRegion != nullptr);
    const QRect dmgBounds = hasPartialDamage
            ? (damageRegion->boundingRect() & QRect(0, 0, width, height))
            : QRect(0, 0, width, height);
    if (dmgBounds.isEmpty())
        return true;
    const int dmgX0 = (dmgBounds.x() / 16) * 16;
    const int dmgY0 = (dmgBounds.y() / 16) * 16;
    const int dmgX1 = std::min(width, ((dmgBounds.x() + dmgBounds.width() + 15) / 16) * 16);
    const int dmgY1 = std::min(height, ((dmgBounds.y() + dmgBounds.height() + 15) / 16) * 16);

    auto isAborted = [abortUpToSeq, jobSeq]() noexcept -> bool {
        return abortUpToSeq && jobSeq > 0
                && abortUpToSeq->load(std::memory_order_relaxed) >= jobSeq;
    };
    if (isAborted())
        return false;

    arena.resolveHierarchy();

    const float dpr = float(devicePixelRatio);
    const float stemDarkening = arena.textStemDarkening();
    const auto &transforms = arena.resolvedTransforms();
    const auto &clips = arena.resolvedClips();
    const auto &prims = arena.primitives();
    const auto &order = arena.drawOrder();
    const auto &images = arena.images();
    const auto &curves = arena.curves();
    auto &bootGlyphCache = m_tilePool->glyphCache;

    hb_raster_draw_t *hbRaster = nullptr;
    std::vector<RasterizedMask> frameMasks;
    std::vector<PreparedDrawOp> ops;
    ops.reserve(order.size());

    auto txPointDev = [dpr](const PhaseResolvedTransform &tx, float lx, float ly, float &ox, float &oy) noexcept {
        ox = (tx.m00 * lx + tx.m01 * ly + tx.tx) * dpr;
        oy = (tx.m10 * lx + tx.m11 * ly + tx.ty) * dpr;
    };

    size_t prepCounter = 0;
    for (uint32_t idx : order) {
        if ((++prepCounter & 31u) == 0u && isAborted()) {
            if (hbRaster)
                hb_raster_draw_destroy(hbRaster);
            return false;
        }
        if (idx >= prims.size())
            continue;
        const PhaseGpuPrimitive &p = prims[idx];
        if ((p.animMode & kPhaseAnimFlagGpuOnly) != 0u)
            continue;
        const PhaseResolvedTransform &tx = transforms[p.transformIndex];
        const PhaseResolvedClip &cl = clips[p.clipIndex];

        const float animOp = QtPhaseMath::evalAnimationOpacity(
                p.animMode, p.animDurationMs, p.animStartNs, p.animFromOpacity, p.animToOpacity, frameTimeNs);
        if (tx.opacity * animOp <= 0.0f)
            continue;

        PreparedDrawOp op{};
        op.prim = &p;
        op.tx = &tx;
        op.animOp = animOp;
        op.baseOpacity = tx.opacity * animOp;
        op.isRotated = (tx.flags & 1u) != 0u;

        const float det = tx.m00 * tx.m11 - tx.m01 * tx.m10;
        if (std::abs(det) <= 1e-8f)
            continue;
        op.invDet = 1.0f / det;
        op.pxScale = op.isRotated ? (std::sqrt(std::abs(det)) * dpr) : (std::abs(tx.m00) * dpr);

        if (p.kind == uint32_t(PhasePrimKind::StrokeSegment)) {
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            txPointDev(tx, p.x, p.y, ax, ay);
            txPointDev(tx, p.w, p.h, bx, by);
            const float halfW = 0.5f * p.p0 * op.pxScale;
            op.devMinX = std::min(ax, bx) - halfW;
            op.devMinY = std::min(ay, by) - halfW;
            op.devMaxX = std::max(ax, bx) + halfW;
            op.devMaxY = std::max(ay, by) + halfW;
        } else if (!op.isRotated) {
            const float x0 = (p.x * tx.m00 + tx.tx) * dpr;
            const float y0 = (p.y * tx.m11 + tx.ty) * dpr;
            const float x1 = ((p.x + p.w) * tx.m00 + tx.tx) * dpr;
            const float y1 = ((p.y + p.h) * tx.m11 + tx.ty) * dpr;
            op.devMinX = std::min(x0, x1);
            op.devMaxX = std::max(x0, x1);
            op.devMinY = std::min(y0, y1);
            op.devMaxY = std::max(y0, y1);
        } else {
            float cx[4], cy[4];
            txPointDev(tx, p.x,       p.y,       cx[0], cy[0]);
            txPointDev(tx, p.x + p.w, p.y,       cx[1], cy[1]);
            txPointDev(tx, p.x,       p.y + p.h, cx[2], cy[2]);
            txPointDev(tx, p.x + p.w, p.y + p.h, cx[3], cy[3]);
            op.devMinX = std::min({cx[0], cx[1], cx[2], cx[3]});
            op.devMaxX = std::max({cx[0], cx[1], cx[2], cx[3]});
            op.devMinY = std::min({cy[0], cy[1], cy[2], cy[3]});
            op.devMaxY = std::max({cy[0], cy[1], cy[2], cy[3]});
        }

        op.rTL = p.rTL * op.pxScale;
        op.rTR = p.rTR * op.pxScale;
        op.rBR = p.rBR * op.pxScale;
        op.rBL = p.rBL * op.pxScale;

        const float clipMinX = cl.cullMinX * dpr;
        const float clipMinY = cl.cullMinY * dpr;
        const float clipMaxX = cl.cullMaxX * dpr;
        const float clipMaxY = cl.cullMaxY * dpr;

        float expandPx = 1.0f;
        if (p.kind == uint32_t(PhasePrimKind::BoxShadow)) {
            const float blurDev = p.p0 * dpr;
            const float spreadDev = p.p1 * dpr;
            expandPx = std::max(1.0f, std::ceil(blurDev * 2.5f + std::max(spreadDev, 0.0f) + 2.0f));
        }

        const float effMinX = std::max(std::floor(op.devMinX - expandPx), std::floor(clipMinX));
        const float effMinY = std::max(std::floor(op.devMinY - expandPx), std::floor(clipMinY));
        const float effMaxX = std::min(std::ceil(op.devMaxX + expandPx), std::ceil(clipMaxX));
        const float effMaxY = std::min(std::ceil(op.devMaxY + expandPx), std::ceil(clipMaxY));

        op.px0 = std::max(0, int(effMinX));
        op.py0 = std::max(0, int(effMinY));
        op.px1 = std::min(width, int(effMaxX));
        op.py1 = std::min(height, int(effMaxY));

        if (op.px0 >= op.px1 || op.py0 >= op.py1)
            continue;
        if (hasPartialDamage && (op.px1 <= dmgX0 || op.px0 >= dmgX1 || op.py1 <= dmgY0 || op.py0 >= dmgY1))
            continue;

        const bool insideLayer = (p.animMode & kPhaseAnimFlagInsideLayer) != 0u;
        if (!insideLayer && !op.isRotated && op.baseOpacity >= 0.9999f && p.c0a >= 0.9999f
            && (p.kind == uint32_t(PhasePrimKind::SolidRect)
                || (p.kind == uint32_t(PhasePrimKind::RoundedBox) && p.p1 < 0.5f))) {
            const float borderInset = (p.kind == uint32_t(PhasePrimKind::RoundedBox)) ? (p.p0 * op.pxScale) : 0.0f;
            const float maxR = std::max({op.rTL, op.rTR, op.rBR, op.rBL, borderInset}) + 1.0f;
            float maxClipR = 1.0f;
            if (cl.hasRoundedAncestorOrSelf != 0u) {
                uint32_t curCl = p.clipIndex;
                for (int hop = 0; hop < 4 && curCl != 0u && curCl < clips.size(); ++hop) {
                    const PhaseResolvedClip &rc = clips[curCl];
                    maxClipR = std::max(maxClipR, std::max({rc.rTL, rc.rTR, rc.rBR, rc.rBL}) * dpr + 1.0f);
                    curCl = rc.parentRoundedClipIndex;
                }
            }
            op.opaqueInMinX = std::max(op.devMinX + maxR, clipMinX + maxClipR);
            op.opaqueInMinY = std::max(op.devMinY + maxR, clipMinY + maxClipR);
            op.opaqueInMaxX = std::min(op.devMaxX - maxR, clipMaxX - maxClipR);
            op.opaqueInMaxY = std::min(op.devMaxY - maxR, clipMaxY - maxClipR);
            if (op.opaqueInMinX < op.opaqueInMaxX && op.opaqueInMinY < op.opaqueInMaxY) {
                op.isOpaqueSolidFill = true;
                const uint8_t r = QtPhaseMath::unormFloatToByte(p.c0r);
                const uint8_t g = QtPhaseMath::unormFloatToByte(p.c0g);
                const uint8_t b = QtPhaseMath::unormFloatToByte(p.c0b);
                op.opaqueSolidPacked = (0xFFu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
            }
        }

        if (p.kind == uint32_t(PhasePrimKind::HbGpuCurve)) {
            const uint32_t curveId = uint32_t(p.p5 + 0.5f);
            if (curveId == 0 || curveId >= curves.size())
                continue;
            const PhaseCurveEntry &ce = curves[curveId];

            const float designW = std::max(ce.xMax - ce.xMin, 1e-4f);
            const float designH = std::max(ce.yMax - ce.yMin, 1e-4f);
            // Local glyph quad maps design (dx, dy) -> local (p.x + (dx - xMin)*(p.w/designW), p.y + (yMax - dy)*(p.h/designH))
            const float locScaleX = p.w / designW;
            const float locScaleY = -p.h / designH;
            const float locOffX = p.x - ce.xMin * locScaleX;
            const float locOffY = p.y - ce.yMax * locScaleY;

            const float xx = tx.m00 * locScaleX * dpr;
            const float xy = tx.m01 * locScaleY * dpr;
            const float yx = tx.m10 * locScaleX * dpr;
            const float yy = tx.m11 * locScaleY * dpr;
            const float originDevX = (tx.m00 * locOffX + tx.m01 * locOffY + tx.tx) * dpr;
            const float originDevY = (tx.m10 * locOffX + tx.m11 * locOffY + tx.ty) * dpr;

            const int intOrigX = int(std::floor(originDevX));
            const int intOrigY = int(std::floor(originDevY));
            const float fracOrigX = originDevX - float(intOrigX);
            const float fracOrigY = originDevY - float(intOrigY);

            const GlyphSubpixelCacheKey cacheKey{
                arena.arenaId(),
                curveId,
                int32_t(std::lround(xx * 4096.0f)),
                int32_t(std::lround(xy * 4096.0f)),
                int32_t(std::lround(yx * 4096.0f)),
                int32_t(std::lround(yy * 4096.0f)),
                uint8_t(int(std::lround(fracOrigX * 64.0f)) & 63),
                uint8_t(int(std::lround(fracOrigY * 64.0f)) & 63)
            };

            auto cachedIt = bootGlyphCache.find(cacheKey);
            if (cachedIt == bootGlyphCache.end()) {
                if (!hbRaster)
                    hbRaster = hb_raster_draw_create_or_fail();
                if (!hbRaster)
                    continue;

                hb_raster_draw_clear(hbRaster);
                hb_raster_draw_set_transform(hbRaster, xx, yx, xy, yy, fracOrigX, fracOrigY);

                if (ce.isGlyph && ce.unscaledHbFont) {
                    hb_raster_draw_glyph(hbRaster, ce.unscaledHbFont, ce.glyphId);
                } else if (!ce.cpuVectorCmds.empty()) {
                    hb_draw_funcs_t *df = hb_raster_draw_get_funcs(hbRaster);
                    hb_draw_state_t st = HB_DRAW_STATE_DEFAULT;
                    for (const PhasePathCmd &c : ce.cpuVectorCmds) {
                        switch (c.op) {
                        case PhasePathCmd::Op::MoveTo:
                            hb_draw_move_to(df, hbRaster, &st, c.x0, c.y0);
                            break;
                        case PhasePathCmd::Op::LineTo:
                            hb_draw_line_to(df, hbRaster, &st, c.x0, c.y0);
                            break;
                        case PhasePathCmd::Op::QuadTo:
                            hb_draw_quadratic_to(df, hbRaster, &st, c.x0, c.y0, c.x1, c.y1);
                            break;
                        case PhasePathCmd::Op::CubicTo:
                            hb_draw_cubic_to(df, hbRaster, &st, c.x0, c.y0, c.x1, c.y1, c.x2, c.y2);
                            break;
                        case PhasePathCmd::Op::Close:
                            hb_draw_close_path(df, hbRaster, &st);
                            break;
                        }
                    }
                }

                hb_raster_image_t *rimg = hb_raster_draw_render(hbRaster);
                if (!rimg)
                    continue;

                hb_raster_extents_t rext{};
                hb_raster_image_get_extents(rimg, &rext);
                const uint8_t *srcBuf = hb_raster_image_get_buffer(rimg);

                CachedSubpixelGlyph cg{};
                if (rext.width > 0 && rext.height > 0 && srcBuf) {
                    cg.relOriginX = rext.x_origin;
                    cg.relOriginY = rext.y_origin;
                    cg.width = int(rext.width);
                    cg.height = int(rext.height);
                    cg.alpha.resize(size_t(cg.width) * size_t(cg.height));
                    for (int row = 0; row < cg.height; ++row) {
                        std::memcpy(cg.alpha.data() + size_t(row) * size_t(cg.width),
                                    srcBuf + size_t(row) * rext.stride,
                                    size_t(cg.width));
                    }
                }
                hb_raster_draw_recycle_image(hbRaster, rimg);
                cachedIt = bootGlyphCache.emplace(cacheKey, std::move(cg)).first;
            }

            const CachedSubpixelGlyph &cg = cachedIt->second;
            if (cg.width == 0 || cg.height == 0)
                continue;

            RasterizedMask mask{};
            mask.originX = intOrigX + cg.relOriginX;
            mask.originY = intOrigY + cg.relOriginY;
            mask.width = cg.width;
            mask.height = cg.height;
            mask.alpha = cg.alpha;

            op.px0 = std::max(op.px0, mask.originX);
            op.py0 = std::max(op.py0, mask.originY);
            op.px1 = std::min(op.px1, mask.originX + mask.width);
            op.py1 = std::min(op.py1, mask.originY + mask.height);
            if (op.px0 >= op.px1 || op.py0 >= op.py1)
                continue;

            op.maskIndex = int(frameMasks.size());
            frameMasks.push_back(std::move(mask));
        }

        ops.push_back(op);
    }

    if (hbRaster)
        hb_raster_draw_destroy(hbRaster);

    const QColor clear = arena.clearColor();
    const float clearA = forceOpaqueAlpha ? 1.0f : clear.alphaF();
    const float initR = QtPhaseMath::unormFloatToByte(clear.redF() * clearA) / 255.0f;
    const float initG = QtPhaseMath::unormFloatToByte(clear.greenF() * clearA) / 255.0f;
    const float initB = QtPhaseMath::unormFloatToByte(clear.blueF() * clearA) / 255.0f;
    const float initA = QtPhaseMath::unormFloatToByte(clearA) / 255.0f;

    const uint32_t clearPacked = (uint32_t(QtPhaseMath::unormFloatToByte(initA)) << 24)
            | (uint32_t(QtPhaseMath::unormFloatToByte(initR)) << 16)
            | (uint32_t(QtPhaseMath::unormFloatToByte(initG)) << 8)
            | uint32_t(QtPhaseMath::unormFloatToByte(initB));

    static constexpr int kTileSize = 16;
    const uint32_t tilesX = uint32_t((width + kTileSize - 1) / kTileSize);
    const uint32_t tilesY = uint32_t((height + kTileSize - 1) / kTileSize);
    const uint32_t totalTiles = tilesX * tilesY;

    std::vector<uint32_t> activeTileList;
    if (hasPartialDamage) {
        std::vector<uint8_t> tileVisited(totalTiles, 0);
        activeTileList.reserve(64);
        for (const QRect &dr : *damageRegion) {
            const QRect r = dr & QRect(0, 0, width, height);
            if (r.isEmpty())
                continue;
            const uint32_t tx0 = uint32_t(r.x() / kTileSize);
            const uint32_t tx1 = std::min(tilesX - 1u, uint32_t((r.x() + r.width() - 1) / kTileSize));
            const uint32_t ty0 = uint32_t(r.y() / kTileSize);
            const uint32_t ty1 = std::min(tilesY - 1u, uint32_t((r.y() + r.height() - 1) / kTileSize));
            for (uint32_t ty = ty0; ty <= ty1; ++ty) {
                for (uint32_t tx = tx0; tx <= tx1; ++tx) {
                    const uint32_t lin = ty * tilesX + tx;
                    if (!tileVisited[lin]) {
                        tileVisited[lin] = 1;
                        activeTileList.push_back(lin);
                    }
                }
            }
        }
        if (activeTileList.empty())
            return true;
    }

    const uint32_t dispatchCount = hasPartialDamage ? uint32_t(activeTileList.size()) : totalTiles;

    return m_tilePool->parallelForTiles(dispatchCount, abortUpToSeq, jobSeq, [&](uint32_t workIdx) {
        const uint32_t tileLinearIdx = hasPartialDamage ? activeTileList[workIdx] : workIdx;
        const int tx = int(tileLinearIdx % tilesX) * kTileSize;
        const int ty = int(tileLinearIdx / tilesX) * kTileSize;
        const int tw = std::min(kTileSize, width - tx);
        const int th = std::min(kTileSize, height - ty);
        const int tx1 = tx + tw;
        const int ty1 = ty + th;

        const PreparedDrawOp *localOps[128];
        std::vector<const PreparedDrawOp *> overflowOps;
        size_t opCount = 0;
        int lastFullOccluder = -1;

        for (const PreparedDrawOp &op : ops) {
            if (op.px0 < tx1 && op.px1 > tx && op.py0 < ty1 && op.py1 > ty) {
                if (op.isOpaqueSolidFill
                    && float(tx) >= op.opaqueInMinX && float(tx1) <= op.opaqueInMaxX
                    && float(ty) >= op.opaqueInMinY && float(ty1) <= op.opaqueInMaxY) {
                    lastFullOccluder = int(opCount);
                }
                if (opCount < 128) {
                    localOps[opCount++] = &op;
                } else {
                    if (overflowOps.empty())
                        overflowOps.assign(localOps, localOps + 128);
                    overflowOps.push_back(&op);
                    ++opCount;
                }
            }
        }

        const PreparedDrawOp *const *tileOps = overflowOps.empty() ? localOps : overflowOps.data();

        if (opCount == 0) {
            for (int row = 0; row < th; ++row) {
                auto *dstRow = reinterpret_cast<uint32_t *>(dstPixels + (ty + row) * bytesPerLine) + tx;
                std::fill(dstRow, dstRow + tw, clearPacked);
            }
            return;
        }

        if (lastFullOccluder == int(opCount) - 1) {
            const uint32_t solidPacked = tileOps[lastFullOccluder]->opaqueSolidPacked;
            for (int row = 0; row < th; ++row) {
                auto *dstRow = reinterpret_cast<uint32_t *>(dstPixels + (ty + row) * bytesPerLine) + tx;
                std::fill(dstRow, dstRow + tw, solidPacked);
            }
            return;
        }

        alignas(32) float tileR[kTileSize * kTileSize];
        alignas(32) float tileG[kTileSize * kTileSize];
        alignas(32) float tileB[kTileSize * kTileSize];
        alignas(32) float tileA[kTileSize * kTileSize];

        // 3-deep per-tile layer stack matching phase_uber.frag
        float savedR[3][kTileSize * kTileSize];
        float savedG[3][kTileSize * kTileSize];
        float savedB[3][kTileSize * kTileSize];
        float savedA[3][kTileSize * kTileSize];
        uint8_t pixelLayerDepth[kTileSize * kTileSize]{};

        const int count = tw * th;
        size_t startOpIdx = 0;
        if (lastFullOccluder >= 0) {
            const PhaseGpuPrimitive &occ = *tileOps[lastFullOccluder]->prim;
            const float orF = float(QtPhaseMath::unormFloatToByte(occ.c0r)) * (1.0f / 255.0f);
            const float ogF = float(QtPhaseMath::unormFloatToByte(occ.c0g)) * (1.0f / 255.0f);
            const float obF = float(QtPhaseMath::unormFloatToByte(occ.c0b)) * (1.0f / 255.0f);
            std::fill_n(tileR, count, orF);
            std::fill_n(tileG, count, ogF);
            std::fill_n(tileB, count, obF);
            std::fill_n(tileA, count, 1.0f);
            startOpIdx = size_t(lastFullOccluder + 1);
        } else {
            std::fill_n(tileR, count, initR);
            std::fill_n(tileG, count, initG);
            std::fill_n(tileB, count, initB);
            std::fill_n(tileA, count, initA);
        }

        for (size_t oi = startOpIdx; oi < opCount; ++oi) {
            const PreparedDrawOp &op = *tileOps[oi];
            const PhaseGpuPrimitive &p = *op.prim;
            const PhaseResolvedTransform &ptx = *op.tx;

            const int rx0 = std::max(tx, op.px0) - tx;
            const int ry0 = std::max(ty, op.py0) - ty;
            const int rx1 = std::min(tx1, op.px1) - tx;
            const int ry1 = std::min(ty1, op.py1) - ty;

            if (p.kind == uint32_t(PhasePrimKind::PushLayer)) {
                for (int ly = ry0; ly < ry1; ++ly) {
                    const int rowOff = ly * tw;
                    for (int lx = rx0; lx < rx1; ++lx) {
                        const int idxPx = rowOff + lx;
                        const uint8_t d = pixelLayerDepth[idxPx];
                        if (d < 3u) {
                            savedR[d][idxPx] = tileR[idxPx];
                            savedG[d][idxPx] = tileG[idxPx];
                            savedB[d][idxPx] = tileB[idxPx];
                            savedA[d][idxPx] = tileA[idxPx];
                            pixelLayerDepth[idxPx] = d + 1u;
                            tileR[idxPx] = 0.0f;
                            tileG[idxPx] = 0.0f;
                            tileB[idxPx] = 0.0f;
                            tileA[idxPx] = 0.0f;
                        }
                    }
                }
                continue;
            }

            if (p.kind == uint32_t(PhasePrimKind::PopLayer)) {
                for (int ly = ry0; ly < ry1; ++ly) {
                    const float fragY = float(ty + ly) + 0.5f;
                    const int rowOff = ly * tw;
                    for (int lx = rx0; lx < rx1; ++lx) {
                        const int idxPx = rowOff + lx;
                        if (pixelLayerDepth[idxPx] > 0u) {
                            const uint8_t d = --pixelLayerDepth[idxPx];
                            const float fragX = float(tx + lx) + 0.5f;
                            const float clipCov = evalCpuClipChainCoverage(fragX, fragY, p.clipIndex, clips, dpr);
                            const float grpOp = std::clamp(p.c0a * op.baseOpacity * clipCov, 0.0f, 1.0f);
                            float dstR = savedR[d][idxPx];
                            float dstG = savedG[d][idxPx];
                            float dstB = savedB[d][idxPx];
                            float dstA = savedA[d][idxPx];
                            QtPhaseMath::compositePremulBlendMode(
                                    p.resourceIndex,
                                    tileR[idxPx] * grpOp, tileG[idxPx] * grpOp,
                                    tileB[idxPx] * grpOp, tileA[idxPx] * grpOp,
                                    dstR, dstG, dstB, dstA);
                            tileR[idxPx] = dstR;
                            tileG[idxPx] = dstG;
                            tileB[idxPx] = dstB;
                            tileA[idxPx] = dstA;
                        }
                    }
                }
                continue;
            }

            for (int ly = ry0; ly < ry1; ++ly) {
                const int py = ty + ly;
                const float fragY = float(py) + 0.5f;
                float *rowR = tileR + ly * tw;
                float *rowG = tileG + ly * tw;
                float *rowB = tileB + ly * tw;
                float *rowA = tileA + ly * tw;

                for (int lx = rx0; lx < rx1; ++lx) {
                    const int px = tx + lx;
                    const float fragX = float(px) + 0.5f;

                    const float clipCov = evalCpuClipChainCoverage(fragX, fragY, p.clipIndex, clips, dpr);
                    if (clipCov <= 0.0f)
                        continue;

                    const float effOpacity = op.baseOpacity * clipCov;
                    float srcPremulR = 0.0f, srcPremulG = 0.0f, srcPremulB = 0.0f, srcPremulA = 0.0f;

                    if (p.kind == uint32_t(PhasePrimKind::StrokeSegment)) {
                        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                        txPointDev(ptx, p.x, p.y, ax, ay);
                        txPointDev(ptx, p.w, p.h, bx, by);
                        const float halfW = std::max(0.25f, 0.5f * p.p0 * op.pxScale);
                        const float dist = QtPhaseMath::sdStrokeSegment(fragX, fragY, ax, ay, bx, by, halfW, p.p1 > 0.5f);
                        const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
                        if (cov <= 0.0f)
                            continue;
                        srcPremulA = p.c0a * cov * effOpacity;
                        srcPremulR = p.c0r * srcPremulA;
                        srcPremulG = p.c0g * srcPremulA;
                        srcPremulB = p.c0b * srcPremulA;
                    } else if (p.kind == uint32_t(PhasePrimKind::HbGpuCurve)) {
                        if (op.maskIndex < 0 || size_t(op.maskIndex) >= frameMasks.size())
                            continue;
                        const RasterizedMask &m = frameMasks[size_t(op.maskIndex)];
                        const int mx = px - m.originX;
                        const int my = py - m.originY;
                        if (mx < 0 || mx >= m.width || my < 0 || my >= m.height)
                            continue;
                        float cov = float(m.alpha[size_t(my) * size_t(m.width) + size_t(mx)]) * (1.0f / 255.0f);
                        if (cov <= 0.0f)
                            continue;
                        cov = QtPhaseMath::applyTextStemDarkening(cov, p.c0r, p.c0g, p.c0b, stemDarkening);
                        srcPremulA = p.c0a * cov * effOpacity;
                        srcPremulR = p.c0r * srcPremulA;
                        srcPremulG = p.c0g * srcPremulA;
                        srcPremulB = p.c0b * srcPremulA;
                    } else {
                        float evalX = fragX;
                        float evalY = fragY;
                        float boxMinX = op.devMinX, boxMinY = op.devMinY;
                        float boxMaxX = op.devMaxX, boxMaxY = op.devMaxY;
                        if (op.isRotated) {
                            const float logX = fragX / dpr - ptx.tx;
                            const float logY = fragY / dpr - ptx.ty;
                            const float locX = (ptx.m11 * logX - ptx.m01 * logY) * op.invDet;
                            const float locY = (-ptx.m10 * logX + ptx.m00 * logY) * op.invDet;
                            evalX = locX * op.pxScale;
                            evalY = locY * op.pxScale;
                            boxMinX = p.x * op.pxScale;
                            boxMinY = p.y * op.pxScale;
                            boxMaxX = (p.x + p.w) * op.pxScale;
                            boxMaxY = (p.y + p.h) * op.pxScale;
                        }

                        if (p.kind == uint32_t(PhasePrimKind::SolidRect)) {
                            const float cov = op.isRotated
                                    ? std::clamp(0.5f - QtPhaseMath::sdRoundedBox(evalX, evalY, boxMinX, boxMinY, boxMaxX, boxMaxY, 0, 0, 0, 0), 0.0f, 1.0f)
                                    : QtPhaseMath::boxPixelCoverage(evalX, evalY, boxMinX, boxMinY, boxMaxX, boxMaxY);
                            if (cov <= 0.0f)
                                continue;
                            srcPremulA = p.c0a * effOpacity * cov;
                            srcPremulR = p.c0r * srcPremulA;
                            srcPremulG = p.c0g * srcPremulA;
                            srcPremulB = p.c0b * srcPremulA;
                        } else if (p.kind == uint32_t(PhasePrimKind::RoundedBox)) {
                            const float outerDist = QtPhaseMath::sdRoundedBox(
                                    evalX, evalY, boxMinX, boxMinY, boxMaxX, boxMaxY,
                                    op.rTL, op.rTR, op.rBR, op.rBL);
                            const float outerCov = std::clamp(0.5f - outerDist, 0.0f, 1.0f);
                            if (outerCov <= 0.0f)
                                continue;

                            float fillR = p.c0r, fillG = p.c0g, fillB = p.c0b, fillA = p.c0a;
                            if (p.p1 > 0.5f) {
                                float g0x = 0.0f, g0y = 0.0f, g1x = 0.0f, g1y = 0.0f;
                                if (op.isRotated) {
                                    g0x = p.p2 * op.pxScale;
                                    g0y = p.p3 * op.pxScale;
                                    g1x = p.p4 * op.pxScale;
                                    g1y = p.p5 * op.pxScale;
                                } else {
                                    txPointDev(ptx, p.p2, p.p3, g0x, g0y);
                                    txPointDev(ptx, p.p4, p.p5, g1x, g1y);
                                }
                                const float gvx = g1x - g0x;
                                const float gvy = g1y - g0y;
                                const float denom = std::max(gvx * gvx + gvy * gvy, 1e-4f);
                                const float gt = std::clamp(((evalX - g0x) * gvx + (evalY - g0y) * gvy) / denom, 0.0f, 1.0f);
                                fillR += (p.c1r - fillR) * gt;
                                fillG += (p.c1g - fillG) * gt;
                                fillB += (p.c1b - fillB) * gt;
                                fillA += (p.c1a - fillA) * gt;
                            }

                            const float borderPx = p.p0 * op.pxScale;
                            if (borderPx > 0.0f && p.ba > 0.0f) {
                                const float inMinX = boxMinX + borderPx;
                                const float inMinY = boxMinY + borderPx;
                                const float inMaxX = boxMaxX - borderPx;
                                const float inMaxY = boxMaxY - borderPx;
                                float innerCov = 0.0f;
                                if (inMinX < inMaxX && inMinY < inMaxY) {
                                    innerCov = std::clamp(0.5f - QtPhaseMath::sdRoundedBox(
                                            evalX, evalY, inMinX, inMinY, inMaxX, inMaxY,
                                            std::max(0.0f, op.rTL - borderPx),
                                            std::max(0.0f, op.rTR - borderPx),
                                            std::max(0.0f, op.rBR - borderPx),
                                            std::max(0.0f, op.rBL - borderPx)), 0.0f, 1.0f);
                                }
                                const float borderCov = std::max(0.0f, outerCov - innerCov);
                                const float fa = fillA * innerCov;
                                const float ba = p.ba * borderCov;
                                srcPremulR = (fillR * fa + p.br * ba) * effOpacity;
                                srcPremulG = (fillG * fa + p.bg * ba) * effOpacity;
                                srcPremulB = (fillB * fa + p.bb * ba) * effOpacity;
                                srcPremulA = (fa + ba) * effOpacity;
                            } else {
                                srcPremulA = fillA * outerCov * effOpacity;
                                srcPremulR = fillR * srcPremulA;
                                srcPremulG = fillG * srcPremulA;
                                srcPremulB = fillB * srcPremulA;
                            }
                        } else if (p.kind == uint32_t(PhasePrimKind::BoxShadow)) {
                            const float sigma = std::max(p.p0 * dpr * 0.5f, 0.5f);
                            const float spread = p.p1 * dpr;
                            const float dist = QtPhaseMath::sdRoundedBox(
                                    evalX, evalY,
                                    boxMinX - spread, boxMinY - spread,
                                    boxMaxX + spread, boxMaxY + spread,
                                    std::max(0.0f, op.rTL + spread),
                                    std::max(0.0f, op.rTR + spread),
                                    std::max(0.0f, op.rBR + spread),
                                    std::max(0.0f, op.rBL + spread));
                            const float shadowCov = 0.5f * (1.0f - QtPhaseMath::erfApprox(dist / (sigma * 1.41421356f)));
                            if (shadowCov <= 0.0f)
                                continue;
                            srcPremulA = p.c0a * shadowCov * effOpacity;
                            srcPremulR = p.c0r * srcPremulA;
                            srcPremulG = p.c0g * srcPremulA;
                            srcPremulB = p.c0b * srcPremulA;
                        } else if (p.kind == uint32_t(PhasePrimKind::Image)) {
                            if (p.resourceIndex >= images.size() || !images[p.resourceIndex].active)
                                continue;
                            const QImage &cpuImg = images[p.resourceIndex].cpuRgbaPremul;
                            if (cpuImg.isNull())
                                continue;

                            const float maskCov = (op.isRotated || op.rTL > 0.0f || op.rTR > 0.0f || op.rBR > 0.0f || op.rBL > 0.0f)
                                    ? std::clamp(0.5f - QtPhaseMath::sdRoundedBox(
                                            evalX, evalY, boxMinX, boxMinY, boxMaxX, boxMaxY,
                                            op.rTL, op.rTR, op.rBR, op.rBL), 0.0f, 1.0f)
                                    : QtPhaseMath::boxPixelCoverage(
                                            evalX, evalY, boxMinX, boxMinY, boxMaxX, boxMaxY);
                            if (maskCov <= 0.0f)
                                continue;

                            const float rw = std::max(boxMaxX - boxMinX, 1e-4f);
                            const float rh = std::max(boxMaxY - boxMinY, 1e-4f);
                            const float nu = std::clamp((evalX - boxMinX) / rw, 0.0f, 1.0f);
                            const float nv = std::clamp((evalY - boxMinY) / rh, 0.0f, 1.0f);
                            const float u = p.p0 + (p.p2 - p.p0) * nu;
                            const float v = p.p1 + (p.p3 - p.p1) * nv;

                            float tr = 0.0f, tg = 0.0f, tb = 0.0f, ta = 0.0f;
                            sampleBilinearPremulRgba8888(cpuImg, u, v, tr, tg, tb, ta);
                            const float w = maskCov * effOpacity * p.c0a;
                            srcPremulR = tr * w;
                            srcPremulG = tg * w;
                            srcPremulB = tb * w;
                            srcPremulA = ta * w;
                        }
                    }

                    const float invA = 1.0f - srcPremulA;
                    rowR[lx] = float(QtPhaseMath::unormFloatToByte(srcPremulR + rowR[lx] * invA)) * (1.0f / 255.0f);
                    rowG[lx] = float(QtPhaseMath::unormFloatToByte(srcPremulG + rowG[lx] * invA)) * (1.0f / 255.0f);
                    rowB[lx] = float(QtPhaseMath::unormFloatToByte(srcPremulB + rowB[lx] * invA)) * (1.0f / 255.0f);
                    rowA[lx] = float(QtPhaseMath::unormFloatToByte(srcPremulA + rowA[lx] * invA)) * (1.0f / 255.0f);
                }
            }
        }

        for (int ly = 0; ly < th; ++ly) {
            auto *dstRow = reinterpret_cast<uint32_t *>(dstPixels + (ty + ly) * bytesPerLine) + tx;
            const float *rowR = tileR + ly * tw;
            const float *rowG = tileG + ly * tw;
            const float *rowB = tileB + ly * tw;
            const float *rowA = tileA + ly * tw;

            for (int lx = 0; lx < tw; ++lx) {
                const uint8_t b = QtPhaseMath::unormFloatToByte(rowB[lx]);
                const uint8_t g = QtPhaseMath::unormFloatToByte(rowG[lx]);
                const uint8_t r = QtPhaseMath::unormFloatToByte(rowR[lx]);
                const uint8_t a = forceOpaqueAlpha ? 255u : QtPhaseMath::unormFloatToByte(rowA[lx]);
                dstRow[lx] = (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
            }
        }
    });
}

QImage QPhaseCpuRasterizer::renderToImage(QPhaseSceneArena &arena,
                                          const QSize &pixelSize,
                                          qreal devicePixelRatio,
                                          uint64_t frameTimeNs)
{
    if (pixelSize.isEmpty())
        return {};
    QImage img(pixelSize, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(devicePixelRatio);
    renderToBuffer(arena, img.bits(), img.width(), img.height(), int(img.bytesPerLine()),
                   devicePixelRatio, false, frameTimeNs);
    return img;
}

QT_END_NAMESPACE
