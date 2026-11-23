// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasescenearena_p.h"
#include "qphaseview.h"
#include "cpu/qphasecpurasterizer_p.h"
#include "cpu/qphasesharedmath_p.h"

#include <QtCore/qelapsedtimer.h>
#include <QtGui/private/qrawfont_p.h>
#include <QtGui/private/qharfbuzzng_p.h>

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb-gpu.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

QT_BEGIN_NAMESPACE

namespace {

static const QElapsedTimer &globalArenaClock()
{
    static const QElapsedTimer clock = []() {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock;
}

static std::vector<PhasePathCmd> parseSvgPathToDesignSpace(QStringView d, const QRectF &viewBox, float targetEm)
{
    std::vector<PhasePathCmd> out;
    const QByteArray utf8 = d.toUtf8();
    const char *p = utf8.constData();
    const char *end = p + utf8.size();

    const float vbX = float(viewBox.width() > 0.0 ? viewBox.x() : 0.0);
    const float vbY = float(viewBox.height() > 0.0 ? viewBox.y() : 0.0);
    const float vbW = float(viewBox.width() > 0.0 ? viewBox.width() : 24.0);
    const float vbH = float(viewBox.height() > 0.0 ? viewBox.height() : 24.0);

    auto mapX = [&](float svgX) noexcept -> float {
        return ((svgX - vbX) / vbW) * targetEm;
    };
    auto mapY = [&](float svgY) noexcept -> float {
        return (1.0f - (svgY - vbY) / vbH) * targetEm;
    };

    auto skipSep = [&]() {
        while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
            ++p;
    };

    auto hasNum = [&]() -> bool {
        skipSep();
        if (p >= end)
            return false;
        return std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.';
    };

    auto readNum = [&]() -> float {
        skipSep();
        char *next = nullptr;
        const float v = std::strtof(p, &next);
        if (next == p) {
            ++p;
            return 0.0f;
        }
        p = next;
        return v;
    };

    char cmd = 0;
    float curX = 0.0f, curY = 0.0f;
    float startX = 0.0f, startY = 0.0f;

    while (p < end) {
        skipSep();
        if (p >= end)
            break;
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            cmd = *p++;
        } else if (cmd == 0) {
            break;
        }

        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char upper = char(std::toupper(static_cast<unsigned char>(cmd)));

        if (upper == 'Z') {
            out.push_back({PhasePathCmd::Op::Close, 0, 0, 0, 0, 0, 0});
            curX = startX;
            curY = startY;
            cmd = 0;
            continue;
        }

        do {
            if (upper == 'M') {
                float x = readNum() + (rel ? curX : 0.0f);
                float y = readNum() + (rel ? curY : 0.0f);
                curX = startX = x;
                curY = startY = y;
                out.push_back({PhasePathCmd::Op::MoveTo, mapX(x), mapY(y), 0, 0, 0, 0});
                cmd = rel ? 'l' : 'L';
            } else if (upper == 'L') {
                float x = readNum() + (rel ? curX : 0.0f);
                float y = readNum() + (rel ? curY : 0.0f);
                curX = x;
                curY = y;
                out.push_back({PhasePathCmd::Op::LineTo, mapX(x), mapY(y), 0, 0, 0, 0});
            } else if (upper == 'H') {
                float x = readNum() + (rel ? curX : 0.0f);
                curX = x;
                out.push_back({PhasePathCmd::Op::LineTo, mapX(curX), mapY(curY), 0, 0, 0, 0});
            } else if (upper == 'V') {
                float y = readNum() + (rel ? curY : 0.0f);
                curY = y;
                out.push_back({PhasePathCmd::Op::LineTo, mapX(curX), mapY(curY), 0, 0, 0, 0});
            } else if (upper == 'Q') {
                float x1 = readNum() + (rel ? curX : 0.0f);
                float y1 = readNum() + (rel ? curY : 0.0f);
                float x = readNum() + (rel ? curX : 0.0f);
                float y = readNum() + (rel ? curY : 0.0f);
                curX = x;
                curY = y;
                out.push_back({PhasePathCmd::Op::QuadTo, mapX(x1), mapY(y1), mapX(x), mapY(y), 0, 0});
            } else if (upper == 'C') {
                float x1 = readNum() + (rel ? curX : 0.0f);
                float y1 = readNum() + (rel ? curY : 0.0f);
                float x2 = readNum() + (rel ? curX : 0.0f);
                float y2 = readNum() + (rel ? curY : 0.0f);
                float x = readNum() + (rel ? curX : 0.0f);
                float y = readNum() + (rel ? curY : 0.0f);
                curX = x;
                curY = y;
                out.push_back({PhasePathCmd::Op::CubicTo, mapX(x1), mapY(y1), mapX(x2), mapY(y2), mapX(x), mapY(y)});
            } else {
                break;
            }
        } while (hasNum());
    }
    return out;
}

inline void transformPoint2D(const PhaseResolvedTransform &tx, float lx, float ly, float &ox, float &oy) noexcept
{
    ox = tx.m00 * lx + tx.m01 * ly + tx.tx;
    oy = tx.m10 * lx + tx.m11 * ly + tx.ty;
}

inline void transformBoxAABB(const PhaseResolvedTransform &tx, float x, float y, float w, float h,
                             float &minX, float &minY, float &maxX, float &maxY) noexcept
{
    if ((tx.flags & 1u) == 0u) {
        const float x0 = x * tx.m00 + tx.tx;
        const float y0 = y * tx.m11 + tx.ty;
        const float x1 = (x + w) * tx.m00 + tx.tx;
        const float y1 = (y + h) * tx.m11 + tx.ty;
        minX = std::min(x0, x1);
        maxX = std::max(x0, x1);
        minY = std::min(y0, y1);
        maxY = std::max(y0, y1);
        return;
    }

    float px[4], py[4];
    transformPoint2D(tx, x,     y,     px[0], py[0]);
    transformPoint2D(tx, x + w, y,     px[1], py[1]);
    transformPoint2D(tx, x,     y + h, px[2], py[2]);
    transformPoint2D(tx, x + w, y + h, px[3], py[3]);
    minX = std::min({px[0], px[1], px[2], px[3]});
    maxX = std::max({px[0], px[1], px[2], px[3]});
    minY = std::min({py[0], py[1], py[2], py[3]});
    maxY = std::max({py[0], py[1], py[2], py[3]});
}

} // namespace

QPhaseSceneArena::QPhaseSceneArena()
{
    static std::atomic<uint64_t> s_nextArenaId{1};
    m_arenaId = s_nextArenaId.fetch_add(1, std::memory_order_relaxed);
    PhaseTransformNode rootTx{};
    rootTx.active = true;
    m_transformNodes.push_back(rootTx);
    m_resolvedTransforms.push_back(PhaseResolvedTransform{});

    PhaseClipNode rootClip{};
    rootClip.active = true;
    m_clipNodes.push_back(rootClip);
    m_resolvedClips.push_back(PhaseResolvedClip{});

    m_curveTexels.push_back(0);
    m_curveTexels.push_back(0);
    m_curves.push_back(PhaseCurveEntry{});

    m_hbGpuDraw = hb_gpu_draw_create_or_fail();
}

QPhaseSceneArena::~QPhaseSceneArena()
{
    for (hb_font_t *f : std::as_const(m_unscaledHbFonts))
        hb_font_destroy(f);
    m_unscaledHbFonts.clear();

    if (m_hbGpuDraw)
        hb_gpu_draw_destroy(m_hbGpuDraw);
}

uint64_t QPhaseSceneArena::monotonicTimestampNs() const noexcept
{
    return uint64_t(globalArenaClock().nsecsElapsed());
}

void QPhaseSceneArena::setClearColor(const QColor &color)
{
    if (m_clearColor != color) {
        m_clearColor = color;
        ++m_revision;
    }
}

void QPhaseSceneArena::setTextStemDarkening(float strength) noexcept
{
    const float clamped = std::clamp(strength, 0.0f, 1.0f);
    if (!qFuzzyCompare(m_textStemDarkening, clamped)) {
        m_textStemDarkening = clamped;
        ++m_revision;
    }
}

uint32_t QPhaseSceneArena::allocateTransform(uint32_t parentIndex)
{
    uint32_t idx = 0;
    if (!m_freeTransforms.empty()) {
        idx = m_freeTransforms.back();
        m_freeTransforms.pop_back();
    } else {
        idx = uint32_t(m_transformNodes.size());
        m_transformNodes.emplace_back();
        m_resolvedTransforms.emplace_back();
    }
    PhaseTransformNode &node = m_transformNodes[idx];
    node = PhaseTransformNode{};
    node.parentIndex = parentIndex < m_transformNodes.size() ? parentIndex : 0;
    node.active = true;
    m_transformsDirty = true;
    m_clipsDirty = true;
    ++m_transformClipRevision;
    ++m_revision;
    return idx;
}

void QPhaseSceneArena::updateTransform(uint32_t index, float tx, float ty, float sx, float sy, float opacity)
{
    updateTransform2D(index, sx, 0.0f, 0.0f, sy, tx, ty, opacity);
}

void QPhaseSceneArena::updateTransform2D(uint32_t index,
                                         float m00, float m01, float m10, float m11,
                                         float tx, float ty, float opacity)
{
    if (index == 0 || index >= m_transformNodes.size())
        return;
    PhaseTransformNode &n = m_transformNodes[index];
    if (n.localTx == tx && n.localTy == ty
        && n.localM00 == m00 && n.localM01 == m01
        && n.localM10 == m10 && n.localM11 == m11
        && n.localOpacity == opacity) {
        return;
    }
    n.localTx = tx;
    n.localTy = ty;
    n.localM00 = m00;
    n.localM01 = m01;
    n.localM10 = m10;
    n.localM11 = m11;
    n.localOpacity = opacity;
    m_transformsDirty = true;
    m_clipsDirty = true;
    ++m_transformClipRevision;
    ++m_revision;
}

void QPhaseSceneArena::setTransformParent(uint32_t index, uint32_t parentIndex)
{
    if (index == 0 || index >= m_transformNodes.size())
        return;
    const uint32_t safeParent = parentIndex < m_transformNodes.size() ? parentIndex : 0;
    if (m_transformNodes[index].parentIndex != safeParent) {
        m_transformNodes[index].parentIndex = safeParent;
        m_transformsDirty = true;
        m_clipsDirty = true;
        ++m_transformClipRevision;
        ++m_revision;
    }
}

void QPhaseSceneArena::freeTransform(uint32_t index)
{
    if (index == 0 || index >= m_transformNodes.size() || !m_transformNodes[index].active)
        return;
    m_transformNodes[index].active = false;
    m_transformsDirty = true;
    m_clipsDirty = true;
    m_freeTransforms.push_back(index);
    ++m_transformClipRevision;
    ++m_revision;
}

uint32_t QPhaseSceneArena::allocateClip(uint32_t parentClipIndex, uint32_t transformIndex,
                                        const QRectF &localRect, const QPhaseCorners &corners)
{
    uint32_t idx = 0;
    if (!m_freeClips.empty()) {
        idx = m_freeClips.back();
        m_freeClips.pop_back();
    } else {
        idx = uint32_t(m_clipNodes.size());
        m_clipNodes.emplace_back();
        m_resolvedClips.emplace_back();
    }
    PhaseClipNode &c = m_clipNodes[idx];
    c.parentClipIndex = parentClipIndex < m_clipNodes.size() ? parentClipIndex : 0;
    c.transformIndex = transformIndex < m_transformNodes.size() ? transformIndex : 0;
    c.localX = float(localRect.x());
    c.localY = float(localRect.y());
    c.localW = float(localRect.width());
    c.localH = float(localRect.height());
    c.corners = corners;
    c.active = true;
    m_clipsDirty = true;
    ++m_transformClipRevision;
    ++m_revision;
    return idx;
}

void QPhaseSceneArena::updateClip(uint32_t index, const QRectF &localRect, const QPhaseCorners &corners)
{
    if (index == 0 || index >= m_clipNodes.size())
        return;
    PhaseClipNode &c = m_clipNodes[index];
    c.localX = float(localRect.x());
    c.localY = float(localRect.y());
    c.localW = float(localRect.width());
    c.localH = float(localRect.height());
    c.corners = corners;
    m_clipsDirty = true;
    ++m_transformClipRevision;
    ++m_revision;
}

void QPhaseSceneArena::freeClip(uint32_t index)
{
    if (index == 0 || index >= m_clipNodes.size() || !m_clipNodes[index].active)
        return;
    m_clipNodes[index].active = false;
    m_clipsDirty = true;
    m_freeClips.push_back(index);
    ++m_transformClipRevision;
    ++m_revision;
}

static inline bool hasActiveAnimBits(uint32_t rawMode) noexcept
{
    return (rawMode & 0x00FFFFFFu) != 0u;
}

uint32_t QPhaseSceneArena::allocatePrimitive(const PhaseGpuPrimitive &prim)
{
    uint32_t idx = 0;
    if (!m_freePrimitives.empty()) {
        idx = m_freePrimitives.back();
        m_freePrimitives.pop_back();
        m_primitives[idx] = prim;
    } else {
        idx = uint32_t(m_primitives.size());
        m_primitives.push_back(prim);
    }
    if (hasActiveAnimBits(prim.animMode))
        ++m_activeAnimCount;
    ++m_primitiveRevision;
    const size_t page = size_t(idx >> 6u);
    if (page >= m_primPageRevisions.size())
        m_primPageRevisions.resize(page + 1, 0);
    m_primPageRevisions[page] = m_primitiveRevision;
    ++m_revision;
    return idx;
}

void QPhaseSceneArena::updatePrimitive(uint32_t index, const PhaseGpuPrimitive &prim)
{
    if (index >= m_primitives.size())
        return;
    if (hasActiveAnimBits(m_primitives[index].animMode) && m_activeAnimCount > 0)
        --m_activeAnimCount;
    if (hasActiveAnimBits(prim.animMode))
        ++m_activeAnimCount;
    m_primitives[index] = prim;
    ++m_primitiveRevision;
    const size_t page = size_t(index >> 6u);
    if (page >= m_primPageRevisions.size())
        m_primPageRevisions.resize(page + 1, 0);
    m_primPageRevisions[page] = m_primitiveRevision;
    ++m_revision;
}

void QPhaseSceneArena::freePrimitive(uint32_t index)
{
    if (index >= m_primitives.size())
        return;
    if (hasActiveAnimBits(m_primitives[index].animMode) && m_activeAnimCount > 0)
        --m_activeAnimCount;
    m_primitives[index] = PhaseGpuPrimitive{};
    m_freePrimitives.push_back(index);
    ++m_primitiveRevision;
    const size_t page = size_t(index >> 6u);
    if (page >= m_primPageRevisions.size())
        m_primPageRevisions.resize(page + 1, 0);
    m_primPageRevisions[page] = m_primitiveRevision;
    ++m_revision;
}

void QPhaseSceneArena::setDrawOrder(std::vector<uint32_t> &&order)
{
    m_drawOrder = std::move(order);
    ++m_drawOrderRevision;
    ++m_revision;
}

void QPhaseSceneArena::syncRootView(QPhaseView *rootView)
{
    if (!rootView)
        return;
    m_rendererReqs = PhaseRendererRequirements{};
    m_cpuBridges.clear();
    std::vector<uint32_t> order;
    order.reserve(m_primitives.size());
    bool orderChanged = false;
    rootView->syncToArena(*this, 0, 0, order, orderChanged);
    if (orderChanged || order != m_drawOrder)
        setDrawOrder(std::move(order));
}

uint32_t QPhaseSceneArena::acquireFreeOrNewImageSlot(PhaseImageEntry &&entry)
{
    static constexpr uint64_t kSlotReuseCooldownFrames = 3;
    static constexpr size_t kHighWaterMark = 3968;
    if (!m_freeImageSlots.empty()) {
        const PendingFreeImageSlot &front = m_freeImageSlots.front();
        const bool cooledDown = (m_frameEpoch == 0)
                || (m_frameEpoch >= front.freedEpoch + kSlotReuseCooldownFrames)
                || (m_images.size() >= kHighWaterMark);
        if (cooledDown) {
            const uint32_t slot = front.slot;
            m_freeImageSlots.pop_front();
            m_images[slot] = std::move(entry);
            return slot;
        }
    }
    const uint32_t slot = uint32_t(m_images.size());
    m_images.push_back(std::move(entry));
    return slot;
}

uint32_t QPhaseSceneArena::allocateCpuBridgeSlot()
{
    PhaseImageEntry entry{};
    entry.generation = ++m_nextImageGen;
    entry.uploadedToGpu = false;
    entry.active = true;

    const uint32_t slot = acquireFreeOrNewImageSlot(std::move(entry));
    ++m_imageTableRevision;
    ++m_revision;
    return slot;
}

void QPhaseSceneArena::registerCpuBridge(PhaseCpuBridgeEntry &&entry)
{
    m_cpuBridges.push_back(std::move(entry));
}

uint32_t QPhaseSceneArena::allocateGpuSurface(int logicalWidth, int logicalHeight, bool needsDepth,
                                              std::shared_ptr<PhaseGpuSurfaceCallback> callback)
{
    PhaseImageEntry entry{};
    entry.width = std::max(1, logicalWidth);
    entry.height = std::max(1, logicalHeight);
    entry.gpuSurfaceCallback = std::move(callback);
    entry.generation = ++m_nextImageGen;
    entry.gpuContentRevision = 1;
    entry.isGpuSurface = true;
    entry.gpuNeedsDepth = needsDepth;
    entry.uploadedToGpu = false;
    entry.active = true;

    const uint32_t slot = acquireFreeOrNewImageSlot(std::move(entry));
    ++m_imageTableRevision;
    ++m_revision;
    return slot;
}

void QPhaseSceneArena::updateGpuSurface(uint32_t slot, int logicalWidth, int logicalHeight, bool needsDepth,
                                        std::shared_ptr<PhaseGpuSurfaceCallback> callback, bool markContentDirty)
{
    if (slot >= m_images.size() || !m_images[slot].active || !m_images[slot].isGpuSurface)
        return;

    PhaseImageEntry &e = m_images[slot];
    const int w = std::max(1, logicalWidth);
    const int h = std::max(1, logicalHeight);
    const bool geomOrDepthChanged = (e.width != w) || (e.height != h) || (e.gpuNeedsDepth != needsDepth);
    e.width = w;
    e.height = h;
    e.gpuNeedsDepth = needsDepth;
    e.gpuSurfaceCallback = std::move(callback);
    if (geomOrDepthChanged || markContentDirty) {
        ++e.gpuContentRevision;
        ++m_imageTableRevision;
        ++m_revision;
    }
}

void QPhaseSceneArena::markGpuSurfaceDirty(uint32_t slot)
{
    if (slot >= m_images.size() || !m_images[slot].active || !m_images[slot].isGpuSurface)
        return;
    ++m_images[slot].gpuContentRevision;
    ++m_imageTableRevision;
    ++m_revision;
}

uint32_t QPhaseSceneArena::registerOrGetImage(const QImage &image)
{
    if (image.isNull())
        return 0;

    const qint64 key = image.cacheKey();
    auto it = m_imageCacheMap.constFind(key);
    if (it != m_imageCacheMap.constEnd()) {
        const uint32_t slot = it.value();
        if (slot < m_images.size() && m_images[slot].active)
            return slot;
    }

    PhaseImageEntry entry{};
    entry.sourceCacheKey = key;
    entry.width = image.width();
    entry.height = image.height();
    entry.cpuRgbaPremul = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    entry.generation = ++m_nextImageGen;
    entry.uploadedToGpu = false;
    entry.active = true;

    const uint32_t slot = acquireFreeOrNewImageSlot(std::move(entry));

    m_imageCacheMap.insert(key, slot);
    ++m_imageTableRevision;
    ++m_revision;
    return slot;
}

void QPhaseSceneArena::unregisterImage(uint32_t slot)
{
    if (slot >= m_images.size() || !m_images[slot].active)
        return;

    PhaseImageEntry &e = m_images[slot];
    if (e.sourceCacheKey != 0)
        m_imageCacheMap.remove(e.sourceCacheKey);
    e.cpuRgbaPremul = QImage();
    e.gpuSurfaceCallback.reset();
    e.isGpuSurface = false;
    e.gpuNeedsDepth = false;
    e.uploadedToGpu = false;
    e.active = false;
    e.generation = ++m_nextImageGen;
    m_cpuBridgeCache.remove(slot);
    m_freeImageSlots.push_back({ slot, m_frameEpoch });
    ++m_imageTableRevision;
    ++m_revision;
}

void QPhaseSceneArena::unregisterImageByKey(qint64 sourceCacheKey)
{
    auto it = m_imageCacheMap.constFind(sourceCacheKey);
    if (it != m_imageCacheMap.constEnd())
        unregisterImage(it.value());
}

size_t QPhaseSceneArena::activeImageSlotCount() const noexcept
{
    size_t count = 0;
    for (const PhaseImageEntry &e : m_images) {
        if (e.active)
            ++count;
    }
    return count;
}

size_t QPhaseSceneArena::activeCpuImageBytes() const noexcept
{
    size_t total = 0;
    for (const PhaseImageEntry &e : m_images) {
        if (e.active && !e.cpuRgbaPremul.isNull())
            total += size_t(e.cpuRgbaPremul.sizeInBytes());
    }
    return total;
}

uint32_t QPhaseSceneArena::registerOrGetGlyph(const QRawFont &rawFont, uint32_t glyphId)
{
    if (!rawFont.isValid() || !m_hbGpuDraw)
        return 0;

    QFontEngine *fe = QRawFontPrivate::get(rawFont)->fontEngine;
    if (!fe)
        return 0;

    hb_face_t *hbFace = hb_qt_face_get_for_engine(fe);
    if (!hbFace)
        return 0;

    const uint64_t mapKey = (uint64_t(reinterpret_cast<uintptr_t>(hbFace)) << 16) ^ (uint64_t(glyphId) * 0x9E3779B97F4A7C15ull);
    auto found = m_glyphCurveMap.constFind(mapKey);
    if (found != m_glyphCurveMap.constEnd()) {
        const PhaseCurveEntry &existing = m_curves[found.value()];
        if (existing.glyphId == glyphId)
            return found.value();
    }

    hb_font_t *unscaledFont = m_unscaledHbFonts.value(hbFace, nullptr);
    if (!unscaledFont) {
        unscaledFont = hb_font_create(hbFace);
        hb_ot_font_set_funcs(unscaledFont);
        m_unscaledHbFonts.insert(hbFace, unscaledFont);
    }

    hb_gpu_draw_clear(m_hbGpuDraw);
    const unsigned int upem = std::max(1u, hb_face_get_upem(hbFace));
    hb_gpu_draw_set_scale(m_hbGpuDraw, int(upem), int(upem));
    hb_gpu_draw_glyph(m_hbGpuDraw, unscaledFont, glyphId);

    hb_glyph_extents_t ext{};
    hb_blob_t *blob = hb_gpu_draw_encode(m_hbGpuDraw, &ext);
    if (!blob)
        return 0;

    unsigned int blobBytes = 0;
    const char *blobData = hb_blob_get_data(blob, &blobBytes);
    if (blobBytes == 0 || ext.width == 0 || ext.height == 0) {
        hb_gpu_draw_recycle_blob(m_hbGpuDraw, blob);
        return 0;
    }

    const uint32_t texelCount = (blobBytes + 7u) / 8u;
    const uint32_t texelOffset = uint32_t(m_curveTexels.size());
    m_curveTexels.resize(texelOffset + texelCount, 0);
    std::memcpy(m_curveTexels.data() + texelOffset, blobData, blobBytes);
    hb_gpu_draw_recycle_blob(m_hbGpuDraw, blob);

    PhaseCurveEntry entry{};
    entry.texelOffset = texelOffset;
    entry.texelCount = texelCount;
    entry.xMin = float(std::min(ext.x_bearing, ext.x_bearing + ext.width));
    entry.xMax = float(std::max(ext.x_bearing, ext.x_bearing + ext.width));
    entry.yMin = float(std::min(ext.y_bearing, ext.y_bearing + ext.height));
    entry.yMax = float(std::max(ext.y_bearing, ext.y_bearing + ext.height));
    entry.unitsPerEm = float(upem);
    entry.isGlyph = true;
    entry.unscaledHbFont = unscaledFont;
    entry.glyphId = glyphId;

    const uint32_t curveId = uint32_t(m_curves.size());
    m_curves.push_back(std::move(entry));
    m_glyphCurveMap.insert(mapKey, curveId);
    ++m_revision;
    return curveId;
}

bool QPhaseSceneArena::tryGetColorGlyphLayers(const QRawFont &rawFont,
                                              uint32_t glyphId,
                                              std::vector<PhaseColorGlyphLayer> &outLayers)
{
    if (!rawFont.isValid())
        return false;
    QFontEngine *fe = QRawFontPrivate::get(rawFont)->fontEngine;
    if (!fe)
        return false;
    hb_face_t *hbFace = hb_qt_face_get_for_engine(fe);
    if (!hbFace || !hb_ot_color_has_layers(hbFace))
        return false;

    unsigned int totalLayers = hb_ot_color_glyph_get_layers(hbFace, glyphId, 0, nullptr, nullptr);
    if (totalLayers == 0)
        return false;

    std::vector<hb_ot_color_layer_t> rawLayers(totalLayers);
    hb_ot_color_glyph_get_layers(hbFace, glyphId, 0, &totalLayers, rawLayers.data());

    unsigned int palCount = 0;
    hb_ot_color_palette_get_colors(hbFace, 0, 0, &palCount, nullptr);
    std::vector<hb_color_t> palette(palCount);
    if (palCount > 0)
        hb_ot_color_palette_get_colors(hbFace, 0, 0, &palCount, palette.data());

    outLayers.reserve(totalLayers);
    for (const hb_ot_color_layer_t &rl : rawLayers) {
        const uint32_t cid = registerOrGetGlyph(rawFont, rl.glyph);
        if (cid == 0)
            continue;
        PhaseColorGlyphLayer layer{};
        layer.curveId = cid;
        if (rl.color_index == 0xFFFFu || rl.color_index >= palette.size()) {
            layer.useForegroundColor = true;
        } else {
            const hb_color_t hc = palette[rl.color_index];
            layer.color = QColor(hb_color_get_red(hc),
                                 hb_color_get_green(hc),
                                 hb_color_get_blue(hc),
                                 hb_color_get_alpha(hc));
        }
        outLayers.push_back(layer);
    }
    return !outLayers.empty();
}

PhaseBitmapGlyphEntry QPhaseSceneArena::tryRegisterBitmapColorGlyph(const QRawFont &rawFont, uint32_t glyphId)
{
    if (!rawFont.isValid())
        return {};
    QFontEngine *fe = QRawFontPrivate::get(rawFont)->fontEngine;
    if (!fe)
        return {};
    hb_face_t *hbFace = hb_qt_face_get_for_engine(fe);
    if (!hbFace)
        return {};

    const bool isColorFace = (fe->glyphFormat == QFontEngine::Format_ARGB)
            || hb_ot_color_has_png(hbFace)
            || (hb_ot_color_has_paint(hbFace) && hb_ot_color_glyph_has_paint(hbFace, glyphId))
            || hb_ot_color_has_svg(hbFace);
    if (!isColorFace)
        return {};

    const int pxKey = int(std::lround(rawFont.pixelSize() * 16.0));
    const uint64_t cacheKey = (uint64_t(reinterpret_cast<uintptr_t>(hbFace)) << 24)
            ^ (uint64_t(uint32_t(pxKey)) << 32)
            ^ (uint64_t(glyphId) * 0x9E3779B97F4A7C15ULL);
    auto found = m_bitmapGlyphMap.constFind(cacheKey);
    if (found != m_bitmapGlyphMap.constEnd()) {
        const PhaseBitmapGlyphEntry &cached = found.value();
        if (!cached.valid || (cached.imageSlot < m_images.size() && m_images[cached.imageSlot].active))
            return cached;
    }

    const QImage glyphImg = rawFont.alphaMapForGlyph(glyphId, QRawFont::PixelAntialiasing);
    if (glyphImg.isNull() || glyphImg.width() <= 0 || glyphImg.height() <= 0) {
        m_bitmapGlyphMap.insert(cacheKey, PhaseBitmapGlyphEntry{});
        return {};
    }

    PhaseBitmapGlyphEntry res{};
    res.imageSlot = registerOrGetImage(glyphImg);
    res.pixelBounds = rawFont.boundingRect(glyphId);
    if (res.pixelBounds.isEmpty())
        res.pixelBounds = QRectF(0.0, -glyphImg.height(), glyphImg.width(), glyphImg.height());
    res.valid = true;
    m_bitmapGlyphMap.insert(cacheKey, res);
    return res;
}

uint32_t QPhaseSceneArena::registerOrGetSvgPath(QStringView svgPathData, const QRectF &viewBox)
{
    if (svgPathData.isEmpty() || !m_hbGpuDraw)
        return 0;

    const QString key = QStringLiteral("%1|%2,%3,%4,%5")
            .arg(svgPathData)
            .arg(viewBox.x()).arg(viewBox.y()).arg(viewBox.width()).arg(viewBox.height());
    auto it = m_svgCurveMap.constFind(key);
    if (it != m_svgCurveMap.constEnd())
        return it.value();

    static constexpr float kVectorDesignEm = 1024.0f;
    std::vector<PhasePathCmd> cmds = parseSvgPathToDesignSpace(svgPathData, viewBox, kVectorDesignEm);
    if (cmds.empty())
        return 0;

    hb_gpu_draw_clear(m_hbGpuDraw);
    hb_gpu_draw_set_scale(m_hbGpuDraw, int(kVectorDesignEm), int(kVectorDesignEm));
    hb_draw_funcs_t *df = hb_gpu_draw_get_funcs(m_hbGpuDraw);
    hb_draw_state_t st = HB_DRAW_STATE_DEFAULT;

    for (const PhasePathCmd &c : cmds) {
        switch (c.op) {
        case PhasePathCmd::Op::MoveTo:
            hb_draw_move_to(df, m_hbGpuDraw, &st, c.x0, c.y0);
            break;
        case PhasePathCmd::Op::LineTo:
            hb_draw_line_to(df, m_hbGpuDraw, &st, c.x0, c.y0);
            break;
        case PhasePathCmd::Op::QuadTo:
            hb_draw_quadratic_to(df, m_hbGpuDraw, &st, c.x0, c.y0, c.x1, c.y1);
            break;
        case PhasePathCmd::Op::CubicTo:
            hb_draw_cubic_to(df, m_hbGpuDraw, &st, c.x0, c.y0, c.x1, c.y1, c.x2, c.y2);
            break;
        case PhasePathCmd::Op::Close:
            hb_draw_close_path(df, m_hbGpuDraw, &st);
            break;
        }
    }

    hb_glyph_extents_t ext{};
    hb_blob_t *blob = hb_gpu_draw_encode(m_hbGpuDraw, &ext);
    if (!blob)
        return 0;

    unsigned int blobBytes = 0;
    const char *blobData = hb_blob_get_data(blob, &blobBytes);
    if (blobBytes == 0) {
        hb_gpu_draw_recycle_blob(m_hbGpuDraw, blob);
        return 0;
    }

    const uint32_t texelCount = (blobBytes + 7u) / 8u;
    const uint32_t texelOffset = uint32_t(m_curveTexels.size());
    m_curveTexels.resize(texelOffset + texelCount, 0);
    std::memcpy(m_curveTexels.data() + texelOffset, blobData, blobBytes);
    hb_gpu_draw_recycle_blob(m_hbGpuDraw, blob);

    PhaseCurveEntry entry{};
    entry.texelOffset = texelOffset;
    entry.texelCount = texelCount;
    entry.xMin = float(std::min(ext.x_bearing, ext.x_bearing + ext.width));
    entry.xMax = float(std::max(ext.x_bearing, ext.x_bearing + ext.width));
    entry.yMin = float(std::min(ext.y_bearing, ext.y_bearing + ext.height));
    entry.yMax = float(std::max(ext.y_bearing, ext.y_bearing + ext.height));
    entry.unitsPerEm = kVectorDesignEm;
    entry.isGlyph = false;
    if (!m_inGpuSteadyState)
        entry.cpuVectorCmds = std::move(cmds);

    const uint32_t curveId = uint32_t(m_curves.size());
    m_curves.push_back(std::move(entry));
    m_svgCurveMap.insert(key, curveId);
    ++m_revision;
    return curveId;
}

void QPhaseSceneArena::markEnteredGpuSteadyState()
{
    m_inGpuSteadyState = true;
    releaseUploadedCpuImagePixels();
    if (m_rendererReqs.cpuOnlyViewCount == 0) {
        for (PhaseCurveEntry &c : m_curves) {
            c.cpuVectorCmds.clear();
            c.cpuVectorCmds.shrink_to_fit();
        }
    }
}

void QPhaseSceneArena::resetGpuSteadyState()
{
    m_inGpuSteadyState = false;
}

void QPhaseSceneArena::syncRenderSnapshotFrom(QPhaseSceneArena &src)
{
    src.resolveHierarchy();

    if (m_arenaId != src.m_arenaId) {
        m_arenaId = src.m_arenaId;
        m_transformClipRevision = 0;
        m_primitiveRevision = 0;
        m_drawOrderRevision = 0;
        m_imageTableRevision = 0;
        m_images.clear();
        m_curveTexels.clear();
        m_curves.clear();
    }

    m_revision = src.m_revision;
    m_clearColor = src.m_clearColor;
    m_textStemDarkening = src.m_textStemDarkening;
    m_activeAnimCount = src.m_activeAnimCount;
    m_inGpuSteadyState = src.m_inGpuSteadyState;

    if (m_transformClipRevision != src.m_transformClipRevision) {
        m_resolvedTransforms = src.m_resolvedTransforms;
        m_resolvedClips = src.m_resolvedClips;
        m_transformClipRevision = src.m_transformClipRevision;
    }
    if (m_primitiveRevision != src.m_primitiveRevision) {
        const uint64_t prevPrimRev = m_primitiveRevision;
        if (m_primitives.size() != src.m_primitives.size() || prevPrimRev == 0) {
            m_primitives = src.m_primitives;
        } else {
            for (size_t page = 0; page < src.m_primPageRevisions.size(); ++page) {
                if (src.m_primPageRevisions[page] > prevPrimRev) {
                    const size_t startIdx = page * 64;
                    const size_t count = std::min<size_t>(64, src.m_primitives.size() - startIdx);
                    std::memcpy(m_primitives.data() + startIdx,
                                src.m_primitives.data() + startIdx,
                                count * sizeof(PhaseGpuPrimitive));
                }
            }
        }
        m_primPageRevisions = src.m_primPageRevisions;
        m_primitiveRevision = src.m_primitiveRevision;
    }
    if (m_drawOrderRevision != src.m_drawOrderRevision) {
        m_drawOrder = src.m_drawOrder;
        m_drawOrderRevision = src.m_drawOrderRevision;
    }
    m_transformsDirty = false;
    m_clipsDirty = false;
    m_rendererReqs = src.m_rendererReqs;

    // Generation-tracked non-destructive sync of bindless image & custom GPU surface slots.
    // QImage is COW ref-counted so copying costs 0 bytes; CPU pixel RAM is only released via
    // explicit GPU upload receipts (acknowledgeGpuImagesUploaded / releaseUploadedCpuImagePixels)
    // so pixels can NEVER be dropped before Vulkan confirms upload!
    if (m_imageTableRevision != src.m_imageTableRevision || m_images.size() != src.m_images.size()) {
        if (m_images.size() < src.m_images.size())
            m_images.resize(src.m_images.size());
        for (size_t i = 0; i < src.m_images.size(); ++i) {
            PhaseImageEntry &dstE = m_images[i];
            const PhaseImageEntry &srcE = src.m_images[i];
            if (dstE.generation != srcE.generation || dstE.active != srcE.active) {
                dstE = srcE;
                dstE.uploadedToGpu = false;
            } else if (srcE.active && srcE.isGpuSurface) {
                dstE.width = srcE.width;
                dstE.height = srcE.height;
                dstE.gpuNeedsDepth = srcE.gpuNeedsDepth;
                dstE.gpuContentRevision = srcE.gpuContentRevision;
                dstE.gpuSurfaceCallback = srcE.gpuSurfaceCallback;
            }
        }
        m_imageTableRevision = src.m_imageTableRevision;
    }

    if (m_curveTexels.size() < src.m_curveTexels.size()) {
        const size_t oldTex = m_curveTexels.size();
        m_curveTexels.resize(src.m_curveTexels.size());
        std::memcpy(m_curveTexels.data() + oldTex,
                    src.m_curveTexels.data() + oldTex,
                    (src.m_curveTexels.size() - oldTex) * sizeof(uint64_t));
    }
    if (m_curves.size() < src.m_curves.size()) {
        const size_t oldCur = m_curves.size();
        m_curves.resize(src.m_curves.size());
        for (size_t i = oldCur; i < src.m_curves.size(); ++i)
            m_curves[i] = src.m_curves[i];
    }
}

void QPhaseSceneArena::releaseUploadedCpuImagePixels()
{
    if (!m_inGpuSteadyState)
        return;
    for (PhaseImageEntry &img : m_images) {
        if (img.active && img.uploadedToGpu && !img.cpuRgbaPremul.isNull())
            img.cpuRgbaPremul = QImage();
    }
}

void QPhaseSceneArena::acknowledgeGpuImagesUploaded(uint64_t upToGeneration)
{
    if (!m_inGpuSteadyState || upToGeneration == 0)
        return;
    for (PhaseImageEntry &img : m_images) {
        if (img.active && !img.isGpuSurface && img.generation <= upToGeneration && !img.cpuRgbaPremul.isNull())
            img.cpuRgbaPremul = QImage();
    }
}

void QPhaseSceneArena::resolveHierarchy()
{
    if (m_transformsDirty) {
        m_resolvedTransforms.resize(m_transformNodes.size());
        m_resolvedTransforms[0] = PhaseResolvedTransform{};

        std::vector<uint8_t> state(m_transformNodes.size(), 0);
        state[0] = 2;

        auto resolveTx = [&](auto &self, uint32_t i) -> PhaseResolvedTransform {
            if (state[i] == 2)
                return m_resolvedTransforms[i];
            if (state[i] == 1 || !m_transformNodes[i].active) {
                m_resolvedTransforms[i] = PhaseResolvedTransform{};
                state[i] = 2;
                return m_resolvedTransforms[i];
            }
            state[i] = 1;
            const PhaseTransformNode &n = m_transformNodes[i];
            const PhaseResolvedTransform p = self(self, n.parentIndex);
            PhaseResolvedTransform r{};
            r.m00 = p.m00 * n.localM00 + p.m01 * n.localM10;
            r.m01 = p.m00 * n.localM01 + p.m01 * n.localM11;
            r.m10 = p.m10 * n.localM00 + p.m11 * n.localM10;
            r.m11 = p.m10 * n.localM01 + p.m11 * n.localM11;
            r.tx  = p.m00 * n.localTx  + p.m01 * n.localTy  + p.tx;
            r.ty  = p.m10 * n.localTx  + p.m11 * n.localTy  + p.ty;
            r.opacity = p.opacity * n.localOpacity;
            r.flags = (std::abs(r.m01) > 1e-6f || std::abs(r.m10) > 1e-6f) ? 1u : 0u;
            m_resolvedTransforms[i] = r;
            state[i] = 2;
            return r;
        };

        for (uint32_t i = 1; i < uint32_t(m_transformNodes.size()); ++i)
            resolveTx(resolveTx, i);

        m_transformsDirty = false;
    }

    if (m_clipsDirty) {
        m_resolvedClips.resize(m_clipNodes.size());
        m_resolvedClips[0] = PhaseResolvedClip{};

        std::vector<uint8_t> state(m_clipNodes.size(), 0);
        state[0] = 2;

        auto resolveCl = [&](auto &self, uint32_t i) -> PhaseResolvedClip {
            if (state[i] == 2)
                return m_resolvedClips[i];
            if (state[i] == 1 || !m_clipNodes[i].active) {
                m_resolvedClips[i] = PhaseResolvedClip{};
                state[i] = 2;
                return m_resolvedClips[i];
            }
            state[i] = 1;
            const PhaseClipNode &c = m_clipNodes[i];
            const PhaseResolvedClip parent = self(self, c.parentClipIndex);
            const PhaseResolvedTransform &tx = m_resolvedTransforms[c.transformIndex];

            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            transformBoxAABB(tx, c.localX, c.localY, c.localW, c.localH, x0, y0, x1, y1);
            const float radiusScale = std::sqrt(std::abs(tx.m00 * tx.m11 - tx.m01 * tx.m10));

            PhaseResolvedClip r{};
            r.selfMinX = x0;
            r.selfMinY = y0;
            r.selfMaxX = x1;
            r.selfMaxY = y1;
            r.cullMinX = std::max(x0, parent.cullMinX);
            r.cullMinY = std::max(y0, parent.cullMinY);
            r.cullMaxX = std::min(x1, parent.cullMaxX);
            r.cullMaxY = std::min(y1, parent.cullMaxY);
            if (r.cullMaxX < r.cullMinX)
                r.cullMaxX = r.cullMinX;
            if (r.cullMaxY < r.cullMinY)
                r.cullMaxY = r.cullMinY;

            r.rTL = c.corners.topLeft * radiusScale;
            r.rTR = c.corners.topRight * radiusScale;
            r.rBR = c.corners.bottomRight * radiusScale;
            r.rBL = c.corners.bottomLeft * radiusScale;

            const bool selfRounded = (r.rTL > 0.0f || r.rTR > 0.0f || r.rBR > 0.0f || r.rBL > 0.0f);
            const bool parentSelfRounded = (parent.rTL > 0.0f || parent.rTR > 0.0f
                                            || parent.rBR > 0.0f || parent.rBL > 0.0f);
            r.parentRoundedClipIndex = parentSelfRounded ? c.parentClipIndex : parent.parentRoundedClipIndex;
            r.hasRoundedAncestorOrSelf = (selfRounded || r.parentRoundedClipIndex != 0) ? 1u : 0u;

            m_resolvedClips[i] = r;
            state[i] = 2;
            return r;
        };

        for (uint32_t i = 1; i < uint32_t(m_clipNodes.size()); ++i)
            resolveCl(resolveCl, i);

        m_clipsDirty = false;
    }
}

namespace {

inline uint64_t fastMixBytes64(uint64_t h, const void *data, size_t len) noexcept
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
    if (len >= 4) {
        uint32_t w = 0;
        std::memcpy(&w, p, 4);
        h = (h ^ uint64_t(w)) * 0x9E3779B97F4A7C15ULL;
        p += 4;
        len -= 4;
    }
    while (len > 0) {
        h = (h ^ uint64_t(*p++)) * 1099511628211ULL;
        --len;
    }
    return h;
}

static QRect computePrimitiveDeviceBounds(const PhaseGpuPrimitive &p,
                                          const PhaseResolvedTransform &tx,
                                          const PhaseResolvedClip &cl,
                                          float dpr,
                                          int vpWidth,
                                          int vpHeight) noexcept
{
    float logMinX = 0.0f, logMinY = 0.0f, logMaxX = 0.0f, logMaxY = 0.0f;
    if (p.kind == uint32_t(PhasePrimKind::StrokeSegment)) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        transformPoint2D(tx, p.x, p.y, ax, ay);
        transformPoint2D(tx, p.w, p.h, bx, by);
        const float scale = std::sqrt(std::abs(tx.m00 * tx.m11 - tx.m01 * tx.m10));
        const float halfW = 0.5f * p.p0 * scale;
        logMinX = std::min(ax, bx) - halfW;
        logMinY = std::min(ay, by) - halfW;
        logMaxX = std::max(ax, bx) + halfW;
        logMaxY = std::max(ay, by) + halfW;
    } else {
        transformBoxAABB(tx, p.x, p.y, p.w, p.h, logMinX, logMinY, logMaxX, logMaxY);
    }

    const float devMinX = logMinX * dpr;
    const float devMinY = logMinY * dpr;
    const float devMaxX = logMaxX * dpr;
    const float devMaxY = logMaxY * dpr;

    const float clipMinX = cl.cullMinX * dpr;
    const float clipMinY = cl.cullMinY * dpr;
    const float clipMaxX = cl.cullMaxX * dpr;
    const float clipMaxY = cl.cullMaxY * dpr;

    float expandPx = 2.0f;
    if (p.kind == uint32_t(PhasePrimKind::BoxShadow)) {
        const float blurDev = p.p0 * dpr;
        const float spreadDev = p.p1 * dpr;
        expandPx = std::max(2.0f, std::ceil(blurDev * 2.5f + std::max(spreadDev, 0.0f) + 3.0f));
    }

    const int x0 = std::max(0, int(std::max(std::floor(devMinX - expandPx), std::floor(clipMinX))));
    const int y0 = std::max(0, int(std::max(std::floor(devMinY - expandPx), std::floor(clipMinY))));
    const int x1 = std::min(vpWidth, int(std::min(std::ceil(devMaxX + expandPx), std::ceil(clipMaxX))));
    const int y1 = std::min(vpHeight, int(std::min(std::ceil(devMaxY + expandPx), std::ceil(clipMaxY))));
    if (x0 >= x1 || y0 >= y1)
        return {};
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

static uint64_t hashPrimitiveAppearance(const PhaseGpuPrimitive &p,
                                        const PhaseResolvedTransform &tx,
                                        const PhaseResolvedClip &cl,
                                        uint16_t quantizedOpacity,
                                        uint64_t imageGen) noexcept
{
    uint64_t h = 14695981039346656037ULL;
    h = fastMixBytes64(h, &p.kind, sizeof(p.kind));
    const size_t geomStart = offsetof(PhaseGpuPrimitive, resourceIndex);
    const size_t geomEnd = offsetof(PhaseGpuPrimitive, animMode);
    h = fastMixBytes64(h, reinterpret_cast<const char *>(&p) + geomStart, geomEnd - geomStart);
    h = fastMixBytes64(h, &tx, sizeof(float) * 6); // tx, ty, m00, m01, m10, m11
    h = fastMixBytes64(h, &cl, sizeof(PhaseResolvedClip));
    h = fastMixBytes64(h, &quantizedOpacity, sizeof(quantizedOpacity));
    if (imageGen != 0)
        h = fastMixBytes64(h, &imageGen, sizeof(imageGen));
    return h;
}

} // namespace

void QPhaseSceneArena::rasterizeDirtyCpuBridges(QPhaseCpuRasterizer &cpuRasterizer,
                                                qreal devicePixelRatio,
                                                uint64_t frameTimeNs)
{
    if (m_cpuBridges.empty() || devicePixelRatio <= 0.0)
        return;

    for (const PhaseCpuBridgeEntry &br : m_cpuBridges) {
        if (br.imageSlot >= m_images.size() || !m_images[br.imageSlot].active)
            continue;

        const int pxW = std::max(1, int(std::ceil(double(br.logicalWidth) * double(devicePixelRatio))));
        const int pxH = std::max(1, int(std::ceil(double(br.logicalHeight) * double(devicePixelRatio))));
        if (br.localPrimOrder.empty() || br.logicalWidth <= 0.0f || br.logicalHeight <= 0.0f)
            continue;

        // Compute content hash of the CpuOnly subtree's primitives + relative child transforms/clips
        uint64_t h = 14695981039346656037ULL;
        h = fastMixBytes64(h, &pxW, sizeof(pxW));
        h = fastMixBytes64(h, &pxH, sizeof(pxH));
        for (uint32_t pid : br.localPrimOrder) {
            if (pid >= m_primitives.size())
                continue;
            const PhaseGpuPrimitive &p = m_primitives[pid];
            h = fastMixBytes64(h, &p, sizeof(PhaseGpuPrimitive));
            if (p.transformIndex != br.selfTransformSlot && p.transformIndex < m_transformNodes.size())
                h = fastMixBytes64(h, &m_transformNodes[p.transformIndex], sizeof(PhaseTransformNode));
            if (p.clipIndex != br.inheritedClipSlot && p.clipIndex < m_clipNodes.size())
                h = fastMixBytes64(h, &m_clipNodes[p.clipIndex], sizeof(PhaseClipNode));
        }

        const PhaseCpuBridgeCacheState prev = m_cpuBridgeCache.value(br.imageSlot);
        if (!br.hasSubtreeAnimations
            && prev.lastSubtreeHash == h
            && qFuzzyCompare(prev.lastDpr, devicePixelRatio)
            && (m_images[br.imageSlot].uploadedToGpu || !m_images[br.imageSlot].cpuRgbaPremul.isNull())) {
            continue;
        }

        // Temporarily anchor the CpuOnly subtree's root transform at local (0, 0) identity and unbind
        // ancestor clip (since the GPU Image primitive applies root transform, rotation, opacity & ancestor clips!)
        const bool hasValidTx = br.selfTransformSlot > 0 && br.selfTransformSlot < m_transformNodes.size();
        PhaseTransformNode savedTx{};
        if (hasValidTx) {
            savedTx = m_transformNodes[br.selfTransformSlot];
            PhaseTransformNode ident{};
            ident.active = true;
            m_transformNodes[br.selfTransformSlot] = ident;
        }

        const bool hasInheritedClip = br.inheritedClipSlot > 0 && br.inheritedClipSlot < m_clipNodes.size();
        PhaseClipNode savedClip{};
        if (hasInheritedClip) {
            savedClip = m_clipNodes[br.inheritedClipSlot];
            PhaseClipNode openClip{};
            openClip.localX = -1e6f;
            openClip.localY = -1e6f;
            openClip.localW = 2e6f;
            openClip.localH = 2e6f;
            openClip.active = true;
            m_clipNodes[br.inheritedClipSlot] = openClip;
        }

        const QColor savedClear = m_clearColor;
        std::vector<uint32_t> savedOrder = std::move(m_drawOrder);
        m_clearColor = QColor(0, 0, 0, 0);
        m_drawOrder = br.localPrimOrder;
        m_transformsDirty = true;
        m_clipsDirty = true;

        QImage bgra(pxW, pxH, QImage::Format_ARGB32_Premultiplied);
        bgra.setDevicePixelRatio(devicePixelRatio);
        cpuRasterizer.renderToBuffer(*this, bgra.bits(), pxW, pxH, int(bgra.bytesPerLine()),
                                     devicePixelRatio, /*forceOpaqueAlpha=*/false, frameTimeNs);

        m_clearColor = savedClear;
        m_drawOrder = std::move(savedOrder);
        if (hasValidTx)
            m_transformNodes[br.selfTransformSlot] = savedTx;
        if (hasInheritedClip)
            m_clipNodes[br.inheritedClipSlot] = savedClip;
        m_transformsDirty = true;
        m_clipsDirty = true;
        resolveHierarchy();

        PhaseImageEntry &ie = m_images[br.imageSlot];
        ie.width = pxW;
        ie.height = pxH;
        ie.cpuRgbaPremul = bgra.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        ie.generation = ++m_nextImageGen;
        ie.uploadedToGpu = false;
        ie.active = true;

        m_cpuBridgeCache.insert(br.imageSlot, { h, devicePixelRatio });
        ++m_imageTableRevision;
        ++m_revision;
    }
}

QPhaseDamageFrameSnapshot QPhaseDamageFrameSnapshot::capture(QPhaseSceneArena &arena,
                                                             const QSize &pixelSize,
                                                             qreal devicePixelRatio,
                                                             uint64_t frameTimeNs,
                                                             QPhaseRendererAffinity targetBackend)
{
    QPhaseDamageFrameSnapshot snap{};
    snap.arenaId = arena.arenaId();
    snap.revision = arena.revision();
    snap.pixelSize = pixelSize;
    snap.dpr = devicePixelRatio;
    snap.clearColor = arena.clearColor();
    snap.textStemDarkening = arena.textStemDarkening();
    snap.hasAnimations = arena.hasActiveAnimations();

    if (pixelSize.isEmpty())
        return snap;

    arena.resolveHierarchy();

    const int vpW = pixelSize.width();
    const int vpH = pixelSize.height();
    const float dpr = float(devicePixelRatio);

    const auto &prims = arena.primitives();
    const auto &transforms = arena.resolvedTransforms();
    const auto &clips = arena.resolvedClips();
    const auto &order = arena.drawOrder();
    const auto &images = arena.images();

    snap.primSlots.resize(prims.size());
    snap.visibleOrder.reserve(order.size());

    for (uint32_t idx : order) {
        if (idx >= prims.size())
            continue;
        const PhaseGpuPrimitive &p = prims[idx];
        if (targetBackend == QPhaseRendererAffinity::CpuOnly && (p.animMode & kPhaseAnimFlagGpuOnly) != 0u)
            continue;
        if (targetBackend == QPhaseRendererAffinity::GpuOnly && (p.animMode & kPhaseAnimFlagCpuOnly) != 0u)
            continue;
        const PhaseResolvedTransform &tx = transforms[p.transformIndex];
        const PhaseResolvedClip &cl = clips[p.clipIndex];

        const float animOp = QtPhaseMath::evalAnimationOpacity(
                p.animMode, p.animDurationMs, p.animStartNs, p.animFromOpacity, p.animToOpacity, frameTimeNs);
        const float effOp = tx.opacity * animOp;
        if (effOp <= 0.0f)
            continue;

        const QRect bounds = computePrimitiveDeviceBounds(p, tx, cl, dpr, vpW, vpH);
        if (bounds.isEmpty())
            continue;

        uint64_t imgGen = 0;
        if (p.kind == uint32_t(PhasePrimKind::Image) && p.resourceIndex < images.size()) {
            const PhaseImageEntry &ie = images[p.resourceIndex];
            imgGen = ie.generation ^ (ie.isGpuSurface ? (ie.gpuContentRevision * 0x9E3779B97F4A7C15ULL) : 0ULL);
        }

        const auto quantOp = uint16_t(std::clamp(int(std::lround(effOp * 512.0f)), 0, 512));
        const uint64_t hash = hashPrimitiveAppearance(p, tx, cl, quantOp, imgGen);

        PhasePrimDamageSlot &slot = snap.primSlots[idx];
        if (!slot.active) {
            slot.active = true;
            slot.contentHash = hash;
            slot.devBounds = bounds;
            snap.visibleOrder.push_back(idx);
        }
    }
    return snap;
}

void QPhaseDamageTracker::reset() noexcept
{
    m_hasValidState = false;
    m_prevSlots.clear();
    m_prevVisibleOrder.clear();
}

bool QPhaseDamageTracker::isUnchanged(const QPhaseSceneArena &arena,
                                      const QSize &pixelSize,
                                      qreal devicePixelRatio) const noexcept
{
    return m_hasValidState
            && m_arenaId == arena.arenaId()
            && m_revision == arena.revision()
            && !arena.hasActiveAnimations()
            && m_pixelSize == pixelSize
            && qFuzzyCompare(m_dpr, devicePixelRatio)
            && m_clearColor == arena.clearColor()
            && qFuzzyCompare(m_textStemDarkening, arena.textStemDarkening());
}

quint64 QPhaseDamageTracker::regionPixelArea(const QRegion &region) noexcept
{
    quint64 area = 0;
    for (const QRect &r : region)
        area += quint64(std::max(0, r.width())) * quint64(std::max(0, r.height()));
    return area;
}

QRegion QPhaseDamageTracker::deviceToLogicalRegion(const QRegion &deviceRegion,
                                                   qreal dpr,
                                                   const QSize &logicalLimit)
{
    if (deviceRegion.isEmpty() || logicalLimit.isEmpty())
        return {};
    if (dpr <= 0.0 || qFuzzyCompare(dpr, 1.0))
        return deviceRegion & QRect(QPoint(0, 0), logicalLimit);

    const QRect bounds(QPoint(0, 0), logicalLimit);
    QRegion logical;
    for (const QRect &dr : deviceRegion) {
        const int lx0 = int(std::floor(qreal(dr.x()) / dpr));
        const int ly0 = int(std::floor(qreal(dr.y()) / dpr));
        const int lx1 = int(std::ceil(qreal(dr.x() + dr.width()) / dpr));
        const int ly1 = int(std::ceil(qreal(dr.y() + dr.height()) / dpr));
        const QRect lr = QRect(lx0, ly0, std::max(0, lx1 - lx0), std::max(0, ly1 - ly0)) & bounds;
        if (!lr.isEmpty())
            logical += lr;
    }
    return logical;
}

QRegion QPhaseDamageTracker::diffAndAdvance(const QPhaseDamageFrameSnapshot &snap,
                                            bool *outIsFullDamage)
{
    if (snap.pixelSize.isEmpty()) {
        if (outIsFullDamage)
            *outIsFullDamage = false;
        return {};
    }

    const int vpW = snap.pixelSize.width();
    const int vpH = snap.pixelSize.height();
    const QRect fullRect(0, 0, vpW, vpH);

    const bool globalInvalid = !m_hasValidState
            || m_arenaId != snap.arenaId
            || m_pixelSize != snap.pixelSize
            || !qFuzzyCompare(m_dpr, snap.dpr)
            || m_clearColor != snap.clearColor
            || !qFuzzyCompare(m_textStemDarkening, snap.textStemDarkening);

    if (!globalInvalid && m_revision == snap.revision && !snap.hasAnimations) {
        if (outIsFullDamage)
            *outIsFullDamage = false;
        return {};
    }

    if (globalInvalid) {
        m_hasValidState = true;
        m_arenaId = snap.arenaId;
        m_revision = snap.revision;
        m_pixelSize = snap.pixelSize;
        m_dpr = snap.dpr;
        m_clearColor = snap.clearColor;
        m_textStemDarkening = snap.textStemDarkening;
        m_prevSlots = snap.primSlots;
        m_prevVisibleOrder = snap.visibleOrder;
        if (outIsFullDamage)
            *outIsFullDamage = true;
        return QRegion(fullRect);
    }

    QRegion dirty;
    auto addDirtyRect = [&](const QRect &r) {
        if (r.isEmpty())
            return;
        if (dirty.rectCount() >= 24)
            dirty = QRegion(dirty.boundingRect().united(r));
        else
            dirty += r;
    };

    std::vector<uint32_t> survivingCurOrder;
    survivingCurOrder.reserve(snap.visibleOrder.size());

    for (uint32_t idx : snap.visibleOrder) {
        const PhasePrimDamageSlot &cur = snap.primSlots[idx];
        if (idx >= m_prevSlots.size() || !m_prevSlots[idx].active) {
            addDirtyRect(cur.devBounds);
        } else {
            survivingCurOrder.push_back(idx);
            if (m_prevSlots[idx].contentHash != cur.contentHash) {
                addDirtyRect(m_prevSlots[idx].devBounds);
                addDirtyRect(cur.devBounds);
            }
        }
    }

    std::vector<uint32_t> survivingPrevOrder;
    survivingPrevOrder.reserve(survivingCurOrder.size());
    for (uint32_t oldIdx : m_prevVisibleOrder) {
        if (oldIdx >= snap.primSlots.size() || !snap.primSlots[oldIdx].active) {
            if (oldIdx < m_prevSlots.size() && m_prevSlots[oldIdx].active)
                addDirtyRect(m_prevSlots[oldIdx].devBounds);
        } else {
            survivingPrevOrder.push_back(oldIdx);
        }
    }

    if (survivingPrevOrder.size() == survivingCurOrder.size()) {
        for (size_t k = 0; k < survivingCurOrder.size(); ++k) {
            if (survivingPrevOrder[k] != survivingCurOrder[k])
                addDirtyRect(snap.primSlots[survivingCurOrder[k]].devBounds);
        }
    } else {
        dirty = QRegion(fullRect);
    }

    dirty &= fullRect;

    m_revision = snap.revision;
    m_prevSlots = snap.primSlots;
    m_prevVisibleOrder = snap.visibleOrder;

    const quint64 totalPx = quint64(vpW) * quint64(vpH);
    const quint64 dirtyPx = regionPixelArea(dirty);
    const bool isFull = (dirtyPx * 10 >= totalPx * 9);
    if (outIsFullDamage)
        *outIsFullDamage = isFull;
    return isFull ? QRegion(fullRect) : dirty;
}

QRegion QPhaseDamageTracker::computeAndAdvance(QPhaseSceneArena &arena,
                                               const QSize &pixelSize,
                                               qreal devicePixelRatio,
                                               uint64_t frameTimeNs,
                                               bool *outIsFullDamage,
                                               QPhaseRendererAffinity targetBackend)
{
    if (isUnchanged(arena, pixelSize, devicePixelRatio)) {
        if (outIsFullDamage)
            *outIsFullDamage = false;
        return {};
    }
    const QPhaseDamageFrameSnapshot snap = QPhaseDamageFrameSnapshot::capture(
            arena, pixelSize, devicePixelRatio, frameTimeNs, targetBackend);
    return diffAndAdvance(snap, outIsFullDamage);
}

QT_END_NAMESPACE
