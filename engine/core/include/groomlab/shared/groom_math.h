// SPDX-License-Identifier: Apache-2.0
#pragma once

// The math the CPU operators and the GPU preview's kernels share: one source, compiled as C++ into
// the core (namespace groomlab::shared; math.h, noise.h and curl_phase.h bring it into groomlab)
// and as Slang into engine/gpu/shaders/look.slang, which includes this file. A fix here lands in
// both, and the GPU parity tests (tests/engine/gpu) compare them.
//
// So it is written in what both languages read alike: functions of float, int, uint and float3,
// `const` locals, fixed-size arrays and constant tables. The shim just below maps the rest:
//   float3   C++: Vec3f. Slang: float3.
//   uint     C++: std::uint32_t. Slang: uint.
//   u64      a 64-bit hash. C++: std::uint64_t. Slang: uint2 (low, high), as cs_5_0 (DirectX 11)
//            has no 64-bit integers; mul64, shr64, lo32 and hi32 do its arithmetic on both.
//   lint     a lattice coordinate. C++: std::int64_t. Slang: int (fromLattice makes it a u64).
//   pmPow, pmLog   C++: portable_math.h's pm::pow, pm::log (the same bits on every platform).
//            Slang: powStd (std::pow's answers where the GPU's pow has none) and log.
//   floor, abs, sqrt, clamp, dot, asuint   C++: std::'s (clamp as std::clamp: NaN stays NaN).
//            Slang: its own.
//   SHARED_UNROLL   Slang's [ForceUnroll] (a small constant loop); nothing in C++.
//   SHARED_CONST    a constant table: C++ inline constexpr, Slang static const.
// The C++ float arithmetic is IEEE 754 as written (no contraction, no fast math: CMakeLists.txt
// "Determinism"), so an edit here that changes an operation's order changes the CPU's bits, which
// tests/data/golden/PresetDigests.json catches. The GPU may fuse multiply-adds; its results differ
// by rounding alone. docs/MATH.md says how to add a function.

#ifdef __SLANG__

#define SHARED_UNROLL [ForceUnroll]
#define SHARED_CONST static const
#define SHARED_U64(hi, lo) uint2(lo, hi)

typedef uint2 u64;
typedef int lint;

// (The C++ form, the CPU engine's: native 64-bit products, the same words.)
uint mulHi(uint a, uint b) {
    __target_switch {
    case cpp: __intrinsic_asm "uint32_t((uint64_t($0) * uint64_t($1)) >> 32)";
    default:
        const uint a0 = a & 0xffff, a1 = a >> 16, b0 = b & 0xffff, b1 = b >> 16;
        const uint p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const uint mid = (p00 >> 16) + (p01 & 0xffff) + (p10 & 0xffff);
        return p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    }
}
u64 mul64(u64 a, u64 b) {
    __target_switch {
    case cpp: __intrinsic_asm "groomlab_mul64($0, $1)";
    default: return uint2(a.x * b.x, mulHi(a.x, b.x) + a.x * b.y + a.y * b.x);
    }
}
u64 shr64(u64 a, uint n) {  // 0 < n < 64
    return n < 32 ? uint2((a.x >> n) | (a.y << (32 - n)), a.y >> n) : uint2(a.y >> (n - 32), 0);
}
u64 fromLattice(lint i) { return uint2(asuint(i), i < 0 ? 0xffffffffu : 0u); }
uint lo32(u64 h) { return h.x; }
uint hi32(u64 h) { return h.y; }

// std::pow: a negative base to a whole power, 0 to 0 is 1.
float powStd(float a, float b) {
    if (b == 0.0f) return 1.0f;
    if (a == 0.0f) return b > 0.0f ? 0.0f : asfloat(0x7f800000u);
    if (a < 0.0f) {
        if (floor(b) != b) return asfloat(0x7fc00000u);
        const float m = pow(-a, b);
        return fmod(abs(b), 2.0f) == 1.0f ? -m : m;
    }
    return pow(a, b);
}
float pmPow(float a, float b) { return powStd(a, b); }
float pmLog(float x) { return log(x); }

#else

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "groomlab/portable_math.h"
#include "groomlab/vec3.h"

#define SHARED_UNROLL
#define SHARED_CONST inline constexpr
#define SHARED_U64(hi, lo) ((static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo))

namespace groomlab::shared {

using float3 = Vec3f;
using uint = std::uint32_t;
using u64 = std::uint64_t;
using lint = std::int64_t;

inline u64 mul64(u64 a, u64 b) { return a * b; }
inline u64 shr64(u64 a, uint n) { return a >> n; }
inline u64 fromLattice(lint i) { return static_cast<u64>(i); }
inline uint lo32(u64 h) { return static_cast<uint>(h); }
inline uint hi32(u64 h) { return static_cast<uint>(h >> 32); }

inline float pmPow(float a, float b) { return pm::pow(a, b); }
inline float pmLog(float x) { return pm::log(x); }
inline float floor(float x) { return std::floor(x); }
inline float abs(float x) { return std::fabs(x); }
inline float sqrt(float x) { return std::sqrt(x); }
inline float clamp(float x, float lo, float hi) { return std::clamp(x, lo, hi); }
inline float dot(const float3& a, const float3& b) { return a.dot(b); }
inline uint asuint(float f) {
    uint bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

}  // namespace groomlab::shared

#endif

#ifndef __SLANG__
namespace groomlab::shared {
#endif

// ---- Blends and steps ---------------------------------------------------------------------------

// a + (b - a) t: the order every blend in the engine takes.
inline float mix1(float a, float b, float t) { return a + (b - a) * t; }

// v within [0, 1]; NaN gives 0.
inline float clamp01(float v) { return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; }

// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3: 0 at 0, 1 at 1, flat at both ends to the second
// derivative (gradient noise's blend; the curl spacing's smoothstep).
inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// 0 at e0, 1 at e1 and a smooth S between, held past the ends (reversed edges run it backwards;
// equal edges make a step at e0), as GLSL and the expression language define it. (Slang's own
// smoothstep has no equal-edge step: hence the name.)
inline float smoothstepStd(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0f : 1.0f;
    const float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// smoothstep's straight sibling: 0 at e0, 1 at e1, a line between, held past the ends (equal
// edges: a step at e0).
inline float linearstep(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    return clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
}

// Perlin's bias: x (0 to 1) bent so that 0.5 goes to b, x^(log b / log 0.5). Not positive: 0; b of
// 1 or more: 1.
inline float biasOf(float x, float b) {
    if (!(x > 0.0f)) return 0.0f;
    if (!(b > 0.0f)) return 0.0f;
    if (b >= 1.0f) return 1.0f;
    return pmPow(x, pmLog(b) / pmLog(0.5f));
}

// Perlin's gain: an S (g below 0.5) or its inverse (above) through (0.5, 0.5), two biases back to back.
inline float gainOf(float x, float g) {
    return x < 0.5f ? 0.5f * biasOf(2.0f * x, 1.0f - g) : 1.0f - 0.5f * biasOf(2.0f - 2.0f * x, 1.0f - g);
}

// x from [a, b] to [c, d], not clamped (a == b: c).
inline float fitOf(float x, float a, float b, float c, float d) { return b != a ? c + (x - a) / (b - a) * (d - c) : c; }

// ---- Expression functions on the GPU ------------------------------------------------------------

// The expression builtins the GPU preview runs (look.slang fieldCall's cases), each a code of its
// own; an expression function's row in expression.cpp's kFns names its code (any other runs on the
// CPU). A new one: a constant here (any unused number), a case in fieldCall, the code in its row.
SHARED_CONST uint kFieldSin = 0u;
SHARED_CONST uint kFieldCos = 1u;
SHARED_CONST uint kFieldTan = 2u;
SHARED_CONST uint kFieldAsin = 3u;
SHARED_CONST uint kFieldAcos = 4u;
SHARED_CONST uint kFieldAtan = 5u;
SHARED_CONST uint kFieldAbs = 6u;
SHARED_CONST uint kFieldSqrt = 7u;
SHARED_CONST uint kFieldFloor = 8u;
SHARED_CONST uint kFieldCeil = 9u;
SHARED_CONST uint kFieldFract = 10u;
SHARED_CONST uint kFieldExp = 11u;
SHARED_CONST uint kFieldLog = 12u;
SHARED_CONST uint kFieldSign = 13u;
SHARED_CONST uint kFieldPow = 14u;
SHARED_CONST uint kFieldAtan2 = 15u;
SHARED_CONST uint kFieldMin = 16u;
SHARED_CONST uint kFieldMax = 17u;
SHARED_CONST uint kFieldMod = 18u;
SHARED_CONST uint kFieldStep = 19u;
SHARED_CONST uint kFieldClamp = 20u;
SHARED_CONST uint kFieldMix = 21u;
SHARED_CONST uint kFieldSmoothstep = 22u;
SHARED_CONST uint kFieldRound = 23u;
SHARED_CONST uint kFieldTrunc = 24u;
SHARED_CONST uint kFieldRadians = 25u;
SHARED_CONST uint kFieldDegrees = 26u;
SHARED_CONST uint kFieldGamma = 27u;
SHARED_CONST uint kFieldBias = 28u;
SHARED_CONST uint kFieldGain = 29u;
SHARED_CONST uint kFieldLerp = 30u;
SHARED_CONST uint kFieldLinearstep = 31u;
SHARED_CONST uint kFieldFit = 32u;

// ---- Ramps (ramp.h Ramp: a monotone cubic through its keys) --------------------------------------

// The slope between keys (p0, v0) and (p1, v1); 0 where they share a position.
inline float rampSecant(float p0, float v0, float p1, float v1) {
    const float h = p1 - p0;
    return h > 0.0f ? (v1 - v0) / h : 0.0f;
}
// Fritsch-Butland's tangent at an inner key: the spans either side h0, h1 long with slopes d0, d1;
// flat at a peak or valley (slopes of other signs, or one 0).
inline float rampInnerTangent(float h0, float h1, float d0, float d1) {
    if (d0 * d1 <= 0.0f) return 0.0f;
    return 3.0f * (h0 + h1) / ((2.0f * h1 + h0) / d0 + (h1 + 2.0f * h0) / d1);
}
// The cubic Hermite between values v0 and v1 at share f of the span, m0 and m1 the tangents
// times the span.
inline float rampHermite(float f, float v0, float m0, float v1, float m1) {
    const float f2 = f * f, f3 = f2 * f;
    return (2.0f * f3 - 3.0f * f2 + 1.0f) * v0 + (f3 - 2.0f * f2 + f) * m0 + (-2.0f * f3 + 3.0f * f2) * v1 + (f3 - f2) * m1;
}

// ---- Gradient noise (noise.h: gradientNoise, fractalNoise, vectorNoise and their 4D forms) -------

// The lattice hash: seed ^ ix C1 ^ iy C2 ^ iz C3 (^ iw C4 in 4D), then mixed (a 64-bit multiply
// and two shifts). Split as the GPU uses it: cornerHashXY's part is the same for every point of a
// strand-space noise at an octave (it walks z alone).
SHARED_CONST u64 kHashX = SHARED_U64(0x9E3779B1u, 0x85EBCA87u);
SHARED_CONST u64 kHashY = SHARED_U64(0xC2B2AE3Du, 0x27D4EB4Fu);
SHARED_CONST u64 kHashZ = SHARED_U64(0x165667B1u, 0x9E3779F9u);
SHARED_CONST u64 kHashW = SHARED_U64(0xD6E8FEB8u, 0x6659FD93u);
SHARED_CONST u64 kHashMix = SHARED_U64(0xBF58476Du, 0x1CE4E5B9u);

inline u64 cornerHashXY(u64 seed, lint ix, lint iy) { return seed ^ mul64(fromLattice(ix), kHashX) ^ mul64(fromLattice(iy), kHashY); }
inline u64 cornerHashZ(u64 xy, lint iz) {
    u64 h = xy ^ mul64(fromLattice(iz), kHashZ);
    h = h ^ shr64(h, 29u);
    h = mul64(h, kHashMix);
    h = h ^ shr64(h, 32u);
    return h;
}
inline u64 cornerHash(lint ix, lint iy, lint iz, u64 seed) { return cornerHashZ(cornerHashXY(seed, ix, iy), iz); }
inline u64 cornerHash4(lint ix, lint iy, lint iz, lint iw, u64 seed) {
    return cornerHashZ(cornerHashXY(seed, ix, iy) ^ mul64(fromLattice(iw), kHashW), iz);
}

// Perlin's 12 edge gradients.
SHARED_CONST float3 kGradients[12] = {float3(1, 1, 0),  float3(-1, 1, 0),  float3(1, -1, 0), float3(-1, -1, 0),
                                      float3(1, 0, 1),  float3(-1, 0, 1),  float3(1, 0, -1), float3(-1, 0, -1),
                                      float3(0, 1, 1),  float3(0, -1, 1),  float3(0, 1, -1), float3(0, -1, -1)};

// Channel k's (0 to 2) 16 bits of a corner hash, [16k, 16k + 16): independent noises from one
// lattice walk.
inline uint gradientBits(u64 h, uint k) { return (k == 0u ? lo32(h) : k == 1u ? (lo32(h) >> 16) : hi32(h)) & 0xffffu; }
// Channel k's gradient of a corner: its bits mod 12.
inline float3 gradientOf(u64 h, uint k) { return kGradients[gradientBits(h, k) % 12u]; }

// Perlin's 32 four-dimensional edge gradients: of channel k's bits mod 32, axis (bits % 32) >> 3 is
// 0 and the other three 1, in axis order each -1 where its bit (0, 1, 2) is set. Its dot with
// (dx, dy, dz, dw), summed in that order.
inline float gradient4(u64 h, uint k, float dx, float dy, float dz, float dw) {
    const uint i = gradientBits(h, k) & 31u, zero = i >> 3;
    float g[4];
    uint bit = 0u;
    SHARED_UNROLL for (uint a = 0u; a < 4u; ++a) {
        if (a == zero) {
            g[a] = 0.0f;
        } else {
            g[a] = ((i >> bit) & 1u) != 0u ? -1.0f : 1.0f;
            ++bit;
        }
    }
    return ((g[0] * dx + g[1] * dy) + g[2] * dz) + g[3] * dw;
}

// The first `channels` (1 to 3) of three gradient noises at q, from one lattice cell's 8 corners
// (corner c at +c&1, +c>>1&1, +c>>2&1) hashed with `seed`, blended along x, then y, then z; the
// other channels 0. Channel 0 alone is gradientNoise.
inline float3 latticeNoise(float3 q, u64 seed, uint channels) {
    const float fx = floor(q.x), fy = floor(q.y), fz = floor(q.z);
    const lint ix = lint(fx), iy = lint(fy), iz = lint(fz);
    const float dx = q.x - fx, dy = q.y - fy, dz = q.z - fz;
    const float u = fade(dx), v = fade(dy), w = fade(dz);
    float corner[3][8];
    SHARED_UNROLL for (uint c = 0u; c < 8u; ++c) {
        const lint ox = lint(c & 1u), oy = lint((c >> 1) & 1u), oz = lint((c >> 2) & 1u);
        const u64 h = cornerHashZ(cornerHashXY(seed, ix + ox, iy + oy), iz + oz);
        const float ex = dx - float(ox), ey = dy - float(oy), ez = dz - float(oz);
        SHARED_UNROLL for (uint k = 0u; k < 3u; ++k) {
            if (k < channels) {
                const float3 g = gradientOf(h, k);
                corner[k][c] = g.x * ex + g.y * ey + g.z * ez;
            }
        }
    }
    float n[3] = {0.0f, 0.0f, 0.0f};
    SHARED_UNROLL for (uint k = 0u; k < 3u; ++k) {
        if (k < channels) {
            const float x00 = mix1(corner[k][0], corner[k][1], u);
            const float x10 = mix1(corner[k][2], corner[k][3], u);
            const float x01 = mix1(corner[k][4], corner[k][5], u);
            const float x11 = mix1(corner[k][6], corner[k][7], u);
            n[k] = mix1(mix1(x00, x10, v), mix1(x01, x11, v), w);
        }
    }
    return float3(n[0], n[1], n[2]);
}

// latticeNoise in four dimensions (q, qw): 16 corners, corner c at (+c&1, +c>>1&1, +c>>2&1,
// +c>>3&1), blended along x, then y, z and w.
inline float3 latticeNoise4(float3 q, float qw, u64 seed, uint channels) {
    const float fx = floor(q.x), fy = floor(q.y), fz = floor(q.z), fw = floor(qw);
    const lint ix = lint(fx), iy = lint(fy), iz = lint(fz), iw = lint(fw);
    const float dx = q.x - fx, dy = q.y - fy, dz = q.z - fz, dw = qw - fw;
    const float u = fade(dx), v = fade(dy), w = fade(dz), t = fade(dw);
    float corner[3][16];
    SHARED_UNROLL for (uint c = 0u; c < 16u; ++c) {
        const lint ox = lint(c & 1u), oy = lint((c >> 1) & 1u), oz = lint((c >> 2) & 1u), ow = lint((c >> 3) & 1u);
        const u64 h = cornerHash4(ix + ox, iy + oy, iz + oz, iw + ow, seed);
        SHARED_UNROLL for (uint k = 0u; k < 3u; ++k) {
            if (k < channels) corner[k][c] = gradient4(h, k, dx - float(ox), dy - float(oy), dz - float(oz), dw - float(ow));
        }
    }
    float n[3] = {0.0f, 0.0f, 0.0f};
    SHARED_UNROLL for (uint k = 0u; k < 3u; ++k) {
        if (k < channels) {
            float atW[2];
            SHARED_UNROLL for (uint b = 0u; b < 2u; ++b) {
                const uint o = b * 8u;
                const float x00 = mix1(corner[k][o], corner[k][o + 1u], u);
                const float x10 = mix1(corner[k][o + 2u], corner[k][o + 3u], u);
                const float x01 = mix1(corner[k][o + 4u], corner[k][o + 5u], u);
                const float x11 = mix1(corner[k][o + 6u], corner[k][o + 7u], u);
                atW[b] = mix1(mix1(x00, x10, v), mix1(x01, x11, v), w);
            }
            n[k] = mix1(atW[0], atW[1], t);
        }
    }
    return float3(n[0], n[1], n[2]);
}

// An octave's weight in a fractal sum read at points `cells` of its lattice cells apart
// (noise.h octaveWeights): 1 to half a cell, fading to 0 at a whole cell.
inline float octaveWeightAt(float cells) { return 1.0f - smoothstepStd(0.5f, 1.0f, cells); }

// ---- Curl's spacing noise (curl_phase.h says what it is) -----------------------------------------

SHARED_CONST float kCurlTwoPi = 6.28318530718f;

// A 32-bit integer hash (Wellons' lowbias32).
inline uint curlHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The key of a curl's spacing noise, from the bits of its four draws.
inline uint curlSpacingKey(float r0, float r1, float r2, float r3) {
    return curlHash(asuint(r0) ^ curlHash(asuint(r1) ^ curlHash(asuint(r2) ^ curlHash(asuint(r3) ^ 0x5BAC1E5u))));
}

// The noise's layers: each a cell kCurlLayerTurns long, weighted (the weights sum to 1, so |n| < 1),
// salted. Two cell lengths apart, so the noise's zeros do not come at regular turns.
SHARED_CONST float kCurlLayerTurns[2] = {2.0f, 1.25f};
SHARED_CONST float kCurlLayerWeight[2] = {0.6f, 0.4f};
SHARED_CONST uint kCurlLayerSalt[2] = {0x9E3779B9u, 0x85EBCA6Bu};

// Lattice value i of a layer of key `key`: in [-1, 1).
inline float curlSpacingLattice(uint key, uint i) { return float(curlHash(key ^ curlHash(i)) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

// One layer at x (in its cells, 0 or more): its value n(x) = 8 (v1 - v0) u² (1 - u)² in cell
// floor(x) (C¹, 0 at the cell's ends, |n| < 1) ...
inline float curlSpacingValue(uint key, float x) {
    const float cell = floor(x);
    const float u = x - cell;
    const uint i = uint(cell);
    const float v0 = curlSpacingLattice(key, i), v1 = curlSpacingLattice(key, i + 1u);
    const float dv = v1 - v0;
    const float w = u * (1.0f - u);
    return 8.0f * dv * w * w;
}
// ... and its integral from 0, N(x) = (4/15) (v0 + (v1 - v0) S5(u) - v(0)), S5 the quintic
// smoothstep (each whole cell adds (4/15)(v1 - v0)).
inline float curlSpacingIntegral(uint key, float x) {
    const float cell = floor(x);
    const float u = x - cell;
    const uint i = uint(cell);
    const float v0 = curlSpacingLattice(key, i), v1 = curlSpacingLattice(key, i + 1u);
    const float dv = v1 - v0;
    return (4.0f / 15.0f) * (v0 + dv * fade(u) - curlSpacingLattice(key, 0u));
}

// The spacing noise n at `turns` (|f| w, 0 or more): in (-1, 1).
inline float curlSpacingRate(uint key, float turns) {
    float n = 0.0f;
    SHARED_UNROLL for (uint l = 0u; l < 2u; ++l) n += kCurlLayerWeight[l] * curlSpacingValue(key ^ kCurlLayerSalt[l], turns / kCurlLayerTurns[l]);
    return n;
}
// S(turns) = ∫0^turns n: the turns the noise adds by then (per unit of spacingRandom).
inline float curlSpacingTurns(uint key, float turns) {
    float total = 0.0f;
    SHARED_UNROLL for (uint l = 0u; l < 2u; ++l) {
        total += kCurlLayerWeight[l] * kCurlLayerTurns[l] * curlSpacingIntegral(key ^ kCurlLayerSalt[l], turns / kCurlLayerTurns[l]);
    }
    return total;
}

// The curl's angle (radians) at profiled arc w, for frequency f (the strand's, varied), start
// `phase` (radians) and the spacing noise's amount and key. (With spacingRandom 0: 2π f w + φ.)
inline float curlAngle(float frequency, float w, float phase, float spacingRandom, uint key) {
    float angle = kCurlTwoPi * frequency * w + phase;
    if (spacingRandom > 0.0f) {
        const float extra = spacingRandom * curlSpacingTurns(key, abs(frequency) * w);
        angle += kCurlTwoPi * (frequency < 0.0f ? -extra : extra);
    }
    return angle;
}

// Its turns at profiled arc w, from the start (the angle less the phase, over 2π).
inline float curlTurns(float frequency, float w, float spacingRandom, uint key) {
    float turns = frequency * w;
    if (spacingRandom > 0.0f) {
        const float extra = spacingRandom * curlSpacingTurns(key, abs(frequency) * w);
        turns += frequency < 0.0f ? -extra : extra;
    }
    return turns;
}

// The local frequency (turns per unit of length, signed as f) where frequencyProfile is
// `profileValue` and the profiled arc is w.
inline float curlLocalFrequency(float frequency, float profileValue, float w, float spacingRandom, uint key) {
    const float p = profileValue > 0.0f ? profileValue : 0.0f;
    const float n = spacingRandom > 0.0f ? curlSpacingRate(key, abs(frequency) * w) : 0.0f;
    return frequency * p * (1.0f + spacingRandom * n);
}

#ifndef __SLANG__
}  // namespace groomlab::shared
#endif
