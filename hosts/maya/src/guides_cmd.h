// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <maya/MDGModifier.h>
#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

namespace groomlab::maya {

// groomLabGuides: edits a groom's guide hierarchy (stored in its guideData
// attribute). One action per call; every edit is undoable.
//
//   -fromCurves/-fc <curve> (multi-use) [-group g] [-tubeRadius r] [-tubeTipRadius r]
//        adds the curves as primary guides to root group g (default "primary")
//   -fromNode/-fnd <graph node> [-group g] [-density d] [-tubeRadius r] [-tubeTipRadius r]
//        bakes the node's output (Guides, e.g. ToGuides; or Strands, one
//        guide per strand), evaluated at density d (default 1), as primary guides of
//        root group g (created if missing); returns the new guide IDs
//   -generateChildren/-gc <parentGroup> [-name n] [-count c] [-rootSpread s]
//        [-tipSpread s] [-twist t] [-jitter j] [-tubeScale s] [-seed n] [-split true]
//        (-split: the children cut the parent's tube into equal slices, ChildGenerationParams::split)
//        fills every parent guide's tube with children in a new group
//   -removeGroup/-rg <group>     removes a group, its sub-groups and guides
//   -lock/-lk | -unlock/-ul  -guide/-gid <id> (multi-use, decimal string)
//   -listGroups/-lg              returns "name|parent|depth|guideCount|#rrggbb" per group (the color it draws in without a guideGroupColors entry)
//   -listGuides/-lsg <group>     returns guide IDs (decimal strings)
//   -guidePoints/-gp <id>        returns the guide's points on the current scalp, x y z ... (world space)
//   -clear                       removes every group and guide
//
// Guide selection (GT-5; stored in guideSelection, undoable). Targets come
// from -guide (multi-use), else -group, else -level, else the selection:
//   -select/-sel [-append/-ap | -deselect/-ds | -toggle/-tgl]    replace or edit the selection
//   -mirrorSelection/-msl x|y|z [-center c] [-append]
//   -listSelected/-lss                           returns the selected IDs
//   -clearSelection/-csl
// Hierarchy tools (shapes kept):
//   -reparent/-rp <parent id>                    targets follow another parent
//   -promote/-pro <group>                        one level up
//   -demote/-dem <group> -under/-u <group>       one level down, under a sibling group
//   -mirror/-mir <root group> [-axis x|y|z] [-center c] [-fromNegative]
//   -setAttribute/-sat <name> -value/-v <v>      per-guide attribute on the targets
//   -listAttributes/-lat                         the attribute layers any guide carries
//   -removeAttribute/-rat <name>                 removes a layer from every guide
//   -renameAttribute/-rna <old> <new>            renames a layer on every guide
//   -resample/-rsm <segments>                    evenly respaces the targets, shape kept
//   -reshape/-rsh <id> <curve> (multi-use)       guides take their curves' edit points (the CVs of a
//                                                degree 1 curve); unchanged ones are skipped, parents
//                                                go first; returns the IDs that changed
//   -setTube/-stb <json>                         edits the targets' tubes (groomlab/tube.h editedTube:
//                                                any of "width", "aspect", "twist" (radians; each a
//                                                number or a list root to tip), "shape", "samples",
//                                                "scaleWidth", "smooth" (width|aspect|twist|all, one
//                                                value per guide point) with "passes"; widths in
//                                                scene units as shown); returns the count
//   -tube/-tb <id>                               returns a guide's tube as JSON (shape, width, aspect, twist)
//   -generateChildren with -guide ids or -onlySelected: only those parents get
//     children (added to, or replacing theirs in, the child group); the parent
//     group may then be "" (taken from the guides)
//
// The groom is the command's object (or the selection).
class GuidesCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return undoable_; }

private:
    MDGModifier modifier_;
    bool undoable_ = false;
};

}  // namespace groomlab::maya
