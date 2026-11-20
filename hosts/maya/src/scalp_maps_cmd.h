// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <maya/MDGModifier.h>
#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

#include "groomlab/asset_files.h"

namespace groomlab::maya {

// groomLabScalpMaps: the maps painted on a groom's scalp (Groom Tools > Paint), of three kinds:
// a layer on the guides ("guides", kept in guideData), values per scalp vertex ("vertices",
// kept in scalpMaps) or an image at the scalp's UVs ("texture", a file named in scalpMaps).
// Vertex and texture maps are gray (1 channel) or color (4: r g b a); guide layers are gray.
// One action per call; the groom is the command's object (or the selection):
//   -list/-ls                         returns "name|kind|texture|channels" per map, every kind
//   -kindOf/-ko <name>                returns the map's kind ("" when there is none)
//   -channelsOf/-cho <name>           returns 1 (gray) or 4 (color), 0 when there is no such map
//   -make/-mk <name> -kind/-k <kind> [-value/-v v] [-color/-clr [-fillColor/-fc r g b a]]
//             [-resolution/-res n] [-texture/-tx path]
//                                     a new map filled with v (0); -color makes a color map
//                                     (vertices or texture), filled with the color or (v v v 1)
//   -remove/-rm <name> [-deleteFile/-df]
//                                     -deleteFile: a texture map's file is deleted too, unless
//                                     another map or groom reads it (put back on undo)
//   -rename/-rn <old> <new> [-texture/-tx path]
//                                     a texture map's file moves to path (undone with the rename);
//                                     copied instead when another groom reads that file
//   -fill/-f <name> (-value/-v <v> | -fillColor/-fc r g b a) [-fillChannels/-fch rgba] [-paintMode/-pmd mode]
//                                     a gray map takes v; a color map takes the color in the
//                                     channels given (all by default), or v in its r g b (a kept)
//   -convert/-cv <name> -kind/-k <kind> [-resolution/-res n] [-texture/-tx path]
//                                     the map resampled into another kind (its name and
//                                     channels kept; a color map onto the guides keeps its
//                                     luminance, with a warning)
//   -setChannels/-sch <name> <1|4>    a gray map made color (the value in r g b, a 1) or a color
//                                     map made gray (its luminance)
//   -values/-val <name>               returns a vertex map's values (channels per scalp vertex)
//   -relink/-rl <name> -texture/-tx path [-copyFrom/-cf image]
//                                     the map becomes a texture map reading path (a guide layer
//                                     or vertex map turns into one; channels kept, a guide layer
//                                     gray). Without -copyFrom, path is an existing PNG linked in
//                                     place; with it, the image (any format the texture loader
//                                     reads) is written as a PNG at path (a free name when taken;
//                                     removed on undo). Returns the stored path.
// Edits to the groom's attributes undo, and so does a rename's file move and a rewrite of a
// texture map's file (its bytes before and after are kept). Files are never shared by
// accident: a new file (-make, -convert to a texture) takes a free name when one is in the way
// (<stem>_2.png...), and a file another groom reads too is copied for this groom (its masks
// folder) before a -fill or -setChannels writes it, the map repointed at the copy in the same
// undo step. Only a .png is ever written (painting writes PNG bytes): a map whose file is not
// one is never filled or recolored in place.
class ScalpMapsCmd : public MPxCommand {
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
    std::pair<std::string, std::string> moved_;  // a renamed texture map's file: from, to (absolute)
    bool copied_ = false;  // the renamed map's file was copied, not moved (another groom reads it)
    // The texture files the command wrote (a map's image rewritten, -relink's copy; a tile set's
    // every tile), as they were and are (asset_files.h FileState): put back on undo, again on redo.
    std::vector<FileState> files_;
    // The files -remove -deleteFile deletes after the map goes (a tile set's every tile).
    std::vector<FileState> deleted_;
};

}  // namespace groomlab::maya
