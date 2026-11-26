// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_photos.h"

#include "groomlab/pin_morph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "groomlab/editor/agent_canvas.h"

namespace groomlab::agent {

using nlohmann::json;

namespace {

constexpr double kFilmMm = 36.0;
const Rgb kGrid{0.95f, 0.95f, 1.0f};
const Rgb kPaired{0.25f, 1.0f, 0.35f};
const Rgb kPending{1.0f, 0.85f, 0.2f};
const Rgb kMeshPoint{1.0f, 0.3f, 0.25f};
const Rgb kFlow{1.0f, 0.55f, 0.1f};
const Rgb kWire{0.3f, 0.8f, 1.0f};

std::optional<RefCamera> cameraFrom(const json& j, int width, int height) {
    if (!j.is_object() || !j.contains("rotation") || !j.contains("position")) return std::nullopt;
    RefCamera c;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t k = 0; k < 3; ++k) c.pose.rotation[i][k] = j["rotation"].at(i).at(k).get<double>();
    }
    for (std::size_t i = 0; i < 3; ++i) c.pose.position[i] = j["position"].at(i).get<double>();
    c.focalPx = j.value("focal_px", 0.0);
    c.width = width;
    c.height = height;
    return c;
}

// The scale that fits `w` x `h` within `maxSize` (never up).
float fitScale(int w, int h, int maxSize) {
    const int larger = std::max(w, h);
    return larger > maxSize && larger > 0 ? static_cast<float>(maxSize) / static_cast<float>(larger) : 1.0f;
}

void bounds(const ScalpMesh& scalp, Vec3f& lo, Vec3f& hi) {
    const auto& pts = scalp.points();
    if (pts.empty()) throw std::invalid_argument("the scalp has no points");
    lo = hi = pts.front();
    for (const Vec3f& p : pts) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
}

}  // namespace

std::vector<AgentPhoto> photosFromJson(const std::string& text) {
    std::vector<AgentPhoto> out;
    if (text.empty()) return out;
    const json all = json::parse(text, nullptr, false);
    if (!all.is_array()) throw std::invalid_argument("referencePhotos is not an array");
    for (const json& j : all) {
        if (!j.is_object()) throw std::invalid_argument("a reference photo is not an object");
        AgentPhoto p;
        p.name = j.value("name", std::string());
        p.image = j.value("image", std::string());
        p.width = j.value("width", 0);
        p.height = j.value("height", 0);
        p.mask = j.contains("mask") && j["mask"].is_string() ? j["mask"].get<std::string>() : std::string();
        p.camera = cameraFrom(j.value("camera", json()), p.width, p.height);
        p.state = referencePhotoStateFromJson(j.contains("state") ? j["state"].dump() : std::string());
        if (j.contains("display")) p.display = j["display"];
        out.push_back(std::move(p));
    }
    return out;
}

json cameraToJson(const RefCamera& c) {
    json rot = json::array();
    for (const auto& row : c.pose.rotation) rot.push_back({row[0], row[1], row[2]});
    return {{"rotation", rot}, {"position", {c.pose.position[0], c.pose.position[1], c.pose.position[2]}}, {"focal_px", c.focalPx}};
}

std::string photosToJson(const std::vector<AgentPhoto>& photos) {
    if (photos.empty()) return {};
    json all = json::array();
    for (const AgentPhoto& p : photos) {
        json j = {{"name", p.name}, {"image", p.image}, {"width", p.width}, {"height", p.height}};
        j["mask"] = p.mask.empty() ? json() : json(p.mask);
        j["camera"] = p.camera ? cameraToJson(*p.camera) : json();
        j["state"] = json::parse(referencePhotoStateToJson(p.state));
        if (!p.display.is_null()) j["display"] = p.display;
        all.push_back(std::move(j));
    }
    return all.dump();
}

double focalMmToPixels(double mm, double width) { return mm / kFilmMm * width; }
double focalPixelsToMm(double px, double width) { return width > 0.0 ? px * kFilmMm / width : 0.0; }

RefCamera viewCamera(const ScalpMesh& scalp, const std::string& upAxis, const std::string& view, int width, int height, double focalMm) {
    if (width < 16 || height < 16) throw std::invalid_argument("the image is too small");
    Vec3f lo, hi;
    bounds(scalp, lo, hi);
    const Vec3f middle = (lo + hi) * 0.5f;
    const float radius = std::max(0.5f * (hi - lo).length(), 1e-6f);
    const bool zUp = upAxis == "Z";
    const Vec3f up = zUp ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0);
    const Vec3f front = zUp ? Vec3f(0, -1, 0) : Vec3f(0, 0, 1);
    const Vec3f left = up.cross(front);
    Vec3f toEye, screenUp = up;
    if (view == "front") {
        toEye = front;
    } else if (view == "back") {
        toEye = -front;
    } else if (view == "left" || view == "side") {
        toEye = left;
    } else if (view == "right") {
        toEye = -left;
    } else if (view == "top") {
        toEye = up;
        screenUp = front;
    } else if (view == "three_quarter") {
        const float az = 35.0f * 3.14159265f / 180.0f, el = 15.0f * 3.14159265f / 180.0f;
        toEye = (front * (std::cos(az) * std::cos(el)) + left * (std::sin(az) * std::cos(el)) + up * std::sin(el)).normalized();
    } else if (view.rfind("three_quarter_", 0) == 0) {
        const std::string rest = view.substr(14);
        const float az = (rest == "front_left" ? 45.0f : rest == "front_right" ? -45.0f : rest == "back_left" ? 135.0f
                                                                                           : rest == "back_right" ? -135.0f : 999.0f);
        if (az > 360.0f) throw std::invalid_argument("unknown view '" + view + "'");
        const float t = az * 3.14159265f / 180.0f;
        toEye = (front * std::cos(t) + left * std::sin(t)).normalized();
    } else {
        throw std::invalid_argument("unknown view '" + view +
                                    "' (front, back, left, right, top, three_quarter, three_quarter_front_left / _right, "
                                    "three_quarter_back_left / _right)");
    }
    RefCamera c;
    c.width = width;
    c.height = height;
    c.focalPx = focalMmToPixels(focalMm, width);
    const double halfFov = std::atan(0.5 * std::min(width, height) / c.focalPx);
    const float distance = static_cast<float>(1.1 * radius / std::sin(halfFov));
    const Vec3f eye = middle + toEye * distance;
    const Vec3f z = toEye;  // the camera looks down -Z
    const Vec3f x = screenUp.cross(z).normalized();
    const Vec3f y = z.cross(x);
    auto part = [](const Vec3f& v, int r) { return static_cast<double>(r == 0 ? v.x : r == 1 ? v.y : v.z); };
    for (int r = 0; r < 3; ++r) c.pose.rotation[static_cast<std::size_t>(r)] = {part(x, r), part(y, r), part(z, r)};
    c.pose.position = {eye.x, eye.y, eye.z};
    return c;
}

const std::vector<std::string>& viewCameraNames() {
    static const std::vector<std::string> names = {"front", "back", "left", "right", "top", "three_quarter", "three_quarter_front_left",
                                                   "three_quarter_front_right", "three_quarter_back_left", "three_quarter_back_right"};
    return names;
}

std::vector<MeshCandidate> meshCandidates(const ScalpMesh& scalp, const RefCamera& camera, int count) {
    // What the camera sees: the depth of the mesh drawn through it, each vertex in front of it.
    const int w = static_cast<int>(camera.width), h = static_cast<int>(camera.height);
    Canvas canvas(w, h);
    std::vector<float> depth;
    canvas.mesh(scalp, camera, {1, 1, 1}, depth);
    const auto& pts = scalp.points();
    Vec3f lo, hi;
    bounds(scalp, lo, hi);
    const float tolerance = 0.01f * (hi - lo).length();
    std::vector<std::size_t> seen;
    std::vector<std::array<float, 3>> where(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        float u = 0.0f, v = 0.0f, d = 0.0f;
        if (!camera.project(pts[i], u, v, &d) || d <= 0.0f) continue;
        const int x = static_cast<int>(u), y = static_cast<int>(v);
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        float nearest = std::numeric_limits<float>::infinity();  // (the vertex's own pixel and its neighbours)
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int px = std::clamp(x + ox, 0, w - 1), py = std::clamp(y + oy, 0, h - 1);
                nearest = std::min(nearest, depth[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px)]);
            }
        }
        if (d > nearest + tolerance) continue;
        seen.push_back(i);
        where[i] = {u, v, d};
    }
    std::vector<std::size_t> picked;
    auto take = [&](std::size_t i) {
        if (std::find(picked.begin(), picked.end(), i) == picked.end()) picked.push_back(i);
    };
    if (!seen.empty()) {
        // The silhouette's extremes, then the nearest to the camera (a nose, a chin).
        auto extreme = [&](auto better) {
            std::size_t best = seen.front();
            for (const std::size_t i : seen) {
                if (better(where[i], where[best])) best = i;
            }
            take(best);
        };
        extreme([](const auto& a, const auto& b) { return a[1] < b[1]; });  // top
        extreme([](const auto& a, const auto& b) { return a[1] > b[1]; });  // bottom
        extreme([](const auto& a, const auto& b) { return a[0] < b[0]; });  // left
        extreme([](const auto& a, const auto& b) { return a[0] > b[0]; });  // right
        extreme([](const auto& a, const auto& b) { return a[2] < b[2]; });  // nearest
        // The rest spread over what it sees, in the image (farthest point first).
        std::vector<float> gap(pts.size(), std::numeric_limits<float>::infinity());
        auto mark = [&](std::size_t from) {
            for (const std::size_t i : seen) {
                const float du = where[i][0] - where[from][0], dv = where[i][1] - where[from][1];
                gap[i] = std::min(gap[i], du * du + dv * dv);
            }
        };
        for (const std::size_t i : picked) mark(i);
        while (static_cast<int>(picked.size()) < count && picked.size() < seen.size()) {
            std::size_t best = seen.front();
            for (const std::size_t i : seen) {
                if (gap[i] > gap[best]) best = i;
            }
            if (!(gap[best] > 1.0f)) break;
            take(best);
            mark(best);
        }
    }
    std::vector<MeshCandidate> out;
    for (const std::size_t i : picked) {
        if (static_cast<int>(out.size()) >= count) break;
        const SurfacePoint sp = scalp.closestPoint(pts[i]);
        out.push_back({pts[i], sp.normal, where[i][0], where[i][1]});
    }
    return out;
}

std::vector<std::uint8_t> photoGridPng(const RefImage& image, const AgentPhoto& photo, int gridStep, int maxSize,
                                       const ReferenceViewPreview* preview, int* shownWidth, int* shownHeight) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas c = Canvas::ofImage(image, k);
    if (shownWidth) *shownWidth = c.width();
    if (shownHeight) *shownHeight = c.height();
    if (preview && !preview->mask.empty()) {
        const auto& crop = preview->crop;
        for (int y = 0; y < c.height(); ++y) {
            for (int x = 0; x < c.width(); ++x) {
                const float px = (static_cast<float>(x) + 0.5f) / k, py = (static_cast<float>(y) + 0.5f) / k;
                const float mu = (px - static_cast<float>(crop[0])) / static_cast<float>(std::max(1, crop[2])) * static_cast<float>(preview->mask.width);
                const float mv = (py - static_cast<float>(crop[1])) / static_cast<float>(std::max(1, crop[3])) * static_cast<float>(preview->mask.height);
                if (mu < 0.0f || mv < 0.0f || mu >= static_cast<float>(preview->mask.width) || mv >= static_cast<float>(preview->mask.height)) continue;
                const float m = preview->mask.sample(mu, mv);
                if (m > 0.0f) c.set(x, y, {1.0f, 0.2f, 0.8f}, 0.35f * std::min(1.0f, m));
            }
        }
        for (const auto& l : preview->flowLines) c.line(l[0] * k, l[1] * k, l[2] * k, l[3] * k, {0.2f, 1.0f, 1.0f});
    }
    // The grid, labelled in photo pixels along the top and the left.
    const int step = std::max(8, gridStep);
    for (int g = step; g < photo.width || g < photo.height; g += step) {
        if (g < image.width) {
            c.line(static_cast<float>(g) * k, 0.0f, static_cast<float>(g) * k, static_cast<float>(c.height()), kGrid, 1.0f, 0.35f);
            c.text(static_cast<int>(static_cast<float>(g) * k) + 3, 3, std::to_string(g), kGrid, 2);
        }
        if (g < image.height) {
            c.line(0.0f, static_cast<float>(g) * k, static_cast<float>(c.width()), static_cast<float>(g) * k, kGrid, 1.0f, 0.35f);
            c.text(3, static_cast<int>(static_cast<float>(g) * k) + 3, std::to_string(g), kGrid, 2);
        }
    }
    // Flow arrows and mask touch-ups.
    for (const RefHintStroke& s : photo.state.strokes) {
        const ReferenceArrow arrow = referenceArrow(s.points, s.directed ? static_cast<float>(photo.width) / 40.0f : 0.0f);
        for (std::size_t i = 1; i < arrow.line.size(); ++i) {
            c.line(arrow.line[i - 1][0] * k, arrow.line[i - 1][1] * k, arrow.line[i][0] * k, arrow.line[i][1] * k, kFlow, 2.0f);
        }
        if (arrow.hasHead) {
            c.line(arrow.head[0][0] * k, arrow.head[0][1] * k, arrow.head[1][0] * k, arrow.head[1][1] * k, kFlow, 2.0f);
            c.line(arrow.head[2][0] * k, arrow.head[2][1] * k, arrow.head[1][0] * k, arrow.head[1][1] * k, kFlow, 2.0f);
        }
    }
    for (const RefMaskStroke& s : photo.state.maskStrokes) {
        const Rgb col = s.include ? Rgb{1.0f, 0.3f, 0.9f} : Rgb{0.3f, 0.3f, 0.3f};
        for (std::size_t i = 1; i < s.points.size(); ++i) {
            c.line(s.points[i - 1][0] * k, s.points[i - 1][1] * k, s.points[i][0] * k, s.points[i][1] * k, col, std::max(1.0f, 2.0f * s.radius * k),
                   0.5f);
        }
    }
    // The landmarks, numbered.
    const auto& marks = photo.state.landmarks;
    const auto& pts = marks.points(kLandmarkPhoto);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Rgb col = marks.matched(kLandmarkPhoto, i) ? kPaired : kPending;
        const float x = pts[i].point.x * k, y = pts[i].point.y * k;
        c.ring(x, y, 5.0f, col, 2.0f);
        c.text(static_cast<int>(x) + 7, static_cast<int>(y) - 3, std::to_string(i + 1), col, 2);
    }
    return c.png();
}

Vec3f markerOn(const ScalpMesh& from, const ScalpMesh& to, const MarkerPoint& point) {
    StrandRoot root = point.root;
    if (!(root.face >= 0 && static_cast<std::size_t>(root.face) < from.triangleCount())) root = from.closestPoint(point.point).root;
    if (root.face >= 0 && static_cast<std::size_t>(root.face) < to.triangleCount() && from.triangleCount() == to.triangleCount()) return to.position(root);
    return point.point;
}

PhotoMorph morphToPhotos(const ScalpMesh& rest, const ScalpMesh& restRefined, const ScalpMesh& current, const std::vector<AgentPhoto>& photos,
                         double stiffness, bool symmetric, int axis) {
    PhotoMorph out;
    if (rest.pointCount() == 0) {
        out.error = "the groom has no scalp";
        return out;
    }
    if (!(stiffness >= 0.0 && stiffness <= 0.95)) {
        out.error = "stiffness is 0 to 0.95";
        return out;
    }
    Vec3f lo, hi;
    bounds(rest, lo, hi);
    const Vec3f middle = (lo + hi) * 0.5f;
    const float size = std::max(1e-6f, (hi - lo).length());
    // About the head's middle (the morph mirrors across the plane where the axis is 0).
    const Vec3f shift = Vec3f(axis == 0 ? middle.x : 0.0f, axis == 1 ? middle.y : 0.0f, axis == 2 ? middle.z : 0.0f);
    auto vec = [&](const Vec3f& p) { return CameraVec3{p.x - shift.x, p.y - shift.y, p.z - shift.z}; };
    std::vector<PinView> views;
    std::vector<MorphPin> pins;
    for (const AgentPhoto& p : photos) {
        if (!p.camera || !(p.camera->focalPx > 0.0) || p.state.landmarks.pairCount() == 0) continue;
        PinView view;
        view.pose = p.camera->pose;
        for (int i = 0; i < 3; ++i) view.pose.position[static_cast<std::size_t>(i)] -= (i == 0 ? shift.x : i == 1 ? shift.y : shift.z);
        view.focalPx = p.camera->focalPx;
        view.width = p.camera->width;
        view.height = p.camera->height;
        views.push_back(view);
        const int index = static_cast<int>(views.size()) - 1;
        const MarkerLists& marks = p.state.landmarks;
        for (std::size_t k = 0; k < marks.pairCount(); ++k) {
            // The pair's spot on the rest scalp: picked there (its point on it), or on the scalp as
            // it is now (a morph since: the same spot of its surface), whichever it lies on.
            const MarkerPoint& mp = marks.point(kLandmarkMesh, k);
            Vec3f at = markerOn(current, restRefined, mp);
            if (!(mp.root.face >= 0)) {
                const SurfacePoint onRest = restRefined.closestPoint(mp.point), onNow = current.closestPoint(mp.point);
                if ((onRest.position - mp.point).length() <= (onNow.position - mp.point).length()) at = onRest.position;
            }
            const CameraVec3 m = vec(at);
            MorphPin* pin = nullptr;
            for (MorphPin& q : pins) {
                const double d = std::sqrt((q.mesh[0] - m[0]) * (q.mesh[0] - m[0]) + (q.mesh[1] - m[1]) * (q.mesh[1] - m[1]) +
                                           (q.mesh[2] - m[2]) * (q.mesh[2] - m[2]));
                if (d <= 1e-4 * size) pin = &q;
            }
            if (!pin) {
                pins.push_back({m, {}});
                pin = &pins.back();
            }
            const MarkerPoint& photo = marks.point(kLandmarkPhoto, k);
            pin->sights.push_back({index, photo.point.x, photo.point.y});
        }
    }
    if (pins.empty()) {
        out.error = "no lined-up photo has landmark pairs: align_photo first";
        return out;
    }
    std::vector<CameraVec3> points;
    for (const Vec3f& p : rest.points()) points.push_back(vec(p));
    PinMorphOptions options;
    options.stiffness = stiffness;
    options.symmetric = symmetric;
    options.axis = axis;
    const PinMorphResult r = morphToPins(points, pins, views, options);
    if (!r.ok) {
        out.error = r.error.empty() ? std::string("the morph failed") : r.error;
        return out;
    }
    out.ok = true;
    out.pins = static_cast<int>(pins.size());
    out.views = static_cast<int>(views.size());
    out.rmsBefore = r.rmsBeforePx;
    out.rmsAfter = r.rmsAfterPx;
    for (const CameraVec3& p : r.points) {
        out.points.push_back(Vec3f(static_cast<float>(p[0]) + shift.x, static_cast<float>(p[1]) + shift.y, static_cast<float>(p[2]) + shift.z));
    }
    return out;
}

std::vector<std::uint8_t> photoRegionPng(const RefImage& image, const AgentPhoto& photo, const std::array<int, 4>& region, int gridStep,
                                         int maxSize, int* shownWidth, int* shownHeight) {
    const int x0 = std::clamp(region[0], 0, image.width - 1), y0 = std::clamp(region[1], 0, image.height - 1);
    const int x1 = std::clamp(region[2], x0 + 1, image.width), y1 = std::clamp(region[3], y0 + 1, image.height);
    const RefImage part = cropImage(image, x0, y0, x1 - x0, y1 - y0);
    const float k = static_cast<float>(maxSize) / static_cast<float>(std::max(part.width, part.height));
    Canvas c = Canvas::ofImage(part, k);
    if (shownWidth) *shownWidth = c.width();
    if (shownHeight) *shownHeight = c.height();
    const int step = std::max(1, gridStep);
    auto sx = [&](float x) { return (x - static_cast<float>(x0)) * k; };
    auto sy = [&](float y) { return (y - static_cast<float>(y0)) * k; };
    // The grid: every `step` pixels, labelled each time (every other when crowded), in photo pixels.
    const int every = step * k < 28.0f ? 2 : 1;
    int n = 0;
    for (int g = (x0 / step + 1) * step; g < x1; g += step, ++n) {
        c.line(sx(static_cast<float>(g)), 0.0f, sx(static_cast<float>(g)), static_cast<float>(c.height()), kGrid, 1.0f, g % (5 * step) == 0 ? 0.6f : 0.3f);
        if (n % every == 0) c.text(static_cast<int>(sx(static_cast<float>(g))) + 2, 3, std::to_string(g), kGrid, 2);
    }
    n = 0;
    for (int g = (y0 / step + 1) * step; g < y1; g += step, ++n) {
        c.line(0.0f, sy(static_cast<float>(g)), static_cast<float>(c.width()), sy(static_cast<float>(g)), kGrid, 1.0f, g % (5 * step) == 0 ? 0.6f : 0.3f);
        if (n % every == 0) c.text(3, static_cast<int>(sy(static_cast<float>(g))) + 2, std::to_string(g), kGrid, 2);
    }
    const auto& marks = photo.state.landmarks;
    const auto& pts = marks.points(kLandmarkPhoto);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const float x = sx(pts[i].point.x), y = sy(pts[i].point.y);
        if (x < 0 || y < 0 || x > static_cast<float>(c.width()) || y > static_cast<float>(c.height())) continue;
        const Rgb col = marks.matched(kLandmarkPhoto, i) ? kPaired : kPending;
        c.ring(x, y, 6.0f, col, 2.0f);
        c.text(static_cast<int>(x) + 8, static_cast<int>(y) - 3, std::to_string(i + 1), col, 2);
    }
    return c.png();
}

std::vector<std::uint8_t> meshCandidatesPng(const ScalpMesh& scalp, const RefCamera& camera, const std::vector<MeshCandidate>& candidates,
                                            int maxSize, const std::vector<PointLabel>& labels) {
    const float k = fitScale(static_cast<int>(camera.width), static_cast<int>(camera.height), maxSize);
    Canvas c(std::max(1, static_cast<int>(camera.width * k)), std::max(1, static_cast<int>(camera.height * k)));
    std::vector<float> depth;
    c.mesh(scalp, camera, {0.62f, 0.55f, 0.5f}, depth, 1.0f, k);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const float x = candidates[i].u * k, y = candidates[i].v * k;
        c.dot(x, y, 3.0f, kMeshPoint);
        c.text(static_cast<int>(x) + 5, static_cast<int>(y) - 4, std::to_string(i + 1), {1.0f, 1.0f, 0.6f}, 2);
    }
    for (const PointLabel& l : labels) {
        const float x = l.u * k, y = l.v * k;
        c.dot(x, y, 3.0f, {0.2f, 0.9f, 1.0f});
        c.text(static_cast<int>(x) + 5, static_cast<int>(y) + 4, l.code, {0.2f, 0.9f, 1.0f}, 2);
    }
    return c.png();
}

std::vector<std::uint8_t> photoOverlayPng(const RefImage& image, const ScalpMesh& scalp, const AgentPhoto& photo, int maxSize) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas c = Canvas::ofImage(image, k);
    if (photo.camera) {
        for (const auto& s : referenceMeshOverlay(scalp, *photo.camera, false)) c.line(s[0] * k, s[1] * k, s[2] * k, s[3] * k, kWire, 1.0f, 0.35f);
        for (const auto& s : referenceMeshOverlay(scalp, *photo.camera, true)) c.line(s[0] * k, s[1] * k, s[2] * k, s[3] * k, kWire, 2.0f);
    }
    const auto& marks = photo.state.landmarks;
    for (std::size_t i = 0; i < marks.pairCount(); ++i) {
        const Vec3f& at = marks.points(kLandmarkPhoto)[i].point;
        const float x = at.x * k, y = at.y * k;
        if (photo.camera) {
            float u = 0.0f, v = 0.0f;
            if (photo.camera->project(marks.points(kLandmarkMesh)[i].point, u, v)) {
                c.line(x, y, u * k, v * k, {1, 1, 1}, 1.0f);
                c.dot(u * k, v * k, 3.0f, kMeshPoint);
            }
        }
        c.ring(x, y, 5.0f, kPaired, 2.0f);
        c.text(static_cast<int>(x) + 7, static_cast<int>(y) - 3, std::to_string(i + 1), kPaired, 2);
    }
    return c.png();
}

}  // namespace groomlab::agent
