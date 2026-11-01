// SPDX-License-Identifier: Apache-2.0
// Grooming from photos (reference.h, docs/BLENDER.md "Photos"): the photos the groom keeps (its
// file's "referencePhotos", as the standalone app's Reference window writes them), the camera solve
// from point pairs (reference_edit.h's solveReferenceLandmarks, Maya's groomLabCameraSolve) and
// Guides from Photos (reference_guides.h's guidesFromReference, Maya's from_reference.generate),
// the guides into a guide group of their own as one undo step.

#include "reference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/camera_solve.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/pin_morph.h"
#include "groomlab/reference_edit.h"
#include "groomlab/reference_guides.h"
#include "groomlab/texture.h"

#include "request.h"
#include "session.h"
#include "tools.h"

namespace groomlab::helper {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- Cameras between the host's space and the groom's ---------------------------------------------
//
// A camera crosses the pipe as {"rotation": 3x3 rows (its columns the camera's X right, Y up and Z
// back axes: it looks down -Z, as Maya's and Blender's cameras do), "position": the eye, "focalPx":
// the focal length in the photo's pixels}. The axes turn as directions do (host_space.h's quarter
// turn), the eye as a point; pixels are no length.

CameraVec3 pointToGroom(const HostSpace& sp, const CameraVec3& p) {
    const double s = sp.scale();
    if (sp.zUp()) return {p[0] * s, p[2] * s, -p[1] * s};
    return {p[0] * s, p[1] * s, p[2] * s};
}

CameraVec3 pointToHost(const HostSpace& sp, const CameraVec3& p) {
    const double s = sp.scale();
    if (sp.zUp()) return {p[0] / s, -p[2] / s, p[1] / s};
    return {p[0] / s, p[1] / s, p[2] / s};
}

CameraMat3 rotationToGroom(const HostSpace& sp, const CameraMat3& r) {
    if (!sp.zUp()) return r;
    CameraMat3 g{};
    for (std::size_t j = 0; j < 3; ++j) {
        g[0][j] = r[0][j];
        g[1][j] = r[2][j];
        g[2][j] = -r[1][j];
    }
    return g;
}

CameraMat3 rotationToHost(const HostSpace& sp, const CameraMat3& g) {
    if (!sp.zUp()) return g;
    CameraMat3 r{};
    for (std::size_t j = 0; j < 3; ++j) {
        r[0][j] = g[0][j];
        r[1][j] = -g[2][j];
        r[2][j] = g[1][j];
    }
    return r;
}

double number(const json& v, const char* what) {
    if (!v.is_number()) fail("bad_request", std::string(what) + " must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d)) fail("bad_request", std::string(what) + " must be finite");
    return d;
}

CameraVec3 vec3(const json& v, const char* what) {
    if (!v.is_array() || v.size() != 3) fail("bad_request", std::string(what) + " is [x, y, z]");
    return {number(v[0], what), number(v[1], what), number(v[2], what)};
}

CameraMat3 mat3(const json& v, const char* what) {
    if (!v.is_array() || v.size() != 3) fail("bad_request", std::string(what) + " is 3 rows of 3");
    CameraMat3 m{};
    for (std::size_t i = 0; i < 3; ++i) {
        const CameraVec3 row = vec3(v[i], what);
        for (std::size_t j = 0; j < 3; ++j) m[i][j] = row[j];
    }
    // Its columns made unit length and square to each other (a host's matrix may carry a scale).
    for (std::size_t j = 0; j < 3; ++j) {
        double len = 0.0;
        for (std::size_t i = 0; i < 3; ++i) len += m[i][j] * m[i][j];
        len = std::sqrt(len);
        if (!(len > 1e-12)) fail("bad_request", std::string(what) + " has a zero axis");
        for (std::size_t i = 0; i < 3; ++i) m[i][j] /= len;
    }
    return m;
}

json mat3Json(const CameraMat3& m) {
    json rows = json::array();
    for (const auto& row : m) rows.push_back({row[0], row[1], row[2]});
    return rows;
}

// A host camera {"rotation", "position"} as a pose in groom space.
CameraPose poseToGroom(const HostSpace& sp, const json& cam) {
    CameraPose pose;
    pose.rotation = rotationToGroom(sp, mat3(need(cam, "rotation"), "camera.rotation"));
    pose.position = pointToGroom(sp, vec3(need(cam, "position"), "camera.position"));
    return pose;
}

json poseToHost(const HostSpace& sp, const CameraPose& pose, double focalPx) {
    const CameraVec3 p = pointToHost(sp, pose.position);
    return {{"rotation", mat3Json(rotationToHost(sp, pose.rotation))}, {"position", {p[0], p[1], p[2]}}, {"focalPx", focalPx}};
}

// ---- The photos the groom keeps ---------------------------------------------------------------------

json storedPhotos(const Session& s) {
    if (s.file.referencePhotos.empty()) return json::array();
    json all = json::parse(s.file.referencePhotos, nullptr, false);
    return all.is_array() ? all : json::array();
}

// A stored photo (the app's form, groom space) in the host's.
json photoToHost(const HostSpace& sp, const json& p) {
    json out = {{"name", p.value("name", std::string())},
                {"image", p.value("image", std::string())},
                {"width", p.value("width", 0)},
                {"height", p.value("height", 0)}};
    out["mask"] = p.contains("mask") && p["mask"].is_string() ? p["mask"] : json();
    out["camera"] = nullptr;
    const json cam = p.value("camera", json());
    if (cam.is_object() && cam.contains("rotation") && cam.contains("position")) {
        try {
            CameraPose pose;
            pose.rotation = mat3(cam["rotation"], "camera.rotation");
            pose.position = vec3(cam["position"], "camera.position");
            out["camera"] = poseToHost(sp, pose, cam.value("focal_px", 0.0));
        } catch (const RequestError&) {
            // a camera the app could not read either: none
        }
    }
    json pairs = json::array();
    const json state = p.value("state", json::object());
    for (const json& l : state.value("landmarks", json::array())) {
        json pair = {{"image", nullptr}, {"mesh", nullptr}};
        const json image = l.value("image", json());
        const json mesh = l.value("mesh", json());
        if (image.is_array() && image.size() >= 2) pair["image"] = {image[0], image[1]};
        if (mesh.is_array() && mesh.size() >= 3) {
            const CameraVec3 h = pointToHost(sp, {mesh[0].get<double>(), mesh[1].get<double>(), mesh[2].get<double>()});
            pair["mesh"] = {h[0], h[1], h[2]};
        }
        if (l.contains("error") && l["error"].is_number()) pair["error"] = l["error"];
        pairs.push_back(std::move(pair));
    }
    out["pairs"] = pairs;
    out["rms"] = p.contains("rms") && p["rms"].is_number() ? p["rms"] : json();
    out["strokes"] = state.value("strokes", json::array()).size();
    out["maskStrokes"] = state.value("mask_strokes", json::array()).size();
    return out;
}

// A host photo into the stored form; `old` (the stored photo of that name, or null) keeps what the
// host does not edit: its flow arrows, mask touch-ups and mask.
json photoFromHost(const HostSpace& sp, const json& p, const json& old) {
    const std::string name = needString(p, "name");
    const std::string image = needString(p, "image");
    const double width = number(need(p, "width"), "width"), height = number(need(p, "height"), "height");
    if (!(width >= 1.0 && height >= 1.0)) fail("bad_request", "a photo's width and height are pixels, 1 or more");
    json out = {{"name", name}, {"image", image}, {"width", static_cast<int>(std::lround(width))},
                {"height", static_cast<int>(std::lround(height))}};
    if (p.contains("mask")) {
        out["mask"] = p["mask"].is_string() ? p["mask"] : json();
    } else {
        out["mask"] = old.is_object() && old.contains("mask") ? old["mask"] : json();
    }
    out["camera"] = nullptr;
    if (p.contains("camera") && p["camera"].is_object()) {
        const json& cam = p["camera"];
        const CameraPose pose = poseToGroom(sp, cam);
        const double focal = number(need(cam, "focalPx"), "camera.focalPx");
        if (!(focal > 0.0)) fail("bad_request", "camera.focalPx must be above 0");
        out["camera"] = {{"rotation", mat3Json(pose.rotation)},
                         {"position", {pose.position[0], pose.position[1], pose.position[2]}},
                         {"focal_px", focal}};
    }
    json landmarks = json::array();
    if (p.contains("pairs") && !p["pairs"].is_null()) {
        if (!p["pairs"].is_array()) fail("bad_request", "pairs is a list");
        for (const json& pair : p["pairs"]) {
            if (!pair.is_object()) fail("bad_request", "a pair is {\"image\": [u, v] or null, \"mesh\": [x, y, z] or null}");
            json l = {{"image", nullptr}, {"mesh", nullptr}};
            const json image = pair.value("image", json());
            if (!image.is_null()) {
                if (!image.is_array() || image.size() != 2) fail("bad_request", "a pair's image point is [u, v] (pixels)");
                l["image"] = {number(image[0], "image point"), number(image[1], "image point")};
            }
            const json mesh = pair.value("mesh", json());
            if (!mesh.is_null()) {
                const CameraVec3 g = pointToGroom(sp, vec3(mesh, "a pair's mesh point"));
                l["mesh"] = {g[0], g[1], g[2]};
            }
            if (pair.contains("error") && pair["error"].is_number() && !l["image"].is_null() && !l["mesh"].is_null())
                l["error"] = pair["error"];
            landmarks.push_back(std::move(l));
        }
    }
    json state = old.is_object() ? old.value("state", json::object()) : json::object();
    if (!state.is_object()) state = json::object();
    state["landmarks"] = landmarks;
    if (!state.contains("strokes")) state["strokes"] = json::array();
    if (!state.contains("mask_strokes")) state["mask_strokes"] = json::array();
    out["state"] = state;
    // The app's Photos tab's display (opacity, depth, locked, visible): kept as the app wrote it.
    if (old.is_object() && old.contains("display") && old["display"].is_object()) out["display"] = old["display"];
    if (p.contains("rms") && p["rms"].is_number()) out["rms"] = p["rms"];
    return out;
}

json referencePhotos(Session& s) {
    json photos = json::array();
    for (const json& p : storedPhotos(s)) photos.push_back(photoToHost(s.space, p));
    return {{"photos", photos}};
}

// set_reference_photos: the host's photos replace the groom's (saved with it; no undo step, as the
// app's Reference window stores them).
json setReferencePhotos(Session& s, const json& args) {
    const json& given = need(args, "photos");
    if (!given.is_array()) fail("bad_request", "photos is a list");
    const json before = storedPhotos(s);
    json all = json::array();
    std::set<std::string> names;
    for (const json& p : given) {
        if (!p.is_object()) fail("bad_request", "a photo is an object");
        const std::string name = needString(p, "name");
        if (name.empty()) fail("bad_request", "a photo needs a name");
        if (!names.insert(name).second) fail("bad_request", "two photos are named '" + name + "'");
        json old;
        for (const json& b : before)
            if (b.value("name", std::string()) == name) old = b;
        all.push_back(photoFromHost(s.space, p, old));
    }
    s.file.referencePhotos = all.empty() ? std::string() : all.dump();
    return {{"photos", all.size()}};
}

// ---- The solve ----------------------------------------------------------------------------------------

// solve_camera: the camera that puts each complete pair's mesh point (host space) on its photo point
// (pixels from the top-left, v down): 4 complete pairs with `keepFocal`, else 6 (the focal length
// solved too); `initial` (a host camera) is where it is now.
json solveCameraCommand(Session& s, const json& args) {
    const json& pairs = need(args, "pairs");
    if (!pairs.is_array()) fail("bad_request", "pairs is a list");
    const double width = optNumber(args, "width", 0.0), height = optNumber(args, "height", 0.0);
    if (!(width >= 1.0 && height >= 1.0)) fail("bad_request", "width and height are the photo's pixels");
    const double focal = optNumber(args, "focalPx", 0.0);
    const bool keepFocal = optBool(args, "keepFocal", false);
    MarkerLists landmarks;
    std::vector<std::size_t> used;  // the given pair each complete one is
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const json& pair = pairs[i];
        if (!pair.is_object()) fail("bad_request", "a pair is {\"image\": [u, v], \"mesh\": [x, y, z]}");
        const json image = pair.value("image", json()), mesh = pair.value("mesh", json());
        if (image.is_null() || mesh.is_null()) continue;  // pending: not used
        if (!image.is_array() || image.size() != 2) fail("bad_request", "a pair's image point is [u, v] (pixels)");
        const CameraVec3 g = pointToGroom(s.space, vec3(mesh, "a pair's mesh point"));
        landmarks.add(kLandmarkPhoto, ReferencePhotoEditor::photoPoint(static_cast<float>(number(image[0], "image point")),
                                                                       static_cast<float>(number(image[1], "image point"))));
        MarkerPoint m;
        m.root.face = -1;
        m.point = Vec3f(static_cast<float>(g[0]), static_cast<float>(g[1]), static_cast<float>(g[2]));
        landmarks.add(kLandmarkMesh, m);
        used.push_back(i);
    }
    CameraPose initial;
    const bool hasInitial = args.contains("initial") && args["initial"].is_object();
    if (hasInitial) initial = poseToGroom(s.space, args["initial"]);
    const ReferenceLandmarkSolve r =
        solveReferenceLandmarks(landmarks, width, height, focal, keepFocal, hasInitial ? &initial : nullptr, true);
    if (!r.ok) fail("state", r.why.empty() ? std::string("the camera did not solve") : r.why);
    json errors = json::array(), bad = json::array();
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        errors.push_back(nullptr);
        bad.push_back(false);
    }
    for (std::size_t k = 0; k < used.size(); ++k) {
        if (k < r.errors.size()) errors[used[k]] = r.errors[k];
        if (k < r.bad.size()) bad[used[k]] = static_cast<bool>(r.bad[k]);
    }
    json out = poseToHost(s.space, r.camera.pose, r.camera.focalPx);
    out["pairs"] = r.pairs;
    out["focalSolved"] = r.focalSolved;
    out["rms"] = r.rms;
    out["errors"] = errors;
    out["bad"] = bad;
    return out;
}

// ---- Morph to pins -------------------------------------------------------------------------------------

// morph_mesh: the mesh's points moved so each pin's mesh point lands on its photo points (pin_morph.h).
// All in the host's space (the morph has no groom space of its own): "points" (list or shared
// memory, x y z per point), "pins" [{"mesh": [x, y, z], "sights": [{"view": i, "image": [u, v]}]}],
// "views" [{"rotation", "position", "focalPx", "width", "height"}] (cameras as solve_camera's),
// "stiffness" 0..1, "symmetric" (mirrored across the plane where coordinate "axis", 0 x, is 0). Answers {"points" (flat), "targets", "rmsBefore", "rmsAfter"} (pixels).
json morphMeshCommand(const json& args) {
    BlockReader blocks;
    const std::vector<double> flat = readArray<double>(need(args, "points"), "points", 3, blocks);
    std::vector<CameraVec3> points(flat.size() / 3);
    for (std::size_t i = 0; i < points.size(); ++i) points[i] = {flat[3 * i], flat[3 * i + 1], flat[3 * i + 2]};
    std::vector<PinView> views;
    const json& viewsJson = need(args, "views");
    if (!viewsJson.is_array()) fail("bad_request", "views is a list of cameras");
    for (const json& v : viewsJson) {
        PinView pv;
        pv.pose.rotation = mat3(need(v, "rotation"), "view.rotation");
        pv.pose.position = vec3(need(v, "position"), "view.position");
        pv.focalPx = number(need(v, "focalPx"), "view.focalPx");
        pv.width = number(need(v, "width"), "view.width");
        pv.height = number(need(v, "height"), "view.height");
        views.push_back(pv);
    }
    std::vector<MorphPin> pins;
    const json& pinsJson = need(args, "pins");
    if (!pinsJson.is_array()) fail("bad_request", "pins is a list");
    for (const json& p : pinsJson) {
        MorphPin pin;
        pin.mesh = vec3(need(p, "mesh"), "pin.mesh");
        for (const json& sight : p.value("sights", json::array())) {
            const json& image = need(sight, "image");
            if (!image.is_array() || image.size() != 2) fail("bad_request", "a sight's image point is [u, v] (pixels)");
            pin.sights.push_back({static_cast<int>(number(need(sight, "view"), "sight.view")),
                                  number(image[0], "image point"), number(image[1], "image point")});
        }
        pins.push_back(pin);
    }
    PinMorphOptions options;
    options.stiffness = optNumber(args, "stiffness", 0.0);
    options.symmetric = optBool(args, "symmetric", false);  // about the plane where `axis` is 0
    options.axis = static_cast<int>(optNumber(args, "axis", 0.0));
    const PinMorphResult r = morphToPins(points, pins, views, options);
    if (!r.ok) fail("state", r.error);
    json outPoints = json::array(), targets = json::array();
    for (const CameraVec3& q : r.points) {
        outPoints.push_back(q[0]);
        outPoints.push_back(q[1]);
        outPoints.push_back(q[2]);
    }
    for (const CameraVec3& t : r.targets) targets.push_back({t[0], t[1], t[2]});
    return {{"points", outPoints}, {"targets", targets}, {"rmsBefore", r.rmsBeforePx}, {"rmsAfter", r.rmsAfterPx}};
}

// ---- Guides from Photos -------------------------------------------------------------------------------

std::vector<std::array<float, 2>> pixels(const json& points) {
    std::vector<std::array<float, 2>> out;
    if (!points.is_array()) return out;
    for (const json& p : points)
        if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) out.push_back({p[0].get<float>(), p[1].get<float>()});
    return out;
}

ReferenceView viewOf(const json& p) {
    const std::string name = p.value("name", std::string());
    const json cam = p.value("camera", json());
    if (!cam.is_object()) fail("state", "the photo " + name + " is not lined up yet: Solve its camera first");
    ReferenceView v;
    v.camera.pose.rotation = mat3(cam.at("rotation"), "camera.rotation");
    v.camera.pose.position = vec3(cam.at("position"), "camera.position");
    v.camera.focalPx = cam.value("focal_px", 0.0);
    v.camera.width = p.value("width", 0);
    v.camera.height = p.value("height", 0);
    const std::string image = p.value("image", std::string());
    std::error_code ec;
    if (image.empty() || !fs::is_regular_file(fs::u8path(image), ec)) fail("io", "the photo " + name + " is missing: " + image);
    try {
        v.image = loadRefImage(image, 3);
        if (p.contains("mask") && p["mask"].is_string() && !p["mask"].get<std::string>().empty())
            v.mask = loadRefImage(p["mask"].get<std::string>(), 1);
    } catch (const TextureError& e) {
        fail("io", "cannot read the photo " + name + ": " + e.what());
    }
    const json state = p.value("state", json::object());
    for (const json& h : state.value("strokes", json::array())) {
        RefHintStroke stroke;
        stroke.points = pixels(h.value("points", json()));
        stroke.directed = h.value("directed", true);
        if (stroke.points.size() >= 2) v.hints.push_back(std::move(stroke));
    }
    for (const json& m : state.value("mask_strokes", json::array())) {
        RefMaskStroke stroke;
        stroke.points = pixels(m.value("points", json()));
        stroke.radius = m.value("radius", 8.0f);
        stroke.include = m.value("include", true);
        if (!stroke.points.empty()) v.maskStrokes.push_back(std::move(stroke));
    }
    return v;
}

json lookJson(const ReferenceLookFit& f) {
    auto a3 = [](const std::array<float, 3>& a) { return json{a[0], a[1], a[2]}; };
    return {{"ok", f.ok},
            {"melanin", f.melanin},
            {"redness", f.redness},
            {"baseColor", a3(f.baseColor)},
            {"rootAppearance", a3(f.rootAppearance)},
            {"tipAppearance", a3(f.tipAppearance)},
            {"curly", f.curly},
            {"clumped", f.clumped},
            {"clumpSize", f.clumpSize},
            {"coverage", f.coverage}};
}

// guides_from_photos: every lined-up photo (or `photos`) grows guides into guide group `group`
// ("fromReference", replaced with `replace`), one undo step.
json guidesFromPhotos(Session& s, const json& args) {
    const MeshPtr scalp = s.groom->scalp();
    if (!scalp || scalp->triangleCount() == 0) fail("state", "the session has no scalp: set_scalp first");
    std::set<std::string> only;
    if (args.contains("photos") && args["photos"].is_array())
        for (const json& n : args["photos"])
            if (n.is_string()) only.insert(n.get<std::string>());
    std::vector<ReferenceView> views;
    for (const json& p : storedPhotos(s)) {
        if (!only.empty() && !only.count(p.value("name", std::string()))) continue;
        if (only.empty() && !p.value("camera", json()).is_object()) continue;  // not lined up: left out
        views.push_back(viewOf(p));
    }
    if (views.empty()) fail("state", "no photo lined up with the mesh: import one (Photos), pick point pairs and Solve");
    ReferenceGuideOptions o;
    o.guideCount = static_cast<int>(optNumber(args, "guideCount", 300.0));
    if (o.guideCount < 1 || o.guideCount > 100000) fail("bad_request", "guideCount is 1 to 100000");
    try {
        o.symmetry = referenceSymmetryFromName(optString(args, "symmetry", "auto"));
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    const double maxLength = optNumber(args, "maxLength", 0.0);  // host units; below 0: times the rim (no unit)
    o.maxLength = static_cast<float>(maxLength > 0.0 ? maxLength * s.space.scale() : maxLength);
    const std::string group = optString(args, "group", "fromReference");
    if (group.empty()) fail("bad_request", "group needs a name");
    const bool replace = optBool(args, "replace", true);
    ReferenceGuidesResult r;
    try {
        r = guidesFromReference(*scalp, views, o);
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", std::string("Guides from Photos: ") + e.what());
    }
    const HierarchyPtr authored = s.groom->authoredHierarchy();
    GuideHierarchy h = authored ? *authored : GuideHierarchy{};
    std::size_t added = 0;
    try {
        GroupId gid = h.findGroup(group);
        if (gid != kNoGroup && replace) {
            h.removeGroup(gid);
            gid = kNoGroup;
        }
        if (gid == kNoGroup) gid = h.addGroup(group);
        for (const ReferenceGuide& g : r.guides) {
            if (g.points.size() < 2) continue;
            h.addGuide(gid, g.points);
            ++added;
        }
        h.evaluate(scalp.get());
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    commitTexts(s, "guides from photos", {{StrokeField::Guides, h.toJson()}});
    return {{"guides", added},
            {"group", group},
            {"views", views.size()},
            {"symmetry", referenceSymmetryName(r.symmetry)},
            {"rootsTried", r.rootsTried},
            {"rootsUnseen", r.rootsUnseen},
            {"rootsBald", r.rootsBald},
            {"rootsShort", r.rootsShort},
            {"maxLength", s.space.lengthToHost(r.maxLength)},
            {"look", lookJson(r.look)},
            {"undo", undoStateJson(s)}};
}

}  // namespace

bool isReferenceCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {"reference_photos", "set_reference_photos", "solve_camera", "guides_from_photos",
                                                    "morph_mesh"};
    return kCommands.count(cmd) > 0;
}

json runReferenceCommand(const std::string& cmd, const json& args, const std::function<Session&()>& session) {
    if (cmd == "morph_mesh") return morphMeshCommand(args);  // (no groom needed)
    Session& s = session();
    if (cmd == "reference_photos") return referencePhotos(s);
    if (cmd == "set_reference_photos") return setReferencePhotos(s, args);
    if (cmd == "solve_camera") return solveCameraCommand(s, args);
    return guidesFromPhotos(s, args);
}

}  // namespace groomlab::helper
