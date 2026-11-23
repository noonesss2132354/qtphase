// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASESHAREDMATH_P_H
#define QPHASESHAREDMATH_P_H

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

#include <QtPhase/qphasestyles.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

QT_BEGIN_NAMESPACE

namespace QtPhaseMath {

inline float boxPixelCoverage(float fragCenterX, float fragCenterY,
                              float boxMinX, float boxMinY,
                              float boxMaxX, float boxMaxY) noexcept
{
    const float cx = std::clamp(std::min(fragCenterX + 0.5f, boxMaxX) - std::max(fragCenterX - 0.5f, boxMinX), 0.0f, 1.0f);
    const float cy = std::clamp(std::min(fragCenterY + 0.5f, boxMaxY) - std::max(fragCenterY - 0.5f, boxMinY), 0.0f, 1.0f);
    return cx * cy;
}

// Signed distance to a 4-corner rounded rectangle [minX..maxX] x [minY..maxY]
// Matches phase_uber.frag sdRoundedBox() identically.
inline float sdRoundedBox(float px, float py,
                          float minX, float minY,
                          float maxX, float maxY,
                          float rTL, float rTR, float rBR, float rBL) noexcept
{
    const float cx = 0.5f * (minX + maxX);
    const float cy = 0.5f * (minY + maxY);
    const float hx = std::max(0.0f, 0.5f * (maxX - minX));
    const float hy = std::max(0.0f, 0.5f * (maxY - minY));

    const float qx = px - cx;
    const float qy = py - cy;

    float r = (qx <= 0.0f)
            ? ((qy <= 0.0f) ? rTL : rBL)
            : ((qy <= 0.0f) ? rTR : rBR);
    r = std::clamp(r, 0.0f, std::min(hx, hy));

    const float dx = std::abs(qx) - hx + r;
    const float dy = std::abs(qy) - hy + r;
    const float outX = std::max(dx, 0.0f);
    const float outY = std::max(dy, 0.0f);
    return std::min(std::max(dx, dy), 0.0f) + std::sqrt(outX * outX + outY * outY) - r;
}

// Signed distance to a capsule / butt-capped line segment [ax,ay] -> [bx,by] with half-width halfW.
// Matches phase_uber.frag sdStrokeSegment() identically.
inline float sdStrokeSegment(float px, float py,
                             float ax, float ay,
                             float bx, float by,
                             float halfW, bool buttCap) noexcept
{
    const float vx = bx - ax;
    const float vy = by - ay;
    const float wx = px - ax;
    const float wy = py - ay;
    const float lenSq = vx * vx + vy * vy;
    if (lenSq <= 1e-6f)
        return std::sqrt(wx * wx + wy * wy) - halfW;

    if (buttCap) {
        const float len = std::sqrt(lenSq);
        const float ux = vx / len;
        const float uy = vy / len;
        const float along = wx * ux + wy * uy;
        const float perp = std::abs(-wx * uy + wy * ux);
        const float dAlong = std::max(along - len, -along);
        const float dPerp = perp - halfW;
        const float ox = std::max(dAlong, 0.0f);
        const float oy = std::max(dPerp, 0.0f);
        return std::min(std::max(dAlong, dPerp), 0.0f) + std::sqrt(ox * ox + oy * oy);
    }

    const float h = std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f);
    const float dx = wx - vx * h;
    const float dy = wy - vy * h;
    return std::sqrt(dx * dx + dy * dy) - halfW;
}

// Abramowitz & Stegun rational approximation of erf(x) matching phase_uber.frag bit-for-bit
inline float erfApprox(float x) noexcept
{
    const float s = (x < 0.0f) ? -1.0f : 1.0f;
    const float a = std::abs(x);
    const float t = 1.0f + (0.278393f + (0.230389f + (0.000972f + 0.078108f * a) * a) * a) * a;
    const float t2 = t * t;
    return s - s / (t2 * t2);
}

inline float evalClipCoverage(float fragX, float fragY,
                              float clipMinX, float clipMinY,
                              float clipMaxX, float clipMaxY,
                              float rTL, float rTR, float rBR, float rBL) noexcept
{
    if (rTL <= 0.0f && rTR <= 0.0f && rBR <= 0.0f && rBL <= 0.0f)
        return boxPixelCoverage(fragX, fragY, clipMinX, clipMinY, clipMaxX, clipMaxY);
    const float d = sdRoundedBox(fragX, fragY, clipMinX, clipMinY, clipMaxX, clipMaxY, rTL, rTR, rBR, rBL);
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

inline float evalAnimationOpacity(uint32_t rawMode, uint32_t durationMs, uint64_t startNs,
                                  float fromOp, float toOp, uint64_t frameTimeNs) noexcept
{
    const uint32_t mode = rawMode & 0x00FFFFFFu;
    if (mode == 0u || durationMs == 0u)
        return 1.0f;

    const uint64_t elapsedNs = (frameTimeNs >= startNs) ? (frameTimeNs - startNs) : 0u;
    const float elapsedMs = float(double(elapsedNs) * 1e-6);
    const float dur = float(durationMs);

    if (mode == 1u) {
        const float t = std::clamp(elapsedMs / dur, 0.0f, 1.0f);
        return fromOp + (toOp - fromOp) * t;
    }
    if (mode == 2u) {
        const float t = std::clamp(elapsedMs / dur, 0.0f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t);
        return fromOp + (toOp - fromOp) * s;
    }
    if (mode == 3u) {
        const float cycle = std::fmod(elapsedMs, dur) / dur;
        const float s = 0.5f - 0.5f * std::cos(6.283185307f * cycle);
        return fromOp + (toOp - fromOp) * s;
    }
    return 1.0f;
}

// Perceptual linear-light / stem-darkening coverage boost for anti-aliased text in sRGB targets.
// Identical formula on CPU and GPU.
inline float applyTextStemDarkening(float cov, float r, float g, float b, float strength) noexcept
{
    if (strength <= 0.0f || cov <= 0.0f || cov >= 1.0f)
        return cov;
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    // Boost light-on-dark stems proportionally to foreground luminance + base stem weight
    const float k = std::clamp(strength * (0.35f + 0.65f * lum), 0.0f, 1.0f);
    const float boosted = std::sqrt(cov);
    return cov + (boosted - cov) * k;
}

inline uint8_t unormFloatToByte(float v) noexcept
{
    return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

inline float quantizeUnorm8Scalar(float v) noexcept
{
    return float(unormFloatToByte(v)) * (1.0f / 255.0f);
}

// Premultiplied Porter-Duff + PDF/CSS separable blend modes matching phase_uber.frag bit-for-bit!
inline void compositePremulBlendMode(uint32_t blendMode,
                                     float srcR, float srcG, float srcB, float srcA,
                                     float &dstR, float &dstG, float &dstB, float &dstA) noexcept
{
    float outR = 0.0f, outG = 0.0f, outB = 0.0f;
    const float outA = srcA + dstA * (1.0f - srcA);

    if (blendMode == uint32_t(QPhaseBlendMode::Multiply)) {
        outR = srcR * dstR + srcR * (1.0f - dstA) + dstR * (1.0f - srcA);
        outG = srcG * dstG + srcG * (1.0f - dstA) + dstG * (1.0f - srcA);
        outB = srcB * dstB + srcB * (1.0f - dstA) + dstB * (1.0f - srcA);
    } else if (blendMode == uint32_t(QPhaseBlendMode::Screen)) {
        outR = srcR + dstR - srcR * dstR;
        outG = srcG + dstG - srcG * dstG;
        outB = srcB + dstB - srcB * dstB;
    } else if (blendMode == uint32_t(QPhaseBlendMode::Additive)) {
        outR = std::min(1.0f, srcR + dstR);
        outG = std::min(1.0f, srcG + dstG);
        outB = std::min(1.0f, srcB + dstB);
    } else {
        outR = srcR + dstR * (1.0f - srcA);
        outG = srcG + dstG * (1.0f - srcA);
        outB = srcB + dstB * (1.0f - srcA);
    }

    dstR = quantizeUnorm8Scalar(outR);
    dstG = quantizeUnorm8Scalar(outG);
    dstB = quantizeUnorm8Scalar(outB);
    dstA = quantizeUnorm8Scalar(outA);
}

} // namespace QtPhaseMath

QT_END_NAMESPACE

#endif // QPHASESHAREDMATH_P_H
