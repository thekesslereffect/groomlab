// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom's way out of the host and the inputs that come with it (docs/BLENDER.md "Pipeline"):
// the game exports Maya's groomlab.game, exports and cards write (glTF cards, Unreal and Unity
// cards with their maps and manifests, Unity's feathers, the scales' mesh and baked maps, strands
// as a mesh: the meshes the host writes as FBX are answered as arrays), Simulate Range in steps,
// its bakes, the host's scalp at other frames feeding it, colliders, instance prototypes and other
// scalps, Transfer Groom to Scalp (markers included), and the shipped sample meshes.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"

#include "host_space.h"

namespace groomlab::helper {

struct Session;
class BlockReader;

// The pipeline's commands (simulate_range, set_meshes, transfer, sample_meshes...).
bool isPipelineCommand(const std::string& cmd);
// Runs one; `session` gives the request's session (every command but sample_meshes takes one).
nlohmann::json runPipelineCommand(const std::string& cmd, const nlohmann::json& args, BlockReader& blocks,
                                  const std::function<Session&()>& session);
// export's formats written here (gltfCards, unrealCards, unityCards, unityFeathers, scalesFbx).
bool isPipelineExport(const std::string& format);
nlohmann::json runPipelineExport(Session& s, const nlohmann::json& args);
// The session's groom takes the host's scalp at other frames (set_scalp_frames) as its Simulate
// nodes' feed: called when the groom is replaced.
void applyScalpFrames(Session& s);
// The session's groom takes its file's card bake ("cardBake": rebuilt cards keep the atlas cells,
// card_stats says when they changed since): called when the groom is replaced.
void applyCardBake(Session& s);

// helper.cpp's, shared: a mesh as set_scalp takes it (points, faceCounts, faceIndices, uvs,
// restPoints, colorSets) in groom space; a mesh's polygons back; the time's inputs that come with
// the file; the file's instance prototypes; flat guides kept at rest handed on; the sample meshes'
// folder.
MeshPtr hostMesh(const nlohmann::json& args, const HostSpace& space, BlockReader& blocks);
void hostMeshFaces(const ScalpMesh& mesh, std::vector<std::int32_t>& counts, std::vector<std::int32_t>& indices,
                   std::vector<float>& uvs, bool& exact);
void feedSessionTime(Session& s);
PrototypeLookup sessionPrototypes(const Session& s);
void handSessionRestGuides(Session& s);
std::string sampleMeshesDir();

}  // namespace groomlab::helper
