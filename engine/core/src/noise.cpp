// SPDX-License-Identifier: Apache-2.0
#include "groomlab/noise.h"

#include <algorithm>
#include <cmath>

#include "groomlab/random.h"

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_NOISE_SSE2 1
#include <emmintrin.h>
#else
#define GROOMLAB_NOISE_SSE2 0
#endif

namespace groomlab {

// The lattice hashes, gradients and per-cell noise are shared/groom_math.h's, the GPU preview's too.
using shared::cornerHash;
using shared::gradientOf;
using shared::kGradients;
using shared::latticeNoise;
using shared::latticeNoise4;

namespace {

// The 8 corner hashes of lattice cell (ix, iy, iz), corner c at (+c&1, +c>>1&1, +c>>2&1).
void cellHashes(std::int64_t ix, std::int64_t iy, std::int64_t iz, std::uint64_t seed, std::uint64_t out[8]) {
    for (int c = 0; c < 8; ++c) out[c] = cornerHash(ix + (c & 1), iy + ((c >> 1) & 1), iz + ((c >> 2) & 1), seed);
}

}  // namespace

float gradientNoise(const Vec3f& p, std::uint64_t seed) { return latticeNoise(p, seed, 1).x; }

float fractalNoise(const Vec3f& p, std::uint64_t seed, int octaves) {
    float sum = 0.0f, amplitude = 1.0f, norm = 0.0f;
    Vec3f q = p;
    for (int o = 0; o < std::max(octaves, 1); ++o) {
        sum += gradientNoise(q, hashCombine(seed, static_cast<std::uint64_t>(o))) * amplitude;
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.0f;
    }
    return sum / norm;
}

float fractalNoise4(const Vec3f& p, float w, std::uint64_t seed, int octaves) {
    float sum = 0.0f, amplitude = 1.0f, norm = 0.0f;
    Vec3f q = p;
    float qw = w;
    for (int o = 0; o < std::max(octaves, 1); ++o) {
        sum += latticeNoise4(q, qw, hashCombine(seed, static_cast<std::uint64_t>(o)), 1).x * amplitude;
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.0f;
        qw *= 2.0f;
    }
    return sum / norm;
}

Vec3f vectorNoise4(const Vec3f& p, float w, std::uint64_t seed, int octaves) {
    Vec3f sum;
    float amplitude = 1.0f, norm = 0.0f;
    Vec3f q = p;
    float qw = w;
    for (int o = 0; o < std::max(octaves, 1); ++o) {
        sum += latticeNoise4(q, qw, hashCombine(seed, static_cast<std::uint64_t>(o)), 3) * amplitude;
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.0f;
        qw *= 2.0f;
    }
    return sum / norm;
}

namespace {
int fbmOctaves(float octaves) { return static_cast<int>(std::clamp(octaves, 1.0f, 8.0f)); }
std::uint64_t fbmSeed(FbmSeeds seeds, int i) { return i == 0 ? seeds.first : seeds.base + static_cast<std::uint64_t>(i); }
}  // namespace

float fbmNoise(const Vec3f& p, float octaves, float lacunarity, float gain, bool turbulent, FbmSeeds seeds) {
    const int count = fbmOctaves(octaves);
    float frequency = 1.0f, amplitude = 1.0f, sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float n = gradientNoise(p * frequency, fbmSeed(seeds, i));
        sum += amplitude * (turbulent ? std::abs(n) : n);
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum;
}

Vec3f vectorFbmNoise(const Vec3f& p, float octaves, float lacunarity, float gain, bool turbulent, FbmSeeds seeds) {
    const int count = fbmOctaves(octaves);
    float frequency = 1.0f, amplitude = 1.0f;
    Vec3f sum{};
    for (int i = 0; i < count; ++i) {
        Vec3f n = vectorNoise(p * frequency, fbmSeed(seeds, i), 1);
        if (turbulent) n = {std::abs(n.x), std::abs(n.y), std::abs(n.z)};
        sum += n * amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum;
}

float fbmNoise4(const Vec3f& p, float w, float octaves, float lacunarity, float gain, FbmSeeds seeds) {
    const int count = fbmOctaves(octaves);
    float frequency = 1.0f, amplitude = 1.0f, sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        sum += amplitude * fractalNoise4(p * frequency, w * frequency, fbmSeed(seeds, i), 1);
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum;
}

Vec3f vectorFbmNoise4(const Vec3f& p, float w, float octaves, float lacunarity, float gain, FbmSeeds seeds) {
    const int count = fbmOctaves(octaves);
    float frequency = 1.0f, amplitude = 1.0f;
    Vec3f sum{};
    for (int i = 0; i < count; ++i) {
        sum += vectorNoise4(p * frequency, w * frequency, fbmSeed(seeds, i), 1) * amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum;
}

std::uint64_t cellHash(float fx, float fy, float fz, std::uint64_t seed) {
    const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy), iz = static_cast<std::int64_t>(fz);
    std::uint64_t h = hashCombine(seed, static_cast<std::uint64_t>(ix));
    h = hashCombine(h, static_cast<std::uint64_t>(iy));
    return hashCombine(h, static_cast<std::uint64_t>(iz));
}

float periodicNoise(const Vec3f& p, const Vec3f& period, std::uint64_t seed) {
    const std::int64_t px = std::max<std::int64_t>(1, truncToInt(period.x)), py = std::max<std::int64_t>(1, truncToInt(period.y)),
                       pz = std::max<std::int64_t>(1, truncToInt(period.z));
    const float fx = latticeFloor(p.x), fy = latticeFloor(p.y), fz = latticeFloor(p.z);
    const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy), iz = static_cast<std::int64_t>(fz);
    const float dx = p.x - fx, dy = p.y - fy, dz = p.z - fz;
    auto wrap = [](std::int64_t i, std::int64_t n) { return ((i % n) + n) % n; };
    float corner[8];
    for (int c = 0; c < 8; ++c) {
        const int ox = c & 1, oy = (c >> 1) & 1, oz = (c >> 2) & 1;
        const std::uint64_t h = cellHash(static_cast<float>(wrap(ix + ox, px)), static_cast<float>(wrap(iy + oy, py)),
                                         static_cast<float>(wrap(iz + oz, pz)), seed);
        const Vec3f& g = kGradients[h % 12u];
        corner[c] = g.x * (dx - static_cast<float>(ox)) + g.y * (dy - static_cast<float>(oy)) + g.z * (dz - static_cast<float>(oz));
    }
    const float u = fade(dx), v = fade(dy), w = fade(dz);
    return mix1(mix1(mix1(corner[0], corner[1], u), mix1(corner[2], corner[3], u), v),
                mix1(mix1(corner[4], corner[5], u), mix1(corner[6], corner[7], u), v), w);
}

void octaveWeights(float spacing, int octaves, float* out) {
    float cells = spacing;  // the spacing in this octave's cells
    for (int o = 0; o < octaves; ++o) {
        out[o] = spacing > 0.0f ? shared::octaveWeightAt(cells) : 1.0f;
        cells *= 2.0f;
    }
}

Vec3f vectorNoise(const Vec3f& p, std::uint64_t seed, int octaves, const float* weights) {
    Vec3f sum;
    float amplitude = 1.0f, norm = 0.0f;
    Vec3f q = p;
    for (int o = 0; o < std::max(octaves, 1); ++o) {
        // A faded octave (weight 0) adds nothing: skipped, the sum as if it were added.
        const float w = weights ? weights[o] : 1.0f;
        if (w != 0.0f) {
            sum += latticeNoise(q, hashCombine(seed, static_cast<std::uint64_t>(o)), 3) * (amplitude * w);
        }
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.0f;
    }
    return sum / norm;
}

VectorNoise::VectorNoise(std::uint64_t seed, int octaves, const float* weights) : count_(std::clamp(octaves, 1, kMaxOctaves)) {
    for (int o = 0; o < count_; ++o) {
        octaves_[o].seed = hashCombine(seed, static_cast<std::uint64_t>(o));
        weights_[o] = weights ? weights[o] : 1.0f;
    }
}

VectorNoise::VectorNoise(const std::uint64_t* octaveSeeds, int octaves, const float* weights) : count_(std::clamp(octaves, 1, kMaxOctaves)) {
    for (int o = 0; o < count_; ++o) {
        octaves_[o].seed = octaveSeeds[o];
        weights_[o] = weights ? weights[o] : 1.0f;
    }
}

Vec3f VectorNoise::operator()(const Vec3f& p) {
    // vectorNoise's sum, octave by octave in the same order.
    Vec3f sum;
    float amplitude = 1.0f, norm = 0.0f;
    Vec3f q = p;
    for (int o = 0; o < count_; ++o) {
        if (weights_[o] == 0.0f) {  // faded out: adds nothing (vectorNoise skips it too)
            norm += amplitude;
            amplitude *= 0.5f;
            q = q * 2.0f;
            continue;
        }
        Octave& oct = octaves_[o];
        const float fx = std::floor(q.x), fy = std::floor(q.y), fz = std::floor(q.z);
        const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy), iz = static_cast<std::int64_t>(fz);
        if (!oct.valid || ix != oct.ix || iy != oct.iy || iz != oct.iz) {
            std::uint64_t hashes[8];
            cellHashes(ix, iy, iz, oct.seed, hashes);
            for (int c = 0; c < 8; ++c) {
                for (std::uint32_t k = 0; k < 3; ++k) {
                    const Vec3f g = gradientOf(hashes[c], k);
                    oct.g[k][0][c] = g.x;
                    oct.g[k][1][c] = g.y;
                    oct.g[k][2][c] = g.z;
                }
            }
            oct.ix = ix;
            oct.iy = iy;
            oct.iz = iz;
            oct.valid = true;
            oct.xyValid = false;
        }
        // noiseFromCell's arithmetic, in its order: g.x * (dx - ox) + g.y * (dy - oy) + g.z * (dz - oz).
        if (!oct.xyValid || q.x != oct.x || q.y != oct.y) {
            const float dx = q.x - fx, dy = q.y - fy;
            oct.u = fade(dx);
            oct.v = fade(dy);
            for (int k = 0; k < 3; ++k) {
                for (int c = 0; c < 8; ++c) {
                    oct.xy[k][c] = oct.g[k][0][c] * (dx - static_cast<float>(c & 1)) +
                                   oct.g[k][1][c] * (dy - static_cast<float>((c >> 1) & 1));
                }
            }
            oct.x = q.x;
            oct.y = q.y;
            oct.xyValid = true;
        }
        const float dz = q.z - fz;
        const float w = fade(dz);
        const float dz1 = dz - 1.0f;
        float v[3];
#if GROOMLAB_NOISE_SSE2
        // Four lanes at a time, the same operations per lane as the scalar code below.
        const __m128 vz0 = _mm_set1_ps(dz), vz1 = _mm_set1_ps(dz1), vu = _mm_set1_ps(oct.u), vv = _mm_set1_ps(oct.v);
        for (int k = 0; k < 3; ++k) {
            const __m128 lo = _mm_add_ps(_mm_loadu_ps(oct.xy[k]), _mm_mul_ps(_mm_loadu_ps(oct.g[k][2]), vz0));
            const __m128 hi = _mm_add_ps(_mm_loadu_ps(oct.xy[k] + 4), _mm_mul_ps(_mm_loadu_ps(oct.g[k][2] + 4), vz1));
            const __m128 even = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));  // c0 c2 c4 c6
            const __m128 odd = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));   // c1 c3 c5 c7
            const __m128 xs = _mm_add_ps(even, _mm_mul_ps(_mm_sub_ps(odd, even), vu));  // x00 x10 x01 x11
            const __m128 a = _mm_shuffle_ps(xs, xs, _MM_SHUFFLE(2, 0, 2, 0));          // x00 x01
            const __m128 b = _mm_shuffle_ps(xs, xs, _MM_SHUFFLE(3, 1, 3, 1));          // x10 x11
            alignas(16) float y[4];
            _mm_store_ps(y, _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), vv)));
            v[k] = mix1(y[0], y[1], w);
        }
#else
        for (int k = 0; k < 3; ++k) {
            float corner[8];
            for (int c = 0; c < 8; ++c) corner[c] = oct.xy[k][c] + oct.g[k][2][c] * (c < 4 ? dz : dz1);
            const float x00 = mix1(corner[0], corner[1], oct.u);
            const float x10 = mix1(corner[2], corner[3], oct.u);
            const float x01 = mix1(corner[4], corner[5], oct.u);
            const float x11 = mix1(corner[6], corner[7], oct.u);
            v[k] = mix1(mix1(x00, x10, oct.v), mix1(x01, x11, oct.v), w);
        }
#endif
        sum += Vec3f(v[0], v[1], v[2]) * (amplitude * weights_[o]);
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.0f;
    }
    return sum / norm;
}

}  // namespace groomlab
