// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_canvas.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "groomlab/png.h"

namespace groomlab::agent {

namespace {

// A 3x5 pixel font: five rows of three bits, the top row first, the left pixel the high bit.
const std::map<char, std::array<std::uint8_t, 5>>& font() {
    static const std::map<char, std::array<std::uint8_t, 5>> glyphs{
        {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
        {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
        {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'-', {0, 0, 7, 0, 0}}, {'.', {0, 0, 0, 0, 2}},
        {',', {0, 0, 0, 2, 4}}, {':', {0, 2, 0, 2, 0}}, {'x', {0, 5, 2, 5, 0}}, {' ', {0, 0, 0, 0, 0}},
        {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}}, {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}},
        {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}}, {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}},
        {'I', {7, 2, 2, 2, 7}}, {'J', {1, 1, 1, 5, 2}}, {'K', {5, 5, 6, 5, 5}}, {'L', {4, 4, 4, 4, 7}},
        {'M', {5, 7, 7, 5, 5}}, {'N', {6, 5, 5, 5, 5}}, {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}},
        {'Q', {2, 5, 5, 6, 3}}, {'R', {6, 5, 6, 5, 5}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}},
        {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}}, {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}},
        {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}}};
    return glyphs;
}

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

}  // namespace

Canvas::Canvas(int width, int height, const Rgb& background)
    : w_(std::max(1, width)), h_(std::max(1, height)), pixels_(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), background) {}

Canvas Canvas::ofImage(const RefImage& image, float scale) {
    const int w = std::max(1, static_cast<int>(std::lround(image.width * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(image.height * scale)));
    Canvas c(w, h);
    if (image.empty()) return c;
    const RefImage sized = (w == image.width && h == image.height) ? image : image.resized(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int n = sized.channels;
            const float r = sized.at(x, y, 0);
            c.pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                n >= 3 ? Rgb{r, sized.at(x, y, 1), sized.at(x, y, 2)} : Rgb{r, r, r};
        }
    }
    return c;
}

Rgb Canvas::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return {0, 0, 0};
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
}

void Canvas::set(int x, int y, const Rgb& c, float alpha) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    Rgb& p = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(k)] += (c[static_cast<std::size_t>(k)] - p[static_cast<std::size_t>(k)]) * alpha;
}

void Canvas::line(float x0, float y0, float x1, float y1, const Rgb& c, float width, float alpha) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(len) || len > 8.0f * static_cast<float>(w_ + h_)) return;
    const int steps = std::max(1, static_cast<int>(std::ceil(len * 2.0f)));
    const float r = 0.5f * std::max(1.0f, width);
    const int reach = static_cast<int>(std::ceil(r - 0.5f));
    // (a stamp of the width every half pixel along it, each pixel set once per stamp)
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float px = x0 + dx * t, py = y0 + dy * t;
        for (int oy = -reach; oy <= reach; ++oy) {
            for (int ox = -reach; ox <= reach; ++ox) {
                if (reach > 0 && static_cast<float>(ox * ox + oy * oy) > r * r) continue;
                set(static_cast<int>(std::floor(px)) + ox, static_cast<int>(std::floor(py)) + oy, c, alpha);
            }
        }
    }
}

void Canvas::dot(float x, float y, float radius, const Rgb& c, float alpha) {
    const int x0 = static_cast<int>(std::floor(x - radius)), x1 = static_cast<int>(std::ceil(x + radius));
    const int y0 = static_cast<int>(std::floor(y - radius)), y1 = static_cast<int>(std::ceil(y + radius));
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const float ddx = static_cast<float>(px) + 0.5f - x, ddy = static_cast<float>(py) + 0.5f - y;
            if (ddx * ddx + ddy * ddy <= radius * radius) set(px, py, c, alpha);
        }
    }
}

void Canvas::ring(float x, float y, float radius, const Rgb& c, float width) {
    const int n = std::max(12, static_cast<int>(radius * 6.0f));
    for (int i = 0; i < n; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
        const float a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(n);
        line(x + radius * std::cos(a0), y + radius * std::sin(a0), x + radius * std::cos(a1), y + radius * std::sin(a1), c, width);
    }
}

int Canvas::textWidth(const std::string& s, int scale) { return static_cast<int>(s.size()) * 4 * scale - (s.empty() ? 0 : scale); }

int Canvas::text(int x, int y, const std::string& s, const Rgb& c, int scale, bool boxed) {
    scale = std::max(1, scale);
    const int width = textWidth(s, scale);
    if (boxed) {
        for (int py = y - scale; py < y + 6 * scale; ++py) {
            for (int px = x - scale; px < x + width + scale; ++px) set(px, py, {0.0f, 0.0f, 0.0f}, 0.65f);
        }
    }
    int at = x;
    for (char ch : s) {
        if (ch >= 'a' && ch <= 'z' && ch != 'x') ch = static_cast<char>(ch - 'a' + 'A');
        const auto it = font().find(ch);
        if (it != font().end()) {
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if (!(it->second[static_cast<std::size_t>(row)] & (4 >> col))) continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) set(at + col * scale + sx, y + row * scale + sy, c);
                    }
                }
            }
        }
        at += 4 * scale;
    }
    return width;
}

void Canvas::mesh(const ScalpMesh& m, const RefCamera& camera, const Rgb& color, std::vector<float>& depth, float alpha, float scale) {
    const std::size_t size = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    if (depth.size() != size) depth.assign(size, std::numeric_limits<float>::infinity());
    const auto& pts = m.points();
    std::vector<std::array<float, 3>> screen(pts.size());
    std::vector<bool> front(pts.size(), false);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        float u = 0.0f, v = 0.0f, d = 0.0f;
        front[i] = camera.project(pts[i], u, v, &d) && d > 0.0f;
        screen[i] = {u * scale, v * scale, d};
    }
    const Vec3f eye = camera.eye();
    const Vec3f light = (camera.axis(1) * 0.6f + camera.axis(0) * 0.4f + camera.axis(2)).normalized();
    for (const auto& t : m.triangles()) {
        const std::size_t a = static_cast<std::size_t>(t[0]), b = static_cast<std::size_t>(t[1]), c = static_cast<std::size_t>(t[2]);
        if (!front[a] || !front[b] || !front[c]) continue;
        Vec3f n = (pts[b] - pts[a]).cross(pts[c] - pts[a]);
        if (!(n.length() > 0.0f)) continue;
        n = n.normalized();
        const Vec3f toEye = (eye - pts[a]).normalized();
        if (n.dot(toEye) < 0.0f) n = -n;
        const float shade = 0.25f + 0.75f * std::max(0.0f, n.dot(light));
        const Rgb col{color[0] * shade, color[1] * shade, color[2] * shade};
        const auto& A = screen[a];
        const auto& B = screen[b];
        const auto& C = screen[c];
        const float area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
        if (std::abs(area) < 1e-12f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({A[0], B[0], C[0]}))));
        const int x1 = std::min(w_ - 1, static_cast<int>(std::ceil(std::max({A[0], B[0], C[0]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({A[1], B[1], C[1]}))));
        const int y1 = std::min(h_ - 1, static_cast<int>(std::ceil(std::max({A[1], B[1], C[1]}))));
        for (int y = y0; y <= y1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float w0 = ((B[0] - px) * (C[1] - py) - (B[1] - py) * (C[0] - px)) / area;
                const float w1 = ((C[0] - px) * (A[1] - py) - (C[1] - py) * (A[0] - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * A[2] + w1 * B[2] + w2 * C[2];
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
                if (z >= depth[i]) continue;
                depth[i] = z;
                set(x, y, col, alpha);
            }
        }
    }
}

std::vector<std::uint8_t> Canvas::png() const {
    std::vector<std::uint8_t> bytes(pixels_.size() * 3);
    for (std::size_t i = 0; i < pixels_.size(); ++i) {
        for (std::size_t k = 0; k < 3; ++k) bytes[i * 3 + k] = static_cast<std::uint8_t>(std::lround(clamp01(pixels_[i][k]) * 255.0f));
    }
    return encodePng(w_, h_, 3, bytes);
}

}  // namespace groomlab::agent
