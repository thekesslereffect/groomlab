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

#include "groomlab/editor/guide_edits.h"
#include "groomlab/editor/map_edits.h"
#include "groomlab/editor/tool_overlay.h"
#include "groomlab/editor/tool_select.h"
#include "groomlab/editor/tool_settings.h"
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
#include "groomlab/tube_braids.h"
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

using PaintTarget = FreezeKept;  // what Paint painted before Freeze took the brush (tool_settings.h)

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
    SeamCache seam;  // the line of symmetry while the scalp and symmetry stay the same (tool_overlay.h)
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
    return pickedTool(t.tool.settings);  // every host's (tool_settings.h)
}

// The rail's tools by the catalog's ids, as the app's BrushSession::setTool picks them: a brush
// mode, "freeze" (Paint on the guides' freeze mask toward 1; Ctrl: 0), or "select" (the lasso).
void setTool(ToolState& t, const std::string& id) {
    if (id == kSelectTool) {
        t.selectTool = true;
        return;
    }
    if (id == "width" || id == "density") fail("bad_request", std::string(kRemovedParamBrushes) + " (the helper: map_for_param)");
    try {
        pickTool(t.tool.settings, id, t.beforeFreeze);  // every host's (the editor's, tool_settings.h)
    } catch (const std::invalid_argument&) {
        fail("bad_request", "no tool '" + id + "': the catalog's ids (tool_catalog), select, freeze or clump");
    }
    t.selectTool = false;
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

// The host's lengths, axes and tube widths as the groom's (the editor's ToolSpace).
ToolSpace toolSpace(const HostSpace& space) {
    ToolSpace out;
    out.lengthIn = [&space](double v) { return toGroomLength(space, v); };
    out.lengthOut = [&space](float v) { return toHostLength(space, v); };
    out.symmetryIn = [&space](const std::string& axis, double center, int& groomAxis, float& groomCenter) {
        symmetryToGroom(space, axis, center, groomAxis, groomCenter);
    };
    out.symmetryOut = [&space](int groomAxis, float groomCenter, std::string& axis, double& center) {
        symmetryToHost(space, groomAxis, groomCenter, axis, center);
    };
    out.tubeIn = [&space](const json& tube) { return tubeEditToGroom(space, tube); };
    out.tubeOut = [&space](const std::string& tube) { return tubeToHost(space, tube); };
    return out;
}

// The settings the palette sends (Maya's groomLabBrushCtx flags by their long names, and the
// catalog's setting ids): lengths in host units, the symmetry in the host's axes. Every host's
// parser (the editor's applyToolSettings, tool_settings.h); softSelectRadius is the helper's own.
void applySettings(ToolState& t, const json& settings) {
    if (!settings.is_object()) fail("bad_request", "settings is an object");
    json rest = settings;
    float softRadius = t.softRadius;
    if (rest.contains("softSelectRadius")) {
        softRadius = std::max(0.0f, toGroomLength(t.session.space, settingNumber(rest["softSelectRadius"], "softSelectRadius")));
        rest.erase("softSelectRadius");
    }
    try {
        applyToolSettings(t.tool, rest, toolSpace(t.session.space));  // (Clone's Pick Source and Reset too)
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    t.softRadius = softRadius;
}

json settingsJson(const ToolState& t) {
    json j = toolSettingsJson(t.tool, toolSpace(t.session.space));  // (Clone's source too)
    j["softSelectRadius"] = toHostLength(t.session.space, t.softRadius);
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
            sculpt = edit.hasNode(node) && (edit.nodeType(node) == "SculptGuides" || edit.nodeType(node) == "SculptStrands");
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
    if (!t.selectTool) {
        // Every host's overlay (the editor's brushOverlay, tool_overlay.h): the guides the brush
        // would touch, the line of symmetry, the ring and a Part or Flow stroke's path.
        prepareHost(t);
        OverlayRequest request;
        request.x = x;
        request.y = y;
        request.withLines = withLines;
        request.offScalp = [&t](float px, float py, Vec3f& at) { return atTargetDepth(t, px, py, at); };
        const BrushOverlay drawn = brushOverlay(t.tool, &t.host, t.view, request, t.seam);
        t.overlay = drawn.lines;
        t.ringValid = drawn.ring;
        t.ringOnScalp = drawn.ringOnScalp;
        t.ringCenter = drawn.ringCenter;
        t.ringRadius = drawn.ringRadius;
        t.ringNormal = drawn.ringNormal;
    }
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
        // The tube hierarchies' levels (a top group with child groups): each level's group and its
        // level (1: the base), a row's tube in Guide groups (its tubes kept in view).
        std::map<std::string, int> tubeLevel;
        std::set<GroupId> parents;
        for (const GuideGroup& g : h->groups()) parents.insert(g.parent);
        for (const GuideGroup& g : h->groups()) {
            if (g.parent != kNoGroup || !parents.count(g.id)) continue;
            const TubeLevels info = tubeLevels(*h, {}, g.name);
            if (info.base != g.name || info.levels.size() < 2) continue;
            for (std::size_t k = 0; k < info.levels.size(); ++k) tubeLevel[info.levels[k]] = static_cast<int>(k + 1);
        }
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
                              {"segments", g.segments},
                              {"segmentLength", g.segmentLength},
                              {"tubeLevel", tubeLevel.count(g.name) ? tubeLevel.at(g.name) : 0}});
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
    json r;
    // The ops of the editor's applyGuideOp (guide_edits.h), every host's: its checks and edit; the
    // helper reads the request and keeps the step on its undo stack.
    static const std::set<std::string> kGuideOps = {
        "add_group", "remove_group", "delete_guides", "lock", "unlock", "set_attribute", "freeze", "unfreeze", "unfreeze_all",
        "invert_frozen", "remove_layer", "mirror_all", "mirror_group", "mirror_shapes", "move_to_group", "duplicate_to_group",
        "new_group_from", "paste_guides", "promote_group", "demote_group", "reparent", "resample", "set_group_segments",
        "reset_group_segments"};
    // A Sculpt or SculptStrands node targeted: Unfreeze All and Invert Frozen are its own (its frozen
    // guides or strands, kept in the node: stroke.h sculptFreezeEdit, one undo step).
    if (op == "unfreeze_all" || op == "invert_frozen") {
        if (sculptFreezeEdit(t.host, op == "invert_frozen" ? "invert" : "unfreeze_all")) {
            r["sculptNode"] = t.host.sculptNode();
            return r;
        }
    }
    if (kGuideOps.count(op)) {
        GuideOp g;
        g.op = op == "invert_frozen" ? std::string("invert_layer") : op;
        if (op == "invert_frozen") g.name = kFreezeTool;
        g.group = optString(args, "group", "");
        if (op == "add_group") g.parent = optString(args, "parent", "");  // (reparent's is a guide ID)
        if (op != "invert_frozen") g.name = optString(args, "name", "");
        // The guides: `ids`, else the selection (resample: `group` or `level` instead when given).
        pruneSelection(t);
        if (given(args, "ids")) {
            g.guides = idsOf(args["ids"]);
        } else if (op != "resample" || !(given(args, "group") || given(args, "level"))) {
            g.guides = t.selection;
        }
        if (op == "set_attribute") g.value = static_cast<float>(optNumber(args, "value", 1.0));
        if (op == "paste_guides") g.text = needString(args, "text");
        if (op == "demote_group") g.parent = needString(args, "parent");
        if (op == "reparent") {
            const json& parent = need(args, "parent");
            if (!parent.is_number_unsigned()) fail("bad_request", "parent is a guide ID");
            g.parentGuide = parent.get<GuideId>();
            g.parent.clear();
        }
        if (op == "mirror_all" || op == "mirror_group") {
            symmetryToGroom(space, optString(args, "axis", "x"), optNumber(args, "center", 0.0), g.axis, g.center);
            if (g.axis < 0) fail("bad_request", "axis is x, y or z");
            g.fromPositive = optBool(args, "fromPositive", true);
        }
        if (op == "mirror_shapes") {
            // Maya's groomLabGuides -mirrorShapes (issue 74): "selected" (the selected or `ids` guides'
            // mirror guides take their mirror images), "positive" or "negative" (that whole side over
            // the other; `group`: one root group's), across `symmetry` (x, y, z at `center`, or
            // topology), as the brushes take it.
            const std::string from = optString(args, "from", "selected");
            if (from != "selected" && from != "positive" && from != "negative") fail("bad_request", "from is selected, positive or negative");
            g.side = from == "positive" ? 1 : from == "negative" ? -1 : 0;
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
            g.symmetry = StrokeSymmetry::of(b, scalp.get());
        }
        if (op == "resample") {
            // Maya's groomLabGuides -resample: each guide `segments` segments evenly along the smooth
            // curve through its points (as the viewport draws it), its shape kept; frozen guides kept.
            g.segments = settingInt(need(args, "segments"), "segments");
            g.level = given(args, "level") ? settingInt(args["level"], "level") : -1;
            g.byLength = given(args, "byLength") && settingBool(args["byLength"], "byLength");  // issue #98
            t.session.groom->setPreviewHierarchy(nullptr);  // a live preview (guides_preview) ends with its commit
        }
        if (op == "set_group_segments") g.segments = settingInt(need(args, "segments"), "segments");
        GuideOpResult done;
        editGuides(t, guideOpLabel(g), [&](GuideHierarchy& h) {
            try {
                done = applyGuideOp(h, g);
            } catch (const GuideEditError& e) {
                fail(e.code, e.what());
            }
        });
        auto ids = [](const std::vector<GuideId>& v) {
            json a = json::array();
            for (const GuideId id : v) a.push_back(id);
            return a;
        };
        if (op == "add_group" || op == "new_group_from") r["group"] = done.group;
        if (op == "delete_guides") r["removed"] = done.count;
        if (op == "mirror_all" || op == "mirror_group") r["added"] = done.made.size();
        if (op == "mirror_shapes") {
            r["reshaped"] = done.mirrored.reshaped.size();
            r["added"] = done.mirrored.created.size();
            r["removed"] = done.mirrored.removed.size();
        }
        if (op == "move_to_group" || op == "duplicate_to_group" || op == "paste_guides") r["made"] = ids(done.made);
        if (op == "resample") r["resampled"] = done.count;
        if (op == "reset_group_segments") r["segments"] = done.segments;
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
        // Every host's Generate Children (the editor's, guide_edits.h): with `selected`, the
        // selected guides (all of one group, `group` when given), else every guide of `group` (the
        // last group by default).
        ChildrenRequest request;
        request.group = args.contains("group") && !args["group"].is_null() ? needString(args, "group") : std::string();
        request.childGroup = optString(args, "childGroup", "");
        ChildGenerationParams& p = request.params;
        p.count = static_cast<std::size_t>(std::clamp(optNumber(args, "count", 5.0), 1.0, static_cast<double>(kMaxChildrenPerGuide)));
        p.rootSpread = static_cast<float>(optNumber(args, "rootSpread", p.rootSpread));
        p.tipSpread = static_cast<float>(optNumber(args, "tipSpread", p.tipSpread));
        p.twist = static_cast<float>(optNumber(args, "twist", p.twist));
        p.jitter = static_cast<float>(optNumber(args, "jitter", p.jitter));
        p.childTubeScale = static_cast<float>(optNumber(args, "childTubeScale", p.childTubeScale));
        p.seed = static_cast<std::uint64_t>(std::max(0.0, optNumber(args, "seed", 0.0)));
        if (optBool(args, "selected", false)) {
            pruneSelection(t);
            request.parents = t.selection;
            if (request.parents.empty()) fail("state", "generate children: no guides selected");
        }
        ChildrenMade made;
        editGuides(t, "generate children", [&](GuideHierarchy& h) {
            try {
                made = groomlab::generateChildren(h, request);
            } catch (const std::invalid_argument& e) {
                fail("state", e.what());
            }
        });
        r["group"] = made.childGroup;
        r["added"] = made.added;
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
    } else {
        fail("bad_request",
             "op is add_group, remove_group, rename_group, generate_children, delete_guides, lock, unlock, set_attribute, "
             "freeze, unfreeze, unfreeze_all, invert_frozen, remove_layer, mirror_all, mirror_group, mirror_shapes, promote_group, "
             "demote_group, reparent, move_to_group, duplicate_to_group, new_group_from, paste_guides, "
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
    const bool byLength = given(args, "byLength") && settingBool(args["byLength"], "byLength");
    previewGuides(t, [&](GuideHierarchy& h) {
        const std::vector<GuideId> ids = targetGuides(t, h, args, true, "preview");
        const float length = byLength ? h.segmentLengthFor(ids, segments) : 0.0f;  // (as resample)
        count = length > 0.0f ? h.resampleByLength(ids, length, resampleGroupWide(t, args))
                              : h.resample(ids, segments, resampleGroupWide(t, args));
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
json tubeEdit(ToolState& t, const json& args, const std::string& label, const json& edit, bool allWhenNone = false) {
    const std::string text = tubeEditToGroom(t.session.space, edit);
    std::size_t changed = 0;
    std::string note;  // (root tubes that kept their drawn root: editTubes says how many)
    editGuides(t, label, [&](GuideHierarchy& h) {
        try {  // every host's (the editor's editTubes, guide_edits.h)
            changed = editTubes(h, targetGuides(t, h, args, allWhenNone, label), text, &note);
        } catch (const GuideEditError& e) {
            fail(e.code, e.what());
        }
    });
    return {{"changed", changed}, {"note", note}, {"undo", undoJson(t)}};
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

// The Tube tool's Fill (issue #135): one channel ("width" in host units, "flatness", "twist"
// radians) set on each target's tube root to tip (tubeFillEdit, every host's): one undo step.
// Targets: "ids", a "group" or a "level", else the selection, else every guide.
json tubeFill(ToolState& t, const json& args) {
    const std::string channel = needString(args, "channel");
    float value = settingNumber(need(args, "value"), "value");
    if (channel == "width") value = static_cast<float>(toGroomLength(t.session.space, value));
    std::string edit;
    try {
        edit = tubeFillEdit(channel, value);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    return tubeEdit(t, args, "fill tube " + channel, json::parse(edit), true);
}

// Make Braid / Unbraid (issue 129, tube_braids.h): the tubes "guides" (none: the selection, else
// the root tool's selected roots) marked as braids ("on", default true) or not: one undo step.
// The graph's tube Braid is the add-on's edit_graph insert_tube_braid.
json tubeBraid(ToolState& t, const json& args) {
    pruneSelection(t);
    std::vector<GuideId> guides;
    if (args.contains("guides") && !args["guides"].is_null()) {
        guides = idsOf(args["guides"]);
    } else if (!t.selection.empty()) {
        guides.assign(t.selection.begin(), t.selection.end());
    } else {
        const auto& picked = t.tool.roots().selected();
        guides.assign(picked.begin(), picked.end());
    }
    if (guides.empty()) fail("state", "select tubes first (any level; or a root with Select root)");
    const bool on = optBool(args, "on", true);
    const std::size_t before = t.host.edits().size();
    std::size_t changed = 0;
    editGuides(t, on ? "make braid" : "unbraid", [&](GuideHierarchy& h) { changed = h.setBraid(guides, on); });
    const bool committed = t.host.edits().size() != before;
    if (committed) {
        t.redoLabels.clear();
        t.redoMaps.clear();
    }
    return {{"changed", changed}, {"committed", committed}, {"undo", undoJson(t)}};
}

// Each guide's braid tube (braidTubeOf: the outermost marked tube of its lineage), 0: none.
json braidOf(ToolState& t, const json& args) {
    const HierarchyPtr h = t.session.groom->authoredHierarchy();
    json out = json::array();
    for (const GuideId g : idsOf(need(args, "guides"))) out.push_back(h ? braidTubeOf(*h, g) : GuideId{0});
    return {{"braids", out}};
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
            {"rootTubes", targets.rootTubes},    // (their Root profile is their drawn root)
            {"splitTubes", targets.splitTubes},  // (a Mid or Tip profile does not carry into the split levels)
            {"tube", targets.guides.empty() ? json(nullptr)
                                            : tubeToHost(t.session.space, tubeToJson(h->guide(targets.guides.front())->tube, h->scale()))},
            {"card", targets.guides.empty() ? json(nullptr) : tubeCardJson(t.session.space, h->guide(targets.guides.front())->tube, h->scale())}};
}

json sectionPointsJson(const std::vector<SectionPoint>& points) {
    json out = json::array();
    for (const SectionPoint& p : points) out.push_back({p.u, p.v});
    return out;
}

// The shapes guides can take: the built-ins, then the groom's library (saved with its guides), its
// owned shapes (the root tubes' drawn roots) marked "owned".
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
                           // A root tube's drawn root (isOwnedTubeShape): listed for its outline, in no picker.
                           {"owned", isOwnedTubeShape(name)},
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
    TubeShapeOp edit;  // every host's (the editor's editTubeShape, guide_edits.h)
    edit.op = op;
    edit.name = needString(args, "name");
    if (op == "rename") edit.to = needString(args, "to");
    if (given(args, "smooth")) edit.smooth = settingBool(args["smooth"], "smooth");
    if (given(args, "points")) edit.points = shapePointsOf(args["points"]);
    std::size_t users = 0;
    t.session.groom->setPreviewHierarchy(nullptr);
    editGuides(t, tubeShapeLabel(edit), [&](GuideHierarchy& h) {
        try {
            users = editTubeShape(h, edit);
        } catch (const GuideEditError& e) {
            fail(e.code, e.what());
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

// Mirror to Other Side for parts or flow (issue 105; Maya's groomLabParts -mirror [-flow]): `from`
// ("positive" or "negative") copied over the other side, across `symmetry` (x, y, z at `center`, or
// topology) as the brushes take it, on the cage the parts and flow are stored on.
json overlayMirror(ToolState& t, const json& args, bool flow) {
    Session& s = t.session;
    const MeshPtr cage = s.groom->cageScalp();
    if (!cage || cage->triangleCount() == 0) fail("state", "the session has no scalp: set_scalp first");
    const std::string from = optString(args, "from", "positive");
    if (from != "positive" && from != "negative") fail("bad_request", "from is positive or negative");
    const std::string symmetry = optString(args, "symmetry", "x");
    BrushSettings b;
    if (symmetry == "topology") {
        b.mirrorTopology = true;
    } else {
        try {
            symmetryToGroom(s.space, symmetry, optNumber(args, "center", 0.0), b.mirrorAxis, b.mirrorCenter);
        } catch (const std::invalid_argument&) {
            b.mirrorAxis = -1;
        }
        if (b.mirrorAxis < 0) fail("bad_request", "symmetry is x, y, z or topology");
    }
    const StrokeSymmetry sym = StrokeSymmetry::of(b, cage.get());
    const int side = from == "positive" ? 1 : -1;
    OverlayMirrorResult done;
    StrokeEdit edit;
    if (flow) {
        FlowSet set;
        try {
            set = FlowSet::fromJson(s.groom->data().flowJson);
        } catch (const std::exception& e) {
            fail("state", std::string("the groom's flow does not read: ") + e.what());
        }
        done = mirrorFlowToOtherSide(set, *cage, sym, side);
        edit.mode = "mirror flow";
        edit.set(StrokeField::Flow, set.empty() ? std::string() : set.toJson());
    } else {
        PartSet set = storedParts(s);
        done = mirrorPartsToOtherSide(set, *cage, sym, side);
        edit.mode = "mirror parts";
        edit.set(StrokeField::Parts, set.empty() ? std::string() : set.toJson());
    }
    if (done.copied + done.removed > 0) commitEdit(t, std::move(edit));
    return {{"copied", done.copied}, {"removed", done.removed}, {"undo", undoJson(t)}};
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
ScalpMapEntry newScalpMapEntry(ToolState& t, const ScalpMesh& scalp, const std::string& name, const std::string& kind, bool color,
                               float value, const PaintColor* fill, int resolution, const std::vector<std::int32_t>& faces = {});
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
    std::optional<PaintColor> fill;
    if (args.contains("fill") && !args["fill"].is_null()) {
        const json& f = args["fill"];
        if (!f.is_array() || (f.size() != 3 && f.size() != 4)) fail("bad_request", "fill is [r, g, b] or [r, g, b, a]");
        fill = PaintColor{0.0f, 0.0f, 0.0f, 1.0f};
        for (std::size_t i = 0; i < f.size(); ++i) (*fill)[i] = std::clamp(settingNumber(f[i], "fill"), 0.0f, 1.0f);
    }
    if (!validMapName(name)) fail("bad_request", mapNameError(name));
    if (!mapKindOf(s, name).empty()) fail("bad_request", "the groom already has a map named '" + name + "'");
    const MeshPtr scalp = s.groom->cageScalp();
    if (!scalp) fail("state", "no scalp to paint on: set_scalp first");
    const std::vector<std::int32_t> faces = facesArg(args);
    const int resolution = static_cast<int>(optNumber(args, "resolution", 1024.0));
    if (kind == "guides") {
        // On faces: the value on the guides rooted on them, 0 on the rest (every host's: addGuideLayer).
        MapCreate made;
        MapCreateRequest create;
        create.kind = kind;
        create.color = color;
        create.value = value;
        create.faces = faces;
        try {
            made = groomlab::createMap(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), scalp.get(), name, create, MapFiles{});
        } catch (const std::invalid_argument& e) {
            fail("bad_request", e.what());
        }
        editGuides(t, "create map " + name, [&](GuideHierarchy& h) { applyCreatedLayer(h, name, made); });
        return {{"name", name}, {"kind", kind}, {"undo", undoJson(t)}};
    }
    ScalpMaps maps = s.groom->scalpMaps();
    maps.maps[name] = newScalpMapEntry(t, *scalp, name, kind, color, value, fill ? &*fill : nullptr, resolution, faces);
    commitMaps(t, "create map " + name, maps);
    json r = {{"name", name}, {"kind", kind}, {"undo", undoJson(t)}};
    if (kind == "texture") r["file"] = maps.maps[name].texture;
    return r;
}

ScalpMapEntry newScalpMapEntry(ToolState& t, const ScalpMesh& scalp, const std::string& name, const std::string& kind, bool color,
                               float value, const PaintColor* fill, int resolution, const std::vector<std::int32_t>& faces) {
    // Every host's New Map (the editor's createMap, map_edits.h); a texture's image written in the
    // maps folder (a free name), its files part of the next commit's undo step.
    Session& s = t.session;
    if (kind == "texture" && !scalp.hasUVs()) fail("state", "the scalp has no UVs to paint a texture map on");
    MapCreateRequest create;
    create.kind = kind;
    create.color = color;
    create.value = value;
    if (fill) create.fill = *fill;
    create.resolution = resolution;
    create.faces = faces;
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    ScalpMaps maps = s.groom->scalpMaps();
    maps.maps.erase(name);  // (a caller replacing an entry of that name)
    MapCreate made;
    try {
        made = groomlab::createMap(maps, nullptr, &scalp, name, create, files);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    for (FileEdit& f : made.files) t.pending.files.push_back(std::move(f));
    return made.maps.maps.at(name);
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
    // Every host's Paint on a setting (the editor's paintParam, map_edits.h): the map the param reads,
    // else its own (one there already linked again, its painting back), made white (with `faces`:
    // on them only) and linked, or a channel of a color map through its Map node; one undo step.
    ParamPaintRequest paint;
    paint.kind = kind;
    paint.name = optString(args, "map", "");
    paint.channel = optString(args, "channel", "");
    paint.faces = facesArg(args);
    paint.resolution = static_cast<int>(optNumber(args, "resolution", 1024.0));
    // New Map for a setting: through a Map node wired into the param; else listed in the param.
    paint.mapNode = optBool(args, "mapNode", false);
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    ParamPaint painted;
    try {
        painted = groomlab::paintParam(graphBefore, s.groom->scalpMaps(), s.groom->storedHierarchy().get(), scalp.get(), node, param, paint, files);
    } catch (const std::invalid_argument& e) {
        fail(std::string(e.what()).find("no guides to paint on") != std::string::npos ? "state" : "bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    const std::string map = painted.map, mapKind = painted.kind;
    const bool reused = !painted.made && !painted.graph.empty();
    if (!painted.graph.empty()) {
        StrokeEdit edit;
        edit.mode = "map " + node + "." + param;
        if (painted.created.guideLayer) {
            GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { applyCreatedLayer(hh, map, painted.created); });
            edit.set(StrokeField::Guides, h.toJson());
        } else if (painted.made) {
            edit.set(StrokeField::ScalpMaps, painted.created.maps.toJson());
        }
        edit.set(StrokeField::Graph, painted.graph);
        for (FileEdit& f : painted.created.files) t.pending.files.push_back(std::move(f));
        t.host.commit(std::move(edit));
    }
    // The Paint brush on it, toward 1: strokes paint the param back to its value, Ctrl toward 0.
    BrushToolSettings& st = t.tool.settings;
    st.setMode("paint");
    st.brush.attribute = map;
    st.paintKind = mapKind;
    const auto entry = s.groom->scalpMaps().maps.find(map);
    st.paintTexture = mapKind == "texture" && entry != s.groom->scalpMaps().maps.end() ? entry->second.texture : std::string();
    st.brush.value = 1.0f;
    if (!painted.channel.empty()) {  // one channel of a color map: white in it only
        st.paintColor = PaintColor{1.0f, 1.0f, 1.0f, 1.0f};
        st.paintChannels = channelMask(painted.channel);
    }
    const bool changed = s.groom->data().graphJson != graphBefore;
    json r = {{"name", map}, {"kind", mapKind}, {"made", changed && !reused}, {"reused", reused}, {"graphChanged", changed},
              {"tool", toolJson(t)}, {"undo", undoJson(t)}};
    if (!painted.channel.empty()) r["channel"] = painted.channel;
    return r;
}

// ---- Map management (Maya's painting.py over groomLabScalpMaps) ---------------------------------------

// "vertices", "texture", "guides", or "" for no map of that name.
std::string mapKindOf(Session& s, const std::string& name) {
    const auto h = s.groom->storedHierarchy();  // (the layers as stored)
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

// Maya's remove_map: what reads the map lets go of it; a texture map's file stays on disk unless
// `deleteFile` and GroomLab made it (it is in the maps folder) and no other map reads it. One undo
// step (undo puts the file back too).
json mapRemove(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    // Every host's remove (the editor's removeMap, map_edits.h).
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    MapRemove removed;
    try {
        removed = removeMap(s.groom->scalpMaps(), s.groom->storedHierarchy().get(), s.groom->data().graphJson, name,
                            optBool(args, "deleteFile", false), files);
    } catch (const std::invalid_argument& e) {
        fail("not_found", e.what());
    } catch (const std::exception& e) {
        fail("graph", std::string("cannot unlink the map's readers: ") + e.what());
    }
    StrokeEdit edit;
    edit.mode = "remove map " + name;
    if (!removed.graph.empty()) edit.set(StrokeField::Graph, removed.graph);
    if (removed.guideLayer) {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.removeAttribute(name); });
        edit.set(StrokeField::Guides, h.toJson());
    } else {
        dropPaints(t);
        edit.set(StrokeField::ScalpMaps, removed.maps.toJson());
    }
    for (FileEdit& f : removed.files) t.pending.files.push_back(std::move(f));
    commitEdit(t, std::move(edit));
    json changed = json::array();
    for (const auto& [node, param] : removed.unlinked) changed.push_back({node, param});
    return {{"removed", name}, {"unlinked", changed}, {"graphChanged", !removed.graph.empty()}, {"fileDeleted", removed.fileDeleted},
            {"undo", undoJson(t)}};
}

// Maya's rename_map: the map renamed, the Map nodes and map params reading it follow
// (retargetMapReaders, compounds too), and a texture map's file follows when it is named after the
// map (renamedMapTexture) and GroomLab made it (in the maps folder; a free name when one is in the
// way). One undo step.
json mapRename(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string from = needMap(s, args);
    const std::string to = needString(args, "to");
    // Every host's rename (the editor's renameMap, map_edits.h): the readers retargeted, a texture
    // map's image following the name (moved, or copied when another map reads it; all or nothing),
    // its files' states kept for the step's undo.
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    MapRename renamed;
    try {
        renamed = renameMap(s.groom->scalpMaps(), s.groom->storedHierarchy().get(), s.groom->data().graphJson, from, to, files);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    if (from == to) return {{"name", to}, {"renamed", from}, {"graphChanged", false}, {"undo", undoJson(t)}};
    StrokeEdit edit;
    edit.mode = "rename map " + from;
    if (renamed.guideLayer) {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.renameAttribute(from, to); });
        edit.set(StrokeField::Guides, h.toJson());
    } else {
        dropPaints(t);
        edit.set(StrokeField::ScalpMaps, renamed.maps.toJson());
    }
    for (FileEdit& f : renamed.files) t.pending.files.push_back(std::move(f));
    if (!renamed.graph.empty()) edit.set(StrokeField::Graph, renamed.graph);
    commitEdit(t, std::move(edit));
    // The Paint brush on it follows.
    BrushToolSettings& st = t.tool.settings;
    if (st.brush.attribute == from) {
        st.brush.attribute = to;
        if (!renamed.file.empty()) st.paintTexture = renamed.file;
    }
    json r = {{"name", to}, {"renamed", from}, {"graphChanged", !renamed.graph.empty()}, {"undo", undoJson(t)}};
    if (!renamed.file.empty()) r["file"] = renamed.file;
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
    if (mapKindOf(s, name) == to) return {{"name", name}, {"kind", to}, {"undo", undoJson(t)}};
    const MeshPtr scalp = mapScalp(s);
    // Every host's Convert (the editor's convertMapKind, map_edits.h).
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    MapConvert converted;
    try {
        converted = convertMapKind(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), *scalp, name, to, resolutionOf(args),
                                   optString(args, "texture", ""), files);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    json warnings = json::array();
    if (converted.colorLost) warnings.push_back(name + " is a color map; on the guides it keeps its luminance (a guide layer is gray)");
    StrokeEdit edit;
    edit.mode = "convert map " + name;
    if (converted.removeLayer || converted.setLayer) {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { applyConvertedLayer(hh, name, converted); });
        edit.set(StrokeField::Guides, h.toJson());
    }
    for (FileEdit& f : converted.files) t.pending.files.push_back(std::move(f));
    dropPaints(t);
    edit.set(StrokeField::ScalpMaps, converted.maps.toJson());
    commitEdit(t, std::move(edit));
    json r = {{"name", name}, {"kind", to}, {"warnings", warnings}, {"undo", undoJson(t)}};
    if (!converted.file.empty()) r["file"] = converted.file;
    return r;
}

// Maya's set_map_channels: gray (1: its luminance) or color (4: the value in r g b, alpha 1). A guide
// layer stays gray. One undo step (none when it already has them).
json mapSetChannels(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const json& c = need(args, "channels");
    if (!c.is_number_integer() || (c.get<int>() != kGrayMap && c.get<int>() != kColorMap)) fail("bad_request", "channels is 1 (gray) or 4 (color)");
    // Every host's Channels (the editor's setMapChannels, map_edits.h).
    const MeshPtr cage = mapKindOf(s, name) == "texture" ? mapScalp(s) : MeshPtr();
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    MapChannels changed;
    try {
        changed = groomlab::setMapChannels(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), cage.get(), name, c.get<int>(),
                                           resolutionOf(args), files);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    if (changed.mapsChanged) {
        if (!changed.files.empty()) dropPaints(t);
        for (FileEdit& f : changed.files) t.pending.files.push_back(std::move(f));
        StrokeEdit edit;
        edit.mode = "map channels " + name;
        edit.set(StrokeField::ScalpMaps, changed.maps.toJson());
        commitEdit(t, std::move(edit));
        if (!changed.file.empty() && t.tool.settings.brush.attribute == name) t.tool.settings.paintTexture = changed.file;
    }
    json r = {{"channels", changed.channels}, {"undo", undoJson(t)}};
    if (!changed.file.empty()) r["file"] = changed.file;
    return r;
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
    const bool selected = optBool(args, "selected", false);
    PaintMode paintMode = PaintMode::Replace;
    try {
        paintMode = paintModeFromString(optString(args, "paintMode", "replace"));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", std::string("paintMode: ") + e.what());
    }
    // Every host's Fill (the editor's fillMap, map_edits.h): its checks, the fill, a texture map's
    // image rewritten (or its own copy when another map reads it), the files kept for the step's undo.
    MapFillRequest fill;
    fill.value = value;
    if (byColor) fill.color = color;
    fill.mask = mask;
    fill.mode = paintMode;
    fill.selectedOnly = selected;
    // `faces`: only those polygons of the scalp (a guide layer: the guides rooted on them).
    fill.faces = facesArg(args);
    fill.resolution = resolutionOf(args);
    const std::string kind = mapKindOf(s, name);
    const HierarchyPtr authored = s.groom->authoredHierarchy();
    if (selected && kind == "guides" && fill.faces.empty() && !byColor) {
        fill.selected = targetGuides(t, authored ? *authored : GuideHierarchy{}, args, false, "fill selected");
    }
    const MeshPtr cage = kind != "guides" || !fill.faces.empty() ? mapScalp(s) : MeshPtr();
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    MapFill filled;
    try {
        filled = groomlab::fillMap(s.groom->scalpMaps(), authored.get(), cage.get(), name, fill, files);
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    const std::string label = "fill map " + name;
    StrokeEdit edit;
    edit.mode = label;
    if (filled.kind == "guides") {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { applyFilledLayer(hh, name, filled); });
        edit.set(StrokeField::Guides, h.toJson());
    } else {
        if (filled.mapsChanged) edit.set(StrokeField::ScalpMaps, filled.maps.toJson());
        if (filled.kind == "texture") dropPaints(t);
    }
    for (FileEdit& f : filled.files) t.pending.files.push_back(std::move(f));
    commitEdit(t, std::move(edit));
    if (!filled.file.empty() && t.tool.settings.brush.attribute == name) t.tool.settings.paintTexture = filled.file;
    json r = {{"name", name}, {"undo", undoJson(t)}};
    if (!filled.file.empty()) r["file"] = filled.file;
    return r;
}

// The image `args` "path" names, as every host links it (the editor's MapImageRequest): resolved
// against the groom's folders; `copy` (none: another format only) into the maps folder as a PNG.
MapImageRequest mapImageArg(Session& s, const json& args) {
    MapImageRequest image;
    const std::string path = needString(args, "path");
    if (path.empty()) fail("bad_request", "give the image's path");
    image.path = resolveMapFile(s, path);
    if (image.path.empty()) image.path = path;
    if (fileStamp(image.path) == 0) fail("not_found", "no image at " + image.path);  // (a tile set: any tile)
    if (given(args, "copy")) image.copy = settingBool(args["copy"], "copy");
    return image;
}

MapFiles helperMapFiles(Session& s) {
    MapFiles files;
    files.mapsFolder = mapsFolder(s);
    files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
    return files;
}

// Maya's import_map: a new texture map reading the image `path`, named `name` or after the file
// (its letters, digits and _; a free name), color when the image has color unless `color` says;
// a PNG in place, another format (or `copy`) copied as a PNG into the maps folder (the editor's
// importMap, every host's). One undo step.
json mapImport(ToolState& t, const json& args) {
    Session& s = t.session;
    const MapImageRequest image = mapImageArg(s, args);
    std::optional<bool> color;
    if (given(args, "color")) color = settingBool(args["color"], "color");
    MapLink linked;
    try {
        linked = groomlab::importMap(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), optString(args, "name", ""), image, color,
                                     helperMapFiles(s));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    for (FileEdit& f : linked.files) t.pending.files.push_back(std::move(f));
    commitMaps(t, "import map " + linked.name, linked.maps);
    pruneSelection(t);
    return {{"name", linked.name}, {"kind", "texture"}, {"channels", linked.channels}, {"file", linked.file}, {"undo", undoJson(t)}};
}

// Maya's set_map_texture: map `name` (any kind) reads the image `path` as a texture map, keeping
// its channels (a guide layer: gray); the image's rules as map_import's (the editor's relinkMap).
// One undo step.
json mapRelink(ToolState& t, const json& args) {
    Session& s = t.session;
    const std::string name = needMap(s, args);
    const MapImageRequest image = mapImageArg(s, args);
    MapLink linked;
    try {
        linked = groomlab::relinkMap(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), s.groom->data().graphJson, name, image,
                                     helperMapFiles(s));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    StrokeEdit edit;
    edit.mode = "relink map " + name;
    if (linked.removeLayer) {
        GuideHierarchy h = editedGuides(t, [&](GuideHierarchy& hh) { hh.removeAttribute(name); });
        edit.set(StrokeField::Guides, h.toJson());
    }
    if (!linked.graph.empty()) edit.set(StrokeField::Graph, linked.graph);
    dropPaints(t);
    edit.set(StrokeField::ScalpMaps, linked.maps.toJson());
    for (FileEdit& f : linked.files) t.pending.files.push_back(std::move(f));
    commitEdit(t, std::move(edit));
    BrushToolSettings& st = t.tool.settings;
    if (st.brush.attribute == name && st.paintKind == "texture") st.paintTexture = linked.file;
    return {{"name", name}, {"file", linked.file}, {"undo", undoJson(t)}};
}

// Maya's reload_map: texture map `name`'s image (every texture map's without a name) read again
// after it changed outside (the next evaluation reads files whose stamp changed). Not an undo step.
json mapReload(ToolState& t, const json& args) {
    Session& s = t.session;
    std::vector<std::string> images;
    try {
        images = reloadMapFiles(s.groom->scalpMaps(), s.groom->authoredHierarchy().get(), optString(args, "name", ""), helperMapFiles(s));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    dropPaints(t);
    return {{"files", images}};
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
        else if (key == "parts") s.parts = settingBool(x, k);            // the part lines (with the guides)
        else if (key == "tubes") s.tubes = settingBool(x, k);            // the Tubes switch: the tubes with every tool
        else if (key == "toolTubes") s.toolTubes = settingBool(x, k);    // the Tube or Roots tool up: its tubes shown
        else if (key == "flow") s.flow = settingBool(x, k);
        else if (key == "tubeWires") s.tubeWires = settingBool(x, k);    // the tubes' look (the core's display)
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
    // The guides the graph shapes (a Curl or Noise on one level's Guides node, issue 130) drawn as
    // shaped: the view's drawn hierarchy while what is shown is the one it was made from.
    if (!sculpted && last.drawnHierarchy && in.hierarchy == last.hierarchy) in.hierarchy = last.drawnHierarchy;
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
            // A point channel (Color by Channel's braidTail, say): a color a point, whatever was asked.
            const std::vector<std::array<float, 3>> points = attributePointColors(*strands, mode, source, lo, hi, nodeValues.get());
            std::vector<float> flat;
            std::size_t count = 0;
            if (!points.empty()) {
                count = points.size();
                flat.reserve(count * 3);
                for (const auto& c3 : points) flat.insert(flat.end(), c3.begin(), c3.end());
                colors["perPoint"] = true;
            } else if (perPoint) {
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
            // (a node's values: theirs, a channel only that node has too)
            if (nodeValues ? channelRange(*nodeValues, &low, &high) : strandChannelRange(*strands, source, low, high)) {
                colors["range"] = {low, high};
            }
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
                         {"corrective", e.corrective}, {"sculptNode", e.sculptNode}});
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

void commitStroke(Session& s, StrokeEdit edit, std::function<void(bool after)> apply) {
    ToolState& t = toolsOf(s);
    const std::string map = edit.textureMap;
    if (!map.empty() && !edit.texture.empty() && !t.host.paintOf(map)) {
        t.host.texturePaint(map, edit.texture.channels, edit.texture.width);  // (the file opened for its tiles)
    }
    if (apply) t.pending.actions.push_back(std::move(apply));
    try {
        commitEdit(t, std::move(edit));
    } catch (...) {
        t.pending = SideEdit{};
        throw;
    }
    pruneTargets(t);
}

const std::vector<StrokeEdit>& doneSteps(Session& s) { return toolsOf(s).host.edits(); }
const std::vector<std::string>& redoLabels(Session& s) { return toolsOf(s).redoLabels; }

bool undoSession(Session& s) {
    ToolState& t = toolsOf(s);
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    return undo(t);
}

bool redoSession(Session& s) {
    ToolState& t = toolsOf(s);
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    return redo(t);
}

void setShotLayer(Session& s, const std::string& layer) {
    ToolState& t = toolsOf(s);
    if (t.tool.stroking()) t.tool.cancel(&t.host);
    t.shotLayer = layer;
    applyShotLayer(t);
}

std::string shotLayer(Session& s) { return s.tools ? s.tools->shotLayer : std::string(); }

bool isToolCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {
        "tool_set",  "tool_get",      "tool_drag",    "stroke_begin", "stroke_drag",   "stroke_end",  "stroke_cancel", "hover",    "hover_clear",
        "select",    "selection_get", "selection_set", "pick_level",   "set_hidden_groups", "guides_info", "guides_edit", "maps_list",
        "map_create", "map_remove",   "map_for_param", "undo",        "redo",          "undo_state",  "display",
        // Groom Tools' Tubes section, Segments slider, parts and flow, map management.
        "tube_targets", "tube_get", "tube_set", "tube_scale", "tube_smooth", "tube_fill", "tube_braid", "braid_of", "tube_shapes", "tube_shape_add", "tube_shape_set",
        "tube_shape_rename", "tube_shape_remove", "tube_shape_preview", "guides_preview", "guides_preview_end", "guide_segments",
        "parts_list", "part_add", "part_remove", "parts_clear", "parts_mirror", "flow_clear", "flow_mirror", "map_rename", "map_convert", "map_set_channels",
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
        {"tube_fill", [](ToolState& t, const json& a, BlockReader&) { return tubeFill(t, a); }},
        {"tube_braid", [](ToolState& t, const json& a, BlockReader&) { return tubeBraid(t, a); }},
        {"braid_of", [](ToolState& t, const json& a, BlockReader&) { return braidOf(t, a); }},
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
        {"parts_mirror", [](ToolState& t, const json& a, BlockReader&) { return overlayMirror(t, a, false); }},
        {"flow_mirror", [](ToolState& t, const json& a, BlockReader&) { return overlayMirror(t, a, true); }},
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
            t.tool.syncClone();  // (another tool: Clone's source let go)
        }
        if (args.contains("settings") && !args["settings"].is_null()) applySettings(t, args["settings"]);
        result = toolJson(t);
        return true;
    }
    if (cmd == "tool_get") {
        result = toolJson(t);
        return true;
    }
    if (cmd == "tool_drag") {
        // The radius or the strength dragged (B / M + drag), every host's rule (the editor's
        // dragToolSetting): from `start` by `dx` pixels.
        float value = 0.0f;
        try {
            value = dragToolSetting(t.tool.settings, needString(args, "setting"), settingNumber(need(args, "start"), "start"),
                                    settingNumber(need(args, "dx"), "dx"));
        } catch (const std::invalid_argument& e) {
            fail("bad_request", e.what());
        }
        result = toolJson(t);
        result["value"] = value;
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
            // Every host's pick (the editor's pickGuidesInView, tool_select.h): a click, a box or a
            // lasso, only what the view shows, the mirror guides with the tool's symmetry.
            const BrushToolSettings& st = t.tool.settings;
            const std::vector<Pixel> lasso = pixelsOf(need(args, "lasso"), 1.0f);
            if (lasso.empty()) fail("bad_request", "lasso holds a point (a click), two (a box) or more");
            std::vector<std::array<float, 2>> pixels;
            for (const Pixel& p : lasso) pixels.push_back({p.x, p.y});
            GuidePick pick;
            pick.level = st.brush.level;
            pick.group = st.brush.group;
            pick.hiddenGroups = t.hidden;
            pick.rootsOnly = optBool(args, "rootsOnly", false);
            pick.clickPixels = static_cast<float>(optNumber(args, "clickPixels", 12.0));
            const MeshPtr scalp = s.groom->scalp();
            pick.symmetry = StrokeSymmetry::of(st.brush, scalp.get());
            picked = pickGuidesInView(*h, t.view, scalp, pixels, pick);
        }
        pruneSelection(t);
        const std::vector<GuideId> selection = combineSelection(t.selection, picked, mode);
        setSelection(t, selection);
        // "solo" (the add-on's Select tool): every guide not selected frozen, one undo step when
        // that changed anything; a plain pick of nothing unfreezes all.
        bool froze = false;
        if (optBool(args, "solo", false) && pickSoloes(picked, mode)) {
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
                } else if (edit == "selectShown") {  // Shift+click on Select root: the level in view's roots
                    t.selectTool = false;
                    t.tool.settings.setMode("roots");
                    roots.selectShown(t.host);
                } else if (edit == "sync") {
                    roots.sync(t.host);
                } else {
                    throw std::invalid_argument("root_edit takes start, leave, finish, undoCv, duplicate, endDuplicate, delete, selectShown or sync");
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
            // (Comb shows the levels' row only on a groom with tube levels or root tubes: #132.)
            result["hasTubes"] = result["levels"].size() > 1 || !rootTubesOf(*h).empty();
            // The graph's tube Braid (Make braid's) and its sections' Sculpt node: the level row's
            // Braid (issue 129), on while the brushes shape that node.
            try {
                const GraphEdit edit(s.groom->data().graphJson);
                const std::string braid = tubeBraidNode(edit);
                const std::string sculpt = braid.empty() ? std::string() : braidSculptNode(edit, braid);
                result["braid"] = braid;
                result["braidSculpting"] = !sculpt.empty() && s.groom->sculptTarget() == sculpt;
            } catch (const std::exception&) {
                result["braid"] = "";
                result["braidSculpting"] = false;
            }
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
