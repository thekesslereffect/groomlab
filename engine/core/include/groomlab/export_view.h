// SPDX-License-Identifier: Apache-2.0
#pragma once

// What a strand set exports, described once for every writer (Alembic, USD,
// Unreal groom, Arnold), so the formats cannot drift: points per strand,
// widths as radii, root UVs on the scalp, the channel names, a per-strand
// color (HairColor's RGB, else from melanin), HairColor's per-point color
// when it has one, and bounds padded by the radii. Also the file helpers the writers share to
// replace an output file atomically.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "groomlab/feathers.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

// Channel names every exporter recognizes.
constexpr const char* kMelaninChannel = "melanin";                // HairColor: 0 blond .. 1 black
constexpr const char* kMelaninRednessChannel = "melaninRedness";  // HairColor: 0 brown .. 1 red
constexpr const char* kClumpIdChannel = "clumpId";                // Clump: the strand's clump (-1: none)
constexpr const char* kGroupIdChannel = "groupId";                // a groom group for engines (Attribute node)
// The color nodes' RGB (HairColor, then Bands and FeatherColor over it): linear 0-1, per strand,
// and per point too when it varies along the strands (a tip color, bands, a feather pattern;
// exporters that take per-point color use those: Arnold's varying hair_color, Alembic's Cd).
constexpr const char* kColorChannels[3] = {"colorR", "colorG", "colorB"};
// HairColor's highlight, per strand: its strength, 0-1 (Arnold's specular tint, by
// create_hair_shader's mapping: hair_color.h hairLobes), spread, 0-1 (Arnold's roughness the same
// way; Unreal reads it as groom_roughness) and cuticle tilt, in degrees (Arnold's shift as it is;
// RenderMan's PxrMarschnerHair offset). Written only where the node sets them off their defaults
// (a value, a map or a randomness): strands without them take hair_color.h's kDefaultHairSpecular,
// kDefaultHairRoughness and kDefaultHairTilt.
constexpr const char* kSpecularChannel = "specular";
constexpr const char* kRoughnessChannel = "roughness";
constexpr const char* kTiltChannel = "tilt";
// FeatherColor's iridescence: per point, 0-1, how much structural color the hair shading mixes in
// (its color and hue shift are the node's: GroomSession::hairLook). Arnold reads it as varying
// user data, Unreal as groom_iridescence (and groom_ao), the card bake into the Color map's alpha.
constexpr const char* kIridescenceChannel = "iridescence";
// RenderData's opacity: per point, 0-1, how opaque the strand is there (render-only: the viewport
// does not draw it; Arnold: varying `opacity` user data, the curves made not opaque when any is
// below 1). Every strand channel reaches Arnold as uniform user data of its name, and Blender as
// an attribute.
constexpr const char* kOpacityChannel = "opacity";
// Wet's wetness (and a Simulate's with water or rain): per point, 0 dry .. 1 soaked (hair_color.h
// wetShading: darker, more saturated and glossier hair; Maya's viewport and the app shade it,
// Arnold: varying `wetness` user data, which create_hair_shader's wet network reads). A strand
// channel of the name (an Attribute node's: wet strands by a map) shades the same, its strand's
// value on every point (Arnold: uniform user data, which the same network reads).
constexpr const char* kWetnessChannel = "wetness";
// The strands' wetness: the point channel (`perPoint` true), else the strand channel, else null
// (a channel of the wrong size counts as absent).
const std::vector<float>* wetnessChannel(const StrandSet& strands, bool* perPoint);
// The continuous per-strand look channels beside HairColor's RGB: its pigment (melanin,
// melaninRedness) and its highlight (specular, roughness, tilt). Blend
// mixes these (and the RGB, per strand and per point) in its "shapeAndLook" mode. IDs and flags
// (clumpId, braidId, featherId, isShaft, side, groupId...) and any other channel are not look
// data: they never mix.
constexpr const char* kLookChannels[] = {kMelaninChannel, kMelaninRednessChannel, kSpecularChannel, kRoughnessChannel,
                                         kTiltChannel};

struct ExportView {
    const StrandSet* strands = nullptr;
    std::vector<std::int32_t> counts;            // points per strand
    std::vector<std::array<float, 2>> rootUVs;   // per strand; empty when the scalp has no UVs
    // Per strand, linear RGB: the hair's base color, or its appearance with kExportAppearanceColors;
    // empty without color or melanin channels.
    std::vector<std::array<float, 3>> colors;
    std::vector<std::array<float, 3>> pointColors;  // per point, linear RGB (pointColors()); usually empty
    std::vector<std::string> strandChannels;     // sorted, as StrandSet::strandChannelNames
    std::vector<std::string> pointChannels;
    Vec3f boundsLo, boundsHi;                    // positions padded by the radii; zero when empty

    std::size_t strandCount() const { return counts.size(); }
    std::size_t pointCount() const { return strands ? strands->pointCount() : 0; }
    bool hasUVs() const { return !rootUVs.empty(); }
    bool hasColors() const { return !colors.empty(); }
    bool hasPointColors() const { return !pointColors.empty(); }
    // Strand width (a diameter) at a point; 1 when the set has no widths.
    float width(std::size_t point) const {
        const std::vector<float>& w = strands->widths();
        return point < w.size() ? w[point] : 1.0f;
    }
    float radius(std::size_t point) const { return 0.5f * width(point); }
    // The channel's values, or null (a channel of the wrong size counts as absent).
    const std::vector<float>* strandChannel(const std::string& name) const;
    const std::vector<float>* pointChannel(const std::string& name) const;
};

// The parts of an ExportView worked out only for the writers that read them.
enum ExportParts : unsigned {
    kExportPointColors = 1u,  // ExportView::pointColors (else left empty)
    kExportBounds = 2u,       // ExportView::boundsLo/boundsHi (else zero)
    kExportAll = kExportPointColors | kExportBounds,
    // colors and pointColors as the hair's appearance (hair_color.h hairAppearanceColor: what it
    // looks like, for writers storing a display or albedo color: USD displayColor, Alembic Cd,
    // Unreal's groom_color), else its base color (what a hair BSDF takes: Arnold's hair_color,
    // RenderMan's rman_color). Not part of kExportAll.
    kExportAppearanceColors = 4u,
};

// Describes `strands` for export; `scalp` (may be null) gives the root UVs. `parts`: which of
// the optional parts to fill (the counts, UVs, per-strand colors and channel names always are).
ExportView describeExport(const StrandSet& strands, const ScalpMesh* scalp, unsigned parts = kExportAll);

// A copy of `strands` for an engine's importer: positions and widths times
// `scale` (about the origin: scene units to the engine's), and with
// `pointsPerStrand` of 2 or more every strand resampled to that many points
// spaced evenly by arc length (widths and per-point channels following; a
// strand of one point repeats it). IDs, roots and per-strand channels stay.
StrandSet exportCopy(const StrandSet& strands, float scale, std::size_t pointsPerStrand = 0);

// Per-strand linear RGB, the hair's BASE color (what a hair BSDF takes): HairColor's RGB channels
// (kColorChannels) when the set has all three, else from the melanin channels (hair_color.h); empty
// when it has neither. Every renderer's and exporter's color comes from here.
std::vector<std::array<float, 3>> strandColors(const StrandSet& strands);
// Per-point linear RGB base color: HairColor's RGB point channels (a tip blend) when the
// set has all three, one value per point; else empty (the color is per strand).
std::vector<std::array<float, 3>> pointColors(const StrandSet& strands);
// The same as the hair's APPEARANCE color (hair_color.h hairAppearanceColor), for what stores or
// draws a color without a hair BSDF (displayColor, Cd, groom_color, the cards' color maps).
std::vector<std::array<float, 3>> strandAppearanceColors(const StrandSet& strands);
std::vector<std::array<float, 3>> pointAppearanceColors(const StrandSet& strands);

// The name an export's root object takes, every host's (Maya: the groom's transform, the Blender
// add-on: the scalp object, the app: the file's stem): letters, digits and _ (every other character
// _), "groom" before a leading digit or for nothing: an Alembic, USD, vrscene and engine name alike.
std::string exportObjectName(const std::string& wanted);

// Valid USD identifiers (usdIdentifier) for `names`, in order, unique among
// themselves and never one of `reserved`: a clash gets a numbered suffix, so
// "a-b" beside "a_b" becomes a_b and a_b_2.
std::vector<std::string> usdIdentifiers(const std::vector<std::string>& names,
                                        const std::vector<std::string>& reserved = {});
// USD primvar names for the channels: usdIdentifiers, never the reserved "st",
// "strandId", "displayColor", "displayOpacity", "widths", "points" or "normals" (USD takes a
// primvars:normals for the normals).
std::vector<std::string> usdChannelNames(const std::vector<std::string>& channels);

// Feathers for engines (feathers.h), by the channels Propagate writes (featherId, isShaft).
// True for a barb: a strand with a feather (featherId 0 or more) that is not its shaft.
bool isBarb(const StrandSet& strands, std::size_t strand);
// Whether any strand is a barb (isBarb), the channels looked up once.
bool hasBarbs(const StrandSet& strands);
// The strands without the feathers' shafts (isShaft 1): with a rachis mesh the shafts are
// geometry, not strands.
StrandSet withoutShafts(const StrandSet& strands);
// Only the feathers' shafts.
StrandSet onlyShafts(const StrandSet& strands);
// Each point's color of `mesh` (buildRachisMesh of the strands' feather layers): its feather's
// shaft strand's (kColorChannels, at its root or per strand), else `fallback`. The renderers
// and the viewport color the rachis with it.
std::vector<std::array<float, 3>> rachisColors(const StrandSet& strands, const RachisMesh& mesh, const std::array<float, 3>& fallback);
// A lighter groom for heavy shots: barbs kept at `density` (0-1, by ID: stable, the same
// barbs every frame), each kept one `widthScale` times as wide so the vanes keep their
// coverage; hair and shafts stay as they are.
StrandSet thinBarbs(const StrandSet& strands, float density, float widthScale);

// A groom group for an engine: the strands with one value of a channel (Unreal gives each
// group its own material, LODs and physics).
struct GroomGroup {
    int id = 0;
    std::string name;
    StrandSet strands;
    bool feathers = false;  // it holds barbs (strands with a featherId)
};
// The strands split by the rounded value of `channel` (below 0 counts as 0), by id; one group
// 0 of every strand when they have no such channel. Group 0 is named `baseName`; others by
// `names` (e.g. featherGroupNames), else "group<id>" (or "<channel><id>" for another channel
// than groupId).
std::vector<GroomGroup> splitGroomGroups(const StrandSet& strands, const std::string& channel,
                                         const std::string& baseName, const std::map<int, std::string>& names);

// Strands per group Unity's hair package takes (com.unity.demoteam.hair, HairSim.MAX_STRAND_COUNT).
constexpr std::size_t kUnityMaxStrandsPerGroup = 64000;
// Groups of at most `maxStrands` strands (0: no limit): a group within it stays as it is; a
// larger one becomes parts "<name>_0", "<name>_1", ... (same id and kind), as few as fit and as
// even as they come, each a run of the group's strands in order, so the same strands give the
// same parts every time.
std::vector<GroomGroup> limitGroupSize(std::vector<GroomGroup> groups, std::size_t maxStrands);
// Every strand's first width above 0 (Unity's hair importer drops a curve set's widths when any
// root width is 0 or less): a strand whose root is not gets its widest point's width, else the
// set's first positive root width, else `fallback`. Returns the strands changed.
std::size_t keepRootWidthsPositive(StrandSet& strands, float fallback);

// The path a writer writes to before replaceFile: "<target>.tmp".
std::filesystem::path tempPathFor(const std::filesystem::path& target);
// Moves `tmp` over `target` (rename; on Windows the target is removed first
// when a rename over it fails). Throws std::runtime_error.
void replaceFile(const std::filesystem::path& tmp, const std::filesystem::path& target);

}  // namespace groomlab
