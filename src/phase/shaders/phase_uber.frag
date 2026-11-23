#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

// Lean, high-occupancy single-pass tile blender:
// - ZERO Bezier curve-winding loops (all curves baked into u_glyphAtlasRead via phase_glyph_bake.comp!)
// - Full 2x2 rotated/sheared transforms, exact nested rounded-clip chains, analytical StrokeSegment
// - 3-deep in-register PushLayer/PopLayer stack for zero-VRAM isolated group opacity & blend modes!

struct ResolvedTransform {
    float tx;
    float ty;
    float m00;
    float m01;
    float m10;
    float m11;
    float opacity;
    uint flags;
};

struct ResolvedClip {
    float cullMinX;
    float cullMinY;
    float cullMaxX;
    float cullMaxY;
    float selfMinX;
    float selfMinY;
    float selfMaxX;
    float selfMaxY;
    float rTL;
    float rTR;
    float rBR;
    float rBL;
    uint parentRoundedClipIndex;
    uint hasRoundedAncestorOrSelf;
};

struct Primitive {
    uint kind;
    uint transformIndex;
    uint clipIndex;
    uint resourceIndex;

    float x;
    float y;
    float w;
    float h;

    float c0r, c0g, c0b, c0a;
    float c1r, c1g, c1b, c1a;
    float br, bg, bb, ba;

    float rTL, rTR, rBR, rBL;

    float p0, p1, p2, p3, p4, p5;

    uint animMode;
    uint animDurationMs;
    uint64_t animStartNs;
    float animFromOpacity;
    float animToOpacity;
};

struct PreparedPrim {
    vec4 effBounds;
    vec4 opaqueBounds;
    uint primIdx;
    uint flags;
    float effAnimOp;
    uint pad0;
};

struct TileHeader {
    uint listOffset;
    uint countAndFlags;
};

struct GlyphSlot {
    uint packedOrigXY;
    uint packedAtlasXY;
    uint packedWH;
};

layout(buffer_reference, scalar) readonly buffer TransformBuf {
    ResolvedTransform items[];
};

layout(buffer_reference, scalar) readonly buffer ClipBuf {
    ResolvedClip items[];
};

layout(buffer_reference, scalar) readonly buffer PrimitiveBuf {
    Primitive items[];
};

layout(buffer_reference, scalar) readonly buffer PreparedPrimBuf {
    PreparedPrim items[];
};

layout(buffer_reference, scalar) readonly buffer TileHeaderBuf {
    TileHeader items[];
};

layout(buffer_reference, scalar) readonly buffer TileItemsBuf {
    uint items[];
};

layout(buffer_reference, scalar) readonly buffer GlyphSlotBuf {
    GlyphSlot items[];
};

layout(set = 0, binding = 0) uniform sampler2D u_bindlessTextures[];
layout(set = 0, binding = 2) uniform sampler2D u_glyphAtlasRead;

layout(push_constant, scalar) uniform PushConstants {
    uint64_t transformsAddr;
    uint64_t clipsAddr;
    uint64_t primitivesAddr;
    uint64_t preparedPrimsAddr;
    uint64_t tileHeadersAddr;
    uint64_t tileItemsAddr;
    uint64_t glyphSlotsAddr;
    vec4 clearColorPremul;
    float viewportWidth;
    float viewportHeight;
    float devicePixelRatio;
    float textStemDarkening;
    uint tilesX;
    uint forceOpaqueAlpha;
} pc;

layout(location = 0) out vec4 outColor;

vec4 quantizeUnorm8(vec4 v) {
    return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) * (1.0 / 255.0);
}

vec4 compositePremulMode(uint blendMode, vec4 src, vec4 dst) {
    float outA = src.a + dst.a * (1.0 - src.a);
    vec3 outRgb;
    if (blendMode == 1u) {
        // Multiply
        outRgb = src.rgb * dst.rgb + src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a);
    } else if (blendMode == 2u) {
        // Screen
        outRgb = src.rgb + dst.rgb - src.rgb * dst.rgb;
    } else if (blendMode == 3u) {
        // Additive
        outRgb = min(vec3(1.0), src.rgb + dst.rgb);
    } else {
        // Normal Porter-Duff Source-Over
        outRgb = src.rgb + dst.rgb * (1.0 - src.a);
    }
    return quantizeUnorm8(vec4(outRgb, outA));
}

float boxPixelCoverage(vec2 fragCenter, vec2 boxMin, vec2 boxMax) {
    float cx = clamp(min(fragCenter.x + 0.5, boxMax.x) - max(fragCenter.x - 0.5, boxMin.x), 0.0, 1.0);
    float cy = clamp(min(fragCenter.y + 0.5, boxMax.y) - max(fragCenter.y - 0.5, boxMin.y), 0.0, 1.0);
    return cx * cy;
}

float sdRoundedBox(vec2 p, vec2 minB, vec2 maxB, vec4 radii) {
    vec2 center = 0.5 * (minB + maxB);
    vec2 halfSize = max(vec2(0.0), 0.5 * (maxB - minB));
    vec2 q = p - center;
    float r = (q.x <= 0.0)
            ? ((q.y <= 0.0) ? radii.x : radii.w)
            : ((q.y <= 0.0) ? radii.y : radii.z);
    r = clamp(r, 0.0, min(halfSize.x, halfSize.y));
    vec2 d = abs(q) - halfSize + vec2(r);
    return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0))) - r;
}

float sdStrokeSegment(vec2 p, vec2 a, vec2 b, float halfW, bool buttCap) {
    vec2 v = b - a;
    vec2 w = p - a;
    float lenSq = dot(v, v);
    if (lenSq <= 1e-6)
        return length(w) - halfW;
    if (buttCap) {
        float len = sqrt(lenSq);
        vec2 u = v / len;
        float along = dot(w, u);
        float perp = abs(-w.x * u.y + w.y * u.x);
        vec2 d = vec2(max(along - len, -along), perp - halfW);
        return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0)));
    }
    float h = clamp(dot(w, v) / lenSq, 0.0, 1.0);
    return length(w - v * h) - halfW;
}

float erfApprox(float x) {
    float s = (x < 0.0) ? -1.0 : 1.0;
    float a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + (0.000972 + 0.078108 * a) * a) * a) * a;
    float t2 = t * t;
    return s - s / (t2 * t2);
}

float evalClipChainCoverage(vec2 fragPos, uint clipIndex, float dpr) {
    if (clipIndex == 0u)
        return 1.0;
    ResolvedClip cl = ClipBuf(pc.clipsAddr).items[clipIndex];
    float cov = boxPixelCoverage(fragPos,
                                 vec2(cl.cullMinX, cl.cullMinY) * dpr,
                                 vec2(cl.cullMaxX, cl.cullMaxY) * dpr);
    if (cov <= 0.0 || cl.hasRoundedAncestorOrSelf == 0u)
        return cov;

    uint cur = clipIndex;
    for (int hop = 0; hop < 4 && cur != 0u && cov > 0.0; ++hop) {
        ResolvedClip rc = (hop == 0) ? cl : ClipBuf(pc.clipsAddr).items[cur];
        vec4 r = vec4(rc.rTL, rc.rTR, rc.rBR, rc.rBL) * dpr;
        if (r.x > 0.0 || r.y > 0.0 || r.z > 0.0 || r.w > 0.0) {
            float rCov = clamp(0.5 - sdRoundedBox(fragPos,
                                                  vec2(rc.selfMinX, rc.selfMinY) * dpr,
                                                  vec2(rc.selfMaxX, rc.selfMaxY) * dpr, r),
                               0.0, 1.0);
            cov *= rCov;
        }
        cur = rc.parentRoundedClipIndex;
    }
    return cov;
}

float applyTextStemDarkening(float cov, vec3 rgb, float strength) {
    if (strength <= 0.0 || cov <= 0.0 || cov >= 1.0)
        return cov;
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    float k = clamp(strength * (0.35 + 0.65 * lum), 0.0, 1.0);
    return mix(cov, sqrt(cov), k);
}

vec2 txPoint(ResolvedTransform tx, vec2 lp, float dpr) {
    return vec2(tx.m00 * lp.x + tx.m01 * lp.y + tx.tx,
                tx.m10 * lp.x + tx.m11 * lp.y + tx.ty) * dpr;
}

// Maps device fragPos into local-scaled pixel space where 1 unit == 1 physical pixel!
bool mapFragToScaledLocal(vec2 fragPos, ResolvedTransform tx, float dpr,
                          out vec2 scaledFrag, out float pxScale) {
    if ((tx.flags & 1u) == 0u) {
        pxScale = abs(tx.m00) * dpr;
        scaledFrag = fragPos;
        return true;
    }
    float det = tx.m00 * tx.m11 - tx.m01 * tx.m10;
    if (abs(det) <= 1e-8)
        return false;
    vec2 logP = fragPos / dpr - vec2(tx.tx, tx.ty);
    vec2 locP = vec2(tx.m11 * logP.x - tx.m01 * logP.y,
                    -tx.m10 * logP.x + tx.m00 * logP.y) / det;
    pxScale = sqrt(abs(det)) * dpr;
    scaledFrag = locP * pxScale;
    return true;
}

void main() {
    vec2 fragPos = gl_FragCoord.xy;
    ivec2 ipos = ivec2(fragPos);
    uint txIdx = uint(ipos.x) >> 4u;
    uint tyIdx = uint(ipos.y) >> 4u;
    uint tileLinearIdx = tyIdx * pc.tilesX + txIdx;

    TileHeader hdr = TileHeaderBuf(pc.tileHeadersAddr).items[tileLinearIdx];
    uint count = hdr.countAndFlags & 0x3FFFFFFFu;
    bool startsOpaque = (hdr.countAndFlags & 0x40000000u) != 0u;

    if (count == 0u) {
        vec4 bg = quantizeUnorm8(pc.clearColorPremul);
        if (pc.forceOpaqueAlpha != 0u)
            bg.a = 1.0;
        outColor = bg;
        return;
    }

    vec4 acc;
    uint startIdx = 0u;
    if (startsOpaque) {
        uint occOrderPos = TileItemsBuf(pc.tileItemsAddr).items[hdr.listOffset];
        uint occPrimIdx = PreparedPrimBuf(pc.preparedPrimsAddr).items[occOrderPos].primIdx;
        Primitive occ = PrimitiveBuf(pc.primitivesAddr).items[occPrimIdx];
        acc = quantizeUnorm8(vec4(occ.c0r, occ.c0g, occ.c0b, 1.0));
        if (count == 1u) {
            outColor = acc;
            return;
        }
        startIdx = 1u;
    } else {
        acc = quantizeUnorm8(pc.clearColorPremul);
    }

    float dpr = pc.devicePixelRatio;
    uint lastClipIdx = 0xFFFFFFFFu;
    float cachedClipCov = 1.0;

    // 3-deep in-register isolated group stack (explicit scalar registers -> 0 VGPR scratch spills, 0 MB VRAM!)
    vec4 savedLayer0 = vec4(0.0);
    vec4 savedLayer1 = vec4(0.0);
    vec4 savedLayer2 = vec4(0.0);
    int layerDepth = 0;

    for (uint i = startIdx; i < count; ++i) {
        uint orderPos = TileItemsBuf(pc.tileItemsAddr).items[hdr.listOffset + i];

        // 1. Fast 1-instruction texelFetch path for all HbGpuCurve glyphs & vectors!
        GlyphSlot gs = GlyphSlotBuf(pc.glyphSlotsAddr).items[orderPos];
        if (gs.packedWH != 0u) {
            int origX = bitfieldExtract(int(gs.packedOrigXY), 0, 16);
            int origY = bitfieldExtract(int(gs.packedOrigXY), 16, 16);
            int gw = int(gs.packedWH & 0xFFFFu);
            int gh = int(gs.packedWH >> 16u);
            int lx = ipos.x - origX;
            int ly = ipos.y - origY;
            if (lx < 0 || lx >= gw || ly < 0 || ly >= gh)
                continue;

            int ax = int(gs.packedAtlasXY & 0xFFFFu);
            int ay = int(gs.packedAtlasXY >> 16u);
            float cov = texelFetch(u_glyphAtlasRead, ivec2(ax + lx, ay + ly), 0).r;
            if (cov <= 0.0)
                continue;

            uint gPrimIdx = PreparedPrimBuf(pc.preparedPrimsAddr).items[orderPos].primIdx;
            float gAnimOp = PreparedPrimBuf(pc.preparedPrimsAddr).items[orderPos].effAnimOp;
            Primitive p = PrimitiveBuf(pc.primitivesAddr).items[gPrimIdx];
            if (p.clipIndex != lastClipIdx) {
                lastClipIdx = p.clipIndex;
                cachedClipCov = evalClipChainCoverage(fragPos, lastClipIdx, dpr);
            }
            if (cachedClipCov <= 0.0)
                continue;

            vec3 fgRgb = vec3(p.c0r, p.c0g, p.c0b);
            cov = applyTextStemDarkening(cov, fgRgb, pc.textStemDarkening);

            ResolvedTransform tx = TransformBuf(pc.transformsAddr).items[p.transformIndex];
            float a = p.c0a * cov * tx.opacity * gAnimOp * cachedClipCov;
            if (a > 0.0) {
                vec4 srcPremul = vec4(fgRgb * a, a);
                acc = quantizeUnorm8(srcPremul + acc * (1.0 - srcPremul.a));
            }
            continue;
        }

        vec4 eb = PreparedPrimBuf(pc.preparedPrimsAddr).items[orderPos].effBounds;
        if (fragPos.x < eb.x || fragPos.x > eb.z
            || fragPos.y < eb.y || fragPos.y > eb.w) {
            continue;
        }

        uint primIdx = PreparedPrimBuf(pc.preparedPrimsAddr).items[orderPos].primIdx;
        float effAnimOp = PreparedPrimBuf(pc.preparedPrimsAddr).items[orderPos].effAnimOp;
        Primitive p = PrimitiveBuf(pc.primitivesAddr).items[primIdx];

        // 2. Isolated In-Register Layer Push / Pop
        if (p.kind == 6u) {
            if (layerDepth == 0) {
                savedLayer0 = acc;
                layerDepth = 1;
                acc = vec4(0.0);
            } else if (layerDepth == 1) {
                savedLayer1 = acc;
                layerDepth = 2;
                acc = vec4(0.0);
            } else if (layerDepth == 2) {
                savedLayer2 = acc;
                layerDepth = 3;
                acc = vec4(0.0);
            }
            continue;
        }
        if (p.kind == 7u) {
            if (layerDepth > 0) {
                --layerDepth;
                if (p.clipIndex != lastClipIdx) {
                    lastClipIdx = p.clipIndex;
                    cachedClipCov = evalClipChainCoverage(fragPos, lastClipIdx, dpr);
                }
                ResolvedTransform tx = TransformBuf(pc.transformsAddr).items[p.transformIndex];
                float grpOp = clamp(p.c0a * tx.opacity * effAnimOp * cachedClipCov, 0.0, 1.0);
                vec4 grpPremul = acc * grpOp;
                vec4 dstLayer = (layerDepth == 0) ? savedLayer0 : ((layerDepth == 1) ? savedLayer1 : savedLayer2);
                acc = compositePremulMode(p.resourceIndex, grpPremul, dstLayer);
            }
            continue;
        }

        if (p.clipIndex != lastClipIdx) {
            lastClipIdx = p.clipIndex;
            cachedClipCov = evalClipChainCoverage(fragPos, lastClipIdx, dpr);
        }
        if (cachedClipCov <= 0.0)
            continue;

        ResolvedTransform tx = TransformBuf(pc.transformsAddr).items[p.transformIndex];
        float baseOpacity = tx.opacity * effAnimOp * cachedClipCov;
        if (baseOpacity <= 0.0)
            continue;

        vec4 srcPremul = vec4(0.0);

        if (p.kind == 5u) {
            // Analytical Anti-Aliased StrokeSegment
            vec2 aDev = txPoint(tx, vec2(p.x, p.y), dpr);
            vec2 bDev = txPoint(tx, vec2(p.w, p.h), dpr);
            float sc = sqrt(abs(tx.m00 * tx.m11 - tx.m01 * tx.m10)) * dpr;
            float halfW = max(0.25, 0.5 * p.p0 * sc);
            float dist = sdStrokeSegment(fragPos, aDev, bDev, halfW, p.p1 > 0.5);
            float cov = clamp(0.5 - dist, 0.0, 1.0);
            float a = p.c0a * cov * baseOpacity;
            if (a <= 0.0)
                continue;
            srcPremul = vec4(p.c0r * a, p.c0g * a, p.c0b * a, a);
        } else {
            bool isRot = (tx.flags & 1u) != 0u;
            vec2 evalFrag;
            vec2 boxMin;
            vec2 boxMax;
            float pxScale;

            if (!isRot) {
                evalFrag = fragPos;
                vec2 d0 = vec2(p.x * tx.m00 + tx.tx, p.y * tx.m11 + tx.ty) * dpr;
                vec2 d1 = vec2((p.x + p.w) * tx.m00 + tx.tx, (p.y + p.h) * tx.m11 + tx.ty) * dpr;
                boxMin = min(d0, d1);
                boxMax = max(d0, d1);
                pxScale = abs(tx.m00) * dpr;
            } else {
                if (!mapFragToScaledLocal(fragPos, tx, dpr, evalFrag, pxScale))
                    continue;
                boxMin = vec2(p.x, p.y) * pxScale;
                boxMax = vec2(p.x + p.w, p.y + p.h) * pxScale;
            }

            vec4 radiiDev = vec4(p.rTL, p.rTR, p.rBR, p.rBL) * pxScale;

            if (p.kind == 0u) {
                float cov = isRot
                        ? clamp(0.5 - sdRoundedBox(evalFrag, boxMin, boxMax, vec4(0.0)), 0.0, 1.0)
                        : boxPixelCoverage(evalFrag, boxMin, boxMax);
                float a = p.c0a * baseOpacity * cov;
                if (a <= 0.0)
                    continue;
                srcPremul = vec4(p.c0r * a, p.c0g * a, p.c0b * a, a);
            } else if (p.kind == 1u) {
                float outerDist = sdRoundedBox(evalFrag, boxMin, boxMax, radiiDev);
                float outerCov = clamp(0.5 - outerDist, 0.0, 1.0);
                if (outerCov <= 0.0)
                    continue;

                vec4 fillCol = vec4(p.c0r, p.c0g, p.c0b, p.c0a);
                if (p.p1 > 0.5) {
                    vec2 g0 = isRot ? (vec2(p.p2, p.p3) * pxScale) : txPoint(tx, vec2(p.p2, p.p3), dpr);
                    vec2 g1 = isRot ? (vec2(p.p4, p.p5) * pxScale) : txPoint(tx, vec2(p.p4, p.p5), dpr);
                    vec2 gv = g1 - g0;
                    float gt = clamp(dot(evalFrag - g0, gv) / max(dot(gv, gv), 1e-4), 0.0, 1.0);
                    fillCol = mix(fillCol, vec4(p.c1r, p.c1g, p.c1b, p.c1a), gt);
                }

                float borderWidthPx = p.p0 * pxScale;
                if (borderWidthPx > 0.0 && p.ba > 0.0) {
                    vec2 innerMin = boxMin + vec2(borderWidthPx);
                    vec2 innerMax = boxMax - vec2(borderWidthPx);
                    float innerCov = 0.0;
                    if (innerMin.x < innerMax.x && innerMin.y < innerMax.y) {
                        vec4 innerRadii = max(radiiDev - vec4(borderWidthPx), vec4(0.0));
                        innerCov = clamp(0.5 - sdRoundedBox(evalFrag, innerMin, innerMax, innerRadii), 0.0, 1.0);
                    }
                    float borderCov = max(0.0, outerCov - innerCov);
                    float fa = fillCol.a * innerCov;
                    float ba = p.ba * borderCov;
                    vec3 premulRgb = fillCol.rgb * fa + vec3(p.br, p.bg, p.bb) * ba;
                    float totalA = fa + ba;
                    srcPremul = vec4(premulRgb * baseOpacity, totalA * baseOpacity);
                } else {
                    float a = fillCol.a * outerCov * baseOpacity;
                    srcPremul = vec4(fillCol.rgb * a, a);
                }
            } else if (p.kind == 2u) {
                float sigma = max(p.p0 * dpr * 0.5, 0.5);
                float spread = p.p1 * dpr;
                vec2 shMin = boxMin - vec2(spread);
                vec2 shMax = boxMax + vec2(spread);
                vec4 shRadii = max(radiiDev + vec4(spread), vec4(0.0));
                float dist = sdRoundedBox(evalFrag, shMin, shMax, shRadii);
                float shadowCov = 0.5 * (1.0 - erfApprox(dist / (sigma * 1.41421356)));
                float a = p.c0a * shadowCov * baseOpacity;
                if (a <= 0.0)
                    continue;
                srcPremul = vec4(p.c0r * a, p.c0g * a, p.c0b * a, a);
            } else if (p.kind == 3u) {
                float maskCov = (isRot || radiiDev.x > 0.0 || radiiDev.y > 0.0 || radiiDev.z > 0.0 || radiiDev.w > 0.0)
                        ? clamp(0.5 - sdRoundedBox(evalFrag, boxMin, boxMax, radiiDev), 0.0, 1.0)
                        : boxPixelCoverage(evalFrag, boxMin, boxMax);
                if (maskCov <= 0.0)
                    continue;
                vec2 rectSize = max(boxMax - boxMin, vec2(1e-4));
                vec2 normUv = clamp((evalFrag - boxMin) / rectSize, vec2(0.0), vec2(1.0));
                vec2 texUv = mix(vec2(p.p0, p.p1), vec2(p.p2, p.p3), normUv);
                vec4 texPremul = textureLod(u_bindlessTextures[nonuniformEXT(p.resourceIndex)], texUv, 0.0);
                srcPremul = texPremul * (maskCov * baseOpacity * p.c0a);
            }
        }

        if (srcPremul.a > 0.0)
            acc = quantizeUnorm8(srcPremul + acc * (1.0 - srcPremul.a));
    }

    if (pc.forceOpaqueAlpha != 0u)
        acc.a = 1.0;
    outColor = acc;
}
