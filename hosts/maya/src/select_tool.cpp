// SPDX-License-Identifier: Apache-2.0
#include "select_tool.h"

#include <algorithm>
#include <set>
#include <string>

#include <maya/M3dView.h>
#include <maya/MArgParser.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MPoint.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>
#include <maya/MUIDrawManager.h>

#include "guarded.h"
#include "active_groom.h"
#include "groom_shape.h"
#include "maya_inputs.h"
#include "groomlab/brush.h"
#include "groomlab/stroke.h"
#include "groomlab/guide_selection.h"
#include "tool_view.h"

namespace groomlab::maya {

namespace {

const char* kLevelFlag[] = {"-l", "-level"};
const char* kGroupFlag[] = {"-g", "-group"};
const char* kRootsOnlyFlag[] = {"-ro", "-rootsOnly"};
const char* kDebugLassoFlag[] = {"-dbl", "-debugLasso"};
const char* kDebugModeFlag[] = {"-dbm", "-debugMode"};
const char* kSoloSelectionFlag[] = {"-sos", "-soloSelection"};
const char* kSymmetryFlag[] = {"-sym", "-symmetry"};

std::string modeFor(const MEvent& event) {
    const bool shift = event.isModifierShift();
    const bool ctrl = event.isModifierControl();
    if (shift && ctrl) return "add";
    if (shift) return "toggle";
    if (ctrl) return "deselect";
    return "replace";
}

}  // namespace

GuideSelectContext::GuideSelectContext() { setTitleString("GroomLab Select Guides"); }

void GuideSelectContext::getClassName(MString& name) const { name = "groomLabSelect"; }

void GuideSelectContext::toolOnSetup(MEvent&) {
    setHelpString("Lasso or click guides of the selected groom: the rest freeze, so brushes work on the selection "
                  "(nothing picked: all unfrozen). Shift: toggle, Ctrl: deselect, Ctrl+Shift: add.");
}

MStatus GuideSelectContext::doPress(MEvent& event, MHWRender::MUIDrawManager&, const MHWRender::MFrameContext&) {
    return groomlab::maya::guarded("GuideSelectContext::doPress", [&]() -> MStatus {
        short x = 0, y = 0;
        event.getPosition(x, y);
        lasso_ = {{float(x), float(y)}};
        mode_ = modeFor(event);
        return MS::kSuccess;
    });
}

MStatus GuideSelectContext::doDrag(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext&) {
    return groomlab::maya::guarded("GuideSelectContext::doDrag", [&]() -> MStatus {
        short x = 0, y = 0;
        event.getPosition(x, y);
        lasso_.push_back({float(x), float(y)});
        draw.beginDrawable();
        draw.setColor(MColor(1.0f, 0.85f, 0.2f));
        draw.setLineWidth(1.5f);
        for (std::size_t i = 0; i + 1 < lasso_.size(); ++i) {
            draw.line2d(MPoint(lasso_[i][0], lasso_[i][1]), MPoint(lasso_[i + 1][0], lasso_[i + 1][1]));
        }
        draw.endDrawable();
        return MS::kSuccess;
    });
}

MStatus GuideSelectContext::doRelease(MEvent&, MHWRender::MUIDrawManager&, const MHWRender::MFrameContext&) {
    return groomlab::maya::guarded("GuideSelectContext::doRelease", [&]() -> MStatus {
        select(lasso_, mode_);
        lasso_.clear();
        // The selection drawn now, as the brushes' release does (BrushContext::endStroke): testers
        // saw a marquee, a Shift pick and a click on nothing wait for the camera to move, though
        // the command schedules a redraw.
        M3dView::active3dView().refresh(true, true);
        return MS::kSuccess;
    });
}

int GuideSelectContext::select(const std::vector<std::array<float, 2>>& lasso, const std::string& mode) {
    GuidePick pick;
    pick.level = level;
    pick.group = group;
    pick.rootsOnly = rootsOnly;
    pick.clickPixels = clickPixels;
    pick.solo = soloSelection;
    pick.symmetry = symmetry;
    return selectGuidesInView(lasso, mode, pick);
}

std::vector<GuideId> pickGuidesInView(const std::vector<std::array<float, 2>>& lasso, const GuidePick& pick) {
    const int level = pick.level;
    const std::string& group = pick.group;
    const bool rootsOnly = pick.rootsOnly;
    const float clickPixels = pick.clickPixels;
    const std::string& symmetry = pick.symmetry;
    MDagPath groom;
    if (!activeGroom(groom)) return {};  // the pinned groom, else the selected one
    auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(groom).userNode());
    const HierarchyPtr h = shape ? shape->evaluatedHierarchy() : nullptr;
    if (!h) return {};

    // Guides are world space (the groom draws in world space, its transform locked).
    // Hidden groups can't be picked.
    const std::vector<GuideId> candidates = guidesAt(*h, level, group, readHiddenGuideGroups(groom.node()));
    const SelectionProjector view = selectionProjector(M3dView::active3dView());
    // Only what the view shows: guide points behind the scalp are not picked.
    const ViewProjector visible = visibleProjector(view.project, scalpVisibility(shape->evaluatedScalp(), activeToolView()));
    std::vector<GuideId> picked = pickGuides(*h, candidates, visible, lasso, rootsOnly, clickPixels,
                                             view.matrix.valid() ? &view.matrix : nullptr);
    if (symmetry != "none" && !picked.empty()) {  // each picked guide's mirror guide too
        BrushToolSettings mirror;
        try {
            mirror.setSymmetry(symmetry);
        } catch (const std::invalid_argument&) {
        }
        const MeshPtr scalp = shape->evaluatedScalp();
        picked = withMirrorGuides(*h, picked, StrokeSymmetry::of(mirror.brush, scalp.get()));
    }
    return picked;
}

bool applyGuideSelection(const std::vector<GuideId>& ids, const std::string& mode, bool solo, bool undoable) {
    MDagPath groom;
    if (!activeGroom(groom)) {  // the pinned groom, else the selected one
        MGlobal::displayWarning("GroomLab select: select a groom (groomLabShape) first");
        return false;
    }
    if (ids.empty() && mode != "replace") return false;
    MString cmd = "groomLabGuides -select";
    if (mode == "add") cmd += " -append";
    if (mode == "toggle") cmd += " -toggle";
    if (mode == "deselect") cmd += " -deselect";
    if (ids.empty()) cmd = "groomLabGuides -clearSelection";
    if (solo) cmd += " -freezeOthers";  // the rest frozen (nothing picked: all unfrozen)
    for (const GuideId id : ids) cmd += MString(" -guide \"") + std::to_string(id).c_str() + "\"";
    cmd += MString(" \"") + groom.fullPathName() + "\"";
    MGlobal::executeCommand(cmd, false, undoable);
    return true;
}

int selectGuidesInView(const std::vector<std::array<float, 2>>& lasso, const std::string& mode, const GuidePick& pick) {
    MDagPath groom;
    if (!activeGroom(groom)) {
        MGlobal::displayWarning("GroomLab select: select a groom (groomLabShape) first");
        return 0;
    }
    const std::vector<GuideId> picked = pickGuidesInView(lasso, pick);
    applyGuideSelection(picked, mode, pick.solo, true);
    return static_cast<int>(picked.size());
}

// ---------------------------------------------------------------------------

const char* GuideSelectContextCmd::kName = "groomLabSelectCtx";

void* GuideSelectContextCmd::creator() { return new GuideSelectContextCmd(); }

MPxContext* GuideSelectContextCmd::makeObj() {
    context_ = new GuideSelectContext();
    return context_;
}

MStatus GuideSelectContextCmd::appendSyntax() {
    MSyntax s = syntax();
    s.addFlag(kLevelFlag[0], kLevelFlag[1], MSyntax::kLong);
    s.addFlag(kGroupFlag[0], kGroupFlag[1], MSyntax::kString);
    s.addFlag(kRootsOnlyFlag[0], kRootsOnlyFlag[1], MSyntax::kBoolean);
    s.addFlag(kDebugLassoFlag[0], kDebugLassoFlag[1], MSyntax::kString);
    s.addFlag(kDebugModeFlag[0], kDebugModeFlag[1], MSyntax::kString);
    s.addFlag(kSoloSelectionFlag[0], kSoloSelectionFlag[1], MSyntax::kBoolean);
    s.addFlag(kSymmetryFlag[0], kSymmetryFlag[1], MSyntax::kString);
    return MS::kSuccess;
}

MStatus GuideSelectContextCmd::doEditFlags() {
    return groomlab::maya::guarded("GuideSelectContextCmd::doEditFlags", [&]() -> MStatus {
        if (!context_) return MS::kFailure;
        MArgParser args = parser();
        MString text;
        int n = 0;
        bool b = false;
        if (args.isFlagSet(kLevelFlag[0]) && args.getFlagArgument(kLevelFlag[0], 0, n)) context_->level = n;
        if (args.isFlagSet(kGroupFlag[0]) && args.getFlagArgument(kGroupFlag[0], 0, text)) context_->group = text.asChar();
        if (args.isFlagSet(kRootsOnlyFlag[0]) && args.getFlagArgument(kRootsOnlyFlag[0], 0, b)) context_->rootsOnly = b;
        if (args.isFlagSet(kSoloSelectionFlag[0]) && args.getFlagArgument(kSoloSelectionFlag[0], 0, b)) context_->soloSelection = b;
        if (args.isFlagSet(kSymmetryFlag[0]) && args.getFlagArgument(kSymmetryFlag[0], 0, text)) {
            try {
                BrushToolSettings check;
                check.setSymmetry(text.asChar());
                context_->symmetry = check.symmetryName();
            } catch (const std::invalid_argument& e) {
                MGlobal::displayError(MString("groomLabSelectCtx: ") + e.what());
                return MS::kFailure;
            }
        }
        if (args.isFlagSet(kDebugLassoFlag[0]) && args.getFlagArgument(kDebugLassoFlag[0], 0, text)) {
            MStringArray words;
            text.split(' ', words);
            std::vector<std::array<float, 2>> lasso;
            for (unsigned i = 0; i + 1 < words.length(); i += 2) lasso.push_back({words[i].asFloat(), words[i + 1].asFloat()});
            MString mode = "replace";
            if (args.isFlagSet(kDebugModeFlag[0])) args.getFlagArgument(kDebugModeFlag[0], 0, mode);
            setResult(context_->select(lasso, mode.asChar()));
        }
        return MS::kSuccess;
    });
}

MStatus GuideSelectContextCmd::doQueryFlags() {
    return groomlab::maya::guarded("GuideSelectContextCmd::doQueryFlags", [&]() -> MStatus {
        if (!context_) return MS::kFailure;
        MArgParser args = parser();
        if (args.isFlagSet(kLevelFlag[0])) setResult(context_->level);
        else if (args.isFlagSet(kGroupFlag[0])) setResult(MString(context_->group.c_str()));
        else if (args.isFlagSet(kRootsOnlyFlag[0])) setResult(context_->rootsOnly);
        else if (args.isFlagSet(kSoloSelectionFlag[0])) setResult(context_->soloSelection);
        else if (args.isFlagSet(kSymmetryFlag[0])) setResult(MString(context_->symmetry.c_str()));
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
