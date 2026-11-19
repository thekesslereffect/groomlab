// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Tube tool's root-first mode in a viewport (issue 61), every host's: the root tubes' work
// (root_tubes.h) driven by the mouse. First written for Maya in Python (root_tube_tool.py).
//
// Place (Draw root): a press on the scalp places a CV, a drag moves it; with Snap a CV snaps onto a
// neighbouring root's CVs or edges within kPickPixels, so roots seal with no gap. A press on the
// first CV (3 or more) closes the outline: with Settings::drawPath the tool goes on to Draw, where a
// stroke drawn from the root's middle sets the tube's path (on release the tube is made); without,
// the tube goes straight off the scalp at once. Undoing the tube just made brings its outline back
// (drawing its path again, or the outline open); redoing it puts the outline away.
//
// Edit: the root tubes (the selected guides' or every one) as their outlines with their CVs, the
// guides and tubes as the Visibility switches have them (hidesGuides: never, since #85); a drag moves a CV on the scalp (CVs sealed with a neighbour move
// together; with Snap onto another root's CVs and edges, never its own), Ctrl+click on an edge adds
// a CV on every outline sharing it, Ctrl+click on a CV takes it out of the root meant (the one last
// touched). undoCv takes back a CV move; finishEdit gives each changed root its outline (one undo
// step), and so does leaving the mode any other way, so no edit is lost.
//
// Select: every root's outline and middle; a click on a middle selects that root (Shift: adds it or
// takes it out), a drag carries the selected roots over the scalp as one (with Snap, their CVs
// sealed onto a root not selected stay), committed on release (one undo step). beginDuplicate: the
// selected roots follow the cursor (hover) and a click sets copies down there, their hierarchies
// with them; deleteSelected removes them.
//
// Events come in port pixels (origin bottom left) of a ToolView, as BrushTool's. Edits are
// committed through the StrokeHost as one StrokeEdit each; the tool reads the groom again when it
// changed under it (an undo, a redo, another tool: sync, which every event and overlay call runs).

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/math.h"
#include "groomlab/root_tubes.h"

namespace groomlab {

class StrokeHost;
struct ToolView;

class RootTubeTool {
public:
    enum class Mode { Place, Draw, Edit, Select, Duplicate };
    static constexpr float kPickPixels = 12.0f;  // how near on screen a press takes a CV, a middle, an edge

    struct Settings {
        bool drawPath = true;  // a path drawn after the outline closes ("Draw guide before extrude")
        bool snap = true;      // CVs snap onto neighbouring roots
        float length = 1.0f;   // a tube straight off the scalp: its length ...
        int segments = 12;     // ... and spans
        float taper = 0.5f;    // the tip's width, a share of the root's
        float density = 1.0f;  // a drawn path's points, times this
        std::string group;     // where new roots go ("": kRootTubeGroup)
    };
    Settings settings;

    // Starts in Place (Draw root), Edit or Select (an edit pending finished first).
    void start(StrokeHost& host, Mode mode);
    // Put away: an edit pending finished, the state cleared.
    void leave(StrokeHost& host);

    // Mouse events (kStrokeCtrl, kStrokeShift in `modifiers`). press returns false when it did
    // nothing (message() says why).
    bool press(StrokeHost& host, const ToolView& view, float x, float y, unsigned modifiers);
    void drag(StrokeHost& host, const ToolView& view, float x, float y);
    void release(StrokeHost& host, const ToolView& view);
    // The cursor moving with no button held: in Duplicate the copies follow it.
    void hover(StrokeHost& host, const ToolView& view, float x, float y);

    // The card's buttons.
    void undoCv(StrokeHost& host);
    int finishEdit(StrokeHost& host);       // the roots changed (0: none, or not editing)
    bool beginDuplicate(StrokeHost& host);  // false with no root selected
    void endDuplicate();
    int deleteSelected(StrokeHost& host);   // the roots removed

    // The groom read again when it changed under the tool (undo, redo, another tool): the tube an
    // undo took back gives its outline back, a redo puts it away, Select reloads its roots, Edit
    // takes the roots as the groom has them. Events and overlay() run it.
    void sync(StrokeHost& host);

    Mode mode() const { return mode_; }
    bool active() const { return started_; }
    // The last thing the tool has to say (the card's status line), and a count bumped with each.
    const std::string& message() const { return message_; }
    std::uint64_t messageCount() const { return messageCount_; }
    // (Edit roots leaves the guides and tubes as the hosts' Visibility switches say: never hidden.)
    bool hidesGuides() const { return false; }
    const std::vector<GuideId>& selected() const { return picked_; }
    std::size_t outlineCvs() const { return cvs_.size(); }  // Place: the CVs placed so far

    // What a host draws, laid on the scalp: each item an outline (its edge along the scalp between
    // its CVs, its fill's triangles, its CVs as dots), a marker (a root's middle) or a drawn
    // stroke (in the air), in a color, with style bits.
    enum Style : unsigned { Closed = 1, Filled = 2, Dots = 4, Highlight = 8, Marker = 16, Dim = 32, Stroke = 1024 };
    struct Item {
        std::vector<Vec3f> edge;       // a polyline (a loop when Closed); a Marker: its one point
        std::vector<Vec3f> triangles;  // the fill, three points each (Filled)
        std::vector<Vec3f> dots;       // the CVs (Dots)
        std::array<float, 3> color{0.85f, 0.85f, 0.9f};
        unsigned style = 0;
    };
    std::vector<Item> overlay(StrokeHost& host);
    // How the hosts draw it (Maya's, Blender's and the app's alike): edge width, dot size (pixels),
    // the fill's opacity, a highlighted edge's color and width, the dots' color.
    static constexpr float kEdgePixels = 3.0f, kDotPixels = 9.0f, kFillOpacity = 0.5f;
    static constexpr std::array<float, 3> kHighlight{0.3f, 1.0f, 0.4f}, kDotColor{1.0f, 0.62f, 0.2f};
    // An edge's color: the root's, lightened (highlighted: kHighlight).
    static std::array<float, 3> edgeColor(const Item& item);

private:
    struct Made {
        GuideId guide = kNoGuide;
        std::vector<Vec3f> cvs;
        Vec3f center, normal;
        bool drawn = false;
    };
    struct Move {
        Vec3f press, middle;
        std::map<GuideId, std::vector<Vec3f>> base;
        std::map<GuideId, std::set<int>> held;
        bool moved = false;
    };
    using Outlines = std::vector<std::pair<GuideId, std::vector<Vec3f>>>;

    void say(std::string text);
    void reset();
    void seen(StrokeHost& host);
    bool commit(StrokeHost& host, const std::string& label, const std::function<void(GuideHierarchy&, const ScalpMesh&)>& edit);
    Outlines snapTargets(StrokeHost& host, const std::set<GuideId>& skip = {}) const;
    void reloadNeighbours(StrokeHost& host);
    std::array<float, 3> colorOf(GuideId guide) const;
    void recolor(StrokeHost& host);

    // Place and Draw.
    Vec3f snapped(const ToolView& view, const Vec3f& p, RootSnap* how = nullptr) const;
    void close(StrokeHost& host);
    GuideId make(StrokeHost& host, const std::vector<Vec3f>& path);
    // Edit.
    void beginEdit(StrokeHost& host);
    float tolerance() const;
    void remember();
    bool cvUnder(const ToolView& view, float x, float y, Vec3f* picked, std::vector<std::pair<GuideId, int>>* owners) const;
    GuideId meant(const ToolView& view, const std::vector<std::pair<GuideId, int>>& owners, float x, float y) const;
    bool removeCv(const ToolView& view, float x, float y);
    void insertCv(StrokeHost& host, const ToolView& view, float x, float y);
    void reconcileEdits(StrokeHost& host);
    std::vector<Vec3f> cleaned(const std::vector<Vec3f>& cvs) const;
    // Select and Duplicate.
    void reloadRoots(StrokeHost& host);
    Vec3f groupMiddle(StrokeHost& host) const;
    void pressSelect(StrokeHost& host, const ToolView& view, float x, float y, unsigned modifiers);
    void releaseSelect(StrokeHost& host);
    void follow(StrokeHost& host, const ToolView& view, float x, float y);
    void placeCopy(StrokeHost& host);
    // The overlay.
    void laid(const ScalpMesh& scalp, const std::vector<Vec3f>& cvs, bool closed, bool filled, Item& item);

    bool started_ = false;
    Mode mode_ = Mode::Place;
    std::vector<Vec3f> cvs_, stroke_;
    std::optional<ScalpPointNormal> center_;
    Outlines neighbours_;
    std::set<std::string> hidden_;  // the guide groups hidden when the roots were last read (shownRoots)
    std::string mixed_;             // ... and the Mixed view
    std::map<GuideId, std::vector<Vec3f>> edits_, original_;
    std::vector<std::pair<GuideId, int>> drag_;
    std::set<GuideId> touched_;
    GuideId current_ = kNoGuide;
    std::vector<std::map<GuideId, std::vector<Vec3f>>> history_;
    bool scope_ = false;
    std::map<GuideId, std::vector<Vec3f>> roots_;
    std::map<GuideId, ScalpPointNormal> middles_;
    std::vector<GuideId> picked_;
    std::optional<Move> move_;
    std::optional<std::pair<Vec3f, std::vector<std::vector<Vec3f>>>> ghost_;
    std::optional<Made> lastMade_, restored_;
    std::map<GuideId, std::array<float, 3>> colors_;
    std::string message_;
    std::uint64_t messageCount_ = 0;
    std::uint64_t seen_ = 0;  // the groom's guides' hash when the tool last read or wrote them
    std::map<std::vector<float>, Item> laidCache_;
};

}  // namespace groomlab
