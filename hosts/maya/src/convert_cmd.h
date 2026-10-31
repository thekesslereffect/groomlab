// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <maya/MObjectHandle.h>
#include <maya/MPxCommand.h>
#include <maya/MString.h>
#include <maya/MSyntax.h>

#include "groomlab/math.h"
#include "groomlab/strand_geometry.h"

namespace groomlab::maya {

// groomLabConvert: a groom's guides or strands as plain Maya geometry (GroomLab > Utilities),
// built in one go through the API (20,000 curves in a few seconds), one undo step
// (undo deletes what it made, redo makes it again). One action per call:
//
//   -guideCurves/-gcv       the guides (as authored, on the current scalp) as NURBS curves, one
//                           per guide, under a group transform; each curve's transform carries
//                           glGuideId (the guide ID) and glGuideGroup (string attributes).
//                           Which: -guide/-gid <id> (multi-use), else -group/-g <group>, else all.
//                           -flat/-fl: every curve right under the group (else a transform per
//                           guide group, nested as the groups are).
//   -strandCurves/-scv      a graph node's strands (or guides) as curves: -node/-nd <name> (""
//                           or absent: the groom's output), at -density/-dn (0-1, default 1),
//                           at most -maxCount/-mc curves (default 20000; thinned by strand ID,
//                           the ones a lower density keeps). -attributes/-at: each curve's shape
//                           carries glStrandId (string), glWidth (a double array, per point) and
//                           the strands' channels (gl<Channel>: a double per strand channel, a
//                           double array per point channel).
//   -strandMesh/-smh        the groom as one polygon mesh (strand_geometry.h): -make/-mk
//                           auto|strands|scales|feathers (auto: scales, else feathers, else the
//                           strands), -style/-st tube|ribbon, -sides/-sds n (tubes),
//                           -widthScale/-ws s, -strandUVs/-suv (a second UV set "strandUVs": a
//                           grid cell per strand), -node, -density, -maxCount (strands; scales
//                           and feathers are never thinned).
//   -degree/-dg 1|3         the curves' degree (default 1: the CVs are the points; 3: a
//                           cubic through them, the points its edit points)
//   -name/-n <name>         the group (curves) or mesh transform's name
//
// The groom is the command's object (or the selection). Returns [the new group or mesh
// transform (full path), how many curves or strands were made, how many there were].
class ConvertCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return undoable_; }

private:
    struct Group {
        MString name;
        int parent = -1;  // index into groups_; -1: the root
    };
    struct Curve {
        std::vector<Vec3f> points;
        int group = -1;
        MString name;
        std::vector<std::pair<MString, MString>> strings;              // on the transform (tagsOnShape_: the shape)
        std::vector<std::pair<MString, double>> numbers;               // on the shape
        std::vector<std::pair<MString, std::vector<double>>> arrays;   // on the shape
    };

    MStatus build();

    bool undoable_ = false;
    MString rootName_;
    std::vector<Group> groups_;
    std::vector<Curve> curves_;
    int degree_ = 1;
    bool mesh_ = false;
    StrandMesh strandMesh_;
    MObjectHandle root_;
    bool tagsOnShape_ = false;  // string tags on the curve's shape (strands) or its transform (guides)
    std::size_t made_ = 0, total_ = 0;
};

}  // namespace groomlab::maya
