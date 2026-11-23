// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QWAYLANDPHASEDWINDOW_P_H
#define QWAYLANDPHASEDWINDOW_P_H

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

#include "qwaylandwindow_p.h"

#if QT_CONFIG(vulkan)
#include "qwaylandvulkaninstance_p.h"
#endif

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

class QWaylandShmBuffer;

class Q_WAYLANDCLIENT_EXPORT QWaylandPhasedWindow : public QWaylandWindow
{
public:
    explicit QWaylandPhasedWindow(QWindow *window, QWaylandDisplay *display);
    ~QWaylandPhasedWindow() override;

    WindowType windowType() const override;
    void invalidateSurface() override;

    bool acquirePhasedBootBuffer(const QSize &pixelSize, qreal dpr, PhasedBootBuffer *outBuffer) override;
    void presentPhasedBootBuffer(void *bufferHandle, const QRegion &damage) override;
    void releasePhasedBootResources() override;
    bool createPhasedVulkanSurface(void *vkInstance, void *vkGetInstanceProcAddrFn, void *vkSurfaceOut) override;
    bool checkPhasedVulkanPresentSupport(void *vkInstance, void *vkPhysicalDevice,
                                         quint32 queueFamilyIndex, void *vkGetInstanceProcAddrFn) override;
    void preparePhasedGpuPresent() override;

#if QT_CONFIG(vulkan)
    VkSurfaceKHR *vkSurface();
#endif

private:
    QList<QWaylandShmBuffer *> m_bootBuffers;
    bool m_bootReleased = false;
#if QT_CONFIG(vulkan)
    VkSurfaceKHR m_vkSurface = VK_NULL_HANDLE;
#endif
};

} // namespace QtWaylandClient

QT_END_NAMESPACE

#endif // QWAYLANDPHASEDWINDOW_P_H
