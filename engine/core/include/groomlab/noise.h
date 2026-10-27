// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "groomlab/math.h"

namespace groomlab {

// Seeded 3D gradient (Perlin-style) noise, roughly in [-1, 1], smooth
// (quintic fade) and deterministic across platforms and threads.
float gradientNoise(const Vec3f& p, std::uint64_t seed);

// Fractal sum of `octaves` gradient noises, each doubling frequency and
// halving amplitude, normalized to about [-1, 1].
float fractalNoise(const Vec3f& p, std::uint64_t seed, int octaves);

// fractalNoise and vectorNoise in four dimensions (p, w), with Perlin's 32 four-dimensional
// gradients: a field that can walk a closed path through its last two (Wind's loop: a circle
// there comes back to where it started, smoothly).
float fractalNoise4(const Vec3f& p, float w, std::uint64_t seed, int octaves);
Vec3f vectorNoise4(const Vec3f& p, float w, std::uint64_t seed, int octaves);

constexpr int kMaxNoiseOctaves = 8;

// Each octave's share of a fractal sum read at points `spacing` apart (in p's units: a strand's
// point spacing in the noise's space), `octaves` values into `out`. Octave o's lattice cells are
// 2^-o across, and an octave fades out as the spacing grows from half a cell (its bumps, a cell
// wide, drawn by two points or more) to a whole cell (one point a bump: the points sample it
// above its Nyquist rate, a look that changes with the point count and shimmers), the way
// renderers filter noise to a footprint. No spacing (0 or less): every octave in full.
void octaveWeights(float spacing, int octaves, float* out);

// Three decorrelated fractal noises: a smooth random displacement direction. With `weights`
// (octaveWeights, one per octave) each octave's amplitude is scaled by its weight while the sum
// stays normalized by the unscaled amplitudes: a strand with fewer points gets the same noise
// less its finer octaves, not other noise.
Vec3f vectorNoise(const Vec3f& p, std::uint64_t seed, int octaves, const float* weights = nullptr);

// SeExpr's FBM (the expressions' fbm, vfbm, turbulence...): `octaves` gradient noises (1 to 8,
// truncated), each `lacunarity` times the frequency and `gain` times the amplitude of the one
// before, summed and not normalized (fractalNoise is); turbulent, their absolute values. Octave 0
// is hashed with `first`, octave i with `base` + i.
struct FbmSeeds {
    std::uint64_t first, base;
};
float fbmNoise(const Vec3f& p, float octaves, float lacunarity, float gain, bool turbulent, FbmSeeds seeds);
Vec3f vectorFbmNoise(const Vec3f& p, float octaves, float lacunarity, float gain, bool turbulent, FbmSeeds seeds);
// In four dimensions (p, w), not turbulent.
float fbmNoise4(const Vec3f& p, float w, float octaves, float lacunarity, float gain, FbmSeeds seeds);
Vec3f vectorFbmNoise4(const Vec3f& p, float w, float octaves, float lacunarity, float gain, FbmSeeds seeds);

// A float floored to a lattice coordinate, kept within ±1e7 (where a float, and an int64, hold
// whole numbers exactly).
inline float latticeFloor(float v) { return std::floor(std::clamp(v, -1.0e7f, 1.0e7f)); }

// A lattice cell's hash from its corner (whole numbers, as floats): hashCombine of the three, not
// the gradient noises' corner hash. SeExpr's cell functions (the expressions' cellnoise, voronoi,
// pnoise) are keyed by it.
std::uint64_t cellHash(float fx, float fy, float fz, std::uint64_t seed);

// Gradient noise repeating every `period` cells along each axis (each component's whole part, 1
// at least), about -1 to 1: the corners hashed by cellHash, their gradient its value mod 12.
float periodicNoise(const Vec3f& p, const Vec3f& period, std::uint64_t seed);

// vectorNoise for a run of nearby points (a strand's, root to tip): the same values, bit for
// bit, with each octave's lattice corners hashed and their gradients looked up once per cell
// rather than once per point, and, while x and y stay the same from one point to the next (a
// strand-space noise walks z alone), the gradients' x and y terms kept too. One per thread or
// strand; cheap to make. Octaves 1 to kMaxOctaves (clamped). `weights` as vectorNoise's.
class VectorNoise {
public:
    VectorNoise(std::uint64_t seed, int octaves, const float* weights = nullptr);
    // Each octave's seed given (hashCombine(seed, octave), as the GPU preview's constants hold them).
    VectorNoise(const std::uint64_t* octaveSeeds, int octaves, const float* weights);
    Vec3f operator()(const Vec3f& p);

    static constexpr int kMaxOctaves = kMaxNoiseOctaves;

private:
    struct Octave {
        std::uint64_t seed = 0;
        std::int64_t ix = 0, iy = 0, iz = 0;
        bool valid = false;
        float g[3][3][8] = {};    // [channel][axis][corner]: the corners' gradients
        float x = 0.0f, y = 0.0f;  // the point x and y the terms below are for
        bool xyValid = false;
        float u = 0.0f, v = 0.0f;  // fade(dx), fade(dy)
        float xy[3][8] = {};       // [channel][corner]: gx * (dx - ox) + gy * (dy - oy)
    };
    Octave octaves_[kMaxOctaves];
    float weights_[kMaxOctaves];
    int count_;
};

}  // namespace groomlab
