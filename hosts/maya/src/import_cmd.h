// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <maya/MDGModifier.h>
#include <maya/MDagModifier.h>
#include <maya/MObjectHandle.h>
#include <maya/MPxCommand.h>
#include <maya/MString.h>
#include <maya/MSyntax.h>

#include "groomlab/groom_file.h"

namespace groomlab::maya {

// groomLabImportGroom: a groom file (.groom, groom_file.h) into Maya, the inverse of
// groomLabExportGroom. The file is read and turned into the scene's up axis (convertUpAxis);
// only centimeter files are read (Maya's internal unit). One action per call:
//   -file/-f path -info/-i         returns JSON of what the file holds, for groomlab.import_groom:
//                                  {"upAxis", "units", "fileUpAxis", "framesPerSecond", "source",
//                                   "followScalp", "directory", "searchPaths",
//                                   "scalp": {"points", "triangles", "rest", "uvs", "colorSets"} | null,
//                                   "maps": {name: {"kind", "texture" (resolved), "channels", "found"}},
//                                   "controls": {name: value},
//                                   "scalpCache" / "guideCache": {"file" (resolved), "object", "found"} | null,
//                                   "cardBake": {"prefix" (resolved), "strands", "found"} | null,
//                                   "correctives": [{"name", "slot", "weightCurve": [[seconds, w]...]}],
//                                   "flatGuides": [{"id", "points": [x, y, z, ...]}]}
//   -file path -checkScalp/-cs mesh
//                                  returns "" when the mesh (its world points) is the file's scalp
//                                  as the guides are rooted on it (as many vertices, the same
//                                  triangles), else why not
//   -file path -makeScalp/-ms [-name/-n name] [-rest/-r]
//                                  makes a mesh of the file's scalp (its rest pose with -rest): its
//                                  triangles as faces, its UVs, its color sets that are not painted
//                                  maps; returns the transform. Undo deletes it.
//   -file path [-caches/-cch on|off] groom
//                                  replaces the groom's graph, guides (guideData), parts, flow,
//                                  correctives (their weight curves left to groomlab.import_groom,
//                                  which keys the weights), painted maps (scalpMaps: vertex maps
//                                  from the scalp's color sets; texture maps' images copied into the
//                                  groom's masks folder, or linked where they are when they are
//                                  that folder's), followScalp and, when the file names a bake, its
//                                  card bake (copied into the groom's cards folder, cardMaps); links
//                                  the groom to the file (groomFile). -caches (on by default) also
//                                  links the file's guide cache. The groom's scalp must be the
//                                  file's (as -checkScalp). One undo step (the files it copied
//                                  removed on undo). Returns JSON: {"copied": [paths], "missing":
//                                  [map names whose image was not found], "bake": prefix or ""}.
class ImportGroomCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return undoable_; }

private:
    MStatus makeScalp();

    bool undoable_ = false;
    // Applying the file to a groom: the attributes, and the files copied for it (written on redo,
    // removed on undo).
    MDGModifier modifier_;
    struct Written {
        std::string path;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Written> written_;
    // Making a scalp mesh: the modifier that made its nodes (undo takes them out, redo puts the
    // same nodes back, so later commands of the same undo step still name them), the transform.
    bool making_ = false;
    std::shared_ptr<const ScalpMesh> mesh_;
    std::vector<std::string> colorSets_;
    // An extra scalp (one of the file's "scalps"): made of its polygons only when Maya splits them
    // as the file does (the roots on it stay on their triangles), else of its triangles.
    bool exactPolygons_ = false;
    MString name_;
    MDagModifier scalpModifier_;
    MObjectHandle made_;
};

}  // namespace groomlab::maya
