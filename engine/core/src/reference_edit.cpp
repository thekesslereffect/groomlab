// SPDX-License-Identifier: Apache-2.0
// The Reference window's model (reference_edit.h): one photo's landmarks, flow arrows and mask
// touch-ups with an undo stack; the live solve, the mesh over the photo and the arrows' drawing.

#include "groomlab/reference_edit.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace {

using Pixels = std::vector<std::array<float, 2>>;

float segmentDistance(const std::array<float, 2>& p, const std::array<float, 2>& a, const std::array<float, 2>& b) {
    const float du = b[0] - a[0], dv = b[1] - a[1];
    const float l2 = du * du + dv * dv;
    const float t = l2 > 0.0f ? std::clamp(((p[0] - a[0]) * du + (p[1] - a[1]) * dv) / l2, 0.0f, 1.0f) : 0.0f;
    const float eu = p[0] - a[0] - t * du, ev = p[1] - a[1] - t * dv;
    return std::sqrt(eu * eu + ev * ev);
}

nlohmann::json pixelsJson(const Pixels& points) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& p : points) out.push_back({p[0], p[1]});
    return out;
}

Pixels pixelsFrom(const nlohmann::json& j) {
    Pixels out;
    for (const auto& p : j) {
        if (!p.is_array() || p.size() < 2) throw std::invalid_argument("a stroke point is [u, v]");
        out.push_back({p[0].get<float>(), p[1].get<float>()});
    }
    return out;
}

}  // namespace

// ---- ReferencePhotoEditor -------------------------------------------------------------------------

void ReferencePhotoEditor::reset(State state) {
    state_ = std::move(state);
    undo_.clear();
    redo_.clear();
    ++revision_;
}

void ReferencePhotoEditor::record(const char* what) {
    undo_.push_back({state_, what});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    ++revision_;
}

MarkerPoint ReferencePhotoEditor::photoPoint(float u, float v) {
    MarkerPoint p;
    p.root.face = -1;
    p.point = Vec3f(u, v, 0.0f);
    return p;
}

std::size_t ReferencePhotoEditor::addLandmark(MarkerSide side, const MarkerPoint& p) {
    record("landmark");
    return state_.landmarks.add(side, p);
}

std::vector<std::size_t> ReferencePhotoEditor::removeLandmark(MarkerSide side, std::size_t i) {
    (void)state_.landmarks.point(side, i);  // throws before anything is recorded
    record("delete landmark");
    return state_.landmarks.remove(side, i);
}

void ReferencePhotoEditor::moveLandmark(MarkerSide side, std::size_t i, const MarkerPoint& p, bool newStep) {
    (void)state_.landmarks.point(side, i);
    if (newStep || undo_.empty()) {
        record("move landmark");
    } else {
        redo_.clear();
        ++revision_;
    }
    state_.landmarks.set(side, i, p);
}

void ReferencePhotoEditor::clearLandmarks() {
    if (state_.landmarks.empty()) return;
    record("clear landmarks");
    state_.landmarks = MarkerLists{};
}

std::size_t ReferencePhotoEditor::addStroke(Pixels points, bool directed) {
    if (points.size() < 2) throw std::invalid_argument("a flow arrow needs 2 points or more");
    record("flow arrow");
    state_.strokes.push_back({std::move(points), directed});
    return state_.strokes.size() - 1;
}

std::size_t ReferencePhotoEditor::eraseStrokes(const Pixels& path, float radius) {
    const auto near = [&](const RefHintStroke& s) {
        for (const auto& q : path) {
            if (s.points.size() == 1 && segmentDistance(q, s.points[0], s.points[0]) <= radius) return true;
            for (std::size_t i = 1; i < s.points.size(); ++i)
                if (segmentDistance(q, s.points[i - 1], s.points[i]) <= radius) return true;
        }
        return false;
    };
    const std::size_t gone = static_cast<std::size_t>(std::count_if(state_.strokes.begin(), state_.strokes.end(), near));
    if (gone == 0) return 0;
    record("erase flow arrows");
    state_.strokes.erase(std::remove_if(state_.strokes.begin(), state_.strokes.end(), near), state_.strokes.end());
    return gone;
}

void ReferencePhotoEditor::clearStrokes() {
    if (state_.strokes.empty()) return;
    record("clear flow arrows");
    state_.strokes.clear();
}

std::size_t ReferencePhotoEditor::addMaskStroke(Pixels points, float radius, bool include) {
    if (points.empty()) throw std::invalid_argument("a mask stroke needs a point");
    record(include ? "mask: hair" : "mask: backdrop");
    state_.maskStrokes.push_back({std::move(points), std::max(0.5f, radius), include});
    return state_.maskStrokes.size() - 1;
}

void ReferencePhotoEditor::clearMaskStrokes() {
    if (state_.maskStrokes.empty()) return;
    record("clear mask strokes");
    state_.maskStrokes.clear();
}

bool ReferencePhotoEditor::undo() {
    if (undo_.empty()) return false;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({std::move(state_), step.what});
    state_ = std::move(step.state);
    ++revision_;
    return true;
}

bool ReferencePhotoEditor::redo() {
    if (redo_.empty()) return false;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({std::move(state_), step.what});
    state_ = std::move(step.state);
    ++revision_;
    return true;
}

// ---- JSON -----------------------------------------------------------------------------------------

std::string referencePhotoStateToJson(const ReferencePhotoEditor::State& state) {
    nlohmann::json j;
    nlohmann::json landmarks = nlohmann::json::array();
    const auto& photo = state.landmarks.points(kLandmarkPhoto);
    const auto& mesh = state.landmarks.points(kLandmarkMesh);
    for (std::size_t i = 0; i < std::max(photo.size(), mesh.size()); ++i) {
        nlohmann::json pair;
        pair["image"] = i < photo.size() ? nlohmann::json({photo[i].point.x, photo[i].point.y}) : nlohmann::json();
        pair["mesh"] = i < mesh.size() ? nlohmann::json({mesh[i].point.x, mesh[i].point.y, mesh[i].point.z}) : nlohmann::json();
        landmarks.push_back(pair);
    }
    j["landmarks"] = landmarks;
    nlohmann::json strokes = nlohmann::json::array();
    for (const RefHintStroke& s : state.strokes) strokes.push_back({{"points", pixelsJson(s.points)}, {"directed", s.directed}});
    j["strokes"] = strokes;
    nlohmann::json masks = nlohmann::json::array();
    for (const RefMaskStroke& s : state.maskStrokes)
        masks.push_back({{"points", pixelsJson(s.points)}, {"radius", s.radius}, {"include", s.include}});
    j["mask_strokes"] = masks;
    return j.dump();
}

ReferencePhotoEditor::State referencePhotoStateFromJson(const std::string& text) {
    ReferencePhotoEditor::State state;
    if (text.empty()) return state;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
        // Each side's list holds its points in order; a pair missing one side (a pending point)
        // only ever sits at the longer list's tail, so the points keep their pair numbers.
        for (const auto& pair : j.value("landmarks", nlohmann::json::array())) {
            const auto image = pair.value("image", nlohmann::json());
            const auto mesh = pair.value("mesh", nlohmann::json());
            if (image.is_array() && image.size() >= 2)
                state.landmarks.add(kLandmarkPhoto, ReferencePhotoEditor::photoPoint(image[0].get<float>(), image[1].get<float>()));
            if (mesh.is_array() && mesh.size() >= 3) {
                MarkerPoint p;
                p.root.face = -1;
                p.point = Vec3f(mesh[0].get<float>(), mesh[1].get<float>(), mesh[2].get<float>());
                state.landmarks.add(kLandmarkMesh, p);
            }
        }
        for (const auto& s : j.value("strokes", nlohmann::json::array()))
            state.strokes.push_back({pixelsFrom(s.at("points")), s.value("directed", true)});
        for (const auto& s : j.value("mask_strokes", nlohmann::json::array()))
            state.maskStrokes.push_back({pixelsFrom(s.at("points")), s.value("radius", 8.0f), s.value("include", true)});
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(std::string("reference photo state: ") + e.what());
    }
    return state;
}

// ---- The solve ------------------------------------------------------------------------------------

std::vector<bool> referenceBadLandmarks(const std::vector<double>& errors, double width) {
    std::vector<bool> out(errors.size(), false);
    if (errors.size() < 2) return out;
    std::vector<double> sorted = errors;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double limit = std::max({3.0, width / 200.0, 2.5 * median});
    for (std::size_t i = 0; i < errors.size(); ++i) out[i] = errors[i] > limit;
    return out;
}

namespace {
// A photo's plausible focal lengths, pixels per photo width: a 10 mm to a ~1000 mm lens on full
// frame. A free solve from a few pairs, or pairs seen nearly flat (a profile's), can slide toward
// a telephoto miles away (the same picture, the depth all but lost): a camera past the far clip
// that shows no mesh to pick on.
constexpr double kFocalMinWidths = 0.25, kFocalMaxWidths = 30.0;
bool plausibleFocal(double focalPx, double width) {
    return focalPx >= kFocalMinWidths * width && focalPx <= kFocalMaxWidths * width;
}
}  // namespace

ReferenceLandmarkSolve solveReferenceLandmarks(const MarkerLists& landmarks, double width, double height, double focalPx,
                                               bool keepFocal, const CameraPose* initial, bool findBad) {
    ReferenceLandmarkSolve out;
    out.pairs = landmarks.pairCount();
    if (width <= 0.0 || height <= 0.0) {
        out.why = "the photo's size is unknown";
        return out;
    }
    bool fix = keepFocal || out.pairs < kReferenceMinPairsFocal;
    // A focal length a runaway solve left behind (kept or not: no photo is shot that long): back to
    // a normal lens.
    if (focalPx > 0.0 && !plausibleFocal(focalPx, width)) focalPx = width;
    if (out.pairs < kReferenceMinPairs) {
        out.why = std::to_string(kReferenceMinPairs) + " landmark pairs line the camera up (" + std::to_string(out.pairs) +
                  " placed)";
        return out;
    }
    if (fix && focalPx <= 0.0) {
        out.why = "the focal length is unknown: place " + std::to_string(kReferenceMinPairsFocal) + " pairs to solve it";
        return out;
    }
    std::vector<CameraCorrespondence> points;
    const auto& photo = landmarks.points(kLandmarkPhoto);
    const auto& mesh = landmarks.points(kLandmarkMesh);
    for (std::size_t i = 0; i < out.pairs; ++i)
        points.push_back({photo[i].point.x, photo[i].point.y, {mesh[i].point.x, mesh[i].point.y, mesh[i].point.z}});
    CameraSolveOptions options;
    options.imageWidth = width;
    options.imageHeight = height;
    options.focalPx = focalPx;
    options.fixFocal = fix;
    if (initial) {
        options.hasInitialPose = true;
        options.initialPose = *initial;
    }
    CameraSolveResult r = solveCamera(points, options);
    if (r.ok && !fix && !plausibleFocal(r.focalPx, width) && focalPx > 0.0) {
        options.fixFocal = fix = true;  // the pairs cannot tell the focal length: keep the one given
        r = solveCamera(points, options);
    }
    if (!r.ok) {
        out.why = r.error.empty() ? std::string("the camera could not be solved") : r.error;
        return out;
    }
    out.ok = true;
    out.focalSolved = !fix;
    out.camera.pose = r.pose;
    out.camera.focalPx = r.focalPx;
    out.camera.width = width;
    out.camera.height = height;
    out.rms = r.rmsPx;
    out.errors = r.errorsPx;
    for (std::size_t i = 0; i < out.pairs; ++i) {
        double u = 0.0, v = 0.0;
        if (!projectToImage(r.pose, r.focalPx, width, height, points[i].world, u, v)) u = v = -1.0;
        out.projected.push_back({u, v});
    }
    // Which pairs disagree with the rest: each one's error through the camera the others give (the
    // focal length as solved), so a bad pair does not hide among the errors it spreads to the rest;
    // the worst one when it stands out, then again without it.
    out.bad.assign(out.pairs, false);
    std::vector<std::size_t> kept(out.pairs);
    for (std::size_t i = 0; i < out.pairs; ++i) kept[i] = i;
    CameraSolveOptions loo = options;
    loo.focalPx = r.focalPx;
    loo.fixFocal = true;
    loo.hasInitialPose = true;
    loo.initialPose = r.pose;
    while (findBad && kept.size() > kReferenceMinPairs) {
        std::vector<double> alone(kept.size(), 0.0);
        for (std::size_t k = 0; k < kept.size(); ++k) {
            std::vector<CameraCorrespondence> others;
            for (std::size_t j = 0; j < kept.size(); ++j)
                if (j != k) others.push_back(points[kept[j]]);
            const CameraCorrespondence& c = points[kept[k]];
            const CameraSolveResult o = solveCamera(others, loo);
            double u = 0.0, v = 0.0;
            alone[k] = o.ok && projectToImage(o.pose, o.focalPx, width, height, c.world, u, v)
                           ? std::sqrt((u - c.u) * (u - c.u) + (v - c.v) * (v - c.v))
                           : out.errors[kept[k]];
        }
        const std::vector<bool> flagged = referenceBadLandmarks(alone, width);
        const std::size_t worst = static_cast<std::size_t>(std::max_element(alone.begin(), alone.end()) - alone.begin());
        if (!flagged[worst]) break;
        out.bad[kept[worst]] = true;
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(worst));
    }
    return out;
}

// ---- The mesh over the photo ------------------------------------------------------------------------

std::vector<std::array<float, 4>> referenceMeshOverlay(const ScalpMesh& mesh, const RefCamera& camera, bool silhouette) {
    std::vector<std::array<float, 4>> out;
    const auto& points = mesh.points();
    const auto& triangles = mesh.triangles();
    if (triangles.empty()) return out;
    // Each triangle's polygon (ScalpMesh::polygons: triangles per polygon, in order); one each without.
    std::vector<std::int32_t> polygonOf(triangles.size());
    {
        std::size_t t = 0;
        std::int32_t poly = 0;
        for (std::int32_t count : mesh.polygons()) {
            for (std::int32_t k = 0; k < count && t < triangles.size(); ++k) polygonOf[t++] = poly;
            ++poly;
        }
        for (; t < triangles.size(); ++t) polygonOf[t] = poly++;
    }
    // Faces toward the camera.
    const Vec3f eye = camera.eye();
    std::vector<std::uint8_t> front(triangles.size(), 0);
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const Vec3f& a = points[static_cast<std::size_t>(triangles[t][0])];
        const Vec3f& b = points[static_cast<std::size_t>(triangles[t][1])];
        const Vec3f& c = points[static_cast<std::size_t>(triangles[t][2])];
        const Vec3f n = (b - a).cross(c - a);
        front[t] = n.dot(eye - (a + b + c) * (1.0f / 3.0f)) > 0.0f ? 1 : 0;
    }
    // Edges: the (up to two) triangles on each.
    struct Edge {
        std::int32_t t0 = -1, t1 = -1;
    };
    std::unordered_map<std::uint64_t, Edge> edges;
    edges.reserve(triangles.size() * 2);
    const auto key = [](std::int32_t a, std::int32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) | static_cast<std::uint32_t>(b);
    };
    for (std::size_t t = 0; t < triangles.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            Edge& e = edges[key(triangles[t][static_cast<std::size_t>(k)], triangles[t][static_cast<std::size_t>((k + 1) % 3)])];
            if (e.t0 < 0) e.t0 = static_cast<std::int32_t>(t);
            else if (e.t1 < 0) e.t1 = static_cast<std::int32_t>(t);
        }
    std::vector<float> u(points.size()), v(points.size());
    std::vector<std::uint8_t> ok(points.size(), 0);
    for (std::size_t i = 0; i < points.size(); ++i) ok[i] = camera.project(points[i], u[i], v[i]) ? 1 : 0;
    for (const auto& [k, e] : edges) {
        const bool f0 = e.t0 >= 0 && front[static_cast<std::size_t>(e.t0)];
        const bool f1 = e.t1 >= 0 && front[static_cast<std::size_t>(e.t1)];
        bool draw = false;
        if (silhouette) {
            draw = e.t1 < 0 ? f0 : f0 != f1;
        } else {
            const bool inner = e.t1 >= 0 && polygonOf[static_cast<std::size_t>(e.t0)] == polygonOf[static_cast<std::size_t>(e.t1)];
            draw = !inner && (f0 || f1);
        }
        if (!draw) continue;
        const std::size_t a = static_cast<std::size_t>(k >> 32), b = static_cast<std::size_t>(k & 0xffffffffu);
        if (a >= points.size() || b >= points.size() || !ok[a] || !ok[b]) continue;
        out.push_back({u[a], v[a], u[b], v[b]});
    }
    // A stable order (the map's is not): by the first point.
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Arrows -----------------------------------------------------------------------------------------

ReferenceArrow referenceArrow(const Pixels& points, float headSize) {
    ReferenceArrow out;
    if (points.size() < 2) {
        out.line = points;
        return out;
    }
    std::vector<Vec3f> run;
    for (const auto& p : points) run.push_back(Vec3f(p[0], p[1], 0.0f));
    const std::vector<Vec3f> smooth = run.size() > 2 ? smoothPolyline(run.data(), run.size(), 4) : run;
    for (const Vec3f& p : smooth) out.line.push_back({p.x, p.y});
    const Vec3f tip = smooth.back();
    const Vec3f back = smooth[smooth.size() >= 4 ? smooth.size() - 4 : 0];
    Vec3f dir = tip - back;
    if (dir.length() < 1e-6f || headSize <= 0.0f) return out;
    dir = dir.normalized();
    const Vec3f side(-dir.y * 0.6f * headSize, dir.x * 0.6f * headSize, 0.0f);
    const Vec3f base = tip - dir * headSize;
    out.head = {{{base.x + side.x, base.y + side.y}, {tip.x, tip.y}, {base.x - side.x, base.y - side.y}}};
    out.hasHead = true;
    return out;
}

}  // namespace groomlab
