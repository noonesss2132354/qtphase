// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASESTYLES_H
#define QPHASESTYLES_H

#include <QtPhase/qtphaseglobal.h>
#include <QtCore/qpoint.h>
#include <QtCore/qrect.h>
#include <QtCore/qsize.h>
#include <QtGui/qcolor.h>

QT_BEGIN_NAMESPACE

enum class QPhaseBlendMode : quint32
{
    Normal   = 0,
    Multiply = 1,
    Screen   = 2,
    Additive = 3
};

enum class QPhaseRendererAffinity : quint8
{
    Any     = 0, // Renders on both CPU and GPU; uses normal 5-frame competitive worker race
    GpuOnly = 1, // Forces GPU Vulkan renderer; skipped on CPU if non-blocking boot frames run
    CpuOnly = 2  // Forces CPU tile-pool renderer; immediately shuts down GPU worker & frees VRAM
};

struct QPhaseGpuSurfaceContext
{
    void *vkInstance = nullptr;
    void *vkPhysicalDevice = nullptr;
    void *vkDevice = nullptr;
    void *vkCommandBuffer = nullptr;
    quint64 colorImage = 0;       // VkImage (VK_FORMAT_R8G8B8A8_UNORM, premultiplied alpha)
    quint64 colorImageView = 0;   // VkImageView
    quint64 depthImage = 0;       // VkImage (VK_FORMAT_D32_SFLOAT when depth enabled, else 0)
    quint64 depthImageView = 0;   // VkImageView
    QSize pixelSize{};
    qreal devicePixelRatio = 1.0;
    quint64 frameTimeNs = 0;
};

struct QPhaseCorners
{
    float topLeft = 0.0f;
    float topRight = 0.0f;
    float bottomRight = 0.0f;
    float bottomLeft = 0.0f;

    static constexpr QPhaseCorners all(float radius) noexcept
    {
        return {radius, radius, radius, radius};
    }

    constexpr bool isZero() const noexcept
    {
        return topLeft <= 0.0f && topRight <= 0.0f && bottomRight <= 0.0f && bottomLeft <= 0.0f;
    }
};

struct QPhaseFill
{
    enum class Type : quint8 {
        Solid = 0,
        LinearGradient = 1
    };

    Type type = Type::Solid;
    QColor color0 = Qt::transparent;
    QColor color1 = Qt::transparent;
    QPointF gradientStart{};
    QPointF gradientEnd{};

    static QPhaseFill solid(const QColor &color) noexcept
    {
        return {Type::Solid, color, color, {}, {}};
    }

    static QPhaseFill linearGradient(QPointF start, QPointF end, const QColor &c0, const QColor &c1) noexcept
    {
        return {Type::LinearGradient, c0, c1, start, end};
    }
};

struct QPhaseBorder
{
    float width = 0.0f;
    QColor color = Qt::transparent;
};

struct QPhaseShadow
{
    float blurRadius = 0.0f;
    float spread = 0.0f;
    QPointF offset{};
    QColor color = Qt::transparent;
};

struct QPhaseBoxStyle
{
    QPhaseCorners radius{};
    QPhaseFill fill{};
    QPhaseBorder border{};
    QPhaseShadow shadow{};
};

struct QPhaseAnimation
{
    enum class Mode : quint8 {
        None = 0,
        LinearOnce = 1,
        SmoothStepOnce = 2,
        CosinePulseInfinite = 3
    };

    Mode mode = Mode::None;
    float fromOpacity = 1.0f;
    float toOpacity = 1.0f;
    quint32 durationMs = 0;
};

QT_END_NAMESPACE

#endif // QPHASESTYLES_H
