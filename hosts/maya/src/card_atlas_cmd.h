// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

namespace groomlab::maya {

// `groomLabCardAtlas <groom> [<groom>...]`: a Card Atlas (card_blocks.h) whose blocks are the
// grooms named, in order (each its cardBlock attribute: its card, its cell, the atlas' settings).
//   -maps <folder> [-name n] [-preset p] [-resolution r] [-format f] [-bitDepth b] [-padding px]
//         [-supersample s] [-nameTemplate t] [-occlusion o]
//       every block's groom evaluated at full density and baked into its cell of one texture
//       set, written as the preset (a name, or its JSON) packs it (the atlas' own preset and name
//       by default). Returns the paths.
//   -preview <folder> [-resolution r] [-occlusion o]
//       the same baked small (512 by default) and written as one PNG a map (preview_<Map>.png),
//       for the Card Atlas window. Returns the paths.
//   -shapes <file.fbx> [-segments n] [-across n] [-bend deg] [-twist deg] [-meters]
//       the blocks' cards as UV-ready meshes, one per block (card_blocks.h cardBlockShapes).
// Changes nothing in the scene.
class CardAtlasCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();
    MStatus doIt(const MArgList& args) override;
};

}  // namespace groomlab::maya
