// SPDX-License-Identifier: Apache-2.0
// GroomLab's own tools for the host (tools.h, docs/BLENDER.md "Tools"): the core's BrushTool over
// a SessionStrokeHost of the session's groom, driven in the host's region pixels through a view
// the host gives in its own space (turned into the groom's here), as the standalone app's
// BrushSession drives it in its viewport; the selection, hidden groups and the guide hierarchy's
// edits; painted maps; the undo stack (one step per stroke or edit); the display buffers
// DisplayBuilder builds for Maya's viewport, answered in the host's space.

#include "tools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/asset_files.h"
#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/card_files.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/correctives.h"
#include "groomlab/flow.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/guide_selection.h"
#include "groomlab/parts.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/root_subdivision.h"
#include "groomlab/root_tubes.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/stroke.h"
#include "groomlab/texture.h"
#include "groomlab/tube.h"
#include "groomlab/view_projection.h"

#include "request.h"
#include "session.h"

namespace groomlab::helper {

namespace fs = std::filesystem;

struct ToolState;

namespace {

void writeTexture(ToolState& t, const std::string& name);

}  // namespace

// The session's groom as the stroke host: inputs and previews through the session, each edit
// applied to it and kept for undo (SessionStrokeHost); texture maps painted in their files (kept
// open between strokes while the file is what they wrote, as every host keeps them).
class HelperStrokeHost : public SessionStrokeHost {
public:
    HelperStrokeHost(Session& s, ToolState& t) : SessionStrokeHost(*s.groom), s_(s), t_(t) {}

    std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override;
    // Kept: a dropped stroke put its paint back as it was (BrushTool::dropMapStroke), and undo
    // and redo write their tiles into it.
    void dropTexturePaint() override {}
    void commit(StrokeEdit edit) override;
    // A Shape or Trim brush's Ctrl+Shift drag (BrushTool): the tools' selection as painted, and at
    // its end every other guide frozen (none: all unfrozen), one undo step, as the Select tool's
    // solo.
    bool paintSelection(const std::vector<GuideId>& selection, bool final) override;

    std::map<std::string, KeptTexturePaint> kept;  // by map name

private:
    Session& s_;
    ToolState& t_;
};

struct PaintTarget {
    std::string attribute = "clump";
    float value = 1.0f;
    std::string kind = "guides", texture;
};

// A file an edit wrote, moved or removed (a texture map's image): its bytes before and after (not
// `before` / not `after`: no file then), so undo and redo put it back as the edit found and left it.
using FileEdit = FileState;  // a file as an edit found and left it (asset_files.h: every host's)

// What an undo step did beside the stored texts the host's stack keeps: files, and the guide
// groups' own colors (display only, kept with the tools' state as Maya keeps guideGroupColors on
// its node).
struct SideEdit {
    std::vector<FileEdit> files;
    bool colors = false;
    std::map<std::string, std::string> colorsBefore, colorsAfter;
    // Other state an edit changed (commitStep: a simulation's bakes, a transfer's scalp): each
    // puts it as the edit left it (true) or found it (false).
    std::vector<std::function<void(bool after)>> actions;
    bool empty() const { return files.empty() && !colors && actions.empty(); }
};

struct ToolState {
    explicit ToolState(Session& s)
        : session(s), host(s, *this), overlayOut("s" + std::to_string(s.id) + "overlay"),
          displayOut("s" + std::to_string(s.id) + "display"), colorsOut("s" + std::to_string(s.id) + "colors") {
        // As the app's BrushSession starts the tool.
        tool.settings.brush.radius = 0.25f;
        tool.settings.screenRadius = 60.0f;
        static std::uint64_t made = 0;
        generation = blockName("s" + std::to_string(s.id) + "display", static_cast<unsigned>(++made));
    }

    Session& session;
    HelperStrokeHost host;
    BrushTool tool;
    PaintTarget beforeFreeze;  // what Paint painted before Freeze took the brush
    bool selectTool = false;   // the Select tool (the lasso): no strokes

    // The view the pixels are in (the last one given), groom space.
    bool hasView = false;
    ToolView view;
    std::array<double, 16> rows{};
    double width = 0.0, height = 0.0;
    bool hasTarget = false;
    Vec3f target;

    // The guide selection (ids), its soft radius (groom units) and the hidden groups.
    std::vector<GuideId> selection;
    float softRadius = 0.0f;
    std::set<std::string> hidden;
    std::uint64_t selectionVersion = 1;

    // The corrective layer strokes sculpt into ("" the base groom; setShotLayer).
    std::string shotLayer;

    // The stroke in flight.
    float pressX = 0.0f, pressY = 0.0f;
    // The undo steps before the stroke's press: a press can commit one (a root closed by a click
    // on its first CV makes its tube), stroke_end reports it with the release's.
    std::size_t pressEdits = 0;

    // What redo would bring back (labels and texture maps), beside the host's stack.
    std::vector<std::string> redoLabels, redoMaps;
    // One SideEdit per step of the host's stack (done) and per step redo would bring back, and
    // the one the next commit takes (an edit's files written, or its colors, before it commits).
    std::vector<SideEdit> doneSide, redoSide;
    SideEdit pending;
    // Files the next commitStep's edit writes (sideFileBefore): their bytes before it.
    std::vector<FileEdit> pendingFiles;

    // The guide groups given their own viewport color: name -> "#rrggbb" (the display's
    // guideGroupColors unless the host's display settings give theirs).
    std::map<std::string, std::string> groupColors;
    // The groom's name the automatic group colors start from (guideHueSeed): the display
    // settings' guideColorSeed, or guides_info's, as last given.
    std::string colorSeed;

    // The brush overlay: the ring, the guides it would touch, a Part or Flow stroke's path.
    DisplayGeometry overlay;
    std::uint64_t overlayVersion = 0;
    bool ringValid = false, ringOnScalp = false;
    Vec3f ringCenter, ringNormal;
    float ringRadius = 0.0f;
    // With symmetry on: the scalp's line of symmetry (symmetrySeam, segment pairs), in red in the
    // overlay as Maya draws it, made again for another scalp or symmetry.
    std::vector<Vec3f> seam;
    std::uint64_t seamKey = 0;
    // The Tube tool's root-first mode (BrushTool::roots): its outlines, CVs and middles.
    RootOverlayGeometry roots;

    // The display (Maya's buffers) and the strand colors.
    DisplayBuilder builder;
    std::string generation;
    Arena overlayOut, displayOut, colorsOut;
    std::uint64_t colorsKey = 0, colorsVersion = 0;
};

Session::Session(int id)
    : id(id), strands("s" + std::to_string(id) + "strands"), guides("s" + std::to_string(id) + "guides"),
      scalp("s" + std::to_string(id) + "scalp") {}

Session::~Session() = default;

namespace {

ToolState& toolsOf(Session& s) {
    if (!s.tools) s.tools = std::make_unique<ToolState>(s);
    return *s.tools;
}

// ---- Spaces -------------------------------------------------------------------------------------

json hostPoint(const HostSpace& space, const Vec3f& p) {
    float h[3];
    space.toHost(p, h);
    return {h[0], h[1], h[2]};
}

Vec3f hostDirection(const HostSpace& space, const Vec3f& d) {
    return space.zUp() ? Vec3f{d.x, -d.z, d.y} : d;
}

Vec3f groomPointOf(const json& v, const HostSpace& space, const char* what) {
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) {
        fail("bad_request", std::string(what) + " is a point [x, y, z]");
    }
    return space.toGroom(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
}

float toGroomLength(const HostSpace& space, double v) { return static_cast<float>(v * space.scale()); }
double toHostLength(const HostSpace& space, float v) { return static_cast<double>(v) / space.scale(); }

// The host's symmetry axis ("x", "y", "z", "none") and center as the groom's: Z up, the host's y
// is the groom's -z and its z the groom's y.
void symmetryToGroom(const HostSpace& space, const std::string& axis, double center, int& groomAxis, float& groomCenter) {
    const int a = symmetryAxisFromName(axis);  // throws std::invalid_argument
    const float c = toGroomLength(space, center);
    if (a < 0 || !space.zUp()) {
        groomAxis = a;
        groomCenter = c;
    } else if (a == 0) {
        groomAxis = 0;
        groomCenter = c;
    } else if (a == 1) {
        groomAxis = 2;
        groomCenter = -c;
    } else {
        groomAxis = 1;
        groomCenter = c;
    }
}

void symmetryToHost(const HostSpace& space, int groomAxis, float groomCenter, std::string& axis, double& center) {
    if (groomAxis < 0) {
        axis = "none";
        center = toHostLength(space, groomCenter);
        return;
    }
    int a = groomAxis;
    double c = toHostLength(space, groomCenter);
    if (space.zUp()) {
        if (groomAxis == 2) {
            a = 1;
            c = -c;
        } else if (groomAxis == 1) {
            a = 2;
        }
    }
    axis = symmetryAxisName(a);
    center = c;
}

// ---- Views --------------------------------------------------------------------------------------

using Mat4 = std::array<double, 16>;  // rows first: m[r * 4 + c]

Mat4 matrixOf(const json& v, const char* what) {
    std::vector<double> values;
    if (v.is_array()) {
        for (const json& row : v) {
            if (row.is_array()) {
                for (const json& x : row) {
                    if (!x.is_number()) fail("bad_request", std::string(what) + " holds something that is not a number");
                    values.push_back(x.get<double>());
                }
            } else if (row.is_number()) {
                values.push_back(row.get<double>());
            } else {
                fail("bad_request", std::string(what) + " holds something that is not a number");
            }
        }
    }
    if (values.size() != 16) fail("bad_request", std::string(what) + " is a 4x4 matrix (4 rows of 4, or 16 numbers rows first)");
    Mat4 m{};
    for (std::size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(values[i])) fail("bad_request", std::string(what) + " holds a NaN or an infinity");
        m[i] = values[i];
    }
    return m;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 m{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a[static_cast<std::size_t>(r * 4 + k)] * b[static_cast<std::size_t>(k * 4 + c)];
            m[static_cast<std::size_t>(r * 4 + c)] = sum;
        }
    }
    return m;
}

// The groom's point (centimeters, Y up) as the host's (column vectors: host = C * groom).
Mat4 groomToHost(const HostSpace& space) {
    const double k = 1.0 / space.scale();
    Mat4 m{};
    m[15] = 1.0;
    if (space.zUp()) {
        m[0] = k;   // x = gx
        m[6] = -k;  // y = -gz
        m[9] = k;   // z = gy
    } else {
        m[0] = m[5] = m[10] = k;
    }
    return m;
}

// The view a request gives (host space; Blender's matrices as they print: rows first, column
// vectors, OpenGL clip space): its world-to-clip matrix taken to the groom's space in the core's
// convention (row vectors: the transpose), over a port of the region's size.
void setView(ToolState& t, const json& v) {
    const HostSpace& space = t.session.space;
    if (!v.is_object()) fail("bad_request", "view is an object: viewMatrix, projectionMatrix (or perspectiveMatrix), width, height");
    Mat4 clip;
    if (v.contains("perspectiveMatrix") && !v["perspectiveMatrix"].is_null()) {
        clip = matrixOf(v["perspectiveMatrix"], "view.perspectiveMatrix");
    } else {
        clip = multiply(matrixOf(need(v, "projectionMatrix"), "view.projectionMatrix"), matrixOf(need(v, "viewMatrix"), "view.viewMatrix"));
    }
    if (v.contains("objectMatrix") && !v["objectMatrix"].is_null()) clip = multiply(clip, matrixOf(v["objectMatrix"], "view.objectMatrix"));
    const Mat4 full = multiply(clip, groomToHost(space));
    Mat4 rows{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) rows[static_cast<std::size_t>(r * 4 + c)] = full[static_cast<std::size_t>(c * 4 + r)];
    }
    const double width = optNumber(v, "width", 0.0), height = optNumber(v, "height", 0.0);
    if (!(width > 0.0) || !(height > 0.0)) fail("bad_request", "view.width and view.height are the region's size in pixels");
    ToolView view = ToolView::fromMatrix(rows, width, height);
    if (!view.projection.valid() || !view.projection.invertible() || !view.hasCamera) {
        fail("bad_request", "the view's matrix cannot be inverted: no camera to brush from");
    }
    t.view = std::move(view);
    t.rows = rows;
    t.width = width;
    t.height = height;
    t.hasView = true;
    t.hasTarget = v.contains("target") && !v["target"].is_null();
    if (t.hasTarget) {
        // The target is in the host's world; with an object matrix, the add-on gives it in the
        // scalp object's space (as every point it sends).
        t.target = groomPointOf(v["target"], space, "view.target");
    }
}

void viewFromArgs(ToolState& t, const json& args) {
    if (args.contains("view") && !args["view"].is_null()) setView(t, args["view"]);
    if (!t.hasView) fail("state", "no view: give \"view\" (the region's matrices and size)");
}

json viewJson(const ToolState& t) {
    const HostSpace& space = t.session.space;
    json j;
    j["eye"] = hostPoint(space, t.view.eye);
    const Vec3f d = hostDirection(space, t.view.viewDirection);
    j["viewDirection"] = {d.x, d.y, d.z};
    j["orthographic"] = t.view.orthographic;
    j["groomMatrix"] = t.rows;  // the core's world-to-clip in groom space (rows first, row vectors)
    j["width"] = t.width;
    j["height"] = t.height;
    return j;
}

// The depth the ring sits at off the scalp: the view's target, else the scalp's middle.
bool depthPoint(ToolState& t, Vec3f& out) {
    if (t.hasTarget) {
        out = t.target;
        return true;
    }
    const MeshPtr scalp = t.session.groom->rawScalp();
    if (!scalp || scalp->pointCount() == 0) return false;
    Vec3f lo = scalp->points().front(), hi = lo;
    for (const Vec3f& p : scalp->points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    out = (lo + hi) * 0.5f;
    return true;
}

bool atTargetDepth(ToolState& t, float x, float y, Vec3f& out) {
    Vec3f origin, direction, eye, along, target;
    bool ortho = false;
    if (!t.view.ray(x, y, origin, direction) || !t.view.camera(eye, along, ortho) || !depthPoint(t, target)) return false;
    const float d = (target - origin).dot(along) / std::max(direction.dot(along), 1e-6f);
    if (d <= 0.0f) return false;
    out = origin + direction * d;
    return true;
}

// ---- Tubes in host units ------------------------------------------------------------------------

// A tube as tubeToJson writes it (widths in the groom's centimeters, at the size shown) with its
// widths in host units: {"shape", "width": [...], "aspect": [...], "twist": [...] (radians)}.
json tubeToHost(const HostSpace& space, const std::string& text) {
    json j = json::parse(text);
    if (j.contains("width") && j["width"].is_array()) {
        for (json& w : j["width"]) w = toHostLength(space, w.get<float>());
    }
    return j;
}

// A tube edit from the host (editedTube's keys; "width" a number or a list in host units) as the
// core takes it.
std::string tubeEditToGroom(const HostSpace& space, json edit) {
    if (edit.contains("width")) {
        json& w = edit["width"];
        auto toGroom = [&](json& x) {
            if (!x.is_number() || !std::isfinite(x.get<double>())) fail("bad_request", "tube width is a number or a list of numbers (host units)");
            x = toGroomLength(space, x.get<double>());
        };
        if (w.is_array()) {
            for (json& x : w) toGroom(x);
        } else {
            toGroom(w);
        }
    }
    return edit.dump();
}

// ---- Settings -----------------------------------------------------------------------------------

constexpr const char* kFreezeTool = "freeze";
constexpr const char* kSelectTool = "select";

std::string toolIdOf(const ToolState& t) {
    if (t.selectTool) return kSelectTool;
    const BrushToolSettings& s = t.tool.settings;
    if (s.tool == BrushToolSettings::Tool::Brush && s.brush.mode == BrushMode::Attribute && s.brush.attribute == kFreezeTool &&
        s.paintKind == "guides") {
        return kFreezeTool;
    }
    return s.modeName();
}

// The rail's tools by the catalog's ids, as the app's BrushSession::setTool picks them: a brush
// mode, "freeze" (Paint on the guides' freeze mask toward 1; Ctrl: 0), or "select" (the lasso).
void setTool(ToolState& t, const std::string& id) {
    BrushToolSettings& s = t.tool.settings;
    const bool freezing = toolIdOf(t) == kFreezeTool;
    if (id == kSelectTool) {
        t.selectTool = true;
        return;
    }
    if (id == kFreezeTool) {
        t.selectTool = false;
        if (!freezing) t.beforeFreeze = {s.brush.attribute, s.brush.value, s.paintKind, s.paintTexture};
        s.setMode("paint");
        s.brush.freeze = kFreezeTool;
        s.brush.attribute = kFreezeTool;
        s.brush.value = 1.0f;
        s.paintKind = "guides";
        s.paintTexture.clear();
        return;
    }
    if (id == "width" || id == "density") fail("bad_request", std::string(kRemovedParamBrushes) + " (the helper: map_for_param)");
    try {
        BrushToolSettings().setMode(id);  // throws for an unknown id, before anything changes
    } catch (const std::invalid_argument&) {
        fail("bad_request", "no tool '" + id + "': the catalog's ids (tool_catalog), select, freeze or clump");
    }
    t.selectTool = false;
    if (freezing) {
        s.brush.attribute = t.beforeFreeze.attribute;
        s.brush.value = t.beforeFreeze.value;
        s.paintKind = t.beforeFreeze.kind;
        s.paintTexture = t.beforeFreeze.texture;
    }
    s.setMode(id);
}

float settingNumber(const json& v, const char* key) {
    if (!v.is_number()) fail("bad_request", std::string("setting ") + key + " is a number");
    const double d = v.get<double>();
    if (!std::isfinite(d)) fail("bad_request", std::string("setting ") + key + " must be finite");
    return static_cast<float>(d);
}

bool settingBool(const json& v, const char* key) {
    if (!v.is_boolean()) fail("bad_request", std::string("setting ") + key + " is true or false");
    return v.get<bool>();
}

int settingInt(const json& v, const char* key) {
    if (!v.is_number_integer()) fail("bad_request", std::string("setting ") + key + " is a whole number");
    return v.get<int>();
}

std::string settingString(const json& v, const char* key) {
    if (v.is_null()) return {};
    if (!v.is_string()) fail("bad_request", std::string("setting ") + key + " is a string");
    return v.get<std::string>();
}

// The settings the palette sends (Maya's groomLabBrushCtx flags by their long names, and the
// catalog's setting ids): lengths in host units, the symmetry in the host's axes.
void applySettings(ToolState& t, const json& settings) {
    if (!settings.is_object()) fail("bad_request", "settings is an object");
    BrushToolSettings s = t.tool.settings;
    float softRadius = t.softRadius;
    const HostSpace& space = t.session.space;
    std::string symmetry;
    bool symmetrySet = false;
    double symmetryCenter = 0.0;
    {
        std::string axis;
        symmetryToHost(space, s.brush.mirrorAxis, s.brush.mirrorCenter, axis, symmetryCenter);
        symmetry = axis;
    }
    try {
        for (const auto& [key, v] : settings.items()) {
            const char* k = key.c_str();
            if (key == "radius") s.setScreenRadius(settingNumber(v, k));
            else if (key == "strength") s.brush.strength = settingNumber(v, k);
            else if (key == "falloff") s.setHardness(1.0f - settingNumber(v, k));
            else if (key == "hardness") s.setHardness(settingNumber(v, k));
            // The curves: a ramp ([[position, value], ...], or its JSON text), null or "" for none.
            else if (key == "falloffProfile") s.setFalloffProfile(v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump());
            else if (key == "strengthProfile") s.setStrengthProfile(v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump());
            else if (key == "level") s.brush.level = settingInt(v, k);
            else if (key == "group") s.brush.group = settingString(v, k);
            else if (key == "propagate") s.brush.propagate = settingBool(v, k);
            else if (key == "length") s.place.length = toGroomLength(space, settingNumber(v, k));
            else if (key == "segments") s.setSegments(settingInt(v, k));
            else if (key == "tubeRadius") {
                const float r = toGroomLength(space, settingNumber(v, k));
                s.place.tube.width = {r, r};
            } else if (key == "tube") {
                // The new guides' tube (Maya's placeTube): shape, width (host units), aspect, twist.
                if (!v.is_object()) fail("bad_request", "setting tube is an object: shape, width, aspect, twist");
                setPlacementTube(s.place, tubeEditToGroom(space, v));
            } else if (key == "tubeChannel") s.brush.tubeChannel = tubeChannelFromString(settingString(v, k));
            else if (key == "spacing") s.place.spacing = toGroomLength(space, settingNumber(v, k));
            else if (key == "matchNeighbors") s.place.matchNeighbors = settingBool(v, k);
            else if (key == "soloNewGuides") s.place.soloNewGuides = settingBool(v, k);
            else if (key == "selectedOnly") s.selectedOnly = settingBool(v, k);
            else if (key == "softSelectRadius") softRadius = std::max(0.0f, toGroomLength(space, settingNumber(v, k)));
            else if (key == "symmetry") {
                symmetry = settingString(v, k);
                if (symmetry.empty()) symmetry = "none";
                symmetrySet = true;
            } else if (key == "symmetryCenter") {
                symmetryCenter = settingNumber(v, k);
                symmetrySet = true;
            } else if (key == "attribute") s.brush.attribute = settingString(v, k);
            else if (key == "value") s.brush.value = settingNumber(v, k);
            else if (key == "pointwise") s.brush.pointwise = settingBool(v, k);
            else if (key == "collide") s.brush.collide = settingBool(v, k);
            else if (key == "frontFacingOnly" || key == "maskByScalp") s.maskByScalp = settingBool(v, k);
            else if (key == "stretch") s.brush.stretch = settingBool(v, k);
            else if (key == "straighten") s.brush.straighten = settingBool(v, k);
            else if (key == "pressure") s.pressure = settingBool(v, k);
            else if (key == "partName") s.partName = settingString(v, k).empty() ? std::string("part") : settingString(v, k);
            else if (key == "flowSink") {
                const std::string sink = settingString(v, k);
                if (!sink.empty()) flowSinkAngle(sink);  // throws for an unknown kind
                s.flowSink = sink;
            } else if (key == "targets") {
                if (v.is_array()) {
                    std::string names;
                    for (const json& n : v) names += (names.empty() ? "" : ",") + settingString(n, k);
                    s.setTargets(names.empty() ? std::string("none") : names);
                } else {
                    s.setTargets(settingString(v, k));
                }
            } else if (key == "eraseGuides") s.eraseGuides = settingBool(v, k);
            else if (key == "eraseParts") s.eraseParts = settingBool(v, k);
            else if (key == "eraseFlow") s.eraseFlow = settingBool(v, k);
            else if (key == "paintKind") {
                const std::string kind = settingString(v, k);
                if (kind != "guides" && kind != "vertices" && kind != "texture") fail("bad_request", "paintKind is guides, vertices or texture");
                s.paintKind = kind;
            } else if (key == "paintTexture") s.paintTexture = settingString(v, k);
            else if (key == "paintResolution") s.setPaintResolution(settingInt(v, k));
            else if (key == "paintColor") {
                if (!v.is_array() || (v.size() != 3 && v.size() != 4)) fail("bad_request", "paintColor is [r, g, b] or [r, g, b, a]");
                PaintColor c{0.0f, 0.0f, 0.0f, 1.0f};
                for (std::size_t i = 0; i < v.size(); ++i) c[i] = settingNumber(v[i], k);
                s.setPaintColor(c);
            } else if (key == "paintChannels") s.paintChannels = channelMask(settingString(v, k).empty() ? std::string("rgba") : settingString(v, k));
            else if (key == "paintMode") s.brush.paintMode = paintModeFromString(settingString(v, k));
            else if (key == "paintWholeFaces") s.paintWholeFaces = settingBool(v, k);
            else if (key == "smoothStrength") s.setSmoothStrength(settingNumber(v, k));
            else if (key == "alpha") {
                try {
                    s.setAlpha(v.is_null() ? std::string() : settingString(v, k));
                } catch (const std::invalid_argument&) {
                    throw;
                } catch (const std::exception& e) {
                    fail("bad_request", std::string("alpha: ") + e.what());
                }
            } else if (key == "alphaRotation") s.setAlphaRotation(settingString(v, k));
            else if (key == "alphaAngle") s.alphaAngle = settingNumber(v, k);
            else if (key == "alphaSpacing") s.setAlphaSpacing(settingNumber(v, k));
            else if (key == "alphaInvert") s.alphaInvert = settingBool(v, k);
            else fail("bad_request", "no tool setting '" + key + "'");
        }
        if (symmetrySet && symmetry == "topology") {
            s.setSymmetry("topology");  // across the scalp's own surface: no axis to turn into the groom's
        } else if (symmetrySet) {
            symmetryToGroom(space, symmetry, symmetryCenter, s.brush.mirrorAxis, s.brush.mirrorCenter);
            s.brush.mirrorTopology = false;
        }
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    t.tool.settings = s;
    t.softRadius = softRadius;
}

json settingsJson(const ToolState& t) {
    const BrushToolSettings& s = t.tool.settings;
    const HostSpace& space = t.session.space;
    std::string axis;
    double center = 0.0;
    symmetryToHost(space, s.brush.mirrorAxis, s.brush.mirrorCenter, axis, center);
    json j;
    j["radius"] = s.screenRadius;
    j["strength"] = s.brush.strength;
    j["falloff"] = 1.0f - s.brush.hardness;
    j["hardness"] = s.brush.hardness;
    // The curves as ramps (null: none).
    j["falloffProfile"] = s.brush.falloffProfile ? json::parse(s.falloffProfileJson()) : json();
    j["strengthProfile"] = s.brush.strengthProfile ? json::parse(s.strengthProfileJson()) : json();
    j["level"] = s.brush.level;
    j["group"] = s.brush.group;
    j["propagate"] = s.brush.propagate;
    j["length"] = toHostLength(space, s.place.length);
    j["segments"] = s.place.segments;
    j["tubeRadius"] = toHostLength(space, s.place.tube.width.empty() ? 0.0f : s.place.tube.width.front());
    j["tube"] = tubeToHost(space, placementTubeJson(s.place));
    j["tubeChannel"] = tubeChannelName(s.brush.tubeChannel);
    j["spacing"] = toHostLength(space, s.place.spacing);
    j["matchNeighbors"] = s.place.matchNeighbors;
    j["soloNewGuides"] = s.place.soloNewGuides;
    j["selectedOnly"] = s.selectedOnly;
    j["softSelectRadius"] = toHostLength(space, t.softRadius);
    j["symmetry"] = s.brush.mirrorTopology ? std::string("topology") : axis;
    j["symmetryCenter"] = center;
    j["attribute"] = s.brush.attribute;
    j["value"] = s.brush.value;
    j["pointwise"] = s.brush.pointwise;
    j["collide"] = s.brush.collide;
    j["frontFacingOnly"] = s.maskByScalp;
    j["stretch"] = s.brush.stretch;
    j["straighten"] = s.brush.straighten;
    j["pressure"] = s.pressure;
    j["partName"] = s.partName;
    j["flowSink"] = s.flowSink;
    j["targets"] = s.targetsName();
    j["eraseGuides"] = s.eraseGuides;
    j["eraseParts"] = s.eraseParts;
    j["eraseFlow"] = s.eraseFlow;
    j["paintKind"] = s.paintKind;
    j["paintTexture"] = s.paintTexture;
    j["paintResolution"] = s.paintResolution;
    j["paintColor"] = s.paintColor;
    j["paintChannels"] = channelLetters(s.paintChannels);
    j["paintMode"] = paintModeName(s.brush.paintMode);
    j["paintWholeFaces"] = s.paintWholeFaces;
    j["smoothStrength"] = s.smoothStrength;
    j["alpha"] = s.alphaPath;
    j["alphaRotation"] = s.alphaRotationName();
    j["alphaAngle"] = s.alphaAngle;
    j["alphaSpacing"] = s.alphaSpacing;
    j["alphaInvert"] = s.alphaInvert;
    return j;
}

json toolJson(const ToolState& t) {
    const BrushEntry* entry = brushEntry(toolIdOf(t));
    return {{"tool", toolIdOf(t)},
            {"mode", t.tool.settings.modeName()},
            {"settings", settingsJson(t)},
            {"shows", entry ? json(entry->settings) : json::array()},
            {"ringColor", brushRingColor(t.tool.settings)},
            {"pathColor", brushPathColor(t.tool.settings)},
            {"hoverColor", brushHoverColor(t.tool.settings)}};
}

// ---- Selection ----------------------------------------------------------------------------------

// The selection less the guides that are gone (an undo, an erase).
void pruneSelection(ToolState& t) {
    if (t.selection.empty()) return;
    const std::shared_ptr<const GuideHierarchy> h = t.session.groom->storedHierarchy();
    std::vector<GuideId> kept;
    for (const GuideId id : t.selection) {
        if (h && h->guide(id)) kept.push_back(id);
    }
    if (kept.size() != t.selection.size()) {
        t.selection = std::move(kept);
        ++t.selectionVersion;
    }
}

void setSelection(ToolState& t, std::vector<GuideId> ids) {
    if (ids == t.selection) return;
    t.selection = std::move(ids);
    ++t.selectionVersion;
}

// What strokes read of the selection and the hidden groups.
// Shot sculpting into the target layer at the session's time (the app's CorrectivesPanel::applyTarget).
void applyShotLayer(ToolState& t) {
    const double seconds = t.session.seconds;
    const long frame = std::lround(secondsToFrame(seconds, t.session.groom->framesPerSecond()));
    t.host.setShotSculpt(!t.shotLayer.empty(), seconds, frame, t.shotLayer);
}

void prepareHost(ToolState& t) {
    pruneSelection(t);
    t.host.setSelection(t.selection, t.softRadius);
    t.host.setHiddenGroups(t.hidden);
    applyShotLayer(t);
}

// The brushes' targets an edit or an undo took away go back to the base groom: a corrective layer
// no longer in the stack, a Sculpt node no longer in the graph (Maya's follow_graph_edit and
// remove_layer clear theirs).
void pruneTargets(ToolState& t) {
    GroomSession& g = *t.session.groom;
    if (!t.shotLayer.empty()) {
        bool found = false;
        try {
            found = Correctives::fromJson(g.data().correctivesJson).layer(t.shotLayer) != nullptr;
        } catch (const std::exception&) {
        }
        if (!found) t.shotLayer.clear();
    }
    const std::string node = g.sculptTarget();
    if (!node.empty()) {
        bool sculpt = false;
        try {
            const GraphEdit edit(g.data().graphJson);
            sculpt = edit.hasNode(node) && edit.nodeType(node) == "Sculpt";
        } catch (const std::exception&) {
        }
        if (!sculpt) g.setSculptTarget({});
    }
    applyShotLayer(t);
}

json selectionJson(const ToolState& t) {
    return {{"selection", t.selection}, {"count", t.selection.size()}, {"softSelectRadius", toHostLength(t.session.space, t.softRadius)},
            {"version", t.selectionVersion}};
}

std::vector<GuideId> idsOf(const json& v) {
    std::vector<GuideId> ids;
    if (!v.is_array()) fail("bad_request", "ids is a list of guide IDs");
    for (const json& id : v) {
        if (!id.is_number_unsigned() && !(id.is_number_integer() && id.get<std::int64_t>() > 0)) fail("bad_request", "a guide ID is a positive integer");
        ids.push_back(id.get<GuideId>());
    }
    return ids;
}

// ---- Side edits: files and group colors ---------------------------------------------------------


// The texture maps' paints kept open between strokes let go: a file an edit wrote (or undo put
// back) is read again by the next stroke, fill or undo of a stroke.
void dropPaints(ToolState& t) {
    for (const auto& kept : t.host.kept) t.host.setTexturePaint(kept.first, nullptr);
    t.host.kept.clear();
}

// `path` as it is now, before the edit that is about to change it (FileEdit::before)...
FileEdit fileBefore(const std::string& path) { return FileEdit{path, readFileState(path), std::nullopt}; }

// ... and after it: kept for the next commit's undo step.
void fileAfter(ToolState& t, FileEdit e) {
    e.after = readFileState(e.path);
    t.pending.files.push_back(std::move(e));
}

// The same for every file a map's image is (udim.h mapImageFiles: each tile of a tile set,
// `tiles` among them).
std::vector<FileEdit> filesBefore(const std::string& path, const std::vector<int>& tiles = {}) {
    std::vector<FileEdit> out;
    for (const std::string& file : mapImageFiles(path, tiles)) out.push_back(fileBefore(file));
    return out;
}

void filesAfter(ToolState& t, std::vector<FileEdit> edits) {
    for (FileEdit& e : edits) fileAfter(t, std::move(e));
}

void applySide(ToolState& t, const SideEdit& side, bool after) {
    restoreFileStates(side.files, after);
    if (!side.files.empty()) dropPaints(t);
    if (side.colors) t.groupColors = after ? side.colorsAfter : side.colorsBefore;
    if (after) {
        for (const auto& action : side.actions) action(true);
    } else {
        for (auto action = side.actions.rbegin(); action != side.actions.rend(); ++action) (*action)(false);
    }
}

// An edit that failed after writing its files: they are put back, nothing kept for undo.
void dropPending(ToolState& t) {
    if (!t.pending.empty()) applySide(t, t.pending, false);
    t.pending = SideEdit{};
}

// The guide groups' colors as the display reads them (DisplaySettings::guideGroupColors):
// "name:#rrggbb name:#rrggbb".
std::string groupColorsText(const ToolState& t) { return guideGroupColorsText(t.groupColors); }  // (display.h)

}  // namespace

void writeGroupDisplay(const Session& s, GroomFile& file) {
    if (!s.tools) return;  // (no tools yet: as the file had them)
    file.guideGroupColors = groupColorsText(*s.tools);
    file.hiddenGuideGroups = hiddenGuideGroupsText(s.tools->hidden);
}

void adoptGroupDisplay(Session& s, const GroomFile& file) {
    ToolState& t = toolsOf(s);
    t.groupColors = guideGroupColorWords(file.guideGroupColors);
    t.hidden = parseHiddenGuideGroups(file.hiddenGuideGroups);
}

namespace {

// ---- Undo ---------------------------------------------------------------------------------------

json undoJson(const ToolState& t) {
    const std::vector<StrokeEdit>& done = t.host.edits();
    return {{"canUndo", !done.empty()},
            {"canRedo", !t.redoLabels.empty()},
            {"undoLabel", done.empty() ? json(nullptr) : json(done.back().mode)},
            {"redoLabel", t.redoLabels.empty() ? json(nullptr) : json(t.redoLabels.back())},
            {"undoDepth", done.size()},
            {"redoDepth", t.redoLabels.size()}};
}

bool undo(ToolState& t) {
    const std::vector<StrokeEdit>& done = t.host.edits();
    if (done.empty()) return false;
    const StrokeEdit& last = done.back();
    const std::string label = last.mode, map = last.textureMap;
    if (!map.empty() && !last.texture.empty() && !t.host.paintOf(map)) {
        try {
            t.host.texturePaint(map, last.texture.channels, last.texture.width);  // the file opened for its tiles
        } catch (const std::exception&) {
        }
    }
    if (!t.host.undo()) return false;
    if (!map.empty()) writeTexture(t, map);
    SideEdit side;
    if (!t.doneSide.empty()) {
        side = std::move(t.doneSide.back());
        t.doneSide.pop_back();
    }
    applySide(t, side, false);
    t.redoLabels.push_back(label);
    t.redoMaps.push_back(map);
    t.redoSide.push_back(std::move(side));
    pruneSelection(t);
    pruneTargets(t);
    return true;
}

bool redo(ToolState& t) {
    if (t.redoLabels.empty()) return false;
    const std::string map = t.redoMaps.back();
    if (!t.host.redo()) return false;
    t.redoLabels.pop_back();
    t.redoMaps.pop_back();
    if (!map.empty()) writeTexture(t, map);
    SideEdit side;
    if (!t.redoSide.empty()) {
        side = std::move(t.redoSide.back());
        t.redoSide.pop_back();
    }
    applySide(t, side, true);
    t.doneSide.push_back(std::move(side));
    pruneSelection(t);
    pruneTargets(t);
    return true;
}

// An edit of the stored guides as one undo step (the app's GroomDocument::editGuides): `edit` on
// a copy of the authored hierarchy, evaluated on the scalp again, stored.
// A copy of the authored hierarchy with `edit` done, evaluated on the scalp again (not stored).
GuideHierarchy editedGuides(ToolState& t, const std::function<void(GuideHierarchy&)>& edit) {
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    GroomSession& session = *t.session.groom;
    const HierarchyPtr authored = session.authoredHierarchy();
    GuideHierarchy h = authored ? *authored : GuideHierarchy{};
    try {
        edit(h);
        h.evaluate(session.scalp().get());
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    return h;
}

// One undo step: the edit's stored texts, with what the edit left in `pending` (files, colors).
void commitEdit(ToolState& t, StrokeEdit edit) {
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    t.host.commit(std::move(edit));
    pruneSelection(t);
}

void editGuides(ToolState& t, const std::string& label, const std::function<void(GuideHierarchy&)>& edit) {
    GuideHierarchy h = editedGuides(t, edit);
    StrokeEdit stroke;
    stroke.mode = label;
    stroke.set(StrokeField::Guides, h.toJson());
    commitEdit(t, std::move(stroke));
}

// ---- Texture maps -------------------------------------------------------------------------------

std::string mapsFolder(Session& s) {
    if (!s.mapsDir.empty()) return s.mapsDir;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / blockName("s" + std::to_string(s.id) + "maps", 0);
    return dir.generic_u8string();
}

void writeTexture(ToolState& t, const std::string& name) {
    const std::shared_ptr<TexturePaint> paint = t.host.paintOf(name);
    if (!paint) return;
    auto it = t.host.kept.find(name);
    std::string path = it != t.host.kept.end() ? it->second.path : std::string();
    if (path.empty()) {
        const auto entry = t.session.groom->scalpMaps().maps.find(name);
        if (entry == t.session.groom->scalpMaps().maps.end()) return;
        path = resolveMapFile(t.session, entry->second.texture);
    }
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    paint->save(path);
    if (it != t.host.kept.end() && it->second.paint == paint) it->second.written();
}

// ---- The overlay --------------------------------------------------------------------------------

constexpr float kPi = 3.14159265358979f;

void appendColoredLine(DisplayGeometry& g, const std::vector<Vec3f>& points, const std::array<float, 3>& color) {
    if (points.size() < 2) return;
    const auto first = static_cast<std::uint32_t>(g.positions.size() / 3);
    for (const Vec3f& p : points) {
        g.positions.insert(g.positions.end(), {p.x, p.y, p.z});
        g.colors.insert(g.colors.end(), {color[0], color[1], color[2], 1.0f});
    }
    for (std::uint32_t k = 0; k + 1 < points.size(); ++k) g.indices.insert(g.indices.end(), {first + k, first + k + 1});
}

void clearOverlay(ToolState& t) {
    t.overlay = DisplayGeometry();
    t.overlay.overlay = true;
    t.roots = RootOverlayGeometry();
    t.ringValid = t.ringOnScalp = false;
    ++t.overlayVersion;
}

// The brush overlay at (x, y), as the app's BrushSession draws it: the guides the brush would
// touch (hovering, `withLines`), the ring on the scalp under the cursor (else at the target's
// depth) as wide as the brush there, facing the camera (a dot for Place and Draw), and a Part or
// Flow stroke's path so far.
void rebuildOverlay(ToolState& t, float x, float y, bool withLines) {
    clearOverlay(t);
    const BrushToolSettings& s = t.tool.settings;
    using Tool = BrushToolSettings::Tool;
    if (s.tool == Tool::Roots && !t.selectTool) {
        // The root tool: no ring, its outlines (the core's: the app draws the same).
        prepareHost(t);
        try {
            if (withLines && t.hasView && !t.tool.rootsPressed()) t.tool.roots().hover(t.host, t.view, x, y);  // (Duplicate: the copies follow)
            const MeshPtr scalp = t.session.groom->scalp();
            t.roots = rootOverlayGeometry(t.tool.roots().overlay(t.host), scalp.get());
        } catch (const std::exception&) {
            t.roots = RootOverlayGeometry();
        }
        return;
    }
    if (withLines && !t.selectTool) {
        prepareHost(t);
        std::vector<std::vector<Vec3f>> lines;
        try {
            lines = t.tool.affectedLines(t.host, t.view, x, y);
        } catch (const std::exception&) {
            lines.clear();
        }
        for (const std::vector<Vec3f>& line : lines) appendColoredLine(t.overlay, line, brushHoverColor(s));
    }
    Vec3f origin, direction;
    if (!t.selectTool && t.view.ray(x, y, origin, direction)) {
        Vec3f center;
        bool have = false;
        const MeshPtr scalp = t.session.groom->scalp();
        SurfacePoint hit;
        if (scalp && scalp->triangleCount() > 0 && scalp->intersectRay(origin, direction, &hit)) {
            center = hit.position;
            have = true;
            t.ringOnScalp = true;
        } else {
            have = atTargetDepth(t, x, y, center);
        }
        if (have) {
            const bool creating = s.tool == Tool::Place || s.tool == Tool::Draw || s.tool == Tool::Lengths;  // (Lengths: the keys' rings)
            const float radius = worldRadiusAt(t.view, center, x, y, creating ? 6.0f : s.screenRadius);
            if (radius > 0.0f) {
                Vec3f eye, along;
                bool ortho = false;
                t.view.camera(eye, along, ortho);
                const Vec3f n = ortho ? along : (center - eye).normalized();
                const Vec3f u = (std::abs(n.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f}).cross(n).normalized();
                const Vec3f v = n.cross(u);
                std::vector<Vec3f> ring;
                constexpr int kSegments = 48;
                for (int k = 0; k <= kSegments; ++k) {
                    const float a = 2.0f * kPi * static_cast<float>(k) / kSegments;
                    ring.push_back(center + u * (std::cos(a) * radius) + v * (std::sin(a) * radius));
                }
                appendColoredLine(t.overlay, ring, brushRingColor(s));
                t.ringValid = true;
                t.ringCenter = center;
                t.ringRadius = radius;
                t.ringNormal = n;
            }
        }
    }
    if (!t.selectTool) {
        // Symmetry on: the scalp's line of symmetry in red (Maya's showSymmetry, symmetrySeam).
        const MeshPtr scalp = t.session.groom->scalp();
        const StrokeSymmetry sym = scalp ? StrokeSymmetry::of(s.brush, scalp.get()) : StrokeSymmetry{};
        if (sym.on()) {
            const std::uint64_t key = hashCombine(hashCombine(scalp->contentHash(), static_cast<std::uint64_t>(sym.axis + 1)),
                                                  hashCombine(hashFloat(hash64(0x5EA4), sym.center), sym.topology() ? 1u : 0u));
            if (key != t.seamKey) {
                t.seam = symmetrySeam(*scalp, sym);
                t.seamKey = key;
            }
            for (std::size_t k = 0; k + 1 < t.seam.size(); k += 2) appendColoredLine(t.overlay, {t.seam[k], t.seam[k + 1]}, {1.0f, 0.15f, 0.15f});
        }
    }
    if (t.tool.stroking() && (s.tool == Tool::Part || s.tool == Tool::Flow)) appendColoredLine(t.overlay, t.tool.drawnPath(), brushPathColor(s));
    if (s.tool == Tool::Lengths && !t.selectTool) {
        // The Length Keys tool's keys (the core's lines: the app draws the same).
        prepareHost(t);
        try {
            for (const LengthKeyTool::ColoredLine& line : t.tool.lengths().overlayLines(t.host, t.view, s, x, y)) {
                appendColoredLine(t.overlay, line.points, line.color);
            }
        } catch (const std::exception&) {
        }
    }
}

json positionsToHost(Arena& arena, const std::vector<float>& xyz, const HostSpace& space) {
    const std::size_t n = xyz.size() / 3;
    ArrayRef ref;
    auto* dst = static_cast<float*>(arena.add(n * 3, sizeof(float), "<f4", {n, 3}, &ref));
    for (std::size_t i = 0; i < n; ++i) space.toHost({xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}, dst + 3 * i);
    return refJson(ref);
}

json directionsToHost(Arena& arena, const std::vector<float>& xyz, const HostSpace& space) {
    const std::size_t n = xyz.size() / 3;
    ArrayRef ref;
    auto* dst = static_cast<float*>(arena.add(n * 3, sizeof(float), "<f4", {n, 3}, &ref));
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f d = hostDirection(space, {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]});
        dst[3 * i] = d.x;
        dst[3 * i + 1] = d.y;
        dst[3 * i + 2] = d.z;
    }
    return refJson(ref);
}

std::size_t geometryBytes(const DisplayGeometry& g) {
    return Arena::reserve(g.positions.size() * 4) + Arena::reserve(g.indices.size() * 4) + Arena::reserve(g.colors.size() * 4) +
           Arena::reserve(g.normals.size() * 4) + (g.mapTexture ? Arena::reserve(g.uvs.size() * 4) : 0);
}

// A geometry's arrays in the host's space: positions (n, 3), indices (pairs or triples), colors
// (n, 4) when per vertex, normals (n, 3) for a shaded surface, uvs (n, 2) for the scalp map's
// image (mapTexture).
json geometryArrays(Arena& arena, const DisplayGeometry& g, const HostSpace& space) {
    json a;
    a["positions"] = positionsToHost(arena, g.positions, space);
    const std::size_t per = g.triangles ? 3 : g.points ? 1 : 2;
    a["indices"] = put(arena, g.indices.data(), g.indices.size() / per, per);
    if (!g.colors.empty()) a["colors"] = put(arena, g.colors.data(), g.colors.size() / 4, 4);
    if (!g.normals.empty()) a["normals"] = directionsToHost(arena, g.normals, space);
    if (g.mapTexture && !g.uvs.empty()) a["uvs"] = put(arena, g.uvs.data(), g.uvs.size() / 2, 2);
    return a;
}

// The scalp map's image as the host's texture takes it: 8-bit RGBA, rows bottom-up (the scalp's
// uvs, v = 0 the bottom row), as Maya's viewport makes its texture. `key` names the image and
// changes when it does: the brush's live image (each revision), else the file (its stamp) shown
// in the channel picked. No pixels: no image.
struct MapImage {
    std::string key;
    std::uint64_t texture = 0;  // which texture (not its texels): the geometry's generation
    int width = 0, height = 0;
    const std::uint8_t* pixels = nullptr;
    std::vector<std::uint8_t> own;  // a file's
};

MapImage mapImage(const ScalpMapView& view, bool withPixels) {
    MapImage m;
    if (view.preview && view.preview->width() > 0 && view.preview->height() > 0) {
        const TexturePreview& p = *view.preview;
        m.texture = hashCombine(hash64(0x9E7A), p.layout());
        m.key = "preview:" + std::to_string(p.layout()) + ":" + std::to_string(p.revision());
        m.width = p.width();
        m.height = p.height();
        m.pixels = p.rgba().data();
        return m;
    }
    const std::string file = view.texture + "@" + std::to_string(fileStamp(view.texture)) + "#" +
                             (view.channels == 4 ? view.channel : std::string("gray"));
    m.texture = hashString(hash64(0x9E7B), file);
    m.key = "file:" + file;
    if (!withPixels) return m;
    TexturePtr texture;
    try {
        texture = loadTexture(view.texture);
    } catch (const std::exception&) {
    }
    const Texture::Image* image = texture ? texture->image() : nullptr;
    if (!image || image->width <= 0 || image->height <= 0) {
        m.key.clear();
        return m;
    }
    m.own = scalpMapCachedPixels(*image, view.channels, scalpMapPick(view.channel));
    m.width = image->width;
    m.height = image->height;
    m.pixels = m.own.data();
    return m;
}

json overlayJson(ToolState& t, bool arrays) {
    const HostSpace& space = t.session.space;
    json r;
    json ring;
    ring["valid"] = t.ringValid;
    if (t.ringValid) {
        ring["center"] = hostPoint(space, t.ringCenter);
        ring["radius"] = toHostLength(space, t.ringRadius);
        const Vec3f n = hostDirection(space, t.ringNormal);
        ring["normal"] = {n.x, n.y, n.z};
        ring["onScalp"] = t.ringOnScalp;
        ring["screenRadius"] = t.tool.settings.screenRadius;
        ring["color"] = brushRingColor(t.tool.settings);
    }
    r["ring"] = ring;
    json overlay;
    overlay["version"] = t.overlayVersion;
    overlay["vertexCount"] = t.overlay.positions.size() / 3;
    const RootOverlayGeometry& ro = t.roots;
    const std::pair<const char*, const DisplayGeometry*> rootParts[] = {
        {"fills", &ro.fills}, {"edges", &ro.edges}, {"highlighted", &ro.highlighted},
        {"dots", &ro.dots},   {"dimDots", &ro.dimDots}, {"markers", &ro.markers}};
    if (arrays) {
        std::size_t bytes = geometryBytes(t.overlay);
        for (const auto& [name, g] : rootParts) bytes += geometryBytes(*g);
        t.overlayOut.begin(bytes + 64);
        overlay["arrays"] = geometryArrays(t.overlayOut, t.overlay, space);
        // The root tool's: see-through fills, edges (the highlighted apart, wider), dots a size in
        // pixels across (RootOverlayGeometry); empty ones left out.
        json roots = json::object();
        for (const auto& [name, g] : rootParts) {
            if (!g->positions.empty() && !g->indices.empty()) roots[name] = geometryArrays(t.overlayOut, *g, space);
        }
        if (!roots.empty()) {
            roots["edgePixels"] = ro.edgePixels;
            roots["highlightPixels"] = ro.highlightPixels;
            roots["dotPixels"] = ro.dotPixels;
            roots["dimDotPixels"] = ro.dimDotPixels;
            roots["markerPixels"] = ro.markerPixels;
        }
        overlay["roots"] = roots;
    }
    r["overlay"] = overlay;
    return r;
}

json warningsJson(ToolState& t) {
    json w = t.host.warnings();
    t.host.clearWarnings();
    return w;
}

// ---- Strokes ------------------------------------------------------------------------------------

struct Pixel {
    float x = 0.0f, y = 0.0f, pressure = 1.0f;
};

std::vector<Pixel> pixelsOf(const json& v, float pressure) {
    std::vector<Pixel> out;
    if (!v.is_array()) fail("bad_request", "points is a list of [x, y] or [x, y, pressure]");
    for (const json& p : v) {
        if (!p.is_array() || p.size() < 2 || p.size() > 3) fail("bad_request", "a point is [x, y] or [x, y, pressure]");
        for (const json& c : p) {
            if (!c.is_number() || !std::isfinite(c.get<double>())) fail("bad_request", "a point holds numbers");
        }
        out.push_back({p[0].get<float>(), p[1].get<float>(), p.size() == 3 ? p[2].get<float>() : pressure});
    }
    return out;
}

// The core's tools throw on bad data: the stroke is dropped, the request answers why.
template <class F>
void guarded(ToolState& t, const char* what, F&& fn) {
    try {
        fn();
    } catch (const RequestError&) {
        t.tool.cancel(&t.host);
        throw;
    } catch (const std::exception& e) {
        t.tool.cancel(&t.host);
        fail("stroke", std::string(what) + ": " + e.what());
    }
}

void requireScalp(Session& s) {
    const MeshPtr scalp = s.groom->rawScalp();
    if (!scalp || scalp->triangleCount() == 0) fail("state", "the session has no scalp: set_scalp first");
}

// ---- Guides ---------------------------------------------------------------------------------------

std::string uniqueGroupName(const GuideHierarchy& h, const std::string& base) { return h.freeGroupName(base); }

GroupId groupNamed(const GuideHierarchy& h, const json& args, const char* key) {
    const std::string name = needString(args, key);
    const GroupId id = h.findGroup(name);
    if (id == kNoGroup) fail("not_found", "no group named '" + name + "'");
    return id;
}

json guidesInfo(ToolState& t) {
    GroomSession& session = *t.session.groom;
    const std::shared_ptr<const GuideHierarchy> h = session.storedHierarchy();
    json groups = json::array();
    std::size_t frozen = 0, primaries = 0;
    if (h) {
        const std::map<GroupId, std::array<float, 3>> automatic = autoGuideColors(*h, guideHueSeed(t.colorSeed));
        for (const GuideGroup& g : h->groups()) {
            const GuideGroup* parent = g.parent == kNoGroup ? nullptr : h->group(g.parent);
            // The color it draws in when the host gives it none: its own (a ToGuides node's), else its automatic one.
            const auto shown = automatic.find(g.id);
            const std::array<float, 3> drawn = g.hasColor ? g.color : shown != automatic.end() ? shown->second : std::array<float, 3>{};
            groups.push_back({{"id", g.id},
                              {"name", g.name},
                              {"parent", parent ? json(parent->name) : json(nullptr)},
                              {"level", h->groupDepth(g.id)},
                              {"guides", h->guidesInGroup(g.id).size()},
                              {"hidden", t.hidden.count(g.name) > 0},
                              {"color", t.groupColors.count(g.name) ? json(t.groupColors.at(g.name)) : json(nullptr)},
                              {"autoColor", guideColorHex(drawn)},
                              {"segments", g.segments}});
        }
        for (const HierarchyGuide& g : h->guides()) {
            if (!g.isChild()) ++primaries;
            float v = 0.0f;
            if (h->effectiveAttribute(g, "freeze", &v) && v >= 0.5f) ++frozen;
        }
    }
    json r;
    r["groups"] = groups;
    r["levels"] = h ? h->maxDepth() + 1 : 0;
    r["guides"] = h ? h->guides().size() : 0;
    r["primaries"] = primaries;
    r["frozen"] = frozen;
    r["layers"] = h ? json(h->attributeNames()) : json::array();
    r["hiddenGroups"] = t.hidden;
    r["guideGroupColors"] = groupColorsText(t);
    r["selection"] = selectionJson(t);
    r["hierarchyError"] = session.hierarchyError();
    return r;
}

bool given(const json& args, const char* key) { return args.contains(key) && !args[key].is_null(); }

std::array<float, 3> rgbOf(const json& v, const char* what);  // Display, below

// The guides an edit takes (Maya's groomLabGuides targets): "ids", else "group", else "level",
// else the selection; with none selected, every guide when `allWhenNone` (Resample), else an error.
std::vector<GuideId> targetGuides(ToolState& t, const GuideHierarchy& h, const json& args, bool allWhenNone, const std::string& what) {
    std::vector<GuideId> ids;
    if (given(args, "ids")) {
        ids = idsOf(args["ids"]);
    } else if (given(args, "group")) {
        const std::string group = needString(args, "group");
        if (h.findGroup(group) == kNoGroup) fail("not_found", "no group named '" + group + "'");
        ids = guidesAt(h, -1, group);
    } else if (given(args, "level")) {
        ids = guidesAt(h, settingInt(args["level"], "level"));
    } else {
        pruneSelection(t);
        ids = t.selection;
        if (ids.empty() && allWhenNone) {
            for (const HierarchyGuide& g : h.guides()) ids.push_back(g.id);
        }
    }
    if (ids.empty()) fail("state", what + ": no guides (give ids, a group or a level, or select some)");
    for (const GuideId id : ids) {
        if (!h.guide(id)) fail("not_found", "no guide " + std::to_string(id));
    }
    return ids;
}

// Whether resample's targets are whole groups (a group, a level, or every guide with none
// selected): guides with segments of their own keep them (else the picked guides get their own).
bool resampleGroupWide(ToolState& t, const json& args) {
    if (given(args, "ids")) return false;
    if (given(args, "group") || given(args, "level")) return true;
    pruneSelection(t);
    return t.selection.empty();
}

// A guide group's color as given: "#rrggbb", [r, g, b] (0-1), or null / "" for none (its
// automatic color again). Returns "#rrggbb" lowered, or "".
std::string groupColorOf(const json& v) {
    if (v.is_null()) return {};
    if (v.is_array()) {
        const std::array<float, 3> c = rgbOf(v, "color");
        char text[8];
        std::snprintf(text, sizeof text, "#%02x%02x%02x", static_cast<int>(std::lround(std::clamp(c[0], 0.0f, 1.0f) * 255.0f)),
                      static_cast<int>(std::lround(std::clamp(c[1], 0.0f, 1.0f) * 255.0f)),
                      static_cast<int>(std::lround(std::clamp(c[2], 0.0f, 1.0f) * 255.0f)));
        return text;
    }
    std::string text = settingString(v, "color");
    if (text.empty()) return {};
    const bool hex = text.size() == 7 && text[0] == '#' &&
                     std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    if (!hex) fail("bad_request", "color is \"#rrggbb\", [r, g, b] (0-1) or null");
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

// The group colors changed by the next commit (undo puts them back).
void pendingColors(ToolState& t, std::map<std::string, std::string> after) {
    t.pending.colors = true;
    t.pending.colorsBefore = t.groupColors;
    t.pending.colorsAfter = after;
    t.groupColors = std::move(after);
}

json guidesEdit(ToolState& t, const json& args) {
    const std::string op = needString(args, "op");
    const HostSpace& space = t.session.space;
    auto targets = [&](const GuideHierarchy&) {
        if (args.contains("ids") && !args["ids"].is_null()) return idsOf(args["ids"]);
        pruneSelection(t);
        if (t.selection.empty()) fail("state", op + ": no guides given and none selected");
        return t.selection;
    };
    json r;
    if (op == "add_group") {
        const std::string parent = optString(args, "parent", "");
        std::string made;
        editGuides(t, "add group", [&](GuideHierarchy& h) {
            GroupId parentId = kNoGroup;
            if (!parent.empty()) {
                parentId = h.findGroup(parent);
                if (parentId == kNoGroup) fail("not_found", "no group named '" + parent + "'");
            }
            const std::string name = optString(args, "name", "");
            made = name.empty() ? uniqueGroupName(h, parent.empty() ? "group" : parent + "_children") : guideGroupName(name);
            if (h.findGroup(made) != kNoGroup) fail("bad_request", "a group named '" + made + "' is there already");
            h.addGroup(made, parentId);
        });
        r["group"] = made;
    } else if (op == "remove_group") {
        editGuides(t, "remove group", [&](GuideHierarchy& h) { h.removeGroup(groupNamed(h, args, "group")); });
    } else if (op == "rename_group") {
        const std::string from = needString(args, "group"), to = guideGroupName(needString(args, "name"));  // one word
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.renameGroup(groupNamed(hh, args, "group"), to); });
        const auto color = t.groupColors.find(from);
        if (color != t.groupColors.end()) {  // its color follows it (and comes back with an undo)
            std::map<std::string, std::string> colors = t.groupColors;
            colors.erase(from);
            colors[to] = color->second;
            pendingColors(t, std::move(colors));
        }
        StrokeEdit stroke;
        stroke.mode = "rename group";
        stroke.set(StrokeField::Guides, h.toJson());
        // The Guides nodes reading or leaving out the group follow it (every host's: graph_inserts.h),
        // in the same undo step.
        bool retargeted = false;
        const std::string graph = renameGroupReaders(t.session.groom->data().graphJson, from, to, &retargeted);
        if (retargeted) stroke.set(StrokeField::Graph, graph);
        commitEdit(t, std::move(stroke));
        if (t.hidden.erase(from)) t.hidden.insert(to);
        r["graphChanged"] = retargeted;
    } else if (op == "generate_children") {
        std::string made;
        std::size_t count = 0;
        editGuides(t, "generate children", [&](GuideHierarchy& h) {
            GroupId id = kNoGroup;
            if (args.contains("group") && !args["group"].is_null()) {
                id = groupNamed(h, args, "group");
            } else if (!h.groups().empty()) {
                id = h.groups().back().id;  // the last group, as Maya's Guides tab
            }
            if (id == kNoGroup) fail("state", "the groom has no guides yet");
            ChildGenerationParams p;
            p.count = static_cast<std::size_t>(std::clamp(optNumber(args, "count", 5.0), 1.0, static_cast<double>(kMaxChildrenPerGuide)));
            p.rootSpread = static_cast<float>(optNumber(args, "rootSpread", p.rootSpread));
            p.tipSpread = static_cast<float>(optNumber(args, "tipSpread", p.tipSpread));
            p.twist = static_cast<float>(optNumber(args, "twist", p.twist));
            p.jitter = static_cast<float>(optNumber(args, "jitter", p.jitter));
            p.childTubeScale = static_cast<float>(optNumber(args, "childTubeScale", p.childTubeScale));
            p.seed = static_cast<std::uint64_t>(std::max(0.0, optNumber(args, "seed", 0.0)));
            std::vector<GuideId> parents;
            const bool selected = optBool(args, "selected", false);
            if (selected) {
                pruneSelection(t);
                for (const GuideId g : t.selection) {
                    const HierarchyGuide* guide = h.guide(g);
                    if (guide && guide->group == id) parents.push_back(g);
                }
                if (parents.empty()) fail("state", "none of the selected guides is in that group");
            } else if (h.guidesInGroup(id).empty()) {
                fail("state", "the group has no guides to be parents");
            }
            made = optString(args, "childGroup", "");
            if (made.empty()) made = h.childGroupFor(id);  // every host's (guide_hierarchy.h)
            const std::size_t before = h.guides().size();
            h.generateChildren(id, made, p, selected ? &parents : nullptr);
            count = h.guides().size() >= before ? h.guides().size() - before : 0;
        });
        r["group"] = made;
        r["added"] = count;
    } else if (op == "delete_guides") {
        std::vector<GuideId> ids;
        editGuides(t, "delete guides", [&](GuideHierarchy& h) {
            ids = targets(h);
            h.removeGuides(ids, true);  // (a child group left empty goes too: every host's delete)
        });
        r["removed"] = ids.size();
    } else if (op == "lock" || op == "unlock") {
        editGuides(t, op + " guides", [&](GuideHierarchy& h) {
            for (const GuideId id : targets(h)) {
                const HierarchyGuide* g = h.guide(id);
                if (!g || !g->isChild()) continue;
                if (op == "lock") h.lock(id);
                else h.unlock(id);
            }
        });
    } else if (op == "set_attribute") {
        const std::string name = needString(args, "name");
        const float value = static_cast<float>(optNumber(args, "value", 1.0));
        if (!validMapName(name)) fail("bad_request", mapNameError(name));
        editGuides(t, "set " + name, [&](GuideHierarchy& h) {
            for (const GuideId id : targets(h)) {
                if (h.guide(id)) h.setAttribute(id, name, value);
            }
        });
    } else if (op == "freeze" || op == "unfreeze") {
        editGuides(t, op + " guides", [&](GuideHierarchy& h) {
            for (const GuideId id : targets(h)) {
                if (h.guide(id)) h.setAttribute(id, kFreezeTool, op == "freeze" ? 1.0f : 0.0f);
            }
        });
    } else if (op == "unfreeze_all") {
        editGuides(t, "unfreeze all", [&](GuideHierarchy& h) { h.removeAttribute(kFreezeTool); });
    } else if (op == "invert_frozen") {
        editGuides(t, "invert frozen", [&](GuideHierarchy& h) { invertFrozen(h, kFreezeTool); });
    } else if (op == "remove_layer") {
        const std::string name = needString(args, "name");
        editGuides(t, "remove layer " + name, [&](GuideHierarchy& h) { h.removeAttribute(name); });
    } else if (op == "mirror_all" || op == "mirror_group") {
        int axis = 0;
        float center = 0.0f;
        symmetryToGroom(space, optString(args, "axis", "x"), optNumber(args, "center", 0.0), axis, center);
        if (axis < 0) fail("bad_request", "axis is x, y or z");
        const bool fromPositive = optBool(args, "fromPositive", true);
        std::size_t made = 0;
        editGuides(t, op == "mirror_all" ? "mirror all" : "mirror group", [&](GuideHierarchy& h) {
            if (op == "mirror_all") {
                made = h.mirrorAll(axis, center, fromPositive).size();
            } else {
                const GroupId id = groupNamed(h, args, "group");
                if (h.groupDepth(id) != 0) fail("bad_request", "mirror a root group: its children follow");
                made = h.mirrorGroup(id, axis, center, fromPositive).size();
            }
        });
        r["added"] = made;
    } else if (op == "mirror_shapes") {
        // Maya's groomLabGuides -mirrorShapes (issue 74): "selected" (the selected or `ids` guides'
        // mirror guides take their mirror images), "positive" or "negative" (that whole side over
        // the other; `group`: one root group's), across `symmetry` (x, y, z at `center`, or
        // topology), as the brushes take it. One undo step.
        const std::string from = optString(args, "from", "selected");
        if (from != "selected" && from != "positive" && from != "negative") fail("bad_request", "from is selected, positive or negative");
        const std::string symmetry = optString(args, "symmetry", "x");
        BrushSettings b;
        if (symmetry == "topology") {
            b.mirrorTopology = true;
        } else {
            symmetryToGroom(space, symmetry, optNumber(args, "center", 0.0), b.mirrorAxis, b.mirrorCenter);
            if (b.mirrorAxis < 0) fail("bad_request", "symmetry is x, y, z or topology");
        }
        const MeshPtr scalp = t.session.groom->scalp();
        if (b.mirrorTopology && !scalp) fail("state", "Topology symmetry needs the groom's scalp");
        GuideHierarchy::MirrorShapesResult done;
        editGuides(t, "mirror to other side", [&](GuideHierarchy& h) {
            const std::vector<GuideId> sources = from == "selected" ? targets(h) : std::vector<GuideId>{};
            const GroupId only = from != "selected" && given(args, "group") ? groupNamed(h, args, "group") : kNoGroup;
            done = mirrorGuideShapes(h, StrokeSymmetry::of(b, scalp.get()), sources,
                                     from == "positive" ? 1 : from == "negative" ? -1 : 0, only);
            if (done.reshaped.empty() && done.created.empty() && done.removed.empty()) {
                fail("state", from == "selected" ? "the selected guides are on the symmetry's seam: nothing to mirror"
                                                 : "no guides on that side");
            }
        });
        r["reshaped"] = done.reshaped.size();
        r["added"] = done.created.size();
        r["removed"] = done.removed.size();
    } else if (op == "promote_group") {
        editGuides(t, "promote group", [&](GuideHierarchy& h) { h.promoteGroup(groupNamed(h, args, "group")); });
    } else if (op == "demote_group") {
        editGuides(t, "demote group", [&](GuideHierarchy& h) { h.demoteGroup(groupNamed(h, args, "group"), groupNamed(h, args, "parent")); });
    } else if (op == "reparent") {
        editGuides(t, "reparent guides", [&](GuideHierarchy& h) {
            const json& parent = need(args, "parent");
            if (!parent.is_number_unsigned()) fail("bad_request", "parent is a guide ID");
            h.reparent(targets(h), parent.get<GuideId>());
        });
    } else if (op == "resample") {
        // Maya's groomLabGuides -resample: each guide `segments` segments evenly along the smooth
        // curve through its points (as the viewport draws it), its shape kept; frozen guides kept.
        // A whole group, a level or every guide: their groups' new guides get these segments too.
        const int segments = settingInt(need(args, "segments"), "segments");
        if (segments < 1 || segments > kMaxGuideSegments) fail("bad_request", "segments: 1 to 256");
        std::size_t count = 0;
        t.session.groom->setPreviewHierarchy(nullptr);  // a live preview (guides_preview) ends with its commit
        editGuides(t, "resample guides", [&](GuideHierarchy& h) {
            const std::vector<GuideId> ids = targetGuides(t, h, args, true, "resample");
            const bool groupWide = resampleGroupWide(t, args);
            const GroupId only = given(args, "group") ? h.findGroup(needString(args, "group")) : kNoGroup;
            const int level = given(args, "level") ? settingInt(args["level"], "level") : -1;
            count = h.resampleGuides(ids, segments, groupWide, only, level);  // every host's (guide_hierarchy.h)
        });
        r["resampled"] = count;
    } else if (op == "mirror_selection") {
        // Maya's mirror_guide_selection: the mirror image of each selected guide (the nearest of its
        // group across the plane) selected; `add` keeps the selection. Not an undo step (a selection).
        int axis = 0;
        float center = 0.0f;
        symmetryToGroom(space, optString(args, "axis", "x"), optNumber(args, "center", 0.0), axis, center);
        if (axis < 0) fail("bad_request", "axis is x, y or z");
        const HierarchyPtr h = t.session.groom->authoredHierarchy();
        pruneSelection(t);
        const std::vector<GuideId> from = given(args, "ids") ? idsOf(args["ids"]) : t.selection;
        std::vector<GuideId> picked;
        if (h && !from.empty()) picked = mirrorSelection(*h, from, axis, center, 1e30f);
        setSelection(t, combineSelection(t.selection, picked, optBool(args, "add", false) ? SelectMode::Add : SelectMode::Replace));
        r["picked"] = picked;
        r["selection"] = selectionJson(t);
    } else if (op == "set_group_color") {
        // Maya's set_guide_group_color: the group drawn in its own color (guideGroupColors), or in
        // its automatic color again (color null). One undo step; kept with the tools' state.
        const std::string group = needString(args, "group");
        const std::shared_ptr<const GuideHierarchy> h = t.session.groom->storedHierarchy();
        if (!h || h->findGroup(group) == kNoGroup) fail("not_found", "no group named '" + group + "'");
        const std::string color = groupColorOf(args.contains("color") ? args["color"] : json(nullptr));
        std::map<std::string, std::string> colors = t.groupColors;
        if (color.empty()) {
            colors.erase(group);
        } else {
            colors[group] = color;
        }
        if (colors != t.groupColors) {
            pendingColors(t, std::move(colors));
            StrokeEdit stroke;
            stroke.mode = "group color";
            commitEdit(t, std::move(stroke));
        }
        r["color"] = color.empty() ? json(nullptr) : json(color);
    } else if (op == "reset_group_segments") {
        // Maya's reset_group_segments: every guide of the group back to its segments (frozen kept).
        const std::string group = needString(args, "group");
        int segments = 0;
        editGuides(t, "reset segments", [&](GuideHierarchy& h) {
            const GroupId gid = h.findGroup(group);
            if (gid == kNoGroup) fail("not_found", "no group named '" + group + "'");
            segments = h.resetGroupSegments(gid);
            if (segments < 1) fail("state", "the group has no guides");
        });
        r["segments"] = segments;
    } else if (op == "set_group_segments") {
        // Maya's set_group_segments: the segments the group's new guides get (0: the tool's own).
        const std::string group = needString(args, "group");
        const int segments = settingInt(need(args, "segments"), "segments");
        if (segments < 0 || segments > 256) fail("bad_request", "segments: 0 to 256");
        editGuides(t, "group segments", [&](GuideHierarchy& h) {
            const GroupId gid = h.findGroup(group);
            if (gid == kNoGroup) fail("not_found", "no group named '" + group + "'");
            h.setGroupSegments(gid, segments);
        });
    } else {
        fail("bad_request",
             "op is add_group, remove_group, rename_group, generate_children, delete_guides, lock, unlock, set_attribute, "
             "freeze, unfreeze, unfreeze_all, invert_frozen, remove_layer, mirror_all, mirror_group, promote_group, demote_group, reparent, "
             "resample, mirror_selection, set_group_color, set_group_segments or reset_group_segments");
    }
    r["undo"] = undoJson(t);
    r["guides"] = guidesInfo(t);
    return r;
}

// ---- Guide previews (Groom Tools' live sliders) ----------------------------------------------------

// A preview of the stored guides with `edit` done, shown and evaluated in their place (no change,
// no undo step) until guides_preview_end or the edit's commit: always from the stored guides, so
// previews never compound (Maya's -previewResample, -previewTubeShape).
void previewGuides(ToolState& t, const std::function<void(GuideHierarchy&)>& edit) {
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    GroomSession& session = *t.session.groom;
    const HierarchyPtr authored = session.authoredHierarchy();
    if (!authored) fail("state", "the groom has no guides");
    auto preview = std::make_shared<GuideHierarchy>(*authored);
    try {
        edit(*preview);
        preview->evaluate(session.scalp().get());
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    session.setPreviewHierarchy(preview);
}

json guidesPreview(ToolState& t, const json& args) {
    const int segments = settingInt(need(args, "segments"), "segments");
    if (segments < 1 || segments > 256) fail("bad_request", "segments: 1 to 256");
    std::size_t count = 0;
    previewGuides(t, [&](GuideHierarchy& h) {
        count = h.resample(targetGuides(t, h, args, true, "preview"), segments, resampleGroupWide(t, args));
    });
    return {{"previewed", count}};
}

// Each target guide's segment count (points - 1): the Segments slider's value. Targets as
// resample's (with none selected, every guide).
json guideSegments(ToolState& t, const json& args) {
    const std::shared_ptr<const GuideHierarchy> h = t.session.groom->storedHierarchy();
    json ids = json::array(), segments = json::array();
    if (h && !h->guides().empty()) {
        for (const GuideId id : targetGuides(t, *h, args, true, "guide_segments")) {
            ids.push_back(id);
            segments.push_back(std::max(static_cast<int>(h->guide(id)->points.size()) - 1, 0));
        }
    }
    return {{"ids", ids}, {"segments", segments}};
}

// ---- Tubes (Maya's groomLabGuides -setTube, -tube, -tubeTargets, -tubeShapes, -editTubeShape) ----

// A tube edit (editedTube's JSON, widths in host units) on each target guide: one undo step.
json tubeEdit(ToolState& t, const json& args, const std::string& label, const json& edit) {
    const std::string text = tubeEditToGroom(t.session.space, edit);
    std::size_t changed = 0;
    editGuides(t, label, [&](GuideHierarchy& h) {
        for (const GuideId id : targetGuides(t, h, args, false, label)) {
            const HierarchyGuide* g = h.guide(id);
            h.setTube(id, editedTube(g->tube, text, h.scale(), g->points.size(), &h.tubeShapes()));
            ++changed;
        }
    });
    return {{"changed", changed}, {"undo", undoJson(t)}};
}

json tubeSet(ToolState& t, const json& args) {
    json edit = json::object();
    for (const char* key : {"shape", "width", "aspect", "twist", "samples", "midShape", "tipShape", "rootHold", "midHold", "tipHold",
                            "midAt"}) {
        if (given(args, key)) edit[key] = args[key];
    }
    if (edit.empty()) fail("bad_request", "give a shape, width, aspect, twist, samples, a mid or tip shape or a hold");
    return tubeEdit(t, args, "set tube", edit);
}

json tubeScale(ToolState& t, const json& args) {
    const float factor = settingNumber(need(args, "factor"), "factor");
    if (!(factor >= 0.0f)) fail("bad_request", "factor: 0 or more");
    return tubeEdit(t, args, "scale tube", json{{"scaleWidth", factor}});
}

json tubeSmooth(ToolState& t, const json& args) {
    // The Shape brush's channel names (tubeSmoothEdit: every host's).
    std::string edit;
    try {
        edit = tubeSmoothEdit(optString(args, "channel", "all"), static_cast<int>(optNumber(args, "passes", 3.0)));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    return tubeEdit(t, args, "smooth tube", json::parse(edit));
}

json tubeGet(ToolState& t, const json& args) {
    const HierarchyPtr h = t.session.groom->authoredHierarchy();
    const json& id = need(args, "guide");  // ("id" is the request's own)
    if (!id.is_number_unsigned()) fail("bad_request", "guide is a guide ID");
    const HierarchyGuide* g = h ? h->guide(id.get<GuideId>()) : nullptr;
    if (!g) fail("not_found", "no guide " + std::to_string(id.get<GuideId>()));
    return {{"tube", tubeToHost(t.session.space, tubeToJson(g->tube, h->scale()))}};
}

// The Tube card's Width (host units), Flatness and Twist for a tube (tubeCardValues, every host's).
json tubeCardJson(const HostSpace& space, const TubeProfile& tube, float scale) {
    const TubeCardValues v = tubeCardValues(tube.width, tube.aspect, tube.twist);
    return {{"width", toHostLength(space, v.width * scale)}, {"flatness", v.flatness}, {"twist", v.twist}};
}

// The guides the Tubes section edits and their profiles (every host's: tubeTargetsOf).
json tubeTargets(ToolState& t) {
    const HierarchyPtr h = t.session.groom->authoredHierarchy();
    if (!h) return {{"source", "none"}, {"guides", json::array()}, {"shapes", json::array()}, {"midShapes", json::array()},
                    {"tipShapes", json::array()}, {"inUse", json::array()}, {"tube", nullptr}};
    pruneSelection(t);
    const std::vector<GuideId> selection(t.selection.begin(), t.selection.end());
    const TubeTargets targets = tubeTargetsOf(*h, selection, kFreezeTool);
    json inUse = json::array();
    for (const auto& [name, count] : targets.inUse) inUse.push_back({{"name", name}, {"guides", count}});
    return {{"source", targets.source},
            {"guides", targets.guides},
            {"shapes", targets.shapes},
            {"midShapes", targets.midShapes},
            {"tipShapes", targets.tipShapes},
            {"inUse", inUse},
            {"tube", targets.guides.empty() ? json(nullptr)
                                            : tubeToHost(t.session.space, tubeToJson(h->guide(targets.guides.front())->tube, h->scale()))},
            {"card", targets.guides.empty() ? json(nullptr) : tubeCardJson(t.session.space, h->guide(targets.guides.front())->tube, h->scale())}};
}

json sectionPointsJson(const std::vector<SectionPoint>& points) {
    json out = json::array();
    for (const SectionPoint& p : points) out.push_back({p.u, p.v});
    return out;
}

// The shapes guides can take: the built-ins, then the groom's library (saved with its guides).
json tubeShapes(ToolState& t) {
    json out = json::array();
    for (const std::string& name : tubeShapeNames()) {
        TubeShape shape = TubeShape::Round;
        parseTubeShape(name, &shape);
        bool smooth = true;
        const std::vector<SectionPoint> points = builtinTubeShapePoints(shape, &smooth);
        out.push_back({{"name", name}, {"builtin", true}, {"points", sectionPointsJson(points)}, {"smooth", smooth}, {"users", nullptr}});
    }
    if (const std::shared_ptr<const GuideHierarchy> h = t.session.groom->storedHierarchy()) {
        for (const auto& [name, shape] : h->tubeShapes()) {
            out.push_back({{"name", name},
                           {"builtin", false},
                           {"points", sectionPointsJson(shape->points)},
                           {"smooth", shape->smooth},
                           {"users", h->tubeShapeUsers(name)},
                           {"outline", sectionPointsJson(shape->outline.points)}});
        }
    }
    return {{"shapes", out}};
}

std::vector<SectionPoint> shapePointsOf(const json& v) {
    if (!v.is_array()) fail("bad_request", "points is a list of [u, v] pairs");
    std::vector<SectionPoint> out;
    for (const json& p : v) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) fail("bad_request", "points is a list of [u, v] pairs");
        out.push_back({p[0].get<float>(), p[1].get<float>()});
    }
    if (out.size() < 3) fail("bad_request", "a tube shape needs at least 3 points");
    return out;
}

// The library edited (Maya's -editTubeShape): add, set (points and/or smooth), rename, remove.
// Every guide taking the shape follows (remove: round). One undo step; a live preview ends.
json tubeShapeEdit(ToolState& t, const std::string& op, const json& args) {
    const std::string name = needString(args, "name");
    std::size_t users = 0;
    t.session.groom->setPreviewHierarchy(nullptr);
    editGuides(t, op == "add" ? "add tube shape" : op == "set" ? "edit tube shape" : op == "rename" ? "rename tube shape" : "remove tube shape",
               [&](GuideHierarchy& h) {
                   const CustomTubeShapePtr old = h.tubeShape(name);
                   if (op == "add" || op == "set") {
                       if (op == "add" && old) fail("bad_request", "a tube shape named '" + name + "' exists");
                       if (op == "set" && !old) fail("not_found", "no tube shape named '" + name + "'");
                       const bool smooth = given(args, "smooth") ? settingBool(args["smooth"], "smooth") : (old ? old->smooth : true);
                       const std::vector<SectionPoint> points =
                           given(args, "points") ? shapePointsOf(args["points"]) : (old ? old->points : std::vector<SectionPoint>{});
                       if (points.empty()) fail("bad_request", "a new tube shape needs its points");
                       h.setTubeShape(name, points, smooth);
                       users = h.tubeShapeUsers(name);
                   } else if (op == "rename") {
                       if (!old) fail("not_found", "no tube shape named '" + name + "'");
                       const std::string to = needString(args, "to");
                       h.renameTubeShape(name, to);
                       users = h.tubeShapeUsers(to);
                   } else {
                       if (!old) fail("not_found", "no tube shape named '" + name + "'");
                       users = h.removeTubeShape(name);
                   }
               });
    return {{"users", users}, {"undo", undoJson(t)}, {"shapes", tubeShapes(t)["shapes"]}};
}

json tubeShapePreview(ToolState& t, const json& args) {
    const std::string name = needString(args, "name");
    const std::vector<SectionPoint> points = shapePointsOf(need(args, "points"));
    const bool smooth = optBool(args, "smooth", true);
    previewGuides(t, [&](GuideHierarchy& h) { h.setTubeShape(name, points, smooth); });
    return {{"previewing", true}};
}

// ---- Parts and flow (Maya's groomLabParts, flow.clear_flow) ------------------------------------------

PartSet storedParts(Session& s) {
    try {
        return PartSet::fromJson(s.groom->data().partsJson);
    } catch (const std::exception& e) {
        fail("state", std::string("the groom's parts do not read: ") + e.what());
    }
}

void commitParts(ToolState& t, const std::string& label, const PartSet& set) {
    StrokeEdit edit;
    edit.mode = label;
    edit.set(StrokeField::Parts, set.empty() ? std::string() : set.toJson());
    commitEdit(t, std::move(edit));
}

// The parts by name, and the flow's count (the Flow card's "34 arrows, 2 sinks"; unreadable: none).
json partsList(ToolState& t) {
    json parts = json::array();
    for (const Part& p : storedParts(t.session).parts) parts.push_back({{"name", p.name}, {"points", p.points.size()}});
    std::size_t arrows = 0, sinks = 0;
    const std::string& flow = t.session.groom->data().flowJson;
    if (!flow.empty()) {
        try {
            const FlowSet set = FlowSet::fromJson(flow);
            arrows = set.pins.size();
            sinks = set.sinks.size();
        } catch (const std::exception&) {
        }
    }
    return {{"parts", parts}, {"flow", {{"arrows", arrows}, {"sinks", sinks}}}};
}

// A part through points on or near the scalp (a curve drawn in the host): bound to the cage (as
// strokes store them), resampled to 64 points, named `name` or name1, name2... when taken.
json partAdd(ToolState& t, const json& args, BlockReader& blocks) {
    Session& s = t.session;
    const MeshPtr cage = s.groom->cageScalp();
    if (!cage || cage->triangleCount() == 0) fail("state", "the session has no scalp: set_scalp first");
    const std::vector<Vec3f> points = pointsToGroom(readArray<float>(need(args, "points"), "points", 3, blocks), s.space);
    if (points.size() < 2) fail("bad_request", "a part needs at least 2 points");
    std::string base = optString(args, "name", "part");
    if (base.empty()) base = "part";
    PartSet set = storedParts(s);
    std::string name;
    try {
        name = set.add(base, points, *cage, 64);
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    commitParts(t, "add part", set);
    return {{"name", name}, {"undo", undoJson(t)}};
}

json partRemove(ToolState& t, const json& args) {
    const std::string name = needString(args, "name");
    PartSet set = storedParts(t.session);
    if (!set.remove(name)) fail("not_found", "no part named '" + name + "'");
    commitParts(t, "remove part", set);
    return {{"removed", name}, {"undo", undoJson(t)}};
}

json partsClear(ToolState& t) {
    const bool any = !storedParts(t.session).empty();
    if (any) commitParts(t, "clear parts", PartSet{});
    return {{"cleared", any}, {"undo", undoJson(t)}};
}

json flowClear(ToolState& t) {
    const std::string& stored = t.session.groom->data().flowJson;
    bool any = !stored.empty();
    try {
        any = any && !FlowSet::fromJson(stored).empty();
    } catch (const std::exception&) {
        // unreadable: cleared all the same
    }
    if (any) {
        StrokeEdit edit;
        edit.mode = "clear flow";
        edit.set(StrokeField::Flow, std::string());
        commitEdit(t, std::move(edit));
    }
    return {{"cleared", any}, {"undo", undoJson(t)}};
}

// ---- Maps ---------------------------------------------------------------------------------------

json mapsList(ToolState& t, json* densityMap = nullptr) {
    Session& s = t.session;
    json maps = json::array();
    if (densityMap) {  // the Tube tool's density map among them (subdivisionDensityMapIn, every host's; "" none)
        std::vector<std::string> names;
        for (const auto& [name, entry] : s.groom->scalpMaps().maps) names.push_back(name);
        *densityMap = subdivisionDensityMapIn(names);
    }
    for (const auto& [name, entry] : s.groom->scalpMaps().maps) {
        json m = {{"name", name}, {"kind", entry.kind}, {"channels", entry.channels}};
        if (entry.kind == "texture") {
            m["texture"] = entry.texture;
            const std::string file = resolveMapFile(s, entry.texture);
            std::error_code ec;
            m["file"] = file;
            m["missing"] = file.empty() || fileStamp(file) == 0;  // the Paint tab's missing-file marker (a tile set: no tile)
        }
        maps.push_back(m);
    }
    if (const auto h = s.groom->storedHierarchy()) {
        for (const std::string& name : h->attributeNames()) {
            maps.push_back({{"name", name}, {"kind", "guides"}, {"channels", 1}, {"freezeMask", name == kFreezeTool}});
        }
    }
    return {{"maps", maps}, {"folder", mapsFolder(s)}};
}

void commitMaps(ToolState& t, const std::string& label, const ScalpMaps& maps) {
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    t.session.mapsOwned = true;
    StrokeEdit edit;
    edit.mode = label;
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    t.host.commit(std::move(edit));
}

// A new scalp map's entry (vertices: `value` or `fill` at every vertex; texture: its image written
// in the maps folder, a free name), not committed. With `faces` (scalp polygon indices) only those
// faces get the value or fill, the rest 0 (a color map: black, alpha 1).
ScalpMapEntry newScalpMapEntry(Session& s, const ScalpMesh& scalp, const std::string& name, const std::string& kind, bool color,
                               float value, const PaintColor& fill, int resolution, const std::vector<std::int32_t>& faces = {});
// "vertices", "texture", "guides", or "" for no map of that name.
std::string mapKindOf(Session& s, const std::string& name);

// The scalp polygons `args` "faces" names (Maya's -faces: a make or fill limited to them); empty
// when it names none. Polygon indices of the mesh set_scalp sent (Blender's mesh.polygons).
std::vector<std::int32_t> facesArg(const json& args) {
    std::vector<std::int32_t> faces;
    if (!args.contains("faces") || args["faces"].is_null()) return faces;
    const json& f = args["faces"];
    if (!f.is_array()) fail("bad_request", "faces is a list of the scalp's polygon indices");
    for (const json& v : f) {
        if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > std::numeric_limits<std::int32_t>::max()) {
            fail("bad_request", "faces is a list of the scalp's polygon indices");
        }
        faces.push_back(v.get<std::int32_t>());
    }
    if (faces.empty()) fail("bad_request", "faces names no face");
    return faces;
}

json mapCreate(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needString(args, "name");
    const std::string kind = optString(args, "kind", "vertices");
    const bool color = optBool(args, "color", false);
    const float value = static_cast<float>(optNumber(args, "value", 0.0));
    PaintColor fill{0.0f, 0.0f, 0.0f, 1.0f};
    if (args.contains("fill") && !args["fill"].is_null()) {
        const json& f = args["fill"];
        if (!f.is_array() || (f.size() != 3 && f.size() != 4)) fail("bad_request", "fill is [r, g, b] or [r, g, b, a]");
        for (std::size_t i = 0; i < f.size(); ++i) fill[i] = std::clamp(settingNumber(f[i], "fill"), 0.0f, 1.0f);
    }
    if (!validMapName(name)) fail("bad_request", mapNameError(name));
    const json existing = mapsList(t);
    for (const json& m : existing["maps"]) {
        if (m["name"] == name) fail("bad_request", "the groom already has a map named '" + name + "'");
    }
    const MeshPtr scalp = s.groom->cageScalp();
    if (!scalp) fail("state", "no scalp to paint on: set_scalp first");
    const std::vector<std::int32_t> faces = facesArg(args);
    if (kind == "guides") {
        if (color) fail("bad_request", "a guide layer is gray: make a color map on the scalp's vertices or as a texture");
        // On faces: the value on the guides rooted on them, 0 on the rest (every host's: addGuideLayer).
        editGuides(t, "create map " + name, [&](GuideHierarchy& h) {
            if (!addGuideLayer(h, name, value, scalp.get(), faces)) fail("state", "no primary guides to carry the layer: place some first");
        });
        return {{"name", name}, {"kind", kind}, {"undo", undoJson(t)}};
    }
    ScalpMaps maps = s.groom->scalpMaps();
    maps.maps[name] = newScalpMapEntry(s, *scalp, name, kind, color, value, fill, static_cast<int>(optNumber(args, "resolution", 1024.0)), faces);
    commitMaps(t, "create map " + name, maps);
    json r = {{"name", name}, {"kind", kind}, {"undo", undoJson(t)}};
    if (kind == "texture") r["file"] = maps.maps[name].texture;
    return r;
}

ScalpMapEntry newScalpMapEntry(Session& s, const ScalpMesh& scalpMesh, const std::string& name, const std::string& kind, bool color,
                               float value, const PaintColor& fill, int resolution, const std::vector<std::int32_t>& faces) {
    const ScalpMesh* scalp = &scalpMesh;
    ScalpMapEntry entry;
    entry.channels = color ? kColorMap : kGrayMap;
    // (Every host's New Map: scalp_paint.h newVertexMapValues, newTextureMapPaint.)
    const PaintColor filled = color ? fill : PaintColor{value, value, value, 1.0f};
    if (kind == "vertices") {
        entry.kind = "vertices";
        entry.values = newVertexMapValues(*scalp, entry.channels, filled, faces);
    } else if (kind == "texture") {
        if (!scalp->hasUVs()) fail("state", "the scalp has no UVs to paint a texture map on");
        const fs::path folder = fs::u8path(mapsFolder(s));
        const fs::path file = fs::u8path(freeFilePath((folder / (name + ".png")).generic_u8string()));  // (a file or a tile set: taken)
        try {
            fs::create_directories(file.parent_path());
            newTextureMapPaint(*scalp, entry.channels, filled, resolution, faces).save(file.generic_u8string());
        } catch (const std::exception& e) {
            fail("io", std::string("cannot write the map's image: ") + e.what());
        }
        entry.kind = "texture";
        entry.texture = file.generic_u8string();
    } else {
        fail("bad_request", "a map's kind is vertices, texture or guides, not '" + kind + "'");
    }
    return entry;
}

json mapForParam(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string node = needString(args, "node"), param = needString(args, "param");
    const std::string kind = optString(args, "kind", "vertices");
    if (kind != "vertices" && kind != "texture" && kind != "guides")
        fail("bad_request", "a setting's map is on the guides, the scalp's vertices or a texture, not '" + kind + "'");
    const MeshPtr scalp = s.groom->cageScalp();
    if (!scalp) fail("state", "no scalp to paint on: set_scalp first");
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    prepareHost(t);
    setTool(t, "paint");  // out of Freeze or Select, as picking Paint does
    const std::string graphBefore = s.groom->data().graphJson;
    ParamMapRef found;
    try {
        found = paramMapOf(graphBefore, s.groom->scalpMaps(), s.groom->storedHierarchy().get(), node, param);
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    std::string map = found.map, mapKind = found.kind;
    // A map made here with `faces` is white on them, black elsewhere (one the param reads already stays).
    const std::vector<std::int32_t> faces = facesArg(args);
    // The param's own map (paramMapName) there already, its link gone: linked again, whatever its
    // kind, its painting back (every host does).
    bool reused = false;
    if (map.empty()) {
        const std::string own = paramMapName(node, param);
        const std::string ownKind = mapKindOf(s, own);
        if (!ownKind.empty()) {
            std::string graph;
            try {
                graph = wireParamMap(graphBefore, s.groom->scalpMaps(), node, param, own, ownKind);
            } catch (const std::exception& e) {
                fail("bad_request", e.what());
            }
            StrokeEdit edit;
            edit.mode = "map " + node + "." + param;
            edit.set(StrokeField::Graph, std::move(graph));
            t.host.commit(std::move(edit));
            map = own;
            mapKind = ownKind;
            reused = true;
        }
    }
    if (map.empty() && kind == "vertices" && faces.empty()) {
        try {
            const ParamMapBinding b = paintParamMap(t.host, t.tool.settings, node, param);  // made and linked: one step
            map = b.map;
            mapKind = b.kind;
        } catch (const std::exception& e) {
            fail("bad_request", e.what());
        }
    } else if (map.empty() && kind == "guides") {
        // A guide layer (Maya's paint_param kind "guides"): every primary guide carries it at 1 (white:
        // the look stays), children inherit it, strands blend it; the layer and its link one step.
        map = paramMapName(node, param);  // (free: one of that name was linked above)
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) {
            // (on faces: 1 on the guides rooted on them, 0 on the rest: addGuideLayer, every host's)
            if (!addGuideLayer(hh, map, 1.0f, scalp.get(), faces)) {
                fail("state", "the groom has no guides to paint on: add guides, or paint on the scalp's vertices");
            }
        });
        std::string graph;
        try {
            graph = wireParamMap(graphBefore, s.groom->scalpMaps(), node, param, map, "guides");
        } catch (const std::exception& e) {
            fail("bad_request", e.what());
        }
        StrokeEdit edit;
        edit.mode = "map " + node + "." + param;
        edit.set(StrokeField::Guides, h.toJson());
        edit.set(StrokeField::Graph, std::move(graph));
        t.host.commit(std::move(edit));
        mapKind = "guides";
    } else if (map.empty()) {
        // A texture (or a vertex map on faces): white, linked in the same step; a texture's image
        // beside the groom, linked by its file.
        map = paramMapName(node, param);  // (free: one of that name was linked above)
        ScalpMaps maps = s.groom->scalpMaps();
        maps.maps[map] = newScalpMapEntry(s, *scalp, map, kind, false, 1.0f, PaintColor{1.0f, 1.0f, 1.0f, 1.0f},
                                          static_cast<int>(optNumber(args, "resolution", 1024.0)), faces);
        std::string graph;
        try {
            graph = wireParamMap(graphBefore, maps, node, param, map, kind);
        } catch (const std::exception& e) {
            fail("bad_request", e.what());
        }
        StrokeEdit edit;
        edit.mode = "map " + node + "." + param;
        edit.set(StrokeField::Graph, std::move(graph));
        edit.set(StrokeField::ScalpMaps, maps.toJson());
        t.host.commit(std::move(edit));
        mapKind = kind;
    }
    // The Paint brush on it, toward 1: strokes paint the param back to its value, Ctrl toward 0.
    BrushToolSettings& st = t.tool.settings;
    st.setMode("paint");
    st.brush.attribute = map;
    st.paintKind = mapKind;
    const auto entry = s.groom->scalpMaps().maps.find(map);
    st.paintTexture = mapKind == "texture" && entry != s.groom->scalpMaps().maps.end() ? entry->second.texture : std::string();
    st.brush.value = 1.0f;
    const bool changed = s.groom->data().graphJson != graphBefore;
    return {{"name", map}, {"kind", mapKind}, {"made", changed && !reused}, {"reused", reused}, {"graphChanged", changed},
            {"tool", toolJson(t)}, {"undo", undoJson(t)}};
}

// ---- Map management (Maya's painting.py over groomLabScalpMaps) ---------------------------------------

bool hasLayer(Session& s, const std::string& name) {
    const auto h = s.groom->storedHierarchy();
    if (!h) return false;
    const std::vector<std::string> layers = h->attributeNames();
    return std::find(layers.begin(), layers.end(), name) != layers.end();
}

// "vertices", "texture", "guides", or "" for no map of that name.
std::string mapKindOf(Session& s, const std::string& name) {
    const auto h = s.groom->storedHierarchy();  // (the layers as stored, as hasLayer reads them)
    return groomlab::mapKindOf(s.groom->scalpMaps(), h.get(), name);  // every host's (scalp_maps.h)
}

// 1 (gray) or 4 (color); guide layers are gray (an unknown name: gray, as a new map starts).
int mapChannelsOf(Session& s, const std::string& name) {
    const auto h = s.groom->storedHierarchy();
    const int channels = groomlab::mapChannelsOf(s.groom->scalpMaps(), h.get(), name);
    return channels == 0 ? kGrayMap : channels;
}

std::string needMap(Session& s, const json& args, const char* key = "name") {
    const std::string name = needString(args, key);
    if (mapKindOf(s, name).empty()) fail("not_found", "no map named '" + name + "'");
    return name;
}

MeshPtr mapScalp(Session& s) {
    const MeshPtr scalp = s.groom->cageScalp();
    if (!scalp || scalp->pointCount() == 0) fail("state", "no scalp: set_scalp first");
    return scalp;
}

// A texture map's default file: <maps folder>/<map>.png, a free name.
std::string defaultMapFile(Session& s, const std::string& name) { return freeFilePath((fs::u8path(mapsFolder(s)) / (name + ".png")).generic_u8string()); }

// A texture paint written to `file` (made or rewritten) as part of the next commit's undo step.
void writePaint(ToolState& t, const TexturePaint& paint, const std::string& file) {
    std::vector<FileEdit> e = filesBefore(file, paint.udim() ? paint.udimLayout()->tiles : std::vector<int>{});
    try {
        std::error_code ec;
        fs::create_directories(fs::u8path(file).parent_path(), ec);
        paint.save(file);  // (a tile set: each tile, udimPathFor(file))
    } catch (const std::exception& ex) {
        fail("io", std::string("cannot write the map's image: ") + ex.what());
    }
    filesAfter(t, std::move(e));
}

// A texture map's paint as every host opens it (scalp_paint.h), to fill or convert.
std::shared_ptr<TexturePaint> openPaint(Session& s, const ScalpMapEntry& entry, int resolution) {
    const std::string full = resolveMapFile(s, entry.texture);
    try {
        requirePaintablePng(full);
        return openTexturePaint(*mapScalp(s), full, entry.channels == kColorMap ? kColorMap : kGrayMap, resolution);
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
}

int resolutionOf(const json& args) {
    return std::clamp(static_cast<int>(optNumber(args, "resolution", 1024.0)), BrushToolSettings::kMinPaintResolution,
                      BrushToolSettings::kMaxPaintResolution);
}

// The Map nodes reading map `name` and the (node, param) pairs it feeds through them and the
// params naming it (Maya's map_nodes, map_users): the core's (scalp_maps.h mapReaders).
json mapUsers(ToolState& t, const json& args) {
    const std::string name = needMap(t.session, args);
    const MapReaders readers = mapReaders(t.session.groom->data().graphJson, name);
    json users = json::array();
    for (const auto& [node, param] : readers.users) users.push_back({node, param});
    return {{"name", name}, {"nodes", readers.nodes}, {"users", users}};
}

// What reads map `name` lets go of it (Maya's _unlink_map): its Map nodes removed, the params they
// fed reading their own values again (an exposed param's port removed with them), the params
// naming it back to the node's default: the core's (scalp_maps.h unlinkMapReaders). Returns the
// graph ("" unchanged); `changed` the pairs.
std::string unlinkMapReaders(const std::string& graphText, const std::string& name, json& changed) {
    changed = json::array();
    if (graphText.empty()) return {};
    try {
        bool touched = false;
        std::vector<std::pair<std::string, std::string>> unlinked;
        std::string graph = groomlab::unlinkMapReaders(graphText, name, &touched, &unlinked);
        for (const auto& [node, param] : unlinked) changed.push_back({node, param});
        return touched ? graph : std::string();
    } catch (const std::exception& e) {
        fail("graph", std::string("cannot unlink the map's readers: ") + e.what());
    }
}

// Maya's remove_map: what reads the map lets go of it; a texture map's file stays on disk unless
// `deleteFile` and GroomLab made it (it is in the maps folder) and no other map reads it. One undo
// step (undo puts the file back too).
json mapRemove(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const std::string kind = mapKindOf(s, name);
    json changed;
    const std::string graph = unlinkMapReaders(s.groom->data().graphJson, name, changed);
    StrokeEdit edit;
    edit.mode = "remove map " + name;
    if (!graph.empty()) edit.set(StrokeField::Graph, graph);
    bool deleted = false;
    if (kind == "guides") {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.removeAttribute(name); });
        edit.set(StrokeField::Guides, h.toJson());
    } else {
        ScalpMaps maps = s.groom->scalpMaps();
        const std::string file = kind == "texture" ? resolveMapFile(s, maps.maps[name].texture) : std::string();
        maps.maps.erase(name);
        const bool shared = std::any_of(maps.maps.begin(), maps.maps.end(), [&](const auto& m) {
            return m.second.kind == "texture" && sameFile(resolveMapFile(s, m.second.texture), file);
        });
        std::error_code ec;
        if (optBool(args, "deleteFile", false) && mapImageDeletable(file, mapsFolder(s), shared)) {  // every host's rule (asset_files.h)
            // (A tile set: every tile's file.)
            std::vector<FileEdit> edits = filesBefore(file);
            for (const FileEdit& e : edits) {
                if (e.before) fs::remove(fs::u8path(e.path), ec);
            }
            filesAfter(t, std::move(edits));
            deleted = true;
        }
        dropPaints(t);
        edit.set(StrokeField::ScalpMaps, maps.toJson());
    }
    commitEdit(t, std::move(edit));
    return {{"removed", name}, {"unlinked", changed}, {"graphChanged", !graph.empty()}, {"fileDeleted", deleted}, {"undo", undoJson(t)}};
}

// Maya's rename_map: the map renamed, the Map nodes and map params reading it follow
// (retargetMapReaders, compounds too), and a texture map's file follows when it is named after the
// map (renamedMapTexture) and GroomLab made it (in the maps folder; a free name when one is in the
// way). One undo step.
json mapRename(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string from = needMap(s, args);
    const std::string to = needString(args, "to");
    if (!validMapName(to)) fail("bad_request", mapNameError(to));
    if (from == to) return {{"name", to}, {"renamed", from}, {"graphChanged", false}, {"undo", undoJson(t)}};
    if (!mapKindOf(s, to).empty()) fail("bad_request", "the groom already has a map named '" + to + "'");
    const std::string kind = mapKindOf(s, from);
    StrokeEdit edit;
    edit.mode = "rename map " + from;
    std::string file, movedFrom;
    if (kind == "guides") {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.renameAttribute(from, to); });
        edit.set(StrokeField::Guides, h.toJson());
    } else {
        ScalpMaps maps = s.groom->scalpMaps();
        auto node = maps.maps.extract(from);
        node.key() = to;
        ScalpMapEntry& entry = node.mapped();
        if (entry.kind == "texture" && !entry.texture.empty()) {
            const std::string full = resolveMapFile(s, entry.texture);
            const std::string target = renamedMapImage(full, from, to, mapsFolder(s));  // every host's rule (asset_files.h)
            std::error_code ec;
            if (!target.empty()) {
                // The image renamed (a tile set: each tile, its naming kept: udim.h mapImageMoves,
                // every host's), each file's before and after kept for undo.
                const auto moves = mapImageMoves(full, target);
                std::vector<FileEdit> edits;
                for (const auto& [a, b] : moves) {
                    edits.push_back(fileBefore(a));
                    edits.push_back(fileBefore(b));
                }
                bool moved = true;
                for (const auto& [a, b] : moves) {
                    fs::rename(fs::u8path(a), fs::u8path(b), ec);
                    moved = moved && !ec;
                }
                filesAfter(t, std::move(edits));
                if (moved) {
                    entry.texture = target;
                    movedFrom = full;
                }
            }
            file = entry.texture;
        }
        maps.maps.insert(std::move(node));
        dropPaints(t);
        edit.set(StrokeField::ScalpMaps, maps.toJson());
    }
    bool retargeted = false;
    std::string graph = retargetMapReaders(s.groom->data().graphJson, from, to, &retargeted);
    bool followed = false;  // texture params reading the file read it where it went
    graph = retargetFileReaders(graph, movedFrom, file, [&s](const std::string& stored) { return resolveMapFile(s, stored); }, &followed);
    retargeted = retargeted || followed;
    if (retargeted) edit.set(StrokeField::Graph, graph);
    commitEdit(t, std::move(edit));
    // The Paint brush on it follows.
    BrushToolSettings& st = t.tool.settings;
    if (st.brush.attribute == from) {
        st.brush.attribute = to;
        if (!file.empty()) st.paintTexture = file;
    }
    json r = {{"name", to}, {"renamed", from}, {"graphChanged", retargeted}, {"undo", undoJson(t)}};
    if (!file.empty()) r["file"] = file;
    return r;
}

// Maya's convert_map: the map resampled into another kind, its name kept (the graph reads it as
// before): guides <-> vertices <-> texture. To a texture: `resolution` square, at `texture`, the
// map's file, or the maps folder (a new file never replaces one). A color map on the guides keeps
// its luminance. One undo step.
json mapConvert(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const std::string to = needString(args, "kind");
    if (to != "guides" && to != "vertices" && to != "texture") fail("bad_request", "kind is guides, vertices or texture");
    const std::string from = mapKindOf(s, name);
    if (from == to) return {{"name", name}, {"kind", to}, {"undo", undoJson(t)}};
    const MeshPtr scalp = mapScalp(s);
    const int resolution = resolutionOf(args);
    const int channels = mapChannelsOf(s, name);
    ScalpMaps maps = s.groom->scalpMaps();
    json warnings = json::array();
    // The source as its kind holds it, converted (every host's: scalp_paint.h convertMap; a guide
    // layer read at the guides' roots on the cage).
    MapSource source;
    source.kind = from;
    source.channels = channels;
    const HierarchyPtr authored = s.groom->authoredHierarchy();
    if (from == "guides") {
        if (!authored) fail("state", "the groom has no guides");
    } else if (from == "vertices") {
        source.vertices = maps.maps[name].values;
    } else {
        const std::string full = resolveMapFile(s, maps.maps[name].texture);
        try {
            source.image = loadTexture(full);
        } catch (const std::exception& e) {
            fail("io", "map " + name + "'s image does not read: " + e.what());
        }
        if (!source.image) fail("io", "map " + name + "'s image does not read");
    }
    StrokeEdit edit;
    edit.mode = "convert map " + name;
    std::string file;
    GuideHierarchy converted = authored ? *authored : GuideHierarchy{};
    try {
        MapConverted out = convertMap(name, source, to, *scalp, converted, resolution);
        if (out.colorLost) warnings.push_back(name + " is a color map; on the guides it keeps its luminance (a guide layer is gray)");
        if (to == "vertices") {
            ScalpMapEntry entry{"vertices", std::move(out.vertices), ""};
            entry.channels = channels;
            maps.maps[name] = std::move(entry);
        } else if (to == "texture") {
            const std::string wanted = optString(args, "texture", "");
            if (!wanted.empty() && !isPngPath(wanted)) fail("bad_request", "a texture map's file is a PNG (painting writes PNG)");
            file = wanted.empty() ? defaultMapFile(s, name) : freeFilePath(wanted);
            writePaint(t, *out.texture, file);
            ScalpMapEntry entry{"texture", {}, file};
            entry.channels = channels;
            maps.maps[name] = std::move(entry);
        } else {
            maps.maps.erase(name);
        }
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    if (to == "guides" || from == "guides") {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) {
            // The layer as convertMap left it (the guides' other edits kept as they are).
            if (from == "guides") hh.removeAttribute(name);
            if (to != "guides") return;
            for (const HierarchyGuide& g : converted.guides()) {
                const auto value = g.attributes.find(name);
                if (value != g.attributes.end() && hh.guide(g.id)) hh.setAttribute(g.id, name, value->second);
            }
        });
        edit.set(StrokeField::Guides, h.toJson());
    }
    dropPaints(t);
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    commitEdit(t, std::move(edit));
    json r = {{"name", name}, {"kind", to}, {"warnings", warnings}, {"undo", undoJson(t)}};
    if (!file.empty()) r["file"] = file;
    return r;
}

// Maya's set_map_channels: gray (1: its luminance) or color (4: the value in r g b, alpha 1). A guide
// layer stays gray. One undo step (none when it already has them).
json mapSetChannels(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const json& c = need(args, "channels");
    if (!c.is_number_integer() || (c.get<int>() != kGrayMap && c.get<int>() != kColorMap)) fail("bad_request", "channels is 1 (gray) or 4 (color)");
    const int wanted = c.get<int>();
    const std::string kind = mapKindOf(s, name);
    if (kind == "guides") {
        if (wanted == kColorMap) fail("bad_request", "a guide layer is gray: convert " + name + " to vertices or a texture first");
        return {{"channels", kGrayMap}, {"undo", undoJson(t)}};
    }
    const int channels = mapChannelsOf(s, name);
    if (channels != wanted) {
        ScalpMaps maps = s.groom->scalpMaps();
        ScalpMapEntry& entry = maps.maps[name];
        if (kind == "vertices") {
            entry.values = convertMapChannels(entry.values, channels, wanted);
        } else {
            const std::shared_ptr<TexturePaint> paint = openPaint(s, entry, resolutionOf(args));
            paint->setChannels(wanted);
            writePaint(t, *paint, resolveMapFile(s, entry.texture));
            dropPaints(t);
        }
        entry.channels = wanted;
        StrokeEdit edit;
        edit.mode = "map channels " + name;
        edit.set(StrokeField::ScalpMaps, maps.toJson());
        commitEdit(t, std::move(edit));
    }
    return {{"channels", wanted}, {"undo", undoJson(t)}};
}

// Maya's fill_map: the map set to `value` everywhere (any kind), or on the selected guides only
// (`selected`: a guide layer); a color map takes `color` in the channels of `channels`, or `value`
// in its r g b (its alpha kept). One undo step.
json mapFill(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const bool byColor = given(args, "color");
    if (!given(args, "value") && !byColor) fail("bad_request", "fill a map with a value or a color");
    const float value = given(args, "value") ? std::clamp(settingNumber(args["value"], "value"), 0.0f, 1.0f) : 0.0f;
    PaintColor color{value, value, value, 1.0f};
    if (byColor) {
        const json& v = args["color"];
        if (!v.is_array() || (v.size() != 3 && v.size() != 4)) fail("bad_request", "color is [r, g, b] or [r, g, b, a]");
        for (std::size_t i = 0; i < v.size(); ++i) color[i] = std::clamp(settingNumber(v[i], "color"), 0.0f, 1.0f);
    }
    unsigned mask = kAllChannels;
    try {
        mask = channelMask(optString(args, "channels", "rgba").empty() ? std::string("rgba") : optString(args, "channels", "rgba"));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", std::string("channels: ") + e.what());
    }
    if (!byColor) mask &= 0x7u;  // a value fills a color map's r g b
    const std::string kind = mapKindOf(s, name);
    const int channels = mapChannelsOf(s, name);
    const bool selected = optBool(args, "selected", false);
    PaintMode paintMode = PaintMode::Replace;
    try {
        paintMode = paintModeFromString(optString(args, "paintMode", "replace"));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", std::string("paintMode: ") + e.what());
    }
    if (byColor && selected) fail("bad_request", "Fill Selected sets a guide layer, which is gray; a color fills a color map");
    if (byColor && channels != kColorMap) fail("bad_request", "a color fills a color map; " + name + " is gray (a value fills it)");
    if (selected && kind != "guides") fail("bad_request", "Fill Selected sets a guide layer on the selected guides; " + name + " lives on the scalp");
    // `faces`: only those polygons of the scalp (a guide layer: the guides rooted on them).
    const std::vector<std::int32_t> faces = facesArg(args);
    if (!faces.empty() && selected) fail("bad_request", "fill the selected guides or the selected faces, not both");
    const MeshPtr faceScalp = faces.empty() ? MeshPtr() : mapScalp(s);
    const std::string label = "fill map " + name;
    // (Every host's Fill: scalp_paint.h fillGuideLayer, fillVertexMap, fillTexturePaint.)
    const PaintColor fill = byColor ? color : PaintColor{value, value, value, 1.0f};
    if (kind == "guides") {
        editGuides(t, label, [&](GuideHierarchy& h) {
            const std::vector<GuideId> picked = selected ? targetGuides(t, h, args, false, "fill selected") : std::vector<GuideId>{};
            fillGuideLayer(h, name, value, paintMode, picked, faceScalp.get(), faces);
        });
    } else if (kind == "vertices") {
        const MeshPtr scalp = mapScalp(s);
        ScalpMaps maps = s.groom->scalpMaps();
        fillVertexMap(maps.maps[name].values, channels, mask, fill, paintMode, *scalp, faces);
        StrokeEdit edit;
        edit.mode = label;
        edit.set(StrokeField::ScalpMaps, maps.toJson());
        commitEdit(t, std::move(edit));
    } else {
        // The file rewritten: its bytes before and after are the undo step.
        const ScalpMapEntry entry = s.groom->scalpMaps().maps.at(name);
        const std::shared_ptr<TexturePaint> paint = openPaint(s, entry, resolutionOf(args));
        fillTexturePaint(*paint, mask, fill, paintMode, *mapScalp(s), faces);
        writePaint(t, *paint, resolveMapFile(s, entry.texture));
        dropPaints(t);
        StrokeEdit edit;
        edit.mode = label;
        commitEdit(t, std::move(edit));
    }
    return {{"name", name}, {"undo", undoJson(t)}};
}

// An image to read as a texture map (Maya's set_map_texture rules): a PNG in place, any other
// image (or a PNG with `copy`) copied into the maps folder as <map>.png (a free name; the copy is
// part of the undo step). Returns the file the map reads; `image` the image read.
std::string mapImageFile(ToolState& t, const std::string& name, const std::string& path, const json& args, TexturePtr& image) {
    Session& s = t.session;
    if (path.empty()) fail("bad_request", "give the image's path");
    std::string full = resolveMapFile(s, path);
    if (full.empty()) full = path;
    std::error_code ec;
    if (fileStamp(full) == 0) fail("not_found", "no image at " + full);  // (a tile set: any tile)
    try {
        image = loadTexture(full);
    } catch (const std::exception& e) {
        fail("bad_request", full + " does not read as an image: " + e.what());
    }
    if (!image || (!image->isUdim() && !image->image())) fail("bad_request", full + " does not read as an image");
    const bool png = isPngPath(full);
    const bool copy = given(args, "copy") ? settingBool(args["copy"], "copy") : !png;
    if (!copy && !png) fail("bad_request", full + " is not a PNG: painting writes PNG, so another format is copied as one (copy true)");
    if (!copy) return fs::u8path(full).generic_u8string();
    const std::string target = defaultMapFile(s, name);
    const std::vector<int> tiles = image->isUdim() ? image->udimTiles() : std::vector<int>{};
    std::vector<FileEdit> e = filesBefore(target, tiles);
    fs::create_directories(fs::u8path(target).parent_path(), ec);
    if (png && !image->isUdim()) {
        fs::copy_file(fs::u8path(full), fs::u8path(target), ec);
        if (ec) fail("io", "could not copy " + full + ": " + ec.message());
    } else if (png) {
        // A tile set of PNGs: each tile copied, into the set udimPathFor(target) names.
        const std::string fromSet = udimPathFor(full), toSet = udimPathFor(target);
        for (const int tile : tiles) {
            fs::copy_file(fs::u8path(udimTilePath(fromSet, tile)), fs::u8path(udimTilePath(toSet, tile)), fs::copy_options::overwrite_existing, ec);
            if (ec) fail("io", "could not copy " + udimTilePath(fromSet, tile) + ": " + ec.message());
        }
    } else {
        try {
            writeImagePng(*image, target);  // (a tile set: each tile)
        } catch (const std::exception& ex) {
            fail("io", std::string("could not write ") + target + ": " + ex.what());
        }
    }
    filesAfter(t, std::move(e));
    return target;
}

// Maya's import_map: a new texture map reading the image `path`, named `name` or after the file
// (its letters, digits and _; a free name), color when the image has color unless `color` says;
// the file rules of mapImageFile. One undo step.
json mapImport(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string path = needString(args, "path");
    std::string name = optString(args, "name", "");
    if (name.empty()) name = mapNameFromFile(path, [&](const std::string& n) { return !mapKindOf(s, n).empty(); });  // (every host's rule)
    if (!validMapName(name)) fail("bad_request", mapNameError(name));
    if (!mapKindOf(s, name).empty()) fail("bad_request", "the groom already has a map named '" + name + "'");
    TexturePtr image;
    const std::string file = mapImageFile(t, name, path, args, image);
    const bool color = given(args, "color") ? settingBool(args["color"], "color") : !isGrayImage(*image);
    ScalpMaps maps = s.groom->scalpMaps();
    ScalpMapEntry entry{"texture", {}, file};
    entry.channels = color ? kColorMap : kGrayMap;
    maps.maps[name] = std::move(entry);
    commitMaps(t, "import map " + name, maps);
    pruneSelection(t);
    return {{"name", name}, {"kind", "texture"}, {"channels", color ? kColorMap : kGrayMap}, {"file", file}, {"undo", undoJson(t)}};
}

// Maya's set_map_texture: map `name` (any kind) reads the image `path` as a texture map, keeping
// its channels (a guide layer: gray); the file rules of mapImageFile. One undo step.
json mapRelink(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const std::string kind = mapKindOf(s, name);
    const int channels = mapChannelsOf(s, name);
    TexturePtr image;
    const std::string file = mapImageFile(t, name, needString(args, "path"), args, image);
    StrokeEdit edit;
    edit.mode = "relink map " + name;
    if (kind == "guides") {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.removeAttribute(name); });
        edit.set(StrokeField::Guides, h.toJson());
    }
    ScalpMaps maps = s.groom->scalpMaps();
    ScalpMapEntry entry{"texture", {}, file};
    entry.channels = channels;
    if (kind == "texture") {  // texture params reading the map's old file read the new one
        bool followed = false;
        const std::string graph = retargetFileReaders(s.groom->data().graphJson, resolveMapFile(s, maps.maps[name].texture), file,
                                                      [&s](const std::string& stored) { return resolveMapFile(s, stored); }, &followed);
        if (followed) edit.set(StrokeField::Graph, graph);
    }
    maps.maps[name] = std::move(entry);
    dropPaints(t);
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    commitEdit(t, std::move(edit));
    BrushToolSettings& st = t.tool.settings;
    if (st.brush.attribute == name && st.paintKind == "texture") st.paintTexture = file;
    return {{"name", name}, {"file", file}, {"undo", undoJson(t)}};
}

// Maya's reload_map: texture map `name`'s image (every texture map's without a name) read again
// after it changed outside (the next evaluation reads files whose stamp changed). Not an undo step.
json mapReload(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string only = optString(args, "name", "");
    if (!only.empty() && mapKindOf(s, only) != "texture") fail("bad_request", only + " is not a texture map");
    json files = json::array();
    for (const auto& [name, entry] : s.groom->scalpMaps().maps) {
        if (entry.kind == "texture" && (only.empty() || name == only)) files.push_back(resolveMapFile(s, entry.texture));
    }
    dropPaints(t);
    return {{"files", files}};
}

// Whether `path` is in a maps folder a session made in the temp folder while its .blend was
// untitled: <temp>/groomlab_<pid>_s<n>maps_<k>/, gl_ on macOS (blockName: this session's or an
// earlier helper's).
bool inTempMapsFolder(const std::string& path) {
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    if (ec) return false;
    const fs::path folder = fs::u8path(path).parent_path();
    const std::string name = folder.filename().u8string();
    return name.rfind(blockNamePrefix(), 0) == 0 && name.find("maps") != std::string::npos && insideFolder(folder.generic_u8string(), temp.generic_u8string());
}

// The host's own maps folder for the groom (Blender: <blend folder>/groomlab/<blend>/masks/<scalp>,
// "" for an untitled file: the temp folder, nothing copied). The texture maps whose image GroomLab
// made in another folder (the session's maps folder before, or a temp maps folder of an untitled
// file, this session's or an earlier one's) are copied into it under the same name (a free one
// when taken) and point there; not an undo step, as Maya's scene save repoints them. Returns the
// folder and what was copied.
json mapsMoveTo(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string dir = optString(args, "dir", "");
    const std::string old = mapsFolder(s);
    s.mapsDir = dir;
    const std::string now = mapsFolder(s);
    json moved = json::array();
    if (dir.empty()) return {{"folder", now}, {"moved", moved}};  // untitled: they stay where they are
    // What the brush holds unwritten goes to its file first.
    for (const auto& kept : t.host.kept) writeTexture(t, kept.first);
    ScalpMaps maps = s.groom->scalpMaps();
    bool changed = false;
    std::error_code ec;
    for (auto& [name, entry] : maps.maps) {
        if (entry.kind != "texture" || entry.texture.empty()) continue;
        const std::string file = resolveMapFile(s, entry.texture);
        // (A tile set: any tile there, udim.h.)
        if (insideFolder(file, now) || !(insideFolder(file, old) || inTempMapsFolder(file)) || fileStamp(file) == 0) continue;
        fs::create_directories(fs::u8path(now), ec);
        // Under its name, or the same image already there (copied before); else a free name. A
        // tile set: every tile.
        const fs::path wanted = fs::u8path(now) / fs::u8path(file).filename();
        std::string target = wanted.generic_u8string();
        if (!sameMapImage(file, target)) {
            target = freeFilePath(target);
            if (copyMapImage(file, target).empty()) continue;
        }
        moved.push_back({{"name", name}, {"from", file}, {"to", target}});
        entry.texture = target;
        changed = true;
    }
    if (changed) {
        dropPaints(t);
        s.groom->setScalpMaps(maps);
        // Texture params reading a moved map's file read the copy (the painted params name it).
        std::string graph = s.groom->data().graphJson;
        bool graphMoved = false;
        for (const json& m : moved) {
            bool followed = false;
            graph = retargetFileReaders(graph, m["from"].get<std::string>(), m["to"].get<std::string>(),
                                        [&s](const std::string& stored) { return resolveMapFile(s, stored); }, &followed);
            graphMoved = graphMoved || followed;
        }
        if (graphMoved) s.groom->setGraphJson(graph);
        BrushToolSettings& st = t.tool.settings;
        const auto it = maps.maps.find(st.brush.attribute);
        if (st.paintKind == "texture" && it != maps.maps.end()) st.paintTexture = it->second.texture;
    }
    return {{"folder", now}, {"moved", moved}};
}

// Collect Files (Maya's collect_files): every file the groom uses from outside `dir` (the .blend's
// folder) copied into its groomlab folders (texture maps into the maps folder, the graph's files
// into `masks`, the card bake into `cards`, Simulate bakes into `caches`) and the groom pointed at
// the copies; a file there already stays, a missing one is reported. Not an undo step.
json filesCollect(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string dir = needString(args, "dir"), masks = needString(args, "masks"), cards = needString(args, "cards"),
                      caches = needString(args, "caches");
    // The bookkeeping every host's Collect Files keeps (asset_files.h FileCollector).
    FileCollector files(dir);
    auto collect = [&](const std::string& what, const std::string& stored, const std::string& full, const std::string& folder) {
        return files.collect(what, stored, full, folder);
    };
    for (const auto& kept : t.host.kept) writeTexture(t, kept.first);
    ScalpMaps maps = s.groom->scalpMaps();
    bool mapsChanged = false;
    for (auto& [name, entry] : maps.maps) {
        if (entry.kind != "texture") continue;
        const std::string now = collect("map " + name, entry.texture, resolveMapFile(s, entry.texture), mapsFolder(s));
        if (now != entry.texture) entry.texture = now, mapsChanged = true;
    }
    if (mapsChanged) {
        dropPaints(t);
        s.groom->setScalpMaps(maps);
    }
    bool graphChanged = false;
    const std::string graph = followGraphFiles(
        s.groom->data().graphJson,
        [&](const std::string& stored) {
            const std::string full = isAbsolutePath(stored) || stored.find("<UDIM>") != std::string::npos
                                         ? (isAbsolutePath(stored) ? stored : (fs::u8path(dir) / fs::u8path(stored)).generic_u8string())
                                         : resolveGroomPath(s.file, stored, dir);
            return collect("param", stored, full.empty() ? (fs::u8path(dir) / fs::u8path(stored)).generic_u8string() : full, masks);
        },
        &graphChanged);
    if (graphChanged) s.groom->setGraphJson(graph);
    for (auto& [node, ref] : s.file.simCaches) ref.file = collect("simulation " + node, ref.file, ref.file, caches);
    s.file.cardBake.prefix = files.collectCardBake(s.file.cardBake.prefix, cards);
    json copied = json::array(), missing = json::array();
    for (const auto& [what, path] : files.copied()) copied.push_back({what, path});
    for (const auto& [what, path] : files.missing()) missing.push_back({what, path});
    return {{"copied", copied}, {"missing", missing}, {"inside", files.inside()}, {"mapsChanged", mapsChanged}, {"graphChanged", graphChanged}};
}

// The map shown on the scalp: the session's view of a vertex map or guide layer; a texture map
// (its file, or the brush's live image while painting) as it is when `textured` (the host draws
// the image at the scalp's uvs, as Maya does: scalpMapImage), else sampled at the scalp's vertices.
ScalpMapView mapView(Session& s, const std::string& name, const std::string& channel, bool textured) {
    ScalpMapView view = s.groom->scalpMapView(name, channel);
    if (view.kind != "texture") return view;
    const MeshPtr scalp = s.groom->scalp();
    if (!scalp || !scalp->hasUVs()) {
        view.kind.clear();
        return view;
    }
    TexturePtr texture;
    if (!view.preview) {
        try {
            texture = loadTexture(view.texture);
        } catch (const std::exception&) {
        }
    }
    const Texture::Image* image = texture ? texture->image() : nullptr;
    if (!view.preview && !image) {
        view.kind.clear();
        return view;
    }
    if (textured) return view;
    const std::shared_ptr<std::vector<float>> values = textureMapAtVertices(view, *scalp);  // every host's (groom_session.h)
    if (!values) {
        view.kind.clear();
        return view;
    }
    view.kind = "vertices";
    view.values = values;
    view.version = hashCombine(hashCombine(view.version, scalp->contentHash()), view.preview ? view.preview->revision() : 0);
    return view;
}

// ---- Display ------------------------------------------------------------------------------------

std::array<float, 3> rgbOf(const json& v, const char* what) {
    if (!v.is_array() || v.size() < 3) fail("bad_request", std::string(what) + " is [r, g, b]");
    return {settingNumber(v[0], what), settingNumber(v[1], what), settingNumber(v[2], what)};
}

std::array<float, 4> rgbaOf(const json& v, const char* what) {
    const std::array<float, 3> c = rgbOf(v, what);
    return {c[0], c[1], c[2], v.size() > 3 ? settingNumber(v[3], what) : 1.0f};
}

int colorModeOf(const json& v) {
    if (v.is_number_integer()) {
        const int m = v.get<int>();
        if (m < kSolidColors || m > kDensityColors) fail("bad_request", "colorMode is 0-5");
        return m;
    }
    const std::string name = settingString(v, "colorMode");
    static const std::map<std::string, int> modes = {{"solid", kSolidColors}, {"colorSet", kColorSetColors}, {"attribute", kAttributeColors},
                                                     {"random", kRandomColors}, {"clump", kClumpColors},     {"density", kDensityColors}};
    const auto it = modes.find(name);
    if (it == modes.end()) fail("bad_request", "colorMode is solid, colorSet, attribute, random, clump or density");
    return it->second;
}

const char* colorModeName(int mode) {
    switch (mode) {
        case kColorSetColors: return "colorSet";
        case kAttributeColors: return "attribute";
        case kRandomColors: return "random";
        case kClumpColors: return "clump";
        case kDensityColors: return "density";
        default: return "solid";
    }
}

DisplaySettings displaySettingsOf(ToolState& t, const json& v) {
    DisplaySettings s;
    s.guideSmoothing = 4;
    std::string hidden;
    for (const std::string& g : t.hidden) hidden += (hidden.empty() ? "" : " ") + g;
    s.hiddenGuideGroups = hidden;
    s.guideGroupColors = groupColorsText(t);  // guides_edit set_group_color's (the settings may give theirs)
    if (v.is_null()) return s;
    if (!v.is_object()) fail("bad_request", "settings is an object");
    for (const auto& [key, x] : v.items()) {
        const char* k = key.c_str();
        if (key == "guides") s.guides = settingBool(x, k);
        else if (key == "tubes") s.tubes = settingBool(x, k);
        else if (key == "flow") s.flow = settingBool(x, k);
        else if (key == "tubeWires") s.tubeWires = settingBool(x, k);    // the Tube brush's tubes (the core's display)
        else if (key == "tubeShaded") s.tubeShaded = settingBool(x, k);
        else if (key == "tubeColors") s.tubeColors = settingBool(x, k);
        else if (key == "tubeLevelColors") s.tubeLevelColors = settingBool(x, k);  // Colors (Tube levels)
        else if (key == "tubeViewGroup") s.tubeViewGroup = settingString(x, k);   // Lock view: that group's tubes alone
        else if (key == "mixedView") s.mixedView = settingString(x, k);           // Mixed view: "<base>:<level>"
        else if (key == "guideWidth") s.guideWidth = settingNumber(x, k);
        else if (key == "guideColorMode") {
            if (x.is_string()) {
                const std::string m = x.get<std::string>();
                s.guideColorMode = m == "layer" ? 1 : m == "group" ? 0 : -1;
                if (s.guideColorMode < 0) fail("bad_request", "guideColorMode is group or layer");
            } else {
                s.guideColorMode = settingInt(x, k) == 1 ? 1 : 0;
            }
        } else if (key == "guideColorLayer") s.guideColorLayer = settingString(x, k);
        else if (key == "guideColorSeed") s.guideColorSeed = t.colorSeed = settingString(x, k);  // the groom's name
        else if (key == "isolateGroup") s.isolateGroup = settingString(x, k);
        else if (key == "guidePoints") {
            if (x.is_string()) {
                const std::string m = x.get<std::string>();
                s.guidePoints = m == "off" ? 0 : m == "ends" ? 1 : m == "all" ? 2 : -1;
                if (s.guidePoints < 0) fail("bad_request", "guidePoints is off, ends or all");
            } else {
                s.guidePoints = std::clamp(settingInt(x, k), 0, 2);
            }
        } else if (key == "guideSmoothing") s.guideSmoothing = settingInt(x, k);
        else if (key == "hiddenGuideGroups") {
            if (x.is_array()) {
                std::string names;
                for (const json& n : x) names += (names.empty() ? "" : " ") + settingString(n, k);
                s.hiddenGuideGroups = names;
            } else {
                s.hiddenGuideGroups = settingString(x, k);
            }
        } else if (key == "guideGroupColors") s.guideGroupColors = settingString(x, k);
        else if (key == "braidTubes") {
            // Show Tubes: the Braid nodes whose section tubes draw (names, one a line, or a list).
            if (x.is_array()) {
                std::vector<std::string> names;
                for (const json& n : x) names.push_back(settingString(n, k));
                s.braidTubes = braidTubesText(names);
            } else {
                s.braidTubes = settingString(x, k);
            }
        }
        else if (key == "theme") {
            if (!x.is_object()) fail("bad_request", "theme is an object");
            DisplayTheme& th = s.theme;
            for (const auto& [tk, tv] : x.items()) {
                const char* n = tk.c_str();
                if (tk == "pointSize") th.pointSize = settingNumber(tv, n);
                else if (tk == "selected") th.selected = rgbaOf(tv, n);
                else if (tk == "softSelected") th.softSelected = rgbaOf(tv, n);
                else if (tk == "frozen") th.frozen = rgbOf(tv, n);
                else if (tk == "parts") th.parts = rgbaOf(tv, n);
                else if (tk == "flow") th.flow = rgbaOf(tv, n);
                else if (tk == "flowField") th.flowField = rgbaOf(tv, n);
                else if (tk == "tubes") th.tubes = rgbaOf(tv, n);
                else if (tk == "clumpCenters") th.clumpCenters = rgbOf(tv, n);
                else fail("bad_request", "no theme color '" + tk + "'");
            }
            th.version = hashString(hash64(0x7E3E), x.dump());
        } else fail("bad_request", "no display setting '" + key + "'");
    }
    t.host.setMixedView(s.mixedView);  // (the root tool shows the roots as Mixed view does)
    return s;
}

const char* layerName(DisplayLayer layer) {
    switch (layer) {
        case DisplayLayer::DormantFilled: return "dormantFilled";
        case DisplayLayer::ActiveWire: return "activeWire";
        case DisplayLayer::Selection: return "selection";
        default: return "dormantWire";
    }
}

constexpr std::array<DisplayItemId, 12> kShownItems{DisplayItemId::Guides,       DisplayItemId::Tubes,     DisplayItemId::Selected,
                                                    DisplayItemId::SoftSelected, DisplayItemId::Parts,     DisplayItemId::Flow,
                                                    DisplayItemId::FlowField,    DisplayItemId::ScalpMap,  DisplayItemId::TubeSurfaces,
                                                    DisplayItemId::TubeWires,    DisplayItemId::SelectedTubeWires,
                                                    DisplayItemId::BraidTubes};  // Show Tubes: the Braid nodes' section tubes

std::uint64_t haveVersion(const json& have, const std::string& name) {
    if (!have.is_object()) return 0;
    const auto it = have.find(name);
    return it != have.end() && it->is_number_unsigned() ? it->get<std::uint64_t>() : 0;
}

json display(ToolState& t, const json& args) {
    Session& s = t.session;
    GroomSession& session = *s.groom;
    const HostSpace& space = s.space;
    DisplaySettings settings = displaySettingsOf(t, args.contains("settings") ? args["settings"] : json(nullptr));
    if (t.tool.settings.tool == BrushToolSettings::Tool::Roots && !t.selectTool && t.tool.roots().hidesGuides()) {
        // Edit roots: the guides and their tubes hidden, only the roots' outlines (Maya's too).
        settings.guides = settings.tubes = settings.tubeWires = settings.tubeShaded = false;
    }
    json have = args.contains("have") ? args["have"] : json::object();
    if (args.contains("generation") && args["generation"] != t.generation) have = json::object();  // another builder's versions

    // The view the host last evaluated, with the guides as they are now (a live stroke's preview
    // without an evaluation), the selection and the map shown.
    const ViewResult& last = session.last();
    DisplayInputs in;
    in.scalp = session.scalp();
    in.parts = last.parts;
    in.flow = last.flow;
    in.clumpCenters = last.clumpCenters;  // a viewed Clump's centers: dots
    // A viewed Scatter's roots: dots in the clump centers' color (as Maya and the app), the hair
    // curves left empty (groom.write_hair).
    if (last.viewedRoots) {
        in.viewedRoots = true;
        in.strands = last.strands;
    }
    in.movingGuides = last.movingGuides;  // a GuideCache's or Simulate's guides, over the groomed ones
    // While a Sculpt node is what brushes shape, its output guides are the guides shown (as Maya's
    // and the app's viewports show them).
    const HierarchyPtr sculpted = session.sculptTarget().empty() ? nullptr : session.sculptHierarchy();
    in.hierarchy = sculpted ? sculpted : session.hierarchy();
    in.hierarchyVersion = in.hierarchy ? hashCombine(in.hierarchy->contentHash(), reinterpret_cast<std::uintptr_t>(in.hierarchy.get())) : 0;
    in.stroke = session.hasPreview();
    pruneSelection(t);
    if (!t.selection.empty()) {
        const std::vector<GuideId> ids = t.selection;
        const float soft = t.softRadius;
        const HierarchyPtr h = in.hierarchy;
        in.selection = [ids, soft, h] {
            if (soft > 0.0f && h) return softSelection(*h, ids, soft);
            std::map<GuideId, float> weights;
            for (const GuideId id : ids) weights[id] = 1.0f;
            return weights;
        };
        in.selectionHash = hashFloat(hash64(t.selectionVersion), soft);
    }
    // A texture map is drawn with its image when the host says it draws one (`textured`).
    bool textured = false;
    if (args.contains("map") && args["map"].is_object()) {
        const json& m = args["map"];
        const std::string name = optString(m, "name", "");
        textured = optBool(m, "textured", false);
        if (!name.empty()) in.scalpMap = mapView(s, name, optString(m, "channel", "rgb"), textured);
    } else if (args.contains("map") && args["map"].is_string() && !args["map"].get<std::string>().empty()) {
        in.scalpMap = mapView(s, args["map"].get<std::string>(), "rgb", false);
    }
    MapImage image;
    const bool imageShown = textured && in.scalpMap.kind == "texture";
    if (imageShown) {
        image = mapImage(in.scalpMap, false);
        in.scalpMapTexture = !image.key.empty();
        in.scalpMapTextureGeneration = image.texture;
    }
    // Show Tubes: the shown Braid nodes' section tubes (the session's mesh, kept while they stay).
    fillBraidTubes(settings, in, [&session]() -> const GroomSession& { return session; });
    const DisplayFrame& frame = t.builder.update(in, settings);

    // What changed against what the host holds: those items' arrays, in one block.
    std::vector<const DisplayItem*> send;
    std::size_t bytes = 0;
    for (const DisplayItemId id : kShownItems) {
        const DisplayItem& item = frame.item(id);
        if (item.geometry && item.colorVersion != haveVersion(have, item.name)) {
            send.push_back(&item);
            bytes += geometryBytes(*item.geometry);
        }
    }
    const std::uint64_t dotsVersion = frame.item(DisplayItemId::Guides).geometryVersion;
    const bool sendDots = dotsVersion != haveVersion(have, "dots");
    if (sendDots) bytes += Arena::reserve(frame.dots.points.size() * 12) * 2;
    // The scalp map's image when the host holds another (its key).
    const bool sendImage = imageShown && !image.key.empty() &&
                           !(have.contains("scalpMapImage") && have["scalpMapImage"].is_string() && have["scalpMapImage"] == image.key);
    if (sendImage) {
        image = mapImage(in.scalpMap, true);
        if (image.pixels) bytes += Arena::reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
    }
    t.displayOut.begin(bytes + 64);

    json items = json::object();
    for (const DisplayItemId id : kShownItems) {
        const DisplayItem& item = frame.item(id);
        json j;
        j["version"] = item.colorVersion;
        j["shown"] = item.shown;
        j["vertexCount"] = item.vertexCount;
        j["color"] = item.color;
        j["lineWidth"] = item.lineWidth;
        j["layer"] = layerName(item.layer);
        const DisplayGeometry* g = item.geometry.get();
        j["primitive"] = g && g->triangles ? "triangles" : "lines";
        j["perVertexColors"] = g && !g->colors.empty();
        j["surface"] = g && g->surface;
        const bool changed = std::find(send.begin(), send.end(), &item) != send.end();
        j["changed"] = changed;
        if (changed) j["arrays"] = geometryArrays(t.displayOut, *g, space);
        items[item.name] = j;
    }
    json dots;
    dots["version"] = dotsVersion;
    dots["count"] = frame.dots.points.size();
    dots["pointSize"] = frame.pointSize;
    dots["changed"] = sendDots;
    if (sendDots) {
        json a;
        a["positions"] = putPoints(t.displayOut, frame.dots.points, space, false);
        std::vector<float> colors;
        colors.reserve(frame.dots.colors.size() * 3);
        for (const auto& c : frame.dots.colors) colors.insert(colors.end(), c.begin(), c.end());
        a["colors"] = put(t.displayOut, colors.data(), frame.dots.colors.size(), 3);
        dots["arrays"] = a;
    }
    items["dots"] = dots;
    if (imageShown) {
        json img;
        img["key"] = image.key;
        img["changed"] = sendImage && image.pixels;
        if (sendImage && image.pixels) {
            img["width"] = image.width;
            img["height"] = image.height;
            const std::size_t texels = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
            img["arrays"] = {{"rgba", put(t.displayOut, image.pixels, texels, 4)}};
        }
        items["scalpMapImage"] = img;
    }

    json r;
    r["generation"] = t.generation;
    r["items"] = items;
    r["overlayPull"] = toHostLength(space, frame.overlayPull);
    r["stroking"] = t.tool.stroking();
    r["selection"] = selectionJson(t);

    // The brush overlay (the ring and what it would touch), when the host holds another.
    json overlay = overlayJson(t, t.overlayVersion != haveVersion(have, "brush"));
    overlay["overlay"]["changed"] = overlay["overlay"].contains("arrays");
    r["brush"] = overlay;

    // The strands' display colors (Maya's strandColorMode) for the last evaluation's strands.
    if (args.contains("strandColors") && args["strandColors"].is_object()) {
        const json& c = args["strandColors"];
        const int mode = colorModeOf(c.contains("mode") ? c["mode"] : json("solid"));
        const std::string source = optString(c, "source", "clumpId");
        const float lo = static_cast<float>(optNumber(c, "min", 0.0)), hi = static_cast<float>(optNumber(c, "max", 1.0));
        const bool perPoint = optBool(c, "perPoint", false);
        const StrandsPtr strands = s.last;
        std::uint64_t key = hashCombine(hash64(reinterpret_cast<std::uintptr_t>(strands.get())), static_cast<std::uint64_t>(mode));
        key = hashString(hashFloat(hashFloat(key, lo), hi), source);
        key = hashCombine(key, perPoint ? 1u : 0u);
        key = hashCombine(key, strands ? strands->strandCount() : 0);
        if (key != t.colorsKey) {
            t.colorsKey = key;
            ++t.colorsVersion;
        }
        const std::string versionName = "strandColors";
        json colors = {{"mode", colorModeName(mode)}, {"version", t.colorsVersion}, {"perPoint", perPoint}};
        const bool want = t.colorsVersion != haveVersion(have, versionName);
        if (mode != kSolidColors && strands && want) {
            // A source naming a node ("<channel>@<node>", a Clump's Clump Colors, issue #77): the
            // channel as that node wrote it, though a later Clump writes the same one.
            std::string ofChannel, ofNode;
            std::shared_ptr<const std::vector<float>> nodeValues;
            if ((mode == kClumpColors || mode == kAttributeColors) && splitNodeColorSource(source, &ofChannel, &ofNode)) {
                nodeValues = session.nodeStrandChannel(ofNode, ofChannel, *strands);
            }
            const std::vector<std::array<float, 3>> per =
                strandColors(*strands, last.scalp ? last.scalp.get() : in.scalp.get(), mode, source, lo, hi, nodeValues.get());
            std::vector<float> flat;
            std::size_t count = 0;
            if (perPoint) {
                count = strands->pointCount();
                flat.reserve(count * 3);
                for (std::size_t i = 0; i < strands->strandCount(); ++i) {
                    for (std::size_t k = 0; k < strands->strandPointCount(i); ++k) flat.insert(flat.end(), per[i].begin(), per[i].end());
                }
            } else {
                count = per.size();
                flat.reserve(count * 3);
                for (const auto& c3 : per) flat.insert(flat.end(), c3.begin(), c3.end());
            }
            t.colorsOut.begin(Arena::reserve(flat.size() * 4));
            colors["colors"] = put(t.colorsOut, flat.data(), count, 3);
            colors["changed"] = true;
            // What the Source offers (the strands' channels, the scalp's color sets) and the source's
            // range (Auto Range), with the colors they go with.
            colors["channels"] = strands->strandChannelNames();
            const ScalpMesh* scalp = last.scalp ? last.scalp.get() : in.scalp.get();
            colors["colorSets"] = scalp ? scalp->colorSetNames() : std::vector<std::string>{};
            float low = 0.0f, high = 0.0f;
            if (strandChannelRange(*strands, source, low, high)) colors["range"] = {low, high};
        } else {
            colors["changed"] = false;
        }
        colors["strands"] = strands ? strands->strandCount() : 0;
        r["strandColors"] = colors;
    }
    return r;
}

}  // namespace

// ---- The host -------------------------------------------------------------------------------------

std::shared_ptr<TexturePaint> HelperStrokeHost::texturePaint(const std::string& name, int channels, int resolution) {
    const auto entry = s_.groom->scalpMaps().maps.find(name);
    if (entry == s_.groom->scalpMaps().maps.end() || entry->second.kind != "texture" || entry->second.texture.empty()) {
        throw std::runtime_error("this groom has no texture map named " + name);
    }
    const std::string full = resolveMapFile(s_, entry->second.texture);
    const MeshPtr scalp = s_.groom->scalp();
    if (!scalp) throw std::runtime_error("the scalp has no UVs to paint a texture map on");
    requirePaintablePng(full);
    std::shared_ptr<TexturePaint> paint;
    try {
        paint = kept[name].take(*scalp, full, channels, resolution);
    } catch (...) {
        kept.erase(name);
        throw;
    }
    setTexturePaint(name, paint);  // undo and redo write their tiles into it
    return paint;
}

bool HelperStrokeHost::paintSelection(const std::vector<GuideId>& selection, bool final) {
    helper::setSelection(t_, selection);  // (the tools' own, not SessionStrokeHost's)
    SessionStrokeHost::setSelection(selection, t_.softRadius);
    if (final) {
        const std::shared_ptr<const GuideHierarchy> stored = s_.groom->authoredHierarchy();
        GuideHierarchy probe = stored ? *stored : GuideHierarchy{};
        if (freezeToSelection(probe, selection)) {
            editGuides(t_, "solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
        }
    }
    return true;
}

void HelperStrokeHost::commit(StrokeEdit edit) {
    const std::string map = edit.textureMap;
    const bool maps = edit.text(StrokeField::ScalpMaps).set || !map.empty();
    SessionStrokeHost::commit(std::move(edit));
    if (!map.empty()) writeTexture(t_, map);
    t_.redoLabels.clear();
    t_.redoMaps.clear();
    t_.redoSide.clear();
    t_.doneSide.push_back(std::move(t_.pending));  // one per step, beside the host's stack
    t_.pending = SideEdit{};
    if (maps) s_.mapsOwned = true;
}

// ---- Maps of a file ---------------------------------------------------------------------------------

std::string resolveMapFile(const Session& s, const std::string& stored) { return resolveGroomPath(s.file, stored); }

void adoptMaps(Session& s, const GroomData& data) {
    s.fileCatalog = data.maps;
    s.mapsOwned = false;
    bool any = false;
    for (const auto& [name, info] : data.maps) any = any || info.kind == "vertices" || info.kind == "texture";
    if (!any) return;
    const ScalpMaps maps = scalpMapsFromGroomData(data);
    if (maps.maps.empty()) return;
    s.groom->setScalpMaps(maps);
    s.mapsOwned = true;
}

void writeMaps(const Session& s, GroomFile& file) {
    if (!s.mapsOwned) return;
    // Every host's save (groom_file.h storeScalpMaps): with other scalps each keeps its own share.
    storeScalpMaps(file.data, s.groom->scalpMaps(), s.fileCatalog);
}

// ---- Commands ---------------------------------------------------------------------------------------

json toolCatalogJson() {
    json tools = json::array();
    for (const BrushEntry& e : brushCatalog()) {
        tools.push_back({{"id", e.id},
                         {"label", e.label},
                         {"category", brushCategoryName(e.category)},
                         {"order", e.order},
                         {"icon", e.icon},
                         {"defaultStrength", e.defaultStrength},
                         {"signed", e.signedStrength},
                         {"hint", e.hint},
                         {"settings", e.settings},
                         {"corrective", e.corrective}});
    }
    json categories = json::array();
    for (const BrushCategory c : brushCategories()) categories.push_back(brushCategoryName(c));
    json settings = json::array();
    for (const BrushSettingSpec& s : brushSettingSpecs()) {
        json j = {{"id", s.id}, {"label", s.label}, {"hint", s.hint}, {"kind", brushSettingKindName(s.kind)}};
        // A value's facts as the core writes them (a length in the groom's units, not the host's).
        if (s.kind == BrushSettingKind::Bool) {
            j["default"] = s.defaultValue != 0.0;
        } else if (s.isValue()) {
            j.update({{"default", s.defaultValue}, {"softMin", s.softMin}, {"softMax", s.softMax}, {"min", s.min}, {"max", s.max},
                      {"decimals", s.decimals}, {"unit", s.unit}});
        }
        settings.push_back(std::move(j));
    }
    // What New Map > For offers first (stroke.h paintableSettings): the node type and its param.
    json paintable = json::array();
    for (const PaintableSetting& p : paintableSettings()) {
        paintable.push_back({{"id", p.id}, {"label", p.label}, {"type", p.type}, {"param", p.param}, {"hint", p.hint}, {"kind", p.kind}});
    }
    return {{"tools", tools}, {"categories", categories}, {"settings", settings}, {"paintable", paintable}};
}

void resetTools(Session& s) {
    if (!s.tools) return;
    const BrushToolSettings settings = s.tools->tool.settings;
    const PaintTarget before = s.tools->beforeFreeze;
    const bool select = s.tools->selectTool;
    const float soft = s.tools->softRadius;
    if (s.tools->tool.stroking()) s.tools->tool.cancel(nullptr);
    s.tools.reset();
    ToolState& t = toolsOf(s);
    t.tool.settings = settings;
    t.beforeFreeze = before;
    t.selectTool = select;
    t.softRadius = soft;
}

std::string mapsFolderOf(Session& s) { return mapsFolder(s); }

void commitTexts(Session& s, const std::string& label, const std::vector<std::pair<StrokeField, std::string>>& texts) {
    ToolState& t = toolsOf(s);
    StrokeEdit edit;
    edit.mode = label;
    for (const auto& [field, text] : texts) edit.set(field, text);
    commitEdit(t, std::move(edit));
    pruneTargets(t);
}

void commitStep(Session& s, const std::string& label, const std::vector<std::pair<StrokeField, std::string>>& texts,
                std::function<void(bool after)> apply) {
    ToolState& t = toolsOf(s);
    for (FileEdit& e : t.pendingFiles) fileAfter(t, std::move(e));
    t.pendingFiles.clear();
    if (apply) t.pending.actions.push_back(std::move(apply));
    StrokeEdit edit;
    edit.mode = label;
    for (const auto& [field, text] : texts) edit.set(field, text);
    commitEdit(t, std::move(edit));
    pruneTargets(t);
}

void sideFileBefore(Session& s, const std::string& path) {
    for (FileEdit& e : filesBefore(path)) toolsOf(s).pendingFiles.push_back(std::move(e));  // (a tile set: each tile)
}

void dropSideFiles(Session& s) {
    ToolState& t = toolsOf(s);
    for (auto f = t.pendingFiles.rbegin(); f != t.pendingFiles.rend(); ++f) putFileState(f->path, f->before);
    if (!t.pendingFiles.empty()) dropPaints(t);
    t.pendingFiles.clear();
}

json undoStateJson(Session& s) { return undoJson(toolsOf(s)); }

void setShotLayer(Session& s, const std::string& layer) {
    ToolState& t = toolsOf(s);
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    t.shotLayer = layer;
    applyShotLayer(t);
}

std::string shotLayer(Session& s) { return s.tools ? s.tools->shotLayer : std::string(); }

bool isToolCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {
        "tool_set",  "tool_get",      "stroke_begin", "stroke_drag",   "stroke_end",  "stroke_cancel", "hover",    "hover_clear",
        "select",    "selection_get", "selection_set", "pick_level",   "set_hidden_groups", "guides_info", "guides_edit", "maps_list",
        "map_create", "map_remove",   "map_for_param", "undo",        "redo",          "undo_state",  "display",
        // Groom Tools' Tubes section, Segments slider, parts and flow, map management.
        "tube_targets", "tube_get", "tube_set", "tube_scale", "tube_smooth", "tube_shapes", "tube_shape_add", "tube_shape_set",
        "tube_shape_rename", "tube_shape_remove", "tube_shape_preview", "guides_preview", "guides_preview_end", "guide_segments",
        "parts_list", "part_add", "part_remove", "parts_clear", "flow_clear", "map_rename", "map_convert", "map_set_channels",
        "map_fill", "map_import", "map_relink", "map_reload", "map_users", "maps_move_to", "files_collect",
        // The Length Keys tool's keys (the Keys box).
        "length_state", "length_edit",
        // The Tube tool's root-first mode and the tube hierarchy (the Tube card's Tube roots and
        // Tube hierarchy blocks).
        "root_state", "root_edit", "root_subdivide", "density_map", "tube_levels", "tube_level_add", "tube_level_remove", "tube_level_show"};
    return kCommands.count(cmd) > 0;
}

// The Length Keys tool's keys as the Keys box shows them: each key's scale and the length it gives
// the guide nearest it (host units), the selection, whether there are edits to apply, and how
// guides take a new length.
json lengthStateJson(ToolState& t, const HostSpace& space) {
    LengthKeyTool& lk = t.tool.lengths();
    const LengthKeySet& set = lk.keys(t.host);
    json scales = json::array(), lengths = json::array();
    for (std::size_t k = 0; k < set.keys.size(); ++k) {
        scales.push_back(set.keys[k].scale);
        lengths.push_back(toHostLength(space, lk.keyLength(t.host, t.tool.settings, static_cast<int>(k))));
    }
    return {{"scales", scales},
            {"lengths", lengths},
            {"activeKey", lk.activeKey()},
            {"edited", lk.edited()},
            {"change", lengthChangeName(lk.change)}};
}

// The Tube tool's root-first mode as the Tube card shows it (Maya's -rootState): whether it is the
// tool, its mode, what it last said (and a count bumped with each), the roots selected, the CVs
// placed, whether the guides hide (editing).
json rootStateJson(ToolState& t) {
    RootTubeTool& roots = t.tool.roots();
    const bool on = roots.active() && t.tool.settings.tool == BrushToolSettings::Tool::Roots && !t.selectTool;
    if (on) {
        prepareHost(t);
        try {
            roots.sync(t.host);  // (an undo or redo since: the tool reads the groom again)
        } catch (const std::exception&) {
        }
    }
    static const char* kModes[] = {"place", "draw", "edit", "select", "duplicate"};
    json selected = json::array();
    for (const GuideId g : roots.selected()) selected.push_back(g);
    return {{"active", on},
            {"mode", kModes[static_cast<int>(roots.mode())]},
            {"message", roots.message()},
            {"messageCount", roots.messageCount()},
            {"selected", selected},
            {"cvs", roots.outlineCvs()},
            {"hidesGuides", on && roots.hidesGuides()}};
}

json tubeLevelsJson(const GuideHierarchy& h, const std::vector<GuideId>& guides, const std::string& picked) {
    const TubeLevels levels = tubeLevels(h, guides, picked);
    return {{"base", levels.base}, {"levels", levels.levels}, {"maxLevels", kMaxTubeLevels}};
}

json toolStateJson(Session& s) {
    ToolState& t = toolsOf(s);
    pruneSelection(t);
    return {{"selection", t.selection},
            {"softSelectRadius", toHostLength(s.space, t.softRadius)},
            {"hiddenGroups", t.hidden},
            {"groupColors", t.groupColors}};
}

void restoreToolState(Session& s, const json& state) {
    ToolState& t = toolsOf(s);
    if (!state.is_object()) return;
    const std::shared_ptr<const GuideHierarchy> h = s.groom->storedHierarchy();
    if (state.contains("selection") && state["selection"].is_array()) {
        std::vector<GuideId> ids = idsOf(state["selection"]);
        ids.erase(std::remove_if(ids.begin(), ids.end(), [&](GuideId id) { return !h || !h->guide(id); }), ids.end());
        setSelection(t, std::move(ids));
    }
    if (state.contains("softSelectRadius") && state["softSelectRadius"].is_number()) {
        t.softRadius = std::max(0.0f, toGroomLength(s.space, state["softSelectRadius"].get<double>()));
    }
    if (state.contains("hiddenGroups") && state["hiddenGroups"].is_array()) {
        t.hidden.clear();
        for (const json& g : state["hiddenGroups"]) {
            if (g.is_string()) t.hidden.insert(g.get<std::string>());
        }
    }
    if (state.contains("groupColors") && state["groupColors"].is_object()) {
        t.groupColors.clear();
        for (const auto& [group, color] : state["groupColors"].items()) {
            if (color.is_string()) t.groupColors[group] = color.get<std::string>();
        }
    }
}

namespace {

// The edits that are not strokes, selection or display (Tubes, Segments, parts, flow, maps).
bool runEditCommand(ToolState& t, const std::string& cmd, const json& args, BlockReader& blocks, json& result) {
    static const std::map<std::string, std::function<json(ToolState&, const json&, BlockReader&)>> kEdits = {
        {"tube_targets", [](ToolState& t, const json&, BlockReader&) { return tubeTargets(t); }},
        {"tube_get", [](ToolState& t, const json& a, BlockReader&) { return tubeGet(t, a); }},
        {"tube_set", [](ToolState& t, const json& a, BlockReader&) { return tubeSet(t, a); }},
        {"tube_scale", [](ToolState& t, const json& a, BlockReader&) { return tubeScale(t, a); }},
        {"tube_smooth", [](ToolState& t, const json& a, BlockReader&) { return tubeSmooth(t, a); }},
        {"tube_shapes", [](ToolState& t, const json&, BlockReader&) { return tubeShapes(t); }},
        {"tube_shape_add", [](ToolState& t, const json& a, BlockReader&) { return tubeShapeEdit(t, "add", a); }},
        {"tube_shape_set", [](ToolState& t, const json& a, BlockReader&) { return tubeShapeEdit(t, "set", a); }},
        {"tube_shape_rename", [](ToolState& t, const json& a, BlockReader&) { return tubeShapeEdit(t, "rename", a); }},
        {"tube_shape_remove", [](ToolState& t, const json& a, BlockReader&) { return tubeShapeEdit(t, "remove", a); }},
        {"tube_shape_preview", [](ToolState& t, const json& a, BlockReader&) { return tubeShapePreview(t, a); }},
        {"guides_preview", [](ToolState& t, const json& a, BlockReader&) { return guidesPreview(t, a); }},
        {"guides_preview_end",
         [](ToolState& t, const json&, BlockReader&) {
             const bool was = t.session.groom->previewHierarchy() != nullptr && !t.tool.stroking();
             if (was) t.session.groom->setPreviewHierarchy(nullptr);
             return json{{"ended", was}};
         }},
        {"guide_segments", [](ToolState& t, const json& a, BlockReader&) { return guideSegments(t, a); }},
        {"parts_list", [](ToolState& t, const json&, BlockReader&) { return partsList(t); }},
        {"part_add", [](ToolState& t, const json& a, BlockReader& b) { return partAdd(t, a, b); }},
        {"part_remove", [](ToolState& t, const json& a, BlockReader&) { return partRemove(t, a); }},
        {"parts_clear", [](ToolState& t, const json&, BlockReader&) { return partsClear(t); }},
        {"flow_clear", [](ToolState& t, const json&, BlockReader&) { return flowClear(t); }},
        {"map_rename", [](ToolState& t, const json& a, BlockReader&) { return mapRename(t, a); }},
        {"map_convert", [](ToolState& t, const json& a, BlockReader&) { return mapConvert(t, a); }},
        {"map_set_channels", [](ToolState& t, const json& a, BlockReader&) { return mapSetChannels(t, a); }},
        {"map_fill", [](ToolState& t, const json& a, BlockReader&) { return mapFill(t, a); }},
        {"map_import", [](ToolState& t, const json& a, BlockReader&) { return mapImport(t, a); }},
        {"map_relink", [](ToolState& t, const json& a, BlockReader&) { return mapRelink(t, a); }},
        {"map_reload", [](ToolState& t, const json& a, BlockReader&) { return mapReload(t, a); }},
        {"map_users", [](ToolState& t, const json& a, BlockReader&) { return mapUsers(t, a); }},
        {"maps_move_to", [](ToolState& t, const json& a, BlockReader&) { return mapsMoveTo(t, a); }},
        {"files_collect", [](ToolState& t, const json& a, BlockReader&) { return filesCollect(t, a); }},
    };
    const auto it = kEdits.find(cmd);
    if (it == kEdits.end()) return false;
    result = it->second(t, args, blocks);
    return true;
}

bool runToolCommandIn(Session& s, ToolState& t, const std::string& cmd, const json& args, BlockReader& blocks, json& result);

}  // namespace

bool runToolCommand(Session& s, const std::string& cmd, const json& args, BlockReader& blocks, json& result) {
    if (!isToolCommand(cmd)) return false;
    ToolState& t = toolsOf(s);
    try {
        if (runEditCommand(t, cmd, args, blocks, result)) return true;
        return runToolCommandIn(s, t, cmd, args, blocks, result);
    } catch (...) {
        dropPending(t);  // an edit refused after writing its files: they are put back
        throw;
    }
}

namespace {

bool runToolCommandIn(Session& s, ToolState& t, const std::string& cmd, const json& args, BlockReader&, json& result) {
    if (cmd == "tool_set") {
        if (args.contains("tool") && !args["tool"].is_null()) {
            if (t.tool.stroking()) t.tool.cancel(&t.host);
            setTool(t, needString(args, "tool"));
        }
        if (args.contains("settings") && !args["settings"].is_null()) applySettings(t, args["settings"]);
        result = toolJson(t);
        return true;
    }
    if (cmd == "tool_get") {
        result = toolJson(t);
        return true;
    }

    if (cmd == "stroke_begin") {
        if (t.selectTool) fail("state", "the Select tool picks guides (select), it does not stroke");
        viewFromArgs(t, args);
        requireScalp(s);
        if (t.tool.stroking()) t.tool.cancel(&t.host);
        const float x = static_cast<float>(optNumber(args, "x", 0.0)), y = static_cast<float>(optNumber(args, "y", 0.0));
        const float pressure = static_cast<float>(optNumber(args, "pressure", 1.0));
        // "adjust": the adjusting button (the Length Keys tool's right-drag: a key's scale).
        const unsigned modifiers = (optBool(args, "ctrl", false) ? kStrokeCtrl : 0u) | (optBool(args, "shift", false) ? kStrokeShift : 0u) |
                                   (optBool(args, "adjust", false) ? kStrokeAdjust : 0u);
        prepareHost(t);
        t.host.clearWarnings();
        t.pressEdits = t.host.edits().size();
        t.pressX = x;
        t.pressY = y;
        // A press that changed the graph (none does now that map_for_param links a setting's map;
        // the add-on's node tree would follow: graphChanged).
        const std::string graphBefore = s.groom->data().graphJson;
        guarded(t, "press", [&] { t.tool.press(t.host, t.view, x, y, pressure, modifiers); });
        rebuildOverlay(t, x, y, false);
        result = overlayJson(t, optBool(args, "overlay", true));
        result["stroking"] = t.tool.stroking() || t.tool.lengthsPressed() || t.tool.rootsPressed();
        result["selecting"] = t.tool.selectingPaint();  // a Ctrl+Shift drag painting the selection
        result["mode"] = t.tool.toolModeName();
        result["graphChanged"] = s.groom->data().graphJson != graphBefore;
        result["view"] = viewJson(t);
        result["warnings"] = warningsJson(t);
        if (t.tool.settings.tool == BrushToolSettings::Tool::Roots) result["roots"] = rootStateJson(t);
        return true;
    }
    if (cmd == "stroke_drag") {
        // (A Shift press has no stroke yet: its drag starts the smoothing.)
        if (!t.tool.stroking() && !t.tool.shiftPending() && !t.tool.lengthsPressed() && !t.tool.rootsPressed() &&
            !t.tool.selectingPaint()) {
            fail("state", "no stroke: stroke_begin first");
        }
        if (args.contains("view") && !args["view"].is_null()) setView(t, args["view"]);
        const float pressure = static_cast<float>(optNumber(args, "pressure", 1.0));
        std::vector<Pixel> points;
        if (args.contains("points") && !args["points"].is_null()) {
            points = pixelsOf(args["points"], pressure);
        } else {
            points.push_back({static_cast<float>(optNumber(args, "x", 0.0)), static_cast<float>(optNumber(args, "y", 0.0)), pressure});
        }
        for (const Pixel& p : points) guarded(t, "drag", [&] { t.tool.drag(t.host, t.view, p.x, p.y, p.pressure); });
        if (!points.empty()) rebuildOverlay(t, points.back().x, points.back().y, false);
        result = overlayJson(t, optBool(args, "overlay", true));
        result["stroking"] = t.tool.stroking();
        result["changed"] = t.tool.changed() || t.tool.lengthsPressed();  // (Length Keys: a live preview)
        result["warnings"] = warningsJson(t);
        return true;
    }
    if (cmd == "stroke_end") {
        const std::size_t before = std::min(t.pressEdits, t.host.edits().size());
        StrokeRelease released = StrokeRelease::None;
        if (t.hasView) guarded(t, "release", [&] { released = t.tool.release(t.host, t.view); });
        result["released"] = released == StrokeRelease::Stroke ? "stroke" : "none";
        result["committed"] = t.host.edits().size() != before;
        if (result["committed"].get<bool>()) {
            t.redoLabels.clear();
            t.redoMaps.clear();
        }
        pruneSelection(t);
        result["undo"] = undoJson(t);
        result["warnings"] = warningsJson(t);
        if (t.tool.settings.tool == BrushToolSettings::Tool::Roots) {
            rebuildOverlay(t, t.pressX, t.pressY, false);
            result.update(overlayJson(t, optBool(args, "overlay", true)));
            result["roots"] = rootStateJson(t);
        }
        return true;
    }
    if (cmd == "stroke_cancel") {
        const bool was = t.tool.stroking();
        if (was) t.tool.cancel(&t.host);
        result = {{"cancelled", was}};
        return true;
    }
    if (cmd == "hover") {
        viewFromArgs(t, args);
        const float x = static_cast<float>(optNumber(args, "x", 0.0)), y = static_cast<float>(optNumber(args, "y", 0.0));
        rebuildOverlay(t, x, y, optBool(args, "lines", true) && !t.tool.stroking());
        result = overlayJson(t, optBool(args, "overlay", true));
        result["view"] = viewJson(t);
        return true;
    }
    if (cmd == "hover_clear") {
        clearOverlay(t);
        result = {{"overlay", {{"version", t.overlayVersion}}}};
        return true;
    }
    if (cmd == "select") {
        viewFromArgs(t, args);
        const SelectMode mode = [&] {
            try {
                return selectModeFromName(optString(args, "mode", "replace"));
            } catch (const std::invalid_argument& e) {
                fail("bad_request", e.what());
            }
        }();
        const HierarchyPtr h = s.groom->hierarchy();
        std::vector<GuideId> picked;
        if (h) {
            const BrushToolSettings& st = t.tool.settings;
            const std::vector<GuideId> candidates = guidesAt(*h, st.brush.level, st.brush.group, t.hidden);
            const std::vector<Pixel> lasso = pixelsOf(need(args, "lasso"), 1.0f);
            if (lasso.empty()) fail("bad_request", "lasso holds a point (a click), two (a box) or more");
            std::vector<std::array<float, 2>> polygon;
            if (lasso.size() == 2) {
                const Pixel& a = lasso[0];
                const Pixel& b = lasso[1];
                polygon = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}};
            } else {
                for (const Pixel& p : lasso) polygon.push_back({p.x, p.y});
            }
            // Only what the view shows: guide points behind the scalp are not picked.
            const ViewProjector visible = visibleProjector(pixelProjector(t.view.projection), scalpVisibility(s.groom->scalp(), t.view));
            picked = pickGuides(*h, candidates, visible, polygon, optBool(args, "rootsOnly", false),
                                static_cast<float>(optNumber(args, "clickPixels", 12.0)), &t.view.projection);
            // With symmetry (the tool's), each picked guide's mirror guide too.
            const MeshPtr scalp = s.groom->scalp();
            picked = withMirrorGuides(*h, picked, StrokeSymmetry::of(st.brush, scalp.get()));
        }
        pruneSelection(t);
        const std::vector<GuideId> selection = combineSelection(t.selection, picked, mode);
        setSelection(t, selection);
        // "solo" (the add-on's Select tool): every guide not selected frozen, one undo step when
        // that changed anything; a plain pick of nothing unfreezes all.
        bool froze = false;
        if (optBool(args, "solo", false) && (!picked.empty() || mode == SelectMode::Replace)) {
            const std::shared_ptr<const GuideHierarchy> stored = s.groom->authoredHierarchy();
            GuideHierarchy probe = stored ? *stored : GuideHierarchy{};
            if (freezeToSelection(probe, selection)) {
                editGuides(t, "solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
                froze = true;
            }
        }
        result = selectionJson(t);
        result["picked"] = picked;
        result["froze"] = froze;
        return true;
    }
    if (cmd == "selection_get") {
        pruneSelection(t);
        result = selectionJson(t);
        return true;
    }
    if (cmd == "selection_set") {
        std::vector<GuideId> ids;
        const std::shared_ptr<const GuideHierarchy> h = s.groom->storedHierarchy();
        if (args.contains("ids") && !args["ids"].is_null()) {
            ids = idsOf(args["ids"]);
        } else if (args.contains("group") && !args["group"].is_null()) {
            if (!h || h->findGroup(needString(args, "group")) == kNoGroup) fail("not_found", "no group named '" + needString(args, "group") + "'");
            ids = guidesAt(*h, -1, needString(args, "group"), t.hidden);
        } else if (args.contains("level") && !args["level"].is_null()) {
            if (h) ids = guidesAt(*h, static_cast<int>(optNumber(args, "level", 0.0)), {}, t.hidden);
        } else if (optBool(args, "all", false)) {
            if (h) ids = guidesAt(*h, -1, {}, t.hidden);
        }
        if (args.contains("softSelectRadius") && !args["softSelectRadius"].is_null()) {
            t.softRadius = std::max(0.0f, toGroomLength(s.space, optNumber(args, "softSelectRadius", 0.0)));
            ++t.selectionVersion;
        }
        SelectMode mode = SelectMode::Replace;
        try {
            mode = selectModeFromName(optString(args, "mode", "replace"));
        } catch (const std::invalid_argument& e) {
            fail("bad_request", e.what());
        }
        if (h) {
            ids.erase(std::remove_if(ids.begin(), ids.end(), [&](GuideId id) { return !h->guide(id); }), ids.end());
        } else {
            ids.clear();
        }
        setSelection(t, combineSelection(t.selection, ids, mode));
        // "solo" (a brush's Ctrl+Shift painting, when it ends): every guide not selected frozen,
        // one undo step when that changed anything.
        bool froze = false;
        if (optBool(args, "solo", false)) {
            const std::shared_ptr<const GuideHierarchy> stored = s.groom->authoredHierarchy();
            GuideHierarchy probe = stored ? *stored : GuideHierarchy{};
            const std::vector<GuideId> selection = t.selection;
            if (freezeToSelection(probe, selection)) {
                editGuides(t, "solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
                froze = true;
            }
        }
        result = selectionJson(t);
        result["froze"] = froze;
        return true;
    }
    if (cmd == "pick_level") {
        viewFromArgs(t, args);
        prepareHost(t);
        int level = -2;
        guarded(t, "pick", [&] {
            level = t.tool.pickLevel(t.host, t.view, static_cast<float>(optNumber(args, "x", 0.0)), static_cast<float>(optNumber(args, "y", 0.0)));
        });
        result = {{"level", level}, {"group", t.tool.settings.brush.group}, {"settings", settingsJson(t)}};
        return true;
    }
    if (cmd == "set_hidden_groups") {
        const json& groups = need(args, "groups");
        if (!groups.is_array()) fail("bad_request", "groups is a list of group names");
        std::set<std::string> hidden;
        for (const json& g : groups) hidden.insert(settingString(g, "groups"));
        t.hidden = std::move(hidden);
        result = {{"hiddenGroups", t.hidden}};
        return true;
    }
    if (cmd == "guides_info") {
        if (given(args, "guideColorSeed")) t.colorSeed = settingString(args["guideColorSeed"], "guideColorSeed");
        result = guidesInfo(t);
        return true;
    }
    if (cmd == "guides_edit") {
        result = guidesEdit(t, args);
        return true;
    }
    if (cmd == "maps_list") {
        json density;
        result = mapsList(t, &density);
        result["densityMap"] = density;
        return true;
    }
    if (cmd == "density_map") {
        // The Tube tool's density map (subdivisionDensityMapIn: the one map, else an older numbered
        // one; "" none yet), the name Subdivide makes it under and its value then.
        std::vector<std::string> names;
        for (const auto& [name, entry] : s.groom->scalpMaps().maps) names.push_back(name);
        result = {{"map", subdivisionDensityMapIn(names)}, {"name", kSubdivisionDensityMap}, {"start", kDefaultSubdivisionDensity}};
        return true;
    }
    if (cmd == "map_create") {
        result = mapCreate(t, args);
        return true;
    }
    if (cmd == "map_remove") {
        result = mapRemove(t, args);
        return true;
    }
    if (cmd == "map_for_param") {
        result = mapForParam(t, args);
        return true;
    }
    if (cmd == "undo" || cmd == "redo") {
        if (t.tool.stroking()) t.tool.cancel(&t.host);
        const std::string graphBefore = s.groom->data().graphJson;
        const MeshPtr scalpBefore = s.groom->rawScalp();
        const bool done = cmd == "undo" ? undo(t) : redo(t);
        result = undoJson(t);
        result["done"] = done;
        result["graphChanged"] = s.groom->data().graphJson != graphBefore;  // a setting's map linked or unlinked (map_for_param)
        result["scalpChanged"] = s.groom->rawScalp() != scalpBefore;  // a transfer undone or redone: the groom is on the other scalp
        return true;
    }
    if (cmd == "undo_state") {
        result = undoJson(t);
        return true;
    }
    if (cmd == "length_state") {
        prepareHost(t);
        result = lengthStateJson(t, s.space);
        return true;
    }
    if (cmd == "length_edit") {
        // One edit of the Length Keys tool's keys: "scale" (value), "length" (value, host units),
        // "change" (name), "select" (key), "deleteKey", "clear", "apply" (the guides set: one undo
        // step), "cancel".
        prepareHost(t);
        const std::string edit = optString(args, "edit", "");
        LengthKeyTool& lk = t.tool.lengths();
        const BrushToolSettings& st = t.tool.settings;
        const std::size_t before = t.host.edits().size();
        LengthChange change = lk.change;
        if (edit == "change") {
            try {
                change = lengthChangeFromName(optString(args, "name", "scale"));
            } catch (const std::invalid_argument& e) {
                fail("bad_request", e.what());
            }
        } else if (edit != "scale" && edit != "length" && edit != "select" && edit != "deleteKey" && edit != "clear" &&
                   edit != "apply" && edit != "cancel") {
            fail("bad_request", "length_edit takes scale, length, change, select, deleteKey, clear, apply or cancel");
        }
        guarded(t, "lengths", [&] {
            if (edit == "scale") lk.setKeyScale(t.host, st, static_cast<float>(optNumber(args, "value", 0.0)));
            else if (edit == "length") lk.setKeyLength(t.host, st, toGroomLength(s.space, optNumber(args, "value", 0.0)));
            else if (edit == "change") lk.setChange(t.host, st, change);
            else if (edit == "select") lk.select(t.host, static_cast<int>(optNumber(args, "key", 0)));
            else if (edit == "deleteKey") lk.deleteKey(t.host, st);
            else if (edit == "clear") lk.clear(t.host, st);
            else if (edit == "apply") lk.apply(t.host, st);
            else lk.cancel(t.host);
        });
        result = lengthStateJson(t, s.space);
        result["committed"] = t.host.edits().size() != before;
        if (result["committed"].get<bool>()) {
            t.redoLabels.clear();
            t.redoMaps.clear();
        }
        result["undo"] = undoJson(t);
        return true;
    }
    if (cmd == "root_state") {
        // The root tool's state, its overlay with it (after an undo the outlines follow).
        result = {{"roots", rootStateJson(t)}};
        if (t.tool.settings.tool == BrushToolSettings::Tool::Roots) {
            rebuildOverlay(t, t.pressX, t.pressY, false);
            result.update(overlayJson(t, optBool(args, "overlay", true)));
        }
        return true;
    }
    if (cmd == "root_edit") {
        // The Tube card's root tube buttons: "start" (mode: place, edit or select; the tool made
        // "roots"), "leave", "finish", "undoCv", "duplicate", "endDuplicate", "delete", "sync";
        // "settings" (drawPath, snap, length in host units, segments, taper, density, group) first.
        requireScalp(s);
        prepareHost(t);
        RootTubeTool& roots = t.tool.roots();
        if (args.contains("settings") && args["settings"].is_object()) {
            const json& j = args["settings"];
            RootTubeTool::Settings& rs = roots.settings;
            rs.drawPath = optBool(j, "drawPath", rs.drawPath);
            rs.snap = optBool(j, "snap", rs.snap);
            if (given(j, "length")) rs.length = std::max(1e-6f, toGroomLength(s.space, optNumber(j, "length", 1.0)));
            rs.segments = std::max(2, static_cast<int>(optNumber(j, "segments", rs.segments)));
            rs.taper = static_cast<float>(optNumber(j, "taper", rs.taper));
            rs.density = std::max(0.01f, static_cast<float>(optNumber(j, "density", rs.density)));
            rs.group = optString(j, "group", rs.group);
        }
        const std::string edit = optString(args, "edit", "");
        const std::size_t before = t.host.edits().size();
        if (!edit.empty()) {
            if (t.tool.stroking()) t.tool.cancel(&t.host);
            guarded(t, "roots", [&] {
                if (edit == "start") {
                    const std::string mode = optString(args, "mode", "place");
                    RootTubeTool::Mode m = RootTubeTool::Mode::Place;
                    if (mode == "edit") m = RootTubeTool::Mode::Edit;
                    else if (mode == "select") m = RootTubeTool::Mode::Select;
                    else if (mode != "place") throw std::invalid_argument("start takes mode place, edit or select");
                    t.selectTool = false;
                    t.tool.settings.setMode("roots");
                    roots.start(t.host, m);
                } else if (edit == "leave") {
                    roots.leave(t.host);
                } else if (edit == "finish") {
                    roots.finishEdit(t.host);
                } else if (edit == "undoCv") {
                    roots.undoCv(t.host);
                } else if (edit == "duplicate") {
                    roots.beginDuplicate(t.host);
                } else if (edit == "endDuplicate") {
                    roots.endDuplicate();
                } else if (edit == "delete") {
                    roots.deleteSelected(t.host);
                } else if (edit == "sync") {
                    roots.sync(t.host);
                } else {
                    throw std::invalid_argument("root_edit takes start, leave, finish, undoCv, duplicate, endDuplicate, delete or sync");
                }
            });
        }
        rebuildOverlay(t, t.pressX, t.pressY, false);
        result = overlayJson(t, optBool(args, "overlay", true));
        result["roots"] = rootStateJson(t);
        result["committed"] = t.host.edits().size() != before;
        if (result["committed"].get<bool>()) {
            t.redoLabels.clear();
            t.redoMaps.clear();
        }
        pruneSelection(t);
        result["undo"] = undoJson(t);
        return true;
    }
    if (cmd == "root_subdivide") {
        // The Tube roots block's Subdivide ("apply" false: the roots "guides", else those the root
        // tool has selected, cut into cells) and Finish painting ("apply" true: the cut roots cut
        // again where the density map changed, and "guides" not cut yet cut), root_subdivision.h;
        // one undo step. The session's scalp carries the painted maps.
        requireScalp(s);
        pruneSelection(t);
        std::vector<GuideId> guides;
        if (args.contains("guides") && !args["guides"].is_null()) {
            guides = idsOf(args["guides"]);
        } else {
            const auto& picked = t.tool.roots().selected();
            guides.assign(picked.begin(), picked.end());
        }
        const bool apply = optBool(args, "apply", false);
        if (!apply && guides.empty()) fail("state", "select a root first (Select root), then Subdivide");
        // The density map ("map", default the one map) and the multiplier; Apply ("apply") cuts every
        // cut by them again all over ("force", default true: the card's Apply).
        RootSubdivisionOptions options;
        // ("map" unset: the groom's density map, an older numbered one too: subdivisionDensityMapIn)
        options.map = optString(args, "map", "");
        if (options.map.empty()) {
            std::vector<std::string> names;
            for (const auto& [name, entry] : s.groom->scalpMaps().maps) names.push_back(name);
            options.map = subdivisionDensityMapIn(names);
            if (options.map.empty()) options.map = kSubdivisionDensityMap;
        }
        options.multiplier = static_cast<float>(optNumber(args, "multiplier", 1.0));
        options.force = apply && optBool(args, "force", true);
        const std::size_t before = t.host.edits().size();
        const MeshPtr scalp = s.groom->scalp();
        SubdivisionResult done;
        editGuides(t, apply ? "apply root density" : "subdivide root", [&](GuideHierarchy& g) {
            done = apply ? applySubdivisionDensity(g, *scalp, {}, options) : subdivideRootTubes(g, *scalp, guides, options);
        });
        result = {{"cells", done.cells}, {"removed", done.removed}, {"kept", done.kept}};
        if (done.cellLevel > 0) result["cellLevel"] = done.cellLevel;  // (the level the cells are in: the card shows it)
        result["committed"] = t.host.edits().size() != before;
        if (result["committed"].get<bool>()) {
            t.redoLabels.clear();
            t.redoMaps.clear();
        }
        result["undo"] = undoJson(t);
        return true;
    }
    if (cmd == "tube_levels" || cmd == "tube_level_add" || cmd == "tube_level_remove" || cmd == "tube_level_show") {
        // The Tube hierarchy block: the levels of the base group (the top group of the first of
        // "guides", else of "picked", else the first top group holding guides); Add splits the
        // tubes under "guides" (none: the base group's), Remove undoes a split (the selection's,
        // else the deepest level), each one undo step; Show gives each level's group and whether
        // it shows (that level alone: the host hides the others).
        const std::shared_ptr<const GuideHierarchy> h = s.groom->authoredHierarchy();
        if (!h && cmd == "tube_levels") {
            result = {{"base", ""}, {"levels", json::array()}, {"maxLevels", kMaxTubeLevels}};
            return true;
        }
        if (!h) fail("state", "the groom has no guides");
        pruneSelection(t);
        // (No "guides": the guides selected, as Maya's card gives its selection.)
        const std::vector<GuideId> guides = args.contains("guides") && !args["guides"].is_null() ? idsOf(args["guides"]) : t.selection;
        const std::string picked = optString(args, "picked", "");
        if (cmd == "tube_levels") {
            result = tubeLevelsJson(*h, guides, picked);
            return true;
        }
        if (cmd == "tube_level_show") {
            json shown = json::object();
            for (const auto& [group, visible] : tubeLevelVisibility(*h, needString(args, "base"), static_cast<int>(optNumber(args, "level", 1)))) {
                shown[group] = visible;
            }
            result = {{"shown", shown}};
            return true;
        }
        const bool adding = cmd == "tube_level_add";
        const std::size_t before = t.host.edits().size();
        std::size_t count = 0;
        const std::string base = adding ? tubeLevels(*h, guides, picked).base : needString(args, "base");
        const MeshPtr scalp = s.groom->scalp();
        editGuides(t, adding ? "tube level add" : "tube level remove", [&](GuideHierarchy& g) {
            count = adding ? addTubeLevel(g, scalp.get(), guides, picked) : removeTubeLevel(g, scalp.get(), base, guides);
        });
        const std::shared_ptr<const GuideHierarchy> after = s.groom->authoredHierarchy();
        result = after ? tubeLevelsJson(*after, {}, base) : json::object();
        result[adding ? "split" : "removed"] = count;
        result["committed"] = t.host.edits().size() != before;
        if (result["committed"].get<bool>()) {
            t.redoLabels.clear();
            t.redoMaps.clear();
        }
        result["undo"] = undoJson(t);
        return true;
    }
    if (cmd == "display") {
        result = display(t, args);
        return true;
    }
    return false;
}

}  // namespace

}  // namespace groomlab::helper
