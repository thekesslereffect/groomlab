// SPDX-License-Identifier: Apache-2.0
#include "groomlab/view_projection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "groomlab/random.h"

namespace groomlab {

namespace {

// The inverse of a 4 x 4 matrix (rows first); false when it has none.
bool invert(const std::array<double, 16>& m, std::array<double, 16>& out) {
    std::array<double, 16> inv;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::abs(det) > 1e-300) || !std::isfinite(det)) return false;
    for (std::size_t i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

}  // namespace

ViewProjection::ViewProjection(const std::array<double, 16>& worldToClip, double width, double height)
    : m_(worldToClip), width_(width), height_(height) {
    valid_ = width_ > 0.0 && height_ > 0.0;
    invertible_ = invert(m_, inverse_);
}

bool ViewProjection::project(const Vec3f& p, float& x, float& y) const {
    // As Maya's MPoint * MMatrix: the point's row vector times the matrix.
    const double px = p.x, py = p.y, pz = p.z;
    const double cx = px * m_[0] + py * m_[4] + pz * m_[8] + m_[12];
    const double cy = px * m_[1] + py * m_[5] + pz * m_[9] + m_[13];
    const double cw = px * m_[3] + py * m_[7] + pz * m_[11] + m_[15];
    if (cw <= 1e-9) return false;
    x = static_cast<float>((cx / cw * 0.5 + 0.5) * width_);
    y = static_cast<float>((cy / cw * 0.5 + 0.5) * height_);
    return true;
}

bool ViewProjection::projectPixel(const Vec3f& p, int& x, int& y) const {
    const double px = p.x, py = p.y, pz = p.z;
    const double cx = px * m_[0] + py * m_[4] + pz * m_[8] + m_[12];
    const double cy = px * m_[1] + py * m_[5] + pz * m_[9] + m_[13];
    const double cz = px * m_[2] + py * m_[6] + pz * m_[10] + m_[14];
    const double cw = px * m_[3] + py * m_[7] + pz * m_[11] + m_[15];
    if (cw <= 1e-9 || std::abs(cx) > cw || std::abs(cy) > cw || std::abs(cz) > cw) return false;
    x = static_cast<int>((cx / cw * 0.5 + 0.5) * width_);
    y = static_cast<int>((cy / cw * 0.5 + 0.5) * height_);
    return true;
}

bool ViewProjection::unproject(float x, float y, Vec3f& nearPoint, Vec3f& farPoint) const {
    if (!valid_ || !invertible_) return false;
    const double nx = static_cast<double>(x) / width_ * 2.0 - 1.0, ny = static_cast<double>(y) / height_ * 2.0 - 1.0;
    auto at = [&](double z, Vec3f& out) {
        const double c[4] = {nx, ny, z, 1.0};
        double r[4] = {0.0, 0.0, 0.0, 0.0};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) r[col] += c[row] * inverse_[static_cast<std::size_t>(row * 4 + col)];
        }
        if (std::abs(r[3]) < 1e-300) return false;
        out = {static_cast<float>(r[0] / r[3]), static_cast<float>(r[1] / r[3]), static_cast<float>(r[2] / r[3])};
        return true;
    };
    return at(-1.0, nearPoint) && at(1.0, farPoint);
}

bool ViewProjection::projectBox(const Vec3f& lo, const Vec3f& hi, float& x0, float& y0, float& x1, float& y1) const {
    x0 = y0 = std::numeric_limits<float>::max();
    x1 = y1 = -std::numeric_limits<float>::max();
    for (int k = 0; k < 8; ++k) {
        const Vec3f c{(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z};
        float x = 0.0f, y = 0.0f;
        if (!project(c, x, y)) return false;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    return true;
}

std::uint64_t ViewProjection::hash() const {
    std::uint64_t h = hash64(0x5C2EE7);
    for (const double v : m_) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        h = hashCombine(h, bits);
    }
    std::uint64_t w = 0, hh = 0;
    std::memcpy(&w, &width_, sizeof(w));
    std::memcpy(&hh, &height_, sizeof(hh));
    return hashCombine(hashCombine(h, w), hh);
}

// ---- What the scalp hides ----------------------------------------------------------

namespace {

float scalpCell(const ScalpMesh& scalp) {
    const float big = std::numeric_limits<float>::max();
    Vec3f lo(big, big, big), hi(-big, -big, -big);
    for (const Vec3f& p : scalp.points()) {
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const float size = scalp.points().empty() ? 1.0f : (hi - lo).length();
    return std::max(size / 150.0f, 1e-6f);
}

// 21 bits a coordinate: cells up to a million wide either way of the origin.
std::int64_t cellIndex(float v, float cell) {
    const auto i = static_cast<std::int64_t>(std::floor(v / cell)) + (std::int64_t{1} << 20);
    return std::clamp<std::int64_t>(i, 0, (std::int64_t{1} << 21) - 1);
}

}  // namespace

std::uint64_t ScalpVisibility::keyOf(const ScalpMesh& scalp, const Vec3f& eye, const Vec3f& viewDirection, bool orthographic) {
    std::uint64_t h = hashCombine(hash64(0x0CC1D3), scalp.surfaceHash());
    for (const float f : {eye.x, eye.y, eye.z, viewDirection.x, viewDirection.y, viewDirection.z}) h = hashFloat(h, f);
    return hashCombine(h, orthographic ? 1u : 0u);
}

ScalpVisibility::ScalpVisibility(std::shared_ptr<const ScalpMesh> scalp, const Vec3f& eye, const Vec3f& viewDirection,
                                 bool orthographic)
    : scalp_(std::move(scalp)), eye_(eye), along_(viewDirection), ortho_(orthographic) {
    if (!scalp_) return;
    cell_ = scalpCell(*scalp_);
    key_ = keyOf(*scalp_, eye_, along_, ortho_);
}

bool ScalpVisibility::cast(std::int64_t ix, std::int64_t iy, std::int64_t iz) const {
    const float c = cell_;
    const auto centerOf = [c](std::int64_t i) { return (static_cast<float>(i - (std::int64_t{1} << 20)) + 0.5f) * c; };
    const Vec3f center{centerOf(ix), centerOf(iy), centerOf(iz)};
    Vec3f origin, direction;
    float distance = 0.0f;
    if (ortho_) {
        direction = along_;
        distance = 1e4f;
        origin = center - direction * distance;
    } else {
        const Vec3f to = center - eye_;
        distance = to.length();
        if (distance < 1e-6f) return false;
        direction = to * (1.0f / distance);
        origin = eye_;
    }
    SurfacePoint hit;
    float t = 0.0f;
    if (!scalp_->intersectRay(origin, direction, &hit, &t)) return false;
    // The scalp is met before the cell, by more than the cell's own size (twice its diagonal: a
    // cell the visible surface crosses stays visible up to 75 degrees from facing the camera).
    const float slack = 2.0f * 1.7320508f * c;
    return t < distance - std::max(1e-4f, 1e-4f * distance) - slack;
}

bool ScalpVisibility::hidden(const Vec3f& p) const {
    if (!scalp_) return false;
    const std::int64_t ix = cellIndex(p.x, cell_), iy = cellIndex(p.y, cell_), iz = cellIndex(p.z, cell_);
    const std::uint64_t key = static_cast<std::uint64_t>(ix) | (static_cast<std::uint64_t>(iy) << 21) | (static_cast<std::uint64_t>(iz) << 42);
    Shard& shard = shards_[hash64(key) % kShards];
    {
        const std::lock_guard<std::mutex> lock(shard.mutex);
        const auto it = shard.cells.find(key);
        if (it != shard.cells.end()) return it->second;
    }
    const bool result = cast(ix, iy, iz);  // the same answer whoever casts it first
    const std::lock_guard<std::mutex> lock(shard.mutex);
    shard.cells.emplace(key, result);
    return result;
}

std::size_t ScalpVisibility::cellsCast() const {
    std::size_t count = 0;
    for (Shard& shard : shards_) {
        const std::lock_guard<std::mutex> lock(shard.mutex);
        count += shard.cells.size();
    }
    return count;
}

// ---- The screen brush ------------------------------------------------------------------

BrushWeightFn screenBrushWeight(const ViewProjection& view, float x, float y, float radius, const BrushFalloff& falloff,
                                std::shared_ptr<const ScalpVisibility> visibility, AlphaStamp alpha) {
    return [view, x, y, radius, falloff, visibility = std::move(visibility), alpha = std::move(alpha)](const Vec3f& p) {
        float px = 0.0f, py = 0.0f;
        if (!view.project(p, px, py)) return 0.0f;
        const float dx = px - x, dy = py - y;
        float w = falloff(std::sqrt(dx * dx + dy * dy), radius);
        if (w > 0.0f && alpha) w *= alpha.at(dx, dy, radius);
        return w > 0.0f && visibility && visibility->hidden(p) ? 0.0f : w;  // the ray only for points in reach
    };
}

BrushReachFn screenBrushReach(const ViewProjection& view, float x, float y, float radius) {
    return [view, x, y, radius](const Vec3f& lo, const Vec3f& hi) {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!view.projectBox(lo, hi, x0, y0, x1, y1)) return true;  // partly behind the camera: no bound
        // The nearest point of the projection's bounds to the cursor, a pixel of slack for rounding.
        const float dx = std::max({x0 - x, 0.0f, x - x1}), dy = std::max({y0 - y, 0.0f, y - y1});
        const float reach = radius + 1.0f;
        return dx * dx + dy * dy < reach * reach;
    };
}

// ---- A tool's view ----------------------------------------------------------------------

ToolView ToolView::fromMatrix(const std::array<double, 16>& worldToClip, double width, double height) {
    ToolView view;
    view.projection = ViewProjection(worldToClip, width, height);
    const ViewProjection& v = view.projection;
    Vec3f nearPoint, farPoint;
    if (!v.valid() || !v.unproject(static_cast<float>(width * 0.5), static_cast<float>(height * 0.5), nearPoint, farPoint)) {
        return view;
    }
    const Vec3f along = farPoint - nearPoint;
    if (along.length() < 1e-12f) return view;
    view.viewDirection = along.normalized();
    // The eye is where clip x, y and w are 0: (0, 0, 1, 0) through the inverse, at infinity for
    // an orthographic view (its rays are parallel).
    const std::array<double, 16>& inv = v.inverse();
    const double w = inv[11];
    const double scale = std::max({std::abs(inv[8]), std::abs(inv[9]), std::abs(inv[10]), 1e-300});
    view.orthographic = std::abs(w) <= 1e-12 * scale;
    view.eye = view.orthographic ? nearPoint
                                 : Vec3f{static_cast<float>(inv[8] / w), static_cast<float>(inv[9] / w), static_cast<float>(inv[10] / w)};
    view.hasCamera = true;
    return view;
}

bool ToolView::project(const Vec3f& p, float& x, float& y) const {
    if (projection.valid()) return projection.project(p, x, y);
    return hostProject && hostProject(p, x, y);
}

bool ToolView::ray(float x, float y, Vec3f& origin, Vec3f& direction) const {
    if (hostRay) return hostRay(x, y, origin, direction);
    Vec3f nearPoint, farPoint;
    if (!projection.unproject(x, y, nearPoint, farPoint)) return false;
    const Vec3f d = farPoint - nearPoint;
    if (d.length() < 1e-12f) return false;
    origin = nearPoint;
    direction = d.normalized();
    return true;
}

bool ToolView::camera(Vec3f& eyeOut, Vec3f& directionOut, bool& orthographicOut) const {
    if (hostCamera) return hostCamera(eyeOut, directionOut, orthographicOut);
    if (!hasCamera) return false;
    eyeOut = eye;
    directionOut = viewDirection;
    orthographicOut = orthographic;
    return true;
}

BrushWeightFn ToolView::brushWeight(float x, float y, float radius, const BrushFalloff& falloff,
                                    std::shared_ptr<const ScalpVisibility> visibility, AlphaStamp alpha) const {
    if (projection.valid()) return screenBrushWeight(projection, x, y, radius, falloff, std::move(visibility), std::move(alpha));
    return [project = hostProject, occlusion = std::move(visibility), alpha = std::move(alpha), x, y, radius, falloff](const Vec3f& p) {
        float px = 0.0f, py = 0.0f;
        if (!project || !project(p, px, py)) return 0.0f;
        const float dx = px - x, dy = py - y;
        float w = falloff(std::sqrt(dx * dx + dy * dy), radius);
        if (w > 0.0f && alpha) w *= alpha.at(dx, dy, radius);
        return w > 0.0f && occlusion && occlusion->hidden(p) ? 0.0f : w;  // the ray only for points in reach
    };
}

BrushReachFn ToolView::brushReach(float x, float y, float radius) const {
    if (!projection.valid()) return {};
    return screenBrushReach(projection, x, y, radius);
}

// ---- Points on screen -------------------------------------------------------------------

ScreenPointGrid::ScreenPointGrid(const ViewProjection& view, const std::vector<const std::vector<Vec3f>*>& polylines, float cell)
    : cell_(std::max(cell, 1.0f)), count_(polylines.size()) {
    // Every projected point, by polyline.
    std::vector<std::pair<float, float>> points;
    std::vector<std::uint32_t> owner;
    float lx = std::numeric_limits<float>::max(), ly = lx, hx = -lx, hy = -lx;
    for (std::size_t k = 0; k < polylines.size(); ++k) {
        if (!polylines[k]) continue;
        for (const Vec3f& p : *polylines[k]) {
            float x = 0.0f, y = 0.0f;
            if (!view.project(p, x, y) || !std::isfinite(x) || !std::isfinite(y)) continue;  // behind the camera: weight 0
            points.emplace_back(x, y);
            owner.push_back(static_cast<std::uint32_t>(k));
            lx = std::min(lx, x);
            ly = std::min(ly, y);
            hx = std::max(hx, x);
            hy = std::max(hy, y);
        }
    }
    if (points.empty()) return;
    // At most 512 cells a side: far-off points (a guide crossing the camera's plane) widen cells.
    cell_ = std::max({cell_, (hx - lx) / 512.0f, (hy - ly) / 512.0f});
    x0_ = lx;
    y0_ = ly;
    nx_ = static_cast<int>((hx - lx) / cell_) + 1;
    ny_ = static_cast<int>((hy - ly) / cell_) + 1;
    const std::size_t cells = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
    auto cellOf = [&](float x, float y) {
        const int ix = std::clamp(static_cast<int>((x - x0_) / cell_), 0, nx_ - 1);
        const int iy = std::clamp(static_cast<int>((y - y0_) / cell_), 0, ny_ - 1);
        return static_cast<std::size_t>(iy) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(ix);
    };
    // A polyline once per cell it has points in (its points come in order: repeats are consecutive
    // within a cell's run of the same polyline).
    std::vector<std::size_t> cellOfPoint(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) cellOfPoint[i] = cellOf(points[i].first, points[i].second);
    std::vector<std::uint32_t> last(cells, std::numeric_limits<std::uint32_t>::max());
    cellStart_.assign(cells + 1, 0);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t c = cellOfPoint[i];
        if (last[c] == owner[i]) continue;
        last[c] = owner[i];
        ++cellStart_[c + 1];
    }
    for (std::size_t c = 0; c < cells; ++c) cellStart_[c + 1] += cellStart_[c];
    cellLines_.resize(cellStart_[cells]);
    std::fill(last.begin(), last.end(), std::numeric_limits<std::uint32_t>::max());
    std::vector<std::uint32_t> fill(cellStart_.begin(), cellStart_.end() - 1);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t c = cellOfPoint[i];
        if (last[c] == owner[i]) continue;
        last[c] = owner[i];
        cellLines_[fill[c]++] = owner[i];
    }
}

std::vector<std::uint32_t> ScreenPointGrid::near(float x, float y, float radius) const {
    std::vector<std::uint32_t> out;
    if (nx_ == 0) return out;
    const float r = std::max(radius, 0.0f) + 1.0f;
    const float fx0 = (x - r - x0_) / cell_, fx1 = (x + r - x0_) / cell_;
    const float fy0 = (y - r - y0_) / cell_, fy1 = (y + r - y0_) / cell_;
    if (fx1 < 0.0f || fy1 < 0.0f || fx0 >= static_cast<float>(nx_) || fy0 >= static_cast<float>(ny_)) return out;
    const int ix0 = std::clamp(static_cast<int>(std::floor(fx0)), 0, nx_ - 1), ix1 = std::clamp(static_cast<int>(std::floor(fx1)), 0, nx_ - 1);
    const int iy0 = std::clamp(static_cast<int>(std::floor(fy0)), 0, ny_ - 1), iy1 = std::clamp(static_cast<int>(std::floor(fy1)), 0, ny_ - 1);
    for (int iy = iy0; iy <= iy1; ++iy) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const std::size_t c = static_cast<std::size_t>(iy) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(ix);
            out.insert(out.end(), cellLines_.begin() + cellStart_[c], cellLines_.begin() + cellStart_[c + 1]);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

float worldRadiusAt(const ToolView& view, const Vec3f& center, float x, float y, float screenRadius) {
    Vec3f eye, along;
    bool ortho = false;
    if (!view.camera(eye, along, ortho)) return 0.0f;
    // The plane through the center facing the camera; the ray one radius to the right meets it
    // where the circle's edge is in the world.
    const Vec3f normal = ortho ? along : (center - eye).normalized();
    Vec3f origin, direction;
    if (!view.ray(x + screenRadius, y, origin, direction)) return 0.0f;
    const float denominator = direction.dot(normal);
    if (std::abs(denominator) < 1e-9f) return 0.0f;
    const float t = (center - origin).dot(normal) / denominator;
    if (t <= 0.0f) return 0.0f;
    return (origin + direction * t - center).length();
}

}  // namespace groomlab
