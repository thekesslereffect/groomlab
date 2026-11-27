// SPDX-License-Identifier: Apache-2.0
// Lock guides from strokes, combing and filling in (lock_guides.h).

#include "groomlab/lock_guides.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>

#include "groomlab/random.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// A Gaussian blur of a sequence (edges padded with the end values).
std::vector<float> blurred(const std::vector<float>& v, float sigma) {
    if (v.size() < 2 || !(sigma > 0.0f)) return v;
    const int r = static_cast<int>(std::ceil(2.5f * sigma));
    std::vector<float> out(v.size());
    const int n = static_cast<int>(v.size());
    for (int i = 0; i < n; ++i) {
        double sum = 0.0, ws = 0.0;
        for (int k = -r; k <= r; ++k) {
            const int j = std::clamp(i + k, 0, n - 1);
            const double w = std::exp(-0.5 * static_cast<double>(k * k) / static_cast<double>(sigma * sigma));
            sum += w * v[static_cast<std::size_t>(j)];
            ws += w;
        }
        out[static_cast<std::size_t>(i)] = static_cast<float>(sum / ws);
    }
    return out;
}

// A 2D polyline resampled every `spacing` along it (its ends kept).
std::vector<std::array<float, 2>> resampled2(const std::vector<std::array<float, 2>>& pts, float spacing) {
    std::vector<std::array<float, 2>> out;
    if (pts.empty()) return out;
    out.push_back(pts.front());
    float carry = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float dx = pts[i][0] - pts[i - 1][0], dy = pts[i][1] - pts[i - 1][1];
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        float at = spacing - carry;
        while (at <= len) {
            out.push_back({pts[i - 1][0] + dx * at / len, pts[i - 1][1] + dy * at / len});
            at += spacing;
        }
        carry = len - (at - spacing);
    }
    const auto& last = pts.back();
    const float dx = last[0] - out.back()[0], dy = last[1] - out.back()[1];
    if (out.size() < 2 || std::sqrt(dx * dx + dy * dy) > 0.3f * spacing) out.push_back(last);
    return out;
}

// The point a tenth of a head radius further in (not inside the head).
Vec3f inward(const HairShell& shell, const Vec3f& p) {
    const HeadFrame& f = shell.frame();
    const Vec3f q = p - f.center;
    const float r = q.length();
    if (r <= 0.0f) return p;
    const Vec3f u = q / r;
    return f.center + u * std::max(shell.head(u), r - 0.1f * f.radius);
}

}  // namespace

// ---- Strokes drawn on an image ----------------------------------------------------------------------

std::vector<ImageStroke> strokesFromImage(const RefImage& image, const StrokeImageOptions& o) {
    if (image.empty() || image.channels < 3) throw std::invalid_argument("strokesFromImage: an RGB image is needed");
    if (!(o.scale > 0.0f)) throw std::invalid_argument("strokesFromImage: the scale must be positive");
    const int w = image.width, h = image.height;
    const float photoW = o.photoWidth > 0 ? static_cast<float>(o.photoWidth) : static_cast<float>(w) / o.scale;
    const float photoH = o.photoHeight > 0 ? static_cast<float>(o.photoHeight) : static_cast<float>(h) / o.scale;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> on(n, 0);
    const float margin = 5.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float r = image.at(x, y, 0), g = image.at(x, y, 1), b = image.at(x, y, 2);
            if (std::abs(r - o.color[0]) > o.tolerance || std::abs(g - o.color[1]) > o.tolerance || std::abs(b - o.color[2]) > o.tolerance) continue;
            if (std::max({r, g, b}) - std::min({r, g, b}) < o.minSaturation) continue;
            const float px = (static_cast<float>(x) + 0.5f - o.offset[0]) / o.scale, py = (static_cast<float>(y) + 0.5f - o.offset[1]) / o.scale;
            if (px < -margin || py < -margin || px > photoW + margin || py > photoH + margin) continue;
            on[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
        }
    const int minPixels = o.minPixels > 0 ? o.minPixels : std::max(12, static_cast<int>(static_cast<double>(n) / 20000.0));
    std::vector<int> label(n, -1);
    std::vector<ImageStroke> out;
    auto index = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    std::vector<int> dist(n, -1);
    std::vector<std::size_t> prev(n, 0);
    int pieces = 0;
    for (std::size_t s = 0; s < n; ++s) {
        if (!on[s] || label[s] >= 0) continue;
        // The piece.
        std::vector<std::size_t> piece;
        std::vector<std::size_t> stack{s};
        label[s] = pieces;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            piece.push_back(i);
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const std::size_t j = index(xx, yy);
                    if (!on[j] || label[j] >= 0) continue;
                    label[j] = pieces;
                    stack.push_back(j);
                }
        }
        ++pieces;
        if (static_cast<int>(piece.size()) < minPixels) continue;
        // The farthest pixel from `from` within the piece (8-connected steps), the way back in prev.
        auto farthest = [&](std::size_t from) {
            for (const std::size_t i : piece) dist[i] = -1;
            std::deque<std::size_t> q{from};
            dist[from] = 0;
            prev[from] = from;
            std::size_t last = from;
            while (!q.empty()) {
                const std::size_t i = q.front();
                q.pop_front();
                last = i;
                const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const std::size_t j = index(xx, yy);
                        if (label[j] != pieces - 1 || dist[j] >= 0) continue;
                        dist[j] = dist[i] + 1;
                        prev[j] = i;
                        q.push_back(j);
                    }
            }
            return last;
        };
        const std::size_t e1 = farthest(s);
        const std::size_t e2 = farthest(e1);
        std::vector<std::size_t> path{e2};
        while (path.back() != e1) path.push_back(prev[path.back()]);
        if (path.size() < 2) continue;
        // Stroke pixels round each end (the dot: a blob a few stroke widths across).
        const float width = static_cast<float>(piece.size()) / static_cast<float>(path.size());
        const int r = std::clamp(static_cast<int>(std::lround(2.5f * width)), 3, 40);
        auto blob = [&](std::size_t e) {
            const int x = static_cast<int>(e % static_cast<std::size_t>(w)), y = static_cast<int>(e / static_cast<std::size_t>(w));
            int count = 0;
            for (int yy = std::max(0, y - r); yy <= std::min(h - 1, y + r); ++yy)
                for (int xx = std::max(0, x - r); xx <= std::min(w - 1, x + r); ++xx) count += label[index(xx, yy)] == pieces - 1 ? 1 : 0;
            return count;
        };
        int b0 = blob(path.front()), b1 = blob(path.back());
        bool flip = false;
        const float y0 = static_cast<float>(path.front() / static_cast<std::size_t>(w)), y1 = static_cast<float>(path.back() / static_cast<std::size_t>(w));
        if (o.root == "top") flip = y1 < y0;
        else if (o.root == "bottom") flip = y1 > y0;
        else flip = b1 > b0;
        if (flip) {
            std::reverse(path.begin(), path.end());
            std::swap(b0, b1);
        }
        std::vector<float> xs, ys;
        for (const std::size_t i : path) {
            xs.push_back(static_cast<float>(i % static_cast<std::size_t>(w)) + 0.5f);
            ys.push_back(static_cast<float>(i / static_cast<std::size_t>(w)) + 0.5f);
        }
        xs = blurred(xs, o.smooth);
        ys = blurred(ys, o.smooth);
        ImageStroke stroke;
        stroke.pixels = static_cast<int>(piece.size());
        stroke.rootBlob = b0;
        stroke.tipBlob = b1;
        std::array<float, 2> kept{xs[0], ys[0]};
        stroke.points.push_back({(kept[0] - o.offset[0]) / o.scale, (kept[1] - o.offset[1]) / o.scale});
        for (std::size_t i = 1; i < xs.size(); ++i) {
            const float dx = xs[i] - kept[0], dy = ys[i] - kept[1];
            if (std::sqrt(dx * dx + dy * dy) < o.spacing && i + 1 < xs.size()) continue;
            kept = {xs[i], ys[i]};
            stroke.points.push_back({(kept[0] - o.offset[0]) / o.scale, (kept[1] - o.offset[1]) / o.scale});
        }
        if (stroke.points.size() >= 2) out.push_back(std::move(stroke));
    }
    return out;
}

// ---- Guides --------------------------------------------------------------------------------------------

float guideLength(const std::vector<Vec3f>& points) {
    float l = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) l += (points[i] - points[i - 1]).length();
    return l;
}

std::vector<Vec3f> resampledGuide(const std::vector<Vec3f>& points, std::size_t count) {
    if (points.size() < 2 || count < 2) return points;
    std::vector<float> s(points.size(), 0.0f);
    for (std::size_t i = 1; i < points.size(); ++i) s[i] = s[i - 1] + (points[i] - points[i - 1]).length();
    const float total = s.back();
    std::vector<Vec3f> out;
    out.reserve(count);
    std::size_t j = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const float t = total * static_cast<float>(k) / static_cast<float>(count - 1);
        while (j + 2 < points.size() && s[j + 1] < t) ++j;
        const float span = std::max(1e-12f, s[j + 1] - s[j]);
        const float f = std::clamp((t - s[j]) / span, 0.0f, 1.0f);
        out.push_back(points[j] + (points[j + 1] - points[j]) * f);
    }
    return out;
}

std::vector<Vec3f> resampledBySpacing(const std::vector<Vec3f>& points, float spacing) {
    if (points.size() < 2 || !(spacing > 0.0f)) return points;
    const float length = guideLength(points);
    const std::size_t count = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(length / spacing)) + 1);
    return resampledGuide(points, count);
}

LiftedStroke liftStroke(const RefCamera& camera, const std::vector<std::array<float, 2>>& pixels, const HairShell& shell, const ScalpMesh& scalp,
                        const StrokeLiftOptions& o) {
    if (pixels.size() < 2) throw std::invalid_argument("liftStroke: a stroke needs 2 points or more");
    if (shell.empty()) throw std::invalid_argument("liftStroke: no shell");
    const float side = static_cast<float>(std::max(camera.width, camera.height));
    const float spacing = o.spacingPx > 0.0f ? o.spacingPx : std::max(1.0f, 0.006f * side);
    const float smooth = o.smoothPx > 0.0f ? o.smoothPx : 0.02f * side;
    const std::vector<std::array<float, 2>> px = resampled2(pixels, spacing);
    const Vec3f eye = camera.eye();
    LiftedStroke out;
    std::vector<float> depth;
    std::vector<Vec3f> rays;
    for (const auto& q : px) {
        const Vec3f d = camera.ray(q[0], q[1]);
        const HairShell::Hit hit = shell.raycast(eye, d);
        rays.push_back(d);
        depth.push_back(hit.t);
        if (hit.hit) ++out.hits;
        else ++out.misses;
    }
    depth = blurred(depth, smooth / spacing);
    std::vector<Vec3f> pts;
    const HeadFrame& f = shell.frame();
    for (std::size_t i = 0; i < rays.size(); ++i) pts.push_back(eye + rays[i] * depth[i]);
    // The root: the scalp under the first point (straight in toward the head's middle).
    const Vec3f u0 = (pts.front() - f.center).normalized();
    const Vec3f under = f.center + u0 * shell.head(u0);
    const Vec3f root = scalp.closestPoint(under).position;
    pts.insert(pts.begin(), root);
    const float pointSpacing = o.pointSpacing > 0.0f ? o.pointSpacing : f.radius / 20.0f;
    out.points = resampledBySpacing(pts, pointSpacing);
    out.points.front() = root;
    out.length = guideLength(out.points);
    return out;
}

std::vector<Vec3f> mirroredGuide(const std::vector<Vec3f>& points, const Vec3f& planePoint, const Vec3f& normalIn, const ScalpMesh* scalp) {
    const Vec3f n = normalIn.normalized();
    std::vector<Vec3f> out;
    out.reserve(points.size());
    for (const Vec3f& p : points) out.push_back(p - n * (2.0f * n.dot(p - planePoint)));
    if (scalp && !out.empty()) {
        const Vec3f shift = scalp->closestPoint(out.front()).position - out.front();
        for (Vec3f& p : out) p += shift;
    }
    return out;
}

std::vector<Vec3f> clippedToPhotos(const std::vector<Vec3f>& points, const HairShell& shell, std::size_t keep) {
    if (shell.photoCount() == 0 || points.size() <= keep) return points;
    const float minLength = 0.15f * shell.frame().radius;
    auto bare = [&](std::size_t i) { return shell.seenBare(points[i]) && shell.seenBare(inward(shell, points[i])); };
    float s = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        s += (points[i] - points[i - 1]).length();
        if (s < minLength || i < keep) continue;
        // (two in a row: one point a mask's ragged edge or a stray pixel calls bare is no end)
        if (bare(i) && (i + 1 == points.size() || bare(i + 1))) return std::vector<Vec3f>(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return points;
}

// ---- Combing -------------------------------------------------------------------------------------------

float RegionLengths::of(const std::string& region, float headRadius) const {
    if (region == "top") return top > 0.0f ? top : 1.6f * headRadius;
    if (region == "front") return front > 0.0f ? front : 1.7f * headRadius;
    if (region == "sides") return sides > 0.0f ? sides : 1.1f * headRadius;
    if (region == "back") return back > 0.0f ? back : 2.3f * headRadius;
    return nape > 0.0f ? nape : 0.7f * headRadius;
}

Vec3f combDirection(const HeadFrame& f, const Vec3f& p, const CombOptions& o) {
    const Vec3f q = p - f.center;
    const Vec3f u = q.normalized();
    const Vec3f l = f.local(u);  // x left, y up, z front
    const float R = f.radius;
    float sidePart = 0.0f, near = 0.0f;
    if (o.part.on) {
        const float off = (p - o.part.point).dot(o.part.normal);
        sidePart = off >= 0.0f ? 1.0f : -1.0f;
        near = std::max(0.0f, 1.0f - std::abs(off) / (0.4f * R));
    }
    const Vec3f away = o.part.normal * sidePart;
    const Vec3f back = -f.front, down = -f.up;
    const float front = smoothstep01((l.z + 0.3f) / 0.6f);
    const float high = std::max(0.0f, l.y);
    const float sideness = std::clamp((std::abs(l.x) - 0.45f) / 0.35f, 0.0f, 1.0f);
    float awayW = front * (0.45f + 0.5f * near) * (1.0f - sideness);
    float backW = (1.0f - front) * 0.6f * high + front * (0.9f * high + 0.5f);
    float forwardW = 0.0f;
    float downW = 0.15f + 0.55f * (1.0f - high) + 0.6f * (1.0f - front);
    if (o.front == FrontSweep::Out) {
        awayW *= 1.8f;
        backW -= 0.5f * front * (0.9f * high + 0.5f) * (1.0f - sideness);
    } else if (o.front == FrontSweep::Forward) {
        // (a fringe: the front falls forward and down over the forehead)
        const float fr = front * (1.0f - sideness);
        backW -= fr * (0.9f * high + 0.5f);
        forwardW = fr * (0.6f + 0.4f * high);
        downW += 0.4f * fr;
        awayW *= 0.5f;
    }
    if (o.sides == SideSweep::Back) backW += front * 0.8f * sideness;
    else downW += front * 0.8f * sideness;
    Vec3f v = away * awayW + back * backW + f.front * forwardW + down * downW;
    v = v - u * v.dot(u);
    const float len = v.length();
    return len > 1e-6f ? v / len : (down - u * down.dot(u)).normalized();
}

float combLength(const HeadFrame& f, const Vec3f& root, const CombOptions& o, std::uint64_t seed) {
    Rng rng(hashCombine(o.seed, seed * 7 + 1));
    const float base = o.lengths.of(headRegion(f, root - f.center), f.radius);
    return base * (1.0f + o.lengthRandom * rng.uniform(-1.0f, 1.0f));
}

std::vector<Vec3f> combGuide(const Vec3f& root, float length, const HairShell& shell, const CombOptions& o, std::uint64_t seed) {
    if (shell.empty()) throw std::invalid_argument("combGuide: no shell");
    const HeadFrame& f = shell.frame();
    const float R = f.radius;
    Rng rng(hashCombine(o.seed, seed));
    const float step = o.step > 0.0f ? o.step : R / 20.0f;
    const Vec3f u0 = (root - f.center).normalized();
    const Vec3f l0 = f.local(u0);
    const bool high = (root - f.center).dot(f.up) > 0.4f * R;
    std::vector<Vec3f> pts{root};
    Vec3f p = root;
    // First: out along the normal (leaning into the flow); low roots (temples, nape) lie down.
    Vec3f d = (u0 * (high ? o.rise : 0.3f * o.rise) + combDirection(f, p, o)).normalized();
    Vec3f jitter;
    jitter.z = rng.uniform(-0.15f, 0.15f);
    jitter.y = rng.uniform(-0.15f, 0.15f);
    jitter.x = rng.uniform(-0.15f, 0.15f);
    float s = 0.0f;
    const int maxSteps = static_cast<int>(std::ceil(length / step)) + 1;
    for (int k = 0; k < maxSteps && s < length; ++k) {
        const Vec3f q = p - f.center;
        const float r = q.length();
        const Vec3f u = q / std::max(r, 1e-9f);
        const float headR = shell.head(u);
        const float want = headR + o.ride * shell.thickness(u) * std::min(1.0f, s / (0.3f * R));
        Vec3f flow = combDirection(f, p, o);
        if (l0.z > 0.3f && high && s < 0.5f * R) {
            // The forehead's hairline: up and back first.
            const float lift = (1.0f - s / (0.5f * R)) * 1.6f * std::min(1.0f, (l0.z - 0.3f) / 0.3f);
            flow = (flow + u0 * (0.6f * lift) + (f.up * 0.4f - f.front) * lift).normalized();
        }
        flow = (flow + jitter).normalized();
        // Steer toward the shell's riding radius.
        d = (d * 0.6f + flow * 0.4f + u * (2.5f * (want - r) / R)).normalized();
        p += d * step;
        // Never into the head.
        const Vec3f q2 = p - f.center;
        const float r2 = q2.length();
        const float floorR = shell.head(q2) + 0.02f * R * std::min(1.0f, (s + step) / (0.1f * R));
        if (r2 < floorR && r2 > 0.0f) p = f.center + q2 / r2 * floorR;
        s += step;
        pts.push_back(p);
    }
    // An S-wave across the flow along the shell, ramped in; the tip flips out and up.
    const std::size_t n = pts.size();
    const float total = std::max(s, step);
    const float amp0 = o.amplitude >= 0.0f ? o.amplitude : 0.08f * R;
    const float wl0 = o.wavelength > 0.0f ? o.wavelength : 0.5f * R;
    const float flip = o.flip >= 0.0f ? o.flip : 0.12f * R;
    const float phase = (1.0f - std::clamp(o.coherence, 0.0f, 1.0f)) * rng.uniform(-kPi, kPi);
    const float amp = amp0 * rng.uniform(0.8f, 1.2f), wl = wl0 * rng.uniform(0.92f, 1.08f);
    std::vector<Vec3f> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float si = static_cast<float>(i) * step;
        const Vec3f tan = (pts[std::min(i + 1, n - 1)] - pts[i > 0 ? i - 1 : 0]).normalized();
        const Vec3f upR = (pts[i] - f.center).normalized();
        const Vec3f across = tan.cross(upR).normalized();
        const float ramp = std::clamp((si - 0.2f * R) / (0.3f * R), 0.0f, 1.0f);
        Vec3f q = pts[i] + across * (amp * ramp * std::sin(2.0f * kPi * si / wl + phase));
        const float t = si / total;
        if (t > 0.78f) {
            const float k = (t - 0.78f) / 0.22f;
            q += upR * (flip * k * k) + f.up * (0.6f * flip * k * k * k);
        }
        out.push_back(q);
    }
    out.front() = root;
    if (o.clip) out = clippedToPhotos(out, shell);
    return out;
}

// ---- Filling in ------------------------------------------------------------------------------------------

FilledGuide fillGuide(const Vec3f& root, const std::vector<std::vector<Vec3f>>& keys, const std::vector<Vec3f>& combed, const HairShell& shell,
                      const FillOptions& o) {
    const HeadFrame& f = shell.frame();
    const float reach = o.reach > 0.0f ? o.reach : 0.4f * f.radius;
    const float keepOut = o.keepOut > 0.0f ? o.keepOut : 0.04f * f.radius;
    const std::size_t n = static_cast<std::size_t>(std::max(2, o.points));
    const std::vector<Vec3f> base = combed.size() == n ? combed : resampledGuide(combed, n);
    std::vector<std::pair<float, std::size_t>> near;
    for (std::size_t j = 0; j < keys.size(); ++j) {
        if (keys[j].size() != n) continue;
        near.push_back({(keys[j].front() - root).length(), j});
    }
    std::sort(near.begin(), near.end());
    if (near.size() > static_cast<std::size_t>(std::max(1, o.nearest))) near.resize(static_cast<std::size_t>(std::max(1, o.nearest)));
    std::vector<float> w;
    float wsum = 0.0f;
    for (const auto& [d, j] : near) {
        w.push_back(std::exp(-(d / reach) * (d / reach)));
        wsum += w.back();
    }
    FilledGuide out;
    if (wsum < 1e-3f) {
        out.points = base;
        out.points.front() = root;
        return out;
    }
    const float share = std::min(1.0f, wsum);
    out.keyShare = share;
    out.points.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Vec3f off;
        for (std::size_t k = 0; k < near.size(); ++k) {
            const std::vector<Vec3f>& key = keys[near[k].second];
            off += (key[i] - key.front()) * (w[k] / wsum);
        }
        Vec3f q = (root + off) * share + base[i] * (1.0f - share);
        // Off the head.
        const Vec3f r = q - f.center;
        const float len = r.length();
        const float floorR = shell.head(r) + keepOut * std::min(1.0f, static_cast<float>(i) / 3.0f);
        if (len > 0.0f && len < floorR) q = f.center + r / len * floorR;
        out.points.push_back(q);
    }
    out.points.front() = root;
    return out;
}

}  // namespace groomlab
