// SPDX-License-Identifier: Apache-2.0
#pragma once

// The color nodes' patterns (Bands, FeatherColor), as plain functions the operators, the bakes and the tests share:
// bands along a strand (agouti, ticked, tipped and shaded fur), the feather laid flat (vane
// space: where every barb point sits in its feather's own 2D space, from the channels Propagate
// writes) with the patterns drawn there (bars, chevrons, spots, an edge fringe, eye spots), and
// the iridescent tint every host's hair shading applies from the view's angle to the fiber.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/ramp.h"

namespace groomlab {

class StrandSet;

// ---- Bands along the strand --------------------------------------------------------------------

// Where along a strand (t: 0 root .. 1 tip) the band pattern stands: `profile` is one period
// (0: the hair's own color, 1: the band's), repeated `bands` times root to tip; `scale` stretches
// it (1: as drawn) and `phase` shifts it by that share of a period. Before the first period and
// past the last the profile's ends hold, so a single band moves along the strand rather than
// wrapping round to the root.
float bandAt(const Ramp& profile, float t, int bands, float scale = 1.0f, float phase = 0.0f);

// ---- Vane space --------------------------------------------------------------------------------

// The channels Propagate writes for the feather laid flat: u along the shaft, 0 at its root .. 1
// at the feather's farthest reach toward its tip; v out from the shaft, 0 on it .. 1 at its vane's
// widest reach (the wider side's: an asymmetric feather's narrow vane stays short of 1). Per barb:
// its root (where it leaves the shaft: u, v 0) and its tip; a point between sits at its place
// along the barb (its curl bends it off that line by a few percent of its length). A shaft runs
// from u 0 to its own end, v 0.
constexpr const char* kVaneUChannel = "vaneU";
constexpr const char* kVaneTipUChannel = "vaneTipU";
constexpr const char* kVaneTipVChannel = "vaneTipV";
// 1 for the barbs of a Propagate's down zone (its `down` above its `start`; else no channel).
constexpr const char* kIsDownChannel = "isDown";
// The vane's normal where a barb grows (the shaft's frame there; its root's for a shaft), as
// designed: what the hosts take as a feather's strands' normal (their shading frames, the
// iridescence's angle) instead of the scalp's under the root, which a feather standing off the
// skin does not face.
constexpr const char* kVaneNormalChannels[3] = {"vaneNormalX", "vaneNormalY", "vaneNormalZ"};

// Reads the vane normals off a strand set (the channels bound once; the set must outlive it).
class VaneNormals {
public:
    explicit VaneNormals(const StrandSet& strands);
    // Strand s's vane normal into `normal`; false when it has none (not a feather's, or a shaft
    // that grew no barbs): the host's own (the scalp's at the root) then.
    bool at(std::size_t s, Vec3f* normal) const;

private:
    const std::vector<float>* c_[3] = {nullptr, nullptr, nullptr};
};

class ScalpMesh;
// Strand i's root normal, the one the iridescence's angle is taken from (every host's: Arnold's
// root_normal, the Blender helper's rootNormal, the app's lookdev): a feather's vane normal, else
// the scalp's under the root, else the way the strand sets out (up for a strand of one point).
Vec3f iridescenceRootNormal(const StrandSet& strands, std::size_t i, const ScalpMesh* scalp, const VaneNormals& vanes);

// One point in vane space: `side` -1 the left vane, +1 the right one (seen from the vane's top,
// the tip up), 0 around the shaft (plumes, down) or the shaft itself; `along` its place along its
// barb (0 at the shaft .. 1 at the barb's tip, the vane's edge); `barb` false for a shaft.
struct VanePoint {
    float u = 0.0f, v = 0.0f;
    int side = 0;
    float along = 0.0f;
    bool barb = false;
};

// Reads vane space off a strand set (the channels bound once; the set must outlive it).
class VaneSpace {
public:
    explicit VaneSpace(const StrandSet& strands);
    // Whether the strands carry vane space at all (a Propagate made them).
    bool available() const { return vaneU_ && tipU_ && tipV_; }
    // Whether strand s is part of a feather (a barb or a shaft).
    bool inFeather(std::size_t s) const;
    // Point i of strand s's n points, in vane space (available() and inFeather(s) only).
    VanePoint at(std::size_t s, std::size_t i, std::size_t n) const;
    // A number every strand of one feather shares and other feathers do not (stable: from the
    // shaft's root on the scalp, else its place in Propagate's input): what per-feather jitter
    // draws from.
    std::uint64_t featherKey(std::size_t s) const;

private:
    const StrandSet& strands_;
    const std::vector<float>* vaneU_ = nullptr;
    const std::vector<float>* tipU_ = nullptr;
    const std::vector<float>* tipV_ = nullptr;
    const std::vector<float>* side_ = nullptr;
    const std::vector<float>* feather_ = nullptr;
    const std::vector<float>* shaft_ = nullptr;
};

// ---- Patterns in vane space --------------------------------------------------------------------

enum class VanePattern { None, Texture, Bars, Chevrons, Spots, Fringe, Eye };
// FeatherColor's pattern names: none, texture, bars, chevrons, spots, fringe, eye.
const std::vector<std::string>& vanePatternNames();
// False for a name that is none of them.
bool parseVanePattern(const std::string& name, VanePattern* out);

// Which vanes a pattern (or the iridescence) covers: both, left, right.
enum class VaneSides { Both, Left, Right };
bool parseVaneSides(const std::string& name, VaneSides* out);
bool onVane(VaneSides sides, int side);

// A typical feather's length over its vane's half-width: x (= side * v) is divided by it before
// a pattern is drawn, so bars at an angle, spots and eye spots keep their shapes on a feather of
// about those proportions (on a narrower one they stretch across, as real ones do).
constexpr float kVaneAspect = 3.0f;

struct VanePatternParams {
    VanePattern kind = VanePattern::None;
    float count = 6.0f;       // bars, chevrons: per feather length; spots: per feather length (rows)
    float width = 0.4f;       // bars, chevrons, spots: the share of each period the shape takes;
                              // eye: its diameter as a share of the feather's length; fringe: the
                              // share of each barb (from its tip)
    float angle = 0.0f;       // radians: bars' tilt from straight across; chevrons' arms
    float sharpness = 0.7f;   // 0 soft edges .. 1 hard ones
    float offset = 0.7f;      // along the feather (0 root .. 1 tip): shifts bars and spots; the eye's center
    Ramp profile = Ramp::constant(1.0f);  // across a shape, its center (0) to its edge (1)
    float jitter = 0.0f;          // per feather: shift, as a share of the spacing (the eye: of its size)
    float scaleJitter = 0.0f;     // per feather: size, +/- this share
    float rotationJitter = 0.0f;  // per feather: radians, +/-
    std::uint64_t seed = 0;
};

// One feather's draw of the jitter (deterministic in `featherKey` and the params' seed).
struct FeatherJitter {
    float offset = 0.0f;  // added to the params' offset (feather lengths)
    float scale = 1.0f;   // multiplies the pattern's size
    float rotation = 0.0f;
};
FeatherJitter featherJitter(const VanePatternParams& params, std::uint64_t featherKey);

// The pattern's weight at a point, 0 (the hair's color) .. 1 (the pattern's), before the vanes it
// covers are picked (the caller's onVane). Texture is the caller's own (0 here).
float vanePatternMask(const VanePatternParams& params, const VanePoint& point, const FeatherJitter& jitter,
                      std::uint64_t featherKey);

// Where a feather's painted texture is read for a point: one image of the whole feather laid flat
// (`perSide` false: the shaft up the middle, the left vane on the left half, its root at the
// bottom: s = 0.5 + 0.5 x side x v, t = u), or one image per vane (`perSide`: s = v, t = u).
std::array<float, 2> vaneTextureCoordinates(const VanePoint& point, bool perSide);

// ---- Iridescence -------------------------------------------------------------------------------

// The structural color seen at `grazing` = 1 - |V . N| (0: the view along the strand's normal,
// facing it .. 1: edge-on), N the strand's normal (its root's, leaned along it: on a feather,
// the vane's): `color`'s hue turned by `shift` turns times that, its saturation and value kept.
// Every host's hair shading computes the same (the app's hair_lookdev.frag, the viewport's
// groomLabHairColor and card fragments, the Arnold network create_hair_shader builds as a ramp
// of five keys of it), mixed into the fiber's color and its highlight's tint by the strands'
// iridescence channel.
std::array<float, 3> iridescentTint(const std::array<float, 3>& color, float shift, float grazing);

}  // namespace groomlab
