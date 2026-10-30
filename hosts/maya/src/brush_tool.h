// SPDX-License-Identifier: Apache-2.0
#pragma once

// Maya's brush tool: a thin host of the core's interactive tools (groomlab/stroke.h). The
// context turns mouse events into BrushTool calls, draws the brush circle and the hover hint,
// and commits each stroke's StrokeEdit through groomLabBrushStroke (one undo step); its host
// reads the groom's plugs, previews through the shape and keeps texture maps' files.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <maya/MDGModifier.h>
#include <maya/MDagPath.h>
#include <maya/MObjectHandle.h>
#include <maya/MPointArray.h>
#include <maya/MPxContext.h>
#include <maya/MPxContextCommand.h>
#include <maya/MPxToolCommand.h>
#include <maya/MSyntax.h>

#include "groomlab/scalp_paint.h"
#include "groomlab/stroke.h"

namespace groomlab::maya {

class GroomShape;

// A file's bytes, shared.
using FileBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

// A texture map's file as the brush paints it: the map's values (a TexturePaint) whose PNG the
// file holds after each stroke on them, and after each undo and redo of those strokes (each
// stroke's edit, its changed tiles, applied backward or forward). brush_tool.cpp.
struct PaintedTexture;

// A texture stroke's undo: the stroke's tiles on the paint they apply to (at revisionBefore and
// revisionAfter of it), and what the file held before the stroke: the PNG of another paint at a
// revision of it (`beforePainted`), or bytes (`beforeBytes`: a file the brush did not write).
struct TextureUndo {
    std::shared_ptr<PaintedTexture> painted;
    TextureEdit edit;
    std::uint64_t revisionBefore = 0, revisionAfter = 0;
    FileBytes beforeBytes;
    std::shared_ptr<PaintedTexture> beforePainted;
    std::uint64_t beforeRevision = 0;
    std::size_t bytes() const;  // what the undo keeps (the paints are the brush's working images)
};

// groomLabBrushStroke: applies one brush stroke to a groom's guides (GT-1).
//
//   groomLabBrushStroke -mode comb|smooth|lift|length|shape|paint|clump|twist|curl|
//                             straighten|attract|erase|place|draw|part [-partName n]
//       [-tubeChannel width|flatness|twist]          (shape: what it paints on the tubes)
//       [-targets guides,parts,flow]                 (what a brush acts on; default guides)
//       [-radius r] [-strength s] [-level l | -group g] [-propagate true]
//       [-selectedOnly true] [-symmetry x|y|z|none [-symmetryCenter c]] [-collide true]
//       [-attribute name -value v]                  (paint; clump paints "clump")
//       [-length l -segments n -tubeRadius r -matchNeighbors true -spacing s]  (place, draw)
//       -point x y z [-point x y z ...] groom
//
// Scripted strokes (groomlab::scriptedStroke) use world-space brush spheres of `radius` along the
// points. The interactive tool (groomLabBrushCtx) records its strokes with this command, so each
// stroke is one undo step (GT-3): the StrokeEdit's texts set through an MDGModifier, a texture
// stroke's tiles (or a scripted one's file bytes) written back by undo and redo.
class BrushStrokeCmd : public MPxToolCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    BrushStrokeCmd();
    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return true; }
    MStatus finalize() override;

    // Used by the context: an interactive stroke's edit (its texts after the stroke).
    void setInteractiveEdit(const MDagPath& groom, const StrokeEdit& edit);
    // Painting a texture map: its file rewritten by the stroke (already written; `before` and
    // `after` are the file's bytes, `before` empty when there was none), and the groom told to
    // read it again. `scalpMapsJson`, when given, is the groom's scalp maps after the stroke (the
    // map moved to its own copy of a file another groom shares).
    void setTextureBytes(const MDagPath& groom, const std::string& path, FileBytes before, FileBytes after,
                         const std::string& scalpMapsJson = {});
    // The same by tiles (an interactive stroke): undo applies the edit backward and writes the
    // file the paint (or the paint before) makes, redo forward; the file is written already.
    void setTextureEdit(const MDagPath& groom, const std::string& path, TextureUndo undo, const std::string& scalpMapsJson = {});
    ~BrushStrokeCmd() override;
    // The bytes the texture strokes in the undo queue keep (their tiles, or files' bytes).
    static std::size_t heldTextureBytes();

private:
    friend class ScriptedStrokeHost;

    MDagPath groom_;
    BrushToolSettings settings_;
    std::string mode_;  // journaled
    std::vector<Vec3f> path_;
    bool interactive_ = false;
    MDGModifier modifier_;
    std::string texturePath_;  // a texture map's file, rewritten by redo (after) and undo (before)
    FileBytes textureBefore_, textureAfter_;
    std::unique_ptr<TextureUndo> textureUndo_;  // an interactive stroke's tiles (instead of the bytes)
    bool textureWritten_ = false;  // the file holds `after` now
    std::size_t heldBytes_ = 0;  // counted in heldTextureBytes()
    void holdBytes(std::size_t bytes);
    void writeTextureBefore();
    void writeTextureAfter();
    void addTexts(const StrokeEdit& edit);
    void bumpBrushTick();
};

class ContextStrokeHost;

// Interactive brush: press on the scalp and drag (BrushTool). The groom is the active one (the
// pinned groom, else the selected one).
class BrushContext : public MPxContext {
public:
    BrushContext();
    ~BrushContext() override;

    void getClassName(MString& name) const override;  // "groomLabBrush": Tool Settings MEL procs
    void toolOnSetup(MEvent& event) override;
    void toolOffCleanup() override;

    MStatus doPress(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doDrag(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doRelease(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doPtrMoved(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;

    // Runs a whole stroke through the interactive path from view coordinates (testing aid).
    // Returns true if the stroke changed the guides. `draw`: the viewport redrawn after the press
    // and after each point, every time (a drag skips a redraw while the last one took longer than
    // the time since it ended; a script's points come at once, so it would skip them all), each
    // step timed: debugStrokeTimes.
    bool simulateStroke(const std::vector<std::pair<short, short>>& points, bool ctrl = false, bool shift = false,
                        bool draw = false, bool adjust = false);
    // A Shift drag (or click, one point) through the given view points, as the mouse would: the
    // selection painted (a click adds the guides in the ring; on none, deselects and unfreezes
    // all). Returns how many guides are selected after it.
    int simulateSelectPaint(const std::vector<std::pair<short, short>>& points);
    // The last drawn simulateStroke's steps in milliseconds, each to its redrawn frame: the press,
    // every point after it, the release (benchmarks: -debugStrokeTimes).
    const std::vector<double>& debugStrokeTimes() const { return debugStrokeTimes_; }
    // ... and each step's breakdown: the groom's evaluation in compute and the viewport update
    // (groomLabGraphStats -drawTimes' JSON), a JSON list (-debugStrokeDraws).
    std::string debugStrokeDraws() const;
    // Shift+click: the guide under the cursor soloed (BrushTool::soloAt, in the release) and its
    // level brushed (BrushTool::pickLevel), said in
    // the viewport. Returns the level, or -2 when no guide is there.
    int pickLevel(short x, short y);
    // Hovering at (x, y): the guides the brush would touch (testing aid: -debugHover).
    std::vector<std::vector<Vec3f>> affectedLines(short x, short y);
    // B / M held (the GroomLab hotkeys): a left drag sets the radius or the strength, as
    // middle-drag does. "radius", "strength" or empty.
    std::string dragSlider;

    BrushToolSettings& settings() { return tool_.settings; }
    // The Length Paths tool (Tool::Lengths): an edit by words (-lengthEdit: "key 2.5", "reach 1",
    // "smooth on", "change trim", "select 0 1", "newPath", "deleteKey", "deletePath", "apply",
    // "cancel"; throws std::invalid_argument for others) and its state as JSON (-lengthState).
    void lengthEdit(const std::string& words);
    std::string lengthState();
    // The mode the tool is set to (-mode's query): during a Shift+drag's smoothing still the
    // tool's own mode, so nothing watching the tool (Groom Tools) mistakes the stroke for a
    // switch to the Smooth brush.
    std::string toolModeName() const { return tool_.toolModeName(); }

private:
    friend class ContextStrokeHost;

    // The events' work; the overrides above call these and keep exceptions from reaching Maya.
    MStatus press(MEvent& event, MHWRender::MUIDrawManager& draw);
    MStatus drag(MEvent& event, MHWRender::MUIDrawManager& draw);
    MStatus release(MEvent& event);
    MStatus guarded(const char* what, const std::function<MStatus()>& fn);
    bool findGroom();
    void endStroke(const ToolView& view);
    // Before a stroke ends: when the GPU preview drew its dabs (the guides it commits), the groom
    // may evaluate the commit off the UI thread (GroomShape::expectCommit).
    void expectGpuCommit();
    // No stroke going on (it ended, or a press began none): a stroke chained on a pending commit
    // (GroomShape::chainStroke) is over.
    void unchainUnlessStroking();
    void drawCircle(MHWRender::MUIDrawManager& draw, short x, short y) const;
    void drawAffected(MHWRender::MUIDrawManager& draw, short x, short y);
    void drawLengths(MHWRender::MUIDrawManager& draw);  // the Length Paths tool's paths and keys
public:
    // Symmetry on: the scalp's line of symmetry (symmetrySeam) handed to the groom, which draws it
    // in red on every redraw (GroomShape::setSymmetrySeam), as Substance Painter shows it: from the
    // moment the tool is on or its symmetry changes, not only while the cursor moves. Off, another
    // groom, the tool put away: the line goes.
    void showSymmetry();

    // The Shape brush on a groom: its tubes drawn as wires (GroomShape::setTubeWires) while it is;
    // another tool, another groom, the tool put away: they go.
    void showTubeWires();

private:
    void clearSymmetry();
    void clearTubeWires();
    MObjectHandle tubeWiresOn_;  // the groom shape drawing them
    // Dragging: the viewport is redrawn after a dab unless the last redraw took longer than the
    // time since it ended (the events come faster than the groom evaluates and draws); the
    // release always redraws.
    void refreshDuringDrag();

    BrushTool tool_;
    std::unique_ptr<ContextStrokeHost> host_;
    MDagPath groom_;
    GroomShape* shape_ = nullptr;
    std::string warnedReference_;  // the referenced groom a stroke was refused on (warned once)
    // The texture map being painted: kept between strokes while its file is what the last stroke
    // wrote (opened again from the file otherwise).
    std::shared_ptr<PaintedTexture> painted_;
    std::chrono::steady_clock::time_point lastRefreshEnd_{};
    double lastRefreshMs_ = 0.0;
    std::vector<double> debugStrokeTimes_;
    std::vector<std::string> debugStrokeDraws_;

    // The symmetry line drawn, kept until the scalp (a new mesh each deformed frame) or the
    // symmetry changes.
    struct Seam {
        MeshPtr scalp;
        int axis = -1;
        float center = 0.0f;
        bool topology = false;
        std::vector<Vec3f> points;
    } seam_;
    MObjectHandle seamOn_;  // the groom shape the line was handed to
    bool active_ = false;   // the tool is on (between toolOnSetup and toolOffCleanup)

    // Middle-drag (or B / M + drag) adjustment of radius / strength.
    bool adjusting_ = false;
    bool adjustStrength_ = false;
    short adjustX_ = 0, adjustY_ = 0;
    float adjustRadius_ = 0.0f, adjustValue_ = 0.0f;

    // Shift (a guide brush: BrushToolSettings::shiftSelects): selecting guides without leaving the
    // brush. A drag paints the selection with the brush ring (each guide it passes over added,
    // shown live), the release one undo step with the rest frozen (as the Select tool's solo); a
    // click adds the guides in the ring, the rest frozen (on none: nothing selected, every guide
    // unfrozen).
    bool painting_ = false, paintMoved_ = false;
    short paintX_ = 0, paintY_ = 0;
    std::vector<GuideId> paintBefore_;  // the selection at the press
    std::set<GuideId> paintedGuides_;  // the guides the ring passed over
    void paintSelection(short x, short y);
    void beginPaintSelection(short x, short y);
    void movePaintSelection(short x, short y);
    void finishPaintSelection();
};

// groomLabBrushCtx: creates and edits brush contexts.
//   groomLabBrushCtx -e -mode comb -radius 60 -strength 1 -level 0 ctxName
// -radius is in pixels here; -length/-segments/-tubeRadius configure Place.
// -debugStroke "x y x y ..." (edit) runs a stroke from view coordinates
// through the interactive code path, for tests in a Maya UI session.
class BrushContextCmd : public MPxContextCommand {
public:
    static const char* kName;
    static void* creator();

    MPxContext* makeObj() override;
    MStatus appendSyntax() override;
    MStatus doEditFlags() override;
    MStatus doQueryFlags() override;

private:
    BrushContext* context_ = nullptr;
};

}  // namespace groomlab::maya
