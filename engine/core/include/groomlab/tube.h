// SPDX-License-Identifier: Apache-2.0
#pragma once

// Tube guides: the tube round a guide, the volume its child guides are made in (GH-3) and the
// one Interpolate's fillTubes fills with strands (anime and cartoon hair: a lock shaped as a
// whole, its hair staying inside it to the tip).
//
// A tube is a cross-section carried along the guide. Its outline is one SHAPE for the whole tube
// (round, ribbon, teardrop, blade, or a custom shape from the groom's library: CustomTubeShape),
// drawn in a unit domain: section coordinates (u, v) with the outline touching -1 and 1 on both
// axes, centered on the guide. Along the guide the
// section changes by three lists, each evenly spaced root to tip and linear between (one value: the
// same all along; the lists may differ in length):
//   width   the section's half-width in scene units: u = 1 lies this far from the guide (a round
//           tube's radius, as tubes had before sections);
//   aspect  the section's height over its width, whatever the shape: 1 as drawn, toward 0 a flat
//           ribbon (clamped to kMinTubeAspect .. kMaxTubeAspect);
//   twist   radians, the section turned about the guide at that point: absolute at each sample,
//           not accumulated (a list 0, 3.14 turns the section half a turn from root to tip; a
//           brush or a ramp sets each sample on its own).
// The world point of section coordinates (u, v) at t (0 root, 1 tip) on a guide is
//   spine(t) + frame(t) * rotate(twist(t)) * (u * width(t), v * width(t) * aspect(t))
// where frame(t) is the guide's rotation-minimizing frame (frames.h) from its frameUp: x along the
// frame's normal, y along its binormal. So a point inside the outline at the root stays inside the
// outline all the way to the tip, however the tube flattens, twists or tapers. t runs over the
// guide's samples by index (evenly spaced samples: evenly along its length).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/frames.h"
#include "groomlab/math.h"

namespace groomlab {

enum class TubeShape : std::uint8_t { Round, Ribbon, Teardrop, Blade };

// "round", "ribbon", "teardrop", "blade" (the JSON and API names).
const char* tubeShapeName(TubeShape shape);
// False (shape untouched) for an unknown name.
bool parseTubeShape(const std::string& name, TubeShape* shape);
const std::vector<std::string>& tubeShapeNames();

constexpr float kMinTubeAspect = 0.01f;
constexpr float kMaxTubeAspect = 100.0f;

// A point in section coordinates.
struct SectionPoint {
    float u = 0.0f, v = 0.0f;
};

// A shape's outline and what is measured on it once: a closed polygon, counter-clockwise, in the
// unit domain (its bounding box -1 to 1 on both axes); its corners and edge points (indices into
// the polygon: where its silhouette turns, what a wire view draws lines along the tube through);
// its deepest point (the farthest from the outline) and how deep that lies.
struct TubeOutline {
    std::vector<SectionPoint> points;
    std::vector<std::size_t> corners;
    SectionPoint deepest;
    float depth = 1.0f;
};

// A custom tube shape: one entry of a groom's shape library (GuideHierarchy::setTubeShape), which
// guides take by name as they take a built-in shape. Drawn by its control points, in order round
// the outline: with `smooth` a smooth closed curve through them (a centripetal Catmull-Rom spline),
// else straight edges between them. Made by makeCustomTubeShape, which checks and fits it;
// never changed after (a library edit makes a new one), so guides and evaluations share it.
struct CustomTubeShape {
    std::string name;
    std::vector<SectionPoint> points;  // fitted: the outline's box -1 to 1, counter-clockwise
    bool smooth = true;
    std::uint64_t hash = 0;  // the points and smooth (not the name)
    TubeOutline outline;     // sampled: kTubeOutlinePoints evenly along it, the corners in place
};
using CustomTubeShapePtr = std::shared_ptr<const CustomTubeShape>;
// A groom's custom shapes by name.
using TubeShapeLibrary = std::map<std::string, CustomTubeShapePtr>;

// Points round every outline, built-in or custom.
constexpr std::size_t kTubeOutlinePoints = 96;
// The most control points a custom shape takes.
constexpr std::size_t kMaxTubeShapePoints = 64;

// A custom shape from control points (at least 3, at most kMaxTubeShapePoints, anywhere: the
// outline is fitted into the unit domain, its box -1 to 1 on both axes, and turned counter-
// clockwise). Throws std::invalid_argument, saying what to fix, when the name is not a good one
// (checkTubeShapeName), the points are too few or too many, not numbers or all on a line, the
// outline crosses itself, or it does not enclose its center (the guide runs through the center).
CustomTubeShapePtr makeCustomTubeShape(const std::string& name, const std::vector<SectionPoint>& points, bool smooth);
// Throws std::invalid_argument unless `name` can name a library shape: 1 to 64 characters, letters,
// digits, spaces, '_', '-' and '.', not starting or ending with a space, not a built-in's name.
void checkTubeShapeName(const std::string& name);
// The closed curve through control points as makeCustomTubeShape samples it, without checking or
// fitting it (what an editor draws while points are dragged). Fewer than 3 points: them.
std::vector<SectionPoint> sampleTubeShapeCurve(const std::vector<SectionPoint>& points, bool smooth);
// Control points that draw a built-in shape closely (an editor's "Edit as custom" starts from
// them), and whether they are meant smooth.
std::vector<SectionPoint> builtinTubeShapePoints(TubeShape shape, bool* smooth);

// One cross-section of a tube, at some t.
struct TubeSection {
    float width = 0.1f;  // scene units
    float aspect = 1.0f;
    float twist = 0.0f;  // radians
};

// A guide's tube: its shape and the sections along it (see above).
struct TubeProfile {
    TubeShape shape = TubeShape::Round;  // a built-in shape, unless `custom`
    std::vector<float> width{0.1f, 0.1f};
    std::vector<float> aspect{1.0f};
    std::vector<float> twist{0.0f};
    // A library shape (shared with the library and every guide that takes it); `shape` is then
    // not read. A guide hierarchy keeps it pointing at its library's entry of that name.
    CustomTubeShapePtr custom;
    // Radians per guide point (evenly spaced root to tip, as the lists): how far the sections turn
    // from the guide's rotation-minimizing frame to lie as the scalp does (a guide hierarchy works
    // it out on the scalp at each evaluate: GuideHierarchy::evaluate), added to the twist by
    // sectionAt, so twist 0 lays the section's width along the scalp all along the guide. Worked
    // out, not authored: not saved, not compared, not in hashTube. Empty: 0.
    std::vector<float> roll;

    float widthAt(float t) const;
    float aspectAt(float t) const;  // clamped
    float twistAt(float t) const;  // the twist as authored (no roll)
    float rollAt(float t) const;
    // Width, aspect, and twist plus roll: the section as placed.
    TubeSection sectionAt(float t) const;
    // The shape's name: the built-in's, or the library shape's.
    std::string shapeName() const;
    // The built-in round shape (not a library shape).
    bool roundShape() const { return !custom && shape == TubeShape::Round; }
    // The same shape: the same built-in, or library shapes of the same name.
    bool sameShape(const TubeProfile& o) const {
        return custom ? o.custom && o.custom->name == custom->name : !o.custom && shape == o.shape;
    }
    // Anything but a plain round tube: another shape, an aspect other than 1 or a twist.
    bool shaped() const;
    // Set by hand: shaped, or a round tube whose width is not the default one (a tube someone
    // sized, which Scatter's guideRadius fills as it fills a shaped one).
    bool edited() const;
    // Every width times k (a tube scaled with its guide).
    void scaleWidth(float k);
    bool operator==(const TubeProfile& o) const {
        const bool sameCustom = custom ? o.custom && custom->name == o.custom->name && custom->hash == o.custom->hash
                                       : !o.custom && shape == o.shape;
        return sameCustom && width == o.width && aspect == o.aspect && twist == o.twist;
    }
    bool operator!=(const TubeProfile& o) const { return !(*this == o); }
};

// h combined with everything the tube holds (cache keys).
std::uint64_t hashTube(std::uint64_t h, const TubeProfile& tube);

// The shape's outline: a closed polygon, counter-clockwise, in the unit domain (its bounding box
// -1 to 1 on both axes). What hosts draw, and the domain children and strands fill.
const std::vector<SectionPoint>& tubeOutline(TubeShape shape);
// The outline's corners and edge points, as indices into tubeOutline: where its silhouette turns
// (a blade's two points and its sides' crests, a ribbon's four corners, a teardrop's point; none
// for round), what a wire view draws lines along the tube through, so the shape reads.
const std::vector<std::size_t>& tubeOutlineCorners(TubeShape shape);
bool insideTubeShape(TubeShape shape, float u, float v);
// How central (u, v) lies: its distance from the outline over the largest such distance in the
// shape: 1 at the shape's deepest point, 0 on the outline, below 0 outside. A round tube's is
// 1 - sqrt(u^2 + v^2): one minus the normalized radius.
float tubeShapeDepth(TubeShape shape, float u, float v);
// How far (u, v) lies outside the shape's outline, in half-widths: the distance in the section
// plane (u across the width, v times `aspect` across the height) to the outline drawn at that
// aspect, over the half-width. 0 inside or on it. So a band of one such distance round the outline
// is equally thick all round, however flat the tube. Beyond `limit` it may give any value above
// `limit` (the outline is not walked there).
float tubeOutsideDistance(TubeShape shape, float aspect, float u, float v,
                          float limit = std::numeric_limits<float>::max());
// A point inside the shape, uniform over its area, from `seed` (the same seed: the same point).
SectionPoint randomPointInTubeShape(TubeShape shape, std::uint64_t seed);
// The same for a tube's shape, built-in or custom (what fill, children, clumps and the wire use).
const TubeOutline& tubeShapeOutline(const TubeProfile& tube);
const std::vector<SectionPoint>& tubeOutline(const TubeProfile& tube);
const std::vector<std::size_t>& tubeOutlineCorners(const TubeProfile& tube);
bool insideTubeShape(const TubeProfile& tube, float u, float v);
float tubeShapeDepth(const TubeProfile& tube, float u, float v);
float tubeOutsideDistance(const TubeProfile& tube, float aspect, float u, float v,
                          float limit = std::numeric_limits<float>::max());
SectionPoint randomPointInTubeShape(const TubeProfile& tube, std::uint64_t seed);

// Section coordinates to the offset in a frame's cross-section plane (x along the frame's normal,
// y along its binormal; z 0): rotate(twist) * (u * w, v * w * aspect), w = width * scale.
Vec3f sectionOffset(const TubeSection& section, float u, float v, float scale = 1.0f);
// The inverse: an offset (x, y) in the cross-section plane to section coordinates (a width or
// height below 1e-6 is taken as 1e-6).
SectionPoint sectionCoords(const TubeSection& section, float x, float y, float scale = 1.0f);

// A tube laid along a curve, ready to place points: each sample's center and the world axes u and
// v map to there, worked out once (Interpolate's fillTubes, the display).
class TubeSpine {
public:
    TubeSpine() = default;
    // `points` (2 or more) evenly spaced along the guide; frames from frameUp; widths times scale.
    TubeSpine(const Vec3f* points, std::size_t count, const Vec3f& frameUp, const TubeProfile& tube, float scale = 1.0f);

    std::size_t samples() const { return centers_.size(); }
    bool empty() const { return centers_.empty(); }
    const TubeProfile& tube() const { return tube_; }
    // The world point of section coordinates (u, v) at t (0 root, 1 tip; linear between samples).
    Vec3f at(float t, float u, float v) const;
    // The same at sample i.
    Vec3f atSample(std::size_t i, float u, float v) const { return centers_[i] + axisU_[i] * u + axisV_[i] * v; }
    // Sample i's center and section axes (atSample's: center + axisU u + axisV v).
    const Vec3f& center(std::size_t i) const { return centers_[i]; }
    const Vec3f& axisU(std::size_t i) const { return axisU_[i]; }
    const Vec3f& axisV(std::size_t i) const { return axisV_[i]; }
    // Lays the tube's root footprint on the surface the guide grows from, its normal there
    // `normal`: the root section turned from the guide's root direction onto the normal (the
    // smallest turn), so it lies flat in the surface's tangent plane however steeply or shallowly
    // the guide leaves it, with the section's area. Across the guide stays across; the section's
    // side away from the surface (a guide lying down) comes to lie behind the root and its side
    // toward the surface ahead of it, as hair rooted behind a lock lies over the hair rooted
    // ahead. Without it (or with a zero normal, or one against the guide's direction) the
    // footprint is the root section itself, in the plane across the guide's root.
    void setRootSurface(const Vec3f& normal);
    // Where world point p lies on the root footprint: its section coordinates there (p taken into
    // the footprint's plane along its normal), and (with `along`) how far off that plane it lies
    // (signed, scene units).
    SectionPoint rootCoords(const Vec3f& p, float* along = nullptr) const;
    // The world point of section coordinates (u, v) on the root footprint.
    Vec3f footprintAt(float u, float v) const { return centers_.front() + footU_ * u + footV_ * v; }
    // The root section's half-width and half-height in scene units.
    float rootWidth() const { return rootWidth_; }
    float rootHeight() const { return rootHeight_; }
    // The share of the guide (in t) over which a strand rooted on the footprint joins the tube's
    // sections: a few of the root section's larger half-extents along the guide, at most all of it.
    float rootJoin() const { return rootJoin_; }

private:
    TubeProfile tube_;
    std::vector<Vec3f> centers_, axisU_, axisV_;
    Vec3f footU_, footV_, footNormal_;
    float rootWidth_ = 0.0f, rootHeight_ = 0.0f, rootJoin_ = 1.0f;
};

// The tube as JSON ({"shape", "width", "aspect", "twist"}; widths times scale; a library shape:
// its name), and a tube from such JSON over `base`: what a key gives replaces it (a "shape": a
// built-in's name or one of `library`'s) (a number: that value all along; a list:
// evenly spaced root to tip), what it leaves out stays; "scaleWidth" multiplies every width (after
// "width": a tube scaled as a whole, its taper kept); "samples" (2 or more) resamples all three
// lists to that many; "smooth" ("width", "aspect", "twist" or "all") smooths those lists
// (smoothedTube, at `points` samples or the lists' own count where that is more) "passes" times
// (default 3). Widths are divided by scale (a host's shown size to the stored).
// Throws std::invalid_argument on a bad shape, a negative width or an empty list.
std::string tubeToJson(const TubeProfile& tube, float scale = 1.0f);
TubeProfile editedTube(const TubeProfile& base, const std::string& editJson, float scale = 1.0f, std::size_t points = 0,
                       const TubeShapeLibrary* library = nullptr);

// The lists a tube holds, as a mask (smoothedTube).
enum TubeLists : unsigned { kTubeWidthList = 1u, kTubeAspectList = 2u, kTubeTwistList = 4u, kTubeAllLists = 7u };

// The tube with the lists in `lists` smoothed along the guide: each resampled to `samples` values
// (2 or more: one per guide point), then `passes` times each inner value moved toward its two
// neighbours' mean by `amount` (0 to 1; times weights[i] where `weights` gives one per sample: a
// brush's falloff), the root's and the tip's kept. Lists not smoothed stay as they were.
TubeProfile smoothedTube(const TubeProfile& tube, unsigned lists, std::size_t samples, int passes, float amount = 0.5f,
                         const std::vector<float>* weights = nullptr);

// A tube blended from others (a new guide's from its neighbours'): the first's shape (built-in or custom), and each
// list the weighted mean of theirs at `samples` evenly spaced points (2 or more), stored as one
// value where all are equal and as root and tip where they lie on a line. Empty `tubes`: the
// default tube.
TubeProfile blendedTube(const std::vector<const TubeProfile*>& tubes, const std::vector<float>& weights, std::size_t samples);

}  // namespace groomlab
