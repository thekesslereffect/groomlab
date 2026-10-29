// SPDX-License-Identifier: Apache-2.0
#pragma once

// The interactive tools on the open groom, without a window: the core's BrushTool (stroke.h)
// over a host that reads the document's session and hands each stroke's edit to the document's
// history, driven in widget pixels (y down) of the view the host sets (the viewport's camera and
// size, tool_view.h); the hover's lines and the brush ring as overlay geometry the renderer
// draws; guide picking for the Select tool (guide_selection.h). No Qt widgets: the tests drive
// it as the viewport does, and the CLI's --stroke runs through it.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QPointF>
#include <QVector3D>

#include "camera.h"
#include "groomlab/display.h"
#include "groomlab/guide_selection.h"
#include "groomlab/stroke.h"
#include "tool_view.h"

namespace groomlab::app {

class GroomDocument;
class DocumentStrokeHost;

class BrushSession {
public:
    BrushSession();
    ~BrushSession();
    BrushSession(const BrushSession&) = delete;
    BrushSession& operator=(const BrushSession&) = delete;

    // The groom to brush (null: none); a stroke in flight is dropped.
    void setDocument(GroomDocument* document);
    GroomDocument* document() const { return document_; }
    // The view the mouse positions are in: the camera over a widget of this size, and where the
    // ring sits when the cursor is off the scalp (the camera's target: that depth).
    void setView(const OrbitCamera& camera, int width, int height);
    const ToolView& view() const { return view_; }
    int width() const { return width_; }
    int height() const { return height_; }

    BrushToolSettings& settings() { return tool_.settings; }
    const BrushToolSettings& settings() const { return tool_.settings; }
    // The rail's tools by the catalog's ids (brush_catalog.h): a brush mode (BrushToolSettings::
    // setMode) or "freeze", the Paint brush on the guides' freeze mask toward 1 (Ctrl: toward 0,
    // unfreeze), as Maya's freeze_tool starts it; the other brushes leave frozen guides alone
    // (brush.h's BrushSettings::freeze) and the display draws them in the theme's frozen color.
    // Leaving Freeze puts back what Paint painted before. Throws std::invalid_argument for an
    // unknown id, leaving the settings as they were.
    void setTool(const std::string& id);
    // The catalog id of the tool the settings make: "freeze" while they paint the freeze mask on
    // the guides, else settings().modeName().
    std::string toolId() const;
    // The view's density while a stroke is live (ViewRequest::sculptDensity): the strands step
    // back to this fraction so each dab evaluates fast; the release evaluates them all again.
    float sculptDensity = 1.0f;
    // Shot sculpting (the core's, as Maya's Sculpt tab): with `on`, strokes reshape the groom as
    // shown at `seconds` (frame `frame`) and record the difference into the corrective layer
    // `layer` (made at that time when the groom has none of the name) instead of the guides.
    void setShotSculpt(bool on, double seconds = 0.0, long frame = 0, const std::string& layer = {});
    bool shotSculpt() const { return shot_; }
    const std::string& shotLayer() const { return shotLayer_; }

    // ---- Strokes (widget pixels) --------------------------------------------------------------
    // A press starts a stroke (Shift: waits for a drag or a click); false when none could (no
    // groom, no scalp: lastError says). Moves continue it (each dab previews through the
    // session: the host evaluates and redraws after one); the release commits it to the document.
    bool press(const QPointF& at, float pressure, unsigned modifiers);
    void move(const QPointF& at, float pressure);
    StrokeRelease release();
    void cancel();  // drops the stroke without committing it
    bool stroking() const { return tool_.stroking(); }
    // A whole stroke through press, moves and release (tests, the CLI): true when it changed something.
    bool stroke(const std::vector<QPointF>& points, float pressure = 1.0f, unsigned modifiers = 0);
    // The level (and group) of the guide under the cursor becomes the brush's; -2 when no guide
    // is there.
    int pickLevel(const QPointF& at);
    // What went wrong in the last press, move or release ("" when nothing): the stroke was dropped.
    const std::string& lastError() const { return error_; }
    // The core's warnings since the last clear (a stroke that touched nothing says why).
    std::vector<std::string> takeWarnings();

    // ---- Hover and the overlay -----------------------------------------------------------------
    // The cursor at `at` (not pressed): the guides the brush would touch and the ring under it
    // go into the overlay. The ring alone while a stroke is live; clearHover empties it.
    void hover(const QPointF& at);
    void clearHover();
    // The Select tool's box between two widget corners, drawn at the camera target's depth.
    void showBox(const QPointF& a, const QPointF& b);
    // The lines the renderer draws over the view (colored, no depth test): the hovered guides,
    // the brush ring on the scalp, a Part or Flow stroke's path so far. The version moves when
    // they change.
    const DisplayGeometry& overlay() const { return overlay_; }
    std::uint64_t overlayVersion() const { return overlayVersion_; }
    // Where the ring sits and how big it is (world units), from the last hover or move; valid
    // while the cursor is over the view.
    bool ringValid() const { return ringValid_; }
    const Vec3f& ringCenter() const { return ringCenter_; }
    float ringRadius() const { return ringRadius_; }

    // ---- Guide selection (the Select tool) -------------------------------------------------------
    // A click (one point) or a box (two corners) in widget pixels: the guides of the brush's
    // level or group (not hidden) picked, combined with the selection by `mode`, set on the
    // document. Returns how many guides the pick found.
    // `solo` (the Select tool): every guide not selected frozen, an undo step when that changed
    // anything (a plain pick of nothing unfreezes every guide).
    std::size_t select(const std::vector<QPointF>& lasso, SelectMode mode, bool solo = false);
    // A brush's Shift drag: the guides under the ring at `at` combined with the selection by `mode`
    // (added: no undo step; soloSelection after it makes the one; `solo` as select's).
    std::size_t paintSelection(const QPointF& at, SelectMode mode = SelectMode::Add, bool solo = false);
    // The guides not selected frozen (the Select tool's solo), an undo step when that changes anything.
    void soloSelection();
    // The hierarchy guides are picked from (as shown); null without a groom.
    std::shared_ptr<const GuideHierarchy> shownHierarchy() const;

    // The stroke document evaluated ahead with the view's request (idle time before a stroke).
    void warm(const ViewRequest& request);

private:
    std::array<float, 2> port(const QPointF& at) const { return toPort(at, height_); }
    void rebuildOverlay(const QPointF& at, bool withLines);
    // The world point under a port pixel at the camera target's depth; false off the view.
    bool atTargetDepth(const std::array<float, 2>& pixel, Vec3f& out) const;
    void guard(const char* what, const std::function<void()>& fn);

    GroomDocument* document_ = nullptr;
    std::unique_ptr<DocumentStrokeHost> host_;
    BrushTool tool_;
    ToolView view_;
    int width_ = 0, height_ = 0;
    QVector3D target_;
    DisplayGeometry overlay_;
    std::uint64_t overlayVersion_ = 0;
    bool ringValid_ = false;
    Vec3f ringCenter_;
    float ringRadius_ = 0.0f;
    // The scalp's line of symmetry (symmetrySeam), drawn red while symmetry is on; kept until the
    // scalp or the symmetry changes.
    struct Seam {
        MeshPtr scalp;
        int axis = -1;
        float center = 0.0f;
        bool topology = false;
        std::vector<Vec3f> points;
    } seam_;
    std::string error_;
    bool shot_ = false;
    double shotSeconds_ = 0.0;
    long shotFrame_ = 0;
    std::string shotLayer_;
    // What Paint painted before Freeze took the brush (setTool puts it back).
    struct PaintTarget {
        std::string attribute = "clump";
        float value = 1.0f;
        std::string kind = "guides", texture;
    };
    PaintTarget beforeFreeze_;
};


}  // namespace groomlab::app
