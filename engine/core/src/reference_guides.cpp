// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos, part 2: the views prepared (working size, masks, what the scalp and
// occluders hide), the shell, the grower, symmetry and the look. See reference_guides.h.

#include "groomlab/portable_math.h"
#include "groomlab/reference_guides.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "groomlab/brush.h"
#include "groomlab/color.h"
#include "groomlab/curve.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mirror.h"
#include "groomlab/strand_set.h"
#include "groomlab/export_view.h"
#include "groomlab/guide_rooting.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---- Small linear algebra ---------------------------------------------------------------------

struct Sym3 {
    double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    void addOuter(const Vec3f& v, double w) {
        const double a[3] = {v.x, v.y, v.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) m[i][j] += w * a[i] * a[j];
    }
    void addIdentity(double w) {
        for (int i = 0; i < 3; ++i) m[i][i] += w;
    }
    void add(const Sym3& o, double w) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) m[i][j] += w * o.m[i][j];
    }
};

// The eigenvector of the smallest eigenvalue (Jacobi rotations).
Vec3f smallestEigenvector(const Sym3& s) {
    double a[3][3], v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[i][j] = s.m[i][j];
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-24) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-30) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq;
                    a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk;
                    a[q][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - sn * vkq;
                    v[k][q] = sn * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (a[i][i] < a[best][best]) best = i;
    return Vec3f(static_cast<float>(v[0][best]), static_cast<float>(v[1][best]), static_cast<float>(v[2][best])).normalized();
}

Vec3f anyPerp(const Vec3f& n) {
    const Vec3f a = std::abs(n.x) < 0.9f ? Vec3f(1, 0, 0) : Vec3f(0, 1, 0);
    return n.cross(a).normalized();
}

// ---- Distance transform (Felzenszwalb & Huttenlocher) ---------------------------------------------

void edt1d(const std::vector<float>& f, std::vector<float>& d, std::vector<int>& v, std::vector<float>& z, int n) {
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float s = 0.0f;
        while (true) {
            const int p = v[static_cast<std::size_t>(k)];
            s = ((f[static_cast<std::size_t>(q)] + static_cast<float>(q * q)) - (f[static_cast<std::size_t>(p)] + static_cast<float>(p * p))) /
                (2.0f * static_cast<float>(q - p));
            if (s <= z[static_cast<std::size_t>(k)] && k > 0) {
                --k;
                continue;
            }
            break;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k + 1)] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k + 1)] < static_cast<float>(q)) ++k;
        const int p = v[static_cast<std::size_t>(k)];
        d[static_cast<std::size_t>(q)] = static_cast<float>((q - p) * (q - p)) + f[static_cast<std::size_t>(p)];
    }
}

// Pixel distance to the nearest set pixel (a large number where none is set).
std::vector<float> distanceTransform(const std::vector<std::uint8_t>& set, int w, int h) {
    const float big = 1e10f;
    std::vector<float> grid(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < grid.size(); ++i) grid[i] = set[i] ? 0.0f : big;
    const int n = std::max(w, h);
    std::vector<float> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y * w + x)];
        edt1d(f, d, v, z, h);
        for (int y = 0; y < h; ++y) grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(y)];
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f[static_cast<std::size_t>(x)] = grid[static_cast<std::size_t>(y * w + x)];
        edt1d(f, d, v, z, w);
        for (int x = 0; x < w; ++x) grid[static_cast<std::size_t>(y * w + x)] = std::sqrt(d[static_cast<std::size_t>(x)]);
    }
    return grid;
}

// The measured look's calibration (tools/research/reference_guides/look_sweep.py, the synthetic
// look sweep: measured against the true graph values).
constexpr float kCurlRadiusGain = 4.0f;  // the radius from the swing's amplitude (the Gabor filters and strands out of
                                         // phase read it about 4x low)
constexpr float kTipThreshold = 0.25f;  // |log(tip / root brightness)| below this: no tips (curls alone reach 0.2)
constexpr float kTipFull = 1.6f;        // ... at this, tipStrength 1
constexpr float kClumpPeriodic = 0.2f;  // the share of profiles with a period over which hair is clumped
// Clump size (the centers' spacing at the roots) from the spacing seen across the flow at mid-length:
// compressed (hair spreads past the scalp; the smallest clumps blur), so size = (seen - offset) / slope;
// curls widen what is seen (each lock swings): a share of the curls' radius off first.
constexpr float kClumpOffset = 0.90f, kClumpSlope = 0.49f, kClumpCurlWiden = 0.4f;
// Clump strength from the profile's swing (deeper gaps between tighter locks); curls deepen the swing,
// the more the larger they are against the clumps' spacing.
constexpr float kClumpStrength0 = -0.065f, kClumpStrength1 = 4.66f, kClumpCurlSwing = 1.9f;
// Flyaway fraction from the strays a pixel of outline (minus what any hair's ends show), where the
// hair stands out from the backdrop (a stray of hair the backdrop's own shade cannot be seen); curly
// hair's locks leave the outline of their own (a share of it counts).
constexpr float kFlyawayGain = 0.111f, kFlyawayFloor = 0.5f, kFlyawayContrast = 0.08f, kFlyawayCurly = 0.3f;
// Frizz Noise fraction from the share of ridges crossing the flow (straight hair: curls and short fur
// cross it of their own).
constexpr float kFrizzGain = 0.94f, kFrizzFloor = 0.05f;
// Curls: a clear share of the flow swinging with a period, by more than this.
constexpr float kCurlShare = 0.22f, kCurlAmplitude = 7.5f;
// ... or a smaller share swinging widely: waves, or curls a photo resolves only in places (a small
// photo, curls in shadow; straight hair's flow swings a degree or two, its noise without a period).
// Curly too (a Curl node puts them back); guides along each wave would wander inside the hair and
// never reach its ends: they follow the coarse flow.
constexpr float kWavyShare = 0.12f, kWavyAmplitude = 15.0f;
// A photo's exposure: what its brightest half percent (balanced) reaches in a well exposed photo
// (skin's highlights, a light backdrop; linear). A photo short of it is brightened to it, never
// darkened: a mid-gray studio backdrop's photo is not taken for underexposed (its dark hair then
// read a medium brown).
constexpr float kPhotoWhite = 0.5f;

// A separable Gaussian blur of `channels` floats a pixel (clamped at the borders).
std::vector<float> gaussianBlurred(const std::vector<float>& in, int w, int h, int channels, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(2.5f * sigma)));
    std::vector<float> g(static_cast<std::size_t>(2 * r + 1));
    float total = 0.0f;
    for (int i = -r; i <= r; ++i) total += g[static_cast<std::size_t>(i + r)] = pm::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
    for (float& x : g) x /= total;
    const std::size_t c = static_cast<std::size_t>(channels);
    std::vector<float> tmp(in.size(), 0.0f), out(in.size(), 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int i = -r; i <= r; ++i) {
                const int xx = std::clamp(x + i, 0, w - 1);
                for (std::size_t k = 0; k < c; ++k)
                    tmp[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c + k] +=
                        g[static_cast<std::size_t>(i + r)] * in[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)) * c + k];
            }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int i = -r; i <= r; ++i) {
                const int yy = std::clamp(y + i, 0, h - 1);
                for (std::size_t k = 0; k < c; ++k)
                    out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c + k] +=
                        g[static_cast<std::size_t>(i + r)] * tmp[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c + k];
            }
    return out;
}

// A picture's strand directions from its brightness gradients: the structure tensor (Sobel) smoothed
// over `sigma` pixels, the strands across the gradient, the coherence the confidence (as an
// OrientationField: its tensor the doubled-angle vectors sample() reads). On curls it follows each
// strand where the Gabor bank (tuned to straight stripes) blurs them.
OrientationField structureField(const RefImage& image, float sigma) {
    OrientationField f;
    const int w = image.width, h = image.height;
    f.width = w;
    f.height = h;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> t(3 * n, 0.0f);
    const std::size_t channels = static_cast<std::size_t>(std::max(1, image.channels));
    auto at = [&](int x, int y) {
        const std::size_t q = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * channels;
        if (channels < 3) return image.pixels[q];
        return (image.pixels[q] + image.pixels[q + 1] + image.pixels[q + 2]) / 3.0f;
    };
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            const float gx = (at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0f * at(x - 1, y) + at(x - 1, y + 1));
            const float gy = (at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0f * at(x, y - 1) + at(x + 1, y - 1));
            const std::size_t q = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            t[3 * q] = gx * gx;
            t[3 * q + 1] = gy * gy;
            t[3 * q + 2] = gx * gy;
        }
    t = gaussianBlurred(t, w, h, 3, sigma);
    f.angle.assign(n, 0.0f);
    f.confidence.assign(n, 0.0f);
    f.tensor.assign(2 * n, 0.0f);
    for (std::size_t q = 0; q < n; ++q) {
        const float a = t[3 * q], b = t[3 * q + 1], c = t[3 * q + 2];
        const float norm = std::hypot(a - b, 2.0f * c), coherence = norm / std::max(1e-9f, a + b);
        if (norm < 1e-12f) continue;
        // (the strands across the gradient: the doubled angle turned half a turn)
        f.tensor[2 * q] = -coherence * (a - b) / norm;
        f.tensor[2 * q + 1] = -coherence * 2.0f * c / norm;
        f.confidence[q] = coherence;
        f.angle[q] = std::fmod(0.5f * pm::atan2(2.0f * c, a - b) + 0.5f * kPi + kPi, kPi);
    }
    return f;
}

float median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    return values[mid];
}

}  // namespace

// ---- Prepared views -------------------------------------------------------------------------------

// A lock's path traced in one view (lockPaths): pixels a step apart, root end first.
struct LockLine {
    std::vector<std::array<float, 2>> line;
    float say = 0.0f;    // the flow's say summed along it (pixels where the flow held the lock's way)
    float score = 0.0f;  // what it was chosen by (its say, its length, falling)
    bool kept = false;   // a lock of its own (not running along a clearer one kept already)
};

struct PreparedView {
    RefCamera camera;  // at the working size
    int width = 0, height = 0;
    RefImage rgb;      // at the working size (may be empty)
    RefImage mask;     // 0-1
    RefImage hullMask; // the mask grown by two pixels (what the hull and the stop read)
    RefImage rootMask; // the mask shrunk a little (where roots may grow)
    std::shared_ptr<const OrientationField> field;
    std::vector<float> depth;           // nearest scalp or occluder, along the view (inf: none)
    std::vector<std::uint8_t> scalpCover;
    std::vector<float> scalpDistance;   // pixels to the scalp's projection
    float rimPx = 0.0f;                 // how far hair reaches past the scalp's outline, pixels
    float rimWorld = 0.0f;              // the same in scene units, at the scalp's typical depth
    std::array<int, 4> crop{0, 0, 0, 0};  // the part of the photo worked on (x0, y0, w, h; its pixels)
    float scalpDepth = 0.0f;            // the scalp's typical depth along the view (0: unseen)
    float reachWorld = 0.0f;            // how far the hair reaches past the scalp's outline (its 90th percentile), scene units
    std::shared_ptr<const OrientationField> coarse;  // the field smoothed past its curls
    std::shared_ptr<const OrientationField> fine;    // the field with its curls (field may become coarse)
    std::vector<float> strandLength;  // a pixel: the longest a strand path runs on from it (strandPathLength; scene units, 0 none)
    CurlEstimate curl;
    // Lock paths: the lines traced from each seed root (indexed as the seeds; empty: not traced here)
    // and the curl period they were traced at (pixels).
    std::vector<LockLine> lockLines;
    float lockPeriodPx = 0.0f;
    std::vector<float> hintWay;         // 3 a pixel: the directed hints' way (u, v) and their strength (empty: none)
    std::vector<float> hintLength;      // 2 a pixel: how long the drawn lines say the hair is from there on (scene units)
                                        // and how much they say it (0-1; empty: none)
    float depthEps = 0.0f;

    bool inFrame(float u, float v) const { return u >= 0 && v >= 0 && u < static_cast<float>(width) && v < static_cast<float>(height); }
    std::size_t pixel(float u, float v) const {
        const int x = std::clamp(static_cast<int>(u), 0, width - 1), y = std::clamp(static_cast<int>(v), 0, height - 1);
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    }
    bool hidden(float u, float v, float z) const {
        const float d = depth[pixel(u, v)];
        return z > d + depthEps + 0.01f * d;
    }
};

namespace {

struct ProjectedMesh {
    std::vector<float> u, v, z;
    std::vector<std::uint8_t> ok;
};

ProjectedMesh projectMesh(const ScalpMesh& mesh, const RefCamera& camera) {
    ProjectedMesh pm;
    const std::size_t n = mesh.pointCount();
    pm.u.resize(n);
    pm.v.resize(n);
    pm.z.resize(n);
    pm.ok.resize(n);
    for (std::size_t i = 0; i < n; ++i) pm.ok[i] = camera.project(mesh.points()[i], pm.u[i], pm.v[i], &pm.z[i]) ? 1 : 0;
    return pm;
}

// Rasterizes the mesh's triangles into `depth` (nearest along the view; 1/z interpolated) and, with
// `cover`, marks the pixels it covers.
void rasterize(const ScalpMesh& mesh, const ProjectedMesh& pm, int w, int h, std::vector<float>& depth, std::vector<std::uint8_t>* cover) {
    const auto& tris = mesh.triangles();
    const int band = 16;
    const int bands = (h + band - 1) / band;
    parallelFor(0, static_cast<std::size_t>(bands), [&](std::size_t b) {
        const int ylo = static_cast<int>(b) * band, yhi = std::min(h, ylo + band);
        for (const auto& t : tris) {
            const std::size_t i0 = static_cast<std::size_t>(t[0]), i1 = static_cast<std::size_t>(t[1]), i2 = static_cast<std::size_t>(t[2]);
            if (!pm.ok[i0] || !pm.ok[i1] || !pm.ok[i2]) continue;
            const float x0 = pm.u[i0], y0 = pm.v[i0], x1 = pm.u[i1], y1 = pm.v[i1], x2 = pm.u[i2], y2 = pm.v[i2];
            const float minY = std::min({y0, y1, y2}), maxY = std::max({y0, y1, y2});
            if (maxY < static_cast<float>(ylo) - 0.5f || minY > static_cast<float>(yhi) + 0.5f) continue;
            const float minX = std::min({x0, x1, x2}), maxX = std::max({x0, x1, x2});
            if (maxX < 0.0f || minX > static_cast<float>(w)) continue;
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::abs(area) < 1e-12f) continue;
            const int px0 = std::max(0, static_cast<int>(std::floor(minX))), px1 = std::min(w - 1, static_cast<int>(std::ceil(maxX)));
            const int py0 = std::max(ylo, static_cast<int>(std::floor(minY))), py1 = std::min(yhi - 1, static_cast<int>(std::ceil(maxY)));
            const float iz0 = 1.0f / pm.z[i0], iz1 = 1.0f / pm.z[i1], iz2 = 1.0f / pm.z[i2];
            for (int y = py0; y <= py1; ++y) {
                const float cy = static_cast<float>(y) + 0.5f;
                for (int x = px0; x <= px1; ++x) {
                    const float cx = static_cast<float>(x) + 0.5f;
                    const float w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) / area;
                    const float w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    const float z = 1.0f / (w0 * iz0 + w1 * iz1 + w2 * iz2);
                    const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                    if (z < depth[at]) depth[at] = z;
                    if (cover) (*cover)[at] = 1;
                }
            }
        }
    }, 1);
}

// The backdrop's color: the median of the image's border.
std::array<float, 3> borderColor(const RefImage& image) {
    std::vector<float> border[3];
    const int c = std::min(image.channels, 3);
    const int pad = std::max(2, std::min(image.width, image.height) / 40);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            if ((x >= pad && x < image.width - pad) && (y >= pad && y < image.height - pad)) continue;
            for (int k = 0; k < c; ++k) border[k].push_back(image.at(x, y, k));
        }
    std::array<float, 3> bg{0, 0, 0};
    for (int k = 0; k < c; ++k) bg[static_cast<std::size_t>(k)] = median(border[k]);
    return bg;
}

// The backdrop as a smooth fit of the photo's border: per channel a quadratic in x and y over the
// photo, so a lit backdrop's gradient (lighter on one side) is not taken for the subject. Border
// pixels far off the fit (the subject at the border: shoulders, a shirt) are left out of it, twice.
struct BackdropFit {
    std::array<std::array<double, 3>, 6> coef{};
    double width = 1.0, height = 1.0;

    static std::array<double, 6> terms(double x, double y) { return {1.0, x, y, x * x, y * y, x * y}; }
    // The backdrop's color at photo pixel (x, y).
    std::array<float, 3> at(double px, double py) const {
        const auto t = terms(2.0 * px / width - 1.0, 2.0 * py / height - 1.0);
        std::array<float, 3> c{0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            double s = 0.0;
            for (int i = 0; i < 6; ++i) s += t[static_cast<std::size_t>(i)] * coef[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)];
            c[static_cast<std::size_t>(k)] = static_cast<float>(s);
        }
        return c;
    }
};

BackdropFit fitBackdrop(const RefImage& image) {
    BackdropFit fit;
    fit.width = image.width;
    fit.height = image.height;
    const int c = std::min(image.channels, 3);
    const int pad = std::max(2, std::min(image.width, image.height) / 40);
    struct Sample {
        std::array<double, 6> t;
        std::array<float, 3> rgb;
    };
    std::vector<Sample> samples;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            if ((x >= pad && x < image.width - pad) && (y >= pad && y < image.height - pad)) continue;
            Sample s{BackdropFit::terms(2.0 * (x + 0.5) / fit.width - 1.0, 2.0 * (y + 0.5) / fit.height - 1.0), {0, 0, 0}};
            for (int k = 0; k < 3; ++k) s.rgb[static_cast<std::size_t>(k)] = image.at(x, y, std::min(k, c - 1));
            samples.push_back(s);
        }
    std::vector<std::uint8_t> use(samples.size(), 1);
    const std::array<float, 3> flat = borderColor(image);
    for (int k = 0; k < 3; ++k) fit.coef[0][static_cast<std::size_t>(k)] = flat[static_cast<std::size_t>(k)];
    for (int round = 0; round < 3; ++round) {
        if (round > 0) {  // the samples far off the last fit left out
            std::vector<float> res(samples.size());
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto& t = samples[i].t;
                float d = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    double s = 0.0;
                    for (int j = 0; j < 6; ++j) s += t[static_cast<std::size_t>(j)] * fit.coef[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)];
                    const float e = static_cast<float>(s) - samples[i].rgb[static_cast<std::size_t>(k)];
                    d += e * e;
                }
                res[i] = std::sqrt(d);
            }
            const float limit = std::max(0.04f, 2.5f * median(res));
            for (std::size_t i = 0; i < samples.size(); ++i) use[i] = res[i] < limit ? 1 : 0;
        }
        // The normal equations (6 x 6, shared by the channels), solved by Gaussian elimination.
        double a[6][9] = {};
        std::size_t n = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (!use[i]) continue;
            ++n;
            const auto& t = samples[i].t;
            for (int r = 0; r < 6; ++r) {
                for (int q = 0; q < 6; ++q) a[r][q] += t[static_cast<std::size_t>(r)] * t[static_cast<std::size_t>(q)];
                for (int k = 0; k < 3; ++k) a[r][6 + k] += t[static_cast<std::size_t>(r)] * samples[i].rgb[static_cast<std::size_t>(k)];
            }
        }
        if (n < 24) break;
        bool ok = true;
        for (int col = 0; col < 6 && ok; ++col) {
            int pivot = col;
            for (int r = col + 1; r < 6; ++r)
                if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
            if (std::abs(a[pivot][col]) < 1e-9) ok = false;
            if (!ok) break;
            for (int q = 0; q < 9; ++q) std::swap(a[col][q], a[pivot][q]);
            for (int r = 0; r < 6; ++r) {
                if (r == col) continue;
                const double f = a[r][col] / a[col][col];
                for (int q = col; q < 9; ++q) a[r][q] -= f * a[col][q];
            }
        }
        if (!ok) break;
        for (int r = 0; r < 6; ++r)
            for (int k = 0; k < 3; ++k) fit.coef[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] = a[r][6 + k] / a[r][r];
    }
    return fit;
}

// Everything not the backdrop: a distance over 0.12 from its fitted color. `crop`: the part of the
// photo `image` shows (photo pixels), at whatever size `image` is.
RefImage autoMask(const RefImage& image, const BackdropFit& backdrop, const std::array<int, 4>& crop) {
    RefImage mask;
    mask.width = image.width;
    mask.height = image.height;
    mask.channels = 1;
    mask.pixels.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height), 0.0f);
    const int c = image.channels;
    const double sx = static_cast<double>(crop[2]) / image.width, sy = static_cast<double>(crop[3]) / image.height;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const std::array<float, 3> bg = backdrop.at(crop[0] + (x + 0.5) * sx, crop[1] + (y + 0.5) * sy);
            float d = 0.0f;
            for (int k = 0; k < std::min(c, 3); ++k) d += (image.at(x, y, k) - bg[static_cast<std::size_t>(k)]) * (image.at(x, y, k) - bg[static_cast<std::size_t>(k)]);
            mask.pixels[static_cast<std::size_t>(y * image.width + x)] = std::sqrt(d) > 0.12f ? 1.0f : 0.0f;
        }
    return mask;
}

// `set` kept where 4-connected to a pixel of `from` (both w x h).
void keepConnected(std::vector<std::uint8_t>& set, const std::vector<std::uint8_t>& from, int w, int h) {
    std::vector<std::uint8_t> keep(set.size(), 0);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < set.size(); ++i)
        if (set[i] && from[i]) {
            keep[i] = 1;
            stack.push_back(i);
        }
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
            const std::size_t o = static_cast<std::size_t>(ny[k]) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx[k]);
            if (keep[o] || !set[o]) continue;
            keep[o] = 1;
            stack.push_back(o);
        }
    }
    set.swap(keep);
}

// An automatic hair mask by the hair's colors (the subject is more than its hair: a face, a beard
// below it, a neck, clothes). `subject`: what is not the backdrop. The hair's colors are learned from
// the subject just outside the scalp's projection on its upper side (`up`: the photo's up, pixels;
// only hair surrounds the top of a head, even a scalp that is the whole head), the rest's from the
// subject's other pixels; then three more rounds, each from the last (the hair found, a little in;
// the rest, a little off it). A pixel is hair where the hair's colors explain it better over a 5 x 5
// neighbourhood, connected to the seeds; the top of the scalp's projection leans to hair (the
// crown, whose shadowed colors can match dark clothes). `seeds`: more sure hair (strokes);
// `notHair`: never hair. False (nothing changed) when too little hair shows past the scalp to learn
// from (short hair, fur on a body).
bool hairByColor(const RefImage& rgb, RefImage& mask, const RefImage& subject, const std::vector<std::uint8_t>& scalpCover,
                 const std::array<float, 2>& up, const std::vector<std::uint8_t>& seedsIn, const std::vector<std::uint8_t>& notHair,
                 const std::vector<std::uint8_t>& occluded) {
    const int w = rgb.width, h = rgb.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    // The scalp's projection along the photo's up: its center and extent.
    float hlo = kInf, hhi = -kInf, cx = 0.0f, cy = 0.0f;
    std::size_t covered = 0;
    for (std::size_t i = 0; i < pixels; ++i)
        if (scalpCover[i]) {
            cx += static_cast<float>(i % static_cast<std::size_t>(w));
            cy += static_cast<float>(i / static_cast<std::size_t>(w));
            ++covered;
        }
    if (covered < 64) return false;
    cx /= static_cast<float>(covered);
    cy /= static_cast<float>(covered);
    const auto height = [&](std::size_t i) {
        return (static_cast<float>(i % static_cast<std::size_t>(w)) - cx) * up[0] + (static_cast<float>(i / static_cast<std::size_t>(w)) - cy) * up[1];
    };
    for (std::size_t i = 0; i < pixels; ++i)
        if (scalpCover[i]) {
            hlo = std::min(hlo, height(i));
            hhi = std::max(hhi, height(i));
        }
    // The subject's parts that reach the scalp (a blotch of backdrop the fit missed is not hair).
    std::vector<std::uint8_t> sil(pixels, 0), near(pixels, 0);
    const std::vector<float> fromScalp = distanceTransform(scalpCover, w, h);
    for (std::size_t i = 0; i < pixels; ++i) {
        sil[i] = subject.pixels[i] >= 0.5f && !notHair[i] ? 1 : 0;
        near[i] = fromScalp[i] <= 3.0f ? 1 : 0;
    }
    keepConnected(sil, near, w, h);
    const float mid = 0.5f * (hlo + hhi), crown = hhi - 0.3f * (hhi - hlo);
    std::vector<std::uint8_t> seeds(pixels, 0);
    std::size_t seedCount = 0, silCount = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        silCount += sil[i];
        // (not where an occluder shows: a whole head's face above the scalp's middle is skin)
        seeds[i] = (sil[i] && !scalpCover[i] && (occluded.empty() || !occluded[i]) && fromScalp[i] > 2.0f && height(i) > mid) ? 1 : 0;
        seedCount += seeds[i];
        if (seedsIn[i] && !notHair[i]) seeds[i] = sil[i] = 1;
    }
    if (seedCount < std::max<std::size_t>(100, silCount / 50)) return false;
    auto bin = [&](std::size_t i) {
        int b = 0;
        for (int c = 0; c < 3; ++c)
            b = b * 16 + std::clamp(static_cast<int>(rgb.pixels[i * static_cast<std::size_t>(rgb.channels) + static_cast<std::size_t>(std::min(c, rgb.channels - 1))] * 15.999f), 0, 15);
        return static_cast<std::size_t>(b);
    };
    std::vector<std::uint8_t> fg = seeds, bg(pixels, 0), hair(pixels, 0);
    for (std::size_t i = 0; i < pixels; ++i) bg[i] = sil[i] && !seeds[i] ? 1 : 0;
    std::vector<float> llr(pixels), box(pixels);
    for (int round = 0; round < 4; ++round) {
        std::vector<double> hf(4096, 0.5), hb(4096, 0.5);
        double nf = 2048.0, nb = 2048.0;
        for (std::size_t i = 0; i < pixels; ++i) {
            if (fg[i]) hf[bin(i)] += 1.0, nf += 1.0;
            else if (bg[i]) hb[bin(i)] += 1.0, nb += 1.0;
        }
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::size_t b = bin(i);
            llr[i] = static_cast<float>(pm::log(hf[b] / nf) - pm::log(hb[b] / nb)) + (scalpCover[i] && height(i) > crown ? 1.5f : 0.0f);
        }
        // A 5 x 5 mean (separable).
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                int n = 0;
                for (int i = std::max(0, x - 2); i <= std::min(w - 1, x + 2); ++i, ++n) s += llr[static_cast<std::size_t>(y * w + i)];
                box[static_cast<std::size_t>(y * w + x)] = s / static_cast<float>(n);
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                int n = 0;
                for (int j = std::max(0, y - 2); j <= std::min(h - 1, y + 2); ++j, ++n) s += box[static_cast<std::size_t>(j * w + x)];
                llr[static_cast<std::size_t>(y * w + x)] = s / static_cast<float>(n);
            }
        for (std::size_t i = 0; i < pixels; ++i) hair[i] = sil[i] && (llr[i] > 0.0f || (seedsIn[i] && !notHair[i])) ? 1 : 0;
        keepConnected(hair, seeds, w, h);
        // The next round's samples: the hair a little in (and the seeds), the rest a little off it.
        std::vector<std::uint8_t> off(pixels);
        for (std::size_t i = 0; i < pixels; ++i) off[i] = hair[i] ? 0 : 1;
        const std::vector<float> in = distanceTransform(off, w, h), out = distanceTransform(hair, w, h);
        for (std::size_t i = 0; i < pixels; ++i) {
            fg[i] = sil[i] && (seeds[i] || in[i] > 2.0f) ? 1 : 0;
            bg[i] = sil[i] && !fg[i] && out[i] > 3.0f ? 1 : 0;
        }
    }
    // (colors that cannot tell the hair from the rest, the seeds' own hair lost: no use)
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pixels; ++i) kept += seeds[i] && hair[i];
    if (2 * kept < seedCount) return false;
    for (std::size_t i = 0; i < pixels; ++i) mask.pixels[i] = hair[i] ? 1.0f : 0.0f;
    return true;
}

RefImage grown(const RefImage& mask, int radius);

// The mask shrunk by `radius` pixels (its complement grown).
RefImage shrunk(const RefImage& mask, int radius) {
    RefImage inverse = mask;
    for (float& v : inverse.pixels) v = 1.0f - v;
    RefImage out = grown(inverse, radius);
    for (float& v : out.pixels) v = 1.0f - v;
    return out;
}

// A mask refined by color (GrabCut-like, without the graph cut): the hair's and the backdrop's
// colors learned from the mask's sure parts (its inside a few pixels in, the hint strokes' pixels;
// beyond a band outside it), each pixel taken by which color model explains it better, smoothed
// over its neighbourhood (neighbours alike), kept where connected to the sure hair; three rounds.
RefImage refineMask(const RefImage& rgb, const RefImage& initial, const std::vector<std::array<float, 2>>& seeds,
                    const std::vector<std::uint8_t>& sureBackdrop, const std::vector<std::uint8_t>& likelyHair) {
    const int w = rgb.width, h = rgb.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    RefImage cur = initial;
    for (std::size_t i = 0; i < pixels; ++i) cur.pixels[i] = cur.pixels[i] >= 0.5f && !sureBackdrop[i] ? 1.0f : 0.0f;
    std::vector<std::uint8_t> seed(pixels, 0);
    for (const auto& s : seeds)
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                const int x = static_cast<int>(s[0]) + i, y = static_cast<int>(s[1]) + j;
                if (x >= 0 && y >= 0 && x < w && y < h) seed[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
            }
    auto bin = [&](std::size_t i) {
        int b = 0;
        for (int c = 0; c < 3; ++c) b = b * 16 + std::clamp(static_cast<int>(rgb.pixels[i * 3 + static_cast<std::size_t>(c)] * 15.999f), 0, 15);
        return b;
    };
    for (int round = 0; round < 3; ++round) {
        // The first round trusts only what is known: the strokes and the scalp's own projection (hair
        // covers it) as hair; beyond the reach, the photo's border and what the first mask left out as
        // backdrop. Later rounds the refined mask's inside and outside.
        // (Sure hair: the mask closed, then a few pixels in, and hair itself: single strands with gaps
        // between them are not eroded away.)
        const RefImage sureHair = shrunk(grown(cur, 2), 5), notBack = grown(cur, 8);
        std::vector<double> fg(4096, 1.0), bg(4096, 1.0);
        double nf = 4096.0, nb = 4096.0;
        std::vector<std::uint8_t> sure(pixels, 0);
        const int border = std::max(2, std::max(w, h) / 40);
        for (std::size_t i = 0; i < pixels; ++i) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            const bool edge = x < border || y < border || x >= w - border || y >= h - border;
            const bool hair = round == 0 ? (seed[i] || (likelyHair[i] && cur.pixels[i] >= 0.5f)) : ((sureHair.pixels[i] >= 0.5f && cur.pixels[i] >= 0.5f) || seed[i]);
            const bool back = sureBackdrop[i] || edge || (round == 0 ? cur.pixels[i] < 0.5f : notBack.pixels[i] < 0.5f);
            if (hair) {
                fg[static_cast<std::size_t>(bin(i))] += 1.0;
                nf += 1.0;
                sure[i] = 1;
            } else if (back) {
                bg[static_cast<std::size_t>(bin(i))] += 1.0;
                nb += 1.0;
                sure[i] = 2;
            }
        }
        std::vector<float> llr(pixels);
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::size_t b = static_cast<std::size_t>(bin(i));
            float v = static_cast<float>(pm::log(fg[b] / nf) - pm::log(bg[b] / nb));
            if (sure[i] == 1) v += 2.0f;
            if (sure[i] == 2) v -= 2.0f;
            llr[i] = std::clamp(v, -8.0f, 8.0f);
        }
        // Neighbours alike: two passes of a 5 x 5 mean (a pixel strongly of the hair's colors stays
        // hair, though: a lone strand against the backdrop).
        const std::vector<float> raw = llr;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> next(pixels);
            parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
                const int y = static_cast<int>(yy);
                for (int x = 0; x < w; ++x) {
                    float sum = 0.0f;
                    int n = 0;
                    for (int j = -2; j <= 2; ++j)
                        for (int i = -2; i <= 2; ++i) {
                            const int px = x + i, py = y + j;
                            if (px < 0 || py < 0 || px >= w || py >= h) continue;
                            sum += llr[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px)];
                            ++n;
                        }
                    next[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = sum / static_cast<float>(n);
                }
            }, 8);
            llr.swap(next);
        }
        // Kept where connected to the sure hair.
        std::vector<std::uint8_t> keep(pixels, 0);
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < pixels; ++i)
            if (raw[i] > 2.0f) llr[i] = std::max(llr[i], raw[i]);
        for (std::size_t i = 0; i < pixels; ++i)
            if (sure[i] == 1 && llr[i] > 0.0f) {
                keep[i] = 1;
                stack.push_back(i);
            }
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                const std::size_t o = static_cast<std::size_t>(ny[k]) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx[k]);
                if (keep[o] || llr[o] <= 0.0f) continue;
                keep[o] = 1;
                stack.push_back(o);
            }
        }
        for (std::size_t i = 0; i < pixels; ++i) cur.pixels[i] = keep[i] ? 1.0f : 0.0f;
    }
    return cur;
}

RefImage grown(const RefImage& mask, int radius) {
    RefImage out = mask;
    const int w = mask.width, h = mask.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float best = mask.at(x, y);
            for (int j = -radius; j <= radius && best < 1.0f; ++j)
                for (int i = -radius; i <= radius; ++i) {
                    const int px = x + i, py = y + j;
                    if (px < 0 || py < 0 || px >= w || py >= h || i * i + j * j > radius * radius) continue;
                    best = std::max(best, mask.at(px, py));
                }
            out.pixels[static_cast<std::size_t>(y * w + x)] = best;
        }
    return out;
}

struct HintSample;
std::vector<HintSample> hintSamples(const ReferenceView& view, float scale, int w, int h);
void applyHints(PreparedView& pv, const std::vector<HintSample>& samples, float sigma, float lineSigma, float worldPerPixel);

// The pixels (w x h) the mask strokes of one kind cover, their points scaled by `scale` (photo
// pixels to these), each at least a pixel wide.
std::vector<std::uint8_t> maskStrokePixels(const std::vector<RefMaskStroke>& strokes, float scale, int w, int h, bool include) {
    std::vector<std::uint8_t> out;
    for (const RefMaskStroke& s : strokes) {
        if (s.include != include || s.points.empty()) continue;
        if (out.empty()) out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
        const float r = std::max(0.75f, s.radius * scale);
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            const float ax = s.points[i][0] * scale, ay = s.points[i][1] * scale;
            const float bx = s.points[i + 1 < s.points.size() ? i + 1 : i][0] * scale;
            const float by = s.points[i + 1 < s.points.size() ? i + 1 : i][1] * scale;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - r)));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - r)));
            const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(ax, bx) + r)));
            const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(ay, by) + r)));
            const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float t = l2 > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.0f, 1.0f) : 0.0f;
                    const float ex = px - ax - t * dx, ey = py - ay - t * dy;
                    if (ex * ex + ey * ey <= r * r) out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
                }
        }
    }
    return out;
}

std::vector<PreparedView> prepareViews(const ScalpMesh& scalp, const std::vector<ReferenceView>& views,
                                       const ReferenceGuideOptions& options, float scalpSize) {
    std::vector<PreparedView> out;
    // The face's side of the head: from the scalp's middle toward the occluders' (a head under its
    // scalp: the face is where the head reaches past the scalp, level), none when they line up (a
    // ball under a cap).
    Vec3f scalpCenter(0, 0, 0), face(0, 0, 0);
    {
        for (const Vec3f& q : scalp.points()) scalpCenter = scalpCenter + q;
        scalpCenter = scalpCenter * (1.0f / static_cast<float>(std::max<std::size_t>(1, scalp.pointCount())));
        Vec3f head(0, 0, 0);
        std::size_t n = 0;
        for (const auto& occluder : options.occluders)
            if (occluder)
                for (const Vec3f& q : occluder->points()) head = head + q, ++n;
        if (n) {
            const Vec3f up = options.up.normalized();
            Vec3f d = head * (1.0f / static_cast<float>(n)) - scalpCenter;
            d = d - up * d.dot(up);
            if (d.length() > 0.05f * scalpSize) face = d.normalized();
        }
    }
    // (each view once; again with the crop's reach doubled while its hair runs out of the crop: long
    // hair past the reach an automatic length starts from)
    for (std::size_t viewIndex = 0, attempt = 0; viewIndex < views.size();) {
        const ReferenceView& original = views[viewIndex];
        const float reachShare = 0.3f * static_cast<float>(1u << attempt);
        PreparedView pv;
        // The subject's part of the photo: where the scalp and the longest hair from it project
        // (finer detail at the same working size). Its image, mask, hints and camera cropped.
        ReferenceView view = original;
        std::array<int, 4> crop{0, 0, 0, 0};
        // The backdrop's color, from the whole photo's border (before any crop).
        const BackdropFit backdrop = original.image.empty() || !original.mask.empty() ? BackdropFit{} : fitBackdrop(original.image);
        if (options.cropToSubject && !original.field && !original.image.empty()) {
            RefCamera cam = original.camera;
            if (cam.width <= 0.0) {
                cam.width = original.image.width;
                cam.height = original.image.height;
            }
            cam = cam.scaled(static_cast<double>(original.image.width) / cam.width);
            float lo[2] = {kInf, kInf}, hi[2] = {-kInf, -kInf};
            // The reach past the scalp: the given length, else a third of the scalp's size; a given
            // mask's hair counts too (all of it is in the crop).
            const float longest = options.maxLength > 0.0f ? options.maxLength : reachShare * scalpSize;
            float reachPx = 0.0f;
            const std::size_t every = std::max<std::size_t>(1, scalp.pointCount() / 20000);
            for (std::size_t i = 0; i < scalp.pointCount(); i += every) {
                float u = 0, v = 0, z = 0;
                if (!cam.project(scalp.points()[i], u, v, &z)) continue;
                lo[0] = std::min(lo[0], u);
                lo[1] = std::min(lo[1], v);
                hi[0] = std::max(hi[0], u);
                hi[1] = std::max(hi[1], v);
                reachPx = std::max(reachPx, static_cast<float>(cam.focalPx) * longest / z);
            }
            if (!original.mask.empty()) {
                // A given mask says where the hair is: its bounds, with a margin, are the subject.
                const float sx = static_cast<float>(original.image.width) / static_cast<float>(original.mask.width);
                const float sy = static_cast<float>(original.image.height) / static_cast<float>(original.mask.height);
                float mlo[2] = {kInf, kInf}, mhi[2] = {-kInf, -kInf};
                for (int y = 0; y < original.mask.height; ++y)
                    for (int x = 0; x < original.mask.width; ++x) {
                        if (original.mask.at(x, y) < 0.5f) continue;
                        mlo[0] = std::min(mlo[0], static_cast<float>(x) * sx);
                        mlo[1] = std::min(mlo[1], static_cast<float>(y) * sy);
                        mhi[0] = std::max(mhi[0], static_cast<float>(x + 1) * sx);
                        mhi[1] = std::max(mhi[1], static_cast<float>(y + 1) * sy);
                    }
                if (mhi[0] > mlo[0]) {
                    const float margin = 0.05f * std::max(mhi[0] - mlo[0], mhi[1] - mlo[1]);
                    lo[0] = mlo[0] - margin;
                    lo[1] = mlo[1] - margin;
                    hi[0] = mhi[0] + margin;
                    hi[1] = mhi[1] + margin;
                    reachPx = 0.0f;
                }
            }
            const float W = static_cast<float>(original.image.width), Hh = static_cast<float>(original.image.height);
            const int x0 = static_cast<int>(std::floor(std::clamp(lo[0] - reachPx, 0.0f, W)));
            const int y0 = static_cast<int>(std::floor(std::clamp(lo[1] - reachPx, 0.0f, Hh)));
            const int x1 = static_cast<int>(std::ceil(std::clamp(hi[0] + reachPx, 0.0f, W)));
            const int y1 = static_cast<int>(std::ceil(std::clamp(hi[1] + reachPx, 0.0f, Hh)));
            if (x1 - x0 >= 16 && y1 - y0 >= 16 && (x1 - x0) * (y1 - y0) < 0.95f * W * Hh) {
                view.camera = cam.cropped(x0, y0, x1 - x0, y1 - y0);
                crop = {x0, y0, x1 - x0, y1 - y0};
                view.image = cropImage(original.image, x0, y0, x1 - x0, y1 - y0);
                if (!original.mask.empty()) {
                    const RefImage m = original.mask.width == original.image.width ? original.mask
                                                                                    : original.mask.resized(original.image.width, original.image.height);
                    view.mask = cropImage(m, x0, y0, x1 - x0, y1 - y0);
                }
                if (!original.hintOverlay.empty()) {
                    const RefImage o = original.hintOverlay.width == original.image.width
                                           ? original.hintOverlay
                                           : original.hintOverlay.resized(original.image.width, original.image.height);
                    view.hintOverlay = cropImage(o, x0, y0, x1 - x0, y1 - y0);
                }
                // Hint strokes are in the photo's pixels as given (camera.width): into the crop's.
                const float k = static_cast<float>(original.image.width) /
                                static_cast<float>(original.camera.width > 0.0 ? original.camera.width : original.image.width);
                for (RefHintStroke& s : view.hints)
                    for (auto& p : s.points) p = {p[0] * k - static_cast<float>(x0), p[1] * k - static_cast<float>(y0)};
                for (RefMaskStroke& s : view.maskStrokes) {
                    for (auto& p : s.points) p = {p[0] * k - static_cast<float>(x0), p[1] * k - static_cast<float>(y0)};
                    s.radius *= k;
                }
            }
        }
        // The size the view comes at: its image's, else its mask's, else its field's.
        int w = view.image.width, h = view.image.height;
        pv.crop = crop[2] > 0 ? crop : std::array<int, 4>{0, 0, w, h};
        if (w <= 0) {
            w = view.mask.width;
            h = view.mask.height;
        }
        if (view.field) {
            w = view.field->width;
            h = view.field->height;
        }
        if (w <= 0 || h <= 0) throw std::invalid_argument("guidesFromReference: a view has no image, mask or field");
        RefCamera camera = view.camera;
        if (camera.width <= 0.0) {
            camera.width = w;
            camera.height = h;
        }
        camera = camera.scaled(static_cast<double>(w) / camera.width);
        double scale = 1.0;
        int limit = options.maxImageSize;
        if (crop[2] > 0 && options.subjectSize > 0) {
            const double native = std::max(w, h), s = options.subjectSize, lo = 1.5 * s, hi = 2.0 * s;
            if (native > lo) {
                const int target = static_cast<int>(std::lround(native >= hi ? s : lo - (native - lo) / (hi - lo) * (lo - s)));
                limit = limit > 0 ? std::min(limit, target) : target;
            }
        }
        if (!view.field && limit > 0 && std::max(w, h) > limit) scale = static_cast<double>(limit) / static_cast<double>(std::max(w, h));
        pv.width = std::max(1, static_cast<int>(std::lround(w * scale)));
        pv.height = std::max(1, static_cast<int>(std::lround(h * scale)));
        pv.camera = camera.scaled(static_cast<double>(pv.width) / static_cast<double>(w));
        if (!view.image.empty()) pv.rgb = (view.image.width == pv.width && view.image.height == pv.height) ? view.image : view.image.resized(pv.width, pv.height);
        // What the scalp and the occluders hide, and where the scalp shows.
        const std::size_t pixels = static_cast<std::size_t>(pv.width) * static_cast<std::size_t>(pv.height);
        pv.depth.assign(pixels, kInf);
        pv.scalpCover.assign(pixels, 0);
        rasterize(scalp, projectMesh(scalp, pv.camera), pv.width, pv.height, pv.depth, &pv.scalpCover);
        // Where an occluder shows in front of the scalp (a whole head's face): no hair seeds there.
        std::vector<std::uint8_t> occluded;
        if (!options.occluders.empty()) {
            const std::vector<float> scalpOnly = pv.depth;
            for (const auto& occluder : options.occluders)
                if (occluder) rasterize(*occluder, projectMesh(*occluder, pv.camera), pv.width, pv.height, pv.depth, nullptr);
            occluded.assign(pixels, 0);
            for (std::size_t i = 0; i < pixels; ++i) occluded[i] = pv.depth[i] < kInf && pv.depth[i] < scalpOnly[i] - 1e-6f ? 1 : 0;
        }
        pv.scalpDistance = distanceTransform(pv.scalpCover, pv.width, pv.height);
        pv.depthEps = 0.01f * scalpSize;
        float scalpDepth = 0.0f;
        {
            std::vector<float> depths;
            const std::size_t every = std::max<std::size_t>(1, scalp.pointCount() / 2000);
            for (std::size_t i = 0; i < scalp.pointCount(); i += every) {
                float u = 0, v = 0, z = 0;
                if (pv.camera.project(scalp.points()[i], u, v, &z) && pv.inFrame(u, v) && !pv.hidden(u, v, z)) depths.push_back(z);
            }
            if (!depths.empty()) {
                std::nth_element(depths.begin(), depths.begin() + static_cast<std::ptrdiff_t>(depths.size() / 2), depths.end());
                scalpDepth = depths[depths.size() / 2];
            }
        }
        if (!view.mask.empty()) {
            pv.mask = (view.mask.width == pv.width && view.mask.height == pv.height) ? view.mask : view.mask.resized(pv.width, pv.height);
            if (pv.mask.channels != 1) pv.mask = pv.mask.gray();
        } else if (!pv.rgb.empty()) {
            pv.mask = autoMask(pv.rgb, backdrop, crop[2] > 0 ? crop : std::array<int, 4>{0, 0, w, h});
        } else {
            pv.mask.width = pv.width;
            pv.mask.height = pv.height;
            pv.mask.channels = 1;
            pv.mask.pixels.assign(static_cast<std::size_t>(pv.width) * static_cast<std::size_t>(pv.height), 1.0f);
        }
        // The touch-up strokes: hair where they include it, backdrop where they exclude it (before
        // the refinement, which takes them as sure, and after it, whatever it says).
        const float strokeScale = static_cast<float>(pv.width) / static_cast<float>(w);
        const std::vector<std::uint8_t> includePixels = maskStrokePixels(view.maskStrokes, strokeScale, pv.width, pv.height, true);
        const std::vector<std::uint8_t> excludePixels = maskStrokePixels(view.maskStrokes, strokeScale, pv.width, pv.height, false);
        const auto touchUp = [&] {
            for (std::size_t i = 0; i < includePixels.size(); ++i)
                if (includePixels[i]) pv.mask.pixels[i] = 1.0f;
            for (std::size_t i = 0; i < excludePixels.size(); ++i)
                if (excludePixels[i]) pv.mask.pixels[i] = 0.0f;
        };
        touchUp();
        // Refined by color (an automatic mask, or any with refineMask on), the hint strokes as sure hair.
        if (!pv.rgb.empty() && pv.rgb.channels >= 3 && (options.refineMask > 0 || (options.refineMask < 0 && view.mask.empty()))) {
            std::vector<std::array<float, 2>> seeds;
            for (std::size_t i = 0; i < includePixels.size(); ++i)
                if (includePixels[i])
                    seeds.push_back({static_cast<float>(i % static_cast<std::size_t>(pv.width)),
                                     static_cast<float>(i / static_cast<std::size_t>(pv.width))});
            const float k = static_cast<float>(pv.width) / static_cast<float>(w);
            for (const RefHintStroke& s : view.hints)
                for (std::size_t i = 1; i < s.points.size(); ++i) {
                    const float du = s.points[i][0] - s.points[i - 1][0], dv = s.points[i][1] - s.points[i - 1][1];
                    const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(du * du + dv * dv) * k)));
                    for (int t = 0; t <= n; ++t)
                        seeds.push_back({(s.points[i - 1][0] + du * static_cast<float>(t) / static_cast<float>(n)) * k,
                                         (s.points[i - 1][1] + dv * static_cast<float>(t) / static_cast<float>(n)) * k});
                }
            // Beyond the longest hair's reach from the scalp in the photo: sure backdrop.
            const float longest = options.maxLength > 0.0f ? options.maxLength : scalpSize;
            const float reach = scalpDepth > 0.0f ? 1.15f * longest * static_cast<float>(pv.camera.focalPx) / scalpDepth : kInf;
            std::vector<std::uint8_t> sureBackdrop(pixels, 0), likelyHair(pixels, 0);
            for (std::size_t i = 0; i < pixels; ++i) sureBackdrop[i] = pv.scalpDistance[i] > reach ? 1 : 0;
            for (std::size_t i = 0; i < excludePixels.size(); ++i)
                if (excludePixels[i]) sureBackdrop[i] = 1;
            // The scalp's projection a little in (not its silhouette's edge), in front of any occluder.
            const std::vector<float> outside = [&] {
                std::vector<std::uint8_t> off(pixels);
                for (std::size_t i = 0; i < pixels; ++i) off[i] = pv.scalpCover[i] ? 0 : 1;
                return distanceTransform(off, pv.width, pv.height);
            }();
            for (std::size_t i = 0; i < pixels; ++i) likelyHair[i] = pv.scalpCover[i] && outside[i] > 3.0f ? 1 : 0;
            // An automatic mask: by the hair's colors (a face, a beard, a neck and clothes are not
            // backdrop either), where enough hair shows past the scalp to learn them from.
            bool byColor = false;
            if (view.mask.empty()) {
                std::array<float, 2> up{0.0f, -1.0f};
                {
                    Vec3f center{0, 0, 0};
                    for (std::size_t i = 0; i < scalp.pointCount(); ++i) center = center + scalp.points()[i];
                    center = center * (1.0f / static_cast<float>(std::max<std::size_t>(1, scalp.pointCount())));
                    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
                    if (pv.camera.project(center, u0, v0) && pv.camera.project(center + options.up * (0.05f * scalpSize), u1, v1)) {
                        const float du = u1 - u0, dv = v1 - v0, l = std::sqrt(du * du + dv * dv);
                        if (l > 1e-6f) up = {du / l, dv / l};
                    }
                }
                std::vector<std::uint8_t> seedPixels(pixels, 0);
                for (const auto& s : seeds) {
                    const int x = static_cast<int>(s[0]), y = static_cast<int>(s[1]);
                    if (x >= 0 && y >= 0 && x < pv.width && y < pv.height) seedPixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(pv.width) + static_cast<std::size_t>(x)] = 1;
                }
                const RefImage subject = pv.mask;
                byColor = hairByColor(pv.rgb, pv.mask, subject, pv.scalpCover, up, seedPixels, sureBackdrop, occluded);
            }
            if (!byColor) pv.mask = refineMask(pv.rgb, pv.mask, seeds, sureBackdrop, likelyHair);
            touchUp();
        }
        // Hair hanging from the scalp (long hair down the back and the neck): down the picture from
        // hair over the scalp without a gap of bare skin, the hair's colors where an occluder shows
        // (the hair over the scalp's median, within three of its median deviations: skin the mask
        // took in is not), never on the face's side of the head (a beard).
        std::vector<std::uint8_t> hanging(pixels, 0);
        {
            std::array<float, 3> hairColor{0, 0, 0};
            float hairSpread = kInf;
            if (!pv.rgb.empty() && pv.rgb.channels >= 3) {
                std::vector<float> channel[3];
                for (std::size_t i = 0; i < pixels; ++i)
                    if (pv.scalpCover[i] && pv.mask.pixels[i] >= 0.5f)
                        for (int c = 0; c < 3; ++c) channel[c].push_back(pv.rgb.pixels[i * static_cast<std::size_t>(pv.rgb.channels) + static_cast<std::size_t>(c)]);
                if (channel[0].size() > 20) {
                    for (int c = 0; c < 3; ++c) hairColor[static_cast<std::size_t>(c)] = median(channel[c]);
                    std::vector<float> off;
                    for (std::size_t k = 0; k < channel[0].size(); ++k)
                        off.push_back(std::hypot(channel[0][k] - hairColor[0], channel[1][k] - hairColor[1], channel[2][k] - hairColor[2]));
                    hairSpread = std::max(0.08f, 3.0f * median(off));
                }
            }
            const Vec3f eye = pv.camera.eye();
            auto hangs = [&](std::size_t i) {
                if (pv.scalpCover[i] || occluded.empty() || !occluded[i]) return true;
                if (hairSpread < kInf) {
                    const float* c = &pv.rgb.pixels[i * static_cast<std::size_t>(pv.rgb.channels)];
                    if (std::hypot(c[0] - hairColor[0], c[1] - hairColor[1], c[2] - hairColor[2]) >= hairSpread) return false;
                }
                if (face.length() <= 0.0f) return true;
                const float u = static_cast<float>(i % static_cast<std::size_t>(pv.width)) + 0.5f, w = static_cast<float>(i / static_cast<std::size_t>(pv.width)) + 0.5f;
                const Vec3f ray = (pv.camera.axis(0) * static_cast<float>((u - 0.5 * pv.camera.width - pv.camera.offsetU) / pv.camera.focalPx) -
                                   pv.camera.axis(1) * static_cast<float>((w - 0.5 * pv.camera.height - pv.camera.offsetV) / pv.camera.focalPx) - pv.camera.axis(2));
                return (eye + ray * pv.depth[i] - scalpCenter).dot(face) <= 0.15f * scalpSize;
            };
            // (down the picture: the world's down as it shows; along its columns or its rows)
            const float du = -options.up.dot(pv.camera.axis(0)), dv = options.up.dot(pv.camera.axis(1));
            const bool columns = std::abs(dv) >= std::abs(du);
            const int lines = columns ? pv.width : pv.height, length = columns ? pv.height : pv.width;
            const bool forward = columns ? dv >= 0.0f : du >= 0.0f;
            const int gapMax = std::max(3, std::max(pv.width, pv.height) / 150);
            for (int a = 0; a < lines; ++a) {
                bool hung = false;
                int gap = 0;
                for (int b = 0; b < length; ++b) {
                    const int c = forward ? b : length - 1 - b;
                    const std::size_t i = columns ? static_cast<std::size_t>(c) * static_cast<std::size_t>(pv.width) + static_cast<std::size_t>(a)
                                                  : static_cast<std::size_t>(a) * static_cast<std::size_t>(pv.width) + static_cast<std::size_t>(c);
                    const bool hair = pv.mask.pixels[i] >= 0.5f && hangs(i);
                    if (hair && pv.scalpCover[i]) hung = true, gap = 0;
                    else if (hair) gap = 0;
                    else if (++gap > gapMax) hung = false;
                    if (hung && hair) hanging[i] = 1;
                }
            }
        }
        // An occluder well away from the scalp's outline (a whole head's beard, the face below the
        // hair) is no hair to grow guides over, whatever its color; hair over the forehead, near the
        // scalp, stays, and so does hair hanging from the scalp over it.
        if (!occluded.empty()) {
            std::size_t cover = 0;
            for (std::uint8_t c : pv.scalpCover) cover += c;
            const float far = 0.25f * std::sqrt(static_cast<float>(cover));
            for (std::size_t i = 0; i < pixels; ++i)
                if (occluded[i] && pv.scalpDistance[i] > far && !hanging[i]) pv.mask.pixels[i] = 0.0f;
        }
        pv.hullMask = grown(pv.mask, 2);
        // Roots grow only where the hair is sure: the mask a few pixels in (an eye's, a nose's or a
        // mouth's hole in the hair keeps its edge free of roots too).
        // (Closed first: a mask of single strands with gaps between them is hair all the same.)
        pv.rootMask = shrunk(grown(pv.mask, 2), 2 + std::max(1, static_cast<int>(std::lround(0.004 * std::max(pv.width, pv.height)))));
        if (view.field) {
            pv.field = view.field;
        } else {
            if (pv.rgb.empty()) throw std::invalid_argument("guidesFromReference: a view without a field needs an image");
            pv.field = std::make_shared<OrientationField>(orientationField(pv.rgb.gray(), pv.mask, options.orientation));
        }
        // Drawn guide lines (optional): over the field, their way and their length for the roots.
        if (!view.hints.empty() || !view.hintOverlay.empty())
            applyHints(pv, hintSamples(view, static_cast<float>(pv.width) / static_cast<float>(w), pv.width, pv.height),
                       0.5f * options.hintRadius * static_cast<float>(std::max(pv.width, pv.height)),
                       0.5f * options.hintLineRadius * static_cast<float>(std::max(pv.width, pv.height)),
                       scalpDepth > 0.0f ? scalpDepth / static_cast<float>(pv.camera.focalPx) : 0.0f);
        // The locks' flow past their curls, and the curls themselves.
        if (!view.field) {
            // (the fine scale: the structure tensor follows each curl's strands; the variance bank is
            // fine already, its confidence low where strands cross)
            const bool fineIsField = options.orientation.method == OrientationMethod::Variance;
            pv.fine = !pv.rgb.empty() && !fineIsField
                          ? std::make_shared<OrientationField>(structureField(pv.rgb, std::max(1.5f, static_cast<float>(std::max(pv.width, pv.height)) / 250.0f)))
                          : pv.field;
            pv.coarse = std::make_shared<OrientationField>(coarseField(*pv.field, pv.mask, 0.02f * static_cast<float>(std::max(pv.width, pv.height)),
                                                                       options.orientation.bilateralCoarse));
            pv.curl = measureCurl(*pv.field, *pv.coarse, pv.mask);
        }
        // The hair's rim past the scalp: at the mask's outline against the backdrop (not against an
        // occluder), the distance to the scalp's projection; the lower quartile (the rim, not the
        // hair hanging far below).
        std::vector<float> rim;
        for (int y = 1; y + 1 < pv.height; ++y)
            for (int x = 1; x + 1 < pv.width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * pv.width + x);
                if (pv.mask.pixels[at] < 0.5f || pv.scalpCover[at]) continue;
                bool edge = false;
                for (const auto& o : {at - 1, at + 1, at - static_cast<std::size_t>(pv.width), at + static_cast<std::size_t>(pv.width)})
                    if (pv.mask.pixels[o] < 0.5f && pv.depth[o] == kInf) edge = true;
                if (edge) rim.push_back(pv.scalpDistance[at]);
            }
        std::sort(rim.begin(), rim.end());
        pv.rimPx = rim.empty() ? 0.0f : rim[rim.size() / 4];
        if (scalpDepth > 0.0f) pv.rimWorld = pv.rimPx * scalpDepth / static_cast<float>(pv.camera.focalPx);
        pv.scalpDepth = scalpDepth;
        {
            // How far the hair reaches past the scalp in the photo (the mask's pixels off the scalp,
            // their distance to it): what an automatic length starts from.
            std::vector<float> reach;
            for (std::size_t i = 0; i < pixels; ++i)
                if (pv.mask.pixels[i] >= 0.5f && !pv.scalpCover[i] && pv.scalpDistance[i] < 1e8f) reach.push_back(pv.scalpDistance[i]);
            if (!reach.empty() && scalpDepth > 0.0f) {
                const std::size_t at = static_cast<std::size_t>(0.9 * static_cast<double>(reach.size() - 1));
                std::nth_element(reach.begin(), reach.begin() + static_cast<std::ptrdiff_t>(at), reach.end());
                pv.reachWorld = reach[at] * scalpDepth / static_cast<float>(pv.camera.focalPx);
            }
        }
        // Hair running out of the crop where the crop is not the photo's edge: the crop again, wider.
        // (only the side the picture's down is toward: hair hanging past the crop)
        if (attempt < 2 && crop[2] > 0 && options.maxLength <= 0.0f && original.mask.empty() && !pv.mask.empty()) {
            const int W = original.image.width, Hh = original.image.height;
            const float du = -options.up.dot(pv.camera.axis(0)), dv = options.up.dot(pv.camera.axis(1));
            const int downSide = std::abs(dv) >= std::abs(du) ? (dv >= 0.0f ? 3 : 1) : (du >= 0.0f ? 2 : 0);
            bool sides[4] = {crop[0] > 0, crop[1] > 0, crop[0] + crop[2] < W, crop[1] + crop[3] < Hh};  // left, top, right, bottom
            for (int side = 0; side < 4; ++side) sides[side] = sides[side] && side == downSide;
            bool spills = false;
            for (int side = 0; side < 4 && !spills; ++side) {
                if (!sides[side]) continue;
                const bool vertical = side == 0 || side == 2;
                const int along = vertical ? pv.height : pv.width;
                int hair = 0;
                for (int k = 0; k < along; ++k) {
                    for (int d = 0; d < 2; ++d) {
                        const int x = vertical ? (side == 0 ? d : pv.width - 1 - d) : k;
                        const int y = vertical ? k : (side == 1 ? d : pv.height - 1 - d);
                        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(pv.width) + static_cast<std::size_t>(x);
                        if (pv.mask.pixels[i] >= 0.5f && hanging[i]) {
                            ++hair;
                            break;
                        }
                    }
                }
                // (hair hanging from the scalp across a good part of the edge: not a beard's sideburns)
                spills = hair > std::max(3, (3 * along) / 20);
            }
            if (spills) {
                ++attempt;
                continue;
            }
        }
        out.push_back(std::move(pv));
        ++viewIndex;
        attempt = 0;
    }
    return out;
}

// ---- Hints ----------------------------------------------------------------------------------------

struct HintSample {
    float u, v, du, dv;
    bool directed;
    float remain = -1.0f;  // a drawn line's length on from here to its tip, pixels (directed lines; else -1)
};

// Samples along the strokes (every pixel at the working size) and from the overlay's stroke pixels:
// each pixel's line from the stroke's shape around it (the principal axis of the stroke pixels
// within a few pixels), its way from the brightness ramp along it (toward the lighter end) where the
// ramp is clear.
std::vector<HintSample> hintSamples(const ReferenceView& view, float scale, int w, int h) {
    std::vector<HintSample> out;
    for (const RefHintStroke& stroke : view.hints) {
        float total = 0.0f;
        for (std::size_t i = 1; i < stroke.points.size(); ++i)
            total += std::hypot(stroke.points[i][0] - stroke.points[i - 1][0], stroke.points[i][1] - stroke.points[i - 1][1]) * scale;
        float along = 0.0f;
        for (std::size_t i = 1; i < stroke.points.size(); ++i) {
            const float u0 = stroke.points[i - 1][0] * scale, v0 = stroke.points[i - 1][1] * scale;
            const float u1 = stroke.points[i][0] * scale, v1 = stroke.points[i][1] * scale;
            const float du = u1 - u0, dv = v1 - v0, len = std::sqrt(du * du + dv * dv);
            if (len < 1e-6f) continue;
            const int n = std::max(1, static_cast<int>(std::ceil(len)));
            for (int k = 0; k < n; ++k) {
                const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
                out.push_back({u0 + du * t, v0 + dv * t, du / len, dv / len, stroke.directed,
                               stroke.directed ? total - along - len * t : -1.0f});
            }
            along += len;
        }
    }
    if (!view.hintOverlay.empty() && view.hintOverlay.channels == 4) {
        const RefImage o = view.hintOverlay.resized(w, h);
        auto alpha = [&](int x, int y) { return o.at(x, y, 3); };
        auto light = [&](int x, int y) { return (o.at(x, y, 0) + o.at(x, y, 1) + o.at(x, y, 2)) / 3.0f; };
        const int r = 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (alpha(x, y) < 0.5f) continue;
                double sxx = 0, sxy = 0, syy = 0, gx = 0, gy = 0, gabs = 0;
                float lo = 1e9f, hi = -1e9f;
                const float l0 = light(x, y);
                int count = 0;
                for (int j = -r; j <= r; ++j)
                    for (int i = -r; i <= r; ++i) {
                        const int px = x + i, py = y + j;
                        if (px < 0 || py < 0 || px >= w || py >= h || i * i + j * j > r * r || alpha(px, py) < 0.5f) continue;
                        sxx += i * i;
                        sxy += i * j;
                        syy += j * j;
                        const float dl = light(px, py) - l0;
                        gx += i * dl;
                        gy += j * dl;
                        gabs += std::sqrt(static_cast<double>(i * i + j * j)) * std::abs(dl);
                        lo = std::min(lo, light(px, py));
                        hi = std::max(hi, light(px, py));
                        ++count;
                    }
                if (count < 4) continue;
                // The principal axis of the stroke pixels around: the line.
                const double angle = 0.5 * pm::atan2(2.0 * sxy, sxx - syy);
                float du = static_cast<float>(pm::cos(angle)), dv = static_cast<float>(pm::sin(angle));
                const double along = gx * du + gy * dv;
                const bool directed = hi - lo > 0.05f && gabs > 1e-9 && std::abs(along) > 0.3 * gabs;
                if (directed && along < 0.0) {
                    du = -du;
                    dv = -dv;
                }
                out.push_back({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, du, dv, directed});
            }
    }
    return out;
}

// The hints splatted (a Gaussian of `sigma` pixels): their line blended into the field over the
// photo's (full weight near a stroke), their way kept for the guides' roots, and a drawn line's
// length on from each of its points (`worldPerPixel`: pixels to scene units at the scalp's depth; 0:
// none) for the roots under it, over the way's reach.
void applyHints(PreparedView& pv, const std::vector<HintSample>& samples, float sigma, float lineSigma, float worldPerPixel) {
    if (samples.empty()) return;
    const int w = pv.width, h = pv.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> line(pixels * 2, 0.0f), weight(pixels, 0.0f), way(pixels * 2, 0.0f), wayWeight(pixels, 0.0f);
    std::vector<float> length(pixels, 0.0f), lengthWeight(pixels, 0.0f);
    const int reach = static_cast<int>(std::ceil(2.5f * sigma));
    for (const HintSample& s : samples) {
        const int cx = static_cast<int>(s.u), cy = static_cast<int>(s.v);
        const float c2 = s.du * s.du - s.dv * s.dv, s2 = 2.0f * s.du * s.dv;
        for (int y = std::max(0, cy - reach); y <= std::min(h - 1, cy + reach); ++y)
            for (int x = std::max(0, cx - reach); x <= std::min(w - 1, cx + reach); ++x) {
                const float dx = static_cast<float>(x) + 0.5f - s.u, dy = static_cast<float>(y) + 0.5f - s.v;
                const float k = pm::exp(-0.5f * (dx * dx + dy * dy) / (sigma * sigma));
                if (k < 1e-3f) continue;
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                const float kl = pm::exp(-0.5f * (dx * dx + dy * dy) / (lineSigma * lineSigma));
                line[2 * at] += kl * c2;
                line[2 * at + 1] += kl * s2;
                weight[at] += kl;
                if (s.directed) {
                    way[2 * at] += k * s.du;
                    way[2 * at + 1] += k * s.dv;
                    wayWeight[at] += k;
                }
                if (s.remain >= 0.0f && worldPerPixel > 0.0f) {
                    length[at] += k * s.remain;
                    lengthWeight[at] += k;
                }
            }
    }
    // The field at the working size (a given field may be another size: hints then leave it be).
    if (pv.field && pv.field->width == w && pv.field->height == h) {
        auto f = std::make_shared<OrientationField>(*pv.field);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (weight[i] <= 1e-6f) continue;
            const float n = std::sqrt(line[2 * i] * line[2 * i] + line[2 * i + 1] * line[2 * i + 1]);
            if (n <= 1e-6f) continue;
            // A few stroke samples' worth of weight is full weight.
            const float a = std::min(1.0f, weight[i] / std::max(1.0f, lineSigma));
            f->tensor[2 * i] = (1.0f - a) * f->tensor[2 * i] + a * line[2 * i] / n;
            f->tensor[2 * i + 1] = (1.0f - a) * f->tensor[2 * i + 1] + a * line[2 * i + 1] / n;
        }
        pv.field = f;
    }
    pv.hintWay.assign(pixels * 3, 0.0f);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float n = std::sqrt(way[2 * i] * way[2 * i] + way[2 * i + 1] * way[2 * i + 1]);
        if (n <= 1e-6f) continue;
        pv.hintWay[3 * i] = way[2 * i] / n;
        pv.hintWay[3 * i + 1] = way[2 * i + 1] / n;
        pv.hintWay[3 * i + 2] = std::min(1.0f, wayWeight[i] / std::max(1.0f, sigma)) * std::min(1.0f, n / std::max(wayWeight[i], 1e-6f));
    }
    if (worldPerPixel > 0.0f && std::any_of(lengthWeight.begin(), lengthWeight.end(), [](float x) { return x > 1e-3f; })) {
        pv.hintLength.assign(pixels * 2, 0.0f);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (lengthWeight[i] <= 1e-3f) continue;
            pv.hintLength[2 * i] = length[i] / lengthWeight[i] * worldPerPixel;
            pv.hintLength[2 * i + 1] = std::min(1.0f, lengthWeight[i] / std::max(1.0f, sigma));
        }
    }
}

// How long the drawn lines say the hair growing at `p` is (scene units) and how much they say it
// (0-1): the views that see it, weighed by how much each says.
float hintedLength(const std::vector<PreparedView>& views, const Vec3f& p, float& say) {
    double sum = 0.0, weight = 0.0;
    say = 0.0f;
    for (const PreparedView& v : views) {
        if (v.hintLength.empty()) continue;
        float u = 0, vv = 0, z = 0;
        if (!v.camera.project(p, u, vv, &z) || !v.inFrame(u, vv) || v.hidden(u, vv, z)) continue;
        const std::size_t at = v.pixel(u, vv);
        const float wgt = v.hintLength[2 * at + 1];
        if (wgt <= 0.0f) continue;
        sum += static_cast<double>(wgt) * v.hintLength[2 * at];
        weight += wgt;
        say = std::max(say, wgt);
    }
    return weight > 0.0 ? static_cast<float>(sum / weight) : 0.0f;
}

// How much a guide's first stretch goes the way the hints say (-1 against .. 1 along; 0 without).
float hintAgreement(const std::vector<PreparedView>& views, const std::vector<Vec3f>& points) {
    double sum = 0.0;
    int count = 0;
    const std::size_t last = std::min<std::size_t>(points.size() - 1, 6);
    for (const PreparedView& v : views) {
        if (v.hintWay.empty()) continue;
        for (std::size_t k = 1; k <= last; ++k) {
            float u0 = 0, v0 = 0, z0 = 0, u1 = 0, v1 = 0, z1 = 0;
            if (!v.camera.project(points[k - 1], u0, v0, &z0) || !v.camera.project(points[k], u1, v1, &z1)) continue;
            if (!v.inFrame(u1, v1) || v.hidden(u1, v1, z1)) continue;
            const float du = u1 - u0, dv = v1 - v0, len = std::sqrt(du * du + dv * dv);
            if (len < 1e-6f) continue;
            const std::size_t at = v.pixel(u1, v1);
            const float strength = v.hintWay[3 * at + 2];
            if (strength <= 0.0f) continue;
            sum += strength * (du * v.hintWay[3 * at] + dv * v.hintWay[3 * at + 1]) / len;
            ++count;
        }
    }
    return count ? static_cast<float>(sum / count) : 0.0f;
}

// The symmetry in use and how points and directions cross it.
struct Mirror {
    StrokeSymmetry sym;
    bool on() const { return sym.on(); }
    Vec3f point(const Vec3f& p) const { return sym.point(p); }
    Vec3f vector(const Vec3f& at, const Vec3f& v) const { return sym.vector(at, v); }
};

// What the views say at a point.
struct Evidence {
    Sym3 m;          // sum of c n n^T over the views (and mirrors) seeing it
    double weight = 0.0;
    int seen = 0;    // views (and mirrors) that see it
    int inside = 0;  // of those, the ones whose mask holds it
    int outside = 0;
};

struct Context {
    const ScalpMesh& scalp;
    const std::vector<PreparedView>& views;
    const ReferenceGuideOptions& options;
    Mirror mirror;
    float scalpSize = 1.0f;
    float maxLength = 1.0f, step = 0.1f, offset = 0.01f;
    bool inflate = false;
    const ReferenceShell* shell = nullptr;
    std::vector<float> occluderSigns;  // +1 where an occluder's normals point out, -1 in, 0 none
    float surfacePrior = 0.15f, gravityPrior = 0.15f;  // the options', or short hair's (lying on the surface)

    // One view's say at `q` (a point, or a point's mirror image): -1 unseen, 0 outside, 1 inside
    // (and the constraint plane's normal, its confidence).
    int look(const PreparedView& v, const Vec3f& q, Vec3f& normal, float& conf, bool hull) const {
        float u = 0, vv = 0, z = 0;
        if (!v.camera.project(q, u, vv, &z) || !v.inFrame(u, vv)) return -1;
        if (v.hidden(u, vv, z)) return -1;
        const float m = (hull ? v.hullMask : v.mask).sample(u, vv);
        if (m < 0.5f) return 0;
        conf = 0.0f;
        float du = 1, dv = 0;
        if (!v.field->sample(u * static_cast<float>(v.field->width) / static_cast<float>(v.width),
                             vv * static_cast<float>(v.field->height) / static_cast<float>(v.height), du, dv, conf))
            return 1;
        const Vec3f along = v.camera.axis(0) * du - v.camera.axis(1) * dv;
        const Vec3f ray = (q - v.camera.eye()).normalized();
        normal = ray.cross(along).normalized();
        return 1;
    }

    Evidence evidence(const Vec3f& p) const {
        Evidence e;
        const int sides = mirror.on() ? 2 : 1;
        Vec3f q[2] = {p, p};
        if (sides == 2) q[1] = mirror.point(p);
        for (const PreparedView& v : views)
            for (int s = 0; s < sides; ++s) {
                Vec3f n;
                float conf = 0.0f;
                const int say = look(v, q[s], n, conf, true);
                if (say < 0) continue;
                ++e.seen;
                if (say == 0) {
                    ++e.outside;
                    continue;
                }
                ++e.inside;
                if (conf <= 0.0f) continue;
                if (s == 1) n = mirror.vector(q[1], n).normalized();
                conf *= options.orientationWeight;
                e.m.addOuter(n, conf);
                e.weight += conf;
            }
        return e;
    }

    // Whether the hull (silhouettes, depth-aware) and the inflated depth admit a point.
    // Whether a view (or its mirror) that sees the point has sure hair over it (rootMask).
    bool rootCovered(const Vec3f& p) const {
        const int sides = mirror.on() ? 2 : 1;
        for (const PreparedView& v : views)
            for (int s = 0; s < sides; ++s) {
                const Vec3f q = s == 0 ? p : mirror.point(p);
                float u = 0, vv = 0, z = 0;
                if (!v.camera.project(q, u, vv, &z) || !v.inFrame(u, vv) || v.hidden(u, vv, z)) continue;
                if (v.rootMask.sample(u, vv) >= 0.5f) return true;
            }
        return false;
    }

    bool admits(const Vec3f& p, float scalpDistance) const {
        for (const PreparedView& v : views) {
            float u = 0, vv = 0, z = 0;
            if (!v.camera.project(p, u, vv, &z) || !v.inFrame(u, vv)) continue;
            const std::size_t at = v.pixel(u, vv);
            if (v.hullMask.sample(u, vv) < 0.5f && !v.hidden(u, vv, z)) return false;
            if (inflate) {
                const float perPx = z / static_cast<float>(v.camera.focalPx);
                const float allowed = std::max(2.0f * offset, v.rimPx * perPx) + options.inflateFactor * v.scalpDistance[at] * perPx;
                if (scalpDistance > allowed) return false;
            }
        }
        return true;
    }
};

}  // namespace

// ---- The shell ------------------------------------------------------------------------------------

struct ReferenceShellBuilder {
    static ReferenceShell make(const Context& ctx) {
        ReferenceShell shell;
        build(shell, ctx);
        return shell;
    }
    static void build(ReferenceShell& shell, const Context& ctx) {
        const ScalpMesh& scalp = ctx.scalp;
        Vec3f lo(kInf, kInf, kInf), hi(-kInf, -kInf, -kInf);
        for (const Vec3f& p : scalp.points()) {
            lo = minPerAxis(lo, p);
            hi = maxPerAxis(hi, p);
        }
        const Vec3f pad(ctx.maxLength, ctx.maxLength, ctx.maxLength);
        lo = lo - pad;
        hi = hi + pad;
        const Vec3f size = hi - lo;
        const float longest = std::max({size.x, size.y, size.z});
        const int res = std::max(8, ctx.options.shellResolution);
        shell.cell_ = longest / static_cast<float>(res);
        shell.lo_ = lo;
        shell.nx_ = std::max(1, static_cast<int>(std::ceil(size.x / shell.cell_)));
        shell.ny_ = std::max(1, static_cast<int>(std::ceil(size.y / shell.cell_)));
        shell.nz_ = std::max(1, static_cast<int>(std::ceil(size.z / shell.cell_)));
        const std::size_t count = static_cast<std::size_t>(shell.nx_) * static_cast<std::size_t>(shell.ny_) * static_cast<std::size_t>(shell.nz_);
        std::vector<std::uint8_t> raw(count, 0);
        parallelFor(0, static_cast<std::size_t>(shell.nz_), [&](std::size_t kz) {
            for (int j = 0; j < shell.ny_; ++j)
                for (int i = 0; i < shell.nx_; ++i) {
                    const Vec3f c = lo + Vec3f((static_cast<float>(i) + 0.5f) * shell.cell_, (static_cast<float>(j) + 0.5f) * shell.cell_,
                                               (static_cast<float>(kz) + 0.5f) * shell.cell_);
                    const SurfacePoint sp = scalp.closestPoint(c, ctx.maxLength + shell.cell_);
                    if (sp.root.face < 0) continue;
                    const float d = (c - sp.position).length();
                    if (!ctx.admits(c, d)) continue;
                    if (ctx.mirror.on()) {
                        const Vec3f m = ctx.mirror.point(c);
                        if (!ctx.admits(m, d)) continue;
                    }
                    raw[(kz * static_cast<std::size_t>(shell.ny_) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(shell.nx_) +
                        static_cast<std::size_t>(i)] = 1;
                }
        }, 1);
        // Grown by a voxel: a guide brushing the hull's edge is not stopped by its steps.
        shell.voxels_.assign(count, 0);
        for (int k = 0; k < shell.nz_; ++k)
            for (int j = 0; j < shell.ny_; ++j)
                for (int i = 0; i < shell.nx_; ++i) {
                    bool any = false;
                    for (int dk = -1; dk <= 1 && !any; ++dk)
                        for (int dj = -1; dj <= 1 && !any; ++dj)
                            for (int di = -1; di <= 1 && !any; ++di) {
                                const int a = i + di, b = j + dj, c = k + dk;
                                if (a < 0 || b < 0 || c < 0 || a >= shell.nx_ || b >= shell.ny_ || c >= shell.nz_) continue;
                                any = raw[(static_cast<std::size_t>(c) * static_cast<std::size_t>(shell.ny_) + static_cast<std::size_t>(b)) *
                                              static_cast<std::size_t>(shell.nx_) + static_cast<std::size_t>(a)] != 0;
                            }
                    shell.voxels_[(static_cast<std::size_t>(k) * static_cast<std::size_t>(shell.ny_) + static_cast<std::size_t>(j)) *
                                      static_cast<std::size_t>(shell.nx_) + static_cast<std::size_t>(i)] = any ? 1 : 0;
                }
    }
};

bool ReferenceShell::inside(const Vec3f& p) const {
    const Vec3f g = (p - lo_) / cell_;
    const int i = static_cast<int>(std::floor(g.x)), j = static_cast<int>(std::floor(g.y)), k = static_cast<int>(std::floor(g.z));
    if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) return false;
    return voxels_[(static_cast<std::size_t>(k) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(nx_) +
                   static_cast<std::size_t>(i)] != 0;
}

float ReferenceShell::fill() const {
    if (voxels_.empty()) return 0.0f;
    std::size_t n = 0;
    for (std::uint8_t v : voxels_) n += v;
    return static_cast<float>(n) / static_cast<float>(voxels_.size());
}

// ---- Options, symmetry ------------------------------------------------------------------------------

const char* referenceSymmetryName(ReferenceSymmetry s) {
    switch (s) {
        case ReferenceSymmetry::None: return "none";
        case ReferenceSymmetry::WorldX: return "x";
        case ReferenceSymmetry::Topology: return "topology";
        default: return "auto";
    }
}

ReferenceSymmetry referenceSymmetryFromName(const std::string& name) {
    if (name == "none" || name == "off" || name.empty()) return ReferenceSymmetry::None;
    if (name == "x" || name == "worldX" || name == "world") return ReferenceSymmetry::WorldX;
    if (name == "topology") return ReferenceSymmetry::Topology;
    if (name == "auto") return ReferenceSymmetry::Auto;
    throw std::invalid_argument("symmetry is none, x, topology or auto, not '" + name + "'");
}

bool scalpLooksSymmetric(const ScalpMesh& scalp) {
    if (scalp.pointCount() == 0) return false;
    const auto mirror = ScalpMirror::of(scalp);
    if (!mirror) return false;
    if (static_cast<double>(mirror->pairedCount()) >= 0.9 * static_cast<double>(scalp.pointCount())) return true;
    // Symmetric in shape, if not to the vertex (a sculpted head, a morphed one): its points mirrored
    // across its middle land on its surface (within 1% of its size), nearly all of them. The mirror's
    // partners are then the nearest surface spots.
    const ScalpMesh rest = scalp.restPose();
    const std::vector<Vec3f>& pts = rest.points();
    Vec3f lo = pts.front(), hi = pts.front();
    for (const Vec3f& p : pts) {
        lo = minPerAxis(lo, p);
        hi = maxPerAxis(hi, p);
    }
    const float middle = 0.5f * (lo.x + hi.x), near = 0.01f * (hi - lo).length();
    const std::size_t every = std::max<std::size_t>(1, pts.size() / 2000);
    std::size_t tried = 0, on = 0;
    for (std::size_t i = 0; i < pts.size(); i += every, ++tried) {
        const Vec3f m(2.0f * middle - pts[i].x, pts[i].y, pts[i].z);
        if ((rest.closestPoint(m).position - m).length() <= near) ++on;
    }
    return static_cast<double>(on) >= 0.9 * static_cast<double>(tried);
}

namespace {

float scalpDiagonal(const ScalpMesh& scalp) {
    Vec3f lo(kInf, kInf, kInf), hi(-kInf, -kInf, -kInf);
    for (const Vec3f& p : scalp.points()) {
        lo = minPerAxis(lo, p);
        hi = maxPerAxis(hi, p);
    }
    return (hi - lo).length();
}

// Roots spread over the scalp: random candidates (by area) kept when no kept one is nearer than a
// spacing that shrinks until `count` are kept.
std::vector<SurfacePoint> blueNoiseRoots(const ScalpMesh& scalp, int count, std::uint64_t seed) {
    std::vector<SurfacePoint> out;
    if (count <= 0 || scalp.triangleCount() == 0) return out;
    std::vector<double> cdf(scalp.triangleCount());
    double total = 0.0;
    for (std::size_t t = 0; t < scalp.triangleCount(); ++t) {
        const auto& tri = scalp.triangles()[t];
        const Vec3f a = scalp.points()[static_cast<std::size_t>(tri[0])], b = scalp.points()[static_cast<std::size_t>(tri[1])],
                    c = scalp.points()[static_cast<std::size_t>(tri[2])];
        total += 0.5 * static_cast<double>((b - a).cross(c - a).length());
        cdf[t] = total;
    }
    Rng rng(seed);
    const std::size_t candidates = static_cast<std::size_t>(count) * 8;
    std::vector<SurfacePoint> pool;
    pool.reserve(candidates);
    for (std::size_t i = 0; i < candidates; ++i) {
        const double r = static_cast<double>(rng.nextFloat()) * total;
        const std::size_t t = std::min(cdf.size() - 1, static_cast<std::size_t>(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin()));
        float u = rng.nextFloat(), v = rng.nextFloat();
        if (u + v > 1.0f) {
            u = 1.0f - u;
            v = 1.0f - v;
        }
        SurfacePoint sp;
        sp.root = StrandRoot{static_cast<std::int32_t>(t), u, v};
        sp.position = scalp.position(sp.root);
        sp.normal = scalp.normal(sp.root);
        pool.push_back(sp);
    }
    float spacing = static_cast<float>(std::sqrt(total / static_cast<double>(count)));
    std::vector<std::uint8_t> taken(pool.size(), 0);
    while (static_cast<int>(out.size()) < count && spacing > 1e-9f) {
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> grid;
        auto key = [&](int x, int y, int z) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 73856093ull) ^
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 19349663ull) ^
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 83492791ull);
        };
        auto cellOf = [&](const Vec3f& p, int& x, int& y, int& z) {
            x = static_cast<int>(std::floor(p.x / spacing));
            y = static_cast<int>(std::floor(p.y / spacing));
            z = static_cast<int>(std::floor(p.z / spacing));
        };
        for (std::size_t i = 0; i < out.size(); ++i) {
            int x, y, z;
            cellOf(out[i].position, x, y, z);
            grid[key(x, y, z)].push_back(i);
        }
        for (std::size_t i = 0; i < pool.size() && static_cast<int>(out.size()) < count; ++i) {
            if (taken[i]) continue;
            int x, y, z;
            cellOf(pool[i].position, x, y, z);
            bool near = false;
            for (int a = -1; a <= 1 && !near; ++a)
                for (int b = -1; b <= 1 && !near; ++b)
                    for (int c = -1; c <= 1 && !near; ++c) {
                        auto it = grid.find(key(x + a, y + b, z + c));
                        if (it == grid.end()) continue;
                        for (std::size_t o : it->second)
                            if ((out[o].position - pool[i].position).length() < spacing) {
                                near = true;
                                break;
                            }
                    }
            if (near) continue;
            taken[i] = 1;
            grid[key(x, y, z)].push_back(out.size());
            out.push_back(pool[i]);
        }
        spacing *= 0.85f;
    }
    return out;
}

struct Grown {
    std::vector<Vec3f> points;
    float score = 0.0f;
    float confidence = 0.0f;
    std::string stop;
    Vec3f tangent;  // the first stretch's direction
};

// The cosine of an angle a thousandth of a radian inside `maxAngle` (far more than the arccosine
// and the cosine are off by): a turn whose cosine reaches it is within `maxAngle` whatever the
// arccosine's last bits say, so limitTurn answers the same without it.
float sureTurnCos(float maxAngle) { return maxAngle > 2e-3f ? pm::cos(maxAngle - 1e-3f) : 2.0f; }

// `to`, turned no more than `maxAngle` from `from`. `sureCos`: sureTurnCos(maxAngle), the cosine
// from which the turn is surely within the angle (no arccosine needed).
Vec3f limitTurn(const Vec3f& from, const Vec3f& to, float maxAngle, float sureCos) {
    const float c = std::clamp(from.dot(to), -1.0f, 1.0f);
    if (c >= sureCos) return to;
    const float angle = pm::acos(c);
    if (angle <= maxAngle) return to;
    Vec3f axis = from.cross(to);
    if (axis.length() < 1e-8f) return from;
    return rotateAxisAngle(from, axis.normalized(), maxAngle).normalized();
}

Grown grow(const Context& ctx, const SurfacePoint& root, float sign, float maxLength) {
    Grown g;
    const Vec3f gravity = -ctx.options.up.normalized();
    const Vec3f n = root.normal.normalized();
    g.points.push_back(root.position);
    Vec3f p = root.position + n * ctx.offset;
    // The first direction: the one in the views' planes lying flattest on the scalp, lifted 25°.
    // The evidence over a small patch around the root (one pixel's say is noisy), weak evidence
    // weighed as weak (not normalized up), gravity along the surface breaking ties.
    Vec3f downSurface = gravity - n * gravity.dot(n);
    downSurface = downSurface.length() > 1e-4f ? downSurface.normalized() : anyPerp(n);
    const Vec3f side = n.cross(downSurface).normalized();
    // Which axis on the scalp the hair around the root follows: of 12 axes, the one the evidence
    // along a short line through the root agrees with best (a single spot can sit on a hairline's
    // ends or a highlight), then refined from the evidence along it.
    const Vec3f at = p + n * ctx.step;
    const int reach = 5;
    double bestScore = -1e30;
    std::vector<Evidence> bestLine;
    // Every axis's line passes through the root's point: its evidence found once (kept by the
    // very point asked, a zero's sign and all, so each line reads what it would have found).
    Vec3f middle;
    Evidence middleEvidence;
    bool middleKnown = false;
    auto sameValue = [](float a, float b) { return a == b && std::signbit(a) == std::signbit(b); };
    for (int a = 0; a < 12; ++a) {
        const float angle = kPi * static_cast<float>(a) / 12.0f;
        const Vec3f axis = downSurface * pm::cos(angle) + side * pm::sin(angle);
        std::vector<Evidence> line;
        double score = 0.0;
        for (int k = -reach; k <= reach; ++k) {
            const Vec3f point = at + axis * (static_cast<float>(k) * ctx.step);
            const bool known = k == 0 && middleKnown && sameValue(point.x, middle.x) && sameValue(point.y, middle.y) &&
                               sameValue(point.z, middle.z);
            const Evidence e = known ? middleEvidence : ctx.evidence(point);
            if (k == 0 && !known) middle = point, middleEvidence = e, middleKnown = true;
            const double a3[3] = {axis.x, axis.y, axis.z};
            double quad = 0.0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) quad += a3[i] * e.m.m[i][j] * a3[j];
            score += e.weight - 2.0 * quad;
            line.push_back(e);
        }
        if (score > bestScore) {
            bestScore = score;
            bestLine = std::move(line);
        }
    }
    Sym3 m;
    double weight = 0.0;
    for (const Evidence& e : bestLine) {
        m.add(e.m, 1.0);
        weight += e.weight;
    }
    const double scale = 1.0 / std::max(weight, 0.3 * static_cast<double>(bestLine.size()));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] *= scale;
    m.addOuter(n, 1.0);
    m.addIdentity(0.05);
    m.addOuter(downSurface, -0.05);
    Vec3f t = smallestEigenvector(m);
    t = (t - n * t.dot(n));
    t = t.length() > 1e-4f ? t.normalized() : downSurface;
    if (t.length() < 0.5f) t = anyPerp(n);
    t = t * sign;
    g.tangent = t;
    Vec3f d = (t + n * pm::tan(25.0f * kPi / 180.0f)).normalized();
    g.points.push_back(p);
    float length = ctx.offset, evidenceSum = 0.0f;
    int blind = 0, seenSteps = 0;
    const int maxSteps = static_cast<int>(std::ceil(maxLength / ctx.step)) + 2;
    const int maxBlind = std::max(3, maxSteps / 4);
    const float maxTurn = 30.0f * kPi / 180.0f;
    const float maxTurnCos = sureTurnCos(maxTurn);
    g.stop = "length";
    for (int k = 0; k < maxSteps; ++k) {
        const Evidence e = ctx.evidence(p);
        if (e.outside > 0 && e.outside >= e.inside) {
            g.stop = "mask";
            for (int drop = 0; drop < 2 && g.points.size() > 4; ++drop) g.points.pop_back();  // (the steps found past the hair: the guide ends in it)
            break;
        }
        // The outward direction off the scalp and the distance to it.
        const SurfacePoint sp = ctx.scalp.closestPoint(p);
        Vec3f out = p - sp.position;
        const float height = out.length();
        out = height > 1e-6f ? out / height : sp.normal;
        if (out.dot(sp.normal) < 0.0f) out = sp.normal;
        Vec3f next;
        if (e.seen == 0) {
            if (++blind > maxBlind) {
                g.stop = "unseen";
                break;
            }
            next = (d + gravity * 0.05f).normalized();
        } else {
            blind = 0;
            ++seenSteps;
            evidenceSum += static_cast<float>(std::min(1.0, e.weight));
            Sym3 system = e.m;
            system.addIdentity(ctx.options.continuityPrior);
            system.addOuter(d, -ctx.options.continuityPrior);
            // Lying along the surface near it (off the root), fading out as the hair leaves it (hanging).
            const float ramp = std::clamp(length / (3.0f * ctx.step), 0.0f, 1.0f) * std::clamp(1.0f - height / (0.1f * ctx.scalpSize), 0.0f, 1.0f);
            system.addOuter(out, ctx.surfacePrior * ramp);
            // Gravity along the surface: what decides where the views say little.
            Vec3f down = gravity - out * (gravity.dot(out) * std::clamp(1.0f - height / (0.1f * ctx.scalpSize), 0.0f, 1.0f));
            if (ctx.gravityPrior > 0.0f && down.length() > 0.1f) {
                down = down.normalized();
                system.addIdentity(ctx.gravityPrior);
                system.addOuter(down, -ctx.gravityPrior);
            }
            next = smallestEigenvector(system);
            if (next.dot(d) < 0.0f) next = -next;
        }
        next = limitTurn(d, next, maxTurn, maxTurnCos);
        Vec3f q = p + next * ctx.step;
        // Above the scalp.
        const SurfacePoint near = ctx.scalp.closestPoint(q, 4.0f * ctx.offset + ctx.step);
        if (near.root.face >= 0) {
            const float h = (q - near.position).dot(near.normal);
            if (h < ctx.offset) q = q + near.normal * (ctx.offset - h);
        }
        // Out of the occluders (a head under a scalp cap, a body): their outward side by the sign
        // their normals point with.
        for (std::size_t o = 0; o < ctx.options.occluders.size(); ++o) {
            const auto& occluder = ctx.options.occluders[o];
            if (!occluder || ctx.occluderSigns[o] == 0.0f) continue;
            const SurfacePoint hit = occluder->closestPoint(q, 4.0f * ctx.offset + ctx.step);
            if (hit.root.face < 0) continue;
            const Vec3f outward = hit.normal * ctx.occluderSigns[o];
            const float h = (q - hit.position).dot(outward);
            if (h < ctx.offset) q = q + outward * (ctx.offset - h);
        }
        // In the shell: pulled in toward the scalp, else the end.
        if (ctx.shell && !ctx.shell->inside(q)) {
            bool fixed = false;
            for (int pull = 1; pull <= 3 && !fixed; ++pull) {
                const Vec3f r = q - out * (ctx.step * 0.5f * static_cast<float>(pull));
                if (ctx.shell->inside(r)) {
                    q = r;
                    fixed = true;
                }
            }
            if (!fixed) {
                g.stop = "shell";
                break;
            }
        }
        const Vec3f moved = q - p;
        const float len = moved.length();
        if (len < 1e-8f) {
            g.stop = "scalp";
            break;
        }
        d = moved / len;
        p = q;
        g.points.push_back(p);
        length += len;
        if (length >= maxLength) {
            g.stop = "length";
            break;
        }
    }
    g.score = evidenceSum * ctx.step;
    g.confidence = seenSteps ? evidenceSum / static_cast<float>(seenSteps) : 0.0f;
    // The direction over the first stretch (for the neighbours' vote).
    const std::size_t far = std::min<std::size_t>(g.points.size() - 1, 4);
    if (far > 0) {
        Vec3f v = g.points[far] - g.points[0];
        v = v - n * v.dot(n);
        if (v.length() > 1e-8f) g.tangent = v.normalized();
    }
    return g;
}

std::vector<Vec3f> smoothed(std::vector<Vec3f> pts, int iterations);

// The hair's thickness over a spot of the scalp (its outward normal `n`): in each view that sees the
// spot near its outline (the normal across the view), how far out along the normal the hair mask
// reaches (the silhouette's rim past the scalp there); the median over those views. Negative: no
// view shows it in profile.
float hairThickness(const Context& ctx, const Vec3f& x, const Vec3f& n, float reach) {
    std::vector<float> seen;
    for (const PreparedView& v : ctx.views) {
        const Vec3f toEye = (v.camera.eye() - x).normalized();
        if (std::abs(n.dot(toEye)) > 0.4f) continue;  // (facing the view or away: no rim there)
        float u = 0, vv = 0, z = 0;
        if (!v.camera.project(x, u, vv, &z) || !v.inFrame(u, vv)) continue;
        float t = 0.0f;
        bool out = false;  // (the march reached the hair's edge where the view sees it)
        for (float s = ctx.step; s <= reach; s += 0.5f * ctx.step) {
            const Vec3f q = x + n * s;
            if (!v.camera.project(q, u, vv, &z) || !v.inFrame(u, vv)) break;
            if (v.hidden(u, vv, z)) {  // (behind the head from here on: as far as it showed hair)
                out = t > 0.0f;
                break;
            }
            if (v.hullMask.sample(u, vv) < 0.5f) {
                out = true;
                break;
            }
            t = s;
        }
        if (out) seen.push_back(t);
    }
    return seen.empty() ? -1.0f : median(seen);
}

// A guide lifted to fill the hair's thickness the silhouettes show: each point at least `share` of
// the thickness over its spot from the scalp (at most half the length out), rising from the root no
// steeper than the guide runs, the lift blended along the guide (no jumps).
std::vector<Vec3f> volumized(const Context& ctx, const std::vector<Vec3f>& path, float share) {
    if (share <= 0.0f || path.size() < 3) return path;
    const float reach = 0.5f * ctx.maxLength;
    std::vector<float> lift(path.size(), 0.0f);
    std::vector<Vec3f> up(path.size());
    float along = 0.0f, last = 0.0f, known = -1.0f;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) along += (path[i] - path[i - 1]).length();
        const SurfacePoint sp = ctx.scalp.closestPoint(path[i]);
        if (sp.root.face < 0) continue;
        Vec3f dir = path[i] - sp.position;
        const float height = dir.length();
        dir = height > 1e-6f ? dir / height : sp.normal.normalized();
        if (dir.dot(sp.normal) < 0.0f) dir = sp.normal.normalized();
        up[i] = dir;
        float thickness = hairThickness(ctx, sp.position, sp.normal.normalized(), reach);
        if (thickness < 0.0f) thickness = known;  // (no view in profile here: the last spot's)
        else known = thickness;
        if (thickness < 0.0f) continue;
        float target = std::min(share * thickness, 0.7f * along);
        target = std::min(target, last + 0.7f * (i > 0 ? (path[i] - path[i - 1]).length() : 0.0f) + 1e-6f);
        last = std::max(height, target);
        lift[i] = std::max(0.0f, target - height);
    }
    // Blended along the guide, then put on.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = lift;
        for (std::size_t i = 1; i + 1 < lift.size(); ++i) next[i] = 0.25f * lift[i - 1] + 0.5f * lift[i] + 0.25f * lift[i + 1];
        lift.swap(next);
    }
    std::vector<Vec3f> out = path;
    for (std::size_t i = 1; i < path.size(); ++i) out[i] = path[i] + up[i] * lift[i];
    return out;
}

// ---- How long the strands are in the photos ------------------------------------------------------

// The hair's strands' lengths as the photos show them: in each view, streamlines along the locks'
// flow (the coarse field) from points spread over the hair, both ways to where the hair ends (the
// mask's edge) or the flow curls round on itself, in scene units at the scalp's depth; the upper
// quartile in each view (a streamline may run on from one lock into the next), the longest view's (a profile sees the hair over the top full length, the front
// foreshortened). 0 without views to read.
float strandPathLength(std::vector<PreparedView>& views) {
    float longest = 0.0f;
    for (PreparedView& v : views) {
        const OrientationField* flow = v.coarse ? v.coarse.get() : v.field.get();
        if (!flow || v.scalpDepth <= 0.0f || v.mask.empty()) continue;
        const float fx = static_cast<float>(flow->width) / static_cast<float>(v.width), fy = static_cast<float>(flow->height) / static_cast<float>(v.height);
        auto inHair = [&](float u, float w) { return v.inFrame(u, w) && v.mask.sample(u, w) >= 0.5f; };
        const int spacing = std::max(6, std::max(v.width, v.height) / 40);
        std::vector<float> lengths;
        v.strandLength.assign(static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height), 0.0f);
        const float perPixel = v.scalpDepth / static_cast<float>(v.camera.focalPx);
        std::vector<std::size_t> visited[2];
        for (int sy = spacing / 2; sy < v.height; sy += spacing)
            for (int sx = spacing / 2; sx < v.width; sx += spacing) {
                const float u0 = static_cast<float>(sx) + 0.5f, w0 = static_cast<float>(sy) + 0.5f;
                if (!inHair(u0, w0)) continue;
                float total = 0.0f;
                visited[0].clear();
                visited[1].clear();
                for (int dir = 0; dir < 2; ++dir) {
                    float u = u0, w = w0, pu = 0, pw = 0, turned = 0.0f;
                    bool first = true;
                    for (int k = 0; k < 4 * (v.width + v.height); ++k) {
                        float du = 0, dw = 0, c = 0;
                        if (!inHair(u, w) || !flow->sample(u * fx, w * fy, du, dw, c) || c < 0.05f) break;
                        if (first) {
                            pu = dir ? -du : du;
                            pw = dir ? -dw : dw;
                            first = false;
                        }
                        if (du * pu + dw * pw < 0.0f) du = -du, dw = -dw;
                        if (!first) turned += std::acos(std::clamp(du * pu + dw * pw, -1.0f, 1.0f));
                        if (turned > kPi) break;  // (curling round on itself: a strand's path does not)
                        pu = du;
                        pw = dw;
                        visited[dir].push_back(v.pixel(u, w));
                        u += du;
                        w += dw;
                        total += 1.0f;
                    }
                }
                lengths.push_back(total * perPixel);
                // Each pixel on it: how far the path runs on from there, the longer way (a root's strand
                // runs one way from it; the other way is other hair passing over).
                const std::size_t a = visited[1].size(), b = visited[0].size();
                for (std::size_t k = 0; k < b; ++k) {
                    const float run = static_cast<float>(std::max(a + k, b - k)) * perPixel;
                    v.strandLength[visited[0][k]] = std::max(v.strandLength[visited[0][k]], run);
                }
                for (std::size_t k = 0; k < a; ++k) {
                    const float run = static_cast<float>(std::max(a - k, b + k)) * perPixel;
                    v.strandLength[visited[1][k]] = std::max(v.strandLength[visited[1][k]], run);
                }
            }
        if (lengths.size() < 10) continue;
        std::sort(lengths.begin(), lengths.end());
        longest = std::max(longest, lengths[static_cast<std::size_t>(0.75 * static_cast<double>(lengths.size() - 1))]);
    }
    return longest;
}

// The longest strand path the views show near a spot (strandPathLength's maps, within a few pixels
// of where each view sees it unhidden); 0 where none shows one.
float localStrandLength(const std::vector<PreparedView>& views, const Vec3f& at) {
    float best = 0.0f;
    for (const PreparedView& v : views) {
        if (v.strandLength.empty()) continue;
        float u = 0, w = 0, z = 0;
        if (!v.camera.project(at, u, w, &z) || !v.inFrame(u, w) || v.hidden(u, w, z)) continue;
        const int r = std::max(2, std::max(v.width, v.height) / 100);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const float x = u + static_cast<float>(dx), y = w + static_cast<float>(dy);
                if (v.inFrame(x, y)) best = std::max(best, v.strandLength[v.pixel(x, y)]);
            }
    }
    return best;
}

// ---- Locks: each guide's own curl ----------------------------------------------------------------

// A lock's curl along a guide as one view shows it: the photo's fine strand direction against the
// guide's projected way, sampled a pixel apart along the longest stretch the view sees in the hair,
// fitted with one wave (period near the view's curl period): a(s) = amplitude cos(2 pi s / period +
// phase). quality: the wave's share of the swing (1 all of it).
struct LockFit {
    bool ok = false;
    int view = -1;
    float periodPx = 0.0f, amplitude = 0.0f, phase = 0.0f, quality = 0.0f;
    float pxPerUnit = 0.0f;    // across the view at the stretch's depth (focal / depth)
    float pxPerLength = 0.0f;  // along it: projected pixels per unit of the guide's length
    std::size_t first = 0, last = 0;  // the stretch (path indices)
    std::vector<float> s;      // each path point's place along the wave, pixels (extended past the stretch)
    float score = 0.0f;        // to pick the view: quality times the turns seen
    float shiftPx = 0.0f;      // the path moved this far sideways in the view onto the lock's middle
    // The lock's own path in the view: how far its strands stand sideways off the guide (pixels, along
    // the guide's way turned a quarter) at each pixel along the stretch: the swing summed up, its
    // drift over a turn taken out (the guide follows the lock's middle).
    std::vector<float> offset;
    // Sideways off the guide at `s` pixels along (past the stretch: the fitted wave, met smoothly).
    float sideways(float at) const {
        const float radius = std::tan(std::min(amplitude, 1.2f)) * periodPx / (2.0f * kPi);
        const auto wave = [&](float x) { return radius * std::sin(2.0f * kPi * x / periodPx + phase); };
        if (offset.empty()) return wave(at);
        const float end = static_cast<float>(offset.size() - 1);
        if (at >= 0.0f && at <= end) {
            const std::size_t k = std::min(offset.size() - 2, static_cast<std::size_t>(at));
            const float t = at - static_cast<float>(k);
            return offset[k] + (offset[k + 1] - offset[k]) * t;
        }
        const float edge = at < 0.0f ? 0.0f : end, past = std::abs(at - edge);
        const float meet = std::clamp(1.0f - past / (0.25f * periodPx), 0.0f, 1.0f);
        return wave(at) + (offset[at < 0.0f ? 0 : offset.size() - 1] - wave(edge)) * meet;
    }
};

// `shiftPx`: the path tried that far sideways in the view (its projected way turned a quarter).
LockFit fitLock(const PreparedView& v, int viewIndex, const std::vector<Vec3f>& path, float shiftPx = 0.0f) {
    LockFit fit;
    fit.view = viewIndex;
    fit.shiftPx = shiftPx;
    const OrientationField* fine = v.fine ? v.fine.get() : v.field.get();
    if (!fine || v.curl.wavelengthPx <= 0.0f || path.size() < 4) return fit;
    const std::size_t n = path.size();
    std::vector<float> u(n), vv(n), z(n);
    std::vector<std::uint8_t> seen(n, 0), front(n, 0);
    for (std::size_t i = 0; i < n; ++i) front[i] = v.camera.project(path[i], u[i], vv[i], &z[i]) ? 1 : 0;
    if (shiftPx != 0.0f) {
        const std::vector<float> u0 = u, v0 = vv;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t a = i > 0 ? i - 1 : 0, b = std::min(n - 1, i + 1);
            const float du = u0[b] - u0[a], dv = v0[b] - v0[a], l = std::hypot(du, dv);
            if (l < 1e-6f) continue;
            u[i] = u0[i] - dv / l * shiftPx;
            vv[i] = v0[i] + du / l * shiftPx;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!front[i] || !v.inFrame(u[i], vv[i])) continue;
        seen[i] = !v.hidden(u[i], vv[i], z[i]) && v.mask.sample(u[i], vv[i]) >= 0.5f ? 1 : 0;
    }
    // The longest stretch seen.
    std::size_t best0 = 0, best1 = 0;
    for (std::size_t i = 0; i < n;) {
        if (!seen[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < n && seen[j + 1]) ++j;
        if (j - i > best1 - best0) best0 = i, best1 = j;
        i = j + 1;
    }
    if (best1 <= best0 + 2) return fit;
    std::vector<float> along(n, 0.0f), length(n, 0.0f);
    for (std::size_t i = 1; i < n; ++i) length[i] = length[i - 1] + (path[i] - path[i - 1]).length();
    for (std::size_t i = best0 + 1; i <= best1; ++i) along[i] = along[i - 1] + std::hypot(u[i] - u[i - 1], vv[i] - vv[i - 1]);
    const float pixels = along[best1], period0 = v.curl.wavelengthPx;
    if (pixels < 1.5f * period0 || length[best1] - length[best0] <= 0.0f) return fit;
    // Sampled a pixel apart: the fine direction's angle to the guide's way.
    std::vector<float> a, at;
    const float fx = static_cast<float>(fine->width) / static_cast<float>(v.width), fy = static_cast<float>(fine->height) / static_cast<float>(v.height);
    std::size_t seg = best0;
    for (float s = 0.0f; s <= pixels; s += 1.0f) {
        while (seg + 1 < best1 && along[seg + 1] < s) ++seg;
        const float l = std::max(1e-6f, along[seg + 1] - along[seg]), t = std::clamp((s - along[seg]) / l, 0.0f, 1.0f);
        const float pu = u[seg] + (u[seg + 1] - u[seg]) * t, pv = vv[seg] + (vv[seg + 1] - vv[seg]) * t;
        const float tu = (u[seg + 1] - u[seg]) / l, tv = (vv[seg + 1] - vv[seg]) / l;
        float du = 0, dv = 0, conf = 0;
        if (!fine->sample(pu * fx, pv * fy, du, dv, conf) || conf < 0.05f) continue;
        float angle = pm::atan2(tu * dv - tv * du, tu * du + tv * dv);
        if (angle > 0.5f * kPi) angle -= kPi;
        if (angle < -0.5f * kPi) angle += kPi;
        a.push_back(angle);
        at.push_back(s);
    }
    if (static_cast<float>(a.size()) < 1.2f * period0) return fit;
    double mean = 0.0;
    for (float x : a) mean += x;
    mean /= static_cast<double>(a.size());
    double var = 0.0;
    for (float& x : a) {
        x -= static_cast<float>(mean);
        var += static_cast<double>(x) * x;
    }
    var /= static_cast<double>(a.size());
    if (var < 1e-6) return fit;
    double bestPower = -1.0, bestRe = 0.0, bestIm = 0.0;
    float bestPeriod = period0;
    for (float period = 0.6f * period0; period <= 1.6f * period0; period += std::max(0.5f, 0.04f * period0)) {
        if (pixels < 1.2f * period) break;
        const double w = 2.0 * kPi / period;
        double re = 0.0, im = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k) {
            re += a[k] * pm::cos(static_cast<float>(w * at[k]));
            im -= a[k] * pm::sin(static_cast<float>(w * at[k]));
        }
        const double power = re * re + im * im;
        if (power > bestPower) bestPower = power, bestRe = re, bestIm = im, bestPeriod = period;
    }
    const double count = static_cast<double>(a.size());
    // The swing summed along the stretch (a pixel a sample; gaps where the field is unsure carry the
    // last swing on), each turn's mean taken out.
    {
        std::vector<float> raw(static_cast<std::size_t>(std::floor(pixels)) + 1, 0.0f);
        std::size_t k = 0;
        float swing = 0.0f, side = 0.0f;
        for (std::size_t x = 1; x < raw.size(); ++x) {
            while (k < at.size() && at[k] <= static_cast<float>(x)) swing = a[k++];
            side += std::tan(std::clamp(swing, -1.05f, 1.05f));
            raw[x] = side;
        }
        const int half = std::max(1, static_cast<int>(0.5f * bestPeriod));
        fit.offset.resize(raw.size());
        for (std::size_t x = 0; x < raw.size(); ++x) {
            const std::size_t lo = x > static_cast<std::size_t>(half) ? x - static_cast<std::size_t>(half) : 0;
            const std::size_t hi = std::min(raw.size() - 1, x + static_cast<std::size_t>(half));
            double m = 0.0;
            for (std::size_t y = lo; y <= hi; ++y) m += raw[y];
            fit.offset[x] = raw[x] - static_cast<float>(m / static_cast<double>(hi - lo + 1));
        }
    }
    fit.periodPx = bestPeriod;
    fit.amplitude = static_cast<float>(2.0 * std::sqrt(std::max(bestPower, 0.0)) / count);
    // (the lock's own path at most half again as far off the guide as the fitted wave swings: what
    // the summed swing drifts past that is the field's bias, not the lock)
    {
        const float reach = 1.5f * std::tan(std::min(fit.amplitude, 1.2f)) * bestPeriod / (2.0f * kPi);
        for (float& o : fit.offset) o = std::clamp(o, -reach, reach);
    }
    fit.phase = static_cast<float>(std::atan2(bestIm, bestRe));
    fit.quality = static_cast<float>(std::min(1.0, 0.5 * fit.amplitude * fit.amplitude / var));
    double depth = 0.0;
    for (std::size_t i = best0; i <= best1; ++i) depth += z[i];
    depth /= static_cast<double>(best1 - best0 + 1);
    fit.pxPerUnit = static_cast<float>(v.camera.focalPx / std::max(depth, 1e-6));
    fit.pxPerLength = pixels / (length[best1] - length[best0]);
    fit.first = best0;
    fit.last = best1;
    fit.s.assign(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        if (i < best0) fit.s[i] = -(length[best0] - length[i]) * fit.pxPerLength;
        else if (i > best1) fit.s[i] = pixels + (length[i] - length[best1]) * fit.pxPerLength;
        else fit.s[i] = along[i];
    }
    fit.score = fit.quality * pixels / bestPeriod;
    fit.ok = true;
    return fit;
}

// The curl baked into a guide: each point moved `side[i]` along the direction the swing shows there
// (`across(i)`, the traced view's sideways) and `depth[i]` along the guide's way crossed with it
// (the helix's other half), growing in over the first half turn from the root (the root holds).
std::vector<Vec3f> bakedCurl(const std::vector<Vec3f>& path, float wavelength, const std::vector<float>& side,
                             const std::vector<float>& depth, const std::function<Vec3f(std::size_t, const Vec3f&)>& across) {
    std::vector<Vec3f> out = path;
    const std::size_t n = path.size();
    float length = 0.0f;
    Vec3f last(0, 0, 0);
    for (std::size_t i = 1; i < n; ++i) {
        length += (path[i] - path[i - 1]).length();
        const Vec3f t = (path[std::min(n - 1, i + 1)] - path[i - 1]).normalized();
        Vec3f a = across(i, t);
        a = a - t * a.dot(t);
        if (a.length() < 1e-6f) a = last;
        if (a.length() < 1e-6f) continue;
        a = a.normalized();
        // (the same side all along: where the guide's projection turns back, its sideways would flip)
        if (last.length() > 0.5f && a.dot(last) < 0.0f) a = -a;
        last = a;
        const Vec3f b = t.cross(a);
        const float grow = std::clamp(length / std::max(1e-6f, 0.5f * wavelength), 0.0f, 1.0f);
        const float k = grow * grow * (3.0f - 2.0f * grow);
        out[i] = path[i] + (a * side[i] + b * depth[i]) * k;
    }
    return out;
}

// The polyline's first `length` units.
std::vector<Vec3f> truncated(const std::vector<Vec3f>& pts, float length) {
    std::vector<Vec3f> out;
    if (pts.empty()) return out;
    out.push_back(pts[0]);
    float before = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float seg = (pts[i] - pts[i - 1]).length();
        if (before + seg >= length) {
            const float f = seg > 1e-12f ? (length - before) / seg : 0.0f;
            out.push_back(pts[i - 1] + (pts[i] - pts[i - 1]) * f);
            return out;
        }
        out.push_back(pts[i]);
        before += seg;
    }
    return out;
}

std::vector<Vec3f> smoothed(std::vector<Vec3f> pts, int iterations) {
    for (int it = 0; it < iterations; ++it) {
        std::vector<Vec3f> next = pts;
        for (std::size_t i = 2; i + 1 < pts.size(); ++i) next[i] = pts[i] * 0.5f + (pts[i - 1] + pts[i + 1]) * 0.25f;
        pts.swap(next);
    }
    return pts;
}

// The melanin and redness whose appearance is nearest `appearance` (log RGB); the RMS error.
// The appearance's highlight (kAppearanceSheen, measured under an even white dome) is fitted too, up
// to the dome's: a photo's soft or frontal light shows dark hair less of it, so its dark brown is
// darker and redder than any appearance with the whole highlight (it would read a medium brown).
float nearestMelanin(const std::array<float, 3>& appearance, float& melanin, float& redness) {
    double best = 1e30;
    constexpr int kSheenSteps = 12;
    for (int mi = 0; mi <= 50; ++mi)
        for (int ri = 0; ri <= 20; ++ri) {
            const float m = static_cast<float>(mi) / 50.0f, r = static_cast<float>(ri) / 20.0f;
            const auto a = hairAppearanceColor(melaninColor(m, r));
            for (int si = kSheenSteps; si >= 0; --si) {
                const float sheen = kAppearanceSheen * static_cast<float>(si) / kSheenSteps;
                double err = 0.0;
                for (int c = 0; c < 3; ++c) {
                    const double d = pm::log(std::max(a[static_cast<std::size_t>(c)] - kAppearanceSheen + sheen, 1e-4f)) -
                                     pm::log(std::max(appearance[static_cast<std::size_t>(c)], 1e-4f));
                    err += d * d;
                }
                if (err < best - 1e-12) {
                    best = err;
                    melanin = m;
                    redness = r;
                }
            }
        }
    return static_cast<float>(std::sqrt(best / 3.0));
}

// The look from the masked pixels and along the guides.
ReferenceLookFit fitLook(const std::vector<PreparedView>& views, const std::vector<ReferenceGuide>& guides,
                         const ReferenceGuideOptions& options) {
    ReferenceLookFit fit;
    std::vector<float> ch[3], luminance;
    for (const PreparedView& v : views) {
        if (v.rgb.empty() || v.rgb.channels < 3) continue;
        const RefImage& core = v.mask;  // eroded by a pixel: no backdrop blended in
        const std::size_t total = static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height);
        const std::size_t stride = std::max<std::size_t>(1, total / 200000);
        for (std::size_t i = 0; i < total; i += stride) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(v.width)), y = static_cast<int>(i / static_cast<std::size_t>(v.width));
            if (x < 1 || y < 1 || x + 1 >= v.width || y + 1 >= v.height) continue;
            if (core.at(x, y) < 0.5f || core.at(x - 1, y) < 0.5f || core.at(x + 1, y) < 0.5f || core.at(x, y - 1) < 0.5f ||
                core.at(x, y + 1) < 0.5f)
                continue;
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = srgbToLinear01(v.rgb.at(x, y, c));
                ch[c].push_back(rgb[c]);
            }
            luminance.push_back(rec709Luminance(rgb[0], rgb[1], rgb[2]));
        }
    }
    fit.samples = static_cast<int>(luminance.size());
    if (luminance.empty()) return fit;
    fit.ok = true;
    // The light's color, divided out keeping the brightness (a photo under a blue sky):
    //  - LookBalance::Gray: shades of gray (Finlayson & Trezzi 2004, p = 6) over the whole photo,
    //    what most of a scene reflects being neutral on average (the backdrop lit by the same sky);
    //  - LookBalance::Highlights: the hair's brightest pixels taken as the light's color.
    //  - LookBalance::Auto: the highlights' color when it is near neutral (a specular highlight shows
    //    the light's color: the light was tinted), none when it is colored (the hair's own color lit
    //    from behind: the light was white).
    float light[3] = {1.0f, 1.0f, 1.0f};
    if (options.lookBalance == LookBalance::Highlights || options.lookBalance == LookBalance::Auto) {
        // The brightest 2% of the hair that is not clipped (a clipped highlight lost its color), their
        // median color.
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < luminance.size(); ++i)
            if (std::max({ch[0][i], ch[1][i], ch[2][i]}) < 0.9f) order.push_back(i);
        if (order.empty())
            for (std::size_t i = 0; i < luminance.size(); ++i) order.push_back(i);
        const std::size_t top = std::max<std::size_t>(1, order.size() / 50);
        std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                         [&](std::size_t a, std::size_t b) { return luminance[a] > luminance[b]; });
        for (int c = 0; c < 3; ++c) {
            std::vector<float> v;
            for (std::size_t k = 0; k < top; ++k) v.push_back(ch[c][order[k]]);
            light[c] = std::max(median(v), 1e-4f);
        }
        const float lo = std::min({light[0], light[1], light[2]}), hi = std::max({light[0], light[1], light[2]});
        if (options.lookBalance == LookBalance::Auto && hi > 1.6f * lo) light[0] = light[1] = light[2] = 1.0f;
    } else if (options.lookBalance == LookBalance::Gray) {
        double sum[3] = {0, 0, 0};
        std::size_t n = 0;
        for (const PreparedView& v : views) {
            if (v.rgb.empty() || v.rgb.channels < 3) continue;
            const std::size_t total = static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height);
            const std::size_t stride = std::max<std::size_t>(1, total / 200000);
            for (std::size_t i = 0; i < total; i += stride) {
                float rgb[3];
                float lo = 1.0f, hi = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    rgb[c] = srgbToLinear01(v.rgb.pixels[i * 3 + static_cast<std::size_t>(c)]);
                    lo = std::min(lo, rgb[c]);
                    hi = std::max(hi, rgb[c]);
                }
                if (hi < 0.01f || hi > 0.98f) continue;  // black, or clipped
                for (int c = 0; c < 3; ++c) sum[c] += pm::pow(static_cast<double>(rgb[c]), 6.0);
                ++n;
            }
        }
        if (n)
            for (int c = 0; c < 3; ++c) light[c] = static_cast<float>(pm::pow(sum[c] / static_cast<double>(n), 1.0 / 6.0));
    }
    const float gray = (light[0] + light[1] + light[2]) / 3.0f;
    for (int c = 0; c < 3; ++c) fit.whiteBalance[static_cast<std::size_t>(c)] = gray / std::max(light[c], 1e-4f);
    // The exposure: the photo's white point (its brightest 0.5%, balanced) lifted to kPhotoWhite when
    // the photo does not reach it (a dim or underexposed shot); never darker.
    if (options.lookExposure) {
        std::vector<float> lum;
        for (const PreparedView& v : views) {
            if (v.rgb.empty() || v.rgb.channels < 3) continue;
            const std::size_t total = static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height);
            const std::size_t stride = std::max<std::size_t>(1, total / 200000);
            for (std::size_t i = 0; i < total; i += stride) {
                float l = 0.0f;
                const float k[3] = {0.2126f, 0.7152f, 0.0722f};
                for (int c = 0; c < 3; ++c)
                    l += k[c] * srgbToLinear01(v.rgb.pixels[i * 3 + static_cast<std::size_t>(c)]) * fit.whiteBalance[static_cast<std::size_t>(c)];
                lum.push_back(l);
            }
        }
        if (!lum.empty()) {
            const std::size_t at = static_cast<std::size_t>(0.995 * static_cast<double>(lum.size() - 1));
            std::nth_element(lum.begin(), lum.begin() + static_cast<std::ptrdiff_t>(at), lum.end());
            fit.exposure = std::clamp(kPhotoWhite / std::max(lum[at], 1e-4f), 1.0f, 4.0f);
        }
    }
    for (int c = 0; c < 3; ++c) fit.whiteBalance[static_cast<std::size_t>(c)] *= fit.exposure;
    for (int c = 0; c < 3; ++c) {
        std::vector<float> v = ch[c];
        const std::size_t at = static_cast<std::size_t>(options.lookQuantile * static_cast<double>(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
        fit.appearance[static_cast<std::size_t>(c)] = std::clamp(v[at] * fit.whiteBalance[static_cast<std::size_t>(c)], 1e-4f, 1.0f);
    }
    // Hair's pigments never make it bluer than red (melanin: red >= green >= blue). Hair that still
    // looks it was lit by a tinted light the highlights could not tell (fur under a blue sky): its
    // own color is taken as neutral.
    if (options.lookBalance == LookBalance::Auto && fit.appearance[2] > 1.05f * fit.appearance[0]) {
        const float mean = (fit.appearance[0] + fit.appearance[1] + fit.appearance[2]) / 3.0f;
        for (int c = 0; c < 3; ++c) {
            const float gain = mean / std::max(fit.appearance[static_cast<std::size_t>(c)], 1e-4f);
            fit.whiteBalance[static_cast<std::size_t>(c)] *= gain;
            fit.appearance[static_cast<std::size_t>(c)] = mean;
        }
    }
    double mean = 0.0, sq = 0.0;
    for (float l : luminance) {
        mean += l;
        sq += static_cast<double>(l) * l;
    }
    mean /= static_cast<double>(luminance.size());
    const double var = std::max(0.0, sq / static_cast<double>(luminance.size()) - mean * mean);
    fit.valueRandomness = mean > 1e-6 ? static_cast<float>(std::sqrt(var) / mean) : 0.0f;
    fit.baseColor = hairBaseOfAppearance(fit.appearance);
    // The melanin and redness whose appearance is nearest (log RGB).
    fit.melaninError = nearestMelanin(fit.appearance, fit.melanin, fit.redness);
    // Roots and tips: the pixels under the guides' first and last thirds, where a view sees them.
    std::vector<float> root[3], tip[3];
    for (const ReferenceGuide& g : guides) {
        const std::size_t n = g.points.size();
        for (std::size_t i = 1; i < n; ++i) {
            const float s = static_cast<float>(i) / static_cast<float>(n - 1);
            if (s > 1.0f / 3.0f && s < 2.0f / 3.0f) continue;
            for (const PreparedView& v : views) {
                if (v.rgb.empty() || v.rgb.channels < 3) continue;
                float u = 0, vv = 0, z = 0;
                if (!v.camera.project(g.points[i], u, vv, &z) || !v.inFrame(u, vv) || v.hidden(u, vv, z) || v.mask.sample(u, vv) < 0.5f) continue;
                for (int c = 0; c < 3; ++c) (s <= 1.0f / 3.0f ? root : tip)[c].push_back(srgbToLinear01(v.rgb.sample(u, vv, c)));
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        fit.rootAppearance[static_cast<std::size_t>(c)] = median(root[c]) * fit.whiteBalance[static_cast<std::size_t>(c)];
        fit.tipAppearance[static_cast<std::size_t>(c)] = median(tip[c]) * fit.whiteBalance[static_cast<std::size_t>(c)];
    }
    // Tips lighter or darker than the roots (sun-bleached, dyed, agouti fur): with a tip color
    // along the length (HairColor's linear tipProfile) the first third averages a sixth of the way
    // to it and the last third five sixths. So the color at the root B = (5 R - P) / 4 and the tip
    // color T = B + (P - B) * 6 / 5 (R, P the thirds' colors), at full strength; the melanin is
    // then the roots' color's.
    if (!root[0].empty() && !tip[0].empty()) {
        auto lum = [](const std::array<float, 3>& c) { return rec709Luminance(c[0], c[1], c[2]); };
        const float lr = lum(fit.rootAppearance), lt = lum(fit.tipAppearance);
        if (lr > 1e-4f && lt > 1e-4f && std::abs(pm::log(lt / lr)) > kTipThreshold) {
            // (In base colors: what HairColor blends.)
            const std::array<float, 3> rb = hairBaseOfAppearance(fit.rootAppearance), pb = hairBaseOfAppearance(fit.tipAppearance);
            std::array<float, 3> base{}, tipColor{};
            for (std::size_t c = 0; c < 3; ++c) {
                base[c] = std::clamp((5.0f * rb[c] - pb[c]) / 4.0f, 1e-4f, 1.0f);
                tipColor[c] = std::clamp(base[c] + (pb[c] - base[c]) * 1.2f, 1e-4f, 1.0f);
            }
            fit.tipStrength = std::clamp(std::abs(pm::log(lt / lr)) / kTipFull, 0.05f, 1.0f);
            // At a strength under 1 the tips go part of the way: the color reached is B + s (T - B), so
            // T itself sits further out.
            for (std::size_t c = 0; c < 3; ++c)
                fit.tipColor[c] = std::clamp(base[c] + (tipColor[c] - base[c]) / fit.tipStrength, 0.0f, 1.0f);
            // (no darker than the hair's whole color reads by more than a little: long guides' roots lie
            // in the hair's own shadow, which is no darker pigment)
            const float overall = fit.melanin;
            fit.melaninError = nearestMelanin(hairAppearanceColor(base), fit.melanin, fit.redness);
            fit.melanin = std::min(fit.melanin, overall + 0.1f);
        }
    }
    return fit;
}

// The measured look (6-10) from every view's photo: clumps, color spread, flyaways and frizz
// (reference_look.cpp's measures, in scene units at the scalp's depth), turned into graph values.
// The values' calibration: the synthetic look sweep (tools/research/reference_guides/look_sweep.py).
void measureLook(const std::vector<PreparedView>& views, float maxLength, bool shortHair, ReferenceLookFit& fit) {
    std::vector<float> spacing, contrast, regular, periodic, spread, coherent, strays, cross, backdrop;
    for (const PreparedView& v : views) {
        if (v.rgb.empty() || v.rgb.channels < 3 || !v.field || !v.coarse || v.scalpDepth <= 0.0f) continue;
        const float unit = v.scalpDepth / static_cast<float>(v.camera.focalPx);  // scene units a pixel
        RefImage luminance;
        luminance.width = v.rgb.width;
        luminance.height = v.rgb.height;
        luminance.channels = 1;
        luminance.pixels.resize(static_cast<std::size_t>(v.rgb.width) * static_cast<std::size_t>(v.rgb.height));
        for (std::size_t i = 0; i < luminance.pixels.size(); ++i)
            luminance.pixels[i] = rec709Luminance(srgbToLinear01(v.rgb.pixels[3 * i]), srgbToLinear01(v.rgb.pixels[3 * i + 1]),
                                                  srgbToLinear01(v.rgb.pixels[3 * i + 2]));
        const ClumpEstimate c = measureClumps(luminance, *v.coarse, v.mask);
        if (c.profiles >= 10) {
            periodic.push_back(c.periodicShare);
            contrast.push_back(c.contrast);
            spread.push_back(c.strandSpread);
            coherent.push_back(c.coherentSpread);
            if (c.spacingPx > 0.0f) {
                spacing.push_back(c.spacingPx * unit);
                regular.push_back(c.clumpPeak);
            }
        }
        // Strays within the hair's reach of the scalp.
        RefImage near;
        near.width = v.width;
        near.height = v.height;
        near.channels = 1;
        near.pixels.resize(static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height));
        const float reachPx = 1.15f * maxLength / unit;
        // (Against the backdrop only: not over the scalp, the face or another occluder.)
        for (std::size_t i = 0; i < near.pixels.size(); ++i) near.pixels[i] = v.scalpDistance[i] <= reachPx && v.depth[i] == kInf ? 1.0f : 0.0f;
        const StrayEstimate s = measureStrays(v.rgb.gray(), *v.coarse, v.mask, near);
        if (s.outlinePx > 0.0f) {
            strays.push_back(s.strayDensity);
            cross.push_back(s.crossShare);
            backdrop.push_back(s.contrast);
        }
    }
    if (!contrast.empty()) {
        fit.clumpContrast = median(contrast);
        fit.strandSpread = median(spread);
        fit.coherentSpread = median(coherent);
        fit.clumpRegularity = median(regular);
        fit.clumpSpacing = median(spacing);
        // (Short fur: its profiles read the fur's pattern over the body, not its tufts: not suggested.)
        fit.clumped = median(periodic) > kClumpPeriodic && fit.clumpSpacing > 0.0f && !shortHair;
        if (fit.clumped) {
            const float curl = fit.curly ? fit.curlRadius : 0.0f;
            const float seen = std::max(fit.clumpSpacing - kClumpCurlWiden * curl, 0.5f * fit.clumpSpacing);
            fit.clumpSize = std::clamp((seen - kClumpOffset) / kClumpSlope, 0.3f * seen, 2.0f * seen);
            const float swing = fit.clumpContrast / (1.0f + kClumpCurlSwing * curl / fit.clumpSpacing);
            fit.clumpStrength = std::clamp(kClumpStrength0 + kClumpStrength1 * swing, 0.1f, 1.0f);
        }
        // (The strands' own color randomness does not show apart from their shading at a photo's
        // usual resolution: measured, not suggested. See look_sweep.py.)
    }
    if (!strays.empty()) {
        fit.strayDensity = median(strays);
        fit.crossShare = median(cross);
        if (median(backdrop) >= kFlyawayContrast)
            fit.flyawayFraction = std::clamp(kFlyawayGain * (fit.strayDensity - kFlyawayFloor) * (fit.curly ? kFlyawayCurly : 1.0f), 0.0f, 0.3f);
        if (!fit.curly && !shortHair) fit.frizzFraction = std::clamp(kFrizzGain * (fit.crossShare - kFrizzFloor), 0.0f, 0.6f);
        if (fit.flyawayFraction < 0.01f) fit.flyawayFraction = 0.0f;
        if (fit.frizzFraction < 0.03f) fit.frizzFraction = 0.0f;
    }
}

// Each scalp vertex: the share of it the hair covers in the views that see it face on.
std::vector<float> scalpCoverage(const ScalpMesh& scalp, const std::vector<PreparedView>& views) {
    std::vector<float> out(scalp.pointCount(), -1.0f);
    const auto& normals = scalp.vertexNormals();
    parallelFor(0, scalp.pointCount(), [&](std::size_t i) {
        const Vec3f& p = scalp.points()[i];
        double sum = 0.0, weight = 0.0;
        for (const PreparedView& v : views) {
            float u = 0, vv = 0, z = 0;
            if (!v.camera.project(p, u, vv, &z) || !v.inFrame(u, vv) || v.hidden(u, vv, z)) continue;
            const float facing = normals[i].dot((v.camera.eye() - p).normalized());
            if (facing < 0.35f) continue;
            float cover = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int k = -1; k <= 1; ++k) cover += v.mask.sample(u + 1.5f * static_cast<float>(k), vv + 1.5f * static_cast<float>(j)) >= 0.5f ? 1.0f : 0.0f;
            sum += facing * cover / 9.0f;
            weight += facing;
        }
        if (weight > 0.0) out[i] = static_cast<float>(sum / weight);
    }, 64);
    return out;
}

}  // namespace

float melaninOfAppearance(const std::array<float, 3>& appearance, float& melanin, float& redness) {
    return nearestMelanin(appearance, melanin, redness);
}

ReferenceShell::ReferenceShell(const ScalpMesh& scalp, const std::vector<ReferenceView>& views, const ReferenceGuideOptions& options,
                               float maxLength) {
    const float size = scalpDiagonal(scalp);
    const std::vector<PreparedView> prepared = prepareViews(scalp, views, options, size);
    Context ctx{scalp, prepared, options, Mirror{}, size, maxLength, maxLength / 80.0f, options.scalpOffset > 0 ? options.scalpOffset : 0.004f * size,
                options.inflate > 0 || (options.inflate < 0 && prepared.size() == 1), nullptr};
    if (options.symmetry == ReferenceSymmetry::WorldX) {
        ctx.mirror.sym.axis = 0;
        ctx.mirror.sym.center = options.symmetryCenter;
    } else if (options.symmetry == ReferenceSymmetry::Topology) {
        ctx.mirror.sym.mirror = ScalpMirror::of(scalp);
        ctx.mirror.sym.scalp = &scalp;
    }
    ReferenceShellBuilder::build(*this, ctx);
}

// A polyline against the prepared views (PhotoScorer, reference_guides.h): per view its samples a
// step apart in the picture, those the head hides left out, the share in the hair mask and, inside
// it, 1 - the pi-periodic angle error / 90 degrees against the coarse and fine fields, weighted by
// their confidence. `skip`: a view left out (-1 none).
static GuideScore scoreOnViews(const std::vector<PreparedView>& views, const std::vector<Vec3f>& path, const GuideScoreOptions& o, int skip = -1) {
    GuideScore out;
    out.photos.resize(views.size());
    double total = 0.0, orientation = 0.0, mask = 0.0, coarseSum = 0.0, fineSum = 0.0, weight = 0.0;
    const float step = std::max(0.25f, o.stepPx);
    for (std::size_t k = 0; k < views.size(); ++k) {
        if (static_cast<int>(k) == skip) continue;
        const PreparedView& v = views[k];
        GuidePhotoScore& ps = out.photos[k];
        const OrientationField* coarse = v.coarse ? v.coarse.get() : v.field.get();
        const OrientationField* fine = v.fine ? v.fine.get() : v.field.get();
        if (!coarse || !fine || v.mask.empty()) continue;
        const float cx = static_cast<float>(coarse->width) / static_cast<float>(v.width), cy = static_cast<float>(coarse->height) / static_cast<float>(v.height);
        const float fx = static_cast<float>(fine->width) / static_cast<float>(v.width), fy = static_cast<float>(fine->height) / static_cast<float>(v.height);
        int inMask = 0;
        double cS = 0.0, cW = 0.0, cE = 0.0, fS = 0.0, fW = 0.0, fE = 0.0, support = 0.0;
        for (std::size_t i = 1; i < path.size(); ++i) {
            float u0 = 0, v0 = 0, z0 = 0, u1 = 0, v1 = 0, z1 = 0;
            if (!v.camera.project(path[i - 1], u0, v0, &z0) || !v.camera.project(path[i], u1, v1, &z1)) continue;
            const float du = u1 - u0, dv = v1 - v0, l = std::hypot(du, dv);
            if (l < 1e-6f) continue;
            const float tu = du / l, tv = dv / l;
            const int n = std::max(1, static_cast<int>(std::ceil(l / step)));
            for (int j = 0; j < n; ++j) {
                const float t = (static_cast<float>(j) + 0.5f) / static_cast<float>(n);
                const float u = u0 + du * t, w = v0 + dv * t, z = z0 + (z1 - z0) * t;
                if (!v.inFrame(u, w)) continue;
                if (v.hidden(u, w, z)) {
                    ++ps.hidden;
                    continue;
                }
                ++ps.samples;
                if (v.mask.sample(u, w) < 0.5f) {
                    support -= 1.0;
                    continue;
                }
                ++inMask;
                float ou = 0, ow = 0, c = 0;
                float sc = 0.5f, sf = 0.5f;  // (this sample's, unsure where the photo says nothing)
                if (coarse->sample(u * cx, w * cy, ou, ow, c) && c > 0.0f) {
                    const float e = std::acos(std::clamp(std::abs(tu * ou + tv * ow), 0.0f, 1.0f));
                    cS += c * (1.0f - e / (0.5f * kPi));
                    cE += c * e;
                    cW += c;
                    sc = 0.5f + std::min(1.0f, c) * (0.5f - e / (0.5f * kPi));
                }
                if (fine->sample(u * fx, w * fy, ou, ow, c) && c > 0.0f) {
                    const float e = std::acos(std::clamp(std::abs(tu * ou + tv * ow), 0.0f, 1.0f));
                    fS += c * (1.0f - e / (0.5f * kPi));
                    fE += c * e;
                    fW += c;
                    sf = 0.5f + std::min(1.0f, c) * (0.5f - e / (0.5f * kPi));
                }
                support += 2.0 * (o.coarseShare * sc + (1.0f - o.coarseShare) * sf) - 1.0;
            }
        }
        if (ps.samples == 0) continue;
        ps.mask = static_cast<float>(inMask) / static_cast<float>(ps.samples);
        ps.coarse = cW > 0.0 ? static_cast<float>(cS / cW) : 0.0f;
        ps.fine = fW > 0.0 ? static_cast<float>(fS / fW) : 0.0f;
        ps.coarseError = cW > 0.0 ? static_cast<float>(cE / cW * 180.0 / kPi) : 90.0f;
        ps.fineError = fW > 0.0 ? static_cast<float>(fE / fW * 180.0 / kPi) : 90.0f;
        ps.confidence = inMask ? static_cast<float>(0.5 * (cW + fW) / inMask) : 0.0f;
        const float orient = o.coarseShare * ps.coarse + (1.0f - o.coarseShare) * ps.fine;
        ps.score = orient * ps.mask;
        ps.support = static_cast<float>(support * step);
        out.support += ps.support;
        ++out.views;
        const double wk = static_cast<double>(ps.samples);
        total += wk * ps.score;
        orientation += wk * orient;
        mask += wk * ps.mask;
        coarseSum += wk * ps.coarse;
        fineSum += wk * ps.fine;
        weight += wk;
    }
    if (weight > 0.0) {
        out.score = static_cast<float>(total / weight);
        out.orientation = static_cast<float>(orientation / weight);
        out.mask = static_cast<float>(mask / weight);
        out.coarse = static_cast<float>(coarseSum / weight);
        out.fine = static_cast<float>(fineSum / weight);
    }
    return out;
}

// How well a guide's way agrees with the photos' fine strand directions where they see it in the
// hair (the mean of cos 2x the angle between them, 1 along).
float fineAgreement(const std::vector<PreparedView>& views, const std::vector<Vec3f>& path) {
    double sum = 0.0;
    int count = 0;
    for (const PreparedView& v : views) {
        const OrientationField* fine = v.fine ? v.fine.get() : v.field.get();
        if (!fine) continue;
        const float fx = static_cast<float>(fine->width) / static_cast<float>(v.width), fy = static_cast<float>(fine->height) / static_cast<float>(v.height);
        for (std::size_t i = 1; i < path.size(); ++i) {
            float u0 = 0, v0 = 0, z0 = 0, u1 = 0, v1 = 0;
            if (!v.camera.project(path[i - 1], u0, v0, &z0) || !v.camera.project(path[i], u1, v1)) continue;
            const float mu = 0.5f * (u0 + u1), mv = 0.5f * (v0 + v1);
            if (!v.inFrame(mu, mv) || v.hidden(mu, mv, z0) || v.mask.sample(mu, mv) < 0.5f) continue;
            const float du = u1 - u0, dv = v1 - v0, l = std::hypot(du, dv);
            if (l < 0.3f) continue;
            float fu = 0, fv = 0, conf = 0;
            if (!fine->sample(mu * fx, mv * fy, fu, fv, conf) || conf < 0.05f) continue;
            const float c = (du * fu + dv * fv) / l;
            sum += 2.0 * c * c - 1.0;
            ++count;
        }
    }
    return count ? static_cast<float>(sum / count) : 0.0f;
}

// Each guide's lock: traced where a view sees it broadside (the best view's fit), its curl baked
// in; the rest take their traced neighbours' curl (radius, turn length, tightness; their own phase).
// Locks seen in a view: streamlines along the locks' coarse flow (seeded half a curl period apart
// in the hair), each with its strands' swing about it fitted with one wave; kept where the wave is
// clear, the clearest first, none within half a period of a lock kept already. Each lock's root end:
// the end nearer the scalp's outline (the higher one when both are over it).
struct Lock2D {
    std::vector<std::array<float, 2>> line;  // pixels, root end first
    float periodPx = 0.0f, amplitude = 0.0f, quality = 0.0f;
};

std::vector<Lock2D> detectLocks(const PreparedView& v) {
    std::vector<Lock2D> out;
    const OrientationField* coarse = v.coarse ? v.coarse.get() : nullptr;
    const OrientationField* fine = v.fine ? v.fine.get() : nullptr;
    const float period0 = v.curl.wavelengthPx;
    if (!coarse || !fine || period0 <= 0.0f) return out;
    const float cx = static_cast<float>(coarse->width) / static_cast<float>(v.width), cy = static_cast<float>(coarse->height) / static_cast<float>(v.height);
    const float fx = static_cast<float>(fine->width) / static_cast<float>(v.width), fy = static_cast<float>(fine->height) / static_cast<float>(v.height);
    auto inHair = [&](float u, float w) { return v.inFrame(u, w) && v.mask.sample(u, w) >= 0.5f; };
    const int spacing = std::max(4, static_cast<int>(0.5f * period0));
    struct Candidate {
        Lock2D lock;
        float score;
    };
    std::vector<Candidate> found;
    for (int sy = spacing / 2; sy < v.height; sy += spacing)
        for (int sx = spacing / 2; sx < v.width; sx += spacing) {
            const float u0 = static_cast<float>(sx) + 0.5f, v0 = static_cast<float>(sy) + 0.5f;
            if (!inHair(u0, v0)) continue;
            std::vector<std::array<float, 2>> half[2];
            std::vector<float> swing[2];
            for (int dir = 0; dir < 2; ++dir) {
                float u = u0, w = v0, pu = 0, pv = 0;
                bool first = true;
                for (int k = 0; k < 160; ++k) {
                    float du = 0, dv = 0, c = 0, gu = 0, gv = 0, gc = 0;
                    if (!inHair(u, w) || !coarse->sample(u * cx, w * cy, du, dv, c) || c < 0.05f || !fine->sample(u * fx, w * fy, gu, gv, gc)) break;
                    if (first) {
                        pu = dir ? -du : du;
                        pv = dir ? -dv : dv;
                        first = false;
                    }
                    if (du * pu + dv * pv < 0.0f) du = -du, dv = -dv;
                    pu = du;
                    pv = dv;
                    float a = pm::atan2(du * gv - dv * gu, du * gu + dv * gv);
                    if (a > 0.5f * kPi) a -= kPi;
                    if (a < -0.5f * kPi) a += kPi;
                    // (along the line's way root-to-tip later: the backward half's swing turns sign)
                    half[dir].push_back({u, w});
                    swing[dir].push_back(dir ? -a : a);
                    u += du;
                    w += dv;
                }
            }
            std::vector<std::array<float, 2>> line(half[1].rbegin(), half[1].rend());
            std::vector<float> a(swing[1].rbegin(), swing[1].rend());
            if (!line.empty()) {
                line.pop_back();
                a.pop_back();
            }
            line.insert(line.end(), half[0].begin(), half[0].end());
            a.insert(a.end(), swing[0].begin(), swing[0].end());
            if (static_cast<float>(line.size()) < 1.5f * period0) continue;
            double mean = 0.0, var = 0.0;
            for (float x : a) mean += x;
            mean /= static_cast<double>(a.size());
            for (float& x : a) {
                x -= static_cast<float>(mean);
                var += static_cast<double>(x) * x;
            }
            var /= static_cast<double>(a.size());
            if (var < 1e-6) continue;
            double bestPower = -1.0;
            float bestPeriod = period0;
            for (float period = 0.6f * period0; period <= 1.6f * period0; period += std::max(0.5f, 0.05f * period0)) {
                if (static_cast<float>(a.size()) < 1.2f * period) break;
                const double w = 2.0 * kPi / period;
                double re = 0.0, im = 0.0;
                for (std::size_t k = 0; k < a.size(); ++k) {
                    re += a[k] * pm::cos(static_cast<float>(w * static_cast<double>(k)));
                    im -= a[k] * pm::sin(static_cast<float>(w * static_cast<double>(k)));
                }
                const double power = re * re + im * im;
                if (power > bestPower) bestPower = power, bestPeriod = period;
            }
            Lock2D lock;
            lock.amplitude = static_cast<float>(2.0 * std::sqrt(std::max(0.0, bestPower)) / static_cast<double>(a.size()));
            lock.quality = static_cast<float>(std::min(1.0, 0.5 * lock.amplitude * lock.amplitude / var));
            lock.periodPx = bestPeriod;
            if (lock.quality < 0.25f || lock.amplitude * 180.0f / kPi < 8.0f) continue;
            // The root end: nearer the scalp's outline, else higher in the picture.
            const auto& f = line.front();
            const auto& b = line.back();
            const float df = v.scalpDistance[v.pixel(f[0], f[1])], db = v.scalpDistance[v.pixel(b[0], b[1])];
            if (db < df || (db == df && b[1] < f[1])) std::reverse(line.begin(), line.end());
            lock.line = std::move(line);
            found.push_back({std::move(lock), 0.0f});
            found.back().score = found.back().lock.quality * static_cast<float>(found.back().lock.line.size()) / bestPeriod;
        }
    std::sort(found.begin(), found.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    for (Candidate& c : found) {
        bool apart = true;
        for (const Lock2D& kept : out) {
            double sum = 0.0;
            for (const auto& p : c.lock.line) {
                float best = 1e9f;
                for (std::size_t k = 0; k < kept.line.size(); k += 2) best = std::min(best, std::hypot(p[0] - kept.line[k][0], p[1] - kept.line[k][1]));
                sum += best;
            }
            if (sum / static_cast<double>(c.lock.line.size()) < 0.5 * c.lock.periodPx) {
                apart = false;
                break;
            }
        }
        if (apart) out.push_back(std::move(c.lock));
    }
    return out;
}

// Where the locks the views show grow from: each lock's root end's ray onto the scalp, facing the
// view; none nearer `minSpacing` than one found already (the clearer views' first).
std::vector<SurfacePoint> lockRoots(const std::vector<PreparedView>& views, const ScalpMesh& scalp, float minSpacing, int* detected) {
    std::vector<SurfacePoint> out;
    int count = 0;
    for (const PreparedView& v : views) {
        const std::vector<Lock2D> locks = detectLocks(v);
        count += static_cast<int>(locks.size());
        const Vec3f eye = v.camera.eye();
        for (const Lock2D& lock : locks) {
            const float u = lock.line.front()[0], w = lock.line.front()[1];
            const Vec3f dir = (v.camera.axis(0) * static_cast<float>((u - 0.5 * v.camera.width - v.camera.offsetU) / v.camera.focalPx) -
                               v.camera.axis(1) * static_cast<float>((w - 0.5 * v.camera.height - v.camera.offsetV) / v.camera.focalPx) -
                               v.camera.axis(2)).normalized();
            // (the ray onto the scalp, or past its outline the scalp's point nearest the ray on the
            // side facing the view: a lock hanging beside the head grows from its rim)
            SurfacePoint hit;
            if (!scalp.intersectRay(eye, dir, &hit) || hit.normal.dot(dir) > 0.0f) {
                float best = 1e30f;
                Vec3f nearest;
                for (const Vec3f& q : scalp.points()) {
                    const Vec3f d = q - eye;
                    const float along = d.dot(dir);
                    if (along <= 0.0f) continue;
                    const float off = (d - dir * along).length();
                    if (off < best) best = off, nearest = q;
                }
                if (best > 1e29f) continue;
                hit = scalp.closestPoint(nearest);
                if (hit.root.face < 0 || hit.normal.dot(dir) > 0.3f) continue;
            }
            bool apart = true;
            for (const SurfacePoint& r : out)
                if ((r.position - hit.position).length() < minSpacing) {
                    apart = false;
                    break;
                }
            if (apart) out.push_back(hit);
        }
    }
    if (detected) *detected = count;
    return out;
}

// The guide moved `shiftPx` sideways in view `v` (its projected way turned a quarter), its root slid
// back onto the scalp.
std::vector<Vec3f> movedSideways(const PreparedView& v, const std::vector<Vec3f>& path, float shiftPx, const ScalpMesh& scalp,
                                 StrandRoot* root) {
    // (all of it the same way: the view's sideways to the guide's way root to tip, at its depth)
    std::vector<Vec3f> moved = path;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0, z = 0;
    if (path.size() < 2 || !v.camera.project(path.front(), u0, v0, &z) || !v.camera.project(path.back(), u1, v1)) return moved;
    const float du = u1 - u0, dv = v1 - v0, l = std::hypot(du, dv);
    if (l < 1e-6f) return moved;
    const Vec3f side = v.camera.axis(0) * (-dv / l) - v.camera.axis(1) * (du / l);
    for (Vec3f& p : moved) p = p + side * (shiftPx * z / static_cast<float>(v.camera.focalPx));
    const SurfacePoint sp = scalp.closestPoint(moved[0]);
    if (sp.root.face >= 0) {
        const Vec3f lift = moved[0] - sp.position;
        for (Vec3f& p : moved) p = p - lift;
        moved[0] = sp.position;
        if (root) *root = sp.root;
    }
    return moved;
}

// A guide following the lock a view traced along it: the lock's own sideways path there (`gain`
// times what the photo shows), its other half a quarter turn on, turned by `hand` (a helix).
std::vector<Vec3f> tracedCurl(const PreparedView& v, const LockFit& f, const std::vector<Vec3f>& path, float gain, float hand) {
    const float wavelength = f.periodPx / f.pxPerLength;
    const auto across = [&](std::size_t k, const Vec3f& t) {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        if (!v.camera.project(path[k], u0, v0) || !v.camera.project(path[k] + t * (0.01f * wavelength), u1, v1)) return Vec3f(0, 0, 0);
        const float du = u1 - u0, dv = v1 - v0, l = std::hypot(du, dv);
        if (l < 1e-9f) return Vec3f(0, 0, 0);
        return v.camera.axis(0) * (-dv / l) - v.camera.axis(1) * (du / l);  // (pixels: v down)
    };
    std::vector<float> side(path.size()), depth(path.size());
    // (a curl at most as long again as the lock's middle: past that the fit read noise, scaled down)
    const float limit = 2.0f * arcLength(path);
    float k = gain / f.pxPerUnit;
    std::vector<Vec3f> out;
    for (int attempt = 0; attempt < 8; ++attempt, k *= 0.7f) {
        for (std::size_t j = 0; j < path.size(); ++j) {
            side[j] = k * f.sideways(f.s[j]);
            depth[j] = hand * k * f.sideways(f.s[j] + 0.25f * f.periodPx);
        }
        out = bakedCurl(path, wavelength, side, depth, across);
        if (arcLength(out) <= limit) break;
    }
    return out;
}

namespace {

// ---- Lock paths: locks as objects ------------------------------------------------------------------
//
// On wavy hair the fine directions follow each wave, not the lock: at a crest the strands run across
// the lock for a moment (the back of a head of loose curls reads as sideways bands). The locks' paths
// come from a coarse flow: the fine directions averaged over half a curl period, each lock traced from
// its root along it holding its own way (a lock bends slowly; a flow across its way is a crest, not a
// turn), the clearest lines kept as locks, all the lines laid into the view's coarse flow (what the
// grower and the lengths then read).

// The way a picture shows the world's down (`up` reversed), pixels (v down); {0, 1} when it looks
// straight down or up.
std::array<float, 2> pictureDown(const RefCamera& camera, const Vec3f& up) {
    const float du = -up.dot(camera.axis(0)), dv = up.dot(camera.axis(1)), l = std::hypot(du, dv);
    return l > 0.2f ? std::array<float, 2>{du / l, dv / l} : std::array<float, 2>{0.0f, 1.0f};
}

constexpr float kWeakFlow = 0.06f;    // a lock line's flow this unsure (its confidence) says nothing
// A lock centreline takes a root's place when the photos (but the one it was traced in) support it at
// least this share as much as the guide grown there (GuideScore::support: their agreeing length, less
// what runs across their flow or off their hair). The mean score alone chose short locks traced part
// way over long grown guides (the synthetic deep waves: the hair a fifth short); the support at 0.8:
// 11 locks there (a mean within 0.01: 11, 0.85 of the length), a six-photo test head 15 (mean rule 20, the
// flow agreement 0.370 against 0.377).
constexpr float kLockSupport = 0.8f;

// A lock's line from (u, w), leaving along (du, dw): a pixel a step along `flow`, its line taken the
// way the lock goes. The way is the lock's own: it turns at most a quarter turn over one and a half
// curl periods, a flow across it by more than 40 degrees says less and past 70 nothing (a curl's
// crest), and where the flow says little it falls a little toward the picture's `down` (long hair
// hangs). It ends where the hair does (the mask's edge, a gap of three pixels crossed), where it
// curls round on itself, or where it has run across a confident flow for a period while not falling
// (a line going sideways over locks that fall), or where the flow has been unsure for a period and a
// half (its confidence under kWeakFlow: the lock is lost; the unsure stretch is cut off).
LockLine traceLockLine(const PreparedView& v, const OrientationField& flow, float u, float w, float du, float dw, float period,
                       const std::array<float, 2>& down, int maxSteps) {
    LockLine out;
    const float fx = static_cast<float>(flow.width) / static_cast<float>(v.width), fy = static_cast<float>(flow.height) / static_cast<float>(v.height);
    const float maxTurn = 0.5f * kPi / (1.5f * period);
    const float cosFull = pm::cos(40.0f * kPi / 180.0f), cosNone = pm::cos(70.0f * kPi / 180.0f);
    float turned = 0.0f;
    int outside = 0, across = 0, weak = 0;
    out.line.push_back({u, w});
    for (int k = 0; k < maxSteps; ++k) {
        if (!v.inFrame(u, w)) break;
        if (v.mask.sample(u, w) < 0.5f) {
            if (++outside > 3) break;
        } else {
            outside = 0;
        }
        float ou = 0, ow = 0, c = 0, say = 0.0f;
        float wu = du, ww = dw;
        if (flow.sample(u * fx, w * fy, ou, ow, c)) {
            float cs = ou * du + ow * dw;
            if (cs < 0.0f) ou = -ou, ow = -ow, cs = -cs;
            const float gate = std::clamp((cs - cosNone) / (cosFull - cosNone), 0.0f, 1.0f);
            say = std::min(1.0f, c) * gate;
            wu += say * ou;
            ww += say * ow;
            // (running across a confident flow while not falling: not this lock's way)
            if (c > 0.25f && gate <= 0.0f && du * down[0] + dw * down[1] < 0.5f) ++across;
            else across = 0;
            // (where the flow is unsure for a period and a half running, the lock is lost: it ends)
            weak = c < kWeakFlow ? weak + 1 : 0;
        }
        wu += 0.15f * (1.0f - say) * down[0];
        ww += 0.15f * (1.0f - say) * down[1];
        const float angle = std::clamp(pm::atan2(du * ww - dw * wu, du * wu + dw * ww), -maxTurn, maxTurn);
        turned += std::abs(angle);
        if (turned > 0.9f * kPi) break;
        const float ca = pm::cos(angle), sa = pm::sin(angle);
        const float nu = ca * du - sa * dw, nw = sa * du + ca * dw;
        du = nu;
        dw = nw;
        u += du;
        w += dw;
        out.line.push_back({u, w});
        out.say += say;
        if (static_cast<float>(across) > period) {
            for (int t = 0; t < across && out.line.size() > 1; ++t) out.line.pop_back();
            break;
        }
        if (static_cast<float>(weak) > 1.5f * period) {
            for (int t = 0; t < weak && out.line.size() > 1; ++t) out.line.pop_back();
            break;
        }
    }
    while (out.line.size() > 1 && (!v.inFrame(out.line.back()[0], out.line.back()[1]) || v.mask.sample(out.line.back()[0], out.line.back()[1]) < 0.5f))
        out.line.pop_back();
    return out;
}

// The locks one view shows: from each seed root it sees under its hair, the lock's line (the way it
// leaves the root: along the flow either way, or falling where the flow runs across the fall; the
// one the flow holds longest, a little toward the longer and the falling); the clearest lines kept
// as locks, none running for half its length within half a lock's width of one kept already; then
// all the lines laid into the view's coarse flow (where none runs, the base flow).
void traceViewLocks(PreparedView& v, const std::vector<SurfacePoint>& seeds, const Vec3f& up, bool bilateral) {
    v.lockLines.assign(seeds.size(), LockLine{});
    if (!v.fine || v.mask.empty()) return;
    const int size = std::max(v.width, v.height);
    float period = v.curl.wavelengthPx > 0.0f ? v.curl.wavelengthPx : 0.05f * static_cast<float>(size);
    period = std::clamp(period, 0.02f * static_cast<float>(size), 0.12f * static_cast<float>(size));
    v.lockPeriodPx = period;
    auto base = std::make_shared<OrientationField>(coarseField(*v.fine, v.mask, 0.5f * period, bilateral));
    const std::array<float, 2> down = pictureDown(v.camera, up);
    const Vec3f eye = v.camera.eye();
    const int maxSteps = 3 * (v.width + v.height);
    const float fx = static_cast<float>(base->width) / static_cast<float>(v.width), fy = static_cast<float>(base->height) / static_cast<float>(v.height);
    parallelFor(0, seeds.size(), [&](std::size_t i) {
        const SurfacePoint& s = seeds[i];
        float u = 0, w = 0, z = 0;
        if (!v.camera.project(s.position, u, w, &z) || !v.inFrame(u, w) || v.hidden(u, w, z)) return;
        if (s.normal.dot((eye - s.position).normalized()) < 0.05f || v.rootMask.sample(u, w) < 0.5f) return;
        float ou = down[0], ow = down[1], c = 0.0f;
        if (!base->sample(u * fx, w * fy, ou, ow, c) || c < 0.02f) ou = down[0], ow = down[1];
        std::vector<std::array<float, 2>> starts = {{ou, ow}, {-ou, -ow}};
        if (std::abs(ou * down[0] + ow * down[1]) < pm::cos(30.0f * kPi / 180.0f)) starts.push_back(down);
        LockLine best;
        best.score = -1e30f;
        for (const auto& d0 : starts) {
            LockLine line = traceLockLine(v, *base, u, w, d0[0], d0[1], period, down, maxSteps);
            line.score = line.say + 0.3f * static_cast<float>(line.line.size()) + 0.2f * period * (d0[0] * down[0] + d0[1] * down[1]);
            if (line.score > best.score) best = std::move(line);
        }
        if (static_cast<float>(best.line.size()) >= 0.5f * period) v.lockLines[i] = std::move(best);
    }, 4);
    // The clearest lines kept as locks.
    const float width = 0.75f * period, radius = 0.5f * width;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < v.lockLines.size(); ++i)
        if (static_cast<float>(v.lockLines[i].line.size()) >= period) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return v.lockLines[a].score > v.lockLines[b].score; });
    std::vector<std::uint8_t> taken(static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height), 0);
    const int r = std::max(1, static_cast<int>(std::ceil(radius)));
    for (std::size_t i : order) {
        LockLine& l = v.lockLines[i];
        std::size_t near = 0;
        for (const auto& p : l.line)
            if (v.inFrame(p[0], p[1]) && taken[v.pixel(p[0], p[1])]) ++near;
        if (2 * near > l.line.size()) continue;
        l.kept = true;
        for (const auto& p : l.line) {
            const int cx = static_cast<int>(p[0]), cy = static_cast<int>(p[1]);
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= v.width || y >= v.height || static_cast<float>(dx * dx + dy * dy) > radius * radius) continue;
                    taken[static_cast<std::size_t>(y) * static_cast<std::size_t>(v.width) + static_cast<std::size_t>(x)] = 1;
                }
        }
    }
    // The lines laid into the coarse flow: their directions (doubled angles) spread over a third of a
    // period, where they run thick the lines' own, else blended into the base flow.
    if (base->width != v.width || base->height != v.height) return;
    const std::size_t pixels = static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height);
    std::vector<float> t(2 * pixels, 0.0f), weight(pixels, 0.0f);
    for (const LockLine& l : v.lockLines) {
        const std::size_t n = l.line.size();
        if (n < 3) continue;
        const float k = l.kept ? 1.0f : 0.5f;
        for (std::size_t j = 0; j < n; ++j) {
            const auto& a = l.line[j >= 2 ? j - 2 : 0];
            const auto& b = l.line[std::min(n - 1, j + 2)];
            const float du = b[0] - a[0], dv = b[1] - a[1], len = std::hypot(du, dv);
            if (len < 1e-6f || !v.inFrame(l.line[j][0], l.line[j][1])) continue;
            const float cu = du / len, cv = dv / len;
            const std::size_t q = v.pixel(l.line[j][0], l.line[j][1]);
            t[2 * q] += k * (cu * cu - cv * cv);
            t[2 * q + 1] += k * 2.0f * cu * cv;
            weight[q] += k;
        }
    }
    const float sigma = std::max(1.0f, 0.35f * period);
    t = gaussianBlurred(t, v.width, v.height, 2, sigma);
    weight = gaussianBlurred(weight, v.width, v.height, 1, sigma);
    const float full = 0.5f / (std::sqrt(2.0f * kPi) * sigma);  // (about one line's weight half a sigma off it)
    auto flow = std::make_shared<OrientationField>(*base);
    for (std::size_t q = 0; q < pixels; ++q) {
        if (v.mask.pixels[q] < 0.5f) {
            flow->tensor[2 * q] = flow->tensor[2 * q + 1] = 0.0f;
            continue;
        }
        const float wq = weight[q];
        const float a = std::clamp(wq / full, 0.0f, 1.0f);
        const float r0 = wq > 1e-6f ? t[2 * q] / wq : 0.0f, r1 = wq > 1e-6f ? t[2 * q + 1] / wq : 0.0f;
        flow->tensor[2 * q] = a * r0 + (1.0f - a) * base->tensor[2 * q];
        flow->tensor[2 * q + 1] = a * r1 + (1.0f - a) * base->tensor[2 * q + 1];
    }
    v.coarse = flow;
}

// A pixel's ray (unit, from the eye).
Vec3f pixelRay(const RefCamera& c, float u, float w) {
    return (c.axis(0) * static_cast<float>((u - 0.5 * c.width - c.offsetU) / c.focalPx) -
            c.axis(1) * static_cast<float>((w - 0.5 * c.height - c.offsetV) / c.focalPx) - c.axis(2))
        .normalized();
}

// How far a point is from the nearest surface (the scalp or an occluder), up to `reach`.
float surfaceDistance(const Context& ctx, const Vec3f& p, float reach) {
    float best = reach;
    const SurfacePoint sp = ctx.scalp.closestPoint(p, reach);
    if (sp.root.face >= 0) best = std::min(best, (p - sp.position).length());
    for (const auto& occluder : ctx.options.occluders) {
        if (!occluder) continue;
        const SurfacePoint hit = occluder->closestPoint(p, best);
        if (hit.root.face >= 0) best = std::min(best, (p - hit.position).length());
    }
    return best;
}

// Along a view's ray through (u, w): how far from the eye the hair's surface is, `thickness` off the
// scalp or occluder the view sees there (the nearest point of the ray that far from them); -1 where
// the ray meets neither.
float hairSurfaceAlong(const Context& ctx, const PreparedView& v, float u, float w, float thickness) {
    if (!v.inFrame(u, w)) return -1.0f;
    const float z = v.depth[v.pixel(u, w)];
    if (!(z < kInf)) return -1.0f;
    const Vec3f eye = v.camera.eye(), ray = pixelRay(v.camera, u, w);
    const float along = std::max(1e-6f, -ray.dot(v.camera.axis(2)));
    const float far = z / along;  // (the surface)
    float lo = std::max(0.5f * far, far - 8.0f * thickness / along), hi = far;
    if (surfaceDistance(ctx, eye + ray * lo, 2.0f * thickness) <= thickness) return lo;
    for (int it = 0; it < 8; ++it) {
        const float mid = 0.5f * (lo + hi);
        if (surfaceDistance(ctx, eye + ray * mid, 2.0f * thickness) <= thickness) hi = mid;
        else lo = mid;
    }
    return 0.5f * (lo + hi);
}

// A placed lock: its guide's path (root on the scalp first) and where it came from.
struct PlacedLock {
    std::vector<Vec3f> path;
    SurfacePoint root;
    std::size_t seed = 0;
    int view = -1;
    bool triangulated = false;
    float score = 0.0f;
    float agreement = 0.0f;  // the other views' score (scoreOnViews)
    float support = 0.0f;    // their support (GuideScore::support)
};

// Two views' lines of one lock (from the same root): for points along a's line, the other view's
// line where a's ray's picture in it (its epipolar line) crosses it, at a clear angle, the crossing
// nearest the way along b's line the walk has reached (never far back); the rays' nearest meeting
// there, kept where they nearly meet (`tolerance`, scene units). `rayT`: how far along a's rays (per
// point of a's line, -1 where none); the points found.
int triangulateLines(const PreparedView& a, const LockLine& la, const PreparedView& b, const LockLine& lb, float tolerance,
                     std::vector<float>& rayT) {
    const std::size_t na = la.line.size(), nb = lb.line.size();
    rayT.assign(na, -1.0f);
    if (na < 4 || nb < 4 || a.scalpDepth <= 0.0f) return 0;
    const Vec3f ea = a.camera.eye(), eb = b.camera.eye();
    std::vector<float> lengthB(nb, 0.0f);
    for (std::size_t j = 1; j < nb; ++j) lengthB[j] = lengthB[j - 1] + std::hypot(lb.line[j][0] - lb.line[j - 1][0], lb.line[j][1] - lb.line[j - 1][1]);
    const float ratio = lengthB.back() / static_cast<float>(na);
    float reached = 0.0f;  // along b's line, pixels
    int found = 0;
    const float sinMin = pm::sin(15.0f * kPi / 180.0f);
    for (std::size_t i = 0; i < na; i += 2) {
        const Vec3f ra = pixelRay(a.camera, la.line[i][0], la.line[i][1]);
        const float along = std::max(1e-6f, -ra.dot(a.camera.axis(2)));
        float u0 = 0, w0 = 0, u1 = 0, w1 = 0;
        if (!b.camera.project(ea + ra * (0.5f * a.scalpDepth / along), u0, w0) || !b.camera.project(ea + ra * (1.5f * a.scalpDepth / along), u1, w1)) continue;
        const float eu = u1 - u0, ew = w1 - w0, el = std::hypot(eu, ew);
        if (el < 1e-6f) continue;
        const float expected = std::max(reached, static_cast<float>(i) * ratio);
        float bestGap = 0.35f * lengthB.back() + 2.0f, bestAt = -1.0f;
        std::array<float, 2> bestPx{0, 0};
        for (std::size_t j = 0; j + 1 < nb; ++j) {
            const float pu = lb.line[j][0], pw = lb.line[j][1], qu = lb.line[j + 1][0] - pu, qw = lb.line[j + 1][1] - pw;
            const float den = eu * qw - ew * qu;
            const float ql = std::hypot(qu, qw);
            if (ql < 1e-6f || std::abs(den) < sinMin * el * ql) continue;
            const float s = ((pu - u0) * qw - (pw - w0) * qu) / den;  // along the epipolar segment
            const float t = ((pu - u0) * ew - (pw - w0) * eu) / den;  // along b's segment
            if (s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f) continue;
            const float at = lengthB[j] + t * (lengthB[j + 1] - lengthB[j]);
            if (at < reached - 0.1f * lengthB.back()) continue;
            const float gap = std::abs(at - expected);
            if (gap < bestGap) bestGap = gap, bestAt = at, bestPx = {pu + t * qu, pw + t * qw};
        }
        if (bestAt < 0.0f) continue;
        const Vec3f rb = pixelRay(b.camera, bestPx[0], bestPx[1]);
        // (the rays' nearest points)
        const Vec3f d0 = ea - eb;
        const float A = ra.dot(ra), B = ra.dot(rb), C = rb.dot(rb), D = ra.dot(d0), E = rb.dot(d0);
        const float den = A * C - B * B;
        if (den < 1e-8f) continue;
        const float ta = (B * E - C * D) / den, tb = (A * E - B * D) / den;
        if (ta <= 0.0f || tb <= 0.0f) continue;
        const Vec3f pa = ea + ra * ta, pb = eb + rb * tb;
        if ((pa - pb).length() > tolerance) continue;
        rayT[i] = ta + 0.5f * (pb - pa).dot(ra);
        reached = std::max(reached, bestAt);
        ++found;
    }
    return found;
}

// Each lock the views show placed in 3D: in each view, each lock's line (from a seed root); where
// another view traces a line from the same root at a clear angle (20-120 degrees apart), the two
// triangulated (the pair with the most points); its points along the line's rays at the hair's
// surface (the hair's thickness over its root off the scalp or occluder under them; past the head's
// outline the last depth carried on), moved to the triangulated depths where those are known (the
// difference blended between them, held past them, smoothed over a period). The guide: the lock's
// root, then the line's points. Counts in result.lockViews.
std::vector<PlacedLock> placeLocks(const Context& ctx, const std::vector<SurfacePoint>& seeds, ReferenceGuidesResult& result) {
    std::vector<PlacedLock> out;
    const std::vector<PreparedView>& views = ctx.views;
    result.lockViews.assign(views.size(), ReferenceGuidesResult::LockViewCounts{});
    struct Job {
        std::size_t view, seed;
    };
    std::vector<Job> jobs;
    for (std::size_t a = 0; a < views.size(); ++a)
        for (std::size_t s = 0; s < views[a].lockLines.size(); ++s)
            if (views[a].lockLines[s].kept) jobs.push_back({a, s});
    std::vector<PlacedLock> placed(jobs.size());
    std::vector<int> partnered(jobs.size(), 0);
    const float share = std::max(0.3f, ctx.options.volume);
    parallelFor(0, jobs.size(), [&](std::size_t k) {
        const std::size_t a = jobs[k].view, s = jobs[k].seed;
        const PreparedView& va = views[a];
        const LockLine& la = va.lockLines[s];
        const SurfacePoint& root = seeds[s];
        const std::size_t n = la.line.size();
        const Vec3f eye = va.camera.eye();
        float thickness = hairThickness(ctx, root.position, root.normal.normalized(), 0.5f * ctx.maxLength);
        if (thickness <= 0.0f) thickness = va.rimWorld;
        thickness = std::max(2.0f * ctx.offset, share * thickness);
        // Each point's ray's view depth factor (its z per unit along it).
        std::vector<float> along(n);
        for (std::size_t i = 0; i < n; ++i) along[i] = std::max(1e-6f, -pixelRay(va.camera, la.line[i][0], la.line[i][1]).dot(va.camera.axis(2)));
        // Single-view: along each ray at the hair's surface (every third point; between, the view depth
        // blended; where no surface is under the line, the nearest known view depth carried on).
        std::vector<float> single(n, -1.0f);
        for (std::size_t i = 0; i < n; i += 3) single[i] = hairSurfaceAlong(ctx, va, la.line[i][0], la.line[i][1], thickness);
        if (n > 1) single[n - 1] = hairSurfaceAlong(ctx, va, la.line[n - 1][0], la.line[n - 1][1], thickness);
        {
            std::vector<float> z(n, -1.0f);
            for (std::size_t i = 0; i < n; ++i)
                if (single[i] >= 0.0f) z[i] = single[i] * along[i];
            std::size_t prev = n;
            for (std::size_t i = 0; i < n; ++i) {
                if (z[i] < 0.0f) continue;
                if (prev < n && i > prev + 1 && i - prev <= 4)
                    for (std::size_t j = prev + 1; j < i; ++j)
                        z[j] = z[prev] + (z[i] - z[prev]) * static_cast<float>(j - prev) / static_cast<float>(i - prev);
                prev = i;
            }
            float carry = -1.0f;
            for (std::size_t i = 0; i < n && carry < 0.0f; ++i)
                if (z[i] >= 0.0f) carry = z[i];
            if (carry < 0.0f) carry = std::max(1e-3f, (root.position + root.normal.normalized() * thickness - eye).dot(-va.camera.axis(2)));
            for (std::size_t i = 0; i < n; ++i) {
                if (z[i] >= 0.0f) carry = z[i];
                single[i] = carry / along[i];
            }
        }
        // Triangulated with the other views' lines from the same root.
        std::vector<float> best;
        int bestFound = 0;
        const Vec3f lookA = -va.camera.axis(2);
        const float perPx = va.scalpDepth > 0.0f ? va.scalpDepth / static_cast<float>(va.camera.focalPx) : 0.0f;
        const float tolerance = std::max(4.0f, 0.75f * va.lockPeriodPx) * perPx;
        for (std::size_t b = 0; b < views.size(); ++b) {
            if (b == a || s >= views[b].lockLines.size() || views[b].lockLines[s].line.size() < 4) continue;
            const float angle = std::acos(std::clamp(lookA.dot(-views[b].camera.axis(2)), -1.0f, 1.0f)) * 180.0f / kPi;
            if (angle < 20.0f || angle > 120.0f) continue;
            partnered[k] = 1;
            std::vector<float> t;
            const int found = triangulateLines(va, la, views[b], views[b].lockLines[s], tolerance, t);
            if (found > bestFound) bestFound = found, best = std::move(t);
        }
        // (enough of the line: a third of the points tried, three at least)
        const bool triangulated = bestFound >= 3 && 6 * bestFound >= static_cast<int>(n);
        std::vector<float> t = single;
        if (triangulated) {
            std::vector<std::size_t> known;
            for (std::size_t i = 0; i < n; ++i)
                if (best[i] >= 0.0f) known.push_back(i);
            std::vector<float> offset(n, 0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                const auto it = std::lower_bound(known.begin(), known.end(), i);
                if (it == known.begin()) offset[i] = best[known.front()] - single[known.front()];
                else if (it == known.end()) offset[i] = best[known.back()] - single[known.back()];
                else {
                    const std::size_t hi = *it, lo = *(it - 1);
                    const float f = static_cast<float>(i - lo) / static_cast<float>(std::max<std::size_t>(1, hi - lo));
                    offset[i] = (best[lo] - single[lo]) * (1.0f - f) + (best[hi] - single[hi]) * f;
                }
            }
            // (smoothed over a period: the views' small misalignments are not the lock's)
            const long half = std::max(2L, static_cast<long>(0.5f * va.lockPeriodPx));
            for (std::size_t i = 0; i < n; ++i) {
                double sum = 0.0;
                int c = 0;
                for (long d = -half; d <= half; ++d) {
                    const long j = static_cast<long>(i) + d;
                    if (j < 0 || j >= static_cast<long>(n)) continue;
                    sum += offset[static_cast<std::size_t>(j)];
                    ++c;
                }
                t[i] = single[i] + static_cast<float>(sum / std::max(1, c));
            }
        }
        PlacedLock& p = placed[k];
        p.root = root;
        p.seed = s;
        p.view = static_cast<int>(a);
        p.triangulated = triangulated;
        p.score = la.score;
        // (the line alone: rootGuide joins it to the scalp, the end nearer it first)
        for (std::size_t i = 0; i < n; i += 2) p.path.push_back(eye + pixelRay(va.camera, la.line[i][0], la.line[i][1]) * t[i]);
    }, 1);
    for (std::size_t k = 0; k < jobs.size(); ++k) {
        ReferenceGuidesResult::LockViewCounts& c = result.lockViews[jobs[k].view];
        ++c.detected;
        if (partnered[k]) ++c.matched;
        if (placed[k].triangulated) ++c.triangulated;
        else ++c.singleView;
        if (placed[k].path.size() >= 4) out.push_back(std::move(placed[k]));
    }
    return out;
}

// A path kept off the scalp and the occluders (each point at least the offset out along the nearest
// surface's outward normal), its root where it is.
std::vector<Vec3f> keptOut(const Context& ctx, std::vector<Vec3f> path) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        Vec3f& q = path[i];
        const SurfacePoint near = ctx.scalp.closestPoint(q, 4.0f * ctx.offset + ctx.step);
        if (near.root.face >= 0) {
            const Vec3f n = near.normal.normalized();
            const float h = (q - near.position).dot(n);
            if (h < ctx.offset) q = q + n * (ctx.offset - h);
        }
        for (std::size_t o = 0; o < ctx.options.occluders.size(); ++o) {
            const auto& occluder = ctx.options.occluders[o];
            if (!occluder || ctx.occluderSigns[o] == 0.0f) continue;
            const SurfacePoint hit = occluder->closestPoint(q, 4.0f * ctx.offset + ctx.step);
            if (hit.root.face < 0) continue;
            const Vec3f outward = hit.normal.normalized() * ctx.occluderSigns[o];
            const float h = (q - hit.position).dot(outward);
            if (h < ctx.offset) q = q + outward * (ctx.offset - h);
        }
    }
    return path;
}

}  // namespace

static void fillLockCurls(const std::vector<PreparedView>& views, const ReferenceGuideOptions& options, std::vector<ReferenceGuide>& guides,
                          std::vector<std::vector<Vec3f>>& paths, ReferenceGuidesResult& result);

static void bakeLocks(const std::vector<PreparedView>& views, const ScalpMesh& scalp, const ReferenceGuideOptions& options,
               std::vector<ReferenceGuide>& guides, std::vector<std::vector<Vec3f>>& paths, ReferenceGuidesResult& result) {
    const std::size_t count = guides.size();
    std::vector<LockFit> fits(count);
    std::vector<float> gains(count, 0.0f);
    // Each guide smoothed a little past the curls first (grown along the coarse flow it still swings
    // with them some, and a swing measured against a swinging guide reads small); not to its chord
    // (that would flatten it onto the scalp).
    if (result.look.curlFrequency > 0.0f)
        for (std::vector<Vec3f>& path : paths) {
            if (path.size() < 4) continue;
            const float step = arcLength(path) / static_cast<float>(path.size() - 1);
            const float sigma = 0.12f / result.look.curlFrequency / std::max(step, 1e-6f);  // samples
            path = smoothed(std::move(path), static_cast<int>(std::clamp(2.0f * sigma * sigma, 0.0f, 60.0f)));
        }
    // Each guide's lock: where a view's strands swing most clearly along it (the view, and a shift
    // sideways within about half a turn: a lock's spacing), the guide moved there and given that
    // lock's curl, both ways round; kept when the photos' strands agree with it better than with the
    // guide as it grew (lockAgreement: one guide down the middle of a lock, in step with its curl).
    parallelFor(0, count, [&](std::size_t i) {
        std::vector<LockFit> candidates;
        for (std::size_t k = 0; k < views.size(); ++k) {
            const float period = views[k].curl.wavelengthPx;
            if (period <= 0.0f) continue;
            const float step = std::max(1.0f, 0.1f * period);
            std::vector<LockFit> here;
            for (float shift = -0.5f * period; shift <= 0.5f * period + 1e-3f; shift += step) {
                LockFit f = fitLock(views[k], static_cast<int>(k), paths[i], shift);
                if (f.ok && f.quality >= 0.2f) here.push_back(std::move(f));
            }
            std::sort(here.begin(), here.end(), [](const LockFit& a, const LockFit& b) { return a.score > b.score; });
            for (std::size_t c = 0; c < std::min<std::size_t>(2, here.size()); ++c) candidates.push_back(std::move(here[c]));
        }
        float best = fineAgreement(views, paths[i]) + 0.02f;
        std::vector<Vec3f> chosen, chosenBase;
        StrandRoot chosenRoot = guides[i].root;
        float chosenHand = 1.0f;
        for (const LockFit& c : candidates) {
            const PreparedView& v = views[static_cast<std::size_t>(c.view)];
            StrandRoot root = guides[i].root;
            const std::vector<Vec3f> moved = c.shiftPx != 0.0f ? movedSideways(v, paths[i], c.shiftPx, scalp, &root) : paths[i];
            const LockFit f = c.shiftPx != 0.0f ? fitLock(v, c.view, moved) : c;
            if (!f.ok) continue;
            for (const float gain : {1.0f})
                for (const float hand : {1.0f, -1.0f}) {
                    std::vector<Vec3f> baked = tracedCurl(v, f, moved, gain, hand);
                    const float score = fineAgreement(views, baked);
                    if (score <= best) continue;
                    best = score;
                    chosen = std::move(baked);
                    chosenBase = moved;
                    chosenRoot = root;
                    chosenHand = hand;
                    fits[i] = f;
                    gains[i] = gain;
                }
        }
        if (chosen.empty()) return;
        // The lock's depth in the traced view: the guide moved sideways in each other view too (the
        // traced view's fit kept on it), where the photos agree better.
        const PreparedView& traced = views[static_cast<std::size_t>(fits[i].view)];
        for (std::size_t k = 0; k < views.size(); ++k) {
            if (static_cast<int>(k) == fits[i].view) continue;
            const float period = views[k].curl.wavelengthPx > 0.0f ? views[k].curl.wavelengthPx : traced.curl.wavelengthPx;
            const float step = std::max(1.0f, 0.125f * period);
            const std::vector<Vec3f> base = chosenBase;
            for (float shift = -0.5f * period; shift <= 0.5f * period + 1e-3f; shift += step) {
                if (std::abs(shift) < 1e-3f) continue;
                StrandRoot root = chosenRoot;
                const std::vector<Vec3f> moved = movedSideways(views[k], base, shift, scalp, &root);
                const LockFit f = fitLock(traced, fits[i].view, moved);
                if (!f.ok) continue;
                std::vector<Vec3f> baked = tracedCurl(traced, f, moved, gains[i], chosenHand);
                const float score = fineAgreement(views, baked);
                if (score <= best) continue;
                best = score;
                chosen = std::move(baked);
                chosenBase = moved;
                chosenRoot = root;
                fits[i] = f;
            }
        }
        // Placed and turned by the curl as the photos read it; baked at its full size (the swing a
        // photo shows reads about half a lock's: its strands out of step, the fields' smoothing).
        gains[i] = options.lockGain;
        chosen = tracedCurl(traced, fits[i], chosenBase, options.lockGain, chosenHand);
        paths[i] = std::move(chosen);
        guides[i].root = chosenRoot;
    }, 1);
    for (std::size_t i = 0; i < count; ++i) {
        const LockFit& f = fits[i];
        if (gains[i] <= 0.0f) continue;
        ReferenceGuide& g = guides[i];
        g.lock = 1;
        g.lockView = f.view;
        g.lockWavelength = f.periodPx / f.pxPerLength;
        g.lockRadius = gains[i] * std::tan(std::min(f.amplitude, 1.2f)) * f.periodPx / (2.0f * kPi) / f.pxPerUnit;
        g.lockStrength = std::clamp(0.5f + 0.45f * f.quality, 0.5f, 0.95f);
    }
    fillLockCurls(views, options, guides, paths, result);
}

// The traced guides' (lock 1) curls counted (result's locksTraced, mean radius and turn length);
// the rest take the nearest traced guides' curl (by their roots, inverse distance), their own phase
// and handedness (the ones the photos' strands agree with best, where any sees it).
static void fillLockCurls(const std::vector<PreparedView>& views, const ReferenceGuideOptions& options, std::vector<ReferenceGuide>& guides,
                          std::vector<std::vector<Vec3f>>& paths, ReferenceGuidesResult& result) {
    const std::size_t count = guides.size();
    auto handOf = [](std::size_t i) { return ((i * 2654435761u) >> 7) & 1u ? 1.0f : -1.0f; };
    std::vector<std::size_t> traced;
    double radiusSum = 0.0, wavelengthSum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        if (guides[i].lock != 1) continue;
        traced.push_back(i);
        radiusSum += guides[i].lockRadius;
        wavelengthSum += guides[i].lockWavelength;
    }
    if (traced.empty()) return;
    result.locksTraced = static_cast<int>(traced.size());
    result.lockRadius = static_cast<float>(radiusSum / static_cast<double>(traced.size()));
    result.lockWavelength = static_cast<float>(wavelengthSum / static_cast<double>(traced.size()));
    for (std::size_t i = 0; i < count; ++i) {
        ReferenceGuide& g = guides[i];
        std::vector<Vec3f>& path = paths[i];
        if (path.size() < 3 || g.lock == 1) continue;
        std::vector<float> side(path.size(), 0.0f), depth(path.size(), 0.0f);
        // Filled: the nearest traced guides' curl, by their roots (inverse distance).
        std::vector<std::pair<float, std::size_t>> near;
        for (std::size_t j : traced) near.push_back({(paths[j][0] - path[0]).length(), j});
        const std::size_t take = std::min<std::size_t>(4, near.size());
        std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(take), near.end());
        double w = 0.0, radius = 0.0, wavelength = 0.0, strength = 0.0;
        for (std::size_t t = 0; t < take; ++t) {
            const double k = 1.0 / (1e-3 + near[t].first);
            w += k;
            radius += k * guides[near[t].second].lockRadius;
            wavelength += k * guides[near[t].second].lockWavelength;
            strength += k * guides[near[t].second].lockStrength;
        }
        g.lock = 2;
        g.lockRadius = static_cast<float>(radius / w);
        g.lockWavelength = static_cast<float>(wavelength / w);
        g.lockStrength = static_cast<float>(strength / w);
        const float phase = 2.0f * kPi * static_cast<float>((i * 40503u) % 1000u) / 1000.0f;
        std::vector<float> theta(path.size(), 0.0f);
        float length = 0.0f;
        for (std::size_t k = 0; k < path.size(); ++k) {
            if (k > 0) length += (path[k] - path[k - 1]).length();
            theta[k] = 2.0f * kPi * length / std::max(1e-6f, g.lockWavelength) + phase;
        }
        const Vec3f up = options.up.normalized();
        const auto across = [&](std::size_t, const Vec3f& t) {
            const Vec3f a = t.cross(up);
            return a.length() > 1e-3f ? a : t.cross(Vec3f(1, 0, 0));
        };
        // (the phase and the handedness the photos' strands agree with best, where any sees it)
        std::vector<Vec3f> best;
        float bestScore = -2.0f;
        for (int turn = 0; turn < 8; ++turn)
            for (const float hand : {handOf(i), -handOf(i)}) {
                for (std::size_t j = 0; j < path.size(); ++j) {
                    const float t = theta[j] + 2.0f * kPi * static_cast<float>(turn) / 8.0f;
                    side[j] = g.lockRadius * std::sin(t);
                    depth[j] = g.lockRadius * std::cos(t) * hand;
                }
                std::vector<Vec3f> baked = bakedCurl(path, g.lockWavelength, side, depth, across);
                const float score = fineAgreement(views, baked);
                if (score > bestScore) bestScore = score, best = std::move(baked);
            }
        path = std::move(best);
        ++result.locksFilled;
    }
}

// Waves inside the locks (lock paths on wavy hair): each guide's fine wave as a view shows it along
// it (fitLock: the strands' swing about the lock's way, its period, size and phase; a lock guide's
// own view, a grown guide's the clearest), baked in at waveGain times that swing (the lock's own path
// round its middle, its other half a quarter turn on: a loose curl), the handedness the photos agree
// with best; kept unless the photos agree with the smooth guide clearly better. The rest take their
// neighbours' (fillLockCurls).
static void waveLocks(const std::vector<PreparedView>& views, const ReferenceGuideOptions& options, std::vector<ReferenceGuide>& guides,
                      std::vector<std::vector<Vec3f>>& paths, const std::vector<int>& lockView, ReferenceGuidesResult& result) {
    parallelFor(0, guides.size(), [&](std::size_t i) {
        // (a lock guide's own view; a grown guide's, the view that shows its wave most clearly)
        LockFit f;
        for (std::size_t k = 0; k < views.size(); ++k) {
            if (i < lockView.size() && lockView[i] >= 0 && static_cast<int>(k) != lockView[i]) continue;
            if (views[k].curl.wavelengthPx <= 0.0f) continue;
            LockFit fit = fitLock(views[k], static_cast<int>(k), paths[i]);
            if (fit.ok && (!f.ok || fit.score > f.score)) f = std::move(fit);
        }
        if (!f.ok || f.quality < 0.15f) return;
        f.offset.clear();  // (the fitted wave, not the swing summed: a wave is even along the lock)
        const PreparedView& v = views[static_cast<std::size_t>(f.view)];
        const float smooth = fineAgreement(views, paths[i]);
        float best = -2.0f;
        std::vector<Vec3f> chosen;
        for (const float hand : {1.0f, -1.0f}) {
            std::vector<Vec3f> baked = tracedCurl(v, f, paths[i], options.waveGain, hand);
            const float score = fineAgreement(views, baked);
            if (score > best) best = score, chosen = std::move(baked);
        }
        if (chosen.empty() || best < smooth - 0.03f) return;
        ReferenceGuide& g = guides[i];
        g.lock = 1;
        g.lockView = f.view;
        g.lockWavelength = f.periodPx / f.pxPerLength;
        g.lockRadius = options.waveGain * std::tan(std::min(f.amplitude, 1.2f)) * f.periodPx / (2.0f * kPi) / f.pxPerUnit;
        g.lockStrength = std::clamp(0.5f + 0.45f * f.quality, 0.5f, 0.95f);
        paths[i] = std::move(chosen);
    }, 1);
    fillLockCurls(views, options, guides, paths, result);
}

ReferenceGuidesResult guidesFromReference(const ScalpMesh& scalp, const std::vector<ReferenceView>& views,
                                          const ReferenceGuideOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    ReferenceGuidesResult result;
    if (scalp.triangleCount() == 0) throw std::invalid_argument("guidesFromReference: the scalp has no triangles");
    if (views.empty()) throw std::invalid_argument("guidesFromReference: no views");
    const float size = scalpDiagonal(scalp);
    std::vector<PreparedView> prepared = prepareViews(scalp, views, options, size);

    Context ctx{scalp, prepared, options, Mirror{}, size, 0, 0, 0, false, nullptr};
    for (const PreparedView& v : prepared) {
        result.rimLength = std::max(result.rimLength, v.rimWorld);
        result.reachLength = std::max(result.reachLength, v.reachWorld);
    }
    // Automatic (0): as far as the photos show the hair reaching past the scalp, a little more (the
    // longest hair hides under the rest), within the scalp's size and a few steps of the scalp.
    // Short hair and fur lies at a low angle to the skin: it reaches past the outline about half its
    // length (hanging hair about all of it).
    const float lean = 1.2f * result.reachLength < size / 7.0f ? 2.0f : 1.2f;
    float measured = std::clamp(lean * result.reachLength, 0.02f * size, size);
    // Long hair lying over the head (swept back, falling down the back) reaches little past the
    // outline for its length: the strands' own paths in the photos say how long it is (their upper
    // quartile, the longest view's), when longer.
    // (the curls judged on the outline's length: the strands' paths are read along the coarse flow
    // the curls decide on)
    const float provisional = result.reachLength > 0 ? measured : size;
    bool lockPaths = false;
    std::vector<SurfacePoint> lockSeeds;
    float seedSpacing = 0.0f;
    // Curls: measured in every view; curly hair grows guides along the locks' coarse flow (a Curl
    // node puts the curls back), not along each curl.
    {
        std::vector<float> shares, amplitudes, wavelengths;
        std::vector<std::pair<float, int>> swings;  // a view's swing, its streamlines
        bool wavy = false;
        for (const PreparedView& v : prepared) {
            if (v.curl.streamlines < 5) continue;
            shares.push_back(v.curl.periodicShare);
            if (v.curl.wavelengthPx > 0.0f && v.scalpDepth > 0.0f) {
                amplitudes.push_back(v.curl.amplitude);
                swings.push_back({v.curl.amplitude, v.curl.streamlines});
                wavelengths.push_back(v.curl.wavelengthPx * v.scalpDepth / static_cast<float>(v.camera.focalPx));
            }
        }
        if (!shares.empty()) {
            result.look.curlShare = median(shares);
            result.look.curlAmplitudeDeg = amplitudes.empty() ? 0.0f : median(amplitudes) * 180.0f / kPi;
            // (Short fur lies flat: what swings in it is not curls.)
            const float longest = options.maxLength > 0 ? options.maxLength : provisional;
            // (Judged by the views' lower quartile swing, each view weighing its streamlines: curls
            // swing in every view that shows much hair; straight hair's strands crossing each other
            // swing in one now and then.)
            float swing = 0.0f;
            if (!swings.empty()) {
                std::sort(swings.begin(), swings.end());
                int total = 0, below = 0;
                for (const auto& s : swings) total += s.second;
                for (const auto& s : swings) {
                    below += s.second;
                    swing = s.first * 180.0f / kPi;
                    if (4 * below > total) break;
                }
            }
            const bool curls = result.look.curlShare > kCurlShare && swing > kCurlAmplitude;
            const bool waves = result.look.curlShare > kWavyShare && swing > kWavyAmplitude;
            // (waves are the strands' own long paths: the guides follow them, no curl of their own)
            result.look.curly = curls && longest >= size / 7.0f;
            wavy = waves && !result.look.curly;
            result.look.wavy = wavy;
            if (!wavelengths.empty()) {
                const float wavelength = median(wavelengths);
                result.look.curlFrequency = 1.0f / std::max(wavelength, 1e-6f);
                result.look.curlRadius = kCurlRadiusGain * wavelength * pm::tan(std::min(median(amplitudes), 1.2f)) / (2.0f * kPi);
            }
        }
        // Locks as objects: in each view the locks' paths from seed roots over the whole scalp (their
        // lines laid into the view's coarse flow, what the grower and the lengths read).
        lockPaths = options.lockPaths > 0 || (options.lockPaths < 0 && wavy);
        if (lockPaths) {
            const int seedCount = std::clamp(2 * options.guideCount, 200, 2000);
            lockSeeds = blueNoiseRoots(scalp, seedCount, options.seed + 101);
            seedSpacing = std::sqrt(scalp.totalArea() / static_cast<float>(std::max<std::size_t>(1, lockSeeds.size())));
            for (PreparedView& v : prepared) traceViewLocks(v, lockSeeds, options.up.normalized(), options.orientation.bilateralCoarse);
        }
        const bool coarse = options.curlFlow > 0 || (options.curlFlow < 0 && (result.look.curly || wavy)) || lockPaths;
        if (coarse)
            for (PreparedView& v : prepared)
                if (v.coarse) v.field = v.coarse;
    }
    result.strandLength = strandPathLength(prepared);
    if (result.reachLength > 0.0f && result.strandLength > measured) measured = std::min(1.1f * result.strandLength, 2.0f * size);
    ctx.maxLength = options.maxLength > 0 ? options.maxLength
                    : options.maxLength < 0 && result.rimLength > 0 ? -options.maxLength * result.rimLength
                    : result.reachLength > 0 ? measured
                                             : size;
    // An automatic length takes the longest drawn guide line in too (a line says how long hair is).
    if (options.maxLength <= 0.0f)
        for (const PreparedView& v : prepared)
            for (std::size_t i = 0; i < v.hintLength.size(); i += 2)
                if (v.hintLength[i + 1] > 0.5f) ctx.maxLength = std::max(ctx.maxLength, 1.05f * v.hintLength[i]);
    ctx.offset = options.scalpOffset > 0 ? options.scalpOffset : 0.004f * size;
    // Short hair and fur (shorter than a seventh of the scalp's size) lies along the surface more than
    // it falls: the surface prior doubled, gravity a fifth.
    ctx.surfacePrior = options.surfacePrior;
    ctx.gravityPrior = options.gravityPrior;
    if (ctx.maxLength < size / 7.0f) {
        ctx.surfacePrior *= 2.0f;
        ctx.gravityPrior *= 0.2f;
    }
    ctx.step = options.step > 0 ? options.step : ctx.maxLength / 80.0f;
    // At least ~1.5 pixels a step in the views (finer steps only resample the same pixels).
    if (options.step <= 0) {
        float finest = kInf;
        for (const PreparedView& v : prepared) {
            float u = 0, vv = 0, z = 0;
            if (scalp.pointCount() && v.camera.project(scalp.points()[0], u, vv, &z)) finest = std::min(finest, 1.5f * z / static_cast<float>(v.camera.focalPx));
        }
        if (finest < kInf) ctx.step = std::max(ctx.step, finest);
    }
    ctx.inflate = options.inflate > 0 || (options.inflate < 0 && prepared.size() == 1);
    for (const auto& occluder : options.occluders) {
        float sign = 0.0f;
        if (occluder && occluder->pointCount()) {
            Vec3f center(0, 0, 0);
            for (const Vec3f& q : occluder->points()) center += q;
            center = center / static_cast<float>(occluder->pointCount());
            double out = 0.0;
            for (std::size_t i = 0; i < occluder->pointCount(); ++i)
                out += static_cast<double>((occluder->points()[i] - center).dot(occluder->vertexNormals()[i]));
            sign = out >= 0.0 ? 1.0f : -1.0f;
        }
        ctx.occluderSigns.push_back(sign);
    }
    ReferenceSymmetry symmetry = options.symmetry;
    if (symmetry == ReferenceSymmetry::Auto) symmetry = scalpLooksSymmetric(scalp) ? ReferenceSymmetry::Topology : ReferenceSymmetry::None;
    if (symmetry == ReferenceSymmetry::WorldX) {
        ctx.mirror.sym.axis = 0;
        ctx.mirror.sym.center = options.symmetryCenter;
    } else if (symmetry == ReferenceSymmetry::Topology) {
        ctx.mirror.sym.mirror = ScalpMirror::of(scalp);
        ctx.mirror.sym.scalp = &scalp;
        if (!ctx.mirror.sym.mirror) symmetry = ReferenceSymmetry::None;
    }
    result.symmetry = symmetry;
    result.step = ctx.step;
    result.maxLength = ctx.maxLength;
    result.scalpOffset = ctx.offset;

    const ReferenceShell shell = ReferenceShellBuilder::make(ctx);
    ctx.shell = &shell;
    result.shellFill = shell.fill();

    // Roots: given, or spread; with symmetry the ones on the first side (and the seam).
    std::vector<SurfacePoint> roots;
    std::vector<std::uint8_t> seam;
    std::vector<int> status;  // 0 grow, 1 unseen, 2 bald
    float spacing = 0.0f;
    const auto spread = [&](int count) {
        roots.clear();
        if (!options.roots.empty()) {
            for (const StrandRoot& r : options.roots) {
                if (!scalp.contains(r)) continue;
                SurfacePoint sp;
                sp.root = r;
                sp.position = scalp.position(r);
                sp.normal = scalp.normal(r);
                roots.push_back(sp);
            }
        } else {
            roots = blueNoiseRoots(scalp, count, options.seed);
            // Curly hair: a root where each lock the photos show grows from, the even spread filling
            // in where none is seen (as many in all).
            if (options.lockCurls && result.look.curly) {
                const float even = std::sqrt(scalp.totalArea() / static_cast<float>(std::max(1, count)));
                int detected = 0;
                std::vector<SurfacePoint> locks = lockRoots(prepared, scalp, 0.6f * even, &detected);
                if (locks.size() > static_cast<std::size_t>(count)) locks.resize(static_cast<std::size_t>(count));
                result.locksDetected = detected;
                result.lockRoots = static_cast<int>(locks.size());
                for (const SurfacePoint& r : roots) {
                    if (locks.size() >= static_cast<std::size_t>(count)) break;
                    bool apart = true;
                    for (std::size_t k = 0; k < static_cast<std::size_t>(result.lockRoots) && apart; ++k)
                        apart = (locks[k].position - r.position).length() >= 0.7f * even;
                    if (apart) locks.push_back(r);
                }
                roots.swap(locks);
            }
        }
        spacing = std::sqrt(scalp.totalArea() / static_cast<float>(std::max<std::size_t>(1, roots.size())));
        seam.assign(roots.size(), 0);
        if (ctx.mirror.on() && options.roots.empty()) {
            std::vector<SurfacePoint> kept;
            std::vector<std::uint8_t> keptSeam;
            for (const SurfacePoint& r : roots) {
                const Vec3f m = ctx.mirror.point(r.position);
                const bool onSeam = (m - r.position).length() < 0.5f * spacing;
                if (onSeam || r.position.x >= m.x) {
                    kept.push_back(r);
                    keptSeam.push_back(onSeam ? 1 : 0);
                }
            }
            roots.swap(kept);
            seam.swap(keptSeam);
        } else if (ctx.mirror.on()) {
            seam.assign(roots.size(), 1);  // given roots: each grown on its own, none mirrored
        }
        // Which roots hair covers: seen by some view (or mirror) with the mask over them.
        status.assign(roots.size(), 0);
        parallelFor(0, roots.size(), [&](std::size_t i) {
            const Vec3f at = roots[i].position + roots[i].normal * (ctx.offset + ctx.step);
            const Evidence e = ctx.evidence(at);
            if (e.seen == 0) status[i] = 1;
            else if (e.inside == 0 || !ctx.rootCovered(at)) status[i] = 2;
        }, 4);
    };
    spread(options.guideCount);
    if (options.roots.empty()) {
        // The guides wanted where the hair is: when many roots fall where there is none (a scalp
        // that is the whole head, the face bare), spread again, as much denser as they fell short.
        const std::size_t hair = static_cast<std::size_t>(std::count(status.begin(), status.end(), 0));
        if (hair > 0 && static_cast<double>(hair) < 0.8 * static_cast<double>(roots.size()))
            spread(static_cast<int>(std::lround(options.guideCount * std::min(5.0, static_cast<double>(roots.size()) / static_cast<double>(hair)))));
    }
    result.rootsTried = static_cast<int>(roots.size());

    // Each root's length: the drawn guide lines' where they say it (blended in by how much they
    // say it), else the longest. Both ways from every root.
    std::vector<float> rootLength(roots.size(), ctx.maxLength), photoLength(roots.size(), 0.0f);
    std::vector<std::uint8_t> drawnLength(roots.size(), 0);
    std::vector<Grown> ways[2];
    ways[0].resize(roots.size());
    ways[1].resize(roots.size());
    parallelFor(0, roots.size(), [&](std::size_t i) {
        if (status[i] != 0) return;
        float say = 0.0f;
        const float drawn = hintedLength(prepared, roots[i].position + roots[i].normal * (ctx.offset + ctx.step), say);
        if (say > 0.05f && drawn > 0.0f) {
            rootLength[i] = std::max(3.0f * ctx.step, ctx.maxLength + say * (std::min(drawn, ctx.maxLength) - ctx.maxLength));
            drawnLength[i] = 1;
        }
        // (automatic: no longer than the longest strand path the views show through the root's spot,
        // a little more: hair over the top long, tucked sides and the nape short)
        if (!drawnLength[i] && options.maxLength <= 0.0f) {
            const float local = localStrandLength(prepared, roots[i].position + roots[i].normal * (ctx.offset + ctx.step));
            photoLength[i] = local;
            if (local > 0.0f) rootLength[i] = std::clamp(local, 6.0f * ctx.step, rootLength[i]);
        }
        ways[0][i] = grow(ctx, roots[i], 1.0f, rootLength[i]);
        ways[1][i] = grow(ctx, roots[i], -1.0f, rootLength[i]);
    }, 1);

    // The way: the one with more evidence along it (a little toward gravity), then the
    // neighbours' vote (hair near hair runs alike).
    std::vector<std::size_t> live;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (status[i] == 1) ++result.rootsUnseen;
        else if (status[i] == 2) ++result.rootsBald;
        else live.push_back(i);
    }
    const Vec3f gravity = -options.up.normalized();
    bool hinted = false;
    for (const PreparedView& v : prepared) hinted = hinted || !v.hintWay.empty();
    std::vector<float> prefer[2];
    prefer[0].assign(roots.size(), 0.0f);
    prefer[1].assign(roots.size(), 0.0f);
    std::vector<int> choice(roots.size(), 0);
    for (std::size_t i : live) {
        const float a = ways[0][i].score, b = ways[1][i].score;
        const float sum = std::max(1e-9f, a + b);
        prefer[0][i] = a / sum + options.downBias * ways[0][i].tangent.dot(gravity);
        prefer[1][i] = b / sum + options.downBias * ways[1][i].tangent.dot(gravity);
        // (a way the photos show hair along twice as far as the other wins over falling: long hair
        // swept back from the forehead, not a fringe over it)
        if (a > 2.0f * b) prefer[0][i] += options.downBias;
        if (b > 2.0f * a) prefer[1][i] += options.downBias;
        if (hinted) {
            for (int w = 0; w < 2; ++w) {
                const std::vector<Vec3f>& pts = ways[w][i].points;
                float agree = hintAgreement(prepared, pts);
                // With symmetry the guide's mirror image is a guide too: the hints there count.
                if (ctx.mirror.on() && !seam[i]) {
                    std::vector<Vec3f> mirrored;
                    for (std::size_t k = 0; k < std::min<std::size_t>(pts.size(), 8); ++k) mirrored.push_back(ctx.mirror.point(pts[k]));
                    agree += hintAgreement(prepared, mirrored);
                }
                prefer[w][i] += options.hintWeight * agree;
            }
        }
        choice[i] = prefer[1][i] > prefer[0][i] ? 1 : 0;
    }
    std::vector<std::vector<std::size_t>> neighbours(roots.size());
    const std::size_t k = 6;
    for (std::size_t i : live) {
        std::vector<std::pair<float, std::size_t>> d;
        for (std::size_t j : live)
            if (j != i) d.push_back({(roots[i].position - roots[j].position).length(), j});
        const std::size_t take = std::min(k, d.size());
        std::partial_sort(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(take), d.end());
        for (std::size_t t = 0; t < take; ++t)
            if (d[t].first < 3.0f * spacing) neighbours[i].push_back(d[t].second);
    }
    for (int iteration = 0; iteration < 6; ++iteration) {
        std::vector<int> next = choice;
        for (std::size_t i : live) {
            float vote[2] = {prefer[0][i], prefer[1][i]};
            for (int s = 0; s < 2; ++s) {
                float agree = 0.0f;
                for (std::size_t j : neighbours[i]) agree += ways[s][i].tangent.dot(ways[choice[j]][j].tangent);
                if (!neighbours[i].empty()) vote[s] += 0.6f * agree / static_cast<float>(neighbours[i].size());
            }
            next[i] = vote[1] > vote[0] ? 1 : 0;
        }
        choice.swap(next);
    }

    // The guides: smoothed, lifted, each following its lock's curl (curly hair), resampled; mirrored
    // pairs with symmetry.
    const float minLength = options.minLength > 0 ? options.minLength : 3.0f * ctx.step;
    std::vector<ReferenceGuide> grownGuides;
    std::vector<std::vector<Vec3f>> paths;
    std::vector<std::uint8_t> grownSeam;
    // Each root's grown guide: smoothed, lifted, cut to its length (empty: too short).
    std::vector<std::vector<Vec3f>> grownPaths(roots.size());
    parallelFor(0, live.size(), [&](std::size_t n) {
        const std::size_t i = live[n];
        const Grown& g = ways[choice[i]][i];
        if (g.points.size() < 3 || arcLength(g.points) < minLength) return;
        std::vector<Vec3f> path = volumized(ctx, smoothed(g.points, 3), options.volume);
        // A guide that reached the given maximum never showed its tip (hidden under the hair above
        // it): hair is mostly shorter than the longest, so it is trimmed (calibrated on the synthetic
        // heads: such guides came out 1.15x too long).
        if (options.maxLength > 0.0f && g.stop == "length" && options.lengthTrim < 1.0f && !drawnLength[i])
            path = truncated(path, options.lengthTrim * arcLength(path));
        // (an automatic length: lifted for volume it is not much longer than it grew)
        if (options.maxLength <= 0.0f && !drawnLength[i] && arcLength(path) > 1.15f * rootLength[i]) path = truncated(path, 1.15f * rootLength[i]);
        path[0] = roots[i].position;
        grownPaths[i] = std::move(path);
    }, 4);
    // Lock paths (locks as objects): each lock's 3D centreline (placeLocks) takes the place of the
    // nearest root's grown guide where the views support it about as much (PhotoScorer's support: the
    // length along which the coarse and fine flow agree with it, weighted by their confidence, less
    // what runs across them or off the hair mask, points the head hides left out; the view it was
    // traced in left out); the rest grow. With symmetry a lock on the mirrored side competes as its
    // mirror image.
    std::vector<int> lockViewOf;
    // The head the guides stay out of (rooting): the first occluder (a whole head), else the scalp.
    const ScalpMesh* headMesh = nullptr;
    for (const auto& o : options.occluders)
        if (o && !headMesh) headMesh = o.get();
    GuideRootingOptions rooting = options.rooting;
    rooting.up = options.up;
    const bool lockGuides = lockPaths && options.roots.empty();
    std::vector<int> lockAt(roots.size(), -1);
    std::vector<PlacedLock> placed;
    if (lockGuides) {
        placed = placeLocks(ctx, lockSeeds, result);
        for (const auto& c : result.lockViews) result.locksDetected += c.detected;
        // (with symmetry the guides on the other side are the grown side's mirrored: a lock there is
        // mirrored over to compete for a root on the grown side, as its mirror image)
        if (ctx.mirror.on())
            for (PlacedLock& l : placed) {
                const Vec3f m = ctx.mirror.point(l.root.position);
                if ((m - l.root.position).length() < 0.5f * spacing || l.root.position.x >= m.x) continue;
                for (Vec3f& p : l.path) p = ctx.mirror.point(p);
                l.root = scalp.closestPoint(m);
            }
        // (each lock checked against all the views: cut where it leaves the hair they show for a few
        // steps running, dropped when that leaves less than half of it; smoothed, kept off the scalp;
        // rooted on the scalp: the end nearer it first, joined to it, out of the head)
        const GuideRootingCounts rootScale = guideRootingScale(scalp, rooting);
        std::vector<RootedGuide> rooted(placed.size());
        parallelFor(0, placed.size(), [&](std::size_t k) {
            std::vector<Vec3f>& path = placed[k].path;
            const float before = arcLength(path);
            int bad = 0;
            for (std::size_t i = 1; i < path.size(); ++i) {
                const Evidence e = ctx.evidence(path[i]);
                const bool out = (e.outside > 0 && e.outside >= e.inside) || (ctx.shell && !ctx.shell->inside(path[i]));
                bad = out ? bad + 1 : 0;
                if (bad >= 3) {
                    path.resize(i - 2);
                    break;
                }
            }
            if (path.size() < 4 || arcLength(path) < 0.5f * before || arcLength(path) < minLength) {
                path.clear();
                return;
            }
            path = keptOut(ctx, smoothed(std::move(path), 3));
            rooted[k] = rootGuide(path, scalp, headMesh, rooting, rootScale);
            if (rooted[k].points.size() < 4 || arcLength(rooted[k].points) < minLength) {
                path.clear();
                return;
            }
            path = rooted[k].points;
            placed[k].root = rooted[k].root;
            if (arcLength(path) > 2.0f * size) path = truncated(path, 2.0f * size);
            const GuideScore sc = scoreOnViews(prepared, path, GuideScoreOptions{}, placed[k].view);
            placed[k].agreement = sc.score;
            placed[k].support = sc.support;
        }, 4);
        result.lockRooting = rootScale;
        for (const RootedGuide& g : rooted) {
            ++result.lockRooting.guides;
            result.lockRooting.flipped += g.flipped;
            result.lockRooting.snapped += g.snapped;
            result.lockRooting.connected += g.connected;
            result.lockRooting.pushed += g.pushed;
            result.lockRooting.cut += g.cut;
            result.lockRooting.droppedGap += g.droppedGap;
            result.lockRooting.droppedShort += g.droppedShort;
        }
        result.lockRooting.dropped = result.lockRooting.droppedGap + result.lockRooting.droppedShort;
        placed.erase(std::remove_if(placed.begin(), placed.end(), [](const PlacedLock& l) { return l.path.size() < 4; }), placed.end());
        // (the views' best scored first; each takes the nearest root not taken, within 0.6 spacing)
        std::vector<std::size_t> order(placed.size());
        for (std::size_t q = 0; q < order.size(); ++q) order[q] = q;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return placed[a].agreement > placed[b].agreement; });
        for (std::size_t q : order) {
            const PlacedLock& l = placed[q];
            std::size_t best = roots.size();
            float bestDistance = 0.6f * spacing;
            for (std::size_t i : live) {
                if (lockAt[i] >= 0) continue;
                const float d = (roots[i].position - l.root.position).length();
                if (d < bestDistance) bestDistance = d, best = i;
            }
            if (best == roots.size()) continue;
            const GuideScore gs = grownPaths[best].empty() ? GuideScore{} : scoreOnViews(prepared, grownPaths[best], GuideScoreOptions{}, l.view);
            if (grownPaths[best].empty() || l.support >= kLockSupport * gs.support) lockAt[best] = static_cast<int>(q);
        }
    }
    for (std::size_t i : live) {
        if (lockAt[i] >= 0) {
            const PlacedLock& l = placed[static_cast<std::size_t>(lockAt[i])];
            ReferenceGuide guide;
            guide.root = l.root.root;
            guide.confidence = std::clamp(l.agreement, 0.0f, 1.0f);
            guide.stop = l.triangulated ? "lock" : "lock (one view)";
            guide.photoLength = localStrandLength(prepared, l.root.position + l.root.normal * (ctx.offset + ctx.step));
            grownGuides.push_back(std::move(guide));
            paths.push_back(l.path);
            grownSeam.push_back(seam[i]);
            lockViewOf.push_back(l.view);
            ++result.lockGuides;
            continue;
        }
        if (grownPaths[i].empty()) {
            ++result.rootsShort;
            continue;
        }
        const Grown& g = ways[choice[i]][i];
        ReferenceGuide guide;
        guide.root = roots[i].root;
        guide.confidence = g.confidence;
        guide.stop = g.stop;
        guide.photoLength = photoLength[i];
        grownGuides.push_back(std::move(guide));
        paths.push_back(std::move(grownPaths[i]));
        grownSeam.push_back(seam[i]);
        lockViewOf.push_back(-1);
    }
    result.lockRoots = std::max(result.lockRoots, result.lockGuides);
    if (options.lockCurls && result.look.curly) bakeLocks(prepared, scalp, options, grownGuides, paths, result);
    else if (options.lockCurls && lockPaths && result.look.wavy) waveLocks(prepared, options, grownGuides, paths, lockViewOf, result);
    // Every guide rooted on the scalp (guide_rooting.h): grown guides start on it and the locks were
    // rooted already; what is left is a pass through the head (cut, or a shallow dip pushed out) and
    // the guides that leaves too short (dropped). Before mirroring: a mirror is its rooted guide's.
    {
        std::vector<std::size_t> kept;
        std::vector<SurfacePoint> rootsOn;
        result.rooting = rootGuides(paths, scalp, headMesh, rooting, &kept, &rootsOn);
        std::vector<ReferenceGuide> keptGuides;
        std::vector<std::uint8_t> keptSeam;
        std::vector<int> keptView;
        for (std::size_t q = 0; q < kept.size(); ++q) {
            keptGuides.push_back(std::move(grownGuides[kept[q]]));
            keptGuides.back().root = rootsOn[q].root;
            keptSeam.push_back(grownSeam[kept[q]]);
            keptView.push_back(lockViewOf[kept[q]]);
        }
        grownGuides.swap(keptGuides);
        grownSeam.swap(keptSeam);
        lockViewOf.swap(keptView);
    }
    for (std::size_t i = 0; i < grownGuides.size(); ++i) {
        std::size_t count = static_cast<std::size_t>(std::max(2, options.points));
        if (grownGuides[i].lockWavelength > 0.0f)
            count = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(12.0f * arcLength(paths[i]) / grownGuides[i].lockWavelength)), count, 128);
        const Vec3f root = paths[i][0];
        grownGuides[i].points = resampleByArcLength(paths[i].data(), paths[i].size(), count);
        grownGuides[i].points[0] = root;
    }
    for (std::size_t i = 0; i < grownGuides.size(); ++i) {
        result.guides.push_back(grownGuides[i]);
        if (!ctx.mirror.on() || grownSeam[i]) continue;
        const int original = static_cast<int>(result.guides.size()) - 1;
        ReferenceGuide m = grownGuides[i];
        for (Vec3f& p : m.points) p = ctx.mirror.point(p);
        const SurfacePoint sp = scalp.closestPoint(m.points[0]);
        if (sp.root.face < 0) continue;
        m.root = sp.root;
        m.points[0] = sp.position;
        m.mirrorOf = original;
        result.guides.push_back(std::move(m));
    }

    // Each guide's score against all the views (PhotoScorer's), and their mean.
    {
        std::vector<float> scores(result.guides.size(), 0.0f);
        parallelFor(0, result.guides.size(), [&](std::size_t i) { scores[i] = scoreOnViews(prepared, result.guides[i].points, GuideScoreOptions{}).score; }, 8);
        double sum = 0.0;
        for (std::size_t i = 0; i < scores.size(); ++i) {
            result.guides[i].score = scores[i];
            sum += scores[i];
        }
        result.score = scores.empty() ? 0.0f : static_cast<float>(sum / static_cast<double>(scores.size()));
    }
    {
        const ReferenceLookFit curls = result.look;
        result.look = fitLook(prepared, grownGuides, options);
        result.look.curly = curls.curly;
        result.look.wavy = curls.wavy;
        result.look.curlShare = curls.curlShare;
        result.look.curlAmplitudeDeg = curls.curlAmplitudeDeg;
        result.look.curlFrequency = curls.curlFrequency;
        result.look.curlRadius = curls.curlRadius;
        measureLook(prepared, ctx.maxLength, ctx.maxLength < size / 7.0f, result.look);
        result.scalpCoverage = scalpCoverage(scalp, prepared);
        double sum = 0.0;
        int seen = 0;
        for (float c : result.scalpCoverage)
            if (c >= 0.0f) {
                sum += c;
                ++seen;
            }
        result.look.coverage = seen ? static_cast<float>(sum / seen) : 0.0f;
    }
    if (options.keepFields)
        for (const PreparedView& v : prepared) {
            result.fields.push_back(*v.field);
            result.masks.push_back(v.mask);
            result.crops.push_back(v.crop);
        }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}


ReferenceViewPreview previewReferenceView(const ScalpMesh& scalp, const ReferenceView& view, const ReferenceGuideOptions& options,
                                          float lineSpacing) {
    if (scalp.triangleCount() == 0) throw std::invalid_argument("previewReferenceView: the scalp has no triangles");
    std::vector<PreparedView> prepared = prepareViews(scalp, {view}, options, scalpDiagonal(scalp));
    PreparedView& pv = prepared.front();
    ReferenceViewPreview out;
    out.mask = pv.mask;
    out.crop = pv.crop;
    if (pv.field && pv.field->width == pv.mask.width && pv.field->height == pv.mask.height) {
        out.confidence = pv.mask;
        for (std::size_t i = 0; i < out.confidence.pixels.size(); ++i)
            out.confidence.pixels[i] = pv.mask.pixels[i] >= 0.5f ? std::clamp(pv.field->confidence[i], 0.0f, 1.0f) : 0.0f;
    }
    if (!pv.field || pv.crop[2] <= 0 || pv.crop[3] <= 0) return out;
    const float sx = static_cast<float>(pv.width) / static_cast<float>(pv.crop[2]);
    const float sy = static_cast<float>(pv.height) / static_cast<float>(pv.crop[3]);
    // Waves (as guidesFromReference reads them, this view alone): the locks traced, the flow theirs.
    const float swing = pv.curl.amplitude * 180.0f / kPi;
    const bool curls = pv.curl.periodicShare > kCurlShare && swing > kCurlAmplitude;
    const bool waves = pv.curl.streamlines >= 5 && !curls && pv.curl.periodicShare > kWavyShare && swing > kWavyAmplitude;
    if (options.lockPaths > 0 || (options.lockPaths < 0 && waves)) {
        traceViewLocks(pv, blueNoiseRoots(scalp, std::clamp(2 * options.guideCount, 200, 2000), options.seed + 101), options.up.normalized(),
                       options.orientation.bilateralCoarse);
        if (pv.coarse) pv.field = pv.coarse;
        for (const LockLine& l : pv.lockLines) {
            if (!l.kept) continue;
            std::vector<std::array<float, 2>> line;
            for (const auto& q : l.line) line.push_back({q[0] / sx + static_cast<float>(pv.crop[0]), q[1] / sy + static_cast<float>(pv.crop[1])});
            out.locks.push_back(std::move(line));
        }
    }
    const float spacing = lineSpacing > 0.0f ? lineSpacing : std::max(4.0f, static_cast<float>(std::max(pv.crop[2], pv.crop[3])) / 48.0f);
    const float half = 0.42f * spacing;
    for (float y = static_cast<float>(pv.crop[1]) + 0.5f * spacing; y < static_cast<float>(pv.crop[1] + pv.crop[3]); y += spacing)
        for (float x = static_cast<float>(pv.crop[0]) + 0.5f * spacing; x < static_cast<float>(pv.crop[0] + pv.crop[2]); x += spacing) {
            const float u = (x - static_cast<float>(pv.crop[0])) * sx, v = (y - static_cast<float>(pv.crop[1])) * sy;
            if (!pv.inFrame(u, v) || pv.mask.pixels[pv.pixel(u, v)] < 0.5f) continue;
            float du = 0, dv = 0, conf = 0;
            if (!pv.field->sample(u, v, du, dv, conf) || conf < 0.08f) continue;
            out.flowLines.push_back({x - du * half, y - dv * half, x + du * half, y + dv * half});
        }
    return out;
}

// ---- Scoring guides against the photos ----------------------------------------------------------

struct PhotoScorer::Impl {
    std::vector<PreparedView> views;
    GuideScoreOptions scoring;
};

PhotoScorer::PhotoScorer(const ScalpMesh& scalp, const std::vector<ReferenceView>& views, const ReferenceGuideOptions& options,
                         const GuideScoreOptions& scoring)
    : impl_(std::make_unique<Impl>()) {
    if (scalp.triangleCount() == 0) throw std::invalid_argument("PhotoScorer: the scalp has no triangles");
    impl_->views = prepareViews(scalp, views, options, scalpDiagonal(scalp));
    impl_->scoring = scoring;
}

PhotoScorer::~PhotoScorer() = default;

std::size_t PhotoScorer::viewCount() const { return impl_->views.size(); }

GuideScore PhotoScorer::score(const std::vector<Vec3f>& points) const { return scoreOnViews(impl_->views, points, impl_->scoring); }

std::vector<GuideScore> PhotoScorer::score(const std::vector<std::vector<Vec3f>>& guides) const {
    std::vector<GuideScore> out(guides.size());
    parallelFor(0, guides.size(), [&](std::size_t i) { out[i] = scoreOnViews(impl_->views, guides[i], impl_->scoring); }, 4);
    return out;
}

// ---- Where the photos show hair on a head --------------------------------------------------------

std::vector<float> photoHairCover(const ScalpMesh& scalp, const ScalpMesh& head, const std::vector<ReferenceView>& views,
                                  const ReferenceGuideOptions& options) {
    if (scalp.triangleCount() == 0 || head.triangleCount() == 0) throw std::invalid_argument("photoHairCover: an empty mesh");
    ReferenceGuideOptions o = options;
    o.occluders.clear();
    o.occluders.push_back(std::make_shared<ScalpMesh>(head));
    const std::vector<PreparedView> prepared = prepareViews(scalp, views, o, scalpDiagonal(scalp));
    const std::vector<Vec3f>& normals = head.vertexNormals();
    std::vector<float> hair(head.pointCount(), 0.0f), seen(head.pointCount(), 0.0f);
    for (const PreparedView& pv : prepared) {
        if (pv.mask.empty() || pv.depth.empty()) continue;
        const Vec3f eye = pv.camera.eye();
        for (std::size_t i = 0; i < head.pointCount(); ++i) {
            const Vec3f& p = head.points()[i];
            const Vec3f toEye = (eye - p).normalized();
            // Seen from the front: an edge-on point's pixel is the hair standing past the head.
            const float facing = i < normals.size() ? normals[i].normalized().dot(toEye) : 1.0f;
            if (facing < 0.35f) continue;
            float u = 0, v = 0, z = 0;
            if (!pv.camera.project(p, u, v, &z) || !pv.inFrame(u, v) || pv.hidden(u, v, z)) continue;
            seen[i] += facing;
            hair[i] += facing * (pv.mask.sample(u, v) >= 0.5f ? 1.0f : 0.0f);
        }
    }
    std::vector<float> out(head.pointCount(), -1.0f);
    for (std::size_t i = 0; i < out.size(); ++i)
        if (seen[i] > 0.0f) out[i] = hair[i] / seen[i];
    return out;
}

// ---- How a groom compares with a photo ----------------------------------------------------------

std::vector<std::uint8_t> photoHairMask(const ScalpMesh& scalp, const ReferenceView& view, const ReferenceGuideOptions& options) {
    const int w = view.image.width, h = view.image.height;
    if (w <= 0 || h <= 0) throw std::invalid_argument("photoHairMask: the photo is empty");
    const ReferenceViewPreview preview = previewReferenceView(scalp, view, options, 1e9f);
    std::vector<std::uint8_t> hair(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    if (!preview.mask.empty() && preview.crop[2] > 0 && preview.crop[3] > 0) {
        const auto& c = preview.crop;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float mu = (static_cast<float>(x) + 0.5f - static_cast<float>(c[0])) / static_cast<float>(c[2]) * static_cast<float>(preview.mask.width);
                const float mv = (static_cast<float>(y) + 0.5f - static_cast<float>(c[1])) / static_cast<float>(c[3]) * static_cast<float>(preview.mask.height);
                if (mu < 0.0f || mv < 0.0f || mu >= static_cast<float>(preview.mask.width) || mv >= static_cast<float>(preview.mask.height)) continue;
                if (preview.mask.sample(mu, mv) >= 0.5f) hair[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
            }
    }
    return hair;
}

std::vector<float> meshDepthMap(const ScalpMesh& mesh, const RefCamera& camera, int width, int height) {
    std::vector<float> depth(static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)), kInf);
    if (!depth.empty() && mesh.triangleCount() > 0) rasterize(mesh, projectMesh(mesh, camera), width, height, depth, nullptr);
    return depth;
}

PhotoComparison compareToPhoto(const ScalpMesh& scalp, const StrandSet& strands, const ReferenceView& view,
                               const ReferenceGuideOptions& options) {
    if (scalp.triangleCount() == 0) throw std::invalid_argument("compareToPhoto: the scalp has no triangles");
    PhotoComparison out;
    const int w = view.image.width, h = view.image.height;
    if (w <= 0 || h <= 0) throw std::invalid_argument("compareToPhoto: the photo is empty");
    out.width = w;
    out.height = h;
    const RefCamera camera = view.camera.width > 0.0 && std::abs(view.camera.width - w) > 0.5 ? view.camera.scaled(w / view.camera.width) : view.camera;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    // The photo's hair, at its size.
    out.photoHair = photoHairMask(scalp, view, options);
    // The groom's: the strands through the camera, behind the scalp hidden.
    std::vector<float> depth = meshDepthMap(scalp, camera, w, h);
    // The photo's hair that is the groom's: its pieces over the scalp (a dark patch of backdrop
    // in a corner is not).
    {
        std::vector<int> label(n, -1);
        std::vector<std::size_t> stack, piece;
        for (std::size_t s = 0; s < n; ++s) {
            if (!out.photoHair[s] || label[s] >= 0) continue;
            bool overScalp = false;
            piece.clear();
            stack.push_back(s);
            label[s] = 1;
            while (!stack.empty()) {
                const std::size_t i = stack.back();
                stack.pop_back();
                piece.push_back(i);
                overScalp = overScalp || depth[i] < kInf;
                const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    const std::size_t j = static_cast<std::size_t>(ny[k]) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx[k]);
                    if (out.photoHair[j] && label[j] < 0) {
                        label[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
            if (!overScalp)
                for (std::size_t i : piece) out.photoHair[i] = 0;
        }
    }
    // Where an occluder shows in front of the scalp (a whole head's face, a beard on it): neither
    // mask counts there (the hair a groom grows is the scalp's).
    std::vector<std::uint8_t> occluded;
    if (!options.occluders.empty()) {
        const std::vector<float> scalpOnly = depth;
        for (const auto& occluder : options.occluders)
            if (occluder) rasterize(*occluder, projectMesh(*occluder, camera), w, h, depth, nullptr);
        occluded.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) occluded[i] = depth[i] < kInf && depth[i] < scalpOnly[i] - 1e-6f ? 1 : 0;
        for (std::size_t i = 0; i < n; ++i)
            if (occluded[i]) out.photoHair[i] = 0;
    }
    std::vector<std::uint8_t> drawn(n, 0);
    const float radius = std::max(1.0f, 0.003f * static_cast<float>(w));
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const Vec3f* p = strands.strandPoints(s);
        const std::size_t count = strands.strandPointCount(s);
        for (std::size_t i = 1; i < count; ++i) {
            float u0 = 0, v0 = 0, z0 = 0, u1 = 0, v1 = 0, z1 = 0;
            if (!camera.project(p[i - 1], u0, v0, &z0) || !camera.project(p[i], u1, v1, &z1)) continue;
            const float len = std::max(std::abs(u1 - u0), std::abs(v1 - v0));
            const int steps = std::max(1, static_cast<int>(std::ceil(len)));
            for (int k = 0; k <= steps; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(steps);
                const float u = u0 + (u1 - u0) * t, v = v0 + (v1 - v0) * t, z = z0 + (z1 - z0) * t;
                const int r = static_cast<int>(std::ceil(radius));
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (static_cast<float>(dx * dx + dy * dy) > radius * radius + 0.25f) continue;
                        const int x = static_cast<int>(u) + dx, y = static_cast<int>(v) + dy;
                        if (x < 0 || y < 0 || x >= w || y >= h) continue;
                        const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                        if (z > depth[at] + 0.002f * z) continue;  // (behind the head)
                        drawn[at] = 1;
                    }
            }
        }
    }
    // The gaps between strands closed (a silhouette, as the photo's mask is one).
    const int close = std::max(1, w / 128);
    {
        RefImage m;
        m.width = w;
        m.height = h;
        m.channels = 1;
        m.pixels.resize(n);
        for (std::size_t i = 0; i < n; ++i) m.pixels[i] = drawn[i] ? 1.0f : 0.0f;
        const RefImage closed = shrunk(grown(m, close), close);
        out.groomHair.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) out.groomHair[i] = (closed.pixels[i] >= 0.5f || drawn[i]) && (occluded.empty() || !occluded[i]) ? 1 : 0;
    }
    for (std::size_t i = 0; i < n; ++i) {
        out.photoPixels += out.photoHair[i];
        out.groomPixels += out.groomHair[i];
        out.both += out.photoHair[i] & out.groomHair[i];
    }
    const int any = out.photoPixels + out.groomPixels - out.both;
    out.iou = any > 0 ? static_cast<double>(out.both) / any : 0.0;
    out.missedShare = out.photoPixels ? 1.0 - static_cast<double>(out.both) / out.photoPixels : 0.0;
    out.extraShare = out.groomPixels ? 1.0 - static_cast<double>(out.both) / out.groomPixels : 0.0;
    // The strands' flow against the photo's: each strand drawn a pixel wide in its own shade (the
    // nearest in front), both pictures' fine strand directions (orientationField), compared where
    // both show hair, each pixel weighing the lesser confidence: curls in step read alike.
    if (view.image.channels >= 1 && out.both > 0) {
        // (drawn at twice the size and halved: smooth lines, as a photo's strands; aliased steps would
        // read as directions of their own)
        const int w2 = 2 * w, h2 = 2 * h;
        RefImage big;
        big.width = w2;
        big.height = h2;
        big.channels = 1;
        big.pixels.assign(static_cast<std::size_t>(w2) * static_cast<std::size_t>(h2), 0.6f);
        std::vector<float> nearest(big.pixels.size(), kInf);
        for (std::size_t s = 0; s < strands.strandCount(); ++s) {
            const float shade = 0.1f + 0.3f * static_cast<float>((s * 2654435761u >> 8) % 1000u) / 1000.0f;
            const Vec3f* p = strands.strandPoints(s);
            const std::size_t count = strands.strandPointCount(s);
            for (std::size_t i = 1; i < count; ++i) {
                float u0 = 0, v0 = 0, z0 = 0, u1 = 0, v1 = 0, z1 = 0;
                if (!camera.project(p[i - 1], u0, v0, &z0) || !camera.project(p[i], u1, v1, &z1)) continue;
                const int steps = std::max(1, static_cast<int>(std::ceil(4.0f * std::max(std::abs(u1 - u0), std::abs(v1 - v0)))));
                for (int k = 0; k <= steps; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(steps);
                    const float u = u0 + (u1 - u0) * t, v = v0 + (v1 - v0) * t;
                    if (u < 0.0f || v < 0.0f || u >= static_cast<float>(w) || v >= static_cast<float>(h)) continue;
                    const std::size_t at = static_cast<std::size_t>(v) * static_cast<std::size_t>(w) + static_cast<std::size_t>(u);
                    const float z = z0 + (z1 - z0) * t;
                    if (z > depth[at] + 0.002f * z) continue;
                    const std::size_t at2 = static_cast<std::size_t>(2.0f * v) * static_cast<std::size_t>(w2) + static_cast<std::size_t>(2.0f * u);
                    if (z > nearest[at2]) continue;
                    nearest[at2] = z;
                    big.pixels[at2] = shade;
                }
            }
        }
        const RefImage render = big.resized(w, h);
        // Each picture's strand directions from its brightness gradients (structureField), smoothed
        // over a couple of pixels.
        const float sigma = std::max(1.5f, static_cast<float>(std::max(w, h)) / 250.0f);
        const OrientationField photoFlow = structureField(view.image, sigma), groomFlow = structureField(render, sigma);
        double sum = 0.0, aligned = 0.0, weight = 0.0;
        for (std::size_t q = 0; q < n; ++q) {
            if (!out.photoHair[q] || !out.groomHair[q]) continue;
            const double c = std::min(photoFlow.confidence[q], groomFlow.confidence[q]);
            if (c <= 0.05) continue;
            float d = std::fmod(std::abs(photoFlow.angle[q] - groomFlow.angle[q]), kPi);
            if (d > 0.5f * kPi) d = kPi - d;
            sum += c * std::cos(2.0 * d);
            aligned += d < 20.0f * kPi / 180.0f ? c : 0.0;
            weight += c;
            ++out.flowPixels;
        }
        if (weight > 0.0) {
            out.flowAgreement = sum / weight;
            out.flowAligned = aligned / weight;
        }
    }
    // Round the head: each direction's farthest hair pixel from the head's middle, the photo's and the groom's.
    Vec3f lo, hi;
    lo = hi = scalp.points().front();
    for (const Vec3f& q : scalp.points()) lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)}, hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    const Vec3f middle = (lo + hi) * 0.5f;
    float cu = 0.5f * static_cast<float>(w), cv = 0.5f * static_cast<float>(h), cz = 1.0f;
    camera.project(middle, cu, cv, &cz);
    out.unitsPerPixel = cz / camera.focalPx;
    constexpr int kBins = 72;
    std::vector<float> reachPhoto(kBins, -1.0f), reachGroom(kBins, -1.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (!out.photoHair[at] && !out.groomHair[at]) continue;
            const float dx = static_cast<float>(x) + 0.5f - cu, dy = cv - (static_cast<float>(y) + 0.5f);
            const float angle = std::atan2(dx, dy);  // 0 up, positive toward the picture's right
            const int bin = std::clamp(static_cast<int>((angle + kPi) / (2.0f * kPi) * kBins), 0, kBins - 1);
            const float r = std::sqrt(dx * dx + dy * dy);
            if (out.photoHair[at]) reachPhoto[static_cast<std::size_t>(bin)] = std::max(reachPhoto[static_cast<std::size_t>(bin)], r);
            if (out.groomHair[at]) reachGroom[static_cast<std::size_t>(bin)] = std::max(reachGroom[static_cast<std::size_t>(bin)], r);
        }
    const char* names[4] = {"top", "right", "lower", "left"};
    for (int region = 0; region < 4; ++region) {
        PhotoComparison::Region rg;
        rg.name = names[region];
        int counted = 0;
        for (int b = 0; b < kBins; ++b) {
            const float angle = (static_cast<float>(b) + 0.5f) / kBins * 2.0f * kPi - kPi;  // (0 up)
            const float a = std::fmod(angle + 2.0f * kPi + 0.25f * kPi, 2.0f * kPi);           // 0..2pi from up-left
            if (static_cast<int>(a / (0.5f * kPi)) != region) continue;
            const float p = reachPhoto[static_cast<std::size_t>(b)], g = reachGroom[static_cast<std::size_t>(b)];
            if (p < 0.0f && g < 0.0f) continue;
            const float d = (std::max(p, 0.0f) - std::max(g, 0.0f)) * static_cast<float>(out.unitsPerPixel);
            if (d > 0.0f) rg.missing += d;
            else rg.extra -= d;
            ++counted;
        }
        if (counted) rg.missing /= counted, rg.extra /= counted;
        out.regions.push_back(rg);
    }
    // The hairline: down each column of the head's middle third, where the hair from the top ends
    // on the head (the face, an ear, the neck below it, not the backdrop), each mask's small holes
    // closed (curls); the median of the groom's against the photo's.
    {
        const int holes = std::max(1, w / 150);
        auto closedMask = [&](const std::vector<std::uint8_t>& m) {
            RefImage im;
            im.width = w;
            im.height = h;
            im.channels = 1;
            im.pixels.resize(n);
            for (std::size_t i = 0; i < n; ++i) im.pixels[i] = m[i] ? 1.0f : 0.0f;
            return shrunk(grown(im, holes), holes);
        };
        const RefImage photoClosed = closedMask(out.photoHair), groomClosed = closedMask(out.groomHair);
        int x0 = w, x1 = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] < kInf) x0 = std::min(x0, x), x1 = std::max(x1, x);
        auto hairEnd = [&](const RefImage& m, int x) {
            int y = 0;
            while (y < h && m.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] < 0.5f) ++y;
            if (y >= h || static_cast<float>(y) > cv) return -1;  // (no hair above the head's middle)
            while (y < h && m.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] >= 0.5f) ++y;
            if (y >= h || depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] >= kInf) return -1;  // (off the head)
            return y;
        };
        std::vector<float> offsets;
        if (x1 > x0)
            for (int x = x0 + (x1 - x0) / 3; x <= x1 - (x1 - x0) / 3; ++x) {
                const int p = hairEnd(photoClosed, x), g = hairEnd(groomClosed, x);
                if (p >= 0 && g >= 0) offsets.push_back(static_cast<float>(g - p));
            }
        if (offsets.size() >= 3) {
            out.hairlineFound = true;
            out.hairlineOffset = median(offsets) * out.unitsPerPixel;
        }
    }
    // The colors: the photo's hair as the look reads it (inside the mask by two pixels, its
    // lookQuantile per channel, balanced by the backdrop's color and a dim photo brightened), the
    // strands' appearance.
    if (view.image.channels >= 3 && out.photoPixels > 0) {
        const std::size_t channels = static_cast<std::size_t>(view.image.channels);
        const std::array<float, 3> border = borderColor(view.image);
        float balance[3];
        const float grey = std::max(1e-3f, (srgbToLinear01(border[0]) + srgbToLinear01(border[1]) + srgbToLinear01(border[2])) / 3.0f);
        for (int c = 0; c < 3; ++c) balance[c] = grey / std::max(1e-3f, srgbToLinear01(border[static_cast<std::size_t>(c)]));
        std::vector<float> lum;
        lum.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            float l = 0.0f;
            const float k[3] = {0.2126f, 0.7152f, 0.0722f};
            for (int c = 0; c < 3; ++c) l += k[c] * balance[c] * srgbToLinear01(view.image.pixels[i * channels + static_cast<std::size_t>(c)]);
            lum.push_back(l);
        }
        float exposure = 1.0f;
        if (options.lookExposure && !lum.empty()) {
            const std::size_t at = static_cast<std::size_t>(0.995 * static_cast<double>(lum.size() - 1));
            std::nth_element(lum.begin(), lum.begin() + static_cast<std::ptrdiff_t>(at), lum.end());
            exposure = std::clamp(kPhotoWhite / std::max(lum[at], 1e-4f), 1.0f, 4.0f);
        }
        auto hair = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && out.photoHair[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
        std::vector<float> ch[3];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!hair(x, y) || !hair(x - 2, y) || !hair(x + 2, y) || !hair(x, y - 2) || !hair(x, y + 2)) continue;
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                for (int c = 0; c < 3; ++c) ch[c].push_back(srgbToLinear01(view.image.pixels[i * channels + static_cast<std::size_t>(c)]) * balance[c] * exposure);
            }
        if (!ch[0].empty()) {
            for (int c = 0; c < 3; ++c) {
                const std::size_t at = static_cast<std::size_t>(options.lookQuantile * static_cast<double>(ch[c].size() - 1));
                std::nth_element(ch[c].begin(), ch[c].begin() + static_cast<std::ptrdiff_t>(at), ch[c].end());
                out.photoColor[static_cast<std::size_t>(c)] = std::clamp(ch[c][at], 1e-4f, 1.0f);
            }
            nearestMelanin(out.photoColor, out.photoMelanin, out.photoRedness);
        }
    }
    if (strands.strandCount() > 0) {
        const std::vector<std::array<float, 3>> colors = strandAppearanceColors(strands);
        std::array<double, 3> sum{0, 0, 0};
        for (const auto& c : colors)
            for (int k = 0; k < 3; ++k) sum[static_cast<std::size_t>(k)] += c[static_cast<std::size_t>(k)];
        for (int k = 0; k < 3; ++k) out.groomColor[static_cast<std::size_t>(k)] = static_cast<float>(sum[static_cast<std::size_t>(k)] / std::max<std::size_t>(1, colors.size()));
        nearestMelanin(out.groomColor, out.groomMelanin, out.groomRedness);
    }
    return out;
}

}  // namespace groomlab
