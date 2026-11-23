// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEVULKANBACKEND_P_H
#define QPHASEVULKANBACKEND_P_H

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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE

class QPlatformWindow;
class QPhaseSceneArena;
struct QPhaseVulkanBackendData;

class Q_PHASE_EXPORT QPhaseVulkanBackend
{
public:
    struct GpuFrameMetrics {
        qint64 cpuSubmitWorkUs = 0;
        qint64 gpuHardwareUs = 0;
        quint64 damagedPixels = 0;
        quint64 syncedSceneBytes = 0;
        uint64_t uploadedImageGen = 0;
    };

    QPhaseVulkanBackend();
    ~QPhaseVulkanBackend();

    Q_DISABLE_COPY_MOVE(QPhaseVulkanBackend)

    // Process-wide lock-free/fast check: true once window #1 has warmed up a hardware Vulkan 1.3 WSI device
    static bool isSharedHardwareDeviceReady() noexcept;

    // Safe to execute on a background worker thread (no Wayland/QWindow calls).
    // Reuses a process-wide shared VkDevice + VmaAllocator + VkPipeline + R8_UNORM glyph atlas singleton!
    bool initializeDevice(bool enableWaylandWsi);
    bool isDeviceReady() const noexcept;
    bool isHardwareGpu() const noexcept;
    bool supportsDmaBufExport() const noexcept;
    bool usedSharedDeviceCache() const noexcept;
    quint64 sharedDeviceHandle() const noexcept;
    uint32_t sharedDeviceUseCount() const noexcept;

    // Zero-swapchain bucketed Vulkan DMA-BUF wl_buffer scanout (atomic main-thread Wayland commits!)
    void releaseDmaBufRing(QPlatformWindow *platformWindow);
    bool renderAndPresentDmaBuf(QPlatformWindow *platformWindow,
                                QPhaseSceneArena &arena,
                                const QSize &pixelSize,
                                const QSize &logicalSize,
                                qreal devicePixelRatio,
                                bool hasAlpha,
                                uint64_t frameTimeNs,
                                GpuFrameMetrics *outMetrics = nullptr,
                                bool waitForGpuCompletion = false);

    // Attaches VkSurfaceKHR + VkSwapchainKHR when claiming the Wayland surface (fallback path)
    bool attachWindow(QPlatformWindow *platformWindow, const QSize &pixelSize, bool hasAlpha);

    // Headless VRAM render + VkFence completion + hardware timestamp query (no Wayland calls, no CPU readback)
    // used while racing the CPU worker before claiming wl_surface ownership.
    bool benchmarkFrameOffscreen(QPhaseSceneArena &arena,
                                 const QSize &pixelSize,
                                 qreal devicePixelRatio,
                                 bool hasAlpha,
                                 uint64_t frameTimeNs,
                                 GpuFrameMetrics *outMetrics = nullptr);
    void releaseRaceResources();

    // Syncs QPhaseSceneArena tables incrementally into UMA/ReBAR BDA memory, uploads any new images via
    // batched staging in cmdBuf, bakes any new curves via 1D 8x8 blocks, runs 2-stage O(drawCount)
    // coarse-to-fine compute tile binning, renders single-pass fullscreen, and presents.
    bool renderAndPresentWindow(QPlatformWindow *platformWindow,
                                QPhaseSceneArena &arena,
                                const QSize &pixelSize,
                                qreal devicePixelRatio,
                                bool hasAlpha,
                                uint64_t frameTimeNs,
                                GpuFrameMetrics *outMetrics = nullptr,
                                bool waitForGpuCompletion = false,
                                const std::function<void(const GpuFrameMetrics &)> &onGpuPixelsReady = {},
                                const std::atomic<uint64_t> *expectedSizeKey = nullptr);

    quint64 glyphCacheHits() const noexcept;
    quint64 glyphCacheMisses() const noexcept;

    // Headless offscreen Vulkan 1.3 render + staging readback for PNG-diff verification
    QImage renderOffscreenToImage(QPhaseSceneArena &arena,
                                  const QSize &pixelSize,
                                  qreal devicePixelRatio = 1.0,
                                  uint64_t frameTimeNs = 0);

private:
    bool syncArenaToGpu(QPhaseSceneArena &arena,
                        uint32_t frameSlot,
                        qreal devicePixelRatio,
                        uint64_t frameTimeNs,
                        quint64 *outSyncedBytes = nullptr);
    bool recreateSwapchain(const QSize &pixelSize, bool hasAlpha);

    std::unique_ptr<QPhaseVulkanBackendData> d;
};

QT_END_NAMESPACE

#endif // QPHASEVULKANBACKEND_P_H
