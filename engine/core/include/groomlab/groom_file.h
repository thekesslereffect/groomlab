// SPDX-License-Identifier: Apache-2.0
#pragma once

// Groom files (.groom): a groom's graph, guides and scalp in one JSON file,
// so the CLI, the Arnold procedural and a farm can evaluate a groom without
// Maya, and grooms move between hosts (Maya, the standalone app). An animated
// scalp lives in an Alembic cache next to it. The format is published for third parties
// (web/content/docs/groom-format.mdx): change that page with the format.
//
//   {"format": "groomlab.groom", "version": 1, "framesPerSecond": 24, "units": "cm", "upAxis": "Y",
//    "source": "scene.ma|groomLab1Shape",
//    "graph": {...}, "hierarchy": {...} | null, "parts": {...}?, "correctives": {...}?, "flow": {...}?,
//    "sculpts": {"<Sculpt node>": {"guides": {"<guide id>": [t, ax, ay, scale, ...]}}}?,
//    "controls": {"name": value}?, "maps": {"name": {"kind", "texture", "channels"?}}?,
//    "scalp": {"points": [x, y, z, ...], "triangles": [a, b, c, ...], "topology": "9f3a...",
//              "polygons": [n, ...]?, "subdivision": 2?, "restPoints": [...]?, "uvs": [u, v, ...]?,
//              "colorSets": {"name": [r, g, b, a, ...]}?},
//    "scalps": [{"name": "ears", "points": [...], "triangles": [...], ... as "scalp" but "topology"}]?,
//    "guides": [{"id": 3, "root": [face, u, v], "points": [x, y, z, ...]}],
//    "scalpCache": {"file": "scalp.abc", "object": "headShape"}?,
//    "searchPaths": ["$PROJECT", "C:/shows/abc/sourceimages"]?,
//    "guideCache": {"file": "sim.abc", "object": ""}?, "followScalp": true?, "scaleSource": "transform"|"off"?,
//    "scaleReference": 412.5?, "scaleTransformReference": 1.0?,
//    "cardBake": {"prefix": "hair_cards/hair_", "strands": "<node>:<hash>,..."}?,
//    "simCaches": {"<Simulate node>": {"file": "hair_sim/sim.abc", "object": "", "stamp": "9f3a..."}}?,
//    "prototypes": {"<name>": {"points": [...], "triangles": [...], "normals": [...]?, "uvs": [...]?}}?,
//    "colliders": {"<name>": {"points": [...], "triangles": [...], "polygons": [...]?, "subdivision": 2?,
//                             "restPoints": [...]?}}?,
//    "transferMarkers": [{"old": {"face", "u", "v", "point"}, "new": {...}}]?}
//
// Numbers are stored exactly (floats in their shortest round-trip form), so a
// groom evaluates the same from the file as in Maya; a NaN or infinity is
// refused on write, and a null read as 0. Texture maps keep the paths in the
// graph; a painted texture map's file ("maps") is relative to the groom file
// when it can be. A map's name follows the rule every host keeps (scalp_maps.h validMapName:
// letters, digits and _, not starting with a digit); a file holding others (an older app's) is
// read with them sanitized, its color sets, guide layers and graph readers with them
// (sanitizeMapNames). "units" names the scene unit of the scalp and guides ("cm" default).
// "upAxis" is the world's up axis the points are in: "Y" (Maya, the app) or "Z" (Blender,
// Houdini set Z-up); convertUpAxis turns a file into the other. "followScalp"
// (GroomData::followScalp, written when on): evaluated on the scalp's rest pose and carried by
// each root (follow_scalp.h), with the scalp cache's frames as the scalp now. "scaleSource"
// (GroomData::scaleSource: "transform" or "off", written when not "area"), "scaleReference"
// (GroomData::scaleReference, the scalp area the groom is 1x at, written when set) and
// "scaleTransformReference" (the transform scale it is 1x at, written when set): the groom scales
// with its scalp (GroomSession::groomScale); without a reference the first one measured is it. "cardBake": the
// last card bake's files, <prefix>Depth|Coverage|Tangent|Attribute|Color|AO|Normal.tga,
// <prefix>Atlas.json and <prefix>Preview.png (the hosts' layout), and the stamp of what each
// Cards node's cards were made from (the stale check; empty: unknown, every node stale).
// "simCaches": each baked Simulate node's guide cache (engine/io sim_cache.h: Alembic curves, a
// guide's ID as its strandId, world space, frames at framesPerSecond) and the stamp of the
// settings it was baked with (SimSettings::hash): GroomScene hands it to the session, which reads
// the node's frames from it while its settings are those (simulation.h). "prototypes": the
// scene meshes the Instance nodes name (scales' plates of the artist's own: InstancePrototype,
// in the prototype's own space, so an up-axis conversion leaves them as they are), inline the
// way the scalp is, so the procedural and a farm render them without the scene; the built-in
// plates are not stored. Normals per point (smooth ones are made when absent), uvs per point.
// "topology": the scalp's fingerprint (ScalpMesh::topologyFingerprint, 16 hex digits: its point
// count and triangles), read into GroomData::scalpFingerprint (the scalp's own when a
// file has none): a host feeding a scalp of another topology is told so (GroomSession). "colliders":
// the meshes the ColliderMesh nodes read by name (GroomData::meshes), world space, as they were
// when the file was written, with their rest poses: every reader collides as the writer did (a
// moving collider is the writer's frame; a host feeding its own replaces it by name).
// "scalps": the groom's other scalps in order (GroomData::scalps, scalp_pieces.h: several scalps
// on one groom, each read by the Scalp nodes naming it), world space like the scalp, joined after
// it; roots of strands and guides index the joined mesh. Absent for a groom of one scalp.
// "transferMarkers": the marker pairs of the groom's last transfer by markers (transfer.h
// transferMarkersToJson: each side's root and world point), so a transfer again reuses them; read
// by no evaluation. Absent when there are none.
// "referencePhotos": the standalone app's Reference window (reference_edit.h): each photo's file,
// size, mask, solved camera and edits ([{"name", "image", "width", "height", "mask", "camera":
// {"rotation", "position", "focal_px"} or null, "state": referencePhotoStateToJson's object, and the
// Photos tab's "display": {"opacity", "depth": "front" or "behind", "locked", "visible"}}]); read by
// no evaluation. Absent when there are none.
// "guideGroupColors": the guide groups' own viewport colors, as Maya's groom attribute of that name
// holds them ("name:#rrggbb name:#rrggbb", sRGB; display.h parseGuideGroupColors), and
// "hiddenGuideGroups": the groups hidden in the viewport ("name name", Maya's attribute). Display
// only, read by no evaluation; absent when empty.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/groom.h"
#include "groomlab/instances.h"
#include "groomlab/transfer.h"

namespace groomlab {

class GroomFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CacheRef {
    std::string file;    // relative paths are relative to the groom file
    std::string object;  // object name in the cache; empty: the first match
    bool empty() const { return file.empty(); }
};

// A card bake's files: <prefix><Map>.tga, <prefix>Atlas.json, <prefix>Preview.png.
struct CardBakeRef {
    std::string prefix;   // relative prefixes are relative to the groom file
    std::string strands;  // what each Cards node's cards were made from (cardsBakeStamp); "": unknown
    bool empty() const { return prefix.empty(); }
};

// A baked Simulate node: its guide cache and the settings stamp it was made with.
struct SimCacheRef {
    std::string file;    // relative paths are relative to the groom file
    std::string object;  // object name in the cache; empty: the first curves
    std::uint64_t stamp = 0;
};

struct GroomFile {
    GroomData data;             // data.scalp: the scalp when the file was written
    CacheRef scalpCache;        // animated scalp: the same vertices over time
    CacheRef guideCache;        // animated guide poses, matched to hierarchy guides by ID
    CardBakeRef cardBake;       // the last card bake (the atlas the cards keep their cells from)
    std::map<std::string, SimCacheRef> simCaches;  // baked Simulate nodes, by node name
    std::map<std::string, InstancePrototype> prototypes;  // the Instance nodes' scene meshes, by name
    double framesPerSecond = 24.0;
    std::string units = "cm";   // scene unit of the scalp and guides: "cm", "m", "in"...
    std::string upAxis = "Y";   // the up axis of the points: "Y" or "Z"
    std::string source;         // where the groom came from (informational)
    // Where relative paths in the graph (texture maps, ImportCurves) resolve,
    // after the groom file's own folder: e.g. the Maya project it came from.
    // Entries may use environment variables.
    std::vector<std::string> searchPaths;
    // The marker pairs of the last transfer by markers (transfer.h), reused by the next one.
    std::vector<TransferMarker> transferMarkers;
    // The Reference window's photos ("referencePhotos", a JSON array as text; "" none).
    std::string referencePhotos;
    // The guide groups' own colors and the hidden groups ("guideGroupColors", "hiddenGuideGroups":
    // Maya's attributes' text; "" none).
    std::string guideGroupColors;
    std::string hiddenGuideGroups;
    std::string directory;      // the folder the file was read from (not stored)
};

std::string groomFileToJson(const GroomFile& file);
// A path the file stores (a texture map's file, ImportCurves' curves) as every host resolves it
// (paths.h resolvePathIn): variables expanded; a relative one joined to the file's folder
// (`directory`, or the file's own: GroomFile::directory) and then its search paths, the first
// that exists (else the folder's join; with neither, as stored).
std::string resolveGroomPath(const GroomFile& file, const std::string& stored, const std::string& directory = {});
// The "simCaches" object alone as text, and back (Maya keeps it in a groom's simCaches attribute,
// paths relative to the scene): relative paths join `baseDir` (empty: kept as they are). Blank
// text is no bakes; throws GroomFileError for text that is not such an object.
std::string simCachesToJson(const std::map<std::string, SimCacheRef>& caches);
std::map<std::string, SimCacheRef> simCachesFromJson(const std::string& text, const std::string& baseDir = {});
// `baseDir` resolves relative cache paths. Throws GroomFileError.
GroomFile groomFileFromJson(const std::string& text, const std::string& baseDir = {});

// A scalp fingerprint (ScalpMesh::topologyFingerprint) as a groom file writes it (16 hex digits),
// and back (0 for text that is not one): Maya keeps a groom's in its scalpTopology attribute.
std::string scalpFingerprintText(std::uint64_t fingerprint);
std::uint64_t parseScalpFingerprint(const std::string& text);

void writeGroomFile(const std::string& path, const GroomFile& file);  // throws GroomFileError
GroomFile readGroomFile(const std::string& path);                      // throws GroomFileError

// The file's points turned into another up axis ("Y" or "Z"; throws GroomFileError on
// another): the scalp and its rest pose, the flat guides and the hierarchy's world shapes (the
// points and frame references of its primary and locked guides), a quarter turn about X
// (Z-up (x, y, z) is Y-up (x, z, -y)), the colliders with their rest poses and the transfer markers' points. What is stored on the
// surface or in the guides' own frames (roots, rooted shapes, children's offsets, parts, flow,
// correctives, sculpts) turns with them as it is. The graph's params that are world directions
// (ParamSpec::worldDirection: Gravity's, Wind's, Bend's, Simulate's directions as vectors, a
// param a node leaves out by its default; Mirror's axis as the axis it becomes, its offset and
// side flipped when that axis points the other way) turn too, compounds' inner nodes with them.
// The same file when it is in that axis already.
void convertUpAxis(GroomFile& file, const std::string& upAxis);

}  // namespace groomlab
