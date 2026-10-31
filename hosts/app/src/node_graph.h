// SPDX-License-Identifier: Apache-2.0
#pragma once

// The node graph drawn straight over the viewport: no canvas, no panel, the groom shows between
// the nodes. Nodes are rounded cards with a header in their category's color (playful.json's
// graph section) and their ports down the sides; wires are curves in the port's color; the
// graph's backdrops (a preset's, Maya's Graph Editor's) are tinted frames behind the nodes. The
// view pans and zooms on its own, apart from the camera.
//
// Interaction, while the graph is shown (Tab or the Graph pill; it owns the mouse then, the
// viewport under it does not see clicks; the viewport dims behind it):
//   left drag on empty space       a rubber band: the nodes it touches are selected (with Ctrl or
//                                  Shift: added); a click there deselects (as Maya's Graph Editor)
//   middle drag, wheel             pan, zoom
//   Alt + any button, Alt + wheel  over the viewport: the camera, as everywhere (the events go to
//                                  the viewport); docked: Alt+left or middle drag pans, Alt+right
//                                  drag zooms (right or down zooms in), as Maya's Graph Editor
//   click a node, drag it          selects it (its params open in a floating panel), moves it (the
//                                  whole selection when it is one of them); a lone unwired node
//                                  dropped on a wire goes into it (the wire lights up first)
//   Ctrl+click, Shift+click        toggles a node in the selection, adds it
//   drag from an output port       a new wire; dropped on an input, it connects (replacing what
//                                  fed it); a drag from a fed input picks that wire up instead
//                                  (dropped on empty space: disconnected); over a port it cannot
//                                  feed, a line by the cursor says why, and a drop there changes
//                                  nothing (a picked-up wire goes back)
//   click a wire                   selects it; Delete removes it
//   double-click a wire            puts a pin there (Unreal's reroute node, the graph's "pins":
//                                  graph_pins.h): the wire runs through it; drag the pin to route
//                                  it, drop it on another pin of the same signal to join them (a
//                                  bundle); drag from its nub (right) to an input for another wire
//                                  of its signal; drop a wire from an output on it to feed every
//                                  wire through it from there (a picked-up input's wire: that
//                                  input takes the pin's signal); Delete removes a selected pin
//   Snap (the Graph Editor's bar)  nodes and pins you drag land on the grid (setSnapping)
//   double-click a node            renames it (a glass prompt); a Compound opens instead: its
//                                  nodes back in the graph (Ungroup)
//   right-click                    a node's menu, a backdrop's or the canvas's: the host fills
//                                  them (setMenuFiller: graph_ui.json's nodeMenu, backdropMenu,
//                                  addMenu, as Maya's Graph Editor)
//   a backdrop's title bar         click selects it, drag moves it with the nodes on it; its
//                                  corner grip resizes it; double-click renames it; Delete
//                                  removes it (as Maya's Graph Editor's backdrops)
//   Delete                         removes the selected nodes (the output node stays)
//   B                              bypasses the selected node (again: back)
//   V                              View Up to Here: the viewport shows the graph's result at the
//                                  selected node (again, or V on the shown one: the output again)
//   Ctrl+C, Ctrl+V                 copies the selected nodes with the links between them (as
//                                  text, Maya's Graph Editor's clipboard), pastes them beside
//   Ctrl+D                         duplicates the selection beside it
//   Ctrl+L                         tidies the layout (every position dropped, laid out afresh)
//   Ctrl+G, Ctrl+Shift+G           groups the selected nodes into a Compound (GraphEdit::collapse,
//                                  Maya's Graph Editor's too), ungroups the selected Compounds
//   Tab                            the add-node search at the cursor (Enter adds, into the wire
//                                  under the cursor, else after the selected node when their
//                                  ports match; Escape closes), ranked as Maya's (nodeSearchRank),
//                                  each type by its label, the groom's painted maps after them
//   F                              frames the selected node (none: the whole graph), as Maya's
//   A                              frames the whole graph
//   Escape                         closes the search, else hides the graph
// A node whose evaluation failed wears a red badge (the theme's graph.error) and a red ring; its
// error shows in its tooltip and in the Node panel. Links that run backwards (to a node left of
// their source) loop under the two nodes instead of through them.
// Every edit goes through GroomDocument (one undo step each); a move (of a node or a backdrop)
// and a backdrop's resize are not undo steps.
// Hidden, the graph takes no events at all: the viewport gets them as if it were not there.

#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <QColor>
#include <QMargins>
#include <QPainterPath>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QTimer;

namespace groomlab::app {

class GlassPanel;
class GroomDocument;
class Viewport;

// Where a node type ranks for the Add Node search `text` (lower first), as Maya's Graph Editor
// ranks it (graph_editor.py search_rank): 0 its type name or label starts with the text, 1 either
// holds it (or the text without spaces is in the name), 2 every word is in its search keywords
// or its category's title (or its name), 3 in its params' names, 4 in its category and description, 5 anywhere (its
// params' descriptions too); -1 no match. `spec` is the type's operator spec. Empty text: 5.
int nodeSearchRank(const QString& text, const nlohmann::json& spec);
// The types the Add Node search lists for `text`, best first, ties (and empty text: everything) in
// the menu's order (category, then the type's place in it); hidden types (Compound, Input) never.
std::vector<std::string> nodeSearchResults(const GroomDocument& document, const QString& text);

class NodeGraphView : public QWidget {
    Q_OBJECT
public:
    explicit NodeGraphView(QWidget* parent = nullptr);

    void setDocument(GroomDocument* document);  // null: nothing to draw
    GroomDocument* document() const { return document_; }
    // Where Alt-drags go when the graph lies over the viewport; none (the Graph Editor dock):
    // an Alt-drag pans the graph, as Maya's.
    void setViewport(Viewport* viewport);
    // The document's graph changed (an edit, undo, another graph): the cards follow it.
    void refresh();
    // The current node (the one whose params show): the last one clicked of the selection.
    const std::string& selectedNode() const { return selected_; }
    void selectNode(const std::string& name);  // "" none; emits selectionChanged when it changed
    // Every selected node, in the order they were picked (the current one among them).
    const std::vector<std::string>& selectedNodes() const { return selection_; }
    // Several at once (the current one the last); unknown names are dropped.
    void setSelection(const std::vector<std::string>& names);
    // The rubber band over a view rectangle, as a drag on empty space makes it (`add`: Ctrl, Shift).
    void selectInRect(const QRectF& widgetRect, bool add);
    // A (Frame All): the whole graph fitted, zoomed in no further than 1:1 and out no further than
    // kReadableScale (a longer graph centered at that size: pan for its ends), as Maya's frame_all.
    void fitAll();
    // The view a graph opens at (and keeps while the dock is resized, until panned or zoomed): the
    // whole graph when it fits at a readable size, else a readable zoom on its look end (from the
    // Interpolate node to the Output, or the Output near the right edge), as Maya's frame_readable.
    void frameReadable();
    // The least zoom the view frames at: node names and port labels stay readable in a short dock.
    static constexpr qreal kReadableScale = 0.8;
    // F: the selected node framed (zoomed in no further than 1:1); none selected: fitAll.
    void frameSelected();
    // The view panned (the zoom kept) just enough to show a node's card whole (a node picked
    // from a list or a script); nothing when it shows already.
    void bringIntoView(const std::string& node);
    qreal viewScale() const { return scale_; }  // scene to widget: widget = scene * viewScale + viewPan
    QPointF viewPan() const { return pan_; }
    QPointF sceneAt(const QPointF& view) const { return toScene(view); }  // a view point in scene units
    // What fitAll keeps clear around the graph (the chrome over the view); unchanged margins
    // change nothing (the view stays where it was panned).
    void setFitMargins(const QMargins& margins);
    QMargins fitMargins() const { return fitMargins_; }
    // The add-node search at the cursor (off the view: its middle), or at a point of the view. Over
    // a wire, the node it adds goes into that wire (Maya's Tab on a wire).
    void openSearch();
    void openSearchAt(const QPointF& where);
    void closeSearch();
    bool searchOpen() const;
    void setSearchText(const QString& text);  // as typed into the open search
    void acceptSearch();                      // as Enter does: the current row added
    // The search's rows as shown ("<label>    <category>") and their types, best first: the node
    // types, and the groom's painted maps ("map:<name>", Maya's MAP_ENTRY: a Map node reading it).
    QStringList searchRows() const;
    std::vector<std::string> searchTypes() const;
    static constexpr const char* kMapEntry = "map:";
    // The wire the open search adds into (-1 none: the search was opened off any wire).
    int searchWire() const { return searchWire_; }
    // A node of `type` added at `at` (scene units; NaN: the view's middle, or beside `after`), as
    // the Add Node menu, the search and the library add one: wired after the current node when
    // `wire` (into its first link the new node fits, else fed by it), into wire `onWire` when given
    // (-1 none); `params` its params (null: the defaults), `name` its name (made unique; "" the
    // type's). Selected after; the name made ("" refused, said).
    std::string addNodeOfType(const std::string& type, const QPointF& at, bool wire, int onWire = -1,
                              const nlohmann::json& params = nlohmann::json(), const std::string& name = {});
    // A wire's link by index (from, output, to, input: the output "" for a node's first).
    bool wireLink(int index, std::string* from, std::string* output, std::string* to, std::string* input) const;
    // The wire a dragged node lies over (not one of its own), the one nearest its middle; -1 none.
    int wireUnderNode(int node) const;
    // While a lone unwired node is dragged: the wire it would go into when dropped (-1 none).
    int dropWire() const { return dropWire_; }

    // ---- The right-click menus: the host fills them (the shared definitions, graph_ui.json) ------
    enum class MenuTarget { Canvas, Node, Backdrop };
    // `menu` filled for what was right-clicked: a node (selected by then: the menu's actions act on
    // the selected node), a backdrop (selected) or the canvas; `at` the click in view coordinates.
    using MenuFiller = std::function<void(QMenu* menu, MenuTarget target, const QPointF& at)>;
    void setMenuFiller(MenuFiller filler) { menuFiller_ = std::move(filler); }
    // What a right-click at `at` (view coordinates) selects and the menu it gets, filled into
    // `menu` without showing it (contextMenuEvent shows it; tests read it).
    MenuTarget fillContextMenu(QMenu* menu, const QPointF& at);
    QPointF menuPoint() const { return menuPoint_; }  // where the last menu was asked (view coordinates)
    QString searchAbout() const;  // the line under the list: the current row's description
    int nodeCount() const { return static_cast<int>(nodes_.size()); }
    int backdropCount() const { return static_cast<int>(backdrops_.size()); }
    int wireCount() const { return static_cast<int>(wires_.size()); }  // the links drawn
    // Each node's time in its last run (ms, by name): a badge in its title row.
    void setNodeTimes(const std::vector<std::pair<std::string, double>>& times);
    // The status badges a node's title row shows (the design's, as Maya's Graph Editor), right to
    // left before the header toggles: its cook time ("0.4 ms", "12 ms"; none under a twentieth of a
    // millisecond) muted, in graph.warning from kSlowNodeMs; the View eye in the theme's blue; a
    // warning triangle (graph.warning) for a note. Bypassed: the card at half opacity, the bypass
    // toggle lit, its wires dashed. Failed: the red ring and corner "!" (no triangle).
    static constexpr double kSlowNodeMs = 10.0;
    struct NodeBadges {
        QString time;      // "" none
        QColor timeColor;
        bool viewed = false;
        bool warning = false;
        bool bypassed = false;
        bool failed = false;
    };
    NodeBadges nodeBadges(const std::string& node) const;
    static QString timeText(double ms);
    static QColor timeColor(double ms);
    // A wire into or out of a bypassed node: drawn dashed (index as wireLink's).
    bool wireDashed(int index) const;
    // Each node's note from the last evaluation (it does nothing as set: a Clump with no centers):
    // a warning triangle in its title row.
    void setNodeNotes(const std::vector<std::pair<std::string, std::string>>& notes);
    std::string nodeNote(const std::string& node) const;
    // Each failing node's error from the last evaluation (GroomDocument::nodeErrors): a red badge.
    void setNodeErrors(const std::vector<std::pair<std::string, std::string>>& errors);
    std::string nodeError(const std::string& node) const;
    // View Up to Here: the node the viewport shows ("" the output). Emits viewNodeChanged.
    void setViewedNode(const std::string& node);
    const std::string& viewedNode() const { return viewed_; }
    void toggleViewSelected();  // V
    // Copy and paste (Ctrl+C, Ctrl+V): the clipboard's text, as Maya's Graph Editor writes it.
    void copySelected();
    void paste();
    void deleteSelected();  // Delete: the selected nodes (or the selected wire, pin or backdrop)
    // Snap: nodes and pins dragged land on the grid (24 scene units, the docked canvas's lines).
    void setSnapping(bool on) { snapping_ = on; }
    bool snapping() const { return snapping_; }
    // The pins as drawn (tests, scripts): their ids, where one is (scene units), the pins a wire
    // runs through, and the selected one ("" none).
    std::vector<std::string> pinIds() const;
    QPointF pinPos(const std::string& pin) const;
    std::vector<std::string> wirePins(int wire) const;
    std::string selectedPin() const;
    void selectPin(const std::string& pin);
    // While a wire is dragged over a port: why it cannot connect there ("" when it can, or when
    // no wire is over a port). connectionProblem says it for the output (node, port) to the input
    // (node, port) by index: "" when they fit.
    QString wireHint() const { return wireHint_; }
    QString connectionProblem(int fromNode, int fromPort, int toNode, int toPort) const;
    // A node's card in view coordinates (empty for an unknown name).
    QRectF nodeRect(const std::string& node) const;
    // A wire's path as drawn, scene units (tests: a backward link loops under its nodes).
    QPainterPath wirePathOf(int index) const { return index >= 0 && index < wireCount() ? wires_[index].path : QPainterPath(); }
    // The edits a node's context menu offers (Rename..., Duplicate, Set as Output, Bypass,
    // Delete) and the canvas's (Add node, Tidy layout, Fit), as the menu does them.
    void renameSelected();     // the prompt, then the document's rename
    void duplicateSelected();  // Ctrl+D
    void setSelectedAsOutput();
    void tidyLayout();         // every position dropped: laid out afresh and fitted (Ctrl+L)
    // Ctrl+G: the selected nodes into one Compound (selected after); Ctrl+Shift+G: the selected
    // Compounds' nodes back in the graph (selected after). Each one undo step.
    void groupSelected();
    void ungroupSelected();
    // A backdrop around the selected nodes (none: 400x240 around `at`, a view point; NaN: the
    // view's middle), then the prompt for its title. One undo step.
    void addBackdrop(const QPointF& at = QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0));
    // The backdrops by index (the graph's list): selected (-1 none), title, color, frame (scene
    // units), its title bar and resize grip in view coordinates (tests drag them).
    int selectedBackdrop() const { return selectedBackdrop_; }
    void selectBackdrop(int index);
    QString backdropTitle(int index) const;
    QColor backdropColor(int index) const;
    QRectF backdropRect(int index) const;
    QRectF backdropTitleBar(int index) const;
    QRectF backdropGrip(int index) const;
    void renameBackdrop(int index);  // the prompt, then the document's edit
    void setBackdropColor(int index, const QColor& color);
    void removeBackdrop(int index);
    // A glass prompt for a name (Save as Preset): Enter gives it through `done`, Escape drops it.
    void promptName(const QString& title, const QString& placeholder, std::function<void(const QString&)> done);
    bool promptOpen() const;
    void acceptPrompt();  // as Enter does (tests)
    void setPromptText(const QString& text);
    // ---- Compound nodes (node_library.h). Save as Node: a glass form over the view (no dialog)
    // for Compound `node`: label, category, description, author, id, version, license.
    void openSaveNode(const std::string& node);
    bool saveNodeOpen() const;
    void setSaveNodeField(const QString& field, const QString& value);  // as typed into it
    QString saveNodeField(const QString& field) const;
    QString acceptSaveNode();  // as Save does: "" or the error (shown on the form)
    // A library node written over its file as it is now, one version up; the library's newer copy.
    void saveToLibrary(const std::string& node);
    void updateLibraryNode(const std::string& node);
    // .glnode files copied into the user's node folder, each added (at `at`, view coordinates; NaN:
    // the view's middle): Import Node... and a file dropped on the view. The nodes added.
    std::vector<std::string> importNodeFiles(const QStringList& paths,
                                             const QPointF& at = QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0));
    // ---- Script nodes (script.h). New Script Node: one from the template (at `at`, view
    // coordinates; NaN: the view's middle), its editor open; the node made.
    std::string newScriptNode(const QPointF& at = QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0));
    // Edit Node...: a glass panel over the view (no dialog) with the node's label, what it writes
    // and its script (monospace); live: a change is checked after a pause (300 ms), its errors
    // listed by line and column (the line tinted), and the node takes it when it checks.
    void openScriptEditor(const std::string& node);
    bool scriptEditorOpen() const;
    void setScriptEditorField(const QString& field, const QString& value);  // "label", "writes", "script": as typed
    QString scriptEditorField(const QString& field) const;
    QString scriptEditorErrors() const;  // as listed ("" none)
    QString applyScriptEditor();         // as the pause does: "" or the errors
    QString saveScriptEditor(bool asNew);  // Save to Library / Save as New: "" or the error
    void closeScriptEditor();

signals:
    void selectionChanged(const std::string& node);
    // The graph changed through this view: the host evaluates and refreshes what shows it.
    void edited();
    void message(const QString& text);  // an error (refused edits)
    void status(const QString& text);   // what an edit did (the Graph Editor's status line)
    void hideRequested();
    // View Up to Here changed: the host evaluates the graph up to that node ("" the output).
    void viewNodeChanged(const std::string& node);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;  // a node's header: rename
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;  // a node's tooltip: its error or note
    bool eventFilter(QObject* watched, QEvent* event) override;  // the search's keys
    void dragEnterEvent(QDragEnterEvent* event) override;  // .glnode files: imported and added
    void dropEvent(QDropEvent* event) override;

private:
    struct Port {
        std::string name, type;
        bool optional = false;
    };
    struct Node {
        std::string name, type, category;
        bool bypass = false;
        std::string evaluation = "always";  // when it runs: always, viewportOnly, renderOnly
        QPointF pos;   // scene units (the card's top-left)
        QSizeF size;
        std::vector<Port> inputs, outputs;
        std::vector<std::string> linkableParams;  // params the header's Expand Inputs button toggles
        std::string libraryLabel;      // a node from the node library: its label
        bool updateAvailable = false;  // the library has a newer copy of it
    };
    struct Wire {
        int from = -1, to = -1;  // node indices
        int output = 0, input = 0;  // port indices
        std::vector<int> pins;     // the pins it runs through (indices into pins_), source first
        QPainterPath path;         // scene units
    };
    // A pin the wires run through (the graph's "pins", graph_pins.h): drawn over the wires in the
    // color of the signal it carries.
    struct Pin {
        std::string id;
        QPointF pos;  // scene units, its center
        QColor color;
        int from = -1, output = 0;  // the signal: its source node and output (as a wire's)
    };
    // A backdrop framing part of the graph (the graph's "backdrops", graph_io.h: Maya's Graph
    // Editor's and a preset's): drawn behind the nodes, layout only.
    struct Backdrop {
        QString title;
        QRectF rect;  // scene units
        QColor color;
    };
    enum class Drag { None, Pan, Zoom, Move, Wire, Camera, Band, Backdrop, BackdropResize, Pin };

    void rebuild();      // nodes_, wires_ and backdrops_ from the document
    void autoLayout();   // positions for nodes the document gives none
    QPointF toScene(const QPointF& widget) const;
    QPointF toWidget(const QPointF& scene) const;
    QPointF inputPos(const Node& node, int port) const;   // scene units, the port's center
    QPointF outputPos(const Node& node, int port) const;
    // A wire from an output to an input; one running backwards loops under `below` (scene y:
    // the lowest edge of the nodes it joins; NaN: a little under its ends).
    QPainterPath wirePath(const QPointF& from, const QPointF& to, qreal below = std::numeric_limits<qreal>::quiet_NaN()) const;
    QPainterPath wirePathFor(const Wire& wire) const;
    int nodeIndex(const std::string& name) const;
    bool isSelected(const std::string& name) const;
    void updateWireHint(const QPointF& scene);
    int nodeAt(const QPointF& scene) const;
    // A port under a scene point: the node and port indices, output or input.
    bool portAt(const QPointF& scene, int* node, int* port, bool* output) const;
    int pinAt(const QPointF& scene, bool* nub = nullptr) const;  // -1 none; `nub`: on its nub
    int pieceAt(const Wire& wire, const QPointF& scene) const;    // which of its pieces (between its points) is nearest
    QPointF snappedIf(const QPointF& scene) const;                // on the grid while Snap is on
    int wireAt(const QPointF& scene) const;
    bool bypassButtonAt(const QPointF& scene, int* node) const;
    bool inputsButtonAt(const QPointF& scene, int* node) const;
    // The backdrop whose title bar (or, `grip`, resize corner) is under a scene point; -1 none.
    int backdropTitleAt(const QPointF& scene, bool* grip = nullptr) const;
    int backdropAt(const QPointF& scene) const;  // anywhere on it
    QRectF bypassButtonRect(const Node& node) const;
    QRectF inputsButtonRect(const Node& node) const;
    void drawNode(QPainter& painter, const Node& node, bool selected);
    void forwardToViewport(QEvent* event);
    void addFromSearch();
    void filterSearch(const QString& text);
    void setBypass(const std::string& node, bool bypass);
    void toggleInputs(const std::string& node);
    void setEvaluation(const std::string& node, const std::string& mode);

    GroomDocument* document_ = nullptr;
    Viewport* viewport_ = nullptr;
    GlassPanel* saveNode_ = nullptr;          // the Save as Node form (made as it opens)
    std::map<QString, QWidget*> saveFields_;  // its fields by name
    QLabel* saveError_ = nullptr;
    std::string saveNodeName_;                // the compound it saves
    bool saveIdTyped_ = false;                // the id typed (no longer follows label and author)
    GlassPanel* scriptEditor_ = nullptr;      // the script node editor (made as it opens)
    std::map<QString, QWidget*> scriptFields_;
    QLabel* scriptErrors_ = nullptr;
    QLabel* scriptStatus_ = nullptr;
    QTimer* scriptPause_ = nullptr;
    std::string scriptNode_;                  // the Script node it edits
    std::vector<Node> nodes_;
    std::vector<Wire> wires_;
    std::vector<Backdrop> backdrops_;
    std::vector<Pin> pins_;
    int selectedPin_ = -1;
    int dragPin_ = -1;      // the pin being moved
    QPointF pinStart_;      // where it was when the move began
    int dragWirePin_ = -1;  // a wire dragged out of a pin's nub: that pin
    bool snapping_ = false;
    std::map<std::string, double> times_;     // ms per node in its last run
    std::map<std::string, std::string> notes_;  // a note per node from its last run
    std::map<std::string, std::string> errors_;  // an error per failing node
    std::string viewed_;  // View Up to Here ("" the output)
    std::map<std::string, QPointF> laidOut_;  // positions the view chose (the document has none)
    std::string selected_;
    std::vector<std::string> selection_;
    QRectF band_;  // the rubber band (view coordinates) while it drags
    bool bandAdds_ = false;
    QPointF moveStart_;  // the pressed node's position when a move began
    QString wireHint_;
    int selectedWire_ = -1;
    int selectedBackdrop_ = -1;
    // A backdrop being moved (the nodes on it carried along, by index) or resized.
    int dragBackdrop_ = -1;
    QRectF backdropStart_;
    std::vector<int> carried_;
    qreal scale_ = 1.0;
    QPointF pan_;
    QMargins fitMargins_{60, 90, 60, 100};
    bool fitted_ = false;
    // The view panned, zoomed or framed by hand since the graph was fitted: a resize (the Graph
    // Editor's dock dragged) keeps it; untouched, a resize fits the graph again.
    bool viewTouched_ = false;
    Drag drag_ = Drag::None;
    QPointF last_;        // widget units
    QPointF pressScene_;  // where the press was
    int dragNode_ = -1;
    // A wire being dragged: from an output port (dragWireFrom_, dragWirePort_) to the cursor;
    // picked up from an input (dragWireInputNode_ >= 0), that connection goes when dropped away.
    int dragWireFrom_ = -1, dragWirePort_ = -1;
    int dragWireInputNode_ = -1, dragWireInputPort_ = -1;
    QPointF dragWireTo_;  // scene units
    // An Alt+right-drag zoom: the scene point pressed stays under the press, the zoom when it began.
    QPointF zoomPress_, zoomAnchor_;
    qreal zoomStart_ = 1.0;
    bool zoomEnded_ = false;  // the right button's release ended a zoom: no menu
    int dropWire_ = -1;       // the wire a dragged lone node would go into
    int searchWire_ = -1;     // the wire the open search adds into
    MenuFiller menuFiller_;
    QPointF menuPoint_;
    // The add-node search.
    GlassPanel* search_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QListWidget* searchList_ = nullptr;
    QLabel* searchAbout_ = nullptr;  // the current type's description under the list
    QPointF searchScene_;
    // The name prompt.
    GlassPanel* prompt_ = nullptr;
    QLabel* promptTitle_ = nullptr;
    QLineEdit* promptEdit_ = nullptr;
    std::function<void(const QString&)> promptDone_;
};

}  // namespace groomlab::app
