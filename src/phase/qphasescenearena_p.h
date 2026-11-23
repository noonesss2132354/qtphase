// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASESCENEARENA_P_H
#define QPHASESCENEARENA_P_H

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
#include <QtPhase/qphasestyles.h>
#include <QtCore/qhash.h>
#include <QtCore/qrect.h>
#include <QtCore/qstring.h>
#include <QtGui/qcolor.h>
#include <QtGui/qimage.h>
#include <QtGui/qrawfont.h>
#include <QtGui/qregion.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct hb_face_t;
struct hb_font_t;
struct hb_gpu_draw_t;

QT_BEGIN_NAMESPACE

class QPhaseView;
class QPhaseCpuRasterizer;

enum class PhasePrimKind : uint32_t {
    SolidRect     = 0,
    RoundedBox    = 1,
    BoxShadow     = 2,
    Image         = 3,
    HbGpuCurve    = 4,
    StrokeSegment = 5, // Analytical capsule/butt line segment: (x,y)->(w,h), width=p0, buttCap=p1
    PushLayer     = 6, // Isolated in-register layer push: bounds=(x,y,w,h), opacity=c0a, blendMode=resourceIndex
    PopLayer      = 7, // Isolated in-register layer pop:  bounds=(x,y,w,h), opacity=c0a, blendMode=resourceIndex
};

// High flag bits packed into PhaseGpuPrimitive::animMode (bits 0..23 hold QPhaseAnimation::Mode):
static constexpr uint32_t kPhaseAnimFlagGpuOnly      = 0x20000000u; // Skipped by CPU rasterizer
static constexpr uint32_t kPhaseAnimFlagCpuOnly      = 0x40000000u; // Skipped by GPU compute prep
static constexpr uint32_t kPhaseAnimFlagInsideLayer  = 0x80000000u; // Prevents lastFullOccluder culling PushLayer
static constexpr uint32_t kPhaseAnimAffinityMask     = (kPhaseAnimFlagGpuOnly | kPhaseAnimFlagCpuOnly);

struct PhaseRendererRequirements {
    uint32_t gpuOnlyViewCount = 0;
    uint32_t gpuBlockingViewCount = 0;
    uint32_t cpuOnlyViewCount = 0;
    uint32_t cpuBlockingViewCount = 0;

    bool forcesGpu() const noexcept { return gpuOnlyViewCount > 0; }
    bool forcesCpu() const noexcept { return gpuOnlyViewCount == 0 && cpuOnlyViewCount > 0; }
    bool hasConflict() const noexcept { return gpuOnlyViewCount > 0 && cpuOnlyViewCount > 0; }
    bool blocksUntilGpuReady() const noexcept { return gpuBlockingViewCount > 0; }
    bool blocksUntilCpuReady() const noexcept { return cpuBlockingViewCount > 0; }
};

struct PhaseTransformNode {
    uint32_t parentIndex = 0;
    float localTx = 0.0f;
    float localTy = 0.0f;
    float localM00 = 1.0f;
    float localM01 = 0.0f;
    float localM10 = 0.0f;
    float localM11 = 1.0f;
    float localOpacity = 1.0f;
    bool active = false;
};

struct PhaseResolvedTransform {
    float tx = 0.0f;
    float ty = 0.0f;
    float m00 = 1.0f;
    float m01 = 0.0f;
    float m10 = 0.0f;
    float m11 = 1.0f;
    float opacity = 1.0f;
    uint32_t flags = 0; // bit 0: non-axis-aligned 2x2 rotation/skew (|m01| > 1e-6 || |m10| > 1e-6)
};
static_assert(sizeof(PhaseResolvedTransform) == 32, "PhaseResolvedTransform must match GLSL ResolvedTransform");

struct PhaseClipNode {
    uint32_t parentClipIndex = 0;
    uint32_t transformIndex = 0;
    float localX = 0.0f;
    float localY = 0.0f;
    float localW = 1e8f;
    float localH = 1e8f;
    QPhaseCorners corners{};
    bool active = false;
};

struct PhaseResolvedClip {
    // Tight intersected AABB across all ancestor clips (O(1) tile culling & rectangular clip fast-path)
    float cullMinX = -1e6f;
    float cullMinY = -1e6f;
    float cullMaxX = 1e6f;
    float cullMaxY = 1e6f;
    // Self transformed clip bounds & corner radii (exact nested rounded-clip chain evaluation)
    float selfMinX = -1e6f;
    float selfMinY = -1e6f;
    float selfMaxX = 1e6f;
    float selfMaxY = 1e6f;
    float rTL = 0.0f;
    float rTR = 0.0f;
    float rBR = 0.0f;
    float rBL = 0.0f;
    uint32_t parentRoundedClipIndex = 0;
    uint32_t hasRoundedAncestorOrSelf = 0;
};
static_assert(sizeof(PhaseResolvedClip) == 56, "PhaseResolvedClip must match GLSL ResolvedClip");

struct PhaseGpuPrimitive {
    uint32_t kind = 0;
    uint32_t transformIndex = 0;
    uint32_t clipIndex = 0;
    uint32_t resourceIndex = 0;

    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float c0r = 0.0f, c0g = 0.0f, c0b = 0.0f, c0a = 1.0f;
    float c1r = 0.0f, c1g = 0.0f, c1b = 0.0f, c1a = 0.0f;
    float br = 0.0f, bg = 0.0f, bb = 0.0f, ba = 0.0f;

    float rTL = 0.0f, rTR = 0.0f, rBR = 0.0f, rBL = 0.0f;

    float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f, p3 = 0.0f, p4 = 0.0f, p5 = 0.0f;

    uint32_t animMode = 0;
    uint32_t animDurationMs = 0;
    uint64_t animStartNs = 0;
    float animFromOpacity = 1.0f;
    float animToOpacity = 1.0f;
};
static_assert(sizeof(PhaseGpuPrimitive) == 144, "PhaseGpuPrimitive must match GLSL Primitive");

using PhaseGpuSurfaceCallback = std::function<void(const QPhaseGpuSurfaceContext &)>;

struct PhaseImageEntry {
    qint64 sourceCacheKey = 0;
    int width = 0;
    int height = 0;
    QImage cpuRgbaPremul; // Cleared once uploaded and cut over to GPU!
    std::shared_ptr<PhaseGpuSurfaceCallback> gpuSurfaceCallback;
    uint64_t generation = 1;
    uint64_t gpuContentRevision = 1;
    bool isGpuSurface = false;
    bool gpuNeedsDepth = false;
    bool uploadedToGpu = false;
    bool active = false;
};

struct PhasePathCmd {
    enum class Op : uint8_t { MoveTo, LineTo, QuadTo, CubicTo, Close };
    Op op = Op::MoveTo;
    float x0 = 0.0f, y0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
    float x2 = 0.0f, y2 = 0.0f;
};

struct PhaseCurveEntry {
    uint32_t texelOffset = 0; // Offset in 8-byte texels into m_curveTexels
    uint32_t texelCount = 0;
    float xMin = 0.0f;
    float yMin = 0.0f;
    float xMax = 0.0f;
    float yMax = 0.0f;
    float unitsPerEm = 1000.0f;
    bool isGlyph = false;
    hb_font_t *unscaledHbFont = nullptr;
    uint32_t glyphId = 0;
    std::vector<PhasePathCmd> cpuVectorCmds; // Freed upon GPU cutover!
};

struct PhaseColorGlyphLayer {
    uint32_t curveId = 0;
    QColor color;
    bool useForegroundColor = false;
};

struct PhaseBitmapGlyphEntry {
    uint32_t imageSlot = 0;
    QRectF pixelBounds; // Relative to glyph baseline origin at registered pixelSize
    bool valid = false;
};

struct PhaseCpuBridgeEntry {
    uint32_t imageSlot = 0;
    uint32_t selfTransformSlot = 0;
    uint32_t inheritedClipSlot = 0;
    float logicalWidth = 0.0f;
    float logicalHeight = 0.0f;
    std::vector<uint32_t> localPrimOrder;
    bool hasSubtreeAnimations = false;
};

struct PhaseCpuBridgeCacheState {
    uint64_t lastSubtreeHash = 0;
    qreal lastDpr = 0.0;
};

class Q_PHASE_EXPORT QPhaseSceneArena
{
public:
    QPhaseSceneArena();
    ~QPhaseSceneArena();

    Q_DISABLE_COPY_MOVE(QPhaseSceneArena)

    void setClearColor(const QColor &color);
    QColor clearColor() const noexcept { return m_clearColor; }

    void setTextStemDarkening(float strength) noexcept;
    float textStemDarkening() const noexcept { return m_textStemDarkening; }

    uint32_t allocateTransform(uint32_t parentIndex = 0);
    void updateTransform(uint32_t index, float tx, float ty, float sx = 1.0f, float sy = 1.0f, float opacity = 1.0f);
    void updateTransform2D(uint32_t index,
                           float m00, float m01, float m10, float m11,
                           float tx, float ty, float opacity = 1.0f);
    void setTransformParent(uint32_t index, uint32_t parentIndex);
    void freeTransform(uint32_t index);

    uint32_t allocateClip(uint32_t parentClipIndex, uint32_t transformIndex, const QRectF &localRect, const QPhaseCorners &corners);
    void updateClip(uint32_t index, const QRectF &localRect, const QPhaseCorners &corners);
    void freeClip(uint32_t index);

    uint32_t allocatePrimitive(const PhaseGpuPrimitive &prim);
    void updatePrimitive(uint32_t index, const PhaseGpuPrimitive &prim);
    void freePrimitive(uint32_t index);

    void setDrawOrder(std::vector<uint32_t> &&order);
    void syncRootView(QPhaseView *rootView);
    void resolveHierarchy();
    void syncRenderSnapshotFrom(QPhaseSceneArena &src);

    // Stage 3: Retained bindless images & custom GPU 3D/shader surfaces with free-list slot recycling!
    uint32_t registerOrGetImage(const QImage &image);
    uint32_t allocateGpuSurface(int logicalWidth, int logicalHeight, bool needsDepth,
                                std::shared_ptr<PhaseGpuSurfaceCallback> callback);
    void updateGpuSurface(uint32_t slot, int logicalWidth, int logicalHeight, bool needsDepth,
                          std::shared_ptr<PhaseGpuSurfaceCallback> callback, bool markContentDirty);
    void markGpuSurfaceDirty(uint32_t slot);
    void unregisterImage(uint32_t slot);
    void unregisterImageByKey(qint64 sourceCacheKey);
    std::vector<PhaseImageEntry> &images() noexcept { return m_images; }
    const std::vector<PhaseImageEntry> &images() const noexcept { return m_images; }
    size_t activeImageSlotCount() const noexcept;
    size_t activeCpuImageBytes() const noexcept;

    const PhaseRendererRequirements &rendererRequirements() const noexcept { return m_rendererReqs; }
    void setRendererRequirements(const PhaseRendererRequirements &reqs) noexcept { m_rendererReqs = reqs; }

    // CPU-to-GPU Subtree Bridge: rasterizes CpuOnly widget subtrees via QPhaseCpuRasterizer into
    // bindless image slots when a GpuOnly widget in the same window forces Vulkan steady state!
    uint32_t allocateCpuBridgeSlot();
    void registerCpuBridge(PhaseCpuBridgeEntry &&entry);
    const std::vector<PhaseCpuBridgeEntry> &cpuBridges() const noexcept { return m_cpuBridges; }
    void rasterizeDirtyCpuBridges(QPhaseCpuRasterizer &cpuRasterizer,
                                  qreal devicePixelRatio,
                                  uint64_t frameTimeNs);

    // Stage 4: Retained hb-gpu / hb-raster glyphs, COLRv0 layers, CBDT/COLRv1 color bitmaps & SVG vectors
    uint32_t registerOrGetGlyph(const QRawFont &rawFont, uint32_t glyphId);
    bool tryGetColorGlyphLayers(const QRawFont &rawFont, uint32_t glyphId, std::vector<PhaseColorGlyphLayer> &outLayers);
    PhaseBitmapGlyphEntry tryRegisterBitmapColorGlyph(const QRawFont &rawFont, uint32_t glyphId);
    uint32_t registerOrGetSvgPath(QStringView svgPathData, const QRectF &viewBox);
    const std::vector<PhaseCurveEntry> &curves() const noexcept { return m_curves; }
    const std::vector<uint64_t> &curveTexels() const noexcept { return m_curveTexels; }

    void markEnteredGpuSteadyState();
    void resetGpuSteadyState();
    void releaseUploadedCpuImagePixels();
    void acknowledgeGpuImagesUploaded(uint64_t upToGeneration);

    const std::vector<PhaseResolvedTransform> &resolvedTransforms() const noexcept { return m_resolvedTransforms; }
    const std::vector<PhaseResolvedClip> &resolvedClips() const noexcept { return m_resolvedClips; }
    const std::vector<PhaseGpuPrimitive> &primitives() const noexcept { return m_primitives; }
    const std::vector<uint64_t> &primitivePageRevisions() const noexcept { return m_primPageRevisions; }
    const std::vector<uint32_t> &drawOrder() const noexcept { return m_drawOrder; }

    uint64_t arenaId() const noexcept { return m_arenaId; }
    uint64_t revision() const noexcept { return m_revision; }
    uint64_t transformClipRevision() const noexcept { return m_transformClipRevision; }
    uint64_t primitiveRevision() const noexcept { return m_primitiveRevision; }
    uint64_t drawOrderRevision() const noexcept { return m_drawOrderRevision; }
    uint64_t imageTableRevision() const noexcept { return m_imageTableRevision; }
    uint64_t monotonicTimestampNs() const noexcept;
    bool hasActiveAnimations() const noexcept { return m_activeAnimCount > 0; }

private:
    QColor m_clearColor{18, 20, 26, 255};
    float m_textStemDarkening = 0.0f;

    std::vector<PhaseTransformNode> m_transformNodes;
    std::vector<PhaseResolvedTransform> m_resolvedTransforms;
    std::vector<uint32_t> m_freeTransforms;

    std::vector<PhaseClipNode> m_clipNodes;
    std::vector<PhaseResolvedClip> m_resolvedClips;
    std::vector<uint32_t> m_freeClips;

    std::vector<PhaseGpuPrimitive> m_primitives;
    std::vector<uint64_t> m_primPageRevisions;
    std::vector<uint32_t> m_freePrimitives;
    std::vector<uint32_t> m_drawOrder;

    // Stage 3 images + bindless free-list
    std::vector<PhaseImageEntry> m_images;
    std::vector<uint32_t> m_freeImageSlots;
    QHash<qint64, uint32_t> m_imageCacheMap;
    uint64_t m_nextImageGen = 1;

    // Stage 4 hb-gpu curve blobs & color glyph caches
    hb_gpu_draw_t *m_hbGpuDraw = nullptr;
    std::vector<uint64_t> m_curveTexels;
    std::vector<PhaseCurveEntry> m_curves;
    QHash<hb_face_t *, hb_font_t *> m_unscaledHbFonts;
    QHash<uint64_t, uint32_t> m_glyphCurveMap;
    QHash<uint64_t, PhaseBitmapGlyphEntry> m_bitmapGlyphMap;
    QHash<QString, uint32_t> m_svgCurveMap;

    bool m_transformsDirty = true;
    bool m_clipsDirty = true;
    bool m_inGpuSteadyState = false;
    PhaseRendererRequirements m_rendererReqs{};
    std::vector<PhaseCpuBridgeEntry> m_cpuBridges;
    QHash<uint32_t, PhaseCpuBridgeCacheState> m_cpuBridgeCache;
    uint32_t m_activeAnimCount = 0;
    uint64_t m_arenaId = 0;
    uint64_t m_revision = 1;
    uint64_t m_transformClipRevision = 1;
    uint64_t m_primitiveRevision = 1;
    uint64_t m_drawOrderRevision = 1;
    uint64_t m_imageTableRevision = 1;
};

struct PhasePrimDamageSlot {
    uint64_t contentHash = 0;
    QRect devBounds;
    bool active = false;
};

// Pre-hashed single-pass scene snapshot shared across swapchain[i] and surface damage trackers!
struct Q_PHASE_EXPORT QPhaseDamageFrameSnapshot {
    uint64_t arenaId = 0;
    uint64_t revision = 0;
    QSize pixelSize;
    qreal dpr = 1.0;
    QColor clearColor;
    float textStemDarkening = 0.0f;
    bool hasAnimations = false;
    std::vector<PhasePrimDamageSlot> primSlots;
    std::vector<uint32_t> visibleOrder;

    static QPhaseDamageFrameSnapshot capture(QPhaseSceneArena &arena,
                                             const QSize &pixelSize,
                                             qreal devicePixelRatio,
                                             uint64_t frameTimeNs,
                                             QPhaseRendererAffinity targetBackend = QPhaseRendererAffinity::Any);
};

class Q_PHASE_EXPORT QPhaseDamageTracker
{
public:
    void reset() noexcept;

    QRegion computeAndAdvance(QPhaseSceneArena &arena,
                              const QSize &pixelSize,
                              qreal devicePixelRatio,
                              uint64_t frameTimeNs,
                              bool *outIsFullDamage = nullptr,
                              QPhaseRendererAffinity targetBackend = QPhaseRendererAffinity::Any);

    QRegion diffAndAdvance(const QPhaseDamageFrameSnapshot &snapshot,
                           bool *outIsFullDamage = nullptr);

    bool isUnchanged(const QPhaseSceneArena &arena,
                     const QSize &pixelSize,
                     qreal devicePixelRatio) const noexcept;

    static quint64 regionPixelArea(const QRegion &region) noexcept;
    static QRegion deviceToLogicalRegion(const QRegion &deviceRegion, qreal dpr, const QSize &logicalLimit);

private:
    bool m_hasValidState = false;
    uint64_t m_arenaId = 0;
    uint64_t m_revision = 0;
    QSize m_pixelSize;
    qreal m_dpr = 1.0;
    QColor m_clearColor;
    float m_textStemDarkening = 0.0f;
    std::vector<PhasePrimDamageSlot> m_prevSlots;
    std::vector<uint32_t> m_prevVisibleOrder;
};

QT_END_NAMESPACE

#endif // QPHASESCENEARENA_P_H
