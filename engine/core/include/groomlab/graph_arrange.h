// SPDX-License-Identifier: Apache-2.0
#pragma once

// How every host's node editor arranges a graph by hand (the app's node graph, Maya's Graph
// Editor): aligning and spacing the selected nodes, laying out just the selection, backdrops that
// grow around the nodes moved on them and fold to their title bar, the shake that pulls a node out
// of its links, the nodes a Shift or Ctrl+drag carries (downstream or upstream) with their pins and
// dots, Houdini's A+drag align gestures, and the three wire styles.
// Layout only: nothing here changes what the graph evaluates. Positions are the editors' scene
// units, y growing downward, each node's (x, y) its card's top-left.

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"

namespace groomlab {

// A node's card (or any rectangle) in an editor: top-left and size.
struct LayoutBox {
    std::string name;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    double centerX() const { return x + w * 0.5; }
    double centerY() const { return y + h * 0.5; }
};

// ---- Align and space (every host's node menu > Align, and its keys) ---------------------------
//   left, centerX, right        the left edges, the centers or the right edges in one column
//   top, centerY, bottom        the top edges, the centers or the bottom edges in one row
//   row                         one row, left to right in their order across, `gap` apart (and
//                               their centers on the first one's): "Align Horizontally"
//   column                      one column, top to bottom, `gap` apart: "Align Vertically"
//   distributeX, distributeY    the outer two stay, the others spread so the gaps are equal
// Returns each box's new top-left by name (every box, moved or not); fewer than two boxes (three
// to distribute) come back where they were. An unknown mode: GraphError.
std::map<std::string, std::pair<double, double>> alignBoxes(const std::vector<LayoutBox>& boxes, const std::string& mode,
                                                            double gap = 40.0);
// The modes, in the menus' order.
std::vector<std::string> alignModes();

// Tidy Selection (Auto Align with nodes selected): the selected nodes laid out as the core lays out
// a graph (layoutPositions: columns by the longest path in among them, rows in the graph's order),
// `column` and `row` apart, the block's top-left where the selection's was. Nodes outside the
// selection stay put. Names not in the graph are left out.
std::map<std::string, std::pair<double, double>> arrangeSelection(const GraphEdit& edit, const std::vector<LayoutBox>& selected,
                                                                  double column, double row);

// ---- Backdrops ("backdrops" in the graph: graph_io.h) ------------------------------------------
// A backdrop holds the nodes whose card's center lies on it.
bool onBackdrop(const LayoutBox& backdrop, const LayoutBox& node);
// A backdrop grown to hold `nodes` (each with `margin` around it, the backdrop's title bar,
// `header` tall, above): it only ever grows, as a network box does when its nodes are moved out
// to its edge. The same box when they fit.
LayoutBox grownBackdrop(const LayoutBox& backdrop, const std::vector<LayoutBox>& nodes, double margin, double header);
// A folded backdrop ("collapsed": true, as a minimized network box): drawn as its title bar and a
// strip under it, `minWidth` to `maxWidth` wide, its nodes hidden; links into or out of them meet
// its edges instead (the left edge for links in, the right for links out). A host passes a minWidth
// at least as wide as the backdrop's title, its chevron and margins, so the title always shows;
// the pins and dots on it (layoutOnBackdrop) are hidden with its nodes.
LayoutBox foldedBackdrop(const LayoutBox& backdrop, double header, double minWidth = 160.0, double maxWidth = 260.0);
// Whether the backdrop in the graph's JSON is folded.
bool backdropFolded(const nlohmann::json& backdrop);
// While a node on a backdrop is dragged: whether the backdrop keeps it (and grows round it) or the
// node leaves it. Dragged slowly past an edge (under kBackdropEscapeSpeed screen pixels a second),
// the backdrop grows with it; dragged out fast, it leaves; once it has left, it stays out for the
// rest of that drag (`left`). A backdrop's "description" and "fontSize" (its title's, in points;
// none: the editors' default) are the editors' too.
constexpr double kBackdropEscapeSpeed = 1100.0;
bool backdropKeeps(bool overIt, double speed, bool left);

// ---- Moving a node with what it is part of (every host's) -------------------------------------
// The nodes a Ctrl+drag of `names` carries: those nodes and everything feeding them (directly or
// not), in the graph's order. A link teleported through a portal (out of one of its exits,
// graph_portals.h) is not followed: what feeds the portal stays where it is.
std::vector<std::string> nodesWithUpstream(const GraphEdit& edit, const std::vector<std::string>& names);
// The nodes a Shift+drag of `names` carries: those nodes and everything they feed (directly or
// not), in the graph's order; again not through a portal's exits.
std::vector<std::string> nodesWithDownstream(const GraphEdit& edit, const std::vector<std::string>& names);
// The pins and dots that go with nodes moved together (several at once: a Shift or Ctrl+drag, a
// selection, a backdrop), as nodes of their own: a pin whose source and every link through it
// end at moved nodes, a portal's entry whose source and every link out of it do, an exit with
// links all of which do. (A dot is (portal, exit), exit -1 its entry.)
struct CarriedLayout {
    std::vector<std::string> pins;
    std::vector<std::pair<std::string, int>> dots;
};
CarriedLayout carriedWith(const nlohmann::json& graph, const std::vector<std::string>& nodes);
// The pins and dots lying on a backdrop (their centers inside `backdrop`): moved with it, hidden
// when it folds, as its nodes are.
CarriedLayout layoutOnBackdrop(const nlohmann::json& graph, const LayoutBox& backdrop);

// ---- Align gestures (Houdini's: hold A and drag from a node) -----------------------------------
// The node dragged from is the anchor; what lines up with it depends on the keys held:
//   chain       A+drag: the anchor's chain upstream (dragged left or up: each node's first fed
//               input's source) or downstream (right or down: each node's first reader)
//   selected    Shift+A+drag: the selected nodes
//   distribute  Alt+A+drag: the selected nodes (and the anchor) spread evenly, the anchor staying
// A drag mostly across makes a row (centers level with the anchor's), mostly up or down a column
// (centers under the anchor's; a chain stacked `gap` apart above or below it). The axis comes
// from the drag so far (alignGestureAxis: "row", "column", or "" while it is under `threshold`).
// Returns the new top-left of each node it moves, by name (nothing while the axis is "").
std::string alignGestureAxis(double dx, double dy, double threshold = 12.0);
std::map<std::string, std::pair<double, double>> alignGesture(const GraphEdit& edit, const std::vector<LayoutBox>& boxes,
                                                              const std::string& anchor, const std::vector<std::string>& selected,
                                                              const std::string& mode, double dx, double dy, double gap = 40.0);
// The gesture modes, as above.
std::vector<std::string> alignGestureModes();

// ---- Shake to disconnect ------------------------------------------------------------------------
// A node shaken while dragged (left and right a few times, quickly) is pulled out of its links
// (GraphEdit::detach: what fed it feeds what it fed). Fed the cursor's positions in screen pixels
// (so the zoom does not matter) with their times in seconds as the drag goes; true once, at the
// move that completes a shake.
class ShakeDetector {
public:
    // Turns in direction (each after at least minTravel pixels one way) within `window` seconds.
    explicit ShakeDetector(int turns = 4, double minTravel = 18.0, double window = 0.7)
        : turns_(turns), minTravel_(minTravel), window_(window) {}
    void reset();
    // A cursor position during the drag. True at the move that completes a shake (then it starts over).
    bool add(double seconds, double x, double y);

private:
    int turns_;
    double minTravel_, window_;
    bool started_ = false;
    double anchorX_ = 0.0;   // where the current stroke began (its turning point)
    double extremeX_ = 0.0;  // how far it has gone
    int direction_ = 0;      // -1 left, 1 right, 0 not yet
    std::vector<double> turnTimes_;
};

// ---- Wire styles ------------------------------------------------------------------------------
//   smooth   a curve leaving the output and reaching the input level (the default)
//   square   straight runs with right angles: across, down or up at the middle, across
//   rounded  the square route with its corners rounded
// A host keeps the style chosen per user (the app's settings, Maya's optionVar), not in the graph.
std::vector<std::string> wireStyles();
// One piece of a wire: a line to (x, y), or a cubic to (x, y) through (c1x, c1y), (c2x, c2y).
struct WireStep {
    bool cubic = false;
    double c1x = 0.0, c1y = 0.0, c2x = 0.0, c2y = 0.0;
    double x = 0.0, y = 0.0;
};
// A wire from an output at (ax, ay) to an input at (bx, by), as steps from (ax, ay). A wire that
// runs backwards (the input left of the output) loops under `below` (the lower edge of the two
// cards; NaN: a little under its ends) so it never crosses them. `through`: the wire runs through
// a pin there (a piece between pins): it leaves and arrives level, no loop. An unknown style is
// drawn smooth.
std::vector<WireStep> wireRoute(const std::string& style, double ax, double ay, double bx, double by,
                                double below = std::nan(""), bool through = false);

}  // namespace groomlab
