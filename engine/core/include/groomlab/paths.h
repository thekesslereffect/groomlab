// SPDX-License-Identifier: Apache-2.0
#pragma once

// File paths stored in grooms (texture maps, Alembic caches, groom files)
// may use environment variables ($VAR, ${VAR}, %VAR%) and may be relative,
// so scenes and groom files move between machines, projects and referenced
// scenes without baked absolute paths.
//
// Relative paths resolve through the resolver of the current evaluation:
// Maya uses the scene's project and folder; a groom file uses its own folder
// and its search paths. Graph::evaluate makes EvalContext::paths current on
// its thread, so operators just call resolvePath.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace groomlab {

// Maps a relative path to a full one; returns "" when it cannot.
using PathResolver = std::function<std::string(const std::string& relative)>;

// An environment variable's value; nullopt when it is not set.
std::optional<std::string> environmentVariable(const std::string& name);

// $VAR and ${VAR} (and %VAR% on Windows) replaced from the environment;
// unknown variables are left as they are.
std::string expandEnvironment(const std::string& path);

bool isAbsolutePath(const std::string& path);

// Variables expanded; a relative path goes through the current resolver
// (unchanged when there is none or it finds nothing). A file whose folder is not
// on this machine (a scene or groom shared with its files) is looked for where
// it came along to, through the current resolver: an absolute path by its
// tails (a folder and the file at least), longest first; a relative one, and
// each tail, also inside a folder of its own first name (groomlab/groomlab/...:
// a zip extracted into a folder of its name). Unchanged when none is there.
std::string resolvePath(const std::string& path);

// A resolver joining the path to each directory in turn: the first that
// exists wins, else the join with the first directory.
PathResolver searchPathResolver(std::vector<std::string> directories);

// resolvePath through searchPathResolver over `directories` (entries may hold variables): a
// stored path (a texture map's file, a cache) resolved for the file or scene it belongs to.
std::string resolvePathIn(const std::string& path, std::vector<std::string> directories);

// How every host stores a path relative to a scene (Maya: the scene's folder, then the project),
// a groom file (its folder) or the standalone app's groom folder, so the stored path resolves
// through searchPathResolver over the same folders:
// - empty, holding a variable ($VAR, %VAR%) or relative already: kept (forward slashes);
// - inside one of `folders` (the first that holds it; the file and the folders compared as the
//   file system sees them: case, links, Windows' short names): relative to that folder, no "..";
// - else, with `beside` (a groom file or the app's groom folder: a groom moves between machines
//   as a folder tree, so a shared texture folder beside it comes along) and on the same volume
//   as the first folder: relative to it with "..";
// - else absolute (a Maya scene: its folder and its project hold what moves with it; a file
//   outside both is the machine's, as Maya stores it; or another drive or share).
// Always forward slashes.
std::string portablePath(const std::string& path, const std::vector<std::string>& folders, bool beside);
// The rule for a path stored relative to a groom file or a groom's folder: portablePath with
// ".." beside it.
inline std::string pathFromGroomFolder(const std::string& path, const std::string& folder) {
    return portablePath(path, {folder}, true);
}

// Makes a resolver current on this thread for the scope's lifetime.
class ScopedPathResolver {
public:
    explicit ScopedPathResolver(const PathResolver* resolver);
    ~ScopedPathResolver();
    ScopedPathResolver(const ScopedPathResolver&) = delete;
    ScopedPathResolver& operator=(const ScopedPathResolver&) = delete;

private:
    const PathResolver* previous_;
};

}  // namespace groomlab
