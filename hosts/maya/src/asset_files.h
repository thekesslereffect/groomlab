// SPDX-License-Identifier: Apache-2.0
#pragma once

// The files GroomLab writes for a groom (texture maps, card and scalp bakes): where they go,
// how they are named, and keeping one groom's writes out of another groom's files.
//
// Layout (groomlab.asset_folder on the Python side says the same):
//   <scene folder>/groomlab/<scene name>/<kind>      (kind: masks, cards, exports, caches, renders)
//   <project>/groomlab/untitled/<kind>                while the scene is untitled
// A groom's files are named by its asset name: the transform's full DAG path made file safe
// (|charA|hair and charA:hair both give charA_hair), e.g. masks/charA_hair/density.png.

#include <cstdint>
#include <string>
#include <vector>

#include <maya/MDagPath.h>
#include <maya/MObject.h>

namespace groomlab::maya {

// Text safe in a file name: every character outside [A-Za-z0-9_.-] becomes '_' (so ':' and
// '|' never reach a path), leading '_' from a leading '|' dropped, trailing dots dropped.
std::string fileSafeName(const std::string& text);
// The groom's asset name (the shape or its transform): its transform's full path, file safe.
std::string assetName(const MDagPath& groom);
// The scene's folder for `kind` (absolute, forward slashes; not created).
std::string assetFolder(const std::string& kind);

// Whether a groomLabShape other than `groom` names the file `full` (resolved): as a texture
// map, or as one of its card bake's files (every file of the bake's prefix).
bool fileUsedByOtherGroom(const MObject& groom, const std::string& full);
// Whether another groom's card bake has the file prefix `prefix` (<folder>/<name>_, resolved).
bool bakePrefixUsedByOtherGroom(const MObject& groom, const std::string& prefix);

// A texture map's file shared with another groom, copied for this one before it is written:
// into this groom's masks folder as <map>.png (unique). Returns the copy's path (absolute);
// throws when the copy fails.
std::string forkTextureFile(const MDagPath& groom, const std::string& mapName, const std::string& full);

// A file's bytes (empty when it cannot be read), and the bytes written back (commands keep a
// file's bytes to undo and redo what they wrote). putFileBytes warns under `who` when it
// cannot write.
std::vector<std::uint8_t> fileBytes(const std::string& path);
void putFileBytes(const std::string& path, const std::vector<std::uint8_t>& bytes, const char* who);

// A count of the files the plugin's commands wrote (putFileBytes, painted and copied maps, undo
// and redo of them): what kept a file's bytes in memory may trust them only while it stands
// where it was (a file's stamp alone can read the same after a quick rewrite of the same size).
std::uint64_t fileWrites();
void noteFileWritten();

}  // namespace groomlab::maya
