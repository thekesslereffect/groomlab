// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include "groomlab/editor/agent_canvas.h"
#include "groomlab/export_view.h"
#include "groomlab/instances.h"
#include "groomlab/png.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::agent {

namespace {

using Color = std::array<float, 3>;

constexpr float kPi = 3.14159265358979f;
const Color kBackground{0.035f, 0.037f, 0.042f};
const Color kSkin{0.42f, 0.36f, 0.33f};
const Color kDefaultHair{0.20f, 0.12f, 0.07f};  // strands without a color channel: a mid brown
const Color kGuide{1.0f, 0.62f, 0.05f};          // guides drawn over the hair: orange, roots brighter
const Color kGuideRoot{1.0f, 0.95f, 0.3f};
const Color kTube{0.25f, 0.85f, 1.0f};           // tube wires: cyan
const Color kClayHair{0.55f, 0.55f, 0.55f};      // the clay look: every strand one neutral gray
const Color kClaySkin{0.36f, 0.38f, 0.42f};

// A view: the direction from the groom toward the camera and the screen's up, Y-up.
struct View {
    Vec3f back, up;
};

View viewNamed(const std::string& name) {
    if (name == "front") return {{0, 0, 1}, {0, 1, 0}};
    if (name == "back") return {{0, 0, -1}, {0, 1, 0}};
    if (name == "side" || name == "left") return {{1, 0, 0}, {0, 1, 0}};
    if (name == "right") return {{-1, 0, 0}, {0, 1, 0}};
    if (name == "top") return {{0, 1, 0}, {0, 0, -1}};
    if (name == "three_quarter") {
        const float az = 35.0f * kPi / 180.0f, el = 15.0f * kPi / 180.0f;
        return {Vec3f(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)), {0, 1, 0}};
    }
    // A turntable's frame: "turn:<degrees>" round the up axis from the front (toward the
    // character's left), a little from above.
    if (name.rfind("turn:", 0) == 0) {
        char* end = nullptr;
        const double degrees = std::strtod(name.c_str() + 5, &end);
        if (end && *end == '\0' && std::isfinite(degrees)) {
            const float az = static_cast<float>(degrees) * kPi / 180.0f, el = 10.0f * kPi / 180.0f;
            return {Vec3f(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)), {0, 1, 0}};
        }
    }
    throw std::invalid_argument("unknown view '" + name + "' (front, back, left, right, side, three_quarter, top)");
}

// A Y-up direction as the same direction in a Z-up world: Y-up (x, y, z) is Z-up (x, -z, y).
Vec3f toZUp(const Vec3f& v) { return {v.x, -v.z, v.y}; }

// An orthographic camera fitted to a box of points (or a pinhole camera), and the buffers of one view.
struct Raster {
    int w = 0, h = 0;
    Vec3f right, up, forward, center;
    float scale = 1.0f, offsetX = 0.0f, offsetY = 0.0f;
    const RefCamera* camera = nullptr;  // perspective through it, its pixels times `scale`
    std::vector<float> depth;
    std::vector<Color> color;

    // Screen x, y in pixels and depth along the view (larger: farther; a pinhole camera's: behind
    // it, -1).
    Vec3f project(const Vec3f& p) const {
        if (camera) {
            float u = 0.0f, v = 0.0f, d = 0.0f;
            if (!camera->project(p, u, v, &d) || !(d > 0.0f)) return {0.0f, 0.0f, -1.0f};
            return {u * scale, v * scale, d};
        }
        const Vec3f d = p - center;
        return {offsetX + d.dot(right) * scale, offsetY - d.dot(up) * scale, d.dot(forward)};
    }
    // A projected point a pinhole camera cannot draw (behind it).
    bool behind(const Vec3f& s) const { return camera && !(s.z > 0.0f); }
    void plot(int x, int y, float z, const Color& c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        if (z >= depth[i]) return;
        depth[i] = z;
        color[i] = c;
    }
};

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

void drawTriangle(Raster& r, const Vec3f& a, const Vec3f& b, const Vec3f& c, const Color& col) {
    if (r.behind(a) || r.behind(b) || r.behind(c)) return;
    const float minX = std::floor(std::min({a.x, b.x, c.x})), maxX = std::ceil(std::max({a.x, b.x, c.x}));
    const float minY = std::floor(std::min({a.y, b.y, c.y})), maxY = std::ceil(std::max({a.y, b.y, c.y}));
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-12f) return;
    const int x0 = std::max(0, static_cast<int>(minX)), x1 = std::min(r.w - 1, static_cast<int>(maxX));
    const int y0 = std::max(0, static_cast<int>(minY)), y1 = std::min(r.h - 1, static_cast<int>(maxY));
    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            r.plot(x, y, w0 * a.z + w1 * b.z + w2 * c.z, col);
        }
    }
}

// A line from a to b (screen space), one pixel wide, its depth pulled toward the camera by `bias`
// so hair lying on the skin is not lost in it.
void drawLine(Raster& r, const Vec3f& a, const Vec3f& b, float bias, const Color& col) {
    if (r.behind(a) || r.behind(b)) return;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
    if (steps > 4 * (r.w + r.h)) return;  // a segment far off the image: not worth walking
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        r.plot(static_cast<int>(std::floor(a.x + dx * t)), static_cast<int>(std::floor(a.y + dy * t)),
               a.z + (b.z - a.z) * t - bias, col);
    }
}

float toSrgb(float linear) {
    const float v = clamp01(linear);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

const std::vector<std::string>& previewViewNames() {
    static const std::vector<std::string> names{"front", "side", "back", "three_quarter", "top", "left", "right"};
    return names;
}

const std::vector<std::string>& previewLookNames() {
    static const std::vector<std::string> names{"studio", "flat", "rim", "clay"};
    return names;
}

PreviewImage renderPreview(const ScalpMesh* scalp, const StrandSet* strands, const PreviewOptions& options) {
    if (options.size < 64 || options.size > 1024) throw std::invalid_argument("size must be between 64 and 1024 pixels");
    if (std::find(previewLookNames().begin(), previewLookNames().end(), options.look) == previewLookNames().end()) {
        throw std::invalid_argument("unknown look '" + options.look + "' (studio, flat, rim, clay)");
    }
    const bool flat = options.look == "flat", rim = options.look == "rim", clay = options.look == "clay";
    std::vector<View> views;
    if (options.camera) {
        if (!(options.camera->width > 0.0 && options.camera->height > 0.0 && options.camera->focalPx > 0.0)) {
            throw std::invalid_argument("the camera has no image size or focal length");
        }
        views.push_back({options.camera->axis(2), options.camera->axis(1)});
    } else {
        if (options.views.empty()) throw std::invalid_argument("no views");
        if (options.views.size() > 12) throw std::invalid_argument("at most 12 views in one image");
        for (const std::string& name : options.views) {
            View v = viewNamed(name);
            if (options.zUp) v = {toZUp(v.back), toZUp(v.up)};
            views.push_back(v);
        }
    }

    PreviewImage image;
    const int count = static_cast<int>(views.size());
    image.columns = count <= 3 ? count : count <= 8 ? (count + 1) / 2 : 4;
    const int rows = (count + image.columns - 1) / image.columns;
    const int tile = options.size / image.columns;
    // A pinhole camera's image: the camera's shape, `size` across.
    const int tileH = options.camera ? std::max(16, static_cast<int>(std::lround(options.size * options.camera->height /
                                                                                       options.camera->width)))
                                     : tile;
    image.width = tile * image.columns;
    image.height = tileH * rows;

    // Which strands to draw: all of them, or every k-th past the cap (the same ones every time).
    const std::size_t total = strands ? strands->strandCount() : 0;
    image.strandsTotal = total;
    image.stride = options.maxStrands > 0 && total > options.maxStrands ? (total + options.maxStrands - 1) / options.maxStrands : 1;
    std::vector<std::size_t> drawn;
    for (std::size_t s = 0; s < total; s += image.stride) drawn.push_back(s);
    image.strandsDrawn = drawn.size();
    std::vector<std::array<float, 3>> strandColor, pointColor;
    if (strands && total) {
        strandColor = strandAppearanceColors(*strands);
        pointColor = pointAppearanceColors(*strands);
    }

    // Everything drawn, for framing.
    std::vector<Vec3f> framed;
    if (scalp) framed = scalp->points();
    for (const std::size_t s : drawn) {
        const Vec3f* p = strands->strandPoints(s);
        const std::size_t n = strands->strandPointCount(s);
        for (std::size_t i = 0; i < n; ++i) framed.push_back(p[i]);
    }
    for (const std::vector<Vec3f>& guide : options.guides) framed.insert(framed.end(), guide.begin(), guide.end());

    const int ss = std::max(tile, tileH) <= 512 ? 2 : 1;  // supersampling: rendered at twice the size up to 512 px tiles
    const int inner = tile * ss, innerH = tileH * ss;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    struct Label {
        float x, y;
        std::string text;
    };
    std::vector<Label> labels;  // in the image's pixels
    for (int v = 0; v < count; ++v) {
        Raster r;
        r.w = inner;
        r.h = innerH;
        r.depth.assign(static_cast<std::size_t>(inner) * static_cast<std::size_t>(innerH), std::numeric_limits<float>::infinity());
        r.color.assign(r.depth.size(), kBackground);
        r.forward = -views[static_cast<std::size_t>(v)].back.normalized();
        r.right = r.forward.cross(views[static_cast<std::size_t>(v)].up).normalized();
        r.up = r.right.cross(r.forward);
        // Fit the points' extent on screen with a margin.
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-lo[0], -lo[1], -lo[2]};
        for (const Vec3f& p : framed) {
            const float c[3] = {p.dot(r.right), p.dot(r.up), p.dot(r.forward)};
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
        if (framed.empty()) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0.0f;
        r.center = r.right * (0.5f * (lo[0] + hi[0])) + r.up * (0.5f * (lo[1] + hi[1])) + r.forward * (0.5f * (lo[2] + hi[2]));
        const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], 1e-6f});
        r.scale = 0.9f * static_cast<float>(inner) / extent;
        r.offsetX = r.offsetY = 0.5f * static_cast<float>(inner);
        float depthRange = std::max(hi[2] - lo[2], 1e-6f);  // scene units, as project()'s depth
        if (options.camera) {
            r.camera = &*options.camera;
            r.scale = static_cast<float>(inner) / static_cast<float>(options.camera->width);
        }

        // The light: from above and to the right of the camera, the same in every view (rim: from
        // behind and above, the hair's outline lit).
        const Vec3f light = rim ? (-views[static_cast<std::size_t>(v)].back * 0.8f + r.up * 0.6f + r.right * 0.3f).normalized()
                                : (views[static_cast<std::size_t>(v)].back + r.up * 0.8f + r.right * 0.5f).normalized();
        const Vec3f toEye = -r.forward;
        if (scalp) {
            const auto& pts = scalp->points();
            std::vector<Vec3f> screen(pts.size());
            for (std::size_t i = 0; i < pts.size(); ++i) screen[i] = r.project(pts[i]);
            const bool colored = options.faceColors.size() == scalp->triangleCount();
            std::size_t face = 0;
            for (const auto& t : scalp->triangles()) {
                const std::size_t ti = face++;
                const Vec3f& a = pts[static_cast<std::size_t>(t[0])];
                const Vec3f& b = pts[static_cast<std::size_t>(t[1])];
                const Vec3f& c = pts[static_cast<std::size_t>(t[2])];
                Vec3f n = (b - a).cross(c - a).normalized();
                if (n.dot(toEye) < 0.0f) n = -n;  // two-sided
                const float shade = flat ? 0.75f : (rim ? 0.35f : 0.18f) + (rim ? 0.65f : 0.82f) * std::max(0.0f, n.dot(light));
                const Color& skin = colored ? options.faceColors[ti] : clay ? kClaySkin : kSkin;
                const Color col{skin[0] * shade, skin[1] * shade, skin[2] * shade};
                drawTriangle(r, screen[static_cast<std::size_t>(t[0])], screen[static_cast<std::size_t>(t[1])],
                             screen[static_cast<std::size_t>(t[2])], col);
            }
        }
        // Instances placed by a matrix with a built-in mesh (beads, cuffs, hair ties): shaded like
        // the skin in a color of their prototype's. (Scene meshes are the host's: not drawn.)
        if (strands && strands->instances() && !strands->instances()->empty()) {
            static const Color kAccessory[4] = {{0.75f, 0.55f, 0.18f}, {0.62f, 0.64f, 0.68f}, {0.20f, 0.45f, 0.55f}, {0.55f, 0.16f, 0.14f}};
            const InstanceSet& set = *strands->instances();
            for (const InstanceSet::Placed& placed : set.resolve(*strands, scalp)) {
                if (placed.meshed || placed.prototype >= set.prototypes.size()) continue;
                const InstancePrototype* mesh = builtinPrototype(set.prototypes[placed.prototype]);
                if (!mesh) continue;
                std::vector<Vec3f> world(mesh->points.size()), screen(mesh->points.size());
                for (std::size_t i = 0; i < world.size(); ++i) {
                    const Vec3f& q = mesh->points[i];
                    world[i] = placed.origin + placed.x * q.x + placed.y * q.y + placed.z * q.z;
                    screen[i] = r.project(world[i]);
                }
                const Color& base = kAccessory[placed.prototype % 4];
                for (std::size_t t = 0; t + 2 < mesh->triangles.size(); t += 3) {
                    const std::uint32_t ia = mesh->triangles[t], ib = mesh->triangles[t + 1], ic = mesh->triangles[t + 2];
                    Vec3f n = (world[ib] - world[ia]).cross(world[ic] - world[ia]);
                    if (!(n.length() > 0.0f)) continue;
                    n = n.normalized();
                    if (n.dot(toEye) < 0.0f) n = -n;
                    const float shade = 0.2f + 0.8f * std::max(0.0f, n.dot(light));
                    const float spec = std::pow(std::max(0.0f, n.dot((light + toEye).normalized())), 40.0f);
                    drawTriangle(r, screen[ia], screen[ib], screen[ic],
                                 {base[0] * shade + 0.3f * spec, base[1] * shade + 0.3f * spec, base[2] * shade + 0.3f * spec});
                }
            }
        }
        // What the scalp hides, for the guides (drawn over the hair, behind the head hidden).
        const std::vector<float> scalpDepth = options.guides.empty() && options.tubeLines.empty() ? std::vector<float>() : r.depth;
        // Strands: Kajiya-Kay diffuse and highlight from each segment's tangent, a little darker
        // toward the root (the hair's own shadow).
        if (options.camera) {  // (depth along the camera's view: the framed points' spread)
            float nearest = std::numeric_limits<float>::max(), farthest = 0.0f;
            for (const Vec3f& p : framed) {
                const Vec3f q = r.project(p);
                if (q.z > 0.0f) {
                    nearest = std::min(nearest, q.z);
                    farthest = std::max(farthest, q.z);
                }
            }
            depthRange = std::max(farthest - nearest, 1e-6f);
        }
        const float bias = options.camera ? 0.006f * depthRange : std::min(0.006f * depthRange, 2.0f / r.scale);  // (at most a couple of pixels deep)
        const Vec3f halfway = (light + toEye).normalized();
        for (const std::size_t s : drawn) {
            const Vec3f* p = strands->strandPoints(s);
            const std::size_t n = strands->strandPointCount(s);
            if (n < 2) continue;
            const std::size_t first = strands->offsets()[s];
            Vec3f prev = r.project(p[0]);
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f next = r.project(p[i]);
                const Vec3f tangent = (p[i] - p[i - 1]).normalized();
                const float tl = tangent.dot(light), th = tangent.dot(halfway);
                const float diffuse = std::sqrt(std::max(0.0f, 1.0f - tl * tl));
                const float spec = std::pow(std::sqrt(std::max(0.0f, 1.0f - th * th)), 60.0f);
                const float along = static_cast<float>(i) / static_cast<float>(n - 1);
                const float shade = flat ? 1.0f : (0.25f + 0.75f * diffuse) * (0.55f + 0.45f * along);
                const float shine = flat ? 0.0f : rim ? 0.35f * spec : 0.12f * spec;
                Color base = clay ? kClayHair : kDefaultHair;
                if (clay) {
                } else if (pointColor.size() == strands->pointCount()) {
                    base = pointColor[first + i];
                } else if (strandColor.size() == total) {
                    base = strandColor[s];
                }
                const Color col{base[0] * shade + shine, base[1] * shade + shine, base[2] * shade + shine};
                drawLine(r, prev, next, bias, col);
                prev = next;
            }
        }
        // Guides: lines two pixels wide over the hair, a dot at the root; the scalp hides them.
        // Tube wires the same, one pixel wide.
        if (!options.tubeLines.empty()) {
            const float tubeBias = std::min(0.01f * depthRange, 3.0f / r.scale);
            for (std::size_t i = 0; i + 1 < options.tubeLines.size(); i += 2) {
                const Vec3f a = r.project(options.tubeLines[i]), b = r.project(options.tubeLines[i + 1]);
                if (r.behind(a) || r.behind(b)) continue;
                const float dx = b.x - a.x, dy = b.y - a.y;
                const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
                if (steps > 4 * (r.w + r.h)) continue;
                for (int k = 0; k <= steps; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(steps);
                    const int x = static_cast<int>(std::floor(a.x + dx * t)), y = static_cast<int>(std::floor(a.y + dy * t));
                    if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
                    const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(r.w) + static_cast<std::size_t>(x);
                    if (a.z + (b.z - a.z) * t - tubeBias >= scalpDepth[at]) continue;
                    r.color[at] = kTube;
                }
            }
        }
        if (!options.guides.empty()) {
            const float guideBias = std::min(0.01f * depthRange, 3.0f / r.scale);
            auto over = [&](float fx, float fy, float z, const Color& c) {
                for (int oy = 0; oy < 2; ++oy) {
                    for (int ox = 0; ox < 2; ++ox) {
                        const int x = static_cast<int>(std::floor(fx)) + ox, y = static_cast<int>(std::floor(fy)) + oy;
                        if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
                        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(r.w) + static_cast<std::size_t>(x);
                        if (z - guideBias >= scalpDepth[i]) continue;
                        r.color[i] = c;
                    }
                }
            };
            for (const std::vector<Vec3f>& guide : options.guides) {
                for (std::size_t i = 1; i < guide.size(); ++i) {
                    const Vec3f a = r.project(guide[i - 1]), b = r.project(guide[i]);
                    if (r.behind(a) || r.behind(b)) continue;
                    const float dx = b.x - a.x, dy = b.y - a.y;
                    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
                    if (steps > 4 * (r.w + r.h)) continue;
                    for (int k = 0; k <= steps; ++k) {
                        const float t = static_cast<float>(k) / static_cast<float>(steps);
                        over(a.x + dx * t, a.y + dy * t, a.z + (b.z - a.z) * t, kGuide);
                    }
                }
                const std::size_t gi = static_cast<std::size_t>(&guide - options.guides.data());
                if (gi < options.guideLabels.size() && !options.guideLabels[gi].empty() && !guide.empty()) {
                    const Vec3f tip = r.project(guide.back());
                    const int tx = static_cast<int>(std::floor(tip.x)), ty = static_cast<int>(std::floor(tip.y));
                    if (!r.behind(tip) && tx >= 0 && ty >= 0 && tx < r.w && ty < r.h &&
                        tip.z - guideBias < scalpDepth[static_cast<std::size_t>(ty) * static_cast<std::size_t>(r.w) + static_cast<std::size_t>(tx)]) {
                        const int tileX = (v % image.columns) * tile, tileY = (v / image.columns) * tileH;
                        labels.push_back({static_cast<float>(tileX) + tip.x / static_cast<float>(ss),
                                          static_cast<float>(tileY) + tip.y / static_cast<float>(ss), options.guideLabels[gi]});
                    }
                }
                if (!guide.empty()) {
                    const Vec3f root = r.project(guide.front());
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            over(root.x + static_cast<float>(ox), root.y + static_cast<float>(oy), root.z, kGuideRoot);
                        }
                    }
                }
            }
        }
        // Filtered down into the image's tile, as sRGB.
        const int tileX = (v % image.columns) * tile, tileY = (v / image.columns) * tileH;
        for (int y = 0; y < tileH; ++y) {
            for (int x = 0; x < tile; ++x) {
                Color sum{0, 0, 0};
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        const Color& c = r.color[static_cast<std::size_t>(y * ss + sy) * static_cast<std::size_t>(inner) +
                                                 static_cast<std::size_t>(x * ss + sx)];
                        for (int k = 0; k < 3; ++k) sum[static_cast<std::size_t>(k)] += c[static_cast<std::size_t>(k)];
                    }
                }
                const std::size_t o = (static_cast<std::size_t>(tileY + y) * static_cast<std::size_t>(image.width) +
                                       static_cast<std::size_t>(tileX + x)) * 3;
                for (int k = 0; k < 3; ++k) {
                    const float value = sum[static_cast<std::size_t>(k)] / static_cast<float>(ss * ss);
                    pixels[o + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(toSrgb(value) * 255.0f));
                }
            }
        }
    }
    // Tiles left empty in the last row keep the background.
    for (int v = count; v < image.columns * rows; ++v) {
        const int tileX = (v % image.columns) * tile, tileY = (v / image.columns) * tileH;
        for (int y = 0; y < tileH; ++y) {
            for (int x = 0; x < tile; ++x) {
                const std::size_t o = (static_cast<std::size_t>(tileY + y) * static_cast<std::size_t>(image.width) +
                                       static_cast<std::size_t>(tileX + x)) * 3;
                for (int k = 0; k < 3; ++k) pixels[o + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(toSrgb(kBackground[static_cast<std::size_t>(k)]) * 255.0f));
            }
        }
    }
    // The guides' labels, each beside its tip.
    if (!labels.empty()) {
        RefImage shown;
        shown.width = image.width;
        shown.height = image.height;
        shown.channels = 3;
        shown.pixels.resize(pixels.size());
        for (std::size_t i = 0; i < pixels.size(); ++i) shown.pixels[i] = static_cast<float>(pixels[i]) / 255.0f;
        Canvas canvas = Canvas::ofImage(shown);
        for (const Label& l : labels) {
            canvas.dot(l.x, l.y, 1.5f, {1.0f, 1.0f, 0.3f});
            canvas.text(static_cast<int>(l.x) + 3, static_cast<int>(l.y) - 3, l.text, {1.0f, 1.0f, 0.6f});
        }
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                const Rgb c = canvas.at(x, y);
                for (int k = 0; k < 3; ++k)
                    pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(k)] =
                        static_cast<std::uint8_t>(std::lround(std::clamp(c[static_cast<std::size_t>(k)], 0.0f, 1.0f) * 255.0f));
            }
    }
    image.png = encodePng(image.width, image.height, 3, pixels);
    image.rgb = std::move(pixels);
    return image;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = std::uint32_t{bytes[i]} << 16;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

}  // namespace groomlab::agent
