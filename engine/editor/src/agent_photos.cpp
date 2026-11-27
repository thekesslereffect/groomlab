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
        for (const auto& lock : preview->locks)
            for (std::size_t i = 1; i < lock.size(); ++i)
                c.line(lock[i - 1][0] * k, lock[i - 1][1] * k, lock[i][0] * k, lock[i][1] * k, {1.0f, 0.85f, 0.1f});
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

std::vector<std::uint8_t> confidencePng(const RefImage& image, const ReferenceViewPreview& preview, int maxSize) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas c = Canvas::ofImage(image, k);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const Rgb p = c.at(x, y);
            const float g = 0.25f * (p[0] + p[1] + p[2]) / 3.0f;
            c.set(x, y, {g, g, g});
        }
    const auto& crop = preview.crop;
    const RefImage& conf = preview.confidence;
    if (!conf.empty() && crop[2] > 0 && crop[3] > 0)
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                const float px = (static_cast<float>(x) + 0.5f) / k, py = (static_cast<float>(y) + 0.5f) / k;
                const float mu = (px - static_cast<float>(crop[0])) / static_cast<float>(crop[2]) * static_cast<float>(conf.width);
                const float mv = (py - static_cast<float>(crop[1])) / static_cast<float>(crop[3]) * static_cast<float>(conf.height);
                if (mu < 0.0f || mv < 0.0f || mu >= static_cast<float>(conf.width) || mv >= static_cast<float>(conf.height)) continue;
                if (!preview.mask.empty() && preview.mask.sample(mu, mv) < 0.5f) continue;
                const float v = std::clamp(conf.sample(mu, mv), 0.0f, 1.0f);
                // (dark blue 0, green 0.5, yellow 1)
                const Rgb col = v < 0.5f ? Rgb{0.05f, 0.1f + 1.4f * v, 0.5f - 0.6f * v} : Rgb{2.0f * (v - 0.5f), 0.8f + 0.2f * (v - 0.5f) * 2.0f, 0.2f - 0.4f * (v - 0.5f)};
                c.set(x, y, col, 0.85f);
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

PairSnap snapToFeature(const RefImage& image, float u, float v, float window) {
    PairSnap out;
    out.u = u;
    out.v = v;
    if (image.empty()) return out;
    const int r = std::max(3, static_cast<int>(std::ceil(window)));
    const int x0 = std::max(2, static_cast<int>(u) - r), x1 = std::min(image.width - 3, static_cast<int>(u) + r);
    const int y0 = std::max(2, static_cast<int>(v) - r), y1 = std::min(image.height - 3, static_cast<int>(v) + r);
    if (x1 <= x0 || y1 <= y0) return out;
    auto gray = [&](int x, int y) {
        if (image.channels < 3) return image.at(x, y, 0);
        return (image.at(x, y, 0) + image.at(x, y, 1) + image.at(x, y, 2)) / 3.0f;
    };
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<float> xx(static_cast<std::size_t>(w * h)), yy(xx.size()), xy(xx.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float gx = 0.5f * (gray(x0 + x + 1, y0 + y) - gray(x0 + x - 1, y0 + y));
            const float gy = 0.5f * (gray(x0 + x, y0 + y + 1) - gray(x0 + x, y0 + y - 1));
            const std::size_t i = static_cast<std::size_t>(y * w + x);
            xx[i] = gx * gx;
            yy[i] = gy * gy;
            xy[i] = gx * gy;
        }
    // The tensor over 5 x 5, then Harris's response.
    std::vector<float> response(xx.size(), 0.0f);
    double sum = 0.0;
    for (int y = 2; y + 2 < h; ++y)
        for (int x = 2; x + 2 < w; ++x) {
            float a = 0, b = 0, c = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const std::size_t i = static_cast<std::size_t>((y + dy) * w + x + dx);
                    a += xx[i];
                    b += yy[i];
                    c += xy[i];
                }
            const float resp = std::max(0.0f, a * b - c * c - 0.04f * (a + b) * (a + b));
            response[static_cast<std::size_t>(y * w + x)] = resp;
            sum += resp;
        }
    float best = 0.0f, bestRaw = 0.0f;
    const float sigma = 0.6f * static_cast<float>(r);
    for (int y = 2; y + 2 < h; ++y)
        for (int x = 2; x + 2 < w; ++x) {
            const float px = static_cast<float>(x0 + x) + 0.5f, py = static_cast<float>(y0 + y) + 0.5f;
            const float d2 = (px - u) * (px - u) + (py - v) * (py - v);
            const float raw = response[static_cast<std::size_t>(y * w + x)];
            const float score = raw * std::exp(-0.5f * d2 / (sigma * sigma));
            if (score > best) {
                best = score;
                bestRaw = raw;
                out.u = px;
                out.v = py;
            }
        }
    const double mean = sum / static_cast<double>(std::max(1, (w - 4) * (h - 4)));
    out.confidence = bestRaw > 0.0f ? static_cast<float>(std::clamp(1.0 - 4.0 * mean / bestRaw, 0.0, 1.0)) : 0.0f;
    if (out.confidence <= 0.0f) {
        out.u = u;
        out.v = v;
    }
    out.moved = std::hypot(out.u - u, out.v - v);
    return out;
}

std::vector<std::uint8_t> pairCropPng(const RefImage& image, float u, float v, const PairSnap& snap, float window, int zoom) {
    const int r = std::max(4, static_cast<int>(std::ceil(1.5f * window)));
    const int x0 = std::clamp(static_cast<int>(u) - r, 0, std::max(0, image.width - 1));
    const int y0 = std::clamp(static_cast<int>(v) - r, 0, std::max(0, image.height - 1));
    const int x1 = std::clamp(static_cast<int>(u) + r, x0 + 1, image.width), y1 = std::clamp(static_cast<int>(v) + r, y0 + 1, image.height);
    const RefImage crop = cropImage(image, x0, y0, x1 - x0, y1 - y0);
    Canvas canvas = Canvas::ofImage(crop, static_cast<float>(zoom));
    const float k = static_cast<float>(zoom);
    for (int gx = (x0 + 9) / 10 * 10; gx < x1; gx += 10) canvas.line((static_cast<float>(gx - x0)) * k, 0.0f, (static_cast<float>(gx - x0)) * k, static_cast<float>(canvas.height()), {0.3f, 0.6f, 1.0f}, 1.0f, 0.35f);
    for (int gy = (y0 + 9) / 10 * 10; gy < y1; gy += 10) canvas.line(0.0f, (static_cast<float>(gy - y0)) * k, static_cast<float>(canvas.width()), (static_cast<float>(gy - y0)) * k, {0.3f, 0.6f, 1.0f}, 1.0f, 0.35f);
    canvas.ring((u - static_cast<float>(x0)) * k, (v - static_cast<float>(y0)) * k, 2.5f * k, {1.0f, 0.9f, 0.2f}, 2.0f);
    const float sx = (snap.u - static_cast<float>(x0)) * k, sy = (snap.v - static_cast<float>(y0)) * k;
    canvas.line(sx - 3.0f * k, sy, sx + 3.0f * k, sy, {0.2f, 1.0f, 0.35f}, 2.0f);
    canvas.line(sx, sy - 3.0f * k, sx, sy + 3.0f * k, {0.2f, 1.0f, 0.35f}, 2.0f);
    canvas.text(2, 2, std::to_string(x0) + "," + std::to_string(y0), {1.0f, 1.0f, 1.0f});
    return canvas.png();
}

std::vector<std::uint8_t> comparisonPng(const RefImage& image, const PhotoComparison& c, int maxSize) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas canvas = Canvas::ofImage(image, k);
    const Rgb green{0.2f, 1.0f, 0.35f}, magenta{1.0f, 0.25f, 0.85f};
    auto at = [&](const std::vector<std::uint8_t>& m, int x, int y) {
        return x >= 0 && y >= 0 && x < c.width && y < c.height && m[static_cast<std::size_t>(y) * static_cast<std::size_t>(c.width) + static_cast<std::size_t>(x)];
    };
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            const int px = std::min(c.width - 1, static_cast<int>((static_cast<float>(x) + 0.5f) / k));
            const int py = std::min(c.height - 1, static_cast<int>((static_cast<float>(y) + 0.5f) / k));
            const bool p = at(c.photoHair, px, py), g = at(c.groomHair, px, py);
            canvas.set(x, y, {0.0f, 0.0f, 0.0f}, 0.35f);
            if (p && !g) canvas.set(x, y, green, 0.45f);
            if (g && !p) canvas.set(x, y, magenta, 0.45f);
            const auto edge = [&](const std::vector<std::uint8_t>& m) {
                return at(m, px, py) && (!at(m, px - 1, py) || !at(m, px + 1, py) || !at(m, px, py - 1) || !at(m, px, py + 1));
            };
            if (edge(c.photoHair)) canvas.set(x, y, green);
            if (edge(c.groomHair)) canvas.set(x, y, magenta);
        }
    return canvas.png();
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

std::vector<std::uint8_t> shellOverlayPng(const RefImage& image, const RefCamera& camera, const HairShell& shell, int maxSize) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas c = Canvas::ofImage(image, k);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) c.set(x, y, {0.0f, 0.0f, 0.0f}, 0.45f);
    const HeadFrame& f = shell.frame();
    // A point is drawn when the camera sees it in front of the head: nearer than the head's point
    // on the same ray (the shell's own head radius along it).
    auto visible = [&](const Vec3f& p) {
        const Vec3f eye = camera.eye();
        const Vec3f d = (p - eye).normalized();
        const float dist = (p - eye).length();
        const float tc = (f.center - eye).dot(d);
        for (float t = std::max(0.0f, tc - 2.0f * shell.maxOuter()); t < dist - 0.02f * f.radius; t += 0.03f * f.radius) {
            const Vec3f q = eye + d * t - f.center;
            if (q.length() < shell.head(q)) return false;
        }
        return true;
    };
    const float R = f.radius;
    for (int i = 0; i < shell.thetaSteps(); ++i)
        for (int j = 0; j < shell.phiSteps(); ++j) {
            const Vec3f d = shell.direction(i, j);
            const Vec3f headP = f.center + d * shell.headAt(i, j), outerP = f.center + d * shell.outerAt(i, j);
            float u = 0.0f, v = 0.0f;
            const float thick = shell.outerAt(i, j) - shell.headAt(i, j);
            if (thick > 0.05f * R && visible(outerP) && camera.project(outerP, u, v)) {
                const float t = std::clamp(thick / (0.6f * R), 0.0f, 1.0f);
                c.dot(u * k, v * k, 1.2f, {0.2f + 0.8f * t, 0.4f + 0.5f * t, 1.0f - 0.8f * t}, 0.8f);
            }
            if ((i + j) % 4 == 0 && visible(headP) && camera.project(headP, u, v)) c.dot(u * k, v * k, 0.7f, {0.3f, 0.85f, 1.0f}, 0.35f);
        }
    return c.png();
}

std::vector<std::uint8_t> strokesOverlayPng(const RefImage& image, const RefCamera& camera, const std::vector<std::vector<std::array<float, 2>>>& strokes,
                                            const std::vector<std::vector<Vec3f>>& guides, int maxSize) {
    const float k = fitScale(image.width, image.height, maxSize);
    Canvas c = Canvas::ofImage(image, k);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) c.set(x, y, {0.0f, 0.0f, 0.0f}, 0.3f);
    const Rgb red{1.0f, 0.25f, 0.2f}, yellow{1.0f, 0.85f, 0.2f};
    for (std::size_t s = 0; s < strokes.size(); ++s) {
        const auto& st = strokes[s];
        for (std::size_t i = 1; i < st.size(); ++i) c.line(st[i - 1][0] * k, st[i - 1][1] * k, st[i][0] * k, st[i][1] * k, red, 2.0f);
        if (!st.empty()) {
            c.ring(st[0][0] * k, st[0][1] * k, 5.0f, red, 2.0f);
            c.text(static_cast<int>(st.back()[0] * k) + 4, static_cast<int>(st.back()[1] * k), std::to_string(s + 1), red, 1);
        }
    }
    for (const auto& g : guides) {
        float pu = 0.0f, pv = 0.0f;
        bool prev = false;
        for (std::size_t i = 0; i < g.size(); ++i) {
            float u = 0.0f, v = 0.0f;
            const bool ok = camera.project(g[i], u, v);
            if (ok && prev) c.line(pu * k, pv * k, u * k, v * k, yellow, 1.0f);
            if (ok && i == 0) c.dot(u * k, v * k, 2.5f, yellow);
            prev = ok;
            pu = u;
            pv = v;
        }
    }
    return c.png();
}

}  // namespace groomlab::agent
