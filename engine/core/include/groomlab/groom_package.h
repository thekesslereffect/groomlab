// SPDX-License-Identifier: Apache-2.0
#pragma once

// A groom file as a document of its own (every host writes it this way: Maya, Blender and the app
// call writeGroomsToFile, so they never drift apart). A .groom holds named grooms (groom_file.h:
// the first at the top, the others in "grooms"), and the files each groom owns live in the file's
// own folder beside it, one folder per groom:
//
//   <folder>/shot.groom
//   <folder>/groomlab/shot/masks/<groom>/      the groom's texture maps
//   <folder>/groomlab/shot/cards/<groom>/      its card bake (<groom>_Color.tga ...)
//   <folder>/groomlab/shot/caches/<groom>/     its Simulate bakes
//
// A Maya scene or a .blend saved as shot.ma / shot.blend keeps its own files in the same
// groomlab/shot/ (asset_files.h), so a groom file named after its scene shares the folder with it
// and nothing is copied twice. A groom written into a file of another name brings copies along, so
// the file stands alone. The files a groom only reads (ImportCurves' Alembic, an image a node
// names, the scalp and guide caches the host wrote beside the file) stay where they are, named
// relative to the file.

#include <string>
#include <vector>

#include "groomlab/groom_file.h"

namespace groomlab {

struct GroomWriteOptions {
    // Keep the file's other grooms: each groom written replaces the file's groom of its name (a
    // file of one unnamed groom: that one), the others stay as they are; new names are added at the
    // end. Off: the file holds the grooms written, alone.
    bool merge = false;
    // Copy the files each groom owns (texture maps, the card bake, Simulate bakes) into the file's
    // folder (off: named where they are, relative to the file).
    bool copyFiles = true;
};

struct GroomWriteReport {
    std::vector<std::string> copied;    // the files written beside the groom file (absolute)
    std::vector<std::string> warnings;  // a file missing, a copy under another name
};

// The folder a groom file keeps a groom's files of `kind` (masks, cards, caches) in:
// <file's folder>/groomlab/<file name>/<kind>/<groom> (the groom's name, file safe; "" the
// file's name: a file of one unnamed groom).
std::string groomFileFolder(const std::string& groomFilePath, const std::string& groomName, const std::string& kind);

// `grooms` (each its name in GroomFile::name; its stored paths absolute, or relative to its
// `directory` / search paths as resolveGroomPath reads them) written to `path` as one groom file:
// the files they own copied into its folder (GroomWriteOptions::copyFiles: a file already there,
// or the same bytes, kept; other content: a free name and a warning), the params of the graph that
// read a texture map by its image (the app's and Blender's painted params) pointed at the copy,
// every path stored relative to the file. Throws GroomFileError (nothing written) for a groom that
// is not a groom, two of one name, or a file it cannot write.
GroomWriteReport writeGroomsToFile(const std::string& path, std::vector<GroomFile> grooms, const GroomWriteOptions& options = {});

// The index of the groom named `name` among a file's grooms (readGroomFiles): that name; "" or no
// such name in a file of one groom: 0; else -1.
int groomIndex(const std::vector<GroomFile>& grooms, const std::string& name);

// The groom file text `existing` with the grooms of `written` (both groom files' text) in it:
// each replaces the groom of its name (a file of one unnamed groom: that one), new ones are added
// at the end, the others kept as they are. Throws GroomFileError for text that is not a groom file.
std::string mergeGroomFilesJson(const std::string& existing, const std::string& written);

}  // namespace groomlab
