// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos, part 2: the views prepared (working size, masks, what the scalp and
// occluders hide), the shell, the grower, symmetry and the look. See reference_guides.h.

#include "groomlab/portable_math.h"
#include "groomlab/reference_guides.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "groomlab/brush.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mirror.h"

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
// Wavy: a smaller share swinging, as widely. Not curly enough for a Curl node, but guides along each
// wave wander inside the hair and never reach its ends: they follow the coarse flow too.
constexpr float kWavyShare = 0.12f, kWavyAmplitude = 15.0f;

float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : pm::pow((c + 0.055f) / 1.055f, 2.4f);
}

float median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    return values[mid];
}

}  // namespace

// ---- Prepared views -------------------------------------------------------------------------------

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
    CurlEstimate curl;
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
                 const std::array<float, 2>& up, const std::vector<std::uint8_t>& seedsIn, const std::vector<std::uint8_t>& notHair) {
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
        seeds[i] = (sil[i] && !scalpCover[i] && fromScalp[i] > 2.0f && height(i) > mid) ? 1 : 0;
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
    for (const ReferenceView& original : views) {
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
            const float longest = options.maxLength > 0.0f ? options.maxLength : 0.3f * scalpSize;
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
        for (const auto& occluder : options.occluders)
            if (occluder) rasterize(*occluder, projectMesh(*occluder, pv.camera), pv.width, pv.height, pv.depth, nullptr);
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
                byColor = hairByColor(pv.rgb, pv.mask, subject, pv.scalpCover, up, seedPixels, sureBackdrop);
            }
            if (!byColor) pv.mask = refineMask(pv.rgb, pv.mask, seeds, sureBackdrop, likelyHair);
            touchUp();
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
            pv.coarse = std::make_shared<OrientationField>(coarseField(*pv.field, pv.mask, 0.02f * static_cast<float>(std::max(pv.width, pv.height))));
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
        out.push_back(std::move(pv));
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
    const float lengthSigma = sigma;
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
                    const float kn = pm::exp(-0.5f * (dx * dx + dy * dy) / (lengthSigma * lengthSigma));
                    length[at] += kn * s.remain;
                    lengthWeight[at] += kn;
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
            pv.hintLength[2 * i + 1] = std::min(1.0f, lengthWeight[i] / std::max(1.0f, lengthSigma));
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
            lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
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
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
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
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
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

Vec3f limitTurn(const Vec3f& from, const Vec3f& to, float maxAngle) {
    const float c = std::clamp(from.dot(to), -1.0f, 1.0f);
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
    for (int a = 0; a < 12; ++a) {
        const float angle = kPi * static_cast<float>(a) / 12.0f;
        const Vec3f axis = downSurface * pm::cos(angle) + side * pm::sin(angle);
        std::vector<Evidence> line;
        double score = 0.0;
        for (int k = -reach; k <= reach; ++k) {
            const Evidence e = ctx.evidence(at + axis * (static_cast<float>(k) * ctx.step));
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
    g.stop = "length";
    for (int k = 0; k < maxSteps; ++k) {
        const Evidence e = ctx.evidence(p);
        if (e.outside > 0 && e.outside >= e.inside) {
            g.stop = "mask";
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
        next = limitTurn(d, next, maxTurn);
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

float polylineLength(const std::vector<Vec3f>& pts) {
    float l = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) l += (pts[i] - pts[i - 1]).length();
    return l;
}

std::vector<Vec3f> resampled(const std::vector<Vec3f>& pts, int count) {
    std::vector<Vec3f> out;
    if (pts.size() < 2 || count < 2) return pts;
    const float total = polylineLength(pts);
    std::size_t seg = 1;
    float before = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float s = total * static_cast<float>(i) / static_cast<float>(count - 1);
        while (seg + 1 < pts.size() && before + (pts[seg] - pts[seg - 1]).length() < s) {
            before += (pts[seg] - pts[seg - 1]).length();
            ++seg;
        }
        const float len = (pts[seg] - pts[seg - 1]).length();
        const float f = len > 1e-12f ? std::clamp((s - before) / len, 0.0f, 1.0f) : 0.0f;
        out.push_back(pts[seg - 1] * (1.0f - f) + pts[seg] * f);
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
float nearestMelanin(const std::array<float, 3>& appearance, float& melanin, float& redness) {
    double best = 1e30;
    for (int mi = 0; mi <= 50; ++mi)
        for (int ri = 0; ri <= 20; ++ri) {
            const float m = static_cast<float>(mi) / 50.0f, r = static_cast<float>(ri) / 20.0f;
            const auto a = hairAppearanceColor(melaninColor(m, r));
            double err = 0.0;
            for (int c = 0; c < 3; ++c) {
                const double d = pm::log(std::max(a[static_cast<std::size_t>(c)], 1e-4f)) -
                                 pm::log(std::max(appearance[static_cast<std::size_t>(c)], 1e-4f));
                err += d * d;
            }
            if (err < best) {
                best = err;
                melanin = m;
                redness = r;
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
        const RefImage core = v.mask;  // eroded by a pixel: no backdrop blended in
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
                rgb[c] = srgbToLinear(v.rgb.at(x, y, c));
                ch[c].push_back(rgb[c]);
            }
            luminance.push_back(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]);
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
                    rgb[c] = srgbToLinear(v.rgb.pixels[i * 3 + static_cast<std::size_t>(c)]);
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
    // The exposure: the photo's white point (its brightest 0.5%, balanced) taken as white, when the
    // photo does not reach it (a dim or underexposed shot); never darker.
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
                    l += k[c] * srgbToLinear(v.rgb.pixels[i * 3 + static_cast<std::size_t>(c)]) * fit.whiteBalance[static_cast<std::size_t>(c)];
                lum.push_back(l);
            }
        }
        if (!lum.empty()) {
            const std::size_t at = static_cast<std::size_t>(0.995 * static_cast<double>(lum.size() - 1));
            std::nth_element(lum.begin(), lum.begin() + static_cast<std::ptrdiff_t>(at), lum.end());
            fit.exposure = std::clamp(1.0f / std::max(lum[at], 1e-4f), 1.0f, 4.0f);
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
                for (int c = 0; c < 3; ++c) (s <= 1.0f / 3.0f ? root : tip)[c].push_back(srgbToLinear(v.rgb.sample(u, vv, c)));
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
        auto lum = [](const std::array<float, 3>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
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
            fit.melaninError = nearestMelanin(hairAppearanceColor(base), fit.melanin, fit.redness);
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
            luminance.pixels[i] = 0.2126f * srgbToLinear(v.rgb.pixels[3 * i]) + 0.7152f * srgbToLinear(v.rgb.pixels[3 * i + 1]) +
                                  0.0722f * srgbToLinear(v.rgb.pixels[3 * i + 2]);
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
    const float measured = std::clamp(lean * result.reachLength, 0.02f * size, size);
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
    // Curls: measured in every view; curly hair grows guides along the locks' coarse flow (a Curl
    // node puts the curls back), not along each curl.
    {
        std::vector<float> shares, amplitudes, wavelengths;
        for (const PreparedView& v : prepared) {
            if (v.curl.streamlines < 5) continue;
            shares.push_back(v.curl.periodicShare);
            if (v.curl.wavelengthPx > 0.0f && v.scalpDepth > 0.0f) {
                amplitudes.push_back(v.curl.amplitude);
                wavelengths.push_back(v.curl.wavelengthPx * v.scalpDepth / static_cast<float>(v.camera.focalPx));
            }
        }
        if (!shares.empty()) {
            result.look.curlShare = median(shares);
            result.look.curlAmplitudeDeg = amplitudes.empty() ? 0.0f : median(amplitudes) * 180.0f / kPi;
            // (Short fur lies flat: what swings in it is not curls.)
            const float longest = options.maxLength > 0 ? options.maxLength : ctx.maxLength;
            result.look.curly = result.look.curlShare > kCurlShare && result.look.curlAmplitudeDeg > kCurlAmplitude && longest >= size / 7.0f;
            if (!wavelengths.empty()) {
                const float wavelength = median(wavelengths);
                result.look.curlFrequency = 1.0f / std::max(wavelength, 1e-6f);
                result.look.curlRadius = kCurlRadiusGain * wavelength * pm::tan(std::min(median(amplitudes), 1.2f)) / (2.0f * kPi);
            }
        }
        const bool wavy = result.look.curlShare > kWavyShare && result.look.curlAmplitudeDeg > kWavyAmplitude;
        const bool coarse = options.curlFlow > 0 || (options.curlFlow < 0 && (result.look.curly || wavy));
        if (coarse)
            for (PreparedView& v : prepared)
                if (v.coarse) v.field = v.coarse;
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
    std::vector<float> rootLength(roots.size(), ctx.maxLength);
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

    // The guides: smoothed, resampled; mirrored pairs with symmetry.
    const float minLength = options.minLength > 0 ? options.minLength : 3.0f * ctx.step;
    std::vector<ReferenceGuide> grownGuides;
    std::vector<std::uint8_t> grownSeam;
    for (std::size_t i : live) {
        const Grown& g = ways[choice[i]][i];
        if (g.points.size() < 3 || polylineLength(g.points) < minLength) {
            ++result.rootsShort;
            continue;
        }
        ReferenceGuide guide;
        guide.root = roots[i].root;
        std::vector<Vec3f> path = smoothed(g.points, 3);
        // A guide that reached the given maximum never showed its tip (hidden under the hair above
        // it): hair is mostly shorter than the longest, so it is trimmed (calibrated on the synthetic
        // heads: such guides came out 1.15x too long).
        if (options.maxLength > 0.0f && g.stop == "length" && options.lengthTrim < 1.0f && !drawnLength[i])
            path = truncated(path, options.lengthTrim * polylineLength(path));
        guide.points = resampled(path, std::max(2, options.points));
        guide.points[0] = roots[i].position;
        guide.confidence = g.confidence;
        guide.stop = g.stop;
        grownGuides.push_back(std::move(guide));
        grownSeam.push_back(seam[i]);
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

    {
        const ReferenceLookFit curls = result.look;
        result.look = fitLook(prepared, grownGuides, options);
        result.look.curly = curls.curly;
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
    const PreparedView& pv = prepared.front();
    ReferenceViewPreview out;
    out.mask = pv.mask;
    out.crop = pv.crop;
    if (!pv.field || pv.crop[2] <= 0 || pv.crop[3] <= 0) return out;
    const float sx = static_cast<float>(pv.width) / static_cast<float>(pv.crop[2]);
    const float sy = static_cast<float>(pv.height) / static_cast<float>(pv.crop[3]);
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

}  // namespace groomlab
