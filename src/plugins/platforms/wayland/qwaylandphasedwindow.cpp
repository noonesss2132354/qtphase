// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwaylandphasedwindow_p.h"
#include "qwaylandbuffer_p.h"
#include "qwaylandshmbackingstore_p.h"
#include "qwaylanddisplay_p.h"
#include "qwaylandsurface_p.h"
#include "qwaylandviewport_p.h"

#include <QtWaylandClient/private/qwayland-linux-dmabuf-unstable-v1.h>

#if QT_CONFIG(vulkan)
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#endif

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

class QWaylandDmaBufBuffer : public QWaylandBuffer
{
public:
    QWaylandDmaBufBuffer(::wl_buffer *buf, const QSize &bufferSize)
        : m_size(bufferSize)
    {
        init(buf);
    }

    QSize size() const override { return m_size; }

private:
    QSize m_size;
};

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
    releaseDmaBufBuffers();
    releasePhasedBootResources();
    invalidateSurface();
}

QWaylandWindow::WindowType QWaylandPhasedWindow::windowType() const
{
    if (m_usingDmaBuf)
        return QWaylandWindow::Shm;
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

void QWaylandPhasedWindow::releaseDmaBufBuffers()
{
    for (QWaylandDmaBufBuffer *buf : std::as_const(m_dmabufBuffers)) {
        if (buf->busy())
            buf->setDeleteOnRelease(true);
        else
            delete buf;
    }
    m_dmabufBuffers.clear();
}

void QWaylandPhasedWindow::invalidateSurface()
{
    {
        QMutexLocker<QRecursiveMutex> lock(&m_waylandCommitMutex);
        m_surfaceAlive.store(false, std::memory_order_release);
    }
    releaseDmaBufBuffers();
    releasePhasedBootResources();
    m_bootReleased = false;
    m_usingDmaBuf = false;
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

    if (mViewport) {
        mViewport->setSource(QRectF(0.0, 0.0, shmBuf->size().width(), shmBuf->size().height()));
        mViewport->setDestination(geometry().size());
    }

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
        if (buf->busy())
            buf->setDeleteOnRelease(true);
        else
            delete buf;
    }
    m_bootBuffers.clear();
}

bool QWaylandPhasedWindow::supportsPhasedDmaBuf() const
{
    return mDisplay && mDisplay->linuxDmabuf() != nullptr && mDisplay->viewporter() != nullptr;
}

QList<quint64> QWaylandPhasedWindow::phasedDmaBufModifiers(quint32 drmFormat) const
{
    if (!mDisplay)
        return {};
    return mDisplay->linuxDmabufModifiers(drmFormat);
}

void *QWaylandPhasedWindow::createPhasedDmaBufBuffer(const PhasedDmaBufDesc &desc)
{
    if (!mDisplay || !mDisplay->linuxDmabuf() || desc.fd < 0
        || desc.bufferWidth <= 0 || desc.bufferHeight <= 0) {
        return nullptr;
    }

    auto *dmabuf = mDisplay->linuxDmabuf();
    ::zwp_linux_buffer_params_v1 *params = dmabuf->create_params();
    if (!params)
        return nullptr;

    zwp_linux_buffer_params_v1_add(params,
                                   desc.fd,
                                   0u,
                                   desc.offset,
                                   desc.stride,
                                   uint32_t(desc.modifier >> 32),
                                   uint32_t(desc.modifier & 0xFFFFFFFFu));
    ::wl_buffer *wlBuf = zwp_linux_buffer_params_v1_create_immed(
            params,
            desc.bufferWidth,
            desc.bufferHeight,
            desc.drmFormat,
            0u);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!wlBuf)
        return nullptr;

    m_usingDmaBuf = true;
    auto *buf = new QWaylandDmaBufBuffer(wlBuf, QSize(desc.bufferWidth, desc.bufferHeight));
    m_dmabufBuffers.push_back(buf);
    return buf;
}

void QWaylandPhasedWindow::destroyPhasedDmaBufBuffer(void *bufferHandle)
{
    if (!bufferHandle)
        return;
    auto *buf = static_cast<QWaylandDmaBufBuffer *>(bufferHandle);
    if (!m_dmabufBuffers.removeOne(buf))
        return;
    if (buf->busy())
        buf->setDeleteOnRelease(true);
    else
        delete buf;
}

bool QWaylandPhasedWindow::isPhasedDmaBufBufferBusy(void *bufferHandle) const
{
    if (!bufferHandle)
        return false;
    auto *buf = static_cast<QWaylandDmaBufBuffer *>(bufferHandle);
    if (!m_dmabufBuffers.contains(buf))
        return false;
    return buf->busy();
}

void QWaylandPhasedWindow::presentPhasedDmaBufBuffer(void *bufferHandle,
                                                     const QSize &pixelSize,
                                                     const QSize &logicalSize,
                                                     const QRegion &bufferPixelDamage)
{
    if (!bufferHandle || !isExposed() || pixelSize.isEmpty() || logicalSize.isEmpty())
        return;

    auto *buf = static_cast<QWaylandDmaBufBuffer *>(bufferHandle);
    if (!m_dmabufBuffers.contains(buf) || !buf->buffer())
        return;

    QReadLocker locker(&mSurfaceLock);
    if (!mSurface)
        return;

    // Ensure wp_viewport exists and buffer_scale == 1 so wp_viewport.set_source is always in exact 1:1 buffer pixels!
    if (!mViewport && mDisplay && mDisplay->viewporter())
        mViewport.reset(new QWaylandViewport(mDisplay->createViewport(this)));

    if (mViewport) {
        if (mSurface->version() >= 3)
            mSurface->set_buffer_scale(1);
        mViewport->setSource(QRectF(0.0, 0.0, double(pixelSize.width()), double(pixelSize.height())));
        mViewport->setDestination(logicalSize);
    }

    if (!mOffset.isNull()) {
        if (mSurface->version() >= WL_SURFACE_OFFSET_SINCE_VERSION)
            mSurface->offset(mOffset.x(), mOffset.y());
        mOffset = QPoint();
    }

    buf->setBusy(true);
    mSurface->attach(buf->buffer(), 0, 0);

    const QRect fullBox(0, 0, pixelSize.width(), pixelSize.height());
    if (mSurface->version() >= 4) {
        if (bufferPixelDamage.isEmpty()) {
            mSurface->damage_buffer(0, 0, pixelSize.width(), pixelSize.height());
        } else {
            for (const QRect &dr : bufferPixelDamage) {
                const QRect r = dr & fullBox;
                if (!r.isEmpty())
                    mSurface->damage_buffer(r.x(), r.y(), r.width(), r.height());
            }
        }
    } else {
        mSurface->damage(0, 0, logicalSize.width(), logicalSize.height());
    }

    mExposeEventNeedsAttachedBuffer = false;
    mSurface->commit();
    locker.unlock();
    mDisplay->flushRequests();
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
