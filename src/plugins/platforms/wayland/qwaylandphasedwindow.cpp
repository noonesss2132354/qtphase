// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwaylandphasedwindow_p.h"
#include "qwaylandshmbackingstore_p.h"
#include "qwaylanddisplay_p.h"

#if QT_CONFIG(vulkan)
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#endif

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

QWaylandPhasedWindow::QWaylandPhasedWindow(QWindow *window, QWaylandDisplay *display)
    : QWaylandWindow(window, display)
{
    mSurfaceFormat.setRedBufferSize(8);
    mSurfaceFormat.setGreenBufferSize(8);
    mSurfaceFormat.setBlueBufferSize(8);
    const QSurfaceFormat reqFormat = window->requestedFormat();
    mSurfaceFormat.setAlphaBufferSize(reqFormat.hasAlpha() ? 8 : 0);
}

QWaylandPhasedWindow::~QWaylandPhasedWindow()
{
    releasePhasedBootResources();
    invalidateSurface();
}

QWaylandWindow::WindowType QWaylandPhasedWindow::windowType() const
{
    return m_bootReleased ? QWaylandWindow::Vulkan : QWaylandWindow::Shm;
}

void QWaylandPhasedWindow::publishConfiguredSizeLocked(const QSize &logicalSize) noexcept
{
    m_configuredLogicalSize = logicalSize;
    if (logicalSize.width() > 0 && logicalSize.height() > 0) {
        const uint64_t key = (uint64_t(uint32_t(logicalSize.width())) << 32)
                | uint64_t(uint32_t(logicalSize.height()));
        m_configuredLogicalKey.store(key, std::memory_order_release);
    } else {
        m_configuredLogicalKey.store(0, std::memory_order_release);
    }
}

void QWaylandPhasedWindow::willUpdateGeometryProtocol()
{
    m_waylandCommitMutex.lock();
}

void QWaylandPhasedWindow::didUpdateGeometryProtocol()
{
    publishConfiguredSizeLocked(geometry().size());
    m_waylandCommitMutex.unlock();
}

void QWaylandPhasedWindow::willApplyConfigure()
{
    m_configureInProgress.store(true, std::memory_order_release);
    m_waylandCommitMutex.lock();
}

void QWaylandPhasedWindow::didApplyConfigure()
{
    publishConfiguredSizeLocked(geometry().size());
    m_configureInProgress.store(false, std::memory_order_release);
    m_waylandCommitMutex.unlock();
}

void QWaylandPhasedWindow::invalidateSurface()
{
    {
        QMutexLocker<QRecursiveMutex> lock(&m_waylandCommitMutex);
        m_surfaceAlive.store(false, std::memory_order_release);
    }
    releasePhasedBootResources();
    m_bootReleased = false;
#if QT_CONFIG(vulkan)
    if (m_vkSurface != VK_NULL_HANDLE) {
        if (QVulkanInstance *inst = window()->vulkanInstance()) {
            if (inst->handle())
                static_cast<QWaylandVulkanInstance *>(inst->handle())->destroySurface(m_vkSurface);
        }
        m_vkSurface = VK_NULL_HANDLE;
    }
#endif
    QWaylandWindow::invalidateSurface();
}

bool QWaylandPhasedWindow::acquirePhasedBootBuffer(const QSize &pixelSize, qreal dpr, PhasedBootBuffer *outBuffer)
{
    if (m_bootReleased || !outBuffer || pixelSize.isEmpty())
        return false;

    // Prune any idle buffers whose dimensions no longer match
    for (qsizetype i = m_bootBuffers.size() - 1; i >= 0; --i) {
        QWaylandShmBuffer *buf = m_bootBuffers[i];
        if (!buf->busy() && buf->size() != pixelSize) {
            m_bootBuffers.removeAt(i);
            delete buf;
        }
    }

    QWaylandShmBuffer *chosen = nullptr;
    for (QWaylandShmBuffer *candidate : std::as_const(m_bootBuffers)) {
        if (!candidate->busy() && candidate->size() == pixelSize) {
            chosen = candidate;
            break;
        }
    }

    static constexpr int kMaxBootBuffers = 4;
    if (!chosen && m_bootBuffers.size() < kMaxBootBuffers) {
        const QImage::Format fmt = format().hasAlpha()
                ? QImage::Format_ARGB32_Premultiplied
                : QImage::Format_RGB32;
        auto *created = new QWaylandShmBuffer(mDisplay, pixelSize, fmt, dpr, nullptr);
        if (created->image()->isNull() || !created->buffer()) {
            delete created;
            return false;
        }
        m_bootBuffers.push_back(created);
        chosen = created;
    }

    if (!chosen)
        return false;

    QImage *img = chosen->image();
    img->setDevicePixelRatio(dpr);
    outBuffer->data = img->bits();
    outBuffer->width = img->width();
    outBuffer->height = img->height();
    outBuffer->bytesPerLine = int(img->bytesPerLine());
    outBuffer->devicePixelRatio = dpr;
    outBuffer->handle = chosen;
    return true;
}

void QWaylandPhasedWindow::presentPhasedBootBuffer(void *bufferHandle, const QRegion &damage)
{
    if (m_bootReleased || !bufferHandle || !isExposed() || !wlSurface())
        return;

    auto *shmBuf = static_cast<QWaylandShmBuffer *>(bufferHandle);
    if (!m_bootBuffers.contains(shmBuf))
        return;

    const QRegion effectiveDamage = damage.isEmpty()
            ? QRegion(QRect(QPoint(0, 0), geometry().size()))
            : damage;
    safeCommit(shmBuf, effectiveDamage);
    mDisplay->flushRequests();
}

void QWaylandPhasedWindow::releasePhasedBootResources()
{
    m_bootReleased = true;
    {
        QMutexLocker lock(&mFrameSyncMutex);
        if (mFrameCallback) {
            wl_callback_destroy(mFrameCallback);
            mFrameCallback = nullptr;
        }
        mFrameCallbackElapsedTimer.invalidate();
        mWaitingForFrameCallback = false;
    }
    if (mFrameCallbackCheckIntervalTimerId != -1) {
        killTimer(mFrameCallbackCheckIntervalTimerId);
        mFrameCallbackCheckIntervalTimerId = -1;
    }
    mFrameCallbackTimedOut = false;
    updateExposure();
    for (QWaylandShmBuffer *buf : std::as_const(m_bootBuffers)) {
        if (buf->busy()) {
            // Wayland compositor still holds this wl_buffer until the new Vulkan DMA-BUF
            // commit replaces it on VSync; QWaylandBuffer::release will delete it automatically.
            buf->setDeleteOnRelease(true);
        } else {
            delete buf;
        }
    }
    m_bootBuffers.clear();
}

bool QWaylandPhasedWindow::createPhasedVulkanSurface(void *vkInstance, void *vkGetInstanceProcAddrFn, void *vkSurfaceOut)
{
#if QT_CONFIG(vulkan)
    if (!vkInstance || !vkGetInstanceProcAddrFn || !vkSurfaceOut)
        return false;

    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddrFn);
    auto inst = reinterpret_cast<VkInstance>(vkInstance);
    auto createSurfaceFn = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
            getProc(inst, "vkCreateWaylandSurfaceKHR"));
    if (!createSurfaceFn)
        return false;

    ::wl_surface *wlSurf = wlSurface();
    ::wl_display *wlDisp = mDisplay ? mDisplay->wl_display() : nullptr;
    if (!wlSurf || !wlDisp)
        return false;

    VkWaylandSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    createInfo.display = wlDisp;
    createInfo.surface = wlSurf;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (createSurfaceFn(inst, &createInfo, nullptr, &surface) != VK_SUCCESS)
        return false;

    {
        QMutexLocker<QRecursiveMutex> lock(&m_waylandCommitMutex);
        m_surfaceAlive.store(true, std::memory_order_release);
        publishConfiguredSizeLocked(geometry().size());
    }

    *static_cast<VkSurfaceKHR *>(vkSurfaceOut) = surface;
    return true;
#else
    Q_UNUSED(vkInstance);
    Q_UNUSED(vkGetInstanceProcAddrFn);
    Q_UNUSED(vkSurfaceOut);
    return false;
#endif
}

bool QWaylandPhasedWindow::checkPhasedVulkanPresentSupport(void *vkInstance, void *vkPhysicalDevice,
                                                           quint32 queueFamilyIndex, void *vkGetInstanceProcAddrFn)
{
#if QT_CONFIG(vulkan)
    if (!vkInstance || !vkPhysicalDevice || !vkGetInstanceProcAddrFn || !mDisplay)
        return false;

    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddrFn);
    auto inst = reinterpret_cast<VkInstance>(vkInstance);
    auto physDev = reinterpret_cast<VkPhysicalDevice>(vkPhysicalDevice);

    auto presSupportFn = reinterpret_cast<PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR>(
            getProc(inst, "vkGetPhysicalDeviceWaylandPresentationSupportKHR"));
    if (!presSupportFn)
        return false;

    return presSupportFn(physDev, queueFamilyIndex, mDisplay->wl_display()) == VK_TRUE;
#else
    Q_UNUSED(vkInstance);
    Q_UNUSED(vkPhysicalDevice);
    Q_UNUSED(queueFamilyIndex);
    Q_UNUSED(vkGetInstanceProcAddrFn);
    return false;
#endif
}

void QWaylandPhasedWindow::preparePhasedGpuPresent()
{
    mExposeEventNeedsAttachedBuffer = false;
}

bool QWaylandPhasedWindow::isPhasedConfigureInProgress() const
{
    return m_configureInProgress.load(std::memory_order_acquire);
}

bool QWaylandPhasedWindow::isPhasedSizeCurrent(const QSize &pixelSize, qreal dpr) const
{
    if (!m_surfaceAlive.load(std::memory_order_acquire)
        || m_configureInProgress.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t key = m_configuredLogicalKey.load(std::memory_order_acquire);
    if (key == 0)
        return true;
    const QSize expectedLogical(int(uint32_t(key >> 32)), int(uint32_t(key & 0xFFFFFFFFu)));
    const QSize expectedPx = (expectedLogical * dpr).expandedTo(QSize(1, 1));
    return expectedPx == pixelSize;
}

bool QWaylandPhasedWindow::beginPhasedGpuPresent(const QSize &pixelSize, qreal dpr)
{
    if (m_configureInProgress.load(std::memory_order_acquire))
        return false;

    m_waylandCommitMutex.lock();
    const QSize expectedLogical = m_configuredLogicalSize.isEmpty()
            ? geometry().size()
            : m_configuredLogicalSize;
    const QSize expectedPx = (expectedLogical * dpr).expandedTo(QSize(1, 1));
    if (!m_surfaceAlive.load(std::memory_order_acquire) || !isExposed() || !wlSurface()
        || (!expectedLogical.isEmpty() && expectedPx != pixelSize)) {
        m_waylandCommitMutex.unlock();
        return false;
    }

    mExposeEventNeedsAttachedBuffer = false;
    return true;
}

void QWaylandPhasedWindow::endPhasedGpuPresent()
{
    m_waylandCommitMutex.unlock();
}

#if QT_CONFIG(vulkan)
VkSurfaceKHR *QWaylandPhasedWindow::vkSurface()
{
    if (m_vkSurface != VK_NULL_HANDLE)
        return &m_vkSurface;

    QVulkanInstance *vulkanInstance = window()->vulkanInstance();
    if (!vulkanInstance || !vulkanInstance->handle())
        return nullptr;

    auto *waylandVulkanInstance = static_cast<QWaylandVulkanInstance *>(vulkanInstance->handle());
    m_vkSurface = waylandVulkanInstance->createSurface(this);
    return &m_vkSurface;
}
#endif

} // namespace QtWaylandClient

QT_END_NAMESPACE
