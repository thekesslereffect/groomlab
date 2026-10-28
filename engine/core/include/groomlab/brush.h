// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

class ScalpMirror;

// Grooming brushes (GT-1). Brushes are spheres in world space; a point's
// weight falls off smoothly from 1 at the center to 0 at the radius.
enum class BrushMode {
    Comb,       // drag guide points along the stroke; roots stay pinned, lengths kept
    Smooth,     // relax kinks; lengths kept
    Lift,       // turn guides toward their root normal (negative strength lays them down)
    Length,     // grow (positive strength) or cut (negative) guides touched by the brush
    ShapeTube,  // paint touched guides' tube sections near the brush: width, flatness or twist (tubeChannel)
    Attribute,  // paint a per-guide attribute toward `value` (clump paint: attribute "clump")
    Twist,      // swing guides around their root normal (signed: counter-clockwise seen from above)
    Curl,       // bend guides progressively toward the tip (signed), lengths kept
    Straighten, // relax guides toward the straight line along their root direction
    Attract,    // pull guide tips toward the brush center (negative: push apart)
    Erase,      // remove guides the brush touches, with their descendants
    Cut,        // trim guides where the brush touches them (the part past the brush goes)
    Slide,      // move roots along the scalp with the drag; each guide keeps its shape, turned with the surface
    Direction,  // turn guides about their root normal toward the stroke's direction, lengths kept (negative: away)
    Noise,      // add smooth random jitter along guides, seeded per guide, lengths kept (negative: smooth it out)
};

enum class PaintMode { Replace, Add, Subtract, ClampMaximum };
const char* paintModeName(PaintMode mode);
PaintMode paintModeFromString(const std::string& name);

// What the Shape brush (BrushMode::ShapeTube) paints on a guide's tube sections (tube.h), each
// section by the brush's weight at its guide point (the painted list first resampled to one value
// per point, so edits are per CV):
//   Width     the section grows (negative strength: shrinks);
//   Flatness  its aspect goes toward 0, a flat ribbon (negative: back toward 1, as drawn);
//   Twist     it turns about the guide (negative: the other way).
enum class TubeChannel { Width, Flatness, Twist };
// width, flatness, twist; tubeChannelFromString throws std::invalid_argument.
const char* tubeChannelName(TubeChannel channel);
TubeChannel tubeChannelFromString(const std::string& name);

// Parses a mode name: comb, smooth, lift, length, shape (ShapeTube), paint (attribute, clump),
// cut, twist, curl, straighten, attract, erase, slide (move), direction, noise. Throws
// std::invalid_argument.
BrushMode brushModeFromString(const std::string& name);
const char* brushModeName(BrushMode mode);

struct BrushSettings;
// A brush's Shift+drag: the Shape brush smooths the channel it paints (smoothTube); every other
// brush becomes Smooth. endShiftSmoothing puts back `mode` (the brush's own) and clears smoothTube.
void useShiftSmoothing(BrushSettings& settings);
void endShiftSmoothing(BrushSettings& settings, BrushMode mode);

struct BrushSettings {
    BrushMode mode = BrushMode::Comb;
    float radius = 0.25f;
    float strength = 1.0f;  // Comb: fraction of the drag; others: effect per stamp (signed)
    // How far out the brush keeps its full weight: 0 falls off smoothly from the center to the
    // radius; 1 is full weight to the edge (a hard brush); between, full to that fraction of
    // the radius, then the falloff.
    float hardness = 0.0f;
    // The falloff as a curve instead: the weight (clamped 0 to 1) over the distance from the
    // center (0) to the radius (1); null: the smooth falloff past `hardness`.
    std::shared_ptr<const Ramp> falloffProfile;
    // Root to tip: the shaping brushes' effect along each guide, its root (0) to its tip (1) by
    // length, times the strength (Comb, Smooth, Straighten, Lift, Twist, Curl, Attract,
    // Direction, Noise); null: the same all along.
    std::shared_ptr<const Ramp> strengthProfile;
    int level = 0;          // hierarchy level to edit (-1: every level)
    std::string group;      // edit only this group instead of a level
    // Also edit locked descendants (following ones move with their parents anyway), once
    // each, unless hidden or frozen.
    bool propagate = false;

    // Attribute mode: the attribute painted and the value it moves toward.
    std::string attribute = "clump";
    float value = 1.0f;
    PaintMode paintMode = PaintMode::Replace;
    // Attribute mode: paint a per-point layer, each point of a guide moving
    // toward `value` by the brush's weight there (a band along the guide),
    // instead of one value per guide. A guide without the layer starts it from
    // its scalar attribute (inherited too), or 0.
    bool pointwise = false;

    // Only these guides, each scaled by its weight (a soft selection); null:
    // every target guide. Locked descendants reached by `propagate` use their
    // ancestor's weight.
    const std::map<GuideId, float>* mask = nullptr;

    // Symmetry: every stamp is repeated mirrored across the plane where
    // coordinate `mirrorAxis` (0 x, 1 y, 2 z) equals `mirrorCenter`; -1: off.
    // A guide touched by both stamps changes once.
    int mirrorAxis = -1;
    float mirrorCenter = 0.0f;
    // Topological symmetry instead: every stamp repeated at its partner spot across the scalp's
    // own surface (ScalpMirror, scalp_mirror.h), whatever the scalp's pose; the partner stamp is
    // a world sphere of `radius`. mirrorAxis is not read while it is on.
    bool mirrorTopology = false;

    // Keep edited guides out of the scalp: points that end up under the surface
    // (or closer than `collideOffset`) are pushed out, lengths kept.
    bool collide = true;
    float collideOffset = 0.0f;

    // Comb: guides stretch with the drag (longer or shorter, points respaced
    // evenly) instead of keeping their length.
    bool stretch = false;

    // Shape (ShapeTube): what it paints on the tubes.
    TubeChannel tubeChannel = TubeChannel::Width;
    // Shape: smooth that channel of the tube sections under the brush along each guide (each
    // section toward its neighbours' mean by the brush's weight there times |strength|) instead
    // of painting it: its Shift+drag (useShiftSmoothing).
    bool smoothTube = false;

    // Freeze mask: guides carrying this attribute (inherited too) are held back
    // by it (1: untouched). Painting the attribute itself is never held back,
    // so frozen guides can be unfrozen. Empty: off.
    std::string freeze = "freeze";

    // Groups hidden in the viewport: their guides are left alone.
    std::set<std::string> hiddenGroups;
};

// A stroke's symmetry as one thing: off, a world plane (mirrorAxis, mirrorCenter) or the scalp's
// own topology (mirrorTopology, on `scalp`: without one it is off).
struct StrokeSymmetry {
    int axis = -1;
    float center = 0.0f;
    std::shared_ptr<const ScalpMirror> mirror;
    const ScalpMesh* scalp = nullptr;

    static StrokeSymmetry of(const BrushSettings& s, const ScalpMesh* scalp);
    bool on() const { return mirror != nullptr || (axis >= 0 && axis <= 2); }
    bool topology() const { return mirror != nullptr; }
    Vec3f point(const Vec3f& p) const;                    // a point's mirror image
    Vec3f vector(const Vec3f& at, const Vec3f& v) const;  // a direction at `at`, mirrored
};

// The scalp's line of symmetry, for hosts to draw while symmetry is on (as Substance Painter draws
// its red center line): where the plane cuts the scalp (a world plane), or the edges whose two
// vertices are their own mirror (topology). Segments as pairs of points (out[2k], out[2k + 1]),
// each lifted off the surface by `lift` along its triangle's normal so a line drawn there shows.
// Empty when symmetry is off or nothing of the scalp lies on the seam.
std::vector<Vec3f> symmetrySeam(const ScalpMesh& scalp, const StrokeSymmetry& sym, float lift);

// The guide mirroring guide `id` under `sym`: of its group, the one whose root is nearest the
// mirror image of its root (itself, for a guide on the seam). kNoGuide when symmetry is off or
// there is no such guide.
GuideId mirrorGuide(const GuideHierarchy& hierarchy, GuideId id, const StrokeSymmetry& sym);
// `ids` and each one's mirror guide, each once, in order (symmetry off: `ids`).
std::vector<GuideId> withMirrorGuides(const GuideHierarchy& hierarchy, const std::vector<GuideId>& ids, const StrokeSymmetry& sym);

// Pushes guide points (root first) out of the scalp to at least `offset`
// above it, keeping segment lengths. The root stays where it is.
std::vector<Vec3f> keepOffScalp(const std::vector<Vec3f>& points, const ScalpMesh& scalp, float offset = 0.0f);

// Optional per-point weight in [0, 1] replacing the world-space sphere, e.g.
// a screen-space falloff around the cursor for interactive tools.
using BrushWeightFn = std::function<float(const Vec3f&)>;

// How a brush fades toward its edge: smoothly past hardness x radius (BrushSettings::hardness),
// or along a curve over the distance from the center (0) to the edge (1) when `profile` has
// keys. A hardness alone converts to one.
struct BrushFalloff {
    float hardness = 0.0f;
    std::shared_ptr<const Ramp> profile;
    BrushFalloff(float h = 0.0f) : hardness(h) {}  // NOLINT(google-explicit-constructor): a hardness is a falloff
    BrushFalloff(float h, std::shared_ptr<const Ramp> curve) : hardness(h), profile(std::move(curve)) {}
    // The weight at `distance` from the center: 0 at the radius and past it.
    float operator()(float distance, float radius) const;
};
inline BrushFalloff falloffOf(const BrushSettings& s) { return {s.hardness, s.falloffProfile}; }

// The brush's weight at `distance` from its center: 1 at the center, 0 at the radius, falling
// off smoothly past hardness x radius (see BrushSettings::hardness), or along the falloff's curve.
float brushFalloff(float distance, float radius, const BrushFalloff& falloff = {});

// A world-space sphere as a weight function (for stamps given without one).
BrushWeightFn sphereWeight(const Vec3f& center, float radius, const BrushFalloff& falloff = {});

// A point's brush weight: `custom` (clamped to [0, 1]) when given, else the
// sphere's falloff around `center`.
float brushWeight(const Vec3f& p, const Vec3f& center, float radius, const BrushFalloff& falloff, const BrushWeightFn& custom);

// Whether a brush's custom weight can be above 0 anywhere in the box [lo, hi]: false only when
// it is 0 at every point of it (e.g. a screen circle the box projects outside of). Lets a stamp
// skip the guides whose bounds it cannot reach, with the same result.
using BrushReachFn = std::function<bool(const Vec3f& lo, const Vec3f& hi)>;

// What a stroke's stamps on one hierarchy share: the guides the brush edits (its level or group,
// mask, hidden groups and freeze) and each one's bounds, kept up to date as the stamps move guides
// (an edited guide and its descendants are measured again), so a stamp looks only at the guides
// within its reach. Made empty for each stroke; it starts over by itself when the settings that
// pick the guides change, when a stamp erases guides, or when the hierarchy has other guides than
// when it was filled (a hierarchy edited between stamps by anything else must clear() it).
class BrushStrokeCache {
public:
    BrushStrokeCache();
    ~BrushStrokeCache();
    BrushStrokeCache(BrushStrokeCache&&) noexcept;
    BrushStrokeCache& operator=(BrushStrokeCache&&) noexcept;
    void clear();
    struct State;
    State& state() { return *state_; }

private:
    std::unique_ptr<State> state_;
};

// Applies one brush stamp centered at `center`. `drag` is the stroke motion
// since the previous stamp (Comb uses it; other modes ignore it). The
// hierarchy must be evaluated; it is re-evaluated on `scalp` afterwards.
// Returns the number of guides changed.
std::size_t applyBrushStamp(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                            const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight = {});
// The same, skipping the guides out of the brush's reach: the world sphere's without a custom
// weight; with one, the boxes `reach` says it cannot reach (none skipped without `reach`). With
// a `cache` (one per stroke), the guides and their bounds are found once for the stroke's stamps.
// The guides' edits are made in parallel when the weight is the world sphere's, or a custom one
// that may be called from several threads at once (`threadSafeWeight`: screenBrushWeight's is).
// The same guides change, by the same amounts, as without any of it.
std::size_t applyBrushStamp(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                            const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight, const BrushReachFn& reach,
                            BrushStrokeCache* cache = nullptr, bool threadSafeWeight = false);

// Applies a stroke along `path`: stamps spaced at most radius / 4 apart, each
// with the motion to the next stamp. A single-point path is one still stamp.
// Returns the number of distinct guides changed.
std::size_t applyBrushStroke(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                             const std::vector<Vec3f>& path);

// ---------------------------------------------------------------------------
// The same brushes on flow arrows (pins and sinks) and part lines. The sphere,
// its falloff, the stamp spacing and symmetry (a mirrored stamp with a mirrored
// drag; something touched by both changes once) are the guides'. Pins, sinks
// and part points are weighed by their world position on `scalp`, and stay
// bound to it: a moved point is re-bound to the closest scalp point, and a
// pin's direction is carried along onto the new tangent plane. Guides are
// never touched. Only `mode`, `radius`, `strength` and the symmetry settings
// are read. What each brush does:
//   Comb        slides pins, sinks and part points with the drag (strength x
//               weight) and turns pins toward the drag.
//   Smooth      pins: their direction averaged with the nearest pins, and the
//               pins of a stroke line evened out along it; parts: the polyline
//               relaxed (interior points toward their neighbours' midpoint).
//   Twist       turns pin directions around the surface normal (signed);
//               sinks turn their `angle` by the same.
//   Length      sinks grow (positive strength) or shrink (negative) their radius.
//   Attract     pulls pins, sinks and part points toward the brush center along
//               the surface (negative pushes them apart).
//   Straighten  parts: interior points toward the chord between the part's
//               ends; pins: turned toward their stroke line's overall direction.
//   Cut         parts: split where the brush touches them (the touched points
//               go; the pieces keep the name, then name1, name2...); pins and
//               sinks unchanged.
//   Erase       pins, sinks and parts under the brush removed.
//   Lift, Curl, ShapeTube, Attribute, Slide, Direction, Noise: nothing.
// Each returns how many pins and sinks (or parts) changed.
std::size_t applyBrushStampToFlow(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& settings,
                                  const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight = {});
std::size_t applyBrushStrokeToFlow(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& settings,
                                   const std::vector<Vec3f>& path);
std::size_t applyBrushStampToParts(PartSet& parts, const ScalpMesh& scalp, const BrushSettings& settings,
                                   const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight = {});
std::size_t applyBrushStrokeToParts(PartSet& parts, const ScalpMesh& scalp, const BrushSettings& settings,
                                    const std::vector<Vec3f>& path);

// How new guides are shaped by Place and Place Many.
struct PlaceSettings {
    // The group new guides go in (a missing one is created as a root group).
    // In a child group, a new guide becomes a child of the nearest guide of
    // the parent group, following it from then on: children placed by hand.
    std::string group = "primary";
    float length = 0.5f;            // standing guides, and when there are no neighbors
    int segments = 12;
    // The new guides' tube (widths at the size shown): its shape always (a host's chosen shape:
    // Groom Tools' Tubes panel), its width, aspect and twist where no guides are near to blend them
    // from (matchNeighbors). `tubeShape`, when set, names the shape instead: a built-in's name or one
    // of the groom's library shapes (GuideHierarchy::tubeShapes; one the groom lacks: tube.shape).
    TubeProfile tube;
    std::string tubeShape;
    // Interpolate the nearest guides (inverse-distance blend of the 3 nearest,
    // turned to the new root's normal): shape, length, point count, the tube's width, aspect and
    // twist (the two nearest's; its shape stays `tube`'s) and painted layers, instead of standing
    // straight up with `length` and `segments`. The group's own guides, or while it has none the other
    // groups' at its level. Draw takes the painted layers.
    bool matchNeighbors = true;
    float spacing = 0.1f;  // Place Many: no new root closer than this to any root of the group
    // Solo new guides (Place, Draw): a stroke's new guides unfrozen and every other guide frozen
    // (soloGuides), in the same undo step, so the brushes after work on the new ones alone.
    bool soloNewGuides = false;
};

// The placement tube (PlaceSettings::tube and tubeShape) as JSON, as tubeToJson writes a tube
// ({"shape", "width", "aspect", "twist"}: widths as shown), and set from such JSON: the keys given
// replace those (a "shape": a built-in's name, or any other name for a library shape, looked up in
// the groom at placing). Throws std::invalid_argument as editedTube does.
std::string placementTubeJson(const PlaceSettings& s);
void setPlacementTube(PlaceSettings& s, const std::string& json);

// Solo: guides `active` and their descendants unfrozen (the freeze layer `freeze` at 0), every
// other guide frozen (1): the brushes leave all but those alone (BrushSettings::freeze). Unknown
// IDs are skipped; none known unfreezes nothing and freezes every guide.
void soloGuides(GuideHierarchy& hierarchy, const std::vector<GuideId>& active, const std::string& freeze = "freeze");
// Every guide unfrozen: the freeze layer removed. False when no guide carried it.
bool unfreezeAll(GuideHierarchy& hierarchy, const std::string& freeze = "freeze");
// The Select tool's solo: the selection (and its descendants) the guides brushes work on, every
// other guide frozen (soloGuides); an empty selection unfreezes every guide. True when the
// freeze layer changed.
bool freezeToSelection(GuideHierarchy& hierarchy, const std::vector<GuideId>& selection, const std::string& freeze = "freeze");

// Place brush: adds a primary guide at scalp point `at`. Returns its ID.
GuideId placeGuide(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& settings,
                   const SurfacePoint& at);

// Place Many: a guide at each point of `path` (projected onto the scalp) that
// is at least `spacing` from every root of the group, including ones just
// placed. Returns the new IDs.
std::vector<GuideId> placeGuides(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& settings,
                                 const std::vector<Vec3f>& path);

// Draw guide: a primary guide shaped like `path`, rooted at the scalp point
// nearest path[0], resampled evenly to settings.segments segments and kept off
// the scalp. Throws std::invalid_argument when the path is too short.
GuideId drawGuide(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& settings,
                  const std::vector<Vec3f>& path);

}  // namespace groomlab
