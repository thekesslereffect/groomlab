// SPDX-License-Identifier: Apache-2.0
#pragma once

// Where every host keeps the files a scene (Maya's .ma, Blender's .blend) or a groom file (the
// app's .groom) makes for its grooms, and how they follow the file when it is saved somewhere
// else (Maya's paths.asset_folder and follow_scene_files, for the app and the Blender helper).

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace groomlab {

// The kinds of files a scene keeps, each a folder of its own: masks (texture maps), cards (card
// and scalp bakes), exports, caches (guide caches, simulation bakes), renders, references (photos).
// <base>/groomlab/<name>/<kind> (no <kind>: the scene's own folder), forward slashes: so two
// scenes in one folder (shot_v001, shot_v002) never share files. `name` is made file safe.
std::string assetFolder(const std::string& base, const std::string& name, const std::string& kind = {});

// A name as a file or folder name, as Maya's asset_name makes one: leading DAG bars dropped, every
// other character but letters, digits, _ . and - becomes _ (|charA|hair and charA:hair both
// charA_hair), no trailing dots, "groom" for nothing.
std::string fileSafeName(const std::string& text);

// Save As: the files a scene names, taken along into the new scene's folders. Each stored path:
// - a file in the old scene's asset folder (`oldRoot`: <old base>/groomlab/<old name>) goes to the
//   same place under the new one's (`newRoot`); one in an `extraRoots` folder (where an untitled
//   scene's files went, or a groom's
//   own folder taking a new name: {from, to}; looked at before the old root) to the same place
//   under its `to`;
// - another relative path keeps its place relative to the new base;
// - an absolute path elsewhere stays (the machine's files), as does a file not on disk.
// A file is copied (the old scene keeps its own), never over other content: then a free name
// (name_2...) and a warning. The new path is stored as the old one was: relative to the new base,
// or absolute.
class FileFollower {
public:
    FileFollower(std::string oldBase, std::string oldRoot, std::string newBase, std::string newRoot,
                 std::vector<std::pair<std::string, std::string>> extraRoots = {});

    // The stored path's new value (the file copied), or `stored` when it does not move.
    std::string follow(const std::string& stored);
    // A set of files sharing a prefix (a card bake: <prefix>Color.tga, <prefix>Atlas.json...): every
    // `prefix + suffix` there goes together; the new prefix (a free one, _2_..., when any of them
    // would land on other content).
    std::string followPrefix(const std::string& storedPrefix, const std::vector<std::string>& suffixes);

    const std::vector<std::string>& copied() const { return copied_; }      // the new files (absolute)
    const std::vector<std::string>& warnings() const { return warnings_; }  // copies under another name

private:
    // Where `source` (absolute, on disk) goes; "" when it stays. `relative`: stored relative.
    std::string destination(const std::string& source, const std::string& stored, bool relative) const;
    std::string stored(const std::string& path, bool relative) const;

    std::string oldBase_, oldRoot_, newBase_, newRoot_;
    std::vector<std::pair<std::string, std::string>> extraRoots_;
    std::vector<std::string> copied_, warnings_;
};

// Collect Files (Maya's collect_files): whether `path` lies inside `folder` (as the file system
// compares them), and a file copied into `folder` under its own name (a UDIM set: every tile,
// the <UDIM> token kept in the path returned), the same file there used as it is, other content
// there: a free name. Returns the copy's path ("" when nothing is there); `copied` gets the
// files written.
bool insideFolder(const std::string& path, const std::string& folder);
std::string collectFile(const std::string& path, const std::string& folder, std::vector<std::string>* copied = nullptr);

// Collect Files' bookkeeping, every host's (Maya's collect_files, the app's, the Blender helper's):
// each file a groom names gathered into the scene's folder `dir`.
class FileCollector {
public:
    explicit FileCollector(std::string dir) : dir_(std::move(dir)) {}
    // A stored path (`full`: as it resolves) into `folder`: one inside `dir` stays (counted), a
    // missing one stays (noted under `what`), any other is copied (collectFile: a UDIM set's every
    // tile, other content a free name) and its copy (absolute) returned and noted. "" stays "".
    std::string collect(const std::string& what, const std::string& stored, const std::string& full, const std::string& folder);
    // A card bake (every file of `prefix`: cardBakeSuffixes and the preview) into `folder` under one
    // prefix (a free one when any file would land on other content): the new prefix (absolute), or
    // `prefix` itself when it is inside `dir` or has no files.
    std::string collectCardBake(const std::string& prefix, const std::string& folder);

    const std::vector<std::pair<std::string, std::string>>& copied() const { return copied_; }   // (what, copy)
    const std::vector<std::pair<std::string, std::string>>& missing() const { return missing_; } // (what, stored)
    int inside() const { return inside_; }

private:
    std::string dir_;
    std::vector<std::pair<std::string, std::string>> copied_, missing_;
    int inside_ = 0;
};

// Every graph param naming a file (Maya's presets._file_params): ImportCurves' and GuideCache's
// path, FeatherColor's textures, a {"texture": ...} value, HairColor's map and Interpolate's
// regionMap when an image's path; inside compounds too. Each one's value given to `follow` and
// replaced by what it returns; the graph's text back (`changed`: whether any value changed).
std::string followGraphFiles(const std::string& graphJson, const std::function<std::string(const std::string&)>& follow,
                             bool* changed = nullptr);
// The same params by label ("node.param"; inside a compound "compound/inner.param"), each with
// its path as stored (empty ones left out), in the graph's order: Find Missing Files, the File
// Path Editor and Collect Files in every host.
std::vector<std::pair<std::string, std::string>> graphFileParams(const std::string& graphJson);
// The graph with the params `paths` labels (graphFileParams) naming their new paths (a texture
// value keeps its other fields); the text back (`changed`: whether any value changed).
std::string withGraphFileParams(const std::string& graphJson, const std::map<std::string, std::string>& paths,
                                bool* changed = nullptr);

// A file as an edit found it and left it (every host's undo of the files an edit writes, moves
// or deletes): its bytes, or none when there was no file.
struct FileState {
    std::string path;
    std::optional<std::string> before, after;
};
// A file's bytes now (none when it is not there).
std::optional<std::string> readFileState(const std::string& path);
// The bytes written (folders made), or the file removed for none.
void putFileState(const std::string& path, const std::optional<std::string>& bytes);
// Every file a texture map's image is (udim.h mapImageFiles: a tile set's every tile, `tiles`
// among them; the one image a tile set replaces), each as it reads before an edit...
std::vector<FileState> mapImageStates(const std::string& path, const std::vector<int>& tiles = {});
// ... each one's bytes after it (`after` read now) ...
void finishFileStates(std::vector<FileState>& states);
// ... and put back as before (undo: the last first) or after (redo).
void restoreFileStates(const std::vector<FileState>& states, bool after);

// Whether a file a groom names is there: a file, or a UDIM tile set when any tile is (any token,
// or a texture map's plain path standing for a set: udim.h). Every host's missing-file check.
bool fileThere(const std::string& path);

// Find Missing Files (every host's): a folder's files by name (case-blind on Windows), and the UDIM
// tile sets among them by the set's name ("dens.<udim>.png", and "dens.png" for a set named
// dens.1001.png..., the plain path a texture map stores: udim.h), each with the folders holding it.
struct FileIndex {
    std::map<std::string, std::vector<std::string>> files;  // a name's key -> the files (absolute)
    std::map<std::string, std::set<std::string>> sets;      // a set's name's key -> its folders
};
FileIndex indexFolder(const std::string& folder, bool recursive = true);
// The file of `index` a stored path names: by its file name (a tile set's by its set name, the
// token or plain path kept); several of that name told apart by how many of the stored path's
// folders end theirs, when one ends it best by at least two parts. "" when none fits or several
// do (then `candidates` gets them, sorted).
std::string matchMissingFile(const FileIndex& index, const std::string& stored, std::vector<std::string>* candidates = nullptr);

// Whether two paths name the same file as the file system compares them (normal, case-blind on
// Windows), whether or not it is there.
bool samePathText(const std::string& a, const std::string& b);
// Whether two paths name the same file: the file system says so (links, Windows' short names)
// when both are there, else samePathText of the paths made canonical as far as they exist.
bool sameFile(const std::string& a, const std::string& b);

// `path` when nothing is there, else the first free <stem>_2<ext>, <stem>_3<ext>... beside it
// (forward slashes): a new file never replaces one that is there.
std::string freeFilePath(const std::string& path);

// A texture map's file moved or was relinked: the graph's file params naming `from` (an absolute
// path; each stored value read through `resolve`) name `to` (as the host stores it) instead. The
// app's and Blender's painted params read a texture map by its file (wireParamMap), so they follow
// the map. The graph's text back (`changed`: whether any did).
std::string retargetFileReaders(const std::string& graphJson, const std::string& from, const std::string& to,
                                const std::function<std::string(const std::string&)>& resolve, bool* changed = nullptr);

}  // namespace groomlab
