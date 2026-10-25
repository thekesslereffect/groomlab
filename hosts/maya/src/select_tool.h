// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string>
#include <vector>

#include <maya/MDagPath.h>
#include <maya/MPxContext.h>
#include <maya/MPxContextCommand.h>

#include "groomlab/guide_hierarchy.h"

namespace groomlab::maya {

// What a pick of guides in the viewport takes: the level (-1: every level) or group picked from,
// the roots alone or any point, a click's reach, solo (every guide not selected frozen, nothing
// picked: all unfrozen, in the selection's undo step) and symmetry ("none", "x" or "topology": each
// picked guide's mirror guide too).
struct GuidePick {
    int level = 0;
    std::string group;
    bool rootsOnly = false;
    float clickPixels = 12.0f;
    bool solo = true;
    std::string symmetry = "none";
};
// Selects the active groom's guides in a lasso (or, with one point, a click) in view coordinates,
// as the Select tool does: mode "replace", "add", "toggle" or "deselect", through `groomLabGuides
// -select` (one undo step). Returns the number of guides the lasso caught. The Select tool's
// clicks and lassos, and a brush's Ctrl+Shift ones (BrushContext).
int selectGuidesInView(const std::vector<std::array<float, 2>>& lasso, const std::string& mode, const GuidePick& pick);
// The guides of the active groom a lasso (or click) in view coordinates catches, as
// selectGuidesInView picks them (nothing selected).
std::vector<GuideId> pickGuidesInView(const std::vector<std::array<float, 2>>& lasso, const GuidePick& pick);
// The active groom's selection set from `ids` by `mode` (as selectGuidesInView applies a pick):
// `undoable` false for a live preview (a brush's Ctrl+Shift painting) that an undoable one replaces.
bool applyGuideSelection(const std::vector<GuideId>& ids, const std::string& mode, bool solo, bool undoable);

// Guide selection tool (GT-5): drag a lasso in the viewport around guides of
// the selected groom, or click near one. Plain: replace the selection;
// Shift: toggle; Ctrl: deselect; Ctrl+Shift: add. The result goes through
// `groomLabGuides -select`, so every change is one undo step.
class GuideSelectContext : public MPxContext {
public:
    GuideSelectContext();
    void getClassName(MString& name) const override;  // "groomLabSelect"
    void toolOnSetup(MEvent& event) override;

    MStatus doPress(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doDrag(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doRelease(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;

    // Selects from a lasso (or, with one point, a click) in view coordinates,
    // as the mouse would. mode: "replace", "add", "toggle" or "deselect".
    // Returns the number of guides the lasso caught.
    int select(const std::vector<std::array<float, 2>>& lasso, const std::string& mode);

    int level = 0;           // hierarchy level picked from (-1: every level)
    std::string group;       // or this group
    bool rootsOnly = false;  // lasso the roots only, not any point of a guide
    float clickPixels = 12.0f;
    // Solo: each pick freezes every guide not selected (nothing picked: all unfrozen), in the
    // selection's undo step (groomLabGuides -freezeOthers).
    bool soloSelection = true;
    // Symmetry ("none", "x" or "topology", as the brushes name it): a pick takes each picked
    // guide's mirror guide too.
    std::string symmetry = "none";

private:
    std::vector<std::array<float, 2>> lasso_;
    std::string mode_ = "replace";
};

// groomLabSelectCtx: creates and edits guide selection contexts.
//   groomLabSelectCtx -e [-level l | -group g] [-rootsOnly b] [-soloSelection b] [-symmetry s] ctxName
//   groomLabSelectCtx -e -debugLasso "x y x y ..." [-debugMode add] ctxName   (tests)
class GuideSelectContextCmd : public MPxContextCommand {
public:
    static const char* kName;
    static void* creator();
    MPxContext* makeObj() override;
    MStatus appendSyntax() override;
    MStatus doEditFlags() override;
    MStatus doQueryFlags() override;

private:
    GuideSelectContext* context_ = nullptr;
};

}  // namespace groomlab::maya
